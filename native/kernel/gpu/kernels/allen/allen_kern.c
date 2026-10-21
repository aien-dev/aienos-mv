/* native/kernel/gpu/kernels/allen/allen_kern.c -- see allen_kern.h. No floats. */
#include "allen_kern.h"
#include "kmath.h"

#define P(i)   (AK_S_P0 + (i))
#define TG     AK_S_TGID
#define T      40u          /* first kmath temporary VGPR (routines use up to T + 12) */
#define ONE    0x3F800000u
#define NEGINF 0xFF800000u
#define EPS_L2 0x358637BDu  /* 1e-6 */

static void lane0_begin(ka_t *k)
{
    ka_vcmp(k, GCN_VOPC_V_CMP_EQ_U32, 106, KK(0), KV(0));
    ka_sop1(k, GCN_SOP1_S_AND_SAVEEXEC_B64, 22, KVCC);
}

/* lanes with sb > v active (sb an SGPR, v a VGPR); old EXEC saved in s[save:save+1] */
static void mask_lt(ka_t *k, uint32_t v, uint32_t sb, uint32_t save)
{
    ka_vcmp(k, GCN_VOPC_V_CMP_GT_U32, 106, KS(sb), KV(v));
    ka_sop1(k, GCN_SOP1_S_AND_SAVEEXEC_B64, save, KVCC);
}

static void exec_restore(ka_t *k, uint32_t save) { ka_sop1(k, GCN_SOP1_S_MOV_B64, 126, KS(save)); }

/* v = v[lane] combined over the 64 lanes into lane 0 (pairwise, step 32, 16, ..., 1);
 * addr = AK_LDS_TREE + 4 lane. With bcast, every lane gets lane 0's result. */
static void tree(ka_t *k, uint32_t v, uint32_t tmp, uint32_t addr, int max, uint32_t bcast)
{
    ka_ds(k, GCN_DS_DS_WRITE_B32, 0, addr, v, 0, 0, 0);
    for (uint32_t off = 32; off; off >>= 1) {
        ka_wait(k, -1, -1, 0);
        ka_ds(k, GCN_DS_DS_READ_B32, tmp, addr, 0, 0, (uint8_t)(off * 4), 0);
        ka_wait(k, -1, -1, 0);
        ka_vop2(k, max ? GCN_VOP2_V_MAX_F32 : GCN_VOP2_V_ADD_F32, v, KV(v), KV(tmp));
        ka_ds(k, GCN_DS_DS_WRITE_B32, 0, addr, v, 0, 0, 0);
    }
    ka_wait(k, -1, -1, 0);
    if (bcast) {
        ka_ds(k, GCN_DS_DS_READ_B32, v, bcast, 0, 0, 0, 0);
        ka_wait(k, -1, -1, 0);
    }
}

static void tree_addr(ka_t *k, uint32_t addr, uint32_t bcast)
{
    ka_vop2(k, GCN_VOP2_V_LSHLREV_B32, addr, KK(2), KV(0));
    ka_vop2(k, GCN_VOP2_V_ADD_U32, addr, KK(AK_LDS_TREE), KV(addr));
    if (bcast) ka_vop1(k, GCN_VOP1_V_MOV_B32, bcast, KK(AK_LDS_TREE));
}

static void loop_head(ka_t *k, int top, int end, uint32_t i, uint32_t n)
{
    ka_bind(k, top);
    ka_sopc(k, GCN_SOPC_S_CMP_GE_U32, KS(i), KS(n));
    ka_branch(k, GCN_SOPP_S_CBRANCH_SCC1, end);
}

