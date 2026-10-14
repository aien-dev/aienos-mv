/* test_pi5_rc_anchor.c -- host test of the Pi 5 release candidate's artifact trust (PI5-8 cut R).
 *
 *   test_pi5_rc_anchor <owner pubkeys file>          run the checks
 *   test_pi5_rc_anchor --print-throwaway-key         print the throwaway public key line (for the red control)
 *
 * Built against the generated ck_owner_anchor.h (tools/ck_owner_gen.c --anchor) through core/artifact_anchor.h with
 * CK_PI5_RC defined, so it runs the very anchor table and selection the RC image links, and calls the same
 * cka_verify / cka_boot_policy / cka_evaluate the kernel loader calls. Every artifact here is signed by a THROWAWAY key
 * derived in this file, or by the public RFC 8032 TEST keys; nothing is ever signed by the owner key (its private half
 * lives only in the YubiKey). Checks:
 *   - the compiled anchor equals the owner_root_ed25519 line of the owner file given, and is no RFC 8032 TEST key;
 *   - an artifact signed by the throwaway key, by RFC 8032 TEST 1 (the SEED-0B anchor) or by TEST 3 is REFUSED
 *     (UntrustedSigner), and the boot policy built from the anchor refuses their signer at admission;
 *   - a forged artifact that names the owner key's fingerprint but carries a throwaway signature is REFUSED
 *     (BadSignature);
 *   - positive control: the same throwaway artifact verifies when the throwaway key is the anchor, and a payload byte
 *     flip is then refused, so the refusals above come from the anchor and not from a broken artifact.
 * Red control (scripts/ck_pi5_rc_check.sh): built with the THROWAWAY key as "owner", this test must FAIL. */
#define main ck_artifact_tool_main
#include "../tools/ck_artifact_tool.c"
#undef main

#define CK_PI5_RC 1
#include "artifact_anchor.h"
#include "ck_test.h"

#ifndef CK_ARTIFACT_OWNER_ANCHOR
#error "artifact_anchor.h did not select the owner anchor under CK_PI5_RC"
#endif

static uint8_t throwaway_seed[32], throwaway_pk[32];

static void throwaway_init(void)
{
    static const char label[] = "AIENOS PI5-8 cut R throwaway signer, never an owner key";
    sha256_hash((const uint8_t *)label, sizeof label - 1, throwaway_seed);
    if (aienos_ed25519_public_key(throwaway_pk, throwaway_seed) != AIENOS_SIG_OK)
        die("throwaway public key");
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* The owner_root_ed25519 line of an owner pubkeys file; 0 ok. */
static int owner_from_file(const char *path, uint8_t out[32])
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char hex[80];
        if (sscanf(line, "owner_root_ed25519 %79s", hex) != 1)
            continue;
        if (strlen(hex) != 64 || found++)
            break;
        for (int i = 0; i < 32; i++) {
            int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
            if (hi < 0 || lo < 0) {
                fclose(f);
                return -1;
            }
            out[i] = (uint8_t)(hi << 4 | lo);
        }
    }
    fclose(f);
    return found == 1 ? 0 : -1;
}

static struct buf art_signed_by(const uint8_t seed[32])
{
    struct buf b = signed_art(1, seed_read(1), res(1, 1, 1, 1, 8), exit0_w, 4, "RCANCHOR");
    sign_with(&b, seed);
    return b;
}

static int verify_rc(const struct buf *b, uint8_t fp[32])
{
    struct cka_parsed p;
    struct cka_ident id;
    return cka_verify(b->p, b->n, anchors, NANCHORS, &p, &id, fp);
}

