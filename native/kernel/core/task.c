/* task.c -- native pipeline task lifecycle (aienos-mv#429). Contract, stage ABI and limits: task.h. */
#include "task.h"

#define SCHED_ID(slot) ((uint32_t)(slot) + 1u) /* run-queue id of a slot (never 0) */

static void zero(void *d, size_t n)
{
    volatile uint8_t *p = d;
    while (n--)
        *p++ = 0;
}

static uint64_t id_make(unsigned slot, uint32_t epoch)
{
    return ((uint64_t)CK_TASK_ID_TAG << 56) | ((uint64_t)(slot & 0xFFu) << 32) | epoch;
}

/* The live record an id names, or 0 (section 4.4: an id from another incarnation is STALE). */
static struct ck_task_rec *id_resolve(const struct ck_task_tab *tab, uint64_t id)
{
    if ((id >> 56) != CK_TASK_ID_TAG || ((id >> 40) & 0xFFFFu) != 0)
        return 0;
    unsigned slot = (unsigned)((id >> 32) & 0xFFu);
    if (slot >= CK_TASK_MAX)
        return 0;
    struct ck_task_rec *r = (struct ck_task_rec *)&tab->tasks[slot];
    if (r->state == CK_TS_FREE || r->epoch != (uint32_t)id)
        return 0;
    return r;
}

static unsigned slot_of(const struct ck_task_tab *tab, const struct ck_task_rec *r)
{
    return (unsigned)(r - tab->tasks);
}

/* ---- wake: the pipe pool calls this for an end with a waiter (our watches) ---- */
static void make_ready(struct ck_task_tab *tab, struct ck_task_rec *r)
{
    if (r->watch_h.generation) {
        ck_pipe_unwatch(tab->pool, &r->caps, r->watch_h);
        r->watch_h.generation = 0;
    }
    r->state = CK_TS_READY;
    r->wait = 0;
    ck_sched_enqueue(&tab->sched, SCHED_ID(slot_of(tab, r)), CK_PRIO_NORMAL);
}

static void on_wake(void *ctx, unsigned pipe, unsigned end)
{
    struct ck_task_tab *tab = ctx;
#ifdef CK_TASK_MUTANT_NO_WAKE
    /* TEST-ONLY mutant (make task-mutants): pipe events never wake a blocked task. test_task must fail. */
    (void)tab;
    (void)pipe;
    (void)end;
    return;
#endif
    for (unsigned i = 0; i < CK_TASK_MAX; i++) {
        struct ck_task_rec *r = &tab->tasks[i];
        if (r->state != CK_TS_BLOCKED || !r->watch_h.generation || r->watch_pipe != pipe || r->watch_end != end)
            continue;
        r->wakes++;
        tab->stats.wakeups++;
        make_ready(tab, r);
    }
}

void ck_task_init(struct ck_task_tab *tab, struct ck_pipe_pool *pool, struct ck_cap_table *session,
                  const uint8_t principal[32], const struct ck_task_runner *run)
{
    zero(tab, sizeof *tab);
    tab->pool = pool;
    tab->session = session;
    for (unsigned i = 0; i < 32; i++)
        tab->principal[i] = principal[i];
    tab->run = *run;
    tab->hooks.ctx = tab;
    tab->hooks.wait = 0; /* a stage never parks a kernel stack: blocking calls get WOULD_BLOCK */
    tab->hooks.wake = on_wake;
    pool->hooks = &tab->hooks;
    static const uint8_t one_class[1] = { 0 };
    ck_sched_init(&tab->sched, 1, one_class, CK_TASK_MAX, 1);
}

void ck_task_image_set(struct ck_task_tab *tab, unsigned i, const void *img)
{
    if (i < CK_TASK_IMAGES)
        tab->images[i] = img;
}

int ck_task_tab_empty(const struct ck_task_tab *tab)
{
    for (unsigned i = 0; i < CK_TASK_MAX; i++)
        if (tab->tasks[i].state != CK_TS_FREE)
            return 0;
    return 1;
}

