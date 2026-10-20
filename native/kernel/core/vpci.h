/* vpci.h -- minimal polled virtio 1.x over PCI for the hypervisor console and
 * entropy (core/vcon.c, core/vrng.c). Root bus of the MCFG segment only, one
 * function per device type, no MSI-X, no interrupts. Only for devices that are
 * the hypervisor's own emulation (firmware at EL1): no SMMU window is asked
 * for, see core/vguest.h. Needs the kernel MMU (ck_mmio_map, ck_dma_alloc). */
#ifndef AIENOS_CK_VPCI_H
#define AIENOS_CK_VPCI_H
#include <stdint.h>

#define CK_VPCI_VQ 16u /* queue entries used (<= the device maximum) */

struct ck_vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
struct ck_vq_avail {
    uint16_t flags, idx, ring[CK_VPCI_VQ], used_event;
};
struct ck_vq_used_elem {
    uint32_t id, len;
};
struct ck_vq_used {
    uint16_t flags, idx;
    struct ck_vq_used_elem ring[CK_VPCI_VQ];
    uint16_t avail_event;
};
struct ck_vq {
    volatile struct ck_vq_desc *desc;
    volatile struct ck_vq_avail *avail;
    volatile struct ck_vq_used *used;
    volatile uint16_t *notify;
    uint16_t index, size, last_used, next_avail;
    uint8_t *buf; /* 8 KiB of DMA buffer space for the caller */
    uint64_t buf_phys;
};
struct ck_vpci {
    volatile uint8_t *cfg, *common, *notify_base;
    uint32_t notify_mult;
    uint8_t bus, dev, fn;
};

/* Errors: no MCFG (-1), no such device on the root bus (-2), no capability
 * list (-3), no common/notify capability (-4), no VERSION_1 (-5),
 * FEATURES_OK refused (-6), queue setup failed (-7). */
/* Finds vendor 0x1af4 with device id `modern` (0x1040 + type) or a
 * transitional id (0x1000..0x103f) with subsystem id `type`; maps its common
 * and notify windows; resets it and negotiates VERSION_1 only. */
int ck_vpci_open(uint16_t modern, struct ck_vpci *v);
/* Queue `index`: rings and an 8 KiB buffer in DMA memory, MSI-X off. */
int ck_vpci_queue(struct ck_vpci *v, unsigned index, struct ck_vq *q);
void ck_vpci_driver_ok(struct ck_vpci *v);
/* The devices stage's bus-master sweep (dev/devices.c) clears every endpoint,
 * these too; QEMU honours that (VZ does not). Turns bus master back on for the
 * functions ck_vpci_open opened, with one log line each. */
void ck_vpci_after_sweep(void);
/* Posts descriptor `slot` (already filled) and notifies the device. */
void ck_vq_post(struct ck_vq *q, uint16_t slot);
/* 1 when the device has used everything posted so far. */
int ck_vq_idle(struct ck_vq *q);
static inline void ck_vpci_wmb(void)
{
    __asm__ volatile("dmb oshst" ::: "memory");
}
static inline void ck_vpci_mb(void)
{
    __asm__ volatile("dmb osh" ::: "memory");
}
#endif
