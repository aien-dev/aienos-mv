/* apple_family.c -- Apple Silicon family table and platform probe (see
 * apple_family.h). Row facts: AsahiLinux/linux 94fb2334 device trees
 * (root compatible, interrupt controller compatible, cpu node count);
 * idle policy per docs/APPLE_SILICON_PLATFORM.md section "Family table". */
#include "apple_family.h"
#include "fmt.h"

#if defined(CK_APPLE_MUTANT_GUESS) && !defined(CK_APPLE_HOST_TEST)
#error "CK_APPLE_MUTANT_GUESS is a host test mutant"
#endif

#define U(k) (uint8_t)(1u << (k))
#define I2C U(CK_APPLE_USBC_CD3217_I2C)
#define SPMI U(CK_APPLE_USBC_ACE3_SPMI)
#define WFI CK_APPLE_IDLE_WFI_RETAINED
#define SPIN CK_APPLE_IDLE_SPIN
#define ST CK_APPLE_START_SPIN_TABLE
#define P16K 16384u

static const struct ck_apple_soc table[] = {
    /* compat         id      name        gen aic idle start dies usbc       cpus dart  aic node compatible */
    {"apple,t8103", 0x8103, "M1", 1, 1, WFI, ST, 1, I2C, 8, P16K, "apple,t8103-aic"},
    {"apple,t6000", 0x6000, "M1 Pro", 1, 2, WFI, ST, 1, I2C, 10, P16K, "apple,aic2"},
    {"apple,t6001", 0x6001, "M1 Max", 1, 2, WFI, ST, 1, I2C, 10, P16K, "apple,aic2"},
    {"apple,t6002", 0x6002, "M1 Ultra", 1, 2, WFI, ST, 2, I2C, 20, P16K, "apple,aic2"},
    {"apple,t8112", 0x8112, "M2", 2, 2, WFI, ST, 1, I2C, 8, P16K, "apple,aic2"},
    {"apple,t6020", 0x6020, "M2 Pro", 2, 2, WFI, ST, 1, I2C, 12, P16K, "apple,aic2"},
    {"apple,t6021", 0x6021, "M2 Max", 2, 2, WFI, ST, 1, I2C, 12, P16K, "apple,aic2"},
    {"apple,t6022", 0x6022, "M2 Ultra", 2, 2, WFI, ST, 2, I2C, 24, P16K, "apple,aic2"},
    {"apple,t8122", 0x8122, "M3", 3, 3, WFI, ST, 1, I2C | SPMI, 8, P16K, "apple,t8122-aic3"},
    {"apple,t6030", 0x6030, "M3 Pro", 3, 3, WFI, ST, 1, SPMI, 12, P16K, "apple,t8122-aic3"},
    {"apple,t6031", 0x6031, "M3 Max", 3, 3, WFI, ST, 1, SPMI, 16, P16K, "apple,t8122-aic3"},
    {"apple,t6034", 0x6034, "M3 Max (binned)", 3, 3, WFI, ST, 1, SPMI, 16, P16K, "apple,t8122-aic3"},
    {"apple,t6032", 0x6032, "M3 Ultra", 3, 3, WFI, ST, 2, 0, 32, P16K, "apple,t8122-aic3"},
    {"apple,t8132", 0x8132, "M4", 4, 3, SPIN, ST, 1, 0, 10, P16K, "apple,t8122-aic3"},
};

#define TABLE_ROWS (sizeof(table) / sizeof(table[0]))

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

const struct ck_apple_soc *ck_apple_soc_by_compat(const char *compat)
{
    if (!compat) return 0;
    for (uint32_t i = 0; i < TABLE_ROWS; i++)
        if (str_eq(table[i].compat, compat)) return &table[i];
    return 0;
}

const struct ck_apple_soc *ck_apple_soc_at(uint32_t i) { return i < TABLE_ROWS ? &table[i] : 0; }

const char *ck_apple_idle_name(uint8_t idle)
{
    return idle == CK_APPLE_IDLE_WFI_RETAINED ? "wfi-after-retention" : idle == CK_APPLE_IDLE_SPIN ? "spin" : "unknown";
}

