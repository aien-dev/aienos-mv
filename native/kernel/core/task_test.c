/* task_test.c -- AIENOS_CK_TASK kernel self-test (TEST ONLY, CK_TASK_TEST=1; gate scripts/qemu_ck_task_test.sh).
 *
 * Runs pipelines of three distinct admitted Omega stages (tests/fixtures/task: tgen.unit, txform.unit, tsink.unit,
 * real oscc output, TEST signer, each with its own checker2 realization receipt, all arriving through the boot disk
 * Store) over native pipes with the task lifecycle of core/task.c and the kernel runner of core/task_osc.c: every
 * step is one ck_osc_launch (receipt verified, code re-hashed, EL0, own address space, torn down).
 *
 * Scheduling, stated plainly: one CPU, cooperative at step granularity. The stages are both alive and progress
 * interleaved (one bounded EL0 step of one stage at a time, round robin), with bytes in flight in the pipe between
 * them; a step is never preempted and resumed. Lines on the serial console:
 *     task_check: name=<check> result=PASS|FAIL <details> clean=<0|1>; <label>
 *     task_test: done checks=<n> failed=<n> launches=<n> not_torn_down=<n> scheduling=cooperative-step; <label>
 * QEMU (aarch64 virt), TEST signer, TEST build, not physical: qualifies nothing physical. */
#ifndef CK_TASK_TEST
#error "task_test is test-only: build with make CK_TASK_TEST=1 CK_SEED0B_TEST_ANCHOR=1"
#endif
#ifdef CK_HARDWARE_STAGING
#error "CK_TASK_TEST cannot be combined with CK_HARDWARE_STAGING"
#endif
#include "ck_internal.h"
#include "fmt.h"
#include "osc_admit.h"
#include "osc_unit.h"
#include "task.h"
#include "task_osc.h"

#define LABEL "QEMU (aarch64 virt), TEST signer, TEST build, not physical"
#define UNIT_MAX 8192u

/* ---- the three stage units, saved when the Store hands them to admission ---- */
enum { U_GEN = 0, U_XFORM = 1, U_SINK = 2, U_COUNT = 3 };
static const char *const u_file[U_COUNT] = { "tgen.unit", "txform.unit", "tsink.unit" };
static const char *const u_rcpt[U_COUNT] = { "tgen.rcpt", "txform.rcpt", "tsink.rcpt" };
static const char *const u_entry[U_COUNT] = { "gen_step", "xform_step", "sink_step" };
static struct {
    uint8_t bytes[UNIT_MAX];
    size_t len;
    int have;
    struct osc_accept acc;
} unit[U_COUNT];

static int streq(const char *a, const char *b)
{
    size_t n = strlen(a);
    return n == strlen(b) && memcmp(a, b, n) == 0;
}

void ck_task_save(const char *name, const uint8_t *b, size_t len, const struct osc_accept *acc)
{
    for (unsigned u = 0; u < U_COUNT; u++)
        if (streq(name, u_file[u]) && len <= UNIT_MAX) {
            memcpy(unit[u].bytes, b, len);
            unit[u].len = len;
            unit[u].acc = *acc;
            unit[u].have = 1;
        }
}

/* images: 0..2 the stages with their own receipts; 3 tsink with NO receipt; 4 tsink presented with tgen's receipt;
 * 5 tgen naming a function it does not have */
enum { I_GEN = 0, I_XFORM = 1, I_SINK = 2, I_SINK_NORCPT = 3, I_SINK_WRONGRCPT = 4, I_NOENTRY = 5, I_COUNT = 6 };
static struct ck_task_osc_image img[I_COUNT];

static void image(unsigned i, unsigned u, unsigned rcpt_of, const char *entry)
{
    img[i].unit = unit[u].bytes;
    img[i].unit_len = unit[u].len;
    img[i].acc = &unit[u].acc;
    img[i].rcpt = rcpt_of < U_COUNT ? ck_osc_receipt_find(u_rcpt[rcpt_of], &img[i].rcpt_len) : 0;
    if (rcpt_of >= U_COUNT)
        img[i].rcpt_len = 0;
    img[i].entry = entry;
}

