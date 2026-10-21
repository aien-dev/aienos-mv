/* native/kernel/gpu/kernels/allen/allen_test.c -- the small Allen on gcn_interp against the host
 * mirror and the host reference (L389). Host test only (floats allowed here).
 *
 *   allen_test MODEL.agp1 IDS.txt NEW [THREADS] [MODE]
 *     IDS.txt: the prompt token ids (whitespace separated); NEW: tokens to generate greedily.
 *     MODE both (default): every launch runs on the interpreter (image A) and on the mirror
 *       (image B); after each launch the work and state region of A and B must be equal byte for
 *       byte (pre-registered tolerance: 0 ulp). MODE interp / mirror: one engine only.
 *   Prints one JSON line: tokens, per-kernel launch / instruction / byte counts, the first
 *   mismatch if any, and the top-1 logit of each generated step.
 *
 * Build: gcc -O2 -ffp-contract=off -I. -I.. -I../.. kernels/allen/{allen_test,allen_kern,allen_ref,allen_plan,ka,kmath,fmath_ref}.c ../gcn_enc.c ../gcn_interp.c \
 *        -lm -lpthread   (see the Makefile in this directory)
 */
#include "allen_kern.h"
#include "allen_plan.h"
#include "allen_ref.h"
#include "../../gcn_interp.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static ka_t code[AK_COUNT][8];
static int built[AK_COUNT][8];

typedef struct {
    uint8_t *img; uint64_t size; const ap_disp *d; const ka_t *k;
    uint32_t next; pthread_mutex_t mu;
    uint64_t insns, bytes; const char *err; uint32_t bad_group;
} job_t;

static void *worker(void *arg)
{
    job_t *j = arg;
    gcn_wave_t *w = malloc(sizeof *w);
    for (;;) {
        pthread_mutex_lock(&j->mu);
        uint32_t g = j->next++;
        pthread_mutex_unlock(&j->mu);
        if (g >= j->d->groups) break;
        gcn_wave_init(w);
        gcn_wave_attach_mem(w, j->img, j->size, 0);
        w->sgpr[0] = 0; w->sgpr[1] = 0; w->sgpr[2] = 0xFFFFFFFFu; w->sgpr[3] = 0x00027FACu;
        for (int i = 0; i < 12; i++) w->sgpr[AK_S_P0 + i] = j->d->p[i];
        w->sgpr[AK_S_TGID] = g;
        gcn_wave_set_threads_1d(w, 64, g, 0);
        for (int l = 0; l < 64; l++) w->vgpr[l][0] = (uint32_t)l;
        gcn_wave_run(w, j->k->w, j->k->n);
        const char *e = w->error ? w->error : w->inexact_ops ? "approximate instruction executed"
                      : w->mad_ops ? "denormal-flushing mad executed" : w->oob ? "out-of-bounds buffer access" : 0;
        pthread_mutex_lock(&j->mu);
        j->insns += w->insn_count; j->bytes += w->vmem_bytes;
        if (e && !j->err) { j->err = e; j->bad_group = g; }
        pthread_mutex_unlock(&j->mu);
    }
    free(w);
    return 0;
}

