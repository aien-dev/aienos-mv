/* test_osh_op.c -- host tests for core/osh_op.c: the AIENOS adapter's executor for OSH request records.
 * Part 1 (OSH-AIENOS-OP): the capability-authorized console write (`klog`).
 * Part 2 (aienos-mv#430): multi-command pipelines of native tasks over native pipes, through C stand-in stages behind the
 * same task runner seam the kernel uses (core/task.h), with ordered redirections into TEST locations, authority at every
 * effect boundary, partial launch, loss after an effect, interrupt, and the direct-argv route.
 * Part 3: the shared OSH launch vectors (aien-protocols-mv specs/osh-platform/vectors/t*.vec on feat/osh-stream-abi-43
 * at 181855c9ff3e, copied into tests/fixtures/osh_exec/), run against this executor; NOT_RUN for every vector whose
 * features or authority domain this adapter lacks.
 * Part 4 (aienos-mv#431): the effect journal: what each intent binds, refusal by name without a usable journal, a
 * poisoned journal, every crash point with exact counts and no duplicate effect, read-only inspection with no OSH
 * session, recovery categories, and operator reconciliation.
 * Requests are built as the expander's request record (OSH_PLATFORM_ABI section 7) without calling the shell core. */
#include <stdlib.h>
#include <string.h>
#include "ck_test.h"
#include "osh_op.h"
#include "sha256.h"

static char cap_buf[4096];
static int cap_calls;
static char logs[64][480];
static int nlogs;
static uint8_t so_buf[1u << 20]; /* the session's stdout */
static size_t so_len;

static int sink(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    cap_calls++;
    CHECK(line[len] == 0 && len > 0 && line[len - 1] == '\n'); /* a C string the kernel sink may print */
    if (strlen(cap_buf) + len < sizeof cap_buf)
    {
        size_t at = strlen(cap_buf);
        memcpy(cap_buf + at, line, len);
        cap_buf[at + len] = 0;
    }
    return 0;
}
static void logger(void *ctx, const char *line)
{
    (void)ctx;
    if (nlogs < 64)
        snprintf(logs[nlogs++], sizeof logs[0], "%s", line);
}
static int so_sink(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (so_len + n > sizeof so_buf)
        return -1;
    memcpy(so_buf + so_len, b, n);
    so_len += n;
    return 0;
}
/* aienos-mv#431: every session records its effects in a journal. These tests run on the TEST RAM backend (the same
 * Cortex bytes in memory, durable=0, accepted only because each session asks for it); a restart closes the journal,
 * which drops every in-memory Cortex state, and reopens it over the same bytes: what a crash leaves. */
void *ck_cx_alloc(size_t n) { return malloc(n); }
void *ck_cx_realloc(void *p, size_t n) { return realloc(p, n); }
void ck_cx_free(void *p) { free(p); }
static uint8_t jbuf[1u << 20];
static struct oshj_ram jram;
static struct osh_journal J0;
static void J(struct osh_session *s) /* a fresh journal */
{
    osh_journal_close(&J0);
    oshj_ram_init(&jram, jbuf, sizeof jbuf);
    CHECK(osh_journal_open(&J0, &oshj_ram_ops, &jram, 0, "ram_test", 0) == CX_OK);
    osh_session_journal(s, &J0, 1);
}
static void JR(struct osh_session *t) /* what survives the crash: the journal's bytes, reopened and replayed */
{
    osh_journal_close(&J0);
    CHECK(osh_journal_open(&J0, &oshj_ram_ops, &jram, 0, "ram_test", 0) == CX_OK);
    osh_session_journal(t, &J0, 1);
}
static const struct osh_env env = { 0, sink, logger, 0, 0 };
static const struct osh_env senv = { 0, sink, logger, so_sink, 0 };

static void reset_capture(void)
{
    cap_buf[0] = 0;
    cap_calls = 0;
    nlogs = 0;
    so_len = 0;
}

static uint64_t req[8 + 8 * 164];
static uint64_t out[8192];

/* one command per block; args NULL-terminated; extra fields set by the caller afterwards */
static void build(unsigned ncmds, const char *const *argv, const char *const *argv2)
{
    memset(req, 0, sizeof req);
    memset(out, 0, sizeof out);
    req[0] = 0x4F524551ull;
    req[1] = ncmds;
    req[3] = 0;
    req[5] = 1;
    uint64_t used = 0;
    for (unsigned c = 0; c < ncmds; c++) {
        const char *const *a = c == 0 ? argv : argv2;
        uint64_t *b = req + 8 + c * 164;
        unsigned n = 0;
        for (; a[n]; n++) {
            size_t l = strlen(a[n]);
            for (size_t i = 0; i < l; i++)
                out[used + i] = (uint8_t)a[n][i];
            b[4 + 2 * n] = used;
            b[5 + 2 * n] = l;
            used += l;
        }
        b[0] = n;
    }
    req[4] = used;
}

static struct osh_result run(struct osh_session *s, const char *const *argv)
{
    struct osh_result r;
    build(1, argv, 0);
    osh_op_exec(s, &env, req, out, 8192, &r);
    return r;
}

/* ---- Part 2 helpers: a request record from a small pipeline text, as the expander would build it ----
 * Words separated by single spaces; "|" separates commands. Redirection words: <name >name >>name N>name N>>name
 * N<name N>&M N<&M. A word "A=b" before the name is a prefix assignment. A word starting with "@" is a builtin id. */
static uint64_t used_out;
static void put_bytes(const char *p, size_t l, uint64_t *off, uint64_t *len)
{
    *off = used_out;
    *len = l;
    for (size_t i = 0; i < l; i++)
        out[used_out++] = (uint8_t)p[i];
}

static void build_line(const char *line, int direct)
{
    memset(req, 0, sizeof req);
    memset(out, 0, sizeof out);
    used_out = 0;
    req[0] = 0x4F524551ull;
    req[3] = direct ? 1 : 0;
    req[5] = 1;
    unsigned nc = 0;
    uint64_t *b = req + 8;
    const char *p = line;
    while (*p) {
        const char *e = strchr(p, ' ');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == 1 && p[0] == '|') {
            nc++;
            b = req + 8 + nc * 164;
        } else {
            const char *w = p;
            size_t wl = l;
            unsigned fd = 9;
            if (wl >= 2 && w[0] >= '0' && w[0] <= '2' && (w[1] == '>' || w[1] == '<')) {
                fd = (unsigned)(w[0] - '0');
                w++;
                wl--;
            }
            if (w[0] == '>' || w[0] == '<') {
                uint64_t *rd = b + 4 + 64 + 64 + 4 * b[2];
                int app = wl >= 2 && w[0] == '>' && w[1] == '>';
                if (wl >= 2 && w[1] == '&') {
                    rd[0] = 4;
                    rd[1] = fd != 9 ? fd : (w[0] == '>' ? 1u : 0u);
                    rd[2] = (uint64_t)(w[2] - '0');
                    rd[3] = 0;
                } else {
                    rd[0] = w[0] == '<' ? 1 : app ? 3 : 2;
                    rd[1] = fd != 9 ? fd : (w[0] == '>' ? 1u : 0u);
                    size_t skip = app ? 2 : 1;
                    put_bytes(w + skip, wl - skip, &rd[2], &rd[3]);
                }
                b[2]++;
            } else if (w[0] == '@') {
                b[3] = (uint64_t)(w[1] - '0');
            } else if (b[0] == 0 && memchr(w, '=', wl)) {
                const char *eq = memchr(w, '=', wl);
                uint64_t *as = b + 4 + 64 + 4 * b[1];
                put_bytes(w, (size_t)(eq - w), &as[0], &as[1]);
                put_bytes(eq + 1, wl - (size_t)(eq - w) - 1, &as[2], &as[3]);
                b[1]++;
            } else {
                put_bytes(w, wl, &b[4 + 2 * b[0]], &b[5 + 2 * b[0]]);
                b[0]++;
            }
        }
        p += l;
        if (*p == ' ')
            p++;
    }
    req[1] = nc + 1;
    req[4] = used_out;
}

/* ---- C stand-in stages (stage ABI v1, core/task.h), the same seam the kernel's OSC runner sits behind ---- */
enum kind { K_GEN, K_XFORM, K_SINK, K_EXIT, K_FLOOD, K_DRAIN, K_HONOR, K_COUNT };
struct img {
    enum kind kind;
    unsigned admit_err;
};
static struct img imgs[K_COUNT] = { { K_GEN, 0 }, { K_XFORM, 0 }, { K_SINK, 0 }, { K_EXIT, 0 },
                                    { K_FLOOD, 0 }, { K_DRAIN, 0 }, { K_HONOR, 0 } };
static struct img img_denied = { K_SINK, OSH_E_DENIED }; /* a unit whose receipt the runner refuses */
#define IMG_DENIED 15u
static uint64_t clock_now, steps_run;

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

static uint64_t xform(const uint8_t *in, size_t n, uint64_t *w)
{
    for (size_t j = 0; j < n; j++)
        w[64 + j] = in[j] ^ (w[8] & 255);
    uint64_t seen = w[16] + n;
    w[16] = seen;
    w[3] = n;
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

static uint64_t sinkst(const uint8_t *in, size_t n, uint64_t *w, int emit)
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
        if (emit) {
            for (unsigned k = 0; k < 8; k++) {
                w[64 + k] = (c >> (k * 8)) & 255;
                w[72 + k] = (h >> (k * 8)) & 255;
            }
            w[3] = 16;
        }
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
    steps_run++;
    r->cls = CK_TASK_STEP_RETURNED;
    r->code = 0;
    r->ticks = 0;
    if (w[0] != CK_STG_MAGIC || w[1] != 1) {
        r->value = 200;
        return;
    }
    switch (g->kind) {
    case K_GEN: r->value = gen(w); break;
    case K_XFORM: r->value = xform(in, in_len, w); break;
    case K_SINK: r->value = sinkst(in, in_len, w, 1); break;
    case K_DRAIN: r->value = sinkst(in, in_len, w, 0); break;
    case K_EXIT: w[3] = 0; w[4] = w[8] & 255; r->value = 1; break;
    case K_FLOOD:
        for (unsigned j = 0; j < 64; j++)
            w[64 + j] = j & 1 ? '\n' : 'y';
        w[3] = 64;
        r->value = 0;
        break;
    case K_HONOR: /* a consumer that ends itself with 42 when interrupted */
        if (w[2] & CK_STG_F_INTR) {
            w[3] = 0;
            w[4] = 42;
            r->value = 1;
        } else {
            r->value = sinkst(in, in_len, w, 0);
        }
        break;
    default: r->value = 9; break;
    }
}

static uint64_t h_now(void *ctx)
{
    (void)ctx;
    return clock_now;
}

