/*
 * Driver side of a first AGX compute job. Freestanding. See agx_job.h.
 */
#include "agx_job.h"

#include "agx_isa.h"


/* Per ring slot: RunCompute at 0, its microsequence at CMD_MS. */
#define CMD_MS 0x400
#define CMD_BYTES AGX_UAT_PGSZ

/* Job block in the user VM, relative to usc_exec_base. */
#define BLK_USC_BYTES 128

static uint64_t align_up(uint64_t v, uint64_t a)
{
	return (v + a - 1) & ~(a - 1);
}

static void zero(uint8_t *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++)
		p[i] = 0;
}

static void copy(uint8_t *d, const uint8_t *s, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++)
		d[i] = s[i];
}

static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

int agx_kalloc(struct agx_dev *d, uint64_t size, uint64_t *va, uint8_t **cpu)
{
	if (size == 0)
		return AGX_JOB_E_INVAL;
	uint64_t bytes = align_up(size, AGX_UAT_PGSZ), pa;
	int err = agx_pages_alloc(&d->pages, bytes >> AGX_UAT_PGBIT, &pa);
	if (err)
		return err;
	if ((err = agx_uat_map(&d->kern, &d->pages, d->kva_next, pa, bytes, AGX_PROT_FW_SHARED_RW)))
		return err;
	*va = d->kva_next;
	*cpu = agx_mem_ptr(&d->mem, pa, bytes);
	d->kva_next += bytes;
	return AGX_JOB_OK;
}

int agx_vm_alloc(struct agx_dev *d, struct agx_vm *vm, uint64_t size, uint64_t prot, uint64_t *va, uint8_t **cpu)
{
	if (size == 0)
		return AGX_JOB_E_INVAL;
	uint64_t bytes = align_up(size, AGX_UAT_PGSZ), pa;
	int err = agx_pages_alloc(&d->pages, bytes >> AGX_UAT_PGBIT, &pa);
	if (err)
		return err;
	if ((err = agx_uat_map(&vm->uat, &d->pages, vm->va_next, pa, bytes, prot)))
		return err;
	*va = vm->va_next;
	*cpu = agx_mem_ptr(&d->mem, pa, bytes);
	/* An unmapped guard page after every allocation: overruns fault. */
	vm->va_next += bytes + AGX_UAT_PGSZ;
	return AGX_JOB_OK;
}

static uint8_t *ttbat_slot(struct agx_dev *d, unsigned slot)
{
	return agx_mem_ptr(&d->mem, d->ttbat_pa + (uint64_t)slot * AGX_TTBAT_SLOT_BYTES, AGX_TTBAT_SLOT_BYTES);
}

int agx_dev_init(struct agx_dev *d, const struct agx_mem *mem, uint64_t pa_start, uint64_t pa_end,
		 uint32_t ring_size, agx_submit_fn submit, void *submit_ctx)
{
	int err;
	uint8_t *p;
	if (ring_size < 2 || ring_size > AGX_JOB_RING_MAX || !submit)
		return AGX_JOB_E_INVAL;
	zero((uint8_t *)d, sizeof(*d));
	d->mem = *mem;
	d->submit = submit;
	d->submit_ctx = submit_ctx;
	if ((err = agx_pages_init(&d->pages, &d->mem, pa_start, pa_end)) ||
	    (err = agx_pages_alloc(&d->pages, 1, &d->ttbat_pa)) || (err = agx_uat_init(&d->kern, &d->pages, 1)))
		return err;
	d->kva_next = AGX_UAT_KERN_BASE;
	/* Context 0 is the firmware's: kernel half only. */
	agx_wr64(ttbat_slot(d, 0) + 8, agx_ttbr(&d->kern, 0));

	if ((err = agx_kalloc(d, AGX_QI_SIZE, &d->qi_va, &d->qi)) ||
	    (err = agx_kalloc(d, AGX_RS_SIZE, &d->rs_va, &d->rs)) ||
	    (err = agx_kalloc(d, 8ull * ring_size, &d->ring_va, &d->ring)) ||
	    (err = agx_kalloc(d, (uint64_t)CMD_BYTES * ring_size, &d->cmd_va, &d->cmd)) ||
	    (err = agx_kalloc(d, 8, &d->stamp_va, &p)))
		return err;
	d->stamp = p;
	d->fw_stamp_va = d->stamp_va + 4;
	d->fw_stamp = p + 4;
	agx_wr64(d->qi + AGX_QI_STATE, d->rs_va);
	agx_wr64(d->qi + AGX_QI_RING, d->ring_va);
	agx_wr32(d->rs + AGX_RS_RB_SIZE, ring_size);
	d->ring_size = ring_size;
	d->next_stamp = 1;
	return AGX_JOB_OK;
}

