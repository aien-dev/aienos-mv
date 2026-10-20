/*
 * Simulated AGX: firmware work-queue model, CDM/USC parsing and an
 * interpreter for the shader ISA subset vector add and matmul need.
 * Freestanding. Overview and limits: agx_sim.h.
 *
 * Claim tags used below:
 *   OBSERVED  checked against this Mac's GPU (make gpu-ref hashes, Metal
 *             pipeline properties) or Apple's compiler output;
 *   INFERRED  from the named source;
 *   UNKNOWN   refused with AGX_SIM_FAULT_UNSUPPORTED.
 */
#include "agx_sim.h"

enum kind {
	K_NONE = 0,
	K_MFSR, K_LDIMM, K_MOV,
	K_IADD, K_ISUB, K_IMUL, K_IMADD, K_IMSUB, K_ICSEL, K_FCSEL,
	K_FADD, K_FMUL, K_FFMA,
	K_IF_ICMP, K_IF_FCMP, K_WHILE_ICMP, K_WHILE_FCMP, K_POP_EXEC, K_JMP_ANY, K_JMP_NONE,
	K_WAIT, K_BARRIER, K_STOP,
	K_LOAD, K_STORE, K_LLOAD, K_LSTORE,
};

static const struct {
	const char *name;
	uint8_t kind;
} kind_names[] = {
	{ "mfsr", K_MFSR }, { "ldimm16", K_LDIMM }, { "ldimm32", K_LDIMM }, { "mov", K_MOV },
	{ "iadd", K_IADD }, { "isub", K_ISUB }, { "imul", K_IMUL }, { "imadd", K_IMADD }, { "imsub", K_IMSUB },
	{ "icsel", K_ICSEL }, { "fcsel", K_FCSEL }, { "fadd", K_FADD }, { "fmul", K_FMUL }, { "ffma", K_FFMA },
	{ "if_icmp", K_IF_ICMP }, { "if_fcmp", K_IF_FCMP }, { "while_icmp", K_WHILE_ICMP },
	{ "while_fcmp", K_WHILE_FCMP }, { "pop_exec", K_POP_EXEC }, { "jmp_any", K_JMP_ANY },
	{ "jmp_none", K_JMP_NONE }, { "wait", K_WAIT }, { "barrier", K_BARRIER }, { "stop", K_STOP },
	{ "load", K_LOAD }, { "store", K_STORE }, { "lload", K_LLOAD }, { "lstore", K_LSTORE },
};

enum { SG_RUN = 0, SG_BARRIER, SG_DONE };

static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void fill(uint8_t *p, uint8_t v, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++)
		p[i] = v;
}

const char *agx_sim_fault_name(int kind)
{
	switch (kind) {
	case AGX_SIM_FAULT_NONE: return "none";
	case AGX_SIM_FAULT_MMU: return "mmu";
	case AGX_SIM_FAULT_DECODE: return "decode";
	case AGX_SIM_FAULT_UNSUPPORTED: return "unsupported";
	case AGX_SIM_FAULT_BOUNDS: return "bounds";
	case AGX_SIM_FAULT_WATCHDOG: return "watchdog";
	case AGX_SIM_FAULT_COMMAND: return "command";
	}
	return "?";
}

int agx_sim_init(struct agx_sim *s, const struct agx_mem *mem, uint64_t ttbat_pa)
{
	if (!agx_mem_ptr(mem, ttbat_pa, AGX_UAT_NUM_CTX * AGX_TTBAT_SLOT_BYTES) || agx_ins_count > 256)
		return -1;
	fill((uint8_t *)s, 0, sizeof(*s));
	s->mem = *mem;
	s->ttbat_pa = ttbat_pa;
	s->step_limit = AGX_SIM_DEFAULT_STEP_LIMIT;
	for (unsigned i = 0; i < agx_ins_count; i++)
		for (unsigned k = 0; k < sizeof(kind_names) / sizeof(kind_names[0]); k++)
			if (str_eq(agx_ins_table[i].name, kind_names[k].name))
				s->kind[i] = kind_names[k].kind;
	return 0;
}

/* ---- faults ---- */

static void fault(struct agx_sim *s, int kind, const char *what)
{
	if (s->faulted)
		return;
	s->faulted = 1;
	s->fault.kind = kind;
	s->fault.what = what;
	s->fault.op = -1;
}

static void fault_mmu(struct agx_sim *s, unsigned slot, uint64_t va, unsigned access, int err, const char *what)
{
	if (s->faulted)
		return;
	fault(s, AGX_SIM_FAULT_MMU, what);
	s->fault.slot = slot;
	s->fault.va = va;
	s->fault.access = access;
	s->fault.uat_err = err;
}

/* ---- memory through the UAT ---- */

static int xfer(struct agx_sim *s, unsigned slot, uint64_t va, uint8_t *buf, uint64_t len, unsigned access,
		const char *what)
{
	while (len) {
		uint64_t chunk = AGX_UAT_PGSZ - (va & (AGX_UAT_PGSZ - 1));
		if (chunk > len)
			chunk = len;
		uint64_t pa;
		int err = agx_uat_translate(&s->mem, s->ttbat_pa, slot, va, chunk, access, &pa);
		if (err != AGX_UAT_OK) {
			fault_mmu(s, slot, va, access, err, what);
			return -1;
		}
		uint8_t *p = agx_mem_ptr(&s->mem, pa, chunk);
		for (uint64_t i = 0; i < chunk; i++) {
			if (access & (AGX_ACC_GPU_W | AGX_ACC_FW_W))
				p[i] = buf[i];
			else
				buf[i] = p[i];
		}
		va += chunk;
		buf += chunk;
		len -= chunk;
	}
	return 0;
}

static int fw_read(struct agx_sim *s, uint64_t va, uint8_t *buf, uint64_t len, const char *what)
{
	return xfer(s, 0, va, buf, len, AGX_ACC_FW_R, what);
}

static int fw_write32(struct agx_sim *s, uint64_t va, uint32_t v, const char *what)
{
	uint8_t b[4];
	agx_wr32(b, v);
	return xfer(s, 0, va, b, 4, AGX_ACC_FW_W, what);
}

static int gpu_read(struct agx_sim *s, uint64_t va, uint8_t *buf, uint64_t len, const char *what)
{
	return xfer(s, s->slot, va, buf, len, AGX_ACC_GPU_R, what);
}

/* ---- half precision (IEEE binary16), for 16-bit float operands ---- */

static uint32_t f32_bits(float f)
{
	union { float f; uint32_t u; } x = { f };
	return x.u;
}

static float bits_f32(uint32_t u)
{
	union { uint32_t u; float f; } x = { u };
	return x.f;
}

