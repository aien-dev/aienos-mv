/* np_linux.c -- the Linux side of the native pipeline comparison (aienos-mv#432, scripts/native_pipe_compare.sh).
 *
 * Runs the workload of core/pipe_bench.h pb_flow on Linux: gen [| xform] | sink, `total` bytes produced 4096 per step
 * (every fifth byte NUL), each stage reading up to 4096 bytes per step, the sink returning count and FNV-1a. Every
 * stage is its own Linux process and the stages are joined by pipe(2). Each stage follows stage ABI v1 (core/task.h:
 * workspace cells, OSTG magic, flags, out_len, params, return 0 MORE / 1 DONE) and is driven by the same loop:
 *   mode c      C step functions with the semantics of tests/fixtures/task/{tgen,txform,tsink}.osc (as tests/test_task.c)
 *   mode omega  the .osc stage sources themselves, compiled by Omega's oscc front end and code generator ONCE in the
 *               parent before any timed run (compile time is not measured), called as native AArch64 code through
 *               osc_native_call in the forked stage processes (built only with -DNP_OMEGA and the Omega sources)
 * Prints one line per scenario with wall time (CLOCK_MONOTONIC, spawn to reaped) and CPU time (getrusage of the stage
 * processes), measured on the machine it runs on. No pass threshold; the bytes and the hash are checked every run.
 * Spec read: aien-protocols-mv STREAM_TASK.md at 181855c9ff3e (ST10.2: Omega evidence only from real Linux processes).
 * UNVERIFIED (not pinned by the spec): one process per stage, pipe(2) between them, the 4096-byte step and the timing
 * window (spawn to reaped) are our own choices to mirror the AIENOS path.
 * Usage: np_linux c|omega STAGES(2|3) TOTAL RUNS [UNIT_DIR]   (UNIT_DIR holds tgen.osc txform.osc tsink.osc) */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef NP_OMEGA
#include "osc_cg.h"
#include "osc_front.h"
#include "osc_native.h"
#endif

/* stage ABI v1 (core/task.h, UNVERIFIED own contract there) */
#define W_FLAGS 2
#define W_OUT_LEN 3
#define W_EXIT 4
#define W_PARAMS 8
#define W_OUT 64
#define OUT_MAX 4096
#define CELLS (W_OUT + OUT_MAX)
#define F_EOF 1u
#define STG_MAGIC 0x4F535447u
#define CHUNK 4096
#define KEY 0x5Au
#define RUNS_MAX 9

enum { S_GEN, S_XFORM, S_SINK };

static uint8_t pat(uint64_t i)
{
    return i % 5 == 0 ? 0 : (uint8_t)((((i * 2654435761ull) & 0xFFFFFFFFull) >> 13) & 255u);
}

static uint64_t fnv(uint64_t total, uint8_t key)
{
    uint64_t h = 14695981039346656037ull;
    for (uint64_t i = 0; i < total; i++)
        h = (h ^ (uint8_t)(pat(i) ^ key)) * 1099511628211ull;
    return h;
}

/* ---- mode c: the C step functions ---- */
static uint64_t c_gen(uint64_t *w)
{
    uint64_t total = w[8], per = w[9], sent = w[16];
    if (per == 0 || per > OUT_MAX)
        per = OUT_MAX;
    uint64_t n = per;
    if (total) {
        if (sent >= total) {
            w[3] = 0;
            w[4] = 0;
            return 1;
        }
        if (total - sent < n)
            n = total - sent;
    }
    for (uint64_t j = 0; j < n; j++)
        w[64 + j] = pat(sent + j);
    w[16] = sent + n;
    w[3] = n;
    if (total && sent + n >= total) {
        w[4] = 0;
        return 1;
    }
    return 0;
}

static uint64_t c_xform(const uint8_t *in, size_t n, uint64_t *w)
{
    for (size_t j = 0; j < n; j++)
        w[64 + j] = in[j] ^ (w[8] & 255);
    w[16] += n;
    w[3] = n;
    if ((w[2] & 1) && n == 0) {
        w[4] = 0;
        return 1;
    }
    return 0;
}

