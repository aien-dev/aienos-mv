/* osh_journal.c -- the durable effect journal of the AIENOS OSH adapter (aienos-mv#431). See osh_journal.h.
 * Pure logic over the canonical Cortex (rx_cortex.h) and SHA-256: no task, pipe, shell or device code, so inspection
 * and recovery stay available when the OSH adapter cannot run. Host-tested in tests/test_osh_op.c. */
#include "osh_journal.h"

#include "sha256.h"

/* TEST-ONLY mutants (host only, make osh-op-mutant): each must make tests/test_osh_op.c FAIL.
 *   NO_POISON          a failed append does not poison the journal (later effects would run unrecorded)
 *   RECOVER_COMPLETED  recovery writes COMPLETED instead of OUTCOME_UNKNOWN for an open intent */
#if (defined(CK_OSH_OP_MUTANT_NO_POISON) || defined(CK_OSH_OP_MUTANT_RECOVER_COMPLETED)) && !__STDC_HOSTED__
#error "CK_OSH_OP_MUTANT_* are host-test-only mutants"
#endif

static void zero(void *p, size_t n)
{
    uint8_t *b = p;
    while (n--)
        *b++ = 0;
}

static void copy(void *d, const void *s, size_t n)
{
    uint8_t *a = d;
    const uint8_t *b = s;
    while (n--)
        *a++ = *b++;
}

static void put32(uint64_t *w, const uint8_t b[32])
{
    for (unsigned i = 0; i < 4; i++) {
        uint64_t v = 0;
        for (unsigned k = 0; k < 8; k++)
            v |= (uint64_t)b[8 * i + k] << (8 * k);
        w[i] = v;
    }
}

static void get32(const uint64_t *w, uint8_t b[32])
{
    for (unsigned i = 0; i < 4; i++)
        for (unsigned k = 0; k < 8; k++)
            b[8 * i + k] = (uint8_t)(w[i] >> (8 * k));
}

/* ---- payload layouts (UNVERIFIED J1) ----
 * common: w0 OSHJ_MAGIC, w2 seq | epoch << 32, w3..w6 request digest.
 * INTENT    32 words: w1 effect | cmd<<8 | ncmds<<16 | argc<<24 | mode<<32 | name_len<<40 | domain<<48;
 *                     w3 resource_class | operation<<16 | cap_index<<32 (the digest is w5..w8 here); w4 cap_generation;
 *                     w9..12 principal, w13..16 stage_id, w17..20 input_hash, w21..24 target_hash, w25..28 target_pre,
 *                     w29..31 name.
 * STARTED    8 words: w1 effect | cmd<<8 | what<<16.
 * OUTCOME    8 words: w1 effect | cmd<<8 | outcome<<16 | err<<24 | recovered<<32; w7 status (u32).
 * RECEIPT 12+n words: w1 n | outcome<<16 | err<<24 | recovered<<32 | ncmds<<40; w7 status; w8.. outcome ids;
 *                     then 4 words SHA-256 over those outcomes' Cortex digests.
 * RECONCILE 13 words: w1 verdict | domain<<8 | resource_class<<16 | operation<<32; w7..10 principal; w11 cap_index;
 *                     w12 cap_generation. */
#define W_INTENT 32u
#define W_SHORT 8u
#define W_RECONCILE 13u

static int is_osh_kind(uint32_t k)
{
    return k >= OSHJ_K_INTENT && k <= OSHJ_K_RECONCILE;
}

static uint32_t cls_of(uint32_t kind)
{
    switch (kind) {
    case OSHJ_K_INTENT: return CX_PLAN;
    case OSHJ_K_STARTED: case OSHJ_K_OUTCOME: return CX_EXECUTION;
    default: return CX_EVIDENCE;
    }
}

int osh_journal_usable(const struct osh_journal *j)
{
    return j && j->open && !j->readonly && !j->poisoned;
}

static void scan_epoch(struct osh_journal *j)
{
    j->max_epoch = 0;
    for (uint64_t id = 1; id <= j->cx.n; id++) {
        const CxObject *o = cx_get(&j->cx, id);
        if (!o || !is_osh_kind(o->kind) || o->n < W_SHORT)
            continue;
        const uint64_t *p = cx_payload(&j->cx, o);
        if (p[0] == OSHJ_MAGIC && (uint32_t)(p[2] >> 32) > j->max_epoch)
            j->max_epoch = (uint32_t)(p[2] >> 32);
    }
}

