/* native/kernel/gpu/gcn_interp.c -- Host-side GCN wavefront interpreter.
 * All arithmetic on float values uses uint32_t union casts to avoid
 * kernel-C float restriction.  The actual computation is in the GPU instruction
 * semantics (f32 add, mul, etc.) using the host's FPU -- that is fine since
 * this is a host-only test tool, never compiled into the kernel.
 */
#include "gcn_interp.h"
#include <string.h>

/* ---- float helpers (host only, never compiled into kernel) ---- */
static inline float u2f(uint32_t u) { float f; __builtin_memcpy(&f, &u, 4); return f; }
static inline uint32_t f2u(float f)  { uint32_t u; __builtin_memcpy(&u, &f, 4); return u; }

/* ---- init ---- */
void gcn_wave_init(gcn_wave_t *w)
{
    memset(w, 0, sizeof *w);
    w->exec_lo = 0xFFFFFFFFu;
    w->exec_hi = 0xFFFFFFFFu;
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
    /* EXEC mask: lower 'nthreads' bits set */
    if (nthreads >= 64) {
        w->exec_lo = 0xFFFFFFFFu;
        w->exec_hi = 0xFFFFFFFFu;
    } else if (nthreads >= 32) {
        w->exec_lo = 0xFFFFFFFFu;
        w->exec_hi = (1u << (nthreads - 32)) - 1u;
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

/* ---- SRC0 read ---- */
uint32_t gcn_src_read(const gcn_wave_t *w, uint32_t src, uint32_t lane,
                      const uint32_t *literal_ptr)
{
    if (src < 104)
        return w->sgpr[src];
    if (src == 106) return w->vcc_lo;
    if (src == 107) return w->vcc_hi;
    if (src == 124) return w->m0;
    if (src == 126) return w->exec_lo;
    if (src == 127) return w->exec_hi;
    if (src == 128) return 0u;
    if (src >= 129 && src <= 192) return src - 128u;   /* inline 1..64 */
    if (src >= 193 && src <= 208) return (uint32_t)(-(int32_t)(src - 192u)); /* -1..-16 */
    if (src == 240) return 0x3F000000u; /* 0.5 */
    if (src == 241) return 0xBF000000u; /* -0.5 */
    if (src == 242) return 0x3F800000u; /* 1.0 */
    if (src == 243) return 0xBF800000u; /* -1.0 */
    if (src == 244) return 0x40000000u; /* 2.0 */
    if (src == 245) return 0xC0000000u; /* -2.0 */
    if (src == 246) return 0x40800000u; /* 4.0 */
    if (src == 247) return 0xC0800000u; /* -4.0 */
    if (src == 255) return literal_ptr ? *literal_ptr : 0u;
    if (src >= 256 && src <= 511)
        return w->vgpr[lane][src - 256u];
    return 0u; /* unknown */
}

/* ---- EXEC lane check ---- */
static inline int exec_lane(const gcn_wave_t *w, uint32_t lane)
{
    if (lane < 32)
        return (w->exec_lo >> lane) & 1u;
    else
        return (w->exec_hi >> (lane - 32u)) & 1u;
}

/* ---- Memory access helpers ---- */
static int mem_load32(const gcn_wave_t *w, uint64_t addr, uint32_t *out)
{
    if (addr < w->mem_base || addr + 4u > w->mem_base + w->mem_size)
        return -1;
    uint64_t off = addr - w->mem_base;
    __builtin_memcpy(out, w->mem + off, 4);
    return 0;
}

static int mem_store32(gcn_wave_t *w, uint64_t addr, uint32_t val)
{
    if (addr < w->mem_base || addr + 4u > w->mem_base + w->mem_size)
        return -1;
    uint64_t off = addr - w->mem_base;
    __builtin_memcpy(w->mem + off, &val, 4);
    return 0;
}

/* LDS helpers */
static uint32_t lds_load32(const gcn_wave_t *w, uint32_t addr)
{
    uint32_t v;
    __builtin_memcpy(&v, w->lds + (addr & 0xffffu), 4);
    return v;
}
static void lds_store32(gcn_wave_t *w, uint32_t addr, uint32_t val)
{
    __builtin_memcpy(w->lds + (addr & 0xffffu), &val, 4);
}

/* ---- step: execute one instruction ---- */
int gcn_wave_step(gcn_wave_t *w, const uint32_t *code, size_t code_dwords)
{
    if (w->pc >= (uint32_t)code_dwords) {
        w->error = "PC out of bounds";
        return -1;
    }
    uint32_t insn = code[w->pc];
    w->pc++;
    w->insn_count++;

    /* ---- Decode format by major encoding ---- */
    uint32_t major = insn >> 30;

    /* SOP2: [31:30]=10, [29:28] != 11 (exclude SOPK/SOP1/SOPC/SOPP) */
    if (major == 2 && (insn >> 28) != 0xBu) {
        uint32_t op    = (insn >> 23) & 0x7fu;
        uint32_t sdst  = (insn >> 16) & 0x7fu;
        uint32_t ssrc1 = (insn >>  8) & 0xffu;
        uint32_t ssrc0 =  insn        & 0xffu;
        uint32_t s0 = gcn_src_read(w, ssrc0, 0, &code[w->pc]);
        uint32_t s1 = gcn_src_read(w, ssrc1, 0, &code[w->pc]);
        uint64_t res64 = 0;
        uint32_t res = 0;
        int set_scc = 1;
        switch (op) {
        case 0:  /* S_ADD_U32 */ res64 = (uint64_t)s0 + s1; res = (uint32_t)res64; w->scc = res64 >> 32; break;
        case 1:  /* S_SUB_U32 */ res64 = (uint64_t)s0 - s1; res = (uint32_t)res64; w->scc = (s1 > s0); break;
        case 2:  /* S_ADD_I32 */ res64 = (uint64_t)(int64_t)((int32_t)s0 + (int32_t)s1); res = (uint32_t)res64; w->scc = (int64_t)((int32_t)s0) + (int32_t)s1 != (int32_t)res; break;
        case 3:  /* S_SUB_I32 */ res = s0 - s1; w->scc = (int64_t)((int32_t)s0) - (int32_t)s1 != (int32_t)res; break;
        case 12: /* S_AND_B32 */ res = s0 & s1; w->scc = (res != 0); break;
        case 14: /* S_OR_B32  */ res = s0 | s1; w->scc = (res != 0); break;
        case 16: /* S_XOR_B32 */ res = s0 ^ s1; w->scc = (res != 0); break;
        case 18: /* S_ANDN2_B32 */ res = s0 & ~s1; w->scc = (res != 0); break;
        case 20: /* S_ORN2_B32 */ res = s0 | ~s1; w->scc = (res != 0); break;
        case 28: /* S_LSHL_B32 */ res = s0 << (s1 & 31); w->scc = (res != 0); break;
        case 30: /* S_LSHR_B32 */ res = s0 >> (s1 & 31); w->scc = (res != 0); break;
        case 32: /* S_ASHR_I32 */ res = (uint32_t)((int32_t)s0 >> (s1 & 31)); w->scc = (res != 0); break;
        case 36: /* S_MUL_I32 */ res = s0 * s1; set_scc = 0; break;
        case 37: /* S_BFE_U32 */ { uint32_t off_=s1&0x1f, sz_=(s1>>16)&0x1f; res = sz_ ? (s0>>off_)&((1u<<sz_)-1u) : 0; w->scc=(res!=0); break; }
        default: set_scc = 0; res = 0; break; /* unimplemented */
        }
        if (sdst < GCN_NUM_SGPR) w->sgpr[sdst] = res;
        else if (sdst == 106) w->vcc_lo = res;
        (void)set_scc;
        return 0;
    }

    /* SOP1: [31:23]=101111101 = 0x17D */
    if ((insn >> 23) == 0x17Du) {
        uint32_t sdst  = (insn >> 16) & 0x7fu;
        uint32_t op    = (insn >>  8) & 0xffu;
        uint32_t ssrc0 =  insn        & 0xffu;
        uint32_t s0 = gcn_src_read(w, ssrc0, 0, &code[w->pc]);
        uint32_t res = 0;
        switch (op) {
        case 0: res = s0; break;      /* S_MOV_B32 */
        case 4: res = ~s0; break;     /* S_NOT_B32 */
        case 8: {                     /* S_BREV_B32: bit reverse */
            uint32_t r = 0;
            for (int b = 0; b < 32; b++) r |= ((s0>>b)&1)<<(31-b);
            res = r; break; }
        case 48: res = ((int32_t)s0 < 0) ? (uint32_t)(-(int32_t)s0) : s0; break; /* S_ABS_I32 */
        default: break;
        }
        if (sdst < GCN_NUM_SGPR) w->sgpr[sdst] = res;
        else if (sdst == 106) w->vcc_lo = res;
        else if (sdst == 126) w->exec_lo = res;
        else if (sdst == 127) w->exec_hi = res;
        return 0;
    }

    /* SOPK: [31:28]=1011 */
    if ((insn >> 28) == 0xBu) {
        uint32_t op    = (insn >> 23) & 0x1fu;
        uint32_t sdst  = (insn >> 16) & 0x7fu;
        int32_t  simm  = (int16_t)(insn & 0xffffu);
        uint32_t res = 0;
        switch (op) {
        case 0:  res = (uint32_t)simm; break;  /* S_MOVK_I32 */
        case 14: res = w->sgpr[sdst] + (uint32_t)simm; w->scc = ((int64_t)(int32_t)w->sgpr[sdst] + simm) != (int32_t)res; break; /* S_ADDK_I32 */
        case 15: res = w->sgpr[sdst] * (uint32_t)simm; break; /* S_MULK_I32 */
        default: break;
        }
        if (sdst < GCN_NUM_SGPR) w->sgpr[sdst] = res;
        return 0;
    }

    /* SOPC: [31:23]=101111110 = 0x17E */
    if ((insn >> 23) == 0x17Eu) {
        uint32_t op    = (insn >> 16) & 0x7fu;
        uint32_t ssrc1 = (insn >>  8) & 0xffu;
        uint32_t ssrc0 =  insn        & 0xffu;
        uint32_t s0 = gcn_src_read(w, ssrc0, 0, &code[w->pc]);
        uint32_t s1 = gcn_src_read(w, ssrc1, 0, &code[w->pc]);
        switch (op) {
        case 0: w->scc = ((int32_t)s0 == (int32_t)s1); break;  /* S_CMP_EQ_I32 */
        case 1: w->scc = ((int32_t)s0 != (int32_t)s1); break;  /* S_CMP_LG_I32 */
        case 2: w->scc = ((int32_t)s0 >  (int32_t)s1); break;
        case 3: w->scc = ((int32_t)s0 >= (int32_t)s1); break;
        case 4: w->scc = ((int32_t)s0 <  (int32_t)s1); break;
        case 5: w->scc = ((int32_t)s0 <= (int32_t)s1); break;
        case 6: w->scc = (s0 == s1); break;  /* S_CMP_EQ_U32 */
        case 7: w->scc = (s0 != s1); break;
        case 8: w->scc = (s0 >  s1); break;
        case 9: w->scc = (s0 >= s1); break;
        case 10: w->scc = (s0 <  s1); break;
        case 11: w->scc = (s0 <= s1); break;
        }
        return 0;
    }

    /* SOPP: [31:23]=101111111 = 0x17F */
    if ((insn >> 23) == 0x17Fu) {
        uint32_t op    = (insn >> 16) & 0x7fu;
        int16_t  simm  = (int16_t)(insn & 0xffffu);
        switch (op) {
        case 1: return -1; /* S_ENDPGM: stop execution (not an error) */
        case 0: break;     /* S_NOP */
        case 2: /* S_BRANCH */ w->pc = (uint32_t)((int32_t)w->pc + simm); break;
        case 4: if (!w->scc)  w->pc = (uint32_t)((int32_t)w->pc + simm); break; /* S_CBRANCH_SCC0 */
        case 5: if ( w->scc)  w->pc = (uint32_t)((int32_t)w->pc + simm); break; /* S_CBRANCH_SCC1 */
        case 6: if (!(w->vcc_lo|w->vcc_hi)) w->pc = (uint32_t)((int32_t)w->pc + simm); break; /* VCCZ */
        case 7: if (  w->vcc_lo|w->vcc_hi)  w->pc = (uint32_t)((int32_t)w->pc + simm); break; /* VCCNZ */
        case 10: break; /* S_BARRIER: no-op in sim */
        case 12: break; /* S_WAITCNT: no-op in sim */
        default: break;
        }
        return 0;
    }

    /* VOP1: [31:25]=0111111 = 0x3F */
    if ((insn >> 25) == 0x3Fu) {
        uint32_t vdst  = (insn >> 17) & 0xffu;
        uint32_t op    = (insn >>  9) & 0xffu;
        uint32_t src0  =  insn        & 0x1ffu;
        int extra = (src0 == 255);
        for (uint32_t lane = 0; lane < GCN_WAVE_LANES; lane++) {
            if (!exec_lane(w, lane)) continue;
            uint32_t s = gcn_src_read(w, src0, lane,
                                      extra ? &code[w->pc] : NULL);
            uint32_t res = 0;
            switch (op) {
            case 1: res = s; break; /* V_MOV_B32 */
            case 5: res = f2u((float)(int32_t)s); break; /* V_CVT_F32_I32 */
            case 6: res = f2u((float)(uint32_t)s); break; /* V_CVT_F32_U32 */
            case 7: res = (uint32_t)(int32_t)u2f(s); break; /* V_CVT_U32_F32: round to zero */
            case 8: { int32_t r = (int32_t)u2f(s); res = (uint32_t)r; break; } /* V_CVT_I32_F32 */
            case 27: { float fv = u2f(s); fv = fv - (float)(int32_t)fv; if (fv<0) fv+=1.f; res = f2u(fv); break; } /* FRACT */
            case 28: res = f2u((float)(int32_t)u2f(s)); break; /* V_TRUNC */
            case 29: { float fv = u2f(s); res = f2u((float)(int32_t)(fv+(fv>=0?1.f:0.f))); (void)fv; } /* CEIL -- simple */
                     res = f2u(__builtin_ceilf(u2f(s))); break;
            case 30: res = f2u(__builtin_rintf(u2f(s))); break; /* RNDNE */
            case 31: res = f2u(__builtin_floorf(u2f(s))); break; /* FLOOR */
            case 32: res = f2u(__builtin_expf(u2f(s))); break; /* EXP */
            case 33: res = f2u(__builtin_logf(u2f(s))); break; /* LOG */
            case 34: res = f2u(1.0f / u2f(s)); break; /* RCP */
            case 36: res = f2u(1.0f / __builtin_sqrtf(u2f(s))); break; /* RSQ */
            case 39: res = f2u(__builtin_sqrtf(u2f(s))); break; /* SQRT */
            case 41: res = f2u(__builtin_sinf(u2f(s))); break; /* SIN */
            case 42: res = f2u(__builtin_cosf(u2f(s))); break; /* COS */
            case 43: res = ~s; break; /* V_NOT_B32 */
            default: res = 0; break;
            }
            if (vdst < GCN_NUM_VGPR) w->vgpr[lane][vdst] = res;
        }
        if (extra) w->pc++;
        return 0;
    }

    /* VOP2: [31]=0 and not VOP1/VOPC prefix */
    if ((insn >> 31) == 0 && (insn >> 25) != 0x3Fu && (insn >> 25) != 0x3Eu) {
        uint32_t op    = (insn >> 25) & 0x3fu;
        uint32_t vdst  = (insn >> 17) & 0xffu;
        uint32_t vsrc1 = (insn >>  9) & 0xffu;
        uint32_t src0  =  insn        & 0x1ffu;
        int extra = (src0 == 255);
        for (uint32_t lane = 0; lane < GCN_WAVE_LANES; lane++) {
            if (!exec_lane(w, lane)) continue;
            uint32_t s0 = gcn_src_read(w, src0, lane, extra ? &code[w->pc] : NULL);
            uint32_t s1 = w->vgpr[lane][vsrc1];
            uint32_t res = 0;
            switch (op) {
            case 1:  res = f2u(u2f(s0) + u2f(s1)); break;  /* V_ADD_F32 */
            case 2:  res = f2u(u2f(s0) - u2f(s1)); break;  /* V_SUB_F32 */
            case 3:  res = f2u(u2f(s1) - u2f(s0)); break;  /* V_SUBREV_F32 */
            case 5:  res = f2u(u2f(s0) * u2f(s1)); break;  /* V_MUL_F32 */
            case 8:  res = (s0 & 0xFFFFFFu) * (s1 & 0xFFFFFFu); break; /* V_MUL_U32_U24 */
            case 10: res = f2u(u2f(s0) < u2f(s1) ? u2f(s0) : u2f(s1)); break; /* V_MIN_F32 */
            case 11: res = f2u(u2f(s0) > u2f(s1) ? u2f(s0) : u2f(s1)); break; /* V_MAX_F32 */
            case 12: res = (int32_t)s0 < (int32_t)s1 ? s0 : s1; break; /* V_MIN_I32 */
            case 13: res = (int32_t)s0 > (int32_t)s1 ? s0 : s1; break; /* V_MAX_I32 */
            case 14: res = s0 < s1 ? s0 : s1; break; /* V_MIN_U32 */
            case 15: res = s0 > s1 ? s0 : s1; break; /* V_MAX_U32 */
            case 16: res = s0 >> (s1 & 31); break; /* V_LSHRREV_B32 */
            case 17: res = (uint32_t)((int32_t)s0 >> (s1 & 31)); break; /* V_ASHRREV_I32 */
            case 18: res = s0 << (s1 & 31); break; /* V_LSHLREV_B32: shift s0 by s1 */
            case 19: res = s0 & s1; break; /* V_AND_B32 */
            case 20: res = s0 | s1; break; /* V_OR_B32 */
            case 21: res = s0 ^ s1; break; /* V_XOR_B32 */
            case 22: /* V_MAC_F32: VDST = s0*s1 + VDST */
                res = f2u(u2f(s0) * u2f(s1) + u2f(w->vgpr[lane][vdst]));
                break;
            default: res = 0; break;
            }
            if (vdst < GCN_NUM_VGPR) w->vgpr[lane][vdst] = res;
        }
        if (extra) w->pc++;
        return 0;
    }

    /* VOPC: [31:25]=0111110 = 0x3E */
    if ((insn >> 25) == 0x3Eu) {
        uint32_t op    = (insn >> 17) & 0xffu;
        uint32_t vsrc1 = (insn >>  9) & 0xffu;
        uint32_t src0  =  insn        & 0x1ffu;
        int extra = (src0 == 255);
        uint64_t mask = 0;
        for (uint32_t lane = 0; lane < GCN_WAVE_LANES; lane++) {
            if (!exec_lane(w, lane)) continue;
            uint32_t s0 = gcn_src_read(w, src0, lane, extra ? &code[w->pc] : NULL);
            uint32_t s1 = w->vgpr[lane][vsrc1];
            int cond = 0;
            switch (op) {
            case 0x41: cond = u2f(s0) <  u2f(s1); break; /* V_CMP_LT_F32 */
            case 0x42: cond = u2f(s0) == u2f(s1); break; /* V_CMP_EQ_F32 */
            case 0x43: cond = u2f(s0) <= u2f(s1); break; /* V_CMP_LE_F32 */
            case 0x44: cond = u2f(s0) >  u2f(s1); break; /* V_CMP_GT_F32 */
            case 0x46: cond = u2f(s0) >= u2f(s1); break; /* V_CMP_GE_F32 */
            case 0xC1: cond = (int32_t)s0 < (int32_t)s1; break; /* V_CMP_LT_I32 */
            case 0xC2: cond = s0 == s1; break;
            case 0xC4: cond = (int32_t)s0 > (int32_t)s1; break;
            case 0xC5: cond = s0 != s1; break;
            case 0xC6: cond = (int32_t)s0 >= (int32_t)s1; break;
            case 0xC9: cond = s0 < s1; break; /* V_CMP_LT_U32 */
            case 0xCA: cond = s0 == s1; break;
            case 0xCC: cond = s0 > s1; break;
            case 0xCE: cond = s0 >= s1; break;
            default: break;
            }
            if (cond) mask |= (1ull << lane);
        }
        w->vcc_lo = (uint32_t)(mask);
        w->vcc_hi = (uint32_t)(mask >> 32);
        if (extra) w->pc++;
        return 0;
    }

    /* SMEM: [31:26]=110000 = 0x30 -- loads into scalar regs from memory */
    if ((insn >> 26) == 0x30u) {
        uint32_t op    = (insn >> 18) & 0x3fu;
        uint32_t sdst  = (insn >>  6) & 0x7fu;
        uint32_t sbase = (insn)        & 0x3fu;  /* SGPR pair / 2 */
        uint32_t offset = code[w->pc]; /* IMM=1 assumed */
        w->pc++;
        uint64_t base = ((uint64_t)w->sgpr[sbase * 2 + 1] << 32) | w->sgpr[sbase * 2];
        uint64_t addr = base + offset;
        uint32_t nwords = (op == 0) ? 1 : (op == 1) ? 2 : (op == 2) ? 4 : 1;
        for (uint32_t i = 0; i < nwords; i++) {
            uint32_t val = 0;
            if (mem_load32(w, addr + i * 4, &val) == 0) {
                if (sdst + i < GCN_NUM_SGPR)
                    w->sgpr[sdst + i] = val;
            }
        }
        return 0;
    }

    /* DS: [31:26]=110110 = 0x36 */
    if ((insn >> 26) == 0x36u) {
        uint32_t op     = (insn >> 17) & 0xffu;
        /* Parse second word */
        uint32_t w1     = code[w->pc++];
        uint32_t vdst   = (w1 >> 24) & 0xffu;
        uint32_t data0  = (w1 >> 8)  & 0xffu;  /* vgpr index for data (writes) */
        uint32_t addr_v = (w1)        & 0xffu;  /* vgpr index for base address */
        uint8_t  offset0 = (insn)    & 0xffu;
        for (uint32_t lane = 0; lane < GCN_WAVE_LANES; lane++) {
            if (!exec_lane(w, lane)) continue;
            uint32_t base = w->vgpr[lane][addr_v] + offset0;
            switch (op) {
            case 0x36: /* DS_READ_B32 */
                if (vdst < GCN_NUM_VGPR)
                    w->vgpr[lane][vdst] = lds_load32(w, base);
                break;
            case 0x0D: /* DS_WRITE_B32 */
                lds_store32(w, base, w->vgpr[lane][data0]);
                break;
            default:
                break;
            }
        }
        return 0;
    }

    /* MUBUF: [31:26]=111000 = 0x38 */
    if ((insn >> 26) == 0x38u) {
        uint32_t op    = (insn >> 18) & 0xffu;
        int offen  = (insn >> 12) & 1;
        uint32_t offset = insn & 0xfffu;
        uint32_t w1    = code[w->pc++];
        uint32_t soffset  = (w1 >> 24) & 0xffu;
        uint32_t srsrc_d4 = (w1 >> 16) & 0xffu;
        uint32_t vdata    = (w1 >>  8) & 0xffu;
        uint32_t vaddr_r  =  w1        & 0xffu;
        /* Buffer descriptor in SGPRs[srsrc_d4*4 .. srsrc_d4*4+3] */
        uint64_t buf_base = ((uint64_t)w->sgpr[srsrc_d4*4+1] << 32) | w->sgpr[srsrc_d4*4];
        /* Soffset: inline const or sgpr */
        uint32_t soff_val = gcn_src_read(w, soffset, 0, NULL);
        for (uint32_t lane = 0; lane < GCN_WAVE_LANES; lane++) {
            if (!exec_lane(w, lane)) continue;
            uint32_t voff = offen ? w->vgpr[lane][vaddr_r] : 0;
            uint64_t addr = buf_base + soff_val + offset + voff;
            switch (op) {
            case 0x14: /* BUFFER_LOAD_DWORD */ {
                uint32_t val = 0;
                if (mem_load32(w, addr, &val) == 0 && vdata < GCN_NUM_VGPR)
                    w->vgpr[lane][vdata] = val;
                break; }
            case 0x1C: /* BUFFER_STORE_DWORD */
                if (vdata < GCN_NUM_VGPR)
                    mem_store32(w, addr, w->vgpr[lane][vdata]);
                break;
            default: break;
            }
        }
        return 0;
    }

    /* FLAT: [31:26]=110111 = 0x37 */
    if ((insn >> 26) == 0x37u) {
        uint32_t op    = (insn >> 18) & 0x3fu;
        uint32_t w1    = code[w->pc++];
        uint32_t vdst  = (w1 >> 24) & 0xffu;
        uint32_t vdata = (w1 >>  8) & 0xffu;
        uint32_t vaddr = (w1)        & 0xffu;
        for (uint32_t lane = 0; lane < GCN_WAVE_LANES; lane++) {
            if (!exec_lane(w, lane)) continue;
            /* 64-bit address from VGPR pair */
            uint64_t addr = (uint64_t)w->vgpr[lane][vaddr]
                          | ((uint64_t)w->vgpr[lane][vaddr+1] << 32);
            switch (op) {
            case 0x14: { /* FLAT_LOAD_DWORD */
                uint32_t val = 0;
                if (mem_load32(w, addr, &val) == 0 && vdst < GCN_NUM_VGPR)
                    w->vgpr[lane][vdst] = val;
                break; }
            case 0x1C: /* FLAT_STORE_DWORD */
                if (vdata < GCN_NUM_VGPR)
                    mem_store32(w, addr, w->vgpr[lane][vdata]);
                break;
            default: break;
            }
        }
        return 0;
    }

    /* Unknown or unimplemented -- skip silently */
    return 0;
}

/* ---- run: execute until S_ENDPGM or step limit ---- */
uint64_t gcn_wave_run(gcn_wave_t *w, const uint32_t *code, size_t code_dwords)
{
    while (1) {
        if (w->step_limit && w->insn_count >= w->step_limit)
            break;
        int r = gcn_wave_step(w, code, code_dwords);
        if (r != 0)
            break; /* S_ENDPGM or error */
    }
    return w->insn_count;
}
