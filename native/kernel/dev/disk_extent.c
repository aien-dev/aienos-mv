/* disk_extent.c -- see disk_extent.h. Every check runs before the region is
 * marked valid; a refused map leaves r->valid == 0 and no disk_dev. */
#include "disk_extent.h"
#include "../../argus/sha256.h"

static const uint8_t map_magic[8] = {'A', 'X', 'M', 'A', 'P', '1', 0, 0};
static const uint8_t seal_magic[8] = {'A', 'I', 'E', 'N', 'R', 'G', 'N', '1'};

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* Heap sort of order[0..n) by piece[].abs (no allocation, n log n). */
static void sift(const ck_ext_piece *pc, uint32_t *o, uint32_t i, uint32_t n)
{
    for (;;) {
        uint32_t big = i, l = 2 * i + 1, r = l + 1;
        if (l < n && pc[o[l]].abs > pc[o[big]].abs) big = l;
        if (r < n && pc[o[r]].abs > pc[o[big]].abs) big = r;
        if (big == i) return;
        uint32_t t = o[i]; o[i] = o[big]; o[big] = t;
        i = big;
    }
}
static void sort_abs(const ck_ext_piece *pc, uint32_t *o, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) o[i] = i;
    for (uint32_t i = n / 2; i-- > 0;) sift(pc, o, i, n);
    for (uint32_t end = n; end-- > 1;) {
        uint32_t t = o[0]; o[0] = o[end]; o[end] = t;
        sift(pc, o, 0, end);
    }
}

static int ext_read(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf);
static int ext_write(void *ctx, uint64_t lba, uint32_t count, const uint8_t *buf);
static int ext_flush(void *ctx);

int ck_ext_parse(ck_ext_region *r, const disk_dev *parent, const uint8_t *map, size_t len,
                 ck_ext_piece *piece, uint32_t *order, uint32_t cap)
{
    if (!r || !parent || !map || !piece || !order) return DISK_EARG;
    r->valid = 0;
    r->parent = parent;
    r->piece = piece;
    r->count = 0;
    r->blocks = 0;
    if (parent->block_size != DISK_SECTOR_BYTES) return CK_EXT_EGEOM;
    if (len < CK_EXT_HDR_BYTES) return CK_EXT_ECOUNT;
    for (int i = 0; i < 8; i++)
        if (map[i] != map_magic[i]) return CK_EXT_EMAGIC;
    if (le32(map + 8) != 1u) return CK_EXT_EMAGIC;
    uint32_t n = le32(map + 12);
    if (n == 0 || n > CK_EXT_MAX || n > cap) return CK_EXT_ECOUNT;
    if ((size_t)n * CK_EXT_PIECE_BYTES != len - CK_EXT_HDR_BYTES) return CK_EXT_ECOUNT;
    uint64_t base = le64(map + 16), wfirst = le64(map + 24), wlast = le64(map + 32);
    if (wfirst > wlast || wlast >= parent->block_count) return CK_EXT_EWINDOW; /* GUARD:ext-window */

    const uint8_t *arr = map + CK_EXT_HDR_BYTES;
    {
        sha256_ctx h;
        sha256_init(&h);
        sha256_update(&h, map, 40);
        sha256_update(&h, arr, (size_t)n * CK_EXT_PIECE_BYTES);
        sha256_final(&h, r->digest);
    }
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(r->digest[i] ^ map[40 + i]);
    if (diff) return CK_EXT_EDIGEST;

    uint64_t next = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = arr + (size_t)i * CK_EXT_PIECE_BYTES;
        uint64_t lg = le64(e), ph = le64(e + 8), ln = le64(e + 16);
        if (ln == 0 || lg != next) return CK_EXT_EORDER;                 /* GUARD:ext-gapfree */
        if (lg + ln < lg) return CK_EXT_EORDER;
        uint64_t a = base + ph;
        if (a < base || a < wfirst || a > wlast || ln - 1 > wlast - a) return CK_EXT_EWINDOW; /* GUARD:ext-piece */
        piece[i].logical = lg;
        piece[i].abs = a;
        piece[i].length = ln;
        next = lg + ln;
    }
    sort_abs(piece, order, n);
    for (uint32_t i = 1; i < n; i++) {
        const ck_ext_piece *lo = &piece[order[i - 1]], *hi = &piece[order[i]];
        if (lo->abs + lo->length > hi->abs) return CK_EXT_EOVERLAP;      /* GUARD:ext-disjoint */
    }
    r->count = n;
    r->blocks = next;
    r->dev.ctx = r;
    r->dev.block_size = DISK_SECTOR_BYTES;
    r->dev.block_count = next;
    r->dev.read = ext_read;
    r->dev.write = ext_write;
    r->dev.flush = ext_flush;
    r->dev.max_blocks_per_io = parent->max_blocks_per_io;
    return CK_EXT_OK;
}

