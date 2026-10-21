/* test_apple_fdt.c -- host tests for apple/fdt.c and apple/apple_family.c.
 *
 * Inputs: the pinned Apple device trees in tests/fixtures/apple (compiled
 * from AsahiLinux/linux 94fb2334 by scripts/apple_dt_fixtures.sh, SOURCES.md
 * there), QEMU's own virt tree, and synthetic trees built here byte by byte
 * (good ones, and one broken variant per check ck_fdt_open makes). The
 * expected values for the Apple trees were read from `dtc -I dtb -O dts` of
 * the same files, not from this parser. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../apple/apple_family.h"
#include "../apple/fdt.h"
#include "ck_test.h"

#ifndef APPLE_FIXTURES
#define APPLE_FIXTURES "tests/fixtures/apple"
#endif

static uint8_t *load(const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", APPLE_FIXTURES, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("  cannot open %s\n", path);
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        free(b);
        return 0;
    }
    fclose(fp);
    *len = (size_t)n;
    return b;
}

/* ---- a tiny FDT writer for synthetic trees ---- */
struct wr {
    uint8_t st[8192];
    uint32_t sn;
    char str[2048];
    uint32_t strn;
    uint8_t out[16384];
    uint32_t outn;
};

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void w_tok(struct wr *w, uint32_t t)
{
    put32(w->st + w->sn, t);
    w->sn += 4;
}

static void w_bytes(struct wr *w, const void *d, uint32_t n)
{
    memcpy(w->st + w->sn, d, n);
    w->sn += n;
    while (w->sn & 3u) w->st[w->sn++] = 0;
}

static void w_begin(struct wr *w, const char *name)
{
    w_tok(w, 1);
    w_bytes(w, name, (uint32_t)strlen(name) + 1u);
}

static void w_end(struct wr *w) { w_tok(w, 2); }

static void w_prop(struct wr *w, const char *name, const void *v, uint32_t len)
{
    uint32_t off = w->strn;
    memcpy(w->str + w->strn, name, strlen(name) + 1u);
    w->strn += (uint32_t)strlen(name) + 1u;
    w_tok(w, 3);
    w_tok(w, len);
    w_tok(w, off);
    if (len) w_bytes(w, v, len);
}

static void w_u32(struct wr *w, const char *name, uint32_t v)
{
    uint8_t b[4];
    put32(b, v);
    w_prop(w, name, b, 4);
}

static void w_cells(struct wr *w, const char *name, const uint32_t *c, uint32_t n)
{
    uint8_t b[64];
    for (uint32_t i = 0; i < n; i++) put32(b + 4 * i, c[i]);
    w_prop(w, name, b, 4 * n);
}

static void w_str(struct wr *w, const char *name, const char *s) { w_prop(w, name, s, (uint32_t)strlen(s) + 1u); }

/* Lay out header, one reservation entry (+ terminator), struct, strings. */
static uint32_t w_finish(struct wr *w)
{
    w_tok(w, 9);
    uint32_t off_rsv = 40, off_struct = off_rsv + 32, off_strings = off_struct + w->sn;
    uint32_t total = off_strings + w->strn;
    memset(w->out, 0, sizeof(w->out));
    put32(w->out + 0, CK_FDT_MAGIC);
    put32(w->out + 4, total);
    put32(w->out + 8, off_struct);
    put32(w->out + 12, off_strings);
    put32(w->out + 16, off_rsv);
    put32(w->out + 20, 17);
    put32(w->out + 24, 16);
    put32(w->out + 28, 0);
    put32(w->out + 32, w->strn);
    put32(w->out + 36, w->sn);
    put32(w->out + off_rsv + 4, 0x80000000u); /* reserve 0x80000000 + 0x4000 */
    put32(w->out + off_rsv + 12, 0x4000u);
    memcpy(w->out + off_struct, w->st, w->sn);
    memcpy(w->out + off_strings, w->str, w->strn);
    w->outn = total;
    return total;
}

