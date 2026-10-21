/* native/kernel/gpu/gcn_interp.c -- Host-side GCN gfx803 wavefront interpreter.
 *
 * Host-only test tool, never compiled into the kernel. Float lane values are
 * uint32_t bit patterns; f32 instruction semantics use the host FPU (IEEE
 * binary32, round-to-nearest-even, denormals kept). Build with
 * -ffp-contract=off so v_mul + v_add never fuse into an fma on the host.
 *
 * Opcode numbers are the gfx8 (VI) ones, checked against
 * llvm-mc-18 -arch=amdgcn -mcpu=gfx803 -show-encoding.
 */
#include "gcn_interp.h"
#include <string.h>
#include <math.h>

static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t f2u(float f)  { uint32_t u; memcpy(&u, &f, 4); return u; }

#define FAIL(w, msg) do { (w)->error = (msg); return -1; } while (0)

/* ---- init ---- */
void gcn_wave_init(gcn_wave_t *w)
{
    memset(w, 0, sizeof *w);
    w->exec_lo = 0xFFFFFFFFu;
    w->exec_hi = 0xFFFFFFFFu;
    for (int i = 0; i < GCN_NUM_SGPR + 2; i++)
        w->s_valu_ws[i] = (uint64_t)-100; /* "long ago" */
}

void gcn_wave_attach_mem(gcn_wave_t *w, void *mem, size_t size,
                         uint64_t base_gpu_addr)
{
    w->mem      = (uint8_t *)mem;
    w->mem_base = base_gpu_addr;
    w->mem_size = size;
}

void gcn_wave_set_threads_1d(gcn_wave_t *w, uint32_t nthreads,
                              uint32_t tgid_x, uint32_t base_tid_x)
{
    if (nthreads > GCN_WAVE_LANES)
        nthreads = GCN_WAVE_LANES;
    w->tgid_x = tgid_x;
    if (nthreads >= 64) {
        w->exec_lo = 0xFFFFFFFFu;
        w->exec_hi = 0xFFFFFFFFu;
    } else if (nthreads >= 32) {
        w->exec_lo = 0xFFFFFFFFu;
        w->exec_hi = (nthreads == 32) ? 0u : ((1u << (nthreads - 32)) - 1u);
    } else {
        w->exec_lo = (1u << nthreads) - 1u;
        w->exec_hi = 0u;
    }
    for (uint32_t i = 0; i < GCN_WAVE_LANES; i++)
        w->tid_x[i] = base_tid_x + i;
}

void gcn_wave_set_threads_2d(gcn_wave_t *w,
                              uint32_t tx, uint32_t ty,
                              uint32_t tgid_x, uint32_t tgid_y,
                              uint32_t base_x, uint32_t base_y)
{
    uint32_t n = tx * ty;
    if (n > GCN_WAVE_LANES) n = GCN_WAVE_LANES;
    gcn_wave_set_threads_1d(w, n, tgid_x, 0);
    w->tgid_y = tgid_y;
    for (uint32_t lane = 0; lane < n; lane++) {
        w->tid_x[lane] = base_x + (lane % tx);
        w->tid_y[lane] = base_y + (lane / tx);
    }
}

/* ---- operand read ---- */
static uint32_t scalar_src(const gcn_wave_t *w, uint32_t src, uint32_t lit)
{
    if (src < GCN_NUM_SGPR) return w->sgpr[src];
    switch (src) {
    case 106: return w->vcc_lo;
    case 107: return w->vcc_hi;
    case 124: return w->m0;
    case 126: return w->exec_lo;
    case 127: return w->exec_hi;
    case 128: return 0u;
    case 240: return 0x3F000000u; /* 0.5 */
    case 241: return 0xBF000000u;
    case 242: return 0x3F800000u; /* 1.0 */
    case 243: return 0xBF800000u;
    case 244: return 0x40000000u; /* 2.0 */
    case 245: return 0xC0000000u;
    case 246: return 0x40800000u; /* 4.0 */
    case 247: return 0xC0800000u;
    case 248: return 0x3E22F983u; /* 1/(2*pi) */
    case 251: return 1u;          /* VCCZ */
    case 252: return 1u;          /* EXECZ */
    case 253: return 1u;          /* SCC */
    case 255: return lit;
    default: break;
    }
    if (src >= 129 && src <= 192) return src - 128u;
    if (src >= 193 && src <= 208) return (uint32_t)(-(int32_t)(src - 192u));
    return 0u;
}

uint32_t gcn_src_read(const gcn_wave_t *w, uint32_t src, uint32_t lane,
                      const uint32_t *literal_ptr)
{
    if (src >= 256 && src <= 511)
        return w->vgpr[lane][src - 256u];
    return scalar_src(w, src, literal_ptr ? *literal_ptr : 0u);
}

/* 64-bit scalar source (pairs, VCC, EXEC, sign-extended inline ints). */
static uint64_t scalar_src64(const gcn_wave_t *w, uint32_t src, uint32_t lit)
{
    if (src + 1 < GCN_NUM_SGPR)
        return (uint64_t)w->sgpr[src] | ((uint64_t)w->sgpr[src + 1] << 32);
    if (src == 106) return (uint64_t)w->vcc_lo | ((uint64_t)w->vcc_hi << 32);
    if (src == 126) return (uint64_t)w->exec_lo | ((uint64_t)w->exec_hi << 32);
    if (src == 255) return (uint64_t)lit;
    return (uint64_t)(int64_t)(int32_t)scalar_src(w, src, lit);
}

static int is_scalar_reg(uint32_t src)
{
    return src < GCN_NUM_SGPR || src == 106 || src == 107 || src == 124 ||
           src == 126 || src == 127 || src == 255;
}

/* ---- issue-order model ---- */
static int vgpr_busy(const gcn_wave_t *w, uint32_t r)
{
    return w->v_vm[r] > w->vm_done || w->v_lgkm[r] > w->lgkm_done;
}
static int sgpr_busy(const gcn_wave_t *w, uint32_t s)
{
    return s < GCN_NUM_SGPR && w->s_lgkm[s] > w->lgkm_done;
}
/* reading a source: VGPR / SGPR not yet delivered by a load */
static int src_hazard(const gcn_wave_t *w, uint32_t src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = src + i;
        if (s >= 256 && s <= 511) { if (vgpr_busy(w, s - 256)) return 1; }
        else if (sgpr_busy(w, s)) return 1;
    }
    return 0;
}
/* writing a VGPR: a load still owes it, or a store still has to read it */
static int vdst_hazard(const gcn_wave_t *w, uint32_t r, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = r + i;
        if (v >= GCN_NUM_VGPR) return 1;
        if (vgpr_busy(w, v) || w->v_exp[v] > w->exp_done) return 1;
    }
    return 0;
}
static int sdst_hazard(const gcn_wave_t *w, uint32_t s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (sgpr_busy(w, s + i)) return 1;
    return 0;
}
static void note_valu_sgpr_write(gcn_wave_t *w, uint32_t s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t d = s + i;
        if (d < GCN_NUM_SGPR) w->s_valu_ws[d] = w->ws;
        else if (d == 106 || d == 107) w->s_valu_ws[GCN_NUM_SGPR] = w->ws;
    }
}
/* VMEM reading SGPR s needs 5 wait states after a VALU write of s */
static int vmem_sgpr_hazard(const gcn_wave_t *w, uint32_t s)
{
    uint64_t t;
    if (s < GCN_NUM_SGPR) t = w->s_valu_ws[s];
    else if (s == 106 || s == 107) t = w->s_valu_ws[GCN_NUM_SGPR];
    else return 0;
    return w->ws - t <= 5;
}

static void sdst_write(gcn_wave_t *w, uint32_t d, uint32_t v)
{
    if (d < GCN_NUM_SGPR) w->sgpr[d] = v;
    else if (d == 106) w->vcc_lo = v;
    else if (d == 107) w->vcc_hi = v;
    else if (d == 124) w->m0 = v;
    else if (d == 126) w->exec_lo = v;
    else if (d == 127) w->exec_hi = v;
}
static void sdst_write64(gcn_wave_t *w, uint32_t d, uint64_t v)
{
    if (d == 106) { w->vcc_lo = (uint32_t)v; w->vcc_hi = (uint32_t)(v >> 32); }
    else if (d == 126) { w->exec_lo = (uint32_t)v; w->exec_hi = (uint32_t)(v >> 32); }
    else { sdst_write(w, d, (uint32_t)v); sdst_write(w, d + 1, (uint32_t)(v >> 32)); }
}