int osh_journal_open(struct osh_journal *j, const CxJournalOps *ops, void *ctx, int durable, const char *label, int ro)
{
    zero(j, sizeof *j);
    j->label = label;
    int rc = cx_open_with(&j->cx, ops, ctx, OSHJ_NAME, OSHJ_SUBJECTS, ro ? CX_OPEN_READONLY : CX_OPEN_SYNC);
    j->last_rc = rc;
    if (rc != CX_OK)
        return rc;
    j->open = 1;
    j->readonly = (uint8_t)(ro != 0);
    j->durable = (uint8_t)(durable != 0);
    scan_epoch(j);
    return CX_OK;
}

void osh_journal_close(struct osh_journal *j)
{
    if (j->open)
        cx_free(&j->cx);
    j->open = 0;
}

static int put(struct osh_journal *j, uint32_t kind, uint32_t epoch, uint64_t link0, uint64_t link1, const uint64_t *w,
               uint32_t n, uint64_t *id)
{
    if (!osh_journal_usable(j)) {
        j->last_rc = CX_ERR_IO;
        return CX_ERR_IO;
    }
    CxHeader h;
    zero(&h, sizeof h);
    h.cls = cls_of(kind);
    h.kind = kind;
    h.subject = OSHJ_SUBJECT;
    h.t = j->cx.n + 1u; /* per subject time never goes backwards: the record count is monotonic */
    h.generation = epoch;
    h.protect = CX_PROT_EFFECT_RECEIPT | CX_PROT_AUTHORITY;
    h.links[0] = link0;
    h.links[1] = link1;
    uint64_t got = 0;
    int rc = cx_append(&j->cx, &h, w, n, &got);
    if (rc != CX_OK) {
#ifndef CK_OSH_OP_MUTANT_NO_POISON
        j->poisoned = 1; /* sticky: a journal that failed once records nothing more this session (Omega's rule) */
#endif
        j->failed++;
        j->last_rc = rc;
        return rc;
    }
    j->appends++;
    if (epoch > j->max_epoch)
        j->max_epoch = epoch;
    if (id)
        *id = got;
    return CX_OK;
}

static void common(uint64_t *w, const struct oshj_effect *e)
{
    w[0] = OSHJ_MAGIC;
    w[2] = (uint64_t)e->seq | (uint64_t)e->epoch << 32;
    put32(w + 3, e->digest);
}

int oshj_put_intent(struct osh_journal *j, const struct oshj_effect *e, uint64_t *id)
{
    uint64_t w[W_INTENT];
    zero(w, sizeof w);
    w[0] = OSHJ_MAGIC;
    w[1] = (uint64_t)e->effect | (uint64_t)e->cmd << 8 | (uint64_t)e->ncmds << 16 | (uint64_t)e->argc << 24 |
           (uint64_t)e->mode << 32 | (uint64_t)e->name_len << 40 | (uint64_t)e->domain << 48;
    w[2] = (uint64_t)e->seq | (uint64_t)e->epoch << 32;
    w[3] = (uint64_t)e->resource_class | (uint64_t)e->operation << 16 | (uint64_t)e->cap_index << 32;
    w[4] = e->cap_generation;
    put32(w + 5, e->digest);
    put32(w + 9, e->principal);
    put32(w + 13, e->stage_id);
    put32(w + 17, e->input_hash);
    put32(w + 21, e->target_hash);
    put32(w + 25, e->target_pre);
    uint8_t nm[OSHJ_NAME_MAX];
    zero(nm, sizeof nm);
    copy(nm, e->name, e->name_len <= OSHJ_NAME_MAX ? e->name_len : OSHJ_NAME_MAX);
    for (unsigned i = 0; i < 3; i++)
        for (unsigned k = 0; k < 8; k++)
            w[29 + i] |= (uint64_t)nm[8 * i + k] << (8 * k);
    return put(j, OSHJ_K_INTENT, e->epoch, 0, 0, w, W_INTENT, id);
}

