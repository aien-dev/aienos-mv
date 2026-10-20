/* vpci.c -- see vpci.h. Shared by the virtio console (core/vcon.c) and the
 * virtio entropy source (core/vrng.c); both are polled and both bind only to
 * the hypervisor's own devices. */
#include "ck_internal.h"
#include "arch.h"
#include "vpci.h"

#define C8(v, o) (*(volatile uint8_t *)((v)->common + (o)))
#define C16(v, o) (*(volatile uint16_t *)((v)->common + (o)))
#define C32(v, o) (*(volatile uint32_t *)((v)->common + (o)))
#define C64(v, o) (*(volatile uint64_t *)((v)->common + (o)))
enum { DFSEL = 0x00, DF = 0x04, GFSEL = 0x08, GF = 0x0c, MSIXCFG = 0x10, NUMQ = 0x12, STATUS = 0x14,
       QSEL = 0x16, QSIZE = 0x18, QMSIX = 0x1a, QEN = 0x1c, QNOFF = 0x1e, QDESC = 0x20, QDRV = 0x28,
       QDEV = 0x30 };

static uint32_t cfg32(volatile uint8_t *cfg, unsigned off)
{
    return *(volatile uint32_t *)(cfg + (off & ~3u)) >> (8 * (off & 3u));
}

/* BAR `bar` of the function at `cfg`, firmware-assigned (0 when unassigned). */
static uint64_t bar_base(volatile uint8_t *cfg, unsigned bar)
{
    if (bar > 5)
        return 0;
    uint32_t lo = cfg32(cfg, 0x10 + 4 * bar);
    if (lo & 1)
        return 0; /* I/O BAR */
    uint64_t b = lo & ~0xfull;
    if (((lo >> 1) & 3) == 2 && bar < 5)
        b |= (uint64_t)cfg32(cfg, 0x14 + 4 * bar) << 32;
    return b;
}

int ck_vpci_open(uint16_t modern, struct ck_vpci *v)
{
    const uint8_t *mcfg = ck_acpi_find("MCFG");
    if (!mcfg || *(const uint32_t *)(mcfg + 4) < 44 + 16)
        return -1;
    uint64_t ecam = *(const uint64_t *)(mcfg + 44);
    uint8_t bus0 = mcfg[44 + 10];
    if (!ecam)
        return -1;
    /* Bus `bus0` only: Virtualization.framework and QEMU virt put the
     * endpoints on the root bus. 32 devices x 8 functions x 4 KiB. */
    volatile uint8_t *bus = ck_mmio_map(ecam, 1u << 20);
    volatile uint8_t *cfg = 0;
    for (unsigned d = 0; d < 32 && !cfg; d++) {
        for (unsigned f = 0; f < 8; f++) {
            volatile uint8_t *c = bus + ((d << 15) | (f << 12));
            uint32_t id = cfg32(c, 0);
            if ((id & 0xffff) == 0xffff) {
                if (f == 0)
                    break;
                continue;
            }
            uint16_t dev = (uint16_t)(id >> 16);
            if ((id & 0xffff) == 0x1af4 &&
                (dev == modern || (dev == modern - 0x40 && (cfg32(c, 0x2c) >> 16) == (uint32_t)(modern - 0x1040)))) {
                cfg = c;
                v->bus = bus0;
                v->dev = (uint8_t)d;
                v->fn = (uint8_t)f;
                break;
            }
            if (f == 0 && !((cfg32(c, 0x0c) >> 16) & 0x80))
                break; /* single-function device */
        }
    }
    if (!cfg)
        return -2;
    v->cfg = cfg;
    /* Virtio vendor capabilities (cap id 0x09): common (1) and notify (2). */
    uint64_t common = 0, notify = 0;
    uint32_t mult = 0, common_len = 0;
    if (!(cfg32(cfg, 0x04) & (1u << 20)))
        return -3; /* no capability list */
    unsigned p = cfg32(cfg, 0x34) & 0xfc, guard = 0;
    while (p && guard++ < 48) {
        uint32_t h = cfg32(cfg, p);
        if ((h & 0xff) == 0x09) {
            unsigned type = (h >> 24) & 0xff, bar = cfg32(cfg, p + 4) & 0xff;
            uint32_t off = cfg32(cfg, p + 8), len = cfg32(cfg, p + 12);
            uint64_t base = bar_base(cfg, bar);
            if (base && type == 1 && !common) {
                common = base + off;
                common_len = len;
            } else if (base && type == 2 && !notify) {
                notify = base + off;
                mult = cfg32(cfg, p + 16);
            }
        }
        p = (h >> 8) & 0xfc;
    }
    if (!common || !notify || common_len < 0x38)
        return -4;
    /* Memory decode + bus master (the firmware may have left them off). */
    *(volatile uint16_t *)(cfg + 4) |= 0x6;
    ck_vpci_mb();
    v->common = ck_mmio_map(common & ~0xfffull, ((common & 0xfff) + common_len + 0xfff) & ~0xfffull) +
                (common & 0xfff);
    v->notify_base = ck_mmio_map(notify & ~0xfffull, 0x2000) + (notify & 0xfff);
    v->notify_mult = mult;
    C8(v, STATUS) = 0; /* reset */
    for (unsigned i = 0; i < 1000000 && C8(v, STATUS) != 0; i++)
        ;
    C8(v, STATUS) = 1 | 2; /* ACKNOWLEDGE | DRIVER */
    C32(v, DFSEL) = 1;
    ck_vpci_mb();
    if (!(C32(v, DF) & 1)) /* VIRTIO_F_VERSION_1 (bit 32) */
        return -5;
    C32(v, GFSEL) = 0;
    C32(v, GF) = 0; /* no device features */
    C32(v, GFSEL) = 1;
    C32(v, GF) = 1; /* VERSION_1 */
    C8(v, STATUS) = 1 | 2 | 8; /* FEATURES_OK */
    ck_vpci_mb();
    if (!(C8(v, STATUS) & 8))
        return -6;
    C16(v, MSIXCFG) = 0xffff;
    return 0;
}