static void test_anchor_is_owner_file(const char *owner_file)
{
    uint8_t want[32];
    CHECK(owner_from_file(owner_file, want) == 0);
    CHECK(NANCHORS == 1u && FEATURE_TEST_ANCHOR == 0u);
    CHECK(memcmp(anchors[0], want, 32) == 0);
    CHECK(memcmp(anchors[0], cka_test1_pk, 32) != 0);
    CHECK(memcmp(anchors[0], cka_test2_pk, 32) != 0);
    uint8_t t3[32];
    CHECK(aienos_ed25519_public_key(t3, untrusted_seed) == AIENOS_SIG_OK);
    CHECK(memcmp(anchors[0], t3, 32) != 0);
}

static void test_refusals(void)
{
    uint8_t fp[32];
    struct buf tw = art_signed_by(throwaway_seed);
    struct buf t1 = art_signed_by(test1_seed);
    struct buf t3 = art_signed_by(untrusted_seed);
    CHECK(verify_rc(&tw, fp) == CKA_UNTRUSTED_SIGNER);
    CHECK(verify_rc(&t1, fp) == CKA_UNTRUSTED_SIGNER);
    CHECK(verify_rc(&t3, fp) == CKA_UNTRUSTED_SIGNER);

    /* Admission: the boot policy built from the RC anchor names the owner key only and refuses the other signers. */
    struct cka_policy pol;
    cka_boot_policy(&pol, anchors, NANCHORS);
    uint8_t ofp[32];
    sha256_hash(anchors[0], 32, ofp);
    CHECK(pol.nsigners == 1 && memcmp(pol.signers[0], ofp, 32) == 0);
    struct cka_parsed p;
    struct cka_ident id;
    struct cka_decision d;
    struct cka_limits avail = {1000, 1000, 1000, 16, 0, 0, 1000000000ull, 1000000000ull, 65536};
    CHECK(cka_identify(tw.p, tw.n, &p, &id) == 0);
    uint8_t twfp[32];
    sha256_hash(throwaway_pk, 32, twfp);
    CHECK(cka_evaluate(&pol, &p, twfp, &avail, &d) != 0);
    uint8_t t1fp[32];
    sha256_hash(cka_test1_pk, 32, t1fp);
    CHECK(cka_evaluate(&pol, &p, t1fp, &avail, &d) != 0);

    /* Forgery: the signature block names the owner key's fingerprint, the signature is the throwaway key's. */
    struct buf fg = clone(&tw, 0);
    CHECK(cka_identify(fg.p, fg.n, &p, &id) == 0);
    memcpy(fg.p + p.sig_off + 4, ofp, 32);
    CHECK(verify_rc(&fg, fp) == CKA_BAD_SIGNATURE);
    free(fg.p);
    free(t3.p);
    free(t1.p);
    free(tw.p);
}

static void test_positive_control(void)
{
    struct buf tw = art_signed_by(throwaway_seed);
    const uint8_t (*tw_anchor)[32] = (const uint8_t (*)[32])throwaway_pk;
    struct cka_parsed p;
    struct cka_ident id;
    uint8_t fp[32], twfp[32];
    CHECK(cka_verify(tw.p, tw.n, tw_anchor, 1, &p, &id, fp) == 0);
    sha256_hash(throwaway_pk, 32, twfp);
    CHECK(memcmp(fp, twfp, 32) == 0);
    struct buf t = clone(&tw, 0);
    t.p[p.payload_off] ^= 1;
    CHECK(cka_verify(t.p, t.n, tw_anchor, 1, &p, &id, fp) != 0);
    free(t.p);
    free(tw.p);
}

int main(int argc, char **argv)
{
    throwaway_init();
    if (argc == 2 && !strcmp(argv[1], "--print-throwaway-key")) {
        printf("owner_root_ed25519 ");
        for (int i = 0; i < 32; i++)
            printf("%02x", throwaway_pk[i]);
        printf("\n");
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: test_pi5_rc_anchor <owner pubkeys file> | --print-throwaway-key\n");
        return 2;
    }
    test_anchor_is_owner_file(argv[1]);
    test_refusals();
    test_positive_control();
    return ck_t_verdict("test_pi5_rc_anchor");
}
