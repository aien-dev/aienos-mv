/* fdt.h -- flattened device tree (DTB) reader for the Apple platform path.
 *
 * Format: Devicetree Specification v0.4 (devicetree.org), chapter 5
 * "Flattened Devicetree (DTB) Format": a big-endian header, a memory
 * reservation block, a structure block of 32-bit tokens and a strings block.
 * Written from the specification; no libfdt code.
 *
 * ck_fdt_open checks the WHOLE blob once (header bounds, every token, every
 * name and property inside its block, every property-name offset inside the
 * strings block, nesting depth, one root, a closing FDT_END). Every other
 * call trusts that check and still bounds every read. Node handles are byte
 * offsets of an FDT_BEGIN_NODE token inside the structure block; a negative
 * value is a CK_FDT_E_* code. Pure C, no allocation, no libc: the same file
 * builds freestanding into the kernel and on the host for tests. */
#ifndef AIENOS_CK_APPLE_FDT_H
#define AIENOS_CK_APPLE_FDT_H
#include <stddef.h>
#include <stdint.h>

#define CK_FDT_MAGIC 0xd00dfeedu
#define CK_FDT_MAX_DEPTH 32      /* nesting refused beyond this */
#define CK_FDT_MAX_SIZE (8u << 20) /* totalsize refused beyond 8 MiB */

#define CK_FDT_OK 0
#define CK_FDT_E_SHORT (-1)      /* buffer smaller than the header or totalsize */
#define CK_FDT_E_MAGIC (-2)      /* not 0xd00dfeed */
#define CK_FDT_E_VERSION (-3)    /* last_comp_version > 17 or version < 16 */
#define CK_FDT_E_LAYOUT (-4)     /* a block outside totalsize, misaligned or overlapping */
#define CK_FDT_E_TOKEN (-5)      /* unknown token or token past the structure block */
#define CK_FDT_E_NAME (-6)       /* node name or string not terminated in its block */
#define CK_FDT_E_PROP (-7)       /* property value past the block or name offset bad */
#define CK_FDT_E_DEPTH (-8)      /* nesting deeper than CK_FDT_MAX_DEPTH or unbalanced */
#define CK_FDT_E_ROOT (-9)       /* not exactly one root node */
#define CK_FDT_E_NOTFOUND (-10)  /* node or property absent */
#define CK_FDT_E_CELLS (-11)     /* #address-cells / #size-cells outside 1..2 (or 0 size) */
#define CK_FDT_E_RANGE (-12)     /* reg index past the property, or address not translatable */
#define CK_FDT_E_RSV (-13)       /* memory reservation block not terminated */

struct ck_fdt {
    const uint8_t *blob;
    uint32_t total;
    uint32_t off_struct, size_struct;
    uint32_t off_strings, size_strings;
    uint32_t off_rsv;
    uint32_t version;
    uint32_t boot_cpuid;
    int root;                    /* node offset of "/" */
    uint32_t nodes, props;       /* counted by ck_fdt_open */
};

/* Plain name of a CK_FDT_E_* code ("short", "magic", ...). */
const char *ck_fdt_strerror(int rc);

/* Checks the blob ([blob, blob + avail)) and fills *f. */
int ck_fdt_open(struct ck_fdt *f, const void *blob, size_t avail);

/* Structure walk. Each returns a node offset or CK_FDT_E_NOTFOUND. */
int ck_fdt_first_child(const struct ck_fdt *f, int node);
int ck_fdt_next_sibling(const struct ck_fdt *f, int node);
int ck_fdt_parent(const struct ck_fdt *f, int node);
/* Next node after `node` in document order (node < 0: the root). */
int ck_fdt_next_node(const struct ck_fdt *f, int node);
const char *ck_fdt_name(const struct ck_fdt *f, int node); /* "" for the root */

/* Absolute path ("/cpus/cpu@0"). A component without '@' also matches a
 * node whose name is that component followed by "@unit". */
int ck_fdt_path(const struct ck_fdt *f, const char *path);

/* Property value (NULL if absent); *len set when non-NULL. */
const uint8_t *ck_fdt_prop(const struct ck_fdt *f, int node, const char *name, uint32_t *len);
/* Property as one big-endian u32 (exactly 4 bytes) or u64 (4 or 8 bytes). */
int ck_fdt_u32(const struct ck_fdt *f, int node, const char *name, uint32_t *out);
int ck_fdt_u64(const struct ck_fdt *f, int node, const char *name, uint64_t *out);
/* NUL-terminated string property (the whole value one string) or NULL. */
const char *ck_fdt_string(const struct ck_fdt *f, int node, const char *name);
/* 1 if the string list property `name` holds `s`; index via *idx if found. */
int ck_fdt_strlist_has(const struct ck_fdt *f, int node, const char *name, const char *s, uint32_t *idx);
/* i-th string of a string list property, or NULL. */
const char *ck_fdt_strlist_get(const struct ck_fdt *f, int node, const char *name, uint32_t i);
int ck_fdt_compatible(const struct ck_fdt *f, int node, const char *compat);
/* Next node after `after` (or from the root when after < 0) with `compat`. */
int ck_fdt_find_compatible(const struct ck_fdt *f, int after, const char *compat);
int ck_fdt_by_phandle(const struct ck_fdt *f, uint32_t phandle);

/* reg entry `idx` of `node` decoded with its parent's #address-cells and
 * #size-cells (each 1 or 2), untranslated (bus address). */
int ck_fdt_reg(const struct ck_fdt *f, int node, uint32_t idx, uint64_t *addr, uint64_t *size);
/* Same entry translated to a CPU physical address through every ancestor's
 * `ranges` (empty ranges = identity; a missing ranges on a bus = refused). */
int ck_fdt_reg_cpu(const struct ck_fdt *f, int node, uint32_t idx, uint64_t *addr, uint64_t *size);
/* reg entry picked by its reg-names entry. */
int ck_fdt_reg_named(const struct ck_fdt *f, int node, const char *rname, uint64_t *addr, uint64_t *size);

/* Memory reservation block entry i: 0 ok, CK_FDT_E_NOTFOUND past the end. */
int ck_fdt_rsv(const struct ck_fdt *f, uint32_t i, uint64_t *addr, uint64_t *size);

#endif
