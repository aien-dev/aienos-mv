/* usb_bt_phase.c -- kernel half of the USB Bluetooth transport (see
 * usb_bt.h, usb_bt_phase.h). Freestanding, polled. */
#include "usb_bt_phase.h"

#if CK_BT_ENABLED
#include "bt_hci.h"
#include "ck.h"
#include "usb_bt.h"
#include "usb_hid.h"
#include "xhci_pipe.h"

#define CFG_MAX 1024u

static ck_xp g_xp;
static uint8_t g_cfg[CFG_MAX];

typedef struct {
    ck_xp *x;
    ck_usb_bt_desc d;
    ck_usb_bt_asm as;
    int armed;
    int fell_back;
} bt_usb;

static bt_usb g_bt;

static int arm_events(bt_usb *u)
{
    int rc = ck_xp_intr_arm(u->x, u->d.ep_event, u->d.event_mps);
    u->armed = rc == 0;
    return rc;
}

/* HCI command: one control transfer on endpoint 0 (Vol 4 Part B 2.2). A composite
 * device is addressed through its interface first; a STALL on the very first
 * command falls back once to the device-addressed form the spec says must also work. */
static int tr_send(void *ctx, const uint8_t *pkt, uint32_t len)
{
    bt_usb *u = ctx;
    int rc = ck_xp_control(u->x, u->d.cmd_rtype, 0, 0, u->d.cmd_index, (void *)(uintptr_t)pkt, (uint16_t)len, 0, 1000);
    if (rc == CK_XP_E_STALL && !u->fell_back && u->d.cmd_rtype == 0x21) {
        u->fell_back = 1;
        u->d.cmd_rtype = 0x20;
        u->d.cmd_index = 0;
        ck_printf("bt: interface-addressed command stalled, retrying device-addressed (0x20)\n");
        rc = ck_xp_control(u->x, u->d.cmd_rtype, 0, 0, u->d.cmd_index, (void *)(uintptr_t)pkt, (uint16_t)len, 0, 1000);
    }
    return rc;
}

/* HCI events: interrupt IN transfers of at most event_mps bytes, reassembled. */
static int tr_poll(void *ctx, uint8_t *buf, uint32_t cap, uint32_t *len)
{
    bt_usb *u = ctx;
    int t = ck_usb_bt_asm_take(&u->as, buf, cap, len);
    if (t < 0) return -1;
    if (t == 1) return 0;
    if (!u->armed && arm_events(u)) return -1;
    uint8_t chunk[CK_XP_INTR_BYTES];
    uint32_t got = 0;
    int rc = ck_xp_intr_poll(u->x, u->d.ep_event, chunk, sizeof chunk, &got);
    if (rc > 0) return 1;
    if (rc < 0) return -1;
    u->armed = 0;
    if (arm_events(u)) return -1;
    if (got && ck_usb_bt_asm_feed(&u->as, chunk, got)) return -1;
    t = ck_usb_bt_asm_take(&u->as, buf, cap, len);
    return t < 0 ? -1 : t == 1 ? 0 : 1;
}

static void print_addr(const char *tag, const uint8_t a[6])
{
    ck_printf("%s%02x:%02x:%02x:%02x:%02x:%02x", tag, a[5], a[4], a[3], a[2], a[1], a[0]);
}