static void ld(ka_t *k, GcnMubufOp op, uint32_t vd, uint32_t va, uint32_t sbase, uint32_t imm)
{
    ka_buf(k, op, vd, va, 0, KS(sbase), imm, 1, 0);
}
#define LD1(vd, va, s, imm)  ld(k, GCN_MUBUF_BUFFER_LOAD_DWORD, vd, va, s, imm)
#define LD4(vd, va, s, imm)  ld(k, GCN_MUBUF_BUFFER_LOAD_DWORDX4, vd, va, s, imm)
#define ST1(vd, va, s, imm)  ld(k, GCN_MUBUF_BUFFER_STORE_DWORD, vd, va, s, imm)
#define ST4(vd, va, s, imm)  ld(k, GCN_MUBUF_BUFFER_STORE_DWORDX4, vd, va, s, imm)
#define VMUL(d, a, b)  ka_vop2(k, GCN_VOP2_V_MUL_F32, d, a, b)
#define VADD(d, a, b)  ka_vop2(k, GCN_VOP2_V_ADD_F32, d, a, b)
#define VSUB(d, a, b)  ka_vop2(k, GCN_VOP2_V_SUB_F32, d, a, b)
#define VMOV(d, a)     ka_vop1(k, GCN_VOP1_V_MOV_B32, d, a)
#define UADD(d, a, b)  ka_vop2(k, GCN_VOP2_V_ADD_U32, d, a, b)
#define SHL(d, n, b)   ka_vop2(k, GCN_VOP2_V_LSHLREV_B32, d, KK(n), b)
#define SADD(d, a, b)  ka_sop2(k, GCN_SOP2_S_ADD_U32, d, a, b)
#define SMUL(d, a, b)  ka_sop2(k, GCN_SOP2_S_MUL_I32, d, a, b)
#define SSHL(d, a, n)  ka_sop2(k, GCN_SOP2_S_LSHL_B32, d, a, KK(n))

/* ---- AK_MATVEC / AK_MATVEC_ACC ----
 * lane j holds x[512 c + 8 j + e] (e < 8) in v[8 + 8 c + e]; its partial sum runs c outer,
 * e inner as fma(w, x, acc); the 64 partials are combined by tree(). */
static void build_matvec(ka_t *k, uint32_t C, int acc)
{
    if (C < 1 || C > 7) { k->err = "matvec: k / 512 must be 1..7"; return; }
    SHL(6, 5, KV(0));
    UADD(6, KS(P(1)), KV(6));
    for (uint32_t c = 0; c < C; c++) {
        uint32_t a = 6;
        if (c) { UADD(7, KK(c * 2048), KV(6)); a = 7; }
        ka_buf(k, GCN_MUBUF_BUFFER_LOAD_DWORDX4, 8 + 8 * c, a, 0, KK(0), 0, 1, 0);
        ka_buf(k, GCN_MUBUF_BUFFER_LOAD_DWORDX4, 12 + 8 * c, a, 0, KK(0), 16, 1, 0);
    }
    SHL(64, 4, KV(0));
    for (uint32_t c = 1; c < C; c++) UADD(64 + c, KK(c * 1024), KV(64));
    tree_addr(k, 4, 0);
    SMUL(20, KS(TG), KS(P(4)));
    SADD(21, KS(20), KS(P(4)));
    ka_sop2(k, GCN_SOP2_S_MIN_U32, 21, KS(21), KS(P(5)));
    SSHL(25, KS(P(3)), 1);
    SMUL(24, KS(20), KS(25));
    SADD(24, KS(24), KS(P(0)));
    SSHL(26, KS(20), 2);
    SADD(26, KS(26), KS(P(2)));
    ka_smov(k, 27, KK(0xFFFF0000u));
    ka_wait(k, 0, -1, -1);
    int top = ka_label(k), end = ka_label(k);
    loop_head(k, top, end, 20, 21);
    for (uint32_t c = 0; c < C; c++) LD4(72 + 4 * c, 64 + c, 24, 0);
    ka_wait(k, 0, -1, -1);
    VMOV(1, KK(0));
    for (uint32_t c = 0; c < C; c++)
        for (uint32_t e = 0; e < 8; e++) {
            uint32_t d = 72 + 4 * c + e / 2;
            if (e & 1) ka_vop2(k, GCN_VOP2_V_AND_B32, 2, KS(27), KV(d));
            else SHL(2, 16, KV(d));
            ka_fma(k, 1, KV(2), KV(8 + 8 * c + e), KV(1), 0);
        }
    tree(k, 1, 2, 4, 0, 0);
    lane0_begin(k);
    if (acc) {
        ka_buf(k, GCN_MUBUF_BUFFER_LOAD_DWORD, 100, 0, 0, KS(26), 0, 0, 0);
        ka_wait(k, 0, -1, -1);
        VADD(1, KV(100), KV(1));
    }
    ka_buf(k, GCN_MUBUF_BUFFER_STORE_DWORD, 1, 0, 0, KS(26), 0, 0, 0);
    exec_restore(k, 22);
    ka_wait(k, 0, 0, -1);
    SADD(20, KS(20), KK(1));
    SADD(24, KS(24), KS(25));
    SADD(26, KS(26), KK(4));
    ka_branch(k, GCN_SOPP_S_BRANCH, top);
    ka_bind(k, end);
    ka_end(k);
}

