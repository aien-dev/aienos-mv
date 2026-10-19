/* osc_resgen.c -- per-resource incarnation counter (ADR 0019, aienos#285). Rules in osc_resgen.h.
 *
 * MUT(n) marks a guard the host mutation run (Makefile osc-resgen-mutants) removes, one at a time: with
 * -DOSC_RESGEN_MUTANT=n guard n is skipped and tests/test_osc_resgen.c must fail. Mutants build only with
 * OSC_RESGEN_HOST_TEST; a kernel build refuses the flag. */
#include <string.h>
#include "osc_resgen.h"
#include "sha256.h"

#ifdef OSC_RESGEN_MUTANT
#ifndef OSC_RESGEN_HOST_TEST
#error "resgen mutants are TEST-only host builds (OSC_RESGEN_HOST_TEST)"
#endif
#define MUT(n) (OSC_RESGEN_MUTANT == (n))
#else
#define MUT(n) 0
#endif

static const uint8_t magic[8] = { 'O', 'S', 'C', 'R', 'G', 'E', 'N', 0 };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static int kind_ok(unsigned kind) { return kind == OSC_RESGEN_KIND_CHANNEL || kind == OSC_RESGEN_KIND_OBJECT; }

static struct osc_resgen_ent *find(struct osc_resgen *t, unsigned kind, uint32_t id)
{
    for (unsigned i = 0; i < t->n; i++)
        if (t->ent[i].kind == kind && t->ent[i].id == id)
            return &t->ent[i];
    return 0;
}

void osc_resgen_init(struct osc_resgen *t, osc_resgen_floor_write fw, void *fw_ctx)
{
    memset(t, 0, sizeof *t);
    t->floor_write = fw;
    t->floor_ctx = fw_ctx;
}

/* The floor holds every counter of the table as it stands. A failed write leaves the table untrusted:
 * the write may have landed or not, so nothing in memory can be shown to match the durable floor. */
static unsigned write_floor(struct osc_resgen *t)
{
    uint8_t rec[OSC_RESGEN_REC_MAX];
    size_t n = osc_resgen_encode(t, OSC_RESGEN_ROLE_FLOOR, rec, sizeof rec);
    if (n == 0 || !t->floor_write || t->floor_write(t->floor_ctx, rec, n) != 0) {
        t->trusted = 0;
        return OSC_RESGEN_FLOOR_IO;
    }
    return OSC_RESGEN_OK;
}

unsigned osc_resgen_genesis(struct osc_resgen *t)
{
    t->n = 0;
    memset(t->ent, 0, sizeof t->ent);
    if (write_floor(t) != OSC_RESGEN_OK)
        return OSC_RESGEN_FLOOR_IO;
    t->trusted = 1;
    return OSC_RESGEN_OK;
}

unsigned osc_resgen_create(struct osc_resgen *t, unsigned kind, uint32_t id, int persist, uint32_t *gen)
{
    *gen = 0;
    if (!MUT(1) && !t->trusted)
        return OSC_RESGEN_UNTRUSTED;
    if (!kind_ok(kind))
        return OSC_RESGEN_BAD_KIND;
    struct osc_resgen_ent *e = find(t, kind, id);
    if (e && !MUT(2) && (e->flags & OSC_RESGEN_LIVE))
        return OSC_RESGEN_BUSY;
    if (e && !MUT(3) && e->gen == UINT32_MAX)
        return OSC_RESGEN_RETIRED;
    if (!e && t->n >= OSC_RESGEN_MAX)
        return OSC_RESGEN_FULL;
    struct osc_resgen_ent old = { 0 };
    int fresh = !e;
    if (fresh) {
        e = &t->ent[t->n++];
        e->kind = (uint16_t)kind;
        e->id = id;
        e->gen = 0;
    }
    old = *e;
    e->gen = e->gen + 1u; /* 0 -> 1 for the first holder; never wraps (UINT32_MAX refused above) */
    e->flags = (uint16_t)(OSC_RESGEN_LIVE | (persist ? OSC_RESGEN_PERSIST : 0u));
    if (!MUT(4) && write_floor(t) != OSC_RESGEN_OK) {
        if (fresh)
            t->n--;
        else
            *e = old;
        return OSC_RESGEN_FLOOR_IO;
    }
    *gen = e->gen;
    return OSC_RESGEN_OK;
}

unsigned osc_resgen_destroy(struct osc_resgen *t, unsigned kind, uint32_t id)
{
    if (!t->trusted)
        return OSC_RESGEN_UNTRUSTED;
    struct osc_resgen_ent *e = find(t, kind, id);
    if (!e || !(e->flags & OSC_RESGEN_LIVE))
        return OSC_RESGEN_NOT_LIVE;
    e->flags = 0; /* the counter stays: the next holder is at gen + 1 */
    return OSC_RESGEN_OK;
}

int osc_resgen_lookup(void *ctx, unsigned domain, unsigned kind, uint32_t id, uint64_t *current)
{
    struct osc_resgen *t = ctx;
    if (!t || (!MUT(5) && !t->trusted) || domain != 1 || !kind_ok(kind))
        return 1;
    struct osc_resgen_ent *e = find(t, kind, id);
    if (!e || (!MUT(6) && !(e->flags & OSC_RESGEN_LIVE)))
        return 1;
    *current = e->gen;
    return 0;
}

