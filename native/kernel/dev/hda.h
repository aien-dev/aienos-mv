/* hda.h -- Intel High Definition Audio controller driver for the AIENOS C
 * kernel (L207): controller reset, CORB/RIRB verb ring, codec and widget
 * enumeration, output path discovery, one cyclic output stream, and an
 * integer sine tone. Polled only (no interrupt, no scheduler). Freestanding.
 *
 * Reference: Intel High Definition Audio Specification Rev. 1.0a (local copy
 * ~/docs/imac-hardware/txt/intel-hda-spec-1.0a.txt; section numbers below are
 * that document's). The Spark's audio is the NVIDIA HDA controller (ACPI
 * NVDA2014, MMIO, one HDMI/DP codec "Nvidia SoC 33", vendor id 0x10de0033,
 * measured read-only under Linux 2026-10-20); QEMU's intel-hda + hda-output is
 * the gate. Linux sound/hda was read for behaviour only (GPL, nothing copied).
 *
 * Register and memory access go through ck_hda_bus so the host tests can put a
 * software model of the controller and codec behind it. DMA memory is one
 * region the caller allocated and (on the kernel) confined with the SMMU; the
 * driver never DMAs outside it.
 *
 * QEMU is not hardware: nothing here is physically qualified on the Spark. */
#ifndef AIENOS_CK_HDA_H
#define AIENOS_CK_HDA_H
#include <stddef.h>
#include <stdint.h>

/* DMA region: page 0 CORB (1 KiB) + RIRB (2 KiB), page 1 BDL, pages 2-4 the
 * 12 KiB cyclic tone buffer (a multiple of 128 bytes, see CK_HDA_BDL_ENTRIES). */
#define CK_HDA_DMA_BYTES (32u * 1024u)
#define CK_HDA_MAX_CODECS 15u   /* STATESTS bits 14:0 */
#define CK_HDA_MAX_WIDGETS 96u  /* across all codecs; more is refused, not truncated */
#define CK_HDA_MAX_CONN 16u
#define CK_HDA_BDL_ENTRIES 2u
#define CK_HDA_BUF_BYTES (12u * 1024u)
#define CK_HDA_TONE_RATE 48000u
#define CK_HDA_TONE_HZ 1000u

enum {
    CK_HDA_OK = 0,
    CK_HDA_E_ARG = -701,      /* bad argument, region, or controller version */
    CK_HDA_E_RESET = -702,    /* controller would not enter or leave reset */
    CK_HDA_E_NOCODEC = -703,  /* STATESTS names no codec */
    CK_HDA_E_RING = -704,     /* CORB/RIRB would not start or reset */
    CK_HDA_E_VERB = -705,     /* a verb got no solicited response in time */
    CK_HDA_E_WIDGETS = -706,  /* more than CK_HDA_MAX_WIDGETS, or a malformed node tree */
    CK_HDA_E_NOPATH = -707,   /* no output pin reaches an output converter */
    CK_HDA_E_FORMAT = -708,   /* the converter supports no 16-bit 48 kHz stereo PCM */
    CK_HDA_E_STREAM = -709,   /* the stream engine would not reset, start or stop */
    CK_HDA_E_DMA64 = -710,    /* controller is not 64-bit capable and the region is above 4 GiB */
    CK_HDA_E_NOSTREAM = -711, /* GCAP reports no output stream */
};

/* Register access seam. width is 1, 2 or 4 bytes. */
typedef struct {
    uint32_t (*rd)(void *ctx, uint32_t off, unsigned width);
    void (*wr)(void *ctx, uint32_t off, unsigned width, uint32_t v);
    void *ctx;
} ck_hda_bus;

/* Widget types (audio widget capabilities, 7.3.4.6, bits 23:20). */
enum { CK_HDA_W_OUT = 0, CK_HDA_W_IN = 1, CK_HDA_W_MIX = 2, CK_HDA_W_SEL = 3, CK_HDA_W_PIN = 4, CK_HDA_W_POWER = 5,
       CK_HDA_W_KNOB = 6, CK_HDA_W_BEEP = 7, CK_HDA_W_VENDOR = 15 };

typedef struct {
    uint8_t codec, nid, type;
    uint32_t caps;    /* audio widget capabilities (param 0x09) */
    uint32_t pincap;  /* pin capabilities (param 0x0c), pins only */
    uint32_t pincfg;  /* configuration default (verb f1c), pins only */
    uint32_t rates;   /* supported PCM sizes/rates (param 0x0a), converters only */
    uint32_t bits;    /* supported stream formats (param 0x0b), converters only */
    uint32_t amp_out; /* output amp capabilities (param 0x12) or 0 */
    uint8_t nconn;
    uint16_t conn[CK_HDA_MAX_CONN]; /* connection list, ranges expanded */
} ck_hda_widget;

