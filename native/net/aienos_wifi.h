/* IEEE 802.11 station-side frame layer, chip independent.
 *
 * Written from IEEE Std 802.11-2020: 9.2.3/9.2.4 (general frame format,
 * frame control), 9.3.3 (management frame bodies: beacon 9.3.3.2, probe
 * request/response 9.3.3.9/10, authentication 9.3.3.11, association
 * request/response 9.3.3.5/6, deauthentication 9.3.3.12), 9.4.1 (fixed
 * fields), 9.4.2 (elements: SSID 9.4.2.2, Supported Rates 9.4.2.3, DSSS
 * Parameter Set 9.4.2.4, RSNE 9.4.2.24) and 9.3.2 plus IEEE 802.1H / RFC 1042
 * LLC/SNAP for data frames. Nothing here is MediaTek specific: the iMac
 * (Broadcom), Raspberry Pi (Cypress/Infineon) and MacBook Wi-Fi drivers can
 * reuse it unchanged.
 *
 * Bounded parsers over (pointer, length); every element walk stops at the
 * frame end and refuses an element that overruns it. No heap, no clock, no
 * I/O. Multi-byte 802.11 fields are little endian. */
#ifndef AIENOS_WIFI_H
#define AIENOS_WIFI_H

#include <stddef.h>
#include <stdint.h>

#define WIFI_ALEN 6u
#define WIFI_SSID_MAX 32u
#define WIFI_HDR_MGMT 24u
#define WIFI_BSS_MAX 16u
#define WIFI_RATES_MAX 16u
#define WIFI_FRAME_MAX 2304u /* largest MSDU (9.2.4.7.1 note) */
#define WIFI_RSNE_MAX 64u

/* Frame control: type (bits 3:2) and subtype (bits 7:4) of byte 0, flags in byte 1. */
#define WIFI_TYPE_MGMT 0u
#define WIFI_TYPE_CTRL 1u
#define WIFI_TYPE_DATA 2u
#define WIFI_ST_ASSOC_REQ 0u
#define WIFI_ST_ASSOC_RESP 1u
#define WIFI_ST_PROBE_REQ 4u
#define WIFI_ST_PROBE_RESP 5u
#define WIFI_ST_BEACON 8u
#define WIFI_ST_DISASSOC 10u
#define WIFI_ST_AUTH 11u
#define WIFI_ST_DEAUTH 12u
#define WIFI_ST_DATA 0u
#define WIFI_ST_QOS_DATA 8u
#define WIFI_FL_TODS 0x01u
#define WIFI_FL_FROMDS 0x02u
#define WIFI_FL_MOREFRAG 0x04u
#define WIFI_FL_PROTECTED 0x40u

/* Element ids. */
#define WIFI_EID_SSID 0u
#define WIFI_EID_RATES 1u
#define WIFI_EID_DS 3u
#define WIFI_EID_HT_CAP 45u
#define WIFI_EID_RSN 48u
#define WIFI_EID_EXT_RATES 50u
#define WIFI_EID_HT_OP 61u
#define WIFI_EID_RSNX 244u

/* Capability information bits (9.4.1.4). */
#define WIFI_CAP_ESS 0x0001u
#define WIFI_CAP_PRIVACY 0x0010u
#define WIFI_CAP_SHORT_SLOT 0x0400u

/* Cipher and AKM selector bits (suite type of OUI 00-0F-AC, Table 9-149 / 9-151). */
#define WIFI_CIPHER_WEP40 (1u << 1)
#define WIFI_CIPHER_TKIP (1u << 2)
#define WIFI_CIPHER_CCMP128 (1u << 4)
#define WIFI_CIPHER_WEP104 (1u << 5)
#define WIFI_CIPHER_BIP_CMAC128 (1u << 6)
#define WIFI_CIPHER_GCMP128 (1u << 8)
#define WIFI_CIPHER_GCMP256 (1u << 9)
#define WIFI_CIPHER_CCMP256 (1u << 10)
#define WIFI_CIPHER_OTHER (1u << 31)
#define WIFI_AKM_8021X (1u << 1)
#define WIFI_AKM_PSK (1u << 2)
#define WIFI_AKM_8021X_SHA256 (1u << 5)
#define WIFI_AKM_PSK_SHA256 (1u << 6)
#define WIFI_AKM_SAE (1u << 8)
#define WIFI_AKM_OTHER (1u << 31)
#define WIFI_RSN_CAP_MFPR 0x0040u
#define WIFI_RSN_CAP_MFPC 0x0080u

