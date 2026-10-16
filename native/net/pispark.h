/* AIENOS Pi <-> Spark session test protocol (aienos-mv#398 item 3/4), in C. No heap, clock or I/O.
 *
 * One authenticated session is the existing stack, used as is:
 *   UDP datagram (IPv4, static addresses)
 *     carries one M6-B record or handshake message (aienos_sec.h, mutual Ed25519 identities, AES-256-GCM-SIV)
 *       an M6-B DATA record carries one M6-A frame (aienos_ctl.h: ordered, exactly once, retransmit, ACK)
 *         an M6-A message carries one application message (here: the probe request/reply below; PST1 later).
 * The Pi is always the initiator (role 0), the Spark the responder (role 1). TEST identities only
 * (sec_test_identity / ct_test_identity from the public labels below, NOT-FOR-PRODUCTION).
 *
 * The UDP destination port on the Spark picks the responder's behaviour, so every negative control is chosen
 * by the Pi, in the open, and never by a field inside the authenticated payload:
 *   PSS_PORT_BASE + 0  NORMAL      correct identity, answers every request
 *   PSS_PORT_BASE + 1  WRONG_PEER  answers the handshake with an identity the Pi was not told to accept
 *   PSS_PORT_BASE + 2  REPLAY      before the reply to request 1 resends its previous record byte for byte
 *   PSS_PORT_BASE + 3  STALE       before the reply to request 0 sends the last record of an EARLIER session
 *   PSS_PORT_BASE + 4  TRUNC       before the reply to request 0 sends that same record cut short by 7 bytes
 *   PSS_PORT_BASE + 5  SILENT      answers request 0, then goes silent (the "disconnect")
 *
 * Probe messages (big-endian), one per M6-A message:
 *   request (Pi -> Spark), 44..CT_MAX_PAYLOAD bytes:
 *     0 "PSQ1"  4 u32 index  8 u16 total length  10 u16 0  12 nonce[32] (fresh, from the Pi's RNG after boot)
 *     44.. filler, byte k = nonce[k % 32] ^ (k & 0xff)
 *   reply (Spark -> Pi), 60 bytes:
 *     0 "PSR1"  4 u32 index  8 SHA-256(request bytes as the Spark received them)[32]
 *     40 spark_nonce[16] (fresh, Spark RNG)  56 u16 request length  58 u16 0
 * The Pi accepts a reply only if index, length and the hash equal its own SHA-256 of what it sent: the same
 * payload bytes hashed on both machines. */
#ifndef AIENOS_PISPARK_H
#define AIENOS_PISPARK_H

#include <stddef.h>
#include <stdint.h>

#include "aienos_ctl.h"
#include "aienos_sec.h"

#define PSS_PORT_BASE 47100u
#define PSS_PI_PORT 47099u
#define PSS_LABEL_PI "aienos-pispark-pi5-TEST-v1"
#define PSS_LABEL_SPARK "aienos-pispark-spark-TEST-v1"
#define PSS_LABEL_WRONG "aienos-pispark-wrong-peer-TEST-v1"
#define PSS_LABEL_CTL "aienos-pispark-ctl-TEST-v1"

enum { PSS_NORMAL = 0, PSS_WRONG_PEER = 1, PSS_REPLAY = 2, PSS_STALE = 3, PSS_TRUNC = 4, PSS_SILENT = 5, PSS_NSCN = 6 };
#define PSS_TRUNC_CUT 7u

#define PSS_REQ_MIN 44u
#define PSS_REP_LEN 60u
#define PSS_MAX_MSGS 32u

/* status codes of this layer (negative = refused / failed) */
enum {
    PSS_OK = 0,
    PSS_E_ARG = -1,
    PSS_E_LEN = -2,
    PSS_E_MAGIC = -3,
    PSS_E_INDEX = -4,
    PSS_E_HASH = -5,
    PSS_E_RESERVED = -6,
};

const char *pss_scenario_name(int scn);
/* Build request `index` of `len` bytes (PSS_REQ_MIN..CT_MAX_PAYLOAD) around `nonce`. Returns len or 0. */
size_t pss_request_build(uint32_t index, const uint8_t nonce[32], size_t len, uint8_t *out, size_t cap);
/* Parse a request: PSS_OK and *index, or a refusal. */
int pss_request_parse(const uint8_t *m, size_t n, uint32_t *index);
size_t pss_reply_build(uint32_t index, const uint8_t req_hash[32], const uint8_t spark_nonce[16], uint16_t req_len,
                       uint8_t *out, size_t cap);
/* Check a reply against what we sent: index, request length and SHA-256 must match. */
int pss_reply_check(const uint8_t *m, size_t n, uint32_t index, const uint8_t req_hash[32], uint16_t req_len);