/* ---- the runner, wrapped to record which image each step ran (the interleaving evidence) ---- */
static struct ck_task_runner base;
static struct ck_task_osc_stats ost;
#define TRACE_MAX 4096u
static uint8_t trace[TRACE_MAX];
static unsigned ntrace;

static void traced_step(void *ctx, const void *im, const uint8_t *in, size_t in_len, uint64_t *ws, uint64_t max_ticks,
                        struct ck_task_step *r)
{
    if (ntrace < TRACE_MAX)
        trace[ntrace++] = (uint8_t)((const struct ck_task_osc_image *)im - img);
    base.step(ctx, im, in, in_len, ws, max_ticks, r);
}

/* ---- the session ---- */
static struct ck_pipe_pool pool;
static struct ck_cap_table sess;
static struct ck_task_tab tab;
static struct ck_handle spawn_cap;
static const uint8_t P[32] = { 0x41, 0x49, 0x45, 0x4e, 0x54, 0x41, 0x53, 0x4b, 1, 2, 3, 4, 5, 6, 7, 8,
                               9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24 };
static unsigned checks, failed;

static void setup(void)
{
    struct ck_task_runner run = base;
    run.step = traced_step;
    ck_pipe_pool_init(&pool, 0);
    ck_cap_init(&sess, 0x5E55u, CK_CAP_SLOTS);
    ck_cap_insert(&sess, CK_TASK_SPAWN_RESOURCE, CK_R_WRITE, &spawn_cap);
    ck_task_init(&tab, &pool, &sess, P, &run);
    for (unsigned i = 0; i < I_COUNT; i++)
        ck_task_image_set(&tab, i, &img[i]);
    ntrace = 0;
}

static unsigned live_caps(const struct ck_cap_table *t)
{
    unsigned n = 0;
    for (unsigned i = 0; i < t->n; i++)
        n += t->slots[i].live && !t->slots[i].tombstone;
    return n;
}

/* no task, no pipe, the session holds only its spawn capability, nothing polled, every EL0 slot torn down */
static int clean(void)
{
    unsigned task_caps = 0;
    for (unsigned i = 0; i < CK_TASK_MAX; i++)
        task_caps += live_caps(&tab.tasks[i].caps);
    return ck_task_tab_empty(&tab) && ck_pipe_pool_empty(&pool) && live_caps(&sess) == 1 && task_caps == 0 &&
           tab.stats.blocked_runs == 0 && ost.not_torn_down == 0;
}

static void verdict(const char *name, int ok, const char *details)
{
    int c = clean();
    checks++;
    failed += !(ok && c);
    ck_printf("task_check: name=%s result=%s %s clean=%d; %s\n", name, ok && c ? "PASS" : "FAIL", details, c, LABEL);
}

/* a snapshot while tasks are deliberately alive: the leak check comes at the end of the scenario */
static void verdict_live(const char *name, int ok, const char *details)
{
    checks++;
    failed += !ok;
    ck_printf("task_check: name=%s result=%s %s clean=alive; %s\n", name, ok ? "PASS" : "FAIL", details, LABEL);
}

static struct ck_task_spawn_req req(unsigned image_ix)
{
    struct ck_task_spawn_req r;
    memset(&r, 0, sizeof r);
    r.image = image_ix;
    memcpy(r.binding.principal_id, P, 32);
    r.binding.domain = CK_TASK_DOMAIN_KERNEL;
    r.binding.cap_index = spawn_cap.index;
    r.binding.cap_generation = spawn_cap.generation;
    r.binding.resource_class = CK_TASK_RC_TASK;
    r.binding.operation = CK_TASK_OP_SPAWN;
    r.max_ticks = 100; /* per step: 1 s of 10 ms ticks, far above what a step needs */
    return r;
}

static uint8_t pat(uint64_t i) /* tgen.osc byte i, computed here independently of the unit */
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

static uint64_t le64(const uint8_t *b)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | b[i];
    return v;
}

