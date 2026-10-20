/* Interplane join tests (hosted): roster, production mode, beacon,
 * rendezvous, signed message, Store round trip, and a simulated LAN of four
 * machines (spark, pi, mac, old) where old is revoked. Deterministic: every
 * key and every byte of "entropy" comes from fixed labels. Prints
 * AIENOS_NET_IPJ: PASS / FAIL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../aienos_ipj.h"
#include "../aienos_ipj_store.h"
#include "../../argus/sha256.h"
#include "../../sig/aienos_sig.h"

static int fails, checks;
#define CHECK(c)                                                                 \
    do {                                                                         \
        checks++;                                                                \
        if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } \
    } while (0)
#define CHECK_EQ(a, b) CHECK((long)(a) == (long)(b))

static void key(uint8_t sk[32], uint8_t pk[32], const char *label)
{
    char b[96];
    snprintf(b, sizeof b, "ipj-test key %s", label);
    sha256_hash((const uint8_t *)b, strlen(b), sk);
    aienos_ed25519_public_key(pk, sk);
}

static uint8_t own_sk[32], own_pk[32];
enum { SPARK, PI, MAC, OLD, MAC2, NM };
static const char *nm[NM] = {"spark", "pi", "mac", "old", "mac"};
static const uint8_t plat[NM] = {IPJ_PLAT_SPARK, IPJ_PLAT_PI, IPJ_PLAT_RUN_MAC, IPJ_PLAT_RUN_WIN, IPJ_PLAT_RUN_MAC};
static uint8_t msk[NM][32], mpk[NM][32];

static size_t sign_r(const ipj_roster *r, uint8_t *out)
{
    size_t w = 0;
    CHECK_EQ(ipj_roster_sign(r, own_sk, out, IPJ_ROSTER_MAX_BYTES, &w), IPJ_OK);
    return w;
}

/* ---- roster ---- */
static void test_roster(void)
{
    static ipj_roster r, a, b;
    static uint8_t blob[IPJ_ROSTER_MAX_BYTES], t[IPJ_ROSTER_MAX_BYTES];
    ipj_roster_init(&r, own_pk);
    for (int i = SPARK; i <= OLD; i++) CHECK_EQ(ipj_roster_add(&r, mpk[i], nm[i], IPJ_KIND_PROD, plat[i]), IPJ_OK);
    CHECK_EQ(r.generation, 4);
    CHECK_EQ(ipj_roster_add(&r, mpk[SPARK], "x", IPJ_KIND_PROD, 1), IPJ_ERR_ARG); /* duplicate key */
    CHECK_EQ(ipj_roster_add(&r, mpk[MAC2], "pi", IPJ_KIND_PROD, 1), IPJ_ERR_ARG); /* active name taken */
    CHECK_EQ(ipj_roster_add(&r, mpk[MAC2], "bad name", IPJ_KIND_PROD, 1), IPJ_ERR_ARG);
    CHECK_EQ(ipj_roster_add(&r, mpk[MAC2], "", IPJ_KIND_PROD, 1), IPJ_ERR_ARG);
    CHECK_EQ(ipj_roster_add(&r, mpk[MAC2], "m", 3, 1), IPJ_ERR_ARG);
    CHECK_EQ(ipj_roster_add(&r, mpk[MAC2], "m", IPJ_KIND_PROD, 0), IPJ_ERR_ARG);
    CHECK_EQ(r.generation, 4);
    size_t n = sign_r(&r, blob);
    CHECK_EQ(n, IPJ_ROSTER_BYTES(4));
    CHECK_EQ(ipj_roster_accept(0, blob, n, own_pk, IPJ_MODE_PRODUCTION, 0, &a), IPJ_OK);
    CHECK_EQ(a.generation, 4);
    CHECK_EQ(a.count, 4);
    CHECK(ipj_roster_member(&a, mpk[OLD], IPJ_MODE_PRODUCTION));
    CHECK(strcmp(a.e[PI].name, "pi") == 0 && a.e[MAC].platform == IPJ_PLAT_RUN_MAC);
    /* every byte matters */
    int all = 1;
    for (size_t i = 0; i < n; i++) {
        memcpy(t, blob, n);
        t[i] ^= 0x01;
        if (ipj_roster_accept(0, t, n, own_pk, IPJ_MODE_PRODUCTION, 0, &b) == IPJ_OK) all = 0;
    }
    CHECK(all);
    for (size_t l = 0; l < n; l++) CHECK(ipj_roster_accept(0, blob, l, own_pk, IPJ_MODE_PRODUCTION, 0, &b) != IPJ_OK);
    /* wrong owner pinned */
    CHECK_EQ(ipj_roster_accept(0, blob, n, mpk[SPARK], IPJ_MODE_PRODUCTION, 0, &b), IPJ_ERR_OWNER);
    /* signed by someone else claiming the owner key */
    CHECK_EQ(ipj_roster_sign(&r, msk[SPARK], t, sizeof t, &n), IPJ_ERR_OWNER);
    /* rollback: same generation and older are refused */
    CHECK_EQ(ipj_roster_accept(&a, blob, IPJ_ROSTER_BYTES(4), own_pk, IPJ_MODE_PRODUCTION, 0, &b), IPJ_ERR_ROLLBACK);
    /* revoke old */
    CHECK_EQ(ipj_roster_revoke(&r, mpk[OLD]), IPJ_OK);
    CHECK_EQ(ipj_roster_revoke(&r, mpk[OLD]), IPJ_ERR_ARG);
    CHECK_EQ(ipj_roster_add(&r, mpk[OLD], "old2", IPJ_KIND_PROD, 4), IPJ_ERR_ARG); /* never comes back */
    n = sign_r(&r, t);
    CHECK_EQ(ipj_roster_accept(&a, t, n, own_pk, IPJ_MODE_PRODUCTION, 0, &b), IPJ_OK);
    CHECK(!ipj_roster_member(&b, mpk[OLD], IPJ_MODE_PRODUCTION));
    CHECK(ipj_roster_member(&b, mpk[MAC], IPJ_MODE_PRODUCTION));
    CHECK_EQ(ipj_roster_accept(&b, blob, IPJ_ROSTER_BYTES(4), own_pk, IPJ_MODE_PRODUCTION, 0, &a), IPJ_ERR_ROLLBACK);
    /* rotate mac -> mac2 */
    CHECK_EQ(ipj_roster_rotate(&r, mpk[MAC], mpk[MAC2]), IPJ_OK);
    n = sign_r(&r, t);
    CHECK_EQ(ipj_roster_accept(&b, t, n, own_pk, IPJ_MODE_PRODUCTION, 0, &a), IPJ_OK);
    CHECK(!ipj_roster_member(&a, mpk[MAC], IPJ_MODE_PRODUCTION));
    CHECK(ipj_roster_member(&a, mpk[MAC2], IPJ_MODE_PRODUCTION));
    CHECK(strcmp(ipj_roster_find(&a, mpk[MAC2])->name, "mac") == 0);
    CHECK_EQ(a.generation, 6);
    /* production mode: TEST entries and a TEST owner are refused */
    static ipj_roster tr;
    ipj_roster_init(&tr, own_pk);
    CHECK_EQ(ipj_roster_add(&tr, mpk[SPARK], "spark", IPJ_KIND_PROD, IPJ_PLAT_SPARK), IPJ_OK);
    CHECK_EQ(ipj_roster_add(&tr, mpk[PI], "pi", IPJ_KIND_TEST, IPJ_PLAT_PI), IPJ_OK);
    n = sign_r(&tr, t);
    CHECK_EQ(ipj_roster_accept(0, t, n, own_pk, IPJ_MODE_PRODUCTION, 0, &a), IPJ_ERR_TEST_KEY);
    CHECK_EQ(ipj_roster_accept(0, t, n, own_pk, IPJ_MODE_TEST, 0, &a), IPJ_OK);
    CHECK(ipj_roster_member(&a, mpk[PI], IPJ_MODE_TEST));
    CHECK(!ipj_roster_member(&a, mpk[PI], IPJ_MODE_PRODUCTION));
    CHECK_EQ(ipj_roster_accept(0, blob, IPJ_ROSTER_BYTES(4), own_pk, IPJ_MODE_PRODUCTION, 1, &a), IPJ_ERR_TEST_KEY);
    CHECK_EQ(ipj_roster_accept(0, blob, IPJ_ROSTER_BYTES(4), own_pk, IPJ_MODE_TEST, 1, &a), IPJ_OK);
    /* a forged roster with a duplicated key, correctly signed, is still refused */
    static ipj_roster d;
    ipj_roster_init(&d, own_pk);
    CHECK_EQ(ipj_roster_add(&d, mpk[SPARK], "spark", IPJ_KIND_PROD, 1), IPJ_OK);
    CHECK_EQ(ipj_roster_add(&d, mpk[PI], "pi", IPJ_KIND_PROD, 3), IPJ_OK);
    memcpy(d.e[1].pk, mpk[SPARK], 32);
    n = sign_r(&d, t);
    CHECK_EQ(ipj_roster_accept(0, t, n, own_pk, IPJ_MODE_PRODUCTION, 0, &a), IPJ_ERR_MALFORMED);
    /* full roster */
    static ipj_roster f;
    ipj_roster_init(&f, own_pk);
    for (unsigned i = 0; i < IPJ_ROSTER_MAX; i++) {
        uint8_t s[32], p[32];
        char l[8];
        snprintf(l, sizeof l, "f%u", i);
        key(s, p, l);
        CHECK_EQ(ipj_roster_add(&f, p, l, IPJ_KIND_PROD, IPJ_PLAT_HUB), IPJ_OK);
    }
    CHECK_EQ(ipj_roster_add(&f, mpk[SPARK], "spark", IPJ_KIND_PROD, 1), IPJ_ERR_FULL);
    n = sign_r(&f, t);
    CHECK(n + 1 <= SEC_MAX_PAYLOAD); /* a full roster still fits one record */
    CHECK_EQ(ipj_roster_accept(0, t, n, own_pk, IPJ_MODE_PRODUCTION, 0, &a), IPJ_OK);
}

