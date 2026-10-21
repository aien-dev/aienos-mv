/* native/kernel/gpu/kernels/allen/kmath.c -- GPU emitters for kmath.h. No floats.
 * Each routine mirrors fmath_ref.c line by line. */
#include "kmath.h"

#define K0 KA_S_K0
#define M0 KA_S_MASK0
#define M1 KA_S_MASK1

void km_exp(ka_t *k, uint32_t vd, ko x, uint32_t t)
{
    uint32_t xc = t, nf = t + 1, r = t + 2, p = t + 3, z = t + 4;
    ka_vop2(k, GCN_VOP2_V_MAX_F32, xc, KK(KM_EXP_LO), x);         /* xc = max(-104, x) */
    ka_vop2(k, GCN_VOP2_V_MIN_F32, xc, KK(KM_EXP_HI), KV(xc));    /* xc = min(hi, xc) */
    ka_vop2(k, GCN_VOP2_V_MUL_F32, nf, KK(KM_LOG2E), KV(xc));
    ka_vop1(k, GCN_VOP1_V_RNDNE_F32, nf, KV(nf));
    ka_smov(k, K0, KK(KM_LN2_HI));
    ka_fma(k, r, KV(nf), KS(K0), KV(xc), 1);                       /* r = fma(-nf, ln2_hi, xc) */
    ka_smov(k, K0, KK(KM_LN2_LO));
    ka_fma(k, r, KV(nf), KS(K0), KV(r), 1);                        /* r = fma(-nf, ln2_lo, r) */
    ka_vop1(k, GCN_VOP1_V_MOV_B32, p, KK(KM_EXP_P0));
    ka_smov(k, K0, KK(KM_EXP_P1)); ka_fma(k, p, KV(p), KV(r), KS(K0), 0);
    ka_smov(k, K0, KK(KM_EXP_P2)); ka_fma(k, p, KV(p), KV(r), KS(K0), 0);
    ka_smov(k, K0, KK(KM_EXP_P3)); ka_fma(k, p, KV(p), KV(r), KS(K0), 0);
    ka_smov(k, K0, KK(KM_EXP_P4)); ka_fma(k, p, KV(p), KV(r), KS(K0), 0);
    ka_fma(k, p, KV(p), KV(r), KK(KM_EXP_P5), 0);                  /* 0.5 is inline */
    ka_vop2(k, GCN_VOP2_V_MUL_F32, z, KV(r), KV(r));
    ka_fma(k, p, KV(p), KV(z), KV(r), 0);                          /* y = fma(p, z, r) */
    ka_vop2(k, GCN_VOP2_V_ADD_F32, p, KK(KM_ONE), KV(p));          /* y = 1 + y */
    ka_vop1(k, GCN_VOP1_V_CVT_I32_F32, nf, KV(nf));                /* ni */
    ka_vop2(k, GCN_VOP2_V_ASHRREV_I32, z, KK(1), KV(nf));          /* n1 = ni >> 1 */
    ka_vop2(k, GCN_VOP2_V_SUB_U32, nf, KV(nf), KV(z));             /* n2 = ni - n1 */
    ka_vop2(k, GCN_VOP2_V_ADD_U32, z, KK(127), KV(z));
    ka_vop2(k, GCN_VOP2_V_LSHLREV_B32, z, KK(23), KV(z));
    ka_vop2(k, GCN_VOP2_V_ADD_U32, nf, KK(127), KV(nf));
    ka_vop2(k, GCN_VOP2_V_LSHLREV_B32, nf, KK(23), KV(nf));
    ka_vop2(k, GCN_VOP2_V_MUL_F32, p, KV(p), KV(z));
    ka_vop2(k, GCN_VOP2_V_MUL_F32, p, KV(p), KV(nf));
    ka_smov(k, K0, KK(KM_EXP_HI));
    ka_vcmp(k, GCN_VOPC_V_CMP_LT_F32, M0, KS(K0), x);              /* hi < x */
    ka_vop1(k, GCN_VOP1_V_MOV_B32, xc, KK(KM_INF));
    ka_cnd(k, vd, KV(p), KV(xc), M0);
}

