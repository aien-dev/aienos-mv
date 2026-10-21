/* AEAD_AES_256_GCM_SIV per RFC 8452, port of crates/aienos-crypto/src/aes_gcm_siv.rs.
 *
 * Secret intermediates (derived keys, key schedules, CTR keystream, padded
 * tails, POLYVAL state) are wiped before every return, including failures.
 * Lines tagged GUARD:<name> are removed one at a time by `make mutants`.
 */
#include <string.h>

#include "aienos_crypto.h"
#include "crypto_internal.h"

void aienos_gcmsiv_derive_keys(const uint8_t key[32], const uint8_t nonce[12],
                               uint8_t auth_key[16], uint8_t enc_key[32])
{
    aienos_aes256_key kc;
    uint8_t block[6 * 16];
    aienos_aes256_init(&kc, key);
    for (uint32_t i = 0; i < 6; i++) {
        block[16 * i] = (uint8_t)i;
        block[16 * i + 1] = block[16 * i + 2] = block[16 * i + 3] = 0;
        memcpy(block + 16 * i + 4, nonce, 12);
    }
    aienos_aes256_encrypt_blocks(&kc, block, 6);
    for (uint32_t i = 0; i < 6; i++) {
        if (i < 2)
            memcpy(auth_key + 8 * i, block + 16 * i, 8);
        else
            memcpy(enc_key + 8 * (i - 2), block + 16 * i, 8);
    }
    aienos_wipe(block, sizeof block);
    aienos_aes256_wipe(&kc);
}

static void absorb_padded(aienos_polyval *pv, const uint8_t *d, size_t len)
{
    size_t full = len - len % 16;
    if (full != 0)
        (void)aienos_polyval_update(pv, d, full); /* full % 16 == 0, d != NULL */
    if (len % 16 != 0) {
        uint8_t b[16] = {0};
        memcpy(b, d + full, len % 16);
        aienos_polyval_update_block(pv, b);
        aienos_wipe(b, sizeof b);
    }
}

static void compute_tag(const uint8_t auth_key[16], const aienos_aes256_key *enc,
                        const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                        const uint8_t *pt, size_t pt_len, uint8_t tag[16])
{
    aienos_polyval pv;
    uint8_t lenblk[16];
    uint64_t abits = (uint64_t)aad_len * 8u, pbits = (uint64_t)pt_len * 8u;

    aienos_polyval_init(&pv, auth_key);
    absorb_padded(&pv, aad, aad_len);
    absorb_padded(&pv, pt, pt_len);
    for (int i = 0; i < 8; i++) {
        lenblk[i] = (uint8_t)(abits >> (8 * i));
        lenblk[8 + i] = (uint8_t)(pbits >> (8 * i));
    }
    aienos_polyval_update_block(&pv, lenblk);
    aienos_polyval_final(&pv, tag); /* wipes pv */

    for (int i = 0; i < 12; i++)
        tag[i] ^= nonce[i];
    tag[15] &= 0x7f; /* GUARD:tag-msb */
    aienos_aes256_encrypt_block(enc, tag);
}

/* AES-CTR with a 32-bit little-endian counter in bytes 0..3 that wraps.
 * Up to CTR_BATCH counter blocks are encrypted per call. */
#define CTR_BATCH 16
static void apply_ctr(const aienos_aes256_key *enc, const uint8_t tag[16], uint8_t *buf, size_t len)
{
    uint8_t ctr[16], ks[16 * CTR_BATCH];
    uint32_t c;
    memcpy(ctr, tag, 16);
    ctr[15] |= 0x80; /* GUARD:ctr-msb */
    c = (uint32_t)ctr[0] | ((uint32_t)ctr[1] << 8) | ((uint32_t)ctr[2] << 16) | ((uint32_t)ctr[3] << 24);
    for (size_t off = 0; off < len; off += 16 * CTR_BATCH) {
        size_t todo = len - off < 16 * CTR_BATCH ? len - off : 16 * CTR_BATCH;
        size_t nblk = (todo + 15) / 16;
        for (size_t b = 0; b < nblk; b++) {
            uint8_t *k = ks + 16 * b;
            memcpy(k, ctr, 16);
            k[0] = (uint8_t)c;
            k[1] = (uint8_t)(c >> 8);
            k[2] = (uint8_t)(c >> 16);
            k[3] = (uint8_t)(c >> 24);
            c += 1u; /* wraps mod 2^32 per RFC 8452 */
        }
        aienos_aes256_encrypt_blocks(enc, ks, nblk);
        for (size_t i = 0; i < todo; i++)
            buf[off + i] ^= ks[i];
    }
    aienos_wipe(ks, sizeof ks);
    aienos_wipe(ctr, sizeof ctr);
}

