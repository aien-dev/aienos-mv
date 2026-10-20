/* xhci_pipe.c -- generic polled xHCI pipes (see xhci_pipe.h). Freestanding.
 * Register and structure layouts: xHCI 1.2c (5.3-5.6 registers, 6.2 contexts,
 * 6.4 TRBs, 6.5 event ring segment table); USB 2.0 chapter 9. Written in the
 * style of usb_kbd.c, which it does not replace. */
#include "xhci_pipe.h"
#include "ck.h"

#define PG 4096u
#define RING_N 256u  /* TRBs in a one-page ring */
#define SMALL_N 64u  /* TRBs in a quarter-page endpoint ring */

/* DMA region layout (pages). Page P_SMALL holds three 64-TRB rings (1 KiB each)
 * and the interrupt IN buffer in its last quarter. */
enum { P_DCBAA = 0, P_DEVCTX, P_INCTX, P_CMD, P_EVT, P_ERST, P_EP0, P_SMALL, P_BUF, P_SP0 };
#define ERST_OFF 0x000u
#define SPARRAY_OFF 0x400u
#define SMALL_RING_OFF(i) ((size_t)(i) * 0x400u)
#define INTR_BUF_OFF 0xc00u
#define SETUP_AREA 64u /* head of the bounce buffer reserved for control bookkeeping */

/* Capability registers. */
#define CAP_LENGTH 0x00u
#define CAP_HCSPARAMS1 0x04u
#define CAP_HCSPARAMS2 0x08u
#define CAP_HCCPARAMS1 0x10u
#define CAP_DBOFF 0x14u
#define CAP_RTSOFF 0x18u
/* Operational registers. */
#define OP_USBCMD 0x00u
#define OP_USBSTS 0x04u
#define OP_PAGESIZE 0x08u
#define OP_CRCR 0x18u
#define OP_DCBAAP 0x30u
#define OP_CONFIG 0x38u
#define OP_PORTSC(p) (0x400u + 0x10u * ((p) - 1u))
#define CMD_RS 0x1u
#define CMD_HCRST 0x2u
#define STS_HCH 0x1u
#define STS_HSE 0x4u
#define STS_CNR 0x800u
#define IR0_ERSTSZ 0x28u
#define IR0_ERSTBA 0x30u
#define IR0_ERDP 0x38u
/* PORTSC */
#define PS_CCS 0x1u
#define PS_PED 0x2u
#define PS_PR 0x10u
#define PS_PP 0x200u
#define PS_PRC 0x200000u
#define PS_CHANGES 0x00fe0000u
#define PS_KEEP (PS_PP | 0x0e000000u)
/* TRB types */
#define T_NORMAL 1u
#define T_SETUP 2u
#define T_DATA 3u
#define T_STATUS 4u
#define T_LINK 6u
#define T_ENABLE_SLOT 9u
#define T_DISABLE_SLOT 10u
#define T_ADDRESS 11u
#define T_CONFIGURE 12u
#define T_EVALUATE 13u
#define T_EV_TRANSFER 32u
#define T_EV_COMMAND 33u
#define TRB_TYPE(t) ((uint32_t)(t) << 10)
#define TRB_IOC 0x20u
#define TRB_ISP 0x4u
#define TRB_IDT 0x40u
#define TRB_TC 0x2u
#define CC_SUCCESS 1u
#define CC_STALL 6u
#define CC_SHORT 13u
/* Endpoint types (context, Table 6-9) */
#define EPT_BULK_OUT 2u
#define EPT_INTR_OUT 3u
#define EPT_CONTROL 4u
#define EPT_BULK_IN 6u
#define EPT_INTR_IN 7u

static uint32_t rd(const ck_xp *x, uint32_t off) { return *(volatile uint32_t *)(x->base + off); }
static void wr(const ck_xp *x, uint32_t off, uint32_t v) { *(volatile uint32_t *)(x->base + off) = v; }
static void wr64(const ck_xp *x, uint32_t off, uint64_t v)
{
    wr(x, off, (uint32_t)v);
    wr(x, off + 4u, (uint32_t)(v >> 32));
}
static uint8_t *page(const ck_xp *x, unsigned p) { return x->mem + (size_t)p * PG; }
static uint64_t page_phys(const ck_xp *x, unsigned p) { return x->phys + (uint64_t)p * PG; }
static void zero(void *p, size_t n)
{
    for (size_t i = 0; i < n; i++) ((volatile uint8_t *)p)[i] = 0;
}

