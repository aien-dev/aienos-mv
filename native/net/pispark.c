/* AIENOS Pi <-> Spark session test protocol. Spec: pispark.h. No heap, clock or I/O. */
#include "pispark.h"

#include <string.h>

#include "../argus/sha256.h"

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

const char *pss_scenario_name(int scn)
{
    switch (scn) {
    case PSS_NORMAL: return "normal";
    case PSS_WRONG_PEER: return "wrong-peer";
    case PSS_REPLAY: return "replay";
    case PSS_STALE: return "stale-session";
    case PSS_TRUNC: return "truncated";
    case PSS_SILENT: return "silent-peer";
    default: return "unknown";
    }
}

size_t pss_request_build(uint32_t index, const uint8_t nonce[32], size_t len, uint8_t *out, size_t cap)
{
    size_t k;
    if (!nonce || !out || len < PSS_REQ_MIN || len > CT_MAX_PAYLOAD || cap < len)
        return 0;
    memcpy(out, "PSQ1", 4);
    wr32(out + 4, index);
    wr16(out + 8, (uint16_t)len);
    wr16(out + 10, 0);
    memcpy(out + 12, nonce, 32);
    for (k = PSS_REQ_MIN; k < len; k++)
        out[k] = (uint8_t)(nonce[k % 32u] ^ (uint8_t)(k & 0xffu));
    return len;
}

int pss_request_parse(const uint8_t *m, size_t n, uint32_t *index)
{
    size_t k;
    if (!m || !index)
        return PSS_E_ARG;
    if (n < PSS_REQ_MIN || n > CT_MAX_PAYLOAD) return PSS_E_LEN; /* GUARD:pss-req-len */
    if (rd16(m + 8) != n) return PSS_E_LEN; /* GUARD:pss-req-lenfield */
    if (memcmp(m, "PSQ1", 4) != 0) return PSS_E_MAGIC; /* GUARD:pss-req-magic */
    if (rd16(m + 10) != 0) return PSS_E_RESERVED; /* GUARD:pss-req-reserved */
    /* filler that does not follow the nonce: the bytes were changed on the way */
    for (k = PSS_REQ_MIN; k < n; k++) if (m[k] != (uint8_t)(m[12 + k % 32u] ^ (uint8_t)(k & 0xffu))) return PSS_E_HASH; /* GUARD:pss-req-filler */
    *index = rd32(m + 4);
    return PSS_OK;
}

size_t pss_reply_build(uint32_t index, const uint8_t req_hash[32], const uint8_t spark_nonce[16], uint16_t req_len,
                       uint8_t *out, size_t cap)
{
    if (!req_hash || !spark_nonce || !out || cap < PSS_REP_LEN)
        return 0;
    memcpy(out, "PSR1", 4);
    wr32(out + 4, index);
    memcpy(out + 8, req_hash, 32);
    memcpy(out + 40, spark_nonce, 16);
    wr16(out + 56, req_len);
    wr16(out + 58, 0);
    return PSS_REP_LEN;
}

int pss_reply_check(const uint8_t *m, size_t n, uint32_t index, const uint8_t req_hash[32], uint16_t req_len)
{
    if (!m || !req_hash)
        return PSS_E_ARG;
    if (n != PSS_REP_LEN) return PSS_E_LEN; /* GUARD:pss-rep-len */
    if (memcmp(m, "PSR1", 4) != 0) return PSS_E_MAGIC; /* GUARD:pss-rep-magic */
    if (rd16(m + 58) != 0) return PSS_E_RESERVED; /* GUARD:pss-rep-reserved */
    if (rd32(m + 4) != index) return PSS_E_INDEX; /* GUARD:pss-rep-index */
    if (rd16(m + 56) != req_len) return PSS_E_INDEX; /* GUARD:pss-rep-reqlen */
    if (memcmp(m + 8, req_hash, 32) != 0) return PSS_E_HASH; /* GUARD:pss-rep-hash */
    return PSS_OK;
}