/* ---- AK_NORM_* : one row per group; lane j takes elements j, j + 64, ... ---- */
static void build_norm(ka_t *k, int mode)
{
    SMUL(20, KS(TG), KS(P(1))); SADD(20, KS(20), KS(P(0)));
    SMUL(21, KS(TG), KS(P(3))); SADD(21, KS(21), KS(P(2)));
    SMUL(28, KS(TG), KS(P(7))); SADD(28, KS(28), KS(P(6)));
    SSHL(24, KS(P(4)), 2);
    SHL(1, 2, KV(0));
    VMOV(10, KK(0));
    ka_smov(k, 23, KK(0));
    int l1 = ka_label(k), e1 = ka_label(k);
    loop_head(k, l1, e1, 23, 24);
    UADD(3, KS(23), KV(1));
    LD1(2, 3, 20, 0);
    ka_wait(k, 0, -1, -1);
    ka_fma(k, 10, KV(2), KV(2), KV(10), 0);
    SADD(23, KS(23), KK(256));
    ka_branch(k, GCN_SOPP_S_BRANCH, l1);
    ka_bind(k, e1);
    tree_addr(k, 4, 5);
    tree(k, 10, 2, 4, 0, 5);
    if (mode == AK_NORM_L2) {
        VADD(10, KK(EPS_L2), KV(10));
    } else {
        ka_sop1(k, GCN_SOP1_S_FF1_I32_B32, 25, KS(P(4)));
        ka_sop2(k, GCN_SOP2_S_SUB_U32, 25, KK(127), KS(25));
        SSHL(25, KS(25), 23);
        VMUL(10, KS(25), KV(10));          /* sum / n, n a power of two: exact */
        VADD(10, KS(P(8)), KV(10));
    }
    km_sqrt(k, 11, KV(10), T);
    km_div(k, 12, KK(ONE), KV(11), T);
    if (mode == AK_NORM_L2) {
        int skip = ka_label(k);
        VMOV(13, KK(ONE));
        ka_sopc(k, GCN_SOPC_S_CMP_EQ_U32, KS(P(9)), KK(0));
        ka_branch(k, GCN_SOPP_S_CBRANCH_SCC1, skip);
        ka_vop1(k, GCN_VOP1_V_CVT_F32_U32, 14, KS(P(9)));
        km_sqrt(k, 15, KV(14), T);
        km_div(k, 13, KK(ONE), KV(15), T);
        ka_bind(k, skip);
    }
    ka_smov(k, 23, KK(0));
    int l2 = ka_label(k), e2 = ka_label(k);
    loop_head(k, l2, e2, 23, 24);
    UADD(3, KS(23), KV(1));
    LD1(2, 3, 20, 0);
    if (mode != AK_NORM_L2) LD1(6, 3, P(5), 0);
    if (mode == AK_NORM_GATED) LD1(7, 3, 28, 0);
    ka_wait(k, 0, -1, -1);
    VMUL(2, KV(2), KV(12));
    if (mode == AK_NORM_ZC) {
        VADD(6, KK(ONE), KV(6));
        VMUL(2, KV(2), KV(6));
    } else if (mode == AK_NORM_GATED) {
        VMUL(2, KV(2), KV(6));
        km_silu(k, 8, KV(7), T);
        VMUL(2, KV(2), KV(8));
    } else {
        VMUL(2, KV(2), KV(13));
    }
    ST1(2, 3, 21, 0);
    ka_wait(k, 0, 0, -1);
    SADD(23, KS(23), KK(256));
    ka_branch(k, GCN_SOPP_S_BRANCH, l2);
    ka_bind(k, e2);
    ka_end(k);
}

