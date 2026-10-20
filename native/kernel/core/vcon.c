/* vcon.c -- virtio console (virtio 1.x over PCI, port 0, POLLED) as the
 * kernel console when firmware hands over at EL1 under a hypervisor and the
 * ACPI tables name no UART (no SPCR). Apple Virtualization.framework is that
 * machine: its ACPI has PCI0 (MCFG), GTDT, MADT and no UART at all; its only
 * text console is a virtio console device (aienos-run on macOS,
 * docs/AIENOS_RUN_MACOS.md). QEMU `-device virtio-serial-pci -device
 * virtconsole` is the same device and the host test path.
 *
 * DMA: the virtqueues live in ck_dma_alloc memory. No SMMU confinement is
 * asked for: the "device" is the hypervisor's console emulation, which can
 * read all guest memory anyway; confinement guards against physical bus
 * masters, and this driver binds only under a hypervisor (kmain checks
 * firmware EL1) and only to vendor 0x1af4 device 0x1043 / transitional 0x1003.
 *
 * No interrupts: TX waits (bounded) for the device to consume each chunk;
 * RX keeps CK_VCON_RXQ small buffers posted and ck_vcon_rx_poll takes bytes. */
#include "ck_internal.h"
#include "arch.h"
#include "vcon.h"

#define VQ 16u            /* queue size we use (<= device max) */
#define RXBUF 64u         /* bytes per posted receive buffer */
#define TXCHUNK 256u      /* bytes per transmit chunk (VQ chunks fit one page) */
#define SPIN 20000000u    /* bounded wait for the device to use a TX buffer */

struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
struct vq_avail {
    uint16_t flags, idx, ring[VQ], used_event;
};
struct vq_used_elem {
    uint32_t id, len;
};
struct vq_used {
    uint16_t flags, idx;
    struct vq_used_elem ring[VQ];
    uint16_t avail_event;
};
struct vq {
    volatile struct vq_desc *desc;
    volatile struct vq_avail *avail;
    volatile struct vq_used *used;
    volatile uint16_t *notify;
    uint16_t size, last_used, next_avail;
    uint8_t *buf;
    uint64_t buf_phys;
};

static struct {
    int on;
    volatile uint8_t *common;
    struct vq rx, tx;
    unsigned rx_pos, rx_len, rx_head; /* the used RX buffer being drained */
    int busy;
    unsigned tx_timeouts;
    uint8_t bus, dev, fn;
} vc;

static inline void wmb(void)
{
    __asm__ volatile("dmb oshst" ::: "memory");
}
static inline void mb(void)
{
    __asm__ volatile("dmb osh" ::: "memory");
}

#define C8(o) (*(volatile uint8_t *)(vc.common + (o)))
#define C16(o) (*(volatile uint16_t *)(vc.common + (o)))
#define C32(o) (*(volatile uint32_t *)(vc.common + (o)))
#define C64(o) (*(volatile uint64_t *)(vc.common + (o)))
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

static int vq_setup(struct vq *q, unsigned index, volatile uint8_t *notify_base, uint32_t mult, int rx)
{
    C16(QSEL) = (uint16_t)index;
    mb();
    uint16_t max = C16(QSIZE);
    if (max == 0)
        return -1;
    q->size = max < VQ ? max : VQ;
    C16(QSIZE) = q->size;
    uint64_t pd, pa, pu;
    q->desc = ck_dma_alloc(4096, 4096, &pd);
    q->avail = ck_dma_alloc(4096, 4096, &pa);
    q->used = ck_dma_alloc(4096, 4096, &pu);
    q->buf = ck_dma_alloc(4096 * 2, 4096, &q->buf_phys);
    if (!q->desc || !q->avail || !q->used || !q->buf)
        return -2;
    q->last_used = q->next_avail = 0;
    C16(QMSIX) = 0xffff; /* no MSI-X: polled */
    C64(QDESC) = pd;
    C64(QDRV) = pa;
    C64(QDEV) = pu;
    q->notify = (volatile uint16_t *)(notify_base + (uint64_t)C16(QNOFF) * mult);
    if (rx) {
        for (uint16_t i = 0; i < q->size; i++) {
            q->desc[i].addr = q->buf_phys + (uint64_t)i * RXBUF;
            q->desc[i].len = RXBUF;
            q->desc[i].flags = 2; /* VIRTQ_DESC_F_WRITE */
            q->avail->ring[i] = i;
        }
        wmb();
        q->avail->idx = q->size;
        q->next_avail = q->size;
    }
    mb();
    C16(QEN) = 1;
    return 0;
}

