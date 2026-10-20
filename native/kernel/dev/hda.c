/* hda.c -- Intel HD Audio controller driver (see hda.h). Freestanding, polled. */
#include "hda.h"
#include "ck.h"

/* Controller registers (spec 3.3, register map p26-29). */
#define R_GCAP 0x00u
#define R_VMIN 0x02u
#define R_VMAJ 0x03u
#define R_GCTL 0x08u
#define R_STATESTS 0x0eu
#define R_CORBLBASE 0x40u
#define R_CORBUBASE 0x44u
#define R_CORBWP 0x48u
#define R_CORBRP 0x4au
#define R_CORBCTL 0x4cu
#define R_CORBSIZE 0x4eu
#define R_RIRBLBASE 0x50u
#define R_RIRBUBASE 0x54u
#define R_RIRBWP 0x58u
#define R_RINTCNT 0x5au
#define R_RIRBCTL 0x5cu
#define R_RIRBSTS 0x5du
#define R_RIRBSIZE 0x5eu
#define GCTL_CRST 0x1u
#define RING_RUN 0x2u    /* CORBCTL.CORBRUN, RIRBCTL.RIRBDMAEN */
#define RING_RESET 0x8000u /* CORBRP.CORBRPRST, RIRBWP.RIRBWPRST */
/* Stream descriptors start at 0x80, 0x20 apart: inputs, outputs, bidirectional. */
#define SD_BASE 0x80u
#define SD_STRIDE 0x20u
#define SD_CTL 0x00u  /* 3 bytes: byte 0 SRST/RUN, byte 2 bits 7:4 stream tag */
#define SD_STS 0x03u
#define SD_LPIB 0x04u
#define SD_CBL 0x08u
#define SD_LVI 0x0cu
#define SD_FMT 0x12u
#define SD_BDLPL 0x18u
#define SD_BDLPU 0x1cu
#define SD_SRST 0x1u
#define SD_RUN 0x2u
#define SD_STS_CLEAR 0x1cu /* BCIS, FIFOE, DESE are write-1-to-clear */

/* Codec verbs (spec ch. 7). */
#define V_GET_PARAM 0xf00u
#define V_GET_CONN_LIST 0xf02u
#define V_GET_PINCFG 0xf1cu
#define V_GET_SUBSYS 0xf20u
#define V_SET_CONN_SEL 0x701u
#define V_SET_POWER 0x705u
#define V_SET_STREAM 0x706u
#define V_SET_PIN_CTL 0x707u
#define V_SET_EAPD 0x70cu
#define V_SET_DIGI1 0x70du
#define V4_SET_FORMAT 0x2u
#define V4_SET_AMP 0x3u
#define P_VENDOR 0x00u
#define P_REVISION 0x02u
#define P_NODES 0x04u
#define P_FGTYPE 0x05u
#define P_WCAPS 0x09u
#define P_RATES 0x0au
#define P_BITS 0x0bu
#define P_PINCAP 0x0cu
#define P_CONN_LEN 0x0eu
#define P_AMP_OUT 0x12u

#ifndef CK_TEST_HDA_MUT
#define CK_TEST_HDA_MUT 0 /* TEST-ONLY host mutants, see stage.mk */
#endif
#define WAIT_US 20000u /* bounded wait for any register handshake */
#define TAG 1u         /* stream tag of the one output stream */

static uint32_t r8(ck_hda *h, uint32_t o) { return h->bus.rd(h->bus.ctx, o, 1); }
static uint32_t r16(ck_hda *h, uint32_t o) { return h->bus.rd(h->bus.ctx, o, 2); }
static uint32_t r32(ck_hda *h, uint32_t o) { return h->bus.rd(h->bus.ctx, o, 4); }
static void w8(ck_hda *h, uint32_t o, uint32_t v) { h->bus.wr(h->bus.ctx, o, 1, v); }
static void w16(ck_hda *h, uint32_t o, uint32_t v) { h->bus.wr(h->bus.ctx, o, 2, v); }
static void w32(ck_hda *h, uint32_t o, uint32_t v) { h->bus.wr(h->bus.ctx, o, 4, v); }

