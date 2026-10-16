/* test_pi5_gem.c -- Pi 5 network lane cuts 3 and 4 (dev/pi5_gem.c) over a FAKE GEM that behaves like the Cadence
 * descriptor DMA and checks every access it makes: any DMA address outside the bounce window, any DMA while RP1
 * bus-master is off, bus-master dropped while the transmitter still runs, malformed rings, missing tie-offs.
 * Hosted: this proves the decisions and the ORDER of register writes, NOT the hardware. Run under ASan/UBSan
 * and with each CK_GEM_MUTANT_* (make pi5-gem-mutants); every mutant must make it FAIL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ck_test.h"
#include "pi5_gem.h"

#define WBASE 0x4400000ull
#define WSIZE 0x200000ull
#define OUTBASE 0x6000000ull /* a pretend page outside the window */

struct fk {
    uint8_t *mem;
    uint8_t outside[4096];
    uint32_t r[0x800 / 4];
    uint32_t tbqp_q[8], rbqp_q[8];
    uint16_t phy[32], shadow[32];
    uint32_t shd_sel;
    uint32_t dcfg1, dcfg6;
    int daw64;
    int tgo_delay_cfg, tgo_hold, tgo_cleared_seen;
    int tx_completes, rx_len_lie, expect_gbe;
    /* observations */
    int bme, bme_on_n, bme_off_n, bme_fail_on, bme_fail_off;
    int barrier_n, barrier_at_kick, kicks;
    int dma_without_bme, outside_access, outside_writes;
    int frames_sent, frames_rcvd;
    uint8_t last_tx[1600];
    uint32_t last_tx_len;
    int bad_desc_mode, queue_ptr_outside, rx_nowrap, tx_runaway, tx_no_last, bad_speed;
    int bme_off_while_tgo, bme_off_while_te;
    int gem_writes;
    uint32_t cap_w0, cap_ctrl0, cap_ctrl1, cap_w2;
    char ev[64];
    int nev;
    /* frames the pretend LAN hands to the receiver after our transmit (WIRE mode only) */
    uint8_t wire[8][1536];
    uint32_t wire_len[8];
    int nwire;
};
static struct fk f;

static void ev(char c)
{
    if (f.nev < 63)
        f.ev[f.nev++] = c;
    f.ev[f.nev] = 0;
}
static int inwin(uint64_t a, uint64_t n) { return a >= WBASE && n <= WSIZE && a - WBASE <= WSIZE - n; }
static uint32_t d_rd(uint64_t a) { uint32_t v; memcpy(&v, f.mem + (a - WBASE), 4); return v; }
static void d_wr(uint64_t a, uint32_t v) { memcpy(f.mem + (a - WBASE), &v, 4); }

static void deliver(const uint8_t *data, uint32_t len)
{
    uint64_t a = ((uint64_t)f.r[CK_GEM_RBQPH / 4] << 32) | f.r[CK_GEM_RBQP / 4];
    uint32_t dsz = f.daw64 ? 16 : 8, i;
    if (!(f.r[CK_GEM_NCR / 4] & CK_GEM_NCR_RE))
        return;
    if (!f.daw64)
        a &= 0xffffffffull;
    for (i = 0; i < 8; i++) {
        uint32_t w0, ctrl, L;
        uint64_t buf;
        if (!inwin(a, dsz)) {
            f.outside_access++;
            return;
        }
        w0 = d_rd(a);
        if (!(w0 & CK_GEMD_RX_USED)) {
            buf = (uint64_t)(w0 & ~3u) | (f.daw64 ? (uint64_t)d_rd(a + 8) << 32 : 0);
            L = len + ((f.r[CK_GEM_NCFGR / 4] & CK_GEM_NCFGR_DRFCS) ? 0u : 4u);
            if (inwin(buf, len)) {
                memcpy(f.mem + (buf - WBASE), data, len);
            } else {
                f.outside_access++;
                f.outside_writes++;
                if (buf >= OUTBASE && buf - OUTBASE + len <= sizeof f.outside)
                    memcpy(f.outside + (buf - OUTBASE), data, len);
            }
            ctrl = L | CK_GEMD_RX_SOF | CK_GEMD_RX_EOF | (1u << 31);
            if (f.rx_len_lie)
                ctrl = 2000u | CK_GEMD_RX_SOF | CK_GEMD_RX_EOF;
            d_wr(a + 4, ctrl);
            d_wr(a, w0 | CK_GEMD_RX_USED);
            f.frames_rcvd++;
            f.r[CK_GEM_RSR / 4] |= CK_GEM_RSR_REC;
            return;
        }
        if (w0 & CK_GEMD_RX_WRAP)
            return;
        a += dsz;
    }
}

static void kick(uint32_t ncr)
{
    uint64_t a;
    uint32_t dsz = f.daw64 ? 16 : 8, i, q;
    ev('K');
    f.kicks++;
    f.barrier_at_kick = f.barrier_n;
    if (!f.bme) {
        f.dma_without_bme++;
        return;
    }
    if (f.daw64 != !!(f.r[CK_GEM_DMACFG / 4] & CK_GEM_DMACFG_ADDR64))
        f.bad_desc_mode++;
    if (f.expect_gbe && !(f.r[CK_GEM_NCFGR / 4] & CK_GEM_NCFGR_GBE))
        f.bad_speed++;
    for (q = 1; q < 8; q++) {
        if (!((f.dcfg6 >> q) & 1u))
            continue;
        if (!inwin(f.tbqp_q[q], dsz) || !inwin(f.rbqp_q[q], dsz) || !(d_rd(f.tbqp_q[q] + 4) & CK_GEMD_TX_USED) ||
            !(d_rd(f.rbqp_q[q]) & CK_GEMD_RX_USED))
            f.queue_ptr_outside++;
    }
    /* RX ring must wrap within 8 slots */
    a = ((uint64_t)f.r[CK_GEM_RBQPH / 4] << 32) | f.r[CK_GEM_RBQP / 4];
    for (i = 0; i < 8; i++) {
        if (!inwin(a, dsz))
            break;
        if (d_rd(a) & CK_GEMD_RX_WRAP)
            break;
        a += dsz;
    }
    if (i == 8 || !inwin(a, dsz))
        f.rx_nowrap++;
    a = ((uint64_t)f.r[CK_GEM_TBQPH / 4] << 32) | f.r[CK_GEM_TBQP / 4];
    for (i = 0; i < 8; i++) {
        uint32_t ctrl, len;
        uint64_t buf;
        uint8_t data[1600];
        if (!inwin(a, dsz)) {
            f.outside_access++;
            return;
        }
        ctrl = d_rd(a + 4);
        if (i == 0) {
            f.cap_w0 = d_rd(a);
            f.cap_ctrl0 = ctrl;
            f.cap_w2 = f.daw64 ? d_rd(a + 8) : 0;
            f.cap_ctrl1 = d_rd(a + dsz + 4);
        }
        if (ctrl & CK_GEMD_TX_USED) {
            f.r[CK_GEM_TSR / 4] |= CK_GEM_TSR_UBR;
            return;
        }
        if (!(ctrl & CK_GEMD_TX_LAST)) {
            f.tx_no_last++;
            return;
        }
        len = ctrl & CK_GEMD_TX_FRMLEN_MASK;
        buf = d_rd(a) | (f.daw64 ? (uint64_t)d_rd(a + 8) << 32 : 0);
        if (len > sizeof data)
            len = sizeof data;
        if (inwin(buf, len)) {
            memcpy(data, f.mem + (buf - WBASE), len);
        } else {
            f.outside_access++;
            memset(data, 0xee, len);
        }
        f.frames_sent++;
        memcpy(f.last_tx, data, len);
        f.last_tx_len = len;
        if (f.tx_completes) {
            d_wr(a + 4, ctrl | CK_GEMD_TX_USED);
            f.r[CK_GEM_TSR / 4] |= CK_GEM_TSR_COMP;
        }
        f.tgo_hold = f.tgo_delay_cfg;
        if ((ncr & CK_GEM_NCR_LLB) || (f.phy[0] & CK_MII_BMCR_LOOPBACK)) {
            deliver(data, len);
        } else {
            int wi;
            for (wi = 0; wi < f.nwire; wi++)
                deliver(f.wire[wi], f.wire_len[wi]);
        }
        if (ctrl & CK_GEMD_TX_WRAP)
            a = ((uint64_t)f.r[CK_GEM_TBQPH / 4] << 32) | f.r[CK_GEM_TBQP / 4];
        else
            a += dsz;
    }
    f.tx_runaway++;
}

