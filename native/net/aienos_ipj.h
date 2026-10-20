/* AIENOS Interplane join (hosted C, no heap, clock or I/O). Design:
 * docs/INTERPLANE_JOIN.md. This header is the wire spec.
 *
 * Every AienOS machine (native on the Spark, iMac or Pi, or a VM inside
 * aienos-run on Windows, macOS or Linux) is one Interplane peer. A peer is an
 * Ed25519 machine key that the owner listed in a signed roster. Sessions
 * between peers are the M6-B secure transport (aienos_sec.h); this file adds:
 *
 *   - the roster: owner-signed list of machine keys (add, revoke, rotate),
 *     with a generation that only goes up (an older roster is refused);
 *   - production mode: refuses TEST entries and a TEST owner key;
 *   - the beacon: a machine-signed "I am here" datagram for LAN discovery;
 *   - rendezvous: register / lookup / answer through the gateway machine, for
 *     peers that LAN broadcast cannot reach (aienos-run behind host NAT);
 *   - the signed message: signed by its origin machine, carried hop by hop
 *     inside secure sessions; relays cannot forge or alter it;
 *   - the node: a table of up to IPJ_PEERS_MAX secure sessions on one UDP
 *     port, with the roster as the only admission rule.
 *
 * All integers big-endian. Signatures are Ed25519 (native/sig) over a
 * NUL-terminated domain label followed by the signed bytes.
 *
 * Machine id: SHA-256("AIENOS-IPJ-v1 machine id\0" | machine_pk)[0..16).
 *
 * Roster (IPJ_ROSTER_HDR + 64 * count + 64 bytes), label "AIENOS-IPJ-v1 roster":
 *   0 magic "AIPR"  4 version 1  5 flags 0  6 count u16 (1..IPJ_ROSTER_MAX)
 *   8 owner_pk[32]  40 generation u64 (>= 1)  48 reserved[16] = 0
 *   entry i at 64 + 64 * i:
 *     0 machine_pk[32]  32 name[16] (ASCII, NUL padded, first byte not NUL)
 *     48 kind (1 PROD, 2 TEST)  49 platform (IPJ_PLAT_*)  50 status (1 active,
 *     2 revoked)  51 reserved[5] = 0  56 changed_gen u64 (<= generation)
 *   trailer: owner signature over everything before it.
 *   Machine keys are unique within a roster; names of active entries too.
 *
 * Beacon (128 bytes, type "AIPB", label "AIENOS-IPJ-v1 beacon"):
 *   0 magic "AIPB"  4 version 1  5 reserved[3] = 0  8 machine_pk[32]
 *   40 roster_gen u64  48 counter u64  56 ipv4[4]  60 port u16
 *   62 reserved u16 = 0  64 machine signature over bytes 0..64.
 *   The counter rises with every beacon of one boot; a receiver keeps the
 *   highest counter per peer and refuses anything not above it.
 *
 * Rendezvous: see the IPJ_RV_* section below.
 */
#ifndef AIENOS_IPJ_H
#define AIENOS_IPJ_H

#include <stddef.h>
#include <stdint.h>

#include "aienos_sec.h"

#define IPJ_VERSION 1u
#define IPJ_PORT 47040u /* the one UDP port of a peer (sessions, beacons, rendezvous) */

/* 15 entries keep a signed roster (1088 bytes) inside one secure record, so a
 * peer can push it whole; more machines need roster fragments (not in v1). */
#define IPJ_ROSTER_MAX 15u
#define IPJ_ROSTER_HDR 64u
#define IPJ_ROSTER_ENTRY 64u
#define IPJ_SIG_LEN 64u
#define IPJ_ROSTER_BYTES(n) (IPJ_ROSTER_HDR + IPJ_ROSTER_ENTRY * (size_t)(n) + IPJ_SIG_LEN)
#define IPJ_ROSTER_MAX_BYTES IPJ_ROSTER_BYTES(IPJ_ROSTER_MAX)
#define IPJ_NAME_LEN 16u

#define IPJ_KIND_PROD 1u
#define IPJ_KIND_TEST 2u
#define IPJ_ST_ACTIVE 1u
#define IPJ_ST_REVOKED 2u

/* Platforms (what the machine is, for the console and routing hints only;
 * never an authority). */
