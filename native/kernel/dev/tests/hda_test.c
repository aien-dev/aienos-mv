/* hda_test.c -- host test of dev/hda.c against a software model of an Intel HDA
 * controller and two codecs (L207). The model follows the specification
 * (CRST handshake, CORB/RIRB rings, stream descriptors, BDL) and checks the
 * DMA rule the SMMU enforces on the kernel: every address the controller reads
 * must lie inside the one region the driver was given.
 *
 * Codec A is QEMU-shaped (a line-out DAC and pin). Codec B is the DGX Spark's
 * "Nvidia SoC 33 HDMI/DP" codec as measured read-only under Linux on
 * 2026-10-20 (/proc/asound/card0/codec#0): vendor 0x10de0033, four digital
 * pins (default 0x185600f0) and four digital converters (wcaps 0x62b1).
 * The model is NOT the real chip: only the property that the driver sends
 * what the specification says is tested here. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ck.h"
#include "hda.h"

static int checks, failures;
#define CHECK(c)                                                       \
    do {                                                               \
        checks++;                                                      \
        if (!(c)) {                                                    \
            failures++;                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
        }                                                              \
    } while (0)

/* ---- codec model ---- */
typedef struct {
    uint8_t nid;
    uint32_t caps, pincap, pincfg, rates, bits, amp;
    uint8_t connlen; /* connection list length byte (bit 7: long form) */
    uint8_t conn[8];
} snode;
typedef struct {
    uint32_t vendor, rev, subsys, afg_rates, afg_bits, afg_amp;
    uint8_t afg;
    const snode *nodes;
    unsigned n;
} scodec;

#define PINCAP_OUT (1u << 4)
static const snode qemu_nodes[] = {
    /* nid 2: output converter, stereo, format override, out amp */
    { 2, 0x00000005u, 0, 0, 0x000005e0u, 0x00020001u, 0, 0, { 0 } },
    /* nid 3: pin, output capable, line out jack, connection list [2] */
    { 3, 0x00400001u, PINCAP_OUT, 0x01014010u, 0, 0, 0, 1, { 2 } },
};
/* Pin caps needs: type 4 in bits 23:20, connection list present bit 8. */
static const scodec codec_qemu = { 0x1af40012u, 0x00100100u, 0x1af40000u, 0x000005e0u, 0x00020001u, 0, 1, qemu_nodes, 2 };

/* Spark codec B: pins 4-7 (digital out, 4 connections, pin 4 as a range), converters 8-11. */
static snode spark_nodes[8];
static scodec codec_spark;
static void build_spark(void)
{
    for (unsigned i = 0; i < 4; i++) {
        snode *p = &spark_nodes[i];
        memset(p, 0, sizeof *p);
        p->nid = (uint8_t)(4 + i);
        p->caps = 0x407381u;
        p->pincap = 0x09000094u;
        p->pincfg = 0x185600f0u;
        if (i == 0) { /* 08, range to 0b */
            p->connlen = 2;
            p->conn[0] = 0x08, p->conn[1] = 0x80 | 0x0b;
        } else {
            p->connlen = 4;
            p->conn[0] = 0x08, p->conn[1] = 0x09, p->conn[2] = 0x0a, p->conn[3] = 0x0b;
        }
        snode *c = &spark_nodes[4 + i];
        memset(c, 0, sizeof *c);
        c->nid = (uint8_t)(8 + i);
        c->caps = 0x62b1u;
        c->rates = 0x7f0u;
        c->bits = 0xe0001u; /* 16 20 24 bit, PCM */
    }
    codec_spark = (scodec){ 0x10de0033u, 0x00100100u, 0x10de0101u, 0, 0, 0, 1, spark_nodes, 8 };
}

