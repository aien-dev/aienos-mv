/* MT7925 station-join MCU commands. See aienos_mt7925_sta.h. */
#include "aienos_mt7925_sta.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void zero(uint8_t *p, uint32_t n) {
    volatile uint8_t *v = p;
    for (uint32_t i = 0; i < n; i++) v[i] = 0;
}
static void cpy(uint8_t *d, const uint8_t *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

/* Append a zeroed TLV {tag, len} and bump the le16 TLV count the BSS_INFO
 * and STA_REC headers carry at bytes 2..3 (reference
 * mt76_connac_mcu_add_nested_tlv). Returns the TLV or NULL if it does not
 * fit. */
static uint8_t *tlv(uint8_t *out, uint32_t cap, uint32_t *len, uint16_t tag, uint16_t tl) {
    if (*len + tl > cap) return 0;
    uint8_t *t = out + *len;
    zero(t, tl);
    put16(t, tag);
    put16(t + 2, tl);
    put16(out + 2, (uint16_t)(get16(out + 2) + 1u));
    *len += tl;
    return t;
}

static int band_ok(const mt7925_join_cfg *c) { return c && c->band >= 1 && c->band <= 3 && c->channel; }

uint32_t mt7925_build_nic_cap_query(uint8_t *out, uint32_t cap) {
    if (!out || cap < 8u) return 0;
    zero(out, 8);
    put16(out + 4, MT7925_NIC_CAP_TAG);
    put16(out + 6, 4);
    return 8;
}

uint32_t mt7925_build_dev_info(uint8_t *out, uint32_t cap, const uint8_t mac[6], int active) {
    if (!out || !mac || cap < 16u) return 0;
    zero(out, 16);
    out[0] = MT7925_OMAC_IDX;
    out[1] = MT7925_BAND_AUTO;
    put16(out + 4, 0); /* DEV_INFO_ACTIVE */
    put16(out + 6, 12);
    out[8] = active ? 1 : 0;
    out[9] = 0; /* link_idx */
    cpy(out + 10, mac, 6);
    return 16;
}

/* BASIC TLV body common to both BSS_INFO forms. */
static void basic_fill(uint8_t *t, uint8_t conn_state, uint16_t bmc_wcid, uint16_t sta_wcid) {
    t[4] = 1; /* active: always true for a station */
    t[5] = MT7925_OMAC_IDX;
    t[6] = MT7925_OMAC_IDX; /* hw_bss_idx: omac_idx below EXT_BSSID_START */
    t[7] = MT7925_BAND_AUTO;
    put32(t + 8, MT7925_CONN_INFRA_STA);
    t[12] = conn_state;
    t[13] = 0; /* wmm_idx */
    put16(t + 20, bmc_wcid);
    put16(t + 26, sta_wcid);
    t[31] = MT7925_BSS_IDX; /* link_idx */
}

uint32_t mt7925_build_bss_own(uint8_t *out, uint32_t cap, int active) {
    if (!out || cap < 36u) return 0;
    zero(out, 36);
    out[0] = MT7925_BSS_IDX;
    /* The reference sends this one as a fixed struct: TLV count stays 0. */
    put16(out + 4, 0);
    put16(out + 6, 32);
    basic_fill(out + 4, 1, MT7925_WCID_OWN, MT7925_WCID_OWN);
    out[8] = active ? 1 : 0;
    return 36;
}

uint32_t mt7925_build_bss_info(uint8_t *out, uint32_t cap, const mt7925_join_cfg *c, int enable) {
    if (!out || !band_ok(c) || cap < 4u) return 0;
    uint32_t n = 4;
    zero(out, 4);
    out[0] = MT7925_BSS_IDX;
    int g24 = c->band == 1;
    uint8_t *t = tlv(out, cap, &n, 0, 32); /* BASIC must be first */
    if (!t) return 0;
    basic_fill(t, enable ? 0 : 1, MT7925_WCID_OWN, MT7925_WCID_AP);
    cpy(t + 14, c->bssid, 6);
    put16(t + 22, c->beacon_int);
    t[24] = c->dtim;
    t[25] = g24 ? 0x06u : 0x01u; /* PHY_MODE_B|G or PHY_MODE_A: no HT offered */
    put16(t + 28, g24 ? 1u : 3u); /* PHY_TYPE_ERP_INDEX / PHY_TYPE_OFDM_INDEX */
    if (!(t = tlv(out, cap, &n, 16, 8))) return 0; /* SEC */
    t[4] = c->cipher == MT7925_CIPHER_CCMP ? 7u : 0u; /* MODE_WPA2_PSK / MODE_OPEN */
    t[5] = c->cipher == MT7925_CIPHER_CCMP ? 6u : 1u;
    t[6] = c->cipher;
    if (!(t = tlv(out, cap, &n, 11, 16))) return 0; /* RATE */
    put16(t + 6, g24 ? 0x000fu : 0x0540u); /* HR_DSSS_ERP_BASIC_RATE / OFDM_BASIC_RATE */
    t[12] = g24 ? 1u : 0u;
    t[13] = t[14] = (uint8_t)(g24 ? MT7925_BASIC_RATES_TBL : MT7925_BASIC_RATES_TBL + 4u);
    if (!(t = tlv(out, cap, &n, 15, 8))) return 0; /* QBSS */
    t[4] = c->qos ? 1u : 0u;
    if (!(t = tlv(out, cap, &n, 26, 20))) return 0; /* MLD: not an MLD */
    t[4] = 0xff;                   /* group_mld_id */
    t[5] = MT7925_BSS_IDX + 32u;   /* own_mld_id */
    cpy(t + 6, c->own_mac, 6);
    t[12] = 0xff;                  /* remap_idx */
    t[13] = 0xff;                  /* link_id */
    if (!(t = tlv(out, cap, &n, 23, 20))) return 0; /* IFS_TIME */
    t[4] = 1;
    put16(t + 8, c->short_slot || !g24 ? 9u : 20u);
    if (enable) {
        if (!(t = tlv(out, cap, &n, 2, 16))) return 0; /* RLM: 20 MHz */
        t[4] = c->channel;
        t[5] = c->channel;
        t[7] = 0;   /* CMD_CBW_20MHZ */
        t[8] = 2;   /* tx streams: 2x2 chip (UNKNOWN J6) */
        t[9] = 2;
        t[10] = 4;  /* ht_op_info: 40 MHz allowed, as the reference sets */
        t[12] = c->band;
        if (!(t = tlv(out, cap, &n, 6, 8))) return 0; /* 11V_MBSSID: not a multi-BSSID set */
    }
    return n;
}

uint32_t mt7925_build_roc(uint8_t *out, uint32_t cap, uint8_t token, uint8_t channel, uint8_t band,
                          uint32_t duration_ms, int abort) {
    if (!out) return 0;
    if (abort) {
        if (cap < 16u) return 0;
        zero(out, 16);
        put16(out + 4, 1); /* UNI_ROC_ABORT */
        put16(out + 6, 12);
        out[8] = MT7925_BSS_IDX;
        out[9] = token;
        out[10] = MT7925_BAND_AUTO;
        return 16;
    }
    if (cap < 28u || !channel || band < 1 || band > 3) return 0;
    zero(out, 28);
    put16(out + 4, 0); /* UNI_ROC_ACQUIRE */
    put16(out + 6, 24);
    out[8] = MT7925_BSS_IDX;
    out[9] = token;
    out[10] = channel;   /* control channel */
    out[11] = 0;         /* sco: 20 MHz, centre == control */
    out[12] = band;
    out[13] = 0;         /* bw CMD_CBW_20MHZ */
    out[14] = channel;   /* centre */
    out[16] = 0;         /* bw from AP */
    out[17] = channel;
    out[19] = MT7925_ROC_REQ_JOIN;
    put32(out + 20, duration_ms);
    out[24] = MT7925_BAND_AUTO;
    return 28;
}

static uint32_t sta_hdr(uint8_t *out, uint32_t cap, uint16_t wcid, uint8_t muar) {
    if (cap < 8u) return 0;
    zero(out, 8);
    out[0] = MT7925_BSS_IDX;
    out[1] = (uint8_t)wcid;          /* wlan_idx_lo: bits 7:0 */
    out[4] = 1;                      /* is_tlv_append */
    out[5] = muar;
    out[6] = (uint8_t)((wcid >> 8) & 7u); /* wlan_idx_hi: bits 10:8 */
    return 8;
}

/* Bitmap of `rates` (raw Supported Rates bytes) over the reference's band
 * rate table: 2.4 GHz 1, 2, 5.5, 11, 6, 9, 12, 18, 24, 36, 48, 54 Mb/s;
 * 5/6 GHz the eight OFDM rates. `basic_only` keeps rates with bit 7 set. */
static uint16_t rate_bits(const uint8_t *rates, uint8_t n, int g24, int basic_only) {
    static const uint8_t t24[12] = {2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108};
    const uint8_t *t = g24 ? t24 : t24 + 4;
    uint32_t tn = g24 ? 12u : 8u;
    uint16_t m = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (basic_only && !(rates[i] & 0x80u)) continue;
        for (uint32_t k = 0; k < tn; k++)
            if ((rates[i] & 0x7fu) == t[k]) m |= (uint16_t)(1u << k);
    }
    return m;
}