/* Status and reason codes used here (9.4.1.9 / 9.4.1.7). */
#define WIFI_STATUS_SUCCESS 0u
#define WIFI_REASON_UNSPECIFIED 1u
#define WIFI_REASON_LEAVING 3u
#define WIFI_REASON_4WAY_TIMEOUT 15u /* Table 9-49 */
#define WIFI_AUTH_OPEN 0u
#define WIFI_AUTH_SAE 3u

typedef enum {
    WIFI_OK = 0,
    WIFI_E_ARG = 1,     /* NULL or a field out of range */
    WIFI_E_SHORT = 2,   /* frame shorter than its fixed part */
    WIFI_E_ELEMENT = 3, /* element overruns the frame, or a bad element length */
    WIFI_E_TYPE = 4,    /* not the frame type asked for */
    WIFI_E_RSN = 5,     /* malformed RSN element */
    WIFI_E_SPACE = 6,   /* output buffer too small */
    WIFI_E_UNSUPPORTED = 7, /* valid but not handled (4-address, fragments, non-SNAP) */
    WIFI_E_PROTECTED = 8,   /* still encrypted: decryption belongs to the chip */
    WIFI_E_CRYPTO = 9,      /* the security the network needs lacks a primitive (wifi_sta_missing_crypto) */
    WIFI_E_TIMEOUT = 10,    /* no answer from the AP within the budget */
    WIFI_E_REFUSED = 11,    /* AP answered with a failure status (wifi_sta.last_status) */
    WIFI_E_RADIO = 12,      /* the chip driver refused an operation */
    WIFI_E_STATE = 13,      /* call out of order (send before association) */
    WIFI_E_NOT_FOUND = 14,  /* no usable BSS with that SSID in the scan table */
} wifi_err;

typedef struct {
    uint8_t type, subtype, flags;
    uint8_t a1[WIFI_ALEN], a2[WIFI_ALEN], a3[WIFI_ALEN];
    uint16_t duration, seq_ctrl, qos;
    uint32_t hdr_len;      /* 24, or 26 with QoS control */
    const uint8_t *body;
    uint32_t body_len;
} wifi_hdr;

typedef struct {
    uint8_t present;
    uint16_t version;
    uint32_t group;      /* WIFI_CIPHER_* bit */
    uint32_t pairwise;   /* OR of WIFI_CIPHER_* bits */
    uint32_t akm;        /* OR of WIFI_AKM_* bits */
    uint16_t caps;
    uint32_t group_mgmt; /* WIFI_CIPHER_BIP_* or 0 */
} wifi_rsn;

typedef struct {
    uint8_t bssid[WIFI_ALEN];
    uint8_t ssid_len;
    uint8_t ssid[WIFI_SSID_MAX];
    uint8_t channel;     /* from DSSS Parameter Set or HT Operation, 0 if neither */
    uint16_t beacon_int, cap;
    uint8_t n_rates;
    uint8_t rates[WIFI_RATES_MAX]; /* Supported + Extended Supported Rates, raw */
    uint8_t has_ht;
    int8_t rssi;         /* filled by the chip driver */
    wifi_rsn rsn;
    uint8_t rsne[WIFI_RSNE_MAX]; /* the RSN element as received (id, len, body); 4-way message 3 must match it */
    uint8_t rsne_len;    /* 0 if absent or longer than WIFI_RSNE_MAX */
    uint32_t seen;       /* update counter */
} wifi_bss;

typedef struct {
    wifi_bss e[WIFI_BSS_MAX];
    uint32_t n;
} wifi_bss_table;