static void build_embed(ka_t *k)
{
    SMUL(20, KS(P(1)), KS(P(3))); SSHL(20, KS(20), 1); SADD(20, KS(20), KS(P(0)));
    SSHL(25, KS(P(3)), 1);
    SHL(1, 1, KV(0));
    SHL(4, 2, KV(0));
    ka_smov(k, 23, KK(0)); ka_smov(k, 24, KK(0));
    int l = ka_label(k), e = ka_label(k);
    loop_head(k, l, e, 23, 25);
    UADD(3, KS(23), KV(1));
    ld(k, GCN_MUBUF_BUFFER_LOAD_USHORT, 2, 3, 20, 0);
    UADD(5, KS(24), KV(4));
    ka_wait(k, 0, -1, -1);
    SHL(2, 16, KV(2));
    ST1(2, 5, P(2), 0);
    ka_wait(k, 0, 0, -1);
    SADD(23, KS(23), KK(128)); SADD(24, KS(24), KK(256));
    ka_branch(k, GCN_SOPP_S_BRANCH, l);
    ka_bind(k, e);
    ka_end(k);
}

static void build_conv(ka_t *k)
{
    SSHL(20, KS(TG), 6);
    UADD(1, KS(20), KV(0));                /* channel */
    SHL(2, 2, KV(1));
    SHL(3, 4, KV(1));
    SSHL(21, KS(P(4)), 2);
    SADD(22, KS(P(1)), KS(21));
    SADD(23, KS(22), KS(21));
    LD4(8, 3, P(2), 0);
    LD1(12, 2, P(1), 0);
    LD1(13, 2, 22, 0);
    LD1(14, 2, 23, 0);
    LD1(15, 2, P(0), 0);
    ka_wait(k, 0, -1, -1);
    VMOV(16, KK(0));
    for (uint32_t j = 0; j < 4; j++) {
        VMUL(17, KV(8 + j), KV(12 + j));
        VADD(16, KV(16), KV(17));
    }
    km_silu(k, 18, KV(16), T);
    ST1(18, 2, P(3), 0);
    ST1(13, 2, P(1), 0);
    ST1(14, 2, 22, 0);
    ST1(15, 2, 23, 0);
    ka_wait(k, 0, 0, -1);
    ka_end(k);
}