void km_div(ka_t *k, uint32_t vd, ko a, ko b, uint32_t t)
{
    uint32_t a1 = t, b1 = t + 1, r = t + 2, e = t + 3, q = t + 4, sc = t + 5;
    ka_smov(k, K0, KK(KM_TWO64));
    ka_vcmp(k, GCN_VOPC_V_CMP_LT_F32, M0, KS(K0), b);              /* 2^64 < b */
    ka_vop1(k, GCN_VOP1_V_MOV_B32, sc, KK(KM_ONE));
    ka_vop1(k, GCN_VOP1_V_MOV_B32, q, KK(KM_TWOM64));
    ka_cnd(k, sc, KV(sc), KV(q), M0);
    ka_vop2(k, GCN_VOP2_V_MUL_F32, a1, a, KV(sc));
    ka_vop2(k, GCN_VOP2_V_MUL_F32, b1, b, KV(sc));
    ka_vop2(k, GCN_VOP2_V_SUB_U32, r, KK(KM_RCP_MAGIC), KV(b1));
    for (int i = 0; i < 4; i++) {
        ka_fma(k, e, KV(b1), KV(r), KK(KM_ONE), 1);                /* e = fma(-b1, r, 1) */
        ka_fma(k, r, KV(e), KV(r), KV(r), 0);                      /* r = fma(e, r, r) */
    }
    ka_vop2(k, GCN_VOP2_V_MUL_F32, e, KV(a1), KV(r));              /* q0 */
    ka_fma(k, q, KV(b1), KV(e), KV(a1), 1);                        /* rem = fma(-b1, q0, a1) */
    ka_fma(k, q, KV(q), KV(r), KV(e), 0);                          /* q1 = fma(rem, r, q0) */
    ka_vcmp(k, GCN_VOPC_V_CMP_EQ_F32, M1, KK(0), KV(e));           /* 0 == q0 */
    ka_cnd(k, q, KV(q), KV(e), M1);
    ka_smov(k, K0, KK(KM_INF));
    ka_vcmp(k, GCN_VOPC_V_CMP_EQ_F32, M0, KS(K0), b);              /* inf == b */
    if ((a >> 32)) ka_vop1(k, GCN_VOP1_V_MOV_B32, e, KK((uint32_t)a & 0x80000000u));
    else ka_vop2(k, GCN_VOP2_V_AND_B32, e, KK(0x80000000u), a);
    ka_cnd(k, vd, KV(q), KV(e), M0);
}

void km_sqrt(ka_t *k, uint32_t vd, ko x, uint32_t t)
{
    uint32_t g = t, y = t + 1, h = t + 2;
    ka_vop2(k, GCN_VOP2_V_LSHRREV_B32, g, KK(1), x);
    ka_vop2(k, GCN_VOP2_V_SUB_U32, g, KK(KM_RSQ_MAGIC), KV(g));
    ka_vop2(k, GCN_VOP2_V_MUL_F32, y, x, KV(g));
    ka_vop2(k, GCN_VOP2_V_MUL_F32, h, KK(0x3F000000u), KV(g));
    for (int i = 0; i < 3; i++) {
        ka_fma(k, g, KV(y), KV(h), KK(0x3F000000u), 1);            /* r = fma(-y, h, 0.5) */
        ka_fma(k, y, KV(y), KV(g), KV(y), 0);                      /* y = fma(y, r, y) */
        ka_fma(k, h, KV(h), KV(g), KV(h), 0);                      /* h = fma(h, r, h) */
    }
    ka_fma(k, g, KV(y), KV(y), x, 1);                              /* d = fma(-y, y, x) */
    ka_fma(k, vd, KV(g), KV(h), KV(y), 0);                         /* y = fma(d, h, y) */
}

