/*
 * Simulated AGX tests: the AienOS driver side (agx_job.c) builds real
 * firmware, command-stream and USC bytes for Apple-compiled kernels from the
 * corpus; the simulated AGX (agx_sim.c) runs them through the UAT and the ISA
 * interpreter. Each result must equal the plain C reference and the hash this
 * MacBook's GPU produced for the same case (make gpu-ref). Then the fault
 * paths: MMU, permissions, refused encodings, watchdog, malformed commands,
 * ring wrap and ring full.
 *
 *   agx_sim_test <corpus dir>
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../agx_cases.h"
#include "../agx_job.h"
#include "../agx_sim.h"

#define DRAM_PA 0x800000000ull
#define DRAM_BYTES (64ull << 20)

static uint8_t *dram;
static struct agx_mem mem;
static struct agx_sim sim;
static struct agx_dev dev;
static const char *corpus;
static int failures, checks;

#define CHECK(cond, ...)                                                    \
	do {                                                                \
		checks++;                                                   \
		if (!(cond)) {                                              \
			failures++;                                         \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
			printf(__VA_ARGS__);                                \
			printf("\n");                                       \
		}                                                           \
	} while (0)

/* OBSERVED: make gpu-ref on this MacBook Air (Apple M2, G14G), 2026-10-20,
 * FNV-1a 64 over the whole output buffer (agx_case_hash). */
static const struct {
	const char *name;
	uint64_t hash;
} gpu_hash[] = {
	{ "vadd", 0x756c2c450553232eull },        { "vadd_i32", 0x86155d9e444cd3f4ull },
	{ "vadd_bounds", 0x2f7e701fd70bfe49ull }, { "saxpy", 0x0a638a1562db4a85ull },
	{ "matmul_naive", 0xcf1cc1653f9d0535ull }, { "matmul_tiled", 0xcf1cc1653f9d0535ull },
};

/* Bytes Metal reports as staticThreadgroupMemoryLength (OBSERVED). */
static uint32_t shared_for(const char *name)
{
	return strcmp(name, "matmul_tiled") == 0 ? 2048 : 0;
}

/* ---- submit hooks ---- */

enum { HOOK_RUN, HOOK_DEFER };
static int hook_mode;
static void (*tamper)(void);
static uint8_t last_msg[AGX_RWQ_SIZE];
static int last_retired;

static int submit_hook(void *ctx, const uint8_t *msg)
{
	(void)ctx;
	memcpy(last_msg, msg, sizeof(last_msg));
	if (hook_mode == HOOK_DEFER)
		return 0;
	if (tamper)
		tamper();
	last_retired = agx_sim_run_work_queue(&sim, msg);
	return last_retired < 0 ? -1 : 0;
}

/* CPU pointer for a VA in a context, as the device would see it. */
static uint8_t *dev_ptr(unsigned slot, uint64_t va, uint64_t len, unsigned access)
{
	uint64_t pa;
	if (agx_uat_translate(&mem, dev.ttbat_pa, slot, va, len, access, &pa) != AGX_UAT_OK)
		return NULL;
	return agx_mem_ptr(&mem, pa, len);
}

/* ---- helpers ---- */

static uint8_t *read_corpus(const char *name, uint32_t *len)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.bin", corpus, name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("cannot open %s\n", path);
		exit(2);
	}
	static uint8_t buf[8][4096];
	static int next;
	uint8_t *b = buf[next++ % 8];
	*len = (uint32_t)fread(b, 1, sizeof(buf[0]), f);
	fclose(f);
	return b;
}

static const struct agx_case_desc *find_case(const char *name)
{
	for (size_t i = 0; i < AGX_NCASES; i++)
		if (strcmp(agx_cases[i].name, name) == 0)
			return &agx_cases[i];
	return NULL;
}

struct bufs {
	uint64_t va[4];
	uint32_t *a, *b, *o;
	unsigned nbuf;
	uint32_t konst;
	int has_const;
};

