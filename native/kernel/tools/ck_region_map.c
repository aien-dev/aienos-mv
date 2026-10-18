/* ck_region_map.c -- write REGION.MAP (dev/disk_extent.h) for a locked,
 * fully written region file on a Linux host. Asks the filesystem for the
 * file's pieces with FS_IOC_FIEMAP (linux/fiemap.h), refuses any piece the
 * kernel flags as not yet on disk (unknown, delayed, unwritten, encoded,
 * inline, shared), and takes the host partition's start and size from sysfs.
 *
 *   ck_region_map FILE PARTITION OUT     e.g. region.img nvme0n1p2 REGION.MAP
 *
 * Read only on FILE; writes only OUT. The file must be immutable (chattr +i)
 * so the pieces cannot move after the map is made. */
#include <errno.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "sha256.h"
#include "disk_extent.h"

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static int sys_u64(const char *part, const char *leaf, uint64_t *v)
{
    char path[256], buf[64];
    snprintf(path, sizeof path, "/sys/class/block/%s/%s", part, leaf);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = fgets(buf, sizeof buf, f) != 0;
    fclose(f);
    if (!ok) return -1;
    *v = strtoull(buf, 0, 10);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s FILE PARTITION OUT\n", argv[0]);
        return 2;
    }
    uint64_t pstart = 0, psize = 0;
    if (sys_u64(argv[2], "start", &pstart) || sys_u64(argv[2], "size", &psize) || !psize) {
        fprintf(stderr, "region_map: cannot read start/size of %s from sysfs\n", argv[2]);
        return 1;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror(argv[1]); return 1; }
    int attr = 0;
    if (ioctl(fd, FS_IOC_GETFLAGS, &attr) || !(attr & FS_IMMUTABLE_FL)) {
        fprintf(stderr, "region_map: %s is not immutable (chattr +i first)\n", argv[1]);
        return 1;
    }
    size_t cap = CK_EXT_MAX;
    struct fiemap *fm = calloc(1, sizeof *fm + cap * sizeof(struct fiemap_extent));
    uint8_t *map = calloc(1, CK_EXT_HDR_BYTES + cap * CK_EXT_PIECE_BYTES);
    if (!fm || !map) return 1;
    fm->fm_start = 0;
    fm->fm_length = FIEMAP_MAX_OFFSET;
    fm->fm_flags = FIEMAP_FLAG_SYNC;
    fm->fm_extent_count = (uint32_t)cap;
    if (ioctl(fd, FS_IOC_FIEMAP, fm)) { perror("FS_IOC_FIEMAP"); return 1; }
    uint32_t n = fm->fm_mapped_extents;
    if (n == 0 || n >= cap) { fprintf(stderr, "region_map: %u pieces (limit %zu)\n", n, cap - 1); return 1; }
    if (!(fm->fm_extents[n - 1].fe_flags & FIEMAP_EXTENT_LAST)) { fprintf(stderr, "region_map: piece list incomplete\n"); return 1; }
    const uint32_t bad = FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_ENCODED |
                         FIEMAP_EXTENT_DATA_ENCRYPTED | FIEMAP_EXTENT_NOT_ALIGNED | FIEMAP_EXTENT_DATA_INLINE |
                         FIEMAP_EXTENT_DATA_TAIL | FIEMAP_EXTENT_UNWRITTEN | FIEMAP_EXTENT_SHARED;
    uint64_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        const struct fiemap_extent *e = &fm->fm_extents[i];
        if ((e->fe_flags & bad) || (e->fe_logical | e->fe_physical | e->fe_length) % 512u) {
            fprintf(stderr, "region_map: piece %u not plainly on disk (flags 0x%x)\n", i, e->fe_flags);
            return 1;
        }
        uint8_t *p = map + CK_EXT_HDR_BYTES + (size_t)i * CK_EXT_PIECE_BYTES;
        put64(p, e->fe_logical / 512u);
        put64(p + 8, e->fe_physical / 512u);
        put64(p + 16, e->fe_length / 512u);
        total += e->fe_length;
    }
    memcpy(map, "AXMAP1\0\0", 8);
    put32(map + 8, 1);
    put32(map + 12, n);
    put64(map + 16, pstart);
    put64(map + 24, pstart);
    put64(map + 32, pstart + psize - 1);
    size_t len = CK_EXT_HDR_BYTES + (size_t)n * CK_EXT_PIECE_BYTES;
    {
        sha256_ctx h;
        sha256_init(&h);
        sha256_update(&h, map, 40);
        sha256_update(&h, map + CK_EXT_HDR_BYTES, len - CK_EXT_HDR_BYTES);
        sha256_final(&h, map + 40);
    }
    FILE *o = fopen(argv[3], "wb");
    if (!o || fwrite(map, 1, len, o) != len || fclose(o)) { perror(argv[3]); return 1; }
    printf("REGION_MAP: pieces=%u bytes=%llu base=%llu window=%llu..%llu out=%s size=%zu\n", n,
           (unsigned long long)total, (unsigned long long)pstart, (unsigned long long)pstart,
           (unsigned long long)(pstart + psize - 1), argv[3], len);
    return 0;
}