int agx_vm_create(struct agx_dev *d, struct agx_vm *vm, unsigned slot)
{
	if (slot == 0 || slot >= AGX_UAT_NUM_CTX)
		return AGX_JOB_E_INVAL;
	uint8_t *e = ttbat_slot(d, slot);
	if (agx_rd64(e) & AGX_TTBR_VALID)
		return AGX_UAT_E_BUSY;
	int err = agx_uat_init(&vm->uat, &d->pages, 0);
	if (err)
		return err;
	vm->slot = slot;
	vm->va_next = 0x1000000; /* 16 MiB: low pages stay unmapped so NULL faults */
	agx_wr64(e, agx_ttbr(&vm->uat, slot));
	agx_wr64(e + 8, agx_ttbr(&d->kern, slot));
	return AGX_JOB_OK;
}

int agx_shader_scan(const uint8_t *code, uint32_t len, struct agx_shader_info *info)
{
	uint32_t pc = 0;
	info->gpr_halves = 0;
	info->code_bytes = 0;
	info->uses_shared = 0;
	while (pc < len) {
		struct agx_ins ins;
		int n = agx_decode(code + pc, len - pc, &ins);
		if (n < 0)
			return AGX_JOB_E_SHADER;
		const struct agx_ins_desc *desc = &agx_ins_table[ins.op];
		for (unsigned f = 0; f < desc->nfields; f++) {
			const struct agx_field *fd = &desc->fields[f];
			struct agx_operand o;
			if (fd->type != AGX_FT_DEST && fd->type != AGX_FT_SRC)
				continue;
			if (agx_operand_resolve(fd->ref, ins.val[f], &o) != AGX_OK)
				return AGX_JOB_E_SHADER;
			if (o.kind == AGX_OPND_REG) {
				uint32_t end = (uint32_t)o.base + (uint32_t)(o.size / 16) * o.count;
				if (end > info->gpr_halves)
					info->gpr_halves = end;
			}
		}
		if (str_eq(desc->name, "lload") || str_eq(desc->name, "lstore"))
			info->uses_shared = 1;
		pc += (uint32_t)n;
		if (str_eq(desc->name, "stop"))
			info->code_bytes = pc;
	}
	if (!info->code_bytes)
		return AGX_JOB_E_SHADER;
	if (info->gpr_halves < 2)
		info->gpr_halves = 2; /* r0l: the execution nesting counter */
	return AGX_JOB_OK;
}

uint32_t agx_queue_errors(const struct agx_dev *d)
{
	return agx_rd32(d->qi + AGX_QI_ERROR_COUNT);
}

int agx_compute_done(const struct agx_dev *d, uint32_t stamp_value)
{
	return (int32_t)(agx_rd32(d->stamp) - stamp_value) >= 0;
}

/* USC word list: Uniform words, Shared, Shader, Registers, No preshader
 * (Mesa cmdbuf.xml, MIT). Returns bytes written, or 0 when it does not fit. */
static uint32_t build_usc(uint8_t *w, uint64_t uni_va, uint32_t uni_halves, uint32_t code_off,
			  const struct agx_shader_info *si, uint32_t shared_bytes)
{
	uint32_t at = 0;
	for (uint32_t start = 0; start < uni_halves; start += 64) {
		uint32_t n = uni_halves - start < 64 ? uni_halves - start : 64;
		uint64_t v = AGX_USC_UNIFORM | (uint64_t)start << 8 | (uint64_t)(n & 0x3f) << 20 |
			     ((uni_va + 2ull * start) >> 2) << 26;
		if (at + 8 > BLK_USC_BYTES)
			return 0;
		agx_wr64(w + at, v);
		at += 8;
	}
	uint32_t groups = (shared_bytes + 255) / 256; /* 256 groups: field 0 */
	agx_wr32(w + at, AGX_USC_SHARED | (uint32_t)(shared_bytes != 0) << 8 | AGX_USC_SHARED_LAYOUT_COMPUTE << 10 |
				 (groups & 0xff) << 24);
	at += 4;
	w[at] = AGX_USC_SHADER;
	w[at + 1] = 0;
	agx_wr32(w + at + 2, code_off);
	at += 6;
	uint32_t regs = (si->gpr_halves + 15) / 16; /* groups of 8 32-bit registers */
	agx_wr32(w + at, AGX_USC_REGISTERS | (regs & 0x1f) << 8);
	at += 4;
	w[at] = AGX_USC_NO_PRESHADER;
	w[at + 1] = 0;
	return at + 2;
}