uint32_t mt7925_build_sta_rec(uint8_t *out, uint32_t cap, const mt7925_join_cfg *c, uint16_t wcid, uint8_t state) {
    static const uint8_t g24_rates[12] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c};
    static const uint8_t ofdm_rates[8] = {0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
    if (!out || !band_ok(c) || wcid > 0x7ffu || state > MT7925_STA_STATE_ASSOC) return 0;
    int own = wcid == MT7925_WCID_OWN, g24 = c->band == 1;
    uint32_t n = sta_hdr(out, cap, wcid, own ? 0x0eu : MT7925_OMAC_IDX);
    uint8_t *t;
    if (!n) return 0;
    if (!own) {
        if (!(t = tlv(out, cap, &n, 0x00, 20))) return 0; /* BASIC */
        put32(t + 4, MT7925_CONN_INFRA_AP);
        t[8] = 2; /* CONN_STATE_PORT_SECURE: the reference's value for every enable */
        t[9] = c->qos ? 1u : 0u;
        put16(t + 10, c->aid);
        cpy(t + 12, c->bssid, 6);
        put16(t + 18, (uint16_t)(1u | (state != MT7925_STA_STATE_ASSOC ? 2u : 0u))); /* VER | NEW */
        /* The AP's rates: our builder offers every rate of the band, so the
         * intersection is the AP's set; the model AP and the doc use that. */
        const uint8_t *r = g24 ? g24_rates : ofdm_rates;
        uint8_t rn = g24 ? 12u : 8u;
        if (!(t = tlv(out, cap, &n, 0x15, 12))) return 0; /* PHY */
        put16(t + 4, rate_bits(r, rn, g24, 1));
        t[6] = g24 ? 0x03u : 0x08u; /* PHY_TYPE_BIT_HR_DSSS|ERP / PHY_TYPE_BIT_OFDM */
        int rcpi = 2 * (int)c->rssi + 220;
        t[9] = (uint8_t)(rcpi < 0 ? 0 : rcpi > 255 ? 255 : rcpi);
        if (!(t = tlv(out, cap, &n, 0x01, 16))) return 0; /* RA */
        uint16_t s = rate_bits(r, rn, g24, 0);
        put16(t + 4, g24 ? (uint16_t)(((s >> 4) << 6) | (s & 0xfu)) : (uint16_t)(s << 6));
        if (!(t = tlv(out, cap, &n, 0x07, 16))) return 0; /* STATE */
        t[4] = state;
        if (state != MT7925_STA_STATE_NONE) {
            if (!(t = tlv(out, cap, &n, 0x20, 28))) return 0; /* MLD, one link */
            cpy(t + 4, c->bssid, 6);
            put16(t + 10, wcid); /* primary_id */
            put16(t + 14, wcid); /* wlan_id */
            t[16] = 1;           /* link_num */
            put16(t + 20, wcid);
            t[22] = MT7925_BSS_IDX;
        }
    }
    if (!(t = tlv(out, cap, &n, 0x2b, 8))) return 0; /* HDR_TRANS: off */
    t[4] = 0;  /* from_ds */
    t[5] = 1;  /* to_ds: station */
    t[6] = 1;  /* dis_rx_hdr_tran */
    return n;
}

