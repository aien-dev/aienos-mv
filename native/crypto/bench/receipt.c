/* receipt.c -- L339 before/after timing through the public APIs only, so the
 * same file builds against origin/main and against this branch (receipt.sh).
 * Prints one line per primitive (best of 5 rounds) and a digest of every
 * output; the before and after digests must be equal. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "aienos_crypto.h"
#include "aienos_sig.h"
#include "sha256.h"
#include "x25519.h"

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static uint8_t big[1 << 22], out[(1 << 22) + 16];
static sha256_ctx dig;

#define BEST(label, unit, scale, reps, body)                                   \
    do {                                                                       \
        double best = 1e30;                                                    \
        for (int r_ = 0; r_ < 5; r_++) {                                       \
            double t0_ = now();                                                \
            for (int i_ = 0; i_ < (reps); i_++) { body; }                      \
            double d_ = (now() - t0_) / (reps);                                \
            if (d_ < best) best = d_;                                          \
        }                                                                      \
        printf("%-26s %12.3f %s\n", label, best * (scale), unit);              \
    } while (0)

int main(void)
{
    uint8_t key[32], nonce[12] = {1}, blk[16] = {2}, h[16] = {3}, d[32], sk[32], pk[32], sig[64];
    uint64_t r = 0x9e3779b97f4a7c15u;
    for (size_t i = 0; i < sizeof big; i++) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; big[i] = (uint8_t)(r >> 24); }
    memcpy(key, big, 32);
    memcpy(sk, big + 32, 32);
    sha256_init(&dig);
    aienos_aes256_key k;

    BEST("sha256 4 MiB", "us", 1e6, 4, sha256_hash(big, sizeof big, d));
    sha256_update(&dig, d, 32);
    BEST("aes256 key schedule", "ns", 1e9, 20000, aienos_aes256_init(&k, key));
    BEST("aes256 block", "ns", 1e9, 20000, aienos_aes256_encrypt_block(&k, blk));
    sha256_update(&dig, blk, 16);
    BEST("polyval dot", "ns", 1e9, 20000, aienos_polyval_dot(h, h, blk));
    sha256_update(&dig, h, 16);
    BEST("gcm-siv seal 64 B", "us", 1e6, 2000, aienos_gcmsiv_seal(key, nonce, NULL, 0, big, 64, out, 80));
    sha256_update(&dig, out, 80);
    BEST("gcm-siv seal 4 KiB", "us", 1e6, 200, aienos_gcmsiv_seal(key, nonce, big, 32, big, 4096, out, 4112));
    sha256_update(&dig, out, 4112);
    BEST("gcm-siv open 4 KiB", "us", 1e6, 200, (void)aienos_gcmsiv_open(key, nonce, big, 32, out, 4112, out + 8192, 4096));
    sha256_update(&dig, out + 8192, 4096);
    BEST("gcm-siv seal 4 MiB", "ms", 1e3, 1, aienos_gcmsiv_seal(key, nonce, NULL, 0, big, sizeof big, out, sizeof out));
    sha256_update(&dig, out, sizeof out);
    BEST("x25519", "us", 1e6, 200, (void)aienos_x25519(d, sk, big + 64));
    sha256_update(&dig, d, 32);
    aienos_ed25519_public_key(pk, sk);
    BEST("ed25519 sign 64 B", "us", 1e6, 200, aienos_ed25519_sign(sig, big, 64, sk));
    sha256_update(&dig, sig, 64);
    int ok = 1;
    BEST("ed25519 verify 64 B", "us", 1e6, 200, ok &= aienos_ed25519_verify(sig, big, 64, pk) == AIENOS_SIG_OK);
    sig[3] ^= 1;
    int bad = aienos_ed25519_verify(sig, big, 64, pk) != AIENOS_SIG_OK;
    sha256_final(&dig, d);
    printf("verify good=%d forged-rejected=%d\noutput digest ", ok, bad);
    for (int i = 0; i < 32; i++) printf("%02x", d[i]);
    printf("\n");
    return ok && bad ? 0 : 1;
}
