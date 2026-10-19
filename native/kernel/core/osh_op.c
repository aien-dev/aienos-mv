/* osh_op.c -- see osh_op.h. The adapter side of OSH on AIENOS: request record in, capability-checked platform
 * operations out (a console write, or a pipeline of native tasks over native pipes). No lexing, parsing or expansion
 * happens here. No libc calls (host and kernel builds share it). */
#include "osh_op.h"
#include "fmt.h"
#include "sha256.h"

/* TEST-ONLY mutants (host only, make osh-op-mutants): each must make tests/test_osh_op.c FAIL.
 *   SKIP_PERM       the console permission check always says OK
 *   FIRST_STATUS    the pipeline status is the first command's, not the last's
 *   REDIR_REVERSE   a command's redirections apply right to left
 *   KEEP_GAP_ENDS   a command that does not start keeps its pipe ends open (its neighbours never see EOF)
 *   LOC_NO_PERM     a location open skips the location capability check */
#if (defined(CK_OSH_OP_MUTANT_SKIP_PERM) || defined(CK_OSH_OP_MUTANT_FIRST_STATUS) || \
     defined(CK_OSH_OP_MUTANT_REDIR_REVERSE) || defined(CK_OSH_OP_MUTANT_KEEP_GAP_ENDS) || \
     defined(CK_OSH_OP_MUTANT_LOC_NO_PERM)) &&                                                \
    !__STDC_HOSTED__
#error "CK_OSH_OP_MUTANT_* are host-test-only mutants"
#endif

#define LINE_MAX_BYTES 200u /* longest klog text after the prefix; over it is LIMIT before any effect */
#define RUN_TURNS 8u        /* scheduler turns between two passes of the adapter's stream pumps */
#define PUMP_CHUNK 512u

static const char PREFIX[] = "osh_op_out: ";

static void zero(void *p, size_t n)
{
    uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        b[i] = 0;
}

