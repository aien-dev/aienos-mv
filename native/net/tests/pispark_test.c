/* Host test for pispark.c: the Pi initiator against the in-process Spark responder, every scenario,
 * with loss, with the Pi's "lose what arrives after the stop" behaviour, and the refusals that must fire. */
#include <stdio.h>
#include <string.h>

#include "../pispark.h"
#include "../../argus/sha256.h"

static unsigned long checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t prng_s = 0x9e3779b97f4a7c15ull;
static uint8_t rnd8(void)
{
    prng_s ^= prng_s << 13; prng_s ^= prng_s >> 7; prng_s ^= prng_s << 17;
    return (uint8_t)(prng_s >> 24);
}

/* ---- fake wire: one responder per port, a queue Spark -> Pi ---- */
#define QMAX 64
typedef struct {
    pss_resp resp[PSS_NSCN];
    pss_stale stale;
    uint64_t now;
    uint8_t q[QMAX][SEC_MAX_DATAGRAM];
    size_t ql[QMAX];
    uint16_t qport[QMAX];
    int qh, qn;
    int drop_rest;      /* the Pi stops listening when on_rx says stop: the rest is lost */
    unsigned loss_mod;  /* drop every loss_mod-th datagram in either direction (0 = none) */
    unsigned long dgs;
    int rand_fail;
    pss_event evlog[256];
    int nev;
    uint32_t pi_rx_lost;
} fake;

static void fake_init(fake *f)
{
    int s;
    memset(f, 0, sizeof *f);
    for (s = 0; s < PSS_NSCN; s++)
        pss_resp_init(&f->resp[s], s, &f->stale, 40, 6);
}

static int lose(fake *f) { f->dgs++; return f->loss_mod && f->dgs % f->loss_mod == 0; }

static void take(fake *f, uint16_t port, pss_out *o)
{
    uint32_t i;
    for (i = 0; i < o->nev && f->nev < 256; i++)
        f->evlog[f->nev++] = o->ev[i];
    for (i = 0; i < o->n; i++) {
        int t;
        if (lose(f) || f->qn >= QMAX)
            continue;
        t = (f->qh + f->qn++) % QMAX;
        memcpy(f->q[t], o->buf[i], o->len[i]);
        f->ql[t] = o->len[i];
        f->qport[t] = port;
    }
}

static pss_out OUT;
static void spark_rx(fake *f, uint16_t port, const uint8_t *d, size_t n)
{
    uint8_t e[80];
    size_t i;
    int s = port - PSS_PORT_BASE;
    if (s < 0 || s >= PSS_NSCN || lose(f))
        return;
    for (i = 0; i < sizeof e; i++)
        e[i] = rnd8();
    pss_resp_input(&f->resp[s], f->now, d, n, e, &OUT);
    take(f, port, &OUT);
}

static void spark_tick(fake *f)
{
    int s;
    for (s = 0; s < PSS_NSCN; s++) {
        pss_resp_poll(&f->resp[s], f->now, &OUT);
        take(f, (uint16_t)(PSS_PORT_BASE + s), &OUT);
    }
}

static int f_xchg(void *io, uint16_t port, const uint8_t *d, size_t n, uint32_t wait_ms,
                  int (*on_rx)(void *, const uint8_t *, size_t), void *rx)
{
    fake *f = (fake *)io;
    uint32_t t;
    if (n)
        spark_rx(f, port, d, n);
    for (t = 0;; t++) {
        while (f->qn) {
            int h = f->qh, stop;
            f->qh = (f->qh + 1) % QMAX;
            f->qn--;
            if (f->qport[h] != port)
                continue; /* another port: the Pi's filter drops it */
            stop = on_rx(rx, f->q[h], f->ql[h]);
            if (stop) {
                if (f->drop_rest) {
                    f->pi_rx_lost += (uint32_t)f->qn;
                    f->qn = 0;
                }
                f->now += 1;
                return 0;
            }
        }
        if (t >= wait_ms)
            break;
        f->now += 1;
        spark_tick(f);
    }
    return 0;
}
static uint64_t f_now(void *io) { return ((fake *)io)->now; }
static uint64_t f_now_us(void *io) { return ((fake *)io)->now * 1000u; }
static int f_rand(void *io, void *b, size_t n)
{
    size_t i;
    if (((fake *)io)->rand_fail)
        return -1;
    for (i = 0; i < n; i++)
        ((uint8_t *)b)[i] = rnd8();
    return 0;
}

