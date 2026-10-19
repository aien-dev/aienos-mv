/* pipe_bench.h -- native pipeline baseline measurements (aienos-mv#432), shared by the host test (tests/test_task.c,
 * `test_task --bench`, C stand-in stages) and the QEMU self-test (core/task_test.c, CK_TASK_TEST, the admitted Omega
 * stages tgen/txform/tsink), so both measure the same scenarios with the same code. Static functions only, no libc, no
 * allocation: the caller owns the world and hands it over in a struct pb_env. Define PB_FMT (an snprintf) first.
 *
 * Every scenario checks its own result (exact count and FNV-1a of the bytes, statuses) and that nothing is left over
 * (env->clean: no task, no pipe, no stray capability), and fills `det` with key=value pairs. The numbers are what the
 * clock said on the machine that ran it: on the host that is the Spark CPU running C stand-ins, in QEMU it is the
 * emulated counter under TCG. Neither is a hardware measurement; the label printed with every line says which.
 *
 * Contract read: aien-protocols-mv specs/osh-platform/OSH_PLATFORM_ABI.md at fb5fca98e667 (8.5 the 64 KiB pipe minimum,
 * 8.6 cancellation, 12 cleanup) and STREAM_TASK.md on feat/osh-stream-abi-43 at 181855c9ff3e. UNVERIFIED (B1): the
 * spec sets no throughput, latency or cancellation-time target, so these are baselines with no pass threshold; the
 * checks are correctness, boundedness and cleanup only. */
#ifndef AIENOS_CK_PIPE_BENCH_H
#define AIENOS_CK_PIPE_BENCH_H

#include "task.h"

#ifndef PB_FMT
#error "define PB_FMT (snprintf or ck_snprintf) before including pipe_bench.h"
#endif

#define PB_RUNS_MAX 9u

struct pb_env {
    struct ck_task_tab *tab;
    struct ck_pipe_pool *pool;
    struct ck_cap_table *sess;
    void (*setup)(void);                          /* a fresh world: empty pool, session with only the spawn cap */
    struct ck_task_spawn_req (*req)(unsigned img); /* a bound spawn request for image img */
    int (*clean)(void);                           /* nothing left over */
    uint64_t (*now_us)(void);                     /* the clock, microseconds */
    unsigned gen, xform, sink;                    /* image indices of the three stages */
};

static uint8_t pb_pat(uint64_t i) /* tgen.osc byte i */
{
    return i % 5 == 0 ? 0 : (uint8_t)((((i * 2654435761ull) & 0xFFFFFFFFull) >> 13) & 255u);
}

static uint64_t pb_fnv(uint64_t total, uint8_t key)
{
    uint64_t h = 14695981039346656037ull;
    for (uint64_t i = 0; i < total; i++)
        h = (h ^ (uint8_t)(pb_pat(i) ^ key)) * 1099511628211ull;
    return h;
}

static uint64_t pb_le64(const uint8_t *b)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | b[i];
    return v;
}

static size_t pb_drain_close(const struct pb_env *e, struct ck_handle hr, uint8_t *buf, size_t max)
{
    size_t got = 0, n = 0;
    while (got < max && ck_stream_read(e->pool, e->sess, hr, buf + got, max - got, 0, &n) == OSH_E_OK && n)
        got += n;
    ck_stream_close(e->pool, e->sess, hr);
    return got;
}