static uint32_t *alloc_words(struct agx_vm *vm, uint32_t words, uint64_t prot, uint64_t *va)
{
	uint8_t *p;
	int err = agx_vm_alloc(&dev, vm, 4ull * words, prot, va, &p);
	if (err) {
		printf("agx_vm_alloc: %d\n", err);
		exit(2);
	}
	return (uint32_t *)p;
}

/* Metal argument table for each case (the buffer(k) indices in the corpus .metal files). */
static void make_bufs(struct agx_vm *vm, const struct agx_case_desc *c, struct bufs *bf, uint64_t out_prot)
{
	uint32_t w = agx_case_words(c);
	uint64_t va_a, va_b, va_o, va_k;
	bf->a = alloc_words(vm, w, AGX_PROT_GPU_SHARED_RO, &va_a);
	bf->b = alloc_words(vm, w, AGX_PROT_GPU_SHARED_RO, &va_b);
	bf->o = alloc_words(vm, w, out_prot, &va_o);
	agx_case_fill(c, bf->a, bf->b, bf->o);
	bf->has_const = 1;
	switch (c->kind) {
	case AGX_CASE_VADD_F32:
	case AGX_CASE_VADD_I32:
		bf->has_const = 0;
		bf->nbuf = 3;
		bf->va[0] = va_a, bf->va[1] = va_b, bf->va[2] = va_o;
		break;
	case AGX_CASE_SAXPY: /* x, y (read and written), alpha */
		bf->nbuf = 3;
		bf->va[0] = va_a, bf->va[1] = va_o;
		bf->konst = agx_case_u(AGX_CASE_SAXPY_ALPHA);
		break;
	default: /* a, b, c, n */
		bf->nbuf = 4;
		bf->va[0] = va_a, bf->va[1] = va_b, bf->va[2] = va_o;
		bf->konst = c->n;
		break;
	}
	if (bf->has_const) {
		uint32_t *k = alloc_words(vm, 1, AGX_PROT_GPU_SHARED_RO, &va_k);
		*k = bf->konst;
		bf->va[bf->nbuf - 1] = va_k;
	}
}

static void make_job(const struct agx_case_desc *c, struct bufs *bf, struct agx_compute *job)
{
	memset(job, 0, sizeof(*job));
	job->code = read_corpus(c->name, &job->code_len);
	job->buf_va = bf->va;
	job->nbuf = bf->nbuf;
	job->consts = &bf->konst;
	job->nconst = bf->has_const ? 1 : 0;
	job->shared_bytes = shared_for(c->name);
	memcpy(job->grid, c->grid, sizeof(job->grid));
	memcpy(job->local, c->tg, sizeof(job->local));
}

/* Submit and run; returns the stamp value, checks the job was accepted. */
static uint32_t run_job(struct agx_vm *vm, const struct agx_compute *job)
{
	uint32_t value = 0;
	int err = agx_compute_submit(&dev, vm, job, &value);
	CHECK(err == AGX_JOB_OK, "agx_compute_submit: %d", err);
	return value;
}

static void setup(uint32_t ring)
{
	memset(dram, 0, DRAM_BYTES);
	int err = agx_dev_init(&dev, &mem, DRAM_PA, DRAM_PA + DRAM_BYTES, ring, submit_hook, NULL);
	if (err || agx_sim_init(&sim, &mem, dev.ttbat_pa)) {
		printf("setup: %d\n", err);
		exit(2);
	}
	hook_mode = HOOK_RUN;
	tamper = NULL;
}

/* ---- tests ---- */

static uint32_t ops_seen[256];

