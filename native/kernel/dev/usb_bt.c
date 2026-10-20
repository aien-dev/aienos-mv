/* usb_bt.c -- pure half of the USB Bluetooth transport (see usb_bt.h): the
 * descriptor walk, HCI event reassembly and the Bulk-Only Transport packets
 * used by the QEMU transfer proof. No kernel calls, host tested. */
#include "usb_bt.h"

static void copy(uint8_t *d, const uint8_t *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

typedef struct {
    int have_if;             /* inside an interface descriptor */
    uint8_t num, alt, cls, sub, proto;
} walk_if;

int ck_usb_bt_parse(const uint8_t *dev, size_t devlen, const uint8_t *cfg, size_t cfglen, ck_usb_bt_desc *out)
{
    if (!dev || !cfg || !out) return CK_UBT_E_ARG;
    if (devlen < 18 || dev[0] != 18 || dev[1] != 1) return CK_UBT_E_MALFORMED;
    if (cfglen < 9 || cfg[0] != 9 || cfg[1] != 2) return CK_UBT_E_MALFORMED;
    size_t total = le16(cfg + 2);
    if (total < 9) return CK_UBT_E_MALFORMED;
    if (total > cfglen) total = cfglen;
    uint8_t *o = (uint8_t *)out;
    for (size_t i = 0; i < sizeof *out; i++) o[i] = 0;
    out->vid = le16(dev + 8);
    out->pid = le16(dev + 10);
    out->dev_class = dev[4];
    out->dev_sub = dev[5];
    out->dev_proto = dev[6];
    out->configuration = cfg[5];
    out->n_interfaces = cfg[4];
    int found = 0, cur_is_hci = 0, have_int_in = 0, have_bulk_in = 0, have_bulk_out = 0;
    uint8_t hci_if = 0;
    walk_if w = {0, 0, 0, 0, 0, 0};
    size_t off = cfg[0];
    while (off < total) {
        if (off + 2 > total) return CK_UBT_E_MALFORMED;
        uint8_t len = cfg[off], type = cfg[off + 1];
        if (len < 2 || off + len > total) return CK_UBT_E_MALFORMED;
        if (type == 4) { /* interface */
            if (len < 9) return CK_UBT_E_MALFORMED;
            w.have_if = 1;
            w.num = cfg[off + 2];
            w.alt = cfg[off + 3];
            w.cls = cfg[off + 5];
            w.sub = cfg[off + 6];
            w.proto = cfg[off + 7];
            int is_hci_class = w.cls == CK_UBT_CLASS && w.sub == CK_UBT_SUBCLASS && w.proto == CK_UBT_PROTOCOL;
            cur_is_hci = 0;
            if (is_hci_class && w.alt == 0 && !found) {
                found = 1;
                cur_is_hci = 1;
                hci_if = w.num;
                out->interface = w.num;
            } else if (is_hci_class && found && w.num != hci_if) {
                out->n_alt_other++; /* other e0/01/01 interfaces: on the Spark interface 1 (7 alternate settings, ISO) and interface 2 (2, interrupt) */
            }
        } else if (type == 5 && cur_is_hci) { /* endpoint of the HCI interface, alternate setting 0 */
            if (len < 7) return CK_UBT_E_MALFORMED;
            uint8_t addr = cfg[off + 2], attr = cfg[off + 3] & 0x03u;
            uint16_t mps = le16(cfg + off + 4) & 0x7ffu;
            if (attr == 3 && (addr & 0x80u) && !have_int_in) {
                have_int_in = 1;
                out->ep_event = addr;
                out->event_mps = mps;
                out->event_interval = cfg[off + 6];
            } else if (attr == 2 && (addr & 0x80u) && !have_bulk_in) {
                have_bulk_in = 1;
                out->ep_acl_in = addr;
                out->acl_in_mps = mps;
            } else if (attr == 2 && !(addr & 0x80u) && !have_bulk_out) {
                have_bulk_out = 1;
                out->ep_acl_out = addr;
                out->acl_out_mps = mps;
            }
        }
        off += len;
    }
    if (!found) return CK_UBT_E_NOTBT;
    if (!have_int_in || !have_bulk_in || !have_bulk_out || out->event_mps == 0 || out->acl_in_mps == 0 ||
        out->acl_out_mps == 0)
        return CK_UBT_E_ENDPOINTS;
    /* Vol 4 Part B 2.2.1/2.2.2: a single-function controller takes commands addressed to the device
     * (0x20); a controller inside a composite device takes them addressed to its interface (0x21,
     * wIndex = interface). The device must also accept the device-addressed form. */
#if defined(CK_TEST_UBT_MUT) && CK_TEST_UBT_MUT == 1
    if (1) { /* TEST-ONLY mutant: composite device addressed as a single function */
#else
    if (out->dev_class == CK_UBT_CLASS) {
#endif
        out->cmd_rtype = 0x20;
        out->cmd_index = 0;
    } else {
        out->cmd_rtype = 0x21;
        out->cmd_index = out->interface;
    }
    return CK_UBT_OK;
}

int ck_usb_find_bot(const uint8_t *cfg, size_t len, ck_usb_bot_desc *out)
{
    if (!cfg || !out || len < 9 || cfg[0] != 9 || cfg[1] != 2) return CK_UBT_E_MALFORMED;
    size_t total = le16(cfg + 2);
    if (total > len) total = len;
    uint8_t *o = (uint8_t *)out;
    for (size_t i = 0; i < sizeof *out; i++) o[i] = 0;
    out->configuration = cfg[5];
    int in_bot = 0, got_in = 0, got_out = 0;
    size_t off = cfg[0];
    while (off < total) {
        if (off + 2 > total) return CK_UBT_E_MALFORMED;
        uint8_t l = cfg[off], t = cfg[off + 1];
        if (l < 2 || off + l > total) return CK_UBT_E_MALFORMED;
        if (t == 4) {
            if (l < 9) return CK_UBT_E_MALFORMED;
            in_bot = !got_in && cfg[off + 3] == 0 && cfg[off + 5] == 8 && cfg[off + 6] == 6 && cfg[off + 7] == 0x50;
            if (in_bot) out->interface = cfg[off + 2];
        } else if (t == 5 && in_bot) {
            if (l < 7) return CK_UBT_E_MALFORMED;
            uint8_t addr = cfg[off + 2];
            if ((cfg[off + 3] & 3u) == 2u) {
                if ((addr & 0x80u) && !got_in) { got_in = 1; out->ep_in = addr; out->mps_in = le16(cfg + off + 4) & 0x7ffu; }
                else if (!(addr & 0x80u) && !got_out) { got_out = 1; out->ep_out = addr; out->mps_out = le16(cfg + off + 4) & 0x7ffu; }
            }
        }
        off += l;
    }
    return got_in && got_out ? CK_UBT_OK : CK_UBT_E_NOTBT;
}

void ck_usb_bt_asm_init(ck_usb_bt_asm *a) { a->have = 0; }

int ck_usb_bt_asm_feed(ck_usb_bt_asm *a, const uint8_t *chunk, uint32_t n)
{
    if (!a || (n && !chunk)) return CK_UBT_E_ARG;
    if (a->have + n > sizeof a->buf) {
        a->have = 0; /* garbage in, drop everything rather than hand out a mis-framed event */
        return CK_UBT_E_OVERFLOW;
    }
    copy(a->buf + a->have, chunk, n);
    a->have += n;
    return CK_UBT_OK;
}

int ck_usb_bt_asm_take(ck_usb_bt_asm *a, uint8_t *out, uint32_t cap, uint32_t *len)
{
    if (a->have < 2) return 0;
    uint32_t want = 2u + a->buf[1];
#if defined(CK_TEST_UBT_MUT) && CK_TEST_UBT_MUT == 2
    want = 2u; /* TEST-ONLY mutant: an event is handed out as soon as its header is there */
#endif
    if (a->have < want) return 0;
    if (want > cap) {
        a->have = 0;
        return CK_UBT_E_OVERFLOW;
    }
    copy(out, a->buf, want);
    if (len) *len = want;
    for (uint32_t i = want; i < a->have; i++) a->buf[i - want] = a->buf[i];
    a->have -= want;
    return 1;
}

int ck_usb_bot_cbw(uint8_t cbw[31], uint32_t tag, uint32_t data_len, int data_in, const uint8_t *cdb, uint8_t cdb_len)
{
    if (!cbw || !cdb || cdb_len == 0 || cdb_len > 16) return CK_UBT_E_ARG;
    for (int i = 0; i < 31; i++) cbw[i] = 0;
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
    for (int i = 0; i < 4; i++) cbw[4 + i] = (uint8_t)(tag >> (8 * i));
    for (int i = 0; i < 4; i++) cbw[8 + i] = (uint8_t)(data_len >> (8 * i));
#if defined(CK_TEST_UBT_MUT) && CK_TEST_UBT_MUT == 3
    cbw[12] = data_in ? 0x00 : 0x80; /* TEST-ONLY mutant: direction flag inverted */
#else
    cbw[12] = data_in ? 0x80 : 0x00;
#endif
    cbw[13] = 0; /* LUN 0 */
    cbw[14] = cdb_len;
    copy(cbw + 15, cdb, cdb_len);
    return CK_UBT_OK;
}

int ck_usb_bot_csw(const uint8_t csw[13], uint32_t tag, uint32_t *residue)
{
    if (!csw) return CK_UBT_E_ARG;
    if (csw[0] != 'U' || csw[1] != 'S' || csw[2] != 'B' || csw[3] != 'S') return CK_UBT_E_MALFORMED;
    uint32_t t = (uint32_t)csw[4] | ((uint32_t)csw[5] << 8) | ((uint32_t)csw[6] << 16) | ((uint32_t)csw[7] << 24);
    if (t != tag) return CK_UBT_E_MALFORMED;
    if (residue) *residue = (uint32_t)csw[8] | ((uint32_t)csw[9] << 8) | ((uint32_t)csw[10] << 16) | ((uint32_t)csw[11] << 24);
    return csw[12] == 0 ? CK_UBT_OK : CK_UBT_E_MALFORMED;
}
