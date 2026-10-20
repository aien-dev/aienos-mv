/* MediaTek MT7925 firmware container parsers. See aienos_mt7925_fw.h. */
#include "aienos_mt7925_fw.h"

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void copy_str(char *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++)
        dst[i] = (char)src[i];
    dst[n] = 0;
}

/* [off, off+n) inside [0, len)? Never computes off+n. */
static int inside(size_t len, uint64_t off, uint64_t n) { return off <= len && n <= len - off; }

uint8_t mt7925_fw_dl_cmd(uint32_t addr) {
    /* Reference mt76_connac_mcu_init_download, MT7925 branch: the patch and
     * the two addresses below take PATCH_START_REQ, everything else
     * TARGET_ADDRESS_LEN_REQ. INFERRED from source, not from a document. */
    if (addr == 0x200000u || addr == 0x900000u || addr == 0xe0002800u)
        return MT7925_MCU_CMD_PATCH_START_REQ;
    return MT7925_MCU_CMD_TARGET_ADDRESS_LEN_REQ;
}

mt7925_fw_err mt7925_fw_patch_mode(uint32_t sec_info, uint32_t *mode) {
    uint32_t m = MT7925_DL_MODE_NEED_RSP;
    if (!mode)
        return MT7925_FW_E_ARG;
    if (sec_info != MT7925_PATCH_SEC_NOT_SUPPORT) {
        switch (sec_info >> MT7925_PATCH_SEC_ENC_TYPE_SHIFT) {
        case MT7925_PATCH_SEC_ENC_TYPE_PLAIN:
            break;
        case MT7925_PATCH_SEC_ENC_TYPE_AES:
            m |= MT7925_DL_MODE_ENCRYPT | MT7925_DL_MODE_RESET_SEC_IV;
            m |= ((sec_info & MT7925_PATCH_SEC_ENC_AES_KEY_MASK) << MT7925_DL_MODE_KEY_IDX_SHIFT) &
                 MT7925_DL_MODE_KEY_IDX_MASK;
            break;
        case MT7925_PATCH_SEC_ENC_TYPE_SCRAMBLE:
            m |= MT7925_DL_MODE_ENCRYPT | MT7925_DL_CONFIG_ENCRY_MODE_SEL | MT7925_DL_MODE_RESET_SEC_IV;
            break;
        default:
            /* The reference logs and carries on with a plain mode; sending
             * encrypted bytes as plain is never right, so refuse. */
            return MT7925_FW_E_ENC;
        }
    }
    *mode = m;
    return MT7925_FW_OK;
}

uint32_t mt7925_fw_ram_mode(uint8_t fs) {
    uint32_t m = MT7925_DL_MODE_NEED_RSP;
    if (fs & MT7925_FW_FEATURE_SET_ENCRYPT)
        m |= MT7925_DL_MODE_ENCRYPT | MT7925_DL_MODE_RESET_SEC_IV;
    if (fs & MT7925_FW_FEATURE_ENCRY_MODE)
        m |= MT7925_DL_CONFIG_ENCRY_MODE_SEL;
    m |= (uint32_t)(fs & MT7925_FW_FEATURE_SET_KEY_IDX_MASK); /* same bit position (2:1) */
    return m;
}

mt7925_fw_err mt7925_fw_patch_parse(const uint8_t *buf, size_t len, mt7925_fw_patch *out) {
    if (!buf || !out)
        return MT7925_FW_E_ARG;
    for (size_t i = 0; i < sizeof(*out); i++)
        ((uint8_t *)out)[i] = 0;
    if (len < MT7925_FW_PATCH_HDR_LEN)
        return MT7925_FW_E_SHORT;
    copy_str(out->build_date, buf, 16);
    copy_str(out->platform, buf + 16, 4);
    out->hw_sw_ver = be32(buf + 20);
    out->patch_ver = be32(buf + 24);
    out->checksum = be16(buf + 28);
    const uint8_t *d = buf + 32;
    out->desc_patch_ver = be32(d);
    out->desc_subsys = be32(d + 4);
    out->desc_feature = be32(d + 8);
    uint32_t n = be32(d + 12);
    out->desc_crc = be32(d + 16);
    if (n == 0 || n > MT7925_FW_MAX_SECTIONS)
        return MT7925_FW_E_COUNT;
    if (!inside(len, MT7925_FW_PATCH_HDR_LEN, (uint64_t)n * MT7925_FW_PATCH_SEC_LEN))
        return MT7925_FW_E_SHORT;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *s = buf + MT7925_FW_PATCH_HDR_LEN + (size_t)i * MT7925_FW_PATCH_SEC_LEN;
        if ((be32(s) & MT7925_PATCH_SEC_TYPE_MASK) != MT7925_PATCH_SEC_TYPE_INFO)
            return MT7925_FW_E_SECTION;
        uint32_t offs = be32(s + 4);
        uint32_t addr = be32(s + 12);
        uint32_t dlen = be32(s + 16);
        uint32_t mode;
        mt7925_fw_err e = mt7925_fw_patch_mode(be32(s + 20), &mode);
        if (e != MT7925_FW_OK)
            return e;
        if (dlen == 0 || (uint64_t)addr + dlen > 0x100000000ull)
            return MT7925_FW_E_LEN;
        if (!inside(len, offs, dlen))
            return MT7925_FW_E_BOUNDS;
        out->dl[i] = (mt7925_fw_dl){mt7925_fw_dl_cmd(addr), addr, dlen, mode, offs};
    }
    out->n_sections = n;
    return MT7925_FW_OK;
}