/* ================= responder ================= */

static void ev(pss_out *o, int kind, int code, uint32_t index, uint32_t len, const uint8_t *hash)
{
    pss_event *e;
    if (o->nev >= PSS_EV_MAX)
        return;
    e = &o->ev[o->nev++];
    e->kind = kind;
    e->code = code;
    e->index = index;
    e->len = len;
    if (hash)
        memcpy(e->hash, hash, 32);
    else
        memset(e->hash, 0, 32);
}

static void out_push(pss_out *o, const uint8_t *d, size_t n)
{
    if (o->n >= PSS_RESP_MAXOUT || n > SEC_MAX_DATAGRAM || n == 0)
        return;
    memcpy(o->buf[o->n], d, n);
    o->len[o->n++] = n;
}

int pss_resp_init(pss_resp *r, int scenario, pss_stale *stale, uint32_t rto, uint32_t max_retries)
{
    if (!r || scenario < 0 || scenario >= PSS_NSCN || rto == 0)
        return PSS_E_ARG;
    memset(r, 0, sizeof *r);
    r->scenario = scenario;
    r->stale = stale;
    r->rto = rto;
    r->max_retries = max_retries;
    return PSS_OK;
}

static int resp_new_endpoint(pss_resp *r, const uint8_t entropy[64])
{
    sec_config c;
    uint8_t pk[32];
    memset(&c, 0, sizeof c);
    c.role = 1;
    if (sec_test_identity(c.id_sk, pk, r->scenario == PSS_WRONG_PEER ? PSS_LABEL_WRONG : PSS_LABEL_SPARK) != SEC_OK)
        return PSS_E_ARG;
    if (sec_test_identity(pk, c.peer_pk, PSS_LABEL_PI) != SEC_OK) /* we keep only the Pi's public key */
        return PSS_E_ARG;
    memset(pk, 0, sizeof pk);
    memcpy(c.entropy, entropy, 64);
    c.rto = r->rto;
    c.max_retries = r->max_retries;
    c.rekey_interval = 1u << 20;
    c.max_records = 1u << 20;
    if (sec_init(&r->sec, &c) != SEC_OK)
        return PSS_E_ARG;
    memset(&c, 0, sizeof c);
    r->active = 1;
    r->ct_ready = 0;
    r->replies = r->requests = 0;
    r->silent = r->injected = 0;
    r->last_rec_len = 0;
    return PSS_OK;
}

static void resp_ct_ready(pss_resp *r, pss_out *o)
{
    ct_identity id;
    if (r->ct_ready || r->sec.state != SEC_ST_ESTABLISHED)
        return;
    if (ct_test_identity(&id, PSS_LABEL_CTL) != CT_OK ||
        ct_init(&r->ct, &id, 1, rd32(r->sec.session_id), r->rto, r->max_retries) != CT_OK)
        return;
    memset(&id, 0, sizeof id);
    r->ct_ready = 1;
    ev(o, PSS_EV_ESTABLISHED, 0, 0, 0, NULL);
}