static int fail(ck_xp *x, int rc, const char *why)
{
    x->why = why;
    return rc;
}

#define WAIT_MS(ms, cond)                                                  \
    ({                                                                     \
        uint64_t t0_ = ck_time_us();                                       \
        int ok_ = 0;                                                       \
        for (;;) {                                                         \
            if (cond) { ok_ = 1; break; }                                  \
            if (ck_time_us() - t0_ > (uint64_t)(ms) * 1000u) break;        \
            ck_udelay(20);                                                 \
        }                                                                  \
        ok_;                                                               \
    })

static void ring_init(ck_xp_ring *r, uint8_t *at, uint64_t phys, uint32_t n)
{
    zero(at, (size_t)n * 16u);
    r->t = (volatile uint32_t *)at;
    r->phys = phys;
    r->i = 0;
    r->c = 1;
    r->n = n;
}

/* Producer side: control word (cycle bit) last; the last slot is a Link TRB
 * back to the start with Toggle Cycle. Returns the TRB's bus address. */
static uint64_t ring_push(ck_xp_ring *r, uint32_t p0, uint32_t p1, uint32_t st, uint32_t ctl)
{
    volatile uint32_t *e = r->t + r->i * 4u;
    uint64_t at = r->phys + (uint64_t)r->i * 16u;
    e[0] = p0;
    e[1] = p1;
    e[2] = st;
    ck_mb();
    e[3] = (ctl & ~1u) | r->c;
    if (++r->i == r->n - 1u) {
        volatile uint32_t *l = r->t + r->i * 4u;
        l[0] = (uint32_t)r->phys;
        l[1] = (uint32_t)(r->phys >> 32);
        l[2] = 0;
        ck_mb();
        l[3] = TRB_TYPE(T_LINK) | TRB_TC | r->c;
        r->i = 0;
        r->c ^= 1u;
    }
    return at;
}

static void doorbell(const ck_xp *x, unsigned target, uint32_t value)
{
    ck_mb();
    wr(x, x->db + 4u * target, value);
}

/* Event ring consumer: one event, or 0. */
static int ev_next(ck_xp *x, uint32_t ev[4])
{
    volatile uint32_t *e = (volatile uint32_t *)page(x, P_EVT) + x->ev_i * 4u;
    if ((e[3] & 1u) != x->ev_c) return 0;
    for (int i = 0; i < 4; i++) ev[i] = e[i];
    if (++x->ev_i == RING_N) {
        x->ev_i = 0;
        x->ev_c ^= 1u;
    }
    wr64(x, x->rt + IR0_ERDP, (page_phys(x, P_EVT) + (uint64_t)x->ev_i * 16u) | 0x8u /* EHB */);
    return 1;
}

/* Drain the event ring into the per-endpoint and command slots, so a transfer
 * event for one endpoint is never lost while another is being waited for.
 * Transfer events of other slots and port status events are dropped. */
static void pump(ck_xp *x)
{
    uint32_t ev[4];
    while (ev_next(x, ev)) {
        uint32_t type = (ev[3] >> 10) & 0x3fu;
        uint32_t code = ev[2] >> 24;
        uint64_t ptr = (uint64_t)ev[0] | ((uint64_t)ev[1] << 32);
        if (type == T_EV_COMMAND) {
            x->cmd_done.ptr = ptr;
            x->cmd_done.code = (uint8_t)code;
            x->cmd_done.resid = ev[3] >> 24; /* slot id of an Enable Slot completion */
            x->cmd_done.valid = 1;
        } else if (type == T_EV_TRANSFER && (ev[3] >> 24) == x->slot && x->slot) {
            ck_xp_done *d = &x->done[(ev[3] >> 16) & 0x1fu];
            /* An error on an earlier TRB of a TD stays visible: it is not overwritten by a later success. */
            if (d->valid && d->code != CC_SUCCESS && d->code != CC_SHORT) continue;
            d->ptr = ptr;
            d->code = (uint8_t)code;
            d->resid = ev[2] & 0xffffffu;
            d->valid = 1;
        }
    }
}

