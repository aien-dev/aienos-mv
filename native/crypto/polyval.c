/* POLYVAL per RFC 8452 Section 3, port of crates/aienos-crypto/src/polyval.rs.
 *
 * Field GF(2^128) modulo x^128 + x^127 + x^126 + x^121 + 1, little-endian
 * bit order. Constant-time: dot() always runs 128 steps; each bit of a
 * selects b through a mask (behind an optimization barrier), never a branch
 * or a secret-dependent address. On CPUs with PMULL the same products run as
 * carry-less multiply instructions (L339, ../argus/isa_arm64.h); this C
 * loop is the fallback and the reference.
 */
#include <string.h>

#include "aienos_crypto.h"
#include "crypto_internal.h"
#include "../argus/isa_arm64.h"

static inline uint64_t load_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static inline void store_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}


#ifdef AIENOS_ISA_ARM64
/* aienos_polyval_arm_blocks(s, h, x, n): n times s = (s ^ x_i) * h * x^-128
 * with PMULL. Karatsuba gives the 256-bit carry-less product X3:X2:X1:X0;
 * two folds by 0xc2 << 56 (x^-64 steps of the field modulus) reduce it,
 * then X3:X2 is added. v0 = s, v1 = h, v2 = h0 ^ h1, v3 = fold constant,
 * v4-v7 and v16-v18 = products; all are zeroed before return. */
void aienos_polyval_arm_blocks(uint8_t s[16], const uint8_t h[16], const uint8_t *x, size_t n);
__asm__(
    ".text\n"
    ".arch_extension aes\n"
    ".balign 16\n"
    ".globl aienos_polyval_arm_blocks\n"
    ".type aienos_polyval_arm_blocks, %function\n"
    "aienos_polyval_arm_blocks:\n"
    "  cbz x3, 2f\n"
    "  ld1 {v0.16b}, [x0]\n"
    "  ld1 {v1.16b}, [x1]\n"
    "  ext v2.16b, v1.16b, v1.16b, #8\n"
    "  eor v2.16b, v2.16b, v1.16b\n"
    "  movz x4, #0xc200, lsl #48\n"
    "  dup v3.2d, x4\n"
    "  movi v4.16b, #0\n"
    "1:\n"
    "  ld1 {v5.16b}, [x2], #16\n"
    "  eor v0.16b, v0.16b, v5.16b\n"
    "  pmull v5.1q, v0.1d, v1.1d\n"       /* lo = X1:X0 before the middle term */
    "  pmull2 v6.1q, v0.2d, v1.2d\n"      /* hi = X3:X2 before the middle term */
    "  ext v7.16b, v0.16b, v0.16b, #8\n"
    "  eor v7.16b, v7.16b, v0.16b\n"
    "  pmull v7.1q, v7.1d, v2.1d\n"
    "  eor v7.16b, v7.16b, v5.16b\n"
    "  eor v7.16b, v7.16b, v6.16b\n"      /* middle = (a0^a1)(h0^h1) ^ lo ^ hi */
    "  ext v16.16b, v4.16b, v7.16b, #8\n" /* 0 : mid.d0 */
    "  ext v17.16b, v7.16b, v4.16b, #8\n" /* mid.d1 : 0 */
    "  eor v5.16b, v5.16b, v16.16b\n"
    "  eor v6.16b, v6.16b, v17.16b\n"
    "  pmull v18.1q, v5.1d, v3.1d\n"
    "  ext v5.16b, v5.16b, v5.16b, #8\n"
    "  eor v5.16b, v5.16b, v18.16b\n"
    "  pmull v18.1q, v5.1d, v3.1d\n"
    "  ext v5.16b, v5.16b, v5.16b, #8\n"
    "  eor v5.16b, v5.16b, v18.16b\n"
    "  eor v0.16b, v5.16b, v6.16b\n"
    "  subs x3, x3, #1\n"
    "  b.ne 1b\n"
    "  st1 {v0.16b}, [x0]\n"
    "  movi v0.16b, #0\n  movi v1.16b, #0\n  movi v2.16b, #0\n  movi v3.16b, #0\n"
    "  movi v5.16b, #0\n  movi v6.16b, #0\n  movi v7.16b, #0\n"
    "  movi v16.16b, #0\n  movi v17.16b, #0\n  movi v18.16b, #0\n"
    "2:\n"
    "  ret\n"
    ".size aienos_polyval_arm_blocks, .-aienos_polyval_arm_blocks\n");