static void phy_man(uint32_t v)
{
    uint32_t reg = (v >> 18) & 31u, op = (v >> 28) & 3u, data = v & 0xffffu;
    if (op == 2) { /* read */
        if (reg == 0x1c)
            f.phy[reg] = (uint16_t)(f.shadow[f.shd_sel] & 0x3ff);
        f.r[CK_GEM_MAN / 4] = (v & 0xffff0000u) | f.phy[reg];
    } else if (op == 1) { /* write */
        if (reg == 0x1c) {
            if (data & 0x8000u)
                f.shadow[(data >> 10) & 31u] = (uint16_t)(data & 0x3ff);
            else
                f.shd_sel = (data >> 10) & 31u;
        } else if (reg == 0x18) {
            if ((data & 7u) == 7u && ((data >> 12) & 7u) == 7u) {
                /* read-select of MISC: nothing stored */
            } else {
                f.phy[reg] = (uint16_t)data;
            }
        } else {
            f.phy[reg] = (uint16_t)data;
        }
        f.r[CK_GEM_MAN / 4] = v;
    }
}

static uint32_t fk_rd(void *c, uint32_t off)
{
    uint32_t o;
    (void)c;
    if (off < CK_RP1_ETH || off >= CK_RP1_ETH + 0x800u)
        return 0;
    o = off - CK_RP1_ETH;
    if (o == CK_GEM_NSR)
        return CK_GEM_NSR_IDLE;
    if (o == CK_GEM_TSR) {
        uint32_t v = f.r[CK_GEM_TSR / 4];
        if (f.tgo_hold > 0) {
            v |= CK_GEM_TSR_TGO;
            f.tgo_hold--;
        } else {
            f.tgo_cleared_seen = 1;
            ev('g');
        }
        return v;
    }
    if (o == CK_GEM_ISR) {
        uint32_t v = f.r[o / 4];
        f.r[o / 4] = 0;
        return v;
    }
    if (o == CK_GEM_MID)
        return 0x00070109u;
    if (o == CK_GEM_DCFG1)
        return f.dcfg1;
    if (o == CK_GEM_DCFG6)
        return f.dcfg6;
    return f.r[o / 4];
}

static void fk_wr(void *c, uint32_t off, uint32_t v)
{
    uint32_t o;
    (void)c;
    if (off < CK_RP1_ETH || off >= CK_RP1_ETH + 0x800u)
        return;
    o = off - CK_RP1_ETH;
    if (o == CK_GEM_MAN) {
        phy_man(v);
        return;
    }
    f.gem_writes++;
    if (o == CK_GEM_NCR) {
        uint32_t old = f.r[o / 4], nv = v;
        nv &= ~(uint32_t)CK_GEM_NCR_CLRSTAT;
        nv &= ~(uint32_t)CK_GEM_NCR_TSTART;
        if ((old & (CK_GEM_NCR_RE | CK_GEM_NCR_TE)) && !(nv & (CK_GEM_NCR_RE | CK_GEM_NCR_TE)))
            ev('E');
        f.r[o / 4] = nv;
        if (v & CK_GEM_NCR_TSTART)
            kick(nv);
        return;
    }
    if (o == CK_GEM_TSR || o == CK_GEM_RSR) {
        f.r[o / 4] &= ~v;
        return;
    }
    if (o >= 0x440 && o < 0x460) {
        f.tbqp_q[((o - 0x440) >> 2) + 1] = v;
        return;
    }
    if (o >= 0x480 && o < 0x4a0) {
        f.rbqp_q[((o - 0x480) >> 2) + 1] = v;
        return;
    }
    f.r[o / 4] = v;
}
static struct ck_rp1_io fio = {fk_rd, fk_wr, 0, 0};

static int fk_bme(void *c, int on)
{
    (void)c;
    if (on) {
        if (f.bme_fail_on)
            return -1;
        f.bme = 1;
        f.bme_on_n++;
        ev('B');
    } else {
        if (f.bme_fail_off)
            return -1;
        if (f.tgo_hold > 0)
            f.bme_off_while_tgo++;
        if (f.r[CK_GEM_NCR / 4] & (CK_GEM_NCR_TE | CK_GEM_NCR_RE))
            f.bme_off_while_te++;
        f.bme = 0;
        f.bme_off_n++;
        ev('b');
    }
    return 0;
}
static void fk_barrier(void) { f.barrier_n++; }

static struct ck_dmaw dw;
static struct ck_gem g;

static void fresh(void)
{
    uint8_t *keep = f.mem;
    memset(&f, 0, sizeof f);
    f.mem = keep ? keep : calloc(1, WSIZE);
    memset(f.mem, 0, WSIZE);
    f.dcfg1 = 4u << CK_GEM_DCFG1_DBWDEF_SHIFT;
    f.daw64 = 1;
    f.dcfg6 = CK_GEM_DCFG6_DAW64 | 0x0eu; /* queues 1..3 exist besides 0 */
    f.tx_completes = 1;
    f.tgo_delay_cfg = 50;
    f.expect_gbe = 1;
    f.r[CK_GEM_NCR / 4] = CK_GEM_NCR_MPE;
    f.r[CK_GEM_NCFGR / 4] = 0x00540000u;
    f.phy[0] = 0x1140u;
    f.phy[1] = 0x796du;
    f.phy[2] = 0x600du;
    f.phy[3] = 0x84a2u;
    f.phy[4] = 0x01e1u;
    f.phy[5] = 0x45e1u;
    f.phy[9] = 0x0300u;
    f.phy[10] = 0x3800u;
    f.shadow[3] = 0x0010;
    CHECK(ck_dmaw_init(&dw, f.mem, WBASE, WSIZE) == CK_DMAW_OK);
    ck_gem_init(&g, &fio, &dw, f.mem, fk_bme, 0, fk_barrier, 1);
    CHECK(ck_gem_probe(&fio, &g.caps) == 0);
}