int agx_compute_submit(struct agx_dev *d, struct agx_vm *vm, const struct agx_compute *job, uint32_t *stamp_value)
{
	struct agx_shader_info si;
	int err;
	uint32_t threads = 1;
	for (int i = 0; i < 3; i++) {
		if (!job->local[i] || !job->grid[i])
			return AGX_JOB_E_INVAL;
		threads *= job->local[i];
	}
	uint32_t uni_halves = 4 * job->nbuf + 2 * job->nconst;
	if (threads > 1024 || uni_halves > AGX_JOB_MAX_UNIFORM_HALVES || job->shared_bytes > 65536 ||
	    !job->code || !job->code_len)
		return AGX_JOB_E_INVAL;
	if ((err = agx_shader_scan(job->code, job->code_len, &si)))
		return err;
	if (si.uses_shared && !job->shared_bytes)
		return AGX_JOB_E_SHADER;
	uint32_t next = (d->wptr + 1) % d->ring_size;
	if (next == agx_rd32(d->rs + AGX_RS_GPU_RPTR))
		return AGX_JOB_E_RING_FULL;

	/* User-VM block: code at 0, then USC words, uniforms and the CDM stream.
	 * Read-only to the GPU, like Metal's pipeline and argument memory. */
	uint32_t usc_off = (uint32_t)align_up(si.code_bytes, 64);
	uint32_t uni_off = usc_off + BLK_USC_BYTES;
	uint32_t cdm_off = (uint32_t)align_up(uni_off + 2 * uni_halves, 64);
	uint32_t cdm_bytes = 32 + 4 + 4; /* launch, barrier, terminate */
	uint64_t blk_va;
	uint8_t *blk;
	if ((err = agx_vm_alloc(d, vm, cdm_off + cdm_bytes, AGX_PROT_GPU_SHARED_RO, &blk_va, &blk)))
		return err;
	copy(blk, job->code, si.code_bytes);
	uint8_t *u = blk + uni_off;
	for (unsigned k = 0; k < job->nbuf; k++)
		agx_wr64(u + 8 * k, job->buf_va[k]);
	for (unsigned k = 0; k < job->nconst; k++)
		agx_wr32(u + 8 * job->nbuf + 4 * k, job->consts[k]);
	if (!build_usc(blk + usc_off, blk_va + uni_off, uni_halves, 0, &si, job->shared_bytes))
		return AGX_JOB_E_INVAL;
	uint8_t *c = blk + cdm_off;
	uint32_t ugroups = (uni_halves + 63) / 64; /* groups of 64 halves, 8: field 0 */
	agx_wr32(c, AGX_CDM_LAUNCH << 29 | (ugroups & 7) << 1);
	agx_wr32(c + 4, usc_off); /* pipeline, 64-byte aligned, relative to usc_exec_base */
	for (int i = 0; i < 3; i++) {
		agx_wr32(c + 8 + 4 * i, job->grid[i]);
		agx_wr32(c + 20 + 4 * i, job->local[i]);
	}
	agx_wr32(c + 32, AGX_CDM_BARRIER << 29);
	agx_wr32(c + 36, AGX_CDM_STREAM_TERMINATE << 29);

	/* Firmware side: RunCompute and its microsequence in this ring slot. */
	uint32_t value = d->next_stamp;
	uint64_t rc_va = d->cmd_va + (uint64_t)CMD_BYTES * d->wptr, ms_va = rc_va + CMD_MS;
	uint8_t *rc = d->cmd + (uint64_t)CMD_BYTES * d->wptr, *ms = rc + CMD_MS;
	zero(rc, CMD_BYTES);
	agx_wr32(rc + AGX_RC_TAG, AGX_CMD_RUN_COMPUTE);
	agx_wr32(rc + AGX_RC_COUNTER, value);
	agx_wr32(rc + AGX_RC_VM_SLOT, vm->slot);
	uint8_t *jp1 = rc + AGX_RC_JOB_PARAMS1, *jp2 = rc + AGX_RC_JOB_PARAMS2;
	agx_wr64(jp1 + AGX_JP1_CDM_BASE, blk_va + cdm_off);
	agx_wr64(jp1 + AGX_JP1_USC_EXEC_BASE, blk_va);
	agx_wr32(jp1 + AGX_JP1_UNK_38, AGX_JP1_UNK_38_VALUE);
	agx_wr32(jp1 + AGX_JP1_UNK_58, AGX_JP1_UNK_58_VALUE);
	agx_wr64(jp2 + AGX_JP2_CDM_END, blk_va + cdm_off + cdm_bytes);
	agx_wr64(rc + AGX_RC_META + AGX_META_STAMP, d->stamp_va);
	agx_wr64(rc + AGX_RC_META + AGX_META_FW_STAMP, d->fw_stamp_va);
	agx_wr32(rc + AGX_RC_META + AGX_META_STAMP_VALUE, value);

	uint32_t off = 0;
	agx_wr32(ms, AGX_MS_START_COMPUTE);
	agx_wr64(ms + AGX_MS_SC_JOB_PARAMS1, rc_va + AGX_RC_JOB_PARAMS1);
	agx_wr64(ms + AGX_MS_SC_WORK_QUEUE, d->qi_va);
	agx_wr32(ms + AGX_MS_SC_VM_SLOT, vm->slot);
	agx_wr64(ms + AGX_MS_SC_JOB_PARAMS2, rc_va + AGX_RC_JOB_PARAMS2);
	off += AGX_MS_START_COMPUTE_SIZE;
	agx_wr32(ms + off, AGX_MS_WAIT_FOR_IDLE_COMPUTE);
	off += AGX_MS_WAIT_FOR_IDLE_SIZE;
	uint8_t *fc = ms + off;
	agx_wr32(fc, AGX_MS_FINALIZE_COMPUTE);
	agx_wr64(fc + AGX_MS_FC_WORK_QUEUE, d->qi_va);
	agx_wr32(fc + AGX_MS_FC_VM_SLOT, vm->slot);
	agx_wr64(fc + AGX_MS_FC_JOB_PARAMS2, rc_va + AGX_RC_JOB_PARAMS2);
	agx_wr64(fc + AGX_MS_FC_FW_STAMP, d->fw_stamp_va);
	agx_wr32(fc + AGX_MS_FC_STAMP_VALUE, value);
	agx_wr64(fc + AGX_MS_FC_RESTART_BRANCH, ms_va + off + AGX_MS_FINALIZE_COMPUTE_SIZE);
	off += AGX_MS_FINALIZE_COMPUTE_SIZE;
	agx_wr32(ms + off, AGX_MS_RETIRE_STAMP);
	off += AGX_MS_RETIRE_STAMP_SIZE;
	agx_wr64(rc + AGX_RC_MICROSEQ, ms_va);
	agx_wr32(rc + AGX_RC_MICROSEQ_SIZE, off);

	/* Publish: ring entry, then cpu_wptr, then tell the firmware. */
	agx_wr64(d->ring + 8ull * d->wptr, rc_va);
	agx_wr32(d->rs + AGX_RS_CPU_WPTR, next);
	uint8_t msg[AGX_RWQ_SIZE];
	zero(msg, sizeof(msg));
	agx_wr32(msg + AGX_RWQ_PIPE_TYPE, AGX_PIPE_COMPUTE);
	agx_wr64(msg + AGX_RWQ_WORK_QUEUE, d->qi_va);
	agx_wr32(msg + AGX_RWQ_WPTR, next);
	agx_wr32(msg + AGX_RWQ_EVENT_SLOT, d->event_slot);
	agx_wr32(msg + AGX_RWQ_IS_NEW, value == 1);
	d->wptr = next;
	d->next_stamp++;
	*stamp_value = value;
	if (d->submit(d->submit_ctx, msg) < 0)
		return AGX_JOB_E_SUBMIT;
	return AGX_JOB_OK;
}