/* Poll register o (width 1/2/4) until (value & mask) == want; 0 or -1 on timeout. */
static int wait_bits(ck_hda *h, uint32_t o, unsigned width, uint32_t mask, uint32_t want)
{
    uint64_t t0 = ck_time_us();
    for (;;) {
        uint32_t v = h->bus.rd(h->bus.ctx, o, width);
        if ((v & mask) == want) return 0;
        if (ck_time_us() - t0 > WAIT_US) return -1;
        ck_udelay(10);
    }
}

static void zero(void *p, size_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n--) *b++ = 0;
}

/* ---- pure helpers ---- */
uint32_t ck_hda_verb12(unsigned codec, unsigned nid, unsigned verb, unsigned payload8)
{
    return ((uint32_t)(codec & 0xfu) << 28) | ((uint32_t)(nid & 0xffu) << 20) | ((uint32_t)(verb & 0xfffu) << 8) |
           (payload8 & 0xffu);
}
uint32_t ck_hda_verb4(unsigned codec, unsigned nid, unsigned verb4, unsigned payload16)
{
    return ((uint32_t)(codec & 0xfu) << 28) | ((uint32_t)(nid & 0xffu) << 20) | ((uint32_t)(verb4 & 0xfu) << 16) |
           (payload16 & 0xffffu);
}

uint16_t ck_hda_format(unsigned rate_hz, unsigned bits, unsigned channels)
{
    unsigned base, mult, div;
    switch (rate_hz) {
    case 8000: base = 0, mult = 1, div = 6; break;
    case 11025: base = 1, mult = 1, div = 4; break;
    case 16000: base = 0, mult = 1, div = 3; break;
    case 22050: base = 1, mult = 1, div = 2; break;
    case 32000: base = 0, mult = 2, div = 3; break;
    case 44100: base = 1, mult = 1, div = 1; break;
    case 48000: base = 0, mult = 1, div = 1; break;
    case 88200: base = 1, mult = 2, div = 1; break;
    case 96000: base = 0, mult = 2, div = 1; break;
    case 176400: base = 1, mult = 4, div = 1; break;
    case 192000: base = 0, mult = 4, div = 1; break;
    default: return 0xffffu;
    }
    unsigned b;
    switch (bits) {
    case 8: b = 0; break;
    case 16: b = 1; break;
    case 20: b = 2; break;
    case 24: b = 3; break;
    case 32: b = 4; break;
    default: return 0xffffu;
    }
    if (channels < 1 || channels > 16) return 0xffffu;
    return (uint16_t)((base << 14) | ((mult - 1) << 11) | ((div - 1) << 8) | (b << 4) | (channels - 1));
}

uint32_t ck_hda_rate_bit(unsigned rate_hz)
{
    static const unsigned r[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000, 384000 };
    for (unsigned i = 0; i < sizeof r / sizeof r[0]; i++)
        if (r[i] == rate_hz) return 1u << i;
    return 0;
}

const char *ck_hda_wtype_name(unsigned type)
{
    switch (type) {
    case CK_HDA_W_OUT: return "out";
    case CK_HDA_W_IN: return "in";
    case CK_HDA_W_MIX: return "mix";
    case CK_HDA_W_SEL: return "sel";
    case CK_HDA_W_PIN: return "pin";
    case CK_HDA_W_POWER: return "power";
    case CK_HDA_W_KNOB: return "knob";
    case CK_HDA_W_BEEP: return "beep";
    default: return "vendor";
    }
}

/* sin(i * pi / 128) * 32767, i = 0..64 (a quarter wave), from libm once. */
static const int16_t sin_q[65] = {
    0,     804,   1608,  2410,  3212,  4011,  4808,  5602,  6393,  7179,  7962,  8739,  9512,
    10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868,
    19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319,
    26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113,
    31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767,
};