static const uint8_t MAC[6] = {0x02, 0x41, 0x49, 0x45, 0x4e, 0x03};

static void mkx(struct ck_gem_xfer *x, int mode, uint8_t *frame, uint32_t seq)
{
    *x = (struct ck_gem_xfer){0};
    ck_gem_frame_build(CK_GEMF_RAW, MAC, seq, 0, frame, 64);
    x->mode = mode;
    x->speed = 1000;
    x->full_duplex = 1;
    memcpy(x->mac, MAC, 6);
    x->frame = frame;
    x->frame_len = 60;
    x->rx_count = 2;
    x->rx_buf_len = 1536;
    x->tx_wait_us = 20000;
    x->rx_wait_us = 20000;
}

static void clean_flags(void)
{
    CHECK(f.dma_without_bme == 0);
    CHECK(f.outside_access == 0);
    CHECK(f.bad_desc_mode == 0);
    CHECK(f.queue_ptr_outside == 0);
    CHECK(f.rx_nowrap == 0);
    CHECK(f.tx_runaway == 0);
    CHECK(f.tx_no_last == 0);
    CHECK(f.bad_speed == 0);
    CHECK(f.bme_off_while_tgo == 0);
    CHECK(f.bme_off_while_te == 0);
}

static void t_probe(void)
{
    struct ck_gem_caps c;
    fresh();
    CHECK(ck_gem_probe(&fio, &c) == 0);
    CHECK(c.is_gem == 1 && c.daw64 == 1 && c.queue_mask == 0x0fu && c.nqueues == 4 && c.desc_bytes == 16 && c.dbw == 2);
    f.dcfg6 = 0; /* 32-bit descriptors, one queue, 64-bit bus */
    f.dcfg1 = 2u << CK_GEM_DCFG1_DBWDEF_SHIFT;
    CHECK(ck_gem_probe(&fio, &c) == 0);
    CHECK(c.daw64 == 0 && c.queue_mask == 1 && c.nqueues == 1 && c.desc_bytes == 8 && c.dbw == 1);
}

static void t_loopback(void)
{
    struct ck_gem_xfer x;
    uint8_t fr[64];
    uint64_t tx_buf;
    fresh();
    mkx(&x, CK_GEMX_LLB, fr, 1);
    CHECK(ck_gem_xfer(&g, &x) == 0);
    CHECK(x.tx_done == 1 && x.rx_done == 1 && x.rx_frames == 1 && x.rx_len == 60);
    CHECK(ck_gem_frame_diff(fr, 60, x.rx_data, x.rx_copied) == 0);
    CHECK(f.frames_sent == 1 && f.frames_rcvd == 1);
    CHECK(f.bme_on_n == 1 && f.bme_off_n == 1 && f.bme == 0 && g.bme_on == 0);
    CHECK(x.bme_enabled_for_us == 1 && x.bme_off_at_end == 1);
    CHECK(strcmp(f.ev, "BKEgb") == 0 || strcmp(f.ev, "BKEgggb") == 0 || (f.ev[0] == 'B' && f.ev[1] == 'K' && f.ev[2] == 'E'));
    /* bus-master on first, then the kick, then the engine off, THEN bus-master off */
    CHECK(f.ev[0] == 'B' && f.ev[1] == 'K' && f.ev[2] == 'E' && f.ev[f.nev - 1] == 'b');
    CHECK(f.barrier_at_kick >= 1);
    CHECK(!(f.r[CK_GEM_NCR / 4] & (CK_GEM_NCR_RE | CK_GEM_NCR_TE | CK_GEM_NCR_LLB)));
    CHECK(f.r[CK_GEM_NCR / 4] & CK_GEM_NCR_MPE); /* MDIO port stays enabled */
    CHECK(!(x.ncr_end & (CK_GEM_NCR_RE | CK_GEM_NCR_TE | CK_GEM_NCR_LLB)));
    CHECK(x.ncr_run & CK_GEM_NCR_LLB);
    /* the descriptors the device saw: address inside the window, length 60, LAST, owned by the device; sentinel USED|WRAP */
    tx_buf = x.tx_bus;
    CHECK(inwin(tx_buf, 64) && f.cap_w0 == (uint32_t)tx_buf && f.cap_w2 == (uint32_t)(tx_buf >> 32));
    CHECK((f.cap_ctrl0 & CK_GEMD_TX_FRMLEN_MASK) == 60 && (f.cap_ctrl0 & CK_GEMD_TX_LAST) && !(f.cap_ctrl0 & CK_GEMD_TX_USED));
    CHECK((f.cap_ctrl1 & CK_GEMD_TX_USED) && (f.cap_ctrl1 & CK_GEMD_TX_WRAP));
    /* DMA configuration: 64-bit descriptors, 1536 byte buffers, burst 16, TXCOEN off */
    CHECK((f.r[CK_GEM_DMACFG / 4] & CK_GEM_DMACFG_ADDR64) && ((f.r[CK_GEM_DMACFG / 4] >> 16) & 0xff) == 24);
    CHECK((f.r[CK_GEM_DMACFG / 4] & 0x1f) == 16 && !(f.r[CK_GEM_DMACFG / 4] & CK_GEM_DMACFG_TXCOEN));
    CHECK((f.r[CK_GEM_NCFGR / 4] & CK_GEM_NCFGR_GBE) && (f.r[CK_GEM_NCFGR / 4] & CK_GEM_NCFGR_FD) &&
          (f.r[CK_GEM_NCFGR / 4] & CK_GEM_NCFGR_DRFCS));
    CHECK(((f.r[CK_GEM_NCFGR / 4] >> 18) & 7u) == 5u); /* MDC divider kept */
    CHECK(f.r[CK_GEM_USRIO / 4] == CK_GEM_USRIO_RGMII);
    CHECK(f.r[CK_GEM_SA1B / 4] == 0x45494102u && f.r[CK_GEM_SA1T / 4] == 0x0000034eu);
    clean_flags();
    /* a second transfer reuses the window cleanly and still sends exactly one more frame */
    mkx(&x, CK_GEMX_LLB, fr, 2);
    CHECK(ck_gem_xfer(&g, &x) == 0 && x.rx_done && ck_gem_frame_diff(fr, 60, x.rx_data, x.rx_copied) == 0);
    CHECK(f.frames_sent == 2);
    CHECK(g.w->next < 0x10000);
    clean_flags();
}