/* ---- beacon, rendezvous, message ---- */
static void test_wire(void)
{
    static ipj_roster r;
    static uint8_t blob[IPJ_ROSTER_MAX_BYTES];
    ipj_roster_init(&r, own_pk);
    for (int i = SPARK; i <= OLD; i++) ipj_roster_add(&r, mpk[i], nm[i], IPJ_KIND_PROD, plat[i]);
    ipj_roster_add(&r, mpk[MAC2], "hub", IPJ_KIND_PROD, IPJ_PLAT_HUB);
    ipj_roster_revoke(&r, mpk[OLD]);
    size_t n = sign_r(&r, blob);
    CHECK_EQ(ipj_roster_accept(0, blob, n, own_pk, IPJ_MODE_PRODUCTION, 0, &r), IPJ_OK);
    const uint8_t ip[4] = {10, 0, 0, 2};
    uint8_t b[IPJ_BEACON_LEN], t[IPJ_BEACON_LEN];
    ipj_beacon bc;
    CHECK_EQ(ipj_beacon_build(msk[PI], r.generation, 7, ip, IPJ_PORT, b), IPJ_OK);
    CHECK_EQ(ipj_beacon_parse(&r, IPJ_MODE_PRODUCTION, b, sizeof b, &bc), IPJ_OK);
    CHECK(bc.counter == 7 && bc.port == IPJ_PORT && memcmp(bc.pk, mpk[PI], 32) == 0 && bc.ip[3] == 2);
    int all = 1;
    for (size_t i = 0; i < sizeof b; i++) {
        memcpy(t, b, sizeof b);
        t[i] ^= 0x80;
        if (ipj_beacon_parse(&r, IPJ_MODE_PRODUCTION, t, sizeof t, &bc) == IPJ_OK) all = 0;
    }
    CHECK(all);
    CHECK_EQ(ipj_beacon_build(msk[OLD], r.generation, 1, ip, IPJ_PORT, b), IPJ_OK);
    CHECK_EQ(ipj_beacon_parse(&r, IPJ_MODE_PRODUCTION, b, sizeof b, &bc), IPJ_ERR_NOT_MEMBER);

    /* rendezvous: the mac VM sits behind host NAT, registers 0.0.0.0:47040 */
    static ipj_gateway g;
    memset(&g, 0, sizeof g);
    uint8_t d[IPJ_RV_LEN], out[IPJ_RV_LEN];
    size_t ol = 0;
    const uint8_t nat_ip[4] = {192, 168, 1, 44}, zero[4] = {0};
    ipj_rv v;
    memset(&v, 0, sizeof v);
    v.op = IPJ_RV_REGISTER;
    memcpy(v.subject, mpk[MAC], 32);
    v.counter = 1;
    v.port = IPJ_PORT;
    memcpy(v.ip, zero, 4);
    CHECK_EQ(ipj_rv_build(msk[MAC], &v, d), IPJ_OK);
    CHECK_EQ(ipj_gateway_handle(&g, &r, 0, msk[MAC2], d, sizeof d, nat_ip, 50123, out, sizeof out, &ol), IPJ_OK);
    CHECK_EQ(ol, 0);
    CHECK_EQ(ipj_gateway_handle(&g, &r, 0, msk[MAC2], d, sizeof d, nat_ip, 50123, out, sizeof out, &ol),
             IPJ_ERR_REPLAY);
    /* REGISTER for someone else is malformed (signer must be the subject) */
    memcpy(v.subject, mpk[SPARK], 32);
    v.counter = 2;
    CHECK_EQ(ipj_rv_build(msk[MAC], &v, d), IPJ_OK);
    CHECK_EQ(ipj_gateway_handle(&g, &r, 0, msk[MAC2], d, sizeof d, nat_ip, 1, out, sizeof out, &ol),
             IPJ_ERR_MALFORMED);
    /* revoked machine cannot register or look up */
    memcpy(v.subject, mpk[OLD], 32);
    CHECK_EQ(ipj_rv_build(msk[OLD], &v, d), IPJ_OK);
    CHECK_EQ(ipj_gateway_handle(&g, &r, 0, msk[MAC2], d, sizeof d, nat_ip, 1, out, sizeof out, &ol),
             IPJ_ERR_NOT_MEMBER);
    /* spark looks the mac up: the answer carries the address the gateway saw */
    memset(&v, 0, sizeof v);
    v.op = IPJ_RV_LOOKUP;
    memcpy(v.subject, mpk[MAC], 32);
    v.counter = 9;
    CHECK_EQ(ipj_rv_build(msk[SPARK], &v, d), IPJ_OK);
    CHECK_EQ(ipj_gateway_handle(&g, &r, 0, msk[MAC2], d, sizeof d, ip, 2, out, sizeof out, &ol), IPJ_OK);
    CHECK_EQ(ol, IPJ_RV_LEN);
    ipj_rv a;
    CHECK_EQ(ipj_rv_parse(&r, 0, out, ol, &a), IPJ_OK);
    CHECK(a.op == IPJ_RV_ANSWER && memcmp(a.ip, nat_ip, 4) == 0 && a.port == IPJ_PORT && a.counter == 9 &&
          memcmp(a.signer, mpk[MAC2], 32) == 0);
    /* every byte of a signed rendezvous datagram matters */
    all = 1;
    for (size_t i = 0; i < IPJ_RV_LEN; i++) {
        uint8_t tt[IPJ_RV_LEN];
        memcpy(tt, out, IPJ_RV_LEN);
        tt[i] ^= 0x10;
        ipj_rv vv;
        if (ipj_rv_parse(&r, 0, tt, sizeof tt, &vv) == IPJ_OK) all = 0;
    }
    CHECK(all);
    memcpy(v.subject, mpk[OLD], 32);
    CHECK_EQ(ipj_rv_build(msk[SPARK], &v, d), IPJ_OK);
    CHECK_EQ(ipj_gateway_handle(&g, &r, 0, msk[MAC2], d, sizeof d, ip, 2, out, sizeof out, &ol),
             IPJ_ERR_NOT_MEMBER);
    memcpy(v.subject, mpk[PI], 32);
    CHECK_EQ(ipj_rv_build(msk[SPARK], &v, d), IPJ_OK);
    CHECK_EQ(ipj_gateway_handle(&g, &r, 0, msk[MAC2], d, sizeof d, ip, 2, out, sizeof out, &ol),
             IPJ_ERR_NOT_FOUND);

    /* message */
    static ipj_msg m, p;
    static uint8_t mb[SEC_MAX_PAYLOAD], mt[SEC_MAX_PAYLOAD];
    memset(&m, 0, sizeof m);
    m.kind = IPJ_MSG_TEXT;
    m.ttl = 4;
    memcpy(m.origin, mpk[SPARK], 32);
    memcpy(m.dest, mpk[MAC], 32);
    m.seq = 1;
    m.len = 5;
    memcpy(m.payload, "hello", 5);
    size_t w = 0;
    CHECK_EQ(ipj_msg_build(msk[SPARK], &m, mb, sizeof mb, &w), IPJ_OK);
    CHECK_EQ(w, IPJ_MSG_HDR + 5 + IPJ_SIG_LEN);
    CHECK_EQ(ipj_msg_parse(&r, 0, mb, w, &p), IPJ_OK);
    CHECK(p.len == 5 && memcmp(p.payload, "hello", 5) == 0 && p.ttl == 4);
    CHECK_EQ(ipj_msg_build(msk[PI], &m, mt, sizeof mt, &n), IPJ_ERR_ARG); /* pi cannot sign as spark */
    all = 1;
    for (size_t i = 0; i < w; i++) {
        memcpy(mt, mb, w);
        mt[i] ^= 0x04;
        int ok = ipj_msg_parse(&r, 0, mt, w, &p) == IPJ_OK;
        if (i == 6 ? !ok : ok) all = 0; /* only the ttl byte may change */
    }
    CHECK(all);
    /* a relay that re-signs with its own key but keeps origin = spark */
    memcpy(m.origin, mpk[PI], 32);
    CHECK_EQ(ipj_msg_build(msk[PI], &m, mt, sizeof mt, &n), IPJ_OK);
    memcpy(mt + 8, mpk[SPARK], 32);
    CHECK_EQ(ipj_msg_parse(&r, 0, mt, n, &p), IPJ_ERR_BAD_SIG);
    memcpy(m.origin, mpk[OLD], 32);
    CHECK_EQ(ipj_msg_build(msk[OLD], &m, mt, sizeof mt, &n), IPJ_OK);
    CHECK_EQ(ipj_msg_parse(&r, 0, mt, n, &p), IPJ_ERR_NOT_MEMBER);
    m.len = IPJ_MSG_MAX + 1;
    CHECK_EQ(ipj_msg_build(msk[OLD], &m, mt, sizeof mt, &n), IPJ_ERR_ARG);
}

