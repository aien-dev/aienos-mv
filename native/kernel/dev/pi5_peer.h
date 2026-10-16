/* pi5_peer.h -- the Pi side of the Pi <-> Spark authenticated session test (aienos-mv#398), over the RP1 GEM.
 *
 * Transport for native/net/pispark.c (the protocol driver): every exchange is ONE validated GEM transfer
 * (ck_gem_xfer: descriptors inside the ADR 0020 bounce window, bus-master on only inside the transfer) that sends one
 * UDP datagram and listens on the same transfer's RX ring. Frames are classified by dev/net_udp.c (the M6-A parsers):
 * UDP from the Spark's address and the scenario port to our port goes to the protocol; ARP requests for our address
 * are answered right after the transfer; everything else is counted and dropped. Static IPv4, no DHCP.
 *
 * The frame-level parts (ck_pp_rx_frame, ck_pp_tx_frame) are pure and host-tested in tests/test_pi5_peer.c; the
 * image glue (ck_pi5peer_run) exists only in the CK_PI5_NET image. TEST identities only (pispark.h labels). */
#ifndef AIENOS_CK_PI5_PEER_H
#define AIENOS_CK_PI5_PEER_H
#include <stddef.h>
#include <stdint.h>

#include "net_udp.h"

#define CK_PP_MAX_ASK 4u
#define CK_PP_FRAME_MAX 1536u

struct ck_pp {
    ck_net_peer p;            /* ours = mac/ip/lport; the Spark = gw_ip/rport (gw_mac learned by ARP) */
    int have_mac;
    uint16_t ip_id;
    /* current exchange */
    int (*on_rx)(void *rx, const uint8_t *d, size_t n);
    void *rx;
    int want_arp;             /* stop the transfer at the Spark's ARP reply */
    int stop;
    /* ARP requests for our address seen in the last transfer, answered after it */
    net_arp_packet ask[CK_PP_MAX_ASK];
    uint32_t nask;
    /* counters (whole boot) */
    uint32_t frames, truncated, udp_ok, keepalive, arp_reply, arp_ask, arp_answered, ignored;
    uint32_t xfers, xfer_err, ring_full, tx_frames, tx_bytes, rx_bytes, last_rc;
};

/* One received frame (n bytes copied, device length len). Returns non-zero to stop the transfer. */
int ck_pp_rx_frame(struct ck_pp *pp, const uint8_t *d, uint32_t n, uint32_t len);
/* The frame to send: the UDP datagram (n > 0) to the Spark, or an ARP request for the Spark (n == 0). Padded to the
 * 60-byte Ethernet minimum. Returns the length, 0 if it cannot be built (no Spark MAC yet for UDP). */
uint32_t ck_pp_tx_frame(struct ck_pp *pp, const uint8_t *d, size_t n, uint8_t *out, uint32_t cap);
/* The ARP answer for pending ask i. */
uint32_t ck_pp_arp_answer(const struct ck_pp *pp, uint32_t i, uint8_t *out, uint32_t cap);

#ifdef CK_PI5_NET
struct ck_gem;
/* The whole session sequence (ARP the Spark, then every pispark scenario), logged line by line. link_drop forces a
 * link loss from the Pi side (PHY power-down for down_ms, then a settled link) and returns 0 when it went down and back. */
void ck_pi5peer_run(struct ck_gem *g, uint32_t speed, int full_duplex, const uint8_t mac[6], const uint8_t ip[4],
                    int (*link_drop)(void *ctx, uint32_t down_ms), void *link_ctx);
#endif
#endif