/* ---- section 9: the binding ---- */
unsigned ck_task_perm_check(const struct ck_task_tab *tab, const struct ck_task_binding *b)
{
    if (!b)
        return OSH_E_DENIED;
    if (b->domain != CK_TASK_DOMAIN_KERNEL)
        return OSH_E_CAP_DOMAIN_MISMATCH; /* no bridge in v1: a hosted (or unknown) domain never names a kernel cap */
    if (b->cap_generation > 0xFFFFFFFFull)
        return OSH_E_CAP_GEN_NARROW; /* never narrowed to fit the 32-bit kernel generation */
    if (b->cap_generation == 0)
        return OSH_E_INVALID_ARG;
    unsigned diff = 0;
    for (unsigned i = 0; i < 32; i++)
        diff |= (unsigned)(b->principal_id[i] ^ tab->principal[i]);
    if (diff)
        return OSH_E_DENIED; /* the capability is usable only by the principal it was granted to */
    if (b->resource_class != CK_TASK_RC_TASK || b->operation != CK_TASK_OP_SPAWN)
        return OSH_E_DENIED;
    if (!tab->session || b->cap_index >= tab->session->n)
        return OSH_E_STALE;
    struct ck_handle h = { b->cap_index, (uint32_t)b->cap_generation };
    uint32_t res = 0;
    int r = ck_cap_lookup(tab->session, h, CK_R_WRITE, &res);
    if (r == CK_CAP_INVALID)
        return OSH_E_STALE; /* slot closed or revoked, or reused under a newer generation */
    if (r != CK_CAP_OK || res != CK_TASK_SPAWN_RESOURCE)
        return OSH_E_DENIED;
    return OSH_E_OK;
}

/* ---- ending and reaping ---- */
static void finish(struct ck_task_tab *tab, struct ck_task_rec *r, unsigned end, uint32_t status, unsigned err)
{
    if (r->state == CK_TS_EXITED || r->state == CK_TS_FREE)
        return;
    if (r->watch_h.generation) {
        ck_pipe_unwatch(tab->pool, &r->caps, r->watch_h);
        r->watch_h.generation = 0;
    }
    ck_sched_remove(&tab->sched, SCHED_ID(slot_of(tab, r))); /* not queued when BLOCKED: that error is fine */
    r->state = CK_TS_EXITED;
    r->wait = 0;
    r->end = end;
    r->status = status;
    r->err = err;
    r->intr_left = 0;
    /* section 12: close every end it held now, so a reader sees EOF and a writer BROKEN_PIPE at once */
    ck_pipe_release_table(tab->pool, &r->caps);
    for (unsigned k = 0; k < 3; k++)
        r->stdio[k].generation = 0;
    zero(r->ws, sizeof r->ws);
    zero(r->in, sizeof r->in);
    zero(r->out, sizeof r->out);
    r->out_off = r->out_len = 0;
}

static void reap(struct ck_task_tab *tab, struct ck_task_rec *r)
{
    r->state = CK_TS_FREE; /* the epoch stays: the old id is STALE from now on */
    tab->stats.reaped++;
}

/* Leave the run queue and watch end h until the pool's wake hook fires for it. */
static void block(struct ck_task_tab *tab, struct ck_task_rec *r, struct ck_handle h, unsigned why)
{
    uint32_t res = 0;
    unsigned e = ck_pipe_watch(tab->pool, &r->caps, h);
    if (e != OSH_E_OK || ck_cap_lookup(&r->caps, h, 0, &res) != CK_CAP_OK) {
        finish(tab, r, CK_TE_STREAM, 137, e != OSH_E_OK ? e : OSH_E_STALE);
        return;
    }
    r->watch_h = h;
    r->watch_pipe = (res >> 20) & 7u;
    r->watch_end = (res >> 23) & 1u;
    r->state = CK_TS_BLOCKED;
    r->wait = why;
    r->blocks++;
    tab->stats.blocks++;
    ck_sched_remove(&tab->sched, SCHED_ID(slot_of(tab, r)));
}