static void t_phy_loopback(void)
{
    struct ck_gem_xfer x;
    uint8_t fr[64];
    uint16_t saved = 0, bmsr = 0;
    fresh();
    CHECK(ck_phy_loopback_enter(&fio, 1, 1000, &saved, &bmsr) == 0);
    CHECK(saved == 0x1140);
    CHECK(f.phy[0] == (CK_MII_BMCR_LOOPBACK | CK_MII_BMCR_FULLDPLX | CK_MII_BMCR_SPEED1000)); /* 0x4140, no autoneg */
    mkx(&x, CK_GEMX_PHYLB, fr, 7);
    CHECK(ck_gem_xfer(&g, &x) == 0 && x.rx_done && ck_gem_frame_diff(fr, 60, x.rx_data, x.rx_copied) == 0);
    CHECK(!(x.ncr_run & CK_GEM_NCR_LLB)); /* the PHY loops, not the MAC */
    CHECK(ck_phy_loopback_exit(&fio, 1, saved) == 0);
    CHECK(f.phy[0] == (0x1140 | CK_MII_BMCR_ANRESTART));
    clean_flags();
    f.phy[1] = 0x7969u; /* link down in loopback: enter reports a timeout, does not claim success */
    CHECK(ck_phy_loopback_enter(&fio, 1, 100, &saved, &bmsr) != 0);
    CHECK(f.phy[0] == (CK_MII_BMCR_LOOPBACK | CK_MII_BMCR_FULLDPLX | CK_MII_BMCR_SPEED100));
}

static void t_wire_no_loop(void)
{
    struct ck_gem_xfer x;
    uint8_t fr[64];
    fresh();
    f.r[CK_GEM_NCR / 4] |= CK_GEM_NCR_LLB; /* left over from an earlier test: the reset must clear it */
    mkx(&x, CK_GEMX_WIRE, fr, 3);
    x.speed = 100;
    f.expect_gbe = 0;
    CHECK(ck_gem_xfer(&g, &x) == 0);
    CHECK(x.tx_done == 1 && x.rx_done == 0 && f.frames_sent == 1 && f.frames_rcvd == 0);
    CHECK(f.r[CK_GEM_NCFGR / 4] & CK_GEM_NCFGR_SPD);
    CHECK(!(f.r[CK_GEM_NCFGR / 4] & CK_GEM_NCFGR_GBE));
    clean_flags();
}

static void hostile(uint64_t tx_addr, uint64_t rx_addr)
{
    struct ck_gem_xfer x;
    uint8_t fr[64];
    int rc;
    fresh();
    mkx(&x, CK_GEMX_LLB, fr, 1);
    x.tx_addr = tx_addr;
    x.rx_addr = rx_addr;
    rc = ck_gem_xfer(&g, &x);
    CHECK(rc == CK_GEM_E_WINDOW);
    CHECK(f.bme_on_n == 0 && f.bme == 0);         /* bus-master never enabled */
    CHECK(f.gem_writes == 0 && f.kicks == 0);      /* the MAC was not even programmed */
    CHECK(f.frames_sent == 0);
    CHECK(g.halted == 1 && ck_dmaw_halted(&dw));   /* latched */
    mkx(&x, CK_GEMX_LLB, fr, 2); /* a good transfer afterwards is refused: sticky until reboot */
    CHECK(ck_gem_xfer(&g, &x) == CK_GEM_E_HALTED);
    CHECK(f.bme_on_n == 0 && f.kicks == 0);
}

static void t_refuse_outside(void)
{
    hostile(WBASE - 64, 0);                 /* just below the window */
    hostile(WBASE + WSIZE, 0);              /* first byte after */
    hostile(WBASE + WSIZE - 32, 0);         /* straddles the end (60 byte frame) */
    hostile(0xfffffffffffffff0ull, 0);      /* wraps when added to a length */
    hostile(OUTBASE, 0);
    hostile(0, WBASE + WSIZE);              /* RX buffer outside */
    hostile(0, WBASE + WSIZE - 1536 + 64);  /* RX buffer overhangs the end */
    hostile(0, 0xfffffffffffffff8ull);
}

static void t_hwwin_gate(void)
{
    struct ck_gem_xfer x;
    uint8_t fr[64];
    fresh();
    g.hw_window_ok = 0;
    mkx(&x, CK_GEMX_LLB, fr, 1);
    CHECK(ck_gem_xfer(&g, &x) == CK_GEM_E_HWWIN);
    CHECK(f.bme_on_n == 0 && f.gem_writes == 0 && f.kicks == 0);
    CHECK(g.halted == 0); /* a refusal here is not a violation: it can be retried once the window is confirmed */
}

static void t_arg_checks(void)
{
    struct ck_gem_xfer x;
    uint8_t fr[64];
    fresh();
    mkx(&x, CK_GEMX_LLB, fr, 1);
    x.frame_len = 1515;
    CHECK(ck_gem_xfer(&g, &x) == CK_GEM_E_ARG);
    x.frame_len = 59;
    CHECK(ck_gem_xfer(&g, &x) == CK_GEM_E_ARG);
    mkx(&x, CK_GEMX_LLB, fr, 1);
    x.speed = 777;
    CHECK(ck_gem_xfer(&g, &x) == CK_GEM_E_ARG);
    mkx(&x, CK_GEMX_LLB, fr, 1);
    x.rx_buf_len = 1537;
    CHECK(ck_gem_xfer(&g, &x) == CK_GEM_E_ARG);
    x.rx_buf_len = 4096;
    CHECK(ck_gem_xfer(&g, &x) == CK_GEM_E_ARG);
    CHECK(f.bme_on_n == 0 && f.gem_writes == 0 && f.kicks == 0 && g.halted == 0);
}

static void t_device_lies(void)
{
    struct ck_gem_xfer x;
    uint8_t fr[64];
    fresh();
    f.rx_len_lie = 1; /* the device claims 2000 bytes in a 1536 byte buffer */
    mkx(&x, CK_GEMX_LLB, fr, 1);
    CHECK(ck_gem_xfer(&g, &x) == CK_GEM_E_DEVICE);
    CHECK(g.halted == 1 && ck_dmaw_halted(&dw));
    CHECK(f.bme == 0 && f.bme_off_n == 1); /* bus-master was already off when the lie was read */
}

static void t_bme_failures(void)
{
    struct ck_gem_xfer x;
    uint8_t fr[64];
    fresh();
    f.bme_fail_on = 1;
    mkx(&x, CK_GEMX_LLB, fr, 1);
    CHECK(ck_gem_xfer(&g, &x) == CK_GEM_E_BME);
    CHECK(f.kicks == 0 && f.frames_sent == 0 && g.halted == 1);
    fresh();
    f.bme_fail_off = 1;
    mkx(&x, CK_GEMX_LLB, fr, 1);
    CHECK(ck_gem_xfer(&g, &x) == CK_GEM_E_BME);
    CHECK(g.halted == 1);
    CHECK(!(f.r[CK_GEM_NCR / 4] & (CK_GEM_NCR_RE | CK_GEM_NCR_TE))); /* the MAC was stopped even though bus-master would not drop */
}

static void t_tieoff_and_queues(void)
{
    struct ck_gem_xfer x;
    uint8_t fr[64];
    uint32_t q;
    fresh();
    mkx(&x, CK_GEMX_LLB, fr, 1);
    CHECK(ck_gem_xfer(&g, &x) == 0);
    for (q = 1; q <= 3; q++) {
        CHECK(inwin(f.tbqp_q[q], 16) && inwin(f.rbqp_q[q], 16));
        CHECK(f.tbqp_q[q] != 0 && f.rbqp_q[q] != 0);
    }
    clean_flags();
}