static float half_to_f32(uint16_t h)
{
	uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
	if (exp == 0x1f)
		return bits_f32(sign | 0x7f800000u | man << 13);
	if (exp == 0) {
		float v = (float)man * (1.0f / 16777216.0f); /* man * 2^-24 */
		return sign ? -v : v;
	}
	return bits_f32(sign | (exp + 112) << 23 | man << 13);
}

static uint16_t f32_to_half(float f)
{
	/* Round to nearest even. */
	uint32_t u = f32_bits(f), sign = (u >> 16) & 0x8000;
	int32_t exp = (int32_t)((u >> 23) & 0xff);
	uint32_t man = u & 0x7fffff;
	if (exp == 0xff)
		return (uint16_t)(sign | 0x7c00 | (man ? 0x200 : 0));
	int32_t e = exp - 127 + 15;
	if (e >= 0x1f)
		return (uint16_t)(sign | 0x7c00);
	if (e <= 0) {
		if (e < -10)
			return (uint16_t)sign;
		man |= 0x800000;
		uint32_t shift = (uint32_t)(14 - e);
		uint32_t h = man >> shift, rem = man & ((1u << shift) - 1), half = 1u << (shift - 1);
		if (rem > half || (rem == half && (h & 1)))
			h++;
		return (uint16_t)(sign | h);
	}
	uint32_t h = (uint32_t)e << 10 | man >> 13, rem = man & 0x1fff;
	if (rem > 0x1000 || (rem == 0x1000 && (h & 1)))
		h++;
	return (uint16_t)(sign | h);
}

/* ---- operands ---- */

#define MAXOPS 6

struct decoded {
	const struct agx_ins_desc *d;
	struct agx_ins ins;
	struct agx_operand dst[2], src[MAXOPS];
	unsigned ndst, nsrc;
};

/* Immediate or modifier field by name (agx_find_field covers modifiers only). */
static int field_val(const struct decoded *x, const char *name, uint64_t *v)
{
	for (unsigned f = 0; f < x->d->nfields; f++) {
		const struct agx_field *fd = &x->d->fields[f];
		if ((fd->type == AGX_FT_IMM || fd->type == AGX_FT_MOD) && str_eq(fd->name, name)) {
			*v = x->ins.val[f];
			return 0;
		}
	}
	return -1;
}

static uint64_t field_or(const struct decoded *x, const char *name, uint64_t dflt)
{
	uint64_t v;
	return field_val(x, name, &v) ? dflt : v;
}

static uint64_t sext(uint64_t v, unsigned bits)
{
	if (bits >= 64)
		return v;
	uint64_t m = 1ull << (bits - 1);
	v &= (1ull << bits) - 1;
	return (v ^ m) - m;
}

/* Halves an operand covers. */
static unsigned op_halves(const struct agx_operand *o)
{
	return (unsigned)(o->size / 16) * o->count;
}

static int reg_ok(struct agx_sim *s, const struct agx_operand *o)
{
	uint64_t end = (uint64_t)o->base + op_halves(o);
	if (o->kind == AGX_OPND_REG && end <= s->gpr_limit)
		return 1;
	if (o->kind == AGX_OPND_UNIFORM && end <= s->uni_limit)
		return 1;
	fault(s, AGX_SIM_FAULT_BOUNDS, o->kind == AGX_OPND_REG ? "register past the USC Registers count"
							       : "uniform past the CDM launch uniform count");
	return 0;
}

/* Raw bits of element i of a register or uniform operand (16/32/64 bits). */
static uint64_t read_raw(struct agx_sim *s, struct agx_sim_sg *g, unsigned lane, const struct agx_operand *o,
			 unsigned i)
{
	const uint16_t *f = o->kind == AGX_OPND_REG ? g->r[lane] : s->uni;
	unsigned n = o->size / 16, at = (unsigned)o->base + i * n;
	uint64_t v = 0;
	for (unsigned k = 0; k < n; k++)
		v |= (uint64_t)f[at + k] << (16 * k);
	return v;
}

static void write_raw(struct agx_sim_sg *g, unsigned lane, const struct agx_operand *o, unsigned i, uint64_t v)
{
	unsigned n = o->size / 16, at = (unsigned)o->base + i * n;
	for (unsigned k = 0; k < n; k++)
		g->r[lane][at + k] = (uint16_t)(v >> (16 * k));
}

/* Integer value of a source, extended to 64 bits (sign-extended when the
 * operand carries sx, or when force_signed for signed compares). */
static int src_int(struct agx_sim *s, struct agx_sim_sg *g, unsigned lane, const struct agx_operand *o,
		   int force_signed, uint64_t *v)
{
	switch (o->kind) {
	case AGX_OPND_NONE:
		*v = 0;
		return 0;
	case AGX_OPND_IMM:
		*v = (uint64_t)o->base;
		break;
	case AGX_OPND_SIGNED:
		*v = sext((uint64_t)o->base, o->imm_bits);
		break;
	case AGX_OPND_REG:
	case AGX_OPND_UNIFORM:
		if (!reg_ok(s, o))
			return -1;
		*v = read_raw(s, g, lane, o, 0);
		if (o->sx || force_signed)
			*v = sext(*v, o->size);
		break;
	default:
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "integer operand kind");
		return -1;
	}
	if (o->abs) {
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "abs on an integer operand");
		return -1;
	}
	if (o->neg)
		*v = 0 - *v;
	return 0;
}

static int src_f32(struct agx_sim *s, struct agx_sim_sg *g, unsigned lane, const struct agx_operand *o, float *f)
{
	switch (o->kind) {
	case AGX_OPND_FIMM:
		*f = bits_f32(agx_fimm_to_f32((uint8_t)o->base));
		break;
	case AGX_OPND_REG:
	case AGX_OPND_UNIFORM:
		if (!reg_ok(s, o))
			return -1;
		if (o->size == 16)
			*f = half_to_f32((uint16_t)read_raw(s, g, lane, o, 0));
		else if (o->size == 32)
			*f = bits_f32((uint32_t)read_raw(s, g, lane, o, 0));
		else {
			fault(s, AGX_SIM_FAULT_UNSUPPORTED, "64-bit float operand");
			return -1;
		}
		break;
	default:
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "float operand kind");
		return -1;
	}
	if (o->abs)
		*f = bits_f32(f32_bits(*f) & 0x7fffffffu);
	if (o->neg)
		*f = bits_f32(f32_bits(*f) ^ 0x80000000u);
	return 0;
}

static int dst_int(struct agx_sim *s, struct agx_sim_sg *g, unsigned lane, const struct agx_operand *o, uint64_t v)
{
	if (o->kind == AGX_OPND_NONE)
		return 0; /* "_": result discarded */
	if (o->kind != AGX_OPND_REG) {
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "destination kind");
		return -1;
	}
	if (!reg_ok(s, o))
		return -1;
	write_raw(g, lane, o, 0, v);
	return 0;
}

