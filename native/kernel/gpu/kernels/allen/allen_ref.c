/* native/kernel/gpu/kernels/allen/allen_ref.c -- host mirror of allen_kern.c.
 * Host test only (floats allowed). Each kernel performs the GPU's IEEE operations in the GPU's
 * order on the same image bytes, so a correct kernel matches it bit for bit. Build with
 * -ffp-contract=off: every a * b + c here is two roundings unless written fmaf(). */
#include "allen_ref.h"
#include "allen_kern.h"
#include "fmath_ref.h"
#include <math.h>
#include <string.h>

static float F(const uint8_t *m, uint32_t off) { float f; memcpy(&f, m + off, 4); return f; }
static void W(uint8_t *m, uint32_t off, float f) { memcpy(m + off, &f, 4); }
static uint32_t U(const uint8_t *m, uint32_t off) { uint32_t u; memcpy(&u, m + off, 4); return u; }
static float BF(const uint8_t *m, uint32_t off)
{
    uint16_t h; memcpy(&h, m + off, 2);
    return km_f((uint32_t)h << 16);
}
static float fmax_hw(float a, float b) { return a > b ? a : b; }

/* tree() in allen_kern.c: v[j] = v[j] op v[j + off] for j < off, off = 32 .. 1 */
static float tree(float *v, int max)
{
    for (int off = 32; off; off >>= 1)
        for (int j = 0; j < off; j++) v[j] = max ? fmax_hw(v[j], v[j + off]) : v[j] + v[j + off];
    return v[0];
}

static void matvec(uint8_t *m, uint32_t C, int acc, const uint32_t *p, uint32_t g)
{
    uint32_t k = p[3], R = p[4], n = p[5];
    for (uint32_t r = g * R; r < g * R + R && r < n; r++) {
        float part[64];
        for (uint32_t j = 0; j < 64; j++) {
            float a = 0.0f;
            for (uint32_t c = 0; c < C; c++)
                for (uint32_t e = 0; e < 8; e++) {
                    uint32_t i = 512 * c + 8 * j + e;
                    a = fmaf(BF(m, p[0] + (r * k + i) * 2), F(m, p[1] + i * 4), a);
                }
            part[j] = a;
        }
        float s = tree(part, 0);
        if (acc) s = F(m, p[2] + r * 4) + s;
        W(m, p[2] + r * 4, s);
    }
}

static void norm(uint8_t *m, int mode, const uint32_t *p, uint32_t g)
{
    uint32_t x = p[0] + g * p[1], y = p[2] + g * p[3], z = p[6] + g * p[7], n = p[4];
    float part[64];
    for (uint32_t j = 0; j < 64; j++) {
        float a = 0.0f;
        for (uint32_t i = j; i < n; i += 64) { float v = F(m, x + i * 4); a = fmaf(v, v, a); }
        part[j] = a;
    }
    float t = tree(part, 0);
    if (mode == AK_NORM_L2) t = km_f(0x358637BDu) + t;
    else {
        uint32_t lg = 0; while ((1u << lg) < n) lg++;
        t = km_f((127u - lg) << 23) * t;
        t = km_f(p[8]) + t;
    }
    float inv = km_div_ref(1.0f, km_sqrt_ref(t));
    float sc = 1.0f;
    if (mode == AK_NORM_L2 && p[9]) sc = km_div_ref(1.0f, km_sqrt_ref((float)p[9]));
    for (uint32_t i = 0; i < n; i++) {
        float v = F(m, x + i * 4) * inv;
        if (mode == AK_NORM_ZC) v = v * (1.0f + F(m, p[5] + i * 4));
        else if (mode == AK_NORM_GATED) { v = v * F(m, p[5] + i * 4); v = v * km_silu_ref(F(m, z + i * 4)); }
        else v = v * sc;
        W(m, y + i * 4, v);
    }
}

static void conv(uint8_t *m, const uint32_t *p, uint32_t g)
{
    uint32_t cd = p[4];
    for (uint32_t ch = g * 64; ch < g * 64 + 64; ch++) {
        float val[4] = { F(m, p[1] + ch * 4), F(m, p[1] + (cd + ch) * 4), F(m, p[1] + (2 * cd + ch) * 4),
                         F(m, p[0] + ch * 4) };
        float a = 0.0f;
        for (int j = 0; j < 4; j++) a = a + F(m, p[2] + (ch * 4 + j) * 4) * val[j];
        W(m, p[3] + ch * 4, km_silu_ref(a));
        for (int j = 0; j < 3; j++) W(m, p[1] + (j * cd + ch) * 4, val[j + 1]);
    }
}