/* ---- controller model ---- */
typedef struct {
    uint8_t r[0x400];
    uint16_t gcap;
    uint8_t vmaj;
    int crst_vis, crst_new, crst_delay; /* CRST as read, as last written, reads until a change shows */
    int corbrp_rst;
    unsigned corbrp, rirbwp;
    const scodec *codec;      /* present at address 0 or NULL */
    int never_ready, no_answer, inject_unsol, ring_sizes;
    uint8_t *mem_lo;          /* the DMA region */
    size_t mem_len;
    int dma_violation;
    uint32_t ctl[8];          /* stream descriptor control byte 0 per stream */
    uint32_t lpib[8];
    int sd_reset_seen;
    int stream_running;
    uint8_t sink[1 << 20];
    size_t sink_n;
    struct { uint32_t verb; } log[256];
    unsigned nlog;
    uint32_t last_stream_verb, last_format_verb, last_pinctl_verb;
} sim_t;

static uint32_t rb(const sim_t *s, uint32_t o, unsigned w)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < w; i++) v |= (uint32_t)s->r[o + i] << (8 * i);
    return v;
}
static void wb(sim_t *s, uint32_t o, unsigned w, uint32_t v)
{
    for (unsigned i = 0; i < w; i++) s->r[o + i] = (uint8_t)(v >> (8 * i));
}

static int in_region(const sim_t *s, uint64_t a, uint64_t len)
{
    uint64_t lo = (uint64_t)(uintptr_t)s->mem_lo;
    return a >= lo && a + len <= lo + s->mem_len;
}

static uint32_t codec_param(const scodec *c, unsigned nid, unsigned p)
{
    if (nid == 0) {
        if (p == 0) return c->vendor;
        if (p == 2) return c->rev;
        if (p == 4) return (1u << 16) | 1u;
        return 0;
    }
    if (nid == c->afg) {
        switch (p) {
        case 5: return 1;
        case 4: return (c->nodes[0].nid << 16) | c->n;
        case 0x0a: return c->afg_rates;
        case 0x0b: return c->afg_bits;
        case 0x12: return c->afg_amp;
        default: return 0;
        }
    }
    for (unsigned i = 0; i < c->n; i++) {
        const snode *n = &c->nodes[i];
        if (n->nid != nid) continue;
        switch (p) {
        case 9: return n->caps | (n->connlen ? (1u << 8) : 0);
        case 0x0a: return n->rates;
        case 0x0b: return n->bits;
        case 0x0c: return n->pincap;
        case 0x0e: return n->connlen;
        case 0x12: return n->amp;
        default: return 0;
        }
    }
    return 0;
}

static uint32_t codec_verb(sim_t *s, uint32_t verb)
{
    const scodec *c = s->codec;
    unsigned nid = (verb >> 20) & 0xff, v12 = (verb >> 8) & 0xfff, p8 = verb & 0xff;
    if (s->nlog < 256) s->log[s->nlog++].verb = verb;
    if (((verb >> 16) & 0xf) == 0x2) s->last_format_verb = verb;
    if (v12 == 0x706) s->last_stream_verb = verb;
    if (v12 == 0x707) s->last_pinctl_verb = verb;
    if (v12 == 0xf00) return codec_param(c, nid, p8);
    if (v12 == 0xf20) return c->subsys;
    if (v12 == 0xf1c) {
        for (unsigned i = 0; i < c->n; i++)
            if (c->nodes[i].nid == nid) return c->nodes[i].pincfg;
    }
    if (v12 == 0xf02) {
        for (unsigned i = 0; i < c->n; i++) {
            if (c->nodes[i].nid != nid) continue;
            uint32_t r = 0;
            for (unsigned j = 0; j < 4; j++) r |= (uint32_t)c->nodes[i].conn[(p8 + j) & 7] << (8 * j);
            return r;
        }
    }
    return 0;
}

static unsigned ring_n(const sim_t *s, uint32_t o)
{
    unsigned code = s->r[o] & 3u;
    return code == 2 ? 256 : code == 1 ? 16 : 2;
}

