/* test_apple_smp.c -- spin-table secondary start (apple/apple_smp.c) on a
 * model of parked cores.
 *
 * Model: the primary's stores land in its cache; a parked core sees only
 * memory (point of coherency), so a release that is not cleaned is never
 * seen. A parked core sits in wfe: it re-reads its release slot only on a
 * sev, then jumps to a non-zero value. A jump to the expected entry checks
 * in; any other value is a crash. booting.rst allows the wfe, so a release
 * without a sev is also never seen. Every model access outside the known
 * slots is counted as stray. Trees: hand-built /memreserve/ blobs and the
 * pinned Apple fixtures (which carry no cpu-release-addr: m1n1 adds it at
 * boot, so every secondary there must be refused, never guessed). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../apple/apple_smp.h"
#include "ck_test.h"

#ifndef APPLE_FIXTURES
#define APPLE_FIXTURES "tests/fixtures/apple"
#endif

#define ENTRY 0x80081000ull
#define NSLOT 8

struct core {
    uint64_t slot;
    uint64_t mem, cache;
    int dirty, parked, checked_in, crashed, dead;
    uint64_t jumped;
};

struct model {
    struct core c[NSLOT];
    uint32_t n, stray, stores, cleans, sevs, polls;
};

static struct core *find(struct model *m, uint64_t pa)
{
    for (uint32_t i = 0; i < m->n; i++)
        if (m->c[i].slot == pa) return &m->c[i];
    m->stray++;
    return 0;
}

static uint64_t m_rd64(void *ctx, uint64_t pa)
{
    struct core *c = find(ctx, pa);
    return !c ? 0 : c->dirty ? c->cache : c->mem;
}

static void m_wr64(void *ctx, uint64_t pa, uint64_t v)
{
    struct model *m = ctx;
    struct core *c = find(m, pa);
    m->stores++;
    if (c) {
        c->cache = v;
        c->dirty = 1;
    }
}

static void m_clean(void *ctx, uint64_t pa)
{
    struct model *m = ctx;
    struct core *c = find(m, pa);
    m->cleans++;
    if (c && c->dirty) {
        c->mem = c->cache;
        c->dirty = 0;
    }
}

static void m_sev(void *ctx)
{
    struct model *m = ctx;
    m->sevs++;
    for (uint32_t i = 0; i < m->n; i++) {
        struct core *c = &m->c[i];
        if (!c->parked || c->dead || c->mem == 0) continue;
        c->parked = 0;
        c->jumped = c->mem;
        if (c->mem == ENTRY) c->checked_in = 1;
        else c->crashed = 1;
    }
}

/* idx is the cpu index; the model keeps core i at slot index i. */
static uint32_t m_checkin(void *ctx, uint32_t idx)
{
    struct model *m = ctx;
    m->polls++;
    return idx < m->n && m->c[idx].checked_in;
}

static struct ck_smp_io io_for(struct model *m)
{
    struct ck_smp_io io = {m_rd64, m_wr64, m_clean, m_sev, m_checkin, m};
    return io;
}

/* Minimal FDT: empty root (BEGIN_NODE "", END_NODE, END), the given
 * /memreserve/ pairs, no strings. */
static int open_rsv(struct ck_fdt *f, uint8_t *buf, const uint64_t *rsv, uint32_t n)
{
    uint32_t off_rsv = 40, off_struct = off_rsv + 16u * (n + 1u), size_struct = 16;
    uint32_t total = off_struct + size_struct;
    uint32_t hdr[10] = {CK_FDT_MAGIC, total, off_struct, total, off_rsv, 17, 16, 0, 0, size_struct};
    memset(buf, 0, total);
    for (int i = 0; i < 10; i++)
        for (int k = 0; k < 4; k++) buf[4 * i + k] = (uint8_t)(hdr[i] >> (24 - 8 * k));
    for (uint32_t i = 0; i < 2u * n; i++)
        for (int k = 0; k < 8; k++) buf[off_rsv + 8u * i + (uint32_t)k] = (uint8_t)(rsv[i] >> (56 - 8 * k));
    const uint8_t st[16] = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 9};
    memcpy(buf + off_struct, st, sizeof(st));
    return ck_fdt_open(f, buf, total);
}