static void test_corpus(void)
{
	for (size_t i = 0; i < sizeof(gpu_hash) / sizeof(gpu_hash[0]); i++) {
		const struct agx_case_desc *c = find_case(gpu_hash[i].name);
		struct agx_vm vm;
		struct bufs bf;
		struct agx_compute job;
		setup(4);
		CHECK(agx_vm_create(&dev, &vm, 1) == AGX_JOB_OK, "vm");
		make_bufs(&vm, c, &bf, AGX_PROT_GPU_SHARED_RW);
		make_job(c, &bf, &job);
		uint32_t w = agx_case_words(c);
		uint32_t *ref = malloc(4ull * w), *a = malloc(4ull * w), *b = malloc(4ull * w);
		agx_case_fill(c, a, b, ref);
		agx_case_reference(c, a, b, ref);

		uint32_t value = run_job(&vm, &job);
		int done = agx_compute_done(&dev, value);
		uint64_t h = agx_case_hash(bf.o, w);
		uint32_t bad = 0, first = w;
		for (uint32_t k = 0; k < w; k++)
			if (bf.o[k] != ref[k] && bad++ == 0)
				first = k;
		CHECK(done && sim.fault.kind == AGX_SIM_FAULT_NONE, "%s: done %d fault %s (%s) pc 0x%" PRIx64, c->name,
		      done, agx_sim_fault_name(sim.fault.kind), sim.fault.what ? sim.fault.what : "",
		      sim.fault.pc);
		CHECK(bad == 0, "%s: %u words differ from the C reference, first [%u] = 0x%08x want 0x%08x", c->name,
		      bad, first, first < w ? bf.o[first] : 0, first < w ? ref[first] : 0);
		CHECK(h == gpu_hash[i].hash, "%s: hash %016" PRIx64 " want the real GPU's %016" PRIx64, c->name, h,
		      gpu_hash[i].hash);
		CHECK(agx_queue_errors(&dev) == 0, "%s: queue errors", c->name);
		printf("sim %-13s %016" PRIx64 " (real GPU %016" PRIx64 ") %s, %" PRIu64 " threadgroups, %" PRIu64
		       " SIMD-group instructions\n",
		       c->name, h, gpu_hash[i].hash, h == gpu_hash[i].hash ? "match" : "MISMATCH", sim.threadgroups,
		       sim.instructions);
		for (unsigned k = 0; k < 256; k++)
			ops_seen[k] += sim.op_hits[k];
		free(ref);
		free(a);
		free(b);
	}
}

/* One vadd job in a fresh device; returns the submitted stamp. */
static uint32_t vadd_job(struct agx_vm *vm, struct bufs *bf, struct agx_compute *job, uint64_t out_prot)
{
	const struct agx_case_desc *c = find_case("vadd");
	make_bufs(vm, c, bf, out_prot);
	make_job(c, bf, job);
	uint32_t value = 0;
	CHECK(agx_compute_submit(&dev, vm, job, &value) == AGX_JOB_OK, "submit");
	return value;
}

static void expect_fault(const char *what, uint32_t value, int kind)
{
	CHECK(!agx_compute_done(&dev, value), "%s: stamp written for a faulted job", what);
	CHECK(sim.fault.kind == kind, "%s: fault %s (%s), want %s", what, agx_sim_fault_name(sim.fault.kind),
	      sim.fault.what ? sim.fault.what : "", agx_sim_fault_name(kind));
	CHECK(agx_queue_errors(&dev) >= 1, "%s: error_count not raised", what);
	CHECK(sim.nevents && sim.events[(sim.nevents - 1) % AGX_SIM_EVENTS].error == 1, "%s: no error event", what);
}