static void build_gdn(ka_t *k)
{
    ka_sop2(k, GCN_SOP2_S_LSHR_B32, 20, KS(TG), KS(P(9)));     /* k head */
    SSHL(21, KS(TG), 2);
    SADD(22, KS(P(1)), KS(21)); ka_smem_s(k, GCN_SMEM_S_BUFFER_LOAD_DWORD, 24, 0, 22);
    SADD(23, KS(P(2)), KS(21)); ka_smem_s(k, GCN_SMEM_S_BUFFER_LOAD_DWORD, 25, 0, 23);
    SADD(28, KS(P(3)), KS(21)); ka_smem_s(k, GCN_SMEM_S_BUFFER_LOAD_DWORD, 26, 0, 28);
    SADD(29, KS(P(4)), KS(21)); ka_smem_s(k, GCN_SMEM_S_BUFFER_LOAD_DWORD, 27, 0, 29);
    ka_wait(k, -1, -1, 0);
    VMOV(21, KS(27));
    VADD(20, KS(24), KV(21));                                    /* a + dt_bias */
    km_softplus(k, 22, KV(20), T);
    VMOV(24, KS(26));
    km_exp(k, 23, KV(24), T);
    VMUL(25, KV(23), KV(22));
    ka_vop2(k, GCN_VOP2_V_XOR_B32, 25, KK(0x80000000u), KV(25)); /* g = -exp(a_log) sp */
    km_exp(k, 10, KV(25), T);                                    /* decay */
    VMOV(26, KS(25));
    km_sigmoid(k, 11, KV(26), T);                                /* beta */
    SMUL(30, KS(TG), KS(P(7))); SSHL(30, KS(30), 9); SADD(30, KS(30), KS(P(5)));
    SMUL(31, KS(20), KS(P(7))); SSHL(31, KS(31), 2); SADD(31, KS(31), KS(P(0)));
    SMUL(33, KS(P(8)), KS(P(7))); SSHL(33, KS(33), 2);
    SADD(32, KS(31), KS(33));
    SSHL(34, KS(TG), 9); SADD(34, KS(34), KS(P(0))); SADD(34, KS(34), KS(33)); SADD(34, KS(34), KS(33));
    SHL(1, 2, KV(0));
    VMOV(12, KK(0)); VMOV(13, KK(0));
    for (int pass = 0; pass < 2; pass++) {
        if (pass) {
            LD1(16, 1, 34, 0); LD1(17, 1, 34, 256);
            ka_wait(k, 0, -1, -1);
            for (uint32_t h = 0; h < 2; h++) {
                VSUB(16 + h, KV(16 + h), KV(12 + h));
                VMUL(16 + h, KV(16 + h), KV(11));
            }
            VMOV(18, KK(0)); VMOV(19, KK(0));
        }
        ka_smov(k, 35, KK(0)); ka_smov(k, 36, KS(30)); ka_smov(k, 37, KS(32)); ka_smov(k, 38, KS(31));
        int l = ka_label(k), e = ka_label(k);
        loop_head(k, l, e, 35, P(7));
        ka_smem_s(k, GCN_SMEM_S_BUFFER_LOAD_DWORDX8, 40, 0, 37);
        if (pass) ka_smem_s(k, GCN_SMEM_S_BUFFER_LOAD_DWORDX8, 48, 0, 38);
        for (uint32_t i = 0; i < 8; i++) { LD1(50 + 2 * i, 1, 36, i * 512); LD1(51 + 2 * i, 1, 36, i * 512 + 256); }
        ka_wait(k, 0, -1, 0);
        for (uint32_t i = 0; i < 8; i++)
            for (uint32_t h = 0; h < 2; h++) {
                uint32_t s = 50 + 2 * i + h;
                if (!pass) {
                    VMUL(s, KV(s), KV(10));
                    VMUL(14, KS(40 + i), KV(s));
                    VADD(12 + h, KV(12 + h), KV(14));
                } else {
                    VMUL(14, KS(40 + i), KV(16 + h));
                    VADD(s, KV(s), KV(14));
                    VMUL(14, KS(48 + i), KV(s));
                    VADD(18 + h, KV(18 + h), KV(14));
                }
            }
        for (uint32_t i = 0; i < 8; i++) { ST1(50 + 2 * i, 1, 36, i * 512); ST1(51 + 2 * i, 1, 36, i * 512 + 256); }
        ka_wait(k, 0, 0, -1);
        SADD(35, KS(35), KK(8)); SADD(36, KS(36), KK(4096)); SADD(37, KS(37), KK(32)); SADD(38, KS(38), KK(32));
        ka_branch(k, GCN_SOPP_S_BRANCH, l);
        ka_bind(k, e);
    }
    SSHL(39, KS(TG), 9); SADD(39, KS(39), KS(P(6)));
    ST1(18, 1, 39, 0); ST1(19, 1, 39, 256);
    ka_wait(k, 0, 0, -1);
    ka_end(k);
}