#define IPJ_PLAT_SPARK 1u   /* native AienOS on the DGX Spark (aarch64) */
#define IPJ_PLAT_IMAC 2u    /* native AienOS on the iMac (x86_64) */
#define IPJ_PLAT_PI 3u      /* native AienOS on the Raspberry Pi 5 */
#define IPJ_PLAT_RUN_WIN 4u /* AienOS in aienos-run on Windows */
#define IPJ_PLAT_RUN_MAC 5u /* AienOS in aienos-run on macOS */
#define IPJ_PLAT_RUN_LNX 6u /* AienOS in aienos-run on Linux */
#define IPJ_PLAT_HUB 7u     /* the gateway (Linux hub until the Pi runs natively) */
#define IPJ_PLAT_MAX 7u

/* Mode of the machine that checks a roster. */
#define IPJ_MODE_PRODUCTION 0u /* TEST entries are not members; TEST owner refused */
#define IPJ_MODE_TEST 1u       /* hosted tests and QEMU fixtures that say so */

typedef enum {
    IPJ_OK = 0,
    IPJ_ERR_ARG = -1,
    IPJ_ERR_MALFORMED = -2,  /* length, magic, version, reserved, duplicates, ranges */
    IPJ_ERR_BAD_SIG = -3,    /* signature did not verify */
    IPJ_ERR_OWNER = -4,      /* roster signed by a key other than the pinned owner */
    IPJ_ERR_ROLLBACK = -5,   /* roster generation not above the one we hold */
    IPJ_ERR_TEST_KEY = -6,   /* production mode met a TEST owner or entry */
    IPJ_ERR_NOT_MEMBER = -7, /* key absent, revoked, or TEST in production */
    IPJ_ERR_REPLAY = -8,     /* beacon / rendezvous / message counter not fresh */
    IPJ_ERR_FULL = -9,
    IPJ_ERR_NOT_FOUND = -10,
    IPJ_ERR_NO_ROUTE = -11,
    IPJ_ERR_TTL = -12,
    IPJ_ERR_SEC = -13,       /* the secure transport refused (see node counters) */
    IPJ_ERR_STORE = -14,
} ipj_status;

typedef struct {
    uint8_t pk[32];
    char name[IPJ_NAME_LEN + 1];
    uint8_t kind, platform, status;
    uint64_t changed_gen;
} ipj_entry;

typedef struct {
    uint8_t owner_pk[32];
    uint64_t generation; /* 0: empty (nothing accepted yet) */
    uint16_t count;
    ipj_entry e[IPJ_ROSTER_MAX];
} ipj_roster;

void ipj_machine_id(uint8_t id[16], const uint8_t pk[32]);
const char *ipj_status_name(ipj_status s);
const char *ipj_platform_name(uint8_t platform);

/* ---- owner side (holds the owner secret key; the owner's own machine or the
 * provisioning tool, never a peer) ---- */
void ipj_roster_init(ipj_roster *r, const uint8_t owner_pk[32]);
/* Each edit bumps generation by one and stamps changed_gen. */
ipj_status ipj_roster_add(ipj_roster *r, const uint8_t pk[32], const char *name, uint8_t kind, uint8_t platform);
ipj_status ipj_roster_revoke(ipj_roster *r, const uint8_t pk[32]);
/* Revoke old_pk and add new_pk with the same name, kind and platform. */
ipj_status ipj_roster_rotate(ipj_roster *r, const uint8_t old_pk[32], const uint8_t new_pk[32]);
/* Encode and sign with owner_sk (whose public key must be r->owner_pk). */
ipj_status ipj_roster_sign(const ipj_roster *r, const uint8_t owner_sk[32], uint8_t *out, size_t cap,
                           size_t *written);

/* ---- peer side ---- */
/* Parse and verify a signed roster against the pinned owner key. Refuses a
 * generation not above cur->generation (cur may be NULL or empty). In
 * production mode a TEST owner (owner_is_test != 0) or any TEST entry is
 * refused. On success *out holds it; on failure *out is untouched. */
ipj_status ipj_roster_accept(const ipj_roster *cur, const uint8_t *blob, size_t len, const uint8_t owner_pk[32],
                             unsigned mode, int owner_is_test, ipj_roster *out);
/* 1 if pk is an active member (and, in production, kind PROD). */
int ipj_roster_member(const ipj_roster *r, const uint8_t pk[32], unsigned mode);
const ipj_entry *ipj_roster_find(const ipj_roster *r, const uint8_t pk[32]);