/* Synthetic tree: two-level bus translation, reg-names, phandles, NOPs. */
static void build_synth(struct wr *w, const char *const *compat, uint32_t ncompat, const char *aic_compat)
{
    memset(w, 0, sizeof(*w));
    char cl[256];
    uint32_t cn = 0;
    for (uint32_t i = 0; i < ncompat; i++) {
        memcpy(cl + cn, compat[i], strlen(compat[i]) + 1u);
        cn += (uint32_t)strlen(compat[i]) + 1u;
    }
    w_begin(w, "");
    w_u32(w, "#address-cells", 2);
    w_u32(w, "#size-cells", 2);
    w_prop(w, "compatible", cl, cn);
    w_str(w, "model", "Synthetic Apple test board");
    w_tok(w, 4); /* NOP between properties */
    w_begin(w, "cpus");
    w_u32(w, "#address-cells", 2);
    w_u32(w, "#size-cells", 0);
    for (uint32_t c = 0; c < 2; c++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "cpu@%x", c ? 0x10100u : 0u);
        w_begin(w, nm);
        w_str(w, "device_type", "cpu");
        uint32_t reg[2] = {0, c ? 0x10100u : 0u};
        w_cells(w, "reg", reg, 2);
        w_str(w, "enable-method", "spin-table");
        uint32_t rel[2] = {0x8, c ? 0x00001000u : 0u};
        w_cells(w, "cpu-release-addr", rel, 2);
        w_end(w);
    }
    w_end(w);
    w_begin(w, "soc");
    w_u32(w, "#address-cells", 2);
    w_u32(w, "#size-cells", 2);
    w_prop(w, "ranges", "", 0);
    w_begin(w, "bus@200000000");
    w_u32(w, "#address-cells", 1);
    w_u32(w, "#size-cells", 1);
    /* child 0x0..0x1000000 -> parent 0x2_0000_0000 */
    uint32_t rng[5] = {0x0, 0x2, 0x0, 0x1000000};
    w_cells(w, "ranges", rng, 4);
    w_begin(w, "interrupt-controller@100000");
    w_str(w, "compatible", aic_compat);
    uint32_t reg[4] = {0x100000, 0x8000, 0x108000, 0x4};
    w_cells(w, "reg", reg, 4);
    w_prop(w, "reg-names", "core\0event", 11);
    w_u32(w, "phandle", 7);
    w_prop(w, "interrupt-controller", "", 0);
    w_end(w);
    w_begin(w, "outside@2000000");
    uint32_t reg2[2] = {0x2000000, 0x10};
    w_cells(w, "reg", reg2, 2);
    w_end(w);
    w_end(w);
    w_end(w);
    w_begin(w, "chosen");
    w_str(w, "bootargs", "console=serial0");
    w_end(w);
    w_end(w);
    w_finish(w);
}

static const char *const synth_ok[] = {"apple,j999", "apple,t8112", "apple,arm-platform"};

