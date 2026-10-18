/* hda_x86.c -- the sound controller (Intel High Definition Audio): a first
 * look, nothing played yet.
 *
 * The firmware may use this controller itself (a startup chime) and leave its
 * command rings (CORB, RIRB) or a stream's DMA running into memory the kernel
 * now owns, so the look first says what it found running and stops it. Then
 * it resets the controller (GCTL.CRST to 0, then 1), waits up to 10 ms for
 * the codecs to report in (STATESTS: one bit per codec address; the local
 * register headers are silent on the settle time, so it polls), and asks each
 * codec, through the immediate command registers (IC, IR, IRS: one command at
 * a time, no DMA), for its maker and model (root node parameter 0), its
 * revision (parameter 2), and which of its function groups is the audio one
 * (parameters 4 and 5). Register offsets and bits: Linux's
 * include/sound/hda_register.h and hda_verbs.h (docs-read record). Bus
 * mastering is left off: nothing here needs DMA. */
#include "../core/ck_internal.h"
#include "x86.h"

#define GCAP 0x00
#define VMIN 0x02
#define VMAJ 0x03
#define GCTL 0x08
#define STATESTS 0x0e
#define CORBCTL 0x4c
#define RIRBCTL 0x5c
#define IC 0x60
#define IR 0x64
#define IRS 0x68
#define IRS_BUSY 1u
#define IRS_VALID 2u
#define SD0 0x80 /* stream descriptors, 0x20 apart: input, output, then bidirectional */
#define RUN 2u   /* CORBCTL, RIRBCTL and SD_CTL: the DMA engine runs */

static volatile uint8_t *g_bar;
static uint64_t g_hz;

static uint8_t r8(unsigned o) { return g_bar[o]; }
static uint16_t r16(unsigned o) { return *(volatile uint16_t *)(g_bar + o); }
static uint32_t r32(unsigned o) { return *(volatile uint32_t *)(g_bar + o); }
static void w8(unsigned o, uint8_t v) { g_bar[o] = v; }
static void w16(unsigned o, uint16_t v) { *(volatile uint16_t *)(g_bar + o) = v; }
static void w32(unsigned o, uint32_t v) { *(volatile uint32_t *)(g_bar + o) = v; }

/* waits up to ms for (reg & mask) == want; 0 when it got there */
static int wait8(unsigned o, uint8_t mask, uint8_t want, unsigned ms)
{
    uint64_t t0 = ck_rdtsc();
    while ((r8(o) & mask) != want)
        if (ck_rdtsc() - t0 > g_hz / 1000 * ms)
            return -1;
        else
            ck_pause();
    return 0;
}

/* one codec command through the immediate registers; 0 with *out set, or -1 */
static int cmd(unsigned addr, unsigned nid, unsigned verb, unsigned payload, uint32_t *out)
{
    uint64_t t0 = ck_rdtsc();
    while (r16(IRS) & IRS_BUSY)
        if (ck_rdtsc() - t0 > g_hz / 1000)
            return -1;
    w16(IRS, (uint16_t)(r16(IRS) | IRS_VALID)); /* clears an old answer */
    w32(IC, (uint32_t)addr << 28 | (uint32_t)nid << 20 | (uint32_t)verb << 8 | (payload & 0xffu));
    w16(IRS, (uint16_t)(r16(IRS) | IRS_BUSY));
    t0 = ck_rdtsc();
    while (!(r16(IRS) & IRS_VALID))
        if (ck_rdtsc() - t0 > g_hz / 1000)
            return -1;
    *out = r32(IR);
    return 0;
}

static int param(unsigned addr, unsigned nid, unsigned p, uint32_t *out) { return cmd(addr, nid, 0xf00, p, out); }

void ck_x86_hda_look(unsigned ctrl, volatile uint8_t *bar, uint64_t tsc_hz)
{
    g_bar = bar;
    g_hz = tsc_hz;
    uint16_t gcap = r16(GCAP);
    unsigned iss = gcap >> 8 & 15u, oss = gcap >> 12 & 15u, bss = gcap >> 3 & 31u, nsd = iss + oss + bss;
    unsigned running = 0, corb = r8(CORBCTL) & RUN, rirb = r8(RIRBCTL) & RUN;
    for (unsigned i = 0; i < nsd; i++)
        running += (r8(SD0 + 0x20 * i) & RUN) != 0;
    ck_printf("sound: ctrl=%u version=%u.%u streams out=%u in=%u both=%u; left by the firmware: in_reset=%u "
              "command_rings=%s streams_running=%u\n", ctrl, r8(VMAJ), r8(VMIN), oss, iss, bss, !(r32(GCTL) & 1),
              corb || rirb ? "running" : "stopped", running);
    int stuck = 0;
    for (unsigned i = 0; i < nsd; i++)
        if (r8(SD0 + 0x20 * i) & RUN) {
            w8(SD0 + 0x20 * i, (uint8_t)(r8(SD0 + 0x20 * i) & ~RUN));
            stuck |= wait8(SD0 + 0x20 * i, RUN, 0, 10);
        }
    if (corb) {
        w8(CORBCTL, (uint8_t)(r8(CORBCTL) & ~RUN));
        stuck |= wait8(CORBCTL, RUN, 0, 10);
    }
    if (rirb) {
        w8(RIRBCTL, (uint8_t)(r8(RIRBCTL) & ~RUN));
        stuck |= wait8(RIRBCTL, RUN, 0, 10);
    }
    if (corb || rirb || running)
        ck_printf("sound: ctrl=%u the firmware's DMA %s\n", ctrl, stuck ? "did NOT stop within 10 ms" : "stopped");
    w32(GCTL, r32(GCTL) & ~1u); /* into reset */
    if (wait8(GCTL, 1, 0, 10)) {
        ck_printf("sound: ctrl=%u did not enter reset\n", ctrl);
        return;
    }
    w32(GCTL, r32(GCTL) | 1u); /* out of reset */
    if (wait8(GCTL, 1, 1, 10)) {
        ck_printf("sound: ctrl=%u did not leave reset\n", ctrl);
        return;
    }
    uint64_t t0 = ck_rdtsc();
    while (!r16(STATESTS) && ck_rdtsc() - t0 < tsc_hz / 100)
        ck_pause();
    uint16_t codecs = r16(STATESTS) & 0x7fffu;
    ck_printf("sound: ctrl=%u codecs answering: mask=0x%x after %llu us\n", ctrl, codecs,
              (unsigned long long)((ck_rdtsc() - t0) * 1000000 / tsc_hz));
    for (unsigned a = 0; a < 15; a++) {
        if (!(codecs >> a & 1))
            continue;
        uint32_t id, rev, nodes, audio = 0;
        if (param(a, 0, 0x00, &id) || param(a, 0, 0x02, &rev) || param(a, 0, 0x04, &nodes)) {
            ck_printf("sound: codec=%u no answer to the immediate command\n", a);
            continue;
        }
        unsigned first = nodes >> 16 & 0xffu, n = nodes & 0xffu;
        for (unsigned k = 0; k < n && k < 8; k++) {
            uint32_t type;
            if (!param(a, first + k, 0x05, &type) && (type & 0xffu) == 1)
                audio = first + k;
        }
        ck_printf("sound: codec=%u id=%04x:%04x rev=0x%08x function_groups=%u audio_group_node=%u\n", a, id >> 16,
                  id & 0xffffu, rev, n, audio);
    }
}
