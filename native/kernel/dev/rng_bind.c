/* rng_bind.c -- bind of a contract-generated virtio entropy driver (see
 * rng_bind.h). Freestanding. Built only with CK_CONTRACT_RNG. */
#include "rng_bind.h"
#include "ck.h"
#include "aienos_virtio_pci.h"
#include "aienos_virtio_rng.h" /* generated: <staged dir>/driver */
#include <string.h>

#define WIN_N 4
#define QSIZE 8u
#define RESET_WAIT_US 100000u
#define READ_WAIT_US 2000000u
#define WANT_BYTES 64u

#ifndef CK_CONTRACT_RNG_ATTENDED
#define CK_CONTRACT_RNG_ATTENDED 0
#endif

typedef struct {
    const pci_func *pf;
    volatile uint8_t *bar_va[PCI_MAX_BARS];
    volatile uint8_t *win[WIN_N];
    vrng_dev dev;
    int bm_on, confined;
    uint32_t stream_id;
} ck_vrng;

static ck_vrng g_rng;
static uint8_t *g_dma_mem; /* ck_dma_alloc memory is never freed: allocate once */
static uint64_t g_dma_phys;
static uint8_t cfg_image[4096];
static char g_short[13];

const char *ck_rng_contract_short(void)
{
    memcpy(g_short, vrng_contract_digest, 12);
    g_short[12] = 0;
    return g_short;
}
const char *ck_rng_contract_proven(void) { return VRNG_CONTRACT_PROVEN ? "yes" : "no"; }

static uint32_t o_read(void *ctx, vrng_window w, uint32_t off, unsigned width)
{
    volatile uint8_t *p = ((ck_vrng *)ctx)->win[w] + off;
    if (width == 1) return *p;
    if (width == 2) return *(volatile uint16_t *)p;
    return *(volatile uint32_t *)p;
}
static void o_write(void *ctx, vrng_window w, uint32_t off, unsigned width, uint32_t v)
{
    volatile uint8_t *p = ((ck_vrng *)ctx)->win[w] + off;
    if (width == 1) *p = (uint8_t)v;
    else if (width == 2) *(volatile uint16_t *)p = (uint16_t)v;
    else *(volatile uint32_t *)p = v;
    ck_mb();
}
static void o_notify(void *ctx, uint32_t off, uint16_t q)
{
    ck_mb(); /* ring writes visible before the doorbell */
    *(volatile uint16_t *)(((ck_vrng *)ctx)->win[VRNG_WIN_NOTIFY] + off) = q;
}

static volatile uint8_t *map_window(ck_vrng *n, const virtio_pci_region *r)
{
    if (!r->present || r->bar >= PCI_MAX_BARS) return 0;
    const pci_bar *b = &n->pf->bar[r->bar];
    if (b->io || b->addr == 0 || b->size == 0) return 0;
    if ((uint64_t)r->offset + r->length > b->size) return 0;
    if (!n->bar_va[r->bar]) n->bar_va[r->bar] = (volatile uint8_t *)ck_mmio_map(b->addr, (size_t)b->size);
    return n->bar_va[r->bar] + r->offset;
}

int ck_rng_live(void) { return g_rng.pf && (g_rng.bm_on || g_rng.confined); }

void ck_rng_release(void)
{
    ck_vrng *n = &g_rng;
    if (!n->pf) return;
    if (n->bm_on) {
        uint32_t st = 0xffu;
        if (n->win[VRNG_WIN_COMMON]) {
            o_write(n, VRNG_WIN_COMMON, VRNG_CC_STATUS, 1, 0);
            uint64_t t0 = ck_time_us();
            while ((st = o_read(n, VRNG_WIN_COMMON, VRNG_CC_STATUS, 1)) != 0 && ck_time_us() - t0 < RESET_WAIT_US)
                ck_udelay(10);
        }
        ck_printf("virtio_rng: device reset status=0x%02x (%s)\n", st, st == 0 ? "stopped" : "TIMEOUT");
        if (pci_bus_master_off(n->pf) == 0) {
            n->bm_on = 0;
            ck_printf("dma_gate: virtio_rng bus master revoked\n");
        } else
            ck_printf("dma_gate: virtio_rng bus master revoke FAILED (command register still has BME)\n");
    }
    if (n->confined) {
        int urc = ck_dma_unconfine(n->stream_id);
        n->confined = 0;
        ck_printf("smmu: virtio_rng stream 0x%x %s (rc=%d)\n", n->stream_id,
                  urc == 0 ? "returned to abort" : "abort FAILED", urc);
    }
}

/* The first function whose id is in the contract's own match table. */
static const pci_func *find_matched(const pci_system *s)
{
    for (uint32_t i = 0; i < s->n; i++)
        for (unsigned m = 0; m < VRNG_MATCH_N; m++)
            if (s->f[i].vendor == vrng_match_ids[m][0] && s->f[i].device == vrng_match_ids[m][1]) return &s->f[i];
    return 0;
}