static void test_synthetic_good(void)
{
    struct wr *w = calloc(1, sizeof(*w));
    build_synth(w, synth_ok, 3, "apple,aic2");
    struct ck_fdt f;
    CHECK(ck_fdt_open(&f, w->out, w->outn) == CK_FDT_OK);
    CHECK(f.nodes == 9);
    CHECK(f.boot_cpuid == 0);
    CHECK(strcmp(ck_fdt_name(&f, f.root), "") == 0);
    int cpu1 = ck_fdt_path(&f, "/cpus/cpu@10100");
    CHECK(cpu1 >= 0);
    CHECK(ck_fdt_path(&f, "/cpus/cpu") >= 0);       /* unit-less match picks the first */
    CHECK(ck_fdt_path(&f, "/cpus/cpu@2") == CK_FDT_E_NOTFOUND);
    CHECK(ck_fdt_path(&f, "/cpu") == CK_FDT_E_NOTFOUND); /* no prefix match */
    CHECK(ck_fdt_path(&f, "/") == f.root);
    CHECK(ck_fdt_parent(&f, cpu1) == ck_fdt_path(&f, "/cpus"));
    CHECK(ck_fdt_parent(&f, f.root) == CK_FDT_E_NOTFOUND);
    uint64_t a = 0, s = 0;
    CHECK(ck_fdt_reg(&f, cpu1, 0, &a, &s) == 0 && a == 0x10100 && s == 0);
    uint64_t rel = 0;
    CHECK(ck_fdt_u64(&f, cpu1, "cpu-release-addr", &rel) == 0 && rel == 0x800001000ull);
    int aic = ck_fdt_find_compatible(&f, -1, "apple,aic2");
    CHECK(aic >= 0);
    CHECK(ck_fdt_by_phandle(&f, 7) == aic);
    CHECK(ck_fdt_by_phandle(&f, 8) == CK_FDT_E_NOTFOUND);
    CHECK(ck_fdt_reg(&f, aic, 1, &a, &s) == 0 && a == 0x108000 && s == 4); /* bus address */
    CHECK(ck_fdt_reg_cpu(&f, aic, 0, &a, &s) == 0 && a == 0x200100000ull && s == 0x8000);
    CHECK(ck_fdt_reg_named(&f, aic, "event", &a, &s) == 0 && a == 0x200108000ull && s == 4);
    CHECK(ck_fdt_reg_named(&f, aic, "nope", &a, &s) == CK_FDT_E_NOTFOUND);
    CHECK(ck_fdt_reg(&f, aic, 2, &a, &s) == CK_FDT_E_RANGE);
    int outside = ck_fdt_path(&f, "/soc/bus/outside");
    CHECK(outside >= 0);
    CHECK(ck_fdt_reg_cpu(&f, outside, 0, &a, &s) == CK_FDT_E_RANGE); /* past the bus window */
    CHECK(strcmp(ck_fdt_string(&f, ck_fdt_path(&f, "/chosen"), "bootargs"), "console=serial0") == 0);
    CHECK(ck_fdt_string(&f, aic, "reg-names") == 0); /* two strings: not one string */
    CHECK(strcmp(ck_fdt_strlist_get(&f, aic, "reg-names", 1), "event") == 0);
    CHECK(ck_fdt_strlist_get(&f, aic, "reg-names", 2) == 0);
    CHECK(ck_fdt_rsv(&f, 0, &a, &s) == 0 && a == 0x80000000ull && s == 0x4000);
    CHECK(ck_fdt_rsv(&f, 1, &a, &s) == CK_FDT_E_NOTFOUND);
    uint32_t n = 0;
    for (int x = ck_fdt_next_node(&f, -1); x >= 0; x = ck_fdt_next_node(&f, x)) n++;
    CHECK(n == f.nodes);

    struct ck_apple_platform *p = calloc(1, sizeof(*p));
    CHECK(ck_apple_probe(&f, p) == CK_APPLE_OK);
    CHECK(p->soc && p->soc->soc_id == 0x8112);
    CHECK(p->aic_base == 0x200100000ull && p->aic_event == 0x200108000ull);
    CHECK(p->ncpus == 2 && p->cpu[1].mpidr == 0x10100 && p->cpu[1].release == 0x800001000ull && p->cpu[0].release == 0x800000000ull);
    char line[256];
    ck_apple_report(p, line, sizeof(line));
    CHECK(strstr(line, "soc=t8112") && strstr(line, "aic=v2") && strstr(line, "idle=wfi-after-retention"));
    free(p);
    free(w);
}