static int same(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static void copy(void *d, const void *s, size_t n)
{
    uint8_t *x = d;
    const uint8_t *y = s;
    for (size_t i = 0; i < n; i++)
        x[i] = y[i];
}

const char *osh_err_name(unsigned e)
{
    return osh_err_label(e); /* osh_err.h; section 8.6: an unknown code is IO, never OK */
}

const char *osh_outcome_name(unsigned o)
{
    static const char *const n[] = { "NOT_STARTED", "COMPLETED", "FAILED_NO_EFFECT", "CANCELLED", "OUTCOME_UNKNOWN" };
    return o < sizeof n / sizeof n[0] ? n[o] : "OUTCOME_UNKNOWN";
}

static void hex32(const uint8_t d[32], char o[65])
{
    static const char x[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) {
        o[2 * i] = x[d[i] >> 4];
        o[2 * i + 1] = x[d[i] & 15];
    }
    o[64] = 0;
}

/* ---- the session ---- */

static void hold(struct osh_session *s)
{
    copy(s->cap_holder, s->principal, 32);
}

void osh_session_init(struct osh_session *s, unsigned grant_rights)
{
    zero(s, sizeof *s);
    ck_cap_init(&s->caps, 0x4f534800u, CK_CAP_SLOTS);
    ck_cap_init(&s->loc_caps, 0x4f53484cu, 1);
    sha256_hash((const uint8_t *)"aienos-osh-test-session", 23, s->principal);
    if (grant_rights && ck_cap_insert(&s->caps, OSH_RES_CONSOLE_LOG, grant_rights, &s->console_cap) == CK_CAP_OK)
        hold(s);
}

int osh_session_grant_spawn(struct osh_session *s, unsigned rights)
{
    if (s->spawn_cap.generation || ck_cap_insert(&s->caps, CK_TASK_SPAWN_RESOURCE, rights, &s->spawn_cap) != CK_CAP_OK)
        return -1;
    hold(s);
    return 0;
}

int osh_session_grant_loc(struct osh_session *s, unsigned rights)
{
    if (s->loc_cap.generation || ck_cap_insert(&s->loc_caps, OSH_RES_LOCATIONS, rights, &s->loc_cap) != CK_CAP_OK)
        return -1;
    hold(s);
    return 0;
}

void osh_session_attach(struct osh_session *s, struct osh_native *nat)
{
    s->nat = nat;
    ck_pipe_pool_init(nat->pool, 0);
    ck_task_init(nat->tab, nat->pool, &s->caps, s->principal, &nat->run);
    for (unsigned i = 0; i < CK_TASK_IMAGES; i++)
        ck_task_image_set(nat->tab, i, nat->images[i]);
}

static void close_end(struct osh_session *s, struct ck_handle *h)
{
    if (h->generation && s->nat)
        (void)ck_stream_close(s->nat->pool, &s->caps, *h);
    h->index = h->generation = 0;
}

/* Section 12 for the pipeline in flight: interrupt every live stage, a bounded grace, terminate what is left, reap all
 * (ck_task_pipeline_cancel), then close every end the adapter holds. status[k] = how stage k ended (-1: not started). */
static unsigned cleanup_live(struct osh_session *s, int status[OSH_MAX_CMDS])
{
    struct osh_live *lv = &s->live;
    unsigned killed = 0;
    for (unsigned k = 0; k < OSH_MAX_CMDS; k++)
        status[k] = -1;
    if (!lv->active)
        return 0;
    struct ck_task_pipeline pl;
    zero(&pl, sizeof pl);
    pl.n = pl.started = lv->n;
    for (unsigned k = 0; k < lv->n; k++)
        pl.ids[k] = lv->ids[k];
    killed = ck_task_pipeline_cancel(s->nat->tab, &pl);
    for (unsigned k = 0; k < lv->n; k++)
        if (lv->ids[k]) {
            status[k] = (int)pl.status[k];
            lv->ids[k] = 0;
        }
    for (unsigned i = 0; i < OSH_PUMPS_MAX; i++)
        if (lv->pumps[i].used)
            close_end(s, &lv->pumps[i].h);
    lv->active = 0;
    return killed;
}

static unsigned live_slots(const struct ck_cap_table *t)
{
    unsigned n = 0;
    for (unsigned i = 0; i < CK_CAP_SLOTS; i++)
        n += t->slots[i].live;
    return n;
}

int osh_session_close(struct osh_session *s)
{
    int st[OSH_MAX_CMDS];
    if (s->nat) {
        (void)cleanup_live(s, st); /* what a lost adapter left running is reaped by the kernel, never resumed */
        (void)ck_pipe_release_table(s->nat->pool, &s->caps);
    }
    if (s->console_cap.generation)
        (void)ck_cap_remove(&s->caps, s->console_cap);
    if (s->spawn_cap.generation)
        (void)ck_cap_remove(&s->caps, s->spawn_cap);
    if (s->loc_cap.generation)
        (void)ck_cap_remove(&s->loc_caps, s->loc_cap);
    int empty = live_slots(&s->caps) == 0 && live_slots(&s->loc_caps) == 0;
    if (s->nat)
        empty = empty && ck_task_tab_empty(s->nat->tab) && ck_pipe_pool_empty(s->nat->pool);
    return empty;
}

void osh_session_restart(struct osh_session *fresh, const struct osh_session *old)
{
    osh_session_init(fresh, 0);
    for (unsigned i = 0; i < OSH_INTENT_MAX; i++)
        fresh->intents[i] = old->intents[i];
    for (unsigned i = 0; i < OSH_LOC_MAX; i++)
        fresh->locs[i] = old->locs[i];
    fresh->pipelines = old->pipelines;
    fresh->epoch = old->epoch + 1u; /* ST8.3: every earlier handle and task id is gone with the old session */
}

const struct osh_loc *osh_loc_find(const struct osh_session *s, const uint8_t *name, size_t len)
{
    for (unsigned i = 0; i < OSH_LOC_MAX; i++)
        if (s->locs[i].used && s->locs[i].name_len == len && same(s->locs[i].name, name, len))
            return &s->locs[i];
    return 0;
}

/* ---- records and receipts ---- */

static void note(const struct osh_env *env, const char *fmt, uint32_t seq, const char *what, const uint8_t dg[32])
{
    char d[65], line[200];
    hex32(dg, d);
    ck_snprintf(line, sizeof line, fmt, (unsigned)seq, what, d);
    env->log(env->ctx, line);
}

/* Mandatory security receipt (section 13): every use of a grant, every denial and every refusal at the effect boundary.
 * Names principal, capability domain/index/generation, resource class, operation and the request digest. The console
 * receipt keeps its original form; a stage pipeline's names the command too. */
static void receipt(const struct osh_env *env, const struct osh_result *r, const struct osh_binding *b, const char *result,
                    const char *effect)
{
    char d[65], p[65], line[420];
    hex32(r->digest, d);
    hex32(b->principal_id, p);
    ck_snprintf(line, sizeof line,
                "osh_op_receipt: seq=%u op=console_write result=%s request_digest=%s effect=%s principal=%s cap_domain=%u "
                "cap_index=%u cap_generation=%llu resource_class=%u operation=%u",
                (unsigned)r->seq, result, d, effect, p, (unsigned)b->domain, (unsigned)b->cap_index,
                (unsigned long long)b->cap_generation, (unsigned)b->resource_class, (unsigned)b->operation);
    env->log(env->ctx, line);
}

static void receipt_cmd(const struct osh_env *env, const struct osh_result *r, unsigned cmd, const char *op,
                        const struct osh_binding *b, unsigned err, const char *effect)
{
    char d[65], p[65], line[440];
    hex32(r->digest, d);
    hex32(b->principal_id, p);
    ck_snprintf(line, sizeof line,
                "osh_op_receipt: seq=%u cmd=%u op=%s result=%s request_digest=%s effect=%s principal=%s cap_domain=%u "
                "cap_index=%u cap_generation=%llu resource_class=%u operation=%u",
                (unsigned)r->seq, cmd, op, osh_err_name(err), d, err == OSH_E_OK ? effect : "none", p,
                (unsigned)b->domain, (unsigned)b->cap_index, (unsigned long long)b->cap_generation,
                (unsigned)b->resource_class, (unsigned)b->operation);
    env->log(env->ctx, line);
}

unsigned osh_op_recover(struct osh_session *s, const struct osh_env *env)
{
    unsigned n = 0;
    for (unsigned i = 0; i < OSH_INTENT_MAX; i++) {
        struct osh_intent *t = &s->intents[i];
        if (!t->used || t->closed)
            continue;
        t->closed = 1;
        t->outcome = OSH_O_OUTCOME_UNKNOWN;
        t->err = OSH_E_OUTCOME_UNKNOWN;
        note(env, "osh_op_record: seq=%u outcome=%s request_digest=%s replayed=0", t->seq, "OUTCOME_UNKNOWN", t->digest);
        n++;
    }
    return n;
}

/* ---- section 9 PERM_CHECK ---- */

/* Validated at the moment of the effect, against the table that holds the capability. The REVOKED / STALE split is this
 * adapter's reading of the draft: a handle whose slot is now empty (or a tombstone) and whose generation was issued is
 * REVOKED; a slot reused under a newer generation is STALE; anything never issued is DENIED. */
static unsigned perm_in(const struct ck_cap_table *t, const uint8_t holder[32], const struct osh_binding *b, uint16_t rc,
                        uint16_t op, uint32_t resource, unsigned right)
{
    if (b->domain == 2)
        return OSH_E_CAP_DOMAIN_MISMATCH; /* these resources live in the kernel IPC table (domain 1) */
    if (b->domain != 1)
        return OSH_E_INVALID_ARG;
    if (b->cap_generation > 0xffffffffull)
        return OSH_E_CAP_GEN_NARROW;
    if (b->resource_class != rc || b->operation != op)
        return OSH_E_DENIED;
    if (!same(b->principal_id, holder, 32)) /* the capability belongs to the principal it was granted to */
        return OSH_E_DENIED;
    uint32_t gen = (uint32_t)b->cap_generation;
    if (gen == 0 || b->cap_index >= t->n)
        return OSH_E_DENIED;
    const struct ck_cap_slot *sl = &t->slots[b->cap_index];
    struct ck_handle h = { b->cap_index, gen };
    if (sl->live && !sl->tombstone && sl->generation == gen) {
        uint32_t res = 0;
        int r = ck_cap_lookup(t, h, right, &res);
        return r == CK_CAP_OK && res == resource ? OSH_E_OK : OSH_E_DENIED;
    }
    if (sl->live && !sl->tombstone && sl->generation > gen)
        return OSH_E_STALE;
    if ((sl->live && sl->tombstone && sl->generation == gen) || (!sl->live && t->generations[b->cap_index] == gen))
        return OSH_E_REVOKED;
    return OSH_E_DENIED;
}

unsigned osh_op_perm_check(const struct osh_session *s, const struct osh_binding *b)
{
#ifdef CK_OSH_OP_MUTANT_SKIP_PERM
    (void)s;
    (void)b;
    return OSH_E_OK;
#else
    return perm_in(&s->caps, s->cap_holder, b, OSH_RC_CONSOLE_LOG, OSH_OP_WRITE, OSH_RES_CONSOLE_LOG, CK_R_WRITE);
#endif
}

static unsigned loc_perm_check(const struct osh_session *s, const struct osh_binding *b, int write)
{
    return perm_in(&s->loc_caps, s->cap_holder, b, OSH_RC_LOCATION, write ? OSH_OP_OPEN_WRITE : OSH_OP_OPEN_READ,
                   OSH_RES_LOCATIONS, write ? CK_R_WRITE : CK_R_READ);
}

/* the binding an effect carries, built from what the session holds. Nothing here grants anything. */
static void bind(const struct osh_session *s, struct ck_handle h, uint16_t rc, uint16_t op, struct osh_binding *b)
{
    zero(b, sizeof *b);
    copy(b->principal_id, s->principal, 32);
    b->domain = 1;
    b->cap_index = h.index;
    b->cap_generation = h.generation;
    b->resource_class = rc;
    b->operation = op;
}

static void task_binding(const struct osh_binding *b, struct ck_task_binding *tb)
{
    zero(tb, sizeof *tb);
    copy(tb->principal_id, b->principal_id, 32);
    tb->domain = b->domain;
    tb->cap_index = b->cap_index;
    tb->cap_generation = b->cap_generation;
    tb->resource_class = b->resource_class;
    tb->operation = b->operation;
}

/* The spawn binding at this moment: nothing held is DENIED (task.c would call a generation-0 binding malformed). */
static unsigned spawn_check(const struct osh_session *s, struct osh_binding *b)
{
    bind(s, s->spawn_cap, CK_TASK_RC_TASK, CK_TASK_OP_SPAWN, b);
    if (!s->spawn_cap.generation)
        return OSH_E_DENIED;
    /* the adapter's own reading first (REVOKED for a revoked grant, as for the console), then the kernel's check */
    unsigned e = perm_in(&s->caps, s->cap_holder, b, CK_TASK_RC_TASK, CK_TASK_OP_SPAWN, CK_TASK_SPAWN_RESOURCE, CK_R_WRITE);
    if (e != OSH_E_OK)
        return e;
    struct ck_task_binding tb;
    task_binding(b, &tb);
    return ck_task_perm_check(s->nat->tab, &tb);
}

/* the shell status for an error (UNVERIFIED U13) */
static int status_for(unsigned err)
{
    if (err == OSH_E_OK)
        return 0;
    switch (err) {
    case OSH_E_NOT_FOUND: return 127;
    case OSH_E_DENIED: case OSH_E_REVOKED: case OSH_E_STALE: case OSH_E_CAP_DOMAIN_MISMATCH: case OSH_E_CAP_GEN_NARROW:
    case OSH_E_NOT_SUPPORTED: case OSH_E_UNAVAILABLE: return 126; /* found but not run (ST7 steps 2, 3) */
    case OSH_E_INTERRUPTED: return 130;
    case OSH_E_LIMIT: case OSH_E_PARTIAL_LAUNCH: return 1; /* ST7 steps 4, 6 */
    default: return 70; /* IO, INVALID_ARG, unknown: an internal-style failure */
    }
}

static void finish(struct osh_result *r, unsigned err, unsigned outcome)
{
    r->error = err;
    r->outcome = outcome;
    r->status = status_for(err);
}

/* a refusal of the whole pipeline before any effect: every command FAILED_NO_EFFECT with the pipeline's status */
static void refuse_all(struct osh_result *r, unsigned err, unsigned outcome)
{
    finish(r, err, outcome);
    r->launch_error = err;
    for (unsigned k = 0; k < r->ncmds; k++) {
        r->cmd_status[k] = r->status;
        r->cmd_outcome[k] = OSH_O_FAILED_NO_EFFECT;
        r->cmd_error[k] = (uint8_t)err;
    }
}

static void digest_request(const uint64_t *req, unsigned ncmds, const uint64_t *out, uint64_t out_used, uint8_t d[32])
{
    sha256_ctx c;
    uint8_t b[8];
    size_t cells = OSH_REQ_HDR + (size_t)ncmds * OSH_REQ_CMD_CELLS;
    sha256_init(&c);
    for (size_t i = 0; i < cells; i++) {
        for (unsigned k = 0; k < 8; k++)
            b[k] = (uint8_t)(req[i] >> (8 * k));
        sha256_update(&c, b, 8);
    }
    for (uint64_t i = 0; i < out_used; i++) {
        b[0] = (uint8_t)out[i];
        sha256_update(&c, b, 1);
    }
    sha256_final(&c, d);
}

/* ---- intents ---- */

static unsigned free_intents(const struct osh_session *s)
{
    unsigned n = 0;
    for (unsigned i = 0; i < OSH_INTENT_MAX; i++)
        n += !s->intents[i].used || s->intents[i].closed;
    return n;
}

/* A slot is free when never used or when its outcome is already written (and logged); only OPEN intents count. */
static int open_intent(struct osh_session *s, const struct osh_result *r, unsigned effect, unsigned cmd)
{
    int t = -1;
    for (unsigned i = 0; i < OSH_INTENT_MAX && t < 0; i++)
        if (!s->intents[i].used)
            t = (int)i;
    for (unsigned i = 0; i < OSH_INTENT_MAX && t < 0; i++)
        if (s->intents[i].closed)
            t = (int)i;
    if (t < 0)
        return -1;
    struct osh_intent *x = &s->intents[t];
    zero(x, sizeof *x);
    x->used = 1;
    x->seq = r->seq;
    x->effect = (uint8_t)effect;
    x->cmd = (uint8_t)cmd;
    copy(x->digest, r->digest, 32);
    return t;
}

static void close_intent(struct osh_session *s, int t, unsigned outcome, unsigned err, int status)
{
    if (t < 0)
        return;
    struct osh_intent *x = &s->intents[t];
    x->closed = 1;
    x->outcome = (uint8_t)outcome;
    x->err = (uint8_t)err;
    x->status = (uint32_t)status;
}

/* ---- step 1: the shape of the record (section 7.1, section 11) ---- */

static int span_ok(const uint64_t *out, uint64_t used, uint64_t off, uint64_t len, int no_nul)
{
    if (off > used || len > used - off)
        return 0;
    for (uint64_t i = 0; i < len; i++)
        if (out[off + i] > 255 || (no_nul && out[off + i] == 0))
            return 0;
    return 1;
}

static int block_ok(const uint64_t *c, const uint64_t *out, uint64_t used, int direct)
{
    uint64_t nargv = c[0], nassign = c[1], nredir = c[2], builtin = c[3];
    if (nargv > OSH_MAX_ARGV || nassign > OSH_MAX_ASSIGN || nredir > OSH_MAX_REDIR || builtin > 6)
        return 0;
    if (direct && builtin) /* section 11: builtin_id must be 0 on a direct submission */
        return 0;
    const uint64_t *av = c + 4, *as = c + 4 + 64, *rd = c + 4 + 64 + 64;
    for (uint64_t k = 0; k < OSH_MAX_ARGV; k++) {
        if (k >= nargv) {
            if (av[2 * k] || av[2 * k + 1])
                return 0; /* entries beyond the count are zero (UNVERIFIED U12) */
        } else if (!span_ok(out, used, av[2 * k], av[2 * k + 1], direct)) {
            return 0;
        }
    }
    for (uint64_t k = 0; k < OSH_MAX_ASSIGN; k++) {
        const uint64_t *e = as + 4 * k;
        if (k >= nassign) {
            if (e[0] || e[1] || e[2] || e[3])
                return 0;
        } else if (e[1] == 0 || !span_ok(out, used, e[0], e[1], direct) || !span_ok(out, used, e[2], e[3], direct)) {
            return 0;
        }
    }
    for (uint64_t k = 0; k < OSH_MAX_REDIR; k++) {
        const uint64_t *e = rd + 4 * k;
        if (k >= nredir) {
            if (e[0] || e[1] || e[2] || e[3])
                return 0;
            continue;
        }
        if (e[0] < OSH_RK_IN || e[0] > OSH_RK_DUP || e[1] > 2)
            return 0;
        if (e[0] == OSH_RK_DUP) {
            if (e[2] > 2 || e[3] != 0)
                return 0;
        } else if (e[3] == 0 || !span_ok(out, used, e[2], e[3], direct)) {
            return 0;
        }
    }
    return 1;
}

/* ---- step 2: RESOLVE ---- */

enum { RS_NOT_FOUND = 0, RS_KLOG = 1, RS_STAGE = 2 };
struct cmd_plan {
    unsigned how; /* RS_* */
    const struct osh_op_def *def;
    uint64_t params[CK_TASK_PARAMS];
    unsigned nparams;
};

static int name_is(const uint64_t *out, uint64_t off, uint64_t len, const char *n)
{
    uint64_t i = 0;
    for (; n[i]; i++)
        if (i >= len || out[off + i] != (uint8_t)n[i])
            return 0;
    return i == len;
}

static int decimal(const uint64_t *out, uint64_t off, uint64_t len, uint64_t *v)
{
    if (len == 0 || len > 20)
        return 0;
    uint64_t x = 0;
    for (uint64_t i = 0; i < len; i++) {
        uint64_t d = out[off + i];
        if (d < '0' || d > '9')
            return 0;
        d -= '0';
        if (x > (UINT64_MAX - d) / 10u)
            return 0;
        x = x * 10u + d;
    }
    *v = x;
    return 1;
}

/* OSH_E_OK, or the whole pipeline's refusal */
static unsigned resolve(const struct osh_session *s, const uint64_t *c, const uint64_t *out, unsigned ncmds,
                        struct cmd_plan *p)
{
    uint64_t nargv = c[0], nassign = c[1], nredir = c[2], builtin = c[3];
    zero(p, sizeof *p);
    /* what this adapter cannot do is refused before any effect, never approximated: builtins (their parent and child
     * contexts, section 7.2), prefix assignments (stage ABI v1 has no environment) and commands with no name */
    if (nargv == 0 || nassign || builtin)
        return OSH_E_NOT_SUPPORTED;
    uint64_t off = c[4], len = c[5];
    for (uint64_t i = 0; i < len; i++)
        if (out[off + i] == '/')
            return OSH_E_UNAVAILABLE; /* a path names a Linux executable (class 4; UNVERIFIED U2): not provided here, never run */
    if (name_is(out, off, len, "klog")) {
        if (ncmds != 1 || nredir)
            return OSH_E_NOT_SUPPORTED; /* klog is a console write, not a stage: one command, no redirection (UNVERIFIED U1) */
        p->how = RS_KLOG;
        return OSH_E_OK;
    }
    const struct osh_op_def *d = 0;
    for (unsigned i = 0; s->nat && i < s->nat->nops && !d; i++)
        if (name_is(out, off, len, s->nat->ops[i].name))
            d = &s->nat->ops[i];
    if (!d)
        return OSH_E_OK; /* not found: this command only (ST7 step 5) */
    if (d->cls != OSH_CLASS_NATIVE_OP)
        return OSH_E_UNAVAILABLE; /* classes never fall back to each other (section 8.3) */
    if (nargv - 1u > d->max_args || nargv - 1u > CK_TASK_PARAMS)
        return OSH_E_LIMIT;
    for (uint64_t k = 1; k < nargv; k++)
        if (!decimal(out, c[4 + 2 * k], c[5 + 2 * k], &p->params[k - 1]))
            return OSH_E_INVALID_ARG; /* a stage parameter is a decimal u64 (stage ABI v1 has no argv slices; UNVERIFIED U4) */
    p->nparams = (unsigned)(nargv - 1u);
    p->def = d;
    p->how = RS_STAGE;
    return OSH_E_OK;
}

/* ---- the klog operation (one command, the console effect) ---- */

static void klog_exec(struct osh_session *s, const struct osh_env *env, const uint64_t *c0, const uint64_t *out,
                      struct osh_result *r)
{
    uint64_t nargv = c0[0];
    /* the text to write: argv[1..] joined by one space; bytes outside 0x20..0x7e become '.' so an argument can never forge
     * a record line */
    char line[sizeof PREFIX + LINE_MAX_BYTES + 3];
    size_t n = 0;
    for (; PREFIX[n]; n++)
        line[n] = PREFIX[n];
    size_t body = 0;
    for (uint64_t k = 1; k < nargv; k++) {
        uint64_t off = c0[4 + 2 * k], len = c0[5 + 2 * k];
        if (body + (k > 1) + len > LINE_MAX_BYTES) {
            refuse_all(r, OSH_E_LIMIT, OSH_O_NOT_STARTED);
            return;
        }
        if (k > 1)
            line[n + body++] = ' ';
        for (uint64_t i = 0; i < len; i++) {
            uint8_t ch = (uint8_t)out[off + i];
            line[n + body++] = ch >= 0x20 && ch <= 0x7e ? (char)ch : '.';
        }
    }
    n += body;
    line[n++] = '\n';
    line[n] = 0; /* the sink may print it as a C string */

    struct osh_binding b;
    bind(s, s->console_cap, OSH_RC_CONSOLE_LOG, OSH_OP_WRITE, &b);

    /* the intent record, before the effect (section 10) */
    int t = open_intent(s, r, OSH_EF_CONSOLE, 0);
    if (t < 0) {
        refuse_all(r, OSH_E_LIMIT, OSH_O_NOT_STARTED);
        receipt(env, r, &b, osh_err_name(OSH_E_LIMIT), "none");
        return;
    }

    if (s->hooks.revoke_before_perm_at == r->seq && s->console_cap.generation)
        (void)ck_cap_remove(&s->caps, s->console_cap); /* revoked between building the request and using it */

    /* the effect boundary: validated now, not at parse time */
    unsigned pc = osh_op_perm_check(s, &b);
    if (pc != OSH_E_OK) {
        close_intent(s, t, OSH_O_FAILED_NO_EFFECT, pc, status_for(pc));
        refuse_all(r, pc, OSH_O_FAILED_NO_EFFECT);
        receipt(env, r, &b, osh_err_name(pc), "none");
        return;
    }
    if (s->hooks.interrupt_before_effect_at == r->seq) {
        close_intent(s, t, OSH_O_CANCELLED, OSH_E_INTERRUPTED, 130);
        finish(r, OSH_E_INTERRUPTED, OSH_O_CANCELLED);
        r->cmd_status[0] = 130;
        r->cmd_outcome[0] = OSH_O_CANCELLED;
        r->cmd_error[0] = OSH_E_INTERRUPTED;
        r->stop = 1;
        note(env, "osh_op_record: seq=%u outcome=%s request_digest=%s effect=none", r->seq, "CANCELLED", r->digest);
        return;
    }

    int w = s->hooks.sink_fail_at == r->seq ? -1 : env->out(env->ctx, line, n);
    r->started = 1;
    if (w != 0) {
        /* a failed write-class effect may have partly happened: unknown, not "no effect" */
        close_intent(s, t, OSH_O_OUTCOME_UNKNOWN, OSH_E_IO, 70);
        finish(r, OSH_E_IO, OSH_O_OUTCOME_UNKNOWN);
        r->cmd_status[0] = 70;
        r->cmd_outcome[0] = OSH_O_OUTCOME_UNKNOWN;
        r->cmd_error[0] = OSH_E_IO;
        note(env, "osh_op_record: seq=%u outcome=%s request_digest=%s effect=unknown", r->seq, "OUTCOME_UNKNOWN", r->digest);
        return;
    }
    /* the grant was used: a security receipt, always (section 13), written even if the adapter is lost right after */
    receipt(env, r, &b, "OK", "console_write");
    if (s->hooks.crash_after_effect_at == r->seq) {
        r->crashed = 1; /* the adapter is lost here: the effect happened, the outcome is never written */
        r->stop = 1;
        r->error = OSH_E_OUTCOME_UNKNOWN;
        r->outcome = OSH_O_OUTCOME_UNKNOWN;
        r->status = 70;
        r->cmd_status[0] = 70;
        r->cmd_outcome[0] = OSH_O_OUTCOME_UNKNOWN;
        r->cmd_error[0] = OSH_E_OUTCOME_UNKNOWN;
        return;
    }
    close_intent(s, t, OSH_O_COMPLETED, OSH_E_OK, 0);
    finish(r, OSH_E_OK, OSH_O_COMPLETED);
    r->cmd_status[0] = 0;
    r->cmd_outcome[0] = OSH_O_COMPLETED;
}

/* ---- stage pipelines ---- */

/* a descriptor of a command being launched: the session handle a stage will get its own copy of */
struct fd_slot {
    struct ck_handle h; /* generation 0 = nothing */
    uint8_t map;        /* osh_fd_kind << 4 | index, for the evidence */
};

static int pump_new(struct osh_session *s, int loc, int feed, unsigned cmd, struct ck_handle h, int intent)
{
    for (unsigned i = 0; i < OSH_PUMPS_MAX; i++) {
        struct osh_pump *p = &s->live.pumps[i];
        if (p->used)
            continue;
        zero(p, sizeof *p);
        p->used = 1;
        p->feed = (uint8_t)feed;
        p->loc = (int8_t)loc;
        p->cmd = (uint8_t)cmd;
        p->h = h;
        p->intent = (int8_t)intent;
        return (int)i;
    }
    return -1;
}

static int loc_lookup(struct osh_session *s, const uint64_t *out, uint64_t off, uint64_t len)
{
    for (unsigned i = 0; i < OSH_LOC_MAX; i++) {
        const struct osh_loc *l = &s->locs[i];
        if (!l->used || l->name_len != len)
            continue;
        uint64_t k = 0;
        while (k < len && l->name[k] == (uint8_t)out[off + k])
            k++;
        if (k == len)
            return (int)i;
    }
    return -1;
}

/* One pass over every end the adapter holds: drain read ends into the session's stdout or a location, feed write ends
 * from a location. Never blocks. Returns bytes moved plus streams finished (progress). */
static uint64_t pump_all(struct osh_session *s, const struct osh_env *env, struct osh_result *r)
{
    uint64_t moved = 0;
    uint8_t buf[PUMP_CHUNK];
    struct ck_pipe_pool *pool = s->nat->pool;
    for (unsigned i = 0; i < OSH_PUMPS_MAX; i++) {
        struct osh_pump *p = &s->live.pumps[i];
        if (!p->used || p->done)
            continue;
        if (p->feed) {
            struct osh_loc *l = &s->locs[p->loc];
            for (;;) {
                if (p->off >= l->len) {
                    close_end(s, &p->h); /* everything delivered: end of stream for the reader */
                    p->done = 1;
                    moved++;
                    break;
                }
                size_t n = 0;
                unsigned e = ck_stream_write(pool, &s->caps, p->h, l->bytes + p->off, l->len - p->off, 0, &n);
                if (e == OSH_E_OK && n) {
                    p->off += (uint32_t)n;
                    p->bytes += n;
                    moved += n;
                    continue;
                }
                if (e != OSH_E_WOULD_BLOCK) { /* BROKEN_PIPE (the reader is gone), or the end was revoked */
                    close_end(s, &p->h);
                    p->done = 1;
                    moved++;
                }
                break;
            }
            continue;
        }
        for (;;) {
            size_t n = 0;
            unsigned e = ck_stream_read(pool, &s->caps, p->h, buf, sizeof buf, 0, &n);
            if (e == OSH_E_WOULD_BLOCK)
                break;
            if (e != OSH_E_OK || n == 0) { /* end of stream, or the end was revoked */
                close_end(s, &p->h);
                p->done = 1;
                moved++;
                break;
            }
            int ok;
            if (p->loc < 0) {
                ok = env->stdout_bytes && env->stdout_bytes(env->ctx, buf, n) == 0;
                if (ok)
                    r->stdout_bytes += n;
            } else {
                struct osh_loc *l = &s->locs[p->loc];
                size_t room = OSH_LOC_BYTES - l->len, k = n < room ? n : room;
                copy(l->bytes + l->len, buf, k);
                l->len += (uint32_t)k;
                ok = k == n;
            }
            p->bytes += n;
            moved += n;
            if (!ok) { /* the destination is full or failed: LIMIT, never a silent cut; the writer sees BROKEN_PIPE (UNVERIFIED U9) */
                p->overflow = 1;
                close_end(s, &p->h);
                p->done = 1;
                break;
            }
        }
    }
    r->pumped += moved;
    return moved;
}

static unsigned stages_alive(const struct osh_session *s)
{
    unsigned n = 0;
    struct ck_task_info in;
    for (unsigned k = 0; k < s->live.n; k++)
        if (s->live.ids[k] && ck_task_state(s->nat->tab, s->live.ids[k], &in) == OSH_E_OK && in.state != CK_TS_EXITED)
            n++;
    return n;
}

static uint64_t bytes_in_pipes(const struct ck_pipe_pool *pool)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < CK_PIPE_POOL_PIPES; i++)
        if (pool->pipes[i].flags & CK_PIPE_LIVE)
            n += pool->pipes[i].len;
    return n;
}