/* ---- Store round trip (in-memory device) ---- */
#define DEV_UNITS 256u
static uint8_t dev_mem[DEV_UNITS][SV1_UNIT];
static int d_read(void *c, uint64_t u, uint8_t o[SV1_UNIT]) { (void)c; if (u >= DEV_UNITS) return -1; memcpy(o, dev_mem[u], SV1_UNIT); return 0; }
static int d_write(void *c, uint64_t u, const uint8_t i[SV1_UNIT]) { (void)c; if (u >= DEV_UNITS) return -1; memcpy(dev_mem[u], i, SV1_UNIT); return 0; }
static int d_flush(void *c) { (void)c; return 0; }
static st_workspace ws;

static void test_store(void)
{
    static ipj_roster r, l;
    static uint8_t b1[IPJ_ROSTER_MAX_BYTES], b2[IPJ_ROSTER_MAX_BYTES], got[IPJ_ROSTER_MAX_BYTES];
    ipj_roster_init(&r, own_pk);
    for (int i = SPARK; i <= OLD; i++) ipj_roster_add(&r, mpk[i], nm[i], IPJ_KIND_PROD, plat[i]);
    size_t n1 = sign_r(&r, b1);
    ipj_roster_revoke(&r, mpk[OLD]);
    size_t n2 = sign_r(&r, b2);
    st_dev dev = {0, DEV_UNITS, d_read, d_write, d_flush};
    static const uint8_t uuid[16] = {0xa1, 0xe5, 0x1b};
    st_store s;
    CHECK_EQ(st_format(&dev, uuid), 0);
    CHECK_EQ(st_open(&s, &dev, &ws), 0);
    size_t len = 0;
    unsigned skipped = 9;
    CHECK_EQ(ipj_roster_load(&s, own_pk, 0, 0, &l, got, sizeof got, &len, &skipped), IPJ_ERR_NOT_FOUND);
    /* newer first, then the older copy: load must still pick the newer */
    CHECK_EQ(ipj_roster_save(&s, b2, n2, own_pk, 0, 0), IPJ_OK);
    CHECK_EQ(ipj_roster_save(&s, b1, n1, own_pk, 0, 0), IPJ_OK);
    b1[100] ^= 1;
    CHECK_EQ(ipj_roster_save(&s, b1, n1, own_pk, 0, 0), IPJ_ERR_BAD_SIG); /* never stored */
    b1[100] ^= 1;
    CHECK_EQ(st_open(&s, &dev, &ws), 0); /* reboot */
    CHECK_EQ(ipj_roster_load(&s, own_pk, 0, 0, &l, got, sizeof got, &len, &skipped), IPJ_OK);
    CHECK_EQ(l.generation, 5);
    CHECK(len == n2 && memcmp(got, b2, n2) == 0);
    CHECK(!ipj_roster_member(&l, mpk[OLD], 0));
    CHECK_EQ(skipped, 1); /* the older roster is refused as a rollback */
    CHECK_EQ(ipj_roster_load(&s, mpk[SPARK], 0, 0, &l, got, sizeof got, &len, &skipped), IPJ_ERR_NOT_FOUND);
}