int oshj_put_started(struct osh_journal *j, uint64_t intent, const struct oshj_effect *e, unsigned what, uint64_t *id)
{
    uint64_t w[W_SHORT];
    zero(w, sizeof w);
    common(w, e);
    w[1] = (uint64_t)e->effect | (uint64_t)e->cmd << 8 | (uint64_t)(what & 0xffu) << 16;
    return put(j, OSHJ_K_STARTED, e->epoch, intent, 0, w, W_SHORT, id);
}

int oshj_put_outcome(struct osh_journal *j, uint64_t intent, const struct oshj_effect *e, unsigned outcome, unsigned err,
                     int32_t status, int recovered, uint64_t *id)
{
    uint64_t w[W_SHORT];
    zero(w, sizeof w);
    common(w, e);
    w[1] = (uint64_t)e->effect | (uint64_t)e->cmd << 8 | (uint64_t)(outcome & 0xffu) << 16 |
           (uint64_t)(err & 0xffu) << 24 | (uint64_t)(recovered != 0) << 32;
    w[7] = (uint32_t)status;
    return put(j, OSHJ_K_OUTCOME, e->epoch, intent, 0, w, W_SHORT, id);
}

int oshj_put_receipt(struct osh_journal *j, const struct oshj_effect *pl, const uint64_t *outcomes, unsigned n,
                     unsigned outcome, unsigned err, int32_t status, int recovered, uint64_t *id)
{
    uint64_t w[12u + OSHJ_RECEIPT_MAX];
    if (n > OSHJ_RECEIPT_MAX || !j->open)
        return CX_ERR_ARG;
    zero(w, sizeof w);
    common(w, pl);
    w[1] = (uint64_t)n | (uint64_t)(outcome & 0xffu) << 16 | (uint64_t)(err & 0xffu) << 24 |
           (uint64_t)(recovered != 0) << 32 | (uint64_t)pl->ncmds << 40;
    w[7] = (uint32_t)status;
    sha256_ctx c;
    sha256_init(&c);
    for (unsigned i = 0; i < n; i++) {
        w[8 + i] = outcomes[i];
        const CxObject *o = cx_get(&j->cx, outcomes[i]);
        if (o)
            sha256_update(&c, o->digest, 32);
    }
    uint8_t d[32];
    sha256_final(&c, d);
    put32(w + 8 + n, d);
    return put(j, OSHJ_K_RECEIPT, pl->epoch, n ? outcomes[0] : 0, 0, w, 12u + n, id);
}

int oshj_put_reconcile(struct osh_journal *j, uint64_t intent, uint64_t unknown_outcome, unsigned verdict,
                       const uint8_t principal[32], uint8_t domain, uint32_t cap_index, uint64_t cap_generation,
                       uint16_t resource_class, uint16_t operation, uint64_t *id)
{
    struct oshj_item it;
    if (!j->open || !oshj_item(&j->cx, intent, &it))
        return CX_ERR_ARG;
    uint64_t w[W_RECONCILE];
    zero(w, sizeof w);
    common(w, &it.e);
    w[1] = (uint64_t)(verdict & 0xffu) | (uint64_t)domain << 8 | (uint64_t)resource_class << 16 |
           (uint64_t)operation << 32;
    put32(w + 7, principal);
    w[11] = cap_index;
    w[12] = cap_generation;
    return put(j, OSHJ_K_RECONCILE, it.e.epoch, intent, unknown_outcome, w, W_RECONCILE, id);
}

/* ---- decoding ---- */

/* the payload of an OSH record of `kind` that decodes, or NULL */
static const uint64_t *rec(const CxStore *cx, uint64_t id, uint32_t kind, const CxObject **out)
{
    const CxObject *o = cx_get(cx, id);
    if (!o || o->kind != kind)
        return 0;
    uint32_t need = kind == OSHJ_K_INTENT ? W_INTENT : kind == OSHJ_K_RECONCILE ? W_RECONCILE : W_SHORT;
    if (o->n < need)
        return 0;
    const uint64_t *p = cx_payload(cx, o);
    if (p[0] != OSHJ_MAGIC)
        return 0;
    if (kind == OSHJ_K_RECEIPT && o->n != 12u + (p[1] & 0xffffu))
        return 0;
    if (out)
        *out = o;
    return p;
}