#if CK_BT
static int bt_live(ck_xp *x, const ck_usb_bt_desc *d)
{
    ck_xp_ep eps[3] = {
        {d->ep_event, CK_XP_EP_INTR, d->event_mps, d->event_interval},
        {d->ep_acl_in, CK_XP_EP_BULK, d->acl_in_mps, 0},
        {d->ep_acl_out, CK_XP_EP_BULK, d->acl_out_mps, 0},
    };
    int rc = ck_xp_configure(x, d->configuration, eps, 3);
    if (rc) {
        ck_printf("bt: configure failed (%s, rc=%d, completion %u)\n", x->why, rc, x->last_code);
        return rc;
    }
    g_bt.x = x;
    g_bt.d = *d;
    ck_usb_bt_asm_init(&g_bt.as);
    if (arm_events(&g_bt)) {
        ck_printf("bt: could not queue the event transfer (%s)\n", x->why);
        return CK_BT_E_TRANSPORT;
    }
    ck_bt_transport tr = {tr_send, tr_poll, &g_bt};
    static ck_bt_host h;
    ck_bt_host_init(&h, &tr);
    h.poll_budget = 50000; /* each poll is 20 us or more of xHCI event ring reads: bounded, about a second */
    ck_printf("bt: hci bring-up (Reset, Read Local Version, Read BD_ADDR, Set Event Mask); no firmware is loaded\n");
    rc = ck_bt_bringup(&h);
    if (rc) {
        ck_printf("bt: bring-up stopped (rc=%d, events seen=%u, dropped=%u)\n", rc, h.events, h.dropped);
        return rc;
    }
    ck_printf("bt: hci_version=0x%02x hci_subversion=0x%04x lmp_version=0x%02x lmp_subversion=0x%04x company=0x%04x\n",
              h.hci_version, h.hci_subversion, h.lmp_version, h.lmp_subversion, h.company);
    print_addr("bt: bd_addr=", h.bd_addr);
    ck_printf("\n");
#if CK_BT_DISCOVER
    rc = ck_bt_inquiry(&h, 2, 8);
    ck_printf("bt: inquiry rc=%d devices=%u\n", rc, h.n_bredr);
    for (uint32_t i = 0; i < h.n_bredr; i++) {
        print_addr("bt:   bredr ", h.bredr[i].addr);
        ck_printf(" cod=%02x%02x%02x\n", h.bredr[i].cod[2], h.bredr[i].cod[1], h.bredr[i].cod[0]);
    }
    rc = ck_bt_le_scan(&h, 20000);
    ck_printf("bt: le scan rc=%d devices=%u\n", rc, h.n_le);
    for (uint32_t i = 0; i < h.n_le; i++) {
        print_addr("bt:   le ", h.le[i].addr);
        ck_printf(" type=%u rssi=%d data=%u bytes\n", h.le[i].addr_type, (int)(int8_t)h.le[i].rssi, h.le[i].data_len);
    }
#endif
    return 0;
}
#endif /* CK_BT */

#if CK_BT_TEST
/* ---- TEST-ONLY transfer proof on QEMU: bulk (usb-storage, BOT) and interrupt (usb-kbd) ---- */
static int bot_cmd(ck_xp *x, const ck_usb_bot_desc *b, uint32_t tag, const uint8_t *cdb, uint8_t cl, int in, void *data,
                   uint32_t len)
{
    uint8_t cbw[31], csw[13];
    uint32_t got = 0, residue = 0;
    ck_usb_bot_cbw(cbw, tag, len, in, cdb, cl);
    int rc = ck_xp_bulk_out(x, b->ep_out, cbw, sizeof cbw, 2000);
    if (rc) return rc;
    if (len && in) {
        rc = ck_xp_bulk_in(x, b->ep_in, data, len, &got, 2000);
        if (rc) return rc;
        if (got != len) return CK_XP_E_FAILED;
    } else if (len) {
        rc = ck_xp_bulk_out(x, b->ep_out, data, len, 2000);
        if (rc) return rc;
    }
    rc = ck_xp_bulk_in(x, b->ep_in, csw, sizeof csw, &got, 2000);
    if (rc) return rc;
    if (got != sizeof csw) return CK_XP_E_FAILED;
    return ck_usb_bot_csw(csw, tag, &residue) == 0 && residue == 0 ? 0 : CK_XP_E_FAILED;
}