/* the command's session handles, each closed once (the stage holds its own copies, or never started) */
static void close_fds(struct osh_session *s, struct fd_slot fd[3], struct ck_handle *extra, unsigned nextra)
{
    struct ck_handle all[3 + 2 * OSH_MAX_REDIR + 2];
    unsigned n = 0;
    for (unsigned k = 0; k < 3; k++)
        all[n++] = fd[k].h;
    for (unsigned k = 0; k < nextra; k++)
        all[n++] = extra[k];
    for (unsigned i = 0; i < n; i++) {
        if (!all[i].generation)
            continue;
        int dup = 0;
        for (unsigned j = 0; j < i; j++)
            dup |= all[j].index == all[i].index && all[j].generation == all[i].generation;
        if (!dup)
            close_end(s, &all[i]);
    }
    for (unsigned k = 0; k < 3; k++)
        fd[k].h.index = fd[k].h.generation = 0;
}

static void cmd_set(struct osh_result *r, unsigned k, int status, unsigned outcome, unsigned err)
{
    r->cmd_status[k] = status;
    r->cmd_outcome[k] = (uint8_t)outcome;
    r->cmd_error[k] = (uint8_t)err;
}

/* Redirections of command k in record order (section 7.2). OSH_E_OK, or why the command cannot run (status 1).
 * opened[] collects the near ends created here (the session closes them after the spawn). */
