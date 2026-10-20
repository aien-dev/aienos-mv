/* net_join.c -- Interplane join loop on the bound virtio-net device
 * (CK_INTERPLANE image; see net_join.h and docs/INTERPLANE_JOIN.md).
 * Freestanding, polled. The node logic is native/net/aienos_ipj.c; this file
 * only moves datagrams between it and the NIC: Ethernet II + IPv4 + UDP on
 * port IPJ_PORT, a small learned IPv4 -> MAC table (from ARP and from
 * received frames), answers ARP for our address, and asks ARP for a peer it
 * has not heard from. Every line it prints starts with "interplane:". */
#include "net_join.h"
#include "ck.h"
#include "../core/join_boot.h"
#include "aienos_crypto.h"
#include "net_udp.h"
#include <string.h>

#define MACS 16u
#define ARP_EVERY_MS 500u

typedef struct {
    vnet_dev *dev;
    ck_net_peer self; /* mac, ip, lport (gw_* filled per send) */
    uint8_t bcast[4];
    struct { uint8_t ip[4]; net_mac mac; int used; uint64_t asked; } m[MACS];
    uint16_t ip_id;
    uint64_t now_ms;
    uint64_t tx, tx_err, tx_bad, tx_drop_noarp, rx_udp;
} lan;

static lan g_lan;
static ipj_node g_node;
static uint8_t g_frame[VNET_FRAME_MAX];
static uint8_t g_rx[VNET_FRAME_MAX];

static const net_mac k_bcast = {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};

static int ip_is(const uint8_t a[4], const uint8_t b[4]) { return memcmp(a, b, 4) == 0; }

static void learn(lan *l, const uint8_t ip[4], const net_mac *mac)
{
    if (mac->b[0] & 1) return; /* never learn a group address */
    unsigned free_i = MACS;
    for (unsigned i = 0; i < MACS; i++) {
        if (l->m[i].used && ip_is(l->m[i].ip, ip)) { l->m[i].mac = *mac; return; }
        if (!l->m[i].used && free_i == MACS) free_i = i;
    }
    if (free_i == MACS) return;
    l->m[free_i].used = 1;
    memcpy(l->m[free_i].ip, ip, 4);
    l->m[free_i].mac = *mac;
}

static int lookup(lan *l, const uint8_t ip[4], net_mac *mac)
{
    for (unsigned i = 0; i < MACS; i++)
        if (l->m[i].used && ip_is(l->m[i].ip, ip) && !(l->m[i].mac.b[0] == 0 && l->m[i].mac.b[1] == 0)) {
            *mac = l->m[i].mac;
            return 1;
        }
    return 0;
}

static void arp_ask(lan *l, const uint8_t ip[4])
{
    ck_net_peer p = l->self;
    memcpy(p.gw_ip.b, ip, 4);
    size_t w = ck_net_arp_request(&p, g_frame, sizeof g_frame);
    if (w && vnet_tx(l->dev, g_frame, w) == VNET_OK) l->tx++;
}

static void io_send(void *ctx, const uint8_t ip[4], uint16_t port, const uint8_t *d, size_t n)
{
    lan *l = ctx;
    ck_net_peer p = l->self;
    memcpy(p.gw_ip.b, ip, 4);
    p.rport = port;
    size_t w;
    if (ip_is(ip, l->bcast)) {
        w = ck_net_udp_broadcast_frame(&p, l->ip_id++, d, n, g_frame, sizeof g_frame);
    } else if (lookup(l, ip, &p.gw_mac)) {
        w = ck_net_udp_frame(&p, l->ip_id++, d, n, g_frame, sizeof g_frame);
    } else {
        l->tx_drop_noarp++; /* the sender's resend timer retries after the ARP answer */
        arp_ask(l, ip);
        return;
    }
    if (!w) {
        l->tx_bad++; /* the frame builder refused it */
        return;
    }
    (void)vnet_tx_reclaim(l->dev, 0);
    if (vnet_tx(l->dev, g_frame, w) == VNET_OK) l->tx++; else l->tx_err++;
}

static void io_entropy(void *ctx, uint8_t out[64])
{
    (void)ctx;
    if (ck_entropy_fill(out, 64) != 0) ck_panic("interplane: no entropy for a session key");
}

static void io_log(void *ctx, const char *line)
{
    (void)ctx;
    ck_printf("%s\n", line);
}

