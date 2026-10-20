/* AIENOS Interplane join (see aienos_ipj.h for the wire spec, and
 * docs/INTERPLANE_JOIN.md for the design). No heap, clock or I/O.
 * Lines tagged GUARD:<name> are removed one at a time by `make mutants`;
 * tests/ipj_test.c must then fail. */
#include "aienos_ipj.h"

#include <string.h>

#include "../argus/sha256.h"
#include "../crypto/aienos_crypto.h"
#include "../sig/aienos_sig.h"

#define L_ROSTER "AIENOS-IPJ-v1 roster"
#define L_BEACON "AIENOS-IPJ-v1 beacon"
#define L_RV "AIENOS-IPJ-v1 rendezvous"
#define L_MSG "AIENOS-IPJ-v1 msg"
#define L_ID "AIENOS-IPJ-v1 machine id"
#define LABEL_MAX 32u

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}
static int all_zero(const uint8_t *p, size_t n)
{
    uint8_t a = 0;
    for (size_t i = 0; i < n; i++) a |= p[i];
    return a == 0;
}

/* Signature over label | 0 | bytes. The largest signed body is a roster. */
static uint8_t sigbuf[LABEL_MAX + IPJ_ROSTER_MAX_BYTES];
static size_t domain(const char *label, const uint8_t *b, size_t n)
{
    size_t l = strlen(label) + 1;
    memcpy(sigbuf, label, l);
    memcpy(sigbuf + l, b, n);
    return l + n;
}
static void sign(uint8_t sig[64], const char *label, const uint8_t *b, size_t n, const uint8_t sk[32])
{
    size_t m = domain(label, b, n);
    aienos_ed25519_sign(sig, sigbuf, m, sk);
    aienos_wipe(sigbuf, m);
}
static int verify(const uint8_t sig[64], const char *label, const uint8_t *b, size_t n, const uint8_t pk[32])
{
    size_t m = domain(label, b, n);
    int ok = aienos_ed25519_verify(sig, sigbuf, m, pk) == AIENOS_SIG_OK;
    aienos_wipe(sigbuf, m);
    return ok;
}

void ipj_machine_id(uint8_t id[16], const uint8_t pk[32])
{
    uint8_t h[32];
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)L_ID, sizeof L_ID);
    sha256_update(&c, pk, 32);
    sha256_final(&c, h);
    memcpy(id, h, 16);
}

const char *ipj_status_name(ipj_status s)
{
    switch (s) {
    case IPJ_OK: return "OK";
    case IPJ_ERR_ARG: return "ARG";
    case IPJ_ERR_MALFORMED: return "MALFORMED";
    case IPJ_ERR_BAD_SIG: return "BAD_SIG";
    case IPJ_ERR_OWNER: return "OWNER";
    case IPJ_ERR_ROLLBACK: return "ROLLBACK";
    case IPJ_ERR_TEST_KEY: return "TEST_KEY";
    case IPJ_ERR_NOT_MEMBER: return "NOT_MEMBER";
    case IPJ_ERR_REPLAY: return "REPLAY";
    case IPJ_ERR_FULL: return "FULL";
    case IPJ_ERR_NOT_FOUND: return "NOT_FOUND";
    case IPJ_ERR_NO_ROUTE: return "NO_ROUTE";
    case IPJ_ERR_TTL: return "TTL";
    case IPJ_ERR_SEC: return "SEC";
    case IPJ_ERR_STORE: return "STORE";
    }
    return "?";
}

const char *ipj_platform_name(uint8_t p)
{
    static const char *const n[] = {"?", "spark", "imac", "pi", "run-windows", "run-macos", "run-linux", "hub"};
    return p <= IPJ_PLAT_MAX ? n[p] : "?";
}

/* ---- roster ---- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int name_ok(const uint8_t *b)
{
    if (b[0] == 0) return 0;
    int end = 0;
    for (unsigned i = 0; i < IPJ_NAME_LEN; i++) {
        if (end) { if (b[i] != 0) return 0; continue; }
        if (b[i] == 0) { end = 1; continue; }
        if (b[i] < 0x21 || b[i] > 0x7e) return 0;
    }
    return 1;
}

static int set_name(ipj_entry *e, const char *name)
{
    size_t l = name ? strlen(name) : 0;
    uint8_t b[IPJ_NAME_LEN];
    if (l == 0 || l > IPJ_NAME_LEN) return 0;
    memset(b, 0, sizeof b);
    memcpy(b, name, l);
    if (!name_ok(b)) return 0;
    memset(e->name, 0, sizeof e->name);
    memcpy(e->name, name, l);
    return 1;
}

static int find_idx(const ipj_roster *r, const uint8_t pk[32])
{
    for (unsigned i = 0; i < r->count; i++)
        if (memcmp(r->e[i].pk, pk, 32) == 0) return (int)i;
    return -1;
}

const ipj_entry *ipj_roster_find(const ipj_roster *r, const uint8_t pk[32])
{
    if (!r || !pk) return 0;
    int i = find_idx(r, pk);
    return i < 0 ? 0 : &r->e[i];
}

int ipj_roster_member(const ipj_roster *r, const uint8_t pk[32], unsigned mode)
{
    const ipj_entry *e = ipj_roster_find(r, pk);
    if (!e || r->generation == 0) return 0;
    if (e->status != IPJ_ST_ACTIVE) return 0; /* GUARD:member-revoked */
    if (mode == IPJ_MODE_PRODUCTION && e->kind != IPJ_KIND_PROD) return 0; /* GUARD:member-test */
    return 1;
}

void ipj_roster_init(ipj_roster *r, const uint8_t owner_pk[32])
{
    memset(r, 0, sizeof *r);
    memcpy(r->owner_pk, owner_pk, 32);
}

static int active_name_taken(const ipj_roster *r, const char *name)
{
    for (unsigned i = 0; i < r->count; i++)
        if (r->e[i].status == IPJ_ST_ACTIVE && str_eq(r->e[i].name, name)) return 1;
    return 0;
}

