/* MT7925 firmware container tests.
 *
 *   mt7925_fw_test                 synthetic containers + every truncation + mutations
 *   mt7925_fw_test real PATCH RAM  parse the raw (decompressed) files on this box,
 *                                  print the plan, check the pinned digests
 *
 * The real mode never sends anything anywhere: it reads two files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../argus/sha256.h"
#include "../aienos_mt7925_fw.h"

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* Patch: header, 2 sections, then the two payloads. */
static size_t make_patch(uint8_t *b) {
    memset(b, 0, 1024);
    memcpy(b, "20240101000000a\0", 16);
    memcpy(b + 16, "ALPS", 4);
    put_be32(b + 20, 0x8a108a10u);
    put_be32(b + 24, 0x00000001u);
    put_be32(b + 32 + 12, 2); /* n_region */
    uint8_t *s0 = b + 96, *s1 = b + 160;
    put_be32(s0, 0x30002); put_be32(s0 + 4, 224); put_be32(s0 + 12, 0x200000); put_be32(s0 + 16, 100);
    put_be32(s0 + 20, 0x01000002); /* AES, key 2 */
    put_be32(s1, 0x2); put_be32(s1 + 4, 324); put_be32(s1 + 12, 0x00e00000); put_be32(s1 + 16, 60);
    put_be32(s1 + 20, 0xffffffffu); /* not supported -> plain */
    for (int i = 224; i < 384; i++) b[i] = (uint8_t)i;
    return 384;
}

/* RAM: 3 regions (download, CLC non-DL, download+override), region table, trailer. */
static size_t make_ram(uint8_t *b) {
    memset(b, 0, 1024);
    uint32_t lens[3] = {200, 50, 120};
    size_t data = 370, off = data;
    for (int i = 0; i < 3; i++, off += 40) {
        uint8_t *r = b + off;
        put_le32(r + 16, i == 0 ? 0x00900000u : (i == 1 ? 0 : 0x02000000u));
        put_le32(r + 20, lens[i]);
        r[24] = i == 1 ? 0x40 : (i == 2 ? (0x20 | 0x01 | 0x04) : 0x00);
        r[25] = i == 1 ? 2 : 0;
    }
    uint8_t *t = b + off;
    t[0] = 0x25; t[1] = 1; t[2] = 3; t[3] = 1;
    memcpy(t + 7, "____010000", 10);
    memcpy(t + 17, "20240101000000", 15);
    return off + 36;
}