static unsigned redirect(struct osh_session *s, const struct osh_env *env, struct osh_result *r, unsigned k,
                         const uint64_t *c, const uint64_t *out, struct fd_slot fd[3], struct ck_handle *opened,
                         unsigned *nopened)
{
    uint64_t nredir = c[2];
    const uint64_t *rd = c + 4 + 64 + 64;
    for (uint64_t q = 0; q < nredir; q++) {
#ifdef CK_OSH_OP_MUTANT_REDIR_REVERSE
        const uint64_t *e = rd + 4 * (nredir - 1u - q);
#else
        const uint64_t *e = rd + 4 * q;
#endif
        unsigned kind = (unsigned)e[0], n = (unsigned)e[1];
        if (kind == OSH_RK_DUP) {
            fd[n] = fd[e[2]]; /* N>&M: N now names what M names right now */
            continue;
        }
        int write = kind != OSH_RK_IN;
        if (s->hooks.revoke_loc_at == r->seq && s->hooks.revoke_loc_cmd == k && s->loc_cap.generation)
            (void)ck_cap_remove(&s->loc_caps, s->loc_cap);
        struct osh_binding b;
        bind(s, s->loc_cap, OSH_RC_LOCATION, write ? OSH_OP_OPEN_WRITE : OSH_OP_OPEN_READ, &b);
        const char *opn = write ? "location_open_write" : "location_open_read";
        unsigned pc = s->loc_cap.generation ? loc_perm_check(s, &b, write) : OSH_E_DENIED;
#ifdef CK_OSH_OP_MUTANT_LOC_NO_PERM
        pc = OSH_E_OK;
#endif
        if (pc != OSH_E_OK) {
            receipt_cmd(env, r, k, opn, &b, pc, "none");
            return pc;
        }
        uint64_t toff = e[2], tlen = e[3];
        if (tlen > OSH_LOC_NAME) {
            receipt_cmd(env, r, k, opn, &b, OSH_E_LIMIT, "none");
            return OSH_E_LIMIT;
        }
        int li = loc_lookup(s, out, toff, tlen);
        if (!write && li < 0) {
            receipt_cmd(env, r, k, opn, &b, OSH_E_NOT_FOUND, "none");
            return OSH_E_NOT_FOUND;
        }
        if (write && li < 0) {
            for (unsigned i = 0; i < OSH_LOC_MAX && li < 0; i++)
                if (!s->locs[i].used)
                    li = (int)i;
            if (li < 0) {
                receipt_cmd(env, r, k, opn, &b, OSH_E_LIMIT, "none");
                return OSH_E_LIMIT;
            }
        }
        struct ck_handle prd, pwr;
        if (ck_pipe_create(s->nat->pool, &s->caps, &prd, &pwr) != OSH_E_OK) {
            receipt_cmd(env, r, k, opn, &b, OSH_E_LIMIT, "none");
            return OSH_E_LIMIT;
        }
        int t = -1;
        if (write) {
            t = open_intent(s, r, OSH_EF_OPEN_WRITE, k); /* reserved at step 4, so it exists */
            struct osh_loc *l = &s->locs[li];
            if (!l->used) {
                l->used = 1;
                l->name_len = (uint8_t)tlen;
                for (uint64_t i = 0; i < tlen; i++)
                    l->name[i] = (uint8_t)out[toff + i];
            }
            if (kind == OSH_RK_OUT)
                l->len = 0; /* the effect: create or truncate */
        }
        receipt_cmd(env, r, k, opn, &b, OSH_E_OK, write ? "open_write" : "open_read");
        int pi = pump_new(s, li, !write, k, write ? prd : pwr, t);
        if (pi < 0) { /* cannot happen: step 4 reserved a pump per location pipe */
            close_end(s, &prd);
            close_end(s, &pwr);
            close_intent(s, t, OSH_O_FAILED_NO_EFFECT, OSH_E_LIMIT, 1);
            return OSH_E_LIMIT;
        }
        struct ck_handle near = write ? pwr : prd;
        opened[(*nopened)++] = near;
        fd[n].h = near;
        fd[n].map = (uint8_t)(((write ? OSH_FD_LOC_OUT : OSH_FD_LOC_IN) << 4) | (unsigned)li);
    }
    return OSH_E_OK;
}