static int command(ck_xp *x, uint32_t p0, uint32_t p1, uint32_t ctl, uint32_t *slot_out, const char *step)
{
    x->cmd_done.valid = 0;
    uint64_t at = ring_push(&x->cmd, p0, p1, 0, ctl);
    doorbell(x, 0, 0);
    uint64_t t0 = ck_time_us();
    while (ck_time_us() - t0 < 1000000u) {
        pump(x);
        if (x->cmd_done.valid && x->cmd_done.ptr == at) {
            x->last_code = x->cmd_done.code;
            if (slot_out) *slot_out = x->cmd_done.resid;
            return x->cmd_done.code == CC_SUCCESS ? 0 : fail(x, CK_XP_E_FAILED, step);
        }
        ck_udelay(20);
    }
    return fail(x, CK_XP_E_TIMEOUT, step);
}

/* Wait for the completion of the TRB at `at` on device context index dci. */
static int transfer_wait(ck_xp *x, unsigned dci, uint64_t at, uint32_t ms, uint32_t *resid, const char *step)
{
    ck_xp_done *d = &x->done[dci];
    uint64_t t0 = ck_time_us();
    for (;;) {
        pump(x);
        if (d->valid) {
            uint32_t cc = d->code;
            x->last_code = cc;
            if (cc == CC_STALL) { d->valid = 0; return fail(x, CK_XP_E_STALL, step); }
            if (cc != CC_SUCCESS && cc != CC_SHORT) { d->valid = 0; return fail(x, CK_XP_E_FAILED, step); }
            if (d->ptr == at) {
                if (resid) *resid = d->resid;
                d->valid = 0;
                return 0;
            }
            d->valid = 0; /* success on an earlier TRB of the TD: keep waiting for the last */
        }
        if (ck_time_us() - t0 >= (uint64_t)ms * 1000u) break;
        ck_udelay(20);
    }
    return fail(x, CK_XP_E_TIMEOUT, step);
}

static volatile uint32_t *ictx(const ck_xp *x, unsigned index)
{
    return (volatile uint32_t *)(page(x, P_INCTX) + (size_t)index * x->ctx);
}

static void ep0_context(ck_xp *x, uint32_t add, unsigned entries)
{
    zero(page(x, P_INCTX), PG);
    ictx(x, 0)[1] = add;
    ictx(x, 1)[0] = ((uint32_t)x->speed << 20) | ((uint32_t)entries << 27);
    ictx(x, 1)[1] = (uint32_t)x->port << 16;
    volatile uint32_t *ep = ictx(x, 2);
    ep[1] = (3u << 1) | (EPT_CONTROL << 3) | ((uint32_t)x->mps0 << 16);
    uint64_t dq = x->ep0.phys + (uint64_t)x->ep0.i * 16u;
    ep[2] = (uint32_t)dq | x->ep0.c;
    ep[3] = (uint32_t)(dq >> 32);
    ep[4] = 8u;
}

static uint32_t intr_interval(uint8_t speed, uint8_t b)
{
    if (speed >= 3) { /* high / super speed: 2^(bInterval-1) microframes */
        uint32_t v = b ? (uint32_t)b - 1u : 0u;
        return v > 15u ? 15u : v;
    }
    uint32_t f = b ? b : 1u, e = 0; /* full / low speed: bInterval frames of 1 ms = 8 microframes each */
    while ((1u << (e + 1u)) <= f * 8u && e < 15u) e++;
    return e < 3u ? 3u : e > 10u ? 10u : e;
}

void ck_xp_stop(ck_xp *x)
{
    if (!x->base) return;
    wr(x, x->op + OP_USBCMD, rd(x, x->op + OP_USBCMD) & ~CMD_RS);
    (void)WAIT_MS(100, rd(x, x->op + OP_USBSTS) & STS_HCH);
}

unsigned ck_xp_max_ports(const ck_xp *x) { return x->max_ports; }