enum { FL_DONE = 0, FL_BLOCKED, FL_RETRY, FL_ENDED };
/* Write the pending output. may_block 0 (interrupt grace): what does not fit is dropped instead of waiting. */
static int flush(struct ck_task_tab *tab, struct ck_task_rec *r, int may_block)
{
    while (r->out_off < r->out_len) {
        if (!r->stdio[1].generation) { /* no stdout: the output goes nowhere */
            r->out_off = r->out_len;
            break;
        }
        size_t n = 0;
        unsigned e = ck_stream_write(tab->pool, &r->caps, r->stdio[1], r->out + r->out_off, r->out_len - r->out_off,
                                     0, &n);
        if (e == OSH_E_OK) {
            r->out_off += (uint32_t)n;
            r->bytes_out += n;
            continue;
        }
        if (e == OSH_E_WOULD_BLOCK) {
            if (!may_block) {
                r->out_off = r->out_len;
                break;
            }
            block(tab, r, r->stdio[1], CK_TW_WRITABLE);
            return r->state == CK_TS_BLOCKED ? FL_BLOCKED : FL_ENDED;
        }
        if (e == OSH_E_INTERRUPTED)
            return FL_RETRY; /* section 10: an interrupted call moved nothing; retried next turn */
#ifndef CK_TASK_MUTANT_GRACE_BROKEN_PIPE
        if (e == OSH_E_BROKEN_PIPE && !may_block) {
            /* aienos-mv#432, UNVERIFIED (T1): in the interrupt grace a reader that is gone is the cancellation itself
             * (a peer stage ended its own grace first), so the output is dropped like output that does not fit and the
             * stage still ends by the interrupt (130). Without this the status of a cancelled producer depended on
             * which stage the round robin reached first (141 or 130). */
            r->out_off = r->out_len;
            break;
        }
#endif
        if (e == OSH_E_BROKEN_PIPE)
            finish(tab, r, CK_TE_BROKEN_PIPE, 141, e);
        else
            finish(tab, r, CK_TE_STREAM, 137, e);
        return FL_ENDED;
    }
    r->out_off = r->out_len = 0;
    return FL_DONE;
}

static void protocol(struct ck_task_tab *tab, struct ck_task_rec *r)
{
    finish(tab, r, CK_TE_PROTOCOL, 139, OSH_E_IO);
}