static uint64_t c_sink(const uint8_t *in, size_t n, uint64_t *w)
{
    uint64_t h = w[17];
    if (w[18] == 0) {
        h = 14695981039346656037ull;
        w[18] = 1;
    }
    for (size_t j = 0; j < n; j++)
        h = (h ^ in[j]) * 1099511628211ull;
    w[17] = h;
    uint64_t c = w[16] + n;
    w[16] = c;
    w[3] = 0;
    if ((w[2] & 1) && n == 0) {
        for (unsigned k = 0; k < 8; k++) {
            w[64 + k] = (c >> (k * 8)) & 255;
            w[72 + k] = (h >> (k * 8)) & 255;
        }
        w[3] = 16;
        w[4] = 0;
        return 1;
    }
    return 0;
}

/* ---- mode omega: the .osc stages as native code ---- */
#ifdef NP_OMEGA
static const char *const unit_name[3] = { "tgen.osc", "txform.osc", "tsink.osc" };
static const char *const entry_name[3] = { "gen_step", "xform_step", "sink_step" };
/* compiled once in the parent before any timed run; the forked stage processes inherit the mapped code */
static OscUnit U[3];
static OscCode code[3];
static OscNative nm[3];
static OscRt rt[3];
static int fi[3] = { -1, -1, -1 };

static int omega_load(const char *dir, int stage)
{
    char path[512], err[200];
    snprintf(path, sizeof path, "%s/%s", dir, unit_name[stage]);
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    static char src[65536];
    size_t len = fread(src, 1, sizeof src - 1, f);
    fclose(f);
    src[len] = 0;
    OscDiag d;
    OscUnit *u = &U[stage];
    if (osc_compile(src, len, u, &d, NULL) != 0 || osc_ir_validate(u, err, sizeof err) != 0)
        return -2;
    for (int i = 0; i < u->nfuncs; i++)
        if (strcmp(u->funcs[i].name, entry_name[stage]) == 0)
            fi[stage] = i;
    if (fi[stage] < 0 || osc_cg_compile(u, &code[stage], err, sizeof err) != 0 ||
        osc_native_map(&nm[stage], code[stage].code, code[stage].len) != 0)
        return -3;
    osc_rt_init(&rt[stage]);
    return 0;
}

static uint64_t omega_step(int s, const uint8_t *in, size_t n, uint64_t *w)
{
    static const uint8_t none[1];
    uint64_t args[4] = { (uint64_t)(uintptr_t)(n ? in : none), n, (uint64_t)(uintptr_t)w, CELLS }, ret = 0;
    osc_rt_reset(&rt[s]);
    if (osc_native_call(&U[s], fi[s], &nm[s], code[s].entry[fi[s]], &rt[s], args, 4, &ret) != 0)
        return 99; /* a trap: the driver treats anything but 0 / 1 as a failure */
    return ret;
}
#endif

static int omega_mode;

static uint64_t step(int stage, const uint8_t *in, size_t n, uint64_t *w)
{
#ifdef NP_OMEGA
    if (omega_mode)
        return omega_step(stage, in, n, w);
#endif
    return stage == S_GEN ? c_gen(w) : stage == S_XFORM ? c_xform(in, n, w) : c_sink(in, n, w);
}