int ck_xp_port_present(ck_xp *x, unsigned port)
{
    return port >= 1 && port <= x->max_ports && (rd(x, x->op + OP_PORTSC(port)) & PS_CCS) != 0;
}

int ck_xp_start(ck_xp *x, volatile uint8_t *bar0, uint8_t *mem, uint64_t phys, size_t bytes)
{
    for (size_t i = 0; i < sizeof *x; i++) ((volatile uint8_t *)x)[i] = 0;
    x->base = bar0;
    x->mem = mem;
    x->phys = phys;
    x->why = "";
    if (!bar0 || !mem || bytes < (size_t)CK_XP_PAGES * PG || (phys & (PG - 1u)))
        return fail(x, CK_XP_E_ARG, "DMA region too small or unaligned");
    uint32_t cap = rd(x, CAP_LENGTH), hcs1 = rd(x, CAP_HCSPARAMS1), hcs2 = rd(x, CAP_HCSPARAMS2), hcc1 = rd(x, CAP_HCCPARAMS1);
    if (cap == 0xffffffffu || (cap & 0xffu) < 0x20u) return fail(x, CK_XP_E_ARG, "xHCI registers absent");
    x->op = cap & 0xffu;
    x->rt = rd(x, CAP_RTSOFF) & ~0x1fu;
    x->db = rd(x, CAP_DBOFF) & ~0x3u;
    x->max_ports = hcs1 >> 24;
    x->ctx = (hcc1 & 0x4u) ? 64u : 32u;
    uint32_t sp = ((hcs2 >> 27) & 0x1fu) | (((hcs2 >> 21) & 0x1fu) << 5);
    if (sp > CK_XP_MAX_SCRATCHPADS) return fail(x, CK_XP_E_UNSUP, "controller asks for more scratchpad pages than the window holds");
    if (!(hcc1 & 0x1u) && phys + bytes > 0x100000000ull) return fail(x, CK_XP_E_UNSUP, "DMA above 4 GiB on a 32-bit controller");
    if (!(rd(x, x->op + OP_PAGESIZE) & 0x1u)) return fail(x, CK_XP_E_UNSUP, "4 KiB pages not supported");
    /* A previous phase (the keyboard) may have left it running: stop it first. HCRST only with HCH = 1 (5.4.1). */
    ck_xp_stop(x);
    if (!(rd(x, x->op + OP_USBSTS) & STS_HCH)) return fail(x, CK_XP_E_TIMEOUT, "controller would not halt");
    wr(x, x->op + OP_USBCMD, CMD_HCRST);
    if (!WAIT_MS(1000, !(rd(x, x->op + OP_USBCMD) & CMD_HCRST) && !(rd(x, x->op + OP_USBSTS) & STS_CNR)))
        return fail(x, CK_XP_E_TIMEOUT, "controller reset");
    zero(mem, (size_t)CK_XP_PAGES * PG);
    if (sp) {
        volatile uint64_t *arr = (volatile uint64_t *)(page(x, P_ERST) + SPARRAY_OFF);
        for (uint32_t i = 0; i < sp; i++) arr[i] = page_phys(x, P_SP0 + i);
        ((volatile uint64_t *)page(x, P_DCBAA))[0] = page_phys(x, P_ERST) + SPARRAY_OFF;
    }
    ring_init(&x->cmd, page(x, P_CMD), page_phys(x, P_CMD), RING_N);
    x->ev_i = 0;
    x->ev_c = 1;
    volatile uint32_t *erst = (volatile uint32_t *)(page(x, P_ERST) + ERST_OFF);
    erst[0] = (uint32_t)page_phys(x, P_EVT);
    erst[1] = (uint32_t)(page_phys(x, P_EVT) >> 32);
    erst[2] = RING_N;
    erst[3] = 0;
    ck_mb();
    wr(x, x->op + OP_CONFIG, (rd(x, x->op + OP_CONFIG) & ~0xffu) | 1u);
    wr64(x, x->op + OP_DCBAAP, page_phys(x, P_DCBAA));
    wr64(x, x->op + OP_CRCR, page_phys(x, P_CMD) | 1u);
    wr(x, x->rt + IR0_ERSTSZ, 1);
    wr64(x, x->rt + IR0_ERDP, page_phys(x, P_EVT));
    wr64(x, x->rt + IR0_ERSTBA, page_phys(x, P_ERST) + ERST_OFF);
    wr(x, x->op + OP_USBCMD, CMD_RS);
    if (!WAIT_MS(100, !(rd(x, x->op + OP_USBSTS) & STS_HCH))) return fail(x, CK_XP_E_TIMEOUT, "controller run");
    if (rd(x, x->op + OP_USBSTS) & STS_HSE) return fail(x, CK_XP_E_FAILED, "host system error");
    x->started = 1;
    /* Connections are re-reported after the reset: give them a bounded moment. */
    (void)WAIT_MS(500, ({
                      int any_ = 0;
                      for (unsigned p_ = 1; p_ <= x->max_ports; p_++) any_ |= rd(x, x->op + OP_PORTSC(p_)) & PS_CCS;
                      any_;
                  }));
    return 0;
}

