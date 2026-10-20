/* Host-only model of the MediaTek MT7925 as aienos_mt7925.c expects it to
 * behave. It encodes the SAME reading of the Linux mt76 driver as the driver
 * does, so a PASS proves the driver is internally consistent with that
 * reading and refuses every hostile device behaviour the model can inject.
 * It does NOT prove the real chip behaves this way: that is the attended
 * boot (docs/SPARK_WIFI_MT7925.md, UNKNOWN rows).
 *
 * The model owns a BAR0 register file, the chip-side registers behind the
 * L1/L2 remap windows, the five WFDMA rings (it reads and writes descriptors
 * in the driver's DMA region and refuses any bus address outside it, like an
 * SMMU window), an MCU that answers the firmware-download and scan commands,
 * and a radio with simulated access points that answer probe/auth/assoc and
 * carry data frames. */
#ifndef AIENOS_MT7925_MODEL_H
#define AIENOS_MT7925_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "../aienos_mt7925_sta.h"
#include "../../argus/sha256.h"

#define MODEL_TRACE_MAX 8192u
#define MODEL_CHIPREG_MAX 64u
#define MODEL_AP_MAX 4u
#define MODEL_FRAME_MAX 2048u

typedef struct {
    uint8_t bssid[6];
    uint8_t ssid_len;
    uint8_t ssid[32];
    uint8_t channel; /* 2.4 GHz channel number */
    int8_t rssi;
    uint8_t rsn[64]; /* RSN element body (no id/len), empty = open */
    uint8_t rsn_len;
    uint8_t ip[4];   /* address the AP answers ARP and ICMP echo for */
    uint8_t mac[6];  /* the AP's own station address behind the BSSID (gateway) */
} model_ap;

typedef enum {
    /* fault injection */
    MODEL_F_OWN_STUCK = 1u << 0,     /* OWN_SYNC never follows SET_OWN */
    MODEL_F_DRV_OWN_STUCK = 1u << 1, /* OWN_SYNC never clears */
    MODEL_F_RESET_STUCK = 1u << 2,   /* WFSYS init-done never sets */
    MODEL_F_MCU_SILENT = 1u << 3,    /* MCU never replies */
    MODEL_F_BAD_DIDX = 1u << 4,      /* TX dma_idx jumps past what was posted */
    MODEL_F_RX_OVERLONG = 1u << 5,   /* RX completion claims more bytes than posted */
    MODEL_F_WRONG_SEQ_FIRST = 1u << 6, /* send a reply with a stale sequence before the real one */
    MODEL_F_SEM_BUSY = 1u << 7,      /* patch semaphore refused */
    MODEL_F_PATCH_LOADED = 1u << 8,  /* patch already in ROM: semaphore says IS_DL */
    MODEL_F_NO_N9_READY = 1u << 9,   /* firmware never reports ready */
    MODEL_F_RING_IDX_BAD = 1u << 10, /* ring dma_idx reads out of range after programming */
    MODEL_F_FINISH_FAIL = 1u << 11,  /* PATCH_FINISH reports failure */
    MODEL_F_AP_DEAUTH = 1u << 12,    /* AP answers auth with a failure status */
    MODEL_F_TXFREE_BOGUS = 1u << 13, /* TX_FREE releases a token that is not in flight */
    MODEL_F_TXFREE_HOLD = 1u << 14,  /* never send TX_FREE: tokens stay held */
    MODEL_F_ROC_SILENT = 1u << 15,   /* ROC acknowledged but never granted */
    MODEL_F_STA_FAIL = 1u << 16,     /* STA_REC result carries a failure status */
    MODEL_F_AP_SILENT = 1u << 17,    /* AP never answers authentication */
    MODEL_F_ASSOC_DENY = 1u << 18,   /* AP answers association with status 17 */
    MODEL_F_TXFREE_SHORT = 1u << 19, /* TX_FREE announces more ids than it carries */
    MODEL_F_M3_BAD_MIC = 1u << 20,   /* AP: message 3 with a broken MIC */
    MODEL_F_M3_DOWNGRADE = 1u << 21, /* AP: message 3 RSNE differs from the beacon */
    MODEL_F_NO_M1 = 1u << 22,        /* AP: never starts the 4-way handshake */
} model_fault;

typedef struct {
    uint32_t op;   /* 'R' or 'W' */
    uint32_t addr; /* chip bus address for remapped accesses, else BAR0 offset */
    uint32_t val;
} model_trace;

