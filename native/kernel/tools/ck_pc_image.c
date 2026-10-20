/* ck_pc_image.c -- the AienOS PC edition image writer (host tool, never part
 * of an image). Every output is a pure function of its inputs: no clock, no
 * random number, no host path. Identifiers that formats call "unique" (GPT
 * disk and partition GUIDs, VHDX file/data/page-83 GUIDs) are derived from
 * SHA-256 of the input bytes, so two builds of one commit are byte-identical
 * and two different builds get different identifiers.
 *
 *   ck_pc_image disk ESP OUT MIB          GPT disk of MIB MiB: the ESP image as partition 1
 *                                         (EFI system), an empty AIENOS partition 2 after it
 *   ck_pc_image iso ESP README OUT EPOCH  hybrid live image: ISO 9660 + El Torito (UEFI,
 *                                         no emulation) for a DVD, and the same GPT layout in
 *                                         the ISO system area for a USB stick written byte
 *                                         for byte; both point at the one ESP image
 *   ck_pc_image vhdx RAW OUT              dynamic VHDX (Hyper-V), 1 MiB blocks, zero blocks
 *                                         left out
 *   ck_pc_image qcow2 RAW OUT             qcow2 version 3 (QEMU), 64 KiB clusters, zero
 *                                         clusters left out
 *   ck_pc_image check-gpt IMG             the kernel's own GPT parser (dev/disk_part.c) on IMG
 *
 * Formats, from the public specifications: UEFI 2.10 section 5.3 (GPT,
 * protective MBR); ECMA-119 4th edition (ISO 9660) sections 8.4, 9.1, 9.4;
 * El Torito 1.0 sections 2.0-2.4 (boot record, validation entry, initial
 * entry, platform 0xEF = EFI); [MS-VHDX] v20240423 sections 2.1-2.6; QEMU
 * docs/interop/qcow2.txt (version 3). Readers used only as independent
 * checks in scripts/pc_edition_build.sh: qemu-img (vhdx, qcow2), sgdisk
 * (GPT), xorriso (ISO). */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "disk.h"
#include "disk_file.h"
#include "disk_part.h"
#include "sha256.h"

uint32_t sv1_crc32c(const uint8_t *bytes, size_t len); /* store/store_v1.c */

#define MIB (1024u * 1024u)

static void die(const char *m)
{
    fprintf(stderr, "ck_pc_image: %s\n", m);
    exit(1);
}

static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void p64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void b16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void b32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (24 - 8 * i));
}
static void b64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i));
}
static void both16(uint8_t *p, uint16_t v) { p16(p, v), b16(p + 2, v); }
static void both32(uint8_t *p, uint32_t v) { p32(p, v), b32(p + 4, v); }

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open an input file");
    if (fseek(f, 0, SEEK_END) != 0) die("seek");
    long n = ftell(f);
    if (n < 0) die("tell");
    rewind(f);
    uint8_t *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) die("read");
    fclose(f);
    *len = (size_t)n;
    return b;
}

static int create(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("cannot create the output file");
    return fd;
}
static void pw(int fd, const void *b, size_t n, uint64_t off)
{
    const uint8_t *p = b;
    while (n) {
        ssize_t w = pwrite(fd, p, n, (off_t)off);
        if (w <= 0) die("write failed");
        p += w, n -= (size_t)w, off += (uint64_t)w;
    }
}
static void finish(int fd, uint64_t size)
{
    if (ftruncate(fd, (off_t)size) != 0 || fsync(fd) != 0 || close(fd) != 0) die("cannot finish the output file");
}

/* A GUID from SHA-256(label || seed), version 4 and RFC 4122 variant bits set
 * (on-disk mixed-endian order: the version nibble is the high nibble of byte 7). */