int ck_vpci_queue(struct ck_vpci *v, unsigned index, struct ck_vq *q)
{
    C16(v, QSEL) = (uint16_t)index;
    ck_vpci_mb();
    uint16_t max = C16(v, QSIZE);
    if (max == 0)
        return -7;
    q->size = max < CK_VPCI_VQ ? max : CK_VPCI_VQ;
    C16(v, QSIZE) = q->size;
    uint64_t pd, pa, pu;
    q->desc = ck_dma_alloc(4096, 4096, &pd);
    q->avail = ck_dma_alloc(4096, 4096, &pa);
    q->used = ck_dma_alloc(4096, 4096, &pu);
    q->buf = ck_dma_alloc(4096 * 2, 4096, &q->buf_phys);
    if (!q->desc || !q->avail || !q->used || !q->buf)
        return -7;
    q->index = (uint16_t)index;
    q->last_used = q->next_avail = 0;
    C16(v, QMSIX) = 0xffff; /* no MSI-X: polled */
    C64(v, QDESC) = pd;
    C64(v, QDRV) = pa;
    C64(v, QDEV) = pu;
    q->notify = (volatile uint16_t *)(v->notify_base + (uint64_t)C16(v, QNOFF) * v->notify_mult);
    ck_vpci_mb();
    C16(v, QEN) = 1;
    return 0;
}

void ck_vpci_driver_ok(struct ck_vpci *v)
{
    C8(v, STATUS) = 1 | 2 | 8 | 4; /* DRIVER_OK */
    ck_vpci_mb();
}

void ck_vq_post(struct ck_vq *q, uint16_t slot)
{
    q->avail->ring[q->next_avail % q->size] = slot;
    ck_vpci_wmb();
    q->next_avail++;
    q->avail->idx = q->next_avail;
    ck_vpci_mb();
    *q->notify = q->index; /* the queue index (no VIRTIO_F_NOTIFICATION_DATA) */
}

int ck_vq_idle(struct ck_vq *q)
{
    ck_vpci_mb();
    if (q->used->idx != q->next_avail)
        return 0;
    q->last_used = q->next_avail;
    return 1;
}