const char *ck_apple_usbc_name(uint8_t usbc)
{
    switch (usbc) {
    case CK_APPLE_USBC_CD3217_I2C: return "cd3217-i2c";
    case CK_APPLE_USBC_ACE3_SPMI: return "ace3-spmi";
    default: return "none";
    }
}

/* Root compatible list as `"a","b"` into buf (bounded, for report lines). */
static void compat_list(const struct ck_fdt *f, char *buf, size_t n)
{
    size_t used = 0;
    buf[0] = 0;
    for (uint32_t i = 0; i < 6; i++) {
        const char *c = ck_fdt_strlist_get(f, f->root, "compatible", i);
        if (!c) break;
        int w = ck_snprintf(buf + used, n - used, "%s\"%s\"", i ? "," : "", c);
        if (w < 0 || (size_t)w >= n - used) break;
        used += (size_t)w;
    }
    if (!buf[0]) ck_snprintf(buf, n, "(none)");
}

static int refuse(struct ck_apple_platform *p, int verdict, const char *why)
{
    p->verdict = verdict;
    ck_snprintf(p->why, sizeof(p->why), "%s", why);
    return verdict;
}

static void probe_cpus(const struct ck_fdt *f, struct ck_apple_platform *p)
{
    int cpus = ck_fdt_path(f, "/cpus");
    for (int c = cpus >= 0 ? ck_fdt_first_child(f, cpus) : -1; c >= 0; c = ck_fdt_next_sibling(f, c)) {
        const char *dt = ck_fdt_string(f, c, "device_type");
        if (!dt || !str_eq(dt, "cpu")) continue;
        if (p->ncpus >= CK_APPLE_MAX_CPUS) break;
        struct ck_apple_cpu *cpu = &p->cpu[p->ncpus];
        uint64_t size;
        if (ck_fdt_reg(f, c, 0, &cpu->mpidr, &size) != 0) continue;
        const char *em = ck_fdt_string(f, c, "enable-method");
        cpu->spin_table = em && str_eq(em, "spin-table");
        cpu->release = 0;
        if (ck_fdt_u64(f, c, "cpu-release-addr", &cpu->release) != 0) cpu->release = 0;
        p->ncpus++;
    }
}

static uint8_t probe_usbc(const struct ck_fdt *f)
{
    if (ck_fdt_find_compatible(f, -1, "apple,sn201202x") >= 0) return CK_APPLE_USBC_ACE3_SPMI;
    if (ck_fdt_find_compatible(f, -1, "apple,cd321x") >= 0) return CK_APPLE_USBC_CD3217_I2C;
    return CK_APPLE_USBC_NONE;
}