static void t_quiesce_order(void)
{
    struct ck_gem_xfer x;
    uint8_t fr[64];
    fresh();
    f.tgo_delay_cfg = 200; /* the transmitter stays busy for 200 status reads */
    mkx(&x, CK_GEMX_LLB, fr, 1);
    CHECK(ck_gem_xfer(&g, &x) == 0);
    CHECK(f.bme_off_while_tgo == 0 && f.bme_off_while_te == 0);
    CHECK(f.tgo_cleared_seen == 1);
    clean_flags();
    /* a transmitter that never stops: halted, bus-master still dropped, error reported */
    fresh();
    f.tgo_delay_cfg = 1000000;
    mkx(&x, CK_GEMX_LLB, fr, 1);
    CHECK(ck_gem_xfer(&g, &x) == CK_GEM_E_QUIESCE);
    CHECK(g.halted == 1 && f.bme == 0);
}

static void t_raw_outside_for_hw_test(void)
{
    struct ck_gem_xfer x;
    uint8_t fr[64];
    fresh();
    /* confinement test path: the hardware-visible RX address is OUTSIDE; the shadow stays valid and nothing latches */
    mkx(&x, CK_GEMX_LLB, fr, 4);
    x.rx_count = 1;
    x.rx_buf_len = 64;
    x.raw_rx_outside = OUTBASE;
    memset(f.outside, 0xa5, sizeof f.outside);
    CHECK(ck_gem_xfer(&g, &x) == 0);
    CHECK(g.halted == 0 && !ck_dmaw_halted(&dw));
    CHECK(f.outside_writes == 1);                       /* the fake (no RC window) let it through */
    CHECK(memcmp(f.outside, fr, 60) == 0 && f.outside[60] == 0xa5);
    CHECK(x.rx_len == 60 && x.rx_done);
    CHECK(f.bme_on_n == 1 && f.bme_off_n == 1);
    /* the in-window RX buffer stayed empty: the data went where the raw descriptor said */
    {
        uint32_t i, nz = 0;
        for (i = 0; i < 60; i++) nz += x.rx_data[i] != 0;
        CHECK(nz == 0);
    }
    fresh();
    mkx(&x, CK_GEMX_LLB, fr, 5);
    x.rx_count = 1;
    x.raw_tx_outside = OUTBASE;
    CHECK(ck_gem_xfer(&g, &x) == 0);
    CHECK(f.outside_access >= 1);  /* the fake saw a read outside; the real test reads whether the hardware blocked it */
    CHECK(g.halted == 0);
}

static void t_frames(void)
{
    uint8_t a[64], b[64], c[64];
    uint8_t tpa[4] = {192, 168, 1, 1};
    CHECK(ck_gem_frame_build(CK_GEMF_RAW, MAC, 0x01020304u, 0, a, sizeof a) == 60);
    CHECK(ck_gem_frame_build(CK_GEMF_RAW, MAC, 0x01020305u, 0, b, sizeof b) == 60);
    CHECK(a[0] == 0xff && a[5] == 0xff && memcmp(a + 6, MAC, 6) == 0 && a[12] == 0x88 && a[13] == 0xb5);
    CHECK(memcmp(a + 14, "AIENOS-PI5-NET3", 16) == 0);
    CHECK(a[30] == 1 && a[31] == 2 && a[32] == 3 && a[33] == 4);   /* sequence number, big endian */
    CHECK(ck_gem_frame_diff(a, 60, b, 60) != 0);                    /* different sequence = different frame */
    CHECK(ck_gem_frame_build(CK_GEMF_ARP, MAC, 9, tpa, c, sizeof c) == 60);
    CHECK(c[12] == 0x08 && c[13] == 0x06 && c[14] == 0 && c[15] == 1 && c[16] == 8 && c[17] == 0 && c[18] == 6 && c[19] == 4 && c[21] == 1);
    CHECK(memcmp(c + 22, MAC, 6) == 0 && c[28] == 0 && c[31] == 0 && memcmp(c + 38, tpa, 4) == 0); /* probe: sender IP 0.0.0.0 */
    CHECK(c[50] == 9);
    CHECK(ck_gem_frame_build(99, MAC, 1, 0, a, sizeof a) == 0 && ck_gem_frame_build(CK_GEMF_RAW, MAC, 1, 0, a, 59) == 0);
    /* diff: identical, last byte, length */
    memcpy(b, a, 60);
    CHECK(ck_gem_frame_diff(a, 60, b, 60) == 0);
    b[59] ^= 1;
    CHECK(ck_gem_frame_diff(a, 60, b, 60) == 60);
    b[59] ^= 1;
    b[0] ^= 1;
    CHECK(ck_gem_frame_diff(a, 60, b, 60) == 1);
    b[0] ^= 1;
    CHECK(ck_gem_frame_diff(a, 60, b, 59) == 60);
}

static void t_mdio_phy(void)
{
    struct ck_phy_link l;
    uint16_t aux = 0, clk = 0;
    int link = 0, fd = 0;
    uint32_t sp = 0;
    fresh();
    /* IEEE 802.3 clause 22 write frame: ST=01 OP=01 PHYAD REGAD TA=10 DATA */
    CHECK(ck_gem_mdio_frame_write(1, 0, 0x4140) == (0x40000000u | (1u << 28) | (1u << 23) | (2u << 16) | 0x4140u));
    CHECK(ck_gem_mdio_write(&fio, 1, 0, 0x4140) == 0 && f.phy[0] == 0x4140);
    CHECK(ck_gem_mdio_write(&fio, 32, 0, 0) != 0 && ck_gem_mdio_write(&fio, 1, 32, 0) != 0);
    /* link resolution: 1000 full from the partner's STAT1000 */
    fresh();
    f.phy[0] = 0x1140; f.phy[1] = 0x796d; f.phy[9] = 0x0300; f.phy[10] = 0x0800;
    CHECK(ck_phy_link_read(&fio, 1, &l) == 0 && l.link && l.aneg_done && l.speed == 1000 && l.full_duplex);
    f.phy[10] = 0; f.phy[4] = 0x01e1; f.phy[5] = 0x0100 | 0x0001;
    CHECK(ck_phy_link_read(&fio, 1, &l) == 0 && l.speed == 100 && l.full_duplex);
    f.phy[5] = 0x0020 | 0x0001;
    CHECK(ck_phy_link_read(&fio, 1, &l) == 0 && l.speed == 10 && !l.full_duplex);
    f.phy[1] = 0x7969; /* no link */
    CHECK(ck_phy_link_read(&fio, 1, &l) == 0 && !l.link && l.speed == 0);
    f.phy[1] = 0x794d; /* link, autoneg not complete: unresolved */
    CHECK(ck_phy_link_read(&fio, 1, &l) == 0 && l.link && !l.aneg_done && l.speed == 0);
    /* ETH_CFG STATUS decode */
    ck_eth_cfg_status_decode(0x00000005u, &link, &sp, &fd);
    CHECK(link == 0 || link == 1);
    ck_eth_cfg_status_decode(0x0000000bu, &link, &sp, &fd);
    CHECK(link == 1 && sp == 100 && fd == 1);
    ck_eth_cfg_status_decode(0x00000004u, &link, &sp, &fd);
    CHECK(link == 0 && sp == 1000 && fd == 0);
    ck_eth_cfg_status_decode(0x00000000u, &link, &sp, &fd);
    CHECK(sp == 10);
    /* RGMII-ID clock delays as Linux sets them */
    fresh();
    CHECK(ck_phy_rgmii_id_config(&fio, 1, &aux, &clk) == 0);
    CHECK((f.phy[0x18] & 0x8000u) && (f.phy[0x18] & 0x0100u) && (f.phy[0x18] & 7u) == 7u);
    CHECK(clk == 0x0010 && (f.shadow[3] & (1u << 9)) && (f.shadow[3] & 0x0010));
}