static void run_corb(sim_t *s, unsigned new_wp)
{
    unsigned n = ring_n(s, 0x4e), rn = ring_n(s, 0x5e);
    uint64_t cb = rb(s, 0x40, 4) | ((uint64_t)rb(s, 0x44, 4) << 32);
    uint64_t rbase = rb(s, 0x50, 4) | ((uint64_t)rb(s, 0x54, 4) << 32);
    if (!(s->r[0x4c] & 2)) return; /* CORB DMA not running */
    while (s->corbrp != new_wp) {
        s->corbrp = (s->corbrp + 1) % n;
        if (!in_region(s, cb + 4ull * s->corbrp, 4) || !in_region(s, rbase, 8ull * rn)) {
            s->dma_violation = 1;
            return;
        }
        uint32_t verb;
        memcpy(&verb, (void *)(uintptr_t)(cb + 4ull * s->corbrp), 4);
        if (s->no_answer || !s->codec) continue;
        if (s->inject_unsol) {
            s->rirbwp = (s->rirbwp + 1) % rn;
            uint32_t u[2] = { 0xdeadbeefu, 0x10u };
            memcpy((void *)(uintptr_t)(rbase + 8ull * s->rirbwp), u, 8);
        }
        uint32_t e[2] = { codec_verb(s, verb), 0 };
        s->rirbwp = (s->rirbwp + 1) % rn;
        memcpy((void *)(uintptr_t)(rbase + 8ull * s->rirbwp), e, 8);
    }
}

static unsigned sd_of(uint32_t o) { return (o - 0x80u) / 0x20u; }

static void consume(sim_t *s, unsigned sd, uint32_t bytes)
{
    uint32_t base = 0x80u + 0x20u * sd;
    uint64_t bdl = rb(s, base + 0x18, 4) | ((uint64_t)rb(s, base + 0x1c, 4) << 32);
    uint32_t cbl = rb(s, base + 0x08, 4), lvi = rb(s, base + 0x0c, 2);
    while (bytes) {
        /* walk BDL to the entry holding lpib */
        uint32_t pos = 0, off = 0;
        uint64_t addr = 0, len = 0;
        int found = 0;
        for (unsigned i = 0; i <= lvi; i++) {
            if (!in_region(s, bdl + 16ull * i, 16)) { s->dma_violation = 1; return; }
            uint32_t e[4];
            memcpy(e, (void *)(uintptr_t)(bdl + 16ull * i), 16);
            uint64_t a = e[0] | ((uint64_t)e[1] << 32);
            if (s->lpib[sd] < pos + e[2]) { addr = a; len = e[2]; off = s->lpib[sd] - pos; found = 1; break; }
            pos += e[2];
        }
        if (!found) { s->dma_violation = 1; return; }
        uint32_t take = (uint32_t)(len - off);
        if (take > bytes) take = bytes;
        if (!in_region(s, addr + off, take)) { s->dma_violation = 1; return; }
        if (s->sink_n + take <= sizeof s->sink) memcpy(s->sink + s->sink_n, (void *)(uintptr_t)(addr + off), take), s->sink_n += take;
        s->lpib[sd] = (s->lpib[sd] + take) % cbl;
        bytes -= take;
    }
}

static uint32_t sim_rd(void *ctx, uint32_t o, unsigned w)
{
    sim_t *s = ctx;
    if (o == 0x00 && w == 2) return s->gcap;
    if (o == 0x03) return s->vmaj;
    if (o == 0x02) return 0;
    if (o == 0x08 && w == 4) {
        if (s->crst_delay > 0 && --s->crst_delay == 0 && !s->never_ready) s->crst_vis = s->crst_new;
        return (uint32_t)s->crst_vis;
    }
    if (o == 0x4a) return s->corbrp | (s->corbrp_rst ? 0x8000u : 0);
    if (o == 0x58) return s->rirbwp;
    if (o >= 0x80 && (o - 0x80u) % 0x20u == 0 && w == 1) return s->ctl[sd_of(o)];
    if (o >= 0x80 && (o - 0x80u) % 0x20u == 4 && w == 4) {
        unsigned sd = sd_of(o);
        if (s->ctl[sd] & 2) consume(s, sd, 2048);
        return s->lpib[sd];
    }
    return rb(s, o, w);
}

