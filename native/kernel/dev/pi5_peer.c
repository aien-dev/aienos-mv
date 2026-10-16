/* pi5_peer.c -- see pi5_peer.h. The frame-level part is pure (host-tested); the image glue is CK_PI5_NET only. */
#include "pi5_peer.h"

#include <string.h>

static void pad60(uint8_t *out, uint32_t *w)
{
    if (*w && *w < 60u) {
        memset(out + *w, 0, 60u - *w);
        *w = 60u;
    }
}

int ck_pp_rx_frame(struct ck_pp *pp, const uint8_t *d, uint32_t n, uint32_t len)
{
    net_mac mac;
    net_arp_packet arp;
    const uint8_t *pl = 0;
    size_t pn = 0;
    int cs = 0;
    uint32_t i;
    if (!pp || !d)
        return 0;
    pp->frames++;
    if (n < len) {
        pp->truncated++; /* only part of it was copied out: never parse a cut frame */
        return 0;
    }
    switch (ck_net_classify(&pp->p, d, n, &mac, &arp, &pl, &pn, &cs)) {
    case CK_NET_ARP_GW:
        pp->arp_reply++;
        pp->p.gw_mac = mac;
        pp->have_mac = 1;
        if (pp->want_arp) {
            pp->stop = 1;
            return 1;
        }
        return 0;
    case CK_NET_ARP_ASK:
        pp->arp_ask++;
        for (i = 0; i < pp->nask; i++)
            if (memcmp(pp->ask[i].sender_ip.b, arp.sender_ip.b, 4) == 0 &&
                memcmp(pp->ask[i].sender_mac.b, arp.sender_mac.b, 6) == 0)
                return 0;
        if (pp->nask < CK_PP_MAX_ASK)
            pp->ask[pp->nask++] = arp;
        return 0;
    case CK_NET_UDP_REPLY:
        if (pn == 4 && memcmp(pl, "PSKA", 4) == 0) {
            pp->keepalive++; /* the Spark peer's keepalive: it holds the Spark firewall's return path open */
            return 0;
        }
        pp->udp_ok++;
        pp->rx_bytes += (uint32_t)pn;
        if (pp->on_rx && pp->on_rx(pp->rx, pl, pn)) {
            pp->stop = 1;
            return 1;
        }
        return 0;
    default:
        pp->ignored++;
        return 0;
    }
}

uint32_t ck_pp_tx_frame(struct ck_pp *pp, const uint8_t *d, size_t n, uint8_t *out, uint32_t cap)
{
    uint32_t w;
    if (!pp || !out || cap < 60u)
        return 0;
    if (n) {
        if (!pp->have_mac)
            return 0;
        w = (uint32_t)ck_net_udp_frame(&pp->p, pp->ip_id++, d, n, out, cap);
    } else {
        w = (uint32_t)ck_net_arp_request(&pp->p, out, cap);
        if (w && pp->have_mac)
            memcpy(out, pp->p.gw_mac.b, 6); /* known: ask the Spark directly, not the whole LAN */
    }
    pad60(out, &w);
    return w;
}

uint32_t ck_pp_arp_answer(const struct ck_pp *pp, uint32_t i, uint8_t *out, uint32_t cap)
{
    uint32_t w;
    if (!pp || !out || cap < 60u || i >= pp->nask)
        return 0;
    w = (uint32_t)ck_net_arp_reply(&pp->p, &pp->ask[i], out, cap);
    pad60(out, &w);
    return w;
}

/* ================= image glue (CK_PI5_NET only) ================= */
#ifdef CK_PI5_NET
#include "ck.h"
#include "pi5_gem.h"
#include "pi5_wdt.h"
#include "pispark.h"

/* The Spark on this LAN (enP7s7). Compile-time, like the Pi's own address in pi5_gem.c. */
static const uint8_t k_spark_ip[4] = {192, 168, 1, 108};

static struct ck_pp PP;
static struct ck_gem *G;
static uint32_t SPEED;
static int FDX;
static uint8_t OURMAC[6];
static uint8_t TXF[CK_PP_FRAME_MAX], RXB[CK_PP_FRAME_MAX], ANS[64];
static pss_run RUN;
const char *ck_entropy_source(void); /* core: "rng200" on the Pi */