typedef struct {
    uint32_t bar[MT7925_BAR0_BYTES / 4];
    struct { uint32_t addr, val; } chipreg[MODEL_CHIPREG_MAX];
    uint32_t n_chipreg;
    uint32_t faults;
    /* DMA region the driver gave the chip (the "SMMU window") */
    uint8_t *mem;
    uint64_t dma;
    size_t mem_len;
    uint32_t dma_faults;     /* accesses outside the window */
    uint64_t last_fault_addr;
    /* trace */
    model_trace trace[MODEL_TRACE_MAX];
    uint32_t n_trace;
    uint64_t time_us;
    /* MCU */
    uint32_t mcu_cmds, mcu_bad;
    uint8_t last_cmd, last_seq, last_uni;
    uint16_t last_uni_cid;
    uint8_t last_payload[MODEL_FRAME_MAX];
    uint32_t last_payload_len;
    int restarted, sem_held, patch_done, fw_started;
    uint32_t dl_addr, dl_len, dl_mode, dl_got; /* current init-download target */
    uint32_t dl_count, scatter_bytes;
    uint8_t dl_cmd;
    uint32_t dl_log_addr[32], dl_log_len[32], dl_log_mode[32];
    uint8_t dl_log_digest[32][32]; /* SHA-256 of the bytes scattered for each download */
    uint32_t start_option, start_addr;
    uint32_t scans;
    /* radio */
    model_ap ap[MODEL_AP_MAX];
    uint32_t n_ap;
    uint8_t sta_mac[6];
    uint32_t tx_frames;
    uint8_t last_tx[MODEL_FRAME_MAX];
    uint32_t last_tx_len;
    uint32_t pending_rx; /* frames the radio still has to deliver */
    uint8_t rxq[8][MODEL_FRAME_MAX];
    uint32_t rxq_len[8], rxq_type[8];
    uint32_t rxq_n;
    uint8_t evq[16][MODEL_FRAME_MAX]; /* MCU events waiting for an RX event slot */
    uint32_t evq_len[16], evq_n;
    sha256_ctx dl_sha;       /* digest of the download in progress */
    /* join state the MCU holds (from the UNI records) */
    uint32_t uni_bad;        /* UNI records whose TLV count or lengths did not add up */
    uint8_t dev_active, own_mac[6];
    uint8_t bss_records, bss_conn_state, bss_has_rlm, bss_cipher, bss_bssid[6];
    uint8_t sta_records, sta_state, sta_peer[6];
    uint16_t sta_wcid;
    uint8_t roc_token, roc_channel, roc_active;
    uint8_t key_pairwise[16], key_group[16];
    uint8_t keys;            /* bit 0 pairwise, bit 1 group */
    uint8_t efuse_mac[6];
    /* cut-through data TX */
    uint8_t last_txd[MT7925_TX_HDR_BYTES];
    uint32_t ct_frames, tx_freed_sent;
    uint16_t tok_pending[32];
    uint32_t n_tok_pending;
    /* access point side */
    int assoc_ap;            /* index of the AP we are associated with, -1 if none */
    uint32_t auths, assocs, arp_replies, echo_replies, data_rx, protected_tx, protected_dropped;
    /* AP authenticator (AKM 6 over the wpa_toy.h doubles, see there) */
    uint8_t ap_wpa;          /* 0 none, 1 message 1 sent, 2 message 3 sent, 3 done */
    uint8_t ap_pmk[32], ap_anonce[32], ap_ptk[48], ap_gtk[16];
    uint64_t ap_replay;
    uint32_t eapol_rx, eapol_bad, unprot_dropped;
} mt7925_model;

void model_init(mt7925_model *m, uint32_t faults);
void model_attach_dma(mt7925_model *m, uint8_t *mem, uint64_t dma, size_t len);
mt7925_ops model_ops(mt7925_model *m);
/* Index of the first trace entry matching op/addr at or after `from`, or -1. */
int model_trace_find(const mt7925_model *m, uint32_t op, uint32_t addr, int from);
/* Read a chip-side register as the model holds it. */
uint32_t model_chipreg(const mt7925_model *m, uint32_t addr);
/* Queue a frame for the RX data ring (type = RXD packet type) and deliver
 * what fits. */
void model_queue_rx(mt7925_model *m, uint32_t type, const uint8_t *buf, uint32_t len);
void model_add_ap(mt7925_model *m, const model_ap *ap);
/* Send TX_FREE for every token held (MODEL_F_TXFREE_HOLD tests). */
void model_release_tokens(mt7925_model *m);
/* PMK the access points use for their 4-way handshakes. */
void model_set_pmk(mt7925_model *m, const uint8_t pmk[32]);

#endif