static void guid_from(const char *label, const uint8_t seed[32], uint8_t out[16])
{
    sha256_ctx c;
    uint8_t d[32];
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)label, strlen(label));
    sha256_update(&c, seed, 32);
    sha256_final(&c, d);
    memcpy(out, d, 16);
    out[7] = (uint8_t)((out[7] & 0x0f) | 0x40);
    out[8] = (uint8_t)((out[8] & 0x3f) | 0x80);
}
static void sha(const uint8_t *b, size_t n, uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, b, n);
    sha256_final(&c, out);
}

/* ---- GPT (512-byte blocks) ---- */
static const uint8_t esp_type[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                     0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};

struct layout {
    uint64_t blocks;               /* 512-byte blocks in the image */
    uint64_t esp_first, esp_last;  /* inclusive */
    uint64_t aien_first, aien_last;
};

static void gpt_write(int fd, const struct layout *L, const uint8_t seed[32])
{
    uint8_t mbr[512] = {0}, ent[128 * 128] = {0}, h[512] = {0};
    uint64_t last = L->blocks - 1;
    uint8_t *r = mbr + 446;
    r[2] = 0x02, r[4] = 0xee, r[5] = r[6] = r[7] = 0xff;
    p32(r + 8, 1);
    p32(r + 12, last > 0xffffffffu ? 0xffffffffu : (uint32_t)last);
    mbr[510] = 0x55, mbr[511] = 0xaa;
    pw(fd, mbr, 512, 0);

    static const char *const names[2] = {"EFI system", "AIENOS"};
    const uint8_t *types[2] = {esp_type, ck_aienos_part_type};
    uint64_t f[2] = {L->esp_first, L->aien_first}, l[2] = {L->esp_last, L->aien_last};
    for (int i = 0; i < 2; i++) {
        uint8_t *e = ent + 128 * i;
        memcpy(e, types[i], 16);
        guid_from(i ? "AIENOS-PC-GPT-PART-2" : "AIENOS-PC-GPT-PART-1", seed, e + 16);
        p64(e + 32, f[i]);
        p64(e + 40, l[i]);
        for (size_t c = 0; names[i][c]; c++) e[56 + 2 * c] = (uint8_t)names[i][c];
    }
    uint32_t ecrc = ck_crc32(0, ent, sizeof ent);
    for (int k = 0; k < 2; k++) {
        uint64_t my = k ? last : 1, alt = k ? 1 : last, elba = k ? last - 32 : 2;
        memset(h, 0, sizeof h);
        memcpy(h, "EFI PART", 8);
        p32(h + 8, 0x00010000u);
        p32(h + 12, 92);
        p64(h + 24, my);
        p64(h + 32, alt);
        p64(h + 40, 34);
        p64(h + 48, last - 33);
        guid_from("AIENOS-PC-GPT-DISK", seed, h + 56);
        p64(h + 72, elba);
        p32(h + 80, 128);
        p32(h + 84, 128);
        p32(h + 88, ecrc);
        p32(h + 16, ck_crc32(0, h, 92));
        pw(fd, ent, sizeof ent, elba * 512);
        pw(fd, h, 512, my * 512);
    }
}

static void layout_for(struct layout *L, uint64_t esp_bytes, uint64_t aien_bytes, uint64_t total_bytes)
{
    L->blocks = total_bytes / 512;
    L->esp_first = 2048;
    L->esp_last = L->esp_first + esp_bytes / 512 - 1;
    L->aien_first = (L->esp_last + 1 + 2047) / 2048 * 2048;
    L->aien_last = aien_bytes ? L->aien_first + aien_bytes / 512 - 1 : L->blocks - 34;
    if (L->aien_last > L->blocks - 34 || L->aien_first > L->aien_last) die("image too small for the layout");
}

