/* mt7925_bind.c -- MT7925 Wi-Fi binding (see mt7925_bind.h). Freestanding. */
#include "mt7925_bind.h"
#include "aienos_mt7925.h"
#include "ck.h"
#include <string.h>

static struct {
    pci_func fn;
    const pci_func *pf;
    volatile uint8_t *bar;
    mt7925_dev dev;
    int bm_on, confined, init_tried; /* init_tried: mt7925_init ran (it may have programmed rings before failing) */
    uint32_t stream_id;
} g_mt;

/* ck_dma_alloc memory is never freed: allocate once, reuse on a later bind. */
static uint8_t *g_mem;
static uint64_t g_phys;

static uint32_t o_rd(void *ctx, uint32_t off) { return *(volatile uint32_t *)((volatile uint8_t *)ctx + off); }
static void o_wr(void *ctx, uint32_t off, uint32_t v)
{
    ck_mb(); /* descriptors and buffers visible before the register write */
    *(volatile uint32_t *)((volatile uint8_t *)ctx + off) = v;
    ck_mb();
}
static void o_delay(void *ctx, uint32_t us) { (void)ctx; ck_udelay(us); }
static void o_barrier(void *ctx) { (void)ctx; ck_mb(); }

int ck_mt7925_live(void) { return g_mt.pf && (g_mt.bm_on || g_mt.confined); }

void ck_mt7925_release(void)
{
    if (!g_mt.pf) return;
    if (g_mt.bm_on) {
        /* Any chip mt7925_init touched is stopped, even when init failed half way. */
        if (g_mt.bar && g_mt.init_tried)
            ck_printf("mt7925: chip stop %s\n", mt7925_strerror(mt7925_stop(&g_mt.dev)));
        if (pci_bus_master_off(g_mt.pf) == 0) {
            g_mt.bm_on = 0;
            ck_printf("dma_gate: mt7925 bus master revoked\n");
        } else
            ck_printf("dma_gate: mt7925 bus master revoke FAILED (command register still has BME)\n");
    }
    if (g_mt.confined) {
        int urc = ck_dma_unconfine(g_mt.stream_id);
        g_mt.confined = 0;
        ck_printf("smmu: mt7925 stream 0x%x %s (rc=%d)\n", g_mt.stream_id, urc == 0 ? "returned to abort" : "abort FAILED",
                  urc);
    }
    g_mt.init_tried = 0;
}

int ck_mt7925_find(const pci_found *found, uint32_t n, pci_func *out)
{
    if (!out) return CK_MT_E_ARG;
    for (uint32_t i = 0; found && i < n; i++) {
        const pci_found *c = &found[i];
        if (c->vendor != MT7925_PCI_VENDOR || (c->device != MT7925_PCI_DEVICE && c->device != MT7925_PCI_DEVICE_ALT))
            continue;
        ck_printf("mt7925: found seg %04x %02x:%02x.%u rev 0x%02x\n", c->segment, c->bus, c->dev, c->fn, c->revision);
        pci_bus_access acc;
        if (pci_stage_disc_access(c->segment, c->bus, &acc) || pci_func_from_found(out, c, &acc)) {
            ck_printf("mt7925: unavailable (cannot read the function as an endpoint; nothing written)\n");
            return CK_MT_E_ARG;
        }
        return CK_MT_OK;
    }
    ck_printf("mt7925: absent (no 14c3:7925 function)\n");
    return CK_MT_ABSENT;
}

int ck_mt7925_probe(const pci_func *f)
{
    if (!f) return CK_MT_E_ARG;
    if (ck_mt7925_live()) ck_mt7925_release();
    memset(&g_mt, 0, sizeof g_mt);
    g_mt.fn = *f;
    const pci_func *pf = g_mt.pf = &g_mt.fn;
    const pci_bar *b = &pf->bar[0];
    mt7925_err ie = mt7925_pci_identity(pf->vendor, pf->device, pf->class_code, b->size, b->is64, !b->io && b->addr);
    if (ie != MT7925_OK) {
        ck_printf("mt7925: unavailable (%s: class 0x%06x BAR0 size 0x%llx is64=%u io=%u; nothing written)\n",
                  mt7925_strerror(ie), pf->class_code, (unsigned long long)b->size, b->is64, b->io);
        return CK_MT_E_ARG;
    }
    size_t need = mt7925_dma_bytes();
    uint64_t win = ((uint64_t)need + 4095u) & ~(uint64_t)4095u;
    /* The SMMU window is whole pages: allocate all of it, so the chip can reach nothing else. */
    if (!g_mem) g_mem = ck_dma_alloc((size_t)win, 4096, &g_phys);
    if (!g_mem) {
        ck_printf("mt7925: unavailable (DMA allocation of %u bytes failed)\n", (unsigned)win);
        return CK_MT_E_DMA;
    }
    uint32_t rid = ((uint32_t)pf->bus << 8) | ((uint32_t)pf->dev << 3) | (uint32_t)pf->fn;
    struct ck_dma_confinement cf;
    int src = ck_dma_confine(pf->segment, rid, g_phys, win, &cf);
    if (src != 0) {
        ck_printf("dma_gate: mt7925 denied (%s rc=%d), bus master stays off\n",
                  src == CK_SMMU_ABSENT ? "NoSmmu" : "SmmuNotReady", src);
        return CK_MT_E_DENIED;
    }
    g_mt.confined = 1;
    g_mt.stream_id = cf.stream_id;
    ck_printf("smmu_dma_window: mt7925 only iova=0x%llx len=0x%llx rid=0x%x stream_id=0x%x\n",
              (unsigned long long)cf.iova, (unsigned long long)cf.len, rid, cf.stream_id);
    if (cf.iova + win > 0x100000000ull) {
        ck_printf("mt7925: unavailable (window above 4 GiB; the chip has a 32-bit DMA mask)\n");
        ck_mt7925_release();
        return CK_MT_E_DMA;
    }
    g_mt.bar = (volatile uint8_t *)ck_mmio_map(b->addr, (size_t)b->size);
    pci_enable(pf, 1);
    g_mt.bm_on = 1;
    ck_printf("dma_gate: mt7925 granted (Confined), bus master on\n");
    mt7925_ops ops = {(void *)g_mt.bar, o_rd, o_wr, o_delay, o_barrier};
    g_mt.init_tried = 1;
    mt7925_err e = mt7925_init(&g_mt.dev, &ops, g_mem, cf.iova, need);
    ck_printf("mt7925: init %s chip_rev=0x%08x\n", mt7925_strerror(e), g_mt.dev.chip_rev);
    if (e != MT7925_OK) {
        ck_mt7925_release();
        return CK_MT_E_INIT;
    }
    ck_printf("mt7925: firmware not packaged in this image (no download; UNKNOWN boot-package path)\n");
    ck_mt7925_release();
    return CK_MT_OK;
}