static void sim_wr(void *ctx, uint32_t o, unsigned w, uint32_t v)
{
    sim_t *s = ctx;
    if (o == 0x08 && w == 4) {
        int want = (int)(v & 1u);
        if (want != s->crst_new) {
            s->crst_new = want;
            if (!want) s->crst_vis = 0;
            s->crst_delay = 3;
            if (want && s->codec) wb(s, 0x0e, 2, 1); /* codec 0 answers */
            if (!want) { /* reset: rings and streams back to defaults */
                s->corbrp = 0; s->rirbwp = 0; s->corbrp_rst = 0;
                memset(s->ctl, 0, sizeof s->ctl);
                memset(s->lpib, 0, sizeof s->lpib);
                s->r[0x4c] = 0; s->r[0x5c] = 0;
            }
        }
        return;
    }
    if (o == 0x0e && w == 2) { wb(s, 0x0e, 2, rb(s, 0x0e, 2) & ~v); return; }
    if (o == 0x4e || o == 0x5e) { s->r[o] = (uint8_t)((s->r[o] & 0xfc) | (v & 3)); return; }
    if (o == 0x4a) { s->corbrp_rst = (v & 0x8000u) != 0; if (s->corbrp_rst) s->corbrp = 0; return; }
    if (o == 0x58) { if (v & 0x8000u) s->rirbwp = 0; return; }
    if (o == 0x48) { wb(s, o, w, v); run_corb(s, v & 0xff); return; }
    if (o == 0x4c || o == 0x5c) { s->r[o] = (uint8_t)v; return; }
    if (o >= 0x80 && (o - 0x80u) % 0x20u == 0 && w == 1) {
        unsigned sd = sd_of(o);
        if ((v & 1u) && !(s->ctl[sd] & 1u)) { s->sd_reset_seen = 1; s->lpib[sd] = 0; }
        s->ctl[sd] = v & 3u;
        return;
    }
    if (o >= 0x80) { wb(s, o, w, v); return; }
    wb(s, o, w, v);
}

static void sim_init(sim_t *s, const scodec *c, uint8_t *mem)
{
    memset(s, 0, sizeof *s);
    s->gcap = (uint16_t)((4u << 12) | (4u << 8) | 2u | 1u); /* 4 out, 4 in, 64OK */
    s->vmaj = 1;
    s->codec = c;
    s->r[0x4e] = 0x70 | 0x2;
    s->r[0x5e] = 0x70 | 0x2;
    s->mem_lo = mem;
    s->mem_len = CK_HDA_DMA_BYTES;
}

static ck_hda_bus bus_of(sim_t *s)
{
    ck_hda_bus b = { sim_rd, sim_wr, s };
    return b;
}

static int find_log(const sim_t *s, unsigned nid, unsigned v12, unsigned payload)
{
    for (unsigned i = 0; i < s->nlog; i++) {
        uint32_t v = s->log[i].verb;
        if (((v >> 20) & 0xff) == nid && ((v >> 8) & 0xfff) == v12 && (v & 0xff) == payload) return 1;
    }
    return 0;
}

static uint8_t *dma(void)
{
    void *p = aligned_alloc(4096, CK_HDA_DMA_BYTES);
    memset(p, 0, CK_HDA_DMA_BYTES);
    return p;
}
#define PHYS(m) ((uint64_t)(uintptr_t)(m))