/* ---- simulated LAN of nodes ---- */
#define MAXQ 4096
typedef struct {
    int from;
    uint8_t dst[4];
    uint16_t dport;
    uint8_t d[SEC_MAX_DATAGRAM];
    size_t n;
} dgram;
static dgram q[MAXQ];
static int qn;
static ipj_node node[NM];
static int live[NM];
static uint8_t ipof[NM][4];
static int cut_spark_mac;   /* drop every datagram between spark and mac */
static unsigned ent_ctr;
static char logs[NM][4096];
static int verbose;

typedef struct { int idx; } nctx;
static nctx ctxs[NM];

static void ent(void *c, uint8_t out[64])
{
    char b[64];
    snprintf(b, sizeof b, "ipj-test entropy %u", ent_ctr++);
    sha256_hash((const uint8_t *)b, strlen(b), out);
    sha256_hash(out, 32, out + 32);
    (void)c;
}
static void snd(void *c, const uint8_t ip[4], uint16_t port, const uint8_t *d, size_t n)
{
    if (qn >= MAXQ || n > SEC_MAX_DATAGRAM) return;
    q[qn].from = ((nctx *)c)->idx;
    memcpy(q[qn].dst, ip, 4);
    q[qn].dport = port;
    memcpy(q[qn].d, d, n);
    q[qn].n = n;
    qn++;
}
static void lg(void *c, const char *line)
{
    int i = ((nctx *)c)->idx;
    if (verbose) printf("  [%s] %s\n", nm[i], line);
    strncat(logs[i], line, sizeof logs[i] - strlen(logs[i]) - 2);
    strncat(logs[i], "\n", sizeof logs[i] - strlen(logs[i]) - 1);
}