static void build_rope(ka_t *k)
{
    mask_lt(k, 0, P(5), 22);
    SMUL(20, KS(TG), KS(P(1))); SADD(20, KS(20), KS(P(0)));
    SSHL(21, KS(P(5)), 2);
    SADD(23, KS(20), KS(21));
    SMUL(24, KS(P(4)), KS(P(5))); SSHL(24, KS(24), 2);
    SADD(25, KS(24), KS(P(3)));
    SADD(24, KS(24), KS(P(2)));
    SHL(1, 2, KV(0));
    LD1(2, 1, 20, 0); LD1(3, 1, 23, 0); LD1(4, 1, 24, 0); LD1(5, 1, 25, 0);
    ka_wait(k, 0, -1, -1);
    VMUL(6, KV(2), KV(4)); VMUL(7, KV(3), KV(5)); VSUB(8, KV(6), KV(7));
    VMUL(9, KV(3), KV(4)); VMUL(10, KV(2), KV(5)); VADD(11, KV(9), KV(10));
    ST1(8, 1, 20, 0); ST1(11, 1, 23, 0);
    ka_wait(k, 0, 0, -1);
    exec_restore(k, 22);
    ka_end(k);
}

static void build_attn(ka_t *k)
{
    ka_sop2(k, GCN_SOP2_S_LSHR_B32, 20, KS(TG), KS(P(7)));
    SSHL(21, KS(20), 10);
    SADD(22, KS(P(1)), KS(21));
    SADD(23, KS(P(2)), KS(21));
    SSHL(24, KS(TG), 10); SADD(24, KS(24), KS(P(0)));
    tree_addr(k, 31, 32);
    VMOV(30, KK(NEGINF));
    VMOV(33, KK(0));
    for (int pass = 0; pass < 3; pass++) {
        ka_smov(k, 25, KK(0));
        int l = ka_label(k), e = ka_label(k);
        loop_head(k, l, e, 25, P(3));
        UADD(2, KS(25), KV(0));
        SHL(7, 2, KV(2));
        mask_lt(k, 2, P(3), 26);
        if (pass == 0) {
            ka_vop3(k, K3_MUL_LO_U32, 3, KV(2), KS(P(8)), KK(0), 0, 0);
            VMOV(4, KK(0));
            ka_smov(k, 28, KK(0)); ka_smov(k, 29, KS(24)); ka_smov(k, 19, KK(1024));
            int ld_ = ka_label(k), ed = ka_label(k);
            loop_head(k, ld_, ed, 28, 19);
            ka_smem_s(k, GCN_SMEM_S_BUFFER_LOAD_DWORDX16, 40, 0, 29);
            UADD(5, KS(28), KV(3));
            for (uint32_t i = 0; i < 4; i++) LD4(8 + 4 * i, 5, 22, 16 * i);
            ka_wait(k, 0, -1, 0);
            for (uint32_t i = 0; i < 16; i++) {
                VMUL(6, KS(40 + i), KV(8 + i));
                VADD(4, KV(4), KV(6));
            }
            SADD(28, KS(28), KK(64)); SADD(29, KS(29), KK(64));
            ka_branch(k, GCN_SOPP_S_BRANCH, ld_);
            ka_bind(k, ed);
            VMUL(4, KS(P(9)), KV(4));
            ka_ds(k, GCN_DS_DS_WRITE_B32, 0, 7, 4, 0, 0, 0);
            ka_vop2(k, GCN_VOP2_V_MAX_F32, 30, KV(30), KV(4));
        } else {
            ka_ds(k, GCN_DS_DS_READ_B32, 4, 7, 0, 0, 0, 0);
            ka_wait(k, -1, -1, 0);
            if (pass == 1) {
                VSUB(4, KV(4), KV(30));
                km_exp(k, 5, KV(4), T);
                VADD(33, KV(33), KV(5));
            } else {
                km_div(k, 5, KV(4), KV(33), T);
            }
            ka_ds(k, GCN_DS_DS_WRITE_B32, 0, 7, 5, 0, 0, 0);
        }
        ka_wait(k, -1, -1, 0);
        exec_restore(k, 26);
        SADD(25, KS(25), KK(64));
        ka_branch(k, GCN_SOPP_S_BRANCH, l);
        ka_bind(k, e);
        if (pass == 0) tree(k, 30, 6, 31, 1, 32);
        if (pass == 1) tree(k, 33, 6, 31, 0, 32);
    }
    SHL(38, 4, KV(0));
    for (uint32_t i = 0; i < 4; i++) VMOV(34 + i, KK(0));
    ka_smov(k, 25, KK(0)); ka_smov(k, 28, KS(23)); ka_smov(k, 29, KK(0));
    int l = ka_label(k), e = ka_label(k);
    loop_head(k, l, e, 25, P(3));
    VMOV(39, KS(29));
    ka_ds(k, GCN_DS_DS_READ_B32, 4, 39, 0, 0, 0, 0);
    LD4(8, 38, 28, 0);
    ka_wait(k, 0, -1, 0);
    for (uint32_t i = 0; i < 4; i++) {
        VMUL(6, KV(4), KV(8 + i));
        VADD(34 + i, KV(34 + i), KV(6));
    }
    SADD(25, KS(25), KK(1)); SADD(28, KS(28), KS(P(8))); SADD(29, KS(29), KK(4));
    ka_branch(k, GCN_SOPP_S_BRANCH, l);
    ka_bind(k, e);
    SMUL(30, KS(TG), KS(P(5))); SADD(30, KS(30), KS(P(4)));
    LD4(8, 38, 30, 0);
    ka_wait(k, 0, -1, -1);
    for (uint32_t i = 0; i < 4; i++) {
        km_sigmoid(k, 12 + i, KV(8 + i), T);
        VMUL(34 + i, KV(34 + i), KV(12 + i));
    }
    SSHL(31, KS(TG), 10); SADD(31, KS(31), KS(P(6)));
    ST4(34, 38, 31, 0);
    ka_wait(k, 0, 0, -1);
    ka_end(k);
}

