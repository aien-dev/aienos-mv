/* fdt.c -- flattened device tree reader (see fdt.h). Written from the
 * Devicetree Specification v0.4, chapter 5; every multi-byte field is
 * big-endian and read byte by byte (the kernel builds with -mstrict-align). */
#include "fdt.h"

/* TEST-ONLY mutants (apple.mk apple-mutants): host builds only. */
#if (defined(CK_FDT_MUTANT_NAMEOFF) || defined(CK_FDT_MUTANT_RANGES_IDENTITY)) && !defined(CK_APPLE_HOST_TEST)
#error "CK_FDT_MUTANT_* is a host test mutant"
#endif

#define TOK_BEGIN_NODE 1u
#define TOK_END_NODE 2u
#define TOK_PROP 3u
#define TOK_NOP 4u
#define TOK_END 9u

#define HDR_SIZE 40u /* version 17 header: ten u32 fields */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

static uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Length of the string at p if a NUL comes before p + max, else -1. */
static int64_t bounded_len(const uint8_t *p, uint32_t max)
{
    for (uint32_t i = 0; i < max; i++)
        if (p[i] == 0) return i;
    return -1;
}

const char *ck_fdt_strerror(int rc)
{
    switch (rc) {
    case CK_FDT_OK: return "ok";
    case CK_FDT_E_SHORT: return "short";
    case CK_FDT_E_MAGIC: return "magic";
    case CK_FDT_E_VERSION: return "version";
    case CK_FDT_E_LAYOUT: return "layout";
    case CK_FDT_E_TOKEN: return "token";
    case CK_FDT_E_NAME: return "name";
    case CK_FDT_E_PROP: return "prop";
    case CK_FDT_E_DEPTH: return "depth";
    case CK_FDT_E_ROOT: return "root";
    case CK_FDT_E_NOTFOUND: return "notfound";
    case CK_FDT_E_CELLS: return "cells";
    case CK_FDT_E_RANGE: return "range";
    case CK_FDT_E_RSV: return "rsv";
    default: return "unknown";
    }
}

static const uint8_t *st(const struct ck_fdt *f, uint32_t pos) { return f->blob + f->off_struct + pos; }

/* One token at structure offset pos of a CHECKED blob: its kind, and the
 * offset of the token after it. */
static uint32_t step(const struct ck_fdt *f, uint32_t pos, uint32_t *next)
{
    uint32_t tok = be32(st(f, pos));
    uint32_t n = pos + 4;
    if (tok == TOK_BEGIN_NODE) {
        n += align4((uint32_t)bounded_len(st(f, n), f->size_struct - n) + 1u);
    } else if (tok == TOK_PROP) {
        n += 8u + align4(be32(st(f, n)));
    }
    *next = n;
    return tok;
}

static int overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return alen && blen && a < b + blen && b < a + alen;
}