/* ---- beacon ---- */
#define IPJ_BEACON_LEN 128u
ipj_status ipj_beacon_build(const uint8_t sk[32], uint64_t roster_gen, uint64_t counter, const uint8_t ip[4],
                            uint16_t port, uint8_t out[IPJ_BEACON_LEN]);
typedef struct {
    uint8_t pk[32];
    uint64_t roster_gen, counter;
    uint8_t ip[4];
    uint16_t port;
} ipj_beacon;
/* Verifies signature and membership (does not judge freshness). */
ipj_status ipj_beacon_parse(const ipj_roster *r, unsigned mode, const uint8_t *in, size_t len, ipj_beacon *b);

/* ---- rendezvous (one IPJ_RV_LEN = 160-byte datagram per op, label
 * "AIENOS-IPJ-v1 rendezvous") ----
 *   0 "AIPV"  4 version 1  5 op  6 reserved u16 = 0  8 signer_pk[32]
 *   40 subject_pk[32]  72 counter u64  80 roster_gen u64  88 ipv4[4]
 *   92 port u16  94 reserved u16 = 0  96 signer signature over bytes 0..96.
 *   op 1 REGISTER: signer = subject = the registering peer; ipv4/port = the
 *        address it can be reached at as far as it knows (0.0.0.0 means "use
 *        the source address you saw"; port = its listening or forwarded port).
 *   op 2 LOOKUP:   signer = requester, subject = the peer it wants; ipv4/port 0.
 *   op 3 ANSWER:   signer = the gateway, subject = the peer, ipv4/port where
 *        the peer can be reached (observed source address when it registered
 *        0.0.0.0, plus its advertised port), counter = the LOOKUP counter.
 * The gateway serves only roster members and answers only for members. */
#define IPJ_RV_LEN 160u
#define IPJ_RV_REGISTER 1u
#define IPJ_RV_LOOKUP 2u
#define IPJ_RV_ANSWER 3u
typedef struct {
    uint8_t op;
    uint8_t signer[32], subject[32];
    uint64_t counter, roster_gen;
    uint8_t ip[4];
    uint16_t port;
} ipj_rv;
ipj_status ipj_rv_build(const uint8_t sk[32], const ipj_rv *v, uint8_t out[IPJ_RV_LEN]);
/* Verifies layout, signature (by v->signer) and the signer's membership. */
ipj_status ipj_rv_parse(const ipj_roster *r, unsigned mode, const uint8_t *in, size_t len, ipj_rv *v);

#define IPJ_GW_MAX 32u
typedef struct {
    struct {
        uint8_t pk[32];
        uint8_t ip[4];
        uint16_t port;
        uint64_t counter;
        int used;
    } t[IPJ_GW_MAX];
    uint64_t answer_counter;
} ipj_gateway;
/* Gateway: handle one datagram from src_ip:src_port. A valid fresh REGISTER
 * updates the table (IPJ_OK, *out_len = 0); a valid LOOKUP for a registered
 * member writes an ANSWER into out (IPJ_OK, *out_len = IPJ_RV_LEN). */
ipj_status ipj_gateway_handle(ipj_gateway *g, const ipj_roster *r, unsigned mode, const uint8_t gw_sk[32],
                              const uint8_t *in, size_t len, const uint8_t src_ip[4], uint16_t src_port,
                              uint8_t *out, size_t cap, size_t *out_len);

/* ---- signed message (inside a secure session DATA record) ----
 * App frame = 1 type byte + body. Types: IPJ_APP_MSG (body = message),
 * IPJ_APP_ROSTER (body = signed roster: the newer roster is pushed to peers).
 * Message ("AIPM", label "AIENOS-IPJ-v1 msg"):
 *   0 "AIPM"  4 version 1  5 kind  6 ttl  7 reserved 0  8 origin_pk[32]
 *   40 dest_pk[32]  72 seq u64  80 len u16  82 reserved u16 = 0
 *   84 payload[len]  then origin signature over the message with byte 6
 *   (ttl) taken as 0, so relays may lower ttl but change nothing else. */
