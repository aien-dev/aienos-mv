/* pipe.c -- see pipe.h. Bounded byte pipes over ipc.h capability tables; no allocation, no libc. */
#include "pipe.h"

static void bytes_zero(uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++)
        d[i] = 0;
}

static void stats_zero(struct ck_pipe_stats *s)
{
    *s = (struct ck_pipe_stats){ 0 };
}

void ck_pipe_pool_init(struct ck_pipe_pool *p, const struct ck_pipe_hooks *hooks)
{
    for (unsigned i = 0; i < CK_PIPE_POOL_PIPES; i++) {
        struct ck_pipe *q = &p->pipes[i];
        bytes_zero(q->buf, CK_PIPE_CAPACITY);
        q->head = q->len = q->epoch = 0;
        q->reader_refs = q->writer_refs = 0;
        q->flags = 0;
        q->waiting[0] = q->waiting[1] = 0;
        stats_zero(&q->stats);
    }
    p->hooks = hooks;
    stats_zero(&p->total);
    p->live = 0;
}

int ck_pipe_pool_empty(const struct ck_pipe_pool *p)
{
    for (unsigned i = 0; i < CK_PIPE_POOL_PIPES; i++)
        if (p->pipes[i].flags & CK_PIPE_LIVE)
            return 0;
    return p->live == 0;
}

/* Every counter is kept twice: this incarnation and the pool total. */
#define BUMP(p, q, field, by)                                                                                          \
    do {                                                                                                               \
        (q)->stats.field += (by);                                                                                      \
        (p)->total.field += (by);                                                                                      \
    } while (0)

static void wake(struct ck_pipe_pool *p, unsigned idx, unsigned end)
{
    struct ck_pipe *q = &p->pipes[idx];
    if (!q->waiting[end] || !p->hooks || !p->hooks->wake)
        return;
    BUMP(p, q, wakeups, 1);
    p->hooks->wake(p->hooks->ctx, idx, end);
}

static unsigned data_right(unsigned end)
{
    return end == CK_PIPE_END_READ ? CK_R_READ : CK_R_WRITE;
}

/* The effect-boundary check, run inside every operation at the moment of its effect.
 * need: rights the capability must hold. want_end: the end the operation needs, or -1 for either.
 * OK with *idx and *end set; otherwise the refusal. A REVOKED pipe is reported only when live_ok is 0. */
static unsigned resolve(struct ck_pipe_pool *p, const struct ck_cap_table *t, struct ck_handle h, unsigned need,
                        int want_end, int live_ok, unsigned *idx, unsigned *end)
{
    if (!t || h.generation == 0 || h.index >= t->n)
        return OSH_E_INVALID_ARG;
    uint32_t res = 0;
    int r = ck_cap_lookup(t, h, need, &res);
    if (r == CK_CAP_INVALID)
        return OSH_E_STALE; /* slot closed, or reused under a newer generation */
    if (r != CK_CAP_OK)
        return OSH_E_DENIED; /* the capability lacks a right this effect needs */
    if ((res >> 24) != CK_PIPE_RES_TAG)
        return OSH_E_DENIED; /* not a stream capability */
    unsigned i = (res >> 20) & 7u, e = (res >> 23) & 1u;
    uint32_t epoch = res & CK_PIPE_EPOCH_MAX;
    if (i >= CK_PIPE_POOL_PIPES)
        return OSH_E_DENIED;
    if (want_end >= 0 && e != (unsigned)want_end)
        return OSH_E_DENIED; /* a read end cannot write, a write end cannot read */
    const struct ck_pipe *q = &p->pipes[i];
    if (!(q->flags & CK_PIPE_LIVE) || q->epoch != epoch)
        return OSH_E_STALE; /* section 4.4: an id from an earlier incarnation */
    if (!live_ok && (q->flags & CK_PIPE_REVOKED))
        return OSH_E_REVOKED;
    *idx = i;
    *end = e;
    return OSH_E_OK;
}

static unsigned refuse(struct ck_pipe_pool *p, unsigned e)
{
    if (e == OSH_E_DENIED || e == OSH_E_STALE || e == OSH_E_REVOKED)
        p->total.refused++;
    return e;
}

