/* test_task.c -- host tests for core/task.c (aienos-mv#429): pipelines of stages over native pipes, wait / wake /
 * cancel, binding and least-privilege checks, PARTIAL_LAUNCH cleanup, limits, the versioned op table, and the pipe
 * watch used by the scheduler. The stages are C stand-ins that follow stage ABI v1 (task.h) exactly as the Omega
 * fixtures do (tests/fixtures/task/{tgen,txform,tsink}.osc: same parameters, same bytes, same results); they run
 * through the same runner seam the kernel's OSC runner uses. Every scenario ends by checking that no task, pipe or
 * session capability is left over. Host evidence only. */
#include <string.h>
#include <time.h>
#include "ck_test.h"
#include "task.h"
#define PB_FMT snprintf
#include "pipe_bench.h"

/* ---- stand-in stages ---- */
enum kind { K_GEN, K_XFORM, K_SINK, K_NOPROG, K_BADRET, K_BIGOUT, K_CELL, K_BIGEXIT, K_HONOR, K_TRAP, K_OVERRUN,
            K_FAULT, K_REFUSED, K_TICKS, K_COUNT };
struct img {
    enum kind kind;
    unsigned admit_err;
};
static struct img imgs[K_COUNT];
static struct img img_denied = { K_SINK, OSH_E_DENIED }; /* a receipt the runner refuses */
#define IMG_DENIED (CK_TASK_IMAGES - 1u)

static uint64_t clock_now;
#define TRACE_MAX 200000u
static uint8_t trace[TRACE_MAX];
static unsigned ntrace;

static uint8_t pat(uint64_t i) /* tgen.osc byte i */
{
    return i % 5 == 0 ? 0 : (uint8_t)((((i * 2654435761ull) & 0xFFFFFFFFull) >> 13) & 255u);
}

