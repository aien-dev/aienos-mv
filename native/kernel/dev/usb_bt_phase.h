/* usb_bt_phase.h -- the Bluetooth step inside the xHCI DMA fence (L207).
 * Built only with CK_BT=1 (inventory and live HCI bring-up of a Bluetooth
 * controller) or CK_BT_TEST=1 (TEST-ONLY QEMU transfer proof on a mass-storage
 * stick and a keyboard); the default image links none of it and sends no byte
 * to any Bluetooth adapter. Runs after the keyboard phase, on the same
 * SMMU-confined DMA window, and leaves the controller for the fence to halt
 * and revoke. */
#ifndef AIENOS_CK_USB_BT_PHASE_H
#define AIENOS_CK_USB_BT_PHASE_H
#include <stddef.h>
#include <stdint.h>

#ifndef CK_BT
#define CK_BT 0
#endif
#ifndef CK_BT_TEST
#define CK_BT_TEST 0
#endif
#ifndef CK_BT_DISCOVER
#define CK_BT_DISCOVER 0 /* with CK_BT=1: also inquiry and an LE scan after bring-up */
#endif
#define CK_BT_ENABLED (CK_BT || CK_BT_TEST)

/* 0 when the phase ran to its end (whatever it found), negative xhci_pipe error otherwise. */
int ck_bt_phase(volatile uint8_t *bar0, uint8_t *mem, uint64_t phys, size_t bytes);
#endif