ipj_status ipj_roster_add(ipj_roster *r, const uint8_t pk[32], const char *name, uint8_t kind, uint8_t platform)
{
    if (!r || !pk || (kind != IPJ_KIND_PROD && kind != IPJ_KIND_TEST) || platform == 0 || platform > IPJ_PLAT_MAX)
        return IPJ_ERR_ARG;
    if (r->count >= IPJ_ROSTER_MAX) return IPJ_ERR_FULL;
    /* A key once listed (even revoked) never comes back. */
    if (find_idx(r, pk) >= 0 || active_name_taken(r, name ? name : "")) return IPJ_ERR_ARG;
    ipj_entry *e = &r->e[r->count];
    memset(e, 0, sizeof *e);
    if (!set_name(e, name)) return IPJ_ERR_ARG;
    memcpy(e->pk, pk, 32);
    e->kind = kind;
    e->platform = platform;
    e->status = IPJ_ST_ACTIVE;
    r->count++;
    e->changed_gen = ++r->generation;
    return IPJ_OK;
}

ipj_status ipj_roster_revoke(ipj_roster *r, const uint8_t pk[32])
{
    if (!r || !pk) return IPJ_ERR_ARG;
    int i = find_idx(r, pk);
    if (i < 0) return IPJ_ERR_NOT_FOUND;
    if (r->e[i].status != IPJ_ST_ACTIVE) return IPJ_ERR_ARG;
    r->e[i].status = IPJ_ST_REVOKED;
    r->e[i].changed_gen = ++r->generation;
    return IPJ_OK;
}

ipj_status ipj_roster_rotate(ipj_roster *r, const uint8_t old_pk[32], const uint8_t new_pk[32])
{
    if (!r || !old_pk || !new_pk) return IPJ_ERR_ARG;
    int i = find_idx(r, old_pk);
    if (i < 0) return IPJ_ERR_NOT_FOUND;
    if (r->e[i].status != IPJ_ST_ACTIVE || find_idx(r, new_pk) >= 0) return IPJ_ERR_ARG;
    if (r->count >= IPJ_ROSTER_MAX) return IPJ_ERR_FULL;
    uint64_t g = r->generation + 1;
    ipj_entry *n = &r->e[r->count];
    *n = r->e[i];
    memcpy(n->pk, new_pk, 32);
    n->changed_gen = g;
    r->e[i].status = IPJ_ST_REVOKED;
    r->e[i].changed_gen = g;
    r->count++;
    r->generation = g;
    return IPJ_OK;
}

ipj_status ipj_roster_sign(const ipj_roster *r, const uint8_t owner_sk[32], uint8_t *out, size_t cap,
                           size_t *written)
{
    if (!r || !owner_sk || !out || !written || r->count == 0 || r->count > IPJ_ROSTER_MAX || r->generation == 0)
        return IPJ_ERR_ARG;
    size_t total = IPJ_ROSTER_BYTES(r->count), body = total - IPJ_SIG_LEN;
    if (cap < total) return IPJ_ERR_ARG;
    uint8_t pk[32];
    if (aienos_ed25519_public_key(pk, owner_sk) != AIENOS_SIG_OK || memcmp(pk, r->owner_pk, 32) != 0)
        return IPJ_ERR_OWNER;
    memset(out, 0, total);
    memcpy(out, "AIPR", 4);
    out[4] = IPJ_VERSION;
    put16(out + 6, r->count);
    memcpy(out + 8, r->owner_pk, 32);
    put64(out + 40, r->generation);
    for (unsigned i = 0; i < r->count; i++) {
        uint8_t *p = out + IPJ_ROSTER_HDR + IPJ_ROSTER_ENTRY * i;
        const ipj_entry *e = &r->e[i];
        memcpy(p, e->pk, 32);
        memcpy(p + 32, e->name, strlen(e->name));
        p[48] = e->kind;
        p[49] = e->platform;
        p[50] = e->status;
        put64(p + 56, e->changed_gen);
    }
    sign(out + body, L_ROSTER, out, body, owner_sk);
    *written = total;
    return IPJ_OK;
}

ipj_status ipj_roster_accept(const ipj_roster *cur, const uint8_t *b, size_t len, const uint8_t owner_pk[32],
                             unsigned mode, int owner_is_test, ipj_roster *out)
{
    static ipj_roster t;
    if (!b || !owner_pk || !out || mode > IPJ_MODE_TEST) return IPJ_ERR_ARG;
    if (len < IPJ_ROSTER_BYTES(1) || len > IPJ_ROSTER_MAX_BYTES) return IPJ_ERR_MALFORMED; /* bounds before any read (unobservable without a sanitizer; ASan run) */
    unsigned count = get16(b + 6);
    if (memcmp(b, "AIPR", 4) != 0 || b[4] != IPJ_VERSION || b[5] != 0 || count == 0 || count > IPJ_ROSTER_MAX ||
        len != IPJ_ROSTER_BYTES(count) || !all_zero(b + 48, 16))
        return IPJ_ERR_MALFORMED;
    if (memcmp(b + 8, owner_pk, 32) != 0) return IPJ_ERR_OWNER; /* GUARD:roster-owner */
    size_t body = len - IPJ_SIG_LEN;
    if (!verify(b + body, L_ROSTER, b, body, owner_pk)) return IPJ_ERR_BAD_SIG; /* GUARD:roster-sig */
    if (mode == IPJ_MODE_PRODUCTION && owner_is_test) return IPJ_ERR_TEST_KEY; /* GUARD:roster-test-owner */
    uint64_t gen = get64(b + 40);
    if (gen == 0) return IPJ_ERR_MALFORMED;
    if (cur && cur->generation && gen <= cur->generation) return IPJ_ERR_ROLLBACK; /* GUARD:roster-rollback */
    memset(&t, 0, sizeof t);
    memcpy(t.owner_pk, owner_pk, 32);
    t.generation = gen;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *p = b + IPJ_ROSTER_HDR + IPJ_ROSTER_ENTRY * i;
        ipj_entry *e = &t.e[i];
        uint64_t cg = get64(p + 56);
        if (!name_ok(p + 32) || (p[48] != IPJ_KIND_PROD && p[48] != IPJ_KIND_TEST) || p[49] == 0 ||
            p[49] > IPJ_PLAT_MAX || (p[50] != IPJ_ST_ACTIVE && p[50] != IPJ_ST_REVOKED) || !all_zero(p + 51, 5) ||
            cg == 0 || cg > gen)
            return IPJ_ERR_MALFORMED;
        if (mode == IPJ_MODE_PRODUCTION && p[48] == IPJ_KIND_TEST) return IPJ_ERR_TEST_KEY; /* GUARD:roster-test-entry */
        memcpy(e->pk, p, 32);
        memcpy(e->name, p + 32, IPJ_NAME_LEN);
        e->name[IPJ_NAME_LEN] = 0;
        e->kind = p[48];
        e->platform = p[49];
        e->status = p[50];
        e->changed_gen = cg;
        t.count = (uint16_t)(i + 1);
        for (unsigned j = 0; j < i; j++) {
            if (memcmp(t.e[j].pk, e->pk, 32) == 0) return IPJ_ERR_MALFORMED; /* GUARD:roster-dup-key */
            if (e->status == IPJ_ST_ACTIVE && t.e[j].status == IPJ_ST_ACTIVE && str_eq(t.e[j].name, e->name))
                return IPJ_ERR_MALFORMED;
        }
    }
    *out = t;
    return IPJ_OK;
}