static int dst_f32(struct agx_sim *s, struct agx_sim_sg *g, unsigned lane, const struct agx_operand *o, float f,
		   int saturate)
{
	if (saturate) /* clamp to [0, 1], NaN to 0 */
		f = (f > 0.0f) ? (f < 1.0f ? f : 1.0f) : 0.0f;
	if (o->kind == AGX_OPND_REG && o->size == 16)
		return dst_int(s, g, lane, o, f32_to_half(f));
	if (o->kind == AGX_OPND_REG && o->size != 32) {
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "64-bit float destination");
		return -1;
	}
	return dst_int(s, g, lane, o, f32_bits(f));
}

/* ---- conditions ---- */

/* icond: bits 0..1 = eq/lt/gt, bit 2 = signed, bit 3 = invert (Mesa AGX2.xml
 * enum values: ieq 0, ult 1, ugt 2, seq 4, slt 5, sgt 6, +8 inverts).
 * Compared at 32 bits unless an operand is 64-bit. INFERRED (Mesa). */
static int icmp(struct agx_sim *s, struct agx_sim_sg *g, unsigned lane, const struct agx_operand *a,
		const struct agx_operand *b, unsigned cond, int *out)
{
	unsigned base = cond & 3, sgn = (cond >> 2) & 1, inv = (cond >> 3) & 1;
	uint64_t x, y;
	if (base == 3) {
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "integer condition 3");
		return -1;
	}
	if (src_int(s, g, lane, a, sgn, &x) || src_int(s, g, lane, b, sgn, &y))
		return -1;
	int wide = a->size == 64 || b->size == 64;
	int r;
	if (sgn) {
		int64_t sx = wide ? (int64_t)x : (int64_t)sext(x, 32), sy = wide ? (int64_t)y : (int64_t)sext(y, 32);
		r = base == 0 ? sx == sy : base == 1 ? sx < sy : sx > sy;
	} else {
		uint64_t ux = wide ? x : (uint32_t)x, uy = wide ? y : (uint32_t)y;
		r = base == 0 ? ux == uy : base == 1 ? ux < uy : ux > uy;
	}
	*out = r ^ (int)inv;
	return 0;
}

/* fcond: low 3 bits eq 0, lt 1, gt 2, gte 5, lte 6 (IEEE: false on NaN),
 * bit 3 inverts. The "n" variants (3, 7) are UNKNOWN in NaN handling. */
static int fcmp(struct agx_sim *s, struct agx_sim_sg *g, unsigned lane, const struct agx_operand *a,
		const struct agx_operand *b, unsigned cond, int *out)
{
	float x, y;
	unsigned base = cond & 7;
	if (src_f32(s, g, lane, a, &x) || src_f32(s, g, lane, b, &y))
		return -1;
	int r;
	switch (base) {
	case 0: r = x == y; break;
	case 1: r = x < y; break;
	case 2: r = x > y; break;
	case 5: r = x >= y; break;
	case 6: r = x <= y; break;
	default:
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "float condition with UNKNOWN NaN rule");
		return -1;
	}
	*out = r ^ (int)((cond >> 3) & 1);
	return 0;
}

/* ---- execution ---- */

/* The execution mask is its own state: control-flow instructions keep a
 * per-lane nesting counter in r0l and then set exec = (r0l == 0); every other
 * instruction may overwrite r0 freely (vadd loads into r0). applegpu
 * pseudocode and its corestate.exec (BSD-3). */
static int lane_active(const struct agx_sim_sg *g, unsigned lane)
{
	return (g->exec >> lane) & 1;
}

static uint32_t exec_mask(const struct agx_sim_sg *g)
{
	return g->exec;
}

static void exec_from_r0l(struct agx_sim_sg *g)
{
	g->exec = 0;
	for (unsigned l = 0; l < AGX_SIM_SIMD; l++)
		if (((g->alive >> l) & 1) && g->r[l][0] == 0)
			g->exec |= 1u << l;
}

static int popc32(uint32_t v)
{
	int n = 0;
	for (; v; v &= v - 1)
		n++;
	return n;
}

static int sr_value(struct agx_sim *s, struct agx_sim_sg *g, unsigned sgi, unsigned lane, unsigned sr, uint64_t *v)
{
	switch (sr) {
	case 0: case 1: case 2: /* threadgroup_in_grid_{x,y,z} */
		*v = s->tg[sr];
		return 0;
	case 4: case 5: case 6: /* threads_per_threadgroup_{x,y,z} */
		*v = s->local[sr - 4];
		return 0;
	case 8: case 9: case 10: /* dispatch_threads_per_threadgroup_{x,y,z} */
		*v = s->local[sr - 8];
		return 0;
	case 48: case 49: case 50: /* thread_in_threadgroup_{x,y,z} */
		*v = g->lid[lane][sr - 48];
		return 0;
	case 51: /* thread_index_in_threadgroup */
		*v = g->tid[lane];
		return 0;
	case 52: /* thread_index_in_simdgroup */
		*v = lane;
		return 0;
	case 53: /* simdgroup_index_in_threadgroup */
		*v = sgi;
		return 0;
	case 58: /* active_thread_index_in_simdgroup */
		*v = (uint64_t)popc32(exec_mask(g) & ((1u << lane) - 1));
		return 0;
	case 59: /* total_active_threads_in_simdgroup */
		*v = (uint64_t)popc32(exec_mask(g));
		return 0;
	case 63: /* is_active_thread */
		*v = 1;
		return 0;
	case 80: case 81: case 82: /* thread_in_grid_{x,y,z} */
		*v = (uint64_t)s->tg[sr - 80] * s->local[sr - 80] + g->lid[lane][sr - 80];
		return 0;
	}
	fault(s, AGX_SIM_FAULT_UNSUPPORTED, "special register");
	return -1;
}

static unsigned format_bytes(uint64_t fmt)
{
	switch (fmt) {
	case 0: return 1; /* i8 */
	case 1: return 2; /* i16 */
	case 2: return 4; /* i32 */
	}
	return 0;
}

/* Memory ops: vector width from the mask, which must be x, xy, xyz or xyzw. */
static int mem_shape(struct agx_sim *s, const struct decoded *x, const struct agx_operand *data, unsigned *esize,
		     unsigned *ncomp)
{
	uint64_t fmt = field_or(x, "format", 99), mask = field_or(x, "mask", 0);
	*esize = format_bytes(fmt);
	if (!*esize) {
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "memory format other than i8/i16/i32");
		return -1;
	}
	if (mask != 1 && mask != 3 && mask != 7 && mask != 15) {
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "non-contiguous memory mask");
		return -1;
	}
	*ncomp = (unsigned)popc32((uint32_t)mask);
	if (data->kind != AGX_OPND_REG || data->count != *ncomp || data->size < *esize * 8 ||
	    (data->size == 64)) {
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "memory register shape");
		return -1;
	}
	return 0;
}

