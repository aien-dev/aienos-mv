/* usb_bt_test.c -- host test of dev/usb_bt.c (L207): the descriptor walk on
 * the real descriptors of the DGX Spark's Bluetooth adapter, event
 * reassembly across 16-byte interrupt packets, and the Bulk-Only packets.
 *
 * SPARK_BT_DESCRIPTORS is the device descriptor (18 bytes) followed by the
 * configuration descriptor (254 bytes) of USB 13d3:3630 ("MediaTek Inc.",
 * "Wireless_Device"), bus 11 port 2, copied on 2026-10-20 from
 * /sys/bus/usb/devices/11-2/descriptors with `head -c 272 | xxd -i`. That file
 * is the kernel's cached copy, so no request went to the adapter. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "usb_bt.h"

static int checks, failures;
#define CHECK(c)                                                       \
    do {                                                               \
        checks++;                                                      \
        if (!(c)) {                                                    \
            failures++;                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
        }                                                              \
    } while (0)

static const uint8_t SPARK_BT_DESCRIPTORS[272] = {
      0x12, 0x01, 0x10, 0x02, 0xef, 0x02, 0x01, 0x40, 0xd3, 0x13, 0x30, 0x36,
      0x00, 0x01, 0x05, 0x06, 0x07, 0x01, 0x09, 0x02, 0xfe, 0x00, 0x03, 0x01,
      0x08, 0xe0, 0x32, 0x08, 0x0b, 0x00, 0x03, 0xe0, 0x01, 0x01, 0x04, 0x09,
      0x04, 0x00, 0x00, 0x03, 0xe0, 0x01, 0x01, 0x01, 0x07, 0x05, 0x81, 0x03,
      0x10, 0x00, 0x01, 0x07, 0x05, 0x82, 0x02, 0x00, 0x02, 0x00, 0x07, 0x05,
      0x02, 0x02, 0x00, 0x02, 0x00, 0x09, 0x04, 0x01, 0x00, 0x02, 0xe0, 0x01,
      0x01, 0x02, 0x07, 0x05, 0x83, 0x01, 0x00, 0x00, 0x04, 0x07, 0x05, 0x03,
      0x01, 0x00, 0x00, 0x04, 0x09, 0x04, 0x01, 0x01, 0x02, 0xe0, 0x01, 0x01,
      0x02, 0x07, 0x05, 0x83, 0x01, 0x09, 0x00, 0x04, 0x07, 0x05, 0x03, 0x01,
      0x09, 0x00, 0x04, 0x09, 0x04, 0x01, 0x02, 0x02, 0xe0, 0x01, 0x01, 0x02,
      0x07, 0x05, 0x83, 0x01, 0x11, 0x00, 0x04, 0x07, 0x05, 0x03, 0x01, 0x11,
      0x00, 0x04, 0x09, 0x04, 0x01, 0x03, 0x02, 0xe0, 0x01, 0x01, 0x02, 0x07,
      0x05, 0x83, 0x01, 0x19, 0x00, 0x04, 0x07, 0x05, 0x03, 0x01, 0x19, 0x00,
      0x04, 0x09, 0x04, 0x01, 0x04, 0x02, 0xe0, 0x01, 0x01, 0x02, 0x07, 0x05,
      0x83, 0x01, 0x21, 0x00, 0x04, 0x07, 0x05, 0x03, 0x01, 0x21, 0x00, 0x04,
      0x09, 0x04, 0x01, 0x05, 0x02, 0xe0, 0x01, 0x01, 0x02, 0x07, 0x05, 0x83,
      0x01, 0x31, 0x00, 0x04, 0x07, 0x05, 0x03, 0x01, 0x31, 0x00, 0x04, 0x09,
      0x04, 0x01, 0x06, 0x02, 0xe0, 0x01, 0x01, 0x02, 0x07, 0x05, 0x83, 0x01,
      0x3f, 0x00, 0x04, 0x07, 0x05, 0x03, 0x01, 0x3f, 0x00, 0x04, 0x09, 0x04,
      0x02, 0x00, 0x02, 0xe0, 0x01, 0x01, 0x03, 0x07, 0x05, 0x8a, 0x03, 0x40,
      0x00, 0x01, 0x07, 0x05, 0x0a, 0x03, 0x40, 0x00, 0x01, 0x09, 0x04, 0x02,
      0x01, 0x02, 0xe0, 0x01, 0x01, 0x03, 0x07, 0x05, 0x8a, 0x03, 0x00, 0x02,
      0x01, 0x07, 0x05, 0x0a, 0x03, 0x00, 0x02, 0x01
};

static void test_spark(void)
{
    ck_usb_bt_desc d;
    const uint8_t *dev = SPARK_BT_DESCRIPTORS, *cfg = SPARK_BT_DESCRIPTORS + 18;
    CHECK(ck_usb_bt_parse(dev, 18, cfg, 254, &d) == 0);
    CHECK(d.vid == 0x13d3 && d.pid == 0x3630);
    CHECK(d.dev_class == 0xef && d.dev_sub == 0x02 && d.dev_proto == 0x01); /* composite, interface association */
    CHECK(d.configuration == 1 && d.n_interfaces == 3 && d.interface == 0);
    CHECK(d.ep_event == 0x81 && d.event_mps == 16 && d.event_interval == 1);
    CHECK(d.ep_acl_in == 0x82 && d.acl_in_mps == 512);
    CHECK(d.ep_acl_out == 0x02 && d.acl_out_mps == 512);
    CHECK(d.n_alt_other == 9); /* interface 1: alternate settings 0 to 6 (ISO, SCO voice); interface 2: alternate settings 0 and 1 (interrupt, vendor channel); none used */
    CHECK(d.cmd_rtype == 0x21 && d.cmd_index == 0); /* composite: interface-addressed (Vol 4 Part B 2.2.2) */
    /* the same bytes with the cfg length cut short are refused, never read past */
    CHECK(ck_usb_bt_parse(dev, 18, cfg, 8, &d) == CK_UBT_E_MALFORMED);
    for (size_t cut = 9; cut < 254; cut++) {
        int rc = ck_usb_bt_parse(dev, 18, cfg, cut, &d);
        CHECK(rc == 0 || rc == CK_UBT_E_MALFORMED || rc == CK_UBT_E_ENDPOINTS || rc == CK_UBT_E_NOTBT);
    }
    /* a device-class 0xe0 controller gets device-addressed commands */
    uint8_t dev2[18];
    memcpy(dev2, dev, 18);
    dev2[4] = 0xe0; dev2[5] = 1; dev2[6] = 1;
    CHECK(ck_usb_bt_parse(dev2, 18, cfg, 254, &d) == 0 && d.cmd_rtype == 0x20 && d.cmd_index == 0);
    CHECK(ck_usb_bt_parse(0, 18, cfg, 254, &d) == CK_UBT_E_ARG);
    CHECK(ck_usb_bt_parse(dev, 17, cfg, 254, &d) == CK_UBT_E_MALFORMED);
    /* no HCI interface: turn every e0/01/01 into e0/01/02 */
    uint8_t cfg2[254];
    memcpy(cfg2, cfg, 254);
    for (size_t o = 9; o + 9 <= 254;) {
        if (cfg2[o + 1] == 4 && cfg2[o + 7] == 1) cfg2[o + 7] = 2;
        if (cfg2[o] < 2) break;
        o += cfg2[o];
    }
    CHECK(ck_usb_bt_parse(dev, 18, cfg2, 254, &d) == CK_UBT_E_NOTBT);
    /* HCI interface without its bulk OUT endpoint */
    memcpy(cfg2, cfg, 254);
    for (size_t o = 9; o + 7 <= 254;) {
        if (cfg2[o + 1] == 5 && cfg2[o + 2] == 0x02) cfg2[o + 2] = 0x82; /* second bulk IN, no OUT */
        if (cfg2[o] < 2) break;
        o += cfg2[o];
    }
    CHECK(ck_usb_bt_parse(dev, 18, cfg2, 254, &d) == CK_UBT_E_ENDPOINTS);
    /* a zero-length descriptor must not loop */
    memcpy(cfg2, cfg, 254);
    cfg2[9] = 0;
    CHECK(ck_usb_bt_parse(dev, 18, cfg2, 254, &d) == CK_UBT_E_MALFORMED);
}