AIENOS_ISA_STATE(pv_isa);
#define PV_HW() ((aienos_isa_caps(&pv_isa) & AIENOS_ISA_PMULL) != 0)

static void pv_arm(uint8_t s[16], const uint8_t h[16], const uint8_t *x, size_t n)
{
    while (n) {
        size_t m = n < 4 * AIENOS_ISA_MAX_BLOCKS ? n : 4 * AIENOS_ISA_MAX_BLOCKS;
        uint64_t daif = aienos_isa_enter();
        aienos_polyval_arm_blocks(s, h, x, m);
        aienos_isa_leave(daif);
        x += 16 * m;
        n -= m;
    }
}
#endif

int aienos_polyval_isa_enable(int on)
{
#ifdef AIENOS_ISA_ARM64
    return aienos_isa_select(&pv_isa, on, AIENOS_ISA_PMULL);
#else
    (void)on;
    return 0;
#endif
}

static void dot_c(uint8_t out[16], const uint8_t a[16], const uint8_t b[16])
{
    uint64_t a0 = load_le64(a), a1 = load_le64(a + 8);
    uint64_t b0 = load_le64(b), b1 = load_le64(b + 8);
    uint64_t v0 = 0, v1 = 0;

    for (int i = 0; i < 128; i++) {
        uint64_t bit = ((i < 64 ? a0 >> i : a1 >> (i - 64))) & 1u;
        AIENOS_CT_BARRIER(bit);
        uint64_t m = 0u - bit;
        v0 ^= b0 & m;
        v1 ^= b1 & m;

        /* v = v * x^-1: shift right one bit, fold x^-1 = 0xe1 << 120 if the
         * bit shifted out was set. */
        uint64_t lsb = v0 & 1u;
        AIENOS_CT_BARRIER(lsb);
        v0 = (v0 >> 1) | (v1 << 63);
        v1 = (v1 >> 1) ^ ((UINT64_C(0xe1) << 56) & (0u - lsb));
    }
    store_le64(out, v0);
    store_le64(out + 8, v1);
    /* a0..b1 are secret register copies; nothing to wipe in memory. */
}

void aienos_polyval_dot(uint8_t out[16], const uint8_t a[16], const uint8_t b[16])
{
#ifdef AIENOS_ISA_ARM64
    if (PV_HW()) {
        static const uint8_t zero[16];
        uint8_t s[16];
        memcpy(s, a, 16);
        pv_arm(s, b, zero, 1);
        memcpy(out, s, 16);
        aienos_wipe(s, sizeof s);
        return;
    }
#endif
    dot_c(out, a, b);
}

void aienos_polyval_init(aienos_polyval *p, const uint8_t h[16])
{
    for (int i = 0; i < 16; i++) {
        p->h[i] = h[i];
        p->s[i] = 0;
    }
}

void aienos_polyval_update_block(aienos_polyval *p, const uint8_t x[16])
{
#ifdef AIENOS_ISA_ARM64
    if (PV_HW()) {
        pv_arm(p->s, p->h, x, 1);
        return;
    }
#endif
    for (int i = 0; i < 16; i++)
        p->s[i] ^= x[i];
    dot_c(p->s, p->s, p->h);
}

int aienos_polyval_update(aienos_polyval *p, const uint8_t *data, size_t len)
{
    if (len % 16 != 0 || (data == NULL && len != 0))
        return AIENOS_CRYPTO_ERR_LENGTH;
#ifdef AIENOS_ISA_ARM64
    if (PV_HW()) {
        pv_arm(p->s, p->h, data, len / 16);
        return AIENOS_CRYPTO_OK;
    }
#endif
    for (size_t off = 0; off < len; off += 16)
        aienos_polyval_update_block(p, data + off);
    return AIENOS_CRYPTO_OK;
}

void aienos_polyval_final(aienos_polyval *p, uint8_t out[16])
{
    for (int i = 0; i < 16; i++)
        out[i] = p->s[i];
    aienos_polyval_wipe(p);
}

void aienos_polyval_wipe(aienos_polyval *p)
{
    aienos_wipe(p, sizeof *p);
}