static int reset_port(ck_xp *x, uint8_t p)
{
    uint32_t off = x->op + OP_PORTSC(p);
    uint32_t s = rd(x, off);
    if (!(s & PS_PED)) {
        wr(x, off, (s & PS_KEEP) | PS_PR);
        if (!WAIT_MS(500, rd(x, off) & PS_PRC)) return fail(x, CK_XP_E_TIMEOUT, "port reset");
        ck_udelay(10000); /* USB 2.0 7.1.7.5 reset recovery, 10 ms */
    }
    s = rd(x, off);
    wr(x, off, (s & PS_KEEP) | (s & PS_CHANGES));
    if (!(s & PS_PED)) return fail(x, CK_XP_E_UNSUP, "port did not enable");
    x->speed = (uint8_t)((s >> 10) & 0xfu);
    return 0;
}

int ck_xp_control(ck_xp *x, uint8_t rtype, uint8_t req, uint16_t val, uint16_t idx, void *data, uint16_t len,
                  uint32_t *got, uint32_t timeout_ms)
{
    if (!x->slot) return fail(x, CK_XP_E_STATE, "control: no slot");
    if (len > CK_XP_BUF_BYTES - SETUP_AREA || (len && !data)) return fail(x, CK_XP_E_ARG, "control: length");
    int in = (rtype & 0x80u) != 0;
    uint8_t *buf = page(x, P_BUF) + SETUP_AREA;
    uint64_t bphys = page_phys(x, P_BUF) + SETUP_AREA;
    if (len && !in)
        for (uint16_t i = 0; i < len; i++) ((volatile uint8_t *)buf)[i] = ((const uint8_t *)data)[i];
    if (len && in) zero(buf, len);
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;
    x->done[1].valid = 0;
    ring_push(&x->ep0, (uint32_t)rtype | ((uint32_t)req << 8) | ((uint32_t)val << 16), (uint32_t)idx | ((uint32_t)len << 16), 8u,
              TRB_TYPE(T_SETUP) | TRB_IDT | (trt << 16));
    if (len) ring_push(&x->ep0, (uint32_t)bphys, (uint32_t)(bphys >> 32), len, TRB_TYPE(T_DATA) | (in ? (1u << 16) : 0u));
    uint64_t st = ring_push(&x->ep0, 0, 0, 0, TRB_TYPE(T_STATUS) | TRB_IOC | ((len && in) ? 0u : (1u << 16)));
    doorbell(x, x->slot, 1);
    int rc = transfer_wait(x, 1, st, timeout_ms ? timeout_ms : 1000u, 0, "control transfer");
    if (rc) return rc;
    if (len && in) {
        for (uint16_t i = 0; i < len; i++) ((uint8_t *)data)[i] = ((volatile uint8_t *)buf)[i];
        if (got) *got = len;
    } else if (got) {
        *got = 0;
    }
    return 0;
}

