/* bench_x86.c -- AIENOS GPU bench console (LAB BUILD ONLY, CK_X86_BENCH=1).
 *
 * Authenticated GPU register RPC over UDP port 47035. The Spark sends bench
 * commands (ck_gpu_bench tool); the kernel executes them against the AMD
 * Radeon Pro 555/560 (PCI 01:00.0, BAR5 = 256 KiB register aperture, BAR0 =
 * VRAM aperture). HMAC-SHA256 with a 32-byte key bound into the signed image
 * at build time; monotone 64-bit counter replay protection.
 *
 * Safety invariants enforced here:
 *   - BAR5 writes restricted to the panel/DCE allowlist (bench_proto.h).
 *   - Any read returning 0xFFFFFFFF is "device gone": batch stops.
 *   - Every write logs the value before the write.
 *   - All waits are TSC-bounded.
 *
 * QEMU note: in QEMU there is no AMD GPU (BAR0/BAR5 never map). The bench
 * console detects this (ck_x86_bench_init returns with no device) and the
 * QEMU bench test exercises the protocol layer against QEMU's bochs VGA BAR
 * (PCI 1234:1111, class 0x030000) at a compile-time override address.
 *
 * This file qualifies nothing physical. */
#include "../core/ck_internal.h"
#include "../../native/argus/sha256.h"
#include "x86.h"
#include "bench_proto.h"
#include "report_x86.h"

#ifndef CK_X86_BENCH
#define CK_X86_BENCH 0
#endif

#if CK_X86_BENCH

/* HMAC-SHA256 key bound into the signed image. */
#ifndef CK_X86_BENCH_KEY
#define CK_X86_BENCH_KEY BENCH_TEST_KEY_INIT
#endif
static const uint8_t bench_key[32] = { CK_X86_BENCH_KEY };

/* GPU device state populated by ck_x86_bench_init. */
static volatile uint32_t *bar5;          /* 256 KiB register aperture */
static volatile uint32_t *bar0;          /* VRAM aperture (first 256 KiB) */
static volatile uint8_t  *pci_cfg;       /* PCI config space (ECAM) */
static int bench_dev_ready;

/* Monotone counter: reject any request with counter <= last_counter. */
static uint64_t last_counter;

/* ---- Inline HMAC-SHA256 (uses argus/sha256.h, already in the build) ---- */
static void bench_hmac(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t out[32])
{
    uint8_t ipad[64], opad[64], inner[32];
    for (unsigned i = 0; i < 32; i++) {
        ipad[i] = key[i] ^ 0x36u;
        opad[i] = key[i] ^ 0x5cu;
    }
    for (unsigned i = 32; i < 64; i++) {
        ipad[i] = 0x36u;
        opad[i] = 0x5cu;
    }
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, ipad, 64);
    sha256_update(&c, msg, len);
    sha256_final(&c, inner);
    sha256_init(&c);
    sha256_update(&c, opad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}