static size_t drain_close(struct ck_handle hr, uint8_t *buf, size_t max)
{
    size_t got = 0, n = 0;
    while (got < max && ck_stream_read(&pool, &sess, hr, buf + got, max - got, 0, &n) == OSH_E_OK && n)
        got += n;
    ck_stream_close(&pool, &sess, hr);
    return got;
}

static char det[320];

/* 1. two distinct admitted stages at once over one bounded pipe, the consumer slow (backpressure) */
static void two_stage(void)
{
    setup();
    const uint64_t total = 163840; /* 2.5 pipe capacities, every fifth byte NUL */
    struct ck_task_spawn_req st[2] = { req(I_GEN), req(I_SINK) };
    st[0].params[0] = total;
    st[0].params[1] = 4096;
    st[0].nparams = 2;
    st[1].in_chunk = 512;
    struct ck_handle hr, hw, none = { 0, 0 };
    struct ck_task_pipeline pl;
    int ok = ck_pipe_create(&pool, &sess, &hr, &hw) == OSH_E_OK;
    ok = ok && ck_task_pipeline_spawn(&tab, st, 2, none, hw, &pl) == OSH_E_OK;
    ck_stream_close(&pool, &sess, hw);
    uint64_t gid = pl.ids[0], sid = pl.ids[1];
    ck_task_run(&tab, 60);
    struct ck_task_info gi, si;
    int mid = ck_task_state(&tab, gid, &gi) == OSH_E_OK && ck_task_state(&tab, sid, &si) == OSH_E_OK &&
              gi.state != CK_TS_EXITED && si.state != CK_TS_EXITED && si.bytes_in > 0 && gi.bytes_out > si.bytes_in &&
              gi.blocks > 0;
    uint64_t in_flight = gi.bytes_out - si.bytes_in;
    ck_snprintf(det, sizeof det,
                "gen_state=%u sink_state=%u gen_out=%llu sink_in=%llu in_flight=%llu gen_blocked_on_full_pipe=%llu",
                gi.state, si.state, (unsigned long long)gi.bytes_out, (unsigned long long)si.bytes_in,
                (unsigned long long)in_flight, (unsigned long long)gi.blocks);
    verdict_live("two_stage_mid_run_both_alive", ok && mid, det);
    uint32_t last = 99;
    ok = ok && ck_task_pipeline_wait(&tab, &pl, CK_TASK_FOREVER, &last) == OSH_E_OK;
    uint8_t res[32];
    size_t got = drain_close(hr, res, sizeof res);
    uint64_t want = fnv(total, 0);
    unsigned last_gen = 0, first_sink = TRACE_MAX, switches = 0;
    for (unsigned i = 0; i < ntrace; i++) {
        if (trace[i] == I_GEN)
            last_gen = i;
        if (trace[i] == I_SINK && first_sink == TRACE_MAX)
            first_sink = i;
        if (i && trace[i] != trace[i - 1])
            switches++;
    }
    ok = ok && last == 0 && pl.status[0] == 0 && got == 16 && le64(res) == total && le64(res + 8) == want &&
         first_sink < last_gen && switches >= 20 && pool.total.max_resident <= CK_PIPE_CAPACITY &&
         tab.stats.wakeups > 0;
    ck_snprintf(det, sizeof det,
                "bytes=%llu sink_count=%llu hash_ok=%d status=%u,%u steps=%llu switches=%u first_sink_step=%u "
                "last_gen_step=%u blocks=%llu wakeups=%llu max_resident=%llu capacity=%u",
                (unsigned long long)total, (unsigned long long)(got == 16 ? le64(res) : 0),
                got == 16 && le64(res + 8) == want, (unsigned)pl.status[0], (unsigned)last,
                (unsigned long long)tab.stats.steps, switches, first_sink, last_gen,
                (unsigned long long)tab.stats.blocks, (unsigned long long)tab.stats.wakeups,
                (unsigned long long)pool.total.max_resident, CK_PIPE_CAPACITY);
    verdict("two_stage_backpressure_exact_bytes", ok, det);
}