static int deliver(uint64_t now)
{
    static dgram cur[MAXQ];
    int n = qn;
    memcpy(cur, q, sizeof(dgram) * (size_t)n);
    qn = 0;
    for (int k = 0; k < n; k++) {
        dgram *g = &cur[k];
        int bcast = g->dst[3] == 255;
        for (int j = 0; j < NM; j++) {
            if (!live[j] || j == g->from) continue;
            if (!bcast && memcmp(ipof[j], g->dst, 4) != 0) continue;
            int a = g->from == MAC || g->from == MAC2, b = j == MAC || j == MAC2;
            if (cut_spark_mac && ((g->from == SPARK && b) || (a && j == SPARK))) continue;
            (void)ipj_node_input(&node[j], now, ipof[g->from], IPJ_PORT, g->d, g->n);
        }
    }
    return n;
}

static uint64_t run(uint64_t now, int ticks, int beacon_every)
{
    static const uint8_t bc[4] = {10, 0, 0, 255};
    for (int t = 0; t < ticks; t++, now++) {
        for (int j = 0; j < NM; j++) {
            if (!live[j]) continue;
            if (beacon_every && t % beacon_every == 0) ipj_node_beacon(&node[j], bc);
            ipj_node_poll(&node[j], now);
        }
        for (int r = 0; r < 8 && deliver(now); r++) {}
    }
    return now;
}

static void start(int i, const uint8_t *blob, size_t n)
{
    static const sec_config base = {.rto = 3, .max_retries = 20, .rekey_interval = 1024, .max_records = 100000};
    ipj_io io = {&ctxs[i], ent, snd, lg};
    ctxs[i].idx = i;
    ipof[i][0] = 10; ipof[i][1] = 0; ipof[i][2] = 0; ipof[i][3] = (uint8_t)(10 + i);
    CHECK_EQ(ipj_node_init(&node[i], msk[i], own_pk, IPJ_MODE_PRODUCTION, 0, blob, n, ipof[i], IPJ_PORT, &io, &base),
             IPJ_OK);
    live[i] = 1;
    logs[i][0] = 0;
}