/* One received frame: ARP for us, or UDP to IPJ_PORT for us or broadcast. */
static void rx_frame(lan *l, const uint8_t *f, size_t len)
{
    net_eth_frame e;
    if (net_eth_parse(f, len, &e) != NET_OK) return;
    if (memcmp(e.source.b, l->self.mac.b, 6) == 0) return; /* our own frame, looped back by the LAN */
    int to_us = memcmp(e.destination.b, l->self.mac.b, 6) == 0, to_all = memcmp(e.destination.b, k_bcast.b, 6) == 0;
    if (!to_us && !to_all) return;
    if (e.ethertype == NET_ETHERTYPE_ARP) {
        net_arp_packet a;
        if (net_arp_parse(e.payload, e.payload_len, &a) != NET_OK || !ip_is(a.target_ip.b, l->self.ip.b)) return;
        learn(l, a.sender_ip.b, &a.sender_mac);
        if (a.operation == NET_ARP_REQUEST) {
            size_t w = ck_net_arp_reply(&l->self, &a, g_frame, sizeof g_frame);
            if (w && vnet_tx(l->dev, g_frame, w) == VNET_OK) l->tx++;
        }
        return;
    }
    if (e.ethertype != NET_ETHERTYPE_IPV4) return;
    net_ipv4_header ih;
    const uint8_t *ipl = 0;
    size_t ipn = 0;
    if (net_ipv4_parse(e.payload, e.payload_len, &ih, &ipl, &ipn) != NET_OK || ih.protocol != NET_IPPROTO_UDP) return;
    if (!ip_is(ih.destination.b, l->self.ip.b) && !ip_is(ih.destination.b, l->bcast)) return;
    net_udp_header uh;
    const uint8_t *pl = 0;
    size_t pn = 0;
    if (net_udp_parse(ipl, ipn, ih.source, ih.destination, &uh, &pl, &pn) != NET_OK) return;
    if (uh.destination_port != IPJ_PORT) return;
    learn(l, ih.source.b, &e.source);
    l->rx_udp++;
    (void)ipj_node_input(&g_node, l->now_ms, ih.source.b, uh.source_port, pl, pn);
}

static const char *nm(const uint8_t pk[32])
{
    const ipj_entry *e = ipj_roster_find(&g_node.roster, pk);
    return e ? e->name : "unknown";
}