/* One scheduler turn of a READY task: flush, read, one step, flush. */
static void turn(struct ck_task_tab *tab, struct ck_task_rec *r)
{
    tab->stats.turns++;
    if (r->state != CK_TS_READY) { /* the run queue never holds a waiting task; counted so a test can prove it */
        tab->stats.blocked_runs++;
        ck_sched_remove(&tab->sched, SCHED_ID(slot_of(tab, r)));
        return;
    }
    int intr = r->intr_left != 0;
    if (r->out_off < r->out_len && flush(tab, r, !intr) != FL_DONE)
        return;
    if (r->done) {
        finish(tab, r, CK_TE_EXIT, r->status, OSH_E_OK);
        return;
    }
    if ((r->max_steps && r->steps >= r->max_steps) || (r->max_task_ticks && r->ticks >= r->max_task_ticks)) {
        finish(tab, r, CK_TE_LIMIT, 137, OSH_E_LIMIT);
        return;
    }
    size_t in_len = 0;
    if (!r->stdio[0].generation)
        r->eof = 1;
    if (!intr && !r->eof) {
        size_t n = 0;
        unsigned e = ck_stream_read(tab->pool, &r->caps, r->stdio[0], r->in, r->in_chunk, 0, &n);
        if (e == OSH_E_WOULD_BLOCK) {
            block(tab, r, r->stdio[0], CK_TW_READABLE);
            return;
        }
        if (e == OSH_E_INTERRUPTED)
            return;
        if (e != OSH_E_OK) {
            finish(tab, r, CK_TE_STREAM, 137, e);
            return;
        }
        if (n == 0)
            r->eof = 1;
        in_len = n;
        r->bytes_in += n;
    }
    uint64_t *w = r->ws;
    w[CK_STG_W_FLAGS] = (r->eof ? CK_STG_F_EOF : 0) | (intr ? CK_STG_F_INTR : 0);
    w[CK_STG_W_OUT_LEN] = 0;
    w[CK_STG_W_EXIT] = 0;
    struct ck_task_step st = { 0, 0, 0, 0 };
    tab->run.step(tab->run.ctx, tab->images[r->image], r->in, in_len, w, r->max_ticks, &st);
    zero(r->in, in_len);
    r->steps++;
    r->ticks += st.ticks;
    tab->stats.steps++;
    if (st.cls == CK_TASK_STEP_TRAPPED) {
        finish(tab, r, CK_TE_TRAPPED, 134, OSH_E_IO);
        return;
    }
    if (st.cls == CK_TASK_STEP_UNKNOWN) {
        finish(tab, r, CK_TE_FAULT, st.code == CK_TASK_UNK_TICK_OVERRUN ? 137 : 139, OSH_E_OUTCOME_UNKNOWN);
        return;
    }
    if (st.cls == CK_TASK_STEP_REFUSED) {
        finish(tab, r, CK_TE_REFUSED, 126, OSH_E_DENIED);
        return;
    }
    if (st.cls != CK_TASK_STEP_RETURNED) {
        protocol(tab, r);
        return;
    }
    /* every cell below was written by the stage: untrusted, each checked before use */
    uint64_t v = st.value, ol = w[CK_STG_W_OUT_LEN];
    if ((v != CK_STG_MORE && v != CK_STG_DONE) || ol > CK_TASK_OUT_MAX) {
        protocol(tab, r);
        return;
    }
    for (uint64_t i = 0; i < ol; i++) {
        uint64_t c = w[CK_STG_W_OUT + i];
        if (c > 255) {
            protocol(tab, r);
            return;
        }
        r->out[i] = (uint8_t)c;
    }
    if (v == CK_STG_DONE) {
        if (w[CK_STG_W_EXIT] > 255) {
            protocol(tab, r);
            return;
        }
        r->done = 1;
        r->status = (uint32_t)w[CK_STG_W_EXIT];
    } else if (in_len == 0 && ol == 0 && !intr) {
        protocol(tab, r); /* no progress: stepping it again would be busy polling */
        return;
    }
    r->out_off = 0;
    r->out_len = (uint32_t)ol;
    if (intr)
        r->intr_left--;
    int f = r->out_len ? flush(tab, r, !intr) : FL_DONE;
    if (f == FL_ENDED || f == FL_BLOCKED)
        return;
    if (f == FL_DONE && r->done) {
        finish(tab, r, CK_TE_EXIT, r->status, OSH_E_OK);
        return;
    }
    if (intr && r->intr_left == 0 && r->state == CK_TS_READY)
        finish(tab, r, CK_TE_INTERRUPTED, 130, OSH_E_INTERRUPTED);
}

unsigned ck_task_run(struct ck_task_tab *tab, unsigned turns)
{
    unsigned ran = 0;
    while (ran < turns) {
        uint32_t sid = 0;
        if (ck_sched_tick(&tab->sched, 0, &sid) != 1 || sid == 0 || sid > CK_TASK_MAX) {
            tab->stats.idle_returns++;
            break;
        }
        turn(tab, &tab->tasks[sid - 1]);
        ran++;
    }
    return ran;
}

/* ---- TASK_SPAWN ---- */
static unsigned end_of(const struct ck_cap_table *t, struct ck_handle h, unsigned *end)
{
    uint32_t res = 0;
    int r = ck_cap_lookup(t, h, 0, &res);
    if (r == CK_CAP_INVALID)
        return OSH_E_STALE;
    if (r != CK_CAP_OK || (res >> 24) != CK_PIPE_RES_TAG)
        return OSH_E_DENIED;
    *end = (res >> 23) & 1u;
    return OSH_E_OK;
}