static void add_cpu(struct ck_apple_platform *p, struct model *m, uint64_t mpidr, uint64_t release, int spin)
{
    uint32_t i = p->ncpus++;
    p->cpu[i].mpidr = mpidr;
    p->cpu[i].release = release;
    p->cpu[i].spin_table = (uint8_t)spin;
    m->c[i].slot = release;
    m->c[i].parked = 1;
    m->n = p->ncpus;
}

static void test_rsv(void)
{
    static uint8_t buf[512];
    struct ck_fdt f;
    const uint64_t rsv[4] = {0x10000000ull, 0x1000, 0xffffffffffffe000ull, 0x1000};
    CHECK(open_rsv(&f, buf, rsv, 2) == CK_FDT_OK);
    CHECK(ck_smp_rsv_contains(&f, 0x10000000ull, 8));
    CHECK(ck_smp_rsv_contains(&f, 0x10000ff8ull, 8));
    CHECK(!ck_smp_rsv_contains(&f, 0x10000ffcull, 8)); /* crosses the end */
    CHECK(!ck_smp_rsv_contains(&f, 0x10001000ull, 8));
    CHECK(!ck_smp_rsv_contains(&f, 0x0ffffff8ull, 8));
    CHECK(ck_smp_rsv_contains(&f, 0xffffffffffffeff8ull, 8)); /* last slot of the top region */
    CHECK(!ck_smp_rsv_contains(&f, 0xfffffffffffffffcull, 8)); /* wraps */
    CHECK(!ck_smp_rsv_contains(&f, 0x10000000ull, 0));
    CHECK(open_rsv(&f, buf, rsv, 0) == CK_FDT_OK);
    CHECK(!ck_smp_rsv_contains(&f, 0x10000000ull, 8)); /* no reservations at all */
}

static void test_release(void)
{
    static uint8_t buf[512];
    struct ck_fdt f;
    const uint64_t rsv[4] = {0x10000000ull, 0x1000, 0x30000000ull, 0x8};
    CHECK(open_rsv(&f, buf, rsv, 2) == CK_FDT_OK);
    struct ck_apple_platform *p = calloc(1, sizeof(*p));
    struct model m = {0};
    struct ck_smp_io io = io_for(&m);
    const uint64_t img_lo = 0x80080000ull, img_hi = 0x80200000ull, boot = 0x80000000ull; /* MPIDR bit 31 RES1 */
    add_cpu(p, &m, 0x0, 0x10000000ull, 1);       /* 0: the boot CPU */
    add_cpu(p, &m, 0x1, 0x10000008ull, 1);       /* 1: good */
    add_cpu(p, &m, 0x10100, 0x30000000ull, 1);   /* 2: good, other cluster, other region */
    add_cpu(p, &m, 0x10101, 0x20000000ull, 1);   /* 3: slot not reserved */
    add_cpu(p, &m, 0x10102, 0x10000010ull, 0);   /* 4: not spin-table */
    add_cpu(p, &m, 0x10103, 0, 1);               /* 5: no cpu-release-addr */
    add_cpu(p, &m, 0x2, 0x10000014ull, 1);       /* 6: misaligned */
    add_cpu(p, &m, 0x3, 0x10000018ull, 1);       /* 7: never answers */
    m.c[7].dead = 1;

#define R(idx, entry, lo, hi) ck_smp_spin_release(&f, p, (idx), boot, (entry), (lo), (hi), &io, 1000)
    CHECK(R(0, ENTRY, img_lo, img_hi) == CK_SMP_E_BOOT);
    CHECK(R(3, ENTRY, img_lo, img_hi) == CK_SMP_E_RSV);
    CHECK(R(4, ENTRY, img_lo, img_hi) == CK_SMP_E_METHOD);
    CHECK(R(5, ENTRY, img_lo, img_hi) == CK_SMP_E_RELEASE);
    CHECK(R(6, ENTRY, img_lo, img_hi) == CK_SMP_E_RELEASE);
    CHECK(R(8, ENTRY, img_lo, img_hi) == CK_SMP_E_IDX);
    CHECK(R(1, 0, img_lo, img_hi) == CK_SMP_E_ENTRY);
    CHECK(R(1, ENTRY + 2, img_lo, img_hi) == CK_SMP_E_ENTRY);
    CHECK(R(1, ENTRY, 0x10000000ull, 0x10000009ull) == CK_SMP_E_IMAGE); /* image overlaps the slot's last byte */
    CHECK(R(1, ENTRY, 0x10000010ull, 0x10001000ull) == 0); /* image starting right after the slot: allowed */
    CHECK(m.c[1].checked_in == 1);
    CHECK(m.stores == 1 && m.sevs == 1 && m.cleans == 1 && m.stray == 0); /* every refusal wrote nothing */
    CHECK(m.c[1].mem == ENTRY && m.c[1].jumped == ENTRY && !m.c[1].crashed);
    CHECK(!m.c[0].checked_in && !m.c[3].checked_in); /* the sev woke nobody else */

    CHECK(R(2, ENTRY, img_lo, img_hi) == 0);
    CHECK(m.c[2].checked_in && m.c[2].jumped == ENTRY);
    CHECK(R(1, ENTRY, img_lo, img_hi) == CK_SMP_E_BUSY); /* already released */
    CHECK(m.stores == 2);

    m.polls = 0;
    CHECK(R(7, ENTRY, img_lo, img_hi) == CK_SMP_E_TIMEOUT); /* bounded, never a hang */
    CHECK(m.polls == 1000);
    CHECK(m.stray == 0);
    struct ck_apple_soc m4 = {.ctrr_hole = 1}; /* M4-class row */
    p->soc = &m4;
    uint32_t st = m.stores;
    CHECK(R(3, ENTRY, img_lo, img_hi) == CK_SMP_E_CTRR && m.stores == st);
    p->soc = 0;
#undef R
    free(p);
}