static void synthetic(void) {
    uint8_t b[1024], m[1024];
    mt7925_fw_patch p;
    mt7925_fw_ram r;
    size_t n = make_patch(b);
    CHECK(mt7925_fw_patch_parse(b, n, &p) == MT7925_FW_OK);
    CHECK(p.n_sections == 2);
    CHECK(strcmp(p.platform, "ALPS") == 0);
    CHECK(p.hw_sw_ver == 0x8a108a10u);
    CHECK(p.dl[0].cmd == MT7925_MCU_CMD_PATCH_START_REQ && p.dl[0].addr == 0x200000 && p.dl[0].len == 100);
    CHECK(p.dl[0].mode == (MT7925_DL_MODE_NEED_RSP | MT7925_DL_MODE_ENCRYPT | MT7925_DL_MODE_RESET_SEC_IV | (2u << 1)));
    CHECK(p.dl[1].cmd == MT7925_MCU_CMD_TARGET_ADDRESS_LEN_REQ && p.dl[1].mode == MT7925_DL_MODE_NEED_RSP);
    CHECK(p.dl[1].file_off == 324);
    /* every truncation fails */
    for (size_t k = 0; k < n; k++) CHECK(mt7925_fw_patch_parse(b, k, &p) != MT7925_FW_OK);
    memcpy(m, b, n); put_be32(m + 44, 0); CHECK(mt7925_fw_patch_parse(m, n, &p) == MT7925_FW_E_COUNT);
    memcpy(m, b, n); put_be32(m + 44, 9); CHECK(mt7925_fw_patch_parse(m, n, &p) == MT7925_FW_E_COUNT);
    memcpy(m, b, n); put_be32(m + 96, 0x3); CHECK(mt7925_fw_patch_parse(m, n, &p) == MT7925_FW_E_SECTION);
    memcpy(m, b, n); put_be32(m + 160 + 4, 0xfffffff0u); CHECK(mt7925_fw_patch_parse(m, n, &p) == MT7925_FW_E_BOUNDS);
    memcpy(m, b, n); put_be32(m + 160 + 16, 61); CHECK(mt7925_fw_patch_parse(m, n, &p) == MT7925_FW_E_BOUNDS);
    memcpy(m, b, n); put_be32(m + 160 + 16, 0); CHECK(mt7925_fw_patch_parse(m, n, &p) == MT7925_FW_E_LEN);
    memcpy(m, b, n); put_be32(m + 160 + 12, 0xffffffe0u); CHECK(mt7925_fw_patch_parse(m, n, &p) == MT7925_FW_E_LEN);
    memcpy(m, b, n); put_be32(m + 96 + 20, 0x07000000u); CHECK(mt7925_fw_patch_parse(m, n, &p) == MT7925_FW_E_ENC);
    memcpy(m, b, n); put_be32(m + 96 + 20, 0x02000000u); CHECK(mt7925_fw_patch_parse(m, n, &p) == MT7925_FW_OK);
    CHECK(p.dl[0].mode == (MT7925_DL_MODE_NEED_RSP | MT7925_DL_MODE_ENCRYPT | MT7925_DL_CONFIG_ENCRY_MODE_SEL | MT7925_DL_MODE_RESET_SEC_IV));
    CHECK(mt7925_fw_patch_parse(NULL, n, &p) == MT7925_FW_E_ARG);
    CHECK(mt7925_fw_patch_parse(b, n, NULL) == MT7925_FW_E_ARG);

    n = make_ram(b);
    CHECK(mt7925_fw_ram_parse(b, n, &r) == MT7925_FW_OK);
    CHECK(r.n_region == 3 && r.n_dl == 2 && r.clc_region == 1);
    CHECK(r.chip_id == 0x25 && strcmp(r.fw_ver, "____010000") == 0 && strcmp(r.build_date, "20240101000000") == 0);
    CHECK(r.dl[0].cmd == MT7925_MCU_CMD_PATCH_START_REQ && r.dl[0].addr == 0x900000 && r.dl[0].file_off == 0);
    CHECK(r.dl[1].addr == 0x02000000u && r.dl[1].file_off == 250 && r.dl[1].len == 120);
    CHECK(r.dl[1].mode == (MT7925_DL_MODE_NEED_RSP | MT7925_DL_MODE_ENCRYPT | MT7925_DL_MODE_RESET_SEC_IV | 0x04));
    CHECK(r.start_addr == 0x02000000u && r.start_option == MT7925_FW_START_OVERRIDE);
    CHECK(r.region[1].file_off == 200 && !r.region[1].download);
    for (size_t k = 0; k < n; k++) CHECK(mt7925_fw_ram_parse(b, k, &r) != MT7925_FW_OK);
    memcpy(m, b, n); m[n - 36 + 2] = 0; CHECK(mt7925_fw_ram_parse(m, n, &r) == MT7925_FW_E_COUNT);
    memcpy(m, b, n); m[n - 36 + 2] = 17; CHECK(mt7925_fw_ram_parse(m, n, &r) == MT7925_FW_E_COUNT);
    memcpy(m, b, n); m[n - 36 + 2] = 13; CHECK(mt7925_fw_ram_parse(m, n, &r) == MT7925_FW_E_SHORT);
    memcpy(m, b, n); put_le32(m + 370 + 80 + 20, 121); CHECK(mt7925_fw_ram_parse(m, n, &r) == MT7925_FW_E_BOUNDS);
    memcpy(m, b, n); put_le32(m + 370 + 20, 0xffffff00u); CHECK(mt7925_fw_ram_parse(m, n, &r) == MT7925_FW_E_BOUNDS);
    memcpy(m, b, n); put_le32(m + 370 + 20, 0); CHECK(mt7925_fw_ram_parse(m, n, &r) == MT7925_FW_E_LEN);
    /* all regions non-DL: nothing to start */
    memcpy(m, b, n); m[370 + 24] = 0x40; m[370 + 80 + 24] = 0x40; CHECK(mt7925_fw_ram_parse(m, n, &r) == MT7925_FW_E_COUNT);
    CHECK(mt7925_fw_ram_parse(NULL, n, &r) == MT7925_FW_E_ARG);
    /* dl command rule */
    CHECK(mt7925_fw_dl_cmd(0xe0002800u) == MT7925_MCU_CMD_PATCH_START_REQ);
    CHECK(mt7925_fw_dl_cmd(0xe0002000u) == MT7925_MCU_CMD_TARGET_ADDRESS_LEN_REQ);
    /* seeded random bytes never crash and never yield an out-of-file plan */
    uint32_t x = 0x7925u;
    for (int it = 0; it < 20000; it++) {
        size_t len = (size_t)(x % 1024);
        for (size_t k = 0; k < len; k++) { x = x * 1664525u + 1013904223u; m[k] = (uint8_t)(x >> 24); }
        if (it & 1) { memcpy(m, b, len < n ? len : n); m[x % (len ? len : 1)] ^= (uint8_t)(1u << (x & 7)); }
        if (mt7925_fw_patch_parse(m, len, &p) == MT7925_FW_OK)
            for (uint32_t i = 0; i < p.n_sections; i++) CHECK((uint64_t)p.dl[i].file_off + p.dl[i].len <= len);
        if (mt7925_fw_ram_parse(m, len, &r) == MT7925_FW_OK)
            for (uint32_t i = 0; i < r.n_dl; i++) CHECK((uint64_t)r.dl[i].file_off + r.dl[i].len <= len);
        x = x * 1664525u + 1013904223u;
    }
}

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1u << 21, n = 0;
    uint8_t *b = malloc(cap);
    for (size_t got; b && (got = fread(b + n, 1, cap - n, f)) > 0;) {
        n += got;
        if (n == cap) { cap *= 2; b = realloc(b, cap); }
    }
    fclose(f);
    *len = n;
    return b;
}

