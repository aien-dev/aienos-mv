/* bt_hci.c -- Bluetooth HCI codec and polled host state machine (see bt_hci.h).
 * Pure C with no kernel calls, so the same file runs in the kernel and in the
 * host test against a controller simulator. */
#include "bt_hci.h"

static void copy(uint8_t *d, const uint8_t *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

int ck_bt_cmd_encode(uint16_t opcode, const uint8_t *params, uint8_t plen, uint8_t *out, uint32_t cap) {
    if (!out || (plen && !params) || cap < 3u + plen) return CK_BT_E_ARG;
#if defined(CK_TEST_BT_MUT) && CK_TEST_BT_MUT == 2
    opcode = (uint16_t)((opcode >> 8) | (opcode << 8)); /* TEST-ONLY mutant: opcode bytes swapped */
#endif
    put16(out, opcode);
    out[2] = plen;
    copy(out + 3, params, plen);
    return (int)(3u + plen);
}

int ck_bt_evt_parse(const uint8_t *buf, uint32_t len, ck_bt_evt *e) {
    if (!buf || !e || len < 2) return CK_BT_E_PROTO;
    if (len != 2u + buf[1]) return CK_BT_E_PROTO; /* a USB event carries exactly one packet */
    e->code = buf[0];
    e->plen = buf[1];
    e->p = buf + 2;
    return CK_BT_OK;
}

void ck_bt_host_init(ck_bt_host *h, const ck_bt_transport *tr) {
    uint8_t *b = (uint8_t *)h;
    for (size_t i = 0; i < sizeof *h; i++) b[i] = 0;
    h->tr = *tr;
    h->credits = 1; /* the host may send one command before the controller has said anything */
    h->poll_budget = CK_BT_DEFAULT_POLLS;
}

static void add_le_report(ck_bt_host *h, const uint8_t *p, uint32_t n) {
    /* p: Num_Reports, then arrays Event_Type[n], Address_Type[n], Address[n*6],
     * and per report Data_Length, Data, RSSI. Section 7.7.65.2 lays the
     * parameters out as arrays of each field, but controllers emit one report
     * per event in practice; the arrays form is what the spec text says, so
     * that is what is parsed. Anything inconsistent is dropped, not guessed. */
    if (n < 1) { h->dropped++; return; }
    uint32_t num = p[0];
    if (num < 1 || num > 0x19u) { h->dropped++; return; }
    uint32_t fixed = num * (1u + 1u + 6u + 1u); /* type, atype, addr, data_len */
    if (n < 1u + fixed) { h->dropped++; return; }
    const uint8_t *type = p + 1;
    const uint8_t *atype = type + num;
    const uint8_t *addr = atype + num;
    const uint8_t *dlen = addr + 6u * num;
    uint32_t total = 0;
    for (uint32_t i = 0; i < num; i++) {
        if (dlen[i] > CK_BT_ADV_DATA_MAX) { h->dropped++; return; }
        total += dlen[i];
    }
    if (n != 1u + fixed + total + num) { h->dropped++; return; } /* data bytes then one RSSI each */
    const uint8_t *data = dlen + num;
    const uint8_t *rssi = data + total;
    uint32_t off = 0;
    for (uint32_t i = 0; i < num; i++) {
        uint32_t k;
        for (k = 0; k < h->n_le; k++) {
            int same = h->le[k].addr_type == atype[i];
            for (int j = 0; j < 6 && same; j++) same = h->le[k].addr[j] == addr[6u * i + (uint32_t)j];
            if (same) break;
        }
        if (k == h->n_le) {
            if (h->n_le == CK_BT_MAX_RESULTS) { h->dropped++; off += dlen[i]; continue; }
            h->n_le++;
        }
        ck_bt_peer *r = &h->le[k];
        copy(r->addr, addr + 6u * i, 6);
        r->addr_type = atype[i];
        r->data_len = dlen[i];
        copy(r->data, data + off, dlen[i]);
        r->rssi = rssi[i];
        off += dlen[i];
    }
    (void)type;
}

static void add_inquiry_result(ck_bt_host *h, const uint8_t *p, uint32_t n) {
    if (n < 1) { h->dropped++; return; }
    uint32_t num = p[0];
    if (num < 1 || n != 1u + num * 14u) { h->dropped++; return; }
    const uint8_t *addr = p + 1;
#if defined(CK_TEST_BT_MUT) && CK_TEST_BT_MUT == 3
    const uint8_t *cod = addr + 6u * num + num + 3u * num; /* TEST-ONLY mutant: class of device read at the wrong offset */
#else
    const uint8_t *cod = addr + 6u * num + num + 2u * num; /* after PSRM and Reserved */
#endif
    for (uint32_t i = 0; i < num; i++) {
        uint32_t k;
        for (k = 0; k < h->n_bredr; k++) {
            int same = 1;
            for (int j = 0; j < 6 && same; j++) same = h->bredr[k].addr[j] == addr[6u * i + (uint32_t)j];
            if (same) break;
        }
        if (k == h->n_bredr) {
            if (h->n_bredr == CK_BT_MAX_RESULTS) { h->dropped++; continue; }
            h->n_bredr++;
        }
        copy(h->bredr[k].addr, addr + 6u * i, 6);
        copy(h->bredr[k].cod, cod + 3u * i, 3);
    }
}

void ck_bt_dispatch(ck_bt_host *h, const ck_bt_evt *e) {
    h->events++;
    switch (e->code) {
    case CK_BT_EVT_INQUIRY_COMPLETE:
        if (e->plen == 1) h->inquiry_done = 1; else h->dropped++;
        break;
    case CK_BT_EVT_INQUIRY_RESULT:
        add_inquiry_result(h, e->p, e->plen);
        break;
    case CK_BT_EVT_DISCONN_COMPLETE:
        if (e->plen != 4) { h->dropped++; break; }
        h->disc_status = e->p[0];
        h->disconnected = 1;
        if (e->p[0] == 0 && h->connected && le16(e->p + 1) == h->handle) {
            h->connected = 0;
            h->disc_reason = e->p[3];
        }
        break;
    case CK_BT_EVT_LE_META:
        if (e->plen < 1) { h->dropped++; break; }
        if (e->p[0] == CK_BT_LE_ADV_REPORT) {
            add_le_report(h, e->p + 1, (uint32_t)e->plen - 1u);
        } else if (e->p[0] == CK_BT_LE_CONN_COMPLETE) {
            /* subevent, status, handle(2), role, peer type, peer(6), interval(2), latency(2), timeout(2), accuracy */
            if (e->plen != 19) { h->dropped++; break; }
            h->conn_status = e->p[1];
            h->handle = (uint16_t)(le16(e->p + 2) & 0x0fffu);
            h->conn_role = e->p[4];
            copy(h->conn_peer, e->p + 6, 6);
            h->connected = e->p[1] == 0;
        } else {
            h->dropped++;
        }
        break;
    default:
        h->dropped++; /* masked or unsupported: ignored, but counted */
        break;
    }
}

/* One poll. Returns 0 with *e parsed, 1 when nothing is ready, <0 on failure. */
static int next_event(ck_bt_host *h, uint8_t *buf, ck_bt_evt *e) {
    uint32_t len = 0;
    int rc = h->tr.poll_event(h->tr.ctx, buf, CK_BT_EVT_MAX, &len);
    if (rc < 0) return CK_BT_E_TRANSPORT;
    if (rc > 0) return 1;
    return ck_bt_evt_parse(buf, len, e);
}

static void take_credits(ck_bt_host *h, uint8_t n) { h->credits = n; }

int ck_bt_command(ck_bt_host *h, uint16_t opcode, const uint8_t *params, uint8_t plen, uint8_t *ret, uint8_t retcap,
                  uint8_t *retlen) {
    uint8_t pkt[CK_BT_CMD_MAX], ev[CK_BT_EVT_MAX];
    ck_bt_evt e;
    if (!h || !h->tr.send_cmd || !h->tr.poll_event) return CK_BT_E_ARG;
    if (h->busy) return CK_BT_E_BUSY;
    uint32_t budget = h->poll_budget ? h->poll_budget : CK_BT_DEFAULT_POLLS;
    /* A controller that announced zero credits must send an update before the next command. */
#if defined(CK_TEST_BT_MUT) && CK_TEST_BT_MUT == 1
    h->credits = 1; /* TEST-ONLY mutant: command credits ignored */
#endif
    for (uint32_t i = 0; h->credits <= 0; i++) {
        if (i >= budget) return CK_BT_E_CREDIT;
        int rc = next_event(h, ev, &e);
        if (rc < 0) return rc;
        if (rc > 0) continue;
        if (e.code == CK_BT_EVT_CMD_COMPLETE && e.plen >= 3) {
            take_credits(h, e.p[0]); /* a NOP completion (opcode 0) is how credits are refilled */
        } else {
            ck_bt_dispatch(h, &e);
        }
    }
    int n = ck_bt_cmd_encode(opcode, params, plen, pkt, sizeof pkt);
    if (n < 0) return n;
    if (h->tr.send_cmd(h->tr.ctx, pkt, (uint32_t)n) != 0) return CK_BT_E_TRANSPORT;
    h->credits--;
    h->busy = 1;
    h->pending = opcode;
    if (retlen) *retlen = 0;
    int result = CK_BT_E_TIMEOUT;
    for (uint32_t i = 0; i < budget; i++) {
        int rc = next_event(h, ev, &e);
        if (rc < 0) { result = rc; break; }
        if (rc > 0) continue;
        if (e.code == CK_BT_EVT_CMD_COMPLETE) {
            if (e.plen < 3) { result = CK_BT_E_PROTO; break; }
            take_credits(h, e.p[0]);
            if (le16(e.p + 1) != opcode) continue; /* credit update or someone else's completion */
            uint8_t rl = (uint8_t)(e.plen - 3);
            if (retlen) *retlen = rl;
            if (ret) copy(ret, e.p + 3, rl < retcap ? rl : retcap);
#if defined(CK_TEST_BT_MUT) && CK_TEST_BT_MUT == 4
            result = CK_BT_OK; /* TEST-ONLY mutant: HCI status ignored */
#else
            result = rl >= 1 && e.p[3] != 0 ? CK_BT_E_STATUS : CK_BT_OK;
#endif
            h->events++;
            break;
        }
        if (e.code == CK_BT_EVT_CMD_STATUS) {
            if (e.plen != 4) { result = CK_BT_E_PROTO; break; }
            take_credits(h, e.p[1]);
            if (le16(e.p + 2) != opcode) continue;
            if (retlen) *retlen = 1;
            if (ret && retcap) ret[0] = e.p[0];
            result = e.p[0] != 0 ? CK_BT_E_STATUS : CK_BT_OK;
            h->events++;
            break;
        }
        ck_bt_dispatch(h, &e);
    }
    h->busy = 0;
    return result;
}

int ck_bt_pump(ck_bt_host *h, uint32_t polls) {
    uint8_t ev[CK_BT_EVT_MAX];
    ck_bt_evt e;
    for (uint32_t i = 0; i < polls; i++) {
        int rc = next_event(h, ev, &e);
        if (rc < 0) return rc;
        if (rc > 0) continue;
        if (e.code == CK_BT_EVT_CMD_COMPLETE && e.plen >= 3) take_credits(h, e.p[0]);
        else if (e.code == CK_BT_EVT_CMD_STATUS && e.plen == 4) take_credits(h, e.p[1]);
        else ck_bt_dispatch(h, &e);
    }
    return CK_BT_OK;
}

/* Wait until *flag is set, in slices so a quiet controller costs polls, not forever. */
static int wait_flag(ck_bt_host *h, const int *flag) {
    uint32_t budget = h->poll_budget ? h->poll_budget : CK_BT_DEFAULT_POLLS;
    uint8_t ev[CK_BT_EVT_MAX];
    ck_bt_evt e;
    for (uint32_t i = 0; i < budget && !*flag; i++) {
        int rc = next_event(h, ev, &e);
        if (rc < 0) return rc;
        if (rc > 0) continue;
        if (e.code == CK_BT_EVT_CMD_COMPLETE && e.plen >= 3) take_credits(h, e.p[0]);
        else if (e.code == CK_BT_EVT_CMD_STATUS && e.plen == 4) take_credits(h, e.p[1]);
        else ck_bt_dispatch(h, &e);
    }
    return *flag ? CK_BT_OK : CK_BT_E_TIMEOUT;
}

int ck_bt_reset(ck_bt_host *h) {
    uint8_t r[1], rl;
    return ck_bt_command(h, CK_BT_OP_RESET, 0, 0, r, sizeof r, &rl);
}

int ck_bt_read_version(ck_bt_host *h) {
    uint8_t r[9], rl;
    int rc = ck_bt_command(h, CK_BT_OP_READ_LOCAL_VERSION, 0, 0, r, sizeof r, &rl);
    if (rc) return rc;
    if (rl != 9) return CK_BT_E_PROTO;
    h->hci_version = r[1];
    h->hci_subversion = le16(r + 2);
    h->lmp_version = r[4];
    h->company = le16(r + 5);
    h->lmp_subversion = le16(r + 7);
    h->have_version = 1;
    return CK_BT_OK;
}

int ck_bt_read_bd_addr(ck_bt_host *h) {
    uint8_t r[7], rl;
    int rc = ck_bt_command(h, CK_BT_OP_READ_BD_ADDR, 0, 0, r, sizeof r, &rl);
    if (rc) return rc;
    if (rl != 7) return CK_BT_E_PROTO;
    copy(h->bd_addr, r + 1, 6);
    h->have_addr = 1;
    return CK_BT_OK;
}

int ck_bt_set_event_mask(ck_bt_host *h, uint64_t mask) {
    uint8_t p[8], r[1], rl;
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(mask >> (8 * i));
    return ck_bt_command(h, CK_BT_OP_SET_EVENT_MASK, p, 8, r, sizeof r, &rl);
}

int ck_bt_bringup(ck_bt_host *h) {
    int rc = ck_bt_reset(h);
    if (rc) return rc;
    rc = ck_bt_read_version(h);
    if (rc) return rc;
    rc = ck_bt_read_bd_addr(h);
    if (rc) return rc;
    /* Inquiry Complete, Inquiry Result, Disconnection Complete, Command Complete/Status,
     * plus the LE Meta bit (bit 61): event code N is mask bit N-1. */
    uint64_t mask = (1ull << 0) | (1ull << 1) | (1ull << 4) | (1ull << 13) | (1ull << 14) | (1ull << 61);
    return ck_bt_set_event_mask(h, mask);
}

int ck_bt_inquiry(ck_bt_host *h, uint8_t length, uint8_t max_responses) {
    uint8_t p[5] = {0x33, 0x8b, 0x9e, length, max_responses}; /* GIAC 0x9E8B33, little endian */
    uint8_t r[1], rl;
    if (length < 1 || length > 0x30) return CK_BT_E_ARG;
    h->inquiry_done = 0;
    h->n_bredr = 0;
    int rc = ck_bt_command(h, CK_BT_OP_INQUIRY, p, sizeof p, r, sizeof r, &rl);
    if (rc) return rc;
    return wait_flag(h, &h->inquiry_done);
}

int ck_bt_le_scan(ck_bt_host *h, uint32_t polls) {
    uint8_t sp[7], en[2], r[1], rl;
    sp[0] = 0x00;           /* passive scanning */
    put16(sp + 1, 0x0010);  /* interval 10 ms */
    put16(sp + 3, 0x0010);  /* window 10 ms */
    sp[5] = 0x00;           /* public own address */
    sp[6] = 0x00;           /* accept all */
    h->n_le = 0;
    int rc = ck_bt_command(h, CK_BT_OP_LE_SET_SCAN_PARAMS, sp, sizeof sp, r, sizeof r, &rl);
    if (rc) return rc;
    en[0] = 1;
    en[1] = 0; /* do not filter duplicates in the controller; the host de-duplicates */
    rc = ck_bt_command(h, CK_BT_OP_LE_SET_SCAN_ENABLE, en, 2, r, sizeof r, &rl);
    if (rc) return rc;
    int prc = ck_bt_pump(h, polls);
    en[0] = 0;
    rc = ck_bt_command(h, CK_BT_OP_LE_SET_SCAN_ENABLE, en, 2, r, sizeof r, &rl);
    return prc ? prc : rc; /* the scan is switched off even if the pump failed */
}

int ck_bt_le_connect(ck_bt_host *h, uint8_t peer_type, const uint8_t peer[6]) {
    uint8_t p[25], r[1], rl;
    if (!peer || peer_type > 1) return CK_BT_E_ARG;
    if (h->connected) return CK_BT_E_STATE;
    put16(p + 0, 0x0060);  /* scan interval 60 ms */
    put16(p + 2, 0x0030);  /* scan window 30 ms */
    p[4] = 0x00;           /* filter policy: use the peer address, not the accept list */
    p[5] = peer_type;
    copy(p + 6, peer, 6);
    p[12] = 0x00;          /* own address: public */
    put16(p + 13, 0x0018); /* connection interval 30 ms .. 30 ms */
    put16(p + 15, 0x0018);
    put16(p + 17, 0x0000); /* peripheral latency */
    put16(p + 19, 0x00c8); /* supervision timeout 2 s */
    put16(p + 21, 0x0000); /* CE length unconstrained */
    put16(p + 23, 0x0000);
    h->conn_status = 0xff;
    int done = 0;
    uint16_t before = (uint16_t)h->events;
    (void)before;
    int rc = ck_bt_command(h, CK_BT_OP_LE_CREATE_CONN, p, sizeof p, r, sizeof r, &rl);
    if (rc) return rc;
    /* LE Connection Complete sets conn_status; 0xff means it has not arrived. */
    uint32_t budget = h->poll_budget ? h->poll_budget : CK_BT_DEFAULT_POLLS;
    uint8_t ev[CK_BT_EVT_MAX];
    ck_bt_evt e;
    for (uint32_t i = 0; i < budget && !done; i++) {
        int pr = next_event(h, ev, &e);
        if (pr < 0) return pr;
        if (pr > 0) continue;
        if (e.code == CK_BT_EVT_CMD_COMPLETE && e.plen >= 3) take_credits(h, e.p[0]);
        else if (e.code == CK_BT_EVT_CMD_STATUS && e.plen == 4) take_credits(h, e.p[1]);
        else ck_bt_dispatch(h, &e);
        done = h->conn_status != 0xff;
    }
    if (!done) return CK_BT_E_TIMEOUT;
    return h->conn_status == 0 ? CK_BT_OK : CK_BT_E_STATUS;
}

int ck_bt_disconnect(ck_bt_host *h) {
    uint8_t p[3], r[1], rl;
    if (!h->connected) return CK_BT_E_STATE;
    put16(p, h->handle);
    p[2] = 0x13; /* remote user terminated connection */
    h->disconnected = 0;
    int rc = ck_bt_command(h, CK_BT_OP_DISCONNECT, p, sizeof p, r, sizeof r, &rl);
    if (rc) return rc;
    rc = wait_flag(h, &h->disconnected);
    if (rc) return rc;
    return h->disc_status == 0 ? CK_BT_OK : CK_BT_E_STATUS;
}