static void gdn(uint8_t *m, const uint32_t *p, uint32_t hv)
{
    uint32_t kd = p[7], kh = p[8], hk = hv >> p[9];
    float sp = km_softplus_ref(F(m, p[1] + hv * 4) + F(m, p[4] + hv * 4));
    float gl = -(km_exp_ref(F(m, p[3] + hv * 4)) * sp);
    float decay = km_exp_ref(gl), beta = km_sigmoid_ref(F(m, p[2] + hv * 4));
    uint32_t S = p[5] + hv * kd * 512, q = p[0] + hk * kd * 4, kk = q + kh * kd * 4;
    uint32_t v = p[0] + 2 * kh * kd * 4 + hv * 512;
    for (uint32_t c = 0; c < 128; c++) {
        float kv = 0.0f, o = 0.0f;
        for (uint32_t r = 0; r < kd; r++) {
            float s = F(m, S + r * 512 + c * 4) * decay;
            kv = kv + F(m, kk + r * 4) * s;
            W(m, S + r * 512 + c * 4, s);
        }
        float d = (F(m, v + c * 4) - kv) * beta;
        for (uint32_t r = 0; r < kd; r++) {
            float s = F(m, S + r * 512 + c * 4) + F(m, kk + r * 4) * d;
            o = o + F(m, q + r * 4) * s;
            W(m, S + r * 512 + c * 4, s);
        }
        W(m, p[6] + hv * 512 + c * 4, o);
    }
}

static void rope(uint8_t *m, const uint32_t *p, uint32_t h)
{
    uint32_t b = p[0] + h * p[1], half = p[5];
    for (uint32_t j = 0; j < half; j++) {
        float x1 = F(m, b + j * 4), x2 = F(m, b + (half + j) * 4);
        float c = F(m, p[2] + (p[4] * half + j) * 4), s = F(m, p[3] + (p[4] * half + j) * 4);
        W(m, b + j * 4, x1 * c - x2 * s);
        W(m, b + (half + j) * 4, x2 * c + x1 * s);
    }
}

static void attn(uint8_t *m, const uint32_t *p, uint32_t hh, float *sc)
{
    uint32_t kh = hh >> p[7], span = p[3], rs = p[8];
    uint32_t q = p[0] + hh * 1024, kc = p[1] + kh * 1024, vc = p[2] + kh * 1024;
    float scale = km_f(p[9]), mx[64], sm[64];
    for (int j = 0; j < 64; j++) { mx[j] = km_f(0xFF800000u); sm[j] = 0.0f; }
    for (uint32_t j = 0; j < span; j++) {
        float d = 0.0f;
        for (uint32_t i = 0; i < 256; i++) d = d + F(m, q + i * 4) * F(m, kc + j * rs + i * 4);
        sc[j] = scale * d;
        mx[j % 64] = fmax_hw(mx[j % 64], sc[j]);
    }
    float M = tree(mx, 1);
    for (uint32_t j = 0; j < span; j++) { sc[j] = km_exp_ref(sc[j] - M); sm[j % 64] = sm[j % 64] + sc[j]; }
    float S = tree(sm, 0);
    for (uint32_t j = 0; j < span; j++) sc[j] = km_div_ref(sc[j], S);
    for (uint32_t d = 0; d < 256; d++) {
        float o = 0.0f;
        for (uint32_t j = 0; j < span; j++) o = o + sc[j] * F(m, vc + j * rs + d * 4);
        o = o * km_sigmoid_ref(F(m, p[4] + hh * p[5] + d * 4));
        W(m, p[6] + hh * 1024 + d * 4, o);
    }
}

static void argmax(uint8_t *m, const uint32_t *p)
{
    float bv[64]; uint32_t bi[64];
    for (uint32_t j = 0; j < 64; j++) {
        bv[j] = km_f(0xFF800000u); bi[j] = 0xFFFFFFFFu;
        for (uint32_t i = j; i < p[1]; i += 64) { float v = F(m, p[0] + i * 4); if (v > bv[j]) { bv[j] = v; bi[j] = i; } }
    }
    for (int off = 32; off; off >>= 1)
        for (int j = 0; j < off; j++) {
            float b = bv[j + off]; uint32_t i = bi[j + off];
            if (b > bv[j] || (b == bv[j] && i < bi[j])) { bv[j] = b; bi[j] = i; }
        }
    memcpy(m + p[2], &bi[0], 4);
}

void ar_run(uint8_t *m, int id, uint32_t c, const uint32_t *p, uint32_t groups)
{
    static float sc[AK_MAX_SPAN];
    for (uint32_t g = 0; g < groups; g++) {
        switch (id) {
        case AK_MATVEC: matvec(m, c, 0, p, g); break;
        case AK_MATVEC_ACC: matvec(m, c, 1, p, g); break;
        case AK_NORM_ZC: case AK_NORM_GATED: case AK_NORM_L2: norm(m, id, p, g); break;
        case AK_EMBED:
            for (uint32_t i = 0; i < p[3]; i++) W(m, p[2] + i * 4, BF(m, p[0] + (p[1] * p[3] + i) * 2));
            break;
        case AK_CONV: conv(m, p, g); break;
        case AK_GDN: gdn(m, p, g); break;
        case AK_ROPE: rope(m, p, g); break;
        case AK_ATTN: attn(m, p, g, sc); break;
        case AK_SWIGLU:
            for (uint32_t i = g * 64; i < g * 64 + 64; i++)
                W(m, p[2] + i * 4, km_silu_ref(F(m, p[0] + i * 4)) * F(m, p[1] + i * 4));
            break;
        case AK_COPY:
            for (uint32_t i = g * 64; i < g * 64 + 64; i++) memcpy(m + p[1] + i * 4, m + p[0] + i * 4, 4);
            break;
        case AK_ARGMAX: argmax(m, p); break;
        }
    }
    (void)U;
}