static uint64_t gen(uint64_t *w)
{
    uint64_t total = w[8], per = w[9], sent = w[16];
    if (per == 0 || per > 4096)
        per = 4096;
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

/* returns 2 for "the OSC code would trap BOUNDS here" */
static uint64_t xform(const uint8_t *in, size_t n, uint64_t *w)
{
    for (size_t j = 0; j < n; j++)
        w[64 + j] = in[j] ^ (w[8] & 255);
    uint64_t seen = w[16] + n;
    w[16] = seen;
    w[3] = n;
    if (w[11] && seen >= w[11])
        return 2;
    if (w[9] && seen >= w[9]) {
        w[4] = w[10];
        return 1;
    }
    if ((w[2] & 1) && n == 0) {
        w[4] = 0;
        return 1;
    }
    return 0;
}

static uint64_t sink(const uint8_t *in, size_t n, uint64_t *w)
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
    if ((w[9] && c >= w[9]) || ((w[2] & 1) && n == 0)) {
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

static unsigned h_admit(void *ctx, const void *img)
{
    (void)ctx;
    return ((const struct img *)img)->admit_err;
}

static void h_step(void *ctx, const void *im, const uint8_t *in, size_t in_len, uint64_t *w, uint64_t max_ticks,
                   struct ck_task_step *r)
{
    (void)ctx;
    (void)max_ticks;
    const struct img *g = im;
    clock_now++;
    if (ntrace < TRACE_MAX)
        trace[ntrace++] = (uint8_t)g->kind;
    r->cls = CK_TASK_STEP_RETURNED;
    r->code = 0;
    r->ticks = 0;
    if (w[0] != CK_STG_MAGIC || w[1] != 1) {
        r->value = 200;
        return;
    }
    switch (g->kind) {
    case K_GEN: r->value = gen(w); break;
    case K_XFORM:
        r->value = xform(in, in_len, w);
        if (r->value == 2) {
            r->cls = CK_TASK_STEP_TRAPPED;
            r->code = 3;
        }
        break;
    case K_SINK: r->value = sink(in, in_len, w); break;
    case K_NOPROG: w[3] = 0; r->value = 0; break;
    case K_BADRET: w[3] = 0; r->value = 7; break;
    case K_BIGOUT: w[3] = CK_TASK_OUT_MAX + 1; r->value = 0; break;
    case K_CELL: w[64] = 300; w[3] = 1; r->value = 0; break;
    case K_BIGEXIT: w[4] = 256; r->value = 1; break;
    case K_HONOR:
        if (w[2] & CK_STG_F_INTR) {
            w[4] = 42;
            r->value = 1;
        } else {
            r->value = sink(in, in_len, w);
        }
        break;
    case K_TRAP: r->cls = CK_TASK_STEP_TRAPPED; r->code = 3; break;
    case K_OVERRUN: r->cls = CK_TASK_STEP_UNKNOWN; r->code = CK_TASK_UNK_TICK_OVERRUN; r->ticks = 5; break;
    case K_FAULT: r->cls = CK_TASK_STEP_UNKNOWN; r->code = 1; break;
    case K_REFUSED: r->cls = CK_TASK_STEP_REFUSED; r->code = 64; break;
    case K_TICKS: w[64] = 'x'; w[3] = 1; r->value = 0; r->ticks = 1; break;
    default: r->value = 9; break;
    }
}

static uint64_t h_now(void *ctx)
{
    (void)ctx;
    return clock_now;
}

static const struct ck_task_runner runner = { 0, h_admit, h_step, h_now };

/* ---- the session ---- */
static struct ck_pipe_pool pool;
static struct ck_cap_table sess;
static struct ck_task_tab tab;
static struct ck_handle spawn_cap;
static const uint8_t P[32] = { 0x41, 0x49, 0x45, 0x4e, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                               13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28 };

static void setup(void)
{
    ck_pipe_pool_init(&pool, 0);
    ck_cap_init(&sess, 0x5E55u, CK_CAP_SLOTS);
    ck_cap_insert(&sess, CK_TASK_SPAWN_RESOURCE, CK_R_WRITE, &spawn_cap);
    ck_task_init(&tab, &pool, &sess, P, &runner);
    for (unsigned k = 0; k < K_COUNT && k < IMG_DENIED; k++) {
        imgs[k].kind = (enum kind)k;
        imgs[k].admit_err = OSH_E_OK;
        ck_task_image_set(&tab, k, &imgs[k]);
    }
    ck_task_image_set(&tab, IMG_DENIED, &img_denied);
    ntrace = 0;
}

static unsigned live_caps(const struct ck_cap_table *t)
{
    unsigned n = 0;
    for (unsigned i = 0; i < t->n; i++)
        n += t->slots[i].live && !t->slots[i].tombstone;
    return n;
}

/* nothing left over: no task slot, no pipe, the session holds only its spawn capability, every task table empty */
static int clean(void)
{
    unsigned task_caps = 0;
    for (unsigned i = 0; i < CK_TASK_MAX; i++)
        task_caps += live_caps(&tab.tasks[i].caps);
    return ck_task_tab_empty(&tab) && ck_pipe_pool_empty(&pool) && live_caps(&sess) == 1 && task_caps == 0 &&
           tab.stats.blocked_runs == 0;
}

static struct ck_task_binding bind(void)
{
    struct ck_task_binding b;
    memset(&b, 0, sizeof b);
    memcpy(b.principal_id, P, 32);
    b.domain = CK_TASK_DOMAIN_KERNEL;
    b.cap_index = spawn_cap.index;
    b.cap_generation = spawn_cap.generation;
    b.resource_class = CK_TASK_RC_TASK;
    b.operation = CK_TASK_OP_SPAWN;
    return b;
}

static struct ck_task_spawn_req req(unsigned image)
{
    struct ck_task_spawn_req r;
    memset(&r, 0, sizeof r);
    r.image = image;
    r.binding = bind();
    return r;
}

static uint64_t fnv(uint64_t total, uint8_t key)
{
    uint64_t h = 14695981039346656037ull;
    for (uint64_t i = 0; i < total; i++)
        h = (h ^ (uint8_t)(pat(i) ^ key)) * 1099511628211ull;
    return h;
}

/* drain the harness end into buf, close it; returns bytes read */
static size_t drain_close(struct ck_handle hr, uint8_t *buf, size_t max)
{
    size_t got = 0, n = 0;
    while (got < max && ck_stream_read(&pool, &sess, hr, buf + got, max - got, 0, &n) == OSH_E_OK && n)
        got += n;
    ck_stream_close(&pool, &sess, hr);
    return got;
}

static uint64_t le64(const uint8_t *b)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | b[i];
    return v;
}

/* ---- 1. two stages, slow consumer: interleaved progress, backpressure, exact bytes ---- */
static void two_stage(void)
{
    setup();
    const uint64_t total = 163840; /* 2.5 pipes */
    struct ck_task_spawn_req st[2] = { req(K_GEN), req(K_SINK) };
    st[0].params[0] = total;
    st[0].params[1] = 4096;
    st[0].nparams = 2;
    st[1].in_chunk = 512; /* the slow side: an eighth of what the producer emits per step */
    struct ck_handle hr, hw, none = { 0, 0 };
    CHECK(ck_pipe_create(&pool, &sess, &hr, &hw) == OSH_E_OK);
    struct ck_task_pipeline pl;
    CHECK(ck_task_pipeline_spawn(&tab, st, 2, none, hw, &pl) == OSH_E_OK);
    ck_stream_close(&pool, &sess, hw);
    struct ck_task_info gi, si;
    uint64_t gid = pl.ids[0], sid = pl.ids[1];
    /* run a while: both alive, bytes moving, the producer held back by a full pipe */
    ck_task_run(&tab, 200);
    CHECK(ck_task_state(&tab, gid, &gi) == OSH_E_OK && ck_task_state(&tab, sid, &si) == OSH_E_OK);
    CHECK(gi.state != CK_TS_EXITED && si.state != CK_TS_EXITED);
    CHECK(si.bytes_in > 0 && gi.bytes_out > si.bytes_in);
    CHECK(gi.blocks > 0); /* backpressure: the producer waited for space */
    uint32_t last = 99;
    CHECK(ck_task_pipeline_wait(&tab, &pl, CK_TASK_FOREVER, &last) == OSH_E_OK);
    CHECK(last == 0 && pl.status[0] == 0 && pl.status[1] == 0);
    uint8_t res[64];
    size_t got = drain_close(hr, res, sizeof res);
    CHECK(got == 16 && le64(res) == total && le64(res + 8) == fnv(total, 0));
    /* interleaving: the sink stepped before the generator's last step, and they alternated many times */
    unsigned last_gen = 0, first_sink = TRACE_MAX, switches = 0;
    for (unsigned i = 0; i < ntrace; i++) {
        if (trace[i] == K_GEN)
            last_gen = i;
        if (trace[i] == K_SINK && first_sink == TRACE_MAX)
            first_sink = i;
        if (i && trace[i] != trace[i - 1])
            switches++;
    }
    CHECK(first_sink < last_gen && switches >= 20);
    CHECK(pool.total.max_resident <= CK_PIPE_CAPACITY && pool.total.max_resident > 0);
    CHECK(tab.stats.wakeups > 0 && tab.stats.blocks > 0);
    printf("two stages: bytes=%llu steps=%llu switches=%u blocks=%llu wakeups=%llu max_resident=%llu idle=%llu\n",
           (unsigned long long)total, (unsigned long long)tab.stats.steps, switches,
           (unsigned long long)tab.stats.blocks, (unsigned long long)tab.stats.wakeups,
           (unsigned long long)pool.total.max_resident, (unsigned long long)tab.stats.idle_returns);
    CHECK(clean());
}

/* ---- 2. three stages ---- */
static void three_stage(void)
{
    setup();
    const uint64_t total = 100003;
    struct ck_task_spawn_req st[3] = { req(K_GEN), req(K_XFORM), req(K_SINK) };
    st[0].params[0] = total;
    st[0].params[1] = 3000;
    st[0].nparams = 2;
    st[1].params[0] = 0x5A;
    st[1].nparams = 1;
    st[1].in_chunk = 1024;
    st[2].in_chunk = 700;
    struct ck_handle hr, hw, none = { 0, 0 };
    CHECK(ck_pipe_create(&pool, &sess, &hr, &hw) == OSH_E_OK);
    struct ck_task_pipeline pl;
    CHECK(ck_task_pipeline_spawn(&tab, st, 3, none, hw, &pl) == OSH_E_OK);
    ck_stream_close(&pool, &sess, hw);
    uint32_t last = 99;
    CHECK(ck_task_pipeline_wait(&tab, &pl, CK_TASK_FOREVER, &last) == OSH_E_OK);
    CHECK(last == 0 && pl.status[0] == 0 && pl.status[1] == 0);
    uint8_t res[64];
    CHECK(drain_close(hr, res, sizeof res) == 16 && le64(res) == total && le64(res + 8) == fnv(total, 0x5A));
    CHECK(clean());
}

/* ---- 3. nonzero exit of the last stage while the producer never ends (consumer exits early) ---- */
static void early_exit(void)
{
    setup();
    struct ck_task_spawn_req st[2] = { req(K_GEN), req(K_XFORM) };
    st[0].nparams = 2; /* total 0: never ends */
    st[0].params[1] = 4096;
    st[1].params[1] = 10000; /* stop after 10000 bytes ... */
    st[1].params[2] = 3;     /* ... with status 3 */
    st[1].nparams = 3;
    struct ck_handle none = { 0, 0 };
    struct ck_task_pipeline pl;
    CHECK(ck_task_pipeline_spawn(&tab, st, 2, none, none, &pl) == OSH_E_OK);
    uint32_t last = 99;
    CHECK(ck_task_pipeline_wait(&tab, &pl, CK_TASK_FOREVER, &last) == OSH_E_OK);
    CHECK(last == 3 && pl.status[1] == 3);
    CHECK(pl.status[0] == 141); /* the endless producer is stopped by BROKEN_PIPE, not left running */
    CHECK(clean());
}

/* ---- 4. a stage crashes (OSC BOUNDS trap) mid-stream ---- */
static void crash(void)
{
    setup();
    struct ck_task_spawn_req st[3] = { req(K_GEN), req(K_XFORM), req(K_SINK) };
    st[0].params[0] = 50000;
    st[0].nparams = 1;
    st[1].params[3] = 5000; /* trap after 5000 bytes */
    st[1].nparams = 4;
    struct ck_handle hr, hw, none = { 0, 0 };
    CHECK(ck_pipe_create(&pool, &sess, &hr, &hw) == OSH_E_OK);
    struct ck_task_pipeline pl;
    CHECK(ck_task_pipeline_spawn(&tab, st, 3, none, hw, &pl) == OSH_E_OK);
    ck_stream_close(&pool, &sess, hw);
    struct ck_task_info xi;
    uint64_t xid = pl.ids[1];
    uint32_t last = 99;
    ck_task_run(&tab, 1000);
    CHECK(ck_task_state(&tab, xid, &xi) == OSH_E_OK && xi.end == CK_TE_TRAPPED && xi.status == 134);
    CHECK(ck_task_pipeline_wait(&tab, &pl, CK_TASK_FOREVER, &last) == OSH_E_OK);
    CHECK(pl.status[1] == 134 && pl.status[0] == 141 && last == 0);
    uint8_t res[64];
    CHECK(drain_close(hr, res, sizeof res) == 16 && le64(res) < 50000);
    CHECK(clean());
}

/* ---- 5. cancellation: deadline, interrupt with grace, terminate ---- */
static void cancel(void)
{
    setup();
    struct ck_task_spawn_req st[2] = { req(K_GEN), req(K_SINK) };
    st[0].nparams = 1; /* endless */
    struct ck_handle none = { 0, 0 };
    struct ck_task_pipeline pl;
    CHECK(ck_task_pipeline_spawn(&tab, st, 2, none, none, &pl) == OSH_E_OK);
    uint32_t last = 99;
    uint64_t t0 = clock_now;
    CHECK(ck_task_pipeline_wait(&tab, &pl, 0, &last) == OSH_E_WOULD_BLOCK && clock_now == t0); /* deadline 0 runs nothing */
    CHECK(ck_task_pipeline_wait(&tab, &pl, 300, &last) == OSH_E_WOULD_BLOCK);
    CHECK(clock_now - t0 >= 300 && clock_now - t0 <= 301); /* the deadline is honoured to one step */
    CHECK(ck_task_pipeline_cancel(&tab, &pl) == 0);          /* both ended within the grace ... */
    CHECK(pl.status[0] == 130 && pl.status[1] == 130);      /* ... of the interrupt they ignored */
    CHECK(clean());

    /* a stage that honours the interrupt ends with its own status; a terminate is 143 at once */
    setup();
    struct ck_handle hr, hw;
    CHECK(ck_pipe_create(&pool, &sess, &hr, &hw) == OSH_E_OK);
    struct ck_task_spawn_req a = req(K_HONOR), b = req(K_SINK);
    a.stdio[0] = hr;
    b.stdio[0] = hr;
    uint64_t ia = 0, ib = 0;
    CHECK(ck_task_spawn(&tab, &a, &ia) == OSH_E_OK && ck_task_spawn(&tab, &b, &ib) == OSH_E_OK);
    ck_task_run(&tab, 10);
    struct ck_task_info in;
    CHECK(ck_task_state(&tab, ia, &in) == OSH_E_OK && in.state == CK_TS_BLOCKED && in.wait == CK_TW_READABLE);
    CHECK(ck_task_cancel(&tab, ia, CK_TASK_CANCEL_INTERRUPT) == OSH_E_OK);
    CHECK(ck_task_state(&tab, ia, &in) == OSH_E_OK && in.state == CK_TS_READY && in.wakes == 1); /* woken by cancel */
    CHECK(ck_task_cancel(&tab, ib, CK_TASK_CANCEL_TERMINATE) == OSH_E_OK);
    CHECK(ck_task_state(&tab, ib, &in) == OSH_E_OK && in.state == CK_TS_EXITED && in.status == 143);
    CHECK(ck_task_cancel(&tab, ib, 3) == OSH_E_INVALID_ARG);
    CHECK(ck_task_cancel(&tab, ib, CK_TASK_CANCEL_TERMINATE) == OSH_E_OK); /* ended: no change */
    uint32_t s = 0;
    CHECK(ck_task_wait(&tab, ia, CK_TASK_FOREVER, &s) == OSH_E_OK && s == 42);
    CHECK(ck_task_wait(&tab, ib, CK_TASK_FOREVER, &s) == OSH_E_OK && s == 143);
    ck_stream_close(&pool, &sess, hr);
    ck_stream_close(&pool, &sess, hw);
    CHECK(clean());

    /* aienos-mv#432 (T1): both stages interrupted, the consumer ends its grace first and closes its end; the producer
     * still in its grace then writes into a pipe with no reader. Its output is dropped and it ends by the interrupt
     * (130), never by BROKEN_PIPE (141): the cancel outcome does not depend on the round-robin order. */
    setup();
    CHECK(ck_pipe_create(&pool, &sess, &hr, &hw) == OSH_E_OK);
    struct ck_task_spawn_req c = req(K_SINK), g = req(K_GEN);
    c.stdio[0] = hr;
    g.stdio[1] = hw;
    g.nparams = 1; /* endless */
    uint64_t ic = 0, ig = 0;
    CHECK(ck_task_spawn(&tab, &c, &ic) == OSH_E_OK && ck_task_spawn(&tab, &g, &ig) == OSH_E_OK);
    ck_stream_close(&pool, &sess, hr);
    ck_stream_close(&pool, &sess, hw);
    ck_task_run(&tab, 6);
    CHECK(ck_task_cancel(&tab, ic, CK_TASK_CANCEL_INTERRUPT) == OSH_E_OK);
    CHECK(ck_task_cancel(&tab, ig, CK_TASK_CANCEL_INTERRUPT) == OSH_E_OK);
    ck_task_run(&tab, 2 * CK_TASK_GRACE_STEPS + 2);
    uint32_t sc = 0, sg = 0;
    CHECK(ck_task_wait(&tab, ic, CK_TASK_FOREVER, &sc) == OSH_E_OK && sc == 130);
    CHECK(ck_task_wait(&tab, ig, CK_TASK_FOREVER, &sg) == OSH_E_OK && sg == 130);
    printf("cancel grace order: consumer=%u producer=%u (130 = interrupted, 141 would be the race)\n", sc, sg);
    CHECK(clean());
}

/* ---- 6. partial launch ---- */
static void partial(void)
{
    setup();
    struct ck_task_spawn_req st[3] = { req(K_GEN), req(K_XFORM), req(IMG_DENIED) };
    st[0].nparams = 1;
    struct ck_handle none = { 0, 0 };
    struct ck_task_pipeline pl;
    CHECK(ck_task_pipeline_spawn(&tab, st, 3, none, none, &pl) == OSH_E_PARTIAL_LAUNCH);
    CHECK(pl.started == 2 && pl.cause == OSH_E_DENIED && tab.stats.partial_launches == 1);
    CHECK(tab.stats.steps == 0); /* nothing ran: spawn never runs a step */
    CHECK(clean());
    /* the first stage refused: its own error, nothing allocated */
    setup();
    struct ck_task_spawn_req s1[2] = { req(IMG_DENIED), req(K_SINK) };
    CHECK(ck_task_pipeline_spawn(&tab, s1, 2, none, none, &pl) == OSH_E_DENIED && pl.started == 0);
    CHECK(clean());
    /* a later stage over the slot limit: PARTIAL_LAUNCH with cause LIMIT */
    setup();
    uint64_t hold[CK_TASK_MAX];
    struct ck_task_spawn_req g = req(K_SINK);
    unsigned held = 0;
    while (held < CK_TASK_MAX - 2 && ck_task_spawn(&tab, &g, &hold[held]) == OSH_E_OK)
        held++;
    struct ck_task_spawn_req s3[3] = { req(K_GEN), req(K_XFORM), req(K_SINK) };
    CHECK(ck_task_pipeline_spawn(&tab, s3, 3, none, none, &pl) == OSH_E_PARTIAL_LAUNCH && pl.cause == OSH_E_LIMIT);
    for (unsigned i = 0; i < held; i++) {
        uint32_t s = 0;
        CHECK(ck_task_wait(&tab, hold[i], CK_TASK_FOREVER, &s) == OSH_E_OK && s == 0); /* no stdin: EOF, sink done */
    }
    CHECK(clean());
    /* a pipe pool or session table that runs out also gives PARTIAL_LAUNCH, never a silent cut */
    setup();
    struct ck_task_spawn_req s6[6] = { req(K_GEN), req(K_XFORM), req(K_XFORM), req(K_XFORM), req(K_XFORM), req(K_SINK) };
    CHECK(ck_task_pipeline_spawn(&tab, s6, 6, none, none, &pl) == OSH_E_LIMIT);
    CHECK(ck_task_pipeline_spawn(&tab, s6, 0, none, none, &pl) == OSH_E_INVALID_ARG);
    CHECK(clean());
}

/* ---- 7. binding (section 9) ---- */
static void binding(void)
{
    setup();
    uint64_t id = 7;
    struct ck_task_spawn_req r = req(K_SINK);
    r.binding.domain = CK_TASK_DOMAIN_HOSTED;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_CAP_DOMAIN_MISMATCH && id == 0);
    r = req(K_SINK);
    r.binding.cap_generation = 0x100000000ull | spawn_cap.generation;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_CAP_GEN_NARROW);
    r = req(K_SINK);
    r.binding.principal_id[31] ^= 1;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_DENIED);
    r = req(K_SINK);
    r.binding.resource_class = 1;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_DENIED);
    r = req(K_SINK);
    r.binding.operation = CK_TASK_OP_WAIT;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_DENIED);
    r = req(K_SINK);
    r.binding.cap_generation = spawn_cap.generation + 1;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_STALE);
    r = req(K_SINK);
    r.binding.cap_generation = 0;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_INVALID_ARG);
    /* a capability without WRITE, and one on another resource */
    struct ck_handle ro, other;
    CHECK(ck_cap_derive(&sess, spawn_cap, CK_R_READ, &ro) == CK_CAP_OK || ck_cap_insert(&sess, CK_TASK_SPAWN_RESOURCE, CK_R_READ, &ro) == CK_CAP_OK);
    r = req(K_SINK);
    r.binding.cap_index = ro.index;
    r.binding.cap_generation = ro.generation;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_DENIED);
    ck_cap_remove(&sess, ro);
    CHECK(ck_cap_insert(&sess, 0x12345678u, CK_R_WRITE, &other) == CK_CAP_OK);
    r = req(K_SINK);
    r.binding.cap_index = other.index;
    r.binding.cap_generation = other.generation;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_DENIED);
    ck_cap_remove(&sess, other);
    /* the binding is checked at the moment of the spawn: a removed (revoked) capability no longer spawns */
    r = req(K_SINK);
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_OK);
    uint32_t s = 0;
    CHECK(ck_task_wait(&tab, id, CK_TASK_FOREVER, &s) == OSH_E_OK && s == 0);
    struct ck_handle old = spawn_cap;
    ck_cap_remove(&sess, spawn_cap);
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_STALE && id == 0);
    ck_cap_insert(&sess, CK_TASK_SPAWN_RESOURCE, CK_R_WRITE, &spawn_cap);
    CHECK(spawn_cap.index != old.index || spawn_cap.generation != old.generation);
    /* the image must be resolved and admitted (the kernel's admission verifies the receipt) */
    r = req(5);
    r.image = CK_TASK_IMAGES;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_NOT_FOUND);
    r = req(IMG_DENIED);
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_DENIED);
    r = req(K_SINK);
    r.nparams = CK_TASK_PARAMS + 1;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_INVALID_ARG);
    r = req(K_SINK);
    r.in_chunk = CK_TASK_IN_MAX + 1;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_INVALID_ARG);
    CHECK(tab.stats.steps == 1); /* only the one admitted spawn ever ran */
    CHECK(clean());
}

