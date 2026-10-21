/* AIENOS X25519 (RFC 7748). See x25519.h.
 *
 * Field GF(2^255 - 19) in five 51-bit limbs (../sig/fe25519.h, shared with
 * Ed25519): 25 inner multiplies per field multiply (L339 G4-os-6; the
 * previous 16-limb TweetNaCl field did 256). The ladder is RFC 7748
 * Section 5 with a masked swap: no branch and no memory index depends on
 * the scalar. fe_frombytes ignores bit 255 of u and fe_tobytes reduces fully
 * mod p, so the RFC 7748 masking and final reduction live in the shared field.
 * Lines tagged GUARD:<name> are removed one at a time by `make mutants`; the
 * tests must catch each removal.
 */
#include "x25519.h"

#include <string.h>

#include "../sig/fe25519.h"

/* Swap p and q when b = 1, keep them when b = 0, without a branch. */
static void fe_cswap(fe p, fe q, uint64_t b)
{
    uint64_t m = 0 - b;
    FE_BARRIER(m);
    for (int i = 0; i < 5; i++) {
        uint64_t t = m & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

/* h = f * 121665 ((A - 2) / 4 for curve25519, RFC 7748 Section 5). */
static void fe_mul121665(fe h, const fe f)
{
    u128 t0 = (u128)f[0] * 121665, t1 = (u128)f[1] * 121665, t2 = (u128)f[2] * 121665;
    u128 t3 = (u128)f[3] * 121665, t4 = (u128)f[4] * 121665;
    t1 += t0 >> 51;
    t2 += t1 >> 51;
    t3 += t2 >> 51;
    t4 += t3 >> 51;
    uint64_t c = (uint64_t)(t4 >> 51);
    uint64_t h0 = (uint64_t)t0 & MASK51;
    h0 += 19 * c; /* GUARD:x-fold */
    h[1] = ((uint64_t)t1 & MASK51) + (h0 >> 51);
    h[0] = h0 & MASK51;
    h[2] = (uint64_t)t2 & MASK51;
    h[3] = (uint64_t)t3 & MASK51;
    h[4] = (uint64_t)t4 & MASK51;
}

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

int aienos_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32])
{
    uint8_t z[32];
    fe x1, x2, z2, x3, z3, a, aa, b, bb, e, c, d, da, cb;
    uint64_t swap = 0;
    memcpy(z, scalar, 32);
    z[31] &= 127; /* bit 255 is never read by the ladder; kept for clarity */
    z[31] |= 64;  /* GUARD:clamp-254 */
    z[0] &= 248;  /* GUARD:clamp-low */
    fe_frombytes(x1, u);
    fe_1(x2); fe_0(z2);
    fe_copy(x3, x1); fe_1(z3);
    /* Montgomery ladder over bits 254..0 (RFC 7748 Section 5). */
    for (int i = 254; i >= 0; i--) {
        uint64_t k = (uint64_t)((z[i >> 3] >> (i & 7)) & 1);
        swap ^= k;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = k;
        fe_add(a, x2, z2);
        fe_sq(aa, a);
        fe_sub(b, x2, z2);
        fe_sq(bb, b);
        fe_sub(e, aa, bb);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);
        fe_add(x3, da, cb);
        fe_sq(x3, x3);
        fe_sub(z3, da, cb);
        fe_sq(z3, z3);
        fe_mul(z3, z3, x1);
        fe_mul(x2, aa, bb);
        fe_mul121665(z2, e);
        fe_add(z2, z2, aa);
        fe_mul(z2, z2, e);
    }
    /* RFC 7748 final swap. swap = bit 0 of the clamped scalar = 0, so it never
     * moves anything here (untagged: no mutant could be killed). */
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= out[i];
    wipe(z, sizeof z);
    wipe(x2, sizeof x2); wipe(z2, sizeof z2); wipe(x3, sizeof x3); wipe(z3, sizeof z3);
    wipe(a, sizeof a); wipe(aa, sizeof aa); wipe(b, sizeof b); wipe(bb, sizeof bb);
    wipe(e, sizeof e); wipe(c, sizeof c); wipe(d, sizeof d); wipe(da, sizeof da);
    wipe(cb, sizeof cb); wipe(x1, sizeof x1);
    /* all-zero output <=> small-order input (RFC 7748 Section 6.1) */
    return ((acc | (uint8_t)-acc) >> 7) ? 0 : -1; /* GUARD:x-small-order */
}

void aienos_x25519_base(uint8_t out[32], const uint8_t scalar[32])
{
    static const uint8_t nine[32] = {9};
    (void)aienos_x25519(out, scalar, nine);
}
