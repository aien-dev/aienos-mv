/* mt7925_bind.h -- bind native/net's polled MediaTek MT7925 Wi-Fi driver
 * (aienos_mt7925.c) to the Spark's Wi-Fi function (14c3:7925, segment 9 bus
 * 1 on the DGX Spark: 0009:01:00.0) through ck.h services.
 *
 * DMA rule (same as NVMe, virtio-net and the RTL8127): no SMMU confinement
 * means no DMA. The driver's one region (rings + buffers, mt7925_dma_bytes)
 * is allocated first and becomes the stream's only SMMU window
 * (ck_dma_confine); only then are memory decoding and bus mastering enabled.
 * The reference driver sets a 32-bit DMA mask, so the window's bus address
 * must lie below 4 GiB (mt7925_init refuses otherwise).
 *
 * What the probe does: identity and BAR 0 check from config space, confine,
 * map BAR 0, mt7925_init (firmware own, driver own, chip revision, WFSYS
 * reset, rings, WFDMA on), print the revision, then mt7925_stop and release.
 * No firmware is sent: the image carries none yet (UNKNOWN: how the boot
 * package delivers WIFI_MT7925_PATCH_MCU_1_1_hdr.bin and
 * WIFI_RAM_CODE_MT7925_1_1.bin, docs/MT7925_FIRMWARE_INVENTORY.md).
 *
 * Compiled only into a CK_MT7925=1 image (the attended-boot image); every
 * QEMU gate image is unchanged. Never run on the chip yet: every register
 * sequence is from the open-source mt76 driver (DOCS SILENT), UNKNOWN rows in
 * docs/SPARK_WIFI_MT7925.md. */
#ifndef AIENOS_CK_MT7925_BIND_H
#define AIENOS_CK_MT7925_BIND_H
#include <stddef.h>
#include <stdint.h>
#include "pci.h"
enum {
    CK_MT_OK = 0,
    CK_MT_ABSENT = -701,   /* no 14c3:7925 function in the discovery */
    CK_MT_E_ARG = -702,    /* bad function, identity or BAR 0 refused, ECAM unmapped */
    CK_MT_E_DMA = -703,    /* DMA region allocation failed or above 4 GiB */
    CK_MT_E_DENIED = -704, /* no SMMU confinement: chip left off */
    CK_MT_E_INIT = -705,   /* mt7925_init failed (own handshake, reset or rings) */
};
/* First 14c3:7925 function of the multi-segment discovery, read as an
 * endpoint into *out. CK_MT_ABSENT when there is none. */
int ck_mt7925_find(const pci_found *found, uint32_t n, pci_func *out);
/* Gate, map BAR 0, mt7925_init, report, stop. Always releases the chip
 * (stop, bus master off, stream to abort) before it returns. Prints
 * mt7925:/dma_gate:/smmu: lines. */
int ck_mt7925_probe(const pci_func *f);
/* Stop the chip, revoke bus mastering, return the stream to abort, if live.
 * Idempotent. */
void ck_mt7925_release(void);
int ck_mt7925_live(void);
#endif