static void test_mmu_faults(void)
{
	struct agx_vm vm, vm2;
	struct bufs bf;
	struct agx_compute job;

	/* Output buffer pointer into the guard page after it: shader store faults. */
	setup(4);
	agx_vm_create(&dev, &vm, 1);
	const struct agx_case_desc *c = find_case("vadd");
	make_bufs(&vm, c, &bf, AGX_PROT_GPU_SHARED_RW);
	make_job(c, &bf, &job);
	bf.va[2] += AGX_UAT_PGSZ;
	uint32_t v = run_job(&vm, &job);
	expect_fault("unmapped output", v, AGX_SIM_FAULT_MMU);
	CHECK(sim.fault.uat_err == AGX_UAT_E_UNMAPPED && sim.fault.access == AGX_ACC_GPU_W &&
		      sim.fault.va == bf.va[2] && sim.fault.slot == 1,
	      "unmapped output: err %d access %u va 0x%" PRIx64, sim.fault.uat_err, sim.fault.access, sim.fault.va);
	/* The queue keeps going after a fault. */
	bf.va[2] -= AGX_UAT_PGSZ;
	v = run_job(&vm, &job);
	CHECK(agx_compute_done(&dev, v) && sim.fault.kind == AGX_SIM_FAULT_NONE, "job after a fault");
	CHECK(agx_queue_errors(&dev) == 1, "error_count %u", agx_queue_errors(&dev));

	/* Store into a buffer mapped read-only to the GPU. */
	setup(4);
	agx_vm_create(&dev, &vm, 1);
	v = vadd_job(&vm, &bf, &job, AGX_PROT_GPU_SHARED_RO);
	expect_fault("read-only output", v, AGX_SIM_FAULT_MMU);
	CHECK(sim.fault.uat_err == AGX_UAT_E_PERM, "read-only output: err %d", sim.fault.uat_err);
	CHECK(bf.o[0] == AGX_CASE_SENTINEL, "read-only output was written");

	/* Isolation: VM 2 cannot reach VM 1's buffers at the same addresses. */
	setup(4);
	agx_vm_create(&dev, &vm, 1);
	agx_vm_create(&dev, &vm2, 2);
	make_bufs(&vm, c, &bf, AGX_PROT_GPU_SHARED_RW);
	make_job(c, &bf, &job);
	uint64_t keep = vm2.va_next;
	vm2.va_next = 0x40000000; /* VM 2's own allocations stay away from VM 1's addresses */
	v = run_job(&vm2, &job);
	vm2.va_next = keep;
	expect_fault("other VM's buffer", v, AGX_SIM_FAULT_MMU);
	CHECK(sim.fault.slot == 2 && sim.fault.access == AGX_ACC_GPU_R, "isolation: slot %u access %u", sim.fault.slot,
	      sim.fault.access);
	CHECK(bf.o[0] == AGX_CASE_SENTINEL, "isolation: VM 1's output changed");
}

static uint8_t patched[4096];