static inline uint64_t exec64(const gcn_wave_t *w)
{
    return (uint64_t)w->exec_lo | ((uint64_t)w->exec_hi << 32);
}
static inline uint64_t vcc64(const gcn_wave_t *w)
{
    return (uint64_t)w->vcc_lo | ((uint64_t)w->vcc_hi << 32);
}

/* ---- memory ---- */
static int mem_ptr(const gcn_wave_t *w, uint64_t addr, uint32_t n, uint8_t **p)
{
    if (addr < w->mem_base || addr - w->mem_base > w->mem_size ||
        w->mem_size - (addr - w->mem_base) < n)
        return -1;
    *p = w->mem + (addr - w->mem_base);
    return 0;
}

/* ---- float helpers ---- */
static inline float fclamp01(float x)
{
    if (!(x > 0.0f)) return 0.0f; /* NaN -> 0 */
    return x > 1.0f ? 1.0f : x;
}
static inline float fmin_hw(float a, float b)
{
    if (a != a) return b;
    if (b != b) return a;
    return a < b ? a : b;
}
static inline float fmax_hw(float a, float b)
{
    if (a != a) return b;
    if (b != b) return a;
    return a > b ? a : b;
}
static inline uint32_t cvt_u32_f32(float f)
{
    if (!(f > 0.0f)) return 0u;
    if (f >= 4294967296.0f) return 0xFFFFFFFFu;
    return (uint32_t)f;
}
static inline uint32_t cvt_i32_f32(float f)
{
    if (f != f) return 0u;
    if (f >= 2147483648.0f) return 0x7FFFFFFFu;
    if (f <= -2147483648.0f) return 0x80000000u;
    return (uint32_t)(int32_t)f;
}
static inline int fcmp(uint32_t op, float a, float b)
{
    switch (op & 15u) {
    case 0:  return 0;
    case 1:  return a < b;
    case 2:  return a == b;
    case 3:  return a <= b;
    case 4:  return a > b;
    case 5:  return a < b || a > b;   /* LG */
    case 6:  return a >= b;
    case 7:  return a == a && b == b; /* O */
    case 8:  return a != a || b != b; /* U */
    case 9:  return !(a >= b);
    case 10: return !(a < b || a > b);
    case 11: return !(a > b);
    case 12: return !(a <= b);
    case 13: return !(a == b);
    case 14: return !(a < b);
    default: return 1;
    }
}
static inline int icmp(uint32_t op, uint32_t a, uint32_t b, int sgn)
{
    int32_t x = (int32_t)a, y = (int32_t)b;
    switch (op & 7u) {
    case 0: return 0;
    case 1: return sgn ? x <  y : a <  b;
    case 2: return a == b;
    case 3: return sgn ? x <= y : a <= b;
    case 4: return sgn ? x >  y : a >  b;
    case 5: return a != b;
    case 6: return sgn ? x >= y : a >= b;
    default: return 1;
    }
}

/* ---- VALU ----
 * Every VALU form is normalised to VOP3 opcode space:
 *   0x000-0x0FF VOPC, 0x100-0x13F VOP2, 0x140-0x1BF VOP1, 0x1C0+ VOP3 only. */
typedef struct {
    uint32_t op, vdst, sdst;      /* sdst: VOPC / carry-out destination */
    uint32_t src[3];              /* 9-bit operand codes */
    uint32_t nsrc;
    uint32_t abs, neg, clamp, omod;
    uint32_t lit;
    int vop3;
} valu_t;

static int is_float_op(uint32_t op)
{
    if (op < 0x100) return (op >= 0x40 && op <= 0x5F);
    if (op >= 0x100 && op < 0x140) {
        uint32_t o = op - 0x100;
        return (o >= 1 && o <= 5) || o == 10 || o == 11 || o == 22 || o == 23 || o == 24;
    }
    if (op >= 0x140 && op < 0x1C0) {
        uint32_t o = op - 0x140;
        return (o >= 27 && o <= 42) || o == 7 || o == 8 || o == 51 || o == 52;
    }
    return op == 0x1C1 || op == 0x1CB || op == 0x1D0 || op == 0x1D3 || op == 0x1D6 || op == 0x288;
}