unsigned ck_task_spawn(struct ck_task_tab *tab, const struct ck_task_spawn_req *rq, uint64_t *id)
{
    if (!tab || !rq || !id)
        return OSH_E_INVALID_ARG;
    *id = 0;
    if (rq->nparams > CK_TASK_PARAMS || rq->in_chunk > CK_TASK_IN_MAX)
        return OSH_E_INVALID_ARG;
    unsigned e = ck_task_perm_check(tab, &rq->binding);
    if (e != OSH_E_OK) {
        tab->stats.refused++;
        return e;
    }
    if (rq->image >= CK_TASK_IMAGES || !tab->images[rq->image])
        return OSH_E_NOT_FOUND;
#ifndef CK_TASK_MUTANT_SKIP_ADMIT
    e = tab->run.admit(tab->run.ctx, tab->images[rq->image]);
#else
    /* TEST-ONLY mutant (make task-mutants): spawn skips the image admission (receipt and entry). test_task must fail. */
    e = OSH_E_OK;
#endif
    if (e != OSH_E_OK) {
        tab->stats.refused++;
        return e >= OSH_E_COUNT ? OSH_E_IO : e;
    }
    for (unsigned k = 0; k < 3; k++) {
        if (!rq->stdio[k].generation)
            continue;
        unsigned end = 0;
        e = end_of(tab->session, rq->stdio[k], &end);
        if (e != OSH_E_OK)
            return e;
        if (end != (k == 0 ? CK_PIPE_END_READ : CK_PIPE_END_WRITE))
            return OSH_E_DENIED; /* stdin must be a read end, stdout and stderr write ends */
    }
    unsigned slot = CK_TASK_MAX;
    for (unsigned i = 0; i < CK_TASK_MAX; i++)
        if (tab->tasks[i].state == CK_TS_FREE) {
            slot = i;
            break;
        }
    if (slot == CK_TASK_MAX)
        return OSH_E_LIMIT;
    struct ck_task_rec *r = &tab->tasks[slot];
    uint32_t epoch = r->epoch + 1u;
    if (epoch == 0)
        epoch = 1;
    zero(r, sizeof *r);
    ck_cap_init(&r->caps, ((uint32_t)CK_TASK_ID_TAG << 24) | slot, CK_CAP_SLOTS);
    for (unsigned k = 0; k < 3; k++) {
        if (!rq->stdio[k].generation)
            continue;
        unsigned right = k == 0 ? CK_R_READ : CK_R_WRITE;
#ifdef CK_TASK_MUTANT_FULL_RIGHTS
        /* TEST-ONLY mutant (make task-mutants): the stage's ends carry every right the spawner had. Must fail. */
        ck_cap_rights(tab->session, rq->stdio[k], &right);
#endif
        e = ck_stream_dup(tab->pool, tab->session, rq->stdio[k], &r->caps, right, &r->stdio[k]);
        if (e != OSH_E_OK) {
            ck_pipe_release_table(tab->pool, &r->caps);
            zero(r, sizeof *r);
            r->epoch = epoch - 1u;
            return e;
        }
    }
    r->epoch = epoch;
    r->image = rq->image;
    r->in_chunk = rq->in_chunk ? rq->in_chunk : CK_TASK_IN_MAX;
    r->max_ticks = rq->max_ticks;
    r->max_steps = rq->max_steps;
    r->max_task_ticks = rq->max_task_ticks;
    r->ws[CK_STG_W_MAGIC] = CK_STG_MAGIC;
    r->ws[CK_STG_W_ABI] = CK_STG_ABI;
    for (unsigned i = 0; i < rq->nparams; i++)
        r->ws[CK_STG_W_PARAM + i] = rq->params[i];
    r->state = CK_TS_READY;
    ck_sched_enqueue(&tab->sched, SCHED_ID(slot), CK_PRIO_NORMAL);
    tab->stats.spawned++;
    *id = id_make(slot, epoch);
    return OSH_E_OK;
}

