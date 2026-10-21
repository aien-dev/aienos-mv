/* isa_probe.c -- L339 measurement: software crypto vs the ARMv8 crypto
 * instructions on the same CPU (hosted, aarch64 only). Answers the review's
 * UNKNOWN inputs for G4-os-3/4/5 before any code changes:
 *   SHA-256 (argus/sha256.c) vs SHA256H/SHA256H2/SHA256SU0/SU1
 *   AES-256 block (crypto/aes.c) vs AESE/AESMC
 *   POLYVAL dot (crypto/polyval.c) vs PMULL Karatsuba + two-fold reduction
 * Every instruction path is first checked bit for bit against the software
 * path on random inputs; a mismatch exits non-zero before any timing.
 *
 *   cc -O2 -march=armv8-a+crypto -I.. -I../../argus isa_probe.c ../aes.c \
 *      ../polyval.c ../ct.c ../../argus/sha256.c -o isa_probe
 */
#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "aienos_crypto.h"
#include "sha256.h"

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static uint64_t rng = 0x9e3779b97f4a7c15u;
static uint8_t rnd8(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint8_t)(rng >> 32);
}
static void fill(uint8_t *p, size_t n) { while (n--) *p++ = rnd8(); }

/* ---------------------------------------------------------------- SHA-256 */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256_hw_blocks(uint32_t st[8], const uint8_t *p, size_t nblk)
{
    uint32x4_t s0 = vld1q_u32(st), s1 = vld1q_u32(st + 4);
    while (nblk--) {
        uint32x4_t a = s0, b = s1, t, w[4];
        for (int i = 0; i < 4; i++)
            w[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16 * i)));
        for (int r = 0; r < 16; r++) {
            uint32x4_t wk = vaddq_u32(w[r & 3], vld1q_u32(K256 + 4 * r));
            t = a;
            a = vsha256hq_u32(a, b, wk);
            b = vsha256h2q_u32(b, t, wk);
            if (r < 12)
                w[r & 3] = vsha256su1q_u32(vsha256su0q_u32(w[r & 3], w[(r + 1) & 3]),
                                           w[(r + 2) & 3], w[(r + 3) & 3]);
        }
        s0 = vaddq_u32(s0, a);
        s1 = vaddq_u32(s1, b);
        p += 64;
    }
    vst1q_u32(st, s0);
    vst1q_u32(st + 4, s1);
}

/* --------------------------------------------------------------- AES-256 */
static void aes_hw_keys(uint8x16_t rk[15], const aienos_aes256_key *k)
{
    for (int r = 0; r < 15; r++) {
        uint8_t b[16];
        for (int c = 0; c < 4; c++) {
            uint32_t w = k->rk[4 * r + c];
            b[4 * c] = (uint8_t)(w >> 24); b[4 * c + 1] = (uint8_t)(w >> 16);
            b[4 * c + 2] = (uint8_t)(w >> 8); b[4 * c + 3] = (uint8_t)w;
        }
        rk[r] = vld1q_u8(b);
    }
}

static inline uint8x16_t aes_hw_block(const uint8x16_t rk[15], uint8x16_t s)
{
    for (int r = 0; r < 13; r++)
        s = vaesmcq_u8(vaeseq_u8(s, rk[r]));
    return veorq_u8(vaeseq_u8(s, rk[13]), rk[14]);
}

/* --------------------------------------------------------------- POLYVAL */
static inline uint8x16_t clmul(uint64_t a, uint64_t b)
{
    return vreinterpretq_u8_p128(vmull_p64((poly64_t)a, (poly64_t)b));
}

/* dot(a, b) = a * b * x^-128 mod x^128 + x^127 + x^126 + x^121 + 1. */
static uint8x16_t polyval_hw_dot(uint8x16_t a, uint8x16_t b)
{
    uint64x2_t A = vreinterpretq_u64_u8(a), B = vreinterpretq_u64_u8(b);
    uint64_t a0 = vgetq_lane_u64(A, 0), a1 = vgetq_lane_u64(A, 1);
    uint64_t b0 = vgetq_lane_u64(B, 0), b1 = vgetq_lane_u64(B, 1);
    uint8x16_t lo = clmul(a0, b0), hi = clmul(a1, b1);
    uint8x16_t mid = veorq_u8(veorq_u8(clmul(a0 ^ a1, b0 ^ b1), lo), hi);
    uint64x2_t L = vreinterpretq_u64_u8(lo), H = vreinterpretq_u64_u8(hi), M = vreinterpretq_u64_u8(mid);
    /* 256-bit product X3:X2:X1:X0 */
    uint64_t x0 = vgetq_lane_u64(L, 0), x1 = vgetq_lane_u64(L, 1) ^ vgetq_lane_u64(M, 0);
    uint64_t x2 = vgetq_lane_u64(H, 0) ^ vgetq_lane_u64(M, 1), x3 = vgetq_lane_u64(H, 1);
    const uint64_t poly = UINT64_C(0xc200000000000000);
    /* two Montgomery folds of 64 bits each */
    uint64x2_t t = vreinterpretq_u64_u8(clmul(x0, poly));
    x1 ^= vgetq_lane_u64(t, 0); /* D = swap(D) ^ T: new low = x1 ^ t0, new high = x0 ^ t1 */
    uint64_t y0 = x1, y1 = x0 ^ vgetq_lane_u64(t, 1);
    t = vreinterpretq_u64_u8(clmul(y0, poly));
    uint64_t z0 = y1 ^ vgetq_lane_u64(t, 0), z1 = y0 ^ vgetq_lane_u64(t, 1);
    uint64x2_t r = vcombine_u64(vcreate_u64(z0 ^ x2), vcreate_u64(z1 ^ x3));
    return vreinterpretq_u8_u64(r);
}