/* ---- tests ---- */
static void test_pure(void)
{
    CHECK(ck_hda_verb12(0, 2, 0xf00, 9) == 0x002f0009u);
    CHECK(ck_hda_verb12(3, 0x1f, 0x706, 0x10) == 0x31f70610u);
    CHECK(ck_hda_verb4(0, 2, 2, 0x0011) == 0x00220011u);
    CHECK(ck_hda_format(48000, 16, 2) == 0x0011);
    CHECK(ck_hda_format(44100, 16, 2) == 0x4011);
    CHECK(ck_hda_format(96000, 24, 2) == 0x0831 && ck_hda_format(32000, 16, 1) == 0x0a10);
    CHECK(ck_hda_format(12345, 16, 2) == 0xffff && ck_hda_format(48000, 7, 2) == 0xffff && ck_hda_format(48000, 16, 0) == 0xffff);
    CHECK(ck_hda_rate_bit(48000) == (1u << 6) && ck_hda_rate_bit(44100) == (1u << 5) && ck_hda_rate_bit(7) == 0);
    /* the tone: exact against libm to 4 LSB at full scale, 64 whole periods, seamless loop */
    enum { N = 3072 };
    static int16_t pcm[N * 2];
    ck_hda_tone_fill(pcm, N, 2, 1000, 48000, 32767);
    int worst = 0;
    for (unsigned i = 0; i < N; i++) {
        double want = 32767.0 * sin(2.0 * M_PI * 1000.0 * i / 48000.0);
        int d = abs((int)pcm[2 * i] - (int)lround(want));
        if (d > worst) worst = d;
        CHECK(pcm[2 * i] == pcm[2 * i + 1]);
    }
    if (worst > 4) printf("  worst tone error %d LSB\n", worst);
    CHECK(worst <= 4); /* linear interpolation of a 64-point quarter wave: about -78 dBFS */
    CHECK(pcm[0] == 0 && pcm[2 * 12] == 32766 && pcm[2 * 36] == -32766); /* 48 frames per period: peak at 12, trough at 36 */
    ck_hda_tone_fill(pcm, 4, 2, 1000, 48000, 0);
    CHECK(pcm[0] == 0 && pcm[2] == 0 && pcm[6] == 0);
    CHECK(strcmp(ck_hda_wtype_name(CK_HDA_W_PIN), "pin") == 0 && strcmp(ck_hda_wtype_name(15), "vendor") == 0);
}

static int zero_crossings(const int16_t *x, size_t frames, size_t stride)
{
    int n = 0;
    for (size_t i = 1; i < frames; i++)
        if ((x[(i - 1) * stride] < 0) != (x[i * stride] < 0)) n++;
    return n;
}