/* ---- beacon ---- */

ipj_status ipj_beacon_build(const uint8_t sk[32], uint64_t roster_gen, uint64_t counter, const uint8_t ip[4],
                            uint16_t port, uint8_t out[IPJ_BEACON_LEN])
{
    if (!sk || !ip || !out) return IPJ_ERR_ARG;
    memset(out, 0, IPJ_BEACON_LEN);
    memcpy(out, "AIPB", 4);
    out[4] = IPJ_VERSION;
    if (aienos_ed25519_public_key(out + 8, sk) != AIENOS_SIG_OK) return IPJ_ERR_ARG;
    put64(out + 40, roster_gen);
    put64(out + 48, counter);
    memcpy(out + 56, ip, 4);
    put16(out + 60, port);
    sign(out + 64, L_BEACON, out, 64, sk);
    return IPJ_OK;
}

ipj_status ipj_beacon_parse(const ipj_roster *r, unsigned mode, const uint8_t *in, size_t len, ipj_beacon *b)
{
    if (!r || !in || !b) return IPJ_ERR_ARG;
    if (len != IPJ_BEACON_LEN || memcmp(in, "AIPB", 4) != 0 || in[4] != IPJ_VERSION || !all_zero(in + 5, 3) ||
        !all_zero(in + 62, 2))
        return IPJ_ERR_MALFORMED; /* GUARD:beacon-layout */
    if (!ipj_roster_member(r, in + 8, mode)) return IPJ_ERR_NOT_MEMBER; /* GUARD:beacon-member */
    if (!verify(in + 64, L_BEACON, in, 64, in + 8)) return IPJ_ERR_BAD_SIG; /* GUARD:beacon-sig */
    memcpy(b->pk, in + 8, 32);
    b->roster_gen = get64(in + 40);
    b->counter = get64(in + 48);
    memcpy(b->ip, in + 56, 4);
    b->port = get16(in + 60);
    return IPJ_OK;
}

/* ---- rendezvous ---- */

ipj_status ipj_rv_build(const uint8_t sk[32], const ipj_rv *v, uint8_t out[IPJ_RV_LEN])
{
    if (!sk || !v || !out || v->op < IPJ_RV_REGISTER || v->op > IPJ_RV_ANSWER) return IPJ_ERR_ARG;
    memset(out, 0, IPJ_RV_LEN);
    memcpy(out, "AIPV", 4);
    out[4] = IPJ_VERSION;
    out[5] = v->op;
    if (aienos_ed25519_public_key(out + 8, sk) != AIENOS_SIG_OK) return IPJ_ERR_ARG;
    memcpy(out + 40, v->subject, 32);
    put64(out + 72, v->counter);
    put64(out + 80, v->roster_gen);
    memcpy(out + 88, v->ip, 4);
    put16(out + 92, v->port);
    sign(out + 96, L_RV, out, 96, sk);
    return IPJ_OK;
}

ipj_status ipj_rv_parse(const ipj_roster *r, unsigned mode, const uint8_t *in, size_t len, ipj_rv *v)
{
    if (!r || !in || !v) return IPJ_ERR_ARG;
    if (len != IPJ_RV_LEN || memcmp(in, "AIPV", 4) != 0 || in[4] != IPJ_VERSION || in[5] < IPJ_RV_REGISTER ||
        in[5] > IPJ_RV_ANSWER || !all_zero(in + 6, 2) || !all_zero(in + 94, 2))
        return IPJ_ERR_MALFORMED;
    if (in[5] == IPJ_RV_REGISTER && memcmp(in + 8, in + 40, 32) != 0) return IPJ_ERR_MALFORMED;
    if (!ipj_roster_member(r, in + 8, mode)) return IPJ_ERR_NOT_MEMBER; /* GUARD:rv-member */
    if (!verify(in + 96, L_RV, in, 96, in + 8)) return IPJ_ERR_BAD_SIG; /* GUARD:rv-sig */
    v->op = in[5];
    memcpy(v->signer, in + 8, 32);
    memcpy(v->subject, in + 40, 32);
    v->counter = get64(in + 72);
    v->roster_gen = get64(in + 80);
    memcpy(v->ip, in + 88, 4);
    v->port = get16(in + 92);
    return IPJ_OK;
}

