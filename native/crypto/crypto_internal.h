/* Internal hooks shared by the library objects and the test. Not public API. */
#ifndef AIENOS_CRYPTO_INTERNAL_H
#define AIENOS_CRYPTO_INTERNAL_H

#include <stdint.h>

/* Constant-time AES S-box on all 16 bytes of s, in place. */
void aienos_aes_sub_bytes16(uint8_t s[16]);
/* n independent 16-byte blocks in place (ECB), same bytes as n single calls. */
void aienos_aes256_encrypt_blocks(const aienos_aes256_key *k, uint8_t *buf, size_t n);
/* 0 = force the C path, 1 = ARMv8 instructions when present (the default).
 * Return 1 when the instructions are in use afterwards (L339). */
int aienos_aes_isa_enable(int on);
int aienos_polyval_isa_enable(int on);

/* Optimization barrier: the compiler must treat v as unknown, so a 0/1
 * value cannot be turned back into a branch. */
#define AIENOS_CT_BARRIER(v) __asm__ volatile("" : "+r"(v))

#endif