uint32_t mt7925_build_key(uint8_t *out, uint32_t cap, const mt7925_join_cfg *c, int pairwise, uint8_t key_id,
                          const uint8_t key[16]) {
    if (!out || !c || !key || key_id > 3u) return 0;
    uint16_t wcid = pairwise ? MT7925_WCID_AP : MT7925_WCID_OWN;
    uint32_t n = sta_hdr(out, cap, wcid, pairwise ? MT7925_OMAC_IDX : 0x0eu);
    uint8_t *t;
    if (!n || !(t = tlv(out, cap, &n, 0x27, 68))) return 0; /* KEY_V3 */
    t[4] = 1;                       /* add */
    t[5] = pairwise ? 1u : 0u;      /* tx_key */
    t[6] = pairwise ? 1u : 0u;      /* key_type */
    t[7] = 0;                       /* not the authenticator */
    cpy(t + 8, c->bssid, 6);
    t[14] = MT7925_BSS_IDX;
    t[15] = MT7925_CIPHER_CCMP;
    t[16] = key_id;
    t[17] = 16;
    t[18] = (uint8_t)wcid;
    t[19] = 1;                      /* mgmt_prot */
    cpy(t + 20, key, 16);
    return n;
}

mt7925_err mt7925_parse_nic_cap(const uint8_t *ev, uint32_t len, uint8_t mac[6]) {
    if (!ev || !mac) return MT7925_E_ARG;
    if (len < MT7925_EVT_HDR_BYTES + 4u) return MT7925_E_DEVICE;
    uint32_t off = MT7925_EVT_HDR_BYTES, n = get16(ev + off);
    off += 4;
    for (uint32_t i = 0; i < n; i++) {
        if (len - off < 4u) return MT7925_E_DEVICE;
        uint32_t tag = get16(ev + off), tl = get16(ev + off + 2);
        if (tl < 4u || tl > len - off) return MT7925_E_DEVICE; /* would stall or overrun */
        if (tag == MT7925_NIC_CAP_MAC_ADDR) {
            if (tl < 10u) return MT7925_E_DEVICE;
            cpy(mac, ev + off + 4, 6);
            return MT7925_OK;
        }
        off += tl;
    }
    return MT7925_E_STATE;
}