/* 2. a third stage */
static void three_stage(void)
{
    setup();
    const uint64_t total = 100003;
    struct ck_task_spawn_req st[3] = { req(I_GEN), req(I_XFORM), req(I_SINK) };
    st[0].params[0] = total;
    st[0].params[1] = 3000;
    st[0].nparams = 2;
    st[1].params[0] = 0x5A;
    st[1].nparams = 1;
    st[1].in_chunk = 1024;
    st[2].in_chunk = 700;
    struct ck_handle hr, hw, none = { 0, 0 };
    struct ck_task_pipeline pl;
    int ok = ck_pipe_create(&pool, &sess, &hr, &hw) == OSH_E_OK;
    ok = ok && ck_task_pipeline_spawn(&tab, st, 3, none, hw, &pl) == OSH_E_OK;
    ck_stream_close(&pool, &sess, hw);
    uint32_t last = 99;
    ok = ok && ck_task_pipeline_wait(&tab, &pl, CK_TASK_FOREVER, &last) == OSH_E_OK;
    uint8_t res[32];
    size_t got = drain_close(hr, res, sizeof res);
    ok = ok && last == 0 && pl.status[0] == 0 && pl.status[1] == 0 && got == 16 && le64(res) == total &&
         le64(res + 8) == fnv(total, 0x5A);
    ck_snprintf(det, sizeof det, "bytes=%llu key=0x5a status=%u,%u,%u hash_ok=%d steps=%llu blocks=%llu",
                (unsigned long long)total, (unsigned)pl.status[0], (unsigned)pl.status[1], (unsigned)last,
                got == 16 && le64(res + 8) == fnv(total, 0x5A), (unsigned long long)tab.stats.steps,
                (unsigned long long)tab.stats.blocks);
    verdict("three_stage_gen_xform_sink", ok, det);
}

/* 3. nonzero exit: the last stage stops early with status 3 while the producer never ends */
static void nonzero_exit(void)
{
    setup();
    struct ck_task_spawn_req st[2] = { req(I_GEN), req(I_XFORM) };
    st[0].params[1] = 4096;
    st[0].nparams = 2; /* total 0: endless */
    st[1].params[1] = 10000;
    st[1].params[2] = 3;
    st[1].nparams = 3;
    struct ck_handle none = { 0, 0 };
    struct ck_task_pipeline pl;
    uint32_t last = 99;
    int ok = ck_task_pipeline_spawn(&tab, st, 2, none, none, &pl) == OSH_E_OK &&
             ck_task_pipeline_wait(&tab, &pl, CK_TASK_FOREVER, &last) == OSH_E_OK;
    ok = ok && last == 3 && pl.status[0] == 141;
    ck_snprintf(det, sizeof det, "pipeline_status=%u endless_producer_status=%u (141 = BROKEN_PIPE)", (unsigned)last,
                (unsigned)pl.status[0]);
    verdict("nonzero_exit_consumer_exits_early", ok, det);
}

/* 4. crash: the middle stage traps (a real OSC BOUNDS trap, code 3) mid-stream */
static void crash(void)
{
    setup();
    struct ck_task_spawn_req st[3] = { req(I_GEN), req(I_XFORM), req(I_SINK) };
    st[0].params[0] = 50000;
    st[0].nparams = 1;
    st[1].params[3] = 5000;
    st[1].nparams = 4;
    struct ck_handle hr, hw, none = { 0, 0 };
    struct ck_task_pipeline pl;
    int ok = ck_pipe_create(&pool, &sess, &hr, &hw) == OSH_E_OK;
    ok = ok && ck_task_pipeline_spawn(&tab, st, 3, none, hw, &pl) == OSH_E_OK;
    ck_stream_close(&pool, &sess, hw);
    uint64_t xid = pl.ids[1];
    ck_task_run(&tab, 400);
    struct ck_task_info xi;
    int trapped = ck_task_state(&tab, xid, &xi) == OSH_E_OK && xi.end == CK_TE_TRAPPED && xi.status == 134;
    uint32_t last = 99;
    ok = ok && trapped && ck_task_pipeline_wait(&tab, &pl, CK_TASK_FOREVER, &last) == OSH_E_OK;
    uint8_t res[32];
    size_t got = drain_close(hr, res, sizeof res);
    ok = ok && pl.status[1] == 134 && pl.status[0] == 141 && last == 0 && got == 16 && le64(res) < 50000;
    ck_snprintf(det, sizeof det, "status=%u,%u,%u (134 = TRAPPED) sink_saw=%llu of 50000", (unsigned)pl.status[0],
                (unsigned)pl.status[1], (unsigned)last, (unsigned long long)(got == 16 ? le64(res) : 0));
    verdict("crash_trap_mid_pipeline", ok, det);
}