static void test_lan(void)
{
    static ipj_roster r;
    static uint8_t g4[IPJ_ROSTER_MAX_BYTES], g5[IPJ_ROSTER_MAX_BYTES], g6[IPJ_ROSTER_MAX_BYTES];
    ipj_roster_init(&r, own_pk);
    for (int i = SPARK; i <= OLD; i++) ipj_roster_add(&r, mpk[i], nm[i], IPJ_KIND_PROD, plat[i]);
    size_t n4 = sign_r(&r, g4);
    ipj_roster_revoke(&r, mpk[OLD]);
    size_t n5 = sign_r(&r, g5);
    ipj_roster_rotate(&r, mpk[MAC], mpk[MAC2]);
    size_t n6 = sign_r(&r, g6);

    /* A machine that is not on its own roster cannot start. */
    static ipj_node x;
    static const sec_config base = {.rto = 3, .max_retries = 20, .rekey_interval = 1024, .max_records = 100000};
    ipj_io io = {&ctxs[0], ent, snd, lg};
    CHECK_EQ(ipj_node_init(&x, msk[OLD], own_pk, IPJ_MODE_PRODUCTION, 0, g5, n5, ipof[0], IPJ_PORT, &io, &base),
             IPJ_ERR_NOT_MEMBER);

    /* spark, pi, mac hold generation 5; old still holds generation 4. */
    start(SPARK, g5, n5);
    start(PI, g5, n5);
    start(MAC, g5, n5);
    start(OLD, g4, n4);
    cut_spark_mac = 1; /* the mac is only reachable through the pi */
    uint64_t now = run(1, 60, 10);
    CHECK(ipj_node_established(&node[SPARK], mpk[PI]));
    CHECK(ipj_node_established(&node[PI], mpk[MAC]));
    CHECK(!ipj_node_established(&node[SPARK], mpk[MAC]));
    for (int i = SPARK; i <= MAC; i++) {
        CHECK(!ipj_node_established(&node[i], mpk[OLD]));
        CHECK(!ipj_node_established(&node[OLD], mpk[i]));
    }
    /* old (lowest or not) was refused by the roster: as initiator its HS3
     * was refused; as responder nobody dialed it. Its roster got updated by
     * nobody, because no session ever came up. */
    CHECK(node[SPARK].c.hs_refused + node[PI].c.hs_refused + node[MAC].c.hs_refused +
          node[SPARK].c.beacon_refused + node[PI].c.beacon_refused + node[MAC].c.beacon_refused > 0);
    CHECK_EQ(node[OLD].roster.generation, 4);

    /* Signed message spark -> pi -> mac. */
    CHECK_EQ(ipj_node_route(&node[SPARK], mpk[MAC], mpk[PI]), IPJ_OK);
    const char *hello = "hello from the spark";
    CHECK_EQ(ipj_node_send(&node[SPARK], now, mpk[MAC], IPJ_MSG_TEXT, (const uint8_t *)hello, strlen(hello)), IPJ_OK);
    now = run(now, 10, 0);
    CHECK_EQ(node[PI].c.relayed, 1);
    CHECK_EQ(node[MAC].c.delivered, 1);
    CHECK(node[MAC].inbox_full && node[MAC].inbox.len == strlen(hello) &&
          memcmp(node[MAC].inbox.payload, hello, strlen(hello)) == 0 &&
          memcmp(node[MAC].inbox.origin, mpk[SPARK], 32) == 0 && node[MAC].inbox.ttl == IPJ_TTL_DEFAULT - 1);
    CHECK(strstr(logs[MAC], "message from spark verified") != 0);
    /* no route at all: refused before anything is sent */
    CHECK_EQ(ipj_node_send(&node[OLD], now, mpk[MAC], IPJ_MSG_TEXT, (const uint8_t *)"x", 1), IPJ_ERR_NO_ROUTE);
    CHECK_EQ(ipj_node_send(&node[SPARK], now, mpk[OLD], IPJ_MSG_TEXT, (const uint8_t *)"x", 1), IPJ_ERR_NOT_MEMBER);

    /* Rotation: the owner gives the spark generation 6 (mac -> mac2). It
     * propagates spark -> pi -> mac by roster push; every holder drops the
     * session with the old mac key; the new mac key joins. */
    CHECK_EQ(ipj_node_roster_update(&node[SPARK], now, g6, n6), IPJ_OK);
    CHECK_EQ(ipj_node_roster_update(&node[SPARK], now, g5, n5), IPJ_ERR_ROLLBACK);
    now = run(now, 10, 0);
    CHECK_EQ(node[PI].roster.generation, 6);
    /* the rotated-away mac key is revoked: nobody keeps a session with it, so it
     * is not told and stays at generation 5, shut out */
    CHECK_EQ(node[MAC].roster.generation, 5);
    CHECK(!ipj_node_established(&node[MAC], mpk[PI]));
    CHECK(!ipj_node_established(&node[PI], mpk[MAC]));
    CHECK(node[PI].c.revoked_closed >= 1);
    live[MAC] = 0;
    start(MAC2, g6, n6);
    now = run(now, 60, 10);
    CHECK(ipj_node_established(&node[PI], mpk[MAC2]));
    CHECK_EQ(ipj_node_route(&node[SPARK], mpk[MAC2], mpk[PI]), IPJ_OK);
    CHECK_EQ(ipj_node_send(&node[SPARK], now, mpk[MAC2], IPJ_MSG_INTERPLANE, (const uint8_t *)"{}", 2), IPJ_OK);
    now = run(now, 10, 0);
    CHECK_EQ(node[MAC2].c.delivered, 1);
    CHECK(node[MAC2].inbox.kind == IPJ_MSG_INTERPLANE);
    CHECK(ipj_node_send(&node[SPARK], now, mpk[MAC], IPJ_MSG_TEXT, (const uint8_t *)"x", 1) != IPJ_OK);
    CHECK(node[SPARK].roster_dirty);

    /* The revoked machine, given generation 4 only, still gets nothing. */
    CHECK(!ipj_node_established(&node[OLD], mpk[SPARK]) && !ipj_node_established(&node[OLD], mpk[PI]));
    (void)now;
}

/* An owner route binds: with a direct spark-mac session up and the pi not yet
 * reachable, a message for the mac waits for the pi instead of going around it
 * (seen in the QEMU gate: the spark sent before its pi session was up). */
