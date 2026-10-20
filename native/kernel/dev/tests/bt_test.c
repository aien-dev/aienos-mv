/* bt_test.c -- host test of dev/bt_hci.c against a software HCI controller
 * (L207). QEMU 8.2.2 on this machine has no usb-bt-dongle, so the host state
 * machine is proven against this simulator, which follows Bluetooth Core v5.4
 * Vol 4 Part E for the commands the host uses and checks the host's side of
 * flow control: a command sent while the host holds no credit is a violation.
 *
 * The simulator is NOT the MediaTek MT7925 controller: nothing is tested about
 * its firmware or about USB here (dev/tests/usb_bt_test.c covers the transport).
 * The identity it reports (company 0x0046, MediaTek) is the value assigned for
 * MediaTek; the version numbers are made up. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bt_hci.h"

static int checks, failures;
#define CHECK(c)                                                       \
    do {                                                               \
        checks++;                                                      \
        if (!(c)) {                                                    \
            failures++;                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
        }                                                              \
    } while (0)

#define QN 24
typedef struct {
    uint8_t q[QN][260];
    uint32_t ql[QN], ready_at[QN];
    int qh, qt;
    uint32_t polls;
    int host_credit; /* credits the host holds, from the events it has been delivered */
    int violations;  /* host sent a command it had no credit for, or a malformed packet */
    uint16_t opcodes[64];
    int nops;
    uint64_t mask;
    int scan_on, scan_enable_cmds;
    int zero_credit_after_reset; /* announce Num=0 for Reset, refill with a NOP completion later */
    int fail_connect;            /* LE Connection Complete carries 0x3e */
    int never_complete;          /* swallow Create Connection's completion event */
    int status_error_opcode;     /* answer this opcode with HCI status 0x0c */
    int fail_poll;               /* transport error from poll_event */
    uint32_t delay;              /* polls before each queued event becomes ready */
    uint8_t last_params[260];
    uint8_t last_plen;
    int connected;
} sim_t;

static void push(sim_t *s, const uint8_t *ev, uint32_t len, uint32_t extra_delay) {
    memcpy(s->q[s->qt], ev, len);
    s->ql[s->qt] = len;
    s->ready_at[s->qt] = s->polls + s->delay + extra_delay;
    s->qt = (s->qt + 1) % QN;
}

static void push_cc(sim_t *s, uint16_t op, const uint8_t *rp, uint8_t rl, uint8_t num) {
    uint8_t e[260];
    e[0] = 0x0e; e[1] = (uint8_t)(3 + rl); e[2] = num; e[3] = (uint8_t)op; e[4] = (uint8_t)(op >> 8);
    memcpy(e + 5, rp, rl);
    push(s, e, 5u + rl, 0);
}

static void push_cs(sim_t *s, uint16_t op, uint8_t status) {
    uint8_t e[6] = {0x0f, 4, status, 1, (uint8_t)op, (uint8_t)(op >> 8)};
    push(s, e, 6, 0);
}