/* a stand-in unit digest for gen: each spawn intent of gen must carry it */
static const uint8_t gen_unit[32] = { 0x67, 0x65, 0x6e, 0x01 };
static const struct osh_op_def ops[] = {
    { "gen", OSH_CLASS_NATIVE_OP, 2, K_GEN, 0, gen_unit },     { "xform", OSH_CLASS_NATIVE_OP, 3, K_XFORM, 1024, 0 },
    { "sink", OSH_CLASS_NATIVE_OP, 2, K_SINK, 512, 0 }, { "exit", OSH_CLASS_NATIVE_OP, 1, K_EXIT, 0, 0 },
    { "cat", OSH_CLASS_NATIVE_OP, 0, K_XFORM, 0, 0 },   { "flood", OSH_CLASS_NATIVE_OP, 0, K_FLOOD, 0, 0 },
    { "drain", OSH_CLASS_NATIVE_OP, 0, K_DRAIN, 0, 0 }, { "honor", OSH_CLASS_NATIVE_OP, 0, K_HONOR, 0, 0 },
    { "badunit", OSH_CLASS_NATIVE_OP, 0, IMG_DENIED, 0, 0 },
    { "foreign", OSH_CLASS_LINUX, 0, 0, 0, 0 },        /* a name RESOLVE maps to a class this adapter does not provide */
};

static struct ck_task_tab tab;
static struct ck_pipe_pool pool;
static struct osh_native nat;

/* a session that may spawn and open locations */
static void native(struct osh_session *s, unsigned spawn_rights, unsigned loc_rights)
{
    osh_session_init(s, CK_R_WRITE);
    J(s);
    if (spawn_rights)
        CHECK(osh_session_grant_spawn(s, spawn_rights) == 0);
    if (loc_rights)
        CHECK(osh_session_grant_loc(s, loc_rights) == 0);
    memset(&nat, 0, sizeof nat);
    nat.tab = &tab;
    nat.pool = &pool;
    nat.run.ctx = 0;
    nat.run.admit = h_admit;
    nat.run.step = h_step;
    nat.run.now = h_now;
    for (unsigned i = 0; i < K_COUNT; i++)
        nat.images[i] = &imgs[i];
    nat.images[IMG_DENIED] = &img_denied;
    nat.ops = ops;
    nat.nops = sizeof ops / sizeof ops[0];
    osh_session_attach(s, &nat);
}

static struct osh_result line(struct osh_session *s, const char *text)
{
    struct osh_result r;
    reset_capture();
    build_line(text, 0);
    osh_op_exec(s, &senv, req, out, 8192, &r);
    return r;
}

static uint64_t le64(const uint8_t *b)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | b[i];
    return v;
}

static uint64_t fnv_pat(uint64_t total, uint8_t key)
{
    uint64_t h = 14695981039346656037ull;
    for (uint64_t i = 0; i < total; i++)
        h = (h ^ (uint8_t)(pat(i) ^ key)) * 1099511628211ull;
    return h;
}

static unsigned open_intents(const struct osh_session *s)
{
    unsigned n = 0;
    for (unsigned i = 0; i < OSH_INTENT_MAX; i++)
        n += s->intents[i].used && !s->intents[i].closed;
    return n;
}

/* between pipelines: no task, no pipe, the session table holds only its authority (console and spawn) */
static int held(const struct ck_cap_table *t, struct ck_handle h)
{
    return h.generation && h.index < CK_CAP_SLOTS && t->slots[h.index].live && !t->slots[h.index].tombstone &&
           t->slots[h.index].generation == h.generation;
}

static int idle(const struct osh_session *s)
{
    unsigned live = 0;
    for (unsigned i = 0; i < CK_CAP_SLOTS; i++)
        live += s->caps.slots[i].live && !s->caps.slots[i].tombstone;
    unsigned want = (unsigned)held(&s->caps, s->console_cap) + (unsigned)held(&s->caps, s->spawn_cap);
    return ck_task_tab_empty(&tab) && ck_pipe_pool_empty(&pool) && live == want && !s->live.active &&
           open_intents(s) == 0;
}

static int count_logs(const char *needle)
{
    int n = 0;
    for (int i = 0; i < nlogs; i++)
        n += strstr(logs[i], needle) != 0;
    return n;
}