static void build_swiglu(ka_t *k)
{
    SSHL(20, KS(TG), 8);
    SHL(1, 2, KV(0));
    UADD(1, KS(20), KV(1));
    LD1(2, 1, P(0), 0); LD1(3, 1, P(1), 0);
    ka_wait(k, 0, -1, -1);
    km_silu(k, 4, KV(2), T);
    VMUL(5, KV(4), KV(3));
    ST1(5, 1, P(2), 0);
    ka_wait(k, 0, 0, -1);
    ka_end(k);
}

static void build_copy(ka_t *k)
{
    SSHL(20, KS(TG), 8);
    SHL(1, 2, KV(0));
    UADD(1, KS(20), KV(1));
    LD1(2, 1, P(0), 0);
    ka_wait(k, 0, -1, -1);
    ST1(2, 1, P(1), 0);
    ka_wait(k, 0, 0, -1);
    ka_end(k);
}

static void build_argmax(ka_t *k)
{
    VMOV(10, KK(NEGINF));
    VMOV(11, KK(0xFFFFFFFFu));
    ka_smov(k, 25, KK(0));
    int l = ka_label(k), e = ka_label(k);
    loop_head(k, l, e, 25, P(1));
    UADD(2, KS(25), KV(0));
    mask_lt(k, 2, P(1), 26);
    SHL(4, 2, KV(2));
    LD1(3, 4, P(0), 0);
    ka_wait(k, 0, -1, -1);
    ka_vcmp(k, GCN_VOPC_V_CMP_GT_F32, 106, KV(3), KV(10));
    ka_cnd(k, 10, KV(10), KV(3), 106);
    ka_cnd(k, 11, KV(11), KV(2), 106);
    exec_restore(k, 26);
    SADD(25, KS(25), KK(64));
    ka_branch(k, GCN_SOPP_S_BRANCH, l);
    ka_bind(k, e);
    tree_addr(k, 5, 0);
    UADD(6, KK(512), KV(5));
    ka_ds(k, GCN_DS_DS_WRITE_B32, 0, 5, 10, 0, 0, 0);
    ka_ds(k, GCN_DS_DS_WRITE_B32, 0, 6, 11, 0, 0, 0);
    for (uint32_t off = 32; off; off >>= 1) {
        ka_wait(k, -1, -1, 0);
        ka_ds(k, GCN_DS_DS_READ_B32, 12, 5, 0, 0, (uint8_t)(off * 4), 0);
        ka_ds(k, GCN_DS_DS_READ_B32, 13, 6, 0, 0, (uint8_t)(off * 4), 0);
        ka_wait(k, -1, -1, 0);
        ka_vcmp(k, GCN_VOPC_V_CMP_GT_F32, 60, KV(12), KV(10));
        ka_vcmp(k, GCN_VOPC_V_CMP_EQ_F32, 62, KV(12), KV(10));
        ka_vcmp(k, GCN_VOPC_V_CMP_LT_U32, 64, KV(13), KV(11));
        ka_sop2(k, GCN_SOP2_S_AND_B64, 62, KS(62), KS(64));
        ka_sop2(k, GCN_SOP2_S_OR_B64, 60, KS(60), KS(62));
        ka_cnd(k, 10, KV(10), KV(12), 60);
        ka_cnd(k, 11, KV(11), KV(13), 60);
        ka_ds(k, GCN_DS_DS_WRITE_B32, 0, 5, 10, 0, 0, 0);
        ka_ds(k, GCN_DS_DS_WRITE_B32, 0, 6, 11, 0, 0, 0);
    }
    ka_wait(k, -1, -1, 0);
    lane0_begin(k);
    ka_buf(k, GCN_MUBUF_BUFFER_STORE_DWORD, 11, 0, 0, KS(P(2)), 0, 0, 0);
    exec_restore(k, 22);
    ka_wait(k, 0, 0, -1);
    ka_end(k);
}

