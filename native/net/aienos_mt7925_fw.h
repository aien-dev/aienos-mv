/* MediaTek MT7925 firmware containers: parse and plan, never load.
 *
 * Two files feed the chip's own processor (the "MCU") before Wi-Fi works:
 *   mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin  ROM patch
 *   mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.bin       RAM firmware (+ CLC)
 * (the distribution ships them zstd-compressed; these parsers take the raw
 * bytes). The layouts are written from a reading of the open-source Linux
 * driver (openwrt/mt76 @467ae2e3, BSD-3-Clause-Clear, mt76_connac_mcu.h
 * structs mt76_connac2_patch_hdr/_patch_sec/_fw_trailer/_fw_region and
 * mt76_connac_mcu.c mt76_connac2_load_patch/_load_ram). No MediaTek document
 * describes them (DOCS SILENT). This is new C, not a copy.
 *
 * Stricter than the reference: every section and region must lie inside the
 * file (the reference does not check the patch sections or the RAM regions on
 * the download path), region counts are capped, and the output is a plan of
 * download commands. The trailer and header checksums are carried but not
 * verified: their algorithm is UNKNOWN (the reference never checks them).
 *
 * No heap, no I/O, no clock. */
#ifndef AIENOS_MT7925_FW_H
#define AIENOS_MT7925_FW_H

#include <stddef.h>
#include <stdint.h>

#define MT7925_FW_PATCH_HDR_LEN 96u /* build_date 16, platform 4, 3x be32, 2x u16, desc 64 */
#define MT7925_FW_PATCH_SEC_LEN 64u
#define MT7925_FW_TRAILER_LEN 36u
#define MT7925_FW_REGION_LEN 40u
#define MT7925_FW_MAX_SECTIONS 8u
#define MT7925_FW_MAX_REGIONS 16u
#define MT7925_FW_SCATTER_MAX 4096u /* largest FW_SCATTER chunk on PCIe (reference max_len) */

/* MCU command ids used by the download path (legacy, non-UNI commands). */
#define MT7925_MCU_CMD_TARGET_ADDRESS_LEN_REQ 0x01u
#define MT7925_MCU_CMD_FW_START_REQ 0x02u
#define MT7925_MCU_CMD_PATCH_START_REQ 0x05u
#define MT7925_MCU_CMD_PATCH_FINISH_REQ 0x07u
#define MT7925_MCU_CMD_PATCH_SEM_CONTROL 0x10u
#define MT7925_MCU_CMD_FW_SCATTER 0xeeu
#define MT7925_MCU_CMD_RESTART_DL_REQ 0xefu

/* Download mode word (init-download request "mode"). */
#define MT7925_DL_MODE_ENCRYPT (1u << 0)
#define MT7925_DL_MODE_KEY_IDX_SHIFT 1u
#define MT7925_DL_MODE_KEY_IDX_MASK (3u << 1)
#define MT7925_DL_MODE_RESET_SEC_IV (1u << 3)
#define MT7925_DL_CONFIG_ENCRY_MODE_SEL (1u << 6)
#define MT7925_DL_MODE_NEED_RSP (1u << 31)

/* RAM region feature_set bits. */
#define MT7925_FW_FEATURE_SET_ENCRYPT (1u << 0)
#define MT7925_FW_FEATURE_SET_KEY_IDX_MASK (3u << 1)
#define MT7925_FW_FEATURE_ENCRY_MODE (1u << 4)
#define MT7925_FW_FEATURE_OVERRIDE_ADDR (1u << 5)
#define MT7925_FW_FEATURE_NON_DL (1u << 6)
#define MT7925_FW_TYPE_CLC 2u

/* FW_START_REQ option bits. */
#define MT7925_FW_START_OVERRIDE (1u << 0)

/* Patch semaphore request ops and reply codes. */
#define MT7925_PATCH_SEM_RELEASE 0u
#define MT7925_PATCH_SEM_GET 1u
#define MT7925_PATCH_NOT_DL_SEM_FAIL 0u
#define MT7925_PATCH_IS_DL 1u
#define MT7925_PATCH_NOT_DL_SEM_SUCCESS 2u
#define MT7925_PATCH_REL_SEM_SUCCESS 3u