/* ---- 8. least privilege ---- */
static void least_privilege(void)
{
    setup();
    struct ck_handle hr, hw;
    CHECK(ck_pipe_create(&pool, &sess, &hr, &hw) == OSH_E_OK);
    unsigned rights = 0;
    CHECK(ck_cap_rights(&sess, hr, &rights) == CK_CAP_OK && (rights & CK_R_REVOKE) && (rights & CK_R_GRANT));
    struct ck_task_spawn_req r = req(K_XFORM);
    uint64_t id = 0;
    r.stdio[0] = hw; /* a write end as stdin */
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_DENIED);
    r.stdio[0] = hr;
    r.stdio[1] = hr; /* a read end as stdout */
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_DENIED);
    r.stdio[1] = hw;
    r.stdio[2] = hw;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_OK);
    const struct ck_task_rec *t = &tab.tasks[(id >> 32) & 0xFF];
    unsigned ri = 0, ro = 0, re = 0;
    CHECK(ck_cap_rights(&t->caps, t->stdio[0], &ri) == CK_CAP_OK && ri == CK_R_READ);
    CHECK(ck_cap_rights(&t->caps, t->stdio[1], &ro) == CK_CAP_OK && ro == CK_R_WRITE);
    CHECK(ck_cap_rights(&t->caps, t->stdio[2], &re) == CK_CAP_OK && re == CK_R_WRITE);
    CHECK(live_caps(&t->caps) == 3);
    /* the task's ends cannot revoke the pipe or hand themselves on */
    struct ck_handle x;
    CHECK(ck_pipe_revoke(&pool, &t->caps, t->stdio[0]) == OSH_E_DENIED);
    CHECK(ck_stream_dup(&pool, (struct ck_cap_table *)&t->caps, t->stdio[1], &sess, CK_R_WRITE, &x) == OSH_E_DENIED);
    /* a stale end is refused at spawn */
    struct ck_handle stale = hr;
    stale.generation++;
    r.stdio[0] = stale;
    uint64_t id2 = 0;
    CHECK(ck_task_spawn(&tab, &r, &id2) == OSH_E_STALE);
    CHECK(ck_task_cancel(&tab, id, CK_TASK_CANCEL_TERMINATE) == OSH_E_OK);
    uint32_t s = 0;
    CHECK(ck_task_wait(&tab, id, 0, &s) == OSH_E_OK && s == 143);
    ck_stream_close(&pool, &sess, hr);
    ck_stream_close(&pool, &sess, hw);
    CHECK(clean());
}

