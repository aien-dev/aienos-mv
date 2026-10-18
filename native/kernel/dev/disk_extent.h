/* disk_extent.h -- an AIENOS region made of many pieces of a shared disk.
 *
 * On a disk whose partition table belongs to another system, the owner can
 * reserve a region as one locked, preallocated file of that system; the
 * file's pieces (extents) are scattered over the disk. This layer turns the
 * piece list into one linear disk_dev: logical block 0 .. blocks-1 maps,
 * piece by piece, to absolute LBAs of the parent namespace. Nothing outside
 * the listed pieces is ever read or written; a request that crosses a piece
 * boundary is split; a list with a gap, an overlap, a piece outside the
 * declared window or outside the parent is refused whole (fail closed).
 *
 * Map (on the boot disk as \EFI\AIENOS\REGION.MAP, little endian):
 *   0   magic   "AXMAP1\0\0"
 *   8   u32     version (1)
 *   12  u32     count of pieces (1 .. CK_EXT_MAX)
 *   16  u64     base: absolute LBA the piece offsets are relative to
 *               (start of the host partition holding the file)
 *   24  u64     window_first, 32 u64 window_last: absolute LBAs every piece
 *               must stay inside (the host partition)
 *   40  u8[32]  SHA-256 of bytes 0..39 then the piece array (header bound too)
 *   72  pieces: count x { u64 logical, u64 physical (relative to base), u64 length }
 * all in 512-byte units, sorted by logical, logical coverage gap free from 0.
 *
 * Seal (logical block 0 of the region, 512 bytes): "AIENRGN1", the map's
 * piece SHA-256, the region length. A zero block 0 is a fresh region and
 * is sealed on first open; any other content that is not this map's seal
 * refuses the region (the pieces moved or a different map was supplied). */
#ifndef AIENOS_CK_DISK_EXTENT_H
#define AIENOS_CK_DISK_EXTENT_H

#include <stddef.h>
#include <stdint.h>
#include "disk.h"

#define CK_EXT_MAX 16384u
#define CK_EXT_HDR_BYTES 72u
#define CK_EXT_PIECE_BYTES 24u

enum {
    CK_EXT_OK = 0,
    CK_EXT_EMAGIC = -501,   /* map magic / version */
    CK_EXT_ECOUNT = -502,   /* zero or too many pieces, or short map */
    CK_EXT_EDIGEST = -503,  /* piece array SHA-256 mismatch */
    CK_EXT_EORDER = -504,   /* gap or overlap in logical coverage */
    CK_EXT_EWINDOW = -505,  /* a piece outside the window or the parent */
    CK_EXT_EOVERLAP = -506, /* two pieces share physical blocks */
    CK_EXT_ESEAL = -507,    /* block 0 holds something that is not our seal */
    CK_EXT_EGEOM = -508,    /* parent block size is not 512 */
};

typedef struct {
    uint64_t logical, abs, length; /* abs = base + physical, checked */
} ck_ext_piece;

typedef struct {
    const disk_dev *parent;
    ck_ext_piece *piece;  /* caller storage, count entries */
    uint32_t count;
    uint64_t blocks;      /* region length */
    uint8_t digest[32];
    int valid;
    disk_dev dev;         /* the linear region; use only when valid */
} ck_ext_region;

/* Parse and check a map (bytes, len) against the parent, filling piece[]
 * (room for cap entries; order[] is scratch of the same size) and r. Does not touch the disk. */
int ck_ext_parse(ck_ext_region *r, const disk_dev *parent, const uint8_t *map, size_t len,
                 ck_ext_piece *piece, uint32_t *order, uint32_t cap);
/* Read block 0: seal a zero block, accept our seal, refuse anything else. */
int ck_ext_open(ck_ext_region *r, uint8_t scratch[512]);
/* One logical run -> absolute LBA and the blocks left in that piece. */
int ck_ext_xlate(const ck_ext_region *r, uint64_t lba, uint64_t *abs, uint64_t *left);
const char *ck_ext_strerror(int rc);

#endif