#define IPJ_APP_MSG 1u
#define IPJ_APP_ROSTER 2u
#define IPJ_MSG_HDR 84u
#define IPJ_MSG_MAX (SEC_MAX_PAYLOAD - 1u - IPJ_MSG_HDR - IPJ_SIG_LEN)
#define IPJ_MSG_TEXT 1u          /* plain text */
#define IPJ_MSG_INTERPLANE 2u    /* an Interplane envelope (JCS JSON), never authority */
#define IPJ_MSG_MODEL_REQUEST 3u /* a model request for the destination's model service */
#define IPJ_MSG_CONSOLE 4u       /* an operator console command (owner-held keys only) */
#define IPJ_TTL_DEFAULT 4u
typedef struct {
    uint8_t kind, ttl;
    uint8_t origin[32], dest[32];
    uint64_t seq;
    uint16_t len;
    uint8_t payload[IPJ_MSG_MAX];
} ipj_msg;
ipj_status ipj_msg_build(const uint8_t sk[32], const ipj_msg *m, uint8_t *out, size_t cap, size_t *written);
/* Verifies layout, the origin signature and the origin's membership. */
ipj_status ipj_msg_parse(const ipj_roster *r, unsigned mode, const uint8_t *in, size_t len, ipj_msg *m);

/* ---- node: one UDP port, up to IPJ_PEERS_MAX secure sessions ---- */
#define IPJ_PEERS_MAX 8u
#define IPJ_ROUTES_MAX 8u
#define IPJ_SEEN_MAX IPJ_ROSTER_MAX
#define IPJ_OUTQ 8u

typedef struct {
    int used;
    uint8_t pk[32]; /* the peer machine key (initiator: pinned; responder: after HS3) */
    uint8_t ip[4];
    uint16_t port;
    sec_endpoint ep;
    int up;            /* established at least once and not closed */
} ipj_peer;

/* Entropy and output callbacks (the node has no clock or I/O of its own). */
typedef struct {
    void *ctx;
    void (*entropy)(void *ctx, uint8_t out[64]);
    /* Put one UDP datagram on the wire to ip:port (broadcast allowed). */
    void (*send)(void *ctx, const uint8_t ip[4], uint16_t port, const uint8_t *d, size_t n);
    /* Optional: a line of status text for the console/log. */
    void (*log)(void *ctx, const char *line);
} ipj_io;

typedef struct {
    uint8_t sk[32], pk[32];
    uint8_t owner_pk[32];
    unsigned mode;
    int owner_is_test;
    uint8_t ip[4];
    uint16_t port;
    ipj_roster roster;
    uint8_t roster_blob[IPJ_ROSTER_MAX_BYTES];
    size_t roster_len;
    int roster_dirty;  /* a newer roster was accepted and is not yet saved */
    ipj_peer peer[IPJ_PEERS_MAX];
    struct {
        uint8_t dest[32], via[32];
        int used;
    } route[IPJ_ROUTES_MAX];
    struct {
        uint8_t pk[32];
        uint64_t beacon_ctr, msg_seq;
        int used;
    } seen[IPJ_SEEN_MAX];
    struct {
        uint8_t frame[SEC_MAX_PAYLOAD];
        size_t len;
        uint8_t to[32];
        int used;
    } outq[IPJ_OUTQ];
    uint8_t gw_pk[32], gw_ip[4];
    uint16_t gw_port;
    int gw_set;
    uint64_t rv_ctr, lookup_ctr;
    uint64_t beacon_ctr, msg_seq;
    ipj_io io;
    sec_config base; /* rto, retries, rekey, max_records for every session */
    /* delivered messages for the owner of the node (latest only) */
    ipj_msg inbox;
    int inbox_full;
    struct {
        uint64_t hs_refused, beacon_refused, msg_refused, relayed, delivered, roster_updates,
            roster_refused, revoked_closed;
    } c;
} ipj_node;

/* blob/len: the signed roster this machine was provisioned with (accepted
 * against owner_pk and mode here; refused rosters fail init). */
ipj_status ipj_node_init(ipj_node *n, const uint8_t sk[32], const uint8_t owner_pk[32], unsigned mode,
                         int owner_is_test, const uint8_t *blob, size_t len, const uint8_t ip[4], uint16_t port,
                         const ipj_io *io, const sec_config *base);
/* Static route: messages for dest go through via (a peer we hold a session to). */
ipj_status ipj_node_route(ipj_node *n, const uint8_t dest[32], const uint8_t via[32]);
/* Ask for a session to a member at ip:port. Who initiates is fixed: the
 * lower machine key (memcmp) dials; the higher one only records the address
 * and waits, so two machines never run two sessions to each other. */