int mt7925_parse_roc_grant(const uint8_t *ev, uint32_t len, uint8_t *token, uint8_t *status) {
    if (!ev || len < MT7925_EVT_HDR_BYTES || ev[MT7925_EVT_EID_OFF] != MT7925_UNI_EVENT_ROC) return 0;
    for (uint32_t off = MT7925_EVT_HDR_BYTES + 4u; len - off >= 4u && off < len;) {
        uint32_t tag = get16(ev + off), tl = get16(ev + off + 2);
        if (tl < 4u || tl > len - off) return 0;
        if (tag == 0u && tl >= 20u) { /* UNI_EVENT_ROC_GRANT */
            if (token) *token = ev[off + 5];
            if (status) *status = ev[off + 6];
            return 1;
        }
        off += tl;
    }
    return 0;
}

/* Send a UNI set command and, for DEV/BSS/STA records, check the result
 * event's cid and status (reference mt7925_mcu_parse_response). */
static mt7925_err uni(mt7925_dev *d, uint16_t cmd, const uint8_t *p, uint32_t len, int secret) {
    uint8_t ev[MT7925_EVT_HDR_BYTES + 16];
    uint32_t got = 0;
    mt7925_err e = secret ? mt7925_mcu_send_secret(d, cmd, p, len, ev, sizeof ev, &got)
                          : mt7925_mcu_send(d, cmd, 1, 0, p, len, 1, ev, sizeof ev, &got);
    if (e != MT7925_OK) return e;
    if (cmd == MT7925_UNI_CMD_DEV_INFO || cmd == MT7925_UNI_CMD_BSS_INFO || cmd == MT7925_UNI_CMD_STA_REC) {
        if (got < MT7925_EVT_UNI_STATUS_OFF + 4u || ev[MT7925_EVT_UNI_CID_OFF] != cmd) return MT7925_E_DEVICE;
        if (get32(ev + MT7925_EVT_UNI_STATUS_OFF) != 0) return MT7925_E_MCU_STATUS;
    }
    return MT7925_OK;
}