ipj_status ipj_gateway_handle(ipj_gateway *g, const ipj_roster *r, unsigned mode, const uint8_t gw_sk[32],
                              const uint8_t *in, size_t len, const uint8_t src_ip[4], uint16_t src_port,
                              uint8_t *out, size_t cap, size_t *out_len)
{
    if (!g || !r || !gw_sk || !in || !src_ip || !out || !out_len) return IPJ_ERR_ARG;
    (void)src_port;
    *out_len = 0;
    ipj_rv v;
    ipj_status s = ipj_rv_parse(r, mode, in, len, &v);
    if (s != IPJ_OK) return s;
    if (v.op == IPJ_RV_REGISTER) {
        int slot = -1;
        for (unsigned i = 0; i < IPJ_GW_MAX; i++)
            if (g->t[i].used && memcmp(g->t[i].pk, v.signer, 32) == 0) slot = (int)i;
        if (slot >= 0 && v.counter <= g->t[slot].counter) return IPJ_ERR_REPLAY; /* GUARD:gw-replay */
        for (unsigned i = 0; slot < 0 && i < IPJ_GW_MAX; i++)
            if (!g->t[i].used) slot = (int)i;
        if (slot < 0) return IPJ_ERR_FULL;
        g->t[slot].used = 1;
        memcpy(g->t[slot].pk, v.signer, 32);
        /* 0.0.0.0: the peer cannot see its own outside address (host NAT); use
         * the address the datagram came from, with the advertised port. */
        memcpy(g->t[slot].ip, all_zero(v.ip, 4) ? src_ip : v.ip, 4);
        g->t[slot].port = v.port;
        g->t[slot].counter = v.counter;
        return IPJ_OK;
    }
    if (v.op != IPJ_RV_LOOKUP) return IPJ_ERR_MALFORMED;
    if (cap < IPJ_RV_LEN) return IPJ_ERR_ARG;
    if (!ipj_roster_member(r, v.subject, mode)) return IPJ_ERR_NOT_MEMBER; /* GUARD:gw-subject */
    for (unsigned i = 0; i < IPJ_GW_MAX; i++) {
        if (!g->t[i].used || memcmp(g->t[i].pk, v.subject, 32) != 0) continue;
        ipj_rv a;
        memset(&a, 0, sizeof a);
        a.op = IPJ_RV_ANSWER;
        memcpy(a.subject, v.subject, 32);
        a.counter = v.counter;
        a.roster_gen = r->generation;
        memcpy(a.ip, g->t[i].ip, 4);
        a.port = g->t[i].port;
        s = ipj_rv_build(gw_sk, &a, out);
        if (s == IPJ_OK) *out_len = IPJ_RV_LEN;
        return s;
    }
    return IPJ_ERR_NOT_FOUND;
}

/* ---- message ---- */

static uint8_t msgbuf[SEC_MAX_PAYLOAD];

ipj_status ipj_msg_build(const uint8_t sk[32], const ipj_msg *m, uint8_t *out, size_t cap, size_t *written)
{
    if (!sk || !m || !out || !written || m->len > IPJ_MSG_MAX || m->kind < IPJ_MSG_TEXT ||
        m->kind > IPJ_MSG_CONSOLE)
        return IPJ_ERR_ARG;
    size_t body = IPJ_MSG_HDR + m->len, total = body + IPJ_SIG_LEN;
    if (cap < total) return IPJ_ERR_ARG;
    memset(out, 0, IPJ_MSG_HDR);
    memcpy(out, "AIPM", 4);
    out[4] = IPJ_VERSION;
    out[5] = m->kind;
    memcpy(out + 8, m->origin, 32);
    uint8_t pk[32];
    if (aienos_ed25519_public_key(pk, sk) != AIENOS_SIG_OK || memcmp(pk, m->origin, 32) != 0) return IPJ_ERR_ARG;
    memcpy(out + 40, m->dest, 32);
    put64(out + 72, m->seq);
    put16(out + 80, m->len);
    memcpy(out + IPJ_MSG_HDR, m->payload, m->len);
    sign(out + body, L_MSG, out, body, sk); /* ttl byte is 0 while signing */
    out[6] = m->ttl;
    *written = total;
    return IPJ_OK;
}

ipj_status ipj_msg_parse(const ipj_roster *r, unsigned mode, const uint8_t *in, size_t len, ipj_msg *m)
{
    if (!r || !in || !m) return IPJ_ERR_ARG;
    if (len < IPJ_MSG_HDR + IPJ_SIG_LEN || len > sizeof msgbuf) return IPJ_ERR_MALFORMED; /* bounds before any read (unobservable without a sanitizer; ASan run) */
    size_t pl = get16(in + 80);
    if (memcmp(in, "AIPM", 4) != 0 || in[4] != IPJ_VERSION || in[5] < IPJ_MSG_TEXT || in[5] > IPJ_MSG_CONSOLE ||
        in[7] != 0 || !all_zero(in + 82, 2) || pl > IPJ_MSG_MAX || len != IPJ_MSG_HDR + pl + IPJ_SIG_LEN)
        return IPJ_ERR_MALFORMED; /* GUARD:msg-layout */
    if (!ipj_roster_member(r, in + 8, mode)) return IPJ_ERR_NOT_MEMBER; /* GUARD:msg-member */
    size_t body = IPJ_MSG_HDR + pl;
    memcpy(msgbuf, in, body);
    msgbuf[6] = 0;
    if (!verify(in + body, L_MSG, msgbuf, body, in + 8)) return IPJ_ERR_BAD_SIG; /* GUARD:msg-sig */
    m->kind = in[5];
    m->ttl = in[6];
    memcpy(m->origin, in + 8, 32);
    memcpy(m->dest, in + 40, 32);
    m->seq = get64(in + 72);
    m->len = (uint16_t)pl;
    memcpy(m->payload, in + IPJ_MSG_HDR, pl);
    return IPJ_OK;
}

/* ---- node ---- */

static void nlog(ipj_node *n, const char *a, const char *b, const char *c)
{
    if (!n->io.log) return;
    char line[160];
    size_t k = 0;
    const char *parts[3] = {a, b, c};
    for (int i = 0; i < 3; i++)
        for (const char *p = parts[i]; p && *p && k + 1 < sizeof line; p++) line[k++] = *p;
    line[k] = 0;
    n->io.log(n->io.ctx, line);
}

static const char *name_of(const ipj_node *n, const uint8_t pk[32])
{
    const ipj_entry *e = ipj_roster_find(&n->roster, pk);
    return e ? e->name : "unknown";
}

/* A session is live once both sides hold keys: the responder after HS3, the
 * initiator only after the responder's first authenticated record (an
 * initiator whose HS3 was refused by the roster never gets there). */
static int live(const ipj_peer *p) { return p->used && p->ep.state == SEC_ST_ESTABLISHED && p->ep.confirmed; }

static ipj_peer *peer_by_pk(ipj_node *n, const uint8_t pk[32])
{
    for (unsigned i = 0; i < IPJ_PEERS_MAX; i++)
        if (n->peer[i].used && n->peer[i].up && memcmp(n->peer[i].pk, pk, 32) == 0) return &n->peer[i];
    for (unsigned i = 0; i < IPJ_PEERS_MAX; i++)
        if (n->peer[i].used && n->peer[i].ep.role == 0 && memcmp(n->peer[i].pk, pk, 32) == 0) return &n->peer[i];
    return 0;
}

