/* hda_fence.c -- HD Audio DMA fence (see hda_fence.h). Freestanding. */
#include "hda_fence.h"
#include "ck.h"
#include "hda.h"

#if defined(CK_TEST_HDA_MUTATION) && defined(CK_HARDWARE_STAGING)
#error "CK_TEST_HDA_MUTATION (TEST-ONLY HD Audio DMA fence mutation) cannot be combined with CK_HARDWARE_STAGING"
#endif
#if defined(CK_TEST_HDA_MUTATION) && defined(CK_QEMU_UNSAFE_DMA)
#error "CK_TEST_HDA_MUTATION cannot be combined with CK_QEMU_UNSAFE_DMA"
#endif
#if defined(CK_HDA_TONE) && CK_HDA_TONE && defined(CK_HARDWARE_STAGING)
#error "CK_HDA_TONE (QEMU test tone) cannot be combined with CK_HARDWARE_STAGING"
#endif
#if defined(CK_HDA_TONE) && CK_HDA_TONE && defined(CK_QEMU_UNSAFE_DMA)
#error "CK_HDA_TONE cannot be combined with CK_QEMU_UNSAFE_DMA"
#endif
#if defined(CK_TEST_HDA_MUTATION)
#if CK_TEST_HDA_MUTATION < 1 || CK_TEST_HDA_MUTATION > 3
#error "CK_TEST_HDA_MUTATION must be 1..3 (no-revoke, grant-no-smmu, wrong-tone)"
#endif
#define HDA_MUT CK_TEST_HDA_MUTATION
#else
#define HDA_MUT 0
#endif
#if defined(CK_HDA_TONE) && CK_HDA_TONE
#define HDA_TONE 1
#else
#define HDA_TONE 0
#endif

#define CMD_BM 0x4u

typedef struct {
    const pci_func *pf;
    int plat;      /* ACPI platform controller (no PCI function, no bus-master bit) */
    char name[48]; /* its ACPI name (platform only) */
    volatile uint8_t *bar;
    int bm_on, confined, mapped;
    uint32_t stream_id;
    const char *state;
} fence_t;

static fence_t g_f;
static ck_hda g_hda;
static int g_hda_init; /* g_hda holds a controller that ck_hda_shutdown must reset */
static uint8_t *g_dma_mem;
static uint64_t g_dma_phys;

int ck_hda_live(void) { return (g_f.pf || g_f.plat) && (g_f.bm_on || g_f.confined); }
const char *ck_hda_state(void) { return g_f.state ? g_f.state : "absent"; }

static uint32_t bus_rd(void *ctx, uint32_t off, unsigned width)
{
    volatile uint8_t *b = ((fence_t *)ctx)->bar;
    switch (width) {
    case 1: return *(volatile uint8_t *)(b + off);
    case 2: return *(volatile uint16_t *)(b + off);
    default: return *(volatile uint32_t *)(b + off);
    }
}

static void bus_wr(void *ctx, uint32_t off, unsigned width, uint32_t v)
{
    volatile uint8_t *b = ((fence_t *)ctx)->bar;
    switch (width) {
    case 1: *(volatile uint8_t *)(b + off) = (uint8_t)v; break;
    case 2: *(volatile uint16_t *)(b + off) = (uint16_t)v; break;
    default: *(volatile uint32_t *)(b + off) = v; break;
    }
}

void ck_hda_release(void)
{
    fence_t *x = &g_f;
    if (!x->pf && !x->plat) return;
    if (g_hda_init && x->mapped) {
        /* Stream stopped and the controller held in reset while it can still DMA. */
        int rc = ck_hda_shutdown(&g_hda);
        g_hda_init = 0;
        ck_printf("hda: shutdown stream stopped, controller in reset: %s (rc=%d)\n", rc == 0 ? "yes" : "NO", rc);
    }
    if (x->plat && x->bm_on) {
        x->bm_on = 0;
        ck_printf("dma_gate: hda %s quiesced\n", x->name);
    }
    if (x->pf && x->bm_on) {
#if HDA_MUT == CK_HDA_MUT_NO_REVOKE
        /* TEST-ONLY mutation: bus mastering is left on. */
        int off = (pci_r16(x->pf->cfg, 0x04) & CMD_BM) ? -1 : 0;
#else
        int off = pci_bus_master_off(x->pf);
#endif
        if (off == 0) {
            x->bm_on = 0;
            ck_printf("dma_gate: hda bus master revoked\n");
        } else /* bm_on stays set: the reset quiesce hook retries the revoke */
            ck_printf("dma_gate: hda bus master revoke FAILED (command register still has BME)\n");
        uint16_t c = pci_r16(x->pf->cfg, 0x04);
        ck_printf("hda_pci: after phase command=0x%04x bus_master=%s\n", c, (c & CMD_BM) ? "on" : "off");
    }
    if (x->confined) {
        int urc = ck_dma_unconfine(x->stream_id);
        x->confined = 0;
        ck_printf("smmu: hda stream 0x%x %s (rc=%d)\n", x->stream_id, urc == 0 ? "returned to abort" : "abort FAILED", urc);
    }
}

