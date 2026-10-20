/* vrng.c -- virtio entropy device (virtio 1.x over PCI, POLLED) for the VM
 * edition (core/vguest.h). One request queue: the kernel posts one 8-byte
 * device-writable buffer, the device fills it from the host's entropy
 * (Apple Virtualization.framework VZVirtioEntropyDeviceConfiguration, QEMU
 * virtio-rng-pci). A short or late answer is a failed read; core/entropy.c
 * retries, runs the stuck-output test and latches refusals. */
#include "ck_internal.h"
#include "vguest.h"
#include "vpci.h"
#include "vrng.h"

#define SPIN 20000000u /* bounded wait for one answer */

static struct {
    int tried, on;
    struct ck_vpci v;
    struct ck_vq q;
} vr;

static int present(void *ctx)
{
    (void)ctx;
    if (!vr.tried) {
        vr.tried = 1;
        if (ck_vguest() && ck_vpci_open(0x1044, &vr.v) == 0 && ck_vpci_queue(&vr.v, 0, &vr.q) == 0) {
            ck_vpci_driver_ok(&vr.v);
            vr.on = 1;
        }
    }
    return vr.on;
}

static int read_word(void *ctx, uint64_t *out)
{
    (void)ctx;
    if (!vr.on)
        return 0;
    struct ck_vq *q = &vr.q;
    uint16_t slot = q->next_avail % q->size;
    volatile uint64_t *b = (volatile uint64_t *)(q->buf + (unsigned)slot * 8u);
    *b = 0;
    q->desc[slot].addr = q->buf_phys + (uint64_t)slot * 8u;
    q->desc[slot].len = 8;
    q->desc[slot].flags = 2; /* VIRTQ_DESC_F_WRITE */
    ck_vq_post(q, slot);
    for (unsigned i = 0; i < SPIN; i++) {
        if (ck_vq_idle(q)) {
            uint32_t got = q->used->ring[(uint16_t)(q->next_avail - 1) % q->size].len;
            ck_vpci_mb();
            *out = *b;
            return got == 8;
        }
    }
    return 0;
}

static const struct ck_rng_ops ops = { present, read_word, 0 };

const struct ck_rng_ops *ck_vrng_ops(void)
{
    return &ops;
}
