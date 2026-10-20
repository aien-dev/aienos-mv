/* hda_fence.h -- DMA fence for the Intel HD Audio controller (L207), the
 * audio counterpart of xhci_fence.h and with the same rules:
 *   1. find the controller: a PCI function of class 0x040300 (QEMU intel-hda,
 *      PCI HDA on other machines) or the ACPI platform device NVDA2014 (the
 *      DGX Spark's HDMI audio, MMIO 0x36078000 measured under Linux);
 *   2. PCI: bus mastering must still be off after the post-exit sweep
 *      ("hda_pci: command=0x.... bus_master=off"), else the controller stays off;
 *   3. DMA gate: no SMMU confinement means no DMA ("dma_gate: hda denied
 *      (NoSmmu), bus master stays off"); the controller's DMA region is its
 *      stream's only SMMU window, there is no unconfined bypass, not even in
 *      the TEST-ONLY CK_QEMU_UNSAFE_DMA=1 build;
 *   4. with the grant: dev/hda.c brings the controller up, enumerates the
 *      codecs and prints them; an image built with CK_HDA_TONE=1 (TEST-ONLY,
 *      QEMU gate) also plays the sine tone through the first output path;
 *   5. release in the NVMe halt order: stream stopped and controller held in
 *      reset while it can still DMA, bus master off with read-back, stream
 *      back to abort.
 *
 * TEST-ONLY mutations (CK_TEST_HDA_MUTATION, refused with CK_HARDWARE_STAGING
 * and CK_QEMU_UNSAFE_DMA); each must make scripts/qemu_ck_hda_test.sh --mutation
 * see its row FAIL:
 *   1 no-revoke      the release skips the bus-master clear
 *   2 grant-no-smmu  with no SMMU the controller is granted DMA anyway
 *   3 wrong-tone     the tone is played at 1250 Hz while the log says 1000 Hz
 * QEMU is not hardware. */
#ifndef AIENOS_CK_HDA_FENCE_H
#define AIENOS_CK_HDA_FENCE_H
#include "pci.h"

#define CK_HDA_PCI_CLASS 0x040300u
#define CK_HDA_ACPI_ID "NVDA2014"
#ifndef CK_HDA_TONE_MS
#define CK_HDA_TONE_MS 1500u
#endif
#define CK_HDA_TONE_AMP 6000 /* about -15 dBFS */

enum {
    CK_HDAF_OK = 0,
    CK_HDAF_ABSENT = 1,       /* no controller: nothing to fence */
    CK_HDAF_E_ARG = -801,     /* BAR unusable, ambiguous, or a controller still live */
    CK_HDAF_E_BME = -802,     /* bus mastering still on before the gate */
    CK_HDAF_E_DENIED = -803,  /* no SMMU confinement: controller left off */
    CK_HDAF_E_DMA = -804,     /* DMA region allocation failed */
    CK_HDAF_E_DRIVER = -805,  /* bring-up or enumeration failed (still released) */
};

#define CK_HDA_MUT_NO_REVOKE 1
#define CK_HDA_MUT_GRANT_NO_SMMU 2
#define CK_HDA_MUT_WRONG_TONE 3

/* Steps 1-5. Always leaves the controller released before returning, except
 * under the TEST-ONLY no-revoke mutation. */
int ck_hda_fence(const pci_system *pci);
/* Stream stopped, controller in reset, bus master off, stream back to abort. Idempotent. */
void ck_hda_release(void);
/* 1 while the controller may still DMA. */
int ck_hda_live(void);
/* Short state for the devices summary line: absent, fenced, denied, ... */
const char *ck_hda_state(void);
#endif