/* sin of a 32-bit phase (2^32 = one turn), Q15. */
static int32_t sine_q15(uint32_t phase)
{
    unsigned quadrant = phase >> 30;           /* 0..3 */
    uint32_t in_q = phase & 0x3fffffffu;       /* position inside the quadrant */
    if (quadrant & 1) in_q = 0x40000000u - in_q; /* mirror quadrants 1 and 3 */
    unsigned idx = in_q >> 24;                 /* 0..64 */
    uint32_t frac = in_q & 0xffffffu;          /* 24 bits */
    int32_t a = sin_q[idx];
    int32_t b = sin_q[idx < 64 ? idx + 1 : 64];
    int32_t v = a + (int32_t)(((int64_t)(b - a) * frac) >> 24);
    return (quadrant & 2) ? -v : v;
}

void ck_hda_tone_fill(int16_t *pcm, unsigned frames, unsigned channels, unsigned freq_hz, unsigned rate_hz, int amp)
{
    for (unsigned i = 0; i < frames; i++) {
        uint32_t phase = (uint32_t)(((uint64_t)i * freq_hz << 32) / rate_hz);
        int64_t prod = (int64_t)sine_q15(phase) * amp; /* symmetric rounding: never -32768 */
        int16_t s = (int16_t)(prod >= 0 ? (prod + 16384) >> 15 : -((-prod + 16384) >> 15));
        for (unsigned c = 0; c < channels; c++) pcm[(size_t)i * channels + c] = s;
    }
}

/* ---- rings ---- */
static uint32_t *corb(ck_hda *h) { return (uint32_t *)(void *)h->mem; }
static volatile uint32_t *rirb(ck_hda *h) { return (volatile uint32_t *)(void *)(h->mem + 1024u); }

static unsigned ring_pick(ck_hda *h, uint32_t reg, int *ok)
{
    uint32_t cap = (r8(h, reg) >> 4) & 0xfu;
    *ok = 1;
    if (cap & 4u) { w8(h, reg, 2); return 256; }
    if (cap & 2u) { w8(h, reg, 1); return 16; }
    if (cap & 1u) { w8(h, reg, 0); return 2; }
    *ok = 0;
    return 0;
}

static void stop_engines(ck_hda *h)
{
    unsigned nstreams = h->iss + h->oss + h->bss;
    for (unsigned i = 0; i < nstreams; i++) {
        uint32_t o = SD_BASE + SD_STRIDE * i + SD_CTL;
        if (r8(h, o) & SD_RUN) {
            w8(h, o, r8(h, o) & ~SD_RUN);
            (void)wait_bits(h, o, 1, SD_RUN, 0);
        }
    }
    if (r8(h, R_CORBCTL) & RING_RUN) {
        w8(h, R_CORBCTL, 0);
        (void)wait_bits(h, R_CORBCTL, 1, RING_RUN, 0);
    }
    if (r8(h, R_RIRBCTL) & RING_RUN) {
        w8(h, R_RIRBCTL, 0);
        (void)wait_bits(h, R_RIRBCTL, 1, RING_RUN, 0);
    }
}

static void read_caps(ck_hda *h)
{
    h->gcap = (uint16_t)r16(h, R_GCAP);
    h->vmin = (uint8_t)r8(h, R_VMIN);
    h->vmaj = (uint8_t)r8(h, R_VMAJ);
    h->oss = (h->gcap >> 12) & 0xfu;
    h->iss = (h->gcap >> 8) & 0xfu;
    h->bss = (h->gcap >> 3) & 0x1fu;
}