int ck_fdt_open(struct ck_fdt *f, const void *blob, size_t avail)
{
    const uint8_t *b = blob;
    f->blob = 0;
    if (!b || avail < HDR_SIZE) return CK_FDT_E_SHORT;
    if (be32(b) != CK_FDT_MAGIC) return CK_FDT_E_MAGIC;
    uint32_t total = be32(b + 4), off_struct = be32(b + 8), off_strings = be32(b + 12);
    uint32_t off_rsv = be32(b + 16), version = be32(b + 20), last_comp = be32(b + 24);
    uint32_t boot_cpuid = be32(b + 28), size_strings = be32(b + 32), size_struct = be32(b + 36);
    if (version < 17 || last_comp > 17) return CK_FDT_E_VERSION;
    if (total < HDR_SIZE || total > avail) return CK_FDT_E_SHORT;
    if (total > CK_FDT_MAX_SIZE) return CK_FDT_E_LAYOUT;
    if ((uint64_t)off_struct + size_struct > total || (uint64_t)off_strings + size_strings > total)
        return CK_FDT_E_LAYOUT;
    if (off_struct < HDR_SIZE || off_strings < HDR_SIZE || off_rsv < HDR_SIZE) return CK_FDT_E_LAYOUT;
    if ((off_struct & 3u) || (size_struct & 3u) || (off_rsv & 7u) || size_struct < 4u) return CK_FDT_E_LAYOUT;
    if (overlaps(off_struct, size_struct, off_strings, size_strings)) return CK_FDT_E_LAYOUT;

    /* Memory reservation block: 16-byte entries ending with a zero pair. */
    uint32_t rsv_end = 0;
    for (uint64_t p = off_rsv; p + 16u <= total; p += 16u) {
        if (be64(b + p) == 0 && be64(b + p + 8) == 0) {
            rsv_end = (uint32_t)(p + 16u);
            break;
        }
    }
    if (!rsv_end) return CK_FDT_E_RSV;
    if (overlaps(off_rsv, rsv_end - off_rsv, off_struct, size_struct) ||
        overlaps(off_rsv, rsv_end - off_rsv, off_strings, size_strings))
        return CK_FDT_E_LAYOUT;

    /* Structure block: every token checked once. */
    const uint8_t *s = b + off_struct;
    uint32_t pos = 0, depth = 0, nodes = 0, props = 0;
    uint64_t had_child = 0; /* bit d: the node at depth d already has a subnode */
    int root = -1, ended = 0;
    while (!ended) {
        if (pos + 4u > size_struct) return CK_FDT_E_TOKEN;
        uint32_t tok = be32(s + pos);
        uint32_t here = pos;
        pos += 4u;
        switch (tok) {
        case TOK_BEGIN_NODE: {
            int64_t nl = bounded_len(s + pos, size_struct - pos);
            if (nl < 0) return CK_FDT_E_NAME;
            if (depth == 0) {
                if (root >= 0 || nl != 0) return CK_FDT_E_ROOT;
                root = (int)here;
            } else {
                if (nl == 0) return CK_FDT_E_NAME;
                had_child |= 1ull << (depth - 1u);
            }
            if (++depth > CK_FDT_MAX_DEPTH) return CK_FDT_E_DEPTH;
            had_child &= ~(1ull << (depth - 1u));
            uint32_t adv = align4((uint32_t)nl + 1u);
            if (adv > size_struct - pos) return CK_FDT_E_NAME;
            pos += adv;
            nodes++;
            break;
        }
        case TOK_END_NODE:
            if (depth == 0) return CK_FDT_E_DEPTH;
            depth--;
            break;
        case TOK_PROP: {
            if (depth == 0) return CK_FDT_E_PROP;
            if (had_child & (1ull << (depth - 1u))) return CK_FDT_E_PROP; /* property after a subnode */
            if (size_struct - pos < 8u) return CK_FDT_E_PROP;
            uint32_t len = be32(s + pos), nameoff = be32(s + pos + 4);
            pos += 8u;
            if (len > size_struct - pos || align4(len) > size_struct - pos) return CK_FDT_E_PROP;
#ifndef CK_FDT_MUTANT_NAMEOFF
            if (nameoff >= size_strings || bounded_len(b + off_strings + nameoff, size_strings - nameoff) < 0)
                return CK_FDT_E_PROP;
#else
            (void)nameoff;
#endif
            pos += align4(len);
            props++;
            break;
        }
        case TOK_NOP:
            break;
        case TOK_END:
            if (depth != 0) return CK_FDT_E_DEPTH;
            if (root < 0) return CK_FDT_E_ROOT;
            if (pos != size_struct) return CK_FDT_E_TOKEN;
            ended = 1;
            break;
        default:
            return CK_FDT_E_TOKEN;
        }
    }

    f->blob = b;
    f->total = total;
    f->off_struct = off_struct;
    f->size_struct = size_struct;
    f->off_strings = off_strings;
    f->size_strings = size_strings;
    f->off_rsv = off_rsv;
    f->version = version;
    f->boot_cpuid = boot_cpuid;
    f->root = root;
    f->nodes = nodes;
    f->props = props;
    return CK_FDT_OK;
}

static int valid_node(const struct ck_fdt *f, int node)
{
    return f->blob && node >= 0 && (uint32_t)node + 4u <= f->size_struct &&
           be32(st(f, (uint32_t)node)) == TOK_BEGIN_NODE;
}

const char *ck_fdt_name(const struct ck_fdt *f, int node)
{
    return valid_node(f, node) ? (const char *)st(f, (uint32_t)node + 4u) : "";
}

/* Offset just past the node header (first property / subnode token). */
static uint32_t body(const struct ck_fdt *f, int node)
{
    uint32_t next;
    step(f, (uint32_t)node, &next);
    return next;
}