size_t osc_resgen_encode(const struct osc_resgen *t, uint32_t role, uint8_t *out, size_t cap)
{
    if (t->n > OSC_RESGEN_MAX)
        return 0;
    size_t len = 16u + 12u * t->n + 32u;
    if (cap < len)
        return 0;
    memcpy(out, magic, 8);
    wr32(out + 8, role);
    wr32(out + 12, t->n);
    for (unsigned i = 0; i < t->n; i++) {
        uint8_t *p = out + 16 + 12u * i;
        wr16(p, t->ent[i].kind);
        wr16(p + 2, role == OSC_RESGEN_ROLE_FLOOR ? 0 : t->ent[i].flags);
        wr32(p + 4, t->ent[i].id);
        wr32(p + 8, t->ent[i].gen);
    }
    sha256_hash(out, len - 32, out + len - 32);
    return len;
}

/* Decode one record into ent[]; 0 on success. Every entry needs a domain-1 kind, a counter >= 1 and a
 * number not seen before in the same record. */
static int decode(const uint8_t *b, size_t len, uint32_t role, struct osc_resgen_ent *ent, unsigned *n)
{
    uint8_t d[32];
    if (!b || len < 48 || memcmp(b, magic, 8) != 0 || rd32(b + 8) != role)
        return 1;
    uint32_t cnt = rd32(b + 12);
    if (cnt > OSC_RESGEN_MAX || len != 16u + 12u * cnt + 32u)
        return 1;
    sha256_hash(b, len - 32, d);
    if (!MUT(7) && memcmp(d, b + len - 32, 32) != 0)
        return 1;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *p = b + 16 + 12u * i;
        ent[i].kind = rd16(p);
        ent[i].flags = rd16(p + 2);
        ent[i].id = rd32(p + 4);
        ent[i].gen = rd32(p + 8);
        if (!kind_ok(ent[i].kind) || ent[i].gen == 0 || (ent[i].flags & ~(OSC_RESGEN_LIVE | OSC_RESGEN_PERSIST)))
            return 1;
        if (role == OSC_RESGEN_ROLE_FLOOR && ent[i].flags)
            return 1;
        for (uint32_t j = 0; j < i; j++)
            if (ent[j].kind == ent[i].kind && ent[j].id == ent[i].id)
                return 1;
    }
    *n = cnt;
    return 0;
}

unsigned osc_resgen_restore(struct osc_resgen *t, const uint8_t *snap, size_t snap_len, const uint8_t *floor,
                            size_t floor_len)
{
    static struct osc_resgen_ent fl[OSC_RESGEN_MAX], sn[OSC_RESGEN_MAX];
    unsigned nf = 0, ns = 0;
    t->trusted = 0;
    t->n = 0;
    memset(t->ent, 0, sizeof t->ent);
    if (decode(floor, floor_len, OSC_RESGEN_ROLE_FLOOR, fl, &nf) != 0 ||
        decode(snap, snap_len, OSC_RESGEN_ROLE_SNAPSHOT, sn, &ns) != 0)
        return OSC_RESGEN_BAD_RECORD;
    /* Every counter starts at its floor, free. */
    for (unsigned i = 0; i < nf; i++) {
        t->ent[i] = fl[i];
        t->ent[i].flags = 0;
    }
    t->n = nf;
    int reissue = 0;
    for (unsigned i = 0; i < ns; i++) {
        struct osc_resgen_ent *e = find(t, sn[i].kind, sn[i].id);
        /* The floor is written before every issue, so it is never behind a snapshot: if it is, the floor
         * itself was rolled back or lost and nothing can be shown monotonic. */
        if (!e || (!MUT(8) && sn[i].gen > e->gen)) {
            t->n = 0;
            memset(t->ent, 0, sizeof t->ent);
            return OSC_RESGEN_BAD_RECORD;
        }
        if (MUT(9))
            e->gen = sn[i].gen; /* mutant: trust the (possibly rolled-back) snapshot counter */
        if (!(sn[i].flags & OSC_RESGEN_LIVE) || !(sn[i].flags & OSC_RESGEN_PERSIST))
            continue; /* gone at reboot: free at its floor counter, the next holder is floor + 1 */
        if (sn[i].gen == e->gen) {
            e->flags = OSC_RESGEN_LIVE | OSC_RESGEN_PERSIST; /* the same incarnation the floor last issued */
            continue;
        }
        /* Rolled back: the snapshot holds an older incarnation of this number. It must not answer the
         * newer counter a signer may have pinned, nor its own old one: it becomes a new incarnation. */
        if (!MUT(10) && e->gen == UINT32_MAX)
            continue; /* retired: the old resource does not come back under this number */
        e->gen++;
        e->flags = OSC_RESGEN_LIVE | OSC_RESGEN_PERSIST;
        reissue = 1;
    }
    t->trusted = 1;
    if (reissue && write_floor(t) != OSC_RESGEN_OK)
        return OSC_RESGEN_FLOOR_IO;
    return OSC_RESGEN_OK;
}