static void test_route_binding(void)
{
    static ipj_roster r;
    static uint8_t g5[IPJ_ROSTER_MAX_BYTES];
    ipj_roster_init(&r, own_pk);
    for (int i = SPARK; i <= OLD; i++) ipj_roster_add(&r, mpk[i], nm[i], IPJ_KIND_PROD, plat[i]);
    ipj_roster_revoke(&r, mpk[OLD]);
    size_t n5 = sign_r(&r, g5);
    memset(live, 0, sizeof live);
    qn = 0;
    cut_spark_mac = 0;
    start(SPARK, g5, n5);
    start(MAC, g5, n5);
    uint64_t now = run(1000, 60, 10);
    CHECK(ipj_node_established(&node[SPARK], mpk[MAC]));
    CHECK_EQ(ipj_node_route(&node[SPARK], mpk[MAC], mpk[PI]), IPJ_OK);
    CHECK_EQ(ipj_node_send(&node[SPARK], now, mpk[MAC], IPJ_MSG_TEXT, (const uint8_t *)"x", 1), IPJ_ERR_NO_ROUTE);
    now = run(now, 10, 0);
    CHECK_EQ(node[MAC].c.delivered, 0);
    start(PI, g5, n5);
    now = run(now, 60, 10);
    CHECK(ipj_node_established(&node[SPARK], mpk[PI]) && ipj_node_established(&node[PI], mpk[MAC]));
    CHECK_EQ(ipj_node_send(&node[SPARK], now, mpk[MAC], IPJ_MSG_TEXT, (const uint8_t *)"x", 1), IPJ_OK);
    now = run(now, 10, 0);
    CHECK_EQ(node[PI].c.relayed, 1);
    CHECK_EQ(node[MAC].c.delivered, 1);
    CHECK_EQ(node[MAC].inbox.ttl, IPJ_TTL_DEFAULT - 1);
    (void)now;
}

static void test_bundle(void)
{
    static ipj_roster r;
    static uint8_t rb[IPJ_ROSTER_MAX_BYTES], out[IPJ_BUNDLE_MAX], t[IPJ_BUNDLE_MAX];
    ipj_roster_init(&r, own_pk);
    ipj_roster_add(&r, mpk[SPARK], "spark", IPJ_KIND_PROD, IPJ_PLAT_SPARK);
    size_t rn = sign_r(&r, rb), n = 0;
    static ipj_bundle b, p;
    memset(&b, 0, sizeof b);
    memcpy(b.sk, msk[SPARK], 32);
    memcpy(b.owner_pk, own_pk, 32);
    b.ip[0] = 10; b.ip[3] = 10;
    b.bcast[0] = 10; b.bcast[3] = 255;
    b.run_seconds = 30;
    memcpy(b.send_dest, mpk[MAC], 32);
    b.send_len = 5;
    memcpy(b.send_text, "hello", 5);
    b.nroutes = 1;
    memcpy(b.route_dest[0], mpk[MAC], 32);
    memcpy(b.route_via[0], mpk[PI], 32);
    CHECK_EQ(ipj_bundle_build(&b, rb, rn, out, sizeof out, &n), IPJ_OK);
    CHECK_EQ(n, IPJ_BUNDLE_HDR + rn);
    CHECK_EQ(ipj_bundle_parse(out, n, &p), IPJ_OK);
    CHECK(p.run_seconds == 30 && p.send_len == 5 && memcmp(p.send_text, "hello", 5) == 0 && p.nroutes == 1 &&
          memcmp(p.route_via[0], mpk[PI], 32) == 0 && p.roster_len == rn && memcmp(p.roster, rb, rn) == 0 &&
          memcmp(p.sk, msk[SPARK], 32) == 0);
    for (size_t l = 0; l < n; l++) CHECK(ipj_bundle_parse(out, l, &p) != IPJ_OK);
    int bad = 0;
    const size_t pos[] = {0, 4, 5, 6, 7, 80, 82, 116, 118, 120 + 5, 248 + 64};
    for (size_t i = 0; i < sizeof pos / sizeof pos[0]; i++) {
        memcpy(t, out, n);
        t[pos[i]] ^= 0x40;
        if (ipj_bundle_parse(t, n, &p) == IPJ_OK) bad++;
    }
    CHECK_EQ(bad, 0);
}