/* ---- 9. wake on readable, writable, closed, revoked, cancelled; never polled while waiting ---- */
static void wakes(void)
{
    setup();
    struct ck_handle hr, hw;
    CHECK(ck_pipe_create(&pool, &sess, &hr, &hw) == OSH_E_OK);
    struct ck_task_spawn_req r = req(K_SINK);
    r.stdio[0] = hr;
    uint64_t id = 0;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_OK);
    ck_stream_close(&pool, &sess, hr);
    struct ck_task_info in;
    uint32_t s = 0;
    CHECK(ck_task_wait(&tab, id, CK_TASK_FOREVER, &s) == OSH_E_WOULD_BLOCK); /* returns: only we can feed it */
    CHECK(ck_task_state(&tab, id, &in) == OSH_E_OK && in.state == CK_TS_BLOCKED && in.wait == CK_TW_READABLE);
    uint64_t steps = tab.stats.steps, turns = tab.stats.turns;
    CHECK(ck_task_run(&tab, 1000) == 0 && tab.stats.steps == steps && tab.stats.turns == turns); /* no polling */
    size_t n = 0;
    uint8_t b[100];
    memset(b, 7, sizeof b);
    CHECK(ck_stream_write(&pool, &sess, hw, b, sizeof b, 0, &n) == OSH_E_OK && n == 100);
    CHECK(ck_task_state(&tab, id, &in) == OSH_E_OK && in.state == CK_TS_READY && in.wakes == 1); /* readable */
    CHECK(ck_task_wait(&tab, id, CK_TASK_FOREVER, &s) == OSH_E_WOULD_BLOCK);
    CHECK(ck_task_state(&tab, id, &in) == OSH_E_OK && in.bytes_in == 100 && in.state == CK_TS_BLOCKED);
    ck_stream_close(&pool, &sess, hw); /* closed: woken, sees EOF, ends 0 */
    CHECK(ck_task_state(&tab, id, &in) == OSH_E_OK && in.state == CK_TS_READY && in.wakes == 2);
    CHECK(ck_task_wait(&tab, id, CK_TASK_FOREVER, &s) == OSH_E_OK && s == 0);
    CHECK(clean());

    /* writable: a producer into a pipe nobody drains blocks once it is full, and a read wakes it */
    setup();
    CHECK(ck_pipe_create(&pool, &sess, &hr, &hw) == OSH_E_OK);
    r = req(K_GEN);
    r.stdio[1] = hw;
    r.nparams = 1; /* endless */
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_OK);
    ck_stream_close(&pool, &sess, hw);
    CHECK(ck_task_wait(&tab, id, CK_TASK_FOREVER, &s) == OSH_E_WOULD_BLOCK);
    CHECK(ck_task_state(&tab, id, &in) == OSH_E_OK && in.state == CK_TS_BLOCKED && in.wait == CK_TW_WRITABLE);
    CHECK(in.bytes_out == CK_PIPE_CAPACITY); /* bounded: exactly one pipe's worth in flight, the rest held back */
    uint8_t big[8192];
    CHECK(ck_stream_read(&pool, &sess, hr, big, sizeof big, 0, &n) == OSH_E_OK && n == sizeof big);
    CHECK(ck_task_state(&tab, id, &in) == OSH_E_OK && in.state == CK_TS_READY);
    /* revoked: the pipe is killed under the blocked producer; it is woken and ends 137 REVOKED */
    CHECK(ck_task_wait(&tab, id, CK_TASK_FOREVER, &s) == OSH_E_WOULD_BLOCK);
    CHECK(ck_pipe_revoke(&pool, &sess, hr) == OSH_E_OK);
    CHECK(ck_task_state(&tab, id, &in) == OSH_E_OK && in.state == CK_TS_READY);
    CHECK(ck_task_wait(&tab, id, CK_TASK_FOREVER, &s) == OSH_E_OK && s == 137);
    ck_stream_close(&pool, &sess, hr);
    CHECK(clean());

    /* a pipe-level cancel of the task's own end interrupts its call once; it is retried, nothing is lost */
    setup();
    CHECK(ck_pipe_create(&pool, &sess, &hr, &hw) == OSH_E_OK);
    r = req(K_SINK);
    r.stdio[0] = hr;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_OK);
    CHECK(ck_pipe_cancel(&pool, &sess, hr) == OSH_E_OK); /* pending on the read end: the task's next read */
    ck_stream_close(&pool, &sess, hr);
    CHECK(ck_task_run(&tab, 1) == 1);
    CHECK(ck_task_state(&tab, id, &in) == OSH_E_OK && in.state == CK_TS_READY && in.steps == 0);
    CHECK(ck_stream_write(&pool, &sess, hw, b, 10, 0, &n) == OSH_E_OK);
    ck_stream_close(&pool, &sess, hw);
    CHECK(ck_task_wait(&tab, id, CK_TASK_FOREVER, &s) == OSH_E_OK && s == 0);
    CHECK(clean());
}