static void stage_exec(struct osh_session *s, const struct osh_env *env, const uint64_t *req, const uint64_t *out,
                       struct cmd_plan *plan, struct osh_result *r)
{
    struct osh_native *nat = s->nat;
    struct ck_task_tab *tab = nat->tab;
    unsigned n = r->ncmds;
    struct osh_binding b;

    /* step 3: the spawn binding, before any effect */
    unsigned pc = spawn_check(s, &b);
    if (pc != OSH_E_OK) {
        refuse_all(r, pc, OSH_O_FAILED_NO_EFFECT);
        receipt_cmd(env, r, 0, "task_spawn", &b, pc, "none");
        return;
    }

    /* step 4: resources, before any effect (lazy creation below cannot then run out of pipes or intents; UNVERIFIED U8) */
    unsigned pipes = n - 1u, effects = 0, stages = 0, stdout_used = 0;
    for (unsigned k = 0; k < n; k++) {
        const uint64_t *c = req + OSH_REQ_HDR + (size_t)k * OSH_REQ_CMD_CELLS;
        if (plan[k].how != RS_STAGE)
            continue;
        stages++;
        effects++;
        /* where the last command's descriptors end up, as the launch will apply them: does any reach the stdout? */
        uint8_t sim[3] = { 0, (uint8_t)(k + 1u == n), 0 };
        for (uint64_t q = 0; q < c[2]; q++) {
            const uint64_t *e = c + 4 + 64 + 64 + 4 * q;
            if (e[0] != OSH_RK_DUP) {
                pipes++;
                effects += e[0] != OSH_RK_IN;
                sim[e[1]] = 0;
            } else {
                sim[e[1]] = sim[e[2]];
            }
        }
        stdout_used |= (sim[0] || sim[1] || sim[2]) && env->stdout_bytes;
    }
    pipes += stdout_used;
    unsigned pumps = pipes - (n - 1u);
    if (pipes > CK_PIPE_POOL_PIPES - nat->pool->live || pumps > OSH_PUMPS_MAX || free_intents(s) < effects ||
        stages > CK_TASK_MAX) {
        refuse_all(r, OSH_E_LIMIT, OSH_O_NOT_STARTED); /* ST7 step 4: nothing created, nothing left allocated */
        return;
    }