/* Guards a well-behaved LAN never reaches: run after test_lan. */
static void test_node_edges(void)
{
    uint64_t now = 100000;
    CHECK_EQ(ipj_node_connect(&node[SPARK], mpk[OLD], ipof[OLD], IPJ_PORT), IPJ_ERR_NOT_MEMBER);
    /* a beacon replayed verbatim is refused */
    uint8_t b[IPJ_BEACON_LEN];
    CHECK_EQ(ipj_beacon_build(msk[PI], node[PI].roster.generation, 1000000, ipof[PI], IPJ_PORT, b), IPJ_OK);
    CHECK_EQ(ipj_node_input(&node[SPARK], now, ipof[PI], IPJ_PORT, b, sizeof b), IPJ_OK);
    uint64_t br = node[SPARK].c.beacon_refused;
    CHECK_EQ(ipj_node_input(&node[SPARK], now, ipof[PI], IPJ_PORT, b, sizeof b), IPJ_ERR_REPLAY);
    CHECK_EQ(node[SPARK].c.beacon_refused, br + 1);
    /* a message the mac already got (spark seq 2), carried again by the pi */
    static ipj_msg m;
    static uint8_t mb[SEC_MAX_PAYLOAD];
    size_t w = 0;
    memset(&m, 0, sizeof m);
    m.kind = IPJ_MSG_INTERPLANE;
    m.ttl = 4;
    memcpy(m.origin, mpk[SPARK], 32);
    memcpy(m.dest, mpk[MAC2], 32);
    m.seq = 2;
    m.len = 2;
    memcpy(m.payload, "{}", 2);
    CHECK_EQ(ipj_msg_build(msk[SPARK], &m, mb, sizeof mb, &w), IPJ_OK);
    uint64_t del = node[MAC2].c.delivered, ref = node[MAC2].c.msg_refused;
    CHECK_EQ(ipj_node_send_signed(&node[SPARK], mpk[PI], mb, w), IPJ_OK);
    now = run(now, 10, 0);
    CHECK_EQ(node[MAC2].c.delivered, del);
    CHECK_EQ(node[MAC2].c.msg_refused, ref + 1);
    /* a fresh seq is still delivered: the refusal above was the replay rule */
    m.seq = 3;
    CHECK_EQ(ipj_msg_build(msk[SPARK], &m, mb, sizeof mb, &w), IPJ_OK);
    CHECK_EQ(ipj_node_send_signed(&node[SPARK], mpk[PI], mb, w), IPJ_OK);
    now = run(now, 10, 0);
    CHECK_EQ(node[MAC2].c.delivered, del + 1);
    /* ttl 0 is never relayed */
    m.seq = 4;
    m.ttl = 0;
    CHECK_EQ(ipj_msg_build(msk[SPARK], &m, mb, sizeof mb, &w), IPJ_OK);
    uint64_t rel = node[PI].c.relayed;
    CHECK_EQ(ipj_node_send_signed(&node[SPARK], mpk[PI], mb, w), IPJ_OK);
    now = run(now, 10, 0);
    CHECK_EQ(node[PI].c.relayed, rel);
    CHECK_EQ(node[MAC2].c.delivered, del + 1);

    /* rendezvous answers: only from the gateway, only for our lookup counter */
    static ipj_roster r;
    static uint8_t rb[IPJ_ROSTER_MAX_BYTES];
    static ipj_node g;
    uint8_t hsk[32], hpk[32];
    key(hsk, hpk, "hub");
    ipj_roster_init(&r, own_pk);
    ipj_roster_add(&r, mpk[SPARK], "spark", IPJ_KIND_PROD, IPJ_PLAT_SPARK);
    ipj_roster_add(&r, mpk[PI], "pi", IPJ_KIND_PROD, IPJ_PLAT_PI);
    ipj_roster_add(&r, hpk, "hub", IPJ_KIND_PROD, IPJ_PLAT_HUB);
    size_t rn = sign_r(&r, rb);
    static const sec_config base = {.rto = 3, .max_retries = 20, .rekey_interval = 1024, .max_records = 100000};
    ipj_io io = {&ctxs[0], ent, snd, lg};
    const uint8_t gip[4] = {10, 0, 0, 99}, pip[4] = {192, 168, 7, 7};
    CHECK_EQ(ipj_node_init(&g, msk[SPARK], own_pk, 0, 0, rb, rn, ipof[SPARK], IPJ_PORT, &io, &base), IPJ_OK);
    CHECK_EQ(ipj_node_set_gateway(&g, mpk[OLD], gip, IPJ_PORT), IPJ_ERR_NOT_MEMBER);
    CHECK_EQ(ipj_node_lookup(&g, mpk[PI]), IPJ_ERR_NO_ROUTE);
    CHECK_EQ(ipj_node_set_gateway(&g, hpk, gip, IPJ_PORT), IPJ_OK);
    CHECK_EQ(ipj_node_lookup(&g, mpk[PI]), IPJ_OK);
    ipj_rv a;
    memset(&a, 0, sizeof a);
    a.op = IPJ_RV_ANSWER;
    memcpy(a.subject, mpk[PI], 32);
    memcpy(a.ip, pip, 4);
    a.port = IPJ_PORT;
    uint8_t d[IPJ_RV_LEN];
    a.counter = g.lookup_ctr + 1;
    CHECK_EQ(ipj_rv_build(hsk, &a, d), IPJ_OK);
    CHECK_EQ(ipj_node_input(&g, now, gip, IPJ_PORT, d, sizeof d), IPJ_ERR_REPLAY);
    a.counter = g.lookup_ctr;
    CHECK_EQ(ipj_rv_build(msk[PI], &a, d), IPJ_OK); /* right counter, signed by a non-gateway member */
    CHECK_EQ(ipj_node_input(&g, now, gip, IPJ_PORT, d, sizeof d), IPJ_ERR_NOT_MEMBER);
    CHECK_EQ(ipj_rv_build(hsk, &a, d), IPJ_OK);
    CHECK_EQ(ipj_node_input(&g, now, gip, IPJ_PORT, d, sizeof d), IPJ_OK);
    CHECK_EQ(ipj_node_input(&g, now, gip, IPJ_PORT, d, sizeof d), IPJ_ERR_REPLAY); /* one answer per lookup */
    int dialed = 0;
    for (unsigned i = 0; i < IPJ_PEERS_MAX; i++)
        if (g.peer[i].used && memcmp(g.peer[i].pk, mpk[PI], 32) == 0 && memcmp(g.peer[i].ip, pip, 4) == 0) dialed = 1;
    CHECK(dialed == (memcmp(mpk[SPARK], mpk[PI], 32) < 0));
    qn = 0;
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    key(own_sk, own_pk, "owner");
    for (int i = 0; i < NM; i++) key(msk[i], mpk[i], i == MAC2 ? "mac2" : nm[i]);
    test_roster();
    test_wire();
    test_store();
    test_bundle();
    test_lan();
    test_node_edges();
    test_route_binding();
    printf("ipj_test: %d checks, %d failed\n", checks, fails);
    printf("AIENOS_NET_IPJ: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
