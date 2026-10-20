//! Chip model backend: the register file, the virtio-pci transport behaviour
//! keyed by register roles, the protocol rules, and the device side of the
//! virtq-split rings, all from the contract. The model implements the
//! driver's ops table, so any driver with that API (the generated one or the
//! hand-written reference) runs against it. Every access the contract does
//! not allow is counted as a violation with a message; tests require zero.

use crate::contract::{Access, Contract, Dir};
use crate::driver::banner;
use std::fmt::Write as _;

pub fn header(c: &Contract) -> String {
    let p = &c.prefix;
    let nq = c.rings.len();
    let mut h = banner(c, "/*", "*/");
    let _ = write!(
        h,
        "/* Chip model of {dev}: register file + virtio-pci transport + virtq-split device side.
 * Hosted only (libc). ctx of {p}_model_ops is a {p}_model. */
#ifndef AIENOS_{UP}_MODEL_H
#define AIENOS_{UP}_MODEL_H
#include <stddef.h>
#include <stdint.h>
#include \"aienos_virtio_net.h\"

#define {UP}_MODEL_NQ {nq}
#define {UP}_MODEL_WIRE 8

typedef struct {{
    uint16_t last_avail, used_idx;
}} {p}_model_q;

typedef struct {p}_model {{
    uint8_t *mem; uint64_t dma; size_t mem_len;
    uint32_t reg[{nregs}][2];          /* register file, [register][bank] */
    uint64_t dev_features, drv_features;
    uint8_t config[64];
    uint16_t numq, qmax[{nq}];
    uint32_t mult;
    uint8_t cfggen;
    {p}_model_q q[{nq}];
    /* knobs */
    int refuse_features, reset_stuck, cfggen_unstable, preenabled;
    /* observations */
    unsigned violations, accesses, notifies[{nq}];
    char last_violation[200];
    uint8_t wire[{UP}_MODEL_WIRE][2048]; size_t wire_len[{UP}_MODEL_WIRE]; unsigned nwire;
    /* register trace: one line per access when set */
    void (*trace)(void *tctx, const char *line);
    void *tctx;
}} {p}_model;

extern const {p}_ops {p}_model_ops_template;
/* Power-on: knobs keep their values; registers take their reset values. */
void {p}_model_init({p}_model *m, uint8_t *mem, uint64_t dma, size_t mem_len);
{p}_ops {p}_model_ops({p}_model *m);
/* Device consumes every pending TX buffer (completes with len 0); returns how many. */
unsigned {p}_model_tx({p}_model *m);
/* Device receives one frame into the next RX buffer; 1 done, 0 no buffer. */
int {p}_model_rx({p}_model *m, const uint8_t *frame, size_t n);
/* Hostile device: write a used entry as-is / move used idx. */
void {p}_model_push_used({p}_model *m, unsigned q, uint32_t id, uint32_t len);
void {p}_model_bump_used({p}_model *m, unsigned q, uint16_t delta);
uint32_t {p}_model_status(const {p}_model *m);
/* Contract checks for fixtures: does this function match, are these windows enough? */
int {p}_model_matches(uint16_t vendor, uint16_t device, uint16_t subsys);
int {p}_model_caps_ok(const virtio_pci_caps *caps);
#endif
",
        dev = c.device,
        UP = c.up(),
        nregs = c.regs.len(),
    );
    h
}

fn role_idx(c: &Contract, role: &str) -> usize {
    c.regs
        .iter()
        .position(|r| r.role.as_deref() == Some(role))
        .expect("checked")
}