static int cmd_disk(const char *esp_path, const char *out, unsigned mib)
{
    size_t n;
    uint8_t *esp = slurp(esp_path, &n);
    if (!n || n % MIB) die("the ESP image must be a whole number of MiB");
    if ((uint64_t)mib * MIB < n + 4u * MIB) die("disk too small");
    uint8_t seed[32];
    sha(esp, n, seed);
    struct layout L;
    layout_for(&L, n, 0, (uint64_t)mib * MIB);
    /* AIENOS ends on a 1 MiB boundary before the backup GPT. */
    L.aien_last = (L.blocks - 34 + 1) / 2048 * 2048 - 1;
    int fd = create(out);
    if (ftruncate(fd, (off_t)L.blocks * 512) != 0) die("size");
    gpt_write(fd, &L, seed);
    pw(fd, esp, n, L.esp_first * 512);
    finish(fd, L.blocks * 512);
    printf("disk: %s mib=%u esp_lba=%llu-%llu aienos_lba=%llu-%llu\n", out, mib, (unsigned long long)L.esp_first,
           (unsigned long long)L.esp_last, (unsigned long long)L.aien_first, (unsigned long long)L.aien_last);
    free(esp);
    return 0;
}

/* ---- hybrid ISO ---- */
#define S 2048u
static void achars(uint8_t *p, size_t w, const char *s)
{
    memset(p, ' ', w);
    memcpy(p, s, strlen(s) < w ? strlen(s) : w);
}
static void dec_date(uint8_t *p, time_t t) /* ECMA-119 8.4.26.1: 16 digits + GMT offset */
{
    struct tm g;
    gmtime_r(&t, &g);
    char b[80];
    snprintf(b, sizeof b, "%04d%02d%02d%02d%02d%02d00", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour,
             g.tm_min, g.tm_sec);
    memcpy(p, b, 16);
    p[16] = 0;
}
static void rec_date(uint8_t *p, time_t t) /* ECMA-119 9.1.5 */
{
    struct tm g;
    gmtime_r(&t, &g);
    p[0] = (uint8_t)g.tm_year, p[1] = (uint8_t)(g.tm_mon + 1), p[2] = (uint8_t)g.tm_mday;
    p[3] = (uint8_t)g.tm_hour, p[4] = (uint8_t)g.tm_min, p[5] = (uint8_t)g.tm_sec, p[6] = 0;
}
static size_t dirrec(uint8_t *p, uint32_t lba, uint32_t len, int dir, const char *name, size_t nlen, time_t t)
{
    size_t rl = 33 + nlen + (nlen % 2 == 0 ? 1 : 0);
    memset(p, 0, rl);
    p[0] = (uint8_t)rl;
    both32(p + 2, lba);
    both32(p + 10, len);
    rec_date(p + 18, t);
    p[25] = dir ? 2 : 0;
    both16(p + 28, 1);
    p[32] = (uint8_t)nlen;
    memcpy(p + 33, name, nlen);
    return rl;
}