/* link-drop steps: the PHY power-down callback, and the exchange (1-based, within the session) before which it runs */
static int (*LINK_DROP)(void *ctx, uint32_t down_ms);
static void *LINK_CTX;
static uint32_t DROP_AT, XCHG_N;
static int DROP_RC = 1; /* 1 = not run */
#define DROP_MS 3000u

static int arp_resolve(uint32_t tries, uint32_t *used);
static void do_link_drop(void)
{
    uint32_t used = 0;
    uint64_t t0;
    int arc = -1;
    DROP_RC = LINK_DROP ? LINK_DROP(LINK_CTX, DROP_MS) : -1;
    PP.have_mac = 0; /* learn the Spark again after the link came back, as a fresh link would */
    t0 = ck_time_us();
    if (DROP_RC == 0)
        arc = arp_resolve(20, &used); /* r1: frames sent in the first seconds after link-up never reached the Spark */
    ck_printf("pi5_peer: LINK DROP %s (rc=%d); Spark ARP again: %s after %u tries of 500 ms, %llu us\n", DROP_RC == 0 ? "DONE" : "FAILED", DROP_RC,
              arc == 0 ? "answered" : "NO answer", used, (unsigned long long)(ck_time_us() - t0));
    if (arc != 0)
        DROP_RC = -2;
}

static int probe(void *ctx, const uint8_t *d, uint32_t n, uint32_t len) { return ck_pp_rx_frame((struct ck_pp *)ctx, d, n, len); }

static int xfer(const uint8_t *f, uint32_t fl, uint32_t wait_us, int listen)
{
    struct ck_gem_xfer x = {0};
    uint32_t k;
    int rc;
    x.mode = CK_GEMX_WIRE;
    x.speed = SPEED;
    x.full_duplex = FDX;
    x.frame = f;
    x.frame_len = fl;
    x.rx_count = listen ? CK_GEM_RXMAX : 0;
    x.rx_buf_len = 1536;
    x.tx_wait_us = 20000;
    x.rx_wait_us = wait_us;
    for (k = 0; k < 6; k++)
        x.mac[k] = OURMAC[k];
    x.rx_probe = probe;
    x.rx_probe_ctx = &PP;
    x.rx_probe_buf = RXB;
    x.rx_probe_cap = sizeof RXB;
    rc = ck_gem_xfer(G, &x);
    PP.xfers++;
    if (rc) {
        PP.xfer_err++;
        PP.last_rc = (uint32_t)rc;
    }
    if (x.tx_done) {
        PP.tx_frames++;
        PP.tx_bytes += fl;
    }
    if (listen && !x.rx_probe_hit && x.rx_frames >= CK_GEM_RXMAX)
        PP.ring_full++;
    return rc;
}

static void answer_asks(void)
{
    uint32_t i, w;
    for (i = 0; i < PP.nask; i++) {
        w = ck_pp_arp_answer(&PP, i, ANS, sizeof ANS);
        if (w && xfer(ANS, w, 0, 0) == 0)
            PP.arp_answered++;
    }
    PP.nask = 0;
}

static int arp_resolve(uint32_t tries, uint32_t *used)
{
    uint32_t t, fl;
    for (t = 0; t < tries; t++) {
        ck_pi5_wdt_pet();
        PP.want_arp = 1;
        PP.stop = 0;
        PP.on_rx = 0;
        fl = ck_pp_tx_frame(&PP, 0, 0, TXF, sizeof TXF);
        if (!fl || xfer(TXF, fl, 500000u, 1) != 0)
            break;
        answer_asks();
        if (PP.have_mac) {
            *used = t + 1;
            PP.want_arp = 0;
            return 0;
        }
    }
    *used = t;
    PP.want_arp = 0;
    return -1;
}