/* The driver phase, inside the confined grant. */
static int phase(void)
{
    ck_hda *h = &g_hda;
    ck_hda_bus bus = { bus_rd, bus_wr, &g_f };
    int rc = ck_hda_init(h, bus, g_dma_mem, g_dma_phys);
    g_hda_init = 1; /* even a failed bring-up leaves the controller to be reset */
    if (rc) {
        ck_printf("hda: init failed rc=%d\n", rc);
        return rc;
    }
    ck_printf("hda: controller version %u.%u gcap=0x%04x oss=%u iss=%u bss=%u addr64=%s codecs=0x%04x corb=%u rirb=%u\n",
              h->vmaj, h->vmin, h->gcap, h->oss, h->iss, h->bss, (h->gcap & 1u) ? "yes" : "no", h->statests, h->corb_n,
              h->rirb_n);
    rc = ck_hda_enumerate(h);
    if (rc) {
        ck_printf("hda: enumerate failed rc=%d\n", rc);
        return rc;
    }
    for (unsigned c = 0; c < CK_HDA_MAX_CODECS; c++) {
        const ck_hda_codec *k = &h->codec[c];
        if (!k->present) continue;
        ck_printf("hda: codec %u vendor=0x%08x revision=0x%08x subsystem=0x%08x afg=%u widgets=%u\n", c, k->vendor,
                  k->revision, k->subsystem, k->afg, k->wn);
        for (unsigned i = 0; i < k->wn; i++) {
            const ck_hda_widget *w = &h->w[k->wfirst + i];
            if (w->type == CK_HDA_W_PIN)
                ck_printf("hda: widget %u.%u pin caps=0x%08x pincap=0x%08x config=0x%08x conns=%u\n", c, w->nid, w->caps,
                          w->pincap, w->pincfg, w->nconn);
            else if (w->type == CK_HDA_W_OUT || w->type == CK_HDA_W_IN)
                ck_printf("hda: widget %u.%u %s caps=0x%08x rates=0x%04x formats=0x%08x\n", c, w->nid,
                          ck_hda_wtype_name(w->type), w->caps, w->rates, w->bits);
            else
                ck_printf("hda: widget %u.%u %s caps=0x%08x conns=%u\n", c, w->nid, ck_hda_wtype_name(w->type), w->caps,
                          w->nconn);
        }
    }
    rc = ck_hda_pick_output(h);
    if (rc) {
        ck_printf("hda: no output path (rc=%d)\n", rc);
        return rc;
    }
    ck_printf("hda: output codec=%d pin=%u dac=%u path=%u widgets\n", h->out_codec, h->pin, h->dac, h->npath);
#if HDA_TONE
    {
#if HDA_MUT == CK_HDA_MUT_WRONG_TONE
        unsigned hz = 1250; /* TEST-ONLY: the log below still says CK_HDA_TONE_HZ */
#else
        unsigned hz = CK_HDA_TONE_HZ;
#endif
        uint64_t want = (uint64_t)CK_HDA_TONE_RATE * 4u * CK_HDA_TONE_MS / 1000u;
        rc = ck_hda_play_tone(h, hz, CK_HDA_TONE_RATE, CK_HDA_TONE_AMP);
        if (rc) {
            ck_printf("hda: tone failed rc=%d\n", rc);
            return rc;
        }
        ck_printf("hda: tone start freq=%u rate=%u channels=2 bits=16 amp=%d ms=%u\n", CK_HDA_TONE_HZ, CK_HDA_TONE_RATE,
                  CK_HDA_TONE_AMP, (unsigned)CK_HDA_TONE_MS);
        uint64_t t0 = ck_time_us(), pos = 0;
        while ((pos = ck_hda_position(h)) < want && ck_time_us() - t0 < 4u * CK_HDA_TONE_MS * 1000u) ck_udelay(500);
        uint64_t dt = ck_time_us() - t0;
        ck_printf("hda: tone played bytes=%llu us=%llu nominal_bytes_per_s=%u\n", (unsigned long long)pos,
                  (unsigned long long)dt, CK_HDA_TONE_RATE * 4u);
        (void)ck_hda_stop(h);
        ck_printf("hda: tone done\n");
        if (pos < want) {
            ck_printf("hda: tone short (%llu of %llu bytes before the wall-clock limit)\n", (unsigned long long)pos,
                      (unsigned long long)want);
            return CK_HDA_E_STREAM;
        }
    }
#endif
    return CK_HDA_OK;
}