/* ---- 10. stage outcomes, protocol violations and limits ---- */
static uint32_t solo(unsigned image, uint64_t max_steps, uint64_t max_task_ticks, unsigned *end)
{
    setup();
    struct ck_task_spawn_req r = req(image);
    r.max_steps = max_steps;
    r.max_task_ticks = max_task_ticks;
    uint64_t id = 0;
    uint32_t s = 999;
    struct ck_task_info in;
    if (ck_task_spawn(&tab, &r, &id) != OSH_E_OK)
        return 1000;
    unsigned e = ck_task_wait(&tab, id, 100000, &s);
    *end = 0;
    if (e != OSH_E_OK) {
        ck_task_state(&tab, id, &in);
        ck_task_cancel(&tab, id, CK_TASK_CANCEL_TERMINATE);
        ck_task_wait(&tab, id, 0, &s);
        return 1001;
    }
    *end = tab.stats.reaped ? 1 : 0;
    return s;
}

static void outcomes(void)
{
    unsigned end = 0;
    CHECK(solo(K_NOPROG, 0, 0, &end) == 139 && clean()); /* no progress is refused, not stepped forever */
    CHECK(solo(K_BADRET, 0, 0, &end) == 139 && clean());
    CHECK(solo(K_BIGOUT, 0, 0, &end) == 139 && clean());
    CHECK(solo(K_CELL, 0, 0, &end) == 139 && clean());
    CHECK(solo(K_BIGEXIT, 0, 0, &end) == 139 && clean());
    CHECK(solo(K_TRAP, 0, 0, &end) == 134 && clean());
    CHECK(solo(K_OVERRUN, 0, 0, &end) == 137 && clean());
    CHECK(solo(K_FAULT, 0, 0, &end) == 139 && clean());
    CHECK(solo(K_REFUSED, 0, 0, &end) == 126 && clean());
    CHECK(solo(K_TICKS, 50, 0, &end) == 137 && tab.stats.steps == 50 && clean());   /* step limit */
    CHECK(solo(K_TICKS, 0, 30, &end) == 137 && tab.stats.steps == 30 && clean());   /* cumulative tick limit */
}