int ck_apple_probe(const struct ck_fdt *f, struct ck_apple_platform *p)
{
    char list[112];
    p->verdict = CK_APPLE_NOT_APPLE;
    p->soc = 0;
    p->why[0] = 0;
    p->model = "";
    p->aic_base = p->aic_size = p->aic_event = p->aic_event_size = 0;
    p->ncpus = 0;
    p->usbc = CK_APPLE_USBC_NONE;
    if (!f || !f->blob) return refuse(p, CK_APPLE_NOT_APPLE, "no device tree");
    const char *model = ck_fdt_string(f, f->root, "model");
    if (model) p->model = model;
    compat_list(f, list, sizeof(list));

    if (!ck_fdt_compatible(f, f->root, "apple,arm-platform")) {
        char why[160];
        ck_snprintf(why, sizeof(why), "not an Apple platform (root compatible %s)", list);
        return refuse(p, CK_APPLE_NOT_APPLE, why);
    }
    /* The SoC row: the first root compatible entry that is a table row.
     * "apple,t...." entries that are not rows make the SoC unknown. */
    const struct ck_apple_soc *soc = 0;
    for (uint32_t i = 0; !soc; i++) {
        const char *c = ck_fdt_strlist_get(f, f->root, "compatible", i);
        if (!c) break;
        soc = ck_apple_soc_by_compat(c);
    }
#ifdef CK_APPLE_MUTANT_GUESS
    if (!soc) soc = &table[0]; /* guesses a chip: the test must catch it */
#endif
    if (!soc) {
        char why[160];
        ck_snprintf(why, sizeof(why), "unknown Apple SoC (root compatible %s; no family table row)", list);
        return refuse(p, CK_APPLE_UNKNOWN_SOC, why);
    }
    p->soc = soc;

    /* Cross-checks: the tree must agree with the row before any driver runs. */
    int aic = ck_fdt_find_compatible(f, -1, soc->aic_compat);
    if (aic < 0) {
        char why[160];
        ck_snprintf(why, sizeof(why), "%s row wants interrupt controller \"%s\"; tree has none", soc->compat,
                    soc->aic_compat);
        return refuse(p, CK_APPLE_MISMATCH, why);
    }
    /* A second controller generation in the same tree is a contradiction. */
    static const char *const other[] = {"apple,aic", "apple,aic2", "apple,t8122-aic3"};
    for (uint32_t v = 1; v <= 3; v++) {
        if (v == soc->aic) continue;
        if (ck_fdt_find_compatible(f, -1, other[v - 1u]) >= 0) {
            char why[160];
            ck_snprintf(why, sizeof(why), "%s row says AIC v%u but the tree also has \"%s\"", soc->compat, soc->aic,
                        other[v - 1u]);
            return refuse(p, CK_APPLE_MISMATCH, why);
        }
    }
    int rc = ck_fdt_reg_cpu(f, aic, 0, &p->aic_base, &p->aic_size);
    if (rc == 0 && soc->aic >= CK_APPLE_AIC_V2)
        rc = ck_fdt_reg_named(f, aic, "event", &p->aic_event, &p->aic_event_size);
    else if (rc == 0)
        p->aic_event = p->aic_base;
    if (rc != 0) {
        char why[160];
        ck_snprintf(why, sizeof(why), "%s interrupt controller registers unreadable (fdt %s)", soc->compat,
                    ck_fdt_strerror(rc));
        return refuse(p, CK_APPLE_MISMATCH, why);
    }

    probe_cpus(f, p);
    if (p->ncpus == 0 || p->ncpus > soc->max_cpus) {
        char why[160];
        ck_snprintf(why, sizeof(why), "%s tree has %u cpu nodes (row allows 1..%u)", soc->compat, p->ncpus,
                    soc->max_cpus);
        return refuse(p, CK_APPLE_MISMATCH, why);
    }
    for (uint32_t i = 0; i < p->ncpus; i++) {
        if (!p->cpu[i].spin_table) {
            char why[160];
            ck_snprintf(why, sizeof(why), "%s cpu mpidr 0x%llx enable-method is not spin-table", soc->compat,
                        (unsigned long long)p->cpu[i].mpidr);
            return refuse(p, CK_APPLE_MISMATCH, why);
        }
    }
    p->usbc = probe_usbc(f);
    p->verdict = CK_APPLE_OK;
    return CK_APPLE_OK;
}

int ck_apple_report(const struct ck_apple_platform *p, char *buf, size_t n)
{
    if (p->verdict != CK_APPLE_OK)
        return ck_snprintf(buf, n, "apple: REFUSED %s; Apple drivers not started", p->why);
    const struct ck_apple_soc *s = p->soc;
    uint32_t released = 0;
    for (uint32_t i = 0; i < p->ncpus; i++)
        if (p->cpu[i].release) released++;
    int unexpected = p->usbc != CK_APPLE_USBC_NONE && !(s->usbc_seen & (1u << p->usbc));
    return ck_snprintf(buf, n,
                       "apple: soc=t%04x name=\"%s\" gen=M%u aic=v%u aic_base=0x%llx aic_event=0x%llx "
                       "idle=%s start=spin-table cpus=%u released=%u dart_page=%u usbc=%s%s",
                       s->soc_id, s->name, s->gen, s->aic, (unsigned long long)p->aic_base,
                       (unsigned long long)p->aic_event, ck_apple_idle_name(s->idle), p->ncpus, released,
                       s->dart_page, ck_apple_usbc_name(p->usbc), unexpected ? "(unexpected)" : "");
}