    /* step 5: start the commands left to right */
    struct osh_live *lv = &s->live;
    zero(lv, sizeof *lv);
    lv->active = 1;
    lv->n = n;
    for (unsigned k = 0; k < OSH_MAX_CMDS; k++)
        lv->intent[k] = -1;
    struct ck_handle stdout_w = { 0, 0 };
    if (stdout_used) {
        struct ck_handle sr;
        if (ck_pipe_create(nat->pool, &s->caps, &sr, &stdout_w) != OSH_E_OK ||
            pump_new(s, -1, 0, n - 1u, sr, -1) < 0) { /* reserved at step 4 */
            int st[OSH_MAX_CMDS];
            close_end(s, &stdout_w);
            (void)cleanup_live(s, st);
            refuse_all(r, OSH_E_LIMIT, OSH_O_NOT_STARTED);
            return;
        }
    }
    uint64_t steps0 = tab->stats.steps, blocks0 = tab->stats.blocks, wakes0 = tab->stats.wakeups;
    struct ck_handle carried = { 0, 0 }; /* read end of the pipe from the left neighbour */
    unsigned fail = OSH_E_OK, fail_at = n;
    int any_refused = 0;
    for (unsigned k = 0; k < n; k++) {
        const uint64_t *c = req + OSH_REQ_HDR + (size_t)k * OSH_REQ_CMD_CELLS;
        struct fd_slot fd[3];
        zero(fd, sizeof fd);
        struct ck_handle nrd = { 0, 0 }, nwr = { 0, 0 };
        if (k + 1u < n && ck_pipe_create(nat->pool, &s->caps, &nrd, &nwr) != OSH_E_OK) {
            close_end(s, &carried);
            fail = OSH_E_LIMIT; /* the session table is full: a resource failure of the launch */
            fail_at = k;
            break;
        }
        if (k > 0) {
            fd[0].h = carried;
            fd[0].map = (uint8_t)((OSH_FD_PIPE_IN << 4) | (k - 1u));
        }
        if (k + 1u < n) {
            fd[1].h = nwr;
            fd[1].map = (uint8_t)((OSH_FD_PIPE_OUT << 4) | k);
        } else if (stdout_w.generation) {
            fd[1].h = stdout_w;
            fd[1].map = (uint8_t)(OSH_FD_STDOUT << 4);
        }
        struct ck_handle opened[OSH_MAX_REDIR + 1u];
        unsigned nopened = 0;
        unsigned e = OSH_E_OK;
        int status = 0;

        /* the decisions that need no effect come first: a name not found, then the binding at this moment, before any open (UNVERIFIED U6) */
        if (plan[k].how != RS_STAGE) {
            e = OSH_E_NOT_FOUND;
            status = 127;
        } else {
            if (s->hooks.revoke_spawn_at == r->seq && s->hooks.revoke_spawn_cmd == k && s->spawn_cap.generation)
                (void)ck_cap_remove(&s->caps, s->spawn_cap); /* revoked after an earlier command started */
            e = spawn_check(s, &b);
            if (e != OSH_E_OK) {
                receipt_cmd(env, r, k, "task_spawn", &b, e, "none");
                status = 126;
            }
        }
        if (e == OSH_E_OK) {
            e = redirect(s, env, r, k, c, out, fd, opened, &nopened);
            if (e != OSH_E_OK)
                status = 1; /* a failed redirection: the command does not run (Omega, POSIX shells; UNVERIFIED U5) */
        }
        for (unsigned q = 0; q < 3; q++)
            r->cmd_fd[k][q] = fd[q].map;
        if (e == OSH_E_OK) {
            struct ck_task_spawn_req rq;
            zero(&rq, sizeof rq);
            rq.image = plan[k].def->image;
            for (unsigned q = 0; q < 3; q++)
                rq.stdio[q] = fd[q].h;
            for (unsigned q = 0; q < plan[k].nparams; q++)
                rq.params[q] = plan[k].params[q];
            rq.nparams = plan[k].nparams;
            rq.in_chunk = plan[k].def->in_chunk;
            rq.max_ticks = nat->step_ticks;
            rq.max_steps = nat->max_steps;
            task_binding(&b, &rq.binding);
            lv->intent[k] = (int8_t)open_intent(s, r, OSH_EF_SPAWN, k); /* reserved at step 4 */
            if (s->hooks.fail_spawn_at == r->seq && s->hooks.fail_spawn_cmd == k)
                e = s->hooks.fail_spawn_err ? s->hooks.fail_spawn_err : OSH_E_LIMIT; /* fault-spawn: the call fails */
            else
                e = ck_task_spawn(tab, &rq, &lv->ids[k]);
            if (e == OSH_E_OK) {
                r->started++;
                receipt_cmd(env, r, k, "task_spawn", &b, OSH_E_OK, "spawn");
            } else if (e == OSH_E_LIMIT || e == OSH_E_IO) {
                close_intent(s, lv->intent[k], OSH_O_FAILED_NO_EFFECT, e, 1);
                lv->intent[k] = -1;
                opened[nopened++] = k + 1u < n ? nwr : stdout_w; /* each session handle closed exactly once */
                close_fds(s, fd, opened, nopened);
                close_end(s, &nrd);
                if (k + 1u == n)
                    stdout_w.index = stdout_w.generation = 0;
                fail = e; /* ST7 step 6: a resource failure of the spawn itself */
                fail_at = k;
                break;
            } else {
                /* refused at the moment of the effect (receipt, entry, end or binding): this command only (UNVERIFIED U7) */
                close_intent(s, lv->intent[k], OSH_O_FAILED_NO_EFFECT, e, 126);
                lv->intent[k] = -1;
                receipt_cmd(env, r, k, "task_spawn", &b, e, "none");
                status = 126;
            }
        }
        if (e != OSH_E_OK) {
            any_refused = 1;
            cmd_set(r, k, status, OSH_O_FAILED_NO_EFFECT, e);
        }
#ifdef CK_OSH_OP_MUTANT_KEEP_GAP_ENDS
        if (e != OSH_E_OK) { /* the session keeps the ends of a command that did not start: its neighbours hang */
            carried = nrd;
            continue;
        }
#endif
        /* the stage holds its own copies now (or never started): the session keeps none, so EOF and BROKEN_PIPE flow */
        opened[nopened++] = k + 1u < n ? nwr : stdout_w; /* each session handle closed exactly once */
        close_fds(s, fd, opened, nopened);
        if (k + 1u == n)
            stdout_w.index = stdout_w.generation = 0;
        carried = nrd;
        if (e == OSH_E_OK && s->hooks.crash_after_effect_at == r->seq && s->hooks.crash_cmd == k) {
            /* the adapter is lost right after this spawn: intents stay open, the stages stay as they are */
            close_end(s, &carried);
            r->crashed = 1;
            r->stop = 1;
            r->error = OSH_E_OUTCOME_UNKNOWN;
            r->outcome = OSH_O_OUTCOME_UNKNOWN;
            r->status = 70;
            for (unsigned q = 0; q <= k; q++)
                if (lv->ids[q])
                    cmd_set(r, q, 70, OSH_O_OUTCOME_UNKNOWN, OSH_E_OUTCOME_UNKNOWN);
            return;
        }
    }
    close_end(s, &stdout_w);