/* ---- 11. ids, epochs, slots ---- */
static void ids(void)
{
    setup();
    struct ck_task_spawn_req r = req(K_SINK);
    uint64_t id = 0, id2 = 0;
    uint32_t s = 0;
    struct ck_task_info in;
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_OK);
    CHECK(ck_task_wait(&tab, id, CK_TASK_FOREVER, &s) == OSH_E_OK);
    CHECK(ck_task_wait(&tab, id, CK_TASK_FOREVER, &s) == OSH_E_STALE); /* reaped */
    CHECK(ck_task_state(&tab, id, &in) == OSH_E_STALE && ck_task_cancel(&tab, id, 1) == OSH_E_STALE);
    CHECK(ck_task_spawn(&tab, &r, &id2) == OSH_E_OK && (id2 >> 32) == (id >> 32) && id2 != id); /* same slot */
    CHECK(ck_task_state(&tab, id, &in) == OSH_E_STALE && ck_task_state(&tab, id2, &in) == OSH_E_OK);
    CHECK(ck_task_state(&tab, id2 ^ (1ull << 60), &in) == OSH_E_STALE); /* forged tag */
    CHECK(ck_task_state(&tab, id2 | (1ull << 45), &in) == OSH_E_STALE);
    CHECK(ck_task_wait(&tab, id2, CK_TASK_FOREVER, &s) == OSH_E_OK);
    uint64_t all[CK_TASK_MAX];
    for (unsigned i = 0; i < CK_TASK_MAX; i++)
        CHECK(ck_task_spawn(&tab, &r, &all[i]) == OSH_E_OK);
    CHECK(ck_task_spawn(&tab, &r, &id) == OSH_E_LIMIT && id == 0); /* section 8.5: LIMIT, never a silent drop */
    for (unsigned i = 0; i < CK_TASK_MAX; i++)
        CHECK(ck_task_wait(&tab, all[i], CK_TASK_FOREVER, &s) == OSH_E_OK && s == 0);
    CHECK(clean());
}