static void part1(void)
{
    struct osh_session s, t;
    struct osh_result r;
    const char *const hello[] = { "klog", "hello", "world", 0 };

    /* allowed: the task holds WRITE on the console resource */
    reset_capture();
    osh_session_init(&s, CK_R_WRITE); J(&s);
    r = run(&s, hello);
    CHECK(r.error == OSH_E_OK && r.outcome == OSH_O_COMPLETED && r.status == 0 && !r.stop);
    CHECK(strcmp(cap_buf, "osh_op_out: hello world\n") == 0 && cap_calls == 1);
    CHECK(s.intents[0].used && s.intents[0].closed && s.intents[0].outcome == OSH_O_COMPLETED && s.intents[0].seq == 1);
    CHECK(memcmp(s.intents[0].digest, r.digest, 32) == 0);
    uint8_t d1[32];
    memcpy(d1, r.digest, 32);
    r = run(&s, hello);
    CHECK(memcmp(d1, r.digest, 32) == 0 && r.seq == 2); /* the same request, the same digest */
    const char *const other[] = { "klog", "hello", "worlD", 0 };
    r = run(&s, other);
    CHECK(memcmp(d1, r.digest, 32) != 0);

    /* no capability at all: refused before any effect, with the named refusal */
    reset_capture();
    osh_session_init(&s, 0); J(&s);
    r = run(&s, hello);
    CHECK(r.error == OSH_E_DENIED && r.outcome == OSH_O_FAILED_NO_EFFECT && r.status == 126);
    CHECK(cap_calls == 0 && cap_buf[0] == 0);
    CHECK(s.intents[0].closed && s.intents[0].outcome == OSH_O_FAILED_NO_EFFECT && s.intents[0].err == OSH_E_DENIED);
    CHECK(nlogs == 1 && strstr(logs[0], "osh_op_receipt:") && strstr(logs[0], "result=DENIED") && strstr(logs[0], "effect=none"));

    /* a capability without WRITE: denied */
    reset_capture();
    osh_session_init(&s, CK_R_READ); J(&s);
    r = run(&s, hello);
    CHECK(r.error == OSH_E_DENIED && cap_calls == 0);

    /* revoked between building the request and using it */
    reset_capture();
    osh_session_init(&s, CK_R_WRITE); J(&s);
    s.hooks.revoke_before_perm_at = 1;
    r = run(&s, hello);
    CHECK(r.error == OSH_E_REVOKED && r.outcome == OSH_O_FAILED_NO_EFFECT && cap_calls == 0);
    r = run(&s, hello); /* and it stays revoked */
    CHECK(r.error == OSH_E_REVOKED && cap_calls == 0);

    /* the binding checks, called directly */
    {
        struct osh_binding b;
        struct ck_handle h2;
        osh_session_init(&s, CK_R_WRITE); J(&s);
        memset(&b, 0, sizeof b);
        memcpy(b.principal_id, s.principal, 32);
        b.domain = 1;
        b.cap_index = s.console_cap.index;
        b.cap_generation = s.console_cap.generation;
        b.resource_class = OSH_RC_CONSOLE_LOG;
        b.operation = OSH_OP_WRITE;
        CHECK(osh_op_perm_check(&s, &b) == OSH_E_OK);
        b.domain = 2;
        CHECK(osh_op_perm_check(&s, &b) == OSH_E_CAP_DOMAIN_MISMATCH);
        b.domain = 1;
        b.cap_generation = 0x100000001ull;
        CHECK(osh_op_perm_check(&s, &b) == OSH_E_CAP_GEN_NARROW);
        b.cap_generation = s.console_cap.generation;
        b.resource_class = 2;
        CHECK(osh_op_perm_check(&s, &b) == OSH_E_DENIED);
        b.resource_class = OSH_RC_CONSOLE_LOG;
        b.operation = 2;
        CHECK(osh_op_perm_check(&s, &b) == OSH_E_DENIED);
        b.operation = OSH_OP_WRITE;
        b.principal_id[0] ^= 1;
        CHECK(osh_op_perm_check(&s, &b) == OSH_E_DENIED);
        b.principal_id[0] ^= 1;
        b.cap_generation = s.console_cap.generation + 5; /* never issued */
        CHECK(osh_op_perm_check(&s, &b) == OSH_E_DENIED);
        b.cap_generation = s.console_cap.generation;
        b.cap_index = 99;
        CHECK(osh_op_perm_check(&s, &b) == OSH_E_DENIED);
        b.cap_index = s.console_cap.index;
        /* revoke, then the old handle is REVOKED; a new capability in the same slot makes the old handle STALE */
        CHECK(ck_cap_remove(&s.caps, s.console_cap) == CK_CAP_OK);
        CHECK(osh_op_perm_check(&s, &b) == OSH_E_REVOKED);
        CHECK(ck_cap_insert(&s.caps, OSH_RES_CONSOLE_LOG, CK_R_WRITE, &h2) == CK_CAP_OK);
        CHECK(h2.index == b.cap_index && h2.generation > s.console_cap.generation);
        CHECK(osh_op_perm_check(&s, &b) == OSH_E_STALE);
        /* a capability on another resource does not authorize the console */
        osh_session_init(&s, 0); J(&s);
        CHECK(ck_cap_insert(&s.caps, 0x1234u, CK_R_WRITE, &s.console_cap) == CK_CAP_OK);
        memcpy(s.cap_holder, s.principal, 32); /* the principal matches: only the resource is wrong */
        r = run(&s, hello);
        CHECK(r.error == OSH_E_DENIED && cap_calls == 0);
    }

    /* an interrupt before the effect: cancelled, no bytes, the list stops */
    reset_capture();
    osh_session_init(&s, CK_R_WRITE); J(&s);
    s.hooks.interrupt_before_effect_at = 2;
    r = run(&s, hello);
    CHECK(r.error == OSH_E_OK && cap_calls == 1);
    r = run(&s, hello);
    CHECK(r.error == OSH_E_INTERRUPTED && r.outcome == OSH_O_CANCELLED && r.stop && r.status == 130 && cap_calls == 1);
    CHECK(s.intents[1].closed && s.intents[1].outcome == OSH_O_CANCELLED);
    CHECK(osh_session_close(&s) == 1);
    CHECK(ck_cap_lookup(&s.caps, s.console_cap, CK_R_WRITE, 0) != CK_CAP_OK); /* capability released */

    /* lost after the effect: the outcome is unknown, restart reports it and never replays it */
    reset_capture();
    osh_session_init(&s, CK_R_WRITE); J(&s);
    s.hooks.crash_after_effect_at = 1;
    r = run(&s, hello);
    CHECK(r.crashed && r.stop && r.outcome == OSH_O_OUTCOME_UNKNOWN && cap_calls == 1);
    CHECK(s.intents[0].used && !s.intents[0].closed);
    osh_session_restart(&t, &s); JR(&t);
    CHECK(t.console_cap.generation == 0); /* the restarted adapter holds no capability */
    CHECK(t.epoch == s.epoch + 1);
    nlogs = 0;
    CHECK(osh_op_recover(&t, &env) == 1);
    CHECK(cap_calls == 1); /* not replayed */
    /* #431: nothing is inherited from memory; the journal says it: one console intent, no STARTED (a console write has
     * none), now OUTCOME_UNKNOWN written by recovery */
    CHECK(!t.intents[0].used && t.recovery.unknown_intent == 1 && t.recovery.unknown_started == 0);
    {
        struct oshj_report rp;
        oshj_inspect(&J0.cx, &rp, 0, 0);
        CHECK(rp.intents == 1 && rp.cat[OSHJ_CAT_RECOVERED] == 1 && rp.outcomes == 1 && rp.receipts == 1 && rp.bad == 0);
    }
    CHECK(nlogs == 1 && strstr(logs[0], "outcome=OUTCOME_UNKNOWN") && strstr(logs[0], "replayed=0"));
    CHECK(osh_op_recover(&t, &env) == 0 && nlogs == 1 && cap_calls == 1); /* idempotent */

    /* a failed console write is unknown, not "no effect" */
    reset_capture();
    osh_session_init(&s, CK_R_WRITE); J(&s);
    s.hooks.sink_fail_at = 1;
    r = run(&s, hello);
    CHECK(r.error == OSH_E_IO && r.outcome == OSH_O_OUTCOME_UNKNOWN && s.intents[0].outcome == OSH_O_OUTCOME_UNKNOWN);

    /* refused before any effect or intent: unknown name, and the forms klog does not have.
     * aienos-mv#430: the old "ncmds != 1 is NOT_SUPPORTED" assertion now means "klog cannot be a pipeline stage" (a
     * pipeline of stage operations runs, Part 2); builtins and assignments are still refused for every command. The
     * redirection and assignment records below carry real entries: an empty entry under a nonzero count is now a
     * malformed record (INVALID_ARG), tested with the other malformed records. */
    reset_capture();
    osh_session_init(&s, CK_R_WRITE); J(&s);
    const char *const ls[] = { "ls", 0 };
    r = run(&s, ls);
    CHECK(r.error == OSH_E_NOT_FOUND && r.status == 127 && r.outcome == OSH_O_NOT_STARTED && !s.intents[0].used);
    const char *const kl[] = { "klog", "a", 0 };
    build(2, kl, kl);
    osh_op_exec(&s, &env, req, out, 8192, &r);
    CHECK(r.error == OSH_E_NOT_SUPPORTED && r.status == 126 && cap_calls == 0 && !s.intents[0].used); /* klog in a pipe */
    CHECK(r.cmd_outcome[0] == OSH_O_FAILED_NO_EFFECT && r.cmd_outcome[1] == OSH_O_FAILED_NO_EFFECT);
    build_line("klog a >f", 0);
    osh_op_exec(&s, &env, req, out, 8192, &r);
    CHECK(r.error == OSH_E_NOT_SUPPORTED && cap_calls == 0); /* klog with a redirection */
    build_line("X=1 klog a", 0);
    osh_op_exec(&s, &env, req, out, 8192, &r);
    CHECK(r.error == OSH_E_NOT_SUPPORTED && cap_calls == 0); /* a prefix assignment */
    build(1, kl, 0);
    req[8 + 3] = 3; /* builtin printf */
    osh_op_exec(&s, &env, req, out, 8192, &r);
    CHECK(r.error == OSH_E_NOT_SUPPORTED && cap_calls == 0);
    CHECK(!s.intents[0].used);

    /* malformed request records are refused without using the memory */
    build(1, kl, 0);
    req[0] ^= 1;
    osh_op_exec(&s, &env, req, out, 8192, &r);
    CHECK(r.error == OSH_E_INVALID_ARG && cap_calls == 0);
    build(1, kl, 0);
    req[8 + 5] = 99; /* argv[0] length past out_used */
    osh_op_exec(&s, &env, req, out, 8192, &r);
    CHECK(r.error == OSH_E_INVALID_ARG && cap_calls == 0);
    build(1, kl, 0);
    out[0] = 0x14b; /* a cell that is not a byte */
    osh_op_exec(&s, &env, req, out, 8192, &r);
    CHECK(r.error == OSH_E_INVALID_ARG && cap_calls == 0);
    build(1, kl, 0);
    req[1] = 9; /* ncmds over 8 */
    osh_op_exec(&s, &env, req, out, 8192, &r);
    CHECK(r.error == OSH_E_INVALID_ARG && cap_calls == 0);
    build(1, kl, 0);
    req[4] = 9000; /* out_used over the OUT region */
    osh_op_exec(&s, &env, req, out, 8192, &r);
    CHECK(r.error == OSH_E_INVALID_ARG && cap_calls == 0);
    build(1, kl, 0);
    req[8 + 2] = 1; /* nredir 1 with an empty entry (kind 0) */
    osh_op_exec(&s, &env, req, out, 8192, &r);
    CHECK(r.error == OSH_E_INVALID_ARG && cap_calls == 0);
    build(1, kl, 0);
    req[8 + 1] = 1; /* nassign 1 with an empty name */
    osh_op_exec(&s, &env, req, out, 8192, &r);
    CHECK(r.error == OSH_E_INVALID_ARG && cap_calls == 0);
    CHECK(!s.intents[0].used);

    /* an argument cannot forge a record line, and an over-long line is a LIMIT before the effect */
    reset_capture();
    const char *const nl[] = { "klog", "a\nosh_op_record: forged", 0 };
    r = run(&s, nl);
    CHECK(r.error == OSH_E_OK && strcmp(cap_buf, "osh_op_out: a.osh_op_record: forged\n") == 0);
    char big[300];
    memset(big, 'x', 299);
    big[299] = 0;
    const char *const bg[] = { "klog", big, 0 };
    reset_capture();
    r = run(&s, bg);
    CHECK(r.error == OSH_E_LIMIT && cap_calls == 0);

    /* closed intents are reclaimed (their outcome is already logged): 40 sequential klogs succeed, each with a
     * "grant used" receipt; only OPEN intents count against the limit */
    reset_capture();
    osh_session_init(&s, CK_R_WRITE); J(&s);
    int okn = 0;
    for (int i = 0; i < 40; i++)
        okn += run(&s, hello).error == OSH_E_OK;
    CHECK(okn == 40 && cap_calls == 40);
    CHECK(nlogs == 40);
    CHECK(strstr(logs[0], "osh_op_receipt: seq=1 op=console_write result=OK ") && strstr(logs[0], "effect=console_write"));
    CHECK(strstr(logs[0], "cap_domain=1 cap_index=0 cap_generation=1 resource_class=1 operation=1") && strstr(logs[0], "principal="));
    /* the receipt names the request digest */
    {
        reset_capture();
        osh_session_init(&s, CK_R_WRITE); J(&s);
        r = run(&s, hello);
        static const char hex[] = "0123456789abcdef";
        char want[64 + 1];
        for (int i = 0; i < 32; i++) {
            want[2 * i] = hex[r.digest[i] >> 4];
            want[2 * i + 1] = hex[r.digest[i] & 0xf];
        }
        want[64] = '\0';
        CHECK(nlogs == 1 && strstr(logs[0], want));
    }
    /* 16 open intents (the adapter lost after each effect) fill the log; the next pipeline is refused LIMIT with a receipt */
    reset_capture();
    osh_session_init(&s, CK_R_WRITE); J(&s);
    for (int i = 0; i < OSH_INTENT_MAX; i++) {
        s.hooks.crash_after_effect_at = s.pipelines + 1;
        r = run(&s, hello);
        CHECK(r.crashed);
    }
    CHECK(cap_calls == OSH_INTENT_MAX);
    nlogs = 0;
    r = run(&s, hello);
    CHECK(r.error == OSH_E_LIMIT && r.outcome == OSH_O_NOT_STARTED && cap_calls == OSH_INTENT_MAX);
    CHECK(nlogs == 1 && strstr(logs[0], "osh_op_receipt:") && strstr(logs[0], "result=LIMIT") && strstr(logs[0], "effect=none"));
    /* the denial receipts carry the binding too */
    reset_capture();
    osh_session_init(&s, 0); J(&s);
    r = run(&s, hello);
    CHECK(nlogs == 1 && strstr(logs[0], "result=DENIED") && strstr(logs[0], "cap_domain=1") && strstr(logs[0], "principal="));

    /* the capability belongs to the principal it was granted to: another principal presenting it is DENIED */
    reset_capture();
    osh_session_init(&s, CK_R_WRITE); J(&s);
    CHECK(memcmp(s.cap_holder, s.principal, 32) == 0);
    s.principal[0] ^= 1; /* a different task now runs the shell session */
    r = run(&s, hello);
    CHECK(r.error == OSH_E_DENIED && cap_calls == 0);
    osh_session_init(&s, 0); J(&s);
    {
        static const uint8_t zero32[32];
        CHECK(memcmp(s.cap_holder, zero32, 32) == 0); /* nothing granted, no holder */
    }

    /* names */
    CHECK(strcmp(osh_err_name(OSH_E_PARTIAL_LAUNCH), "PARTIAL_LAUNCH") == 0 && strcmp(osh_err_name(99), "IO") == 0);
    CHECK(strcmp(osh_outcome_name(OSH_O_CANCELLED), "CANCELLED") == 0);
}

static struct osh_session S, T;