/* ---- bus-master through the root complex config window ---- */
static uint32_t cmd_status, cfg_index, stuck_bme;
static int cfg_writes_status_ones;
static uint32_t rc_rd(void *c, uint32_t off)
{
    (void)c;
    if (off == CK_PI5PCIE_EXT_CFG_INDEX)
        return cfg_index;
    if (off >= 0x8000 && off < 0x9000)
        return (cfg_index == 0x00100000u && off - 0x8000 == CK_PI5NET_PCI_COMMAND) ? cmd_status : 0;
    return 0;
}
static void rc_wr(void *c, uint32_t off, uint32_t v)
{
    (void)c;
    if (off == CK_PI5PCIE_EXT_CFG_INDEX) {
        cfg_index = v;
        return;
    }
    if (off >= 0x8000 && off < 0x9000 && cfg_index == 0x00100000u && off - 0x8000 == CK_PI5NET_PCI_COMMAND) {
        if (v >> 16)
            cfg_writes_status_ones++;
        cmd_status = (cmd_status & 0xffff0000u & ~(v & 0xffff0000u)) | (v & 0xffffu);
        if (stuck_bme)
            cmd_status |= CK_PI5NET_CMD_BME;
    }
}
static struct ck_pi5pcie_io rcio = {rc_rd, rc_wr, 0};

static void t_bme_set(void)
{
    cmd_status = (0x0010u << 16) | 0x0002u;
    cfg_index = 0;
    stuck_bme = 0;
    cfg_writes_status_ones = 0;
    CHECK(ck_pi5gem_bme_set(&rcio, 1) == 0 && (cmd_status & 0xffffu) == 0x0006u);
    CHECK((cmd_status >> 16) == 0x0010u && cfg_writes_status_ones == 0); /* RW1C status never written back as ones */
    CHECK(ck_pi5gem_bme_set(&rcio, 0) == 0 && (cmd_status & 0xffffu) == 0x0002u);
    stuck_bme = 1; /* a device that ignores the clear must be reported, not trusted */
    cmd_status = (0x0010u << 16) | 0x0006u;
    CHECK(ck_pi5gem_bme_set(&rcio, 0) == CK_GEM_E_BME);
}


/* ================= Cut 4: ARP request/reply and the exchange ================= */
static const uint8_t OURMAC[6] = {0x2c, 0xcf, 0x67, 0x3f, 0x54, 0x76};
static const uint8_t OURIP[4] = {192, 168, 1, 192};
static const uint8_t GWIP[4] = {192, 168, 1, 1};
static const uint8_t GWMAC[6] = {0x4c, 0x43, 0x41, 0x1f, 0x5f, 0xb4};

/* A well formed reply from the gateway to us (60 bytes, padded). */
static void mkreply(uint8_t *r)
{
    memset(r, 0, 60);
    memcpy(r, OURMAC, 6);
    memcpy(r + 6, GWMAC, 6);
    r[12] = 0x08; r[13] = 0x06;
    r[14] = 0; r[15] = 1; r[16] = 8; r[17] = 0; r[18] = 6; r[19] = 4; r[20] = 0; r[21] = 2;
    memcpy(r + 22, GWMAC, 6);
    memcpy(r + 28, GWIP, 4);
    memcpy(r + 32, OURMAC, 6);
    memcpy(r + 38, OURIP, 4);
}
static int chk(const uint8_t *r, uint32_t len)
{
    uint8_t sm[6], si[4];
    return ck_arp_reply_check(r, len < CK_GEM_RXKEEP ? len : CK_GEM_RXKEEP, len, OURMAC, OURIP, GWIP, sm, si);
}

static void t_arp_build(void)
{
    uint8_t q[64];
    uint32_t i;
    CHECK(ck_arp_request_build(OURMAC, OURIP, GWIP, q, sizeof q) == 60);
    for (i = 0; i < 6; i++) CHECK(q[i] == 0xff);
    CHECK(memcmp(q + 6, OURMAC, 6) == 0 && q[12] == 0x08 && q[13] == 0x06);
    CHECK(q[14] == 0 && q[15] == 1 && q[16] == 8 && q[17] == 0 && q[18] == 6 && q[19] == 4);
    CHECK(q[20] == 0 && q[21] == 1);                                  /* request */
    CHECK(memcmp(q + 22, OURMAC, 6) == 0 && memcmp(q + 28, OURIP, 4) == 0); /* sender: us, with our real IP (not a probe) */
    for (i = 32; i < 38; i++) CHECK(q[i] == 0);                       /* target hardware unknown */
    CHECK(memcmp(q + 38, GWIP, 4) == 0);
    for (i = 42; i < 60; i++) CHECK(q[i] == 0);
    CHECK(ck_arp_request_build(OURMAC, OURIP, GWIP, q, 59) == 0 && ck_arp_request_build(0, OURIP, GWIP, q, 64) == 0);
}

static void t_arp_check(void)
{
    uint8_t r[1536], sm[6] = {0}, si[4] = {0};
    mkreply(r);
    CHECK(ck_arp_reply_check(r, 60, 60, OURMAC, OURIP, GWIP, sm, si) == CK_ARP_OK);
    CHECK(memcmp(sm, GWMAC, 6) == 0 && memcmp(si, GWIP, 4) == 0);
    CHECK(chk(r, 42) == CK_ARP_OK);                                   /* unpadded 42 bytes is a valid reply */
    /* each refusal exactly as listed */
    r[0] ^= 1;                                                        /* addressed to another MAC */
    CHECK(chk(r, 60) == CK_ARP_E_DST);
    mkreply(r); memset(r, 0xff, 6);                                   /* broadcast is not "addressed to us" */
    CHECK(chk(r, 60) == CK_ARP_E_DST);
    mkreply(r); r[21] = 1;                                            /* a request */
    CHECK(chk(r, 60) == CK_ARP_E_OP);
    mkreply(r); r[21] = 3;
    CHECK(chk(r, 60) == CK_ARP_E_OP);
    mkreply(r); r[20] = 1;
    CHECK(chk(r, 60) == CK_ARP_E_OP);
    mkreply(r); r[13] = 0x00;                                         /* IPv4 EtherType */
    CHECK(chk(r, 60) == CK_ARP_E_ETYPE);
    mkreply(r); r[15] = 6;                                            /* htype not Ethernet */
    CHECK(chk(r, 60) == CK_ARP_E_HDR);
    mkreply(r); r[19] = 16;                                           /* plen not 4 */
    CHECK(chk(r, 60) == CK_ARP_E_HDR);
    mkreply(r); r[22] ^= 0x80;                                        /* Ethernet source != ARP sender hardware */
    CHECK(chk(r, 60) == CK_ARP_E_SRC);
    mkreply(r); r[32 + 5] ^= 1;                                       /* target hardware is not us */
    CHECK(chk(r, 60) == CK_ARP_E_THA);
    mkreply(r); r[38 + 3] ^= 1;                                       /* target IP is not ours */
    CHECK(chk(r, 60) == CK_ARP_E_TPA);
    mkreply(r); r[28 + 3] ^= 1;                                       /* sender IP is not the one we asked about */
    CHECK(chk(r, 60) == CK_ARP_E_SPA);
    /* lengths */
    mkreply(r);
    CHECK(chk(r, 41) == CK_ARP_E_LEN && chk(r, 0) == CK_ARP_E_LEN);
    CHECK(chk(r, 65) == CK_ARP_E_LEN);                                /* oversize */
    CHECK(chk(r, 1500) == CK_ARP_E_LEN);
    CHECK(chk(r, 64) == CK_ARP_OK);
    CHECK(ck_arp_reply_check(r, 50, 60, OURMAC, OURIP, GWIP, 0, 0) == CK_ARP_E_LEN); /* kept bytes do not cover the frame */
    CHECK(ck_arp_reply_check(0, 60, 60, OURMAC, OURIP, GWIP, 0, 0) == CK_ARP_E_ARG);
    CHECK(ck_arp_reply_check(r, 60, 60, OURMAC, OURIP, 0, 0, 0) == CK_ARP_E_ARG);
}