static int valu_exec(gcn_wave_t *w, valu_t *v)
{
    uint32_t op = v->op;
    uint64_t ex = exec64(w);
    int fl = is_float_op(op);
    if (!fl && (v->abs || v->neg)) FAIL(w, "VALU: abs/neg on integer op");
    if (v->clamp && !fl) FAIL(w, "VALU: clamp on integer op");
    if (v->omod) {
        if (!fl || op < 0x100) FAIL(w, "VALU: omod on non-float result");
    }

    /* constant bus: one distinct SGPR/literal per instruction */
    {
        uint32_t used[4]; int nu = 0;
        uint32_t extra = 0xFFFFFFFFu;
        uint32_t o2 = op - 0x100;
        if (!v->vop3 && op >= 0x100 && op < 0x140 &&
            (o2 == 0 || o2 == 28 || o2 == 29 || o2 == 30))
            extra = 106; /* implicit VCC read */
        for (uint32_t i = 0; i < v->nsrc + (extra != 0xFFFFFFFFu ? 1u : 0u); i++) {
            uint32_t s = i < v->nsrc ? v->src[i] : extra;
            if (!is_scalar_reg(s)) continue;
            if (s == 107) s = 106;
            if (s == 127) s = 126;
            int seen = 0;
            for (int j = 0; j < nu; j++) if (used[j] == s) seen = 1;
            if (!seen && nu < 4) used[nu++] = s;
        }
        if (nu > 1) FAIL(w, "VALU: constant bus limit (more than one SGPR/literal)");
        if (v->vop3) for (uint32_t i = 0; i < v->nsrc; i++)
            if (v->src[i] == 255) FAIL(w, "VOP3: literal operand not allowed on gfx8");
    }

    /* operand hazards */
    for (uint32_t i = 0; i < v->nsrc; i++) {
        uint32_t n = 1;
        if (src_hazard(w, v->src[i], n)) FAIL(w, "hazard: VALU reads a register a load still owes");
    }

    /* scalar operand values (VGPR operands read per lane) */
    uint32_t sv[3] = {0, 0, 0};
    int isv[3] = {0, 0, 0};
    for (uint32_t i = 0; i < v->nsrc; i++) {
        if (v->src[i] >= 256) isv[i] = 1, sv[i] = v->src[i] - 256;
        else sv[i] = scalar_src(w, v->src[i], v->lit);
    }
#define OPND(i, lane) (isv[i] ? w->vgpr[lane][sv[i]] : sv[i])

    /* ---- VOPC ---- */
    if (op < 0x100) {
        uint64_t mask = 0;
        int sgn = 0, isf = 0, cmpx = 0;
        if (op >= 0x40 && op <= 0x4F) isf = 1;
        else if (op >= 0x50 && op <= 0x5F) isf = 1, cmpx = 1;
        else if (op >= 0xC0 && op <= 0xC7) sgn = 1;
        else if (op >= 0xC8 && op <= 0xCF) sgn = 0;
        else if (op >= 0xD0 && op <= 0xD7) sgn = 1, cmpx = 1;
        else if (op >= 0xD8 && op <= 0xDF) sgn = 0, cmpx = 1;
        else if (op == 0x10 || op == 0x11) { /* v_cmp_class_f32 / cmpx */
            for (uint32_t lane = 0; lane < 64; lane++) {
                if (!((ex >> lane) & 1)) continue;
                float a = u2f(OPND(0, lane)); uint32_t m = OPND(1, lane);
                int c = fpclassify(a), neg = signbit(a) != 0, bit;
                if (a != a) bit = 1; /* quiet NaN (signalling not modelled) */
                else if (c == FP_INFINITE) bit = neg ? 2 : 9;
                else if (c == FP_NORMAL) bit = neg ? 3 : 8;
                else if (c == FP_SUBNORMAL) bit = neg ? 4 : 7;
                else bit = neg ? 5 : 6;
                if ((m >> bit) & 1) mask |= 1ull << lane;
            }
            sdst_write64(w, v->sdst, mask);
            if (op == 0x11) { w->exec_lo = (uint32_t)mask; w->exec_hi = (uint32_t)(mask >> 32); }
            note_valu_sgpr_write(w, v->sdst, 2);
            return 0;
        }
        else FAIL(w, "VOPC: opcode not modelled");
        for (uint32_t lane = 0; lane < 64; lane++) {
            if (!((ex >> lane) & 1)) continue;
            uint32_t a = OPND(0, lane), b = OPND(1, lane);
            int c;
            if (isf) {
                if (v->abs & 1) a &= 0x7FFFFFFFu;
                if (v->abs & 2) b &= 0x7FFFFFFFu;
                if (v->neg & 1) a ^= 0x80000000u;
                if (v->neg & 2) b ^= 0x80000000u;
                c = fcmp(op, u2f(a), u2f(b));
            } else c = icmp(op, a, b, sgn);
            if (c) mask |= 1ull << lane;
        }
        if (v->sdst < GCN_NUM_SGPR && sdst_hazard(w, v->sdst, 2)) FAIL(w, "hazard: VOPC overwrites an SGPR a load still owes");
        sdst_write64(w, v->sdst, mask);
        note_valu_sgpr_write(w, v->sdst, 2);
        if (cmpx) { w->exec_lo = (uint32_t)mask; w->exec_hi = (uint32_t)(mask >> 32); }
        return 0;
    }

    /* ---- lane-crossing / scalar-result ops ---- */
    if (op == 0x142) { /* v_readfirstlane_b32 */
        uint32_t val = 0;
        for (uint32_t lane = 0; lane < 64; lane++)
            if ((ex >> lane) & 1) { val = OPND(0, lane); break; }
        if (ex == 0) val = OPND(0, 0);
        sdst_write(w, v->vdst, val);
        note_valu_sgpr_write(w, v->vdst, 1);
        return 0;
    }
    if (op == 0x289) { /* v_readlane_b32 sdst, vsrc0, ssrc1 */
        uint32_t lane = sv[1] & 63u;
        if (!isv[0]) FAIL(w, "v_readlane: src0 must be a VGPR");
        if (vmem_sgpr_hazard(w, v->src[1]) && v->src[1] < 256) { /* 4 wait states */
            if (w->ws - (v->src[1] < GCN_NUM_SGPR ? w->s_valu_ws[v->src[1]] : w->s_valu_ws[GCN_NUM_SGPR]) <= 4)
                FAIL(w, "hazard: v_readlane lane select written by VALU < 4 wait states ago");
        }
        sdst_write(w, v->vdst, w->vgpr[lane][sv[0]]);
        note_valu_sgpr_write(w, v->vdst, 1);
        return 0;
    }
    if (op == 0x28A) { /* v_writelane_b32 vdst, ssrc0, ssrc1 */
        uint32_t lane = sv[1] & 63u;
        if (vdst_hazard(w, v->vdst, 1)) FAIL(w, "hazard: VALU overwrites a VGPR with an access outstanding");
        w->vgpr[lane][v->vdst] = sv[0];
        return 0;
    }

    /* ---- per-lane ops ---- */
    if (op >= 0x14F && op <= 0x150) FAIL(w, "VOP1: f64 op not modelled");
    if (vdst_hazard(w, v->vdst, 1)) FAIL(w, "hazard: VALU overwrites a VGPR with an access outstanding");
    uint64_t carry_out = 0;
    uint64_t cin = 0;
    uint32_t o2 = op - 0x100;
    int has_cin = (op >= 0x100 && op < 0x140) && (o2 == 0 || o2 == 28 || o2 == 29 || o2 == 30);
    int has_cout = (op >= 0x100 && op < 0x140) && (o2 >= 25 && o2 <= 30);
    if (has_cin) {
        uint32_t m = v->vop3 ? v->src[2] : 106u;
        if (src_hazard(w, m, 2)) FAIL(w, "hazard: carry/mask SGPR still owed by a load");
        cin = scalar_src64(w, m, v->lit);
    }
    uint32_t vd = v->vdst;
    if (op == 0x116 || op == 0x117 || op == 0x118 || op == 0x1C1 || op == 0x1C0) w->mad_ops++;
    if (op == 0x160 || op == 0x161 || op == 0x162 || op == 0x163 || op == 0x164 ||
        op == 0x167 || op == 0x169 || op == 0x16A) w->inexact_ops++;

    for (uint32_t lane = 0; lane < 64; lane++) {
        if (!((ex >> lane) & 1)) continue;
        uint32_t a = OPND(0, lane);
        uint32_t b = v->nsrc > 1 ? OPND(1, lane) : 0u;
        uint32_t c = v->nsrc > 2 ? OPND(2, lane) : 0u;
        if (fl) {
            if (v->abs & 1) a &= 0x7FFFFFFFu;
            if (v->abs & 2) b &= 0x7FFFFFFFu;
            if (v->abs & 4) c &= 0x7FFFFFFFu;
            if (v->neg & 1) a ^= 0x80000000u;
            if (v->neg & 2) b ^= 0x80000000u;
            if (v->neg & 4) c ^= 0x80000000u;
        }
        float fa = u2f(a), fb = u2f(b), fc = u2f(c);
        uint32_t r = 0;
        int rf = 0; float fr = 0.0f;
        switch (op) {
        /* VOP2 */
        case 0x100: r = ((cin >> lane) & 1) ? b : a; break;             /* v_cndmask_b32 */
        case 0x101: fr = fa + fb; rf = 1; break;                         /* v_add_f32 */
        case 0x102: fr = fa - fb; rf = 1; break;                         /* v_sub_f32 */
        case 0x103: fr = fb - fa; rf = 1; break;                         /* v_subrev_f32 */
        case 0x104: fr = (fa == 0.0f || fb == 0.0f) ? 0.0f : fa * fb; rf = 1; break; /* mul_legacy */
        case 0x105: fr = fa * fb; rf = 1; break;                         /* v_mul_f32 */
        case 0x106: r = (uint32_t)((int32_t)(a << 8) >> 8) * (uint32_t)((int32_t)(b << 8) >> 8); break;
        case 0x107: r = (uint32_t)(((int64_t)((int32_t)(a << 8) >> 8) * (int64_t)((int32_t)(b << 8) >> 8)) >> 32); break;
        case 0x108: r = (a & 0xFFFFFFu) * (b & 0xFFFFFFu); break;       /* v_mul_u32_u24 */
        case 0x109: r = (uint32_t)(((uint64_t)(a & 0xFFFFFFu) * (b & 0xFFFFFFu)) >> 32); break;
        case 0x10A: fr = fmin_hw(fa, fb); rf = 1; break;
        case 0x10B: fr = fmax_hw(fa, fb); rf = 1; break;
        case 0x10C: r = (int32_t)a < (int32_t)b ? a : b; break;
        case 0x10D: r = (int32_t)a > (int32_t)b ? a : b; break;
        case 0x10E: r = a < b ? a : b; break;
        case 0x10F: r = a > b ? a : b; break;
        case 0x110: r = b >> (a & 31u); break;                           /* v_lshrrev_b32: D = S1 >> S0 */
        case 0x111: r = (uint32_t)((int32_t)b >> (a & 31u)); break;      /* v_ashrrev_i32 */
        case 0x112: r = b << (a & 31u); break;                           /* v_lshlrev_b32: D = S1 << S0 */
        case 0x113: r = a & b; break;
        case 0x114: r = a | b; break;
        case 0x115: r = a ^ b; break;
        case 0x116: fr = fa * fb; fr = fr + u2f(w->vgpr[lane][vd]); rf = 1; break; /* v_mac_f32 (unfused) */
        case 0x117: fr = fa * u2f(v->lit); fr = fr + fb; rf = 1; break;  /* v_madmk_f32 */
        case 0x118: fr = fa * fb; fr = fr + u2f(v->lit); rf = 1; break;  /* v_madak_f32 */
        case 0x119: { uint64_t s = (uint64_t)a + b; r = (uint32_t)s; if (s >> 32) carry_out |= 1ull << lane; break; }
        case 0x11A: { r = a - b; if (b > a) carry_out |= 1ull << lane; break; }
        case 0x11B: { r = b - a; if (a > b) carry_out |= 1ull << lane; break; }
        case 0x11C: { uint64_t s = (uint64_t)a + b + ((cin >> lane) & 1); r = (uint32_t)s; if (s >> 32) carry_out |= 1ull << lane; break; }
        case 0x11D: { uint64_t bb = (uint64_t)b + ((cin >> lane) & 1); r = (uint32_t)((uint64_t)a - bb); if (bb > a) carry_out |= 1ull << lane; break; }
        case 0x11E: { uint64_t aa = (uint64_t)a + ((cin >> lane) & 1); r = (uint32_t)((uint64_t)b - aa); if (aa > b) carry_out |= 1ull << lane; break; }
        /* VOP1 */
        case 0x140: r = w->vgpr[lane][vd]; break;                        /* v_nop: keep */
        case 0x141: r = a; break;                                         /* v_mov_b32 */
        case 0x145: fr = (float)(int32_t)a; rf = 1; break;               /* v_cvt_f32_i32 */
        case 0x146: fr = (float)a; rf = 1; break;                        /* v_cvt_f32_u32 */
        case 0x147: r = cvt_u32_f32(fa); break;                          /* v_cvt_u32_f32 */
        case 0x148: r = cvt_i32_f32(fa); break;                          /* v_cvt_i32_f32 */
        case 0x15B: { float t = fa - floorf(fa); fr = t < 0x1.fffffep-1f ? t : 0x1.fffffep-1f; if (fa != fa) fr = fa; rf = 1; break; } /* fract */
        case 0x15C: fr = truncf(fa); rf = 1; break;
        case 0x15D: fr = ceilf(fa); rf = 1; break;
        case 0x15E: fr = rintf(fa); rf = 1; break;                       /* v_rndne_f32 */
        case 0x15F: fr = floorf(fa); rf = 1; break;
        case 0x160: fr = exp2f(fa); rf = 1; break;                       /* v_exp_f32 (2^x, approx on hw) */
        case 0x161: fr = log2f(fa); rf = 1; break;                       /* v_log_f32 (log2, approx on hw) */
        case 0x162: case 0x163: fr = 1.0f / fa; rf = 1; break;           /* v_rcp(_iflag)_f32 (approx) */
        case 0x164: fr = 1.0f / sqrtf(fa); rf = 1; break;                /* v_rsq_f32 (approx) */
        case 0x167: fr = sqrtf(fa); rf = 1; break;                       /* v_sqrt_f32 (approx) */
        case 0x169: fr = sinf(fa * 6.28318530717958647692f); rf = 1; break; /* v_sin_f32: input in turns */
        case 0x16A: fr = cosf(fa * 6.28318530717958647692f); rf = 1; break;
        case 0x16B: r = ~a; break;                                       /* v_not_b32 */
        case 0x16C: { uint32_t x = a, y = 0; for (int i = 0; i < 32; i++) { y = (y << 1) | (x & 1); x >>= 1; } r = y; break; }
        case 0x16D: r = a ? (uint32_t)__builtin_clz(a) : 0xFFFFFFFFu; break; /* v_ffbh_u32 */
        case 0x16E: r = a ? (uint32_t)__builtin_ctz(a) : 0xFFFFFFFFu; break; /* v_ffbl_b32 */
        case 0x173: { int e = 0; if (isfinite(fa) && fa != 0.0f) frexpf(fa, &e); r = (uint32_t)e; break; }
        case 0x174: { int e; fr = isfinite(fa) ? frexpf(fa, &e) : fa; rf = 1; break; }
        /* VOP3 only */
        case 0x1C1: fr = fa * fb; fr = fr + fc; rf = 1; break;           /* v_mad_f32 (unfused) */
        case 0x1C2: r = (uint32_t)((int32_t)(a << 8) >> 8) * (uint32_t)((int32_t)(b << 8) >> 8) + c; break;
        case 0x1C3: r = (a & 0xFFFFFFu) * (b & 0xFFFFFFu) + c; break;   /* v_mad_u32_u24 */
        case 0x1C8: { uint32_t off = b & 31u, sz = c & 31u; r = sz ? (a >> off) & ((sz == 32) ? 0xFFFFFFFFu : ((1u << sz) - 1u)) : 0u; break; }
        case 0x1C9: { uint32_t off = b & 31u, sz = c & 31u; if (!sz) { r = 0; break; } int32_t x = (int32_t)(a << (32 - off - sz > 31 ? 0 : 32 - off - sz)); r = (uint32_t)(x >> (32 - sz)); break; }
        case 0x1CA: r = (a & b) | (~a & c); break;                       /* v_bfi_b32 */
        case 0x1CB: fr = fmaf(fa, fb, fc); rf = 1; break;                /* v_fma_f32 */
        case 0x1CE: r = (uint32_t)((((uint64_t)a << 32) | b) >> (c & 31u)); break; /* v_alignbit_b32 */
        case 0x1D0: fr = fmin_hw(fmin_hw(fa, fb), fc); rf = 1; break;
        case 0x1D1: { int32_t x = (int32_t)a, y = (int32_t)b, z = (int32_t)c; int32_t m = x < y ? x : y; r = (uint32_t)(m < z ? m : z); break; }
        case 0x1D2: { uint32_t m = a < b ? a : b; r = m < c ? m : c; break; }
        case 0x1D3: fr = fmax_hw(fmax_hw(fa, fb), fc); rf = 1; break;
        case 0x1D4: { int32_t x = (int32_t)a, y = (int32_t)b, z = (int32_t)c; int32_t m = x > y ? x : y; r = (uint32_t)(m > z ? m : z); break; }
        case 0x1D5: { uint32_t m = a > b ? a : b; r = m > c ? m : c; break; }
        case 0x1D6: { float lo = fmin_hw(fa, fb), hi = fmax_hw(fa, fb); fr = fmax_hw(lo, fmin_hw(hi, fc)); rf = 1; break; }
        case 0x1D7: { int32_t x = (int32_t)a, y = (int32_t)b, z = (int32_t)c; int32_t lo = x < y ? x : y, hi = x > y ? x : y; int32_t m = hi < z ? hi : z; r = (uint32_t)(lo > m ? lo : m); break; }
        case 0x1D8: { uint32_t lo = a < b ? a : b, hi = a > b ? a : b; uint32_t m = hi < c ? hi : c; r = lo > m ? lo : m; break; }
        case 0x285: r = a * b; break;                                    /* v_mul_lo_u32 / v_mul_lo_i32 */
        case 0x286: r = (uint32_t)(((uint64_t)a * b) >> 32); break;      /* v_mul_hi_u32 */
        case 0x287: r = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32); break;
        case 0x288: fr = ldexpf(fa, (int32_t)b); rf = 1; break;         /* v_ldexp_f32 */
        case 0x28B: r = (uint32_t)__builtin_popcount(a) + b; break;      /* v_bcnt_u32_b32 */
        case 0x28C: r = (uint32_t)__builtin_popcount(a & (lane < 32 ? ((1u << lane) - 1u) : 0xFFFFFFFFu)) + b; break;
        case 0x28D: r = (uint32_t)__builtin_popcount(a & (lane < 32 ? 0u : (lane == 32 ? 0u : ((1u << (lane - 32)) - 1u)))) + b; break;
        default:
            FAIL(w, "VALU: opcode not modelled");
        }
        if (rf) {
            if (v->omod == 1) fr = fr * 2.0f;
            else if (v->omod == 2) fr = fr * 4.0f;
            else if (v->omod == 3) fr = fr * 0.5f;
            if (v->clamp) fr = fclamp01(fr);
            r = f2u(fr);
        }
        w->vgpr[lane][vd] = r;
    }
    if (has_cout && o2 != 0) {
        uint32_t d = v->vop3 ? v->sdst : 106u;
        sdst_write64(w, d, carry_out);
        note_valu_sgpr_write(w, d, 2);
    }
    return 0;