int ck_vcon_attach(void)
{
    if (vc.on)
        return 0;
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
                (dev == 0x1043 || (dev == 0x1003 && (cfg32(c, 0x2c) >> 16) == 3))) {
                cfg = c;
                vc.bus = bus0;
                vc.dev = (uint8_t)d;
                vc.fn = (uint8_t)f;
                break;
            }
            if (f == 0 && !((cfg32(c, 0x0c) >> 16) & 0x80))
                break; /* single-function device */
        }
    }
    if (!cfg)
        return -2;
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
    mb();
    vc.common = ck_mmio_map(common & ~0xfffull, ((common & 0xfff) + common_len + 0xfff) & ~0xfffull) +
                (common & 0xfff);
    volatile uint8_t *nb = ck_mmio_map(notify & ~0xfffull, 0x2000) + (notify & 0xfff);
    C8(STATUS) = 0; /* reset */
    for (unsigned i = 0; i < 1000000 && C8(STATUS) != 0; i++)
        ;
    C8(STATUS) = 1 | 2; /* ACKNOWLEDGE | DRIVER */
    C32(DFSEL) = 1;
    mb();
    if (!(C32(DF) & 1)) /* VIRTIO_F_VERSION_1 (bit 32) */
        return -5;
    C32(GFSEL) = 0;
    C32(GF) = 0; /* no MULTIPORT, no EMERG_WRITE, no SIZE */
    C32(GFSEL) = 1;
    C32(GF) = 1; /* VERSION_1 */
    C8(STATUS) = 1 | 2 | 8; /* FEATURES_OK */
    mb();
    if (!(C8(STATUS) & 8))
        return -6;
    C16(MSIXCFG) = 0xffff;
    if (vq_setup(&vc.rx, 0, nb, mult, 1) || vq_setup(&vc.tx, 1, nb, mult, 0))
        return -7;
    C8(STATUS) = 1 | 2 | 8 | 4; /* DRIVER_OK */
    mb();
    *vc.rx.notify = 0;
    vc.on = 1;
    return 0;
}

int ck_vcon_on(void)
{
    return vc.on;
}

void ck_vcon_where(uint8_t *bus, uint8_t *dev, uint8_t *fn)
{
    *bus = vc.bus;
    *dev = vc.dev;
    *fn = vc.fn;
}

unsigned ck_vcon_tx_timeouts(void)
{
    return vc.tx_timeouts;
}

/* One chunk through descriptor 0 of the TX queue; waits until used. */
static void tx_chunk(const uint8_t *s, unsigned n)
{
    struct vq *q = &vc.tx;
    uint16_t slot = q->next_avail % q->size;
    for (unsigned i = 0; i < n; i++)
        q->buf[(unsigned)slot * TXCHUNK + i] = s[i];
    q->desc[slot].addr = q->buf_phys + (uint64_t)slot * TXCHUNK;
    q->desc[slot].len = n;
    q->desc[slot].flags = 0;
    q->avail->ring[q->next_avail % q->size] = slot;
    wmb();
    q->next_avail++;
    q->avail->idx = q->next_avail;
    mb();
    *q->notify = 1;
    for (unsigned i = 0; i < SPIN; i++) {
        mb();
        if (q->used->idx == q->next_avail) {
            q->last_used = q->next_avail;
            return;
        }
    }
    vc.tx_timeouts++;
}

void ck_vcon_write(const char *s, size_t n)
{
    if (!vc.on || vc.busy)
        return;
    vc.busy = 1;
    uint8_t chunk[TXCHUNK];
    unsigned k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n')
            chunk[k++] = '\r';
        chunk[k++] = (uint8_t)s[i];
        if (k >= sizeof chunk - 1) {
            tx_chunk(chunk, k);
            k = 0;
        }
    }
    if (k)
        tx_chunk(chunk, k);
    vc.busy = 0;
}

int ck_vcon_rx_poll(void)
{
    if (!vc.on)
        return -1;
    struct vq *q = &vc.rx;
    for (;;) {
        if (vc.rx_pos < vc.rx_len)
            return q->buf[(unsigned)vc.rx_head * RXBUF + vc.rx_pos++];
        if (vc.rx_len) { /* drained: give the buffer back */
            q->avail->ring[q->next_avail % q->size] = (uint16_t)vc.rx_head;
            wmb();
            q->next_avail++;
            q->avail->idx = q->next_avail;
            mb();
            *q->notify = 0;
            vc.rx_len = vc.rx_pos = 0;
        }
        mb();
        if (q->used->idx == q->last_used)
            return -1;
        volatile struct vq_used_elem *e = &q->used->ring[q->last_used % q->size];
        vc.rx_head = e->id % q->size;
        vc.rx_len = e->len > RXBUF ? RXBUF : e->len;
        vc.rx_pos = 0;
        q->last_used++;
        if (!vc.rx_len) { /* empty completion: repost at once */
            vc.rx_len = 1;
            vc.rx_pos = 1;
        }
    }
}