static void test_family_refusals(void)
{
    struct wr *w = calloc(1, sizeof(*w));
    struct ck_apple_platform *p = calloc(1, sizeof(*p));
    struct ck_fdt f;
    char line[256];

    static const char *const m5[] = {"apple,j999", "apple,t8142", "apple,arm-platform"};
    build_synth(w, m5, 3, "apple,t8122-aic3");
    CHECK(ck_fdt_open(&f, w->out, w->outn) == 0);
    CHECK(ck_apple_probe(&f, p) == CK_APPLE_UNKNOWN_SOC);
    ck_apple_report(p, line, sizeof(line));
    CHECK(strstr(line, "apple: REFUSED unknown Apple SoC") && strstr(line, "\"apple,t8142\"") &&
          strstr(line, "Apple drivers not started"));

    /* t8112 row (AIC v2) but the tree only has an AIC v3 node. */
    build_synth(w, synth_ok, 3, "apple,t8122-aic3");
    CHECK(ck_fdt_open(&f, w->out, w->outn) == 0);
    CHECK(ck_apple_probe(&f, p) == CK_APPLE_MISMATCH);
    ck_apple_report(p, line, sizeof(line));
    CHECK(strstr(line, "REFUSED apple,t8112 row wants interrupt controller \"apple,aic2\""));

    static const char *const notapple[] = {"linux,dummy-virt"};
    build_synth(w, notapple, 1, "apple,aic2");
    CHECK(ck_fdt_open(&f, w->out, w->outn) == 0);
    CHECK(ck_apple_probe(&f, p) == CK_APPLE_NOT_APPLE);

    /* "apple,t8112" without "apple,arm-platform" is not taken as Apple. */
    static const char *const half[] = {"apple,t8112"};
    build_synth(w, half, 1, "apple,aic2");
    CHECK(ck_fdt_open(&f, w->out, w->outn) == 0);
    CHECK(ck_apple_probe(&f, p) == CK_APPLE_NOT_APPLE);

    CHECK(ck_apple_probe(0, p) == CK_APPLE_NOT_APPLE);
    CHECK(ck_apple_soc_by_compat("apple,t8142") == 0);
    CHECK(ck_apple_soc_by_compat("apple,t811") == 0);
    CHECK(ck_apple_soc_by_compat("apple,t81122") == 0);
    free(p);
    free(w);
}