static int exec_device_mem(struct agx_sim *s, struct agx_sim_sg *g, const struct decoded *x, int store)
{
	/* load/store: data, membase, memindex. Address = base + index * element
	 * size << shift. Element scaling OBSERVED (vadd indexes 4-byte elements
	 * by thread id and matches the GPU); shift INFERRED (Mesa). */
	if (x->ndst + x->nsrc != 3) {
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "device memory operand count");
		return -1;
	}
	const struct agx_operand *data = store ? &x->src[0] : &x->dst[0];
	const struct agx_operand *base = store ? &x->src[1] : &x->src[0];
	const struct agx_operand *index = store ? &x->src[2] : &x->src[1];
	if (field_or(x, "b26", 1) != 1) {
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "device memory bit 26 clear (A3, UNKNOWN)");
		return -1;
	}
	unsigned esize, ncomp;
	if (mem_shape(s, x, data, &esize, &ncomp) || !reg_ok(s, data))
		return -1;
	unsigned shift = (unsigned)field_or(x, "shift", 0);
	for (unsigned l = 0; l < AGX_SIM_SIMD; l++) {
		if (!lane_active(g, l))
			continue;
		uint64_t b, i;
		if (base->kind != AGX_OPND_REG && base->kind != AGX_OPND_UNIFORM) {
			fault(s, AGX_SIM_FAULT_UNSUPPORTED, "device memory base kind");
			return -1;
		}
		if (!reg_ok(s, base))
			return -1;
		b = read_raw(s, g, l, base, 0);
		if (src_int(s, g, l, index, 0, &i))
			return -1;
		if (index->kind == AGX_OPND_REG && !index->sx)
			i = (uint32_t)i;
		uint64_t addr = b + ((i * esize) << shift);
		for (unsigned c = 0; c < ncomp; c++) {
			uint8_t buf[4];
			uint64_t va = addr + (uint64_t)c * esize;
			if (store) {
				uint64_t v = read_raw(s, g, l, data, c);
				for (unsigned k = 0; k < esize; k++)
					buf[k] = (uint8_t)(v >> (8 * k));
				if (xfer(s, s->slot, va, buf, esize, AGX_ACC_GPU_W, "shader store"))
					return -1;
			} else {
				if (xfer(s, s->slot, va, buf, esize, AGX_ACC_GPU_R, "shader load"))
					return -1;
				uint64_t v = 0;
				for (unsigned k = 0; k < esize; k++)
					v |= (uint64_t)buf[k] << (8 * k);
				write_raw(g, l, data, c, v);
			}
		}
	}
	return 0;
}

static int exec_local_mem(struct agx_sim *s, struct agx_sim_sg *g, const struct decoded *x, int store)
{
	/* lload/lstore: data, localmembase (16-bit byte address), localmemindex.
	 * tgu = 1 with tg47 set and an immediate index: address = base + index *
	 * element size. INFERRED from matmul_tiled: both 16x16 float tiles then
	 * occupy exactly [0, 2048), the 2048 bytes Metal reports as the
	 * pipeline's staticThreadgroupMemoryLength (OBSERVED); a byte offset
	 * would put the top of one tile at 2060. Other tgu values, a clear tg47
	 * and register indices are UNKNOWN (docs/AGX_ISA_NOTES.md A1, A2). */
	if (x->ndst + x->nsrc != 3) {
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "threadgroup memory operand count");
		return -1;
	}
	const struct agx_operand *data = store ? &x->src[0] : &x->dst[0];
	const struct agx_operand *base = store ? &x->src[1] : &x->src[0];
	const struct agx_operand *index = store ? &x->src[2] : &x->src[1];
	if (field_or(x, "tgu", 0) != 1 || field_or(x, "tg47", 0) != 1) {
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "lload/lstore tgu/tg47 values with UNKNOWN meaning (A1, A2)");
		return -1;
	}
	if (index->kind != AGX_OPND_SIGNED && index->kind != AGX_OPND_IMM) {
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "lload/lstore register index (UNKNOWN unit)");
		return -1;
	}
	unsigned esize, ncomp;
	if (mem_shape(s, x, data, &esize, &ncomp) || !reg_ok(s, data))
		return -1;
	for (unsigned l = 0; l < AGX_SIM_SIMD; l++) {
		if (!lane_active(g, l))
			continue;
		uint64_t b = 0, i;
		if (base->kind == AGX_OPND_REG || base->kind == AGX_OPND_UNIFORM) {
			if (src_int(s, g, l, base, 0, &b))
				return -1;
		} else if (base->kind != AGX_OPND_NONE) {
			fault(s, AGX_SIM_FAULT_UNSUPPORTED, "threadgroup memory base kind");
			return -1;
		}
		if (src_int(s, g, l, index, 0, &i))
			return -1;
		int64_t addr = (int64_t)b + (int64_t)i * (int64_t)esize;
		if (addr < 0 || (uint64_t)addr + (uint64_t)ncomp * esize > s->shared_size) {
			fault(s, AGX_SIM_FAULT_BOUNDS, "threadgroup memory access past the USC Shared size");
			s->fault.va = (uint64_t)addr;
			return -1;
		}
		for (unsigned c = 0; c < ncomp; c++) {
			uint8_t *p = s->shared + addr + (int64_t)c * esize;
			if (store) {
				uint64_t v = read_raw(s, g, l, data, c);
				for (unsigned k = 0; k < esize; k++)
					p[k] = (uint8_t)(v >> (8 * k));
			} else {
				uint64_t v = 0;
				for (unsigned k = 0; k < esize; k++)
					v |= (uint64_t)p[k] << (8 * k);
				write_raw(g, l, data, c, v);
			}
		}
	}
	return 0;
}