static void pb_sort(uint64_t *v, unsigned n)
{
    for (unsigned i = 1; i < n; i++)
        for (unsigned j = i; j > 0 && v[j - 1] > v[j]; j--) {
            uint64_t t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
}

static uint64_t pb_rate(uint64_t bytes, uint64_t us) /* bytes per second, integer; 0 when the clock did not move */
{
    return us ? bytes * 1000000ull / us : 0;
}

#define PB_KEY 0x5Au

/* gen [| xform] | sink, `total` bytes in steps of 4096, every stage reading up to 4096 per step, `runs` times.
 * Wall time from the spawn to the pipeline being reaped. Checks count, hash, statuses and cleanup on every run. */
static int pb_flow(const struct pb_env *e, unsigned nst, uint64_t total, unsigned runs, char *det, size_t detn)
{
    uint64_t us[PB_RUNS_MAX];
    int ok = nst >= 2 && nst <= 3 && runs >= 1 && runs <= PB_RUNS_MAX;
    unsigned leaks = 0, bad = 0;
    uint64_t steps = 0, blocks = 0, wakeups = 0, maxres = 0;
    for (unsigned r = 0; ok && r < runs; r++) {
        e->setup();
        struct ck_task_spawn_req st[3] = { e->req(e->gen), e->req(nst == 3 ? e->xform : e->sink), e->req(e->sink) };
        st[0].params[0] = total;
        st[0].params[1] = 4096;
        st[0].nparams = 2;
        st[0].in_chunk = 4096;
        st[1].in_chunk = 4096;
        st[2].in_chunk = 4096;
        if (nst == 3) {
            st[1].params[0] = PB_KEY;
            st[1].nparams = 1;
        }
        struct ck_handle hr, hw, none = { 0, 0 };
        struct ck_task_pipeline pl;
        uint32_t last = 99;
        int run_ok = ck_pipe_create(e->pool, e->sess, &hr, &hw) == OSH_E_OK;
        uint64_t t0 = e->now_us();
        run_ok = run_ok && ck_task_pipeline_spawn(e->tab, st, nst, none, hw, &pl) == OSH_E_OK;
        ck_stream_close(e->pool, e->sess, hw);
        run_ok = run_ok && ck_task_pipeline_wait(e->tab, &pl, CK_TASK_FOREVER, &last) == OSH_E_OK;
        uint64_t t1 = e->now_us();
        uint8_t res[32];
        size_t got = pb_drain_close(e, hr, res, sizeof res);
        run_ok = run_ok && last == 0 && got == 16 && pb_le64(res) == total &&
                 pb_le64(res + 8) == pb_fnv(total, nst == 3 ? PB_KEY : 0);
        for (unsigned k = 0; run_ok && k < nst; k++)
            run_ok = pl.status[k] == 0;
        us[r] = t1 - t0;
        steps = e->tab->stats.steps;
        blocks = e->tab->stats.blocks;
        wakeups = e->tab->stats.wakeups;
        if (e->pool->total.max_resident > maxres)
            maxres = e->pool->total.max_resident;
        leaks += !e->clean();
        bad += !run_ok;
    }
    if (!ok) {
        PB_FMT(det, detn, "bad_arguments=1");
        return 0;
    }
    pb_sort(us, runs);
    uint64_t med = us[runs / 2];
    ok = bad == 0 && leaks == 0 && maxres <= CK_PIPE_CAPACITY;
    PB_FMT(det, detn,
           "stages=%u bytes=%llu chunk=4096 runs=%u wall_us_min=%llu wall_us_median=%llu wall_us_max=%llu "
           "bytes_per_s_median=%llu steps=%llu blocks=%llu wakeups=%llu max_resident=%llu capacity=%u "
           "bad_runs=%u leaked_runs=%u",
           nst, (unsigned long long)total, runs, (unsigned long long)us[0], (unsigned long long)med,
           (unsigned long long)us[runs - 1], (unsigned long long)pb_rate(total, med),
           (unsigned long long)steps, (unsigned long long)blocks, (unsigned long long)wakeups,
           (unsigned long long)maxres, CK_PIPE_CAPACITY, bad, leaks);
    return ok;
}

/* First-byte latency: from the spawn of gen | sink until the sink has taken its first byte (one scheduler turn at a
 * time), then the pipeline is cancelled. */
static int pb_first_byte(const struct pb_env *e, char *det, size_t detn)
{
    e->setup();
    struct ck_task_spawn_req st[2] = { e->req(e->gen), e->req(e->sink) };
    st[0].params[0] = 1 << 20;
    st[0].params[1] = 4096;
    st[0].nparams = 2;
    struct ck_handle none = { 0, 0 };
    struct ck_task_pipeline pl;
    struct ck_task_info si;
    si.bytes_in = 0;
    unsigned turns = 0;
    uint64_t t0 = e->now_us();
    int ok = ck_task_pipeline_spawn(e->tab, st, 2, none, none, &pl) == OSH_E_OK;
    while (ok && turns < 1000 && ck_task_state(e->tab, pl.ids[1], &si) == OSH_E_OK && si.bytes_in == 0) {
        if (!ck_task_run(e->tab, 1))
            break; /* nothing READY: the first byte will never come */
        turns++;
    }
    uint64_t t1 = e->now_us();
    ok = ok && si.bytes_in > 0;
    if (pl.n)
        ck_task_pipeline_cancel(e->tab, &pl);
    int c = e->clean();
    PB_FMT(det, detn, "first_byte_us=%llu turns_to_first_byte=%u first_read_bytes=%llu", (unsigned long long)(t1 - t0),
           turns, (unsigned long long)si.bytes_in);
    return ok && c;
}

/* Cancellation responsiveness: an endless gen | sink runs until bytes move, then ck_task_pipeline_cancel; the time and
 * the stage steps from the cancel until every stage is reaped. Both ignore the interrupt, so both end 130 after the
 * grace (task.h CK_TASK_GRACE_STEPS). */
static int pb_cancel(const struct pb_env *e, char *det, size_t detn)
{
    e->setup();
    struct ck_task_spawn_req st[2] = { e->req(e->gen), e->req(e->sink) };
    st[0].nparams = 1; /* endless */
    struct ck_handle none = { 0, 0 };
    struct ck_task_pipeline pl;
    int ok = ck_task_pipeline_spawn(e->tab, st, 2, none, none, &pl) == OSH_E_OK;
    struct ck_task_info si;
    si.bytes_in = 0;
    for (unsigned i = 0; ok && i < 200 && si.bytes_in < 2 * CK_PIPE_CAPACITY; i++) {
        ck_task_run(e->tab, 8);
        ok = ck_task_state(e->tab, pl.ids[1], &si) == OSH_E_OK;
    }
    uint64_t moved = si.bytes_in, s0 = e->tab->stats.steps, t0 = e->now_us();
    unsigned terminated = ok ? ck_task_pipeline_cancel(e->tab, &pl) : 0;
    uint64_t t1 = e->now_us(), s1 = e->tab->stats.steps;
    ok = ok && moved > 0 && pl.status[0] == 130 && pl.status[1] == 130;
    int c = e->clean();
    PB_FMT(det, detn, "bytes_before_cancel=%llu cancel_us=%llu cancel_steps=%llu terminated=%u status=%u,%u grace_steps=%u",
           (unsigned long long)moved, (unsigned long long)(t1 - t0), (unsigned long long)(s1 - s0), terminated,
           (unsigned)pl.status[0], (unsigned)pl.status[1], (unsigned)CK_TASK_GRACE_STEPS);
    return ok && c;
}

/* 64 KiB capacity and backpressure at the pipe: non-blocking writes of 4096 until WOULD_BLOCK must hold exactly
 * CK_PIPE_CAPACITY bytes; draining returns them in order (binary, NULs included); one read frees room again. */
static int pb_capacity(const struct pb_env *e, char *det, size_t detn)
{
    e->setup();
    struct ck_handle hr, hw;
    int ok = ck_pipe_create(e->pool, e->sess, &hr, &hw) == OSH_E_OK;
    uint8_t blk[4096];
    uint64_t in = 0, out = 0, mism = 0;
    unsigned wb = 0, e1 = OSH_E_OK;
    size_t n = 0;
    while (ok && in < 2u * CK_PIPE_CAPACITY) {
        for (unsigned i = 0; i < sizeof blk; i++)
            blk[i] = pb_pat(in + i);
        e1 = ck_stream_write(e->pool, e->sess, hw, blk, sizeof blk, 0, &n);
        if (e1 != OSH_E_OK) {
            wb = e1 == OSH_E_WOULD_BLOCK;
            break;
        }
        in += n;
        if (n < sizeof blk) { /* short write at the boundary: the next one must block */
            e1 = ck_stream_write(e->pool, e->sess, hw, blk + n, sizeof blk - n, 0, &n);
            wb = e1 == OSH_E_WOULD_BLOCK;
            break;
        }
    }
    uint64_t held = in;
    while (ok && out < held && ck_stream_read(e->pool, e->sess, hr, blk, sizeof blk, 0, &n) == OSH_E_OK && n) {
        for (size_t i = 0; i < n; i++)
            mism += blk[i] != pb_pat(out + i);
        out += n;
    }
    size_t again = 0;
    unsigned e2 = ok ? ck_stream_write(e->pool, e->sess, hw, blk, 1, 0, &again) : OSH_E_IO;
    ck_stream_close(e->pool, e->sess, hw);
    ck_stream_close(e->pool, e->sess, hr);
    ok = ok && held == CK_PIPE_CAPACITY && wb && out == held && mism == 0 && e2 == OSH_E_OK && again == 1 &&
         e->pool->total.max_resident == CK_PIPE_CAPACITY;
    int c = e->clean();
    PB_FMT(det, detn,
           "capacity_bytes=%llu would_block_when_full=%u drained=%llu out_of_order_bytes=%llu write_after_drain=%llu "
           "max_resident=%llu",
           (unsigned long long)held, wb, (unsigned long long)out, (unsigned long long)mism,
           (unsigned long long)again, (unsigned long long)e->pool->total.max_resident);
    return ok && c;
}

/* A stage blocked on its stdin (reader) or on a full stdout (writer); the session revokes the pipe. The blocked stage
 * must be woken and end (stdio revoked: 137, task.h), with nothing left over. side 0 reader, 1 writer. */
static int pb_revoke(const struct pb_env *e, unsigned side, char *det, size_t detn)
{
    e->setup();
    struct ck_handle hr, hw;
    int ok = ck_pipe_create(e->pool, e->sess, &hr, &hw) == OSH_E_OK;
    struct ck_task_spawn_req r = e->req(side ? e->gen : e->sink);
    if (side) {
        r.stdio[1] = hw;
        r.nparams = 1; /* endless */
    } else {
        r.stdio[0] = hr;
    }
    uint64_t id = 0;
    ok = ok && ck_task_spawn(e->tab, &r, &id) == OSH_E_OK;
    struct ck_task_info in;
    in.state = CK_TS_FREE;
    for (unsigned i = 0; ok && i < 400; i++) {
        ck_task_run(e->tab, 4);
        ok = ck_task_state(e->tab, id, &in) == OSH_E_OK;
        if (in.state == CK_TS_BLOCKED)
            break;
    }
    unsigned blocked = ok && in.state == CK_TS_BLOCKED, wait = in.wait;
    uint64_t held = e->pool->total.max_resident, t0 = e->now_us();
    unsigned rv = ok ? ck_pipe_revoke(e->pool, e->sess, side ? hr : hw) : OSH_E_IO;
    uint32_t st = 0;
    ok = ok && rv == OSH_E_OK && ck_task_wait(e->tab, id, CK_TASK_FOREVER, &st) == OSH_E_OK;
    uint64_t t1 = e->now_us();
    ck_stream_close(e->pool, e->sess, hr);
    ck_stream_close(e->pool, e->sess, hw);
    ok = ok && blocked && st == 137;
    int c = e->clean();
    PB_FMT(det, detn, "side=%s blocked=%u wait=%u resident_at_revoke=%llu revoke_to_reaped_us=%llu status=%u",
           side ? "writer" : "reader", blocked, wait, (unsigned long long)(side ? held : 0),
           (unsigned long long)(t1 - t0), (unsigned)st);
    return ok && c;
}

/* Static footprint: everything the native pipeline uses is fixed at build time (no allocation). */
static void pb_footprint(char *det, size_t detn)
{
    PB_FMT(det, detn,
           "pipe_pool_bytes=%llu task_tab_bytes=%llu task_rec_bytes=%llu pipes=%u tasks=%u pipe_capacity=%u allocation=none",
           (unsigned long long)sizeof(struct ck_pipe_pool), (unsigned long long)sizeof(struct ck_task_tab),
           (unsigned long long)sizeof(struct ck_task_rec), CK_PIPE_POOL_PIPES, CK_TASK_MAX, CK_PIPE_CAPACITY);
}

#endif /* AIENOS_CK_PIPE_BENCH_H */
