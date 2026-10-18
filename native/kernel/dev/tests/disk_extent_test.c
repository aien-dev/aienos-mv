/* disk_extent_test.c -- host tests for the many-piece region (dev/disk_extent.c).
 * An in-memory parent disk whose every block outside the pieces carries a
 * guard pattern; after each run the guards are checked, so a single stray
 * write anywhere fails the test. Each refusal path has a negative case. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "disk.h"
#include "disk_extent.h"
#include "sha256.h"

static int checks, failures;
#define CHECK(c)                                                                 \
    do {                                                                         \
        checks++;                                                                \
        if (!(c)) {                                                              \
            failures++;                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                \
        }                                                                        \
    } while (0)

#define DISK_BLOCKS 4096u
static uint8_t g_disk[DISK_BLOCKS * 512u];
static uint32_t g_max_io = 7;

static int mem_read(void *ctx, uint64_t lba, uint32_t n, uint8_t *b) { (void)ctx; memcpy(b, g_disk + lba * 512u, n * 512u); return 0; }
static int mem_write(void *ctx, uint64_t lba, uint32_t n, const uint8_t *b) { (void)ctx; memcpy(g_disk + lba * 512u, b, n * 512u); return 0; }
static int mem_flush(void *ctx) { (void)ctx; return 0; }
static disk_dev g_parent = {0, 512, DISK_BLOCKS, mem_read, mem_write, mem_flush, 0};

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

typedef struct { uint64_t lg, ph, ln; } pc;
static uint8_t g_map[CK_EXT_HDR_BYTES + 64 * CK_EXT_PIECE_BYTES];

static size_t build(const pc *p, uint32_t n, uint64_t base, uint64_t wf, uint64_t wl)
{
    memset(g_map, 0, sizeof g_map);
    memcpy(g_map, "AXMAP1\0\0", 8);
    put32(g_map + 8, 1);
    put32(g_map + 12, n);
    put64(g_map + 16, base);
    put64(g_map + 24, wf);
    put64(g_map + 32, wl);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *e = g_map + CK_EXT_HDR_BYTES + i * CK_EXT_PIECE_BYTES;
        put64(e, p[i].lg); put64(e + 8, p[i].ph); put64(e + 16, p[i].ln);
    }
    size_t len = CK_EXT_HDR_BYTES + (size_t)n * CK_EXT_PIECE_BYTES;
    {
        sha256_ctx h;
        sha256_init(&h);
        sha256_update(&h, g_map, 40);
        sha256_update(&h, g_map + CK_EXT_HDR_BYTES, len - CK_EXT_HDR_BYTES);
        sha256_final(&h, g_map + 40);
    }
    return len;
}

/* Out of order on disk on purpose; base 100, window 100..4095. */
static const pc good[] = {{0, 900, 5}, {5, 200, 3}, {8, 3000, 40}, {48, 1, 1}, {49, 500, 11}};
#define NGOOD 5u
#define GOOD_BLOCKS 60u

static int in_pieces(uint64_t lba)
{
    for (uint32_t i = 0; i < NGOOD; i++)
        if (lba >= 100 + good[i].ph && lba < 100 + good[i].ph + good[i].ln) return 1;
    return 0;
}
static void guard_fill(void)
{
    for (uint32_t b = 0; b < DISK_BLOCKS; b++) memset(g_disk + b * 512u, in_pieces(b) ? 0 : 0xA5, 512);
}
static int guards_intact(void)
{
    for (uint32_t b = 0; b < DISK_BLOCKS; b++)
        if (!in_pieces(b))
            for (uint32_t i = 0; i < 512; i++)
                if (g_disk[b * 512u + i] != 0xA5) return 0;
    return 1;
}

static int parse(ck_ext_region *r, size_t len)
{
    static ck_ext_piece piece[64];
    static uint32_t order[64];
    return ck_ext_parse(r, &g_parent, g_map, len, piece, order, 64);
}

