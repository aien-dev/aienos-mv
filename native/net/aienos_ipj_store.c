/* Interplane roster in the Store (see aienos_ipj_store.h). */
#include "aienos_ipj_store.h"

#include <string.h>

ipj_status ipj_roster_save(st_store *s, const uint8_t *blob, size_t len, const uint8_t owner_pk[32],
                           unsigned mode, int owner_is_test)
{
    static ipj_roster t;
    if (!s || !blob || !owner_pk) return IPJ_ERR_ARG;
    ipj_status v = ipj_roster_accept(0, blob, len, owner_pk, mode, owner_is_test, &t);
    if (v != IPJ_OK) return v;
    st_object o = {IPJ_STORE_KIND_ROSTER, IPJ_STORE_VERSION, blob, len};
    return st_transact(s, &o, 1, 0, 0) == 0 ? IPJ_OK : IPJ_ERR_STORE;
}

ipj_status ipj_roster_load(st_store *s, const uint8_t owner_pk[32], unsigned mode, int owner_is_test,
                           ipj_roster *out, uint8_t *blob, size_t cap, size_t *len, unsigned *skipped)
{
    static ipj_roster best, t;
    static uint8_t buf[IPJ_ROSTER_MAX_BYTES];
    if (!s || !owner_pk || !out || !blob || !len) return IPJ_ERR_ARG;
    if (skipped) *skipped = 0;
    memset(&best, 0, sizeof best);
    size_t best_len = 0;
    uint32_t n = 0;
    const sv1_entry *cat = st_catalog(s, &n);
    for (uint32_t i = 0; cat && i < n; i++) {
        if (cat[i].kind != IPJ_STORE_KIND_ROSTER || cat[i].version != IPJ_STORE_VERSION) continue;
        size_t l = 0;
        if (cat[i].byte_length > sizeof buf || st_read_object(s, cat[i].object_id, buf, sizeof buf, &l) != 0 ||
            ipj_roster_accept(&best, buf, l, owner_pk, mode, owner_is_test, &t) != IPJ_OK) {
            if (skipped) (*skipped)++;
            continue;
        }
        if (l > cap) return IPJ_ERR_ARG;
        best = t;
        memcpy(blob, buf, l);
        best_len = l;
    }
    if (best.generation == 0) return IPJ_ERR_NOT_FOUND;
    *out = best;
    *len = best_len;
    return IPJ_OK;
}
