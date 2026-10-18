/* ck_region_probe.c -- prove a REGION.MAP on a Linux host: for each logical
 * block named on the command line, read it through FILE (the filesystem's
 * view) and through dev/disk_extent.c over the raw DISK opened read only,
 * and require byte equality. The translator is the same code the kernel
 * links; the raw disk is never written.
 *
 *   ck_region_probe MAP FILE DISK LBA...   (LBA in 512-byte logical blocks) */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <unistd.h>
#include "disk.h"
#include "disk_extent.h"

static int g_raw = -1;
static int raw_read(void *ctx, uint64_t lba, uint32_t n, uint8_t *b)
{
    (void)ctx;
    return pread(g_raw, b, (size_t)n * 512u, (off_t)(lba * 512u)) == (ssize_t)(n * 512u) ? 0 : DISK_EIO;
}
static int raw_write(void *ctx, uint64_t lba, uint32_t n, const uint8_t *b) { (void)ctx; (void)lba; (void)n; (void)b; return DISK_EIO; }
static int raw_flush(void *ctx) { (void)ctx; return 0; }

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: %s MAP FILE DISK LBA...\n", argv[0]); return 2; }
    FILE *m = fopen(argv[1], "rb");
    if (!m) { perror(argv[1]); return 1; }
    static uint8_t map[CK_EXT_HDR_BYTES + CK_EXT_MAX * CK_EXT_PIECE_BYTES];
    size_t len = fread(map, 1, sizeof map, m);
    fclose(m);
    int file = open(argv[2], O_RDONLY);
    g_raw = open(argv[3], O_RDONLY);
    if (file < 0 || g_raw < 0) { perror("open"); return 1; }
    uint64_t bytes = 0;
    if (ioctl(g_raw, BLKGETSIZE64, &bytes)) { perror("BLKGETSIZE64"); return 1; }
    disk_dev parent = {0, 512, bytes / 512u, raw_read, raw_write, raw_flush, 256};
    static ck_ext_piece piece[CK_EXT_MAX];
    static uint32_t order[CK_EXT_MAX];
    ck_ext_region r;
    int rc = ck_ext_parse(&r, &parent, map, len, piece, order, CK_EXT_MAX);
    if (rc) { printf("REGION_PROBE: FAIL parse %s\n", ck_ext_strerror(rc)); return 1; }
    int bad = 0;
    for (int i = 4; i < argc; i++) {
        uint64_t lba = strtoull(argv[i], 0, 10), abs = 0, left = 0;
        uint8_t a[512], b[512];
        rc = ck_ext_xlate(&r, lba, &abs, &left);
        if (rc || raw_read(0, abs, 1, a) || pread(file, b, 512, (off_t)(lba * 512u)) != 512) {
            printf("  lba %llu: io error\n", (unsigned long long)lba);
            bad++;
            continue;
        }
        if (memcmp(a, b, 512)) { printf("  lba %llu -> %llu: MISMATCH\n", (unsigned long long)lba, (unsigned long long)abs); bad++; }
    }
    printf("REGION_PROBE: %s pieces=%u blocks=%llu checked=%d mismatches=%d\n", bad ? "FAIL" : "PASS", r.count,
           (unsigned long long)r.blocks, argc - 4, bad);
    return bad ? 1 : 0;
}