static int io_xchg(void *io, uint16_t port, const uint8_t *d, size_t n, uint32_t wait_ms,
                   int (*on_rx)(void *, const uint8_t *, size_t), void *rx)
{
    uint64_t end;
    uint32_t fl, used = 0;
    (void)io;
    ck_pi5_wdt_pet();
    if (DROP_AT && ++XCHG_N == DROP_AT)
        do_link_drop();
    if (PP.nask)
        answer_asks();
    if (!n) {
        ck_udelay(wait_ms * 1000u); /* listening needs a transfer and a transfer sends a frame: just wait */
        return 0;
    }
    if (!PP.have_mac && arp_resolve(3, &used) != 0)
        return -1;
    PP.p.rport = port;
    PP.on_rx = on_rx;
    PP.rx = rx;
    PP.stop = 0;
    fl = ck_pp_tx_frame(&PP, d, n, TXF, sizeof TXF);
    if (!fl)
        return -1;
    end = ck_time_us() + (uint64_t)wait_ms * 1000u;
    for (;;) {
        uint64_t now = ck_time_us();
        int rc = xfer(TXF, fl, now < end ? (uint32_t)(end - now) : 0u, 1);
        if (PP.nask)
            answer_asks();
        if (rc)
            return -1;
        if (PP.stop || ck_time_us() >= end)
            return 0;
        /* the ring filled before the time was up: listen again (a transfer must send something: ask the Spark) */
        fl = ck_pp_tx_frame(&PP, 0, 0, TXF, sizeof TXF);
        if (!fl)
            return -1;
    }
}
static uint64_t io_now_ms(void *io) { (void)io; return ck_time_us() / 1000u; }
static uint64_t io_now_us(void *io) { (void)io; return ck_time_us(); }
static int io_rand(void *io, void *b, size_t n) { (void)io; return ck_entropy_fill(b, n) == 0 ? 0 : -1; }

static void hexs(const uint8_t *b, uint32_t n, char *o)
{
    static const char d[] = "0123456789abcdef";
    uint32_t i;
    for (i = 0; i < n; i++) {
        o[2 * i] = d[b[i] >> 4];
        o[2 * i + 1] = d[b[i] & 15];
    }
    o[2 * n] = 0;
}

enum { EXP_OK, EXP_KEY_REFUSED, EXP_IDENTITY, EXP_REPLAY, EXP_STALE, EXP_TRUNC, EXP_SILENT, EXP_SURVIVE_OR_DOWN };
struct step {
    const char *name;
    int scn;
    const char *label;
    uint32_t nmsg;
    int all_max; /* every message CT_MAX_PAYLOAD bytes (throughput) */
    int expect;
    uint32_t drop; /* 0 none; DROP_BEFORE: link drop before the session; n > 0: before exchange n of the session */
};
#define DROP_BEFORE 0xffffffffu

static int judge(const struct step *s, const pss_run *r)
{
    switch (s->expect) {
    case EXP_OK: return r->result == PSS_OK && r->confirmed && r->verified == s->nmsg && r->closed_sent;
    case EXP_KEY_REFUSED: return !r->confirmed && r->verified == 0 && r->sent == 0;
    case EXP_IDENTITY: return r->result == SEC_ERR_IDENTITY && !r->established && r->verified == 0 && r->sec_refusals[-SEC_ERR_IDENTITY] >= 1;
    case EXP_REPLAY: return r->result == PSS_OK && r->verified == s->nmsg && r->sec_refusals[-SEC_ERR_REPLAY] >= 1;
    case EXP_STALE: return r->result == PSS_OK && r->verified == s->nmsg && r->sec_refusals[-SEC_ERR_REFUSED] >= 1;
    case EXP_TRUNC: return r->result == PSS_OK && r->verified == s->nmsg && r->sec_refusals[-SEC_ERR_MALFORMED] + r->sec_refusals[-SEC_ERR_BAD_TAG] >= 1;
    case EXP_SILENT: return r->link_down && r->verified == 1 && r->result == CT_ERR_LINK_DOWN && !r->closed_sent;
    /* the outage (power-down plus link settle) can outlast the 8-retry window; both endings are correct if nothing is
     * claimed that was not verified: the session finishes, or it ends link-down after the handshake with fewer verified */
    case EXP_SURVIVE_OR_DOWN:
        return r->confirmed && ((r->result == PSS_OK && r->verified == s->nmsg && r->closed_sent) ||
                                (r->link_down && r->verified < s->nmsg && r->verified == r->delivered && !r->closed_sent));
    default: return 0;
    }
}