/* Execute one decoded instruction for SIMD group g. Returns 0, or -1 on fault. */
static int exec_one(struct agx_sim *s, struct agx_sim_sg *g, unsigned sgi, const struct decoded *x)
{
	unsigned k = s->kind[x->ins.op];
	uint64_t next = g->pc + x->ins.len;
	int sat = (int)field_or(x, "saturate", 0);
	const struct agx_operand *D = &x->dst[0], *A = &x->src[0], *B = &x->src[1], *C = &x->src[2];

	switch (k) {
	case K_WAIT: /* loads complete immediately in the model */
		break;
	case K_STOP:
		g->state = SG_DONE;
		return 0;
	case K_BARRIER:
		g->state = SG_BARRIER;
		break;
	case K_JMP_ANY:
	case K_JMP_NONE: {
		/* applegpu: next_pc = pc + sign_extend(off, 32), any() over the exec mask */
		int any = exec_mask(g) != 0;
		if (any == (k == K_JMP_ANY))
			next = g->pc + (uint64_t)sext(field_or(x, "offset", 0), 32);
		break;
	}
	case K_IF_ICMP:
	case K_IF_FCMP:
	case K_WHILE_ICMP:
	case K_WHILE_FCMP: {
		/* applegpu pseudocode, over every lane that exists:
		 *   if:    v != 0 ? v += n : (!cond ? v = 1 : v)
		 *   while: v < n ? v = (cond ? 0 : n) : v */
		unsigned n = (unsigned)field_or(x, "nest", 0), cond = (unsigned)field_or(x, "cond", 0);
		int is_f = k == K_IF_FCMP || k == K_WHILE_FCMP, is_while = k == K_WHILE_ICMP || k == K_WHILE_FCMP;
		for (unsigned l = 0; l < AGX_SIM_SIMD; l++) {
			if (!((g->alive >> l) & 1))
				continue;
			unsigned v = g->r[l][0];
			int c = 0;
			if (is_while ? v < n : v == 0) {
				if (is_f ? fcmp(s, g, l, A, B, cond, &c) : icmp(s, g, l, A, B, cond, &c))
					return -1;
			}
			if (is_while) {
				if (v < n)
					v = c ? 0 : n;
			} else if (v != 0) {
				v += n;
			} else if (!c) {
				v = 1;
			}
			g->r[l][0] = (uint16_t)v;
		}
		exec_from_r0l(g);
		break;
	}
	case K_POP_EXEC: {
		unsigned n = (unsigned)field_or(x, "nest", 0);
		for (unsigned l = 0; l < AGX_SIM_SIMD; l++)
			if ((g->alive >> l) & 1)
				g->r[l][0] = (uint16_t)(g->r[l][0] > n ? g->r[l][0] - n : 0);
		exec_from_r0l(g);
		break;
	}
	case K_LOAD:
	case K_STORE:
		if (exec_device_mem(s, g, x, k == K_STORE))
			return -1;
		break;
	case K_LLOAD:
	case K_LSTORE:
		if (exec_local_mem(s, g, x, k == K_LSTORE))
			return -1;
		break;
	default: {
		if (k == K_NONE || x->ndst != 1) {
			fault(s, AGX_SIM_FAULT_UNSUPPORTED, "instruction outside the simulated subset");
			return -1;
		}
		if (sat && k != K_FADD && k != K_FMUL && k != K_FFMA) {
			fault(s, AGX_SIM_FAULT_UNSUPPORTED, "integer saturate");
			return -1;
		}
		for (unsigned l = 0; l < AGX_SIM_SIMD; l++) {
			if (!lane_active(g, l))
				continue;
			uint64_t a = 0, b = 0, c = 0, r = 0;
			float fa, fb, fc;
			int t;
			unsigned sh = (unsigned)field_or(x, "shift", 0);
			switch (k) {
			case K_MFSR:
				if (sr_value(s, g, sgi, l, (unsigned)field_or(x, "sr", 0xff), &r))
					return -1;
				break;
			case K_LDIMM:
				r = field_or(x, "imm", 0);
				break;
			case K_MOV:
				if (src_int(s, g, l, A, 0, &r))
					return -1;
				break;
			case K_IADD:
			case K_ISUB:
				if (src_int(s, g, l, A, 0, &a) || src_int(s, g, l, B, 0, &b))
					return -1;
				r = k == K_IADD ? a + (b << sh) : a - (b << sh);
				break;
			case K_IMUL:
				if (src_int(s, g, l, A, 0, &a) || src_int(s, g, l, B, 0, &b))
					return -1;
				r = a * b;
				break;
			case K_IMADD:
			case K_IMSUB:
				if (src_int(s, g, l, A, 0, &a) || src_int(s, g, l, B, 0, &b) ||
				    src_int(s, g, l, C, 0, &c))
					return -1;
				r = k == K_IMADD ? a * b + (c << sh) : a * b - (c << sh);
				break;
			case K_ICSEL:
			case K_FCSEL:
				/* D = cond(A, B) ? X : Y (Mesa icmpsel/fcmpsel) */
				if (x->nsrc != 4)
					return fault(s, AGX_SIM_FAULT_UNSUPPORTED, "csel operands"), -1;
				if ((k == K_ICSEL ? icmp(s, g, l, A, B, (unsigned)field_or(x, "condition", 0), &t)
						  : fcmp(s, g, l, A, B, (unsigned)field_or(x, "condition", 0), &t)) ||
				    src_int(s, g, l, t ? &x->src[2] : &x->src[3], 0, &r))
					return -1;
				break;
			case K_FADD:
			case K_FMUL:
				if (src_f32(s, g, l, A, &fa) || src_f32(s, g, l, B, &fb))
					return -1;
				if (dst_f32(s, g, l, D, k == K_FADD ? fa + fb : fa * fb, sat))
					return -1;
				continue;
			case K_FFMA:
				/* fused multiply-add, one rounding. INFERRED (Mesa ffma);
				 * the matmul cases are exact either way. */
				if (src_f32(s, g, l, A, &fa) || src_f32(s, g, l, B, &fb) || src_f32(s, g, l, C, &fc))
					return -1;
				if (dst_f32(s, g, l, D, __builtin_fmaf(fa, fb, fc), sat))
					return -1;
				continue;
			default:
				fault(s, AGX_SIM_FAULT_UNSUPPORTED, "instruction outside the simulated subset");
				return -1;
			}
			if (dst_int(s, g, l, D, r))
				return -1;
		}
		break;
	}
	}
	g->pc = next;
	return 0;
}