static void part2(void)
{
    struct osh_session *s = &S;
    struct osh_result r;
    uint8_t mk = (uint8_t)(OSH_FD_LOC_OUT << 4);

    /* producer | transform | consumer: 2.5 pipe capacities through two pipes, no location used (no temporary file) */
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    s->hooks.snapshot_at = 1;
    s->hooks.snapshot_after_turns = 40;
    uint64_t st0 = steps_run;
    r = line(s, "gen 163840 4096 | xform 90 | sink");
    CHECK(r.launch_error == OSH_E_OK && r.error == OSH_E_OK && r.outcome == OSH_O_COMPLETED && r.status == 0);
    CHECK(r.ncmds == 3 && r.started == 3 && r.cmd_status[0] == 0 && r.cmd_status[1] == 0 && r.cmd_status[2] == 0);
    CHECK(r.cmd_outcome[0] == OSH_O_COMPLETED && r.cmd_outcome[2] == OSH_O_COMPLETED);
    CHECK(so_len == 16 && le64(so_buf) == 163840 && le64(so_buf + 8) == fnv_pat(163840, 90)); /* EOF reached, exact */
    CHECK(r.stdout_bytes == 16);
    CHECK(r.max_resident > 0 && r.max_resident <= CK_PIPE_CAPACITY); /* bounded: never more than one pipe's worth */
    CHECK(r.blocks > 0 && r.wakeups > 0 && r.steps == steps_run - st0);
    CHECK(r.mid_alive == 3 && r.mid_in_flight > 0); /* all three alive at once with bytes in flight */
    CHECK(r.cmd_fd[0][0] == 0 && r.cmd_fd[0][1] == ((OSH_FD_PIPE_OUT << 4) | 0) && r.cmd_fd[1][0] == ((OSH_FD_PIPE_IN << 4) | 0));
    CHECK(r.cmd_fd[2][1] == (OSH_FD_STDOUT << 4));
    for (unsigned i = 0; i < OSH_LOC_MAX; i++)
        CHECK(!s->locs[i].used);
    CHECK(count_logs("op=task_spawn result=OK") == 3 && count_logs("effect=spawn") == 3);
    CHECK(idle(s));
    for (unsigned i = 0; i < OSH_INTENT_MAX; i++)
        if (s->intents[i].used)
            CHECK(s->intents[i].closed && s->intents[i].outcome == OSH_O_COMPLETED && s->intents[i].effect == OSH_EF_SPAWN);

    /* the pipeline status is the last command's (ST7 step 7) */
    r = line(s, "exit 3 | cat");
    CHECK(r.status == 0 && r.cmd_status[0] == 3 && r.cmd_status[1] == 0 && idle(s));
    r = line(s, "exit 0 | exit 5");
    CHECK(r.status == 5 && r.cmd_status[0] == 0 && r.cmd_status[1] == 5 && idle(s));
    /* a consumer that ends early: the producer gets BROKEN_PIPE (141), the status is the consumer's */
    r = line(s, "flood | exit 0");
    CHECK(r.status == 0 && r.cmd_status[0] == 141 && r.cmd_status[1] == 0 && idle(s));
    r = line(s, "gen 200000 | xform 0 50000 7 | drain");
    CHECK(r.status == 0 && r.cmd_status[0] == 141 && r.cmd_status[1] == 7 && r.cmd_status[2] == 0 && idle(s));

    /* a name not found ends only that command (127); its neighbours see EOF and the others run (ST7 step 5) */
    r = line(s, "nosuch | cat");
    CHECK(r.launch_error == OSH_E_OK && r.status == 0 && r.cmd_status[0] == 127 && r.cmd_status[1] == 0);
    CHECK(r.cmd_outcome[0] == OSH_O_FAILED_NO_EFFECT && r.cmd_error[0] == OSH_E_NOT_FOUND && r.outcome == OSH_O_COMPLETED);
    CHECK(idle(s));
    r = line(s, "exit 2 | nosuch");
    CHECK(r.status == 127 && r.cmd_status[0] == 2 && r.cmd_status[1] == 127 && idle(s));
    r = line(s, "gen 100000 | nosuch | sink");
    CHECK(r.cmd_status[0] == 141 && r.cmd_status[1] == 127 && r.cmd_status[2] == 0 && so_len == 16 && le64(so_buf) == 0);
    CHECK(idle(s));
    r = line(s, "nosuch | alsonot");
    CHECK(r.error == OSH_E_NOT_FOUND && r.status == 127 && r.outcome == OSH_O_NOT_STARTED && r.started == 0 && idle(s));

    /* a unit whose receipt the runner refuses: that command 126, the others run */
    r = line(s, "gen 1000 | badunit");
    CHECK(r.status == 126 && r.cmd_error[1] == OSH_E_DENIED && r.cmd_status[0] == 141 && idle(s));
    CHECK(count_logs("cmd=1 op=task_spawn result=DENIED") == 1);

    /* revoked between two stages: the first ran, the second is refused REVOKED at its own effect */
    s->hooks.revoke_spawn_at = s->pipelines + 1;
    s->hooks.revoke_spawn_cmd = 1;
    r = line(s, "exit 4 | exit 5");
    CHECK(r.launch_error == OSH_E_OK && r.status == 126 && r.cmd_status[0] == 4 && r.cmd_status[1] == 126);
    CHECK(r.cmd_error[1] == OSH_E_REVOKED && r.cmd_outcome[1] == OSH_O_FAILED_NO_EFFECT && r.error == OSH_E_REVOKED);
    CHECK(count_logs("cmd=1 op=task_spawn result=REVOKED") == 1 && idle(s));
    r = line(s, "exit 0");
    CHECK(r.launch_error == OSH_E_REVOKED && r.status == 126 && r.started == 0 && idle(s)); /* it stays revoked */
    osh_session_close(s);

    /* whole-pipeline refusals happen before any effect: nothing starts, no intent is written (ST7 steps 1 to 4) */
    native(s, 0, 0);
    uint64_t s0 = steps_run;
    r = line(s, "gen 10 | sink");
    CHECK(r.launch_error == OSH_E_DENIED && r.status == 126 && r.outcome == OSH_O_FAILED_NO_EFFECT && r.started == 0);
    CHECK(r.cmd_status[0] == 126 && r.cmd_status[1] == 126 && r.cmd_outcome[1] == OSH_O_FAILED_NO_EFFECT);
    CHECK(count_logs("op=task_spawn result=DENIED") == 1 && !s->intents[0].used && steps_run == s0);
    osh_session_close(s);
    native(s, CK_R_READ, 0); /* a spawn capability without WRITE */
    r = line(s, "gen 10");
    CHECK(r.launch_error == OSH_E_DENIED && r.started == 0 && !s->intents[0].used);
    osh_session_close(s);
    native(s, CK_R_WRITE, 0);
    uint64_t steps_before = steps_run;
    r = line(s, "gen 10 | /bin/sh");
    CHECK(r.error == OSH_E_UNAVAILABLE && r.status == 126 && r.cmd_outcome[0] == OSH_O_FAILED_NO_EFFECT);
    r = line(s, "cat | foreign");
    CHECK(r.error == OSH_E_UNAVAILABLE && r.status == 126 && r.cmd_status[0] == 126 && r.cmd_status[1] == 126);
    r = line(s, "@1 cd | cat");
    CHECK(r.error == OSH_E_NOT_SUPPORTED && r.status == 126);
    r = line(s, "X=1 gen 10");
    CHECK(r.error == OSH_E_NOT_SUPPORTED);
    r = line(s, "gen abc");
    CHECK(r.error == OSH_E_INVALID_ARG && r.status == 70);
    r = line(s, "gen 18446744073709551616");
    CHECK(r.error == OSH_E_INVALID_ARG); /* 2^64 does not fit */
    r = line(s, "exit 1 2");
    CHECK(r.error == OSH_E_LIMIT && r.status == 1); /* more parameters than the stage takes */
    r = line(s, "cat | cat | cat | cat | cat | cat");
    CHECK(r.error == OSH_E_LIMIT && r.status == 1 && r.outcome == OSH_O_NOT_STARTED); /* 6 pipes: the pool has 4 */
    CHECK(steps_run == steps_before && !s->intents[0].used && idle(s) && count_logs("result=OK") == 0);
    r = line(s, "cat | cat | cat | sink");
    CHECK(r.status == 0 && r.started == 4 && so_len == 16 && le64(so_buf) == 0 && idle(s)); /* 3 pipes + stdout: the pool */
    r = line(s, "cat | cat | cat | cat | sink");
    CHECK(r.error == OSH_E_LIMIT && r.started == 0 && idle(s)); /* 4 pipes + stdout: over the pool, LIMIT up front */
    osh_session_close(s);

    /* redirections, in record order (section 7.2), through the session's location capability */
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    r = line(s, "gen 5000 700 >o");
    const struct osh_loc *lo = osh_loc_find(s, (const uint8_t *)"o", 1);
    CHECK(r.status == 0 && lo && lo->len == 5000 && so_len == 0 && r.cmd_fd[0][1] == mk);
    {
        int okb = 1;
        for (uint64_t i = 0; lo && i < 5000; i++)
            okb &= lo->bytes[i] == pat(i);
        CHECK(okb);
    }
    CHECK(count_logs("cmd=0 op=location_open_write result=OK") == 1 && idle(s));
    r = line(s, "xform 0 <o | sink");
    CHECK(r.status == 0 && so_len == 16 && le64(so_buf) == 5000 && le64(so_buf + 8) == fnv_pat(5000, 0) && idle(s));
    CHECK(r.cmd_fd[0][0] == (uint8_t)(OSH_FD_LOC_IN << 4));
    r = line(s, "gen 5000 >>o");
    CHECK(r.status == 0 && lo->len == 10000 && lo->bytes[5000] == pat(0) && idle(s)); /* append */
    r = line(s, "gen 100 >o 2>&1");
    CHECK(r.cmd_fd[0][1] == mk && r.cmd_fd[0][2] == mk && lo->len == 100); /* both go to o */
    r = line(s, "gen 100 2>&1 >o");
    CHECK(r.cmd_fd[0][1] == mk && r.cmd_fd[0][2] == (OSH_FD_STDOUT << 4)); /* 2 went where 1 was: the stdout */
    CHECK(so_len == 0 && lo->len == 100 && idle(s));
    r = line(s, "gen 100 >p >o");
    CHECK(r.status == 0 && osh_loc_find(s, (const uint8_t *)"p", 1)->len == 0 && lo->len == 100 && idle(s));
    r = line(s, "cat <nope | cat");
    CHECK(r.cmd_status[0] == 1 && r.cmd_error[0] == OSH_E_NOT_FOUND && r.cmd_status[1] == 0 && r.status == 0 && idle(s));
    r = line(s, "gen 200000 4096 >big"); /* more than the pipe and the location together */
    CHECK(r.cmd_error[0] == OSH_E_LIMIT && r.cmd_status[0] == 141 && osh_loc_find(s, (const uint8_t *)"big", 3)->len ==
          OSH_LOC_BYTES && idle(s)); /* never a silent cut: LIMIT, and the writer saw BROKEN_PIPE */
    r = line(s, "nosuch >o");
    CHECK(r.cmd_status[0] == 127 && lo->len == 100 && idle(s)); /* the name is decided before the open: o untouched */
    r = line(s, "gen 10 >o56789012345678901234567890123456789012");
    CHECK(r.cmd_status[0] == 1 && r.cmd_error[0] == OSH_E_LIMIT && idle(s)); /* a location name longer than 32 */
    /* the location capability is checked at the moment of each open */
    s->hooks.revoke_loc_at = s->pipelines + 1;
    s->hooks.revoke_loc_cmd = 1;
    r = line(s, "gen 10 >a | gen 10 >b");
    CHECK(r.cmd_status[0] == 0 && r.cmd_status[1] == 1 && r.cmd_error[1] == OSH_E_REVOKED && idle(s));
    CHECK(osh_loc_find(s, (const uint8_t *)"a", 1) && !osh_loc_find(s, (const uint8_t *)"b", 1));
    CHECK(count_logs("cmd=1 op=location_open_write result=REVOKED") == 1);
    osh_session_close(s);
    /* descriptor 2 goes nowhere in stage ABI v1 (UNVERIFIED U17): `2>e` creates e empty, stdout is untouched */
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    r = line(s, "gen 10 2>e");
    CHECK(r.status == 0 && osh_loc_find(s, (const uint8_t *)"e", 1) && osh_loc_find(s, (const uint8_t *)"e", 1)->len == 0 &&
          so_len == 10 && idle(s));
    osh_session_close(s);
    native(s, CK_R_WRITE, 0); /* no location capability */
    r = line(s, "gen 10 >o");
    CHECK(r.cmd_status[0] == 1 && r.cmd_error[0] == OSH_E_DENIED && !osh_loc_find(s, (const uint8_t *)"o", 1) && idle(s));
    osh_session_close(s);
    native(s, CK_R_WRITE, CK_R_READ); /* read only */
    r = line(s, "gen 10 >o");
    CHECK(r.cmd_status[0] == 1 && r.cmd_error[0] == OSH_E_DENIED && !osh_loc_find(s, (const uint8_t *)"o", 1) && idle(s));
    CHECK(count_logs("op=location_open_write result=DENIED") == 1);
    osh_session_close(s);

    /* a launch failure (fault-spawn): what started is cancelled and reaped, PARTIAL_LAUNCH, status 1 (ST7 step 6) */
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    s->hooks.fail_spawn_at = s->pipelines + 1;
    s->hooks.fail_spawn_cmd = 2;
    s->hooks.fail_spawn_err = OSH_E_LIMIT;
    r = line(s, "gen 0 | honor | sink");
    CHECK(r.launch_error == OSH_E_PARTIAL_LAUNCH && r.error == OSH_E_PARTIAL_LAUNCH && r.status == 1 && r.started == 2);
    CHECK(r.cmd_outcome[0] == OSH_O_CANCELLED && r.cmd_outcome[1] == OSH_O_CANCELLED && r.cmd_outcome[2] == OSH_O_FAILED_NO_EFFECT);
    /* honor exits 42; gen, in its interrupt grace, finds no reader: its output is dropped and it ends by the interrupt
     * (130). Before aienos-mv#432 (task.c T1) it ended 141, or 130 when the round robin reached it first. */
    CHECK(r.cmd_status[0] == 130 && r.cmd_status[1] == 42 && r.cmd_status[2] == 1);
    CHECK(idle(s));
    s->hooks.fail_spawn_at = s->pipelines + 1;
    s->hooks.fail_spawn_cmd = 1;
    s->hooks.fail_spawn_err = OSH_E_IO;
    r = line(s, "gen 0 | cat | cat");
    CHECK(r.error == OSH_E_PARTIAL_LAUNCH && r.cmd_outcome[1] == OSH_O_FAILED_NO_EFFECT && r.cmd_outcome[2] == OSH_O_NOT_STARTED);
    CHECK(r.cmd_status[2] == -1 && idle(s));
    s->hooks.fail_spawn_at = s->pipelines + 1;
    s->hooks.fail_spawn_cmd = 0;
    s->hooks.fail_spawn_err = OSH_E_LIMIT;
    r = line(s, "gen 0 | cat");
    CHECK(r.error == OSH_E_LIMIT && r.status == 1 && r.started == 0 && r.cmd_outcome[0] == OSH_O_FAILED_NO_EFFECT);
    CHECK(r.cmd_outcome[1] == OSH_O_NOT_STARTED && idle(s));

    /* an interrupt while running: section 12 cleanup (interrupt, grace, terminate), every stage reaped */
    s->hooks.interrupt_at = s->pipelines + 1;
    s->hooks.interrupt_after_turns = 30;
    r = line(s, "gen 0 | honor");
    CHECK(r.error == OSH_E_INTERRUPTED && r.outcome == OSH_O_CANCELLED && r.stop && r.status == 42);
    CHECK(r.cmd_outcome[0] == OSH_O_CANCELLED && r.cmd_status[0] == 130 && r.cmd_outcome[1] == OSH_O_CANCELLED && idle(s)); /* T1 */
    /* a deadline does the same */
    nat.deadline_ticks = 50;
    r = line(s, "gen 0 | sink");
    CHECK(r.error == OSH_E_INTERRUPTED && r.stop && r.cmd_status[0] == 130 && r.cmd_status[1] == 130 && idle(s));
    nat.deadline_ticks = 0;
    r = line(s, "gen 10 | sink"); /* and the session goes on */
    CHECK(r.status == 0 && le64(so_buf) == 10 && idle(s));
    osh_session_close(s);

    /* lost right after an effect: the intents stay open, the kernel reaps what was left running, a restart reports
     * OUTCOME_UNKNOWN and never replays */
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    s->hooks.crash_after_effect_at = 1;
    s->hooks.crash_cmd = 1;
    uint64_t spawned0 = tab.stats.spawned;
    r = line(s, "gen 0 >w | cat | sink");
    CHECK(r.crashed && r.stop && r.outcome == OSH_O_OUTCOME_UNKNOWN && r.started == 2 && open_intents(s) == 3);
    CHECK(tab.stats.spawned == spawned0 + 2 && !ck_task_tab_empty(&tab));
    CHECK(osh_session_close(s) == 1); /* everything the lost adapter left is reaped and closed */
    osh_session_restart(&T, s); JR(&T);
    CHECK(T.epoch == s->epoch + 1 && T.spawn_cap.generation == 0 && T.nat == 0);
    nlogs = 0;
    CHECK(osh_op_recover(&T, &env) == 3 && nlogs == 3 && count_logs("replayed=0") == 3);
    CHECK(tab.stats.spawned == spawned0 + 2); /* nothing replayed */
    CHECK(osh_loc_find(&T, (const uint8_t *)"w", 1) != 0); /* the open happened: the location exists */
    CHECK(osh_op_recover(&T, &env) == 0);

    /* the direct-argv route (section 11) reaches the same executor and the same effect gate */
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    struct osh_result a, d;
    a = line(s, "gen 70000 3000 | xform 7 | sink");
    uint8_t sa[16];
    memcpy(sa, so_buf, 16);
    reset_capture();
    build_line("gen 70000 3000 | xform 7 | sink", 1);
    osh_op_exec(s, &senv, req, out, 8192, &d);
    CHECK(a.status == 0 && d.status == 0 && memcmp(sa, so_buf, 16) == 0 && le64(so_buf) == 70000);
    CHECK(a.started == 3 && d.started == 3 && count_logs("op=task_spawn result=OK") == 3);
    CHECK(memcmp(a.digest, d.digest, 32) != 0); /* the flags cell differs, so the request digest does */
    build_line("@3 gen 1", 1);
    osh_op_exec(s, &senv, req, out, 8192, &d);
    CHECK(d.error == OSH_E_INVALID_ARG); /* a builtin on a direct submission is malformed (section 11) */
    build_line("gen 1", 1);
    out[0] = 0; /* a NUL in argv[0] */
    osh_op_exec(s, &senv, req, out, 8192, &d);
    CHECK(d.error == OSH_E_INVALID_ARG && idle(s));
    /* the same direct record without a spawn capability is refused exactly like the shell's */
    osh_session_close(s);
    native(s, 0, 0);
    reset_capture();
    build_line("gen 1 | sink", 1);
    osh_op_exec(s, &senv, req, out, 8192, &d);
    CHECK(d.launch_error == OSH_E_DENIED && d.started == 0 && count_logs("op=task_spawn result=DENIED") == 1);
    osh_session_close(s);

    /* malicious records: refused before any effect, nothing started, nothing allocated */
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    uint64_t sp = tab.stats.spawned;
    const char *bad[] = { "kind", "fd", "dupsrc", "dupt", "beyond", "off2", "conn", "redirlen" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        reset_capture();
        build_line("gen 10 | xform 1 >o | sink", 0);
        uint64_t *c1 = req + 8 + 164, *rd = c1 + 4 + 64 + 64;
        switch (i) {
        case 0: rd[0] = 9; break;              /* redirection kind 9 */
        case 1: rd[1] = 3; break;              /* descriptor 3 */
        case 2: rd[0] = 4; rd[2] = 5; rd[3] = 0; break; /* N>&5 */
        case 3: rd[0] = 4; rd[2] = 1; rd[3] = 1; break; /* a dup with a target length */
        case 4: c1[4 + 2 * 5] = 1; break;      /* an argv entry beyond nargv */
        case 5: (req + 8 + 2 * 164)[4] = 9999; break; /* command 2's argv[0] outside OUT */
        case 6: req[2] = 4; break;             /* connector 4 */
        case 7: rd[3] = 0; break;              /* a file redirection with no target */
        }
        osh_op_exec(s, &senv, req, out, 8192, &r);
        CHECK(r.error == OSH_E_INVALID_ARG && r.started == 0 && nlogs == 0);
    }
    CHECK(tab.stats.spawned == sp && !osh_loc_find(s, (const uint8_t *)"o", 1) && idle(s) && !s->intents[0].used);
    CHECK(osh_session_close(s) == 1);
}