static uint8_t *load(const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", APPLE_FIXTURES, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, fp) != (size_t)n) {
        free(b);
        b = 0;
    }
    fclose(fp);
    *len = (size_t)n;
    return b;
}

/* Pinned Apple trees: the loader has not filled cpu-release-addr, so no
 * secondary may be released; the boot CPU is never one. */
static void test_fixtures(void)
{
    const char *names[] = {"t8103-j313.dtb", "t8112-j413.dtb", "t8122-j613.dtb", "t8132-j713.dtb", "t6031-j514c.dtb"};
    for (unsigned k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
        size_t len = 0;
        uint8_t *b = load(names[k], &len);
        CHECK(b != 0);
        if (!b) continue;
        struct ck_fdt f;
        struct ck_apple_platform *p = calloc(1, sizeof(*p));
        struct model m = {0};
        struct ck_smp_io io = io_for(&m);
        CHECK(ck_fdt_open(&f, b, (uint32_t)len) == CK_FDT_OK);
        CHECK(ck_apple_probe(&f, p) == CK_APPLE_OK);
        uint32_t boot = 0, release = 0, ctrr = 0;
        for (uint32_t i = 0; i < p->ncpus; i++) {
            int rc = ck_smp_spin_release(&f, p, i, p->cpu[0].mpidr, ENTRY, 0x80080000ull, 0x80200000ull, &io, 10);
            boot += rc == CK_SMP_E_BOOT;
            release += rc == CK_SMP_E_RELEASE;
            ctrr += rc == CK_SMP_E_CTRR;
        }
        printf("  %s: cpus=%u boot=%u refused-no-release-addr=%u refused-ctrr=%u writes=%u\n", names[k], p->ncpus,
               boot, release, ctrr, m.stores);
        /* M4-class (t8132): the CTRR refusal comes first; others: no release address. */
        uint32_t want_ctrr = p->soc && p->soc->ctrr_hole ? p->ncpus - 1u : 0;
        CHECK(boot == 1 && ctrr == want_ctrr && release + ctrr == p->ncpus - 1u && m.stores == 0 && m.sevs == 0);
        if (strcmp(names[k], "t8132-j713.dtb") == 0) CHECK(want_ctrr == 9);
        free(p);
        free(b);
    }
}

int main(void)
{
    test_rsv();
    test_release();
    test_fixtures();
    return ck_t_verdict("test_apple_smp");
}