/* Offset just past the END_NODE matching `node`. */
static uint32_t skip_node(const struct ck_fdt *f, int node)
{
    uint32_t pos = body(f, node), depth = 1, next;
    while (depth && pos < f->size_struct) {
        uint32_t tok = step(f, pos, &next);
        if (tok == TOK_BEGIN_NODE) depth++;
        else if (tok == TOK_END_NODE) depth--;
        else if (tok == TOK_END) break;
        pos = next;
    }
    return pos;
}

/* First BEGIN_NODE or END_NODE at or after pos, skipping props and NOPs. */
static uint32_t next_structural(const struct ck_fdt *f, uint32_t pos, uint32_t *tok_out)
{
    uint32_t next;
    while (pos < f->size_struct) {
        uint32_t tok = step(f, pos, &next);
        if (tok == TOK_BEGIN_NODE || tok == TOK_END_NODE || tok == TOK_END) {
            *tok_out = tok;
            return pos;
        }
        pos = next;
    }
    *tok_out = TOK_END;
    return pos;
}

int ck_fdt_first_child(const struct ck_fdt *f, int node)
{
    if (!valid_node(f, node)) return CK_FDT_E_NOTFOUND;
    uint32_t tok, pos = next_structural(f, body(f, node), &tok);
    return tok == TOK_BEGIN_NODE ? (int)pos : CK_FDT_E_NOTFOUND;
}

int ck_fdt_next_sibling(const struct ck_fdt *f, int node)
{
    if (!valid_node(f, node) || node == f->root) return CK_FDT_E_NOTFOUND;
    uint32_t tok, pos = next_structural(f, skip_node(f, node), &tok);
    return tok == TOK_BEGIN_NODE ? (int)pos : CK_FDT_E_NOTFOUND;
}

int ck_fdt_next_node(const struct ck_fdt *f, int node)
{
    if (!f->blob) return CK_FDT_E_NOTFOUND;
    if (node < 0) return f->root;
    if (!valid_node(f, node)) return CK_FDT_E_NOTFOUND;
    uint32_t pos = body(f, node), next;
    while (pos < f->size_struct) {
        uint32_t tok = step(f, pos, &next);
        if (tok == TOK_BEGIN_NODE) return (int)pos;
        if (tok == TOK_END) break;
        pos = next;
    }
    return CK_FDT_E_NOTFOUND;
}

int ck_fdt_parent(const struct ck_fdt *f, int node)
{
    if (!valid_node(f, node) || node == f->root) return CK_FDT_E_NOTFOUND;
    int stack[CK_FDT_MAX_DEPTH];
    uint32_t depth = 0, pos = (uint32_t)f->root, next;
    while (pos < f->size_struct) {
        uint32_t tok = step(f, pos, &next);
        if (tok == TOK_BEGIN_NODE) {
            if ((int)pos == node) return depth ? stack[depth - 1u] : CK_FDT_E_NOTFOUND;
            if (depth >= CK_FDT_MAX_DEPTH) return CK_FDT_E_DEPTH;
            stack[depth++] = (int)pos;
        } else if (tok == TOK_END_NODE) {
            if (!depth) break;
            depth--;
        } else if (tok == TOK_END) {
            break;
        }
        pos = next;
    }
    return CK_FDT_E_NOTFOUND;
}

/* Name `name` matches path component [c, c + len): exactly, or as
 * "<component>@unit" when the component has no '@'. */
static int name_match(const char *name, const char *c, uint32_t len)
{
    int has_at = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (name[i] != c[i]) return 0;
        if (c[i] == '@') has_at = 1;
    }
    return name[len] == 0 || (!has_at && name[len] == '@');
}

int ck_fdt_path(const struct ck_fdt *f, const char *path)
{
    if (!f->blob || !path || path[0] != '/') return CK_FDT_E_NOTFOUND;
    int node = f->root;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        uint32_t len = 0;
        while (p[len] && p[len] != '/') len++;
        int child = ck_fdt_first_child(f, node), found = CK_FDT_E_NOTFOUND;
        while (child >= 0) {
            if (name_match(ck_fdt_name(f, child), p, len)) {
                found = child;
                break;
            }
            child = ck_fdt_next_sibling(f, child);
        }
        if (found < 0) return CK_FDT_E_NOTFOUND;
        node = found;
        p += len;
    }
    return node;
}