static const char *expect_text(int e)
{
    switch (e) {
    case EXP_OK: return "every request answered with the same SHA-256, then CLOSE";
    case EXP_KEY_REFUSED: return "the Spark refuses our (wrong) key: no session, nothing sent";
    case EXP_IDENTITY: return "we refuse the Spark's (wrong) identity: no session";
    case EXP_REPLAY: return "one replayed record refused, every request still answered";
    case EXP_STALE: return "one record of an earlier session refused, every request still answered";
    case EXP_TRUNC: return "one truncated record refused, every request still answered";
    case EXP_SILENT: return "request 0 answered, then the peer goes silent: link down after max retries, nothing claimed";
    case EXP_SURVIVE_OR_DOWN: return "the link drops mid-session: the session finishes, or ends link-down with only verified requests claimed";
    default: return "?";
    }
}

static void run_step(const struct step *s, const pss_io *io, uint32_t *pass, uint32_t *total)
{
    pss_run *r = &RUN;
    char h[65], nn[65], sn[33], sid[17];
    uint32_t i, ok;
    uint64_t mn = ~0ull, mx = 0, sum = 0, bytes = 0;
    uint32_t pp_udp0 = PP.udp_ok, pp_x0 = PP.xfers, pp_ign0 = PP.ignored, pp_rf0 = PP.ring_full;
    memset(r, 0, sizeof *r);
    r->scenario = s->scn;
    r->pi_label = s->label;
    r->nmsg = s->nmsg;
    for (i = 0; i < s->nmsg; i++)
        r->msg_len[i] = (uint16_t)((s->all_max || i + 1 == s->nmsg) ? CT_MAX_PAYLOAD : PSS_REQ_MIN + 37u * i);
    r->rto_ms = 200;
    r->max_retries = 8;
    r->budget_ms = s->drop ? 25000 : 12000;
    ck_pi5_wdt_pet();
    ck_printf("pi5_peer: SESSION %s: port %u scenario=%s pi_identity=TEST(%s) messages=%u expect: %s%s\n", s->name,
              (unsigned)(PSS_PORT_BASE + (unsigned)s->scn), pss_scenario_name(s->scn), s->label, s->nmsg, expect_text(s->expect),
              s->drop == DROP_BEFORE ? " (after a forced link drop and a fresh ARP)" : s->drop ? " (across a forced link drop inside the session)" : "");
    DROP_RC = 1;
    XCHG_N = 0;
    DROP_AT = (s->drop && s->drop != DROP_BEFORE) ? s->drop : 0;
    if (s->drop == DROP_BEFORE)
        do_link_drop();
    if (s->drop == DROP_BEFORE && DROP_RC != 0) {
        ck_printf("pi5_peer: VERDICT %s FAIL (the link did not come back; no session attempted)\n", s->name);
        (*total)++;
        return;
    }
    (void)pss_initiator_run(r, io);
    DROP_AT = 0;
    ck_pi5_wdt_pet();
    hexs(r->session_id, 8, sid);
    ck_printf("pi5_peer: %s result=%d hs_status=%d established=%u confirmed=%u session=%s hs_us=%llu total_us=%llu sent=%u delivered=%u verified=%u link_down=%u closed_sent=%u first_reply_err=%d\n",
              s->name, r->result, r->hs_status, (unsigned)r->established, (unsigned)r->confirmed, r->confirmed ? sid : "none",
              (unsigned long long)r->hs_us, (unsigned long long)r->total_us, r->sent, r->delivered, r->verified, (unsigned)r->link_down,
              (unsigned)r->closed_sent, r->first_reply_err);
    for (i = 0; i < r->nmsg; i++) {
        if (r->rtt_us[i]) {
            sum += r->rtt_us[i];
            bytes += r->msg_len[i];
            if (r->rtt_us[i] < mn) mn = r->rtt_us[i];
            if (r->rtt_us[i] > mx) mx = r->rtt_us[i];
        }
        if (i < r->sent && (!s->all_max || i == 0 || i + 1 == r->nmsg)) {
            hexs(r->req_hash[i], 32, h);
            hexs(r->req_nonce[i], 32, nn);
            hexs(r->spark_nonce[i], 16, sn);
            ck_printf("pi5_peer: %s REQ index=%u len=%u nonce=%s sha256=%s reply=%s spark_nonce=%s rtt_us=%llu\n", s->name, i,
                      r->msg_len[i], nn, h, r->rtt_us[i] ? "VERIFIED(same sha256)" : "none", r->rtt_us[i] ? sn : "-",
                      (unsigned long long)r->rtt_us[i]);
        }
    }
    if (r->verified)
        ck_printf("pi5_peer: %s rtt_us min=%llu avg=%llu max=%llu over %u round trips; request bytes verified=%llu; goodput=%llu bytes/s (request bytes / summed round-trip time, stop-and-wait)\n",
                  s->name, (unsigned long long)mn, (unsigned long long)(sum / r->verified), (unsigned long long)mx, r->verified,
                  (unsigned long long)bytes, (unsigned long long)(sum ? bytes * 1000000ull / sum : 0));
    ck_printf("pi5_peer: %s refusals sec: malformed=%u bad_tag=%u refused(session)=%u identity=%u bad_sig=%u replay=%u state=%u | ctl: malformed=%u bad_tag=%u refused=%u | sec rx_records=%llu tx_records=%llu tx_hs=%llu hs_resend=%llu | ctl tx_frames=%llu retransmits=%llu rx_delivered=%llu rx_duplicate=%llu\n",
              s->name, r->sec_refusals[2], r->sec_refusals[3], r->sec_refusals[4], r->sec_refusals[5], r->sec_refusals[6], r->sec_refusals[7],
              r->sec_refusals[10], r->ctl_refusals[2], r->ctl_refusals[3], r->ctl_refusals[4], (unsigned long long)r->sc.rx_records,
              (unsigned long long)r->sc.tx_records, (unsigned long long)r->sc.tx_hs, (unsigned long long)r->sc.tx_hs_resend,
              (unsigned long long)r->cc.tx_frames, (unsigned long long)r->cc.tx_retransmits, (unsigned long long)r->cc.rx_delivered,
              (unsigned long long)r->cc.rx_duplicate);
    ck_printf("pi5_peer: %s wire: exchanges=%u datagrams_rx=%u gem_transfers=%u udp_from_spark=%u other_frames_dropped=%u ring_full=%u\n", s->name,
              r->xchgs, r->datagrams_rx, PP.xfers - pp_x0, PP.udp_ok - pp_udp0, PP.ignored - pp_ign0, PP.ring_full - pp_rf0);
    ok = (uint32_t)judge(s, r) && (!s->drop || DROP_RC == 0);
    if (s->drop)
        ck_printf("pi5_peer: %s link drop rc=%d (0 = the link went down and came back settled)\n", s->name, DROP_RC);
    ck_printf("pi5_peer: VERDICT %s %s\n", s->name, ok ? "PASS" : "FAIL");
    *pass += ok;
    (*total)++;
}