int aienos_gcmsiv_seal(const uint8_t key[32], const uint8_t nonce[12],
                       const uint8_t *aad, size_t aad_len,
                       const uint8_t *pt, size_t pt_len,
                       uint8_t *out, size_t out_len)
{
    uint8_t auth_key[16], enc_key[32], tag[16];
    aienos_aes256_key enc;

    if (key == NULL || nonce == NULL || out == NULL ||
        (aad == NULL && aad_len != 0) || (pt == NULL && pt_len != 0))
        return AIENOS_CRYPTO_ERR_LENGTH;
    if ((uint64_t)pt_len > AIENOS_GCMSIV_MAX_PT_LEN) return AIENOS_CRYPTO_ERR_LENGTH; /* GUARD:pt-max */
    if ((uint64_t)aad_len > AIENOS_GCMSIV_MAX_AAD_LEN) return AIENOS_CRYPTO_ERR_LENGTH; /* GUARD:aad-max-seal */
    if (out_len < AIENOS_GCMSIV_TAG_LEN || out_len - AIENOS_GCMSIV_TAG_LEN != pt_len)
        return AIENOS_CRYPTO_ERR_LENGTH;

    aienos_gcmsiv_derive_keys(key, nonce, auth_key, enc_key);
    aienos_aes256_init(&enc, enc_key);
    compute_tag(auth_key, &enc, nonce, aad, aad_len, pt, pt_len, tag);

    if (out != pt && pt_len != 0)
        memmove(out, pt, pt_len);
    apply_ctr(&enc, tag, out, pt_len);
    memcpy(out + pt_len, tag, 16);

    aienos_wipe(auth_key, sizeof auth_key);
    aienos_wipe(enc_key, sizeof enc_key);
    aienos_aes256_wipe(&enc);
    return AIENOS_CRYPTO_OK;
}

int aienos_gcmsiv_open(const uint8_t key[32], const uint8_t nonce[12],
                       const uint8_t *aad, size_t aad_len,
                       const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t out_len)
{
    uint8_t auth_key[16], enc_key[32], got[16], want[16];
    aienos_aes256_key enc;
    size_t ct_len;
    int ok;

    if (key == NULL || nonce == NULL || in == NULL || (aad == NULL && aad_len != 0) ||
        (out == NULL && out_len != 0))
        return AIENOS_CRYPTO_ERR_LENGTH;
    if (in_len < AIENOS_GCMSIV_TAG_LEN)
        return AIENOS_CRYPTO_ERR_LENGTH;
    if ((uint64_t)in_len > AIENOS_GCMSIV_MAX_CT_LEN) return AIENOS_CRYPTO_ERR_LENGTH; /* GUARD:ct-max */
    if ((uint64_t)aad_len > AIENOS_GCMSIV_MAX_AAD_LEN) return AIENOS_CRYPTO_ERR_LENGTH; /* GUARD:aad-max-open */
    ct_len = in_len - AIENOS_GCMSIV_TAG_LEN;
    if (out_len != ct_len)
        return AIENOS_CRYPTO_ERR_LENGTH;

    memcpy(got, in + ct_len, 16); /* before any in-place write */
    aienos_gcmsiv_derive_keys(key, nonce, auth_key, enc_key);
    aienos_aes256_init(&enc, enc_key);

    if (out != in && ct_len != 0)
        memmove(out, in, ct_len);
    apply_ctr(&enc, got, out, ct_len);
    compute_tag(auth_key, &enc, nonce, aad, aad_len, out, ct_len, want);

    ok = 1;
    ok &= aienos_ct_equal(got, want, 16); /* GUARD:tag-check */
    if (!ok)
        aienos_wipe(out, out_len); /* GUARD:wipe-on-fail */

    aienos_wipe(auth_key, sizeof auth_key);
    aienos_wipe(enc_key, sizeof enc_key);
    aienos_wipe(want, sizeof want);
    aienos_aes256_wipe(&enc);
    return ok ? AIENOS_CRYPTO_OK : AIENOS_CRYPTO_ERR_AUTH;
}