static int count_ev(const fake *f, int kind, int code)
{
    int i, c = 0;
    for (i = 0; i < f->nev; i++)
        if (f->evlog[i].kind == kind && (code == 999 || f->evlog[i].code == code))
            c++;
    return c;
}

static pss_run R;
static void setup(int scn, const char *label, uint32_t nmsg)
{
    uint32_t i;
    memset(&R, 0, sizeof R);
    R.scenario = scn;
    R.pi_label = label;
    R.nmsg = nmsg;
    for (i = 0; i < nmsg; i++)
        R.msg_len[i] = (uint16_t)(i + 1 == nmsg ? CT_MAX_PAYLOAD : PSS_REQ_MIN + 13u * i);
    R.rto_ms = 40;
    R.max_retries = 6;
    R.budget_ms = 20000;
}

static int run(fake *f)
{
    pss_io io = {f_xchg, f_now, f_rand, f_now_us, f};
    f->nev = 0;
    return pss_initiator_run(&R, &io);
}

/* every request hash the Spark logged equals the Pi's own hash of what it sent */
static int hashes_agree(const fake *f, uint32_t n)
{
    uint32_t i, ok = 0;
    int k;
    for (i = 0; i < n; i++)
        for (k = 0; k < f->nev; k++)
            if (f->evlog[k].kind == PSS_EV_REQUEST && f->evlog[k].index == i &&
                memcmp(f->evlog[k].hash, R.req_hash[i], 32) == 0) {
                ok++;
                break;
            }
    return ok == n;
}

static void test_codec(void)
{
    uint8_t nonce[32], m[CT_MAX_PAYLOAD], h[32], rep[PSS_REP_LEN], sn[16];
    uint32_t idx = 99;
    size_t n, i;
    for (i = 0; i < 32; i++)
        nonce[i] = (uint8_t)(i * 7 + 1);
    memset(sn, 0x5a, sizeof sn);
    CHECK(pss_request_build(3, nonce, PSS_REQ_MIN - 1, m, sizeof m) == 0, "short request built");
    CHECK(pss_request_build(3, nonce, CT_MAX_PAYLOAD + 1, m, sizeof m) == 0, "oversize request built");
    n = pss_request_build(3, nonce, 300, m, sizeof m);
    CHECK(n == 300 && pss_request_parse(m, n, &idx) == PSS_OK && idx == 3, "request round trip");
    CHECK(pss_request_parse(m, n - 1, &idx) == PSS_E_LEN, "length field not checked");
    m[200] ^= 1;
    CHECK(pss_request_parse(m, n, &idx) == PSS_E_HASH, "changed filler byte accepted");
    m[200] ^= 1;
    m[0] = 'X';
    CHECK(pss_request_parse(m, n, &idx) == PSS_E_MAGIC, "bad magic accepted");
    m[0] = 'P';
    m[11] = 1;
    CHECK(pss_request_parse(m, n, &idx) == PSS_E_RESERVED, "reserved accepted");
    m[11] = 0;
    sha256_hash(m, n, h);
    CHECK(pss_reply_build(3, h, sn, 300, rep, sizeof rep) == PSS_REP_LEN, "reply build");
    CHECK(pss_reply_check(rep, sizeof rep, 3, h, 300) == PSS_OK, "reply check");
    CHECK(pss_reply_check(rep, sizeof rep, 4, h, 300) == PSS_E_INDEX, "wrong index accepted");
    CHECK(pss_reply_check(rep, sizeof rep, 3, h, 301) == PSS_E_INDEX, "wrong length accepted");
    CHECK(pss_reply_check(rep, sizeof rep - 1, 3, h, 300) == PSS_E_LEN, "short reply accepted");
    rep[0] = 'X';
    CHECK(pss_reply_check(rep, sizeof rep, 3, h, 300) == PSS_E_MAGIC, "reply bad magic accepted");
    rep[0] = 'P';
    rep[59] = 1;
    CHECK(pss_reply_check(rep, sizeof rep, 3, h, 300) == PSS_E_RESERVED, "reply reserved accepted");
    rep[59] = 0;
    {   /* a header that claims its own short length: no read past the 12 header bytes, refused */
        uint8_t s[12] = {'P', 'S', 'Q', '1', 0, 0, 0, 1, 0, 12, 0, 0};
        static uint8_t big[CT_MAX_PAYLOAD + 2];
        CHECK(pss_request_parse(s, sizeof s, &idx) == PSS_E_LEN, "short request accepted");
        pss_request_build(1, nonce, CT_MAX_PAYLOAD, big, sizeof big);
        big[8] = (uint8_t)((CT_MAX_PAYLOAD + 1) >> 8);
        big[9] = (uint8_t)(CT_MAX_PAYLOAD + 1);
        big[CT_MAX_PAYLOAD] = (uint8_t)(nonce[CT_MAX_PAYLOAD % 32] ^ (CT_MAX_PAYLOAD & 0xff));
        CHECK(pss_request_parse(big, CT_MAX_PAYLOAD + 1, &idx) == PSS_E_LEN, "oversize request accepted");
    }
    h[31] ^= 0x80;
    CHECK(pss_reply_check(rep, sizeof rep, 3, h, 300) == PSS_E_HASH, "wrong hash accepted (instrument can say no)");
}