/* Common tail: DMA region, SMMU window, enable, run the phase, release. */
static int run(const char *what, int (*confine)(struct ck_dma_confinement *cf), const char *name_for_log)
{
    fence_t *x = &g_f;
    (void)what;
    (void)name_for_log;
    if (!g_dma_mem) g_dma_mem = ck_dma_alloc(CK_HDA_DMA_BYTES, 4096, &g_dma_phys);
    if (!g_dma_mem) {
        x->state = "dma-alloc-failed";
        ck_printf("hda: unavailable (DMA allocation of %u bytes failed)\n", (unsigned)CK_HDA_DMA_BYTES);
        return CK_HDAF_E_DMA;
    }
    struct ck_dma_confinement cf = { 0, 0, 0, 0 };
    int src = confine(&cf);
    if (src == 0) {
        x->confined = 1;
        x->stream_id = cf.stream_id;
        ck_printf("smmu_dma_window: hda only, translation active iova=0x%llx len=0x%llx stream=0x%x\n",
                  (unsigned long long)cf.iova, (unsigned long long)cf.len, cf.stream_id);
        ck_printf("dma_gate: hda granted (Confined) smmu=0x%llx stream=0x%x, bus master on\n",
                  (unsigned long long)cf.smmu_base, cf.stream_id);
    } else {
#if HDA_MUT == CK_HDA_MUT_GRANT_NO_SMMU
        /* TEST-ONLY mutation: DMA granted without confinement. */
        ck_printf("dma_gate: hda granted (TestMutationNoSmmu rc=%d), bus master on\n", src);
#else
        ck_printf("dma_gate: hda denied (%s), bus master stays off\n",
                  src == CK_SMMU_ABSENT ? "NoSmmu" : src == CK_SMMU_NOSTREAM ? "NoStream" : src == CK_SMMU_OTHER ? "OtherSmmu" : "SmmuNotReady");
        ck_printf("hda: unavailable (SMMU DMA isolation not active)\n");
        if (x->pf) {
            uint16_t c = pci_r16(x->pf->cfg, 0x04);
            ck_printf("hda_pci: after deny command=0x%04x bus_master=%s\n", c, (c & CMD_BM) ? "on" : "off");
        }
        x->state = "denied";
        return CK_HDAF_E_DENIED;
#endif
    }
    if (x->pf) {
        x->bar = (volatile uint8_t *)ck_mmio_map(x->pf->bar[0].addr, (size_t)x->pf->bar[0].size);
        pci_enable(x->pf, 1);
    }
    x->bm_on = 1;
    x->mapped = 1;
    ck_printf("hda: dma_window=0x%llx+0x%x\n", (unsigned long long)g_dma_phys, (unsigned)CK_HDA_DMA_BYTES);
    int rc = phase();
    x->state = rc ? "driver-failed" : "fenced";
    ck_hda_release();
    return rc ? CK_HDAF_E_DRIVER : CK_HDAF_OK;
}

static uint32_t g_rid;
static uint16_t g_seg;
static int confine_pci(struct ck_dma_confinement *cf) { return ck_dma_confine(g_seg, g_rid, g_dma_phys, CK_HDA_DMA_BYTES, cf); }
static int confine_named(struct ck_dma_confinement *cf) { return ck_dma_confine_named(g_f.name, g_dma_phys, CK_HDA_DMA_BYTES, cf); }

