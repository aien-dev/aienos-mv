/* usb_bt.h -- Bluetooth HCI over USB (L207): descriptor walk, event
 * reassembly, and the transport that joins dev/bt_hci.c to the xHCI pipes.
 *
 * Reference: Bluetooth Core v5.4 Vol 4 Part B (HCI USB transport):
 *   commands   control endpoint 0, class request bmRequestType 0x20 (device)
 *              or 0x21 (interface, composite device), bRequest 0 (section 2.2)
 *   events     interrupt IN endpoint, 0x81 in the suggested layout (Table 2.1)
 *   ACL data   bulk IN 0x82 / bulk OUT 0x02 (Table 2.1), not used here
 *   class      0xE0 / 0x01 / 0x01 on the interface (section 3.1)
 * and the descriptors of the DGX Spark's MediaTek adapter 13d3:3630, read
 * from sysfs on 2026-10-20 (a read-only copy of what the kernel cached; no
 * request was sent to the device): usb_bt_test.c holds the 272 bytes.
 *
 * The pure half (parse, reassembly) is host tested. The kernel half
 * (dev/usb_bt_phase.c) runs inside the xHCI fence after the keyboard phase and
 * only with CK_BT=1 (live HCI) or CK_BT_TEST=1 (QEMU transfer proof); the
 * default image never sends a byte to a Bluetooth adapter.
 *
 * No firmware is loaded and none is shipped: the MT7925 loads its Bluetooth
 * RAM code through MediaTek vendor commands, which this driver does not issue
 * (docs/SPARK_BT_HDA.md). QEMU is not hardware. */
#ifndef AIENOS_CK_USB_BT_H
#define AIENOS_CK_USB_BT_H
#include <stddef.h>
#include <stdint.h>

#include "bt_hci.h"

#define CK_UBT_CLASS 0xe0u
#define CK_UBT_SUBCLASS 0x01u
#define CK_UBT_PROTOCOL 0x01u

enum {
    CK_UBT_OK = 0,
    CK_UBT_E_ARG = -931,        /* bad pointer or buffer */
    CK_UBT_E_MALFORMED = -932,  /* a descriptor length or type does not hold together */
    CK_UBT_E_NOTBT = -933,      /* no interface of class e0/01/01 */
    CK_UBT_E_ENDPOINTS = -934,  /* the interface lacks interrupt IN, bulk IN or bulk OUT */
    CK_UBT_E_OVERFLOW = -935,   /* event larger than any HCI event, or buffer full */
};

typedef struct {
    uint16_t vid, pid;
    uint8_t dev_class, dev_sub, dev_proto;
    uint8_t configuration;     /* bConfigurationValue */
    uint8_t interface;         /* bInterfaceNumber of the HCI interface (alternate setting 0) */
    uint8_t n_interfaces;      /* interfaces in the configuration */
    uint8_t n_alt_other;       /* alternate settings of class e0/01/01 on interfaces other than the HCI one (SCO/ISO and vendor channels) */
    uint8_t ep_event, ep_acl_in, ep_acl_out;
    uint16_t event_mps, acl_in_mps, acl_out_mps;
    uint8_t event_interval;
    uint8_t cmd_rtype;         /* 0x20, or 0x21 on a composite device (Vol 4 Part B 2.2.2) */
    uint16_t cmd_index;        /* wIndex of the command request */
} ck_usb_bt_desc;

/* dev: 18-byte device descriptor, cfg: whole configuration descriptor. Every
 * length is bounds checked against min(len, wTotalLength). */
int ck_usb_bt_parse(const uint8_t *dev, size_t devlen, const uint8_t *cfg, size_t cfglen, ck_usb_bt_desc *out);

/* Bulk-Only Mass Storage (class 8 / 6 / 0x50): QEMU transfer proof only. */
typedef struct {
    uint8_t configuration, interface, ep_in, ep_out;
    uint16_t mps_in, mps_out;
} ck_usb_bot_desc;
int ck_usb_find_bot(const uint8_t *cfg, size_t len, ck_usb_bot_desc *out);

/* Event reassembly: interrupt transfers are at most the endpoint's packet size
 * (16 bytes on the Spark's adapter), an HCI event can be up to 257 bytes. */
typedef struct {
    uint8_t buf[2u * CK_BT_EVT_MAX];
    uint32_t have;
} ck_usb_bt_asm;
void ck_usb_bt_asm_init(ck_usb_bt_asm *a);
/* Append a received chunk. CK_UBT_OK, or CK_UBT_E_OVERFLOW (the buffer is then cleared). */
int ck_usb_bt_asm_feed(ck_usb_bt_asm *a, const uint8_t *chunk, uint32_t n);
/* Remove one complete event packet (header included) into out; 1 if there was one, 0 if not yet. */
int ck_usb_bt_asm_take(ck_usb_bt_asm *a, uint8_t *out, uint32_t cap, uint32_t *len);

/* Bulk-Only Transport structures (USB MSC BOT 1.0), QEMU proof only. */
int ck_usb_bot_cbw(uint8_t cbw[31], uint32_t tag, uint32_t data_len, int data_in, const uint8_t *cdb, uint8_t cdb_len);
/* 0 when the CSW is valid for tag with status 0 (and *residue set), else <0. */
int ck_usb_bot_csw(const uint8_t csw[13], uint32_t tag, uint32_t *residue);
#endif