int ck_ext_xlate(const ck_ext_region *r, uint64_t lba, uint64_t *abs, uint64_t *left)
{
    if (!r || !r->piece || r->count == 0 || !abs || !left) return DISK_ESTATE;
    if (lba >= r->blocks) return DISK_ERANGE;
    uint32_t lo = 0, hi = r->count; /* last piece with logical <= lba */
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (r->piece[mid].logical <= lba) lo = mid; else hi = mid;
    }
    const ck_ext_piece *p = &r->piece[lo];
    uint64_t off = lba - p->logical;
    if (off >= p->length) return DISK_ERANGE;
    *abs = p->abs + off;
    *left = p->length - off;
    return DISK_OK;
}

/* Split a request at piece boundaries; each part goes to the parent. */
static int ext_io(ck_ext_region *r, uint64_t lba, uint32_t count, uint8_t *rd, const uint8_t *wr)
{
    if (!r || !r->parent || r->count == 0) return DISK_ESTATE;
    if (count == 0) return DISK_EARG;
    if (lba >= r->blocks || (uint64_t)count > r->blocks - lba) return DISK_ERANGE; /* second line: disk_read/disk_write check block_count first */
    while (count) {
        uint64_t abs = 0, left = 0;
        int rc = ck_ext_xlate(r, lba, &abs, &left);
        if (rc) return rc;
        uint32_t n = left < count ? (uint32_t)left : count;
        rc = rd ? disk_read(r->parent, abs, n, rd) : disk_write(r->parent, abs, n, wr);
        if (rc) return rc;
        lba += n;
        count -= n;
        if (rd) rd += (size_t)n * DISK_SECTOR_BYTES; else wr += (size_t)n * DISK_SECTOR_BYTES;
    }
    return DISK_OK;
}
static int ext_read(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf)
{
    ck_ext_region *r = ctx;
    if (!r || !r->valid) return DISK_ESTATE;
    return ext_io(r, lba, count, buf, 0);
}
static int ext_write(void *ctx, uint64_t lba, uint32_t count, const uint8_t *buf)
{
    ck_ext_region *r = ctx;
    if (!r || !r->valid) return DISK_ESTATE;
    if (lba == 0) return DISK_ERANGE; /* GUARD:ext-seal: block 0 is the seal, written only by open */
    return ext_io(r, lba, count, 0, buf);
}
static int ext_flush(void *ctx)
{
    ck_ext_region *r = ctx;
    if (!r || !r->valid || !r->parent) return DISK_ESTATE;
    return disk_flush(r->parent);
}

static void make_seal(const ck_ext_region *r, uint8_t s[512])
{
    for (int i = 0; i < 512; i++) s[i] = 0;
    for (int i = 0; i < 8; i++) s[i] = seal_magic[i];
    for (int i = 0; i < 32; i++) s[8 + i] = r->digest[i];
    put64(s + 40, r->blocks);
}

int ck_ext_open(ck_ext_region *r, uint8_t scratch[512])
{
    if (!r || !scratch || r->count == 0 || r->valid) return DISK_ESTATE;
    int rc = ext_io(r, 0, 1, scratch, 0);
    if (rc) return rc;
    uint8_t any = 0;
    for (int i = 0; i < 512; i++) any |= scratch[i];
    uint8_t want[512];
    make_seal(r, want);
    if (!any) {
        rc = ext_io(r, 0, 1, 0, want);
        if (!rc) rc = disk_flush(r->parent);
        if (!rc) rc = ext_io(r, 0, 1, scratch, 0);
        if (rc) return rc;
    }
    uint8_t diff = 0;
    for (int i = 0; i < 512; i++) diff |= (uint8_t)(scratch[i] ^ want[i]);
    if (diff) return CK_EXT_ESEAL;                                       /* GUARD:ext-sealmatch */
    r->valid = 1;
    return CK_EXT_OK;
}

const char *ck_ext_strerror(int rc)
{
    switch (rc) {
    case CK_EXT_OK: return "ok";
    case CK_EXT_EMAGIC: return "region map magic or version";
    case CK_EXT_ECOUNT: return "region map piece count";
    case CK_EXT_EDIGEST: return "region map digest mismatch";
    case CK_EXT_EORDER: return "region map gap or overlap";
    case CK_EXT_EWINDOW: return "region piece outside its window";
    case CK_EXT_EOVERLAP: return "region pieces overlap on disk";
    case CK_EXT_ESEAL: return "region seal does not match this map";
    case CK_EXT_EGEOM: return "region parent block size";
    default: return "disk error";
    }
}
