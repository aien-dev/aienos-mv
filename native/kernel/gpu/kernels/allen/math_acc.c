/* native/kernel/gpu/kernels/allen/math_acc.c -- host accuracy sweep of the kmath mirror vs glibc.
 * Exhaustive over each routine's working domain; prints mismatch counts and max ulp error.
 * Host test only (floats allowed). Build: see Makefile target math-acc. */
#include "fmath_ref.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define NT 8
static int64_t ord(uint32_t u) { return (u & 0x80000000u) ? -(int64_t)(u & 0x7FFFFFFFu) : (int64_t)u; }
static uint64_t ulpd(float a, float b)
{
    if (a != a || b != b) return (a != a && b != b) ? 0 : UINT64_MAX;
    int64_t d = ord(km_u(a)) - ord(km_u(b));
    return (uint64_t)(d < 0 ? -d : d);
}

typedef struct { int fn; uint32_t lo, hi; uint64_t n, bad, maxu; uint32_t worst; } job_t;

static float run_fn(int fn, float x, float *want)
{
    switch (fn) {
    case 0: *want = sqrtf(x); return km_sqrt_ref(x);
    case 1: *want = 1.0f / x; return km_div_ref(1.0f, x);
    case 2: *want = expf(x); return km_exp_ref(x);
    case 3: *want = log1pf(x); return km_log1p_ref(x);
    case 4: *want = x / (1.0f + expf(-x)); return km_silu_ref(x);
    case 5: *want = 1.0f / (1.0f + expf(-x)); return km_sigmoid_ref(x);
    default: *want = x > 20.0f ? x : log1pf(expf(x)); return km_softplus_ref(x);
    }
}

static void *work(void *p)
{
    job_t *j = p;
    for (uint64_t u = j->lo; u <= j->hi; u++) {
        float x = km_f((uint32_t)u), want, got = run_fn(j->fn, x, &want);
        uint64_t d = ulpd(got, want);
        j->n++;
        if (d) { j->bad++; if (d > j->maxu) { j->maxu = d; j->worst = (uint32_t)u; } }
    }
    return 0;
}

/* sweep bit patterns [lo, hi] (same sign) with NT threads */
static void sweep(const char *name, int fn, uint32_t lo, uint32_t hi)
{
    pthread_t th[NT]; job_t jb[NT];
    uint64_t span = (uint64_t)hi - lo + 1;
    for (int i = 0; i < NT; i++) {
        jb[i] = (job_t){ fn, (uint32_t)(lo + span * i / NT), (uint32_t)(lo + span * (i + 1) / NT - 1), 0, 0, 0, 0 };
        pthread_create(&th[i], 0, work, &jb[i]);
    }
    uint64_t n = 0, bad = 0, maxu = 0; uint32_t worst = 0;
    for (int i = 0; i < NT; i++) {
        pthread_join(th[i], 0);
        n += jb[i].n; bad += jb[i].bad;
        if (jb[i].maxu > maxu) { maxu = jb[i].maxu; worst = jb[i].worst; }
    }
    float w = km_f(worst), want; float got = run_fn(fn, w, &want);
    printf("%-9s [%a, %a] n=%llu differ=%llu (%.6f%%) max_ulp=%llu",
           name, km_f(lo), km_f(hi), (unsigned long long)n, (unsigned long long)bad,
           100.0 * (double)bad / (double)n, (unsigned long long)maxu);
    if (bad) printf(" worst x=%a got=%a want=%a", w, got, want);
    printf("\n");
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t xr(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

int main(int argc, char **argv)
{
    int quick = argc > 1;
    uint32_t step = quick ? 1 : 0;
    (void)step;
    /* sqrt: all positive normals (correctly rounded expected) */
    sweep("sqrt", 0, 0x00800000u, quick ? 0x00FFFFFFu : 0x7F7FFFFFu);
    /* reciprocal: b in [2^-64, 2^127] (correctly rounded expected) */
    sweep("rcp", 1, 0x1F800000u, quick ? 0x3FFFFFFFu : 0x7F000000u);
    /* division: random pairs */
    {
        uint64_t n = quick ? 1000000 : 200000000, bad = 0;
        for (uint64_t i = 0; i < n; i++) {
            uint32_t ea = 127 - 60 + xr() % 121, eb = 127 - 30 + xr() % 131;
            float a = km_f((xr() & 0x807FFFFFu) | (ea << 23)), b = km_f((xr() & 0x007FFFFFu) | (eb << 23));
            if (km_div_ref(a, b) != a / b) bad++;
        }
        printf("div       random a in 2^[-60,60], b in 2^[-30,100]: n=%llu differ=%llu\n",
               (unsigned long long)n, (unsigned long long)bad);
    }
    /* exp vs glibc expf: x in [-104, 0] and [0, 88.7228] */
    sweep("exp-", 2, 0x80000000u, quick ? 0xBF800000u : 0xC2D00000u);
    sweep("exp+", 2, 0x00000000u, quick ? 0x3F800000u : 0x42B17217u);
    /* log1p vs glibc log1pf: y in [0, 5e8] (softplus range: exp(x <= 20)) */
    sweep("log1p", 3, 0x00000000u, quick ? 0x3F800000u : 0x4DEE6B28u);
    /* activations, full working ranges */
    sweep("silu-", 4, 0x80000000u, quick ? 0xBF800000u : 0xC2D00000u);
    sweep("silu+", 4, 0x00000000u, quick ? 0x3F800000u : 0x7F7FFFFFu);
    sweep("sigm-", 5, 0x80000000u, quick ? 0xBF800000u : 0xC2D00000u);
    sweep("sigm+", 5, 0x00000000u, quick ? 0x3F800000u : 0x7F7FFFFFu);
    sweep("softp-", 6, 0x80000000u, quick ? 0xBF800000u : 0xC2D00000u);
    sweep("softp+", 6, 0x00000000u, quick ? 0x3F800000u : 0x7F7FFFFFu);
    return 0;
}