static const char *interp(uint8_t *img, uint64_t size, const ap_disp *d, int threads, uint64_t *insns, uint64_t *bytes)
{
    if (!built[d->id][d->c]) {
        if (ak_build(&code[d->id][d->c], d->id, d->c)) return code[d->id][d->c].err;
        built[d->id][d->c] = 1;
    }
    job_t j = { img, size, d, &code[d->id][d->c], 0, PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, 0 };
    pthread_t t[64];
    if (threads > 64) threads = 64;
    for (int i = 0; i < threads; i++) pthread_create(&t[i], 0, worker, &j);
    for (int i = 0; i < threads; i++) pthread_join(t[i], 0);
    *insns += j.insns; *bytes += j.bytes;
    return j.err;
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n ? *n : 1);
    if (fread(b, 1, *n, f) != *n) { free(b); b = 0; }
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: allen_test MODEL.agp1 IDS.txt NEW [THREADS] [both|interp|mirror] [THETA]\n"); return 2; }
    int threads = argc > 4 ? atoi(argv[4]) : 8;
    const char *mode = argc > 5 ? argv[5] : "both";
    double theta = argc > 6 ? atof(argv[6]) : 1e7;
    int use_i = strcmp(mode, "mirror") != 0, use_m = strcmp(mode, "interp") != 0;
    size_t fn;
    uint8_t *file = slurp(argv[1], &fn);
    if (!file) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    uint32_t ids[4096]; int np = 0;
    FILE *fi = fopen(argv[2], "r");
    if (!fi) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    while (np < 4096 && fscanf(fi, "%u", &ids[np]) == 1) np++;
    fclose(fi);
    int nnew = atoi(argv[3]);
    static ap_model m;
    const char *err;
    if (ap_parse(file, fn, (uint32_t)(np + nnew + 1), &m, &err)) { fprintf(stderr, "%s\n", err); return 1; }
    uint8_t *A = use_i ? calloc(1, m.size) : 0, *B = use_m ? calloc(1, m.size) : 0;
    uint32_t half = m.rot / 2;
    for (uint8_t *img = A; ; img = B) {
        if (img) {
            ap_fill(&m, file, img);
            for (uint32_t p = 0; p < m.max_pos; p++)
                for (uint32_t j = 0; j < half; j++) {
                    double inv = 1.0 / pow(theta, (double)(2 * j) / (double)m.rot);
                    float c = (float)cos(p * inv), s = (float)sin(p * inv);
                    memcpy(img + m.cos + (p * half + j) * 4, &c, 4);
                    memcpy(img + m.sin + (p * half + j) * 4, &s, 4);
                }
        }
        if (img == B) break;
    }
    free(file);
    uint64_t lo = m.x, hi = m.size; /* work buffers, tables and state: everything not a weight */
    static ap_disp d[AP_MAX_DISP];
    uint64_t nl[AK_COUNT] = {0}, ni[AK_COUNT] = {0}, nb[AK_COUNT] = {0};
    char mism[256] = "";
    uint32_t out[4096]; int nout = 0;
    float top[4096];
    uint32_t tok = ids[0];
    struct timespec t0, t1;
    double s_int = 0, s_mir = 0;
    for (int pos = 0; pos < np + nnew - 1; pos++) {
        int nd = ap_token(&m, tok, (uint32_t)pos, d);
        for (int i = 0; i < nd; i++) {
            if (use_i) {
                clock_gettime(CLOCK_MONOTONIC, &t0);
                const char *e = interp(A, m.size, &d[i], threads, &ni[d[i].id], &nb[d[i].id]);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                s_int += (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
                if (e) {
                    printf("{\"result\":\"FAIL\",\"pos\":%d,\"launch\":%d,\"kernel\":\"%s\",\"error\":\"%s\"}\n", pos, i, ak_name(d[i].id), e);
                    return 1;
                }
            }
            if (use_m) {
                clock_gettime(CLOCK_MONOTONIC, &t0);
                ar_run(B, d[i].id, d[i].c, d[i].p, d[i].groups);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                s_mir += (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
            }
            nl[d[i].id]++;
            if (use_i && use_m && !mism[0] && memcmp(A + lo, B + lo, hi - lo)) {
                uint64_t o = lo; while (A[o] == B[o]) o++;
                snprintf(mism, sizeof mism, "pos %d launch %d kernel %s first differing byte at image offset %llu",
                         pos, i, ak_name(d[i].id), (unsigned long long)o);
                memcpy(A + lo, B + lo, hi - lo); /* carry on from the mirror's state to find every kind */
            }
        }
        uint8_t *R = use_i ? A : B;
        uint32_t next; memcpy(&next, R + m.tok, 4);
        if (pos + 1 < np) tok = ids[pos + 1];
        else {
            memcpy(&top[nout], R + m.logits + next * 4, 4);
            out[nout++] = next; tok = next;
        }
        fprintf(stderr, "pos %d -> %u (interp %.1f s, mirror %.1f s)\n", pos, next, s_int, s_mir);
    }
    uint32_t next; memcpy(&next, (use_i ? A : B) + m.tok, 4);
    if (nnew > 0 && np + nnew - 1 >= np) { (void)next; }
    printf("{\"result\":\"%s\",\"mode\":\"%s\",\"prompt_tokens\":%d,\"generated_ids\":[", mism[0] ? "MISMATCH" : "PASS", mode, np);
    for (int i = 0; i < nout; i++) printf("%s%u", i ? "," : "", out[i]);
    printf("],\"top1_logit\":[");
    for (int i = 0; i < nout; i++) printf("%s%.6f", i ? "," : "", top[i]);
    printf("],\"mismatch\":\"%s\",\"seconds_interp\":%.1f,\"seconds_mirror\":%.1f,\"kernels\":{", mism, s_int, s_mir);
    int first = 1;
    for (int k = 0; k < AK_COUNT; k++) {
        if (!nl[k]) continue;
        printf("%s\"%s\":{\"launches\":%llu,\"wave_insns\":%llu,\"bytes\":%llu}", first ? "" : ",", ak_name(k),
               (unsigned long long)nl[k], (unsigned long long)ni[k], (unsigned long long)nb[k]);
        first = 0;
    }
    printf("},\"image_bytes\":%llu}\n", (unsigned long long)m.size);
    return mism[0] ? 1 : 0;
}