static void t_classify(void)
{
    uint8_t r[64];
    mkreply(r);
    CHECK(ck_frame_class(r, 60, OURMAC) == CK_FC_UNICAST_US);
    memset(r, 0xff, 6);
    CHECK(ck_frame_class(r, 60, OURMAC) == CK_FC_BROADCAST);
    r[0] = 0x01; r[1] = 0x00; r[2] = 0x5e; r[3] = 0; r[4] = 0; r[5] = 0xfb;
    CHECK(ck_frame_class(r, 60, OURMAC) == CK_FC_MULTICAST);
    mkreply(r); r[3] ^= 1;
    CHECK(ck_frame_class(r, 60, OURMAC) == CK_FC_UNICAST_OTHER);
    CHECK(ck_frame_class(r, 13, OURMAC) == CK_FC_RUNT && ck_frame_class(0, 60, OURMAC) == CK_FC_RUNT);
}

static void wire_put(const uint8_t *d, uint32_t len)
{
    memcpy(f.wire[f.nwire], d, len);
    f.wire_len[f.nwire++] = len;
}
static void mkex(struct ck_arp_exchange *e)
{
    *e = (struct ck_arp_exchange){0};
    memcpy(e->mac, OURMAC, 6);
    memcpy(e->our_ip, OURIP, 4);
    memcpy(e->target_ip, GWIP, 4);
    e->speed = 1000;
    e->full_duplex = 1;
    e->attempts = 3;
    e->wait_us = 20000;
}
static void bcast(uint8_t *b, uint8_t tag)
{
    memset(b, 0, 60);
    memset(b, 0xff, 6);
    b[6] = 0x10; b[7] = tag;
    b[12] = 0x08; b[13] = 0x06; b[14] = 0; b[15] = 1; b[16] = 8; b[17] = 0; b[18] = 6; b[19] = 4; b[21] = 1; /* someone else's request */
}

static uint64_t fake_clock;
static uint64_t fclock(void) { fake_clock += 1000; return fake_clock; }

static void t_set_link(void)
{
    uint32_t n;
    fresh();
    CHECK(ck_gem_set_link(&g, 1000, 1) == 0);
    n = f.r[CK_GEM_NCFGR / 4];
    CHECK((n & CK_GEM_NCFGR_GBE) && (n & CK_GEM_NCFGR_FD) && (n & CK_GEM_NCFGR_DRFCS) && !(n & CK_GEM_NCFGR_SPD));
    CHECK(((n >> 18) & 7u) == 5u);                 /* MDC divider kept */
    CHECK(((n >> CK_GEM_NCFGR_DBW_SHIFT) & 3u) == g.caps.dbw);
    CHECK(ck_gem_set_link(&g, 100, 0) == 0);
    n = f.r[CK_GEM_NCFGR / 4];
    CHECK((n & CK_GEM_NCFGR_SPD) && !(n & CK_GEM_NCFGR_GBE) && !(n & CK_GEM_NCFGR_FD));
    CHECK(ck_gem_set_link(&g, 10, 1) == 0);
    n = f.r[CK_GEM_NCFGR / 4];
    CHECK(!(n & CK_GEM_NCFGR_SPD) && !(n & CK_GEM_NCFGR_GBE) && (n & CK_GEM_NCFGR_FD));
    CHECK(ck_gem_set_link(&g, 777, 1) == CK_GEM_E_ARG && ck_gem_set_link(0, 1000, 1) == CK_GEM_E_ARG);
    CHECK(f.bme_on_n == 0 && f.kicks == 0);        /* no DMA, no bus-master */
    f.r[CK_GEM_NCR / 4] |= CK_GEM_NCR_TE;          /* refused while the MAC runs */
    CHECK(ck_gem_set_link(&g, 1000, 1) == CK_GEM_E_ARG);
    f.r[CK_GEM_NCR / 4] &= ~(uint32_t)CK_GEM_NCR_TE;
    g.halted = 1;
    CHECK(ck_gem_set_link(&g, 1000, 1) == CK_GEM_E_HALTED);
}

static uint32_t pre_calls, pre_last;
static void pre_cb(void *ctx, uint32_t a) { pre_calls++; pre_last = a; CHECK(ctx == (void *)&pre_calls); }

static void t_exchange_ok(void)
{
    struct ck_arp_exchange e;
    uint8_t b1[60], b2[60], rp[60], q[60];
    fresh();
    bcast(b1, 1); bcast(b2, 2); mkreply(rp);
    wire_put(b1, 60); wire_put(rp, 60); wire_put(b2, 60);
    mkex(&e);
    pre_calls = 0; e.pre_attempt = pre_cb; e.pre_ctx = (void *)&pre_calls;
    e.now_us = fclock;
    CHECK(ck_gem_arp_exchange(&g, &e) == 0);
    CHECK(e.matched == 1 && e.attempts_used == 1 && e.tx_ok == 1);
    CHECK(pre_calls == 1 && pre_last == 0);
    CHECK(e.attempt_start_us[0] != 0 && e.attempt_start_us[1] == 0);
    CHECK(e.seen_n == 3 && e.seen_len[0] == 60 && e.seen_attempt[0] == 1 && e.seen_attempt[2] == 1);
    CHECK(memcmp(e.seen_hdr[1], rp, CK_ARP_HDRKEEP) == 0 && memcmp(e.seen_hdr[0], b1, CK_ARP_HDRKEEP) == 0);
    CHECK(memcmp(e.reply_mac, GWMAC, 6) == 0 && memcmp(e.reply_ip, GWIP, 4) == 0 && e.reply_len == 60);
    CHECK(memcmp(e.reply_frame, rp, 60) == 0);
    CHECK(e.frames_seen == 3 && e.n_broadcast == 2 && e.n_multicast == 0 && e.n_unicast_us == 0 && e.n_unicast_other == 0 && e.n_runt == 0);
    /* what went out was the 60 byte broadcast request with our real MAC and IP */
    CHECK(ck_arp_request_build(OURMAC, OURIP, GWIP, q, sizeof q) == 60);
    CHECK(f.frames_sent == 1 && f.last_tx_len == 60 && memcmp(f.last_tx, q, 60) == 0);
    CHECK(f.r[CK_GEM_SA1B / 4] == 0x3f67cf2cu && f.r[CK_GEM_SA1T / 4] == 0x00007654u);
    /* bus-master and the MAC are off afterwards */
    CHECK(f.bme == 0 && g.bme_on == 0 && f.bme_on_n == 1 && f.bme_off_n == 1 && e.bme_off_at_end == 1);
    CHECK(!(f.r[CK_GEM_NCR / 4] & (CK_GEM_NCR_RE | CK_GEM_NCR_TE | CK_GEM_NCR_LLB)));
    CHECK(g.halted == 0);
    clean_flags();
}