const uint8_t *ck_fdt_prop(const struct ck_fdt *f, int node, const char *name, uint32_t *len)
{
    if (!valid_node(f, node) || !name) return 0;
    uint32_t pos = body(f, node), next;
    while (pos < f->size_struct) {
        uint32_t tok = step(f, pos, &next);
        if (tok == TOK_PROP) {
            uint32_t plen = be32(st(f, pos + 4u)), nameoff = be32(st(f, pos + 8u));
            const char *pn = (const char *)(f->blob + f->off_strings + nameoff);
            if (str_eq(pn, name)) {
                if (len) *len = plen;
                return st(f, pos + 12u);
            }
        } else if (tok != TOK_NOP) {
            break; /* properties end at the first subnode or END_NODE */
        }
        pos = next;
    }
    return 0;
}

int ck_fdt_u32(const struct ck_fdt *f, int node, const char *name, uint32_t *out)
{
    uint32_t len;
    const uint8_t *v = ck_fdt_prop(f, node, name, &len);
    if (!v) return CK_FDT_E_NOTFOUND;
    if (len != 4u) return CK_FDT_E_PROP;
    *out = be32(v);
    return CK_FDT_OK;
}

int ck_fdt_u64(const struct ck_fdt *f, int node, const char *name, uint64_t *out)
{
    uint32_t len;
    const uint8_t *v = ck_fdt_prop(f, node, name, &len);
    if (!v) return CK_FDT_E_NOTFOUND;
    if (len == 4u) *out = be32(v);
    else if (len == 8u) *out = be64(v);
    else return CK_FDT_E_PROP;
    return CK_FDT_OK;
}

const char *ck_fdt_string(const struct ck_fdt *f, int node, const char *name)
{
    uint32_t len;
    const uint8_t *v = ck_fdt_prop(f, node, name, &len);
    if (!v || len == 0 || v[len - 1u] != 0 || bounded_len(v, len) != (int64_t)len - 1) return 0;
    return (const char *)v;
}

const char *ck_fdt_strlist_get(const struct ck_fdt *f, int node, const char *name, uint32_t i)
{
    uint32_t len;
    const uint8_t *v = ck_fdt_prop(f, node, name, &len);
    if (!v || len == 0 || v[len - 1u] != 0) return 0;
    uint32_t off = 0, k = 0;
    while (off < len) {
        int64_t sl = bounded_len(v + off, len - off);
        if (sl < 0) return 0;
        if (k == i) return (const char *)(v + off);
        off += (uint32_t)sl + 1u;
        k++;
    }
    return 0;
}

int ck_fdt_strlist_has(const struct ck_fdt *f, int node, const char *name, const char *s, uint32_t *idx)
{
    for (uint32_t i = 0;; i++) {
        const char *e = ck_fdt_strlist_get(f, node, name, i);
        if (!e) return 0;
        if (str_eq(e, s)) {
            if (idx) *idx = i;
            return 1;
        }
    }
}

int ck_fdt_compatible(const struct ck_fdt *f, int node, const char *compat)
{
    return ck_fdt_strlist_has(f, node, "compatible", compat, 0);
}

int ck_fdt_find_compatible(const struct ck_fdt *f, int after, const char *compat)
{
    for (int n = ck_fdt_next_node(f, after); n >= 0; n = ck_fdt_next_node(f, n))
        if (ck_fdt_compatible(f, n, compat)) return n;
    return CK_FDT_E_NOTFOUND;
}

int ck_fdt_by_phandle(const struct ck_fdt *f, uint32_t phandle)
{
    if (phandle == 0 || phandle == 0xffffffffu) return CK_FDT_E_NOTFOUND;
    for (int n = ck_fdt_next_node(f, -1); n >= 0; n = ck_fdt_next_node(f, n)) {
        uint32_t v;
        if ((ck_fdt_u32(f, n, "phandle", &v) == 0 || ck_fdt_u32(f, n, "linux,phandle", &v) == 0) && v == phandle)
            return n;
    }
    return CK_FDT_E_NOTFOUND;
}

/* #address-cells / #size-cells a node gives its children (spec 2.3.5
 * defaults 2 and 1). Address 1..2 and size 0..2 cells are supported. */
static int bus_cells(const struct ck_fdt *f, int bus, uint32_t *ac, uint32_t *sc)
{
    *ac = 2;
    *sc = 1;
    if (ck_fdt_prop(f, bus, "#address-cells", 0) && ck_fdt_u32(f, bus, "#address-cells", ac) != 0)
        return CK_FDT_E_CELLS;
    if (ck_fdt_prop(f, bus, "#size-cells", 0) && ck_fdt_u32(f, bus, "#size-cells", sc) != 0)
        return CK_FDT_E_CELLS;
    if (*ac < 1u || *ac > 2u || *sc > 2u) return CK_FDT_E_CELLS;
    return CK_FDT_OK;
}