static void decode_intent(const uint64_t *w, struct oshj_effect *e)
{
    zero(e, sizeof *e);
    e->effect = (uint8_t)w[1];
    e->cmd = (uint8_t)(w[1] >> 8);
    e->ncmds = (uint8_t)(w[1] >> 16);
    e->argc = (uint8_t)(w[1] >> 24);
    e->mode = (uint8_t)(w[1] >> 32);
    e->name_len = (uint8_t)(w[1] >> 40);
    if (e->name_len > OSHJ_NAME_MAX)
        e->name_len = OSHJ_NAME_MAX;
    e->domain = (uint8_t)(w[1] >> 48);
    e->seq = (uint32_t)w[2];
    e->epoch = (uint32_t)(w[2] >> 32);
    e->resource_class = (uint16_t)w[3];
    e->operation = (uint16_t)(w[3] >> 16);
    e->cap_index = (uint32_t)(w[3] >> 32);
    e->cap_generation = w[4];
    get32(w + 5, e->digest);
    get32(w + 9, e->principal);
    get32(w + 13, e->stage_id);
    get32(w + 17, e->input_hash);
    get32(w + 21, e->target_hash);
    get32(w + 25, e->target_pre);
    for (unsigned i = 0; i < 3; i++)
        for (unsigned k = 0; k < 8; k++)
            e->name[8 * i + k] = (uint8_t)(w[29 + i] >> (8 * k));
}

static int same_pipeline(const uint64_t *a, const uint64_t *b)
{
    return a[2] == b[2]; /* seq | epoch << 32 */
}

int oshj_item(const CxStore *cx, uint64_t intent, struct oshj_item *it)
{
    zero(it, sizeof *it);
    const uint64_t *w = rec(cx, intent, OSHJ_K_INTENT, 0);
    if (!w)
        return 0;
    it->intent_id = intent;
    decode_intent(w, &it->e);
    it->status = -1;
    for (uint64_t id = intent + 1u; id <= cx->n; id++) {
        const CxObject *o = cx_get(cx, id);
        const uint64_t *p;
        if (o->links[0] == intent && (p = rec(cx, id, OSHJ_K_STARTED, 0)) != 0 && !it->started_id)
            it->started_id = id;
        else if (o->links[0] == intent && (p = rec(cx, id, OSHJ_K_OUTCOME, 0)) != 0 && !it->outcome_id) {
            it->outcome_id = id;
            it->outcome = (uint8_t)(p[1] >> 16);
            it->err = (uint8_t)(p[1] >> 24);
            it->recovered = (uint8_t)((p[1] >> 32) & 1u);
            it->status = (int32_t)(uint32_t)p[7];
        } else if (o->links[0] == intent && (p = rec(cx, id, OSHJ_K_RECONCILE, 0)) != 0 && !it->reconcile_id) {
            it->reconcile_id = id;
            it->verdict = (uint8_t)p[1];
        } else if ((p = rec(cx, id, OSHJ_K_RECEIPT, 0)) != 0 && same_pipeline(p, w)) {
            it->sealed = 1;
        }
    }
    if (it->reconcile_id)
        it->cat = OSHJ_CAT_RECONCILED;
    else if (it->outcome_id)
        it->cat = it->recovered ? OSHJ_CAT_RECOVERED : OSHJ_CAT_FINISHED;
    else
        it->cat = it->started_id ? OSHJ_CAT_UNKNOWN_STARTED : OSHJ_CAT_UNKNOWN_INTENT;
    return 1;
}

/* 1 when no intent before `id` belongs to the same pipeline */
static int first_of_pipeline(const CxStore *cx, uint64_t id, const uint64_t *w)
{
    for (uint64_t k = 1; k < id; k++) {
        const uint64_t *p = rec(cx, k, OSHJ_K_INTENT, 0);
        if (p && same_pipeline(p, w))
            return 0;
    }
    return 1;
}

/* every intent of the pipeline of intent `id` has an outcome */
static int pipeline_closed(const CxStore *cx, uint64_t id, const uint64_t *w)
{
    for (uint64_t k = id; k <= cx->n; k++) {
        const uint64_t *p = rec(cx, k, OSHJ_K_INTENT, 0);
        struct oshj_item it;
        if (p && same_pipeline(p, w) && (!oshj_item(cx, k, &it) || !it.outcome_id))
            return 0;
    }
    return 1;
}