/* ---- 12. versioned op table ---- */
static void dispatch(void)
{
    setup();
    uint64_t f[32];
    struct ck_task_ret ret;
    memset(f, 0, sizeof f);
    f[0] = CK_PIPE_FRAME_MAGIC;
    f[1] = CK_TASK_ABI_VERSION;
    f[2] = CK_TASK_OP_SPAWN;
    uint64_t *a = f + 4;
    a[0] = K_SINK;
    for (unsigned i = 0; i < 32; i++)
        a[8 + i / 8] |= (uint64_t)P[i] << (8 * (i % 8));
    a[12] = CK_TASK_DOMAIN_KERNEL;
    a[13] = spawn_cap.index;
    a[14] = spawn_cap.generation;
    a[15] = CK_TASK_RC_TASK;
    a[16] = CK_TASK_OP_SPAWN;
    CHECK(ck_task_dispatch(&tab, f, 31, &ret) == OSH_E_OK && ret.error == OSH_E_OK);
    uint64_t id = ret.value;
    uint64_t g[8] = { CK_PIPE_FRAME_MAGIC, CK_TASK_ABI_VERSION, CK_TASK_OP_STATE, 0, id };
    CHECK(ck_task_dispatch(&tab, g, 5, &ret) == OSH_E_OK && (ret.value & 0xFF) == CK_TS_READY);
    g[2] = CK_TASK_OP_WAIT;
    g[5] = CK_TASK_FOREVER;
    CHECK(ck_task_dispatch(&tab, g, 6, &ret) == OSH_E_OK && ret.value == 0);
    g[2] = CK_TASK_OP_CANCEL;
    g[5] = CK_TASK_CANCEL_TERMINATE;
    CHECK(ck_task_dispatch(&tab, g, 6, &ret) == OSH_E_STALE); /* reaped by the wait */
    /* refusals, all before any effect */
    uint64_t steps = tab.stats.spawned;
    f[0] ^= 1;
    CHECK(ck_task_dispatch(&tab, f, 31, &ret) == OSH_E_INVALID_ARG);
    f[0] ^= 1;
    f[3] = 1;
    CHECK(ck_task_dispatch(&tab, f, 31, &ret) == OSH_E_INVALID_ARG);
    f[3] = 0;
    f[1] = 2;
    CHECK(ck_task_dispatch(&tab, f, 31, &ret) == OSH_E_NOT_SUPPORTED);
    f[1] = CK_TASK_ABI_VERSION;
    f[2] = CK_PIPE_OP_READ;
    CHECK(ck_task_dispatch(&tab, f, 31, &ret) == OSH_E_NOT_SUPPORTED);
    f[2] = 99;
    CHECK(ck_task_dispatch(&tab, f, 31, &ret) == OSH_E_NOT_SUPPORTED);
    f[2] = CK_TASK_OP_SPAWN;
    CHECK(ck_task_dispatch(&tab, f, 30, &ret) == OSH_E_INVALID_ARG);
    CHECK(ck_task_dispatch(&tab, f, 3, &ret) == OSH_E_INVALID_ARG);
    a[12] = 0x100;
    CHECK(ck_task_dispatch(&tab, f, 31, &ret) == OSH_E_INVALID_ARG);
    a[12] = CK_TASK_DOMAIN_HOSTED;
    CHECK(ck_task_dispatch(&tab, f, 31, &ret) == OSH_E_CAP_DOMAIN_MISMATCH);
    a[12] = CK_TASK_DOMAIN_KERNEL;
    a[1] = 5; /* a stdin handle with generation 0 */
    CHECK(ck_task_dispatch(&tab, f, 31, &ret) == OSH_E_INVALID_ARG);
    a[1] = 0;
    a[17] = CK_TASK_PARAMS + 1;
    CHECK(ck_task_dispatch(&tab, f, 31, &ret) == OSH_E_INVALID_ARG);
    CHECK(tab.stats.spawned == steps);
    CHECK(clean());
}