static int decode_at(struct agx_sim *s, uint64_t pc, struct decoded *x)
{
	uint8_t buf[AGX_MAX_INS_BYTES];
	uint64_t va = s->code_va + pc;
	/* Fetch up to 12 bytes, but not across into an unmapped page. */
	uint64_t room = AGX_UAT_PGSZ - (va & (AGX_UAT_PGSZ - 1));
	uint64_t want = room < AGX_MAX_INS_BYTES ? room : AGX_MAX_INS_BYTES;
	if (gpu_read(s, va, buf, want, "shader fetch"))
		return -1;
	if (want < AGX_MAX_INS_BYTES) {
		uint64_t pa;
		if (agx_uat_translate(&s->mem, s->ttbat_pa, s->slot, va + want, AGX_MAX_INS_BYTES - want,
				      AGX_ACC_GPU_R, &pa) == AGX_UAT_OK &&
		    gpu_read(s, va + want, buf + want, AGX_MAX_INS_BYTES - want, "shader fetch") == 0)
			want = AGX_MAX_INS_BYTES;
	}
	int n = agx_decode(buf, (size_t)want, &x->ins);
	if (n < 0) {
		fault(s, n == AGX_E_SHORT ? AGX_SIM_FAULT_MMU : AGX_SIM_FAULT_DECODE, "shader decode");
		s->fault.va = va;
		s->fault.pc = pc;
		s->fault.op = n == AGX_E_NO_MATCH ? -1 : x->ins.op;
		return -1;
	}
	x->d = &agx_ins_table[x->ins.op];
	x->ndst = x->nsrc = 0;
	for (unsigned f = 0; f < x->d->nfields; f++) {
		const struct agx_field *fd = &x->d->fields[f];
		if (fd->type != AGX_FT_DEST && fd->type != AGX_FT_SRC)
			continue;
		struct agx_operand *o;
		if (fd->type == AGX_FT_DEST) {
			if (x->ndst >= 2)
				goto shape;
			o = &x->dst[x->ndst++];
		} else {
			if (x->nsrc >= MAXOPS)
				goto shape;
			o = &x->src[x->nsrc++];
		}
		if (agx_operand_resolve(fd->ref, x->ins.val[f], o) != AGX_OK)
			goto shape;
	}
	return 0;
shape:
	fault(s, AGX_SIM_FAULT_DECODE, "operand shape");
	s->fault.va = va;
	s->fault.pc = pc;
	s->fault.op = x->ins.op;
	return -1;
}

/* Run SIMD group g until it stops, reaches a barrier, or faults. */
static int run_sg(struct agx_sim *s, unsigned sgi)
{
	struct agx_sim_sg *g = &s->sg[sgi];
	struct decoded x;
	while (g->state == SG_RUN) {
		if (s->steps++ >= s->step_limit) {
			fault(s, AGX_SIM_FAULT_WATCHDOG, "step limit");
			s->fault.pc = g->pc;
			return -1;
		}
		if (decode_at(s, g->pc, &x))
			return -1;
		s->instructions++;
		s->op_hits[x.ins.op]++;
		uint64_t pc = g->pc;
		if (exec_one(s, g, sgi, &x)) {
			if (s->fault.op < 0) {
				s->fault.op = x.ins.op;
				s->fault.pc = pc;
				if (s->fault.kind != AGX_SIM_FAULT_MMU)
					s->fault.va = s->code_va + pc;
			}
			return -1;
		}
	}
	return 0;
}

static int run_threadgroup(struct agx_sim *s)
{
	uint32_t n = s->local[0] * s->local[1] * s->local[2];
	unsigned nsg = (n + AGX_SIM_SIMD - 1) / AGX_SIM_SIMD;
	/* Shared memory content is undefined at threadgroup start: poison it so a
	 * shader that reads before writing is visible in its output. */
	fill(s->shared, 0xcd, s->shared_size);
	for (unsigned i = 0; i < nsg; i++) {
		struct agx_sim_sg *g = &s->sg[i];
		fill((uint8_t *)g->r, 0, sizeof(g->r));
		g->alive = 0;
		g->pc = 0;
		g->state = SG_RUN;
		for (unsigned l = 0; l < AGX_SIM_SIMD; l++) {
			uint32_t t = i * AGX_SIM_SIMD + l;
			if (t >= n)
				break;
			uint32_t lx = t % s->local[0], ly = (t / s->local[0]) % s->local[1],
				 lz = t / (s->local[0] * s->local[1]);
			g->lid[l][0] = (uint16_t)lx;
			g->lid[l][1] = (uint16_t)ly;
			g->lid[l][2] = (uint16_t)lz;
			g->tid[l] = (uint16_t)t;
			/* dispatchThreads: threads past the grid edge are not launched */
			if (s->tg[0] * s->local[0] + lx < s->grid[0] && s->tg[1] * s->local[1] + ly < s->grid[1] &&
			    s->tg[2] * s->local[2] + lz < s->grid[2])
				g->alive |= 1u << l;
		}
		g->exec = g->alive;
		if (!g->alive)
			g->state = SG_DONE;
		s->simdgroups++;
	}
	for (;;) {
		int waiting = 0, running = 0;
		for (unsigned i = 0; i < nsg; i++) {
			if (s->sg[i].state == SG_RUN) {
				running = 1;
				if (run_sg(s, i))
					return -1;
			}
		}
		for (unsigned i = 0; i < nsg; i++)
			waiting += s->sg[i].state == SG_BARRIER;
		if (!waiting && !running)
			return 0;
		if (waiting) /* everyone is at the barrier or done: release */
			for (unsigned i = 0; i < nsg; i++)
				if (s->sg[i].state == SG_BARRIER)
					s->sg[i].state = SG_RUN;
	}
}

/* ---- USC word list and CDM control stream ---- */

static int parse_usc(struct agx_sim *s, uint64_t usc_base, uint64_t pipeline_va)
{
	int have_shader = 0;
	uint64_t va = pipeline_va;
	s->gpr_limit = 0;
	s->shared_size = 0;
	for (unsigned words = 0; words < 64; words++) {
		uint8_t w[8];
		if (gpu_read(s, va, w, 2, "USC word"))
			return -1;
		switch (w[0]) {
		case AGX_USC_UNIFORM: {
			if (gpu_read(s, va, w, 8, "USC word"))
				return -1;
			uint64_t v = agx_rd64(w);
			unsigned start = (unsigned)(v >> 8) & 0xff, size = (unsigned)(v >> 20) & 0x3f;
			uint64_t buf = ((v >> 26) & ((1ull << 38) - 1)) << 2;
			if (!size)
				size = 64;
			if (start + size > s->uni_limit) {
				fault(s, AGX_SIM_FAULT_BOUNDS, "USC Uniform past the CDM launch uniform count");
				return -1;
			}
			uint8_t data[128];
			if (gpu_read(s, buf, data, 2ull * size, "USC Uniform data"))
				return -1;
			for (unsigned i = 0; i < size; i++)
				s->uni[start + i] = (uint16_t)(data[2 * i] | data[2 * i + 1] << 8);
			va += 8;
			break;
		}
		case AGX_USC_SHARED: {
			if (gpu_read(s, va, w, 4, "USC word"))
				return -1;
			uint32_t v = agx_rd32(w), bytes = (v >> 24) & 0xff;
			if ((v >> 8) & 1)
				s->shared_size = (bytes ? bytes : 256) * 256;
			if (((v >> 10) & 0x3f) != AGX_USC_SHARED_LAYOUT_COMPUTE) {
				fault(s, AGX_SIM_FAULT_UNSUPPORTED, "USC Shared layout other than compute");
				return -1;
			}
			va += 4;
			break;
		}
		case AGX_USC_SHADER: {
			if (gpu_read(s, va, w, 6, "USC word"))
				return -1;
			s->code_va = usc_base + agx_rd32(w + 2);
			have_shader = 1;
			va += 6;
			break;
		}
		case AGX_USC_REGISTERS: {
			if (gpu_read(s, va, w, 4, "USC word"))
				return -1;
			unsigned n = (agx_rd32(w) >> 8) & 0x1f;
			s->gpr_limit = (n ? n : 32) * 8 * 2;
			va += 4;
			break;
		}
		case AGX_USC_NO_PRESHADER:
			if (!have_shader || !s->gpr_limit) {
				fault(s, AGX_SIM_FAULT_COMMAND, "USC list without Shader or Registers");
				return -1;
			}
			return 0;
		default:
			/* Uniform high, samplers, textures, preshaders: not needed for
			 * compute without resources, and not simulated. */
			fault(s, AGX_SIM_FAULT_UNSUPPORTED, "USC word outside the simulated subset");
			s->fault.va = va;
			return -1;
		}
	}
	fault(s, AGX_SIM_FAULT_COMMAND, "USC list without an end");
	return -1;
}