#define N_SHA (16u << 20)
static uint8_t big[N_SHA];

int main(void)
{
    int bad = 0;
    fill(big, sizeof big);

    /* SHA-256: equality on 1000 random lengths, then 16 MiB timing */
    for (int it = 0; it < 1000; it++) {
        size_t n = (size_t)(rnd8() | (rnd8() << 8)) % 4096;
        uint8_t d1[32], d2[32];
        sha256_hash(big + it, n, d1);
        sha256_ctx c;
        sha256_init(&c);
        size_t full = n / 64;
        sha256_hw_blocks(c.state, big + it, full);
        /* finish the tail with the software padding: reuse the ctx */
        c.count = (uint64_t)full * 64u;
        sha256_update(&c, big + it + full * 64, n - full * 64);
        sha256_final(&c, d2);
        if (memcmp(d1, d2, 32)) { bad = 1; break; }
    }
    printf("sha256 hw==sw on 1000 random lengths: %s\n", bad ? "MISMATCH" : "ok");
    if (bad) return 1;

    double t0 = now_s();
    uint8_t dg[32];
    for (int r = 0; r < 4; r++) sha256_hash(big, N_SHA, dg);
    double sw = (now_s() - t0) / 4;
    uint32_t st[8] = {0};
    t0 = now_s();
    for (int r = 0; r < 4; r++) sha256_hw_blocks(st, big, N_SHA / 64);
    double hw = (now_s() - t0) / 4;
    printf("SHA256 16MiB: sw %.1f MB/s  hw %.1f MB/s  ratio %.2f  (sink %02x%08x)\n",
           N_SHA / sw / 1e6, N_SHA / hw / 1e6, sw / hw, dg[0], st[0]);

    /* AES-256 */
    uint8_t key[32];
    aienos_aes256_key k;
    uint8x16_t rk[15];
    fill(key, 32);
    aienos_aes256_init(&k, key);
    aes_hw_keys(rk, &k);
    for (int it = 0; it < 10000; it++) {
        uint8_t b1[16], b2[16];
        fill(b1, 16);
        memcpy(b2, b1, 16);
        aienos_aes256_encrypt_block(&k, b1);
        vst1q_u8(b2, aes_hw_block(rk, vld1q_u8(b2)));
        if (memcmp(b1, b2, 16)) { bad = 1; break; }
    }
    printf("aes256 hw==sw on 10000 random blocks: %s\n", bad ? "MISMATCH" : "ok");
    if (bad) return 1;
    const int NA = 200000;
    uint8_t blk[16] = {0};
    t0 = now_s();
    for (int i = 0; i < NA; i++) aienos_aes256_encrypt_block(&k, blk);
    sw = (now_s() - t0) / NA;
    uint8x16_t s = vld1q_u8(blk);
    const int NH = 20000000;
    t0 = now_s();
    for (int i = 0; i < NH; i++) s = aes_hw_block(rk, s);
    hw = (now_s() - t0) / NH;
    vst1q_u8(blk, s);
    printf("AES256 block (serial chain): sw %.1f ns  hw %.2f ns  ratio %.1f  (sink %02x)\n",
           sw * 1e9, hw * 1e9, sw / hw, blk[0]);
    t0 = now_s();
    for (int i = 0; i < 100000; i++) aienos_aes256_init(&k, key);
    printf("AES256 key schedule sw: %.1f ns\n", (now_s() - t0) / 100000 * 1e9);

    /* POLYVAL */
    for (int it = 0; it < 10000; it++) {
        uint8_t a[16], b[16], o1[16], o2[16];
        fill(a, 16); fill(b, 16);
        aienos_polyval_dot(o1, a, b);
        vst1q_u8(o2, polyval_hw_dot(vld1q_u8(a), vld1q_u8(b)));
        if (memcmp(o1, o2, 16)) { bad = 1; break; }
    }
    printf("polyval hw==sw on 10000 random pairs: %s\n", bad ? "MISMATCH" : "ok");
    if (bad) return 1;
    uint8_t h[16], acc[16] = {0};
    fill(h, 16);
    const int NP = 2000000;
    t0 = now_s();
    for (int i = 0; i < NP; i++) aienos_polyval_dot(acc, acc, h);
    sw = (now_s() - t0) / NP;
    uint8x16_t va = vld1q_u8(acc), vh = vld1q_u8(h);
    const int NQ = 50000000;
    t0 = now_s();
    for (int i = 0; i < NQ; i++) va = polyval_hw_dot(va, vh);
    hw = (now_s() - t0) / NQ;
    vst1q_u8(acc, va);
    printf("POLYVAL dot (serial chain): sw %.1f ns  hw %.2f ns  ratio %.1f  (sink %02x)\n",
           sw * 1e9, hw * 1e9, sw / hw, acc[0]);
    return 0;
}