int ck_rng_bind_selftest(const pci_system *s, const pci_func **bound_out)
{
    ck_vrng *n = &g_rng;
    if (bound_out) *bound_out = 0;
    if (ck_rng_live()) {
        ck_printf("virtio_rng: not bound (previous bind still live; release it first)\n");
        return CK_RNG_E_INIT;
    }
    memset(n, 0, sizeof *n);
    ck_printf("virtio_rng: driver from contract %s digest=%s proven=%s attended=%s\n", VRNG_CONTRACT_DEVICE,
              vrng_contract_digest, ck_rng_contract_proven(), CK_CONTRACT_RNG_ATTENDED ? "yes" : "no");
    const pci_func *f = find_matched(s);
    if (!f) {
        ck_printf("virtio_rng: not bound (no function matches the contract)\n");
        return CK_RNG_ABSENT;
    }
    if (bound_out) *bound_out = f;
    if (!VRNG_CONTRACT_PROVEN && !CK_CONTRACT_RNG_ATTENDED) {
        ck_printf("virtio_rng: not bound (contract UNPROVEN and this image is not an attended image)\n");
        return CK_RNG_E_UNPROVEN;
    }
    for (uint32_t off = 0; off < sizeof cfg_image; off += 4) {
        uint32_t v = pci_r32(f->cfg, off);
        cfg_image[off] = (uint8_t)v;
        cfg_image[off + 1] = (uint8_t)(v >> 8);
        cfg_image[off + 2] = (uint8_t)(v >> 16);
        cfg_image[off + 3] = (uint8_t)(v >> 24);
    }
    virtio_pci_caps caps;
    virtio_pci_err pe = virtio_pci_parse_caps(cfg_image, sizeof cfg_image, &caps);
    if (pe != VIRTIO_PCI_OK) {
        ck_printf("virtio_rng: not bound (capability parse failed: %s)\n", virtio_pci_err_name(pe));
        return CK_RNG_E_CAPS;
    }
    n->pf = f;
    size_t need = vrng_mem_size(QSIZE);
    uint64_t win = ((uint64_t)need + 4095u) & ~(uint64_t)4095u;
    if (need && !g_dma_mem) g_dma_mem = ck_dma_alloc((size_t)win, 4096, &g_dma_phys);
    if (!need || !g_dma_mem) {
        ck_printf("virtio_rng: unavailable (DMA allocation of %u bytes failed)\n", (unsigned)win);
        return CK_RNG_E_DMA;
    }
    uint32_t rid = ((uint32_t)f->bus << 8) | ((uint32_t)f->dev << 3) | (uint32_t)f->fn;
    struct ck_dma_confinement cf;
    int src = ck_dma_confine(f->segment, rid, g_dma_phys, win, &cf);
    if (src != 0) {
        ck_printf("dma_gate: virtio_rng denied (%s rc=%d), bus master stays off\n",
                  src == CK_SMMU_ABSENT ? "NoSmmu" : "SmmuNotReady", src);
        ck_printf("virtio_rng: not bound (SMMU DMA isolation not active)\n");
        return CK_RNG_E_DENIED;
    }
    n->confined = 1;
    n->stream_id = cf.stream_id;
    ck_printf("smmu_dma_window: virtio_rng only iova=0x%llx len=0x%llx rid=0x%x stream_id=0x%x\n",
              (unsigned long long)cf.iova, (unsigned long long)cf.len, rid, cf.stream_id);
    const virtio_pci_region *r[WIN_N] = {&caps.common_cfg, &caps.notify_cfg, &caps.isr_cfg, &caps.device_cfg};
    for (int i = 0; i < WIN_N; i++) {
        n->win[i] = map_window(n, r[i]);
        /* ISR is unused (polled) and the entropy device has no device config. */
        if (!n->win[i] && (i == VRNG_WIN_COMMON || i == VRNG_WIN_NOTIFY)) {
            ck_printf("virtio_rng: not bound (capability window %d outside its BAR)\n", i);
            ck_rng_release();
            return CK_RNG_E_CAPS;
        }
    }
    pci_enable(f, 1);
    n->bm_on = 1;
    ck_printf("dma_gate: virtio_rng granted (Confined), bus master on\n");
    vrng_ops ops = {n, o_read, o_write, o_notify};
    vrng_err e = vrng_init_flags(&n->dev, &ops, &caps, g_dma_mem, g_dma_phys, need, QSIZE,
                                 VRNG_INIT_REQUIRE_ACCESS_PLATFORM);
    if (e != VRNG_OK) {
        ck_printf("virtio_rng: init FAIL (%s)\n", vrng_err_name(e));
        ck_rng_release();
        return CK_RNG_E_INIT;
    }
    uint8_t got_bytes[WANT_BYTES];
    memset(got_bytes, 0, sizeof got_bytes);
    size_t have = 0;
    uint64_t t0 = ck_time_us();
    while (have < WANT_BYTES && ck_time_us() - t0 < READ_WAIT_US) {
        size_t got = 0;
        e = vrng_read(&n->dev, got_bytes + have, WANT_BYTES - have, &got);
        if (e != VRNG_OK) break;
        have += got;
        if (!got) ck_udelay(100);
    }
    /* A trivially stuck source (all bytes one value) is not entropy. */
    unsigned distinct = 0;
    uint8_t seen[256];
    memset(seen, 0, sizeof seen);
    for (size_t i = 0; i < have; i++)
        if (!seen[got_bytes[i]]++) distinct++;
    int ok = e == VRNG_OK && have == WANT_BYTES && distinct > 16u;
    ck_printf("virtio_rng: %s %02x:%02x.%u rid=0x%x qsize=%u features=0x%llx access_platform=%s bytes=%u distinct=%u "
              "first8=%02x%02x%02x%02x%02x%02x%02x%02x contract=%s (%s)\n",
              ok ? "up" : "FAIL", f->bus, f->dev, f->fn, rid, n->dev.q[0].size, (unsigned long long)n->dev.features,
              (n->dev.features >> VRNG_F_ACCESS_PLATFORM & 1) ? "yes" : "no", (unsigned)have, distinct, got_bytes[0],
              got_bytes[1], got_bytes[2], got_bytes[3], got_bytes[4], got_bytes[5], got_bytes[6], got_bytes[7],
              ck_rng_contract_short(), vrng_err_name(e));
    ck_rng_release();
    return ok ? CK_RNG_OK : CK_RNG_E_READ;
}
