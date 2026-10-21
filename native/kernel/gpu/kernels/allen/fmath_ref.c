/* native/kernel/gpu/kernels/allen/fmath_ref.c -- host mirror of kmath.c, op for op. */
#define KMATH_HOST_ONLY
#include "kmath.h"
#include "fmath_ref.h"
#include <math.h>

#define F km_f
#define U km_u

/* GCN v_max_f32 / v_min_f32 on non-NaN operands (src0, src1) */
static inline float gmax(float a, float b) { return a > b ? a : b; }
static inline float gmin(float a, float b) { return a < b ? a : b; }

float km_exp_ref(float x)
{
    float xc = gmax(F(KM_EXP_LO), x);
    xc = gmin(F(KM_EXP_HI), xc);
    float nf = F(KM_LOG2E) * xc;
    nf = rintf(nf);
    float r = fmaf(-nf, F(KM_LN2_HI), xc);
    r = fmaf(-nf, F(KM_LN2_LO), r);
    float p = F(KM_EXP_P0);
    p = fmaf(p, r, F(KM_EXP_P1));
    p = fmaf(p, r, F(KM_EXP_P2));
    p = fmaf(p, r, F(KM_EXP_P3));
    p = fmaf(p, r, F(KM_EXP_P4));
    p = fmaf(p, r, F(KM_EXP_P5));
    float z = r * r;
    p = fmaf(p, z, r);
    p = F(KM_ONE) + p;
    int32_t ni = (int32_t)nf;
    int32_t n1 = ni >> 1;
    int32_t n2 = ni - n1;
    p = p * F((uint32_t)(n1 + 127) << 23);
    p = p * F((uint32_t)(n2 + 127) << 23);
    return F(KM_EXP_HI) < x ? F(KM_INF) : p;
}

float km_div_ref(float a, float b)
{
    float sc = F(KM_TWO64) < b ? F(KM_TWOM64) : F(KM_ONE);
    float a1 = a * sc, b1 = b * sc;
    float r = F(KM_RCP_MAGIC - U(b1));
    for (int i = 0; i < 4; i++) {
        float e = fmaf(-b1, r, F(KM_ONE));
        r = fmaf(e, r, r);
    }
    float q0 = a1 * r;
    float q = fmaf(-b1, q0, a1);
    q = fmaf(q, r, q0);
    float q1 = q;
    q = fmaf(-b1, q1, a1);
    q = fmaf(q, r, q1);
    if (0.0f == q0) q = q0;
    return F(KM_INF) == b ? F(U(a) & 0x80000000u) : q;
}

float km_sqrt_ref(float x)
{
    float g = F(KM_RSQ_MAGIC - (U(x) >> 1));
    float y = x * g;
    float h = 0.5f * g;
    for (int i = 0; i < 3; i++) {
        float r = fmaf(-y, h, 0.5f);
        y = fmaf(y, r, y);
        h = fmaf(h, r, h);
    }
    float d = fmaf(-y, y, x);
    return fmaf(d, h, y);
}

float km_log1p_ref(float y)
{
    static const uint32_t P[9] = { KM_LOG_P0, KM_LOG_P1, KM_LOG_P2, KM_LOG_P3, KM_LOG_P4,
                                   KM_LOG_P5, KM_LOG_P6, KM_LOG_P7, KM_LOG_P8 };
    float u = F(KM_ONE) + y;
    int32_t e = (int32_t)(U(u) >> 23) - 127;
    uint32_t mb = (U(u) & 0x007FFFFFu) | KM_ONE;
    if (KM_SQRT2 < mb) { mb -= 0x00800000u; e += 1; }
    float fe = (float)e;
    float f = F(0xBF800000u) + F(mb);
    float z = f * f;
    float p = F(P[0]);
    for (int i = 1; i < 9; i++) p = fmaf(p, f, F(P[i]));
    p = p * f;
    p = p * z;
    p = fmaf(fe, F(KM_LOG_Q1), p);
    p = fmaf(z, F(0xBF000000u), p);
    float c = F(0xBF800000u) + u;
    c = y - c;
    c = km_div_ref(c, u);
    p = p + c;
    p = f + p;
    p = fmaf(fe, F(KM_LOG_Q2), p);
    return F(KM_ONE) == u ? y : p;
}

float km_silu_ref(float x)
{
    float e = km_exp_ref(F(U(x) ^ 0x80000000u));
    e = F(KM_ONE) + e;
    return km_div_ref(x, e);
}

float km_sigmoid_ref(float x)
{
    float e = km_exp_ref(F(U(x) ^ 0x80000000u));
    e = F(KM_ONE) + e;
    return km_div_ref(F(KM_ONE), e);
}

float km_softplus_ref(float x)
{
    float e = km_exp_ref(x);
    float l = km_log1p_ref(e);
    return F(KM_TWENTY) < x ? x : l;
}