int ck_hda_init(ck_hda *h, ck_hda_bus bus, uint8_t *mem, uint64_t phys)
{
    if (!h || !bus.rd || !bus.wr || !mem || (phys & 0xfffu)) return CK_HDA_E_ARG;
    zero(h, sizeof *h);
    h->bus = bus;
    h->mem = mem;
    h->phys = phys;
    h->out_codec = -1;
    read_caps(h);
    if (h->vmaj != 1) return CK_HDA_E_ARG;
    if (!(h->gcap & 1u) && phys + CK_HDA_DMA_BYTES > (1ull << 32)) return CK_HDA_E_DMA64;
    /* 3.3.7: engines stopped before CRST goes to 0. */
    stop_engines(h);
    w32(h, R_GCTL, r32(h, R_GCTL) & ~GCTL_CRST);
    if (wait_bits(h, R_GCTL, 4, GCTL_CRST, 0)) return CK_HDA_E_RESET;
    ck_udelay(200); /* link RESET# pulse (5.5): comfortably over 100 us */
    w32(h, R_GCTL, r32(h, R_GCTL) | GCTL_CRST);
    if (wait_bits(h, R_GCTL, 4, GCTL_CRST, GCTL_CRST)) return CK_HDA_E_RESET;
    ck_udelay(1000); /* codecs signal presence within 521 us of the link coming up */
    read_caps(h);
    h->statests = (uint16_t)(r16(h, R_STATESTS) & 0x7fffu);
    w16(h, R_STATESTS, h->statests); /* write-1-to-clear: only after we noted them */

    int ok;
    h->corb_n = ring_pick(h, R_CORBSIZE, &ok);
    if (!ok) return CK_HDA_E_RING;
    h->rirb_n = ring_pick(h, R_RIRBSIZE, &ok);
    if (!ok) return CK_HDA_E_RING;
    zero(h->mem, 4096);
    w32(h, R_CORBLBASE, (uint32_t)phys);
    w32(h, R_CORBUBASE, (uint32_t)(phys >> 32));
    w32(h, R_RIRBLBASE, (uint32_t)(phys + 1024u));
    w32(h, R_RIRBUBASE, (uint32_t)((phys + 1024u) >> 32));
    w16(h, R_CORBWP, 0);
    /* 3.3.21: write 1 to CORBRPRST, read it back as 1, write 0, read back 0. */
    w16(h, R_CORBRP, RING_RESET);
    if (wait_bits(h, R_CORBRP, 2, RING_RESET, RING_RESET)) return CK_HDA_E_RING;
    w16(h, R_CORBRP, 0);
    if (wait_bits(h, R_CORBRP, 2, RING_RESET, 0)) return CK_HDA_E_RING;
    w16(h, R_RIRBWP, RING_RESET); /* self-clearing */
    w16(h, R_RINTCNT, 1);
    ck_mb();
    w8(h, R_CORBCTL, RING_RUN);
    w8(h, R_RIRBCTL, RING_RUN);
    if (wait_bits(h, R_CORBCTL, 1, RING_RUN, RING_RUN) || wait_bits(h, R_RIRBCTL, 1, RING_RUN, RING_RUN))
        return CK_HDA_E_RING;
    h->corb_wp = 0;
    h->rirb_rp = 0;
    return CK_HDA_OK;
}

int ck_hda_cmd(ck_hda *h, uint32_t verb, uint32_t *resp)
{
    unsigned next = (h->corb_wp + 1) % h->corb_n;
    corb(h)[next] = verb;
    ck_mb();
    w16(h, R_CORBWP, next);
    h->corb_wp = next;
    uint64_t t0 = ck_time_us();
    for (;;) {
        unsigned wp = r16(h, R_RIRBWP) & 0xffu;
        while (h->rirb_rp != wp) {
            h->rirb_rp = (h->rirb_rp + 1) % h->rirb_n;
            ck_mb();
            uint32_t data = rirb(h)[h->rirb_rp * 2u];
            uint32_t ex = rirb(h)[h->rirb_rp * 2u + 1u];
            if (ex & 0x10u) { /* unsolicited (3.6.5): counted, never an answer */
                h->unsol++;
                continue;
            }
            w8(h, R_RIRBSTS, 0x5u); /* RINTFL, RIRBOIS: write 1 to clear */
            if (resp) *resp = data;
            return CK_HDA_OK;
        }
        if (ck_time_us() - t0 > WAIT_US) return CK_HDA_E_VERB;
        ck_udelay(10);
    }
}