static int pipeline_sealed(const CxStore *cx, const uint64_t *w)
{
    for (uint64_t k = 1; k <= cx->n; k++) {
        const uint64_t *p = rec(cx, k, OSHJ_K_RECEIPT, 0);
        if (p && same_pipeline(p, w))
            return 1;
    }
    return 0;
}

void oshj_inspect(const CxStore *cx, struct oshj_report *rep, void (*each)(void *u, const struct oshj_item *it), void *u)
{
    zero(rep, sizeof *rep);
    rep->records = cx->n;
    rep->chain_ok = cx_verify_chain(cx) == CX_OK;
    for (uint64_t id = 1; id <= cx->n; id++) {
        const CxObject *o = cx_get(cx, id);
        if (!is_osh_kind(o->kind)) {
            rep->other++;
            continue;
        }
        const uint64_t *w = rec(cx, id, o->kind, 0);
        if (!w) {
            rep->bad++;
            continue;
        }
        rep->osh++;
        if ((uint32_t)(w[2] >> 32) > rep->max_epoch)
            rep->max_epoch = (uint32_t)(w[2] >> 32);
        switch (o->kind) {
        case OSHJ_K_INTENT: {
            struct oshj_item it;
            rep->intents++;
            (void)oshj_item(cx, id, &it);
            rep->cat[it.cat]++;
            for (uint64_t k = id + 1u; k <= cx->n; k++) /* more than one outcome for an intent */
                if (cx_get(cx, k)->links[0] == id && rec(cx, k, OSHJ_K_OUTCOME, 0) && k != it.outcome_id)
                    rep->dup_outcomes++;
            if (first_of_pipeline(cx, id, w)) {
                rep->pipelines++;
                if (pipeline_sealed(cx, w))
                    rep->sealed++;
                else if (pipeline_closed(cx, id, w))
                    rep->unsealed_finished++;
            }
            if (each)
                each(u, &it);
            break;
        }
        case OSHJ_K_RECEIPT:
            rep->receipts++;
            break;
        default: {
            /* STARTED, OUTCOME, RECONCILE must close an intent that comes before them */
            if (!o->links[0] || o->links[0] >= id || !rec(cx, o->links[0], OSHJ_K_INTENT, 0)) {
                rep->bad++;
                rep->osh--;
                break;
            }
            if (o->kind == OSHJ_K_STARTED)
                rep->started++;
            else if (o->kind == OSHJ_K_OUTCOME)
                rep->outcomes++;
            else
                rep->reconciles++;
        }
        }
    }
}

const char *oshj_cat_name(unsigned c)
{
    static const char *const n[OSHJ_CATS] = { "NONE", "FINISHED", "UNKNOWN_INTENT_ONLY", "UNKNOWN_STARTED",
                                              "RECOVERED_UNKNOWN", "RECONCILED" };
    return c < OSHJ_CATS ? n[c] : "NONE";
}

const char *oshj_effect_name(unsigned e)
{
    return e == OSHJ_EF_CONSOLE ? "console_write" : e == OSHJ_EF_SPAWN ? "task_spawn" : e == OSHJ_EF_OPEN_WRITE
                                                                                          ? "open_write"
                                                                                          : "unknown";
}

/* ---- recovery ---- */