static int cmd_iso(const char *esp_path, const char *readme_path, const char *out, long long epoch)
{
    size_t n, rn;
    uint8_t *esp = slurp(esp_path, &n);
    uint8_t *readme = slurp(readme_path, &rn);
    if (!n || n % MIB || n / 512 > 0xffff) die("the ESP image must be whole MiB and at most 31 MiB (El Torito count)");
    if (rn > 8 * S) die("README too long");
    time_t t = (time_t)epoch;
    uint8_t seed[32];
    sha(esp, n, seed);
    struct layout L;
    uint64_t total = MIB + n + 4u * MIB + MIB; /* ESP at 1 MiB, AIENOS 4 MiB, 1 MiB tail holds the backup GPT */
    layout_for(&L, n, 4u * MIB, total);
    uint32_t sectors = (uint32_t)(total / S), esp_lba = (uint32_t)(L.esp_first * 512 / S);
    enum { PVD = 16, BOOTREC = 17, TERM = 18, CAT = 19, LPT = 20, MPT = 21, ROOT = 22, README = 23 };

    int fd = create(out);
    if (ftruncate(fd, (off_t)total) != 0) die("size");
    gpt_write(fd, &L, seed);
    uint8_t sec[S];

    /* root directory: ".", "..", EFIBOOT.IMG;1, README.TXT;1 (sorted) */
    memset(sec, 0, S);
    size_t o = 0;
    o += dirrec(sec + o, ROOT, S, 1, "\0", 1, t);
    o += dirrec(sec + o, ROOT, S, 1, "\1", 1, t);
    o += dirrec(sec + o, esp_lba, (uint32_t)n, 0, "EFIBOOT.IMG;1", 13, t);
    o += dirrec(sec + o, README, (uint32_t)rn, 0, "README.TXT;1", 12, t);
    pw(fd, sec, S, (uint64_t)ROOT * S);
    pw(fd, readme, rn, (uint64_t)README * S);

    /* path tables: the root alone */
    memset(sec, 0, S);
    sec[0] = 1;
    p32(sec + 2, ROOT);
    p16(sec + 6, 1);
    pw(fd, sec, S, (uint64_t)LPT * S);
    memset(sec, 0, S);
    sec[0] = 1;
    b32(sec + 2, ROOT);
    b16(sec + 6, 1);
    pw(fd, sec, S, (uint64_t)MPT * S);

    /* primary volume descriptor */
    memset(sec, 0, S);
    sec[0] = 1;
    memcpy(sec + 1, "CD001", 5);
    sec[6] = 1;
    achars(sec + 8, 32, "");
    achars(sec + 40, 32, "AIENOS_PC");
    both32(sec + 80, sectors);
    both16(sec + 120, 1);
    both16(sec + 124, 1);
    both16(sec + 128, S);
    both32(sec + 132, 10);
    p32(sec + 140, LPT);
    b32(sec + 148, MPT);
    dirrec(sec + 156, ROOT, S, 1, "\0", 1, t);
    achars(sec + 190, 128, "");
    achars(sec + 318, 128, "AIENOS");
    achars(sec + 446, 128, "CK_PC_IMAGE");
    achars(sec + 574, 128, "AIENOS PC EDITION LIVE IMAGE");
    achars(sec + 702, 37, "");
    achars(sec + 739, 37, "");
    achars(sec + 776, 37, "");
    dec_date(sec + 813, t);
    dec_date(sec + 830, t);
    memset(sec + 847, '0', 16);
    dec_date(sec + 864, t);
    sec[881] = 1;
    pw(fd, sec, S, (uint64_t)PVD * S);

    /* El Torito boot record */
    memset(sec, 0, S);
    memcpy(sec + 1, "CD001", 5);
    sec[6] = 1;
    memcpy(sec + 7, "EL TORITO SPECIFICATION", 23);
    p32(sec + 71, CAT);
    pw(fd, sec, S, (uint64_t)BOOTREC * S);

    /* terminator */
    memset(sec, 0, S);
    sec[0] = 255;
    memcpy(sec + 1, "CD001", 5);
    sec[6] = 1;
    pw(fd, sec, S, (uint64_t)TERM * S);

    /* boot catalog: validation entry (platform 0xEF, EFI) and the initial entry */
    memset(sec, 0, S);
    sec[0] = 1;
    sec[1] = 0xef;
    memcpy(sec + 4, "AIENOS", 6);
    sec[30] = 0x55, sec[31] = 0xaa;
    uint16_t sum = 0;
    for (int i = 0; i < 32; i += 2) sum = (uint16_t)(sum + (sec[i] | sec[i + 1] << 8));
    p16(sec + 28, (uint16_t)(0x10000u - sum));
    sec[32] = 0x88; /* bootable, no emulation */
    p16(sec + 38, (uint16_t)(n / 512));
    p32(sec + 40, esp_lba);
    pw(fd, sec, S, (uint64_t)CAT * S);

    pw(fd, esp, n, L.esp_first * 512);
    finish(fd, total);
    printf("iso: %s bytes=%llu sectors=%u esp_sector=%u esp_lba512=%llu aienos_lba512=%llu-%llu\n", out,
           (unsigned long long)total, sectors, esp_lba, (unsigned long long)L.esp_first,
           (unsigned long long)L.aien_first, (unsigned long long)L.aien_last);
    free(esp), free(readme);
    return 0;
}