static int param(ck_hda *h, unsigned c, unsigned nid, unsigned p, uint32_t *out)
{
    return ck_hda_cmd(h, ck_hda_verb12(c, nid, V_GET_PARAM, p), out);
}

/* Expand the connection list of w (7.3.3.3): 8-bit or 16-bit entries, a set
 * top bit makes the entry the end of a range that starts at the previous one. */
static int read_conn(ck_hda *h, ck_hda_widget *w)
{
    uint32_t len;
    int rc = param(h, w->codec, w->nid, P_CONN_LEN, &len);
    if (rc) return rc;
    unsigned lng = (len >> 7) & 1u, n = len & 0x7fu, per = lng ? 2 : 4;
    uint16_t prev = 0;
    w->nconn = 0;
    for (unsigned i = 0; i < n; i += per) {
        uint32_t r;
        rc = ck_hda_cmd(h, ck_hda_verb12(w->codec, w->nid, V_GET_CONN_LIST, i), &r);
        if (rc) return rc;
        for (unsigned j = 0; j < per && i + j < n; j++) {
            uint16_t e = lng ? (uint16_t)((r >> (16 * j)) & 0xffffu) : (uint16_t)((r >> (8 * j)) & 0xffu);
            uint16_t top = lng ? 0x8000u : 0x80u;
            uint16_t id = (uint16_t)(e & ~top);
            if ((e & top) && w->nconn) {
                for (uint16_t k = (uint16_t)(prev + 1); k <= id && w->nconn < CK_HDA_MAX_CONN; k++) w->conn[w->nconn++] = k;
            } else if (w->nconn < CK_HDA_MAX_CONN) {
                w->conn[w->nconn++] = id;
            }
            prev = id;
        }
    }
    return CK_HDA_OK;
}

int ck_hda_enumerate(ck_hda *h)
{
    int found = 0;
    h->nwidgets = 0;
    for (unsigned c = 0; c < CK_HDA_MAX_CODECS; c++) {
        if (!(h->statests & (1u << c))) continue;
        ck_hda_codec *k = &h->codec[c];
        k->present = 1;
        uint32_t v, nodes;
        int rc = param(h, c, 0, P_VENDOR, &k->vendor);
        if (!rc) rc = param(h, c, 0, P_REVISION, &k->revision);
        if (!rc) rc = param(h, c, 0, P_NODES, &nodes);
        if (rc) return rc;
        found++;
        unsigned fg0 = (nodes >> 16) & 0xffu, nfg = nodes & 0xffu;
        k->afg = 0;
        for (unsigned f = 0; f < nfg && !k->afg; f++) {
            rc = param(h, c, fg0 + f, P_FGTYPE, &v);
            if (rc) return rc;
            if ((v & 0xffu) == 1u) k->afg = (uint8_t)(fg0 + f);
        }
        if (!k->afg) continue;
        rc = ck_hda_cmd(h, ck_hda_verb12(c, k->afg, V_GET_SUBSYS, 0), &k->subsystem);
        uint32_t afg_rates = 0, afg_bits = 0, afg_amp = 0;
        if (!rc) rc = param(h, c, k->afg, P_RATES, &afg_rates);
        if (!rc) rc = param(h, c, k->afg, P_BITS, &afg_bits);
        if (!rc) rc = param(h, c, k->afg, P_AMP_OUT, &afg_amp);
        if (!rc) rc = param(h, c, k->afg, P_NODES, &nodes);
        if (rc) return rc;
        unsigned w0 = (nodes >> 16) & 0xffu, nw = nodes & 0xffu;
        k->wfirst = (uint16_t)h->nwidgets;
        for (unsigned i = 0; i < nw; i++) {
            if (h->nwidgets >= CK_HDA_MAX_WIDGETS) return CK_HDA_E_WIDGETS;
            ck_hda_widget *w = &h->w[h->nwidgets];
            zero(w, sizeof *w);
            w->codec = (uint8_t)c;
            w->nid = (uint8_t)(w0 + i);
            if (w0 + i > 0xffu) return CK_HDA_E_WIDGETS;
            rc = param(h, c, w->nid, P_WCAPS, &w->caps);
            if (rc) return rc;
            w->type = (uint8_t)((w->caps >> 20) & 0xfu);
            if (w->type == CK_HDA_W_PIN) {
                rc = param(h, c, w->nid, P_PINCAP, &w->pincap);
                if (!rc) rc = ck_hda_cmd(h, ck_hda_verb12(c, w->nid, V_GET_PINCFG, 0), &w->pincfg);
                if (rc) return rc;
            }
            if (w->type == CK_HDA_W_OUT || w->type == CK_HDA_W_IN) {
                if (w->caps & 1u) { /* format override: own rates and formats */
                    rc = param(h, c, w->nid, P_RATES, &w->rates);
                    if (!rc) rc = param(h, c, w->nid, P_BITS, &w->bits);
                    if (rc) return rc;
                } else {
                    w->rates = afg_rates;
                    w->bits = afg_bits;
                }
            }
            if (w->caps & (1u << 2)) { /* output amp present */
                if (w->caps & (1u << 3)) { /* amp parameter override */
                    rc = param(h, c, w->nid, P_AMP_OUT, &w->amp_out);
                    if (rc) return rc;
                } else {
                    w->amp_out = afg_amp;
                }
            }
            if (w->caps & (1u << 8)) { /* connection list present */
                rc = read_conn(h, w);
                if (rc) return rc;
            }
            h->nwidgets++;
        }
        k->wn = (uint16_t)(h->nwidgets - k->wfirst);
    }
    return found ? CK_HDA_OK : CK_HDA_E_NOCODEC;
}