mt7925_err mt7925_read_mac(mt7925_dev *d, uint8_t mac[6]) {
    uint8_t q[8], ev[MT7925_EVT_HDR_BYTES + 512];
    uint32_t got = 0;
    if (!d || !mac) return MT7925_E_ARG;
    if (!d->fw_running) return MT7925_E_STATE;
    uint32_t n = mt7925_build_nic_cap_query(q, sizeof q);
    mt7925_err e = mt7925_mcu_send(d, MT7925_UNI_CMD_CHIP_CONFIG, 1, 1, q, n, 1, ev, sizeof ev, &got);
    if (e != MT7925_OK) return e;
    return mt7925_parse_nic_cap(ev, got < sizeof ev ? got : sizeof ev, mac);
}

mt7925_err mt7925_join_iface_up(mt7925_dev *d, const uint8_t mac[6]) {
    uint8_t b[MT7925_JOIN_CMD_MAX];
    if (!d || !mac) return MT7925_E_ARG;
    if (!d->fw_running) return MT7925_E_STATE;
    mt7925_err e = uni(d, MT7925_UNI_CMD_DEV_INFO, b, mt7925_build_dev_info(b, sizeof b, mac, 1), 0);
    if (e != MT7925_OK) return e;
    return uni(d, MT7925_UNI_CMD_BSS_INFO, b, mt7925_build_bss_own(b, sizeof b, 1), 0);
}

typedef struct {
    mt7925_event_fn prev;
    void *prev_ctx;
    uint8_t want, granted, status;
} roc_wait;

static void roc_hook(void *ctx, uint8_t eid, const uint8_t *ev, uint32_t len) {
    roc_wait *w = ctx;
    uint8_t tok = 0, st = 0;
    if (mt7925_parse_roc_grant(ev, len, &tok, &st) && tok == w->want) {
        w->granted = 1;
        w->status = st;
        return;
    }
    if (w->prev) w->prev(w->prev_ctx, eid, ev, len);
}