/* ---- TASK_WAIT, TASK_STATE, TASK_CANCEL ---- */
unsigned ck_task_wait(struct ck_task_tab *tab, uint64_t id, uint64_t deadline, uint32_t *status)
{
    struct ck_task_rec *r = id_resolve(tab, id);
    if (!r || !status)
        return r ? OSH_E_INVALID_ARG : OSH_E_STALE;
    uint64_t start = tab->run.now(tab->run.ctx);
    for (;;) {
        if (r->state == CK_TS_EXITED) {
            *status = r->status;
            reap(tab, r);
            return OSH_E_OK;
        }
        if (deadline == 0)
            return OSH_E_WOULD_BLOCK;
        if (deadline != CK_TASK_FOREVER && tab->run.now(tab->run.ctx) - start >= deadline)
            return OSH_E_WOULD_BLOCK;
        if (ck_task_run(tab, 1) == 0)
            return OSH_E_WOULD_BLOCK; /* nothing can run: only the caller can unblock it, so return, never spin */
    }
}

unsigned ck_task_state(const struct ck_task_tab *tab, uint64_t id, struct ck_task_info *info)
{
    const struct ck_task_rec *r = id_resolve(tab, id);
    if (!r)
        return OSH_E_STALE;
    if (!info)
        return OSH_E_INVALID_ARG;
    info->state = r->state;
    info->wait = r->wait;
    info->end = r->end;
    info->err = r->err;
    info->status = r->state == CK_TS_EXITED ? r->status : 0;
    info->steps = r->steps;
    info->ticks = r->ticks;
    info->bytes_in = r->bytes_in;
    info->bytes_out = r->bytes_out;
    info->blocks = r->blocks;
    info->wakes = r->wakes;
    return OSH_E_OK;
}

unsigned ck_task_cancel(struct ck_task_tab *tab, uint64_t id, unsigned mode)
{
    struct ck_task_rec *r = id_resolve(tab, id);
    if (!r)
        return OSH_E_STALE;
    if (mode != CK_TASK_CANCEL_INTERRUPT && mode != CK_TASK_CANCEL_TERMINATE)
        return OSH_E_INVALID_ARG;
    if (r->state == CK_TS_EXITED)
        return OSH_E_OK;
    tab->stats.cancels++;
    if (mode == CK_TASK_CANCEL_TERMINATE) {
        finish(tab, r, CK_TE_TERMINATED, 143, OSH_E_INTERRUPTED);
        return OSH_E_OK;
    }
    if (r->intr_left)
        return OSH_E_OK; /* already interrupted: the grace does not restart */
    r->intr_left = CK_TASK_GRACE_STEPS;
    r->out_off = r->out_len = 0; /* output not yet delivered is dropped by an interrupt (UNVERIFIED) */
    if (r->state == CK_TS_BLOCKED) {
        r->wakes++;
        make_ready(tab, r); /* cancelled: a waiting task is woken to see its interrupt */
    }
    return OSH_E_OK;
}

/* ---- pipelines ---- */
static void close_session(struct ck_task_tab *tab, struct ck_handle *h)
{
    if (h->generation)
        ck_stream_close(tab->pool, tab->session, *h);
    h->generation = 0;
}

static void kill_reap(struct ck_task_tab *tab, uint64_t id)
{
    struct ck_task_rec *r = id_resolve(tab, id);
    if (!r)
        return;
    finish(tab, r, CK_TE_TERMINATED, 143, OSH_E_INTERRUPTED);
    reap(tab, r);
}