static void t_exchange_refusals(void)
{
    struct ck_arp_exchange e;
    uint8_t rp[1536], bc[60];
    int k;
    /* the "reply" arrives but fails exactly one check; the exchange must not accept it, on every attempt */
    for (k = 0; k < 6; k++) {
        fresh();
        mkreply(rp);
        if (k == 0) rp[0] ^= 2;                  /* to another MAC: seen as unicast_other */
        if (k == 1) rp[21] = 1;                  /* wrong opcode */
        if (k == 2) rp[38] ^= 1;                 /* not our IP */
        if (k == 3) rp[28] ^= 1;                 /* not the gateway's IP */
        if (k == 4) rp[32] ^= 1;                 /* not our target MAC */
        if (k == 5) { memset(rp + 60, 0, 40); }  /* 100 byte frame: oversize */
        wire_put(rp, k == 5 ? 100 : 60);
        mkex(&e);
        CHECK(ck_gem_arp_exchange(&g, &e) == 0);
        CHECK(e.matched == 0 && e.attempts_used == 3 && e.frames_seen == 3 && f.frames_sent == 3);
        CHECK(e.reply_len == 0);
        if (k == 0) CHECK(e.n_unicast_other == 3 && e.n_unicast_us == 0);
        if (k == 1) CHECK(e.n_unicast_us == 3 && e.n_arp_refused[-CK_ARP_E_OP] == 3);
        if (k == 2) CHECK(e.n_arp_refused[-CK_ARP_E_TPA] == 3);
        if (k == 3) CHECK(e.n_arp_refused[-CK_ARP_E_SPA] == 3);
        if (k == 4) CHECK(e.n_arp_refused[-CK_ARP_E_THA] == 3);
        if (k == 5) CHECK(e.n_unicast_us == 3 && e.n_arp_refused[-CK_ARP_E_LEN] == 3);
        CHECK(f.bme == 0 && f.bme_on_n == 3 && f.bme_off_n == 3);
        clean_flags();
    }
    /* only broadcasts on the wire: counted, never accepted */
    fresh();
    bcast(bc, 1); wire_put(bc, 60); bcast(bc, 2); wire_put(bc, 60);
    mkex(&e);
    e.attempts = 1;
    CHECK(ck_gem_arp_exchange(&g, &e) == 0);
    CHECK(e.matched == 0 && e.frames_seen == 2 && e.n_broadcast == 2);
    clean_flags();
    /* nothing at all */
    fresh();
    mkex(&e);
    CHECK(ck_gem_arp_exchange(&g, &e) == 0 && e.matched == 0 && e.frames_seen == 0 && e.attempts_used == 3 && e.tx_ok == 3);
    clean_flags();
    /* a reply on the second attempt stops the loop there */
    fresh();
    mkex(&e);
    CHECK(ck_gem_arp_exchange(&g, &e) == 0 && e.matched == 0);
    mkreply(rp);
    wire_put(rp, 60);
    mkex(&e);
    CHECK(ck_gem_arp_exchange(&g, &e) == 0 && e.matched == 1 && e.attempts_used == 1);
}

static void t_exchange_args_and_lies(void)
{
    struct ck_arp_exchange e;
    struct ck_gem_xfer x;
    uint8_t fr[64], rp[60];
    fresh();
    mkex(&e); e.attempts = 0;
    CHECK(ck_gem_arp_exchange(&g, &e) == CK_GEM_E_ARG);
    mkex(&e); e.attempts = 9;
    CHECK(ck_gem_arp_exchange(&g, &e) == CK_GEM_E_ARG);
    mkex(&e); e.speed = 7;
    CHECK(ck_gem_arp_exchange(&g, &e) == CK_GEM_E_ARG);
    mkex(&e); e.wait_us = 10000001u;
    CHECK(ck_gem_arp_exchange(&g, &e) == CK_GEM_E_ARG);
    CHECK(ck_gem_arp_exchange(&g, 0) == CK_GEM_E_ARG && ck_gem_arp_exchange(0, &e) == CK_GEM_E_ARG);
    CHECK(f.bme_on_n == 0 && f.kicks == 0 && g.halted == 0);
    /* RX buffer count: 7 allowed, 8 refused */
    mkx(&x, CK_GEMX_LLB, fr, 1);
    x.rx_count = CK_GEM_RXMAX + 1;
    CHECK(ck_gem_xfer(&g, &x) == CK_GEM_E_ARG);
    mkx(&x, CK_GEMX_LLB, fr, 1);
    x.rx_count = CK_GEM_RXMAX;
    CHECK(ck_gem_xfer(&g, &x) == 0 && x.rx_done && x.rx_all_n == 1 && x.rx_all_len[0] == 60);
    clean_flags();
    /* a device that claims more bytes than the buffer holds: the exchange fails, nothing is accepted, window latched */
    fresh();
    mkreply(rp);
    wire_put(rp, 60);
    f.rx_len_lie = 1;
    mkex(&e);
    CHECK(ck_gem_arp_exchange(&g, &e) == CK_GEM_E_DEVICE);
    CHECK(e.matched == 0 && g.halted == 1 && ck_dmaw_halted(&dw) && f.bme == 0);
    mkex(&e);
    CHECK(ck_gem_arp_exchange(&g, &e) == CK_GEM_E_HALTED);
    /* the root-complex gate and the descriptor validator still apply */
    fresh();
    g.hw_window_ok = 0;
    mkex(&e);
    CHECK(ck_gem_arp_exchange(&g, &e) == CK_GEM_E_HWWIN && f.bme_on_n == 0 && f.kicks == 0);
}

int main(void)
{
    t_probe();
    t_loopback();
    t_phy_loopback();
    t_wire_no_loop();
    t_refuse_outside();
    t_hwwin_gate();
    t_arg_checks();
    t_device_lies();
    t_bme_failures();
    t_tieoff_and_queues();
    t_quiesce_order();
    t_raw_outside_for_hw_test();
    t_frames();
    t_mdio_phy();
    t_bme_set();
    t_arp_build();
    t_arp_check();
    t_classify();
    t_set_link();
    t_exchange_ok();
    t_exchange_refusals();
    t_exchange_args_and_lies();
    free(f.mem);
    return ck_t_verdict("test_pi5_gem");
}