mt7925_err mt7925_join_prepare(mt7925_dev *d, const mt7925_join_cfg *c) {
    uint8_t b[MT7925_JOIN_CMD_MAX];
    if (!d || !band_ok(c)) return MT7925_E_ARG;
    if (!d->fw_running) return MT7925_E_STATE;
    uint32_t n = mt7925_build_bss_info(b, sizeof b, c, 0);
    mt7925_err e = uni(d, MT7925_UNI_CMD_BSS_INFO, b, n, 0);
    if (e != MT7925_OK) return e;
    n = mt7925_build_sta_rec(b, sizeof b, c, MT7925_WCID_AP, MT7925_STA_STATE_NONE);
    if ((e = uni(d, MT7925_UNI_CMD_STA_REC, b, n, 0)) != MT7925_OK) return e;
    if (!++d->roc_token) d->roc_token = 1;
    roc_wait w = {d->on_event, d->cb_ctx, d->roc_token, 0, 0};
    n = mt7925_build_roc(b, sizeof b, w.want, c->channel, c->band, MT7925_ROC_MS, 0);
    /* The grant may arrive while the command's own reply is awaited, so the
     * hook is in place before the send. */
    d->on_event = roc_hook;
    d->cb_ctx = &w;
    e = uni(d, MT7925_UNI_CMD_ROC, b, n, 0);
    for (uint32_t spent = 0; e == MT7925_OK && !w.granted; spent += MT7925_POLL_STEP_US) {
        if (spent >= MT7925_MCU_REPLY_US) {
            e = MT7925_E_TIMEOUT;
            break;
        }
        int r = mt7925_poll_rings(d);
        if (r < 0) {
            e = (mt7925_err)(-r);
            break;
        }
        if (!r && !w.granted) d->ops.udelay(d->ops.ctx, MT7925_POLL_STEP_US);
    }
    d->on_event = w.prev;
    d->cb_ctx = w.prev_ctx;
    d->roc_status = w.status; /* the reference does not act on it either */
    if (e == MT7925_E_TIMEOUT) { /* the reference aborts an ungranted ROC */
        n = mt7925_build_roc(b, sizeof b, w.want, 0, 0, 0, 1);
        (void)uni(d, MT7925_UNI_CMD_ROC, b, n, 0);
    }
    return e;
}

mt7925_err mt7925_join_assoc_done(mt7925_dev *d, const mt7925_join_cfg *c) {
    uint8_t b[MT7925_JOIN_CMD_MAX];
    if (!d || !band_ok(c)) return MT7925_E_ARG;
    if (!d->fw_running) return MT7925_E_STATE;
    mt7925_err e = uni(d, MT7925_UNI_CMD_BSS_INFO, b, mt7925_build_bss_info(b, sizeof b, c, 1), 0);
    if (e != MT7925_OK) return e;
    uint32_t n = mt7925_build_sta_rec(b, sizeof b, c, MT7925_WCID_AP, MT7925_STA_STATE_ASSOC);
    if ((e = uni(d, MT7925_UNI_CMD_STA_REC, b, n, 0)) != MT7925_OK) return e;
    n = mt7925_build_sta_rec(b, sizeof b, c, MT7925_WCID_OWN, MT7925_STA_STATE_ASSOC);
    return uni(d, MT7925_UNI_CMD_STA_REC, b, n, 0);
}

mt7925_err mt7925_join_install_key(mt7925_dev *d, const mt7925_join_cfg *c, int pairwise, uint8_t key_id,
                                   const uint8_t key[16]) {
    uint8_t b[MT7925_JOIN_CMD_MAX];
    if (!d || !c || !key) return MT7925_E_ARG;
    if (!d->fw_running) return MT7925_E_STATE;
    uint32_t n = mt7925_build_key(b, sizeof b, c, pairwise, key_id, key);
    mt7925_err e = n ? uni(d, MT7925_UNI_CMD_STA_REC, b, n, 1) : MT7925_E_ARG;
    zero(b, sizeof b);
    return e;
}

mt7925_err mt7925_join_authorized(mt7925_dev *d) {
    uint8_t b[16];
    if (!d) return MT7925_E_ARG;
    if (!d->fw_running) return MT7925_E_STATE;
    return uni(d, MT7925_UNI_CMD_ROC, b, mt7925_build_roc(b, sizeof b, d->roc_token, 0, 0, 0, 1), 0);
}

/* ---- Station adapter: wifi_radio over an mt7925_dev ---- */