static void test_bulk(ck_xp *x, const ck_usb_bot_desc *b)
{
    ck_xp_ep eps[2] = {{b->ep_in, CK_XP_EP_BULK, b->mps_in, 0}, {b->ep_out, CK_XP_EP_BULK, b->mps_out, 0}};
    int rc = ck_xp_configure(x, b->configuration, eps, 2);
    if (rc) {
        ck_printf("bt_xfer: bulk configure failed (%s, rc=%d)\n", x->why, rc);
        return;
    }
    static uint8_t buf[4096];
    uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
    rc = bot_cmd(x, b, 0x1001, inq, 6, 1, buf, 36);
    if (rc) {
        ck_printf("bt_xfer: bulk INQUIRY failed (%s, rc=%d, completion %u)\n", x->why, rc, x->last_code);
        return;
    }
    char vendor[9], product[17];
    for (int i = 0; i < 8; i++) vendor[i] = buf[8 + i] >= 32 && buf[8 + i] < 127 ? (char)buf[8 + i] : '?';
    for (int i = 0; i < 16; i++) product[i] = buf[16 + i] >= 32 && buf[16 + i] < 127 ? (char)buf[16 + i] : '?';
    vendor[8] = product[16] = 0;
    ck_printf("bt_xfer: bulk INQUIRY ok (36 bytes in, max packet in=%u out=%u) vendor='%s' product='%s'\n", b->mps_in, b->mps_out,
              vendor, product);
    /* READ(10) 8 blocks from LBA 0: a 4096-byte bulk IN, many packets, against a pattern the gate script wrote. */
    uint8_t rd[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 8, 0};
    rc = bot_cmd(x, b, 0x1002, rd, 10, 1, buf, 4096);
    if (rc) {
        ck_printf("bt_xfer: bulk READ(10) failed (%s, rc=%d, completion %u)\n", x->why, rc, x->last_code);
        return;
    }
    static const char pat[] = "AIENOS-L207-BULK-";
    unsigned bad = 0;
    for (unsigned i = 0; i < 4096; i++) bad += buf[i] != (uint8_t)pat[i % (sizeof pat - 1)];
    ck_printf("bt_xfer: bulk READ(10) 4096 bytes in, pattern %s (%u bad bytes)\n", bad ? "MISMATCH" : "ok", bad);
    /* WRITE(10) one block at LBA 100, READ it back: bulk OUT data stage, then the same bytes on bulk IN. */
    static uint8_t blk[512], back[512];
    for (unsigned i = 0; i < 512; i++) blk[i] = (uint8_t)(i * 7u + 3u);
    uint8_t wr[10] = {0x2a, 0, 0, 0, 0, 100, 0, 0, 1, 0};
    uint8_t rb[10] = {0x28, 0, 0, 0, 0, 100, 0, 0, 1, 0};
    rc = bot_cmd(x, b, 0x1003, wr, 10, 0, blk, 512);
    if (!rc) rc = bot_cmd(x, b, 0x1004, rb, 10, 1, back, 512);
    unsigned diff = 0;
    for (unsigned i = 0; i < 512; i++) diff += back[i] != blk[i];
    ck_printf("bt_xfer: bulk WRITE(10)+READ(10) 512 bytes round trip %s (rc=%d, %u differing bytes)\n", rc == 0 && diff == 0 ? "ok" : "FAILED", rc, diff);
}

static void test_interrupt(ck_xp *x, const ck_boot_kbd *k)
{
    ck_xp_ep ep = {k->endpoint, CK_XP_EP_INTR, k->max_packet, k->interval};
    int rc = ck_xp_configure(x, k->configuration, &ep, 1);
    if (rc) {
        ck_printf("bt_xfer: interrupt configure failed (%s, rc=%d)\n", x->why, rc);
        return;
    }
    if ((rc = ck_xp_intr_arm(x, k->endpoint, 8))) {
        ck_printf("bt_xfer: interrupt arm failed (%s, rc=%d)\n", x->why, rc);
        return;
    }
    ck_printf("bt_xfer: interrupt in armed (endpoint 0x%02x, max packet %u), waiting for a key\n", k->endpoint, k->max_packet);
    uint64_t t0 = ck_time_us();
    uint8_t rep[8];
    uint32_t got = 0;
    while (ck_time_us() - t0 < 30000000u) {
        rc = ck_xp_intr_poll(x, k->endpoint, rep, sizeof rep, &got);
        if (rc < 0) {
            ck_printf("bt_xfer: interrupt poll failed (%s, rc=%d, completion %u)\n", x->why, rc, x->last_code);
            return;
        }
        if (rc == 0 && got) {
            ck_printf("bt_xfer: interrupt report %u bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n", got, rep[0], rep[1], rep[2],
                      rep[3], rep[4], rep[5], rep[6], rep[7]);
            return;
        }
        ck_udelay(200);
    }
    ck_printf("bt_xfer: interrupt in saw no report in 30 s\n");
}
#endif /* CK_BT_TEST */

