/* fe25519.h -- GF(2^255 - 19) in five 51-bit limbs with 128-bit products,
 * shared by Ed25519 (sig/ed25519.c) and X25519 (net/x25519.c; L339 G4-os-6:
 * 25 inner multiplies per field multiply instead of 256 with 16 limbs).
 * Moved unchanged from ed25519.c. Every function is branch free and has no
 * secret-dependent memory index; fe_pow takes a PUBLIC exponent.
 */
#ifndef AIENOS_FE25519_H
#define AIENOS_FE25519_H

#include <stdint.h>

#include "sig_internal.h"

#define FE_BARRIER(v) AIENOS_SIG_BARRIER(v)

typedef unsigned __int128 u128;
typedef uint64_t fe[5];
#define MASK51 ((uint64_t)0x7ffffffffffff)

/* ---------------------------------------------------------------- field */

static inline void fe_0(fe h) { h[0] = h[1] = h[2] = h[3] = h[4] = 0; }
static inline void fe_1(fe h) { fe_0(h); h[0] = 1; }
static inline void fe_copy(fe h, const fe f) { for (int i = 0; i < 5; i++) h[i] = f[i]; }

/* Weak reduction: afterwards h1..h4 < 2^51 and h0 < 2^51 + 2^18. */
static inline void fe_carry(fe h)
{
    uint64_t c;
    c = h[0] >> 51; h[0] &= MASK51; h[1] += c;
    c = h[1] >> 51; h[1] &= MASK51; h[2] += c;
    c = h[2] >> 51; h[2] &= MASK51; h[3] += c;
    c = h[3] >> 51; h[3] &= MASK51; h[4] += c;
    c = h[4] >> 51; h[4] &= MASK51; h[0] += 19 * c;
}

static inline void fe_add(fe h, const fe f, const fe g)
{
    for (int i = 0; i < 5; i++) h[i] = f[i] + g[i];
    fe_carry(h);
}

/* h = f - g computed as f + 4p - g (g limbs are below 2^53 - 76). */
static inline void fe_sub(fe h, const fe f, const fe g)
{
    h[0] = f[0] + 0x1fffffffffffb4 - g[0];
    h[1] = f[1] + 0x1ffffffffffffc - g[1];
    h[2] = f[2] + 0x1ffffffffffffc - g[2];
    h[3] = f[3] + 0x1ffffffffffffc - g[3];
    h[4] = f[4] + 0x1ffffffffffffc - g[4];
    fe_carry(h);
}

static inline void fe_neg(fe h, const fe f)
{
    fe z;
    fe_0(z);
    fe_sub(h, z, f);
}

static inline void fe_mul(fe h, const fe f, const fe g)
{
    uint64_t f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    uint64_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
    uint64_t g1_19 = 19 * g1, g2_19 = 19 * g2, g3_19 = 19 * g3, g4_19 = 19 * g4;
    u128 r0 = (u128)f0 * g0 + (u128)f1 * g4_19 + (u128)f2 * g3_19 + (u128)f3 * g2_19 + (u128)f4 * g1_19;
    u128 r1 = (u128)f0 * g1 + (u128)f1 * g0 + (u128)f2 * g4_19 + (u128)f3 * g3_19 + (u128)f4 * g2_19;
    u128 r2 = (u128)f0 * g2 + (u128)f1 * g1 + (u128)f2 * g0 + (u128)f3 * g4_19 + (u128)f4 * g3_19;
    u128 r3 = (u128)f0 * g3 + (u128)f1 * g2 + (u128)f2 * g1 + (u128)f3 * g0 + (u128)f4 * g4_19;
    u128 r4 = (u128)f0 * g4 + (u128)f1 * g3 + (u128)f2 * g2 + (u128)f3 * g1 + (u128)f4 * g0;
    r1 += r0 >> 51;
    r2 += r1 >> 51;
    r3 += r2 >> 51;
    r4 += r3 >> 51;
    u128 c = r4 >> 51;
    uint64_t h0 = (uint64_t)r0 & MASK51, h1 = (uint64_t)r1 & MASK51, h2 = (uint64_t)r2 & MASK51;
    uint64_t h3 = (uint64_t)r3 & MASK51, h4 = (uint64_t)r4 & MASK51;
    u128 t = (u128)h0 + c * 19;
    h0 = (uint64_t)t & MASK51;
    h1 += (uint64_t)(t >> 51);
    h[0] = h0; h[1] = h1; h[2] = h2; h[3] = h3; h[4] = h4;
}

static inline void fe_sq(fe h, const fe f) { fe_mul(h, f, f); }

static inline uint64_t load_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static inline void store_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

/* Reads 255 bits (bit 255 ignored); the value may be >= p. */
static inline void fe_frombytes(fe h, const uint8_t s[32])
{
    h[0] = load_le64(s) & MASK51;
    h[1] = (load_le64(s + 6) >> 3) & MASK51;
    h[2] = (load_le64(s + 12) >> 6) & MASK51;
    h[3] = (load_le64(s + 19) >> 1) & MASK51;
    h[4] = (load_le64(s + 24) >> 12) & MASK51;
}

/* Canonical encoding (fully reduced mod p), branch free. */
static inline void fe_tobytes(uint8_t s[32], const fe f)
{
    fe t;
    fe_copy(t, f);
    fe_carry(t);
    fe_carry(t); /* now t < 2^255 + 19 < 2p */
    uint64_t q = (t[0] + 19) >> 51;
    q = (t[1] + q) >> 51;
    q = (t[2] + q) >> 51;
    q = (t[3] + q) >> 51;
    q = (t[4] + q) >> 51; /* q = 1 iff t >= p */
    t[0] += 19 * q;
    t[1] += t[0] >> 51; t[0] &= MASK51;
    t[2] += t[1] >> 51; t[1] &= MASK51;
    t[3] += t[2] >> 51; t[2] &= MASK51;
    t[4] += t[3] >> 51; t[3] &= MASK51;
    t[4] &= MASK51; /* drop q * 2^255 */
    store_le64(s, t[0] | (t[1] << 51));
    store_le64(s + 8, (t[1] >> 13) | (t[2] << 38));
    store_le64(s + 16, (t[2] >> 26) | (t[3] << 25));
    store_le64(s + 24, (t[3] >> 39) | (t[4] << 12));
}

/* f = g if b == 1, unchanged if b == 0; b must be 0 or 1. */
static inline void fe_cmov(fe f, const fe g, uint64_t b)
{
    uint64_t m = 0 - b;
    AIENOS_SIG_BARRIER(m);
    for (int i = 0; i < 5; i++)
        f[i] ^= m & (f[i] ^ g[i]);
}

/* out = z^e, e a PUBLIC little-endian 256-bit exponent. */
static inline void fe_pow(fe out, const fe z, const uint8_t e[32])
{
    fe r, b;
    fe_1(r);
    fe_copy(b, z);
    for (int i = 255; i >= 0; i--) {
        fe_sq(r, r);
        if ((e[i >> 3] >> (i & 7)) & 1)
            fe_mul(r, r, b);
    }
    fe_copy(out, r);
}

/* p - 2 = 2^255 - 21 */
static const uint8_t EXP_INV[32] = {
    0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};

static inline void fe_invert(fe out, const fe z) { fe_pow(out, z, EXP_INV); }

#endif