static ipj_peer *peer_by_addr(ipj_node *n, const uint8_t ip[4], uint16_t port)
{
    for (unsigned i = 0; i < IPJ_PEERS_MAX; i++)
        if (n->peer[i].used && n->peer[i].port == port && memcmp(n->peer[i].ip, ip, 4) == 0) return &n->peer[i];
    return 0;
}

static ipj_peer *peer_alloc(ipj_node *n)
{
    for (unsigned i = 0; i < IPJ_PEERS_MAX; i++)
        if (!n->peer[i].used) {
            memset(&n->peer[i], 0, sizeof n->peer[i]);
            return &n->peer[i];
        }
    return 0;
}

static void peer_free(ipj_peer *p)
{
    aienos_wipe(p, sizeof *p);
}

static int roster_ok(void *ctx, const uint8_t pk[32])
{
    ipj_node *n = ctx;
    if (memcmp(pk, n->pk, 32) == 0) return 0;
    return ipj_roster_member(&n->roster, pk, n->mode);
}

static void *seen_slot(ipj_node *n, const uint8_t pk[32])
{
    for (unsigned i = 0; i < IPJ_SEEN_MAX; i++)
        if (n->seen[i].used && memcmp(n->seen[i].pk, pk, 32) == 0) return &n->seen[i];
    for (unsigned i = 0; i < IPJ_SEEN_MAX; i++)
        if (!n->seen[i].used) {
            n->seen[i].used = 1;
            memcpy(n->seen[i].pk, pk, 32);
            n->seen[i].beacon_ctr = n->seen[i].msg_seq = 0;
            return &n->seen[i];
        }
    return 0;
}

ipj_status ipj_node_init(ipj_node *n, const uint8_t sk[32], const uint8_t owner_pk[32], unsigned mode,
                         int owner_is_test, const uint8_t *blob, size_t len, const uint8_t ip[4], uint16_t port,
                         const ipj_io *io, const sec_config *base)
{
    if (!n || !sk || !owner_pk || !blob || !ip || !io || !io->send || !io->entropy || !base) return IPJ_ERR_ARG;
    memset(n, 0, sizeof *n);
    memcpy(n->sk, sk, 32);
    if (aienos_ed25519_public_key(n->pk, sk) != AIENOS_SIG_OK) return IPJ_ERR_ARG;
    memcpy(n->owner_pk, owner_pk, 32);
    n->mode = mode;
    n->owner_is_test = owner_is_test;
    ipj_status s = ipj_roster_accept(0, blob, len, owner_pk, mode, owner_is_test, &n->roster);
    if (s != IPJ_OK) { aienos_wipe(n, sizeof *n); return s; }
    if (!ipj_roster_member(&n->roster, n->pk, mode)) { aienos_wipe(n, sizeof *n); return IPJ_ERR_NOT_MEMBER; }
    memcpy(n->roster_blob, blob, len);
    n->roster_len = len;
    memcpy(n->ip, ip, 4);
    n->port = port;
    n->io = *io;
    n->base = *base;
    return IPJ_OK;
}

ipj_status ipj_node_route(ipj_node *n, const uint8_t dest[32], const uint8_t via[32])
{
    if (!n || !dest || !via) return IPJ_ERR_ARG;
    for (unsigned i = 0; i < IPJ_ROUTES_MAX; i++)
        if (!n->route[i].used || memcmp(n->route[i].dest, dest, 32) == 0) {
            n->route[i].used = 1;
            memcpy(n->route[i].dest, dest, 32);
            memcpy(n->route[i].via, via, 32);
            return IPJ_OK;
        }
    return IPJ_ERR_FULL;
}

int ipj_node_established(const ipj_node *n, const uint8_t pk[32])
{
    for (unsigned i = 0; i < IPJ_PEERS_MAX; i++)
        if (live(&n->peer[i]) && memcmp(n->peer[i].pk, pk, 32) == 0) return 1; /* GUARD:live-confirmed */
    return 0;
}

static sec_status start_endpoint(ipj_node *n, ipj_peer *p, uint8_t role, const uint8_t *peer_pk)
{
    sec_config c = n->base;
    c.role = role;
    memcpy(c.id_sk, n->sk, 32);
    memset(c.peer_pk, 0, 32);
    if (peer_pk) memcpy(c.peer_pk, peer_pk, 32);
    n->io.entropy(n->io.ctx, c.entropy);
    c.peer_ok = role == 1 ? roster_ok : 0;
    c.peer_ctx = n;
    sec_status s = sec_init(&p->ep, &c);
    aienos_wipe(&c, sizeof c);
    return s;
}

ipj_status ipj_node_connect(ipj_node *n, const uint8_t pk[32], const uint8_t ip[4], uint16_t port)
{
    if (!n || !pk || !ip) return IPJ_ERR_ARG;
    if (memcmp(pk, n->pk, 32) == 0) return IPJ_ERR_ARG;
    if (!ipj_roster_member(&n->roster, pk, n->mode)) return IPJ_ERR_NOT_MEMBER; /* GUARD:connect-member */
    ipj_peer *p = peer_by_pk(n, pk);
    if (p) {
        if (p->ep.state != SEC_ST_ESTABLISHED) { memcpy(p->ip, ip, 4); p->port = port; }
        return IPJ_OK;
    }
    if (memcmp(n->pk, pk, 32) > 0) return IPJ_OK; /* the lower key dials; we wait */
    p = peer_alloc(n);
    if (!p) return IPJ_ERR_FULL;
    p->used = 1;
    memcpy(p->pk, pk, 32);
    memcpy(p->ip, ip, 4);
    p->port = port;
    if (start_endpoint(n, p, 0, pk) != SEC_OK) { peer_free(p); return IPJ_ERR_SEC; }
    nlog(n, "interplane: dialing ", name_of(n, pk), "");
    return IPJ_OK;
}

ipj_status ipj_node_beacon(ipj_node *n, const uint8_t bcast_ip[4])
{
    if (!n || !bcast_ip) return IPJ_ERR_ARG;
    uint8_t b[IPJ_BEACON_LEN];
    ipj_status s = ipj_beacon_build(n->sk, n->roster.generation, ++n->beacon_ctr, n->ip, n->port, b);
    if (s == IPJ_OK) n->io.send(n->io.ctx, bcast_ip, n->port, b, sizeof b);
    return s;
}