/* ---- output path ---- */
static ck_hda_widget *find_w(ck_hda *h, unsigned codec, unsigned nid)
{
    const ck_hda_codec *k = &h->codec[codec];
    for (unsigned i = 0; i < k->wn; i++)
        if (h->w[k->wfirst + i].nid == nid) return &h->w[k->wfirst + i];
    return 0;
}

/* Depth-first from widget w toward an output converter; fills path[depth..]. */
static int walk(ck_hda *h, ck_hda_widget *w, unsigned depth, uint8_t path[4], uint8_t sel[4])
{
    path[depth] = w->nid;
    if (w->type == CK_HDA_W_OUT) {
        h->npath = (uint8_t)(depth + 1);
        return 1;
    }
    if (depth >= 3) return 0;
    for (unsigned i = 0; i < w->nconn; i++) {
        ck_hda_widget *n = find_w(h, w->codec, w->conn[i]);
        if (!n) continue;
        if (n->type != CK_HDA_W_OUT && n->type != CK_HDA_W_MIX && n->type != CK_HDA_W_SEL) continue;
        sel[depth] = (uint8_t)i;
        if (walk(h, n, depth + 1, path, sel)) return 1;
    }
    return 0;
}

/* Lower is better: speaker, line out, headphone, then digital outputs. */
static int pin_rank(const ck_hda_widget *w)
{
    unsigned conn = (w->pincfg >> 30) & 3u, dev = (w->pincfg >> 20) & 0xfu;
    if (w->type != CK_HDA_W_PIN || !(w->pincap & (1u << 4)) || conn == 1u) return -1;
    switch (dev) {
    case 1: return 0; /* speaker */
    case 0: return 1; /* line out */
    case 2: return 2; /* headphone out */
    case 4: case 5: return 3; /* S/PDIF out, other digital out (HDMI/DP) */
    default: return 9;
    }
}