unsigned ck_pipe_create(struct ck_pipe_pool *p, struct ck_cap_table *t, struct ck_handle *rd, struct ck_handle *wr)
{
    if (!t || !rd || !wr)
        return OSH_E_INVALID_ARG;
    unsigned idx = CK_PIPE_POOL_PIPES;
    for (unsigned i = 0; i < CK_PIPE_POOL_PIPES; i++)
        if (!(p->pipes[i].flags & CK_PIPE_LIVE) && p->pipes[i].epoch < CK_PIPE_EPOCH_MAX) {
            idx = i;
            break;
        }
    if (idx == CK_PIPE_POOL_PIPES)
        return OSH_E_LIMIT;
    struct ck_pipe *q = &p->pipes[idx];
    uint32_t epoch = q->epoch + 1;
    struct ck_handle r, w;
    if (ck_cap_insert(t, ck_pipe_resource(idx, CK_PIPE_END_READ, epoch), CK_R_READ | CK_PIPE_CTL_RIGHTS, &r) !=
        CK_CAP_OK)
        return OSH_E_LIMIT;
    if (ck_cap_insert(t, ck_pipe_resource(idx, CK_PIPE_END_WRITE, epoch), CK_R_WRITE | CK_PIPE_CTL_RIGHTS, &w) !=
        CK_CAP_OK) {
        ck_cap_remove(t, r); /* nothing stays allocated on failure */
        return OSH_E_LIMIT;
    }
    q->epoch = epoch;
    q->head = q->len = 0;
    q->reader_refs = q->writer_refs = 1;
    q->flags = CK_PIPE_LIVE;
    q->waiting[0] = q->waiting[1] = 0;
    stats_zero(&q->stats);
    p->live++;
    *rd = r;
    *wr = w;
    return OSH_E_OK;
}

static unsigned read_once(struct ck_pipe_pool *p, const struct ck_cap_table *t, struct ck_handle h, uint8_t *buf,
                          size_t max, size_t *n, unsigned *idx_out)
{
    unsigned idx, end;
    unsigned e = resolve(p, t, h, CK_R_READ, CK_PIPE_END_READ, 0, &idx, &end);
    if (e != OSH_E_OK)
        return refuse(p, e);
    struct ck_pipe *q = &p->pipes[idx];
    *idx_out = idx;
    if (q->flags & CK_PIPE_CANCEL_READ) {
        q->flags &= ~CK_PIPE_CANCEL_READ;
        BUMP(p, q, cancellations, 1);
        return OSH_E_INTERRUPTED;
    }
    if (max == 0)
        return OSH_E_OK;
    if (q->len == 0) {
        if (q->writer_refs == 0) {
            BUMP(p, q, eof, 1);
            return OSH_E_OK; /* every writer closed and the buffer is drained: EOF */
        }
        BUMP(p, q, would_block, 1);
        return OSH_E_WOULD_BLOCK;
    }
    size_t take = max < q->len ? max : q->len;
    for (size_t i = 0; i < take; i++) {
        buf[i] = q->buf[q->head];
        q->buf[q->head] = 0; /* consumed bytes do not stay resident */
        q->head = (q->head + 1) % CK_PIPE_CAPACITY;
    }
    q->len -= (uint32_t)take;
    if (q->len == 0)
        q->head = 0;
    *n = take;
    BUMP(p, q, bytes_read, take);
    BUMP(p, q, reads, 1);
    wake(p, idx, CK_PIPE_END_WRITE);
    return OSH_E_OK;
}

static unsigned write_once(struct ck_pipe_pool *p, const struct ck_cap_table *t, struct ck_handle h,
                           const uint8_t *buf, size_t len, size_t *n, unsigned *idx_out)
{
    unsigned idx, end;
#ifdef CK_PIPE_MUTANT_SKIP_RIGHTS
    /* TEST-ONLY mutant (make pipe-mutant): the write right and the end are not checked. test_pipe must fail. */
    unsigned e = resolve(p, t, h, 0, -1, 0, &idx, &end);
#else
    unsigned e = resolve(p, t, h, CK_R_WRITE, CK_PIPE_END_WRITE, 0, &idx, &end);
#endif
    if (e != OSH_E_OK)
        return refuse(p, e);
    struct ck_pipe *q = &p->pipes[idx];
    *idx_out = idx;
    if (q->flags & CK_PIPE_CANCEL_WRITE) {
        q->flags &= ~CK_PIPE_CANCEL_WRITE;
        BUMP(p, q, cancellations, 1);
        return OSH_E_INTERRUPTED;
    }
    if (q->reader_refs == 0) {
        BUMP(p, q, broken_pipe, 1);
        return OSH_E_BROKEN_PIPE;
    }
    if (len == 0)
        return OSH_E_OK;
    size_t room = CK_PIPE_CAPACITY - q->len;
    if (room == 0) {
        BUMP(p, q, would_block, 1);
        return OSH_E_WOULD_BLOCK;
    }
    size_t put = len < room ? len : room;
    uint32_t at = (q->head + q->len) % CK_PIPE_CAPACITY;
    for (size_t i = 0; i < put; i++) {
        q->buf[at] = buf[i];
        at = (at + 1) % CK_PIPE_CAPACITY;
    }
    q->len += (uint32_t)put;
    *n = put;
    BUMP(p, q, bytes_written, put);
    BUMP(p, q, writes, 1);
    if (q->len > q->stats.max_resident)
        q->stats.max_resident = q->len;
    if (q->len > p->total.max_resident)
        p->total.max_resident = q->len;
    wake(p, idx, CK_PIPE_END_READ);
    return OSH_E_OK;
}