/* Seal every M6-A frame that is due and push it; DATA records are remembered (REPLAY source, stale store). */
static void resp_flush(pss_resp *r, uint64_t now, pss_out *o, int inject_for)
{
    static uint8_t frame[CT_MAX_FRAME], dg[SEC_MAX_DATAGRAM];
    size_t fw = 0, w = 0;
    int guard;
    for (guard = 0; guard < 4; guard++) {
        sec_status s = sec_poll_tx(&r->sec, now, dg, sizeof dg, &w);
        if (s != SEC_OK || w == 0)
            break;
        out_push(o, dg, w);
    }
    if (!r->ct_ready)
        return;
    for (guard = 0; guard < (int)CT_WINDOW + 1; guard++) {
        ct_status c = ct_poll_tx(&r->ct, now, frame, sizeof frame, &fw);
        if (c != CT_OK || fw == 0)
            break;
        if (sec_seal(&r->sec, frame, fw, dg, sizeof dg, &w) != SEC_OK)
            break;
        if (inject_for >= 0 && frame[5] == CT_TYPE_DATA && !r->injected) {
            /* the negative control goes on the wire just before the real reply */
            if (r->scenario == PSS_REPLAY && r->last_rec_len) {
                out_push(o, r->last_rec, r->last_rec_len);
                r->injected = 1;
                ev(o, PSS_EV_INJECT, PSS_REPLAY, (uint32_t)inject_for, (uint32_t)r->last_rec_len, NULL);
            } else if (r->scenario == PSS_STALE) {
                if (r->stale && r->stale->len)
                    out_push(o, r->stale->rec, r->stale->len);
                r->injected = 1;
                ev(o, PSS_EV_INJECT, PSS_STALE, (uint32_t)inject_for, r->stale ? (uint32_t)r->stale->len : 0u, NULL);
            } else if (r->scenario == PSS_TRUNC && w > PSS_TRUNC_CUT) {
                out_push(o, dg, w - PSS_TRUNC_CUT);
                r->injected = 1;
                ev(o, PSS_EV_INJECT, PSS_TRUNC, (uint32_t)inject_for, (uint32_t)(w - PSS_TRUNC_CUT), NULL);
            }
        }
        out_push(o, dg, w);
        if (frame[5] == CT_TYPE_DATA) {
            memcpy(r->last_rec, dg, w);
            r->last_rec_len = w;
            if (r->stale) {
                memcpy(r->stale->rec, dg, w);
                r->stale->len = w;
            }
        }
    }
}

int pss_resp_input(pss_resp *r, uint64_t now, const uint8_t *in, size_t n, const uint8_t entropy[80], pss_out *o)
{
    static uint8_t msg[SEC_MAX_PAYLOAD], app[CT_MAX_PAYLOAD], rep[PSS_REP_LEN];
    size_t ml = 0, al = 0;
    int inject_for = -1;
    sec_status s;
    if (!r || !in || !entropy || !o)
        return PSS_E_ARG;
    o->n = o->nev = 0;
    if (n == SEC_HS1_LEN && memcmp(in, "ASC1", 4) == 0 && in[5] == SEC_TYPE_HS1 &&
        (!r->active || (r->sec.state != SEC_ST_START && memcmp(in, r->sec.hs1, SEC_HS1_LEN) != 0))) {
        if (resp_new_endpoint(r, entropy) != PSS_OK)
            return PSS_E_ARG;
        ev(o, PSS_EV_HS1_NEW, 0, 0, 0, NULL);
    }
    if (!r->active) {
        ev(o, PSS_EV_SEC, SEC_ERR_STATE, 0, (uint32_t)n, NULL);
        return PSS_OK;
    }
    if (r->silent) {
        ev(o, PSS_EV_SILENT_DROP, 0, 0, (uint32_t)n, NULL);
        return PSS_OK;
    }
    s = sec_receive(&r->sec, now, in, n, msg, sizeof msg, &ml);
    resp_ct_ready(r, o);
    if (s == SEC_DATA && r->ct_ready) {
        ct_status c = ct_receive(&r->ct, msg, ml, app, sizeof app, &al);
        if (c == CT_DELIVERED) {
            uint32_t idx = 0;
            uint8_t h[32];
            int p = pss_request_parse(app, al, &idx);
            if (p != PSS_OK) {
                ev(o, PSS_EV_BAD_REQ, p, 0, (uint32_t)al, NULL);
            } else {
                sha256_hash(app, al, h);
                ev(o, PSS_EV_REQUEST, 0, idx, (uint32_t)al, h);
                if (r->scenario == PSS_SILENT && r->requests >= 1) {
                    /* from now on nothing is answered, not even an ACK */
                    r->silent = 1; /* GUARD:pss-silent */
                    ev(o, PSS_EV_SILENT_DROP, 1, idx, (uint32_t)al, NULL);
                    return PSS_OK;
                }
                r->requests++;
                pss_reply_build(idx, h, entropy + 64, (uint16_t)al, rep, sizeof rep);
                if (ct_send(&r->ct, rep, sizeof rep) == CT_OK) {
                    r->replies++;
                    int at = (r->scenario == PSS_REPLAY && idx == 1) ||
                             ((r->scenario == PSS_STALE || r->scenario == PSS_TRUNC) && idx == 0);
                    if (at) inject_for = (int)idx; /* GUARD:pss-inject */
                }
            }
        } else if (c < 0) {
            ev(o, PSS_EV_CTL, c, 0, (uint32_t)ml, NULL);
        }
    } else if (s == SEC_PEER_CLOSED) {
        ev(o, PSS_EV_CLOSED, 0, 0, 0, NULL);
        r->active = 0;
        return PSS_OK;
    } else if (s != SEC_OK && s != SEC_DATA) {
        ev(o, PSS_EV_SEC, s, 0, (uint32_t)n, NULL);
    }
    resp_flush(r, now, o, inject_for);
    return PSS_OK;
}

