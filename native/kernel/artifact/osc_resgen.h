/* osc_resgen.h -- per-resource incarnation counter for OSC unit capability pins (ADR 0019, aienos#285).
 *
 * One u32 counter per domain-1 resource number (ADR 0013 ResourceKind 2 channel, 3 object):
 *   - 0 is never issued; the first resource to hold a number is at 1; each reuse adds 1;
 *   - it never wraps: a number whose counter is at UINT32_MAX is retired when that resource goes;
 *   - it never decreases, across reboot and rollback: every issue is written ahead to the floor (a
 *     high-water record that rollback does not touch) and a restore never goes below the floor;
 *   - it is never derived from a handle-slot generation, authority reference generation, authority
 *     epoch, boot generation or system Generation (it counts only create calls on its own number);
 *   - a table whose counters cannot be shown monotonic (no floor restored, a corrupt record, a floor
 *     behind the snapshot, a failed floor write) is untrusted: every lookup answers "no such resource",
 *     so every nonzero pin stays CAP_GENERATION_STALE, and no number can be issued.
 * Pure: no kernel state, no allocation. The kernel holds one table (core/osc_admit.c). */
#ifndef OSC_RESGEN_H
#define OSC_RESGEN_H

#include <stddef.h>
#include <stdint.h>

#define OSC_RESGEN_MAX 64u /* resource numbers one table tracks (live, free or retired) */

enum { OSC_RESGEN_KIND_CHANNEL = 2, OSC_RESGEN_KIND_OBJECT = 3 };

enum {
    OSC_RESGEN_OK = 0,
    OSC_RESGEN_UNTRUSTED = 1, /* counters not shown monotonic: nothing issued, nothing found */
    OSC_RESGEN_BAD_KIND = 2,  /* not a domain-1 kind (2 or 3) */
    OSC_RESGEN_BUSY = 3,      /* the number is held by a live resource */
    OSC_RESGEN_RETIRED = 4,   /* the counter reached UINT32_MAX; the number is never given out again */
    OSC_RESGEN_FULL = 5,      /* the table tracks OSC_RESGEN_MAX numbers already */
    OSC_RESGEN_NOT_LIVE = 6,  /* destroy of a number no live resource holds */
    OSC_RESGEN_FLOOR_IO = 7,  /* the floor write failed; the table is now untrusted */
    OSC_RESGEN_BAD_RECORD = 8 /* restore: a record is corrupt, too short or of the wrong role */
};

enum { OSC_RESGEN_LIVE = 1u, OSC_RESGEN_PERSIST = 2u };

struct osc_resgen_ent {
    uint16_t kind;
    uint16_t flags; /* OSC_RESGEN_LIVE, OSC_RESGEN_PERSIST (survives reboot) */
    uint32_t id;
    uint32_t gen;   /* current or last issued counter; 0 only for an unused slot */
};

/* Writes the floor record before a counter is issued. Returns 0 when the record is durable. */
typedef int (*osc_resgen_floor_write)(void *ctx, const uint8_t *rec, size_t len);

struct osc_resgen {
    int trusted;
    unsigned n;
    struct osc_resgen_ent ent[OSC_RESGEN_MAX];
    osc_resgen_floor_write floor_write; /* NULL: the table can never issue (kernel default today) */
    void *floor_ctx;
};

/* Record layout (both roles): "OSCRGEN\0" | u32le role (1 snapshot, 2 floor) | u32le count |
 * count * { u16le kind, u16le flags, u32le id, u32le gen } | SHA-256 of everything before it. */
#define OSC_RESGEN_ROLE_SNAPSHOT 1u
#define OSC_RESGEN_ROLE_FLOOR 2u
#define OSC_RESGEN_REC_MAX (16u + 12u * OSC_RESGEN_MAX + 32u)

/* An empty, untrusted table: a fresh boot knows no history until osc_resgen_restore or osc_resgen_genesis. */
void osc_resgen_init(struct osc_resgen *t, osc_resgen_floor_write fw, void *fw_ctx);
/* First boot ever of a store with no domain-1 resources: an empty trusted table. Writes an empty floor
 * first; OSC_RESGEN_FLOOR_IO leaves the table untrusted. Only for a store proven never to have held one. */
unsigned osc_resgen_genesis(struct osc_resgen *t);
/* Give `id` of `kind` to a new resource. On OSC_RESGEN_OK *gen is its counter (>= 1). The floor is
 * written with the new counter before it is issued. */
unsigned osc_resgen_create(struct osc_resgen *t, unsigned kind, uint32_t id, int persist, uint32_t *gen);
/* The resource holding `id` is gone; its counter stays so the next holder gets counter + 1. */
unsigned osc_resgen_destroy(struct osc_resgen *t, unsigned kind, uint32_t id);
/* osc_policy.gen_lookup: 0 and the live resource's counter, nonzero for "no such resource" (any domain
 * but 1, any kind but 2 or 3, an untrusted table, a number no live resource holds). */
int osc_resgen_lookup(void *ctx, unsigned domain, unsigned kind, uint32_t id, uint64_t *current);
/* Encode the table as a snapshot (live persistent resources and every counter) or as a floor. */
size_t osc_resgen_encode(const struct osc_resgen *t, uint32_t role, uint8_t *out, size_t cap);
/* After a reboot or a rollback: rebuild from the floor (never rolled back) and the snapshot (may be an
 * older one). Every counter is at least its floor; a persistent resource at its floor counter comes back
 * live; one below its floor (a rolled-back incarnation) comes back as a new incarnation, floor + 1,
 * written ahead to the floor (retired at UINT32_MAX); non-persistent resources come back free.
 * Any decode failure, or a snapshot counter above its floor, leaves the table untrusted. */
unsigned osc_resgen_restore(struct osc_resgen *t, const uint8_t *snap, size_t snap_len, const uint8_t *floor,
                            size_t floor_len);

#endif