static int write_all(int fd, const uint8_t *b, size_t n)
{
    while (n) {
        ssize_t k = write(fd, b, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

/* one stage process: read up to CHUNK per step (none for gen), step, write the output cells as bytes */
static int stage_main(int stage, int in_fd, int out_fd, uint64_t total)
{
    static uint64_t w[CELLS];
    static uint8_t in[CHUNK], out[OUT_MAX];
    memset(w, 0, sizeof w);
    w[0] = STG_MAGIC;
    w[1] = 1;
    if (stage == S_GEN) {
        w[W_PARAMS] = total;
        w[W_PARAMS + 1] = CHUNK;
    } else if (stage == S_XFORM) {
        w[W_PARAMS] = KEY;
    }
    int eof = stage == S_GEN; /* gen has no stdin: it reads as EOF (task.h) */
    for (;;) {
        ssize_t n = 0;
        if (!eof) {
            do
                n = read(in_fd, in, sizeof in);
            while (n < 0 && errno == EINTR);
            if (n < 0)
                return 74;
            if (n == 0)
                eof = 1;
        }
        w[W_FLAGS] = eof ? F_EOF : 0;
        uint64_t r = step(stage, in, (size_t)n, w);
        if (r > 1 || w[W_OUT_LEN] > OUT_MAX)
            return 71;
        for (uint64_t j = 0; j < w[W_OUT_LEN]; j++)
            out[j] = (uint8_t)w[W_OUT + j];
        if (w[W_OUT_LEN] && write_all(out_fd, out, w[W_OUT_LEN]) != 0)
            return 141;
        if (r == 1)
            return (int)(w[W_EXIT] & 255);
    }
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static uint64_t tv_us(struct timeval t)
{
    return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_usec;
}

static void sort(uint64_t *v, int n)
{
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
            uint64_t t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
}

int main(int argc, char **argv)
{
    if (argc < 5 || (strcmp(argv[1], "c") != 0 && strcmp(argv[1], "omega") != 0)) {
        fprintf(stderr, "usage: np_linux c|omega STAGES TOTAL RUNS [UNIT_DIR]\n");
        return 2;
    }
    omega_mode = strcmp(argv[1], "omega") == 0;
#ifndef NP_OMEGA
    if (omega_mode) {
        fprintf(stderr, "np_linux: built without NP_OMEGA\n");
        return 2;
    }
#endif
    int nst = atoi(argv[2]), runs = atoi(argv[4]);
    uint64_t total = strtoull(argv[3], 0, 10);
    const char *dir = argc > 5 ? argv[5] : ".";
    (void)dir;
    if ((nst != 2 && nst != 3) || runs < 1 || runs > RUNS_MAX)
        return 2;
#ifdef NP_OMEGA
    for (int s = 0; omega_mode && s < 3; s++)
        if (omega_load(dir, s) != 0) {
            fprintf(stderr, "np_linux: cannot compile %s/%s\n", dir, unit_name[s]);
            return 1;
        }
#endif
    uint64_t wall[RUNS_MAX], cpu[RUNS_MAX];
    int bad = 0;
    for (int r = 0; r < runs; r++) {
        struct rusage ru0, ru1;
        getrusage(RUSAGE_CHILDREN, &ru0);
        int kind[3] = { S_GEN, nst == 3 ? S_XFORM : S_SINK, S_SINK };
        int fds[3][2];
        for (int k = 0; k < nst; k++)
            if (pipe(fds[k]) != 0)
                return 1;
        uint64_t t0 = now_us();
        pid_t pid[3];
        for (int k = 0; k < nst; k++) {
            pid[k] = fork();
            if (pid[k] == 0) {
                int in = k ? fds[k - 1][0] : -1, out = fds[k][1];
                for (int j = 0; j < nst; j++) {
                    if (fds[j][0] != in)
                        close(fds[j][0]);
                    if (fds[j][1] != out)
                        close(fds[j][1]);
                }
                _exit(stage_main(kind[k], in, out, total));
            }
        }
        for (int j = 0; j < nst; j++) {
            close(fds[j][1]);
            if (j != nst - 1)
                close(fds[j][0]);
        }
        uint8_t res[16];
        size_t got = 0;
        ssize_t n;
        while (got < sizeof res && (n = read(fds[nst - 1][0], res + got, sizeof res - got)) > 0)
            got += (size_t)n;
        close(fds[nst - 1][0]);
        int st_ok = 1;
        for (int k = 0; k < nst; k++) {
            int st = 0;
            waitpid(pid[k], &st, 0);
            st_ok = st_ok && WIFEXITED(st) && WEXITSTATUS(st) == 0;
        }
        uint64_t t1 = now_us();
        getrusage(RUSAGE_CHILDREN, &ru1);
        uint64_t c = 0, h = 0;
        for (int k = 7; k >= 0; k--) {
            c = (c << 8) | res[k];
            h = (h << 8) | res[8 + k];
        }
        bad += !(st_ok && got == 16 && c == total && h == fnv(total, nst == 3 ? KEY : 0));
        wall[r] = t1 - t0;
        cpu[r] = tv_us(ru1.ru_utime) + tv_us(ru1.ru_stime) - tv_us(ru0.ru_utime) - tv_us(ru0.ru_stime);
    }
    sort(wall, runs);
    sort(cpu, runs);
    uint64_t med = wall[runs / 2];
    printf("linux_bench: mode=%s stages=%d bytes=%llu chunk=%d runs=%d wall_us_min=%llu wall_us_median=%llu "
           "wall_us_max=%llu bytes_per_s_median=%llu cpu_us_median=%llu bad_runs=%d; Linux %s, one process per stage, "
           "pipe(2), not physical qualification\n",
           argv[1], nst, (unsigned long long)total, CHUNK, runs, (unsigned long long)wall[0], (unsigned long long)med,
           (unsigned long long)wall[runs - 1], (unsigned long long)(med ? total * 1000000ull / med : 0),
           (unsigned long long)cpu[runs / 2], bad, omega_mode ? "Omega oscc native code" : "C stages");
    return bad ? 1 : 0;
}