/* Park on (idx, end) through the wait hook. OK = retry; anything else ends the call with no byte moved. */
static unsigned park(struct ck_pipe_pool *p, unsigned idx, unsigned end)
{
    if (!p->hooks || !p->hooks->wait)
        return OSH_E_WOULD_BLOCK;
    struct ck_pipe *q = &p->pipes[idx];
    uint32_t epoch = q->epoch;
    q->waiting[end]++;
    BUMP(p, q, waits, 1);
    unsigned w = p->hooks->wait(p->hooks->ctx, idx, end);
    /* The pipe may have been freed and recreated while parked; count only our own wait out. */
    if (q->epoch == epoch && q->waiting[end])
        q->waiting[end]--;
    if (w != OSH_E_OK) {
        if (w == OSH_E_INTERRUPTED)
            BUMP(p, q, cancellations, 1);
        return w >= OSH_E_COUNT ? OSH_E_IO : w; /* section 8.6: an unknown code is IO, never OK */
    }
    return OSH_E_OK;
}

unsigned ck_stream_read(struct ck_pipe_pool *p, const struct ck_cap_table *t, struct ck_handle h, uint8_t *buf,
                        size_t max, unsigned flags, size_t *n)
{
    if (!n || (max && !buf) || (flags & ~CK_PIPE_F_BLOCK))
        return OSH_E_INVALID_ARG;
    for (;;) {
        unsigned idx = 0;
        *n = 0;
        unsigned e = read_once(p, t, h, buf, max, n, &idx);
        if (e != OSH_E_WOULD_BLOCK || !(flags & CK_PIPE_F_BLOCK))
            return e;
        e = park(p, idx, CK_PIPE_END_READ);
        if (e != OSH_E_OK)
            return e;
    }
}

unsigned ck_stream_write(struct ck_pipe_pool *p, const struct ck_cap_table *t, struct ck_handle h, const uint8_t *buf,
                         size_t len, unsigned flags, size_t *n)
{
    if (!n || (len && !buf) || (flags & ~CK_PIPE_F_BLOCK))
        return OSH_E_INVALID_ARG;
    for (;;) {
        unsigned idx = 0;
        *n = 0;
        unsigned e = write_once(p, t, h, buf, len, n, &idx);
        if (e != OSH_E_WOULD_BLOCK || !(flags & CK_PIPE_F_BLOCK))
            return e;
        e = park(p, idx, CK_PIPE_END_WRITE);
        if (e != OSH_E_OK)
            return e;
    }
}

static void pipe_free(struct ck_pipe_pool *p, struct ck_pipe *q)
{
    bytes_zero(q->buf, CK_PIPE_CAPACITY);
    q->head = q->len = 0;
    q->flags = 0;
    q->waiting[0] = q->waiting[1] = 0;
    p->live--;
}

unsigned ck_stream_close(struct ck_pipe_pool *p, struct ck_cap_table *t, struct ck_handle h)
{
    if (!t || h.generation == 0 || h.index >= t->n)
        return OSH_E_INVALID_ARG;
    uint32_t res = 0;
    if (ck_cap_lookup(t, h, 0, &res) != CK_CAP_OK)
        return refuse(p, OSH_E_STALE); /* already closed, or the slot belongs to someone newer */
    if ((res >> 24) != CK_PIPE_RES_TAG || ((res >> 20) & 7u) >= CK_PIPE_POOL_PIPES)
        return refuse(p, OSH_E_DENIED); /* not ours to close here */
    unsigned idx, end;
    unsigned e = resolve(p, t, h, 0, -1, 1, &idx, &end);
    ck_cap_remove(t, h); /* the handle is released whatever the pipe's state */
    if (e != OSH_E_OK)
        return refuse(p, e);
    struct ck_pipe *q = &p->pipes[idx];
    if (end == CK_PIPE_END_READ) {
        if (q->reader_refs)
            q->reader_refs--;
        if (q->reader_refs == 0)
            wake(p, idx, CK_PIPE_END_WRITE); /* a blocked writer now sees BROKEN_PIPE */
    } else {
        if (q->writer_refs)
            q->writer_refs--;
        if (q->writer_refs == 0)
            wake(p, idx, CK_PIPE_END_READ); /* a blocked reader now sees EOF once drained */
    }
    if (q->reader_refs == 0 && q->writer_refs == 0)
        pipe_free(p, q);
    return OSH_E_OK;
}