static void reset_state(void)
{
    fence_t *x = &g_f;
    x->pf = 0;
    x->plat = 0;
    x->name[0] = 0;
    x->bar = 0;
    x->bm_on = x->confined = x->mapped = 0;
    x->stream_id = 0;
    x->state = "absent";
    g_hda_init = 0;
}

int ck_hda_fence(const pci_system *pci)
{
    fence_t *x = &g_f;
    if (ck_hda_live()) {
        ck_printf("hda: unavailable (previous HD Audio fence still live; release it first)\n");
        return CK_HDAF_E_ARG;
    }
#if HDA_MUT
    ck_printf("WARNING: TEST-ONLY HD AUDIO MUTATION BUILD (CK_TEST_HDA_MUTATION=%d, QEMU gate self-test only, never a PASS)\n", HDA_MUT);
#endif
    reset_state();
    const pci_func *f = pci ? pci_find_class(pci, CK_HDA_PCI_CLASS, 0xffff00u) : 0;
    if (f) {
        const pci_bar *b = &f->bar[0];
        x->pf = f;
        ck_printf("hda: pci %04x:%02x:%02x.%u vendor=0x%04x device=0x%04x mmio 0x%llx\n", pci->ecam.segment, f->bus, f->dev,
                  f->fn, f->vendor, f->device, (unsigned long long)b->addr);
        uint16_t command = pci_r16(f->cfg, 0x04);
        ck_printf("hda_pci: command=0x%04x bus_master=%s\n", command, (command & CMD_BM) ? "on" : "off");
        if (command & CMD_BM) {
            x->state = "bme-stuck";
            ck_printf("hda: unavailable (bus master would not clear)\n");
            return CK_HDAF_E_BME;
        }
        if (b->io || b->addr == 0 || b->size < 0x1000u) {
            x->state = "bar-unusable";
            ck_printf("hda: unavailable (BAR0 unusable)\n");
            return CK_HDAF_E_ARG;
        }
        g_seg = f->segment;
        g_rid = ((uint32_t)f->bus << 8) | ((uint32_t)f->dev << 3) | (uint32_t)f->fn;
        return run("pci", confine_pci, 0);
    }
    /* No PCI HDA: the ACPI platform device (the DGX Spark has NVDA2014). */
    static const char *const ids[] = { CK_HDA_ACPI_ID };
    struct ck_platform_dev p[2];
    struct ck_acpi_scan_info info;
    int n = ck_acpi_platform_devices(ids, 1, p, 2, &info);
    ck_printf("hda_acpi: %d platform controller(s) (id %s)\n", n < 0 ? 0 : n, CK_HDA_ACPI_ID);
    if (n <= 0) {
        ck_printf("hda: unavailable (no HD Audio controller)\n");
        return CK_HDAF_ABSENT;
    }
    for (int i = 0; i < n && i < 2; i++)
        ck_printf("hda_acpi: %s hid=%s table=%s mmio=0x%llx+0x%llx\n", p[i].name, p[i].hid[0] ? p[i].hid : "-", p[i].table,
                  (unsigned long long)p[i].mmio_base, (unsigned long long)p[i].mmio_len);
    if (n > 1) {
        x->state = "ambiguous";
        ck_printf("hda: unavailable (more than one %s device)\n", CK_HDA_ACPI_ID);
        return CK_HDAF_E_ARG;
    }
    unsigned k = 0;
    for (; k + 1 < sizeof x->name && p[0].name[k]; k++) x->name[k] = p[0].name[k];
    x->name[k] = 0;
    if (!p[0].mmio_base || p[0].mmio_len < 0x1000u || (p[0].mmio_base & 0xfffu)) {
        x->state = "bar-unusable";
        ck_printf("hda: unavailable (no usable _CRS memory range)\n");
        return CK_HDAF_E_ARG;
    }
    volatile uint8_t *bar = (volatile uint8_t *)ck_mmio_try_map(p[0].mmio_base, (size_t)p[0].mmio_len);
    if (!bar) {
        x->state = "bar-unusable";
        ck_printf("hda: unavailable (memory range not mappable as MMIO)\n");
        return CK_HDAF_E_ARG;
    }
    x->bar = bar;
    x->plat = 1;
    return run("platform", confine_named, x->name);
}