static void test_qemu_codec(void)
{
    static sim_t s;
    uint8_t *m = dma();
    sim_init(&s, &codec_qemu, m);
    s.inject_unsol = 1;
    ck_hda h;
    CHECK(ck_hda_init(&h, bus_of(&s), m, PHYS(m)) == CK_HDA_OK);
    CHECK(h.vmaj == 1 && h.oss == 4 && h.iss == 4 && h.statests == 1 && h.corb_n == 256 && h.rirb_n == 256);
    CHECK(rb(&s, 0x40, 4) == (uint32_t)PHYS(m) && rb(&s, 0x50, 4) == (uint32_t)(PHYS(m) + 1024));
    CHECK(ck_hda_enumerate(&h) == CK_HDA_OK);
    CHECK(h.unsol > 0);
    CHECK(h.codec[0].vendor == 0x1af40012u && h.codec[0].afg == 1 && h.codec[0].wn == 2 && h.nwidgets == 2);
    CHECK(h.w[0].type == CK_HDA_W_OUT && h.w[1].type == CK_HDA_W_PIN && h.w[1].nconn == 1 && h.w[1].conn[0] == 2);
    CHECK(ck_hda_pick_output(&h) == CK_HDA_OK && h.pin == 3 && h.dac == 2 && h.npath == 2);
    CHECK(ck_hda_play_tone(&h, CK_HDA_TONE_HZ, CK_HDA_TONE_RATE, 6000) == CK_HDA_OK);
    CHECK(h.streaming && h.sd == 4);
    CHECK(find_log(&s, 2, 0x706, 0x10));            /* stream tag 1, channel 0 on the DAC */
    CHECK((s.last_format_verb & 0xffff) == 0x0011); /* 48 kHz, 16 bit, 2 channels */
    CHECK((s.last_pinctl_verb & 0xff) == 0x40);      /* output enable */
    CHECK(rb(&s, 0x80 + 0x20 * 4 + 0x08, 4) == CK_HDA_BUF_BYTES && rb(&s, 0x80 + 0x20 * 4 + 0x0c, 2) == 1);
    CHECK(s.r[0x80 + 0x20 * 4 + 2] == 0x10); /* SDnCTL byte 2: stream tag 1 in bits 7:4 */
    uint64_t last = 0;
    while (ck_hda_position(&h) < 6 * CK_HDA_BUF_BYTES) {
        uint64_t p = ck_hda_position(&h);
        CHECK(p >= last);
        last = p;
    }
    CHECK(s.dma_violation == 0);
    CHECK(s.sink_n >= 6 * CK_HDA_BUF_BYTES);
    const int16_t *pcm = (const int16_t *)(void *)s.sink;
    size_t frames = s.sink_n / 4;
    int zc = zero_crossings(pcm, frames, 2);
    /* 1000 Hz: 2000 zero crossings per second of audio */
    CHECK(abs(zc - (int)(frames * 2000u / 48000u)) <= 2);
    int peak = 0;
    for (size_t i = 0; i < frames; i++) if (abs(pcm[2 * i]) > peak) peak = abs(pcm[2 * i]);
    CHECK(peak >= 5990 && peak <= 6000);
    CHECK(ck_hda_stop(&h) == CK_HDA_OK && !h.streaming && !(s.ctl[4] & 2));
    CHECK(find_log(&s, 2, 0x706, 0x00) && find_log(&s, 3, 0x707, 0x00)); /* tag 0, pin off */
    CHECK(ck_hda_shutdown(&h) == CK_HDA_OK && s.crst_vis == 0 && !(s.r[0x4c] & 2) && !(s.r[0x5c] & 2));
    CHECK(ck_hda_shutdown(&h) == CK_HDA_OK); /* idempotent */
    free(m);
}

static void test_spark_codec(void)
{
    static sim_t s;
    uint8_t *m = dma();
    build_spark();
    sim_init(&s, &codec_spark, m);
    ck_hda h;
    CHECK(ck_hda_init(&h, bus_of(&s), m, PHYS(m)) == CK_HDA_OK);
    CHECK(ck_hda_enumerate(&h) == CK_HDA_OK);
    CHECK(h.codec[0].vendor == 0x10de0033u && h.codec[0].subsystem == 0x10de0101u && h.codec[0].wn == 8);
    CHECK(h.w[0].type == CK_HDA_W_PIN && h.w[0].pincfg == 0x185600f0u && h.w[0].nconn == 4);
    CHECK(h.w[0].conn[0] == 8 && h.w[0].conn[3] == 0x0b); /* the range 08-0b expanded */
    CHECK(h.w[1].nconn == 4 && h.w[1].conn[2] == 0x0a);
    CHECK(h.w[4].type == CK_HDA_W_OUT && h.w[4].rates == 0x7f0u && (h.w[4].caps & (1u << 9)));
    CHECK(ck_hda_pick_output(&h) == CK_HDA_OK && h.pin == 4 && h.dac == 8 && h.sel[0] == 0);
    CHECK(ck_hda_play_tone(&h, CK_HDA_TONE_HZ, CK_HDA_TONE_RATE, 3000) == CK_HDA_OK);
    CHECK(find_log(&s, 8, 0x70d, 0x01)); /* digital converter enabled */
    CHECK(find_log(&s, 4, 0x701, 0x00));
    CHECK(ck_hda_stop(&h) == CK_HDA_OK);
    CHECK(ck_hda_shutdown(&h) == CK_HDA_OK && s.dma_violation == 0);
    free(m);
}