static void test_refused_encodings(void)
{
	struct agx_vm vm;
	struct bufs bf;
	struct agx_compute job;
	const struct agx_case_desc *c = find_case("vadd");
	uint32_t v;

	/* vadd's first load with bit 26 clear (b26clr: meaning UNKNOWN). */
	setup(4);
	agx_vm_create(&dev, &vm, 1);
	make_bufs(&vm, c, &bf, AGX_PROT_GPU_SHARED_RW);
	make_job(c, &bf, &job);
	memcpy(patched, job.code, job.code_len);
	CHECK(patched[7] & 0x04, "vadd load bit 26 not where expected");
	patched[7] &= (uint8_t)~0x04;
	job.code = patched;
	v = run_job(&vm, &job);
	expect_fault("b26clr", v, AGX_SIM_FAULT_UNSUPPORTED);
	CHECK(sim.fault.pc == 4, "b26clr: pc 0x%" PRIx64, sim.fault.pc);

	/* matmul_tiled's first lstore with tgu = 2 (UNKNOWN). */
	const struct agx_case_desc *t = find_case("matmul_tiled");
	setup(4);
	agx_vm_create(&dev, &vm, 1);
	make_bufs(&vm, t, &bf, AGX_PROT_GPU_SHARED_RW);
	make_job(t, &bf, &job);
	memcpy(patched, job.code, job.code_len);
	CHECK((patched[0xa8 + 5] & 7) == 1, "lstore tgu not where expected");
	patched[0xa8 + 5] = (uint8_t)((patched[0xa8 + 5] & ~7) | 2);
	job.code = patched;
	v = run_job(&vm, &job);
	expect_fault("tgu 2", v, AGX_SIM_FAULT_UNSUPPORTED);
	CHECK(sim.fault.pc == 0xa8, "tgu: pc 0x%" PRIx64, sim.fault.pc);

	/* matmul_tiled with less threadgroup memory than Metal reports: the
	 * element-unit reading of lload/lstore indexes needs all 2048 bytes. */
	setup(4);
	agx_vm_create(&dev, &vm, 1);
	make_bufs(&vm, t, &bf, AGX_PROT_GPU_SHARED_RW);
	make_job(t, &bf, &job);
	job.shared_bytes = 2048 - 256;
	v = run_job(&vm, &job);
	expect_fault("shared 1792", v, AGX_SIM_FAULT_BOUNDS);

	/* An endless loop: jmp_any to itself, then stop (for the scanner). */
	static const uint8_t spin[] = { 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x88, 0x00 };
	setup(4);
	agx_vm_create(&dev, &vm, 1);
	make_bufs(&vm, c, &bf, AGX_PROT_GPU_SHARED_RW);
	make_job(c, &bf, &job);
	job.code = spin;
	job.code_len = sizeof(spin);
	sim.step_limit = 100000;
	v = run_job(&vm, &job);
	expect_fault("spin", v, AGX_SIM_FAULT_WATCHDOG);

	/* Undecodable bytes are refused by the driver before submission. */
	static const uint8_t junk[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	job.code = junk;
	job.code_len = sizeof(junk);
	CHECK(agx_compute_submit(&dev, &vm, &job, &v) == AGX_JOB_E_SHADER, "junk shader accepted");
	/* A shader using threadgroup memory without any allocated. */
	make_job(t, &bf, &job);
	job.shared_bytes = 0;
	CHECK(agx_compute_submit(&dev, &vm, &job, &v) == AGX_JOB_E_SHADER, "lload without shared memory accepted");
}

/* Tampering with what the driver built, between submit and the firmware. */
static uint8_t *cur_rc(void)
{
	uint32_t w = agx_rd32(dev.rs + AGX_RS_CPU_WPTR);
	uint32_t slot = (w + dev.ring_size - 1) % dev.ring_size;
	return dev_ptr(0, agx_rd64(dev.ring + 8ull * slot), AGX_RC_SIZE, AGX_ACC_FW_R);
}

static uint8_t *cur_usc_word(uint8_t tag)
{
	uint8_t *rc = cur_rc();
	uint64_t usc = agx_rd64(rc + AGX_RC_JOB_PARAMS1 + AGX_JP1_USC_EXEC_BASE);
	uint64_t cdm = agx_rd64(rc + AGX_RC_JOB_PARAMS1 + AGX_JP1_CDM_BASE);
	uint8_t *c = dev_ptr(1, cdm, 32, AGX_ACC_GPU_R);
	uint8_t *w = dev_ptr(1, usc + agx_rd32(c + 4), 128, AGX_ACC_GPU_R);
	for (int at = 0; at < 128;) {
		if (w[at] == tag)
			return w + at;
		at += w[at] == AGX_USC_UNIFORM ? 8 : w[at] == AGX_USC_SHADER ? 6 : w[at] == AGX_USC_NO_PRESHADER ? 2 : 4;
	}
	return NULL;
}

static void tamper_preshader(void) { *cur_usc_word(AGX_USC_NO_PRESHADER) = AGX_USC_PRESHADER; }
static void tamper_registers(void) { cur_usc_word(AGX_USC_REGISTERS)[1] = 1; /* 8 registers */ }
static void tamper_microseq(void)
{
	uint8_t *rc = cur_rc();
	agx_wr32(dev_ptr(0, agx_rd64(rc + AGX_RC_MICROSEQ), 4, AGX_ACC_FW_R), 0x12345678);
}
static void tamper_indirect(void)
{
	uint8_t *rc = cur_rc();
	uint8_t *c = dev_ptr(1, agx_rd64(rc + AGX_RC_JOB_PARAMS1 + AGX_JP1_CDM_BASE), 4, AGX_ACC_GPU_R);
	agx_wr32(c, agx_rd32(c) | 1u << 27);
}
static void tamper_terminate(void)
{
	uint8_t *rc = cur_rc();
	uint8_t *c = dev_ptr(1, agx_rd64(rc + AGX_RC_JOB_PARAMS1 + AGX_JP1_CDM_BASE), 40, AGX_ACC_GPU_R);
	agx_wr32(c + 36, AGX_CDM_BARRIER << 29);
}

static void test_command_faults(void)
{
	static const struct {
		const char *what;
		void (*fn)(void);
		const char *kase;
		int kind;
	} t[] = {
		{ "preshader", tamper_preshader, "vadd", AGX_SIM_FAULT_UNSUPPORTED },
		{ "registers below use", tamper_registers, "matmul_tiled", AGX_SIM_FAULT_BOUNDS },
		{ "microsequence header", tamper_microseq, "vadd", AGX_SIM_FAULT_COMMAND },
		{ "indirect launch", tamper_indirect, "vadd", AGX_SIM_FAULT_UNSUPPORTED },
		{ "no terminate", tamper_terminate, "vadd", AGX_SIM_FAULT_COMMAND },
	};
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		struct agx_vm vm;
		struct bufs bf;
		struct agx_compute job;
		const struct agx_case_desc *c = find_case(t[i].kase);
		setup(4);
		agx_vm_create(&dev, &vm, 1);
		make_bufs(&vm, c, &bf, AGX_PROT_GPU_SHARED_RW);
		make_job(c, &bf, &job);
		tamper = t[i].fn;
		uint32_t v = run_job(&vm, &job);
		tamper = NULL;
		expect_fault(t[i].what, v, t[i].kind);
	}
}