/* ---- VHDX (dynamic) ---- */
static const uint8_t g_bat[16] = {0x66, 0x77, 0xc2, 0x2d, 0x23, 0xf6, 0x00, 0x42,
                                  0x9d, 0x64, 0x11, 0x5e, 0x9b, 0xfd, 0x4a, 0x08};
static const uint8_t g_meta[16] = {0x06, 0xa2, 0x7c, 0x8b, 0x90, 0x47, 0x9a, 0x4b,
                                   0xb8, 0xfe, 0x57, 0x5f, 0x05, 0x0f, 0x88, 0x6e};
static const uint8_t g_fparam[16] = {0x37, 0x67, 0xa1, 0xca, 0x36, 0xfa, 0x43, 0x4d,
                                     0xb3, 0xb6, 0x33, 0xf0, 0xaa, 0x44, 0xe7, 0x6b};
static const uint8_t g_vsize[16] = {0x24, 0x42, 0xa5, 0x2f, 0x1b, 0xcd, 0x76, 0x48,
                                    0xb2, 0x11, 0x5d, 0xbe, 0xd8, 0x3b, 0xf4, 0xb8};
static const uint8_t g_page83[16] = {0xab, 0x12, 0xca, 0xbe, 0xe6, 0xb2, 0x23, 0x45,
                                     0x93, 0xef, 0xc3, 0x09, 0xe0, 0x00, 0xc7, 0x46};
static const uint8_t g_lss[16] = {0x1d, 0xbf, 0x41, 0x81, 0x6f, 0xa9, 0x09, 0x47,
                                  0xba, 0x47, 0xf2, 0x33, 0xa8, 0xfa, 0xab, 0x5f};
static const uint8_t g_pss[16] = {0xc7, 0x48, 0xa3, 0xcd, 0x5d, 0x44, 0x71, 0x44,
                                  0x9c, 0xc9, 0xe9, 0x88, 0x52, 0x51, 0xc5, 0x56};

static int all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i]) return 0;
    return 1;
}