mt7925_fw_err mt7925_fw_ram_parse(const uint8_t *buf, size_t len, mt7925_fw_ram *out) {
    if (!buf || !out)
        return MT7925_FW_E_ARG;
    for (size_t i = 0; i < sizeof(*out); i++)
        ((uint8_t *)out)[i] = 0;
    out->clc_region = -1;
    if (len < MT7925_FW_TRAILER_LEN)
        return MT7925_FW_E_SHORT;
    const uint8_t *t = buf + len - MT7925_FW_TRAILER_LEN;
    out->chip_id = t[0];
    out->eco_code = t[1];
    out->n_region = t[2];
    out->format_ver = t[3];
    out->format_flag = t[4];
    copy_str(out->fw_ver, t + 7, 10);
    copy_str(out->build_date, t + 17, 15);
    out->crc = le32(t + 32);
    uint32_t n = out->n_region;
    if (n == 0 || n > MT7925_FW_MAX_REGIONS)
        return MT7925_FW_E_COUNT;
    uint64_t table = (uint64_t)n * MT7925_FW_REGION_LEN;
    if (table > len - MT7925_FW_TRAILER_LEN)
        return MT7925_FW_E_SHORT;
    size_t table_off = len - MT7925_FW_TRAILER_LEN - (size_t)table;
    uint64_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *r = buf + table_off + (size_t)i * MT7925_FW_REGION_LEN;
        mt7925_fw_region *g = &out->region[i];
        g->decomp_crc = le32(r);
        g->decomp_len = le32(r + 4);
        g->decomp_blk_sz = le32(r + 8);
        g->addr = le32(r + 16);
        g->len = le32(r + 20);
        g->feature_set = r[24];
        g->type = r[25];
        g->download = (g->feature_set & MT7925_FW_FEATURE_NON_DL) ? 0 : 1;
        /* Region bytes must end before the region table. */
        if (!inside(table_off, off, g->len))
            return MT7925_FW_E_BOUNDS;
        g->file_off = (uint32_t)off;
        if (!g->download) {
            if (g->type == MT7925_FW_TYPE_CLC && out->clc_region < 0)
                out->clc_region = (int)i;
        } else {
            if (g->len == 0 || (uint64_t)g->addr + g->len > 0x100000000ull)
                return MT7925_FW_E_LEN;
            if (g->feature_set & MT7925_FW_FEATURE_OVERRIDE_ADDR)
                out->start_addr = g->addr;
            out->dl[out->n_dl++] =
                (mt7925_fw_dl){mt7925_fw_dl_cmd(g->addr), g->addr, g->len, mt7925_fw_ram_mode(g->feature_set),
                               g->file_off};
        }
        off += g->len;
    }
    if (out->n_dl == 0)
        return MT7925_FW_E_COUNT;
    out->start_option = out->start_addr ? MT7925_FW_START_OVERRIDE : 0;
    return MT7925_FW_OK;
}

const char *mt7925_fw_strerror(mt7925_fw_err e) {
    switch (e) {
    case MT7925_FW_OK: return "ok";
    case MT7925_FW_E_ARG: return "bad argument";
    case MT7925_FW_E_SHORT: return "file too short";
    case MT7925_FW_E_COUNT: return "bad section/region count";
    case MT7925_FW_E_SECTION: return "section type not INFO";
    case MT7925_FW_E_BOUNDS: return "section/region outside the file";
    case MT7925_FW_E_ENC: return "unknown encryption type";
    case MT7925_FW_E_LEN: return "zero or wrapping download";
    }
    return "?";
}