/* 5. cancellation: a pipeline that never ends, a deadline, then interrupt with grace and terminate (section 12) */
static void cancel(void)
{
    setup();
    struct ck_task_spawn_req st[2] = { req(I_GEN), req(I_SINK) };
    st[0].nparams = 1; /* endless */
    struct ck_handle none = { 0, 0 };
    struct ck_task_pipeline pl;
    uint32_t last = 99;
    int ok = ck_task_pipeline_spawn(&tab, st, 2, none, none, &pl) == OSH_E_OK;
    uint64_t t0 = base.now(base.ctx);
    ok = ok && ck_task_pipeline_wait(&tab, &pl, 30, &last) == OSH_E_WOULD_BLOCK;
    uint64_t waited = base.now(base.ctx) - t0;
    uint64_t steps = tab.stats.steps;
    unsigned killed = ck_task_pipeline_cancel(&tab, &pl);
    ok = ok && waited >= 30 && pl.status[0] == 130 && pl.status[1] == 130;
    ck_snprintf(det, sizeof det,
                "deadline_ticks=30 waited_ticks=%llu steps_before_cancel=%llu status=%u,%u (130 = interrupted) "
                "terminated_after_grace=%u",
                (unsigned long long)waited, (unsigned long long)steps, (unsigned)pl.status[0],
                (unsigned)pl.status[1], killed);
    verdict("cancel_deadline_interrupt", ok, det);
}

/* 6. partial spawn: the third stage's receipt is missing; the two started are terminated and reaped */
static void partial(void)
{
    setup();
    struct ck_task_spawn_req st[3] = { req(I_GEN), req(I_XFORM), req(I_SINK_NORCPT) };
    st[0].nparams = 1;
    struct ck_handle none = { 0, 0 };
    struct ck_task_pipeline pl;
    uint64_t launches = ost.launches;
    unsigned e = ck_task_pipeline_spawn(&tab, st, 3, none, none, &pl);
    int ok = e == OSH_E_PARTIAL_LAUNCH && pl.started == 2 && pl.cause == OSH_E_DENIED && ost.launches == launches &&
             ost.last_refusal == 64;
    ck_snprintf(det, sizeof det, "result=%s started=%u cause=%s receipt_refusal=%u (64 = RECEIPT_MISSING) steps_run=%llu",
                osh_err_label(e), pl.started, osh_err_label(pl.cause), (unsigned)ost.last_refusal,
                (unsigned long long)(ost.launches - launches));
    verdict("partial_spawn_missing_receipt", ok, det);
}

/* 7. spawn refusals before any effect: another program's receipt, an unknown entry, a wrong principal */
static void refusals(void)
{
    setup();
    uint64_t id = 0, launches = ost.launches;
    struct ck_task_spawn_req r = req(I_SINK_WRONGRCPT);
    unsigned e1 = ck_task_spawn(&tab, &r, &id);
    unsigned rc1 = ost.last_refusal;
    r = req(I_NOENTRY);
    unsigned e2 = ck_task_spawn(&tab, &r, &id);
    r = req(I_SINK);
    r.binding.principal_id[0] ^= 1;
    unsigned e3 = ck_task_spawn(&tab, &r, &id);
    r = req(I_SINK);
    r.binding.domain = CK_TASK_DOMAIN_HOSTED;
    unsigned e4 = ck_task_spawn(&tab, &r, &id);
    int ok = e1 == OSH_E_DENIED && rc1 != 0 && e2 == OSH_E_NOT_FOUND && e3 == OSH_E_DENIED &&
             e4 == OSH_E_CAP_DOMAIN_MISMATCH && id == 0 && ost.launches == launches;
    ck_snprintf(det, sizeof det,
                "other_programs_receipt=%s(refusal %u) unknown_entry=%s wrong_principal=%s hosted_domain=%s "
                "steps_run=%llu",
                osh_err_label(e1), rc1, osh_err_label(e2), osh_err_label(e3), osh_err_label(e4),
                (unsigned long long)(ost.launches - launches));
    verdict("spawn_refused_before_any_effect", ok, det);
}