void ck_pi5peer_run(struct ck_gem *g, uint32_t speed, int full_duplex, const uint8_t mac[6], const uint8_t ip[4],
                    int (*link_drop)(void *ctx, uint32_t down_ms), void *link_ctx)
{
    static const struct step steps[] = {
        {"normal", PSS_NORMAL, PSS_LABEL_PI, 9, 0, EXP_OK, 0},
        {"wrong-pi-key", PSS_NORMAL, PSS_LABEL_WRONG, 1, 0, EXP_KEY_REFUSED, 0},
        {"wrong-spark-identity", PSS_WRONG_PEER, PSS_LABEL_PI, 1, 0, EXP_IDENTITY, 0},
        {"replay", PSS_REPLAY, PSS_LABEL_PI, 3, 0, EXP_REPLAY, 0},
        {"stale-session", PSS_STALE, PSS_LABEL_PI, 2, 0, EXP_STALE, 0},
        {"truncated", PSS_TRUNC, PSS_LABEL_PI, 2, 0, EXP_TRUNC, 0},
        {"silent-peer", PSS_SILENT, PSS_LABEL_PI, 3, 0, EXP_SILENT, 0},
        {"reconnect", PSS_NORMAL, PSS_LABEL_PI, 3, 0, EXP_OK, 0},
        {"throughput-1024x32", PSS_NORMAL, PSS_LABEL_PI, 32, 1, EXP_OK, 0},
        /* forced link loss from the Pi side (PHY power-down), not a cable pull */
        {"across-link-drop", PSS_NORMAL, PSS_LABEL_PI, 4, 0, EXP_SURVIVE_OR_DOWN, 6},
        {"after-link-drop", PSS_NORMAL, PSS_LABEL_PI, 3, 0, EXP_OK, DROP_BEFORE},
    };
    pss_io io = {io_xchg, io_now_ms, io_rand, io_now_us, 0};
    uint32_t i, used = 0, pass = 0, total = 0;
    uint64_t t0;
    if (!g)
        return;
    G = g;
    LINK_DROP = link_drop;
    LINK_CTX = link_ctx;
    SPEED = speed;
    FDX = full_duplex;
    memset(&PP, 0, sizeof PP);
    for (i = 0; i < 6; i++)
        OURMAC[i] = PP.p.mac.b[i] = mac[i];
    for (i = 0; i < 4; i++) {
        PP.p.ip.b[i] = ip[i];
        PP.p.gw_ip.b[i] = k_spark_ip[i];
    }
    PP.p.lport = (uint16_t)PSS_PI_PORT;
    PP.p.rport = (uint16_t)PSS_PORT_BASE;
    ck_pi5_wdt_pet();
    ck_printf("pi5_peer: Pi <-> Spark session test (aienos-mv#398): we are %u.%u.%u.%u:%u, the Spark is %u.%u.%u.%u:%u..%u; M6-B (TEST Ed25519 identities, X25519, AES-256-GCM-SIV) carrying M6-A; entropy source=%s state=%s; static state: sec_endpoint=%u ct_endpoint=%u run=%u bytes, no heap\n",
              ip[0], ip[1], ip[2], ip[3], (unsigned)PSS_PI_PORT, k_spark_ip[0], k_spark_ip[1], k_spark_ip[2], k_spark_ip[3],
              (unsigned)PSS_PORT_BASE, (unsigned)(PSS_PORT_BASE + PSS_NSCN - 1), ck_entropy_source(), ck_entropy_reason(), (unsigned)sizeof(sec_endpoint),
              (unsigned)sizeof(ct_endpoint), (unsigned)sizeof(pss_run));
    t0 = ck_time_us();
    if (arp_resolve(6, &used) != 0) {
        ck_printf("pi5_peer: ARP SPARK FAIL: no ARP reply from %u.%u.%u.%u in %u tries of 500 ms (frames seen=%u arp_asks=%u answered=%u ignored=%u gem_err=%u last_rc=%d); no session attempted\n",
                  k_spark_ip[0], k_spark_ip[1], k_spark_ip[2], k_spark_ip[3], used, PP.frames, PP.arp_ask, PP.arp_answered, PP.ignored,
                  PP.xfer_err, (int)PP.last_rc);
        return;
    }
    ck_printf("pi5_peer: ARP SPARK PASS: %u.%u.%u.%u is-at %02x:%02x:%02x:%02x:%02x:%02x (try %u, %llu us; RX of a unicast reply on the wire)\n",
              k_spark_ip[0], k_spark_ip[1], k_spark_ip[2], k_spark_ip[3], PP.p.gw_mac.b[0], PP.p.gw_mac.b[1], PP.p.gw_mac.b[2],
              PP.p.gw_mac.b[3], PP.p.gw_mac.b[4], PP.p.gw_mac.b[5], used, (unsigned long long)(ck_time_us() - t0));
    for (i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        run_step(&steps[i], &io, &pass, &total);
        if (G->halted) {
            ck_printf("pi5_peer: STOP the GEM latched halted (rc=%d); no further transfers\n", (int)PP.last_rc);
            break;
        }
    }
    ck_printf("pi5_peer: totals: gem_transfers=%u transfer_errors=%u frames_rx=%u udp_from_spark=%u keepalives=%u arp_asks_seen=%u arp_answered=%u truncated_copies=%u other_dropped=%u ring_full=%u tx_frames=%u tx_bytes=%u udp_rx_bytes=%u elapsed_us=%llu\n",
              PP.xfers, PP.xfer_err, PP.frames, PP.udp_ok, PP.keepalive, PP.arp_ask, PP.arp_answered, PP.truncated, PP.ignored, PP.ring_full,
              PP.tx_frames, PP.tx_bytes, PP.rx_bytes, (unsigned long long)(ck_time_us() - t0));
    ck_printf("pi5_peer: SUMMARY %u/%u sessions as expected\n", pass, total);
}
#endif