static int sim_send(void *ctx, const uint8_t *pkt, uint32_t len) {
    sim_t *s = ctx;
    if (len < 3 || len != 3u + pkt[2]) { s->violations++; return -1; }
    uint16_t op = (uint16_t)(pkt[0] | (pkt[1] << 8));
    uint8_t plen = pkt[2];
    if (s->host_credit <= 0) s->violations++;
    s->host_credit = 0;
    if (s->nops < 64) s->opcodes[s->nops++] = op;
    memcpy(s->last_params, pkt + 3, plen);
    s->last_plen = plen;
    uint8_t ok[1] = {0};
    if (op == s->status_error_opcode && op) { uint8_t bad[1] = {0x0c}; push_cc(s, op, bad, 1, 1); return 0; }
    switch (op) {
    case CK_BT_OP_RESET:
        if (plen) s->violations++;
        if (s->zero_credit_after_reset) {
            push_cc(s, op, ok, 1, 0);
            uint8_t nop[5] = {0x0e, 3, 1, 0, 0};
            push(s, nop, 5, 7);
        } else push_cc(s, op, ok, 1, 1);
        break;
    case CK_BT_OP_READ_LOCAL_VERSION: {
        uint8_t r[9] = {0, 0x0d, 0x34, 0x12, 0x0d, 0x46, 0x00, 0x78, 0x56};
        push_cc(s, op, r, 9, 1);
        break; }
    case CK_BT_OP_READ_BD_ADDR: {
        uint8_t r[7] = {0, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
        push_cc(s, op, r, 7, 1);
        break; }
    case CK_BT_OP_SET_EVENT_MASK:
        if (plen != 8) s->violations++;
        else { s->mask = 0; for (int i = 0; i < 8; i++) s->mask |= (uint64_t)pkt[3 + i] << (8 * i); }
        push_cc(s, op, ok, 1, 1);
        break;
    case CK_BT_OP_INQUIRY: {
        if (plen != 5 || pkt[3] != 0x33 || pkt[4] != 0x8b || pkt[5] != 0x9e) s->violations++;
        push_cs(s, op, 0);
        /* event 1: two devices; event 2: one new, one repeat. Layout: arrays of each field. */
        uint8_t a[2 + 1 + 14 * 2 - 0];
        memset(a, 0, sizeof a);
        a[0] = 0x02; a[1] = 1 + 14 * 2; a[2] = 2;
        uint8_t *p = a + 3;
        memcpy(p, "\x01\x00\x00\x00\x00\xaa" "\x02\x00\x00\x00\x00\xbb", 12);
        p += 12;                   /* BD_ADDR[2] */
        p[0] = 1; p[1] = 1; p += 2; /* PSRM[2] */
        p += 4;                    /* Reserved[2] x2 */
        memcpy(p, "\x04\x05\x24" "\x0c\x02\x5a", 6); /* COD[2] x3 */
        p += 6;
        p += 4;                    /* Clock_Offset[2] x2 */
        push(s, a, 2u + a[1], 0);
        uint8_t b[2 + 1 + 14 * 2];
        memset(b, 0, sizeof b);
        b[0] = 0x02; b[1] = 1 + 14 * 2; b[2] = 2;
        p = b + 3;
        memcpy(p, "\x02\x00\x00\x00\x00\xbb" "\x03\x00\x00\x00\x00\xcc", 12);
        p += 12 + 2 + 4;
        memcpy(p, "\x0c\x02\x5a" "\x00\x01\x1f", 6);
        uint8_t done[3] = {0x01, 1, 0};
        push(s, b, 2u + b[1], 0);
        push(s, done, 3, 3);
        break; }
    case CK_BT_OP_LE_SET_SCAN_PARAMS:
        if (plen != 7) s->violations++;
        push_cc(s, op, ok, 1, 1);
        break;
    case CK_BT_OP_LE_SET_SCAN_ENABLE:
        if (plen != 2) s->violations++;
        s->scan_enable_cmds++;
        s->scan_on = pkt[3];
        push_cc(s, op, ok, 1, 1);
        if (s->scan_on) {
            /* one report for peer 11:..:11 with 3 data bytes, then two reports: peer 22 (new) and 11 again (update) */
            uint8_t r1[] = {0x3e, 0, 0x02, 1, 0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 3, 0x02, 0x01, 0x06, 0xc4};
            r1[1] = sizeof r1 - 2;
            push(s, r1, sizeof r1, 1);
            uint8_t r2[] = {0x3e, 0, 0x02, 2, 0x03, 0x00, 0x00, 0x00,
                            0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                            1, 2, 0xff, 0xee, 0xdd, 0xa0, 0xb0};
            r2[1] = sizeof r2 - 2;
            push(s, r2, sizeof r2, 2);
        }
        break;
    case CK_BT_OP_LE_CREATE_CONN: {
        if (plen != 25) { s->violations++; push_cs(s, op, 0x12); break; }
        if (pkt[3 + 4] != 0) s->violations++; /* filter policy: not the accept list */
        push_cs(s, op, 0);
        if (s->never_complete) break;
        uint8_t cc[21] = {0x3e, 19, 0x01, 0, 0x40, 0x00, 0, 0, 0, 0, 0, 0, 0, 0x18, 0, 0, 0, 0xc8, 0, 0};
        if (s->fail_connect) cc[3] = 0x3e;
        memcpy(cc + 7, pkt + 3 + 6, 6);
        cc[6] = pkt[3 + 5];
        cc[5] = 0x00;
        cc[4] = 0x40; cc[5] = 0xf0; /* handle 0x0040 with junk in the flag bits: upper 4 bits must be masked */
        cc[6] = 0; /* role central */
        cc[7] = pkt[3 + 5];
        memcpy(cc + 8, pkt + 3 + 6, 6);
        if (!s->fail_connect) s->connected = 1;
        push(s, cc, 21, 2);
        break; }
    case CK_BT_OP_DISCONNECT: {
        if (plen != 3 || !s->connected) { s->violations++; push_cs(s, op, 0x02); break; }
        push_cs(s, op, 0);
        uint8_t d[6] = {0x05, 4, 0, pkt[3], pkt[4] & 0x0f, pkt[5]};
        s->connected = 0;
        push(s, d, 6, 2);
        break; }
    default: {
        uint8_t bad[1] = {0x01}; /* unknown HCI command */
        push_cc(s, op, bad, 1, 1);
        break; }
    }
    return 0;
}

static int sim_poll(void *ctx, uint8_t *buf, uint32_t cap, uint32_t *len) {
    sim_t *s = ctx;
    s->polls++;
    if (s->fail_poll) return -1;
    if (s->qh == s->qt || s->ready_at[s->qh] > s->polls) return 1;
    uint32_t n = s->ql[s->qh];
    if (n > cap) return -1;
    memcpy(buf, s->q[s->qh], n);
    *len = n;
    if (buf[0] == 0x0e && buf[1] >= 3) s->host_credit = buf[2];
    else if (buf[0] == 0x0f && buf[1] == 4) s->host_credit = buf[3];
    s->qh = (s->qh + 1) % QN;
    return 0;
}

static void setup(sim_t *s, ck_bt_host *h) {
    memset(s, 0, sizeof *s);
    s->host_credit = 1;
    ck_bt_transport tr = {sim_send, sim_poll, s};
    ck_bt_host_init(h, &tr);
    h->poll_budget = 400;
}

static void test_codec(void) {
    uint8_t out[8];
    CHECK(ck_bt_cmd_encode(CK_BT_OP_RESET, 0, 0, out, sizeof out) == 3);
    CHECK(out[0] == 0x03 && out[1] == 0x0c && out[2] == 0); /* 0x0c03: OGF 3, OCF 3 */
    CHECK(CK_BT_OP_READ_LOCAL_VERSION == 0x1001 && CK_BT_OP_READ_BD_ADDR == 0x1009);
    CHECK(CK_BT_OP_SET_EVENT_MASK == 0x0c01 && CK_BT_OP_INQUIRY == 0x0401 && CK_BT_OP_DISCONNECT == 0x0406);
    CHECK(CK_BT_OP_LE_SET_SCAN_PARAMS == 0x200b && CK_BT_OP_LE_SET_SCAN_ENABLE == 0x200c &&
          CK_BT_OP_LE_CREATE_CONN == 0x200d);
    uint8_t p[3] = {1, 2, 3};
    CHECK(ck_bt_cmd_encode(0x0406, p, 3, out, sizeof out) == 6 && out[3] == 1 && out[5] == 3);
    CHECK(ck_bt_cmd_encode(0x0406, p, 3, out, 5) == CK_BT_E_ARG); /* too small */
    CHECK(ck_bt_cmd_encode(0x0406, 0, 3, out, sizeof out) == CK_BT_E_ARG);
    ck_bt_evt e;
    uint8_t ev[] = {0x0e, 4, 1, 0x03, 0x0c, 0};
    CHECK(ck_bt_evt_parse(ev, sizeof ev, &e) == 0 && e.code == 0x0e && e.plen == 4 && e.p == ev + 2);
    CHECK(ck_bt_evt_parse(ev, sizeof ev - 1, &e) == CK_BT_E_PROTO); /* truncated */
    CHECK(ck_bt_evt_parse(ev, 1, &e) == CK_BT_E_PROTO);
    uint8_t longer[] = {0x0e, 1, 0, 0};
    CHECK(ck_bt_evt_parse(longer, sizeof longer, &e) == CK_BT_E_PROTO); /* trailing byte */
}

static void test_bringup(void) {
    sim_t s; ck_bt_host h;
    setup(&s, &h);
    CHECK(ck_bt_bringup(&h) == 0);
    CHECK(s.nops == 4 && s.opcodes[0] == CK_BT_OP_RESET && s.opcodes[1] == CK_BT_OP_READ_LOCAL_VERSION &&
          s.opcodes[2] == CK_BT_OP_READ_BD_ADDR && s.opcodes[3] == CK_BT_OP_SET_EVENT_MASK);
    CHECK(h.have_version && h.hci_version == 0x0d && h.hci_subversion == 0x1234 && h.lmp_version == 0x0d &&
          h.company == 0x0046 && h.lmp_subversion == 0x5678);
    CHECK(h.have_addr && h.bd_addr[0] == 0x66 && h.bd_addr[5] == 0x11);
    CHECK((s.mask >> 61 & 1) == 1);  /* LE Meta enabled */
    CHECK((s.mask >> 0 & 1) == 1 && (s.mask >> 4 & 1) == 1);
    CHECK(s.violations == 0);
    CHECK(h.busy == 0 && h.credits == 1);

    /* the controller refuses the first command with a status: reported, not swallowed */
    setup(&s, &h);
    s.status_error_opcode = CK_BT_OP_READ_LOCAL_VERSION;
    CHECK(ck_bt_reset(&h) == 0);
    CHECK(ck_bt_read_version(&h) == CK_BT_E_STATUS);
    CHECK(!h.have_version && h.busy == 0);

    /* Num_HCI_Command_Packets = 0 after Reset: the host waits for the NOP refill */
    setup(&s, &h);
    s.zero_credit_after_reset = 1;
    CHECK(ck_bt_reset(&h) == 0);
    CHECK(h.credits == 0);
    CHECK(ck_bt_read_version(&h) == 0);
    CHECK(s.violations == 0 && h.have_version);

    /* ... and gives up with CK_BT_E_CREDIT if the refill never comes */
    setup(&s, &h);
    s.zero_credit_after_reset = 1;
    h.poll_budget = 3;
    CHECK(ck_bt_reset(&h) == 0);
    CHECK(ck_bt_read_version(&h) == CK_BT_E_CREDIT);
    CHECK(s.nops == 1);

    /* controller silent: timeout, not a hang; the host is usable again afterwards */
    setup(&s, &h);
    s.delay = 100000;
    h.poll_budget = 50;
    CHECK(ck_bt_reset(&h) == CK_BT_E_TIMEOUT);
    CHECK(h.busy == 0);

    /* transport failure on receive */
    setup(&s, &h);
    s.fail_poll = 1;
    CHECK(ck_bt_reset(&h) == CK_BT_E_TRANSPORT);

    /* a command whose reply is for another opcode is not mistaken for ours */
    setup(&s, &h);
    uint8_t stray[] = {0x0e, 4, 1, 0x09, 0x10, 0};
    push(&s, stray, sizeof stray, 0);
    CHECK(ck_bt_reset(&h) == 0);
    CHECK(h.have_addr == 0);
}

static void test_inquiry(void) {
    sim_t s; ck_bt_host h;
    setup(&s, &h);
    CHECK(ck_bt_bringup(&h) == 0);
    CHECK(ck_bt_inquiry(&h, 2, 10) == 0);
    CHECK(h.inquiry_done);
    CHECK(h.n_bredr == 3); /* 0xbb repeated: de-duplicated */
    CHECK(h.bredr[0].addr[5] == 0xaa && h.bredr[1].addr[5] == 0xbb && h.bredr[2].addr[5] == 0xcc);
    CHECK(h.bredr[0].cod[0] == 0x04 && h.bredr[0].cod[1] == 0x05 && h.bredr[0].cod[2] == 0x24);
    CHECK(h.bredr[1].cod[0] == 0x0c && h.bredr[1].cod[1] == 0x02 && h.bredr[1].cod[2] == 0x5a); /* repeat keeps its class */
    CHECK(h.bredr[2].cod[0] == 0x00 && h.bredr[2].cod[1] == 0x01 && h.bredr[2].cod[2] == 0x1f);
    CHECK(s.violations == 0 && h.dropped == 0);
    CHECK(ck_bt_inquiry(&h, 0, 10) == CK_BT_E_ARG && ck_bt_inquiry(&h, 0x31, 10) == CK_BT_E_ARG);

    /* Command Status with an error stops the wait */
    setup(&s, &h);
    CHECK(ck_bt_reset(&h) == 0);
    s.status_error_opcode = CK_BT_OP_INQUIRY;
    CHECK(ck_bt_inquiry(&h, 2, 10) == CK_BT_E_STATUS);

    /* inquiry that never completes: timeout */
    setup(&s, &h);
    h.poll_budget = 2;
    CHECK(ck_bt_inquiry(&h, 2, 10) == CK_BT_E_TIMEOUT || !h.inquiry_done);
}

static void test_le(void) {
    sim_t s; ck_bt_host h;
    setup(&s, &h);
    CHECK(ck_bt_bringup(&h) == 0);
    CHECK(ck_bt_le_scan(&h, 30) == 0);
    CHECK(s.scan_enable_cmds == 2 && s.scan_on == 0); /* enabled, then disabled */
    CHECK(h.n_le == 2);
    CHECK(h.le[0].addr[0] == 0x11 && h.le[0].addr_type == 0 && h.le[0].data_len == 2 && h.le[0].data[0] == 0xee &&
          h.le[0].rssi == 0xb0); /* second report for 0x11 replaced the first */
    CHECK(h.le[1].addr[0] == 0x22 && h.le[1].addr_type == 0 && h.le[1].data_len == 1 && h.le[1].data[0] == 0xff && h.le[1].rssi == 0xa0);
    CHECK(h.le[1].addr[5] == 0x22);
    CHECK(s.violations == 0);
    CHECK(h.dropped == 0);
}

static void test_connect(void) {
    sim_t s; ck_bt_host h;
    uint8_t peer[6] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
    setup(&s, &h);
    CHECK(ck_bt_bringup(&h) == 0);
    CHECK(ck_bt_le_connect(&h, 0, peer) == 0);
    CHECK(h.connected && h.handle == 0x0040 && h.conn_role == 0);
    CHECK(memcmp(h.conn_peer, peer, 6) == 0);
    CHECK(ck_bt_le_connect(&h, 0, peer) == CK_BT_E_STATE); /* already connected */
    CHECK(ck_bt_disconnect(&h) == 0);
    CHECK(!h.connected && h.disc_status == 0 && h.disc_reason == 0x13);
    CHECK(ck_bt_disconnect(&h) == CK_BT_E_STATE);
    CHECK(s.violations == 0);

    /* connection fails at the link layer: reported with the status, no handle */
    setup(&s, &h);
    s.fail_connect = 1;
    CHECK(ck_bt_bringup(&h) == 0);
    CHECK(ck_bt_le_connect(&h, 1, peer) == CK_BT_E_STATUS);
    CHECK(!h.connected && h.conn_status == 0x3e);

    /* peer never answers */
    setup(&s, &h);
    s.never_complete = 1;
    CHECK(ck_bt_bringup(&h) == 0);
    CHECK(ck_bt_le_connect(&h, 0, peer) == CK_BT_E_TIMEOUT);
    CHECK(!h.connected);

    CHECK(ck_bt_le_connect(&h, 2, peer) == CK_BT_E_ARG);
    CHECK(ck_bt_le_connect(&h, 0, 0) == CK_BT_E_ARG);
}

static void test_malformed(void) {
    sim_t s; ck_bt_host h;
    setup(&s, &h);
    CHECK(ck_bt_bringup(&h) == 0);
    ck_bt_evt e;
    /* truncated LE advertising report: dropped, never read past its end */
    uint8_t bad1[] = {0x3e, 4, 0x02, 1, 0, 0};
    CHECK(ck_bt_evt_parse(bad1, sizeof bad1, &e) == 0);
    uint32_t d0 = h.dropped;
    ck_bt_dispatch(&h, &e);
    CHECK(h.dropped == d0 + 1 && h.n_le == 0);
    /* declared data length longer than the packet */
    uint8_t bad2[] = {0x3e, 12, 0x02, 1, 0, 0, 1, 2, 3, 4, 5, 6, 31, 9};
    CHECK(ck_bt_evt_parse(bad2, sizeof bad2, &e) == 0);
    ck_bt_dispatch(&h, &e);
    CHECK(h.dropped == d0 + 2 && h.n_le == 0);
    /* inquiry result whose count does not match its length */
    uint8_t bad3[] = {0x02, 3, 5, 0, 0};
    CHECK(ck_bt_evt_parse(bad3, sizeof bad3, &e) == 0);
    ck_bt_dispatch(&h, &e);
    CHECK(h.dropped == d0 + 3 && h.n_bredr == 0);
    /* more than the table holds: the extras are counted, not written */
    for (int i = 0; i < 12; i++) {
        uint8_t adv[] = {0x3e, 12 + 1 + 1, 0x02, 1, 0, 0, (uint8_t)i, 0, 0, 0, 0, 1, 0, 0x80};
        adv[1] = sizeof adv - 2;
        CHECK(ck_bt_evt_parse(adv, sizeof adv, &e) == 0);
        ck_bt_dispatch(&h, &e);
    }
    CHECK(h.n_le == CK_BT_MAX_RESULTS && h.dropped >= d0 + 3 + 4);
    /* a disconnect for another handle leaves our link alone */
    h.connected = 1; h.handle = 0x40;
    uint8_t d[] = {0x05, 4, 0, 0x41, 0, 0x13};
    CHECK(ck_bt_evt_parse(d, sizeof d, &e) == 0);
    ck_bt_dispatch(&h, &e);
    CHECK(h.connected == 1);
    /* an unknown event is counted */
    uint8_t unk[] = {0x77, 0};
    CHECK(ck_bt_evt_parse(unk, sizeof unk, &e) == 0);
    uint32_t d1 = h.dropped;
    ck_bt_dispatch(&h, &e);
    CHECK(h.dropped == d1 + 1);
}

int main(void) {
    test_codec();
    test_bringup();
    test_inquiry();
    test_le();
    test_connect();
    test_malformed();
    printf("CK_BT_HOST: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
