/* Copied from aien-dev/omega src/sha256.{c,h} at ceb68d6 (in-house lineage). L339 adds the
 * ARMv8 SHA-256 instruction path (sha256_blocks, isa_arm64.h); the C transform stays the fallback. */
#include "sha256.h"
#include <string.h>

#include "isa_arm64.h"

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(sha256_ctx *ctx, const uint8_t *data) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    size_t i, j;

    for (i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) |
               ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) |
               ((uint32_t)data[j + 3]);
    }
    for (; i < 64; ++i) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}


#ifdef AIENOS_ISA_ARM64
/* aienos_sha256_arm_blocks(state, data, nblk, K): nblk >= 0 blocks with
 * SHA256H/H2/SU0/SU1; same state update as sha256_transform per block.
 * v0/v1 = state, v2/v3 = working ABCD/EFGH, v4-v7 = W, v16 = W+K, v17 = ABCD
 * before the round. All are zeroed before return (see isa_arm64.h). */
void aienos_sha256_arm_blocks(uint32_t state[8], const uint8_t *data, size_t nblk, const uint32_t *k);
__asm__(
    ".text\n"
    ".arch_extension sha2\n"
    ".macro aienos_sha_qr w0, w1, w2, w3, upd\n"
    "  ld1 {v16.4s}, [x4], #16\n"
    "  add v16.4s, v16.4s, \\w0\\().4s\n"
    "  mov v17.16b, v2.16b\n"
    "  sha256h q2, q3, v16.4s\n"
    "  sha256h2 q3, q17, v16.4s\n"
    "  .if \\upd\n"
    "  sha256su0 \\w0\\().4s, \\w1\\().4s\n"
    "  sha256su1 \\w0\\().4s, \\w2\\().4s, \\w3\\().4s\n"
    "  .endif\n"
    ".endm\n"
    ".balign 16\n"
    ".globl aienos_sha256_arm_blocks\n"
    ".type aienos_sha256_arm_blocks, %function\n"
    "aienos_sha256_arm_blocks:\n"
    "  cbz x2, 2f\n"
    "  ld1 {v0.4s, v1.4s}, [x0]\n"
    "1:\n"
    "  ld1 {v4.16b, v5.16b, v6.16b, v7.16b}, [x1], #64\n"
    "  rev32 v4.16b, v4.16b\n"
    "  rev32 v5.16b, v5.16b\n"
    "  rev32 v6.16b, v6.16b\n"
    "  rev32 v7.16b, v7.16b\n"
    "  mov v2.16b, v0.16b\n"
    "  mov v3.16b, v1.16b\n"
    "  mov x4, x3\n"
    "  aienos_sha_qr v4, v5, v6, v7, 1\n"
    "  aienos_sha_qr v5, v6, v7, v4, 1\n"
    "  aienos_sha_qr v6, v7, v4, v5, 1\n"
    "  aienos_sha_qr v7, v4, v5, v6, 1\n"
    "  aienos_sha_qr v4, v5, v6, v7, 1\n"
    "  aienos_sha_qr v5, v6, v7, v4, 1\n"
    "  aienos_sha_qr v6, v7, v4, v5, 1\n"
    "  aienos_sha_qr v7, v4, v5, v6, 1\n"
    "  aienos_sha_qr v4, v5, v6, v7, 1\n"
    "  aienos_sha_qr v5, v6, v7, v4, 1\n"
    "  aienos_sha_qr v6, v7, v4, v5, 1\n"
    "  aienos_sha_qr v7, v4, v5, v6, 1\n"
    "  aienos_sha_qr v4, v5, v6, v7, 0\n"
    "  aienos_sha_qr v5, v6, v7, v4, 0\n"
    "  aienos_sha_qr v6, v7, v4, v5, 0\n"
    "  aienos_sha_qr v7, v4, v5, v6, 0\n"
    "  add v0.4s, v0.4s, v2.4s\n"
    "  add v1.4s, v1.4s, v3.4s\n"
    "  subs x2, x2, #1\n"
    "  b.ne 1b\n"
    "  st1 {v0.4s, v1.4s}, [x0]\n"
    "  movi v0.16b, #0\n  movi v1.16b, #0\n  movi v2.16b, #0\n  movi v3.16b, #0\n"
    "  movi v4.16b, #0\n  movi v5.16b, #0\n  movi v6.16b, #0\n  movi v7.16b, #0\n"
    "  movi v16.16b, #0\n  movi v17.16b, #0\n"
    "2:\n"
    "  ret\n"
    ".size aienos_sha256_arm_blocks, .-aienos_sha256_arm_blocks\n"
    ".purgem aienos_sha_qr\n");