ipj_status ipj_node_connect(ipj_node *n, const uint8_t pk[32], const uint8_t ip[4], uint16_t port);
/* Broadcast one beacon to bcast_ip:port. */
ipj_status ipj_node_beacon(ipj_node *n, const uint8_t bcast_ip[4]);
/* Hand one received UDP datagram from src_ip:src_port to the node. */
ipj_status ipj_node_input(ipj_node *n, uint64_t now, const uint8_t src_ip[4], uint16_t src_port, const uint8_t *d,
                          size_t len);
/* Resend timers, queued frames and handshake flights. */
void ipj_node_poll(ipj_node *n, uint64_t now);
/* Sign and send a message from this node to dest (directly or by route). */
ipj_status ipj_node_send(ipj_node *n, uint64_t now, const uint8_t dest[32], uint8_t kind, const uint8_t *payload,
                         size_t len);
/* Accept a newer signed roster (from the owner tool or a peer): drop sessions
 * of machines that are no longer members, push it to every live peer. */
ipj_status ipj_node_roster_update(ipj_node *n, uint64_t now, const uint8_t *blob, size_t len);
/* Send an already signed message (built by ipj_msg_build, any origin) to the
 * peer via, as one frame. For hosts that carry messages they did not write;
 * every receiver re-verifies origin, membership, replay and ttl itself. */
ipj_status ipj_node_send_signed(ipj_node *n, const uint8_t via[32], const uint8_t *msg, size_t len);
/* 1 if a session to pk is established. */
int ipj_node_established(const ipj_node *n, const uint8_t pk[32]);

/* Rendezvous through the gateway machine (a roster member). */
ipj_status ipj_node_set_gateway(ipj_node *n, const uint8_t gw_pk[32], const uint8_t ip[4], uint16_t port);
/* adv_ip 0.0.0.0: "use the address you see"; adv_port: our listening or forwarded port. */
ipj_status ipj_node_register(ipj_node *n, const uint8_t adv_ip[4], uint16_t adv_port);
ipj_status ipj_node_lookup(ipj_node *n, const uint8_t target[32]);


/* ---- provisioning bundle (\EFI\AIENOS\JOIN.BIN on a QEMU or lab boot volume) ----
 * QUALIFICATION ONLY: it carries the machine secret key in the clear. The
 * production path seals the machine key under the M5 key hierarchy (TRUST-1);
 * that path is not built here.
 *   0 "AIPJ"  4 version 1  5 mode (IPJ_MODE_*)  6 owner_is_test (0/1)
 *   7 nroutes (0..IPJ_BUNDLE_ROUTES)  8 machine_sk[32]  40 owner_pk[32]
 *   72 ipv4[4]  76 broadcast ipv4[4]  80 run_seconds u16 (1..3600)
 *   82 roster_len u16  84 send_dest_pk[32] (all zero: send nothing)
 *   116 send_len u16 (<= IPJ_BUNDLE_TEXT)  118 reserved u16 = 0
 *   120 send_text[IPJ_BUNDLE_TEXT] (zero after send_len)
 *   248 routes: IPJ_BUNDLE_ROUTES x (dest_pk[32] via_pk[32]), unused = 0
 *   504 roster[roster_len]   total = 504 + roster_len exactly */
#define IPJ_BUNDLE_ROUTES 4u
#define IPJ_BUNDLE_TEXT 128u
#define IPJ_BUNDLE_HDR 504u
#define IPJ_BUNDLE_MAX (IPJ_BUNDLE_HDR + IPJ_ROSTER_MAX_BYTES)
typedef struct {
    uint8_t mode, owner_is_test, nroutes;
    uint8_t sk[32], owner_pk[32], ip[4], bcast[4];
    uint16_t run_seconds, send_len;
    uint8_t send_dest[32];
    uint8_t send_text[IPJ_BUNDLE_TEXT];
    uint8_t route_dest[IPJ_BUNDLE_ROUTES][32], route_via[IPJ_BUNDLE_ROUTES][32];
    const uint8_t *roster; /* points into the parsed buffer */
    uint16_t roster_len;
} ipj_bundle;
ipj_status ipj_bundle_build(const ipj_bundle *b, const uint8_t *roster, size_t roster_len, uint8_t *out, size_t cap,
                            size_t *written);
/* Layout only (the roster is verified later by ipj_node_init). */
ipj_status ipj_bundle_parse(const uint8_t *in, size_t len, ipj_bundle *b);

#endif