/* ---- Part 3: the shared launch vectors ---- */
#ifndef OSH_EXEC_VEC_DIR
#define OSH_EXEC_VEC_DIR "tests/fixtures/osh_exec"
#endif
#ifndef OSH_EXEC_IMPL
#define OSH_EXEC_IMPL "unknown"
#endif
#define CORPUS "aien-protocols-mv@181855c9ff3ec9bcc635d2043d5d7d213cee6021"

static void q_fmt(const uint8_t *b, size_t n, char *o, size_t cap)
{
    size_t k = 0;
    o[k++] = '"';
    for (size_t i = 0; i < n && k + 6 < cap; i++) {
        if (b[i] == '"' || b[i] == '\\') {
            o[k++] = '\\';
            o[k++] = (char)b[i];
        } else if (b[i] >= 0x20 && b[i] <= 0x7e) {
            o[k++] = (char)b[i];
        } else {
            k += (size_t)snprintf(o + k, cap - k, "\\x%02x", b[i]);
        }
    }
    o[k++] = '"';
    o[k] = 0;
}

static size_t q_parse(const char *p, uint8_t *o, size_t cap, const char **end)
{
    size_t n = 0;
    if (*p != '"')
        return (size_t)-1;
    for (p++; *p && *p != '"' && n < cap; p++) {
        if (*p == '\\' && p[1] == 'x') {
            char h[3] = { p[2], p[3], 0 };
            o[n++] = (uint8_t)strtoul(h, 0, 16);
            p += 3;
        } else if (*p == '\\') {
            o[n++] = (uint8_t)*++p;
        } else {
            o[n++] = (uint8_t)*p;
        }
    }
    *end = *p == '"' ? p + 1 : p;
    return n;
}

struct vres {
    char name[8];
    struct osh_result r;
    uint8_t out[256];
    size_t out_len;
};

static unsigned err_code(const char *n)
{
    for (unsigned e = 0; e < OSH_E_COUNT; e++)
        if (strcmp(osh_err_label(e), n) == 0)
            return e;
    return OSH_E_IO;
}