static int launch(struct agx_sim *s, uint64_t usc_base, uint64_t pipeline_va)
{
	if (parse_usc(s, usc_base, pipeline_va))
		return -1;
	uint64_t threads = 1;
	for (int d = 0; d < 3; d++) {
		if (s->local[d] == 0) {
			fault(s, AGX_SIM_FAULT_COMMAND, "zero threadgroup size");
			return -1;
		}
		threads *= s->local[d];
	}
	if (threads > AGX_SIM_MAX_TG_THREADS) {
		fault(s, AGX_SIM_FAULT_COMMAND, "threadgroup larger than 1024 threads");
		return -1;
	}
	uint32_t n[3];
	for (int d = 0; d < 3; d++)
		n[d] = s->grid[d] / s->local[d] + (s->grid[d] % s->local[d] != 0);
	for (s->tg[2] = 0; s->tg[2] < n[2]; s->tg[2]++)
		for (s->tg[1] = 0; s->tg[1] < n[1]; s->tg[1]++)
			for (s->tg[0] = 0; s->tg[0] < n[0]; s->tg[0]++) {
				s->threadgroups++;
				if (run_threadgroup(s))
					return -1;
			}
	return 0;
}

static int run_cdm(struct agx_sim *s, uint64_t base, uint64_t end, uint64_t usc_base)
{
	uint64_t va = base;
	while (va < end) {
		uint8_t w[4 * 8];
		if (gpu_read(s, va, w, 4, "CDM word"))
			return -1;
		uint32_t w0 = agx_rd32(w);
		switch (w0 >> 29) {
		case AGX_CDM_LAUNCH: {
			/* word 0, word 1, global size x3, local size x3 (direct mode;
			 * no G14X extra word on G14G). */
			if (gpu_read(s, va, w, 32, "CDM launch"))
				return -1;
			if (((w0 >> 27) & 3) != 0) {
				fault(s, AGX_SIM_FAULT_UNSUPPORTED, "indirect CDM launch");
				return -1;
			}
			if (((w0 >> 4) & 0x1f) || ((w0 >> 9) & 7)) {
				fault(s, AGX_SIM_FAULT_UNSUPPORTED, "CDM launch with textures or samplers");
				return -1;
			}
			unsigned u = (w0 >> 1) & 7;
			s->uni_limit = (u ? u : 8) * 64;
			uint64_t pipeline = usc_base + ((uint64_t)(agx_rd32(w + 4) >> 6) << 6);
			for (int d = 0; d < 3; d++) {
				s->grid[d] = agx_rd32(w + 8 + 4 * d);
				s->local[d] = agx_rd32(w + 20 + 4 * d);
			}
			if (launch(s, usc_base, pipeline))
				return -1;
			va += 32;
			break;
		}
		case AGX_CDM_BARRIER:
			va += 4;
			break;
		case AGX_CDM_STREAM_TERMINATE:
			return 0;
		default:
			fault(s, AGX_SIM_FAULT_UNSUPPORTED, "CDM block outside the simulated subset");
			s->fault.va = va;
			return -1;
		}
	}
	fault(s, AGX_SIM_FAULT_COMMAND, "CDM stream ran past cdm_ctrl_stream_end without a terminate");
	return -1;
}

/* ---- firmware: commands and the work queue ---- */

struct job {
	uint64_t jp1, jp2, work_queue, fw_stamp;
	uint32_t vm_slot, stamp_value;
	int finalize, retire;
};

static int parse_microseq(struct agx_sim *s, const uint8_t *ms, uint32_t size, uint64_t queue_va, uint32_t vm_slot,
			  struct job *j)
{
	uint32_t off = 0;
	int started = 0;
	while (off + 4 <= size) {
		uint32_t h = agx_rd32(ms + off), len;
		if (h == AGX_MS_START_COMPUTE && !started) {
			len = AGX_MS_START_COMPUTE_SIZE;
			if (off + len > size)
				break;
			j->jp1 = agx_rd64(ms + off + AGX_MS_SC_JOB_PARAMS1);
			j->jp2 = agx_rd64(ms + off + AGX_MS_SC_JOB_PARAMS2);
			j->work_queue = agx_rd64(ms + off + AGX_MS_SC_WORK_QUEUE);
			j->vm_slot = agx_rd32(ms + off + AGX_MS_SC_VM_SLOT);
			started = 1;
		} else if ((h & 0xff) == AGX_MS_TIMESTAMP && started) {
			len = AGX_MS_TIMESTAMP_SIZE;
		} else if (h == AGX_MS_WAIT_FOR_IDLE_COMPUTE && started) {
			len = AGX_MS_WAIT_FOR_IDLE_SIZE;
		} else if (h == AGX_MS_FINALIZE_COMPUTE && started && !j->finalize) {
			len = AGX_MS_FINALIZE_COMPUTE_SIZE;
			if (off + len > size)
				break;
			j->fw_stamp = agx_rd64(ms + off + AGX_MS_FC_FW_STAMP);
			j->stamp_value = agx_rd32(ms + off + AGX_MS_FC_STAMP_VALUE);
			if (agx_rd64(ms + off + AGX_MS_FC_WORK_QUEUE) != queue_va ||
			    agx_rd32(ms + off + AGX_MS_FC_VM_SLOT) != vm_slot)
				break;
			j->finalize = 1;
		} else if (h == AGX_MS_RETIRE_STAMP && j->finalize) {
			len = AGX_MS_RETIRE_STAMP_SIZE;
			j->retire = 1;
		} else {
			break;
		}
		off += len;
	}
	if (!started || !j->finalize || !j->retire || off != size || j->work_queue != queue_va ||
	    j->vm_slot != vm_slot) {
		fault(s, AGX_SIM_FAULT_COMMAND, "microsequence is not StartCompute .. FinalizeCompute, RetireStamp");
		return -1;
	}
	return 0;
}