int ck_xp_attach(ck_xp *x, unsigned port)
{
    if (!x->started || port < 1 || port > x->max_ports || port > 255u) return fail(x, CK_XP_E_ARG, "attach: port");
    x->slot = 0;
    for (unsigned i = 0; i < 32; i++) x->done[i].valid = 0;
    int rc = reset_port(x, (uint8_t)port);
    if (rc) return rc;
    x->port = (uint8_t)port;
    uint32_t slot = 0;
    if ((rc = command(x, 0, 0, TRB_TYPE(T_ENABLE_SLOT), &slot, "Enable Slot"))) return rc;
    if (slot == 0 || slot > 255u) return fail(x, CK_XP_E_UNSUP, "slot id 0");
    x->slot = (uint8_t)slot;
    zero(page(x, P_DEVCTX), PG);
    ((volatile uint64_t *)page(x, P_DCBAA))[x->slot] = page_phys(x, P_DEVCTX);
    ring_init(&x->ep0, page(x, P_EP0), page_phys(x, P_EP0), RING_N);
    x->mps0 = x->speed == 4 ? 512u : x->speed == 3 ? 64u : 8u;
    ep0_context(x, 0x3u, 1);
    if ((rc = command(x, (uint32_t)page_phys(x, P_INCTX), (uint32_t)(page_phys(x, P_INCTX) >> 32),
                      TRB_TYPE(T_ADDRESS) | ((uint32_t)x->slot << 24), 0, "Address Device")))
        return rc;
    uint8_t d8[18];
    if ((rc = ck_xp_control(x, 0x80, 6, 0x0100, 0, d8, 8, 0, 1000))) return rc;
    if (d8[0] >= 8 && d8[1] == 1 && d8[7]) {
        uint16_t m = x->speed == 4 ? (uint16_t)(1u << (d8[7] > 9 ? 9 : d8[7])) : d8[7];
        if (m != x->mps0) {
            x->mps0 = m;
            ep0_context(x, 0x2u, 1);
            if ((rc = command(x, (uint32_t)page_phys(x, P_INCTX), (uint32_t)(page_phys(x, P_INCTX) >> 32),
                              TRB_TYPE(T_EVALUATE) | ((uint32_t)x->slot << 24), 0, "Evaluate Context")))
                return rc;
        }
    }
    if ((rc = ck_xp_control(x, 0x80, 6, 0x0100, 0, x->dev_desc, 18, 0, 1000))) return rc;
    if (x->dev_desc[0] != 18 || x->dev_desc[1] != 1) return fail(x, CK_XP_E_FAILED, "malformed device descriptor");
    return 0;
}

void ck_xp_detach(ck_xp *x)
{
    if (!x->slot) return;
    (void)command(x, 0, 0, TRB_TYPE(T_DISABLE_SLOT) | ((uint32_t)x->slot << 24), 0, "Disable Slot");
    x->slot = 0;
    x->ep_n = 0;
}

int ck_xp_config_desc(ck_xp *x, uint8_t index, uint8_t *out, size_t cap, size_t *len)
{
    uint8_t h[9];
    int rc = ck_xp_control(x, 0x80, 6, (uint16_t)(0x0200u | index), 0, h, 9, 0, 1000);
    if (rc) return rc;
    if (h[1] != 2 || h[0] != 9) return fail(x, CK_XP_E_FAILED, "malformed configuration descriptor");
    uint16_t total = (uint16_t)(h[2] | (h[3] << 8));
    if (total < 9 || total > CK_XP_BUF_BYTES - SETUP_AREA) return fail(x, CK_XP_E_UNSUP, "configuration descriptor size");
    if (total > cap) return fail(x, CK_XP_E_ARG, "configuration descriptor larger than the caller's buffer");
    rc = ck_xp_control(x, 0x80, 6, (uint16_t)(0x0200u | index), 0, out, total, 0, 1000);
    if (rc) return rc;
    if (len) *len = total;
    return 0;
}

static int ep_slot(const ck_xp *x, uint8_t addr)
{
    for (unsigned i = 0; i < x->ep_n; i++)
        if (x->ep_addr[i] == addr) return (int)i;
    return -1;
}