/* one vector: 1 PASS, 0 MISMATCH, -1 NOT_RUN (reason printed) */
static int run_vector(const char *path, const char *file)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("NOT_RUN %s cannot open\n", file);
        return -1;
    }
    char lines[64][320];
    int nl = 0;
    while (nl < 64 && fgets(lines[nl], sizeof lines[0], f)) {
        lines[nl][strcspn(lines[nl], "\n")] = 0;
        nl++;
    }
    fclose(f);
    /* header: authority domain and features; every command and option must be one this runner implements */
    static const char *const feats[] = { "tasks", "launch", "test-authority", "fault-spawn" };
    for (int i = 0; i < nl; i++) {
        const char *L = lines[i];
        if (!strncmp(L, "domain ", 7) && strcmp(L + 7, "1") != 0) {
            printf("NOT_RUN %s domain %s (this adapter's spawn authority is the kernel IPC table, domain 1)\n", file, L + 7);
            return -1;
        }
        if (!strncmp(L, "needs ", 6)) {
            char buf[200];
            snprintf(buf, sizeof buf, "%s", L + 6);
            for (char *w = strtok(buf, " "); w; w = strtok(0, " ")) {
                int ok = 0;
                for (unsigned k = 0; k < sizeof feats / sizeof feats[0]; k++)
                    ok |= strcmp(w, feats[k]) == 0;
                if (!ok) {
                    printf("NOT_RUN %s needs %s\n", file, w);
                    return -1;
                }
            }
        }
        char cmd[320]; /* only the command side: the expected result after " => " may quote anything */
        snprintf(cmd, sizeof cmd, "%.300s", L);
        char *arrow = strstr(cmd, " => ");
        if (arrow)
            *arrow = 0;
        if (strstr(cmd, " gen=") || strstr(cmd, " domain=") || strstr(cmd, " index=") || strstr(cmd, " none") ||
            strstr(cmd, " out=") || (!strncmp(cmd, "spawn ", 6) && strstr(cmd, " in="))) {
            printf("NOT_RUN %s line %d uses a binding override or stream slot this runner does not drive\n", file, i + 1);
            return -1;
        }
    }
    struct osh_session *s = &S;
    native(s, 0, CK_R_READ | CK_R_WRITE); /* the location capability carries `in=` bytes to the first command */
    struct vres res[8];
    int nres = 0;
    for (int i = 0; i < nl; i++) {
        const char *L = lines[i];
        if (!*L || L[0] == '#' || !strncmp(L, "case ", 5) || !strncmp(L, "kind ", 5) || !strncmp(L, "exec ", 5) ||
            !strncmp(L, "domain ", 7) || !strncmp(L, "needs ", 6))
            continue;
        char got[400] = "OK";
        const char *want = strstr(L, " => ");
        if (!strcmp(L, "check empty")) {
            if (!idle(s)) {
                printf("MISMATCH %s:%d got \"not empty\" want \"empty\"\n", file, i + 1);
                osh_session_close(s);
                return 0;
            }
            continue;
        }
        if (!want) {
            printf("NOT_RUN %s line %d: no expected result\n", file, i + 1);
            osh_session_close(s);
            return -1;
        }
        want += 4;
        if (!strncmp(L, "cap ", 4)) {
            if (osh_session_grant_spawn(s, CK_R_WRITE) != 0)
                snprintf(got, sizeof got, "LIMIT");
        } else if (!strncmp(L, "revokecap ", 10)) {
            ck_cap_remove(&s->caps, s->spawn_cap);
        } else if (!strncmp(L, "bind ", 5) || !strcmp(L, "run => OK")) {
            /* the binding is built by the adapter from what the session holds; `run`: launch already ran to the end */
        } else if (!strncmp(L, "launch ", 7) || !strncmp(L, "spawn ", 6)) {
            int is_spawn = L[0] == 's';
            char name[8] = { 0 };
            sscanf(L + (is_spawn ? 6 : 7), "%7s", name);
            const char *colon = is_spawn ? 0 : strstr(L, " : ");
            const char *opts = L + (is_spawn ? 6 : 7) + strlen(name);
            char text[340] = "";
            uint8_t inb[128];
            size_t inl = 0;
            int has_in = 0;
            const char *ip = strstr(opts, " in=");
            if (ip && (!colon || ip < colon)) {
                const char *e;
                inl = q_parse(ip + 4, inb, sizeof inb, &e);
                has_in = 1;
            }
            const char *rv = strstr(opts, " revoke="), *ij = strstr(opts, " inject=");
            if (rv && (!colon || rv < colon)) {
                s->hooks.revoke_spawn_at = s->pipelines + 1;
                s->hooks.revoke_spawn_cmd = (uint32_t)atoi(rv + 8) - 1u;
            }
            if (ij && (!colon || ij < colon)) {
                char en[24] = { 0 };
                s->hooks.fail_spawn_at = s->pipelines + 1;
                s->hooks.fail_spawn_cmd = (uint32_t)atoi(ij + 8) - 1u;
                sscanf(strchr(ij, ':') + 1, "%23s", en);
                s->hooks.fail_spawn_err = err_code(en);
            }
            /* the program list becomes a direct-argv record (section 11): "a b ! c" -> "a b | c" */
            const char *pl = is_spawn ? opts + 1 : colon + 3;
            size_t pn = is_spawn ? (size_t)(strstr(pl, " bind=") - pl) : (size_t)(want - 4 - pl);
            size_t k = 0;
            for (size_t j = 0; j < pn && k + 8 < sizeof text; j++)
                text[k++] = pl[j] == '!' ? '|' : pl[j];
            text[k] = 0;
            if (has_in) { /* stdin of the first command is these bytes, then end of stream: a location read */
                struct osh_loc *l = &s->locs[OSH_LOC_MAX - 1];
                memset(l, 0, sizeof *l);
                l->used = 1;
                l->name_len = 4;
                memcpy(l->name, "vin_", 4);
                memcpy(l->bytes, inb, inl);
                l->len = (uint32_t)inl;
                char t2[360];
                char *bar = strchr(text, '|');
                if (bar)
                    snprintf(t2, sizeof t2, "%.*s<vin_ %.300s", (int)(bar - text), text, bar);
                else
                    snprintf(t2, sizeof t2, "%.300s <vin_", text);
                snprintf(text, sizeof text, "%.330s", t2);
            }
            struct vres *v = &res[nres < 8 ? nres++ : 7];
            snprintf(v->name, sizeof v->name, "%s", name);
            reset_capture();
            build_line(text, 1);
            osh_op_exec(s, &senv, req, out, 8192, &v->r);
            v->out_len = so_len < sizeof v->out ? so_len : sizeof v->out;
            memcpy(v->out, so_buf, v->out_len);
            if (v->r.launch_error != OSH_E_OK) {
                size_t g = (size_t)snprintf(got, sizeof got, "%s", osh_err_name(v->r.launch_error));
                if (!is_spawn) {
                    g += (size_t)snprintf(got + g, sizeof got - g, " status=%d outcomes=", v->r.status);
                    for (unsigned c = 0; c < v->r.ncmds; c++)
                        g += (size_t)snprintf(got + g, sizeof got - g, "%s%s", c ? "," : "", osh_outcome_name(v->r.cmd_outcome[c]));
                    g += (size_t)snprintf(got + g, sizeof got - g, " statuses=");
                    for (unsigned c = 0; c < v->r.ncmds; c++) {
                        if (v->r.cmd_status[c] < 0)
                            g += (size_t)snprintf(got + g, sizeof got - g, "%s-", c ? "," : "");
                        else
                            g += (size_t)snprintf(got + g, sizeof got - g, "%s%d", c ? "," : "", v->r.cmd_status[c]);
                    }
                }
            }
        } else if (!strncmp(L, "waitp ", 6)) {
            char name[8] = { 0 };
            sscanf(L + 6, "%7s", name);
            struct vres *v = 0;
            for (int j = 0; j < nres; j++)
                if (!strcmp(res[j].name, name))
                    v = &res[j];
            if (!v) {
                snprintf(got, sizeof got, "STALE");
            } else {
                char q[300];
                size_t g = (size_t)snprintf(got, sizeof got, "OK status=%d outcomes=", v->r.status);
                for (unsigned c = 0; c < v->r.ncmds; c++)
                    g += (size_t)snprintf(got + g, sizeof got - g, "%s%s", c ? "," : "", osh_outcome_name(v->r.cmd_outcome[c]));
                g += (size_t)snprintf(got + g, sizeof got - g, " statuses=");
                for (unsigned c = 0; c < v->r.ncmds; c++)
                    g += (size_t)snprintf(got + g, sizeof got - g, "%s%d", c ? "," : "", v->r.cmd_status[c]);
                q_fmt(v->out, v->out_len, q, sizeof q);
                snprintf(got + g, sizeof got - g, " out=%s", q);
            }
        } else {
            printf("NOT_RUN %s line %d: command not driven by this runner\n", file, i + 1);
            osh_session_close(s);
            return -1;
        }
        if (strcmp(got, want) != 0) {
            printf("MISMATCH %s:%d got \"%s\" want \"%s\"\n", file, i + 1, got, want);
            osh_session_close(s);
            return 0;
        }
    }
    osh_session_close(s);
    printf("PASS %s\n", file);
    return 1;
}

static void part3(void)
{
    static const char *const files[] = { "t001_task_stream_binding.vec", "t002_stage_nonzero_exit.vec",
                                         "t003_pipeline_broken_pipe.vec", "t004_task_cancel.vec",
                                         "t005_cancel_blocked_task.vec", "t006_partial_launch.vec",
                                         "t007_refusal_before_effects.vec", "t008_not_found_stage.vec",
                                         "t009_revoke_between_stages.vec", "t010_domain1_generation.vec",
                                         "t011_domain2_generation.vec", "t012_outcome_unknown_crash.vec",
                                         "t013_outcome_unknown_lost.vec", "t014_reset_cleanup.vec",
                                         "t015_spawn_argument_refusals.vec", "t016_eof_all_task_writers.vec" };
    unsigned n = sizeof files / sizeof files[0], pass = 0, fail = 0, not_run = 0;
    int got[16];
    printf("OSHSV1_RUN impl=aienos-mv@%s corpus=%s toolchain=gcc %s target=host\n", OSH_EXEC_IMPL, CORPUS, __VERSION__);
    for (unsigned i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", OSH_EXEC_VEC_DIR, files[i]);
        got[i] = run_vector(path, files[i]);
        pass += got[i] == 1;
        fail += got[i] == 0;
        not_run += got[i] < 0;
    }
    printf("OSHSV1_CONFORMANCE impl=aienos-osh-op vectors=%u pass=%u fail=%u not_run=%u\n", n, pass, fail, not_run);
    CHECK(fail == 0);
    CHECK(got[1] == 1 && got[7] == 1 && got[8] == 1); /* t002, t008, t009: the domain-1 launch vectors */
}

/* ---- Part 4 (aienos-mv#431): durable intent before every effect, outcome after, crash points, read-only inspection,
 * recovery categories, no replay, operator reconciliation ---- */

static struct osh_session R; /* a restart that may run again (Part 4) */

static int item_at(uint64_t id, struct oshj_item *it)
{
    return oshj_item(&J0.cx, id, it);
}