/* ============ responder (the Spark side; also the in-process fake in the host test) ============ */
#define PSS_RESP_MAXOUT 4u
#define PSS_EV_MAX 8u
enum {
    PSS_EV_HS1_NEW = 1,     /* a new HS1: fresh endpoint created */
    PSS_EV_SEC = 2,         /* sec_receive returned code (non-OK, non-DATA) */
    PSS_EV_ESTABLISHED = 3, /* responder installed keys (valid HS3) */
    PSS_EV_REQUEST = 4,     /* request delivered: index, len, hash */
    PSS_EV_INJECT = 5,      /* a negative-control datagram sent: code = scenario */
    PSS_EV_CTL = 6,         /* ct_receive returned code (refusal) */
    PSS_EV_CLOSED = 7,      /* authenticated CLOSE from the Pi */
    PSS_EV_SILENT_DROP = 8, /* SILENT: a datagram received and deliberately not answered */
    PSS_EV_BAD_REQ = 9,     /* a delivered message that is not a valid request: code */
};
typedef struct {
    int kind, code;
    uint32_t index, len;
    uint8_t hash[32];
} pss_event;

typedef struct {
    uint8_t buf[PSS_RESP_MAXOUT][SEC_MAX_DATAGRAM];
    size_t len[PSS_RESP_MAXOUT];
    uint32_t n;
    pss_event ev[PSS_EV_MAX];
    uint32_t nev;
} pss_out;

/* The last record a previous session sent, for the STALE control (shared by all responders of one peer process). */
typedef struct {
    uint8_t rec[SEC_MAX_DATAGRAM];
    size_t len;
} pss_stale;

typedef struct {
    int scenario;
    int active;            /* an endpoint exists */
    int ct_ready;
    sec_endpoint sec;
    ct_endpoint ct;
    uint8_t last_rec[SEC_MAX_DATAGRAM]; /* the last DATA record we sent (REPLAY / stale source) */
    size_t last_rec_len;
    uint32_t replies, requests;
    int silent;            /* SILENT has gone quiet */
    int injected;
    pss_stale *stale;
    uint32_t rto, max_retries;
} pss_resp;

/* scenario = PSS_*; stale = shared store (may be NULL: then STALE injects nothing and says so in an event). */
int pss_resp_init(pss_resp *r, int scenario, pss_stale *stale, uint32_t rto, uint32_t max_retries);
/* One received UDP payload. entropy = 80 fresh random bytes (64 for a new endpoint, 16 for the reply nonce). */
int pss_resp_input(pss_resp *r, uint64_t now, const uint8_t *in, size_t n, const uint8_t entropy[80], pss_out *o);
/* Timers (M6-A retransmits, M6-B flights). */
int pss_resp_poll(pss_resp *r, uint64_t now, pss_out *o);

/* ============ initiator (the Pi side) ============ */
typedef struct {
    /* Send one UDP datagram (n > 0) to Spark port `port`, then listen up to wait_ms; hand every datagram that came
     * from that port to on_rx in arrival order until on_rx returns non-zero or the time is up. n == 0: only wait
     * (an implementation may just sleep; anything that arrives meanwhile may be lost). < 0 = transport failure. */
    int (*xchg)(void *io, uint16_t port, const uint8_t *d, size_t n, uint32_t wait_ms,
                int (*on_rx)(void *rx, const uint8_t *d, size_t n), void *rx);
    uint64_t (*now_ms)(void *io);
    int (*rand)(void *io, void *buf, size_t n); /* 0 = ok; anything else fails the run closed */
    uint64_t (*now_us)(void *io);               /* optional, for RTT; NULL = use now_ms * 1000 */
    void *io;
} pss_io;

typedef struct {
    /* in */
    int scenario;
    const char *pi_label;     /* PSS_LABEL_PI, or another label for the wrong-key control */
    uint32_t nmsg;            /* requests to send, <= PSS_MAX_MSGS */
    uint16_t msg_len[PSS_MAX_MSGS];
    uint32_t rto_ms, max_retries, budget_ms;
    /* out */
    int hs_status;            /* the first refusal seen during the handshake (SEC_OK if none) */
    int established, confirmed, link_down, closed_sent;
    uint32_t sent, delivered, verified;
    int first_reply_err;      /* first pss_reply_check refusal (PSS_OK if none) */
    uint32_t sec_refusals[16];/* count by -sec_status (1..14) seen from sec_receive */
    uint32_t ctl_refusals[12];/* count by -ct_status */
    uint8_t req_hash[PSS_MAX_MSGS][32];
    uint8_t req_nonce[PSS_MAX_MSGS][32]; /* the fresh nonce inside each request (from io->rand) */
    uint8_t spark_nonce[PSS_MAX_MSGS][16];
    uint64_t rtt_us[PSS_MAX_MSGS];
    uint64_t hs_us, total_us;
    uint8_t session_id[8];
    uint32_t xchgs, datagrams_rx;
    sec_counters sc;
    ct_counters cc;
    int result;               /* PSS_OK, or the reason the run stopped */
} pss_run;

/* Run one session from handshake to CLOSE (or to the expected refusal). Uses static scratch: not reentrant. */
int pss_initiator_run(pss_run *r, const pss_io *io);

#endif