static void test_asm(void)
{
    ck_usb_bt_asm a;
    uint8_t ev[40], out[300];
    uint32_t n = 0;
    ck_usb_bt_asm_init(&a);
    ev[0] = 0x0e; ev[1] = 38;
    for (int i = 2; i < 40; i++) ev[i] = (uint8_t)i;
    /* 40 bytes arrive as 16 + 16 + 8 */
    CHECK(ck_usb_bt_asm_feed(&a, ev, 16) == 0 && ck_usb_bt_asm_take(&a, out, sizeof out, &n) == 0);
    CHECK(ck_usb_bt_asm_feed(&a, ev + 16, 16) == 0 && ck_usb_bt_asm_take(&a, out, sizeof out, &n) == 0);
    CHECK(ck_usb_bt_asm_feed(&a, ev + 32, 8) == 0 && ck_usb_bt_asm_take(&a, out, sizeof out, &n) == 1);
    CHECK(n == 40 && memcmp(out, ev, 40) == 0 && a.have == 0);
    /* two events in one chunk, then one split after its header byte */
    uint8_t two[9] = {0x0f, 4, 0, 1, 3, 0x0c, 0x0e, 1, 0};
    CHECK(ck_usb_bt_asm_feed(&a, two, 9) == 0);
    CHECK(ck_usb_bt_asm_take(&a, out, sizeof out, &n) == 1 && n == 6 && out[0] == 0x0f);
    CHECK(ck_usb_bt_asm_take(&a, out, sizeof out, &n) == 1 && n == 3 && out[0] == 0x0e && a.have == 0);
    /* overflow clears and reports */
    ck_usb_bt_asm_init(&a);
    uint8_t big[CK_BT_EVT_MAX];
    memset(big, 0xff, sizeof big);
    CHECK(ck_usb_bt_asm_feed(&a, big, sizeof big) == 0);
    CHECK(ck_usb_bt_asm_feed(&a, big, sizeof big) == 0);
    CHECK(ck_usb_bt_asm_feed(&a, big, 1) == CK_UBT_E_OVERFLOW && a.have == 0);
    /* a header that promises more than the caller's buffer */
    ck_usb_bt_asm_init(&a);
    uint8_t long_ev[2 + 255];
    memset(long_ev, 0, sizeof long_ev);
    long_ev[0] = 0x3e; long_ev[1] = 255;
    CHECK(ck_usb_bt_asm_feed(&a, long_ev, sizeof long_ev) == 0);
    CHECK(ck_usb_bt_asm_take(&a, out, 100, &n) == CK_UBT_E_OVERFLOW && a.have == 0);
}