unsigned ck_task_pipeline_spawn(struct ck_task_tab *tab, const struct ck_task_spawn_req *stages, unsigned n,
                                struct ck_handle in, struct ck_handle out, struct ck_task_pipeline *pl)
{
    if (!tab || !stages || !pl || n == 0)
        return OSH_E_INVALID_ARG;
    zero(pl, sizeof *pl);
    if (n > CK_TASK_MAX || n - 1u > CK_PIPE_POOL_PIPES)
        return OSH_E_LIMIT;
    pl->n = n;
    struct ck_handle rd = { 0, 0 };
    unsigned e = OSH_E_OK;
    for (unsigned k = 0; k < n; k++) {
        struct ck_handle nrd = { 0, 0 }, wr = { 0, 0 };
        if (k + 1 < n) {
            e = ck_pipe_create(tab->pool, tab->session, &nrd, &wr);
            if (e != OSH_E_OK) {
                close_session(tab, &rd);
                break;
            }
        }
        struct ck_task_spawn_req rq = stages[k];
        rq.stdio[0] = k == 0 ? in : rd;
        rq.stdio[1] = k + 1 == n ? out : wr;
        e = ck_task_spawn(tab, &rq, &pl->ids[k]);
        /* the session never keeps an inner end: the stage holds its own copy (or failed to start) */
        close_session(tab, &rd);
#ifndef CK_TASK_MUTANT_KEEP_SESSION_END
        close_session(tab, &wr);
#else
        /* TEST-ONLY mutant (make task-mutants): the session keeps its copy of the write end, so the next stage never
         * sees EOF. test_task must fail. */
        (void)wr;
#endif
        rd = nrd;
        if (e != OSH_E_OK) {
            close_session(tab, &rd);
            break;
        }
        pl->started++;
    }
    if (e == OSH_E_OK)
        return OSH_E_OK;
    if (pl->started == 0)
        return e; /* nothing started: the stage's own error, nothing left allocated */
    /* section 10: a later stage could not start; cancel and reap what did, then report PARTIAL_LAUNCH */
#ifndef CK_TASK_MUTANT_NO_PARTIAL_REAP
    for (unsigned k = 0; k < pl->started; k++)
        kill_reap(tab, pl->ids[k]);
#else
    /* TEST-ONLY mutant (make task-mutants): the started stages are left running. test_task must fail. */
    (void)kill_reap;
#endif
    pl->cause = e;
    tab->stats.partial_launches++;
    return OSH_E_PARTIAL_LAUNCH;
}

unsigned ck_task_pipeline_wait(struct ck_task_tab *tab, struct ck_task_pipeline *pl, uint64_t deadline, uint32_t *last)
{
    if (!tab || !pl || !last || pl->n == 0 || pl->started != pl->n)
        return OSH_E_INVALID_ARG;
    uint64_t start = tab->run.now(tab->run.ctx);
    for (unsigned k = 0; k < pl->n; k++) {
        if (!pl->ids[k])
            continue; /* already reaped by an earlier call */
        uint64_t left = deadline;
        if (deadline != CK_TASK_FOREVER) {
            uint64_t used = tab->run.now(tab->run.ctx) - start;
            left = used >= deadline ? 0 : deadline - used;
        }
        unsigned e = ck_task_wait(tab, pl->ids[k], left, &pl->status[k]);
        if (e != OSH_E_OK)
            return e;
        pl->ids[k] = 0;
    }
    *last = pl->status[pl->n - 1];
    return OSH_E_OK;
}

unsigned ck_task_pipeline_cancel(struct ck_task_tab *tab, struct ck_task_pipeline *pl)
{
    if (!tab || !pl)
        return 0;
    unsigned live = 0, killed = 0;
    for (unsigned k = 0; k < pl->started; k++)
        if (pl->ids[k] && ck_task_cancel(tab, pl->ids[k], CK_TASK_CANCEL_INTERRUPT) == OSH_E_OK)
            live++;
    ck_task_run(tab, live * CK_TASK_GRACE_STEPS); /* the bounded grace: at most this many turns, fewer if idle */
    for (unsigned k = 0; k < pl->started; k++) {
        struct ck_task_rec *r = pl->ids[k] ? id_resolve(tab, pl->ids[k]) : 0;
        if (!r)
            continue;
        if (r->state != CK_TS_EXITED) {
            finish(tab, r, CK_TE_TERMINATED, 143, OSH_E_INTERRUPTED);
            killed++;
        }
        pl->status[k] = r->status;
        reap(tab, r);
        pl->ids[k] = 0;
    }
    return killed;
}