unsigned ck_stream_dup(struct ck_pipe_pool *p, struct ck_cap_table *from, struct ck_handle h, struct ck_cap_table *to,
                       unsigned rights, struct ck_handle *out)
{
    if (!to || !out)
        return OSH_E_INVALID_ARG;
    unsigned need = to == from ? CK_R_DERIVE : CK_R_GRANT;
    unsigned idx, end;
    unsigned e = resolve(p, from, h, need, -1, 0, &idx, &end);
    if (e != OSH_E_OK)
        return refuse(p, e);
    unsigned have = 0;
    ck_cap_rights(from, h, &have);
    unsigned dr = data_right(end);
    if (!(rights & dr) || (rights & ~(dr | CK_PIPE_CTL_RIGHTS)))
        return OSH_E_INVALID_ARG; /* a dup carries its end's data right and nothing of the other end */
    if (rights & ~have)
        return refuse(p, OSH_E_DENIED); /* attenuation only: never more than the source holds */
    struct ck_pipe *q = &p->pipes[idx];
    struct ck_handle c;
    if (ck_cap_insert(to, ck_pipe_resource(idx, end, q->epoch), rights, &c) != CK_CAP_OK)
        return OSH_E_LIMIT;
    if (end == CK_PIPE_END_READ)
        q->reader_refs++;
    else
        q->writer_refs++;
    *out = c;
    return OSH_E_OK;
}

unsigned ck_pipe_revoke(struct ck_pipe_pool *p, const struct ck_cap_table *t, struct ck_handle h)
{
    unsigned idx, end;
    unsigned e = resolve(p, t, h, CK_R_REVOKE, -1, 0, &idx, &end);
    if (e != OSH_E_OK)
        return refuse(p, e);
    struct ck_pipe *q = &p->pipes[idx];
    q->flags |= CK_PIPE_REVOKED;
    q->flags &= ~(CK_PIPE_CANCEL_READ | CK_PIPE_CANCEL_WRITE);
    bytes_zero(q->buf, CK_PIPE_CAPACITY); /* buffered bytes are never delivered after a revoke */
    q->head = q->len = 0;
    BUMP(p, q, revocations, 1);
    wake(p, idx, CK_PIPE_END_READ);
    wake(p, idx, CK_PIPE_END_WRITE);
    return OSH_E_OK;
}

unsigned ck_pipe_cancel(struct ck_pipe_pool *p, const struct ck_cap_table *t, struct ck_handle h)
{
    unsigned idx, end, rights = 0;
    unsigned e = resolve(p, t, h, 0, -1, 0, &idx, &end);
    if (e != OSH_E_OK)
        return refuse(p, e);
    ck_cap_rights(t, h, &rights);
    if (!(rights & data_right(end))) /* the end's own data right: READ to interrupt reads, WRITE to interrupt writes */
        return refuse(p, OSH_E_DENIED);
    p->pipes[idx].flags |= end == CK_PIPE_END_READ ? CK_PIPE_CANCEL_READ : CK_PIPE_CANCEL_WRITE;
    wake(p, idx, end);
    return OSH_E_OK;
}

/* A watch is a waiter that is not parked on a stack: it raises the same per-end count park() does, so wake() reaches
 * the hook for it. A revoked pipe can still be watched (the watcher then learns REVOKED on its next call). */
static unsigned watch_resolve(struct ck_pipe_pool *p, const struct ck_cap_table *t, struct ck_handle h, unsigned *idx,
                              unsigned *end)
{
    unsigned rights = 0;
    unsigned e = resolve(p, t, h, 0, -1, 1, idx, end);
    if (e != OSH_E_OK)
        return refuse(p, e);
    ck_cap_rights(t, h, &rights);
    if (!(rights & data_right(*end)))
        return refuse(p, OSH_E_DENIED);
    return OSH_E_OK;
}

