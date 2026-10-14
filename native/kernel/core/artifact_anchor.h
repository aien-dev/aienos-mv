/* artifact_anchor.h -- the artifact loader's trust anchors, chosen at build time.
 *
 *   ordinary build:                 no anchor; every signed candidate is refused (UntrustedSigner).
 *   CK_SEED0B_TEST_ANCHOR=1:        the RFC 8032 TEST 1 public key only (SEED-0B, TEST ONLY).
 *   CK_PI5_RC=1 (make pi5-rc):      the owner root public key from the PI5-8 key ceremony only, read from the
 *                                   CK_OWNER_PUBKEYS file by tools/ck_owner_gen.c --anchor into the generated
 *                                   ck_owner_anchor.h. No TEST anchor; the two modes never combine.
 *
 * Shared by core/artifact_loader.c and the host test tests/test_pi5_rc_anchor.c, so the host test runs the very
 * table the release candidate image links. Only public key bytes; nothing here can sign. */
#ifndef AIENOS_CK_ARTIFACT_ANCHOR_H
#define AIENOS_CK_ARTIFACT_ANCHOR_H
#include "ck_artifact.h"

#if defined(CK_SEED0B_TEST_ANCHOR) && defined(CK_PI5_RC)
#error "CK_PI5_RC (owner anchor) cannot be combined with CK_SEED0B_TEST_ANCHOR (TEST anchor)"
#endif

#ifdef CK_SEED0B_TEST_ANCHOR
static const uint8_t (*const anchors)[32] = &cka_test1_pk;
#define NANCHORS 1u
#define FEATURE_TEST_ANCHOR 1u
#elif defined(CK_PI5_RC)
#include "ck_owner_anchor.h"
#ifndef CK_OWNER_ANCHOR_PROVISIONED
#error "CK_PI5_RC needs the generated owner anchor header (make pi5-rc CK_OWNER_PUBKEYS=<owner pubkeys file>)"
#endif
static const uint8_t (*const anchors)[32] = ck_owner_anchor_pk;
#define NANCHORS 1u
#define FEATURE_TEST_ANCHOR 0u
#define CK_ARTIFACT_OWNER_ANCHOR 1
#else
static const uint8_t (*const anchors)[32] = 0;
#define NANCHORS 0u
#define FEATURE_TEST_ANCHOR 0u
#endif

#endif