    if (fail != OSH_E_OK) {
        /* ST7 step 6: cancel and reap what started, close every end, PARTIAL_LAUNCH (or the error if none started) */
        int st[OSH_MAX_CMDS];
        unsigned started = r->started;
        (void)cleanup_live(s, st);
        for (unsigned k = 0; k < n; k++) {
            if (k == fail_at) {
                cmd_set(r, k, 1, OSH_O_FAILED_NO_EFFECT, fail);
            } else if (k > fail_at) {
                cmd_set(r, k, -1, OSH_O_NOT_STARTED, OSH_E_OK);
            } else if (st[k] >= 0) {
                cmd_set(r, k, st[k], OSH_O_CANCELLED, OSH_E_INTERRUPTED);
                close_intent(s, lv->intent[k], OSH_O_CANCELLED, OSH_E_INTERRUPTED, st[k]);
            }
        }
        for (unsigned i = 0; i < OSH_PUMPS_MAX; i++)
            if (lv->pumps[i].used)
                close_intent(s, lv->pumps[i].intent, OSH_O_COMPLETED, OSH_E_OK, 0); /* the open itself happened */
        r->error = r->launch_error = started ? OSH_E_PARTIAL_LAUNCH : fail;
        r->outcome = started ? OSH_O_CANCELLED : OSH_O_FAILED_NO_EFFECT;
        r->status = 1;
        return;
    }