int pss_resp_poll(pss_resp *r, uint64_t now, pss_out *o)
{
    if (!r || !o)
        return PSS_E_ARG;
    o->n = o->nev = 0;
    if (!r->active || r->silent)
        return PSS_OK;
    resp_flush(r, now, o, -1);
    return PSS_OK;
}

/* ================= initiator ================= */

static struct {
    sec_endpoint sec;
    ct_endpoint ct;
    uint8_t dg[SEC_MAX_DATAGRAM];
    uint8_t msg[SEC_MAX_PAYLOAD];
    uint8_t app[CT_MAX_PAYLOAD];
    uint8_t req[CT_MAX_PAYLOAD];
    uint8_t frame[CT_MAX_FRAME];
} S;

struct rxc {
    pss_run *r;
    const pss_io *io;
    int handshake;
    uint32_t want;
    uint16_t want_len;
    int got;
};

static uint64_t t_us(const pss_io *io) { return io->now_us ? io->now_us(io->io) : io->now_ms(io->io) * 1000u; }

static int on_rx(void *vc, const uint8_t *d, size_t n)
{
    struct rxc *x = (struct rxc *)vc;
    pss_run *r = x->r;
    size_t ml = 0, al = 0;
    uint8_t was_conf = S.sec.confirmed, was_state = S.sec.state;
    sec_status s;
    r->datagrams_rx++;
    s = sec_receive(&S.sec, x->io->now_ms(x->io->io), d, n, S.msg, sizeof S.msg, &ml);
    if (s < 0) {
        if ((unsigned)(-s) < 16u)
            r->sec_refusals[-s]++;
        if (x->handshake && r->hs_status == SEC_OK)
            r->hs_status = s;
        /* a peer we were not told to accept, or one that cannot prove its key: refuse it, stop listening */
        return x->handshake && (s == SEC_ERR_IDENTITY || s == SEC_ERR_BAD_SIG || s == SEC_ERR_WEAK_KEY);
    }
    if (s == SEC_PEER_CLOSED)
        return 1;
    if (x->handshake)
        return (S.sec.state != was_state) || (S.sec.confirmed && !was_conf);
    if (s == SEC_DATA) {
        ct_status c = ct_receive(&S.ct, S.msg, ml, S.app, sizeof S.app, &al);
        if (c == CT_DELIVERED) {
            int k = pss_reply_check(S.app, al, x->want, r->req_hash[x->want], x->want_len);
            if (k == PSS_OK) {
                memcpy(r->spark_nonce[x->want], S.app + 40, 16);
                x->got = 1;
                return 1;
            }
            if (r->first_reply_err == PSS_OK)
                r->first_reply_err = k;
        } else if (c < 0 && (unsigned)(-c) < 12u) {
            r->ctl_refusals[-c]++;
        }
    }
    return 0;
}