unsigned ck_pipe_watch(struct ck_pipe_pool *p, const struct ck_cap_table *t, struct ck_handle h)
{
    unsigned idx, end;
    unsigned e = watch_resolve(p, t, h, &idx, &end);
    if (e != OSH_E_OK)
        return e;
    p->pipes[idx].waiting[end]++;
    BUMP(p, &p->pipes[idx], waits, 1);
    return OSH_E_OK;
}

unsigned ck_pipe_unwatch(struct ck_pipe_pool *p, const struct ck_cap_table *t, struct ck_handle h)
{
    unsigned idx, end;
    unsigned e = watch_resolve(p, t, h, &idx, &end);
    if (e != OSH_E_OK)
        return e;
    if (p->pipes[idx].waiting[end])
        p->pipes[idx].waiting[end]--;
    return OSH_E_OK;
}

unsigned ck_pipe_release_table(struct ck_pipe_pool *p, struct ck_cap_table *t)
{
    unsigned closed = 0;
    for (unsigned i = 0; i < t->n; i++) {
        const struct ck_cap_slot *s = &t->slots[i];
        if (!s->live || s->tombstone || (s->resource >> 24) != CK_PIPE_RES_TAG)
            continue;
        struct ck_handle h = { i, s->generation };
        ck_stream_close(p, t, h);
        closed++;
    }
    return closed;
}

/* ---- versioned op table ---- */
static const uint8_t op_args[] = { [CK_PIPE_OP_CREATE] = 0, [CK_PIPE_OP_READ] = 2, [CK_PIPE_OP_WRITE] = 2,
                                   [CK_PIPE_OP_CLOSE] = 1, [CK_PIPE_OP_DUP] = 3 };
#define OP_MAX CK_PIPE_OP_DUP

unsigned ck_pipe_dispatch(struct ck_pipe_pool *p, struct ck_cap_table *caller, struct ck_cap_table *peer,
                          const uint64_t *frame, size_t ncells, uint8_t *slice, size_t slice_len,
                          struct ck_pipe_ret *ret)
{
    struct ck_pipe_ret r = { 0 };
    unsigned e;
    struct ck_handle h = { 0, 0 };
    if (!frame || ncells < 4 || frame[0] != CK_PIPE_FRAME_MAGIC) {
        e = OSH_E_INVALID_ARG;
        goto done;
    }
    if (frame[1] != CK_PIPE_ABI_VERSION) {
        e = OSH_E_NOT_SUPPORTED; /* section 3: never guess at another version */
        goto done;
    }
    if (frame[3] != 0) {
        e = OSH_E_INVALID_ARG;
        goto done;
    }
    if (frame[2] == 0 || frame[2] > OP_MAX) {
        e = OSH_E_NOT_SUPPORTED; /* unknown op: refused before any effect */
        goto done;
    }
    unsigned op = (unsigned)frame[2];
    if (ncells != 4u + op_args[op] || (op != CK_PIPE_OP_CREATE && ck_handle_from_raw(frame[4], &h) != 0)) {
        e = OSH_E_INVALID_ARG;
        goto done;
    }
    size_t n = 0;
    switch (op) {
    case CK_PIPE_OP_CREATE: {
        struct ck_handle rd, wr;
        e = ck_pipe_create(p, caller, &rd, &wr);
        if (e == OSH_E_OK) {
            r.value = ck_handle_raw(rd);
            r.value2 = ck_handle_raw(wr);
        }
        break;
    }
    case CK_PIPE_OP_READ:
    case CK_PIPE_OP_WRITE:
        if (frame[5] & ~(uint64_t)CK_PIPE_F_BLOCK) {
            e = OSH_E_INVALID_ARG;
            break;
        }
        e = op == CK_PIPE_OP_READ ? ck_stream_read(p, caller, h, slice, slice_len, (unsigned)frame[5], &n)
                                  : ck_stream_write(p, caller, h, slice, slice_len, (unsigned)frame[5], &n);
        r.value = n;
        break;
    case CK_PIPE_OP_CLOSE:
        e = ck_stream_close(p, caller, h);
        break;
    default: { /* CK_PIPE_OP_DUP */
        struct ck_handle c;
        if (frame[5] & ~(uint64_t)CK_R_ALL || frame[6] > 1 || (frame[6] == 1 && !peer)) {
            e = OSH_E_INVALID_ARG;
            break;
        }
        e = ck_stream_dup(p, caller, h, frame[6] ? peer : caller, (unsigned)frame[5], &c);
        if (e == OSH_E_OK)
            r.value = ck_handle_raw(c);
        break;
    }
    }
done:
    r.error = e;
    if (ret)
        *ret = r;
    return e;
}