void km_log1p(ka_t *k, uint32_t vd, ko y, uint32_t t)
{
    uint32_t u = t, e = t + 1, f = t + 2, z = t + 3, p = t + 4, c = t + 5;
    static const uint32_t P[9] = { KM_LOG_P0, KM_LOG_P1, KM_LOG_P2, KM_LOG_P3, KM_LOG_P4,
                                   KM_LOG_P5, KM_LOG_P6, KM_LOG_P7, KM_LOG_P8 };
    ka_vop2(k, GCN_VOP2_V_ADD_F32, u, KK(KM_ONE), y);              /* u = 1 + y */
    ka_vop2(k, GCN_VOP2_V_LSHRREV_B32, e, KK(23), KV(u));
    ka_vop2(k, GCN_VOP2_V_SUB_U32, e, KV(e), KK(127));             /* e = (bits >> 23) - 127 */
    ka_vop2(k, GCN_VOP2_V_AND_B32, f, KK(0x007FFFFFu), KV(u));
    ka_vop2(k, GCN_VOP2_V_OR_B32, f, KK(KM_ONE), KV(f));           /* m in [1, 2) */
    ka_smov(k, K0, KK(KM_SQRT2));
    ka_vcmp(k, GCN_VOPC_V_CMP_LT_U32, M0, KS(K0), KV(f));          /* sqrt2 < m */
    ka_vop2(k, GCN_VOP2_V_SUB_U32, z, KV(f), KK(0x00800000u));
    ka_cnd(k, f, KV(f), KV(z), M0);                                /* m /= 2 */
    ka_vop2(k, GCN_VOP2_V_ADD_U32, z, KK(1), KV(e));
    ka_cnd(k, e, KV(e), KV(z), M0);                                /* e += 1 */
    ka_vop1(k, GCN_VOP1_V_CVT_F32_I32, e, KV(e));                  /* fe */
    ka_vop2(k, GCN_VOP2_V_ADD_F32, f, KK(0xBF800000u), KV(f));     /* f = m - 1 */
    ka_vop2(k, GCN_VOP2_V_MUL_F32, z, KV(f), KV(f));               /* z = f * f */
    ka_vop1(k, GCN_VOP1_V_MOV_B32, p, KK(P[0]));
    for (int i = 1; i < 9; i++) { ka_smov(k, K0, KK(P[i])); ka_fma(k, p, KV(p), KV(f), KS(K0), 0); }
    ka_vop2(k, GCN_VOP2_V_MUL_F32, p, KV(p), KV(f));
    ka_vop2(k, GCN_VOP2_V_MUL_F32, p, KV(p), KV(z));
    ka_smov(k, K0, KK(KM_LOG_Q1));
    ka_fma(k, p, KV(e), KS(K0), KV(p), 0);                         /* t = fma(fe, q1, t) */
    ka_fma(k, p, KV(z), KK(0xBF000000u), KV(p), 0);                /* t = fma(z, -0.5, t) */
    ka_vop2(k, GCN_VOP2_V_ADD_F32, c, KK(0xBF800000u), KV(u));     /* u - 1 */
    ka_vop2(k, GCN_VOP2_V_SUB_F32, c, y, KV(c));                   /* y - (u - 1) */
    km_div(k, c, KV(c), KV(u), t + 6);                             /* c = (y - (u-1)) / u */
    ka_vop2(k, GCN_VOP2_V_ADD_F32, p, KV(p), KV(c));               /* t = t + c */
    ka_vop2(k, GCN_VOP2_V_ADD_F32, p, KV(f), KV(p));               /* r = f + t */
    ka_smov(k, K0, KK(KM_LOG_Q2));
    ka_fma(k, p, KV(e), KS(K0), KV(p), 0);                         /* r = fma(fe, q2, r) */
    ka_vcmp(k, GCN_VOPC_V_CMP_EQ_F32, M0, KK(KM_ONE), KV(u));      /* 1 == u */
    ka_cnd(k, vd, KV(p), y, M0);
}

void km_silu(ka_t *k, uint32_t vd, ko x, uint32_t t)
{
    ka_vop2(k, GCN_VOP2_V_XOR_B32, t, KK(0x80000000u), x);         /* -x */
    km_exp(k, t + 1, KV(t), t + 2);
    ka_vop2(k, GCN_VOP2_V_ADD_F32, t + 1, KK(KM_ONE), KV(t + 1));  /* 1 + exp(-x) */
    km_div(k, vd, x, KV(t + 1), t + 2);
}

void km_sigmoid(ka_t *k, uint32_t vd, ko x, uint32_t t)
{
    ka_vop2(k, GCN_VOP2_V_XOR_B32, t, KK(0x80000000u), x);
    km_exp(k, t + 1, KV(t), t + 2);
    ka_vop2(k, GCN_VOP2_V_ADD_F32, t + 1, KK(KM_ONE), KV(t + 1));
    km_div(k, vd, KK(KM_ONE), KV(t + 1), t + 2);
}

void km_softplus(ka_t *k, uint32_t vd, ko x, uint32_t t)
{
    km_exp(k, t, x, t + 1);
    km_log1p(k, t + 1, KV(t), t + 2);
    ka_smov(k, K0, KK(KM_TWENTY));
    ka_vcmp(k, GCN_VOPC_V_CMP_LT_F32, M0, KS(K0), x);              /* 20 < x */
    ka_cnd(k, vd, KV(t + 1), x, M0);
}