static void test_refusals(void)
{
    static sim_t s;
    uint8_t *m = dma();
    ck_hda h;
    /* wrong spec major version */
    sim_init(&s, &codec_qemu, m);
    s.vmaj = 2;
    CHECK(ck_hda_init(&h, bus_of(&s), m, PHYS(m)) == CK_HDA_E_ARG);
    /* controller never reports CRST */
    sim_init(&s, &codec_qemu, m);
    s.never_ready = 1;
    CHECK(ck_hda_init(&h, bus_of(&s), m, PHYS(m)) == CK_HDA_E_RESET);
    /* codec silent: init works, the first verb times out */
    sim_init(&s, &codec_qemu, m);
    s.no_answer = 1;
    CHECK(ck_hda_init(&h, bus_of(&s), m, PHYS(m)) == CK_HDA_OK);
    CHECK(ck_hda_enumerate(&h) == CK_HDA_E_VERB);
    /* no codec on the link */
    sim_init(&s, NULL, m);
    CHECK(ck_hda_init(&h, bus_of(&s), m, PHYS(m)) == CK_HDA_OK && h.statests == 0);
    CHECK(ck_hda_enumerate(&h) == CK_HDA_E_NOCODEC);
    CHECK(ck_hda_pick_output(&h) == CK_HDA_E_NOPATH && ck_hda_play_tone(&h, 1000, 48000, 1) == CK_HDA_E_NOPATH);
    /* not 64-bit capable but the region sits above 4 GiB */
    sim_init(&s, &codec_qemu, m);
    s.gcap &= (uint16_t)~1u;
    CHECK(ck_hda_init(&h, bus_of(&s), m, (1ull << 32)) == CK_HDA_E_DMA64);
    /* unaligned region and bad arguments */
    sim_init(&s, &codec_qemu, m);
    CHECK(ck_hda_init(&h, bus_of(&s), m, PHYS(m) + 8) == CK_HDA_E_ARG && ck_hda_init(NULL, bus_of(&s), m, 0) == CK_HDA_E_ARG);
    /* a converter that does not do 16-bit PCM */
    static snode n2[2];
    static scodec c2;
    memcpy(n2, qemu_nodes, sizeof n2);
    n2[0].bits = 0x00080001u; /* 24 bit only */
    c2 = codec_qemu;
    c2.nodes = n2;
    sim_init(&s, &c2, m);
    CHECK(ck_hda_init(&h, bus_of(&s), m, PHYS(m)) == CK_HDA_OK && ck_hda_enumerate(&h) == CK_HDA_OK);
    CHECK(ck_hda_pick_output(&h) == CK_HDA_OK && ck_hda_play_tone(&h, 1000, 48000, 1000) == CK_HDA_E_FORMAT && !h.streaming);
    /* a pin whose connectivity is "none" is never chosen */
    n2[0].bits = 0x00020001u;
    n2[1].pincfg = 0x41014010u; /* port connectivity 1: no physical connection */
    sim_init(&s, &c2, m);
    CHECK(ck_hda_init(&h, bus_of(&s), m, PHYS(m)) == CK_HDA_OK && ck_hda_enumerate(&h) == CK_HDA_OK);
    CHECK(ck_hda_pick_output(&h) == CK_HDA_E_NOPATH);
    /* a controller with no output stream */
    sim_init(&s, &codec_qemu, m);
    s.gcap = (uint16_t)((0u << 12) | (4u << 8) | 3u);
    CHECK(ck_hda_init(&h, bus_of(&s), m, PHYS(m)) == CK_HDA_OK && ck_hda_enumerate(&h) == CK_HDA_OK);
    CHECK(ck_hda_pick_output(&h) == CK_HDA_OK && ck_hda_play_tone(&h, 1000, 48000, 1000) == CK_HDA_E_NOSTREAM);
    free(m);
}

int main(void)
{
    test_pure();
    test_qemu_codec();
    test_spark_codec();
    test_refusals();
    printf("hda_test: %d checks, %d failures\n", checks, failures);
    printf("CK_HDA_HOST: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