static int run_command(struct agx_sim *s, uint64_t cmd_va, uint64_t queue_va, uint32_t *stamp_value)
{
	uint8_t rc[AGX_RC_SIZE], ms[AGX_SIM_MICROSEQ_MAX], jp[0x160];
	struct job j = { 0 };
	if (fw_read(s, cmd_va, rc, sizeof(rc), "RunCompute"))
		return -1;
	if (agx_rd32(rc + AGX_RC_TAG) != AGX_CMD_RUN_COMPUTE) {
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "work queue command other than RunCompute");
		return -1;
	}
	uint32_t vm_slot = agx_rd32(rc + AGX_RC_VM_SLOT), ms_size = agx_rd32(rc + AGX_RC_MICROSEQ_SIZE);
	if (vm_slot == 0 || vm_slot >= AGX_UAT_NUM_CTX || ms_size == 0 || ms_size > sizeof(ms)) {
		fault(s, AGX_SIM_FAULT_COMMAND, "RunCompute vm_slot or microsequence size");
		return -1;
	}
	if (fw_read(s, agx_rd64(rc + AGX_RC_MICROSEQ), ms, ms_size, "microsequence") ||
	    parse_microseq(s, ms, ms_size, queue_va, vm_slot, &j))
		return -1;
	/* StartCompute points at the parameters inside RunCompute (drm/asahi
	 * builds it that way); the firmware model reads them where it points. */
	if (fw_read(s, j.jp1, jp, 0x160, "JobParameters1"))
		return -1;
	uint64_t cdm_base = agx_rd64(jp + AGX_JP1_CDM_BASE), usc_base = agx_rd64(jp + AGX_JP1_USC_EXEC_BASE);
	if (fw_read(s, j.jp2, jp, 0x60, "JobParameters2"))
		return -1;
	uint64_t cdm_end = agx_rd64(jp + AGX_JP2_CDM_END);
	if (cdm_end <= cdm_base) {
		fault(s, AGX_SIM_FAULT_COMMAND, "empty CDM stream");
		return -1;
	}
	s->slot = vm_slot;
	s->steps = 0;
	if (run_cdm(s, cdm_base, cdm_end, usc_base))
		return -1;
	/* Completion: FinalizeCompute's firmware stamp, then the stamp in meta. */
	if (fw_write32(s, j.fw_stamp, j.stamp_value, "FinalizeCompute fw_stamp") ||
	    fw_write32(s, agx_rd64(rc + AGX_RC_META + AGX_META_STAMP), j.stamp_value, "JobMeta stamp"))
		return -1;
	*stamp_value = j.stamp_value;
	return 0;
}

static void push_event(struct agx_sim *s, uint32_t slot, uint32_t value, uint32_t error)
{
	struct agx_sim_event *e = &s->events[s->nevents % AGX_SIM_EVENTS];
	e->slot = slot;
	e->value = value;
	e->error = error;
	s->nevents++;
}

int agx_sim_run_work_queue(struct agx_sim *s, const uint8_t *msg)
{
	uint8_t qi[AGX_QI_SIZE], rs[AGX_RS_SIZE];
	s->faulted = 0;
	s->fault = (struct agx_sim_fault){ 0 };
	s->fault.op = -1;
	if (agx_rd32(msg + AGX_RWQ_PIPE_TYPE) != AGX_PIPE_COMPUTE) {
		fault(s, AGX_SIM_FAULT_UNSUPPORTED, "pipe other than compute");
		return -1;
	}
	uint64_t queue_va = agx_rd64(msg + AGX_RWQ_WORK_QUEUE);
	uint32_t event_slot = agx_rd32(msg + AGX_RWQ_EVENT_SLOT);
	if (fw_read(s, queue_va, qi, sizeof(qi), "QueueInfo"))
		return -1;
	uint64_t state_va = agx_rd64(qi + AGX_QI_STATE), ring_va = agx_rd64(qi + AGX_QI_RING);
	if (fw_read(s, state_va, rs, sizeof(rs), "RingState"))
		return -1;
	uint32_t rptr = agx_rd32(rs + AGX_RS_GPU_RPTR), wptr = agx_rd32(rs + AGX_RS_CPU_WPTR),
		 size = agx_rd32(rs + AGX_RS_RB_SIZE);
	if (size == 0 || rptr >= size || wptr >= size || agx_rd32(msg + AGX_RWQ_WPTR) != wptr) {
		fault(s, AGX_SIM_FAULT_COMMAND, "ring pointers");
		return -1;
	}
	int retired = 0;
	while (rptr != wptr) {
		uint8_t e[8];
		uint32_t value = 0;
		if (fw_read(s, ring_va + 8ull * rptr, e, 8, "work queue ring"))
			return -1;
		s->faulted = 0;
		s->jobs++;
		if (run_command(s, agx_rd64(e), queue_va, &value)) {
			struct agx_sim_fault keep = s->fault;
			s->jobs_faulted++;
			uint8_t b[4];
			s->faulted = 0;
			if (fw_read(s, queue_va + AGX_QI_ERROR_COUNT, b, 4, "QueueInfo error_count") == 0)
				fw_write32(s, queue_va + AGX_QI_ERROR_COUNT, agx_rd32(b) + 1, "QueueInfo error_count");
			s->fault = keep;
			s->faulted = 1;
			push_event(s, event_slot, 0, 1);
		} else {
			push_event(s, event_slot, value, 0);
		}
		rptr = (rptr + 1) % size;
		int was = s->faulted;
		struct agx_sim_fault keep = s->fault;
		s->faulted = 0;
		if (fw_write32(s, state_va + AGX_RS_GPU_RPTR, rptr, "RingState gpu_rptr") ||
		    fw_write32(s, state_va + AGX_RS_GPU_DONEPTR, rptr, "RingState gpu_doneptr") ||
		    fw_write32(s, queue_va + AGX_QI_GPU_RPTR1, rptr, "QueueInfo gpu_rptr1"))
			return -1;
		if (was) {
			s->fault = keep;
			s->faulted = 1;
		}
		retired++;
	}
	return retired;
}