int ck_net_join_run(vnet_dev *dev)
{
    static ipj_bundle b;
    lan *l = &g_lan;
    memset(l, 0, sizeof *l);
    if (ck_join_boot.state != CK_JOIN_FOUND || ipj_bundle_parse(ck_join_boot.buf, ck_join_boot.len, &b) != IPJ_OK) {
        ck_printf("interplane: no join bundle; nothing to do\n");
        return CK_NET_E_ARG;
    }
    l->dev = dev;
    memcpy(l->self.mac.b, dev->mac, 6);
    memcpy(l->self.ip.b, b.ip, 4);
    memcpy(l->bcast, b.bcast, 4);
    l->self.lport = IPJ_PORT;
    l->ip_id = 1;
    static const sec_config base = {.rto = 300, .max_retries = 60, .rekey_interval = 1024, .max_records = 1u << 20};
    ipj_io io = {l, io_entropy, io_send, io_log};
    ipj_status s = ipj_node_init(&g_node, b.sk, b.owner_pk, b.mode, b.owner_is_test, b.roster, b.roster_len, b.ip,
                                 IPJ_PORT, &io, &base);
    volatile uint8_t *w = b.sk;
    for (unsigned i = 0; i < sizeof b.sk; i++) w[i] = 0;
    w = ck_join_boot.buf;
    for (unsigned i = 0; i < 40; i++) w[i] = 0; /* the bundle's key bytes */
    if (s != IPJ_OK) {
        ck_printf("interplane: join refused (%s) mode=%s\n", ipj_status_name(s),
                  b.mode == IPJ_MODE_PRODUCTION ? "production" : "test");
        return CK_NET_E_ARG;
    }
    const ipj_entry *me = ipj_roster_find(&g_node.roster, g_node.pk);
    unsigned members = 0;
    for (unsigned i = 0; i < g_node.roster.count; i++)
        members += ipj_roster_member(&g_node.roster, g_node.roster.e[i].pk, g_node.mode) ? 1u : 0u;
    uint8_t id[16];
    ipj_machine_id(id, g_node.pk);
    ck_printf("interplane: joined as %s platform=%s id=%02x%02x%02x%02x roster_gen=%llu members=%u mode=%s "
              "ip=%u.%u.%u.%u port=%u\n",
              me->name, ipj_platform_name(me->platform), id[0], id[1], id[2], id[3],
              (unsigned long long)g_node.roster.generation, members,
              g_node.mode == IPJ_MODE_PRODUCTION ? "production" : "test", b.ip[0], b.ip[1], b.ip[2], b.ip[3],
              (unsigned)IPJ_PORT);
    for (unsigned i = 0; i < b.nroutes; i++) {
        if (ipj_node_route(&g_node, b.route_dest[i], b.route_via[i]) == IPJ_OK)
            ck_printf("interplane: route to %s via %s\n", nm(b.route_dest[i]), nm(b.route_via[i]));
    }
    int want_send = 0;
    for (unsigned i = 0; i < 32; i++) want_send |= b.send_dest[i];
    uint64_t t0 = ck_time_us() / 1000, last_beacon = 0, last_status = 0;
    uint64_t end = t0 + (uint64_t)b.run_seconds * 1000u;
    uint64_t delivered = 0;
    int sent = 0;
    while ((l->now_ms = ck_time_us() / 1000) < end) {
        uint64_t now = l->now_ms;
        if (now - last_beacon >= 1000 || last_beacon == 0) {
            last_beacon = now;
            (void)ipj_node_beacon(&g_node, l->bcast);
        }
        for (int k = 0; k < 16; k++) {
            size_t len = 0;
            int got = 0;
            vnet_err e = vnet_rx(dev, g_rx, sizeof g_rx, &len, &got);
            if (e != VNET_OK) {
                ck_printf("interplane: rx error %s\n", vnet_err_name(e));
                return CK_NET_E_INIT;
            }
            if (!got) break;
            rx_frame(l, g_rx, len);
        }
        (void)vnet_tx_reclaim(dev, 0);
        ipj_node_poll(&g_node, now);
        if (want_send && !sent && now - last_status >= 1000) {
            last_status = now;
            s = ipj_node_send(&g_node, now, b.send_dest, IPJ_MSG_TEXT, b.send_text, b.send_len);
            if (s == IPJ_OK) {
                sent = 1;
                ck_printf("interplane: sent signed message to %s seq=%llu len=%u\n", nm(b.send_dest),
                          (unsigned long long)g_node.msg_seq, (unsigned)b.send_len);
            }
        }
        if (g_node.c.delivered != delivered) {
            delivered = g_node.c.delivered;
            char text[IPJ_BUNDLE_TEXT + 1];
            ck_net_printable(g_node.inbox.payload, g_node.inbox.len, text, sizeof text);
            ck_printf("interplane: inbox from=%s seq=%llu ttl=%u kind=%u origin_sig=verified text=\"%s\"\n",
                      nm(g_node.inbox.origin), (unsigned long long)g_node.inbox.seq, (unsigned)g_node.inbox.ttl,
                      (unsigned)g_node.inbox.kind, text);
        }
    }
    unsigned up = 0;
    for (unsigned i = 0; i < IPJ_PEERS_MAX; i++) {
        const ipj_peer *p = &g_node.peer[i];
        if (!p->used || p->ep.state != SEC_ST_ESTABLISHED || !p->ep.confirmed) continue;
        up++;
        ck_printf("interplane: established with %s\n", nm(p->pk));
    }
    ck_printf("interplane: summary name=%s roster_gen=%llu sessions=%u hs_refused=%llu beacon_refused=%llu "
              "msg_refused=%llu relayed=%llu delivered=%llu sent=%d tx=%llu tx_err=%llu "
              "tx_bad=%llu tx_noarp=%llu rx_udp=%llu\n",
              me->name, (unsigned long long)g_node.roster.generation, up, (unsigned long long)g_node.c.hs_refused,
              (unsigned long long)g_node.c.beacon_refused, (unsigned long long)g_node.c.msg_refused,
              (unsigned long long)g_node.c.relayed, (unsigned long long)g_node.c.delivered, sent,
              (unsigned long long)l->tx, (unsigned long long)l->tx_err, (unsigned long long)l->tx_bad,
              (unsigned long long)l->tx_drop_noarp, (unsigned long long)l->rx_udp);
    aienos_wipe(&g_node, sizeof g_node);
    return CK_NET_OK;
}