pub fn source(c: &Contract) -> String {
    let p = &c.prefix;
    let up = c.up();
    let nq = c.rings.len();
    let mut o = banner(c, "/*", "*/");
    o += "#include \"vnet_model.h\"\n\n#include <stdio.h>\n#include <string.h>\n\n";
    // register table
    o += "typedef struct { const char *name; uint8_t win, width, access, count; uint32_t off; int8_t bank; uint8_t has_reset; uint32_t reset; } mreg;\n";
    o += "enum { A_RO = 0, A_RW = 1, A_WO = 2 };\n";
    o += "static const char *const win_name[] = {";
    o += &c
        .windows
        .iter()
        .map(|w| format!("\"{}\"", w.name))
        .collect::<Vec<_>>()
        .join(", ");
    o += "};\nstatic const mreg regs[] = {\n";
    for r in &c.regs {
        let w = c.window(&r.window).unwrap();
        let bank = r
            .bank
            .as_ref()
            .map(|b| c.regs.iter().position(|x| &x.name == b).unwrap() as i64)
            .unwrap_or(-1);
        let acc = match r.access {
            Access::Ro => "A_RO",
            Access::Rw => "A_RW",
            Access::Wo => "A_WO",
        };
        let _ = writeln!(
            o,
            "    {{\"{}\", {}, {}, {acc}, {}, 0x{:x}, {bank}, {}, 0x{:x}}},",
            r.name,
            w.index,
            r.width,
            r.count,
            r.offset,
            r.reset.is_some() as u8,
            r.reset.unwrap_or(0)
        );
    }
    o += "};\n#define NREGS (sizeof regs / sizeof regs[0])\n";
    let roles = [
        "status", "cfggen", "numq", "offer", "accept", "qsize", "qenable", "qnoff", "qdesc",
        "qavail", "qused", "config",
    ];
    for r in roles {
        let _ = writeln!(o, "#define R_{} {}", r.to_uppercase(), role_idx(c, r));
    }
    let db = c.doorbell();
    let status = c.role("status");
    let sbits = c.bits_of(&status.name).unwrap();
    for (n, v) in &sbits.bits {
        let _ = writeln!(o, "#define M_{n} 0x{:x}u", 1u32 << v);
    }
    let (fr, fbit) = c.failmark.as_ref().unwrap();
    let _ = fr;
    // rules
    o += "typedef struct { int reg; int write; uint32_t requires, lacks; } mrule;\nstatic const mrule rules[] = {\n";
    let mask = |b: &[String]| -> String {
        if b.is_empty() {
            "0".into()
        } else {
            b.iter()
                .map(|x| format!("M_{x}"))
                .collect::<Vec<_>>()
                .join(" | ")
        }
    };
    let mut db_rule = None;
    for r in &c.rules {
        if r.reg == db.name {
            db_rule = Some(r);
            continue;
        }
        let i = c.regs.iter().position(|x| x.name == r.reg).unwrap();
        let _ = writeln!(
            o,
            "    {{{i}, {}, {}, {}}},",
            r.write as u8,
            mask(&r.requires),
            mask(&r.lacks)
        );
    }
    o += "    {-1, 0, 0, 0}\n};\n";
    let (db_req, db_lacks) = db_rule
        .map(|r| (mask(&r.requires), mask(&r.lacks)))
        .unwrap_or(("0".into(), "0".into()));
    // ring direction per queue
    o += "static const int ring_device_writes[] = {";
    o += &c
        .rings
        .iter()
        .map(|r| if r.dir == Dir::DeviceWrites { "1" } else { "0" })
        .collect::<Vec<_>>()
        .join(", ");
    let buf = c.konst(&c.rings[0].buf).unwrap().value;
    let hdr = c.konst(&c.rings[0].hdr).unwrap().value;
    let _ = writeln!(
        o,
        "}};\n#define BUF_LEN {buf}u\n#define HDR_LEN {hdr}u\n#define FAILBIT M_{fbit}"
    );
    // matches + caps
    let _ = writeln!(
        o,
        "\nint {p}_model_matches(uint16_t vendor, uint16_t device, uint16_t subsys)\n{{"
    );
    for m in &c.matches {
        let sub = m
            .subsys
            .map(|s| format!(" && subsys == {s}"))
            .unwrap_or_default();
        let _ = writeln!(
            o,
            "    if (vendor == 0x{:04x} && device == 0x{:04x}{sub}) return 1;",
            m.vendor, m.device
        );
    }
    o += "    (void)subsys;\n    return 0;\n}\n\n";
    let _ = write!(
        o,
        "int {p}_model_caps_ok(const virtio_pci_caps *caps)\n{{\n    return caps"
    );
    for w in &c.windows {
        let f = match w.cap_type {
            1 => "common_cfg",
            2 => "notify_cfg",
            3 => "isr_cfg",
            _ => "device_cfg",
        };
        let _ = write!(o, " && caps->{f}.present");
        if w.min > 0 {
            let _ = write!(o, " && caps->{f}.length >= {}", w.min);
        }
        if w.multiplier {
            o += " && caps->has_notify_off_multiplier";
        }
    }
    o += ";\n}\n";
    let _ = write!(
        o,
        r#"
static void violate({p}_model *m, const char *what)
{{
    m->violations++;
    snprintf(m->last_violation, sizeof m->last_violation, "%s", what);
}}
static void tr({p}_model *m, char k, unsigned w, uint32_t off, unsigned width, uint32_t v)
{{
    m->accesses++;
    if (!m->trace) return;
    char line[96];
    snprintf(line, sizeof line, "%c %s+0x%02x/%u 0x%x", k, w < sizeof win_name / sizeof win_name[0] ? win_name[w] : "?", off, width, v);
    m->trace(m->tctx, line);
}}

uint32_t {p}_model_status(const {p}_model *m) {{ return m->reg[R_STATUS][0]; }}

static void power_on({p}_model *m)
{{
    for (unsigned i = 0; i < NREGS; i++)
        for (unsigned b = 0; b < 2; b++) m->reg[i][b] = regs[i].has_reset ? regs[i].reset : 0;
    for (unsigned q = 0; q < {nq} && q < 2; q++) {{
        m->reg[R_QSIZE][q] = m->qmax[q];
        m->reg[R_QNOFF][q] = q;
        m->q[q].last_avail = m->q[q].used_idx = 0;
    }}
    if (m->preenabled) m->reg[R_QENABLE][0] = 1;
    m->drv_features = 0;
}}

void {p}_model_init({p}_model *m, uint8_t *mem, uint64_t dma, size_t mem_len)
{{
    m->mem = mem; m->dma = dma; m->mem_len = mem_len;
    power_on(m);
}}

/* the register at (w, off) with this width, or -1; *el = element index */
static int lookup(unsigned w, uint32_t off, unsigned width, uint32_t *el)
{{
    for (unsigned i = 0; i < NREGS; i++) {{
        const mreg *r = &regs[i];
        if (r->win != w || off < r->off || off >= r->off + (uint32_t)r->width * r->count) continue;
        if (width != r->width || (off - r->off) % r->width) return -1;
        *el = (off - r->off) / r->width;
        return (int)i;
    }}
    return -1;
}}
static unsigned bank_of({p}_model *m, int i, int *valid)
{{
    *valid = 1;
    if (regs[i].bank < 0) return 0;
    uint32_t sel = m->reg[(int)regs[i].bank][0];
    unsigned nb = i == R_OFFER || i == R_ACCEPT ? 2u : {nq}u;
    if (sel >= nb) {{ *valid = 0; return 0; }}
    return sel;
}}
static int rules_ok({p}_model *m, int i, int write)
{{
    uint32_t s = m->reg[R_STATUS][0];
    for (const mrule *r = rules; r->reg >= 0; r++) {{
        if (r->reg != i || r->write != write) continue;
        if ((s & r->requires) != r->requires || (s & r->lacks)) {{
            char b[160];
            snprintf(b, sizeof b, "rule: %s %s with status 0x%x", write ? "write" : "read", regs[i].name, s);
            violate(m, b);
            return 0;
        }}
    }}
    return 1;
}}

static uint32_t m_read(void *ctx, {p}_window w, uint32_t off, unsigned width)
{{
    {p}_model *m = ctx;
    uint32_t el = 0, v = 0;
    int i = lookup((unsigned)w, off, width, &el), valid;
    if (i < 0 || regs[i].access == A_WO) {{
        char b[120];
        snprintf(b, sizeof b, "read of undeclared or write-only register %s+0x%x/%u", (unsigned)w < sizeof win_name / sizeof win_name[0] ? win_name[w] : "?", off, width);
        violate(m, b);
        tr(m, 'R', (unsigned)w, off, width, 0);
        return 0;
    }}
    rules_ok(m, i, 0);
    unsigned b = bank_of(m, i, &valid);
    if (i == R_OFFER) v = valid ? (uint32_t)(m->dev_features >> (32 * b)) : 0;
    else if (i == R_STATUS) v = m->reset_stuck ? 1u : m->reg[i][0];
    else if (i == R_CFGGEN) v = m->cfggen_unstable ? m->cfggen++ : m->cfggen;
    else if (i == R_NUMQ) v = m->numq;
    else if (i == R_CONFIG) v = el < sizeof m->config ? m->config[el] : 0;
    else v = valid ? m->reg[i][b] : 0;
    tr(m, 'R', (unsigned)w, off, width, v);
    return v;
}}

static void m_write(void *ctx, {p}_window w, uint32_t off, unsigned width, uint32_t v)
{{
    {p}_model *m = ctx;
    uint32_t el = 0;
    int i = lookup((unsigned)w, off, width, &el), valid;
    tr(m, 'W', (unsigned)w, off, width, v);
    if (i < 0 || regs[i].access == A_RO) {{
        char b[120];
        snprintf(b, sizeof b, "write of undeclared or read-only register %s+0x%x/%u", (unsigned)w < sizeof win_name / sizeof win_name[0] ? win_name[w] : "?", off, width);
        violate(m, b);
        return;
    }}
    if (width < 4 && v >> (8 * width)) violate(m, "write value wider than the register");
    if (!rules_ok(m, i, 1)) return;
    unsigned b = bank_of(m, i, &valid);
    if (i == R_STATUS) {{
        if (v == 0) {{ power_on(m); return; }}
        uint32_t old = m->reg[i][0];
        if ((v & M_FEATURES_OK) && !(old & M_FEATURES_OK) &&
            (m->refuse_features || (m->drv_features & ~m->dev_features)))
            v &= ~M_FEATURES_OK;
        m->reg[i][0] = v;
        return;
    }}
    if (!valid) return;
    m->reg[i][b] = v;
    if (i == R_ACCEPT) {{
        uint64_t lo = b ? (uint32_t)m->drv_features : v, hi = b ? v : (uint32_t)(m->drv_features >> 32);
        m->drv_features = lo | hi << 32;
    }}
    if (i == R_QSIZE && (v == 0 || v > m->qmax[b] || (v & (v - 1)))) violate(m, "queue size not a power of two within the offered maximum");
}}

static void m_notify(void *ctx, uint32_t off, uint16_t queue)
{{
    {p}_model *m = ctx;
    tr(m, 'N', {dbw}, off, {dbwidth}, queue);
    uint32_t s = m->reg[R_STATUS][0];
    if ((s & ({db_req})) != ({db_req}) || (s & ({db_lacks}))) violate(m, "rule: doorbell before DRIVER_OK or after FAILED");
    if (queue >= {nq} || off != m->reg[R_QNOFF][queue] * m->mult) {{ violate(m, "doorbell at the wrong offset or queue"); return; }}
    m->notifies[queue]++;
}}

const {p}_ops {p}_model_ops_template = {{0, m_read, m_write, m_notify}};
{p}_ops {p}_model_ops({p}_model *m)
{{
    {p}_ops o = {p}_model_ops_template;
    o.ctx = m;
    return o;
}}

/* ---- device side of virtq-split ---- */
static uint64_t qaddr({p}_model *m, int lo_reg, unsigned q) {{ return m->reg[lo_reg][q] | (uint64_t)m->reg[lo_reg + 1][q] << 32; }}
static uint8_t *bus({p}_model *m, uint64_t a, size_t n)
{{
    if (a < m->dma || a - m->dma > m->mem_len || n > m->mem_len - (a - m->dma)) return NULL;
    return m->mem + (a - m->dma);
}}
static uint16_t g16(const uint8_t *x) {{ uint16_t v; memcpy(&v, x, 2); return v; }}
static void s16(uint8_t *x, uint16_t v) {{ memcpy(x, &v, 2); }}
static void s32(uint8_t *x, uint32_t v) {{ memcpy(x, &v, 4); }}

/* next available descriptor of q, checked against the ring direction */
static int pop({p}_model *m, unsigned q, uint32_t *id, uint8_t **buf, uint32_t *len)
{{
    if (!(m->reg[R_STATUS][0] & M_DRIVER_OK) || !m->reg[R_QENABLE][q]) return 0;
    uint16_t size = (uint16_t)m->reg[R_QSIZE][q];
    uint8_t *avail = bus(m, qaddr(m, R_QAVAIL, q), 4u + 2u * size);
    uint8_t *desc = bus(m, qaddr(m, R_QDESC, q), 16u * size);
    if (!avail || !desc) {{ violate(m, "ring outside the DMA region"); return 0; }}
    if (g16(avail + 2) == m->q[q].last_avail) return 0;
    uint16_t i = g16(avail + 4 + 2u * (m->q[q].last_avail % size));
    m->q[q].last_avail++;
    if (i >= size) {{ violate(m, "avail entry out of range"); return 0; }}
    const uint8_t *e = desc + 16u * i;
    uint64_t a; uint32_t l; uint16_t f, next;
    memcpy(&a, e, 8); memcpy(&l, e + 8, 4); memcpy(&f, e + 12, 2); memcpy(&next, e + 14, 2);
    if (((f & 2u) != 0) != (ring_device_writes[q] != 0) || (f & ~2u) || next) {{ violate(m, "descriptor flags do not match the ring direction"); return 0; }}
    if (l > BUF_LEN || !(*buf = bus(m, a, l))) {{ violate(m, "descriptor buffer outside the DMA region or too long"); return 0; }}
    *id = i; *len = l;
    return 1;
}}

void {p}_model_push_used({p}_model *m, unsigned q, uint32_t id, uint32_t len)
{{
    uint16_t size = (uint16_t)m->reg[R_QSIZE][q];
    uint8_t *used = bus(m, qaddr(m, R_QUSED, q), 4u + 8u * size);
    if (!used || !size) return;
    uint8_t *e = used + 4 + 8u * (m->q[q].used_idx % size);
    s32(e, id); s32(e + 4, len);
    m->q[q].used_idx++;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s16(used + 2, m->q[q].used_idx);
}}
void {p}_model_bump_used({p}_model *m, unsigned q, uint16_t delta)
{{
    uint16_t size = (uint16_t)m->reg[R_QSIZE][q];
    uint8_t *used = bus(m, qaddr(m, R_QUSED, q), 4u + 8u * size);
    if (!used) return;
    m->q[q].used_idx = (uint16_t)(m->q[q].used_idx + delta);
    s16(used + 2, m->q[q].used_idx);
}}

unsigned {p}_model_tx({p}_model *m)
{{
    unsigned n = 0;
    for (unsigned q = 0; q < {nq}; q++) {{
        if (ring_device_writes[q]) continue;
        uint32_t id, len; uint8_t *b;
        while (pop(m, q, &id, &b, &len)) {{
            if (len >= HDR_LEN && m->nwire < {UP}_MODEL_WIRE) {{
                memcpy(m->wire[m->nwire], b + HDR_LEN, len - HDR_LEN);
                m->wire_len[m->nwire++] = len - HDR_LEN;
            }}
            {p}_model_push_used(m, q, id, 0);
            n++;
        }}
    }}
    return n;
}}

int {p}_model_rx({p}_model *m, const uint8_t *frame, size_t n)
{{
    for (unsigned q = 0; q < {nq}; q++) {{
        if (!ring_device_writes[q]) continue;
        uint32_t id, len; uint8_t *b;
        if (!pop(m, q, &id, &b, &len)) return 0;
        if (len < HDR_LEN + n) {{ violate(m, "RX buffer shorter than header + frame"); return 0; }}
        memset(b, 0, HDR_LEN);
        memcpy(b + HDR_LEN, frame, n);
        {p}_model_push_used(m, q, id, (uint32_t)(HDR_LEN + n));
        return 1;
    }}
    return 0;
}}
"#,
        UP = up,
        dbw = c.window(&db.window).unwrap().index,
        dbwidth = db.width,
    );
    o
}