static void hex(const uint8_t *d, char *out) {
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

/* Raw (decompressed) digests, linux-firmware-mediatek 20240318.git3b128b60-0ubuntu3.1. */
#define PIN_PATCH "089dd0252a7eb95feed55950ad0fd9e6f751a07b4cf22273de722b73fa50d49e"
#define PIN_RAM "7e4ed27d1e9fe21cdefda35a22da32137b2473fada350617fb603489af19346a"

static int real(const char *pp, const char *rp) {
    size_t pl = 0, rl = 0;
    uint8_t *pb = slurp(pp, &pl), *rb = slurp(rp, &rl), d[32];
    char h[65];
    if (!pb || !rb) { printf("MT7925_FW_REAL: FAIL (cannot read %s or %s)\n", pp, rp); return 1; }
    int ok = 1;
    mt7925_fw_patch p;
    mt7925_fw_ram r;
    sha256_hash(pb, pl, d); hex(d, h);
    printf("patch %s bytes=%zu sha256=%s %s\n", pp, pl, h, strcmp(h, PIN_PATCH) ? "PIN-MISMATCH" : "pinned");
    ok &= strcmp(h, PIN_PATCH) == 0;
    mt7925_fw_err e = mt7925_fw_patch_parse(pb, pl, &p);
    printf("patch parse: %s\n", mt7925_fw_strerror(e));
    ok &= e == MT7925_FW_OK;
    if (e == MT7925_FW_OK) {
        printf("patch build_date=%s platform=%s hw_sw_ver=0x%08x patch_ver=0x%08x checksum=0x%04x subsys=0x%08x feature=0x%08x sections=%u\n",
               p.build_date, p.platform, p.hw_sw_ver, p.patch_ver, p.checksum, p.desc_subsys, p.desc_feature, p.n_sections);
        for (uint32_t i = 0; i < p.n_sections; i++)
            printf("  patch dl[%u] cmd=0x%02x addr=0x%08x len=%u mode=0x%08x file_off=%u\n", i, p.dl[i].cmd, p.dl[i].addr,
                   p.dl[i].len, p.dl[i].mode, p.dl[i].file_off);
    }
    sha256_hash(rb, rl, d); hex(d, h);
    printf("ram %s bytes=%zu sha256=%s %s\n", rp, rl, h, strcmp(h, PIN_RAM) ? "PIN-MISMATCH" : "pinned");
    ok &= strcmp(h, PIN_RAM) == 0;
    e = mt7925_fw_ram_parse(rb, rl, &r);
    printf("ram parse: %s\n", mt7925_fw_strerror(e));
    ok &= e == MT7925_FW_OK;
    if (e == MT7925_FW_OK) {
        printf("ram chip_id=0x%02x eco=0x%02x regions=%u format_ver=%u format_flag=0x%02x fw_ver=%s build_date=%s crc=0x%08x\n",
               r.chip_id, r.eco_code, r.n_region, r.format_ver, r.format_flag, r.fw_ver, r.build_date, r.crc);
        for (uint32_t i = 0; i < r.n_region; i++)
            printf("  region[%u] addr=0x%08x len=%u file_off=%u feature=0x%02x type=%u %s\n", i, r.region[i].addr,
                   r.region[i].len, r.region[i].file_off, r.region[i].feature_set, r.region[i].type,
                   r.region[i].download ? "download" : "host-only");
        for (uint32_t i = 0; i < r.n_dl; i++)
            printf("  ram dl[%u] cmd=0x%02x addr=0x%08x len=%u mode=0x%08x\n", i, r.dl[i].cmd, r.dl[i].addr, r.dl[i].len,
                   r.dl[i].mode);
        printf("ram start addr=0x%08x option=0x%x clc_region=%d\n", r.start_addr, r.start_option, r.clc_region);
    }
    free(pb);
    free(rb);
    printf("MT7925_FW_REAL: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "real") == 0)
        return real(argv[2], argv[3]);
    synthetic();
    printf("MT7925_FW_TESTS: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
