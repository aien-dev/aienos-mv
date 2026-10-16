/* test_pi5_peer.c -- host test of the frame-level Pi glue for the Pi <-> Spark session (dev/pi5_peer.c):
 * which received frames reach the protocol, which are answered (ARP), which are dropped, and what is sent. */
#include <string.h>

#include "ck_test.h"
#include "pi5_peer.h"

static const net_mac PI_MAC = {{0x2c, 0xcf, 0x67, 0x3f, 0x54, 0x76}};
static const net_mac SP_MAC = {{0x4c, 0xbb, 0x47, 0x01, 0x02, 0x03}};
static const net_mac RT_MAC = {{0x4c, 0x43, 0x41, 0x1f, 0x5f, 0xb4}};
static const net_ipv4 PI_IP = {{192, 168, 1, 192}};
static const net_ipv4 SP_IP = {{192, 168, 1, 108}};
static const net_ipv4 RT_IP = {{192, 168, 1, 1}};

static int got_n;
static uint8_t got[1600];
static size_t got_len;
static int stop_on;
static int on_rx(void *rx, const uint8_t *d, size_t n)
{
    (void)rx;
    got_n++;
    memcpy(got, d, n);
    got_len = n;
    return stop_on;
}

static void pp_init(struct ck_pp *pp)
{
    memset(pp, 0, sizeof *pp);
    pp->p.mac = PI_MAC;
    pp->p.ip = PI_IP;
    pp->p.gw_ip = SP_IP;
    pp->p.lport = 47099;
    pp->p.rport = 47100;
    pp->on_rx = on_rx;
}

/* a frame the Spark (or someone pretending) would send us */
static size_t spark_udp(net_mac src_mac, net_ipv4 src_ip, uint16_t sport, uint16_t dport, const uint8_t *d, size_t n,
                        uint8_t *out)
{
    ck_net_peer s;
    memset(&s, 0, sizeof s);
    s.mac = src_mac;
    s.ip = src_ip;
    s.gw_mac = PI_MAC;
    s.gw_ip = PI_IP;
    s.lport = sport;
    s.rport = dport;
    return ck_net_udp_frame(&s, 7, d, n, out, 1600);
}

static size_t arp_from(net_mac m, net_ipv4 ip, net_ipv4 target, int reply, uint8_t *out)
{
    ck_net_peer s;
    net_arp_packet req;
    memset(&s, 0, sizeof s);
    s.mac = m;
    s.ip = ip;
    s.gw_ip = target;
    if (!reply)
        return ck_net_arp_request(&s, out, 1600);
    memset(&req, 0, sizeof req);
    req.operation = NET_ARP_REQUEST;
    req.sender_mac = PI_MAC;
    req.sender_ip = PI_IP;
    req.target_ip = ip;
    return ck_net_arp_reply(&s, &req, out, 1600);
}