int oshj_recover(struct osh_journal *j, struct oshj_recovery *rv, void (*each)(void *u, const struct oshj_item *it),
                 void *u)
{
    zero(rv, sizeof *rv);
    if (!osh_journal_usable(j))
        return CX_ERR_IO;
    uint64_t n0 = j->cx.n; /* only what the journal held before this recovery */
    for (uint64_t id = 1; id <= n0; id++) {
        struct oshj_item it;
        if (!oshj_item(&j->cx, id, &it) || it.outcome_id)
            continue;
        if (it.started_id)
            rv->unknown_started++;
        else
            rv->unknown_intent++;
        uint64_t oid = 0;
#ifdef CK_OSH_OP_MUTANT_RECOVER_COMPLETED
        if (oshj_put_outcome(j, id, &it.e, OSHJ_O_COMPLETED, 0, 0, 0, &oid) == CX_OK) {
#else
        if (oshj_put_outcome(j, id, &it.e, OSHJ_O_OUTCOME_UNKNOWN, 11u /* OSH_E_OUTCOME_UNKNOWN */, 70, 1, &oid) ==
            CX_OK) {
#endif
            rv->outcomes_written++;
            it.outcome_id = oid;
            it.outcome = OSHJ_O_OUTCOME_UNKNOWN;
            it.recovered = 1;
            it.status = 70;
        } else {
            rv->failed++;
        }
        if (each)
            each(u, &it);
    }
    /* seal every pipeline left without a receipt (crash during receipt writing, or recovered just now) */
    for (uint64_t id = 1; id <= n0; id++) {
        const uint64_t *w = rec(&j->cx, id, OSHJ_K_INTENT, 0);
        if (!w || !first_of_pipeline(&j->cx, id, w) || pipeline_sealed(&j->cx, w))
            continue;
        uint64_t ids[OSHJ_RECEIPT_MAX];
        unsigned n = 0, unknown = 0;
        int32_t last = -1;
        struct oshj_item first;
        (void)oshj_item(&j->cx, id, &first);
        for (uint64_t k = id; k <= n0; k++) {
            const uint64_t *p = rec(&j->cx, k, OSHJ_K_INTENT, 0);
            struct oshj_item it;
            if (!p || !same_pipeline(p, w) || !oshj_item(&j->cx, k, &it) || !it.outcome_id || n >= OSHJ_RECEIPT_MAX)
                continue;
            ids[n++] = it.outcome_id;
            unknown += it.outcome == OSHJ_O_OUTCOME_UNKNOWN;
            last = it.status;
        }
        if (oshj_put_receipt(j, &first.e, ids, n, unknown ? OSHJ_O_OUTCOME_UNKNOWN : OSHJ_O_COMPLETED,
                             unknown ? 11u : 0u, last, 1, 0) == CX_OK)
            rv->receipts_written++;
        else
            rv->failed++;
    }
    return rv->failed ? CX_ERR_IO : CX_OK;
}

#ifdef CK_OSH_OP_TEST
/* ---- TEST-ONLY RAM backend (not durable) ---- */

void oshj_ram_init(struct oshj_ram *r, uint8_t *buf, size_t cap)
{
    zero(r, sizeof *r);
    r->buf = buf;
    r->cap = cap;
}

static int ram_open(void *ctx, const char *name, int ro, void **h)
{
    struct oshj_ram *r = ctx;
    (void)name;
    if (r->unavailable)
        return CX_ERR_IO;
    if (ro) {
        r->ro_handles++;
        *h = &r->ro_handles;
        return CX_OK;
    }
    if (r->writer)
        return CX_ERR_WRITER;
    r->writer = 1;
    *h = r;
    return CX_OK;
}

static int ram_size(void *ctx, void *h, size_t *len)
{
    (void)h;
    *len = ((struct oshj_ram *)ctx)->len;
    return CX_OK;
}

static int ram_read(void *ctx, void *h, size_t off, uint8_t *out, size_t n)
{
    struct oshj_ram *r = ctx;
    (void)h;
    if (off > r->len || n > r->len - off)
        return CX_ERR_IO;
    copy(out, r->buf + off, n);
    return CX_OK;
}

static int ram_append(void *ctx, void *h, const uint8_t *b, size_t n, int sync)
{
    struct oshj_ram *r = ctx;
    (void)sync;
    if (h != (void *)r)
        return CX_ERR_IO;
    r->attempts++;
    if (r->fail_at && r->attempts == r->fail_at)
        return CX_ERR_IO; /* the injected failure: nothing written */
    if (n > r->cap - r->len)
        return CX_ERR_IO;
    copy(r->buf + r->len, b, n);
    r->len += n;
    r->appends++;
    return CX_OK;
}

static int ram_truncate(void *ctx, void *h, size_t len)
{
    struct oshj_ram *r = ctx;
    return h == (void *)r && len == r->len ? CX_OK : CX_ERR_IO;
}

static void ram_close(void *ctx, void *h)
{
    struct oshj_ram *r = ctx;
    if (h == (void *)r)
        r->writer = 0;
    else if (r->ro_handles)
        r->ro_handles--;
}

const CxJournalOps oshj_ram_ops = { ram_open, ram_size, ram_read, ram_append, ram_truncate, ram_close };
#endif