int ak_build(ka_t *k, int id, uint32_t c)
{
    ka_init(k);
    ka_smov(k, 124, KK(0xFFFFFFFFu)); /* M0 = -1: gfx8 clamps LDS addresses to M0 */
    switch (id) {
    case AK_MATVEC: build_matvec(k, c, 0); break;
    case AK_MATVEC_ACC: build_matvec(k, c, 1); break;
    case AK_NORM_ZC: case AK_NORM_GATED: case AK_NORM_L2: build_norm(k, id); break;
    case AK_EMBED: build_embed(k); break;
    case AK_CONV: build_conv(k); break;
    case AK_GDN: build_gdn(k); break;
    case AK_ROPE: build_rope(k); break;
    case AK_ATTN: build_attn(k); break;
    case AK_SWIGLU: build_swiglu(k); break;
    case AK_COPY: build_copy(k); break;
    case AK_ARGMAX: build_argmax(k); break;
    default: k->err = "ak_build: unknown kernel"; return -1;
    }
    if (k->err) return -1;
    return ka_finish(k);
}

const char *ak_name(int id)
{
    static const char *n[AK_COUNT] = { "matvec", "matvec_acc", "norm_zc", "norm_gated", "norm_l2",
        "embed", "conv", "gdn", "rope", "attn", "swiglu", "copy", "argmax" };
    return (id >= 0 && id < AK_COUNT) ? n[id] : "?";
}