int ck_xp_configure(ck_xp *x, uint8_t configuration_value, const ck_xp_ep *eps, unsigned n)
{
    if (!x->slot) return fail(x, CK_XP_E_STATE, "configure: no slot");
    if (n == 0 || n > CK_XP_MAX_EPS || !eps) return fail(x, CK_XP_E_ARG, "configure: endpoints");
    zero(page(x, P_INCTX), PG);
    uint32_t add = 1u, max_dci = 1;
    for (unsigned i = 0; i < n; i++) {
        uint8_t num = eps[i].addr & 0x0fu;
        int in = (eps[i].addr & 0x80u) != 0;
        if (num == 0 || (eps[i].type != CK_XP_EP_BULK && eps[i].type != CK_XP_EP_INTR) || eps[i].max_packet == 0 ||
            (eps[i].max_packet & 0x7ffu) > 1024u)
            return fail(x, CK_XP_E_ARG, "configure: endpoint descriptor");
        uint8_t dci = (uint8_t)(2u * num + (in ? 1u : 0u));
        uint32_t type = eps[i].type == CK_XP_EP_BULK ? (in ? EPT_BULK_IN : EPT_BULK_OUT) : (in ? EPT_INTR_IN : EPT_INTR_OUT);
        uint16_t mps = (uint16_t)(eps[i].max_packet & 0x7ffu);
        x->ep_dci[i] = dci;
        x->ep_addr[i] = eps[i].addr;
        x->ep_mps[i] = mps;
        add |= 1u << dci;
        if (dci > max_dci) max_dci = dci;
        ck_xp_ring *r = &x->ring[i];
        ring_init(r, page(x, P_SMALL) + SMALL_RING_OFF(i), page_phys(x, P_SMALL) + SMALL_RING_OFF(i), SMALL_N);
        x->done[dci].valid = 0;
        volatile uint32_t *ep = ictx(x, 1u + dci);
        ep[0] = eps[i].type == CK_XP_EP_INTR ? intr_interval(x->speed, eps[i].interval) << 16 : 0u;
        ep[1] = (3u << 1) | (type << 3) | ((uint32_t)mps << 16);
        ep[2] = (uint32_t)r->phys | 1u;
        ep[3] = (uint32_t)(r->phys >> 32);
        ep[4] = eps[i].type == CK_XP_EP_INTR ? (uint32_t)mps | ((uint32_t)mps << 16) : 1024u;
    }
    x->ep_n = (uint8_t)n;
    ictx(x, 0)[1] = add;
    ictx(x, 1)[0] = ((uint32_t)x->speed << 20) | (max_dci << 27);
    ictx(x, 1)[1] = (uint32_t)x->port << 16;
    int rc = command(x, (uint32_t)page_phys(x, P_INCTX), (uint32_t)(page_phys(x, P_INCTX) >> 32),
                     TRB_TYPE(T_CONFIGURE) | ((uint32_t)x->slot << 24), 0, "Configure Endpoint");
    if (rc) return rc;
    return ck_xp_control(x, 0x00, 9, configuration_value, 0, 0, 0, 0, 1000);
}

int ck_xp_bulk_out(ck_xp *x, uint8_t ep_addr, const void *data, uint32_t len, uint32_t timeout_ms)
{
    int i = ep_slot(x, ep_addr);
    if (!x->slot || i < 0 || (ep_addr & 0x80u)) return fail(x, CK_XP_E_STATE, "bulk out: endpoint not configured");
    if (len > CK_XP_BUF_BYTES || (len && !data)) return fail(x, CK_XP_E_ARG, "bulk out: length");
    uint8_t *buf = page(x, P_BUF);
    uint64_t bphys = page_phys(x, P_BUF);
    for (uint32_t k = 0; k < len; k++) ((volatile uint8_t *)buf)[k] = ((const uint8_t *)data)[k];
    unsigned dci = x->ep_dci[i];
    x->done[dci].valid = 0;
#if defined(CK_TEST_XP_MUT) && CK_TEST_XP_MUT == 1
    if (len > 1) len--; /* TEST-ONLY mutant: bulk out one byte short */
#endif
    uint64_t at = ring_push(&x->ring[i], (uint32_t)bphys, (uint32_t)(bphys >> 32), len, TRB_TYPE(T_NORMAL) | TRB_IOC);
    doorbell(x, x->slot, dci);
    return transfer_wait(x, dci, at, timeout_ms ? timeout_ms : 1000u, 0, "bulk out");
}

