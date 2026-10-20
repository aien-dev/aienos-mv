/* TEST DOUBLE, NOT CRYPTOGRAPHY. Never link into the kernel.
 *
 * Stand-ins for the two primitives native/ lacks (EAPOL MIC, AES-128 Key
 * Wrap) so the host suite can drive the WPA state machine, the chip model's
 * access point and the key install path end to end. The "MIC" is the first
 * 16 bytes of HMAC-SHA256 under the KCK zero-padded to 32 bytes; the "wrap"
 * XORs with an HMAC-SHA256 keystream and puts eight 0xA6 bytes in front
 * (checked on unwrap). Neither is a standard construction and neither is
 * secure: they only have the right shapes and fail when bytes are changed.
 * The PTK derivation is the real one (wpa_kdf_sha256). */
#ifndef AIENOS_WPA_TOY_H
#define AIENOS_WPA_TOY_H

#include <string.h>

#include "../aienos_wpa.h"
#include "../../crypto/aienos_crypto.h"

typedef struct {
    uint32_t counter; /* deterministic "random" */
    uint8_t salt;
} toy_ctx;

static inline int toy_random(void *ctx, uint8_t *out, uint32_t n) {
    toy_ctx *t = ctx;
    for (uint32_t i = 0; i < n; i++) out[i] = (uint8_t)(t->salt ^ (t->counter * 131u + i * 7u));
    t->counter++;
    return 0;
}

static inline int toy_mic(void *ctx, const uint8_t kck[16], const uint8_t *frame, uint32_t len, uint8_t mic[16]) {
    uint8_t k[32] = {0}, h[32];
    (void)ctx;
    memcpy(k, kck, 16);
    aienos_hmac_sha256(k, frame, len, h);
    memcpy(mic, h, 16);
    return 0;
}

static inline void toy_stream(const uint8_t kek[16], uint8_t *buf, uint32_t n) {
    uint8_t k[32] = {0}, h[32], c[4];
    memcpy(k, kek, 16);
    for (uint32_t blk = 0; blk * 32u < n; blk++) {
        c[0] = (uint8_t)blk; c[1] = (uint8_t)(blk >> 8); c[2] = 0x5a; c[3] = 0xa5;
        aienos_hmac_sha256(k, c, 4, h);
        for (uint32_t i = 0; i < 32u && blk * 32u + i < n; i++) buf[blk * 32u + i] ^= h[i];
    }
}

static inline uint32_t toy_wrap(const uint8_t kek[16], const uint8_t *in, uint32_t n, uint8_t *out) {
    memset(out, 0xa6, 8);
    memcpy(out + 8, in, n);
    toy_stream(kek, out, n + 8u);
    return n + 8u;
}

static inline int toy_unwrap(void *ctx, const uint8_t kek[16], const uint8_t *in, uint32_t in_len, uint8_t *out,
                             uint32_t *out_len) {
    uint8_t tmp[WPA_KEYDATA_MAX + 8];
    (void)ctx;
    if (in_len < 16 || in_len > sizeof tmp) return -1;
    memcpy(tmp, in, in_len);
    toy_stream(kek, tmp, in_len);
    for (int i = 0; i < 8; i++)
        if (tmp[i] != 0xa6) return -1;
    memcpy(out, tmp + 8, in_len - 8u);
    *out_len = in_len - 8u;
    return 0;
}

/* The native table plus the two doubles. */
static inline wpa_crypto toy_crypto(wifi_sec sec, toy_ctx *t) {
    wpa_crypto c = wpa_crypto_native(sec, toy_random, t);
    c.mic = toy_mic;
    c.unwrap = toy_unwrap;
    return c;
}

#endif
