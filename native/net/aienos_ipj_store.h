/* Interplane roster in the Store (native/store, Store v1): one application
 * object per accepted roster, kind IPJ_STORE_KIND_ROSTER, version 1, bytes =
 * the owner-signed roster exactly as received. The Store proves the bytes are
 * intact (structural integrity, unkeyed); the owner signature proves who
 * issued them; the generation inside proves freshness against older copies
 * in the same Store. Keyed sealing and hardware anti-rollback of the roster
 * are not claimed here (they follow the M5 / TRUST-1 path). */
#ifndef AIENOS_IPJ_STORE_H
#define AIENOS_IPJ_STORE_H

#include "aienos_ipj.h"
#include "store_engine.h" /* native/store (built with -I../store -I../argus -I../disk) */

#define IPJ_STORE_KIND_ROSTER 25u
#define IPJ_STORE_VERSION 1u

/* Commit blob as one new object (refuses a blob that does not verify). */
ipj_status ipj_roster_save(st_store *s, const uint8_t *blob, size_t len, const uint8_t owner_pk[32],
                           unsigned mode, int owner_is_test);
/* Load the highest-generation roster object that verifies. Objects that fail
 * are skipped and counted in *skipped (may be NULL). IPJ_ERR_NOT_FOUND when
 * none verifies. */
ipj_status ipj_roster_load(st_store *s, const uint8_t owner_pk[32], unsigned mode, int owner_is_test,
                           ipj_roster *out, uint8_t *blob, size_t cap, size_t *len, unsigned *skipped);

#endif