#undef OPND
}

/* ---- s_waitcnt ---- */
static int do_waitcnt(gcn_wave_t *w, uint32_t simm)
{
    uint32_t vm = simm & 0xFu, exp = (simm >> 4) & 7u, lgkm = (simm >> 8) & 0xFu;
    if (w->vm_issued - w->vm_done > vm) w->vm_done = w->vm_issued - vm;
    if (w->exp_issued - w->exp_done > exp) w->exp_done = w->exp_issued - exp;
    if (w->lgkm_issued - w->lgkm_done > lgkm) {
        if (lgkm != 0 && w->smem_last > w->lgkm_done)
            FAIL(w, "hazard: s_waitcnt lgkmcnt(N>0) with SMEM outstanding (SMEM returns out of order)");
        w->lgkm_done = w->lgkm_issued - lgkm;
    }
    return 0;
}

/* ---- step: execute one instruction ---- */
int gcn_wave_step(gcn_wave_t *w, const uint32_t *code, size_t code_dwords)
{
    if (w->pc >= (uint32_t)code_dwords)
        FAIL(w, "PC out of bounds");
    uint32_t insn = code[w->pc];
    uint32_t pc0 = w->pc;
    w->pc++;
    w->insn_count++;
    w->ws++;
#define LIT() (w->pc < code_dwords ? code[w->pc] : 0u)
    uint32_t major = insn >> 30;

    /* ---------------- scalar ---------------- */
    if (major == 2 && (insn >> 28) != 0xBu) { /* SOP2 */
        uint32_t op = (insn >> 23) & 0x7fu, sdst = (insn >> 16) & 0x7fu;
        uint32_t ssrc1 = (insn >> 8) & 0xffu, ssrc0 = insn & 0xffu;
        uint32_t lit = LIT();
        int uses_lit = (ssrc0 == 255 || ssrc1 == 255);
        if (src_hazard(w, ssrc0, 2) || src_hazard(w, ssrc1, 2))
            FAIL(w, "hazard: SALU reads an SGPR a load still owes");
        uint32_t s0 = scalar_src(w, ssrc0, lit), s1 = scalar_src(w, ssrc1, lit);
        uint64_t a = scalar_src64(w, ssrc0, lit), b = scalar_src64(w, ssrc1, lit);
        uint32_t res = 0; uint64_t r64 = 0; int is64 = 0;
        switch (op) {
        case 0: { uint64_t s = (uint64_t)s0 + s1; res = (uint32_t)s; w->scc = (uint32_t)(s >> 32); break; }
        case 1: res = s0 - s1; w->scc = s1 > s0; break;
        case 2: { int64_t s = (int64_t)(int32_t)s0 + (int32_t)s1; res = (uint32_t)s; w->scc = s != (int32_t)res; break; }
        case 3: { int64_t s = (int64_t)(int32_t)s0 - (int32_t)s1; res = (uint32_t)s; w->scc = s != (int32_t)res; break; }
        case 4: { uint64_t s = (uint64_t)s0 + s1 + (w->scc & 1); res = (uint32_t)s; w->scc = (uint32_t)(s >> 32); break; }
        case 5: { uint64_t bb = (uint64_t)s1 + (w->scc & 1); res = (uint32_t)((uint64_t)s0 - bb); w->scc = bb > s0; break; }
        case 6: w->scc = (int32_t)s0 < (int32_t)s1; res = w->scc ? s0 : s1; break;
        case 7: w->scc = s0 < s1; res = w->scc ? s0 : s1; break;
        case 8: w->scc = (int32_t)s0 > (int32_t)s1; res = w->scc ? s0 : s1; break;
        case 9: w->scc = s0 > s1; res = w->scc ? s0 : s1; break;
        case 10: res = w->scc ? s0 : s1; break;
        case 11: r64 = w->scc ? a : b; is64 = 1; break;
        case 12: res = s0 & s1; w->scc = res != 0; break;
        case 13: r64 = a & b; is64 = 1; w->scc = r64 != 0; break;
        case 14: res = s0 | s1; w->scc = res != 0; break;
        case 15: r64 = a | b; is64 = 1; w->scc = r64 != 0; break;
        case 16: res = s0 ^ s1; w->scc = res != 0; break;
        case 17: r64 = a ^ b; is64 = 1; w->scc = r64 != 0; break;
        case 18: res = s0 & ~s1; w->scc = res != 0; break;
        case 19: r64 = a & ~b; is64 = 1; w->scc = r64 != 0; break;
        case 20: res = s0 | ~s1; w->scc = res != 0; break;
        case 21: r64 = a | ~b; is64 = 1; w->scc = r64 != 0; break;
        case 22: res = ~(s0 & s1); w->scc = res != 0; break;
        case 23: r64 = ~(a & b); is64 = 1; w->scc = r64 != 0; break;
        case 24: res = ~(s0 | s1); w->scc = res != 0; break;
        case 25: r64 = ~(a | b); is64 = 1; w->scc = r64 != 0; break;
        case 26: res = ~(s0 ^ s1); w->scc = res != 0; break;
        case 27: r64 = ~(a ^ b); is64 = 1; w->scc = r64 != 0; break;
        case 28: res = s0 << (s1 & 31); w->scc = res != 0; break;
        case 29: r64 = a << (s1 & 63); is64 = 1; w->scc = r64 != 0; break;
        case 30: res = s0 >> (s1 & 31); w->scc = res != 0; break;
        case 31: r64 = a >> (s1 & 63); is64 = 1; w->scc = r64 != 0; break;
        case 32: res = (uint32_t)((int32_t)s0 >> (s1 & 31)); w->scc = res != 0; break;
        case 33: r64 = (uint64_t)((int64_t)a >> (s1 & 63)); is64 = 1; w->scc = r64 != 0; break;
        case 34: res = (((s0 & 31) == 0) ? 0u : ((1u << (s0 & 31)) - 1u)) << (s1 & 31); break;
        case 36: res = s0 * s1; break;
        case 37: { uint32_t off = s1 & 0x1f, sz = (s1 >> 16) & 0x7f; res = sz ? (s0 >> off) & (sz >= 32 ? 0xFFFFFFFFu : ((1u << sz) - 1u)) : 0u; w->scc = res != 0; break; }
        case 38: { uint32_t off = s1 & 0x1f, sz = (s1 >> 16) & 0x7f; if (!sz) res = 0; else { uint32_t x = s0 >> off; if (sz < 32) { x &= (1u << sz) - 1u; if (x >> (sz - 1)) x |= ~((1u << sz) - 1u); } res = x; } w->scc = res != 0; break; }
        case 42: { int32_t d = (int32_t)s0 - (int32_t)s1; res = (uint32_t)(d < 0 ? -d : d); w->scc = res != 0; break; }
        default: FAIL(w, "SOP2: opcode not modelled");
        }
        if (sdst_hazard(w, sdst, is64 ? 2 : 1)) FAIL(w, "hazard: SALU overwrites an SGPR a load still owes");
        if (is64) sdst_write64(w, sdst, r64); else sdst_write(w, sdst, res);
        if (uses_lit) w->pc++;
        return 0;
    }

    if ((insn >> 23) == 0x17Du) { /* SOP1 */
        uint32_t sdst = (insn >> 16) & 0x7fu, op = (insn >> 8) & 0xffu, ssrc0 = insn & 0xffu;
        uint32_t lit = LIT();
        if (src_hazard(w, ssrc0, 2)) FAIL(w, "hazard: SALU reads an SGPR a load still owes");
        uint32_t s0 = scalar_src(w, ssrc0, lit);
        uint64_t a = scalar_src64(w, ssrc0, lit);
        uint64_t ex = exec64(w);
        uint32_t res = 0; uint64_t r64 = 0; int is64 = 0, wr = 1;
        switch (op) {
        case 0: res = s0; break;
        case 1: r64 = a; is64 = 1; break;
        case 2: if (w->scc) res = s0; else wr = 0; break;
        case 3: if (w->scc) { r64 = a; is64 = 1; } else wr = 0; break;
        case 4: res = ~s0; w->scc = res != 0; break;
        case 5: r64 = ~a; is64 = 1; w->scc = r64 != 0; break;
        case 8: { uint32_t x = s0, y = 0; for (int i = 0; i < 32; i++) { y = (y << 1) | (x & 1); x >>= 1; } res = y; break; }
        case 10: res = (uint32_t)__builtin_popcount(~s0); w->scc = res != 0; break;
        case 12: res = (uint32_t)__builtin_popcount(s0); w->scc = res != 0; break;
        case 13: res = (uint32_t)__builtin_popcountll(a); w->scc = res != 0; break;
        case 16: res = s0 ? (uint32_t)__builtin_ctz(s0) : 0xFFFFFFFFu; break;
        case 17: res = a ? (uint32_t)__builtin_ctzll(a) : 0xFFFFFFFFu; break;
        case 18: res = s0 ? (uint32_t)__builtin_clz(s0) : 0xFFFFFFFFu; break;
        case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39: {
            uint64_t ne;
            switch (op) {
            case 32: ne = a & ex; break;          /* s_and_saveexec_b64 */
            case 33: ne = a | ex; break;          /* s_or_saveexec_b64 */
            case 34: ne = a ^ ex; break;          /* s_xor_saveexec_b64 */
            case 35: ne = a & ~ex; break;         /* s_andn2_saveexec_b64 */
            case 36: ne = a | ~ex; break;         /* s_orn2_saveexec_b64 */
            case 37: ne = ~(a & ex); break;
            case 38: ne = ~(a | ex); break;
            default: ne = ~(a ^ ex); break;
            }
            r64 = ex; is64 = 1;
            if (sdst_hazard(w, sdst, 2)) FAIL(w, "hazard: SALU overwrites an SGPR a load still owes");
            sdst_write64(w, sdst, r64);
            w->exec_lo = (uint32_t)ne; w->exec_hi = (uint32_t)(ne >> 32);
            w->scc = ne != 0;
            if (ssrc0 == 255) w->pc++;
            return 0;
        }
        case 48: res = ((int32_t)s0 < 0) ? (uint32_t)(-(int32_t)s0) : s0; w->scc = res != 0; break;
        default: FAIL(w, "SOP1: opcode not modelled");
        }
        if (wr) {
            if (sdst_hazard(w, sdst, is64 ? 2 : 1)) FAIL(w, "hazard: SALU overwrites an SGPR a load still owes");
            if (is64) sdst_write64(w, sdst, r64); else sdst_write(w, sdst, res);
        }
        if (ssrc0 == 255) w->pc++;
        return 0;
    }

    if ((insn >> 28) == 0xBu && (insn >> 23) < 0x17Du) { /* SOPK (0x17D.. are SOP1/SOPC/SOPP) */
        uint32_t op = (insn >> 23) & 0x1fu, sdst = (insn >> 16) & 0x7fu;
        uint32_t uimm = insn & 0xffffu;
        int32_t simm = (int16_t)uimm;
        uint32_t d = scalar_src(w, sdst, 0);
        if (op != 0 && src_hazard(w, sdst, 1)) FAIL(w, "hazard: SALU reads an SGPR a load still owes");
        switch (op) {
        case 0: if (sdst_hazard(w, sdst, 1)) FAIL(w, "hazard"); sdst_write(w, sdst, (uint32_t)simm); break;
        case 1: if (w->scc) sdst_write(w, sdst, (uint32_t)simm); break;
        case 2: w->scc = (int32_t)d == simm; break;
        case 3: w->scc = (int32_t)d != simm; break;
        case 4: w->scc = (int32_t)d >  simm; break;
        case 5: w->scc = (int32_t)d >= simm; break;
        case 6: w->scc = (int32_t)d <  simm; break;
        case 7: w->scc = (int32_t)d <= simm; break;
        case 8: w->scc = d == uimm; break;
        case 9: w->scc = d != uimm; break;
        case 10: w->scc = d >  uimm; break;
        case 11: w->scc = d >= uimm; break;
        case 12: w->scc = d <  uimm; break;
        case 13: w->scc = d <= uimm; break;
        case 14: { int64_t s = (int64_t)(int32_t)d + simm; sdst_write(w, sdst, (uint32_t)s); w->scc = s != (int32_t)(uint32_t)s; break; }
        case 15: sdst_write(w, sdst, d * (uint32_t)simm); break;
        default: FAIL(w, "SOPK: opcode not modelled");
        }
        return 0;
    }

    if ((insn >> 23) == 0x17Eu) { /* SOPC */
        uint32_t op = (insn >> 16) & 0x7fu, ssrc1 = (insn >> 8) & 0xffu, ssrc0 = insn & 0xffu;
        uint32_t lit = LIT();
        if (src_hazard(w, ssrc0, 1) || src_hazard(w, ssrc1, 1)) FAIL(w, "hazard: SALU reads an SGPR a load still owes");
        uint32_t s0 = scalar_src(w, ssrc0, lit), s1 = scalar_src(w, ssrc1, lit);
        switch (op) {
        case 0: w->scc = (int32_t)s0 == (int32_t)s1; break;
        case 1: w->scc = (int32_t)s0 != (int32_t)s1; break;
        case 2: w->scc = (int32_t)s0 >  (int32_t)s1; break;
        case 3: w->scc = (int32_t)s0 >= (int32_t)s1; break;
        case 4: w->scc = (int32_t)s0 <  (int32_t)s1; break;
        case 5: w->scc = (int32_t)s0 <= (int32_t)s1; break;
        case 6: w->scc = s0 == s1; break;
        case 7: w->scc = s0 != s1; break;
        case 8: w->scc = s0 >  s1; break;
        case 9: w->scc = s0 >= s1; break;
        case 10: w->scc = s0 <  s1; break;
        case 11: w->scc = s0 <= s1; break;
        case 12: w->scc = ((s0 >> (s1 & 31)) & 1) == 0; break;
        case 13: w->scc = ((s0 >> (s1 & 31)) & 1) == 1; break;
        default: FAIL(w, "SOPC: opcode not modelled");
        }
        if (ssrc0 == 255 || ssrc1 == 255) w->pc++;
        return 0;
    }

    if ((insn >> 23) == 0x17Fu) { /* SOPP */
        uint32_t op = (insn >> 16) & 0x7fu;
        int16_t simm = (int16_t)(insn & 0xffffu);
        int32_t tgt = (int32_t)w->pc + simm;
        switch (op) {
        case 0: w->ws += (uint32_t)(insn & 0xfu); break;           /* s_nop N: N+1 wait states */
        case 1: w->pc = pc0; return -1;                              /* s_endpgm */
        case 2: w->pc = (uint32_t)tgt; break;
        case 4: if (!w->scc) w->pc = (uint32_t)tgt; break;
        case 5: if (w->scc)  w->pc = (uint32_t)tgt; break;
        case 6: if (!(w->vcc_lo | w->vcc_hi)) w->pc = (uint32_t)tgt; break;
        case 7: if (w->vcc_lo | w->vcc_hi) w->pc = (uint32_t)tgt; break;
        case 8: if (!(w->exec_lo | w->exec_hi)) w->pc = (uint32_t)tgt; break;
        case 9: if (w->exec_lo | w->exec_hi) w->pc = (uint32_t)tgt; break;
        case 10: break;                                              /* s_barrier: one wave per group */
        case 12: return do_waitcnt(w, insn & 0xffffu);
        case 14: case 15: case 19: break;                            /* s_sleep, s_setprio, s_icache_inv */
        default: FAIL(w, "SOPP: opcode not modelled");
        }
        return 0;
    }

    /* ---------------- vector ALU ---------------- */
    if ((insn >> 25) == 0x3Fu) { /* VOP1 */
        valu_t v; memset(&v, 0, sizeof v);
        v.op = 0x140 + ((insn >> 9) & 0xffu);
        v.vdst = (insn >> 17) & 0xffu;
        v.src[0] = insn & 0x1ffu; v.nsrc = 1;
        v.lit = LIT();
        if (v.op == 0x140) { if (v.src[0] == 255) w->pc++; return 0; } /* v_nop */
        if (valu_exec(w, &v)) return -1;
        if (v.src[0] == 255) w->pc++;
        return 0;
    }
    if ((insn >> 25) == 0x3Eu) { /* VOPC */
        valu_t v; memset(&v, 0, sizeof v);
        v.op = (insn >> 17) & 0xffu;
        v.sdst = 106;
        v.src[0] = insn & 0x1ffu; v.src[1] = 256 + ((insn >> 9) & 0xffu); v.nsrc = 2;
        v.lit = LIT();
        if (valu_exec(w, &v)) return -1;
        if (v.src[0] == 255) w->pc++;
        return 0;
    }
    if ((insn >> 31) == 0) { /* VOP2 */
        valu_t v; memset(&v, 0, sizeof v);
        uint32_t o = (insn >> 25) & 0x3fu;
        v.op = 0x100 + o;
        v.vdst = (insn >> 17) & 0xffu;
        v.src[0] = insn & 0x1ffu; v.src[1] = 256 + ((insn >> 9) & 0xffu); v.nsrc = 2;
        v.lit = LIT();
        int has_k = (o == 23 || o == 24);
        if (v.src[0] == 255 && has_k) FAIL(w, "VOP2: madmk/madak cannot take a second literal");
        if (has_k) {
            /* the K literal is not a constant-bus operand of the source list */
        }
        if (valu_exec(w, &v)) return -1;
        if (v.src[0] == 255 || has_k) w->pc++;
        return 0;
    }
    if ((insn >> 26) == 0x34u) { /* VOP3a / VOP3b */
        if (w->pc >= code_dwords) FAIL(w, "VOP3: truncated");
        uint32_t w1 = code[w->pc++];
        valu_t v; memset(&v, 0, sizeof v);
        v.vop3 = 1;
        v.op = (insn >> 16) & 0x3ffu;
        v.vdst = insn & 0xffu;
        v.clamp = (insn >> 15) & 1u;
        v.src[0] = w1 & 0x1ffu; v.src[1] = (w1 >> 9) & 0x1ffu; v.src[2] = (w1 >> 18) & 0x1ffu;
        v.omod = (w1 >> 27) & 3u;
        v.neg = (w1 >> 29) & 7u;
        uint32_t o = v.op;
        int vop3b = (o >= 0x119 && o <= 0x11E) || o == 0x1E0 || o == 0x1E1;
        if (vop3b) { v.sdst = (insn >> 8) & 0x7fu; v.abs = 0; }
        else v.abs = (insn >> 8) & 7u;
        if (o < 0x100) { v.sdst = v.vdst; v.nsrc = 2; }
        else if (o < 0x140) {
            uint32_t o2 = o - 0x100;
            v.nsrc = (o2 == 0 || o2 == 28 || o2 == 29 || o2 == 30) ? 3 : 2;
            if (o2 == 22 || o2 == 23 || o2 == 24) FAIL(w, "VOP3: mac/madmk/madak e64 not modelled");
        }
        else if (o < 0x1C0) v.nsrc = 1;
        else if (o >= 0x285) v.nsrc = 2;
        else v.nsrc = 3;
        if (o == 0x289 || o == 0x28A) {
            /* readlane/writelane: src1 is the lane select SGPR or inline const */
        }
        if (valu_exec(w, &v)) return -1;
        return 0;
    }

    /* ---------------- memory ---------------- */
    if ((insn >> 26) == 0x30u) { /* SMEM (VI) */
        if (w->pc >= code_dwords) FAIL(w, "SMEM: truncated");
        uint32_t w1 = code[w->pc++];
        uint32_t op = (insn >> 18) & 0xffu, imm = (insn >> 17) & 1u;
        uint32_t sdst = (insn >> 6) & 0x7fu, sbase = (insn & 0x3fu) * 2u;
        if (op == 0x20 || op == 0x21 || op == 0x22 || op == 0x23) return 0; /* s_dcache_inv / wb */
        if (op == 0x24 || op == 0x25) {                              /* s_memtime / s_memrealtime */
            if (sdst_hazard(w, sdst, 2)) FAIL(w, "hazard");
            sdst_write64(w, sdst, w->insn_count);
            w->lgkm_issued++; w->smem_last = w->lgkm_issued;
            if (sdst + 1 < GCN_NUM_SGPR) w->s_lgkm[sdst] = w->s_lgkm[sdst + 1] = w->lgkm_issued;
            return 0;
        }
        uint32_t n;
        int buf = 0;
        switch (op) {
        case 0: n = 1; break; case 1: n = 2; break; case 2: n = 4; break;
        case 3: n = 8; break; case 4: n = 16; break;
        case 8: n = 1; buf = 1; break; case 9: n = 2; buf = 1; break; case 10: n = 4; buf = 1; break;
        case 11: n = 8; buf = 1; break; case 12: n = 16; buf = 1; break;
        default: FAIL(w, "SMEM: opcode not modelled");
        }
        if (src_hazard(w, sbase, buf ? 4u : 2u)) FAIL(w, "hazard: SMEM base SGPR still owed by a load");
        uint32_t off;
        if (imm) off = w1 & 0xFFFFFu;
        else {
            if (src_hazard(w, w1 & 0xffu, 1)) FAIL(w, "hazard: SMEM offset SGPR still owed by a load");
            off = scalar_src(w, w1 & 0xffu, 0);
        }
        uint64_t base = (uint64_t)w->sgpr[sbase] | ((uint64_t)(w->sgpr[sbase + 1] & (buf ? 0xFFFFu : 0xFFFFFFFFu)) << 32);
        if (off & 3u) FAIL(w, "SMEM: unaligned offset");
        if (sdst + n > GCN_NUM_SGPR) FAIL(w, "SMEM: destination past s103");
        if ((n == 2 && (sdst & 1u)) || (n >= 4 && (sdst & 3u))) FAIL(w, "SMEM: destination tuple misaligned");
        if (sdst_hazard(w, sdst, n)) FAIL(w, "hazard: SMEM overwrites an SGPR a load still owes");
        for (uint32_t i = 0; i < n; i++) {
            uint32_t val = 0;
            uint8_t *p;
            if (buf && (uint64_t)off + 4u * i + 4u > w->sgpr[sbase + 2]) { w->oob++; }
            else {
                if (mem_ptr(w, base + off + 4u * i, 4, &p)) FAIL(w, "SMEM: address outside memory");
                memcpy(&val, p, 4);
            }
            w->sgpr[sdst + i] = val;
        }
        w->vmem_bytes += 4u * n;
        w->lgkm_issued++;
        w->smem_last = w->lgkm_issued;
        for (uint32_t i = 0; i < n; i++) w->s_lgkm[sdst + i] = w->lgkm_issued;
        return 0;
    }

    if ((insn >> 26) == 0x36u) { /* DS */
        if (w->pc >= code_dwords) FAIL(w, "DS: truncated");
        uint32_t w1 = code[w->pc++];
        uint32_t op = (insn >> 17) & 0xffu;
        uint32_t off0 = insn & 0xffu, off1 = (insn >> 8) & 0xffu;
        if ((insn >> 16) & 1u) FAIL(w, "DS: GDS not modelled");
        uint32_t vdst = (w1 >> 24) & 0xffu, d1 = (w1 >> 16) & 0xffu, d0 = (w1 >> 8) & 0xffu, av = w1 & 0xffu;
        uint64_t ex = exec64(w);
        uint32_t nread = 0, nw0 = 0, nw1 = 0, size = 4, two = 0, st64 = 0;
        int is_read = 0;
        switch (op) {
        case 0x0D: nw0 = 1; break;                           /* ds_write_b32 */
        case 0x0E: nw0 = 1; nw1 = 1; two = 1; break;         /* ds_write2_b32 */
        case 0x0F: nw0 = 1; nw1 = 1; two = 1; st64 = 1; break;
        case 0x1E: nw0 = 1; size = 1; break;                 /* ds_write_b8 */
        case 0x1F: nw0 = 1; size = 2; break;                 /* ds_write_b16 */
        case 0x4D: nw0 = 2; size = 8; break;                 /* ds_write_b64 */
        case 0x36: nread = 1; is_read = 1; break;            /* ds_read_b32 */
        case 0x37: nread = 2; is_read = 1; two = 1; break;   /* ds_read2_b32 */
        case 0x38: nread = 2; is_read = 1; two = 1; st64 = 1; break;
        case 0x3A: nread = 1; is_read = 1; size = 1; break;  /* ds_read_u8 */
        case 0x3C: nread = 1; is_read = 1; size = 2; break;  /* ds_read_u16 */
        case 0x76: nread = 2; is_read = 1; size = 8; break;  /* ds_read_b64 */
        default: FAIL(w, "DS: opcode not modelled");
        }
        if (src_hazard(w, 256 + av, 1)) FAIL(w, "hazard: DS address VGPR still owed by a load");
        if (nw0 && src_hazard(w, 256 + d0, nw0)) FAIL(w, "hazard: DS data VGPR still owed by a load");
        if (nw1 && src_hazard(w, 256 + d1, 1)) FAIL(w, "hazard: DS data VGPR still owed by a load");
        if (is_read && vdst_hazard(w, vdst, nread)) FAIL(w, "hazard: DS read overwrites a VGPR with an access outstanding");
        for (uint32_t lane = 0; lane < 64; lane++) {
            if (!((ex >> lane) & 1)) continue;
            uint32_t base = w->vgpr[lane][av];
            uint32_t addr[2]; uint32_t na = two ? 2 : 1;
            if (two) {
                uint32_t sc = st64 ? 256u : 4u;
                addr[0] = base + off0 * sc; addr[1] = base + off1 * sc;
            } else addr[0] = base + (off0 | (off1 << 8));
            for (uint32_t k = 0; k < na; k++) {
                uint32_t sz = two ? 4u : size;
                if ((uint64_t)addr[k] + sz > GCN_LDS_BYTES || (uint64_t)addr[k] + sz > (uint64_t)w->m0)
                    FAIL(w, "DS: address beyond LDS or M0 (set M0 = -1)");
                if (addr[k] % (sz > 4 ? 4u : sz)) FAIL(w, "DS: unaligned address");
                if (addr[k] + sz > w->lds_hi) w->lds_hi = addr[k] + sz;
                if (is_read) {
                    uint32_t lo = 0, hi = 0;
                    if (sz == 8) { memcpy(&lo, w->lds + addr[k], 4); memcpy(&hi, w->lds + addr[k] + 4, 4); }
                    else memcpy(&lo, w->lds + addr[k], sz);
                    if (sz == 8) { w->vgpr[lane][vdst] = lo; w->vgpr[lane][vdst + 1] = hi; }
                    else w->vgpr[lane][vdst + k] = lo;
                } else {
                    if (sz == 8) {
                        memcpy(w->lds + addr[k], &w->vgpr[lane][d0], 4);
                        memcpy(w->lds + addr[k] + 4, &w->vgpr[lane][d0 + 1], 4);
                    } else {
                        uint32_t val = w->vgpr[lane][k ? d1 : d0];
                        memcpy(w->lds + addr[k], &val, sz);
                    }
                }
            }
        }
        w->lgkm_issued++;
        if (is_read) for (uint32_t i = 0; i < nread; i++) w->v_lgkm[vdst + i] = w->lgkm_issued;
        return 0;
    }

    if ((insn >> 26) == 0x38u) { /* MUBUF (VI) */
        if (w->pc >= code_dwords) FAIL(w, "MUBUF: truncated");
        uint32_t w1 = code[w->pc++];
        uint32_t op = (insn >> 18) & 0x7fu;
        uint32_t offen = (insn >> 12) & 1u, idxen = (insn >> 13) & 1u, lds = (insn >> 16) & 1u;
        uint32_t ioff = insn & 0xfffu;
        uint32_t soffset = (w1 >> 24) & 0xffu, srsrc = ((w1 >> 16) & 0x1fu) * 4u;
        uint32_t vdata = (w1 >> 8) & 0xffu, vaddr = w1 & 0xffu;
        if (idxen || lds) FAIL(w, "MUBUF: idxen/lds not modelled");
        uint32_t n = 0, size = 4, store = 0, sext = 0;
        switch (op) {
        case 0x10: n = 1; size = 1; break;
        case 0x11: n = 1; size = 1; sext = 1; break;
        case 0x12: n = 1; size = 2; break;
        case 0x13: n = 1; size = 2; sext = 1; break;
        case 0x14: n = 1; break;
        case 0x15: n = 2; break;
        case 0x16: n = 3; break;
        case 0x17: n = 4; break;
        case 0x18: n = 1; size = 1; store = 1; break;
        case 0x19: n = 1; size = 2; store = 1; break;
        case 0x1C: n = 1; store = 1; break;
        case 0x1D: n = 2; store = 1; break;
        case 0x1E: n = 3; store = 1; break;
        case 0x1F: n = 4; store = 1; break;
        default: FAIL(w, "MUBUF: opcode not modelled");
        }
        for (uint32_t i = 0; i < 4; i++)
            if (vmem_sgpr_hazard(w, srsrc + i)) FAIL(w, "hazard: VMEM reads a descriptor SGPR written by VALU < 5 wait states ago");
        if (vmem_sgpr_hazard(w, soffset)) FAIL(w, "hazard: VMEM reads soffset written by VALU < 5 wait states ago");
        if (src_hazard(w, srsrc, 4) || src_hazard(w, soffset, 1)) FAIL(w, "hazard: VMEM descriptor/soffset SGPR still owed by a load");
        if (offen && src_hazard(w, 256 + vaddr, 1)) FAIL(w, "hazard: VMEM address VGPR still owed by a load");
        if (store && src_hazard(w, 256 + vdata, n)) FAIL(w, "hazard: VMEM store data VGPR still owed by a load");
        if (!store && vdst_hazard(w, vdata, n)) FAIL(w, "hazard: VMEM load overwrites a VGPR with an access outstanding");
        uint32_t d0 = w->sgpr[srsrc], d1 = w->sgpr[srsrc + 1], d2 = w->sgpr[srsrc + 2], d3 = w->sgpr[srsrc + 3];
        if (((d3 >> 15) & 0xfu) == 0) FAIL(w, "MUBUF: descriptor DATA_FORMAT is 0 (invalid)");
        if ((d1 >> 16) & 0x3fffu) FAIL(w, "MUBUF: strided descriptor not modelled");
        uint64_t base = (uint64_t)d0 | ((uint64_t)(d1 & 0xffffu) << 32);
        uint32_t soff = scalar_src(w, soffset, 0);
        uint64_t ex = exec64(w);
        uint32_t bytes = size * n;
        for (uint32_t lane = 0; lane < 64; lane++) {
            if (!((ex >> lane) & 1)) continue;
            uint32_t voff = offen ? w->vgpr[lane][vaddr] : 0u;
            uint64_t roff = (uint64_t)voff + ioff;
            if (roff + bytes > d2) {
                w->oob++;
                if (!store) for (uint32_t i = 0; i < n; i++) w->vgpr[lane][vdata + i] = 0;
                continue;
            }
            uint64_t addr = base + soff + roff;
            uint8_t *p;
            if (mem_ptr(w, addr, bytes, &p)) FAIL(w, "MUBUF: address outside memory");
            if (store) {
                if (size == 4) memcpy(p, &w->vgpr[lane][vdata], 4u * n);
                else memcpy(p, &w->vgpr[lane][vdata], size);
            } else {
                if (size == 4) memcpy(&w->vgpr[lane][vdata], p, 4u * n);
                else {
                    uint32_t val = 0; memcpy(&val, p, size);
                    if (sext) val = size == 1 ? (uint32_t)(int32_t)(int8_t)val : (uint32_t)(int32_t)(int16_t)val;
                    w->vgpr[lane][vdata] = val;
                }
            }
            w->vmem_bytes += bytes;
        }
        w->vm_issued++;
        if (store) {
            w->exp_issued++;
            for (uint32_t i = 0; i < n; i++) w->v_exp[vdata + i] = w->exp_issued;
        } else {
            for (uint32_t i = 0; i < n; i++) w->v_vm[vdata + i] = w->vm_issued;
        }
        return 0;
    }

    if ((insn >> 26) == 0x37u) { /* FLAT */
        if (w->pc >= code_dwords) FAIL(w, "FLAT: truncated");
        uint32_t w1 = code[w->pc++];
        uint32_t op = (insn >> 18) & 0x7fu;
        uint32_t vdst = (w1 >> 24) & 0xffu, vdata = (w1 >> 8) & 0xffu, vaddr = w1 & 0xffu;
        uint32_t n = 0, store = 0;
        switch (op) {
        case 0x14: n = 1; break; case 0x15: n = 2; break; case 0x17: n = 4; break;
        case 0x1C: n = 1; store = 1; break; case 0x1D: n = 2; store = 1; break; case 0x1F: n = 4; store = 1; break;
        default: FAIL(w, "FLAT: opcode not modelled");
        }
        if (src_hazard(w, 256 + vaddr, 2)) FAIL(w, "hazard: FLAT address VGPR still owed by a load");
        if (store && src_hazard(w, 256 + vdata, n)) FAIL(w, "hazard: FLAT store data still owed");
        if (!store && vdst_hazard(w, vdst, n)) FAIL(w, "hazard: FLAT load overwrites a busy VGPR");
        uint64_t ex = exec64(w);
        for (uint32_t lane = 0; lane < 64; lane++) {
            if (!((ex >> lane) & 1)) continue;
            uint64_t addr = (uint64_t)w->vgpr[lane][vaddr] | ((uint64_t)w->vgpr[lane][vaddr + 1] << 32);
            uint8_t *p;
            if (mem_ptr(w, addr, 4u * n, &p)) FAIL(w, "FLAT: address outside memory");
            if (store) memcpy(p, &w->vgpr[lane][vdata], 4u * n);
            else memcpy(&w->vgpr[lane][vdst], p, 4u * n);
            w->vmem_bytes += 4u * n;
        }
        /* FLAT counts in both vmcnt and lgkmcnt */
        w->vm_issued++; w->lgkm_issued++;
        if (store) { w->exp_issued++; for (uint32_t i = 0; i < n; i++) w->v_exp[vdata + i] = w->exp_issued; }
        else for (uint32_t i = 0; i < n; i++) { w->v_vm[vdst + i] = w->vm_issued; }
        return 0;
    }

    FAIL(w, "unknown instruction encoding");
#undef LIT
}

/* ---- run: execute until S_ENDPGM, error or step limit ---- */
uint64_t gcn_wave_run(gcn_wave_t *w, const uint32_t *code, size_t code_dwords)
{
    while (1) {
        if (w->step_limit && w->insn_count >= w->step_limit) {
            w->error = "step limit reached";
            break;
        }
        if (gcn_wave_step(w, code, code_dwords) != 0)
            break; /* s_endpgm (error stays NULL) or error */
    }
    return w->insn_count;
}