int ck_hda_pick_output(ck_hda *h)
{
    int best = 99;
    h->out_codec = -1;
    for (unsigned c = 0; c < CK_HDA_MAX_CODECS; c++) {
        const ck_hda_codec *k = &h->codec[c];
        if (!k->present || !k->afg) continue;
        for (unsigned i = 0; i < k->wn; i++) {
            ck_hda_widget *w = &h->w[k->wfirst + i];
            int rank = pin_rank(w);
            if (rank < 0 || rank >= best) continue;
            uint8_t path[4] = { 0 }, sel[4] = { 0 };
            if (!walk(h, w, 0, path, sel)) continue;
            best = rank;
            h->out_codec = (int)c;
            h->pin = w->nid;
            h->dac = path[h->npath - 1];
            for (unsigned p = 0; p < 4; p++) h->path[p] = path[p], h->sel[p] = sel[p];
        }
    }
    return h->out_codec >= 0 ? CK_HDA_OK : CK_HDA_E_NOPATH;
}

/* ---- stream ---- */
static uint32_t sd_off(const ck_hda *h) { return SD_BASE + SD_STRIDE * h->sd; }

static int vcmd(ck_hda *h, uint32_t verb) { return ck_hda_cmd(h, verb, 0); }

static int stream_reset(ck_hda *h)
{
    uint32_t o = sd_off(h) + SD_CTL;
    w8(h, o, SD_SRST);
    if (wait_bits(h, o, 1, SD_SRST, SD_SRST)) return CK_HDA_E_STREAM;
    w8(h, o, 0);
    if (wait_bits(h, o, 1, SD_SRST, 0)) return CK_HDA_E_STREAM;
    return CK_HDA_OK;
}

int ck_hda_play_tone(ck_hda *h, unsigned freq_hz, unsigned rate_hz, int amp)
{
    if (h->out_codec < 0) return CK_HDA_E_NOPATH;
    if (h->oss == 0) return CK_HDA_E_NOSTREAM;
    unsigned c = (unsigned)h->out_codec;
    const ck_hda_codec *k = &h->codec[c];
    ck_hda_widget *dac = find_w(h, c, h->dac), *pin = find_w(h, c, h->pin);
    uint16_t fmt = ck_hda_format(CK_TEST_HDA_MUT == 3 ? 44100u : rate_hz, 16, 2);
    if (!dac || !pin || fmt == 0xffffu || !(dac->rates & ck_hda_rate_bit(rate_hz)) || !(dac->bits & (1u << 17)) ||
        !(dac->bits & 1u))
        return CK_HDA_E_FORMAT;
    h->sd = h->iss; /* first output stream descriptor */
    int rc = vcmd(h, ck_hda_verb12(c, k->afg, V_SET_POWER, 0));
    ck_udelay(10000); /* power state settle (7.3.3.10): the longest the spec allows is far below this */
    for (unsigned i = 0; i < h->npath && !rc; i++) {
        ck_hda_widget *w = find_w(h, c, h->path[i]);
        if (!w) return CK_HDA_E_NOPATH;
        rc = vcmd(h, ck_hda_verb12(c, w->nid, V_SET_POWER, 0));
        if (!rc && i + 1 < h->npath && w->nconn > 1) rc = vcmd(h, ck_hda_verb12(c, w->nid, V_SET_CONN_SEL, h->sel[i]));
        if (!rc && (w->caps & (1u << 2))) /* unmute at the 0 dB step */
            rc = vcmd(h, ck_hda_verb4(c, w->nid, V4_SET_AMP, 0xb000u | (w->amp_out & 0x7fu)));
    }
    if (!rc) rc = vcmd(h, ck_hda_verb12(c, h->pin, V_SET_PIN_CTL, 0x40u | (((pin->pincfg >> 20) & 0xfu) == 2u ? 0x80u : 0)));
    if (!rc && (pin->pincap & (1u << 16))) rc = vcmd(h, ck_hda_verb12(c, h->pin, V_SET_EAPD, 0x2u));
    if (!rc && (dac->caps & (1u << 9))) rc = vcmd(h, ck_hda_verb12(c, h->dac, V_SET_DIGI1, 0x1u)); /* digital converter on */
    if (!rc) rc = vcmd(h, ck_hda_verb4(c, h->dac, V4_SET_FORMAT, fmt));
    if (!rc) rc = vcmd(h, ck_hda_verb12(c, h->dac, V_SET_STREAM, (CK_TEST_HDA_MUT == 1 ? 2u : TAG) << 4));
    if (rc) return rc;

    rc = stream_reset(h);
    if (rc) return rc;
    uint32_t o = sd_off(h);
    uint8_t *buf = h->mem + 8192u;
    uint64_t bphys = h->phys + 8192u;
    ck_hda_tone_fill((int16_t *)(void *)buf, CK_HDA_BUF_BYTES / 4u, 2, freq_hz, rate_hz, amp);
    uint32_t *bdl = (uint32_t *)(void *)(h->mem + 4096u);
    for (unsigned i = 0; i < CK_HDA_BDL_ENTRIES; i++) { /* 3.6.3: address, length, IOC */
        uint64_t a = bphys + (uint64_t)i * (CK_HDA_BUF_BYTES / CK_HDA_BDL_ENTRIES);
        bdl[i * 4u + 0] = (uint32_t)a;
        bdl[i * 4u + 1] = (uint32_t)(a >> 32);
        bdl[i * 4u + 2] = CK_HDA_BUF_BYTES / CK_HDA_BDL_ENTRIES;
        bdl[i * 4u + 3] = 0;
    }
    ck_mb();
    w8(h, o + SD_CTL + 2, TAG << 4);
    w8(h, o + SD_STS, SD_STS_CLEAR);
    w32(h, o + SD_CBL, CK_TEST_HDA_MUT == 2 ? CK_HDA_BUF_BYTES / 2 : CK_HDA_BUF_BYTES);
    w16(h, o + SD_LVI, CK_HDA_BDL_ENTRIES - 1u);
    w16(h, o + SD_FMT, fmt);
    w32(h, o + SD_BDLPL, (uint32_t)(h->phys + 4096u));
    w32(h, o + SD_BDLPU, (uint32_t)((h->phys + 4096u) >> 32));
    h->cbl = CK_HDA_BUF_BYTES;
    h->lpib_last = 0;
    h->laps = 0;
    w8(h, o + SD_CTL, SD_RUN);
    if (wait_bits(h, o + SD_CTL, 1, SD_RUN, SD_RUN)) {
        (void)ck_hda_stop(h);
        return CK_HDA_E_STREAM;
    }
    h->streaming = 1;
    return CK_HDA_OK;
}