/* ---- versioned op table ---- */
static const uint8_t op_args[] = { [CK_TASK_OP_SPAWN] = 27, [CK_TASK_OP_WAIT] = 2, [CK_TASK_OP_STATE] = 1,
                                   [CK_TASK_OP_CANCEL] = 2 };

static int raw_handle(uint64_t raw, struct ck_handle *h)
{
    if (raw == 0) {
        h->index = h->generation = 0;
        return 1;
    }
    return ck_handle_from_raw(raw, h) == 0;
}

unsigned ck_task_dispatch(struct ck_task_tab *tab, const uint64_t *frame, size_t ncells, struct ck_task_ret *ret)
{
    if (!ret)
        return OSH_E_INVALID_ARG;
    ret->error = OSH_E_OK;
    ret->value = 0;
    unsigned e = OSH_E_OK;
    if (!tab || !frame || ncells < 4 || frame[0] != CK_PIPE_FRAME_MAGIC || frame[3] != 0)
        e = OSH_E_INVALID_ARG;
    else if (frame[1] != CK_TASK_ABI_VERSION || frame[2] < CK_TASK_OP_SPAWN || frame[2] > CK_TASK_OP_CANCEL)
        e = OSH_E_NOT_SUPPORTED;
    else if (ncells != 4u + op_args[frame[2]])
        e = OSH_E_INVALID_ARG;
    if (e != OSH_E_OK)
        return ret->error = e;
    const uint64_t *a = frame + 4;
    uint32_t st = 0;
    switch (frame[2]) {
    case CK_TASK_OP_SPAWN: {
        struct ck_task_spawn_req rq;
        zero(&rq, sizeof rq);
        if (a[0] >= CK_TASK_IMAGES || !raw_handle(a[1], &rq.stdio[0]) || !raw_handle(a[2], &rq.stdio[1]) ||
            !raw_handle(a[3], &rq.stdio[2]) || a[4] > CK_TASK_IN_MAX || a[12] > 0xFFu || a[13] > 0xFFFFFFFFull ||
            a[15] > 0xFFFFu || a[16] > 0xFFFFu || a[17] > CK_TASK_PARAMS)
            return ret->error = OSH_E_INVALID_ARG;
        rq.image = (unsigned)a[0];
        rq.in_chunk = (uint32_t)a[4];
        rq.max_ticks = a[5];
        rq.max_steps = a[6];
        rq.max_task_ticks = a[7];
        for (unsigned i = 0; i < 32; i++)
            rq.binding.principal_id[i] = (uint8_t)(a[8 + i / 8] >> (8 * (i % 8)));
        rq.binding.domain = (uint8_t)a[12];
        rq.binding.cap_index = (uint32_t)a[13];
        rq.binding.cap_generation = a[14];
        rq.binding.resource_class = (uint16_t)a[15];
        rq.binding.operation = (uint16_t)a[16];
        rq.nparams = (unsigned)a[17];
        for (unsigned i = 0; i < rq.nparams; i++)
            rq.params[i] = a[18 + i];
        e = ck_task_spawn(tab, &rq, &ret->value);
        break;
    }
    case CK_TASK_OP_WAIT:
        e = ck_task_wait(tab, a[0], a[1], &st);
        if (e == OSH_E_OK)
            ret->value = st;
        break;
    case CK_TASK_OP_STATE: {
        struct ck_task_info in;
        e = ck_task_state(tab, a[0], &in);
        if (e == OSH_E_OK)
            ret->value = (uint64_t)in.state | ((uint64_t)in.wait << 8) | ((uint64_t)in.end << 16) |
                         ((uint64_t)in.status << 32);
        break;
    }
    default: /* CK_TASK_OP_CANCEL */
        e = a[1] > 0xFFFFFFFFull ? OSH_E_INVALID_ARG : ck_task_cancel(tab, a[0], (unsigned)a[1]);
        break;
    }
    return ret->error = e;
}