static void test_ring(void)
{
	struct agx_vm vm;
	struct bufs bf;
	struct agx_compute job;
	uint32_t v[3], x;

	/* Ten jobs through a four-entry ring: wraps twice, stamps in order. */
	setup(4);
	agx_vm_create(&dev, &vm, 1);
	for (int i = 0; i < 10; i++) {
		x = vadd_job(&vm, &bf, &job, AGX_PROT_GPU_SHARED_RW);
		CHECK(x == (uint32_t)i + 1 && agx_compute_done(&dev, x) && !agx_compute_done(&dev, x + 1),
		      "ring job %d: stamp %u", i, x);
		CHECK(bf.o[5] == agx_case_u(agx_case_f(bf.a[5]) + agx_case_f(bf.b[5])), "ring job %d result", i);
	}
	CHECK(agx_rd32(dev.rs + AGX_RS_GPU_RPTR) == 10 % 4 && agx_rd32(dev.rs + AGX_RS_GPU_DONEPTR) == 10 % 4,
	      "rptr %u", agx_rd32(dev.rs + AGX_RS_GPU_RPTR));
	CHECK(sim.jobs == 10 && sim.jobs_faulted == 0, "sim jobs %" PRIu64, sim.jobs);

	/* Firmware not running yet: three fit, the fourth finds the ring full;
	 * one doorbell then retires all three. */
	setup(4);
	agx_vm_create(&dev, &vm, 1);
	hook_mode = HOOK_DEFER;
	for (int i = 0; i < 3; i++)
		v[i] = vadd_job(&vm, &bf, &job, AGX_PROT_GPU_SHARED_RW);
	CHECK(agx_compute_submit(&dev, &vm, &job, &x) == AGX_JOB_E_RING_FULL, "fourth job accepted into a full ring");
	CHECK(!agx_compute_done(&dev, v[0]), "stamp before the firmware ran");
	CHECK(agx_sim_run_work_queue(&sim, last_msg) == 3, "doorbell did not retire 3");
	CHECK(agx_compute_done(&dev, v[2]), "stamps after the doorbell");

	/* A doorbell whose wptr disagrees with the ring is refused. */
	uint8_t bad[AGX_RWQ_SIZE];
	memcpy(bad, last_msg, sizeof(bad));
	agx_wr32(bad + AGX_RWQ_WPTR, 1);
	CHECK(agx_sim_run_work_queue(&sim, bad) < 0 && sim.fault.kind == AGX_SIM_FAULT_COMMAND, "bad wptr accepted");
	agx_wr32(bad + AGX_RWQ_WPTR, agx_rd32(last_msg + AGX_RWQ_WPTR));
	agx_wr32(bad + AGX_RWQ_PIPE_TYPE, 0);
	CHECK(agx_sim_run_work_queue(&sim, bad) < 0 && sim.fault.kind == AGX_SIM_FAULT_UNSUPPORTED,
	      "vertex pipe accepted");
}