/* 8. least privilege: what each stage's own table holds */
static void least_privilege(void)
{
    setup();
    struct ck_handle hr, hw;
    int ok = ck_pipe_create(&pool, &sess, &hr, &hw) == OSH_E_OK;
    struct ck_task_spawn_req r = req(I_XFORM);
    r.stdio[0] = hr;
    r.stdio[1] = hw;
    uint64_t id = 0;
    ok = ok && ck_task_spawn(&tab, &r, &id) == OSH_E_OK;
    const struct ck_task_rec *t = &tab.tasks[(id >> 32) & 0xFFu];
    unsigned ri = 0, ro = 0, rs = 0;
    ck_cap_rights(&sess, hr, &rs);
    ok = ok && ck_cap_rights(&t->caps, t->stdio[0], &ri) == CK_CAP_OK && ck_cap_rights(&t->caps, t->stdio[1], &ro) ==
                                                                              CK_CAP_OK;
    ok = ok && ri == CK_R_READ && ro == CK_R_WRITE && live_caps(&t->caps) == 2 &&
         ck_pipe_revoke(&pool, &t->caps, t->stdio[0]) == OSH_E_DENIED;
    uint32_t s = 0;
    ck_task_cancel(&tab, id, CK_TASK_CANCEL_TERMINATE);
    ok = ok && ck_task_wait(&tab, id, 0, &s) == OSH_E_OK && s == 143;
    ck_stream_close(&pool, &sess, hr);
    ck_stream_close(&pool, &sess, hw);
    ck_snprintf(det, sizeof det, "session_end_rights=0x%x stage_stdin_rights=0x%x stage_stdout_rights=0x%x", rs, ri,
                ro);
    verdict("least_privilege_endpoints", ok, det);
}

void ck_task_selftest(void)
{
    ck_printf("task_test: TEST-ONLY task self-test begin abi_version=%u max_tasks=%u stage_cells=%u in_max=%u "
              "out_max=%u pipe_capacity=%u scheduling=cooperative-step (one EL0 step at a time, never preempted and "
              "resumed); %s\n",
              CK_TASK_ABI_VERSION, CK_TASK_MAX, (unsigned)CK_STG_CELLS, CK_TASK_IN_MAX, CK_TASK_OUT_MAX,
              CK_PIPE_CAPACITY, LABEL);
    for (unsigned u = 0; u < U_COUNT; u++)
        if (!unit[u].have) {
            ck_printf("task_check: name=units_arrived result=FAIL missing=%s; %s\n", u_file[u], LABEL);
            ck_printf("task_test: done checks=1 failed=1 launches=0 not_torn_down=0 scheduling=cooperative-step; %s\n",
                      LABEL);
            return;
        }
    ck_task_osc_runner(&base, &ost);
    image(I_GEN, U_GEN, U_GEN, u_entry[U_GEN]);
    image(I_XFORM, U_XFORM, U_XFORM, u_entry[U_XFORM]);
    image(I_SINK, U_SINK, U_SINK, u_entry[U_SINK]);
    image(I_SINK_NORCPT, U_SINK, U_COUNT, u_entry[U_SINK]);
    image(I_SINK_WRONGRCPT, U_SINK, U_GEN, u_entry[U_SINK]);
    image(I_NOENTRY, U_GEN, U_GEN, "no_such_step");
    two_stage();
    three_stage();
    nonzero_exit();
    crash();
    cancel();
    partial();
    refusals();
    least_privilege();
    ck_printf("task_test: done checks=%u failed=%u launches=%llu not_torn_down=%llu scheduling=cooperative-step; %s\n",
              checks, failed, (unsigned long long)ost.launches, (unsigned long long)ost.not_torn_down, LABEL);
}