int ck_xp_bulk_in(ck_xp *x, uint8_t ep_addr, void *out, uint32_t cap, uint32_t *got, uint32_t timeout_ms)
{
    int i = ep_slot(x, ep_addr);
    if (!x->slot || i < 0 || !(ep_addr & 0x80u)) return fail(x, CK_XP_E_STATE, "bulk in: endpoint not configured");
    if (cap > CK_XP_BUF_BYTES || !out) return fail(x, CK_XP_E_ARG, "bulk in: length");
    uint8_t *buf = page(x, P_BUF);
    uint64_t bphys = page_phys(x, P_BUF);
    zero(buf, cap);
    unsigned dci = x->ep_dci[i];
    x->done[dci].valid = 0;
    uint64_t at = ring_push(&x->ring[i], (uint32_t)bphys, (uint32_t)(bphys >> 32), cap, TRB_TYPE(T_NORMAL) | TRB_IOC | TRB_ISP);
    doorbell(x, x->slot, dci);
    uint32_t resid = 0;
    int rc = transfer_wait(x, dci, at, timeout_ms ? timeout_ms : 1000u, &resid, "bulk in");
    if (rc) return rc;
    uint32_t n = resid <= cap ? cap - resid : 0;
#if defined(CK_TEST_XP_MUT) && CK_TEST_XP_MUT == 2
    for (uint32_t k = 0; k < n; k++) ((uint8_t *)out)[k] = ((volatile uint8_t *)buf)[k + 1]; /* TEST-ONLY mutant: bulk in read one byte late */
#else
    for (uint32_t k = 0; k < n; k++) ((uint8_t *)out)[k] = ((volatile uint8_t *)buf)[k];
#endif
    if (got) *got = n;
    return 0;
}

int ck_xp_intr_arm(ck_xp *x, uint8_t ep_addr, uint32_t len)
{
    int i = ep_slot(x, ep_addr);
    if (!x->slot || i < 0 || !(ep_addr & 0x80u)) return fail(x, CK_XP_E_STATE, "interrupt in: endpoint not configured");
    if (len == 0 || len > CK_XP_INTR_BYTES) return fail(x, CK_XP_E_ARG, "interrupt in: length");
    uint64_t bphys = page_phys(x, P_SMALL) + INTR_BUF_OFF;
    zero(page(x, P_SMALL) + INTR_BUF_OFF, len);
    unsigned dci = x->ep_dci[i];
    x->done[dci].valid = 0;
    (void)ring_push(&x->ring[i], (uint32_t)bphys, (uint32_t)(bphys >> 32), len, TRB_TYPE(T_NORMAL) | TRB_IOC | TRB_ISP);
    x->armed[i] = len;
    doorbell(x, x->slot, dci);
    return 0;
}

int ck_xp_intr_poll(ck_xp *x, uint8_t ep_addr, void *out, uint32_t cap, uint32_t *got)
{
    int i = ep_slot(x, ep_addr);
    if (!x->slot || i < 0 || !(ep_addr & 0x80u)) return fail(x, CK_XP_E_STATE, "interrupt in: endpoint not configured");
    unsigned dci = x->ep_dci[i];
    ck_xp_done *d = &x->done[dci];
    pump(x);
#if defined(CK_TEST_XP_MUT) && CK_TEST_XP_MUT == 3
    d->valid = 0; /* TEST-ONLY mutant: interrupt reports are discarded */
#endif
    if (!d->valid) return 1;
    uint32_t cc = d->code;
    x->last_code = cc;
    d->valid = 0;
    if (cc == CC_STALL) return fail(x, CK_XP_E_STALL, "interrupt in");
    if (cc != CC_SUCCESS && cc != CC_SHORT) return fail(x, CK_XP_E_FAILED, "interrupt in");
    uint32_t armed = x->armed[i]; /* resid counts down from the length that was armed */
    uint32_t n = d->resid <= armed ? armed - d->resid : 0;
    if (n > cap) n = cap;
    const volatile uint8_t *b = page(x, P_SMALL) + INTR_BUF_OFF;
    for (uint32_t k = 0; k < n; k++) ((uint8_t *)out)[k] = b[k];
    if (got) *got = n;
    return 0;
}