static int cmd_vhdx(const char *raw_path, const char *out)
{
    size_t n;
    uint8_t *raw = slurp(raw_path, &n);
    const uint32_t bs = MIB, chunk = (uint32_t)((1ull << 23) * 512 / bs); /* [MS-VHDX] 2.2.4.1: chunk ratio */
    if (!n || n % bs) die("the raw disk must be a whole number of MiB");
    uint8_t seed[32];
    sha(raw, n, seed);
    uint64_t blocks = n / bs, entries = blocks + (blocks - 1) / chunk;
    uint64_t bat_off = 3u * MIB, bat_len = (entries * 8 + MIB - 1) / MIB * MIB, data_off = bat_off + bat_len;
    int fd = create(out);
    uint8_t *buf = calloc(1, 320u * 1024); /* identifier, two headers, two region tables: 5 x 64 KiB */
    if (!buf) die("memory");

    /* file type identifier */
    memcpy(buf, "vhdxfile", 8);
    const char *creator = "AienOS ck_pc_image";
    for (size_t i = 0; creator[i]; i++) buf[8 + 2 * i] = (uint8_t)creator[i];
    /* headers at 64 KiB and 128 KiB */
    for (int k = 0; k < 2; k++) {
        uint8_t *h = buf + 64u * 1024 * (1 + k);
        memcpy(h, "head", 4);
        p64(h + 8, (uint64_t)k + 1);
        guid_from("AIENOS-PC-VHDX-FILE-WRITE", seed, h + 16);
        guid_from("AIENOS-PC-VHDX-DATA-WRITE", seed, h + 32);
        p16(h + 64, 0);       /* LogVersion */
        p16(h + 66, 1);       /* Version */
        p32(h + 68, MIB);     /* LogLength */
        p64(h + 72, MIB);     /* LogOffset */
        p32(h + 4, sv1_crc32c(h, 4096));
    }
    /* region tables at 192 KiB and 256 KiB */
    for (int k = 0; k < 2; k++) {
        uint8_t *r = buf + 64u * 1024 * (3 + k);
        memcpy(r, "regi", 4);
        p32(r + 8, 2);
        memcpy(r + 16, g_bat, 16);
        p64(r + 32, bat_off);
        p32(r + 40, (uint32_t)bat_len);
        p32(r + 44, 1);
        memcpy(r + 48, g_meta, 16);
        p64(r + 64, 2u * MIB);
        p32(r + 72, MIB);
        p32(r + 76, 1);
        p32(r + 4, sv1_crc32c(r, 64u * 1024));
    }
    pw(fd, buf, 256u * 1024 + 64u * 1024, 0);
    free(buf);

    /* metadata region at 2 MiB (the log at 1 MiB stays zero: LogGuid zero, nothing to replay) */
    uint8_t *m = calloc(1, MIB);
    if (!m) die("memory");
    memcpy(m, "metadata", 8);
    p16(m + 10, 5);
    const uint8_t *ids[5] = {g_fparam, g_vsize, g_page83, g_lss, g_pss};
    const uint32_t offs[5] = {0x10000, 0x10008, 0x10010, 0x10020, 0x10024}, lens[5] = {8, 8, 16, 4, 4};
    const uint32_t flags[5] = {4, 6, 6, 6, 6}; /* IsRequired; IsVirtualDisk|IsRequired */
    for (int i = 0; i < 5; i++) {
        uint8_t *e = m + 32 + 32 * i;
        memcpy(e, ids[i], 16);
        p32(e + 16, offs[i]);
        p32(e + 20, lens[i]);
        p32(e + 24, flags[i]);
    }
    p32(m + 0x10000, bs);
    p32(m + 0x10004, 0);
    p64(m + 0x10008, n);
    guid_from("AIENOS-PC-VHDX-PAGE83", seed, m + 0x10010);
    p32(m + 0x10020, 512);
    p32(m + 0x10024, 4096);
    pw(fd, m, MIB, 2u * MIB);
    free(m);

    /* BAT and payload blocks: a block of zeros is left NOT_PRESENT (reads as zeros) */
    uint8_t *bat = calloc(1, bat_len);
    if (!bat) die("memory");
    uint64_t at = data_off, present = 0;
    for (uint64_t b = 0; b < blocks; b++) {
        uint64_t idx = b + b / chunk;
        if (all_zero(raw + b * bs, bs)) continue;
        pw(fd, raw + b * bs, bs, at);
        p64(bat + 8 * idx, (at / MIB) << 20 | 6u); /* PAYLOAD_BLOCK_FULLY_PRESENT */
        at += bs;
        present++;
    }
    pw(fd, bat, bat_len, bat_off);
    finish(fd, at);
    printf("vhdx: %s virtual_bytes=%zu block=%u blocks=%llu present=%llu file_bytes=%llu\n", out, n, bs,
           (unsigned long long)blocks, (unsigned long long)present, (unsigned long long)at);
    free(bat), free(raw);
    return 0;
}