static void test_uat(void)
{
	struct agx_vm vm;
	uint64_t va, pa;
	uint8_t *p;
	setup(4);
	CHECK(agx_vm_create(&dev, &vm, 1) == AGX_JOB_OK, "vm");
	CHECK(agx_vm_create(&dev, &vm, 1) == AGX_UAT_E_BUSY, "slot reused");
	CHECK(agx_vm_create(&dev, &vm, 0) == AGX_JOB_E_INVAL, "slot 0 is the firmware's");
	agx_vm_alloc(&dev, &vm, 3 * AGX_UAT_PGSZ, AGX_PROT_GPU_SHARED_RW, &va, &p);
	CHECK(agx_uat_translate(&mem, dev.ttbat_pa, 1, va + 2 * AGX_UAT_PGSZ + 8, 8, AGX_ACC_GPU_W, &pa) ==
			  AGX_UAT_OK &&
		      agx_mem_ptr(&mem, pa, 8) == p + 2 * AGX_UAT_PGSZ + 8,
	      "translate");
	CHECK(agx_uat_translate(&mem, dev.ttbat_pa, 1, va + AGX_UAT_PGSZ - 4, 8, AGX_ACC_GPU_R, &pa) == AGX_UAT_E_RANGE,
	      "translate across a page");
	CHECK(agx_uat_translate(&mem, dev.ttbat_pa, 1, va, 8, AGX_ACC_FW_R, &pa) == AGX_UAT_E_PERM,
	      "firmware read of GPU-only memory");
	CHECK(agx_uat_translate(&mem, dev.ttbat_pa, 3, va, 8, AGX_ACC_GPU_R, &pa) == AGX_UAT_E_SLOT, "unbound slot");
	CHECK(agx_uat_translate(&mem, dev.ttbat_pa, 1, va + 3 * AGX_UAT_PGSZ, 8, AGX_ACC_GPU_R, &pa) ==
		      AGX_UAT_E_UNMAPPED,
	      "guard page");
	CHECK(agx_uat_translate(&mem, dev.ttbat_pa, 1, dev.qi_va, 8, AGX_ACC_GPU_R, &pa) == AGX_UAT_E_PERM,
	      "GPU read of firmware memory");
	CHECK(agx_uat_translate(&mem, dev.ttbat_pa, 1, dev.qi_va, 8, AGX_ACC_FW_W, &pa) == AGX_UAT_OK,
	      "firmware memory through a user slot's ttb1");
	CHECK(agx_uat_map(&vm.uat, &dev.pages, va, DRAM_PA, AGX_UAT_PGSZ, AGX_PROT_GPU_SHARED_RW) == AGX_UAT_E_BUSY,
	      "map over a mapping");
	CHECK(agx_uat_map(&vm.uat, &dev.pages, va + 1, DRAM_PA, AGX_UAT_PGSZ, AGX_PROT_GPU_SHARED_RW) ==
		      AGX_UAT_E_ALIGN,
	      "unaligned map");
	CHECK(agx_uat_map(&vm.uat, &dev.pages, AGX_UAT_USER_TOP, DRAM_PA, AGX_UAT_PGSZ, AGX_PROT_GPU_SHARED_RW) ==
		      AGX_UAT_E_RANGE,
	      "user map past 39 bits");
	CHECK(agx_uat_map(&dev.kern, &dev.pages, AGX_UAT_KERN_TOP - AGX_UAT_PGSZ, DRAM_PA, 2 * AGX_UAT_PGSZ,
			  AGX_PROT_FW_SHARED_RW) == AGX_UAT_E_RANGE,
	      "kernel map past its range");
	CHECK(agx_uat_map(&vm.uat, &dev.pages, 0x7000000000ull, DRAM_PA + DRAM_BYTES, AGX_UAT_PGSZ,
			  AGX_PROT_GPU_SHARED_RW) == AGX_UAT_E_RANGE,
	      "map of memory outside DRAM");
	CHECK(agx_uat_unmap(&vm.uat, &mem, va + AGX_UAT_PGSZ, AGX_UAT_PGSZ) == AGX_UAT_OK, "unmap");
	CHECK(agx_uat_translate(&mem, dev.ttbat_pa, 1, va + AGX_UAT_PGSZ, 8, AGX_ACC_GPU_R, &pa) == AGX_UAT_E_UNMAPPED,
	      "translate after unmap");
	CHECK(agx_uat_translate(&mem, dev.ttbat_pa, 1, va, 8, AGX_ACC_GPU_R, &pa) == AGX_UAT_OK, "neighbour kept");
	CHECK(agx_uat_map(&vm.uat, &dev.pages, va + AGX_UAT_PGSZ, DRAM_PA, AGX_UAT_PGSZ, AGX_PROT_GPU_SHARED_RO) ==
		      AGX_UAT_OK,
	      "remap");
	CHECK(agx_uat_translate(&mem, dev.ttbat_pa, 1, va + AGX_UAT_PGSZ, 8, AGX_ACC_GPU_W, &pa) == AGX_UAT_E_PERM,
	      "write to the read-only remap");

	/* Rights per protection, as drm/asahi names them (INFERRED, row U2). */
	CHECK(agx_pte_rights(AGX_PROT_GPU_SHARED_RW) == (AGX_ACC_GPU_R | AGX_ACC_GPU_W), "GPU_SHARED_RW");
	CHECK(agx_pte_rights(AGX_PROT_GPU_SHARED_RO) == AGX_ACC_GPU_R, "GPU_SHARED_RO");
	CHECK(agx_pte_rights(AGX_PROT_FW_SHARED_RW) == (AGX_ACC_FW_R | AGX_ACC_FW_W), "FW_SHARED_RW");
	CHECK(agx_pte_rights(AGX_PROT_GPU_RO_FW_PRIV_RW) == (AGX_ACC_GPU_R | AGX_ACC_FW_R | AGX_ACC_FW_W),
	      "GPU_RO_FW_PRIV_RW");
	CHECK(agx_pte_rights(AGX_PROT_GPU_FW_SHARED_RW) == (AGX_ACC_GPU_R | AGX_ACC_GPU_W | AGX_ACC_FW_R | AGX_ACC_FW_W),
	      "GPU_FW_SHARED_RW");

	/* Allocator exhaustion is an error, not a wrap. */
	struct agx_pages pg;
	CHECK(agx_pages_init(&pg, &mem, DRAM_PA, DRAM_PA + AGX_UAT_PGSZ) == AGX_UAT_OK &&
		      agx_pages_alloc(&pg, 1, &pa) == AGX_UAT_OK && agx_pages_alloc(&pg, 1, &pa) == AGX_UAT_E_NOMEM,
	      "allocator exhaustion");
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <corpus dir>\n", argv[0]);
		return 2;
	}
	corpus = argv[1];
	dram = malloc(DRAM_BYTES);
	if (!dram)
		return 2;
	mem = (struct agx_mem){ dram, DRAM_PA, DRAM_BYTES };

	test_corpus();
	test_mmu_faults();
	test_refused_encodings();
	test_command_faults();
	test_ring();
	test_uat();

	unsigned distinct = 0;
	for (unsigned k = 0; k < agx_ins_count; k++)
		distinct += ops_seen[k] != 0;
	printf("AGX_SIM_OPS: %u of %u table entries executed by the corpus:", distinct, agx_ins_count);
	for (unsigned k = 0; k < agx_ins_count; k++)
		if (ops_seen[k])
			printf(" %s", agx_ins_table[k].name);
	printf("\n");
	free(dram);
	if (failures) {
		printf("AIENOS_AGX_SIM: FAIL (%d of %d checks)\n", failures, checks);
		return 1;
	}
	printf("AIENOS_AGX_SIM: PASS (%d checks)\n", checks);
	return 0;
}