int ck_bt_phase(volatile uint8_t *bar0, uint8_t *mem, uint64_t phys, size_t bytes)
{
#if defined(CK_TEST_XP_MUT)
    ck_printf("WARNING: TEST-ONLY USB PIPE MUTATION BUILD (CK_TEST_BT_MUTATION=%d)\n", (int)CK_TEST_XP_MUT);
#endif
    int rc = ck_xp_start(&g_xp, bar0, mem, phys, bytes);
    if (rc) {
        ck_printf("bt: unavailable (%s, rc=%d)\n", g_xp.why, rc);
        return rc;
    }
    ck_printf("bt: xhci pipes running ports=%u context_bytes=%u\n", ck_xp_max_ports(&g_xp), g_xp.ctx);
    int found_bt = 0, did_bulk = 0, did_intr = 0;
    for (unsigned p = 1; p <= ck_xp_max_ports(&g_xp) && p <= 255u; p++) {
        if (!ck_xp_port_present(&g_xp, p)) continue;
        rc = ck_xp_attach(&g_xp, p);
        if (rc) {
            ck_printf("bt: port %u not usable (%s, rc=%d, completion %u)\n", p, g_xp.why, rc, g_xp.last_code);
            ck_xp_detach(&g_xp);
            continue;
        }
        const uint8_t *dd = g_xp.dev_desc;
        ck_printf("bt: port %u device %04x:%04x class %02x/%02x/%02x\n", p, dd[8] | (dd[9] << 8), dd[10] | (dd[11] << 8), dd[4], dd[5],
                  dd[6]);
        size_t cl = 0;
        rc = ck_xp_config_desc(&g_xp, 0, g_cfg, sizeof g_cfg, &cl);
        if (rc) {
            ck_printf("bt: port %u configuration descriptor failed (%s, rc=%d)\n", p, g_xp.why, rc);
            ck_xp_detach(&g_xp);
            continue;
        }
        ck_usb_bt_desc d;
        int prc = ck_usb_bt_parse(dd, 18, g_cfg, cl, &d);
        if (prc == 0) {
            found_bt++;
            ck_printf("bt: controller usb=%04x:%04x interface=%u event=0x%02x/%u acl_in=0x%02x/%u acl_out=0x%02x/%u "
                      "command_request=0x%02x other_alt_settings=%u\n",
                      d.vid, d.pid, d.interface, d.ep_event, d.event_mps, d.ep_acl_in, d.acl_in_mps, d.ep_acl_out, d.acl_out_mps,
                      d.cmd_rtype, d.n_alt_other);
#if CK_BT
            (void)bt_live(&g_xp, &d);
#else
            ck_printf("bt: inventory only (this image was not built with CK_BT=1, no HCI command is sent)\n");
#endif
        }
#if CK_BT_TEST
        ck_usb_bot_desc b;
        ck_boot_kbd k;
        if (!did_bulk && ck_usb_find_bot(g_cfg, cl, &b) == 0) {
            did_bulk = 1;
            test_bulk(&g_xp, &b);
        } else if (!did_intr && ck_usb_find_boot_kbd(g_cfg, cl, &k) == 0) {
            did_intr = 1;
            test_interrupt(&g_xp, &k);
        }
#endif
        ck_xp_detach(&g_xp);
    }
    (void)did_bulk;
    (void)did_intr;
    if (!found_bt) ck_printf("bt: no Bluetooth controller\n");
    return 0;
}

#else /* !CK_BT_ENABLED */
int ck_bt_phase(volatile uint8_t *bar0, uint8_t *mem, uint64_t phys, size_t bytes)
{
    (void)bar0;
    (void)mem;
    (void)phys;
    (void)bytes;
    return 0;
}
#endif