/* the k-th intent (1-based) of the journal */
static uint64_t nth_intent(unsigned k)
{
    for (uint64_t id = 1; id <= J0.cx.n; id++) {
        struct oshj_item it;
        if (item_at(id, &it) && --k == 0)
            return id;
    }
    return 0;
}

static void sha(const char *p, size_t n, uint8_t d[32])
{
    sha256_hash((const uint8_t *)p, n, d);
}

/* a crash at `cp` (nth intent) in pipeline 1 of a fresh session, then a read-only look with no OSH session, then a
 * restart: exact counts at every step, nothing replayed, a new request runs once */
static void crash_case(const char *what, const char *text, unsigned cp, unsigned nth, int interrupt, unsigned want_spawned,
                       unsigned want_ui, unsigned want_us, unsigned want_rw, int want_o)
{
    struct osh_session *s = &S;
    struct oshj_report rp;
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    if (interrupt) {
        s->hooks.interrupt_at = 1;
        s->hooks.interrupt_after_turns = 30;
    }
    s->hooks.crash_at = 1;
    s->hooks.crash_point = cp;
    s->hooks.crash_nth = nth;
    uint64_t sp0 = tab.stats.spawned;
    struct osh_result r = line(s, text);
    int ok = r.crashed && r.stop && r.crash_point == cp && tab.stats.spawned - sp0 == want_spawned;
    CHECK(osh_session_close(s) == 1); /* the kernel reaps and closes what the lost adapter left */
    osh_journal_close(&J0);           /* the lost adapter's handle goes with it */

    /* read-only inspection needs no OSH session and cannot write */
    struct osh_journal ro;
    CHECK(osh_journal_open(&ro, &oshj_ram_ops, &jram, 0, "ram_test", 1) == CX_OK);
    size_t len0 = jram.len;
    oshj_inspect(&ro.cx, &rp, 0, 0);
    ok = ok && rp.cat[OSHJ_CAT_UNKNOWN_INTENT] == want_ui && rp.cat[OSHJ_CAT_UNKNOWN_STARTED] == want_us &&
         rp.bad == 0 && rp.chain_ok && rp.dup_outcomes == 0;
    if (cp == OSH_CP_DURING_RECEIPT)
        ok = ok && rp.cat[OSHJ_CAT_FINISHED] == 3 && rp.unsealed_finished == 1 && rp.receipts == 0;
    struct oshj_effect dummy;
    memset(&dummy, 0, sizeof dummy);
    CHECK(!osh_journal_usable(&ro) && oshj_put_outcome(&ro, 1, &dummy, 1, 0, 0, 0, 0) != CX_OK && jram.len == len0);
    osh_journal_close(&ro);

    /* restart: the journal is what survives; recovery writes OUTCOME_UNKNOWN once per open intent, seals, runs nothing */
    osh_session_restart(&R, s);
    JR(&R);
    nlogs = 0;
    unsigned open = osh_op_recover(&R, &env);
    ok = ok && open == want_ui + want_us && R.recovery.unknown_intent == want_ui && R.recovery.unknown_started == want_us &&
         R.recovery.outcomes_written == want_ui + want_us && R.recovery.receipts_written == want_rw &&
         R.recovery.failed == 0 && count_logs("replayed=0") == (int)(want_ui + want_us);
    ok = ok && tab.stats.spawned - sp0 == want_spawned; /* nothing replayed */
    ok = ok && (osh_loc_find(&R, (const uint8_t *)"o", 1) != 0) == want_o;
    ok = ok && osh_op_recover(&R, &env) == 0 && R.recovery.receipts_written == 0; /* reported once */
    oshj_inspect(&J0.cx, &rp, 0, 0);
    ok = ok && rp.cat[OSHJ_CAT_UNKNOWN_INTENT] == 0 && rp.cat[OSHJ_CAT_UNKNOWN_STARTED] == 0 &&
         rp.cat[OSHJ_CAT_RECOVERED] == want_ui + want_us && rp.dup_outcomes == 0 && rp.sealed == rp.pipelines &&
         rp.unsealed_finished == 0 && rp.bad == 0;
    ok = ok && R.epoch > s->epoch && rp.max_epoch < R.epoch;

    /* a new request after the loss is a new decision: it runs once, with its own records */
    CHECK(osh_session_grant_spawn(&R, CK_R_WRITE) == 0);
    osh_session_attach(&R, &nat);
    uint64_t sp1 = tab.stats.spawned;
    r = line(&R, "gen 4096 512 | sink");
    ok = ok && r.status == 0 && r.outcome == OSH_O_COMPLETED && !r.journal_failed && tab.stats.spawned - sp1 == 2;
    oshj_inspect(&J0.cx, &rp, 0, 0);
    ok = ok && rp.sealed == rp.pipelines && rp.cat[OSHJ_CAT_FINISHED] >= 2;
    CHECK(osh_session_close(&R) == 1);
    if (!ok)
        printf("  crash case %s: spawned=%llu ui=%u us=%u rw=%u open=%u\n", what,
               (unsigned long long)(tab.stats.spawned - sp0), R.recovery.unknown_intent, R.recovery.unknown_started,
               R.recovery.receipts_written, open);
    CHECK(ok);
}