uint64_t ck_hda_position(ck_hda *h)
{
    uint32_t lpib = r32(h, sd_off(h) + SD_LPIB);
    if (lpib < h->lpib_last) h->laps++;
    h->lpib_last = lpib;
    return h->laps * h->cbl + lpib;
}

int ck_hda_stop(ck_hda *h)
{
    int rc = CK_HDA_OK;
    if (h->out_codec >= 0 && h->streaming) {
        unsigned c = (unsigned)h->out_codec;
        (void)vcmd(h, ck_hda_verb12(c, h->dac, V_SET_STREAM, 0));
        (void)vcmd(h, ck_hda_verb12(c, h->pin, V_SET_PIN_CTL, 0));
    }
    if (h->streaming || (h->oss && (r8(h, sd_off(h) + SD_CTL) & SD_RUN))) {
        uint32_t o = sd_off(h) + SD_CTL;
        w8(h, o, r8(h, o) & ~SD_RUN);
        if (wait_bits(h, o, 1, SD_RUN, 0)) rc = CK_HDA_E_STREAM;
        if (stream_reset(h)) rc = CK_HDA_E_STREAM;
    }
    h->streaming = 0;
    return rc;
}

int ck_hda_shutdown(ck_hda *h)
{
    int rc = ck_hda_stop(h);
    stop_engines(h);
    if (CK_TEST_HDA_MUT == 4) return rc;
    w32(h, R_GCTL, r32(h, R_GCTL) & ~GCTL_CRST);
    if (wait_bits(h, R_GCTL, 4, GCTL_CRST, 0)) rc = CK_HDA_E_RESET;
    return rc;
}