ipj_status ipj_node_set_gateway(ipj_node *n, const uint8_t gw_pk[32], const uint8_t ip[4], uint16_t port)
{
    if (!n || !gw_pk || !ip) return IPJ_ERR_ARG;
    if (!ipj_roster_member(&n->roster, gw_pk, n->mode)) return IPJ_ERR_NOT_MEMBER;
    memcpy(n->gw_pk, gw_pk, 32);
    memcpy(n->gw_ip, ip, 4);
    n->gw_port = port;
    n->gw_set = 1;
    return IPJ_OK;
}

static ipj_status rv_send(ipj_node *n, uint8_t op, const uint8_t subject[32], const uint8_t ip[4], uint16_t port)
{
    if (!n->gw_set) return IPJ_ERR_NO_ROUTE;
    ipj_rv v;
    memset(&v, 0, sizeof v);
    v.op = op;
    memcpy(v.subject, subject, 32);
    v.counter = ++n->rv_ctr;
    if (op == IPJ_RV_LOOKUP) n->lookup_ctr = v.counter;
    v.roster_gen = n->roster.generation;
    if (ip) memcpy(v.ip, ip, 4);
    v.port = port;
    uint8_t d[IPJ_RV_LEN];
    ipj_status s = ipj_rv_build(n->sk, &v, d);
    if (s == IPJ_OK) n->io.send(n->io.ctx, n->gw_ip, n->gw_port, d, sizeof d);
    return s;
}

ipj_status ipj_node_register(ipj_node *n, const uint8_t adv_ip[4], uint16_t adv_port)
{
    if (!n || !adv_ip) return IPJ_ERR_ARG;
    return rv_send(n, IPJ_RV_REGISTER, n->pk, adv_ip, adv_port);
}

ipj_status ipj_node_lookup(ipj_node *n, const uint8_t target[32])
{
    if (!n || !target) return IPJ_ERR_ARG;
    return rv_send(n, IPJ_RV_LOOKUP, target, 0, 0);
}

static ipj_status seal_to(ipj_node *n, ipj_peer *p, const uint8_t *frame, size_t len)
{
    static uint8_t d[SEC_MAX_DATAGRAM];
    size_t w = 0;
    if (sec_seal(&p->ep, frame, len, d, sizeof d, &w) != SEC_OK) return IPJ_ERR_SEC;
    n->io.send(n->io.ctx, p->ip, p->port, d, w);
    return IPJ_OK;
}

/* Send one app frame to peer pk now, or queue it until its session is up. */
static ipj_status frame_to(ipj_node *n, const uint8_t pk[32], const uint8_t *frame, size_t len)
{
    ipj_peer *p = peer_by_pk(n, pk);
    if (p && live(p)) return seal_to(n, p, frame, len);
    if (!p) return IPJ_ERR_NO_ROUTE;
    for (unsigned i = 0; i < IPJ_OUTQ; i++)
        if (!n->outq[i].used) {
            n->outq[i].used = 1;
            memcpy(n->outq[i].frame, frame, len);
            n->outq[i].len = len;
            memcpy(n->outq[i].to, pk, 32);
            return IPJ_OK;
        }
    return IPJ_ERR_FULL;
}

static const uint8_t *next_hop(ipj_node *n, const uint8_t dest[32])
{
    /* An owner-configured route wins over a direct session (for example: reach
     * the laptop through the Pi). */
    for (unsigned i = 0; i < IPJ_ROUTES_MAX; i++)
        if (n->route[i].used && memcmp(n->route[i].dest, dest, 32) == 0 && peer_by_pk(n, n->route[i].via))
            return n->route[i].via;
    if (peer_by_pk(n, dest)) return dest;
    return 0;
}

static uint8_t frame_buf[SEC_MAX_PAYLOAD];

ipj_status ipj_node_send(ipj_node *n, uint64_t now, const uint8_t dest[32], uint8_t kind, const uint8_t *payload,
                         size_t len)
{
    (void)now;
    static ipj_msg m;
    if (!n || !dest || (len && !payload) || len > IPJ_MSG_MAX) return IPJ_ERR_ARG;
    if (!ipj_roster_member(&n->roster, dest, n->mode)) return IPJ_ERR_NOT_MEMBER;
    const uint8_t *hop = next_hop(n, dest);
    if (!hop) return IPJ_ERR_NO_ROUTE;
    memset(&m, 0, sizeof m);
    m.kind = kind;
    m.ttl = IPJ_TTL_DEFAULT;
    memcpy(m.origin, n->pk, 32);
    memcpy(m.dest, dest, 32);
    m.seq = ++n->msg_seq;
    m.len = (uint16_t)len;
    if (len) memcpy(m.payload, payload, len);
    size_t w = 0;
    frame_buf[0] = IPJ_APP_MSG;
    ipj_status s = ipj_msg_build(n->sk, &m, frame_buf + 1, sizeof frame_buf - 1, &w);
    if (s != IPJ_OK) return s;
    return frame_to(n, hop, frame_buf, w + 1);
}

static void push_roster(ipj_node *n, ipj_peer *p)
{
    frame_buf[0] = IPJ_APP_ROSTER;
    memcpy(frame_buf + 1, n->roster_blob, n->roster_len);
    (void)seal_to(n, p, frame_buf, n->roster_len + 1);
}

static void drop_peer(ipj_node *n, ipj_peer *p, uint64_t now)
{
    static uint8_t d[SEC_MAX_DATAGRAM];
    size_t w = 0;
    if (p->ep.state == SEC_ST_ESTABLISHED && sec_close(&p->ep, now, d, sizeof d, &w) == SEC_OK)
        n->io.send(n->io.ctx, p->ip, p->port, d, w);
    peer_free(p);
}