typedef struct {
    uint8_t present;
    uint32_t vendor, revision, subsystem; /* params 0x00, 0x02 and verb f20 on the AFG */
    uint8_t afg;            /* first audio function group, 0 when none */
    uint16_t wfirst, wn;    /* widget index range in ck_hda.w */
} ck_hda_codec;

typedef struct {
    ck_hda_bus bus;
    uint8_t *mem;       /* DMA region, CK_HDA_DMA_BYTES, 4 KiB aligned */
    uint64_t phys;      /* its bus address */
    uint16_t gcap;
    uint8_t vmaj, vmin;
    unsigned iss, oss, bss; /* input, output, bidirectional stream counts */
    unsigned corb_n, rirb_n, corb_wp, rirb_rp;
    uint16_t statests;  /* codecs that answered the reset */
    unsigned unsol;     /* unsolicited responses seen and dropped */
    unsigned nwidgets;
    ck_hda_codec codec[CK_HDA_MAX_CODECS];
    ck_hda_widget w[CK_HDA_MAX_WIDGETS];
    /* chosen output path */
    int out_codec;      /* -1: none */
    uint8_t pin, dac;   /* pin widget and output converter */
    uint8_t path[4], npath; /* nids from the pin to the converter, pin first */
    uint8_t sel[4];     /* connection index chosen at each path widget (pin first) */
    unsigned sd;        /* output stream descriptor index (ISS + 0) */
    int streaming;
    uint32_t cbl, lpib_last; /* cyclic buffer length in bytes; last LPIB read */
    uint64_t laps;
} ck_hda;

/* ---- pure helpers (no hardware) ---- */
/* Verb word, 4.4.1: codec 31:28, node 27:20, 12-bit verb + 8-bit payload or
 * 4-bit verb + 16-bit payload in 19:0. */
uint32_t ck_hda_verb12(unsigned codec, unsigned nid, unsigned verb, unsigned payload8);
uint32_t ck_hda_verb4(unsigned codec, unsigned nid, unsigned verb4, unsigned payload16);
/* Stream format word, 3.7.1: PCM, base 48 kHz, mult/div 1, 16 bits, channels. */
uint16_t ck_hda_format(unsigned rate_hz, unsigned bits, unsigned channels);
/* Rate bit in the supported-rates parameter for rate_hz, or 0. */
uint32_t ck_hda_rate_bit(unsigned rate_hz);
/* Fill `frames` frames of `channels` 16-bit samples with a sine of freq_hz at
 * rate_hz and amplitude amp (0..32767), phase 0 at frame 0, integer only (a
 * 65-point quarter-wave table, linear interpolation). Frame i uses the exact
 * phase i*freq/rate, so a buffer holding a whole number of periods loops
 * seamlessly. */
void ck_hda_tone_fill(int16_t *pcm, unsigned frames, unsigned channels, unsigned freq_hz, unsigned rate_hz, int amp);
/* Name of a widget type, for the enumeration lines. */
const char *ck_hda_wtype_name(unsigned type);

/* ---- controller ---- */
/* Reset the controller (stops the engines first), read GCAP and the version,
 * set up CORB and RIRB in `mem`, read STATESTS. 0 or a CK_HDA_E_*. The region
 * must be inside what the SMMU window allows; the caller owns that. */
int ck_hda_init(ck_hda *h, ck_hda_bus bus, uint8_t *mem, uint64_t phys);
/* One verb to a codec: waits for its solicited response (unsolicited ones are
 * counted and dropped). 0 or CK_HDA_E_VERB. */
int ck_hda_cmd(ck_hda *h, uint32_t verb, uint32_t *resp);
/* Probe every codec STATESTS names: root, function groups, widgets of the
 * first audio function group. 0 or an error; CK_HDA_E_NOCODEC when none. */
int ck_hda_enumerate(ck_hda *h);
/* Choose the output path: the best output pin (speaker, line out, headphone,
 * then digital) that reaches an output converter through at most 4 widgets.
 * Sets out_codec/pin/dac/path. CK_HDA_E_NOPATH when there is none. */
int ck_hda_pick_output(ck_hda *h);
/* Program the path (power D0, amps, pin enable, converter format and stream
 * tag 1), the stream descriptor, the BDL and the sine tone, then start the
 * DMA engine. 0 or an error; on an error the engine is left stopped. */
int ck_hda_play_tone(ck_hda *h, unsigned freq_hz, unsigned rate_hz, int amp);
/* Link position since play_tone started, in bytes (wraps counted): the laps of
 * the cyclic buffer plus SDnLPIB. Call at least once per lap of the buffer. */
uint64_t ck_hda_position(ck_hda *h);
/* Stop the stream (RUN=0, wait, stream reset). Idempotent. */
int ck_hda_stop(ck_hda *h);
/* Leave the controller in reset with every DMA engine stopped (the revoke
 * order the fence needs before bus mastering goes off). Idempotent. */
int ck_hda_shutdown(ck_hda *h);
#endif