static int send_or_wait(pss_run *r, const pss_io *io, uint16_t port, const uint8_t *d, size_t n, uint32_t wait_ms,
                        struct rxc *x)
{
    r->xchgs++;
    return io->xchg(io->io, port, d, n, wait_ms, on_rx, x);
}

int pss_initiator_run(pss_run *r, const pss_io *io)
{
    sec_config c;
    ct_identity id;
    uint8_t pk[32];
    struct rxc x;
    uint16_t port;
    uint64_t t0, th;
    uint32_t i;
    size_t w = 0, fw = 0;
    if (!r || !io || !io->xchg || !io->now_ms || !io->rand || r->scenario < 0 || r->scenario >= PSS_NSCN ||
        r->nmsg > PSS_MAX_MSGS || !r->pi_label || r->rto_ms == 0)
        return PSS_E_ARG;
    for (i = 0; i < r->nmsg; i++)
        if (r->msg_len[i] < PSS_REQ_MIN || r->msg_len[i] > CT_MAX_PAYLOAD)
            return PSS_E_ARG;
    port = (uint16_t)(PSS_PORT_BASE + (uint32_t)r->scenario);
    r->hs_status = SEC_OK;
    r->established = r->confirmed = r->link_down = r->closed_sent = 0;
    r->sent = r->delivered = r->verified = 0;
    r->first_reply_err = PSS_OK;
    memset(r->sec_refusals, 0, sizeof r->sec_refusals);
    memset(r->ctl_refusals, 0, sizeof r->ctl_refusals);
    memset(r->rtt_us, 0, sizeof r->rtt_us);
    r->hs_us = r->total_us = 0;
    r->xchgs = r->datagrams_rx = 0;
    r->result = PSS_OK;

    memset(&c, 0, sizeof c);
    c.role = 0;
    if (sec_test_identity(c.id_sk, pk, r->pi_label) != SEC_OK || sec_test_identity(pk, c.peer_pk, PSS_LABEL_SPARK) != SEC_OK)
        return PSS_E_ARG;
    memset(pk, 0, sizeof pk);
    /* no fresh randomness: fail closed */
    if (io->rand(io->io, c.entropy, sizeof c.entropy) != 0) { memset(&c, 0, sizeof c); return r->result = PSS_E_ARG; } /* GUARD:pss-rand */
    c.rto = r->rto_ms;
    c.max_retries = r->max_retries;
    c.rekey_interval = 1u << 20;
    c.max_records = 1u << 20;
    if (sec_init(&S.sec, &c) != SEC_OK) {
        memset(&c, 0, sizeof c);
        return r->result = PSS_E_ARG;
    }
    memset(&c, 0, sizeof c);

    /* ---- handshake: HS1 -> HS2 -> HS3 -> CONFIRM ---- */
    x.r = r; x.io = io; x.handshake = 1; x.want = 0; x.want_len = 0; x.got = 0;
    t0 = io->now_ms(io->io);
    th = t_us(io);
    while (!S.sec.confirmed) {
        sec_status s;
        if (io->now_ms(io->io) - t0 > r->budget_ms) {
            r->result = SEC_ERR_LINK_DOWN;
            break;
        }
        s = sec_poll_tx(&S.sec, io->now_ms(io->io), S.dg, sizeof S.dg, &w);
        if (s == SEC_ERR_LINK_DOWN) {
            r->link_down = 1;
            r->result = s;
            break;
        }
        if (s == SEC_OK && w) {
            if (send_or_wait(r, io, port, S.dg, w, r->rto_ms, &x) < 0) {
                r->result = SEC_ERR_LINK_DOWN;
                break;
            }
        } else if (s == SEC_IDLE) {
            (void)send_or_wait(r, io, port, NULL, 0, 5, &x);
        } else {
            r->result = s;
            break;
        }
        /* refused: this is not the peer we were told to accept */
        if (r->hs_status == SEC_ERR_IDENTITY || r->hs_status == SEC_ERR_BAD_SIG || r->hs_status == SEC_ERR_WEAK_KEY) break; /* GUARD:pss-hs-fatal */
    }
    if (!S.sec.confirmed && r->result == PSS_OK)
        r->result = r->hs_status != SEC_OK ? r->hs_status : SEC_ERR_LINK_DOWN;
    r->established = S.sec.state == SEC_ST_ESTABLISHED;
    r->confirmed = S.sec.confirmed && r->established;
    r->hs_us = t_us(io) - th;
    if (!r->confirmed) goto done; /* GUARD:pss-confirmed-first */
    memcpy(r->session_id, S.sec.session_id, 8);
    if (ct_test_identity(&id, PSS_LABEL_CTL) != CT_OK ||
        ct_init(&S.ct, &id, 0, rd32(S.sec.session_id), r->rto_ms, r->max_retries) != CT_OK) {
        r->result = PSS_E_ARG;
        goto done;
    }
    memset(&id, 0, sizeof id);

    /* ---- requests: one at a time, each answered before the next ---- */
    x.handshake = 0;
    for (i = 0; i < r->nmsg && !r->link_down; i++) {
        uint8_t nonce[32];
        uint64_t ts;
        size_t len;
        if (io->rand(io->io, nonce, sizeof nonce) != 0) {
            r->result = PSS_E_ARG;
            break;
        }
        memcpy(r->req_nonce[i], nonce, 32);
        len = pss_request_build(i, nonce, r->msg_len[i], S.req, sizeof S.req);
        sha256_hash(S.req, len, r->req_hash[i]);
        if (ct_send(&S.ct, S.req, len) != CT_OK) {
            r->result = PSS_E_ARG;
            break;
        }
        r->sent++;
        x.want = i; x.want_len = (uint16_t)len; x.got = 0;
        ts = t_us(io);
        while (!x.got) {
            ct_status cs;
            if (io->now_ms(io->io) - t0 > r->budget_ms) {
                r->result = CT_ERR_LINK_DOWN;
                r->link_down = 1;
                break;
            }
            cs = ct_poll_tx(&S.ct, io->now_ms(io->io), S.frame, sizeof S.frame, &fw);
            if (cs == CT_ERR_LINK_DOWN) {
                r->link_down = 1;
                r->result = cs;
                break;
            }
            if (cs == CT_OK && fw) {
                if (sec_seal(&S.sec, S.frame, fw, S.dg, sizeof S.dg, &w) != SEC_OK) {
                    r->result = PSS_E_ARG;
                    r->link_down = 1;
                    break;
                }
                if (send_or_wait(r, io, port, S.dg, w, r->rto_ms, &x) < 0) {
                    r->result = CT_ERR_LINK_DOWN;
                    r->link_down = 1;
                    break;
                }
            } else {
                (void)send_or_wait(r, io, port, NULL, 0, 5, &x);
            }
        }
        if (x.got) {
            r->rtt_us[i] = t_us(io) - ts;
            r->delivered++;
            r->verified++;
        }
    }
    /* ---- the last ACK, then CLOSE ---- */
    if (!r->link_down) {
        if (ct_poll_tx(&S.ct, io->now_ms(io->io), S.frame, sizeof S.frame, &fw) == CT_OK && fw &&
            sec_seal(&S.sec, S.frame, fw, S.dg, sizeof S.dg, &w) == SEC_OK)
            (void)send_or_wait(r, io, port, S.dg, w, 0, &x);
        if (sec_close(&S.sec, io->now_ms(io->io), S.dg, sizeof S.dg, &w) == SEC_OK) {
            (void)send_or_wait(r, io, port, S.dg, w, 0, &x);
            r->closed_sent = 1;
        }
    }
done:
    r->sc = S.sec.c;
    r->cc = S.ct.c;
    r->total_us = t_us(io) - th;
    memset(&S.sec, 0, sizeof S.sec); /* no key material left behind */
    memset(&S.ct, 0, sizeof S.ct);
    return r->result;
}