/* Constant-time 32-byte compare (avoid timing oracle on HMAC check). */
static int ct_eq32(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    for (unsigned i = 0; i < 32; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

/* ---- Register allowlist ---- */
static int bar5_write_allowed(uint32_t reg_idx)
{
    return (reg_idx >= BENCH_BAR5_WRITE_LO0 && reg_idx <= BENCH_BAR5_WRITE_HI0) ||
           (reg_idx >= BENCH_BAR5_WRITE_LO1 && reg_idx <= BENCH_BAR5_WRITE_HI1);
}


/* ---- Command execution ---- */
static uint8_t exec_cmd(const uint8_t *req, uint32_t *result_out, uint32_t *before_out,
                         uint64_t tsc_hz)
{
    uint8_t cmd   = req[5];
    uint32_t arg0 = bench_r32(req + 16);
    uint32_t arg1 = bench_r32(req + 20);
    uint32_t arg2 = bench_r32(req + 24);
    uint32_t arg3 = bench_r32(req + 28);
    *result_out = 0;
    *before_out = 0;

    if (!bench_dev_ready && cmd != BENCH_CMD_PING)
        return BENCH_ERR_NODEV;

    switch (cmd) {
    case BENCH_CMD_PING:
        return BENCH_OK;

    case BENCH_CMD_PCI_CFG: {
        if (arg0 + 4 > 256) return BENCH_ERR_RANGE;
        uint32_t v = *(volatile uint32_t *)(pci_cfg + arg0);
        if (v == BENCH_ALLONES) return BENCH_ERR_ALLONES;
        *result_out = v;
        return BENCH_OK;
    }
    case BENCH_CMD_BAR5_READ: {
        if (arg0 > BENCH_BAR5_READ_MAX) return BENCH_ERR_RANGE;
        uint32_t v = bar5[arg0];
        if (v == BENCH_ALLONES) return BENCH_ERR_ALLONES;
        *result_out = v;
        return BENCH_OK;
    }
    case BENCH_CMD_BAR5_WRITE: {
        if (!bar5_write_allowed(arg0)) return BENCH_ERR_RANGE;
        uint32_t before = bar5[arg0];
        if (before == BENCH_ALLONES) return BENCH_ERR_ALLONES;
        bar5[arg0] = arg1;
        uint32_t after = bar5[arg0];
        ck_printf("bench: bar5_write reg=0x%x 0x%08x -> 0x%08x\n", arg0, before, after);
        *before_out = before;
        *result_out = after;
        return BENCH_OK;
    }
    case BENCH_CMD_BAR0_READ: {
        if (arg0 >= BENCH_BAR0_MAX_WORDS) return BENCH_ERR_RANGE;
        uint32_t v = bar0[arg0];
        if (v == BENCH_ALLONES) return BENCH_ERR_ALLONES;
        *result_out = v;
        return BENCH_OK;
    }
    case BENCH_CMD_BAR0_WRITE: {
        if (arg0 >= BENCH_BAR0_MAX_WORDS) return BENCH_ERR_RANGE;
        uint32_t before = bar0[arg0];
        if (before == BENCH_ALLONES) return BENCH_ERR_ALLONES;
        bar0[arg0] = arg1;
        ck_printf("bench: bar0_write off=0x%x 0x%08x -> 0x%08x\n", arg0, before, bar0[arg0]);
        *before_out = before;
        *result_out = bar0[arg0];
        return BENCH_OK;
    }
    case BENCH_CMD_BAR5_POLL: {
        if (arg0 > BENCH_BAR5_READ_MAX) return BENCH_ERR_RANGE;
        uint64_t t0 = ck_rdtsc(), limit = t0 + (uint64_t)tsc_hz / 1000 * arg3;
        uint32_t v;
        do {
            v = bar5[arg0];
            if (v == BENCH_ALLONES) return BENCH_ERR_ALLONES;
            if ((v & arg1) == (arg2 & arg1)) { *result_out = v; return BENCH_OK; }
            ck_pause();
        } while (ck_rdtsc() < limit);
        *result_out = v;
        return BENCH_ERR_TIMEOUT;
    }
    case BENCH_CMD_BAR5_RMW: {
        if (!bar5_write_allowed(arg0)) return BENCH_ERR_RANGE;
        uint32_t before = bar5[arg0];
        if (before == BENCH_ALLONES) return BENCH_ERR_ALLONES;
        uint32_t newval = (before & ~arg1) | (arg2 & arg1);
        bar5[arg0] = newval;
        uint32_t after = bar5[arg0];
        ck_printf("bench: bar5_rmw reg=0x%x 0x%08x -> 0x%08x\n", arg0, before, after);
        *before_out = before;
        *result_out = after;
        (void)arg3;
        return BENCH_OK;
    }
    default:
        return BENCH_ERR_CMD;
    }
}

/* ---- Protocol: receive a request and send a response ---- */
/* Builds one response datagram into out[BENCH_RESP_BYTES]. Returns byte count. */
static size_t build_resp(uint8_t status, uint64_t counter, uint64_t tsc,
                          uint32_t result, uint32_t before, uint8_t out[BENCH_RESP_BYTES])
{
    memset(out, 0, BENCH_RESP_BYTES);
    memcpy(out, BENCH_MAGIC_RESP, 4);
    out[4] = BENCH_VERSION;
    out[5] = status;
    bench_w64(out + 8,  counter);
    bench_w64(out + 16, tsc);
    bench_w32(out + 24, result);
    bench_w32(out + 28, before);
    bench_hmac(bench_key, out, BENCH_HMAC_OFF, out + BENCH_HMAC_OFF);
    return BENCH_RESP_BYTES;
}

static void handle_bench_packet(const uint8_t *pkt, size_t pkt_len,
                                  const uint8_t src_mac[6], uint64_t tsc_hz)
{
    static uint8_t resp[2048]; /* Ethernet + IP + UDP + bench */
    if (pkt_len < BENCH_REQ_BYTES) return;
    if (memcmp(pkt, BENCH_MAGIC_REQ, 4) != 0) return;
    if (pkt[4] != BENCH_VERSION) return;

    /* Verify HMAC (covers bytes 0..31, hmac is at 32..63). */
    uint8_t expected[32];
    bench_hmac(bench_key, pkt, BENCH_HMAC_OFF, expected);
    if (!ct_eq32(expected, pkt + BENCH_HMAC_OFF)) {
        /* Send auth error (no HMAC on error for auth failures). */
        uint8_t body[BENCH_RESP_BYTES];
        size_t blen = build_resp(BENCH_ERR_AUTH, 0, ck_rdtsc(), 0, 0, body);
        ck_x86_udp_send(src_mac, body, blen, CK_X86_BENCH_PORT, CK_X86_BENCH_PORT);
        return;
    }

    uint64_t counter = bench_r64(pkt + 8);
    if (counter <= last_counter) {
        uint8_t body[BENCH_RESP_BYTES];
        size_t blen = build_resp(BENCH_ERR_AUTH, counter, ck_rdtsc(), 0, 0, body);
        ck_x86_udp_send(src_mac, body, blen, CK_X86_BENCH_PORT, CK_X86_BENCH_PORT);
        return;
    }
    last_counter = counter;

    uint32_t result = 0, before = 0;
    uint8_t status = exec_cmd(pkt, &result, &before, tsc_hz);

    uint8_t body[BENCH_RESP_BYTES];
    size_t blen = build_resp(status, counter, ck_rdtsc(), result, before, body);
    ck_x86_udp_send(src_mac, body, blen, CK_X86_BENCH_PORT, CK_X86_BENCH_PORT);
    (void)resp;
}

/* ---- Public API ---- */

void ck_x86_bench_init(volatile uint8_t *gpu_cfg, volatile uint32_t *gpu_bar5,
                        volatile uint32_t *gpu_bar0)
{
    if (!gpu_cfg || !gpu_bar5 || !gpu_bar0) {
        ck_puts("bench: GPU not found (no BAR5/BAR0); bench console disabled\n");
        return;
    }
    pci_cfg = gpu_cfg;
    bar5    = gpu_bar5;
    bar0    = gpu_bar0;
    bench_dev_ready = 1;
    ck_printf("bench: GPU bench console ready port=%u (HMAC-SHA256, key bound in image)\n",
              (unsigned)CK_X86_BENCH_PORT);
}

void ck_x86_bench_step(uint64_t tsc_hz)
{
    /* Poll for a received bench packet from tg3 (LAB BUILD). */
    static uint8_t pkt[2048];
    size_t pkt_len = 0;
    uint8_t src_mac[6];
    /* ck_x86_tg3_recv_udp: fills pkt, pkt_len with the UDP payload (no IP/UDP
     * headers) and src_mac when a bench packet arrives on port 47035. */
    if (ck_x86_tg3_recv_bench(pkt, sizeof pkt, &pkt_len, src_mac) == 0 && pkt_len)
        handle_bench_packet(pkt, pkt_len, src_mac, tsc_hz);
}

#else  /* !CK_X86_BENCH */
void ck_x86_bench_init(volatile uint8_t *cfg, volatile uint32_t *b5, volatile uint32_t *b0)
{ (void)cfg; (void)b5; (void)b0; }
void ck_x86_bench_step(uint64_t tsc_hz) { (void)tsc_hz; }
#endif /* CK_X86_BENCH */