int main(void)
{
    g_parent.max_blocks_per_io = g_max_io;
    ck_ext_region r;
    uint8_t s[512];

    /* 1. Good map: parse, seal a fresh region, round trip across every piece. */
    guard_fill();
    size_t len = build(good, NGOOD, 100, 100, DISK_BLOCKS - 1);
    CHECK(parse(&r, len) == CK_EXT_OK);
    CHECK(r.blocks == GOOD_BLOCKS);
    CHECK(ck_ext_open(&r, s) == CK_EXT_OK);
    CHECK(memcmp(g_disk + (100 + 900) * 512u, "AIENRGN1", 8) == 0);
    static uint8_t w[GOOD_BLOCKS * 512u], rb[GOOD_BLOCKS * 512u];
    for (size_t i = 0; i < sizeof w; i++) w[i] = (uint8_t)(i * 31u + 7u);
    CHECK(disk_write(&r.dev, 1, GOOD_BLOCKS - 1, w + 512) == DISK_OK);
    CHECK(disk_read(&r.dev, 1, GOOD_BLOCKS - 1, rb + 512) == DISK_OK);
    CHECK(memcmp(w + 512, rb + 512, (GOOD_BLOCKS - 1) * 512u) == 0);
    /* logical 48 lives at absolute 101 */
    CHECK(memcmp(g_disk + 101u * 512u, w + 48u * 512u, 512) == 0);
    CHECK(guards_intact());

    /* 2. Bounds: past the end, straddling the end, block 0 write. */
    CHECK(disk_write(&r.dev, GOOD_BLOCKS, 1, w) != DISK_OK);
    CHECK(disk_write(&r.dev, GOOD_BLOCKS - 2, 3, w) != DISK_OK);
    CHECK(disk_write(&r.dev, 0, 1, w) != DISK_OK);
    CHECK(guards_intact());

    /* 3. Reopen: our seal accepted; a foreign block 0 refused. */
    ck_ext_region r2;
    CHECK(parse(&r2, len) == CK_EXT_OK);
    CHECK(ck_ext_open(&r2, s) == CK_EXT_OK);
    g_disk[(100 + 900) * 512u + 20] ^= 1;
    ck_ext_region r3;
    CHECK(parse(&r3, len) == CK_EXT_OK);
    CHECK(ck_ext_open(&r3, s) == CK_EXT_ESEAL);
    CHECK(disk_read(&r3.dev, 1, 1, rb) != DISK_OK); /* unopened region does no I/O */

    /* 4. Refusals, each one mutation of the good map. */
    pc m[NGOOD];
    memcpy(m, good, sizeof m); m[2].lg = 9;                       /* gap */
    CHECK(parse(&r, build(m, NGOOD, 100, 100, DISK_BLOCKS - 1)) == CK_EXT_EORDER);
    memcpy(m, good, sizeof m); m[1].ph = 902;                     /* overlaps piece 0 on disk */
    CHECK(parse(&r, build(m, NGOOD, 100, 100, DISK_BLOCKS - 1)) == CK_EXT_EOVERLAP);
    memcpy(m, good, sizeof m); m[2].ph = DISK_BLOCKS - 120;       /* runs past the window */
    CHECK(parse(&r, build(m, NGOOD, 100, 100, DISK_BLOCKS - 1)) == CK_EXT_EWINDOW);
    CHECK(parse(&r, build(good, NGOOD, 100, 1100, DISK_BLOCKS - 1)) == CK_EXT_EWINDOW); /* window excludes pieces */
    CHECK(parse(&r, build(good, NGOOD, 100, 100, DISK_BLOCKS)) == CK_EXT_EWINDOW);      /* window past parent */
    memcpy(m, good, sizeof m); m[3].ln = 0;
    CHECK(parse(&r, build(m, NGOOD, 100, 100, DISK_BLOCKS - 1)) == CK_EXT_EORDER);
    len = build(good, NGOOD, 100, 100, DISK_BLOCKS - 1);
    g_map[CK_EXT_HDR_BYTES + 3] ^= 1;                              /* piece edited, digest stale */
    CHECK(parse(&r, len) == CK_EXT_EDIGEST);
    len = build(good, NGOOD, 100, 100, DISK_BLOCKS - 1);
    g_map[16] ^= 1;                                               /* base edited, header digest stale */
    CHECK(parse(&r, len) == CK_EXT_EDIGEST);
    len = build(good, NGOOD, 100, 100, DISK_BLOCKS - 1);
    g_map[0] = 'Z';
    CHECK(parse(&r, len) == CK_EXT_EMAGIC);
    len = build(good, NGOOD, 100, 100, DISK_BLOCKS - 1);
    CHECK(parse(&r, len - 1) == CK_EXT_ECOUNT);
    CHECK(r.valid == 0);
    CHECK(guards_intact());

    printf("disk_extent: %d checks, %d failures\n", checks, failures);
    printf("DISK_EXTENT_HOST: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