/* One broken variant per ck_fdt_open check. */
static void test_malformed(void)
{
    struct wr *w = calloc(1, sizeof(*w));
    struct ck_fdt f;
    uint8_t *b = malloc(sizeof(w->out));
    build_synth(w, synth_ok, 3, "apple,aic2");
    uint32_t n = w->outn, off_struct = 72, off_strings = 72 + w->sn;
#define MUT(stmt, avail, want)                                                     \
    do {                                                                           \
        memcpy(b, w->out, n);                                                      \
        stmt;                                                                      \
        int rc_ = ck_fdt_open(&f, b, (avail));                                     \
        if (rc_ != (want)) printf("  mutant '%s': got %d want %d\n", #stmt, rc_, (want)); \
        CHECK(rc_ == (want));                                                      \
        CHECK(rc_ == 0 || f.blob == 0);                                            \
    } while (0)
    MUT((void)0, n, CK_FDT_OK);
    MUT((void)0, 39, CK_FDT_E_SHORT);
    MUT((void)0, n - 1, CK_FDT_E_SHORT);
    MUT(b[0] = 0xd1, n, CK_FDT_E_MAGIC);
    MUT(put32(b + 20, 16), n, CK_FDT_E_VERSION);
    MUT(put32(b + 24, 18), n, CK_FDT_E_VERSION);
    MUT(put32(b + 8, off_struct + 2), n, CK_FDT_E_LAYOUT);           /* misaligned struct */
    MUT(put32(b + 36, w->sn + 4), n, CK_FDT_E_LAYOUT);               /* struct past total */
    MUT(put32(b + 12, off_struct + 8), n, CK_FDT_E_LAYOUT);          /* strings overlap struct */
    MUT(put32(b + 16, 44), n, CK_FDT_E_LAYOUT);                      /* rsv misaligned */
    MUT(put32(b + 16, off_struct - 8), n, CK_FDT_E_RSV);             /* rsv runs into struct, no terminator */
    MUT((put32(b + 12, 40), put32(b + 32, 16)), n, CK_FDT_E_LAYOUT); /* strings block over the rsv block */
    MUT(put32(b + 4, 0xffffffffu), 0xffffffffu, CK_FDT_E_LAYOUT);    /* totalsize past 8 MiB */
    MUT(put32(b + off_struct, 7), n, CK_FDT_E_TOKEN);                /* unknown token */
    MUT(put32(b + off_struct + w->sn - 4, 4), n, CK_FDT_E_TOKEN);    /* FDT_END replaced by NOP */
    MUT(put32(b + off_struct + w->sn - 8, 9), n, CK_FDT_E_DEPTH);    /* END before root closes */
    MUT(put32(b + off_struct + 4, 0x41424344u), n, CK_FDT_E_ROOT);   /* root name not empty */
    MUT(put32(b + off_struct + 12, 0xffffffu), n, CK_FDT_E_PROP);    /* first prop len huge */
    MUT(put32(b + off_struct + 16, 0x7ffff), n, CK_FDT_E_PROP);      /* nameoff past strings */
    MUT(b[off_strings + w->strn - 1] = 'x', n, CK_FDT_E_PROP);      /* last name unterminated */
    free(b);

    /* Structural mutants that need a rebuilt tree. */
    struct wr *v = calloc(1, sizeof(*v));
    w_begin(v, ""); w_end(v); w_begin(v, ""); w_end(v); w_finish(v);
    CHECK(ck_fdt_open(&f, v->out, v->outn) == CK_FDT_E_ROOT); /* two roots */
    memset(v, 0, sizeof(*v));
    w_begin(v, "");
    for (int i = 0; i < CK_FDT_MAX_DEPTH; i++) w_begin(v, "n");
    for (int i = 0; i < CK_FDT_MAX_DEPTH; i++) w_end(v);
    w_end(v);
    w_finish(v);
    CHECK(ck_fdt_open(&f, v->out, v->outn) == CK_FDT_E_DEPTH); /* 33 levels */
    memset(v, 0, sizeof(*v));
    w_begin(v, "");
    for (int i = 0; i < CK_FDT_MAX_DEPTH - 1; i++) w_begin(v, "n");
    for (int i = 0; i < CK_FDT_MAX_DEPTH - 1; i++) w_end(v);
    w_end(v);
    w_finish(v);
    CHECK(ck_fdt_open(&f, v->out, v->outn) == CK_FDT_OK); /* exactly 32 levels */
    memset(v, 0, sizeof(*v));
    w_begin(v, ""); w_begin(v, "a"); w_end(v); w_u32(v, "late", 1); w_end(v); w_finish(v);
    CHECK(ck_fdt_open(&f, v->out, v->outn) == CK_FDT_E_PROP); /* property after a subnode */
    memset(v, 0, sizeof(*v));
    w_begin(v, ""); w_begin(v, ""); w_end(v); w_end(v); w_finish(v);
    CHECK(ck_fdt_open(&f, v->out, v->outn) == CK_FDT_E_NAME); /* empty child name */
    memset(v, 0, sizeof(*v));
    w_begin(v, ""); w_end(v); w_end(v); w_finish(v);
    CHECK(ck_fdt_open(&f, v->out, v->outn) == CK_FDT_E_DEPTH); /* unbalanced END_NODE */
    memset(v, 0, sizeof(*v));
    w_begin(v, ""); w_end(v); w_finish(v);
    memset(v->out + 40, 0xff, 32); /* reservation block never terminates before the struct */
    CHECK(ck_fdt_open(&f, v->out, v->outn) == CK_FDT_E_LAYOUT || ck_fdt_open(&f, v->out, v->outn) == CK_FDT_E_RSV);
    CHECK(ck_fdt_open(&f, 0, 100) == CK_FDT_E_SHORT);
    CHECK(strcmp(ck_fdt_strerror(CK_FDT_E_DEPTH), "depth") == 0);
    free(v);
    free(w);
#undef MUT
}

struct expect {
    const char *file;
    uint32_t soc_id, aic, ncpus;
    uint64_t aic_base, aic_event;
    uint8_t usbc, idle;
    const char *model;
};

/* From `dtc -I dtb -O dts` of each fixture (see the file comment). */
static const struct expect apple[] = {
    {"t8103-j313.dtb", 0x8103, 1, 8, 0x23b100000ull, 0x23b100000ull, CK_APPLE_USBC_CD3217_I2C, CK_APPLE_IDLE_WFI_RETAINED, "Apple MacBook Air (M1, 2020)"},
    {"t8112-j413.dtb", 0x8112, 2, 8, 0x23b0c0000ull, 0x23b0c8000ull, CK_APPLE_USBC_CD3217_I2C, CK_APPLE_IDLE_WFI_RETAINED, "Apple MacBook Air (13-inch, M2, 2022)"},
    {"t8122-j613.dtb", 0x8122, 3, 8, 0x2d1000000ull, 0x2d1040000ull, CK_APPLE_USBC_ACE3_SPMI, CK_APPLE_IDLE_WFI_RETAINED, "Apple MacBook Air (13-inch, M3, 2024)"},
    {"t8132-j713.dtb", 0x8132, 3, 10, 0x381000000ull, 0x381040000ull, CK_APPLE_USBC_NONE, CK_APPLE_IDLE_SPIN, "Apple MacBook Air (13-inch, M4, 2024)"},
    {"t6000-j314s.dtb", 0x6000, 2, 10, 0x28e100000ull, 0x28e10c000ull, CK_APPLE_USBC_CD3217_I2C, CK_APPLE_IDLE_WFI_RETAINED, "Apple MacBook Pro (14-inch, M1 Pro, 2021)"},
    {"t6020-j414s.dtb", 0x6020, 2, 12, 0x28e100000ull, 0x28e10c000ull, CK_APPLE_USBC_CD3217_I2C, CK_APPLE_IDLE_WFI_RETAINED, "Apple MacBook Pro (14-inch, M2 Pro, 2023)"},
    {"t6031-j514c.dtb", 0x6031, 3, 16, 0x292400000ull, 0x292440000ull, CK_APPLE_USBC_ACE3_SPMI, CK_APPLE_IDLE_WFI_RETAINED, "Apple MacBook Pro (14-inch, M3 Max, 16 CPU cores, Nov 2023)"},
};

static void test_apple_fixtures(void)
{
    struct ck_apple_platform *p = calloc(1, sizeof(*p));
    for (size_t i = 0; i < sizeof(apple) / sizeof(apple[0]); i++) {
        const struct expect *e = &apple[i];
        size_t len = 0;
        uint8_t *b = load(e->file, &len);
        CHECK(b != 0);
        if (!b) continue;
        struct ck_fdt f;
        int rc = ck_fdt_open(&f, b, len);
        CHECK(rc == CK_FDT_OK);
        if (rc) {
            printf("  %s: open %s\n", e->file, ck_fdt_strerror(rc));
            free(b);
            continue;
        }
        rc = ck_apple_probe(&f, p);
        char line[256];
        ck_apple_report(p, line, sizeof(line));
        printf("  %s: %s\n", e->file, line);
        CHECK(rc == CK_APPLE_OK);
        CHECK(p->soc && p->soc->soc_id == e->soc_id);
        CHECK(p->soc && p->soc->aic == e->aic);
        CHECK(p->soc && p->soc->idle == e->idle);
        CHECK(p->ncpus == e->ncpus);
        CHECK(p->aic_base == e->aic_base);
        CHECK(p->aic_event == e->aic_event);
        CHECK(p->usbc == e->usbc);
        if (e->model) CHECK(strcmp(p->model, e->model) == 0);
        /* The loader fills cpu-release-addr; raw trees carry zero. */
        for (uint32_t c = 0; c < p->ncpus; c++) CHECK(p->cpu[c].release == 0 && p->cpu[c].spin_table);
        /* First cpu is MPIDR 0 (cluster 0 core 0) on every pinned tree. */
        CHECK(p->ncpus && p->cpu[0].mpidr == 0);
        /* Every node reachable in document order is reachable from its parent. */
        uint32_t walked = 0;
        for (int x = ck_fdt_next_node(&f, -1); x >= 0; x = ck_fdt_next_node(&f, x)) {
            walked++;
            if (x != f.root) {
                int par = ck_fdt_parent(&f, x), c = ck_fdt_first_child(&f, par), seen = 0;
                for (; c >= 0; c = ck_fdt_next_sibling(&f, c))
                    if (c == x) seen = 1;
                if (!seen) {
                    CHECK(seen);
                    break;
                }
            }
        }
        CHECK(walked == f.nodes);
        free(b);
    }
    free(p);
}

static void test_qemu_virt(void)
{
    size_t len = 0;
    uint8_t *b = load("qemu-virt.dtb", &len);
    CHECK(b != 0);
    if (!b) return;
    struct ck_fdt f;
    CHECK(ck_fdt_open(&f, b, len) == 0);
    int chosen = ck_fdt_path(&f, "/chosen");
    const char *out = ck_fdt_string(&f, chosen, "stdout-path");
    CHECK(out && strcmp(out, "/pl011@9000000") == 0);
    int uart = out ? ck_fdt_path(&f, out) : -1;
    uint64_t a = 0, s = 0;
    CHECK(ck_fdt_reg_cpu(&f, uart, 0, &a, &s) == 0 && a == 0x9000000 && s == 0x1000);
    CHECK(ck_fdt_compatible(&f, uart, "arm,pl011"));
    CHECK(ck_fdt_reg_cpu(&f, ck_fdt_path(&f, "/memory"), 0, &a, &s) == 0 && a == 0x40000000ull &&
          s == 0x40000000ull);
    struct ck_apple_platform *p = calloc(1, sizeof(*p));
    CHECK(ck_apple_probe(&f, p) == CK_APPLE_NOT_APPLE);
    char line[256];
    ck_apple_report(p, line, sizeof(line));
    printf("  qemu-virt.dtb: %s\n", line);
    CHECK(strcmp(line, "apple: REFUSED not an Apple platform (root compatible \"linux,dummy-virt\"); Apple "
                       "drivers not started") == 0);
    free(p);
    free(b);
}

/* Deterministic byte flips over a real tree: open either refuses or yields a
 * tree every accessor walks without leaving the blob (ASan run: sanitize). */
static void test_flips(void)
{
    size_t len = 0;
    uint8_t *orig = load("t8112-j413.dtb", &len);
    CHECK(orig != 0);
    if (!orig) return;
    uint8_t *b = malloc(len);
    struct ck_apple_platform *p = calloc(1, sizeof(*p));
    uint32_t x = 0x12345678u, opened = 0, refused = 0;
    for (int it = 0; it < 3000; it++) {
        memcpy(b, orig, len);
        for (int k = 0; k < 4; k++) {
            x = x * 1103515245u + 12345u;
            uint32_t pos = (x >> 8) % (uint32_t)(it < 1500 ? 512u : len);
            x = x * 1103515245u + 12345u;
            b[pos] ^= (uint8_t)(1u << ((x >> 16) & 7u));
        }
        struct ck_fdt f;
        if (ck_fdt_open(&f, b, len) != 0) {
            refused++;
            continue;
        }
        opened++;
        for (int n = ck_fdt_next_node(&f, -1); n >= 0; n = ck_fdt_next_node(&f, n)) {
            uint64_t a, s;
            (void)ck_fdt_reg_cpu(&f, n, 0, &a, &s);
            (void)ck_fdt_string(&f, n, "compatible");
        }
        ck_apple_probe(&f, p);
        char line[256];
        ck_apple_report(p, line, sizeof(line));
    }
    printf("  flips: %u opened, %u refused\n", opened, refused);
    CHECK(opened + refused == 3000 && refused > 0);
    free(p);
    free(b);
    free(orig);
}

static void test_table(void)
{
    uint32_t rows = 0;
    for (const struct ck_apple_soc *s; (s = ck_apple_soc_at(rows)); rows++) {
        CHECK(s->aic >= 1 && s->aic <= 3);
        CHECK(s->start == CK_APPLE_START_SPIN_TABLE);
        CHECK(s->wfxt_ok == (s->gen < 4) && s->ctrr_hole == (s->gen >= 4));
        CHECK(s->idle == (s->gen >= 4 ? CK_APPLE_IDLE_SPIN : CK_APPLE_IDLE_WFI_RETAINED));
        CHECK(s->aic == (s->gen >= 3 ? 3 : s->soc_id == 0x8103 ? 1 : 2));
        CHECK(ck_apple_soc_by_compat(s->compat) == s);
    }
    CHECK(rows == 14);
}

int main(void)
{
    test_synthetic_good();
    test_family_refusals();
    test_malformed();
    test_apple_fixtures();
    test_qemu_virt();
    test_table();
    /* Last: flips walk whatever open accepts, so a broken open can crash here;
     * earlier failures are reported first. */
    if (ck_t_fail) printf("  flips skipped: earlier checks failed\n");
    else test_flips();
    return ck_t_verdict("test_apple_fdt");
}