/* ---- 13. pipe watch (pipe.c addition) ---- */
static unsigned hook_calls[CK_PIPE_POOL_PIPES][2];
static void count_wake(void *ctx, unsigned p, unsigned e)
{
    (void)ctx;
    hook_calls[p][e]++;
}

static void watch(void)
{
    static const struct ck_pipe_hooks hk = { 0, 0, count_wake };
    ck_pipe_pool_init(&pool, &hk);
    ck_cap_init(&sess, 0x5E55u, CK_CAP_SLOTS);
    memset(hook_calls, 0, sizeof hook_calls);
    struct ck_handle r, w;
    size_t n = 0;
    uint8_t b[4] = { 1, 2, 3, 4 };
    CHECK(ck_pipe_create(&pool, &sess, &r, &w) == OSH_E_OK);
    CHECK(ck_stream_write(&pool, &sess, w, b, 4, 0, &n) == OSH_E_OK && hook_calls[0][0] == 0); /* nobody watching */
    CHECK(ck_pipe_watch(&pool, &sess, r) == OSH_E_OK);
    CHECK(ck_stream_write(&pool, &sess, w, b, 4, 0, &n) == OSH_E_OK && hook_calls[0][0] == 1);
    CHECK(ck_pipe_unwatch(&pool, &sess, r) == OSH_E_OK);
    CHECK(ck_stream_write(&pool, &sess, w, b, 4, 0, &n) == OSH_E_OK && hook_calls[0][0] == 1);
    CHECK(ck_pipe_watch(&pool, &sess, w) == OSH_E_OK);
    CHECK(ck_stream_read(&pool, &sess, r, b, 4, 0, &n) == OSH_E_OK && hook_calls[0][1] == 1);
    CHECK(ck_pipe_unwatch(&pool, &sess, w) == OSH_E_OK);
    CHECK(ck_pipe_unwatch(&pool, &sess, w) == OSH_E_OK && pool.pipes[0].waiting[1] == 0); /* never below zero */
    struct ck_handle stale = r;
    stale.generation++;
    CHECK(ck_pipe_watch(&pool, &sess, stale) == OSH_E_STALE);
    CHECK(ck_pipe_revoke(&pool, &sess, r) == OSH_E_OK);
    CHECK(ck_pipe_watch(&pool, &sess, r) == OSH_E_OK); /* a revoked pipe can be watched (and is closed below) */
    ck_stream_close(&pool, &sess, r);
    ck_stream_close(&pool, &sess, w);
    CHECK(ck_pipe_pool_empty(&pool) && pool.pipes[0].waiting[0] == 0);
}

/* ---- 15. the native pipeline baselines (core/pipe_bench.h, aienos-mv#432) ----
 * In the plain run every scenario runs once as a check (a mutant that breaks bytes, cleanup or revoke turns it red);
 * `test_task --bench` runs them with repeats and prints the numbers: host CPU, C stand-in stages, not physical. */
static uint64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static struct ck_task_spawn_req req_any(unsigned image)
{
    return req(image);
}

#define BENCH_LABEL "host (Spark CPU), C stand-in stages, not Omega code, not QEMU, not physical"
static int bench(unsigned runs, int print)
{
    const struct pb_env e = { &tab, &pool, &sess, setup, req_any, clean, mono_us, K_GEN, K_XFORM, K_SINK };
    static char det[400];
    int all = 1, ok;
#define BENCH_CASE(name, call)                                                                               \
    do {                                                                                                     \
        ok = (call);                                                                                         \
        CHECK(ok);                                                                                           \
        all = all && ok;                                                                                     \
        if (print)                                                                                           \
            printf("pipe_bench: case=%s result=%s %s; %s\n", name, ok ? "PASS" : "FAIL", det, BENCH_LABEL); \
    } while (0)
    BENCH_CASE("two_stage_163840", pb_flow(&e, 2, 163840, runs, det, sizeof det));
    BENCH_CASE("two_stage_1048576", pb_flow(&e, 2, 1048576, runs, det, sizeof det));
    BENCH_CASE("three_stage_163840", pb_flow(&e, 3, 163840, runs, det, sizeof det));
    BENCH_CASE("three_stage_1048576", pb_flow(&e, 3, 1048576, runs, det, sizeof det));
    BENCH_CASE("tiny_one_byte", pb_flow(&e, 2, 1, runs, det, sizeof det));
    BENCH_CASE("first_byte", pb_first_byte(&e, det, sizeof det));
    BENCH_CASE("cancel_endless", pb_cancel(&e, det, sizeof det));
    BENCH_CASE("capacity_64k", pb_capacity(&e, det, sizeof det));
    BENCH_CASE("revoke_blocked_reader", pb_revoke(&e, 0, det, sizeof det));
    BENCH_CASE("revoke_blocked_writer", pb_revoke(&e, 1, det, sizeof det));
#undef BENCH_CASE
    pb_footprint(det, sizeof det);
    if (print)
        printf("pipe_bench: case=footprint result=PASS %s; %s\n", det, BENCH_LABEL);
    return all;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--bench") == 0) {
        int ok = bench(5, 1);
        printf("test_task_bench: %s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    two_stage();
    three_stage();
    early_exit();
    crash();
    cancel();
    partial();
    binding();
    least_privilege();
    wakes();
    outcomes();
    ids();
    dispatch();
    watch();
    bench(1, 0);
    return ck_t_verdict("test_task");
}