ipj_status ipj_node_roster_update(ipj_node *n, uint64_t now, const uint8_t *blob, size_t len)
{
    static ipj_roster t;
    if (!n || !blob) return IPJ_ERR_ARG;
    ipj_status s = ipj_roster_accept(&n->roster, blob, len, n->owner_pk, n->mode, n->owner_is_test, &t);
    if (s != IPJ_OK) { if (s != IPJ_ERR_ROLLBACK) n->c.roster_refused++; return s; }
    n->roster = t;
    memcpy(n->roster_blob, blob, len);
    n->roster_len = len;
    n->roster_dirty = 1;
    n->c.roster_updates++;
    nlog(n, "interplane: roster updated", "", "");
    for (unsigned i = 0; i < IPJ_PEERS_MAX; i++) {
        ipj_peer *p = &n->peer[i];
        if (!p->used) continue;
        int known = p->up || p->ep.role == 0;
        int gone = known && !ipj_roster_member(&n->roster, p->pk, n->mode);
        if (gone) { nlog(n, "interplane: closed session with revoked ", name_of(n, p->pk), ""); n->c.revoked_closed++; drop_peer(n, p, now); continue; } /* GUARD:update-drop */
        if (live(p)) push_roster(n, p);
    }
    for (unsigned i = 0; i < IPJ_OUTQ; i++)
        if (n->outq[i].used && !ipj_roster_member(&n->roster, n->outq[i].to, n->mode)) n->outq[i].used = 0;
    return IPJ_OK;
}

static void on_app(ipj_node *n, uint64_t now, ipj_peer *p, const uint8_t *f, size_t len)
{
    static ipj_msg m;
    if (len < 1) return;
    if (f[0] == IPJ_APP_ROSTER) {
        (void)ipj_node_roster_update(n, now, f + 1, len - 1);
        return;
    }
    if (f[0] != IPJ_APP_MSG) { n->c.msg_refused++; return; }
    ipj_status s = ipj_msg_parse(&n->roster, n->mode, f + 1, len - 1, &m);
    if (s != IPJ_OK) {
        n->c.msg_refused++;
        nlog(n, "interplane: message refused (", ipj_status_name(s), ")");
        return;
    }
    if (memcmp(m.dest, n->pk, 32) == 0) {
        struct { uint8_t pk[32]; uint64_t beacon_ctr, msg_seq; int used; } *sv = seen_slot(n, m.origin);
        if (!sv || m.seq <= sv->msg_seq) { n->c.msg_refused++; nlog(n, "interplane: message refused (REPLAY)", "", ""); return; } /* GUARD:msg-replay */
        sv->msg_seq = m.seq;
        n->inbox = m;
        n->inbox_full = 1;
        n->c.delivered++;
        nlog(n, "interplane: message from ", name_of(n, m.origin), " verified (origin signature)");
        return;
    }
    if (m.ttl == 0) { n->c.msg_refused++; return; } /* GUARD:msg-ttl */
    const uint8_t *hop = next_hop(n, m.dest);
    if (!hop || memcmp(hop, p->pk, 32) == 0) { n->c.msg_refused++; return; }
    memcpy(frame_buf, f, len);
    frame_buf[1 + 6] = (uint8_t)(m.ttl - 1); /* the only byte a relay may change */
    if (frame_to(n, hop, frame_buf, len) == IPJ_OK) {
        n->c.relayed++;
        nlog(n, "interplane: relayed message from ", name_of(n, m.origin), "");
    }
}

static void session_up(ipj_node *n, ipj_peer *p)
{
    p->up = 1;
    /* One session per machine: a newer one replaces any other slot. */
    for (unsigned i = 0; i < IPJ_PEERS_MAX; i++) {
        ipj_peer *o = &n->peer[i];
        if (o != p && o->used && memcmp(o->pk, p->pk, 32) == 0) peer_free(o);
    }
    nlog(n, "interplane: session up with ", name_of(n, p->pk), "");
}

ipj_status ipj_node_input(ipj_node *n, uint64_t now, const uint8_t src_ip[4], uint16_t src_port, const uint8_t *d,
                          size_t len)
{
    static uint8_t msg[SEC_MAX_PAYLOAD];
    if (!n || !src_ip || !d) return IPJ_ERR_ARG;
    if (len >= 4 && memcmp(d, "AIPB", 4) == 0) {
        ipj_beacon b;
        ipj_status s = ipj_beacon_parse(&n->roster, n->mode, d, len, &b);
        if (s == IPJ_OK && memcmp(b.pk, n->pk, 32) == 0) return IPJ_OK; /* our own broadcast */
        struct { uint8_t pk[32]; uint64_t beacon_ctr, msg_seq; int used; } *sv = s == IPJ_OK ? seen_slot(n, b.pk) : 0;
        if (s == IPJ_OK && (!sv || b.counter <= sv->beacon_ctr)) s = IPJ_ERR_REPLAY; /* GUARD:beacon-replay */
        if (s != IPJ_OK) { n->c.beacon_refused++; return s; }
        sv->beacon_ctr = b.counter;
        return ipj_node_connect(n, b.pk, src_ip, b.port);
    }
    if (len >= 4 && memcmp(d, "AIPV", 4) == 0) {
        ipj_rv v;
        ipj_status s = ipj_rv_parse(&n->roster, n->mode, d, len, &v);
        if (s != IPJ_OK) return s;
        if (v.op != IPJ_RV_ANSWER || !n->gw_set || memcmp(v.signer, n->gw_pk, 32) != 0) return IPJ_ERR_NOT_MEMBER;
        if (v.counter != n->lookup_ctr || v.counter == 0) return IPJ_ERR_REPLAY; /* GUARD:rv-answer-ctr */
        n->lookup_ctr = 0;
        return ipj_node_connect(n, v.subject, v.ip, v.port);
    }
    ipj_peer *p = peer_by_addr(n, src_ip, src_port);
    int fresh = 0;
    if (!p) {
        if (len < 6 || memcmp(d, "ASC1", 4) != 0 || d[5] != SEC_TYPE_HS1) return IPJ_ERR_NOT_FOUND;
        p = peer_alloc(n);
        if (!p) return IPJ_ERR_FULL;
        p->used = 1;
        memcpy(p->ip, src_ip, 4);
        p->port = src_port;
        if (start_endpoint(n, p, 1, 0) != SEC_OK) { peer_free(p); return IPJ_ERR_SEC; }
        fresh = 1;
    }
    size_t ml = 0;
    int was = live(p);
    sec_status s = sec_receive(&p->ep, now, d, len, msg, sizeof msg, &ml);
    if (s == SEC_ERR_IDENTITY && p->ep.role == 1) {
        n->c.hs_refused++;
        nlog(n, "interplane: handshake refused (not an active roster member)", "", "");
        peer_free(p);
        return IPJ_ERR_NOT_MEMBER;
    }
    if (s < 0) {
        if (fresh) peer_free(p);
        return IPJ_ERR_SEC;
    }
    if (live(p) && !was) {
        if (p->ep.role == 1) memcpy(p->pk, p->ep.peer_pk, 32);
        session_up(n, p);
        push_roster(n, p);
    }
    if (s == SEC_PEER_CLOSED) { peer_free(p); return IPJ_OK; }
    if (s == SEC_DATA) {
        if (!ipj_roster_member(&n->roster, p->pk, n->mode)) { drop_peer(n, p, now); return IPJ_ERR_NOT_MEMBER; } /* defense in depth: an update already closed it */
        on_app(n, now, p, msg, ml);
    }
    return IPJ_OK;
}