static uint64_t cells(const uint8_t *p, uint32_t n)
{
    return n == 2u ? be64(p) : n == 1u ? be32(p) : 0;
}

int ck_fdt_reg(const struct ck_fdt *f, int node, uint32_t idx, uint64_t *addr, uint64_t *size)
{
    int parent = ck_fdt_parent(f, node);
    if (parent < 0) return CK_FDT_E_NOTFOUND;
    uint32_t ac, sc, len;
    int rc = bus_cells(f, parent, &ac, &sc);
    if (rc) return rc;
    const uint8_t *v = ck_fdt_prop(f, node, "reg", &len);
    if (!v) return CK_FDT_E_NOTFOUND;
    uint32_t entry = (ac + sc) * 4u;
    if (len % entry) return CK_FDT_E_PROP;
    if (idx >= len / entry) return CK_FDT_E_RANGE;
    v += idx * entry;
    *addr = cells(v, ac);
    *size = cells(v + ac * 4u, sc);
    return CK_FDT_OK;
}

/* Translate [*addr, *addr + size) on bus `bus` into the bus's parent
 * address space through bus's `ranges`. */
static int translate_up(const struct ck_fdt *f, int bus, uint64_t *addr, uint64_t size)
{
    int up = ck_fdt_parent(f, bus);
    if (up < 0) return CK_FDT_E_RANGE;
    uint32_t len, cac, csc, pac, psc;
    const uint8_t *r = ck_fdt_prop(f, bus, "ranges", &len);
    if (!r) return CK_FDT_E_RANGE; /* no ranges: not memory-mapped from above */
#ifdef CK_FDT_MUTANT_RANGES_IDENTITY
    len = 0;
#endif
    if (len == 0) return CK_FDT_OK; /* identity */
    int rc = bus_cells(f, bus, &cac, &csc);
    if (rc) return rc;
    rc = bus_cells(f, up, &pac, &psc);
    if (rc) return rc;
    if (csc == 0) return CK_FDT_E_CELLS;
    uint32_t entry = (cac + pac + csc) * 4u;
    if (len % entry) return CK_FDT_E_PROP;
    for (uint32_t off = 0; off < len; off += entry) {
        uint64_t child = cells(r + off, cac), parent = cells(r + off + cac * 4u, pac);
        uint64_t rlen = cells(r + off + (cac + pac) * 4u, csc);
        if (*addr >= child && *addr - child < rlen && size <= rlen - (*addr - child)) {
            *addr = *addr - child + parent;
            return CK_FDT_OK;
        }
    }
    return CK_FDT_E_RANGE;
}

int ck_fdt_reg_cpu(const struct ck_fdt *f, int node, uint32_t idx, uint64_t *addr, uint64_t *size)
{
    int rc = ck_fdt_reg(f, node, idx, addr, size);
    if (rc) return rc;
    int bus = ck_fdt_parent(f, node);
    for (uint32_t hops = 0; bus >= 0 && bus != f->root; hops++) {
        if (hops >= CK_FDT_MAX_DEPTH) return CK_FDT_E_DEPTH;
        rc = translate_up(f, bus, addr, *size);
        if (rc) return rc;
        bus = ck_fdt_parent(f, bus);
    }
    return CK_FDT_OK;
}

int ck_fdt_reg_named(const struct ck_fdt *f, int node, const char *rname, uint64_t *addr, uint64_t *size)
{
    uint32_t idx;
    if (!ck_fdt_strlist_has(f, node, "reg-names", rname, &idx)) return CK_FDT_E_NOTFOUND;
    return ck_fdt_reg_cpu(f, node, idx, addr, size);
}

int ck_fdt_rsv(const struct ck_fdt *f, uint32_t i, uint64_t *addr, uint64_t *size)
{
    if (!f->blob) return CK_FDT_E_NOTFOUND;
    /* Walk from entry 0: nothing after the terminating zero pair counts. */
    for (uint64_t k = 0, p = f->off_rsv; p + 16u <= f->total; k++, p += 16u) {
        uint64_t a = be64(f->blob + p), s = be64(f->blob + p + 8);
        if (a == 0 && s == 0) break;
        if (k == i) {
            *addr = a;
            *size = s;
            return CK_FDT_OK;
        }
    }
    return CK_FDT_E_NOTFOUND;
}