/* Patch section info / encryption encoding. */
#define MT7925_PATCH_SEC_TYPE_MASK 0xffffu
#define MT7925_PATCH_SEC_TYPE_INFO 0x2u
#define MT7925_PATCH_SEC_NOT_SUPPORT 0xffffffffu
#define MT7925_PATCH_SEC_ENC_TYPE_SHIFT 24u
#define MT7925_PATCH_SEC_ENC_TYPE_PLAIN 0x00u
#define MT7925_PATCH_SEC_ENC_TYPE_AES 0x01u
#define MT7925_PATCH_SEC_ENC_TYPE_SCRAMBLE 0x02u
#define MT7925_PATCH_SEC_ENC_AES_KEY_MASK 0xffu

typedef enum {
    MT7925_FW_OK = 0,
    MT7925_FW_E_ARG = 1,      /* NULL buffer or output */
    MT7925_FW_E_SHORT = 2,    /* file shorter than its header/trailer */
    MT7925_FW_E_COUNT = 3,    /* zero or too many sections/regions */
    MT7925_FW_E_SECTION = 4,  /* section type is not INFO */
    MT7925_FW_E_BOUNDS = 5,   /* a section/region's bytes are not inside the file */
    MT7925_FW_E_ENC = 6,      /* unknown encryption type (the reference only logs it) */
    MT7925_FW_E_LEN = 7,      /* zero-length download, or an address/length that wraps */
} mt7925_fw_err;

/* One init-download request plus the bytes to scatter after it. */
typedef struct {
    uint8_t cmd;       /* PATCH_START_REQ or TARGET_ADDRESS_LEN_REQ */
    uint32_t addr;     /* chip address */
    uint32_t len;      /* bytes to scatter */
    uint32_t mode;     /* DL mode word */
    uint32_t file_off; /* where the bytes start in the file */
} mt7925_fw_dl;

typedef struct {
    char build_date[17]; /* NUL-terminated copy of the 16 header bytes */
    char platform[5];
    uint32_t hw_sw_ver;
    uint32_t patch_ver;
    uint16_t checksum;   /* carried, algorithm UNKNOWN, not verified */
    uint32_t desc_patch_ver, desc_subsys, desc_feature, desc_crc;
    uint32_t n_sections;
    mt7925_fw_dl dl[MT7925_FW_MAX_SECTIONS];
} mt7925_fw_patch;

typedef struct {
    uint32_t addr, len, file_off;
    uint32_t decomp_crc, decomp_len, decomp_blk_sz;
    uint8_t feature_set, type;
    uint8_t download;    /* 0 when NON_DL (CLC and other host-side data) */
} mt7925_fw_region;

typedef struct {
    uint8_t chip_id, eco_code, n_region, format_ver, format_flag;
    char fw_ver[11];     /* NUL-terminated copy of the 10 trailer bytes */
    char build_date[16]; /* NUL-terminated copy of the 15 trailer bytes */
    uint32_t crc;        /* carried, algorithm UNKNOWN, not verified */
    mt7925_fw_region region[MT7925_FW_MAX_REGIONS];
    uint32_t n_dl;
    mt7925_fw_dl dl[MT7925_FW_MAX_REGIONS];
    uint32_t start_addr;   /* FW_START_REQ addr (override region, else 0) */
    uint32_t start_option; /* FW_START_REQ option */
    int clc_region;        /* index of the NON_DL CLC region, -1 if none */
} mt7925_fw_ram;

/* Parse the ROM patch file and produce its download plan. */
mt7925_fw_err mt7925_fw_patch_parse(const uint8_t *buf, size_t len, mt7925_fw_patch *out);

/* Parse the RAM firmware file (trailer at the end, regions before it, region
 * bytes concatenated from offset 0) and produce its download plan. */
mt7925_fw_err mt7925_fw_ram_parse(const uint8_t *buf, size_t len, mt7925_fw_ram *out);

/* Init-download mode for a patch section's sec_key_idx word. */
mt7925_fw_err mt7925_fw_patch_mode(uint32_t sec_info, uint32_t *mode);

/* Init-download mode for a RAM region's feature_set byte. */
uint32_t mt7925_fw_ram_mode(uint8_t feature_set);

/* Which init-download command the chip expects for `addr` (MT7925 rule). */
uint8_t mt7925_fw_dl_cmd(uint32_t addr);

const char *mt7925_fw_strerror(mt7925_fw_err e);

#endif