static void link_rx(void *ctx, const uint8_t *buf, uint32_t len) {
    mt7925_link *l = ctx;
    mt7925_rx_info ri;
    mt7925_rx_err e = mt7925_rx_parse(buf, len, &ri);
    if (e != MT7925_RX_OK) {
        l->rx_refused[e < MT7925_LINK_RX_ERRS ? e : 0]++;
        return;
    }
    wifi_sta_rx_pn(l->sta, ri.frame, ri.frame_len, ri.decrypted, ri.has_pn, ri.pn, ri.has_rssi ? ri.rssi : 0);
}

static int link_join_prepare(void *ctx, const wifi_bss *b) {
    mt7925_link *l = ctx;
    mt7925_join_cfg *c = &l->cfg;
    for (int i = 0; i < 6; i++) c->bssid[i] = b->bssid[i];
    c->channel = b->channel;
    c->band = b->channel <= 14 ? 1u : 2u; /* 6 GHz needs the HE operation element: not parsed yet */
    c->beacon_int = b->beacon_int;
    c->dtim = 1;                          /* TIM element not parsed: the reference default when unknown */
    c->qos = 0;                           /* no WMM element offered by us */
    c->short_slot = (b->cap & WIFI_CAP_SHORT_SLOT) != 0;
    c->aid = 0;
    c->cipher = b->rsn.present ? MT7925_CIPHER_CCMP : MT7925_CIPHER_NONE;
    c->rssi = b->rssi;
    return mt7925_join_prepare(l->d, c) == MT7925_OK ? 0 : -1;
}

static int link_assoc_done(void *ctx, const wifi_bss *b, uint16_t aid) {
    mt7925_link *l = ctx;
    (void)b;
    l->cfg.aid = aid;
    return mt7925_join_assoc_done(l->d, &l->cfg) == MT7925_OK ? 0 : -1;
}

static int link_tx(void *ctx, const uint8_t *frame, uint32_t len, int protect) {
    mt7925_link *l = ctx;
    mt7925_tx_opts o = {MT7925_WCID_AP, MT7925_OMAC_IDX, 0, (uint8_t)(protect ? 1 : 0), 0};
    mt7925_err e = mt7925_tx_80211(l->d, frame, len, &o);
    if (e == MT7925_E_FULL) { /* tokens come back through TX_FREE on the RX rings */
        if (mt7925_poll_rings(l->d) < 0) return -1;
        e = mt7925_tx_80211(l->d, frame, len, &o);
    }
    return e == MT7925_OK ? 0 : -1;
}

/* CCMP-128 keys only (the RSN layer refuses every other cipher). */
static int link_set_key(void *ctx, int pairwise, uint8_t key_id, const uint8_t *key, uint32_t len) {
    mt7925_link *l = ctx;
    if (len != 16u) return -1;
    return mt7925_join_install_key(l->d, &l->cfg, pairwise, key_id, key) == MT7925_OK ? 0 : -1;
}

static int link_poll(void *ctx) {
    mt7925_link *l = ctx;
    return mt7925_poll_rings(l->d);
}

static void link_udelay(void *ctx, uint32_t us) {
    mt7925_link *l = ctx;
    l->d->ops.udelay(l->d->ops.ctx, us);
}

void mt7925_link_attach(mt7925_link *l, mt7925_dev *d, wifi_sta *s, const uint8_t mac[6]) {
    if (!l || !d || !s || !mac) return;
    *l = (mt7925_link){0};
    l->d = d;
    l->sta = s;
    for (int i = 0; i < 6; i++) l->cfg.own_mac[i] = mac[i];
    wifi_radio r = {l, link_join_prepare, link_assoc_done, link_tx, link_poll, link_udelay, link_set_key,
                    0, 0 /* entropy is the platform's: the binding sets s->r.random after attach */};
    wifi_sta_init(s, &r, mac);
    d->on_rx = link_rx;
    d->rx_ctx = l;
}