AIENOS_ISA_STATE(sha_isa);
#endif

int sha256_isa_enable(int on)
{
#ifdef AIENOS_ISA_ARM64
    return aienos_isa_select(&sha_isa, on, AIENOS_ISA_SHA256);
#else
    (void)on;
    return 0;
#endif
}

/* n whole blocks: the instructions when the CPU has them, else C. */
static void sha256_blocks(sha256_ctx *ctx, const uint8_t *data, size_t n)
{
#ifdef AIENOS_ISA_ARM64
    if (aienos_isa_caps(&sha_isa) & AIENOS_ISA_SHA256) {
        while (n) {
            size_t m = n < AIENOS_ISA_MAX_BLOCKS ? n : AIENOS_ISA_MAX_BLOCKS;
            uint64_t daif = aienos_isa_enter();
            aienos_sha256_arm_blocks(ctx->state, data, m, K);
            aienos_isa_leave(daif);
            data += m * SHA256_BLOCK_SIZE;
            n -= m;
        }
        return;
    }
#endif
    for (; n; n--, data += SHA256_BLOCK_SIZE)
        sha256_transform(ctx, data);
}

void sha256_init(sha256_ctx *ctx) {
    ctx->count = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len) {
    size_t buffer_index = (size_t)(ctx->count & 0x3f);
    ctx->count += len;

    if (buffer_index > 0) {
        size_t left = SHA256_BLOCK_SIZE - buffer_index;
        if (len < left) {
            memcpy(&ctx->buffer[buffer_index], data, len);
            return;
        }
        memcpy(&ctx->buffer[buffer_index], data, left);
        sha256_blocks(ctx, ctx->buffer, 1);
        data += left;
        len -= left;
    }

    if (len >= SHA256_BLOCK_SIZE) {
        size_t n = len / SHA256_BLOCK_SIZE;
        sha256_blocks(ctx, data, n);
        data += n * SHA256_BLOCK_SIZE;
        len -= n * SHA256_BLOCK_SIZE;
    }

    if (len > 0) {
        memcpy(ctx->buffer, data, len);
    }
}

void sha256_final(sha256_ctx *ctx, uint8_t digest[SHA256_DIGEST_SIZE]) {
    uint8_t count_bytes[8];
    uint64_t bit_count = ctx->count * 8;
    size_t pad_len;
    size_t buffer_index = (size_t)(ctx->count & 0x3f);
    static const uint8_t padding[64] = { 0x80 };

    for (int i = 0; i < 8; ++i) {
        count_bytes[7 - i] = (uint8_t)(bit_count >> (i * 8));
    }

    pad_len = (buffer_index < 56) ? (56 - buffer_index) : (120 - buffer_index);
    sha256_update(ctx, padding, 1);
    if (pad_len > 1) {
        static const uint8_t zeroes[64] = { 0 };
        sha256_update(ctx, zeroes, pad_len - 1);
    }
    sha256_update(ctx, count_bytes, 8);

    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void sha256_hash(const uint8_t *data, size_t len, uint8_t digest[SHA256_DIGEST_SIZE]) {
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
}