typedef enum {
    WIFI_SEC_OPEN = 0,
    WIFI_SEC_WPA2_PSK = 1,        /* AKM 2, CCMP-128 */
    WIFI_SEC_WPA2_PSK_SHA256 = 2, /* AKM 6, CCMP-128, PMF */
    WIFI_SEC_WPA3_SAE = 3,        /* AKM 8, CCMP-128, PMF required */
    WIFI_SEC_UNSUPPORTED = 4,     /* WEP, TKIP-only, 802.1X, unknown suites */
} wifi_sec;

wifi_err wifi_parse_hdr(const uint8_t *f, uint32_t len, wifi_hdr *h);
/* Find element `eid` in an element list. Returns WIFI_OK and the body view,
 * WIFI_E_TYPE if absent, WIFI_E_ELEMENT if the list is malformed. */
wifi_err wifi_find_element(const uint8_t *ies, uint32_t len, uint8_t eid, const uint8_t **body, uint8_t *blen);
wifi_err wifi_check_elements(const uint8_t *ies, uint32_t len);
wifi_err wifi_parse_rsn(const uint8_t *body, uint32_t len, wifi_rsn *r);
/* Element bytes (id, len, body) for our association request. */
wifi_err wifi_build_rsn(wifi_sec sec, uint8_t *out, uint32_t cap, uint32_t *out_len);
wifi_err wifi_parse_beacon(const uint8_t *f, uint32_t len, wifi_bss *b);
wifi_sec wifi_choose_security(const wifi_bss *b);

void wifi_bss_table_init(wifi_bss_table *t);
/* Insert or refresh by BSSID; a full table replaces the weakest entry. */
void wifi_bss_table_update(wifi_bss_table *t, const wifi_bss *b);
/* Strongest BSS with this SSID whose security we can handle; NULL if none. */
const wifi_bss *wifi_bss_select(const wifi_bss_table *t, const uint8_t *ssid, uint8_t ssid_len);

wifi_err wifi_build_probe_req(uint8_t *out, uint32_t cap, const uint8_t sa[6], const uint8_t *ssid,
                              uint8_t ssid_len, uint32_t *out_len);
wifi_err wifi_build_auth(uint8_t *out, uint32_t cap, const uint8_t bssid[6], const uint8_t sa[6], uint16_t alg,
                         uint16_t seq, uint16_t status, const uint8_t *extra, uint32_t extra_len, uint32_t *out_len);
wifi_err wifi_parse_auth(const uint8_t *f, uint32_t len, uint16_t *alg, uint16_t *seq, uint16_t *status,
                         const uint8_t **extra, uint32_t *extra_len);
wifi_err wifi_build_assoc_req(uint8_t *out, uint32_t cap, const wifi_bss *b, const uint8_t sa[6], wifi_sec sec,
                              uint16_t listen_interval, uint32_t *out_len);
wifi_err wifi_parse_assoc_resp(const uint8_t *f, uint32_t len, uint16_t *cap_info, uint16_t *status, uint16_t *aid);
wifi_err wifi_build_deauth(uint8_t *out, uint32_t cap, const uint8_t da[6], const uint8_t sa[6],
                           const uint8_t bssid[6], uint16_t reason, uint32_t *out_len);

/* Ethernet II frame (dst, src, type, payload) -> 802.11 data frame to the AP
 * (ToDS, addr1 = BSSID, addr2 = SA, addr3 = DA) with RFC 1042 LLC/SNAP. */
wifi_err wifi_eth_to_80211(const uint8_t *eth, uint32_t eth_len, const uint8_t bssid[6], uint16_t seq_ctrl,
                           uint8_t *out, uint32_t cap, uint32_t *out_len);
/* 802.11 data frame from the AP (FromDS; plain or QoS data) -> Ethernet II
 * frame (dst = addr1, src = addr3). A frame with the Protected bit is taken
 * only when hw_decrypted says the chip decrypted it and stripped the CCMP
 * header and MIC; otherwise WIFI_E_PROTECTED. */
wifi_err wifi_80211_to_eth(const uint8_t *f, uint32_t len, const uint8_t bssid[6], int hw_decrypted, uint8_t *out,
                           uint32_t cap, uint32_t *out_len);

const char *wifi_strerror(wifi_err e);

#endif
