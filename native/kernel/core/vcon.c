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
 * No interrupts. TX is a ring: each chunk goes into the next free slot and the
 * writer goes on at once; it waits (bounded) only when every slot is still
 * in flight, and ck_vcon_flush waits until the device has used them all (the
 * reset path calls it so the last lines are not lost). Waiting for each chunk
 * cost 24.5 ms per line under Apple Virtualization.framework (MEASURED on an
 * M2, L291), about 3 s of every boot. RX keeps small buffers posted and
 * ck_vcon_rx_poll takes bytes. */
#include "ck_internal.h"
#include "arch.h"
#include "vcon.h"
#include "vpci.h"

#define RXBUF 64u     /* bytes per posted receive buffer */
#define TXCHUNK 256u  /* bytes per transmit chunk (CK_VPCI_VQ chunks fit the buffer) */
#define SPIN 20000000u /* bounded wait for the device to use a TX buffer */

static struct {
    int on;
    struct ck_vpci v;
    struct ck_vq rx, tx;
    unsigned rx_pos, rx_len, rx_head; /* the used RX buffer being drained */
    int busy;
    unsigned tx_timeouts;
    uint64_t tx_chunks, tx_full_waits;
} vc;

int ck_vcon_attach(void)
{
    if (vc.on)
        return 0;
    int rc = ck_vpci_open(0x1043, &vc.v); /* virtio console (type 3) */
    if (rc)
        return rc;
    if (ck_vpci_queue(&vc.v, 0, &vc.rx) || ck_vpci_queue(&vc.v, 1, &vc.tx))
        return -7;
    struct ck_vq *q = &vc.rx;
    for (uint16_t i = 0; i < q->size; i++) {
        q->desc[i].addr = q->buf_phys + (uint64_t)i * RXBUF;
        q->desc[i].len = RXBUF;
        q->desc[i].flags = 2; /* VIRTQ_DESC_F_WRITE */
        q->avail->ring[i] = i;
    }
    ck_vpci_wmb();
    q->avail->idx = q->size;
    q->next_avail = q->size;
    ck_vpci_driver_ok(&vc.v);
    *q->notify = q->index;
    vc.on = 1;
    return 0;
}

int ck_vcon_on(void)
{
    return vc.on;
}

void ck_vcon_where(uint8_t *bus, uint8_t *dev, uint8_t *fn)
{
    *bus = vc.v.bus;
    *dev = vc.v.dev;
    *fn = vc.v.fn;
}

unsigned ck_vcon_tx_timeouts(void)
{
    return vc.tx_timeouts;
}

/* Chunks posted and not yet used by the device. The console device uses TX
 * buffers in order, so slot next_avail % size is free once fewer than size
 * chunks are in flight. */
static uint16_t tx_in_flight(void)
{
    ck_vpci_mb();
    return (uint16_t)(vc.tx.next_avail - vc.tx.used->idx);
}

/* Waits (bounded) until fewer than `limit` chunks are in flight; 0 when so. */
static int tx_wait(uint16_t limit)
{
    for (unsigned i = 0; i < SPIN; i++)
        if (tx_in_flight() < limit)
            return 0;
    vc.tx_timeouts++;
    return -1;
}

/* One chunk through the next TX slot; waits only for a free slot. */
static void tx_chunk(const uint8_t *s, unsigned n)
{
    struct ck_vq *q = &vc.tx;
    if (tx_in_flight() >= q->size) {
        vc.tx_full_waits++;
        if (tx_wait(q->size) != 0)
            return; /* device stuck: drop the chunk rather than overwrite one in flight */
    }
    uint16_t slot = q->next_avail % q->size;
    for (unsigned i = 0; i < n; i++)
        q->buf[(unsigned)slot * TXCHUNK + i] = s[i];
    q->desc[slot].addr = q->buf_phys + (uint64_t)slot * TXCHUNK;
    q->desc[slot].len = n;
    q->desc[slot].flags = 0;
    ck_vq_post(q, slot);
    vc.tx_chunks++;
}

void ck_vcon_flush(void)
{
    if (vc.on && tx_in_flight())
        (void)tx_wait(1);
}

void ck_vcon_tx_stats(uint64_t *chunks, uint64_t *full_waits, unsigned *timeouts)
{
    *chunks = vc.tx_chunks;
    *full_waits = vc.tx_full_waits;
    *timeouts = vc.tx_timeouts;
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
    struct ck_vq *q = &vc.rx;
    for (;;) {
        if (vc.rx_pos < vc.rx_len)
            return q->buf[(unsigned)vc.rx_head * RXBUF + vc.rx_pos++];
        if (vc.rx_len) { /* drained: give the buffer back */
            ck_vq_post(q, (uint16_t)vc.rx_head);
            vc.rx_len = vc.rx_pos = 0;
        }
        ck_vpci_mb();
        if (q->used->idx == q->last_used)
            return -1;
        volatile struct ck_vq_used_elem *e = &q->used->ring[q->last_used % q->size];
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