/* ---- qcow2 version 3 ---- */
static int cmd_qcow2(const char *raw_path, const char *out)
{
    size_t n;
    uint8_t *raw = slurp(raw_path, &n);
    const uint64_t cs = 65536, l2n = cs / 8;
    if (!n || n % cs) die("the raw disk must be a whole number of 64 KiB clusters");
    uint64_t clusters = n / cs, l1n = (clusters + l2n - 1) / l2n;
    if (l1n * 8 > cs) die("disk too large for one L1 cluster");
    uint8_t *used_l2 = calloc(l1n, 1);
    uint64_t data = 0;
    for (uint64_t c = 0; c < clusters; c++)
        if (!all_zero(raw + c * cs, cs)) used_l2[c / l2n] = 1, data++;
    uint64_t nl2 = 0;
    for (uint64_t i = 0; i < l1n; i++) nl2 += used_l2[i];
    /* clusters: 0 header, 1 refcount table, 2 refcount block, 3 L1, then L2 tables, then data */
    uint64_t total = 4 + nl2 + data;
    if (total > cs / 2) die("file too large for one refcount block");
    int fd = create(out);
    uint8_t *hdr = calloc(1, cs), *rt = calloc(1, cs), *rb = calloc(1, cs), *l1 = calloc(1, cs);
    uint8_t *l2 = calloc(nl2 ? nl2 : 1, cs);
    if (!hdr || !rt || !rb || !l1 || !l2 || !used_l2) die("memory");
    b32(hdr, 0x514649fbu);
    b32(hdr + 4, 3);
    b32(hdr + 20, 16);        /* cluster_bits */
    b64(hdr + 24, n);         /* size */
    b32(hdr + 36, (uint32_t)l1n);
    b64(hdr + 40, 3 * cs);    /* l1_table_offset */
    b64(hdr + 48, 1 * cs);    /* refcount_table_offset */
    b32(hdr + 56, 1);         /* refcount_table_clusters */
    b32(hdr + 96, 4);         /* refcount_order: 16-bit refcounts */
    b32(hdr + 100, 104);      /* header_length; the end-of-extensions marker at 104 is zero */
    b64(rt, 2 * cs);
    uint64_t next_l2 = 4, next_data = 4 + nl2, k = 0;
    for (uint64_t i = 0; i < l1n; i++) {
        if (!used_l2[i]) continue;
        uint8_t *t = l2 + k++ * cs;
        b64(l1 + 8 * i, (next_l2 * cs) | 1ull << 63); /* QCOW_OFLAG_COPIED: refcount 1 */
        for (uint64_t j = 0; j < l2n; j++) {
            uint64_t c = i * l2n + j;
            if (c >= clusters || all_zero(raw + c * cs, cs)) continue;
            b64(t + 8 * j, (next_data * cs) | 1ull << 63);
            pw(fd, raw + c * cs, cs, next_data * cs);
            next_data++;
        }
        pw(fd, t, cs, next_l2 * cs);
        next_l2++;
    }
    for (uint64_t c = 0; c < total; c++) b16(rb + 2 * c, 1);
    pw(fd, hdr, cs, 0);
    pw(fd, rt, cs, cs);
    pw(fd, rb, cs, 2 * cs);
    pw(fd, l1, cs, 3 * cs);
    finish(fd, total * cs);
    printf("qcow2: %s virtual_bytes=%zu clusters_used=%llu file_bytes=%llu\n", out, n, (unsigned long long)total,
           (unsigned long long)(total * cs));
    free(hdr), free(rt), free(rb), free(l1), free(l2), free(used_l2), free(raw);
    return 0;
}

/* ---- the kernel's own GPT parser on an image ---- */
static int cmd_check_gpt(const char *img)
{
    disk_file df;
    disk_dev d;
    if (disk_file_open(&df, &d, img, 512, 0, 0) != DISK_OK) die("cannot open the image as a 512-byte disk");
    ck_part p;
    ck_gpt_info info;
    int rc = ck_gpt_find_aienos(&d, &p, &info);
    if (rc != CK_GPT_OK) {
        printf("check-gpt: %s FAIL %s\n", img, ck_gpt_strerror(rc));
        return 1;
    }
    printf("check-gpt: %s PASS used=%u aienos first_lba=%llu blocks=%llu\n", img, info.used,
           (unsigned long long)p.first_lba, (unsigned long long)p.blocks);
    disk_file_close(&df);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "disk")) return cmd_disk(argv[2], argv[3], (unsigned)strtoul(argv[4], 0, 10));
    if (argc == 6 && !strcmp(argv[1], "iso")) return cmd_iso(argv[2], argv[3], argv[4], strtoll(argv[5], 0, 10));
    if (argc == 4 && !strcmp(argv[1], "vhdx")) return cmd_vhdx(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "qcow2")) return cmd_qcow2(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "check-gpt")) return cmd_check_gpt(argv[2]);
    fprintf(stderr, "usage: ck_pc_image disk ESP OUT MIB | iso ESP README OUT EPOCH | vhdx RAW OUT | qcow2 RAW OUT | "
                    "check-gpt IMG\n");
    return 2;
}