static void part4(void)
{
    struct osh_session *s = &S;
    struct osh_result r;
    struct oshj_report rp;
    static const char *const hello[] = { "klog", "hello", 0 };

    /* 1. the records bind what the effect is: request digest, principal, capability, stage, location hashes */
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    r = line(s, "gen 4096 512 | sink >o");
    CHECK(r.status == 0 && r.outcome == OSH_O_COMPLETED && !r.journal_failed && r.journal_refused == 0);
    oshj_inspect(&J0.cx, &rp, 0, 0);
    CHECK(rp.intents == 3 && rp.started == 3 && rp.outcomes == 3 && rp.receipts == 1 && rp.pipelines == 1 &&
          rp.sealed == 1 && rp.cat[OSHJ_CAT_FINISHED] == 3 && rp.bad == 0 && rp.other == 0 && rp.chain_ok);
    {
        struct oshj_item a, b, c;
        uint8_t ho[32], zero32[32];
        memset(zero32, 0, 32);
        sha("o", 1, ho);
        CHECK(item_at(nth_intent(1), &a) && item_at(nth_intent(2), &b) && item_at(nth_intent(3), &c));
        CHECK(a.e.effect == OSHJ_EF_SPAWN && a.e.cmd == 0 && a.e.ncmds == 2 && a.e.argc == 3 && a.e.name_len == 3 &&
              memcmp(a.e.name, "gen", 3) == 0 && memcmp(a.e.stage_id, gen_unit, 32) == 0);
        CHECK(memcmp(a.e.digest, r.digest, 32) == 0 && memcmp(a.e.principal, s->principal, 32) == 0 && a.e.domain == 1 &&
              a.e.cap_index == s->spawn_cap.index && a.e.cap_generation == s->spawn_cap.generation &&
              a.e.resource_class == CK_TASK_RC_TASK && a.e.operation == CK_TASK_OP_SPAWN && a.e.seq == r.seq &&
              a.e.epoch == s->epoch);
        CHECK(memcmp(a.e.target_hash, zero32, 32) == 0 && memcmp(a.e.input_hash, zero32, 32) == 0);
        CHECK(b.e.effect == OSHJ_EF_OPEN_WRITE && b.e.cmd == 1 && b.e.mode == OSH_RK_OUT && memcmp(b.e.target_hash, ho, 32) == 0 &&
              memcmp(b.e.target_pre, zero32, 32) == 0 && b.e.resource_class == OSH_RC_LOCATION &&
              b.e.operation == OSH_OP_OPEN_WRITE && b.e.cap_index == s->loc_cap.index);
        CHECK(c.e.effect == OSHJ_EF_SPAWN && c.e.cmd == 1 && memcmp(c.e.name, "sink", 4) == 0 &&
              memcmp(c.e.target_hash, ho, 32) == 0 && memcmp(c.e.stage_id, zero32, 32) == 0);
        CHECK(a.started_id > a.intent_id && a.outcome_id > a.started_id && a.outcome == OSH_O_COMPLETED && a.status == 0 &&
              a.sealed && !a.recovered);
        /* the provisional result comes before the effect's end; the receipt is last and lists the three outcomes */
        const CxObject *last = cx_get(&J0.cx, J0.cx.n);
        CHECK(last->kind == OSHJ_K_RECEIPT && last->cls == CX_EVIDENCE && (cx_payload(&J0.cx, last)[1] & 0xffffu) == 3 &&
              (last->protect & CX_PROT_EFFECT_RECEIPT));
        /* an input location is bound by the hash of its bytes; an append by the bytes it found */
        const struct osh_loc *lo = osh_loc_find(s, (const uint8_t *)"o", 1);
        uint8_t hb[32];
        CHECK(lo && lo->len == 16);
        sha256_hash(lo->bytes, lo->len, hb);
        r = line(s, "xform 7 <o | sink");
        CHECK(r.status == 0 && item_at(nth_intent(4), &a) && a.e.cmd == 0 && memcmp(a.e.input_hash, hb, 32) == 0);
        r = line(s, "gen 10 1 >>o");
        CHECK(r.status == 0 && item_at(nth_intent(6), &a) && a.e.effect == OSHJ_EF_OPEN_WRITE && a.e.mode == OSH_RK_APPEND &&
              memcmp(a.e.target_pre, hb, 32) == 0);
    }
    CHECK(osh_session_close(s) == 1);

    /* 2. no usable journal: every effect refused BY NAME before it happens (ST8.2), never a silent downgrade */
    uint64_t sp0;
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    osh_session_journal(s, 0, 0);
    sp0 = tab.stats.spawned;
    r = line(s, "gen 4096 512 | sink >o");
    CHECK(r.status == 1 && r.error == OSH_E_IO && r.journal_refused == 2 && tab.stats.spawned == sp0 && idle(s));
    CHECK(r.cmd_status[0] == 1 && r.cmd_outcome[0] == OSH_O_FAILED_NO_EFFECT && r.cmd_error[0] == OSH_E_IO);
    CHECK(r.cmd_status[1] == 1 && r.cmd_outcome[1] == OSH_O_FAILED_NO_EFFECT && r.cmd_error[1] == OSH_E_IO);
    CHECK(!osh_loc_find(s, (const uint8_t *)"o", 1)); /* the open did not happen either */
    CHECK(count_logs("osh_op_refused: seq=1 cmd=0 name=gen effect=task_spawn error=IO outcome=FAILED_NO_EFFECT "
                     "reason=journal_unavailable") == 1);
    CHECK(count_logs("osh_op_refused: seq=1 cmd=1 name=sink effect=open_write error=IO outcome=FAILED_NO_EFFECT "
                     "reason=journal_unavailable") == 1);
    CHECK(count_logs("op=task_spawn result=IO") == 1 && count_logs("op=location_open_write result=IO") == 1);
    CHECK(osh_session_close(s) == 1);
    reset_capture();
    osh_session_init(s, CK_R_WRITE); /* the console write too */
    r = run(s, hello);
    CHECK(r.status == 1 && r.error == OSH_E_IO && r.outcome == OSH_O_FAILED_NO_EFFECT && cap_calls == 0 && r.journal_refused == 1);
    CHECK(count_logs("name=klog effect=console_write error=IO outcome=FAILED_NO_EFFECT reason=journal_unavailable") == 1);
    CHECK(osh_session_close(s) == 1);

    /* a volatile journal while the session demands durability (the default): refused, nothing recorded */
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    osh_session_journal(s, &J0, 0);
    sp0 = tab.stats.spawned;
    r = line(s, "gen 4096 512 | sink");
    CHECK(r.status == 1 && r.journal_refused == 2 && tab.stats.spawned == sp0 && J0.cx.n == 0 &&
          count_logs("reason=journal_not_durable durable=0") == 2);
    CHECK(osh_session_close(s) == 1);
    /* the same with a journal the caller declares durable (only the policy is tested here: these bytes are RAM) */
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    osh_journal_close(&J0);
    CHECK(osh_journal_open(&J0, &oshj_ram_ops, &jram, 1, "ram_declared_durable_TEST", 0) == CX_OK);
    osh_session_journal(s, &J0, 0);
    r = line(s, "gen 4096 512 | sink");
    CHECK(r.status == 0 && r.journal_refused == 0 && tab.stats.spawned == sp0 + 2);
    CHECK(osh_session_close(s) == 1);
    /* a journal opened read-only */
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    osh_journal_close(&J0);
    CHECK(osh_journal_open(&J0, &oshj_ram_ops, &jram, 0, "ram_test", 1) == CX_OK);
    osh_session_journal(s, &J0, 1);
    sp0 = tab.stats.spawned;
    r = line(s, "gen 4096 512 | sink");
    CHECK(r.status == 1 && tab.stats.spawned == sp0 && count_logs("reason=journal_read_only") == 2);
    CHECK(osh_session_close(s) == 1);

    /* the intent append fails: that effect does not happen, the journal is poisoned for good, every later one refused */
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    jram.fail_at = jram.attempts + 1u;
    sp0 = tab.stats.spawned;
    r = line(s, "gen 4096 512 | sink >o");
    CHECK(r.status == 1 && r.journal_refused == 2 && tab.stats.spawned == sp0 && J0.poisoned && J0.cx.n == 0);
    CHECK(count_logs("name=gen effect=task_spawn error=IO outcome=FAILED_NO_EFFECT reason=intent_not_durable") == 1);
    CHECK(count_logs("name=sink effect=open_write error=IO outcome=FAILED_NO_EFFECT reason=journal_failed") == 1);
    CHECK(!osh_loc_find(s, (const uint8_t *)"o", 1) && idle(s));
    r = line(s, "gen 4096 512 | sink");
    CHECK(r.status == 1 && tab.stats.spawned == sp0 && count_logs("reason=journal_failed") == 2);
    CHECK(osh_session_close(s) == 1);

    /* an outcome append fails after the effects ran: the pipeline's own result stands, its intents stay open, recovery
     * reports them UNKNOWN (started), nothing is replayed */
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    jram.fail_at = jram.attempts + 5u; /* intent, started, intent, started, then the first outcome */
    sp0 = tab.stats.spawned;
    r = line(s, "gen 4096 512 | sink");
    CHECK(r.status == 0 && r.outcome == OSH_O_COMPLETED && r.journal_failed == 1 && tab.stats.spawned == sp0 + 2);
    CHECK(osh_session_close(s) == 1);
    osh_session_restart(&R, s);
    JR(&R);
    CHECK(osh_op_recover(&R, &env) == 2 && R.recovery.unknown_started == 2 && R.recovery.receipts_written == 1 &&
          tab.stats.spawned == sp0 + 2);
    CHECK(osh_session_close(&R) == 1);

    /* 3. every crash point, exact counts, no duplicate effect. Pipeline: intent 1 = spawn gen, 2 = open o, 3 = spawn sink */
    static const char P[] = "gen 4096 512 | sink >o";
    crash_case("before_intent_1", P, OSH_CP_BEFORE_INTENT, 1, 0, 0, 0, 0, 0, 0);
    crash_case("before_intent_2", P, OSH_CP_BEFORE_INTENT, 2, 0, 1, 0, 1, 1, 0);
    crash_case("after_intent_1", P, OSH_CP_AFTER_INTENT, 1, 0, 0, 1, 0, 1, 0);
    crash_case("after_intent_2", P, OSH_CP_AFTER_INTENT, 2, 0, 1, 1, 1, 1, 0);
    crash_case("after_spawn_1", P, OSH_CP_AFTER_EFFECT, 1, 0, 1, 1, 0, 1, 0);
    crash_case("after_write_2", P, OSH_CP_AFTER_EFFECT, 2, 0, 1, 1, 1, 1, 1);
    crash_case("after_spawn_3", P, OSH_CP_AFTER_EFFECT, 3, 0, 2, 1, 2, 1, 1);
    crash_case("after_started_3", P, OSH_CP_AFTER_STARTED, 3, 0, 2, 0, 3, 1, 1);
    crash_case("after_finish", P, OSH_CP_AFTER_FINISH, 0, 0, 2, 0, 3, 1, 1);
    crash_case("during_cancel", "gen 0 | sink >o", OSH_CP_DURING_CANCEL, 0, 1, 2, 0, 3, 1, 1);
    crash_case("during_receipt", P, OSH_CP_DURING_RECEIPT, 0, 0, 2, 0, 0, 1, 1);
    /* the console write: lost after the intent, nothing printed; lost after the write, printed once and never again */
    reset_capture();
    osh_session_init(s, CK_R_WRITE);
    J(s);
    s->hooks.crash_at = 1;
    s->hooks.crash_point = OSH_CP_AFTER_INTENT;
    s->hooks.crash_nth = 1;
    r = run(s, hello);
    CHECK(r.crashed && cap_calls == 0);
    osh_session_restart(&R, s);
    JR(&R);
    CHECK(osh_op_recover(&R, &env) == 1 && R.recovery.unknown_intent == 1 && cap_calls == 0);
    reset_capture();
    osh_session_init(s, CK_R_WRITE);
    J(s);
    s->hooks.crash_at = 1;
    s->hooks.crash_point = OSH_CP_AFTER_EFFECT;
    s->hooks.crash_nth = 1;
    r = run(s, hello);
    CHECK(r.crashed && cap_calls == 1);
    osh_session_restart(&R, s);
    JR(&R);
    CHECK(osh_op_recover(&R, &env) == 1 && R.recovery.unknown_intent == 1 && cap_calls == 1);

    /* 4. operator reconciliation through the capability root's grant: a statement, never a re-run */
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    s->hooks.crash_at = 1;
    s->hooks.crash_point = OSH_CP_AFTER_INTENT;
    s->hooks.crash_nth = 1;
    sp0 = tab.stats.spawned;
    r = line(s, P);
    CHECK(r.crashed && osh_session_close(s) == 1);
    osh_session_restart(&R, s);
    JR(&R);
    CHECK(osh_op_recover(&R, &env) == 1);
    uint64_t in1 = nth_intent(1);
    uint8_t opid[32];
    sha("operator-test", 13, opid);
    uint64_t n0 = J0.cx.n;
    nlogs = 0;
    CHECK(osh_op_reconcile(&R, &env, opid, in1, OSHJ_V_OBSERVED_ABSENT) == OSH_E_DENIED && J0.cx.n == n0); /* no grant */
    CHECK(count_logs("osh_op_receipt: op=reconcile result=DENIED") == 1);
    CHECK(osh_session_grant_reconcile(&R, opid, CK_R_WRITE) == 0);
    CHECK(osh_op_reconcile(&R, &env, R.principal, in1, OSHJ_V_OBSERVED_ABSENT) == OSH_E_DENIED); /* the shell's principal */
    CHECK(osh_op_reconcile(&R, &env, opid, in1, 9) == OSH_E_INVALID_ARG && J0.cx.n == n0);   /* no such verdict */
    CHECK(osh_op_reconcile(&R, &env, opid, in1, OSHJ_V_OBSERVED_ABSENT) == OSH_E_OK && J0.cx.n == n0 + 1);
    {
        struct oshj_item it;
        CHECK(item_at(in1, &it) && it.cat == OSHJ_CAT_RECONCILED && it.verdict == OSHJ_V_OBSERVED_ABSENT &&
              it.outcome == OSH_O_OUTCOME_UNKNOWN && it.recovered); /* the unknown outcome itself is unchanged */
    }
    CHECK(osh_op_reconcile(&R, &env, opid, in1, OSHJ_V_OBSERVED_DONE) == OSH_E_INVALID_ARG); /* once */
    CHECK(osh_op_reconcile(&R, &env, opid, J0.cx.n, OSHJ_V_OBSERVED_DONE) == OSH_E_INVALID_ARG); /* not an intent */
    CHECK(tab.stats.spawned == sp0); /* nothing ran */
    CHECK(osh_session_grant_spawn(&R, CK_R_WRITE) == 0);
    osh_session_attach(&R, &nat);
    r = line(&R, "gen 4096 512 | sink");
    CHECK(r.status == 0 && osh_op_reconcile(&R, &env, opid, nth_intent(2), OSHJ_V_OBSERVED_DONE) == OSH_E_INVALID_ARG);
    CHECK(ck_cap_remove(&R.rec_caps, R.rec_cap) == CK_CAP_OK);
    CHECK(osh_op_reconcile(&R, &env, opid, in1, OSHJ_V_ACCEPT_UNKNOWN) == OSH_E_REVOKED);
    CHECK(osh_session_close(&R) == 1);

    /* 5. a journal that cannot be opened at all (the device is absent): no session can record, so nothing runs */
    struct oshj_ram gone;
    struct osh_journal jg;
    static uint8_t gbuf[4096];
    oshj_ram_init(&gone, gbuf, sizeof gbuf);
    gone.unavailable = 1;
    CHECK(osh_journal_open(&jg, &oshj_ram_ops, &gone, 0, "ram_test", 0) != CX_OK && !osh_journal_usable(&jg));
    native(s, CK_R_WRITE, CK_R_READ | CK_R_WRITE);
    osh_session_journal(s, &jg, 1);
    sp0 = tab.stats.spawned;
    r = line(s, "gen 4096 512 | sink");
    CHECK(r.status == 1 && tab.stats.spawned == sp0 && count_logs("reason=journal_unavailable") == 2);
    nlogs = 0;
    CHECK(osh_op_recover(s, &env) == 0 && count_logs("osh_op_recover: no usable effect journal") == 1);
    CHECK(osh_session_close(s) == 1);
    osh_journal_close(&J0);
}

int main(void)
{
    part1();
    part2();
    part3();
    part4();
    return ck_t_verdict("test_osh_op");
}