void ipj_node_poll(ipj_node *n, uint64_t now)
{
    static uint8_t d[SEC_MAX_DATAGRAM];
    if (!n) return;
    for (unsigned i = 0; i < IPJ_PEERS_MAX; i++) {
        ipj_peer *p = &n->peer[i];
        if (!p->used) continue;
        for (int k = 0; k < 4; k++) {
            size_t w = 0;
            sec_status s = sec_poll_tx(&p->ep, now, d, sizeof d, &w);
            if (s == SEC_OK && w) { n->io.send(n->io.ctx, p->ip, p->port, d, w); continue; }
            if (s == SEC_ERR_LINK_DOWN) {
                nlog(n, "interplane: link down to ", name_of(n, p->pk), "");
                peer_free(p);
            }
            break;
        }
        if (!p->used || p->ep.state == SEC_ST_ESTABLISHED) continue;
    }
    for (unsigned i = 0; i < IPJ_OUTQ; i++) {
        if (!n->outq[i].used) continue;
        ipj_peer *p = peer_by_pk(n, n->outq[i].to);
        if (!p) { n->outq[i].used = 0; continue; }
        if (!live(p)) continue;
        (void)seal_to(n, p, n->outq[i].frame, n->outq[i].len);
        n->outq[i].used = 0;
    }
}

/* ---- provisioning bundle ---- */

ipj_status ipj_bundle_build(const ipj_bundle *b, const uint8_t *roster, size_t roster_len, uint8_t *out, size_t cap,
                            size_t *written)
{
    if (!b || !roster || !out || !written || roster_len == 0 || roster_len > IPJ_ROSTER_MAX_BYTES ||
        b->mode > IPJ_MODE_TEST || b->owner_is_test > 1 || b->nroutes > IPJ_BUNDLE_ROUTES ||
        b->send_len > IPJ_BUNDLE_TEXT || b->run_seconds == 0 || b->run_seconds > 3600 ||
        cap < IPJ_BUNDLE_HDR + roster_len)
        return IPJ_ERR_ARG;
    memset(out, 0, IPJ_BUNDLE_HDR);
    memcpy(out, "AIPJ", 4);
    out[4] = IPJ_VERSION;
    out[5] = b->mode;
    out[6] = b->owner_is_test;
    out[7] = b->nroutes;
    memcpy(out + 8, b->sk, 32);
    memcpy(out + 40, b->owner_pk, 32);
    memcpy(out + 72, b->ip, 4);
    memcpy(out + 76, b->bcast, 4);
    put16(out + 80, b->run_seconds);
    put16(out + 82, (uint16_t)roster_len);
    memcpy(out + 84, b->send_dest, 32);
    put16(out + 116, b->send_len);
    memcpy(out + 120, b->send_text, b->send_len);
    for (unsigned i = 0; i < b->nroutes; i++) {
        memcpy(out + 248 + 64 * i, b->route_dest[i], 32);
        memcpy(out + 248 + 64 * i + 32, b->route_via[i], 32);
    }
    memcpy(out + IPJ_BUNDLE_HDR, roster, roster_len);
    *written = IPJ_BUNDLE_HDR + roster_len;
    return IPJ_OK;
}

ipj_status ipj_bundle_parse(const uint8_t *in, size_t len, ipj_bundle *b)
{
    if (!in || !b) return IPJ_ERR_ARG;
    if (len < IPJ_BUNDLE_HDR + 1 || len > IPJ_BUNDLE_MAX || memcmp(in, "AIPJ", 4) != 0 || in[4] != IPJ_VERSION ||
        in[5] > IPJ_MODE_TEST || in[6] > 1 || in[7] > IPJ_BUNDLE_ROUTES)
        return IPJ_ERR_MALFORMED;
    uint16_t rs = get16(in + 80), rl = get16(in + 82), sl = get16(in + 116);
    if (rs == 0 || rs > 3600 || rl == 0 || len != IPJ_BUNDLE_HDR + (size_t)rl || sl > IPJ_BUNDLE_TEXT ||
        !all_zero(in + 118, 2) || !all_zero(in + 120 + sl, IPJ_BUNDLE_TEXT - sl) ||
        !all_zero(in + 248 + 64 * in[7], 64 * (IPJ_BUNDLE_ROUTES - in[7])))
        return IPJ_ERR_MALFORMED;
    memset(b, 0, sizeof *b);
    b->mode = in[5];
    b->owner_is_test = in[6];
    b->nroutes = in[7];
    memcpy(b->sk, in + 8, 32);
    memcpy(b->owner_pk, in + 40, 32);
    memcpy(b->ip, in + 72, 4);
    memcpy(b->bcast, in + 76, 4);
    b->run_seconds = rs;
    memcpy(b->send_dest, in + 84, 32);
    b->send_len = sl;
    memcpy(b->send_text, in + 120, sl);
    for (unsigned i = 0; i < b->nroutes; i++) {
        memcpy(b->route_dest[i], in + 248 + 64 * i, 32);
        memcpy(b->route_via[i], in + 248 + 64 * i + 32, 32);
    }
    b->roster = in + IPJ_BUNDLE_HDR;
    b->roster_len = rl;
    return IPJ_OK;
}

ipj_status ipj_node_send_signed(ipj_node *n, const uint8_t via[32], const uint8_t *msg, size_t len)
{
    if (!n || !via || !msg || len == 0 || len > sizeof frame_buf - 1) return IPJ_ERR_ARG;
    frame_buf[0] = IPJ_APP_MSG;
    memcpy(frame_buf + 1, msg, len);
    return frame_to(n, via, frame_buf, len + 1);
}