static fake F;

static void test_normal(int drop_rest, unsigned loss)
{
    fake_init(&F);
    F.drop_rest = drop_rest;
    F.loss_mod = loss;
    setup(PSS_NORMAL, PSS_LABEL_PI, 9);
    CHECK(run(&F) == PSS_OK, "normal (drop_rest %d loss %u) result %d", drop_rest, loss, R.result);
    CHECK(R.confirmed && R.verified == 9 && R.delivered == 9 && R.sent == 9, "normal verified %u", R.verified);
    CHECK(R.closed_sent && !R.link_down, "normal close");
    CHECK(hashes_agree(&F, 9), "normal: Spark hashes differ from Pi hashes");
    if (!loss)
        CHECK(count_ev(&F, PSS_EV_CLOSED, 999) == 1, "Spark did not log the authenticated CLOSE");
    CHECK(R.rtt_us[8] > 0 && R.hs_us > 0, "timings missing");
    CHECK(memcmp(R.spark_nonce[0], R.spark_nonce[1], 16) != 0, "Spark nonce not fresh per reply");
    CHECK(memcmp(R.req_nonce[0], R.req_nonce[8], 32) != 0 && memcmp(R.req_nonce[3], R.req_nonce[4], 32) != 0,
          "Pi request nonce not fresh per request");
    CHECK(F.stale.len > 0, "stale store not filled");
}