    /* step 7: run every stage to its end; the adapter moves the bytes of the ends it holds in between */
    uint64_t t0 = tab->run.now(tab->run.ctx);
    unsigned turns = 0;
    int cancelled = 0;
    for (;;) {
        uint64_t moved = pump_all(s, env, r);
        unsigned alive = stages_alive(s), open = 0;
        for (unsigned i = 0; i < OSH_PUMPS_MAX; i++)
            open += lv->pumps[i].used && !lv->pumps[i].done;
        if (!alive && !open)
            break;
        unsigned ran = alive ? ck_task_run(tab, RUN_TURNS) : 0;
        turns += ran;
        if (s->hooks.snapshot_at == r->seq && !r->mid_alive && turns >= s->hooks.snapshot_after_turns) {
            r->mid_alive = stages_alive(s);
            r->mid_in_flight = bytes_in_pipes(nat->pool);
        }
        int intr = s->hooks.interrupt_at == r->seq && turns >= s->hooks.interrupt_after_turns;
        int late = nat->deadline_ticks && tab->run.now(tab->run.ctx) - t0 >= nat->deadline_ticks; /* UNVERIFIED U10 */
        if (alive && (intr || late)) {
            cancelled = 1; /* section 12: interrupt, bounded grace, terminate, reap */
            break;
        }
        if (!ran && !moved) {
            cancelled = 2; /* nothing can move: never spin; clean up and report it as IO (UNVERIFIED U10) */
            break;
        }
    }
    int st[OSH_MAX_CMDS];
    for (unsigned k = 0; k < OSH_MAX_CMDS; k++)
        st[k] = -1;
    if (cancelled) {
        (void)cleanup_live(s, st);
    } else {
        for (unsigned k = 0; k < n; k++) {
            uint32_t v = 0;
            if (lv->ids[k] && ck_task_wait(tab, lv->ids[k], 0, &v) == OSH_E_OK)
                st[k] = (int)v;
            lv->ids[k] = 0;
        }
        for (unsigned i = 0; i < OSH_PUMPS_MAX; i++)
            if (lv->pumps[i].used)
                close_end(s, &lv->pumps[i].h);
        lv->active = 0;
    }
    for (unsigned k = 0; k < n; k++) {
        if (st[k] < 0)
            continue; /* never started: already set */
        unsigned oc = cancelled ? OSH_O_CANCELLED : OSH_O_COMPLETED;
        unsigned er = cancelled == 1 ? OSH_E_INTERRUPTED : cancelled == 2 ? OSH_E_IO : OSH_E_OK;
        cmd_set(r, k, st[k], oc, er);
        close_intent(s, lv->intent[k], oc, er, st[k]);
    }
    for (unsigned i = 0; i < OSH_PUMPS_MAX; i++) {
        struct osh_pump *p = &lv->pumps[i];
        if (!p->used)
            continue;
        if (p->overflow && r->cmd_error[p->cmd] == OSH_E_OK)
            r->cmd_error[p->cmd] = p->loc < 0 ? OSH_E_IO : OSH_E_LIMIT;
        close_intent(s, p->intent, OSH_O_COMPLETED, p->overflow ? OSH_E_LIMIT : OSH_E_OK, p->overflow);
    }
    r->steps = tab->stats.steps - steps0;
    r->blocks = tab->stats.blocks - blocks0;
    r->wakeups = tab->stats.wakeups - wakes0;
    r->turns = turns;
    r->max_resident = nat->pool->total.max_resident;

    /* ST7 step 7: the pipeline status is the last command's status */
#ifdef CK_OSH_OP_MUTANT_FIRST_STATUS
    r->status = r->cmd_status[0];
#else
    r->status = r->cmd_status[n - 1u];
#endif
    r->error = OSH_E_OK;
    for (unsigned k = 0; k < n && r->error == OSH_E_OK; k++) /* the first error that is not "not found" (Omega) */
        if (r->cmd_error[k] != OSH_E_OK && r->cmd_error[k] != OSH_E_NOT_FOUND)
            r->error = r->cmd_error[k];
    if (cancelled) {
        r->error = cancelled == 1 ? OSH_E_INTERRUPTED : OSH_E_IO;
        r->outcome = OSH_O_CANCELLED;
        r->stop = 1;
    } else if (r->started) {
        r->outcome = OSH_O_COMPLETED;
    } else {
        if (r->error == OSH_E_OK)
            r->error = r->cmd_error[n - 1u]; /* nothing started: say why */
        r->outcome = any_refused && r->error != OSH_E_NOT_FOUND ? OSH_O_FAILED_NO_EFFECT : OSH_O_NOT_STARTED;
    }
}

void osh_op_exec(struct osh_session *s, const struct osh_env *env, const uint64_t *req, const uint64_t *out, size_t n_out,
                 struct osh_result *r)
{
    zero(r, sizeof *r);
    r->seq = ++s->pipelines;
    for (unsigned k = 0; k < OSH_MAX_CMDS; k++)
        r->cmd_status[k] = -1;

    /* 1. the record is the expander's output (or AIEN's), but it sits in memory a unit could write: check it before use */
    uint64_t ncmds = req[1], out_used = req[4];
    if (req[0] != OSH_REQ_MAGIC || req[5] != 1 || req[2] > 3 || (req[3] & ~OSH_REQ_F_DIRECT) || req[6] || req[7] ||
        ncmds < 1 || ncmds > OSH_MAX_CMDS || out_used > n_out) {
        finish(r, OSH_E_INVALID_ARG, OSH_O_NOT_STARTED);
        r->launch_error = OSH_E_INVALID_ARG;
        return;
    }
    r->ncmds = (unsigned)ncmds;
    digest_request(req, (unsigned)ncmds, out, out_used, r->digest);
    int direct = (req[3] & OSH_REQ_F_DIRECT) != 0;
    for (unsigned k = 0; k < ncmds; k++)
        if (!block_ok(req + OSH_REQ_HDR + (size_t)k * OSH_REQ_CMD_CELLS, out, out_used, direct)) {
            refuse_all(r, OSH_E_INVALID_ARG, OSH_O_NOT_STARTED);
            return;
        }

    /* 2. RESOLVE (section 8.3): a name is a lookup key only */
    struct cmd_plan plan[OSH_MAX_CMDS];
    unsigned nstage = 0;
    for (unsigned k = 0; k < ncmds; k++) {
        unsigned e = resolve(s, req + OSH_REQ_HDR + (size_t)k * OSH_REQ_CMD_CELLS, out, (unsigned)ncmds, &plan[k]);
        if (e != OSH_E_OK) {
            refuse_all(r, e, OSH_O_NOT_STARTED);
            return;
        }
        nstage += plan[k].how == RS_STAGE;
    }
    if (plan[0].how == RS_KLOG) {
        klog_exec(s, env, req + OSH_REQ_HDR, out, r);
        return;
    }
    if (nstage == 0) { /* nothing resolved: every command is not found, no effect, no authority needed */
        for (unsigned k = 0; k < ncmds; k++)
            cmd_set(r, k, 127, OSH_O_FAILED_NO_EFFECT, OSH_E_NOT_FOUND);
        finish(r, OSH_E_NOT_FOUND, OSH_O_NOT_STARTED);
        return;
    }
    stage_exec(s, env, req, out, plan, r);
}