int main(void)
{
    struct ck_pp pp;
    uint8_t f[1600], t[1600], dg[1141];
    size_t n;
    uint32_t w, i;
    net_eth_frame e;
    for (i = 0; i < sizeof dg; i++)
        dg[i] = (uint8_t)(i * 13 + 5);

    /* 1. nothing can be sent as UDP before the Spark's MAC is known; the ARP request goes to broadcast */
    pp_init(&pp);
    CHECK(ck_pp_tx_frame(&pp, dg, 72, t, sizeof t) == 0);
    w = ck_pp_tx_frame(&pp, 0, 0, t, sizeof t);
    CHECK(w == 60);
    CHECK(memcmp(t, "\xff\xff\xff\xff\xff\xff", 6) == 0 && memcmp(t + 6, PI_MAC.b, 6) == 0 && t[12] == 0x08 && t[13] == 0x06);
    CHECK(memcmp(t + 38, SP_IP.b, 4) == 0); /* target protocol address = the Spark */

    /* 2. the Spark's ARP reply: MAC learned, the transfer stops only when we asked for it */
    n = arp_from(SP_MAC, SP_IP, PI_IP, 1, f);
    CHECK(n > 0 && ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 0 && pp.have_mac && pp.arp_reply == 1);
    CHECK(memcmp(pp.p.gw_mac.b, SP_MAC.b, 6) == 0);
    pp.have_mac = 0;
    pp.want_arp = 1;
    CHECK(ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 1 && pp.stop && pp.have_mac);
    pp.want_arp = 0;
    pp.stop = 0;
    /* an ARP reply from the router is not the Spark: ignored, MAC unchanged */
    n = arp_from(RT_MAC, RT_IP, PI_IP, 1, f);
    CHECK(ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 0 && memcmp(pp.p.gw_mac.b, SP_MAC.b, 6) == 0);
    /* once known, the ARP request goes to the Spark only */
    w = ck_pp_tx_frame(&pp, 0, 0, t, sizeof t);
    CHECK(w == 60 && memcmp(t, SP_MAC.b, 6) == 0);

    /* 3. UDP datagram out: parses back as ours, to the Spark, every byte intact; the IP id moves */
    w = ck_pp_tx_frame(&pp, dg, sizeof dg, t, sizeof t);
    CHECK(w == 14 + 20 + 8 + sizeof dg);
    CHECK(net_eth_parse(t, w, &e) == NET_OK && memcmp(e.destination.b, SP_MAC.b, 6) == 0 && memcmp(e.source.b, PI_MAC.b, 6) == 0);
    CHECK(memcmp(t + 42, dg, sizeof dg) == 0);
    CHECK(t[36] == (47100 >> 8) && t[37] == (47100 & 0xff)); /* destination port = the scenario port */
    {
        uint8_t t2[1600];
        uint32_t w2 = ck_pp_tx_frame(&pp, dg, 72, t2, sizeof t2);
        CHECK(w2 == 14 + 20 + 8 + 72 && (t2[18] != t[18] || t2[19] != t[19]));
    }

    /* 4. received UDP from the Spark, right ports: payload handed over byte for byte */
    n = spark_udp(SP_MAC, SP_IP, 47100, 47099, dg, 200, f);
    stop_on = 0;
    got_n = 0;
    CHECK(ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 0 && got_n == 1 && got_len == 200 && memcmp(got, dg, 200) == 0);
    stop_on = 1;
    CHECK(ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 1 && pp.stop && got_n == 2);
    CHECK(pp.udp_ok == 2 && pp.rx_bytes == 400);
    /* the largest datagram the protocol sends (1141 bytes) fits and arrives whole */
    n = spark_udp(SP_MAC, SP_IP, 47100, 47099, dg, sizeof dg, f);
    CHECK(n == 14 + 20 + 8 + sizeof dg && ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 1 && got_len == sizeof dg);

    /* 5. what must never reach the protocol (the instrument can say no) */
    got_n = 0;
    i = pp.ignored;
    n = spark_udp(SP_MAC, SP_IP, 47101, 47099, dg, 100, f); /* another scenario's port */
    CHECK(ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 0);
    n = spark_udp(SP_MAC, SP_IP, 47100, 47098, dg, 100, f); /* not our port */
    CHECK(ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 0);
    n = spark_udp(RT_MAC, RT_IP, 47100, 47099, dg, 100, f); /* not the Spark's address */
    CHECK(ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 0);
    n = spark_udp(SP_MAC, SP_IP, 47100, 47099, dg, 100, f);
    f[42 + 50] ^= 0x01; /* one payload bit flipped: the UDP checksum no longer matches */
    CHECK(ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 0);
    f[42 + 50] ^= 0x01;
    f[0] = 0x02; /* unicast to someone else */
    CHECK(ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 0);
    CHECK(got_n == 0 && pp.ignored == i + 5);
    /* a frame only partly copied out of the window is never parsed */
    n = spark_udp(SP_MAC, SP_IP, 47100, 47099, dg, 100, f);
    CHECK(ck_pp_rx_frame(&pp, f, (uint32_t)n - 1, (uint32_t)n) == 0 && got_n == 0 && pp.truncated == 1);
    /* the keepalive is counted, never handed over */
    n = spark_udp(SP_MAC, SP_IP, 47100, 47099, (const uint8_t *)"PSKA", 4, f);
    CHECK(ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 0 && got_n == 0 && pp.keepalive == 1);

    /* 6. ARP requests for our address: queued once per asker, answered to the asker */
    n = arp_from(RT_MAC, RT_IP, PI_IP, 0, f);
    CHECK(ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 0 && pp.nask == 1);
    CHECK(ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 0 && pp.nask == 1 && pp.arp_ask == 2);
    n = arp_from(SP_MAC, SP_IP, PI_IP, 0, f);
    CHECK(ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 0 && pp.nask == 2);
    n = arp_from(SP_MAC, SP_IP, RT_IP, 0, f); /* asks for someone else: not ours to answer */
    CHECK(ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n) == 0 && pp.nask == 2);
    w = ck_pp_arp_answer(&pp, 0, t, sizeof t);
    CHECK(w == 60 && memcmp(t, RT_MAC.b, 6) == 0 && memcmp(t + 6, PI_MAC.b, 6) == 0 && t[20] == 0 && t[21] == 2);
    CHECK(memcmp(t + 22, PI_MAC.b, 6) == 0 && memcmp(t + 28, PI_IP.b, 4) == 0 && memcmp(t + 38, RT_IP.b, 4) == 0);
    w = ck_pp_arp_answer(&pp, 1, t, sizeof t);
    CHECK(w == 60 && memcmp(t, SP_MAC.b, 6) == 0);
    CHECK(ck_pp_arp_answer(&pp, 2, t, sizeof t) == 0);
    for (i = 0; i < 6; i++) {
        net_mac m = RT_MAC;
        m.b[5] = (uint8_t)(0x10 + i);
        n = arp_from(m, RT_IP, PI_IP, 0, f);
        (void)ck_pp_rx_frame(&pp, f, (uint32_t)n, (uint32_t)n);
    }
    CHECK(pp.nask == CK_PP_MAX_ASK);
    CHECK(ck_pp_tx_frame(&pp, dg, 10, t, 59) == 0);
    return ck_t_verdict("test_pi5_peer");
}