static void test_scenarios(void)
{
    fake_init(&F);
    /* 1 Pi presents a key the Spark was not told to accept: Spark refuses HS3, Pi never gets CONFIRM */
    setup(PSS_NORMAL, PSS_LABEL_WRONG, 2);
    run(&F);
    CHECK(!R.confirmed && R.verified == 0 && R.link_down, "wrong Pi key: confirmed %d link_down %d", R.confirmed,
          R.link_down);
    CHECK(count_ev(&F, PSS_EV_SEC, SEC_ERR_IDENTITY) >= 1, "Spark did not log IDENTITY for the wrong Pi key");

    /* 2 a Spark identity the Pi was not told to accept */
    setup(PSS_WRONG_PEER, PSS_LABEL_PI, 2);
    run(&F);
    CHECK(R.result == SEC_ERR_IDENTITY && !R.established && R.verified == 0, "wrong peer result %d", R.result);
    CHECK(R.sec_refusals[-SEC_ERR_IDENTITY] == 1, "wrong peer refusals %u", R.sec_refusals[5]);

    /* 3 STALE before any session ran on this peer: nothing to inject, the run stays clean and says so */
    setup(PSS_STALE, PSS_LABEL_PI, 2);
    run(&F);
    CHECK(R.result == PSS_OK && R.verified == 2 && R.sec_refusals[-SEC_ERR_REFUSED] == 0, "stale-empty");
    CHECK(count_ev(&F, PSS_EV_INJECT, PSS_STALE) == 1, "stale-empty inject event");

    /* 4 replay of the previous record before reply 1 */
    setup(PSS_REPLAY, PSS_LABEL_PI, 3);
    run(&F);
    CHECK(R.result == PSS_OK && R.verified == 3, "replay run %d verified %u", R.result, R.verified);
    CHECK(R.sec_refusals[-SEC_ERR_REPLAY] == 1, "replay refusals %u", R.sec_refusals[7]);
    CHECK(count_ev(&F, PSS_EV_INJECT, PSS_REPLAY) == 1, "replay inject event");

    /* 5 a record from the previous session (the REPLAY run just above) */
    setup(PSS_STALE, PSS_LABEL_PI, 2);
    run(&F);
    CHECK(R.result == PSS_OK && R.verified == 2, "stale run");
    CHECK(R.sec_refusals[-SEC_ERR_REFUSED] == 1, "stale refusals %u", R.sec_refusals[4]);

    /* 6 truncated record */
    setup(PSS_TRUNC, PSS_LABEL_PI, 2);
    run(&F);
    CHECK(R.result == PSS_OK && R.verified == 2, "trunc run");
    CHECK(R.sec_refusals[-SEC_ERR_MALFORMED] + R.sec_refusals[-SEC_ERR_BAD_TAG] == 1, "trunc refusals %u/%u",
          R.sec_refusals[2], R.sec_refusals[3]);

    /* 7 peer answers request 0, then goes silent: the Pi gives up after max_retries, never claims delivery */
    setup(PSS_SILENT, PSS_LABEL_PI, 3);
    run(&F);
    CHECK(R.result == CT_ERR_LINK_DOWN && R.link_down && R.verified == 1 && !R.closed_sent, "silent result %d v %u",
          R.result, R.verified);
    CHECK(count_ev(&F, PSS_EV_SILENT_DROP, 999) >= (int)R.max_retries, "silent drops %d",
          count_ev(&F, PSS_EV_SILENT_DROP, 999));
    CHECK(R.cc.tx_retransmits >= R.max_retries && F.now < 20000, "silent: Pi gave up by retries, not by budget");

    /* 8 recovery: a new session on the normal port after the disconnect */
    setup(PSS_NORMAL, PSS_LABEL_PI, 3);
    run(&F);
    CHECK(R.result == PSS_OK && R.verified == 3, "reconnect after silent: %d", R.result);
    CHECK(hashes_agree(&F, 3), "reconnect hashes");
}

static void test_fail_closed(void)
{
    pss_io io = {f_xchg, f_now, f_rand, f_now_us, &F};
    fake_init(&F);
    F.rand_fail = 1;
    setup(PSS_NORMAL, PSS_LABEL_PI, 1);
    CHECK(pss_initiator_run(&R, &io) != PSS_OK && !R.confirmed && R.xchgs == 0, "no RNG must fail closed");
    F.rand_fail = 0;
    setup(PSS_NORMAL, PSS_LABEL_PI, 1);
    R.msg_len[0] = CT_MAX_PAYLOAD + 1;
    CHECK(pss_initiator_run(&R, &io) == PSS_E_ARG, "oversize message accepted");
    setup(PSS_NSCN, PSS_LABEL_PI, 1);
    CHECK(pss_initiator_run(&R, &io) == PSS_E_ARG, "bad scenario accepted");
    /* the whole silence: no Spark at all (wrong port) -> LINK_DOWN, nothing claimed */
    setup(PSS_NORMAL, PSS_LABEL_PI, 1);
    R.scenario = PSS_SILENT;
    F.loss_mod = 1; /* every datagram lost */
    run(&F);
    CHECK(R.link_down && !R.confirmed && R.verified == 0, "dead wire claimed something");
}

int main(void)
{
    test_codec();
    test_normal(0, 0);
    test_normal(1, 0);
    test_normal(1, 5);
    test_normal(0, 3);
    test_scenarios();
    test_fail_closed();
    printf("PISPARK_TEST: %s (%lu checks, %lu failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