static void test_bot(void)
{
    uint8_t cbw[31], cdb[6] = {0x12, 0, 0, 0, 36, 0};
    CHECK(ck_usb_bot_cbw(cbw, 0x11223344u, 36, 1, cdb, 6) == 0);
    CHECK(memcmp(cbw, "USBC", 4) == 0 && cbw[4] == 0x44 && cbw[7] == 0x11 && cbw[8] == 36 && cbw[12] == 0x80);
    CHECK(cbw[14] == 6 && cbw[15] == 0x12 && cbw[19] == 36 && cbw[20] == 0 && cbw[30] == 0);
    CHECK(ck_usb_bot_cbw(cbw, 1, 0, 0, cdb, 17) == CK_UBT_E_ARG);
    uint8_t csw[13] = {'U', 'S', 'B', 'S', 0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0, 0};
    uint32_t res = 99;
    CHECK(ck_usb_bot_csw(csw, 0x11223344u, &res) == 0 && res == 0);
    CHECK(ck_usb_bot_csw(csw, 0x11223345u, &res) == CK_UBT_E_MALFORMED); /* wrong tag */
    csw[12] = 1;
    CHECK(ck_usb_bot_csw(csw, 0x11223344u, &res) == CK_UBT_E_MALFORMED); /* command failed */
    csw[12] = 0; csw[0] = 'X';
    CHECK(ck_usb_bot_csw(csw, 0x11223344u, &res) == CK_UBT_E_MALFORMED); /* bad signature */
    /* a mass-storage configuration */
    uint8_t cfg[] = {9, 2, 32, 0, 1, 1, 0, 0x80, 50,
                     9, 4, 0, 0, 2, 8, 6, 0x50, 0,
                     7, 5, 0x81, 2, 0, 2, 0,
                     7, 5, 0x02, 2, 0, 2, 0};
    cfg[2] = sizeof cfg;
    ck_usb_bot_desc b;
    CHECK(ck_usb_find_bot(cfg, sizeof cfg, &b) == 0 && b.ep_in == 0x81 && b.ep_out == 0x02 && b.mps_in == 512 && b.configuration == 1);
    cfg[16] = 0x62; /* protocol UAS: not BOT */
    CHECK(ck_usb_find_bot(cfg, sizeof cfg, &b) == CK_UBT_E_NOTBT);
}

int main(void)
{
    test_spark();
    test_asm();
    test_bot();
    printf("CK_USB_BT_HOST: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
