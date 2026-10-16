/* pi5_gem.c -- see pi5_gem.h. Pi 5 network cut 3: one frame through the RP1 GEM with descriptors only in the
 * ADR 0020 bounce window. software-only DMA confinement (Pi 5, weaker than the Spark). */
#include "pi5_gem.h"

/* TEST-only mutants (make pi5-gem-mutants): each removes one decision and must make test_pi5_gem FAIL.
 * Host builds only; never part of an image. */
#if defined(CK_GEM_MUTANT_NO_VALIDATE) || defined(CK_GEM_MUTANT_BME_EARLY) || defined(CK_GEM_MUTANT_BME_LEFT_ON) || \
    defined(CK_GEM_MUTANT_NO_QUIESCE_WAIT) || defined(CK_GEM_MUTANT_NO_RETE_OFF) || defined(CK_GEM_MUTANT_NO_BARRIER) || \
    defined(CK_GEM_MUTANT_NO_TIEOFF) || defined(CK_GEM_MUTANT_NO_RXLEN_CHECK) || defined(CK_GEM_MUTANT_NO_TX_LAST) || \
    defined(CK_GEM_MUTANT_NO_SENTINEL_USED) || defined(CK_GEM_MUTANT_NO_ADDR64) || defined(CK_GEM_MUTANT_NO_HWWIN) || \
    defined(CK_GEM_MUTANT_NOT_STICKY) || defined(CK_GEM_MUTANT_NO_FRAMECAP) || defined(CK_GEM_MUTANT_NO_SEQ) || \
    defined(CK_GEM_MUTANT_NO_DIFF) || defined(CK_GEM_MUTANT_MDIO_OP) || defined(CK_GEM_MUTANT_PHYLB_BIT) || \
    defined(CK_GEM_MUTANT_NO_WRAP) || defined(CK_GEM_MUTANT_NO_GBE) || defined(CK_GEM_MUTANT_NO_DRFCS) || \
    defined(CK_GEM_MUTANT_NO_BME_VERIFY) || defined(CK_GEM_MUTANT_NO_LLB_CLEAR)
#if !__STDC_HOSTED__
#error "CK_GEM_MUTANT_* are host test mutants only"
#endif
#ifdef CK_HARDWARE_STAGING
#error "CK_GEM_MUTANT_* cannot be combined with CK_HARDWARE_STAGING"
#endif
#endif

#define GEM(off) (CK_RP1_ETH + (off))
static uint32_t grd(const struct ck_gem *g, uint32_t off) { return g->rio->rd(g->rio->ctx, GEM(off)); }
static void gwr(const struct ck_gem *g, uint32_t off, uint32_t v) { g->rio->wr(g->rio->ctx, GEM(off), v); }
static void gdelay(const struct ck_gem *g, uint32_t us)
{
    if (g->rio->delay_us && us)
        g->rio->delay_us(g->rio->ctx, us);
}
static void gbarrier(const struct ck_gem *g)
{
#ifndef CK_GEM_MUTANT_NO_BARRIER
    if (g->barrier)
        g->barrier();
#else
    (void)g;
#endif
}

/* ---------------- probe ---------------- */

int ck_gem_probe(const struct ck_rp1_io *io, struct ck_gem_caps *c)
{
    uint32_t dbwdef, i;
    if (!io || !c)
        return CK_GEM_E_ARG;
    *c = (struct ck_gem_caps){0};
    c->mid = io->rd(io->ctx, GEM(CK_GEM_MID));
    c->is_gem = ((c->mid >> CK_GEM_MID_IDNUM_SHIFT) & 0xfffu) >= 2u; /* hw_is_gem [M2] */
    if (!c->is_gem)
        return CK_GEM_E_NOGEM;
    c->dcfg1 = io->rd(io->ctx, GEM(CK_GEM_DCFG1));
    c->dcfg2 = io->rd(io->ctx, GEM(CK_GEM_DCFG2));
    c->dcfg6 = io->rd(io->ctx, GEM(CK_GEM_DCFG6));
    c->daw64 = (c->dcfg6 & CK_GEM_DCFG6_DAW64) != 0;
    c->queue_mask = 1u | (c->dcfg6 & 0xffu); /* macb_probe_queues: bit 0 is never set but queue 0 always exists */
    for (i = 0; i < CK_GEM_MAX_QUEUES; i++)
        if (c->queue_mask & (1u << i))
            c->nqueues++;
    c->desc_bytes = c->daw64 ? 16u : 8u;
    dbwdef = (c->dcfg1 >> CK_GEM_DCFG1_DBWDEF_SHIFT) & 7u;
    c->dbw = dbwdef == 4 ? 2u : dbwdef == 2 ? 1u : 0u; /* macb_dbw: 128 / 64 / 32 bit */
    return 0;
}

void ck_gem_init(struct ck_gem *g, const struct ck_rp1_io *rio, struct ck_dmaw *w, volatile uint8_t *va,
                 int (*bme)(void *, int), void *bme_ctx, void (*barrier)(void), int hw_window_ok)
{
    *g = (struct ck_gem){0};
    g->rio = rio;
    g->w = w;
    g->va = va;
    g->bme = bme;
    g->bme_ctx = bme_ctx;
    g->barrier = barrier;
    g->hw_window_ok = hw_window_ok;
}

/* ---------------- window memory (CPU side) ---------------- */

static int in_win(const struct ck_gem *g, uint64_t bus, uint64_t len)
{
    return bus >= g->w->base && len <= g->w->size && (bus - g->w->base) <= (g->w->size - len);
}
static uint32_t m_rd(const struct ck_gem *g, uint64_t bus)
{
    return in_win(g, bus, 4) ? *(volatile uint32_t *)(g->va + (bus - g->w->base)) : 0xdeadbeefu;
}
static void m_wr(const struct ck_gem *g, uint64_t bus, uint32_t v)
{
    if (in_win(g, bus, 4))
        *(volatile uint32_t *)(g->va + (bus - g->w->base)) = v;
}
static void m_zero(const struct ck_gem *g, uint64_t bus, uint64_t len)
{
    uint64_t i;
    if (!in_win(g, bus, len))
        return;
    for (i = 0; i < len; i++)
        g->va[(bus - g->w->base) + i] = 0;
}

/* ---------------- frames ---------------- */

static const char k_magic[] = "AIENOS-PI5-NET3"; /* 15 chars + NUL = 16 bytes */

uint32_t ck_gem_frame_build(int kind, const uint8_t mac[6], uint32_t seq, const uint8_t tpa[4], uint8_t *out, uint32_t cap)
{
    uint32_t i;
    if (!mac || !out || cap < 60u)
        return 0;
    for (i = 0; i < 60u; i++)
        out[i] = 0;
    for (i = 0; i < 6; i++) {
        out[i] = 0xff;
        out[6 + i] = mac[i];
    }
    if (kind == CK_GEMF_RAW) {
        out[12] = (uint8_t)(CK_GEMF_ETHERTYPE_EXP >> 8);
        out[13] = (uint8_t)CK_GEMF_ETHERTYPE_EXP;
        for (i = 0; i < 16; i++)
            out[14 + i] = (uint8_t)k_magic[i];
#ifndef CK_GEM_MUTANT_NO_SEQ
        out[30] = (uint8_t)(seq >> 24);
        out[31] = (uint8_t)(seq >> 16);
        out[32] = (uint8_t)(seq >> 8);
        out[33] = (uint8_t)seq;
#endif
        for (i = 34; i < 60u; i++)
            out[i] = (uint8_t)(seq + i * 7u + 1u);
        return 60u;
    }
    if (kind == CK_GEMF_ARP) {
        static const uint8_t hdr[8] = {0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01}; /* htype 1, ptype IPv4, hlen 6, plen 4, request */
        out[12] = 0x08;
        out[13] = 0x06;
        for (i = 0; i < 8; i++)
            out[14 + i] = hdr[i];
        for (i = 0; i < 6; i++)
            out[22 + i] = mac[i]; /* sender hardware address; sender IP (28..31) and target hardware (32..37) stay zero: a probe */
        if (tpa)
            for (i = 0; i < 4; i++)
                out[38 + i] = tpa[i];
        out[42] = 'A'; out[43] = 'I'; out[44] = 'E'; out[45] = 'N'; out[46] = '3';
#ifndef CK_GEM_MUTANT_NO_SEQ
        out[47] = (uint8_t)(seq >> 24);
        out[48] = (uint8_t)(seq >> 16);
        out[49] = (uint8_t)(seq >> 8);
        out[50] = (uint8_t)seq;
#endif
        for (i = 51; i < 60u; i++)
            out[i] = (uint8_t)(seq + i * 7u + 1u);
        return 60u;
    }
    return 0;
}

uint32_t ck_gem_frame_diff(const uint8_t *a, uint32_t alen, const uint8_t *b, uint32_t blen)
{
    uint32_t i, n = alen < blen ? alen : blen;
#ifdef CK_GEM_MUTANT_NO_DIFF
    if (n)
        n--; /* mutant: ignores the last byte */
    if (alen != blen && n == 0)
        return 1;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return i + 1;
    return 0;
#else
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return i + 1;
    return alen == blen ? 0 : n + 1;
#endif
}

/* ---------------- bus-master ---------------- */

int ck_pi5gem_bme_set(const struct ck_pi5pcie_io *io, int on)
{
    uint32_t v = 0, nv;
    if (!io || ck_pi5pcie_cfg_read32(io, 1, 0, 0, CK_PI5NET_PCI_COMMAND, &v))
        return CK_GEM_E_ARG;
    nv = (v & 0xffffu); /* status half written as zero: RW1C, nothing cleared */
    nv = on ? (nv | CK_PI5NET_CMD_BME) : (nv & ~(uint32_t)CK_PI5NET_CMD_BME);
    if (ck_pi5net_cfg_write32(io, 1, 0, 0, CK_PI5NET_PCI_COMMAND, nv))
        return CK_GEM_E_ARG;
#ifndef CK_GEM_MUTANT_NO_BME_VERIFY
    if (ck_pi5pcie_cfg_read32(io, 1, 0, 0, CK_PI5NET_PCI_COMMAND, &v))
        return CK_GEM_E_ARG;
    if (((v & CK_PI5NET_CMD_BME) != 0) != (on != 0))
        return CK_GEM_E_BME;
#endif
    return 0;
}

/* ---------------- descriptors (CPU writes into the window) ---------------- */

/* Hand a TX slot to the device: address words first, ctrl (USED clear) last [M2 macb_tx_map]. */
static void tx_desc_write(const struct ck_gem *g, uint64_t slot_bus, uint64_t buf, uint32_t len, uint32_t flags_ctrl)
{
    uint32_t dsz = g->caps.desc_bytes;
    m_wr(g, slot_bus + 0, (uint32_t)buf);
    if (dsz == 16) {
        m_wr(g, slot_bus + 8, (uint32_t)(buf >> 32));
        m_wr(g, slot_bus + 12, 0);
    }
    gbarrier(g);
    m_wr(g, slot_bus + 4, (len & CK_GEMD_TX_FRMLEN_MASK) | flags_ctrl);
}

/* RX slot: ctrl 0 and the high address first, the low address word (USED clear) last [M2 gem_rx_refill]. */
static void rx_desc_write(const struct ck_gem *g, uint64_t slot_bus, uint64_t buf, uint32_t addr_flags)
{
    uint32_t dsz = g->caps.desc_bytes;
    m_wr(g, slot_bus + 4, 0);
    if (dsz == 16) {
        m_wr(g, slot_bus + 8, (uint32_t)(buf >> 32));
        m_wr(g, slot_bus + 12, 0);
    }
    gbarrier(g);
    m_wr(g, slot_bus + 0, ((uint32_t)buf & ~3u) | addr_flags);
}

static int post(struct ck_dmaw_ring *r, struct ck_dmaw *w, uint64_t addr, uint32_t len, uint32_t flags, uint32_t *idx, int caller_chosen)
{
#ifdef CK_GEM_MUTANT_NO_VALIDATE
    if (caller_chosen) { /* mutant: a caller-chosen address is trusted */
        struct ck_dmaw_slot *s = &r->slots[r->head];
        s->d.addr = addr; s->d.len = len; s->d.flags = flags; s->state = CK_DMAW_SLOT_INFLIGHT;
        *idx = r->head; r->head = (r->head + 1) & (r->n - 1); r->inflight++;
        return CK_DMAW_OK;
    }
#else
    (void)caller_chosen;
#endif
    return ck_dmaw_ring_post(r, w, addr, len, flags, idx);
}

#ifdef CK_GEM_MUTANT_NO_VALIDATE
static int emit(struct ck_dmaw_ring *r, struct ck_dmaw *w, uint32_t i, struct ck_dmaw_desc *d)
{
    (void)w;
    *d = r->slots[i].d; /* mutant: shadow trusted without re-validation */
    return 0;
}
#else
#define emit ck_dmaw_ring_emit
#endif

/* ---------------- hardware programming ---------------- */

static void gem_reset_hw(const struct ck_gem *g)
{
    uint32_t ncr = grd(g, CK_GEM_NCR), q;
    ncr &= ~(CK_GEM_NCR_RE | CK_GEM_NCR_TE | CK_GEM_NCR_TSTART); /* macb_reset_hw [M2] */
#ifndef CK_GEM_MUTANT_NO_LLB_CLEAR
    ncr &= ~CK_GEM_NCR_LLB;
#endif
    ncr |= CK_GEM_NCR_CLRSTAT;
    gwr(g, CK_GEM_NCR, ncr);
    gwr(g, CK_GEM_TSR, 0xffffffffu);
    gwr(g, CK_GEM_RSR, 0xffffffffu);
    gwr(g, CK_GEM_PBUFRXCUT, 0);
    gwr(g, CK_GEM_IDR, 0xffffffffu);
    (void)grd(g, CK_GEM_ISR); /* read clears (no MACB_CAPS_ISR_CLEAR_ON_WRITE for RP1) */
    for (q = 1; q < CK_GEM_MAX_QUEUES; q++) {
        if (!(g->caps.queue_mask & (1u << q)))
            continue;
        gwr(g, CK_GEM_QN_IDR(q), 0xffffffffu);
        (void)grd(g, CK_GEM_QN_ISR(q));
    }
}

static void gem_set_mac(const struct ck_gem *g, const uint8_t m[6])
{
    gwr(g, CK_GEM_SA1B, (uint32_t)m[0] | ((uint32_t)m[1] << 8) | ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24));
    gwr(g, CK_GEM_SA1T, (uint32_t)m[4] | ((uint32_t)m[5] << 8));
    gwr(g, CK_GEM_SA2B, 0); gwr(g, CK_GEM_SA2B + 4, 0);
    gwr(g, CK_GEM_SA2B + 8, 0); gwr(g, CK_GEM_SA2B + 12, 0);
    gwr(g, CK_GEM_SA2B + 16, 0); gwr(g, CK_GEM_SA2B + 20, 0);
    gwr(g, CK_GEM_HRB, 0);
    gwr(g, CK_GEM_HRT, 0);
}

static uint32_t ncfgr_for(const struct ck_gem *g, uint32_t speed, int full_duplex)
{
    uint32_t cfg = grd(g, CK_GEM_NCFGR);
    /* NCFGR [M2 macb_init_hw + macb_mac_link_up]: keep the MDC divider the MDIO code set; data bus width from DCFG1;
     * discard the FCS on receive; broadcast accepted (NBC clear); no copy-all; standard frame size (no BIG/JFRAME). */
    cfg &= (uint32_t)CK_GEM_NCFGR_CLK_MASK;
    cfg |= g->caps.dbw << CK_GEM_NCFGR_DBW_SHIFT;
#ifndef CK_GEM_MUTANT_NO_DRFCS
    cfg |= CK_GEM_NCFGR_DRFCS;
#endif
    if (full_duplex)
        cfg |= CK_GEM_NCFGR_FD;
    if (speed == 100)
        cfg |= CK_GEM_NCFGR_SPD;
#ifndef CK_GEM_MUTANT_NO_GBE
    if (speed == 1000)
        cfg |= CK_GEM_NCFGR_GBE;
#endif
    return cfg;
}

/* Cut 4: Linux macb_mac_link_up writes the MAC speed the moment the PHY link is up, before any traffic. The RP1 RGMII
 * transmit clock follows the MAC speed in hardware (RP1 datasheet ETH_CFG CLKGEN SPEED_FROM_MAC), so a MAC left at its
 * 10 Mbit default drives a 2.5 MHz clock into a PHY that runs at 1000. RE/TE stay off; no DMA is involved. */
int ck_gem_set_link(struct ck_gem *g, uint32_t speed, int full_duplex)
{
    if (!g || !g->rio || (speed != 10 && speed != 100 && speed != 1000))
        return CK_GEM_E_ARG;
    if (g->halted)
        return CK_GEM_E_HALTED;
    if (grd(g, CK_GEM_NCR) & (CK_GEM_NCR_RE | CK_GEM_NCR_TE))
        return CK_GEM_E_ARG;
#ifndef CK_GEM_MUTANT_SETLINK_NO_WRITE
    gwr(g, CK_GEM_NCFGR, ncfgr_for(g, speed, full_duplex));
#else
    (void)full_duplex;
#endif
    return 0;
}

static void gem_config(const struct ck_gem *g, const struct ck_gem_xfer *x)
{
    uint32_t dma, amp;
    gwr(g, CK_GEM_NCFGR, ncfgr_for(g, x->speed, x->full_duplex));
    /* gem_init_axi [M2]: AXI pipelines as rp1.dtsi sets them. */
    amp = grd(g, CK_GEM_AMP);
    amp |= CK_GEM_AMP_AW2B_FILL;
    amp = (amp & ~(uint32_t)CK_GEM_AMP_AW2W_MASK) | (CK_GEM_AXI_PIPE << CK_GEM_AMP_AW2W_SHIFT);
    amp = (amp & ~(uint32_t)CK_GEM_AMP_AR2R_MASK) | CK_GEM_AXI_PIPE;
    gwr(g, CK_GEM_AMP, amp);
    /* macb_configure_dma [M2] */
    dma = grd(g, CK_GEM_DMACFG);
    dma &= ~(uint32_t)(CK_GEM_DMACFG_RXBS_MASK | CK_GEM_DMACFG_FBLDO_MASK | CK_GEM_DMACFG_ENDIA_PKT |
                       CK_GEM_DMACFG_ENDIA_DESC | CK_GEM_DMACFG_TXCOEN | CK_GEM_DMACFG_RXEXT | CK_GEM_DMACFG_TXEXT |
                       CK_GEM_DMACFG_ADDR64);
    dma |= ((x->rx_buf_len ? x->rx_buf_len : 64u) / CK_GEM_RXBS_UNIT) << CK_GEM_DMACFG_RXBS_SHIFT;
    dma |= CK_GEM_BURST;
    dma |= CK_GEM_DMACFG_TXPBMS | CK_GEM_DMACFG_RXBMS_MASK;
#ifndef CK_GEM_MUTANT_NO_ADDR64
    if (g->caps.daw64)
        dma |= CK_GEM_DMACFG_ADDR64;
#endif
    gwr(g, CK_GEM_DMACFG, dma);
    /* macb_init: USRIO for an RGMII phy-mode (rp1.dtsi: rgmii-id). */
    gwr(g, CK_GEM_USRIO, CK_GEM_USRIO_RGMII);
}

/* ---------------- one transfer ---------------- */

static int refuse(struct ck_gem *g, int rc)
{
    (void)g;
#ifndef CK_GEM_MUTANT_NOT_STICKY
    g->halted = 1;
#endif
    return rc;
}

int ck_gem_xfer(struct ck_gem *g, struct ck_gem_xfer *x)
{
    struct ck_dmaw *w;
    struct ck_dmaw_slot txs[CK_GEM_TXRING], rxs[CK_GEM_RXRING];
    struct ck_dmaw_ring txr, rxr;
    struct ck_dmaw_desc d;
    uint64_t next0, txring, rxring, tie_tx, tie_rx, txbuf, sentbuf, rxbuf[CK_GEM_RXMAX] = {0};
    uint32_t dsz, i, idx, polls, q, ncr, flags, scan = 0;
    int probe_stop = 0;
    int rc = 0, qrc = 0, prc;

    if (!g || !g->w || !g->rio || !x || !x->frame)
        return CK_GEM_E_ARG;
    w = g->w;
    x->tx_done = x->rx_done = 0;
    x->tx_ctrl = x->rx_word0 = x->rx_ctrl = x->rx_len = 0;
    x->tsr = x->rsr = x->isr = x->ncr_run = x->ncfgr_run = x->dmacfg_run = x->ncr_end = x->tsr_end = 0;
    x->tx_bus = x->rx_bus = 0;
    x->rx_copied = x->rx_frames = x->rx_slot = 0;
    x->rx_all_n = 0;
    x->rx_probe_hit = 0;
    for (i = 0; i < CK_GEM_RXMAX; i++)
        x->rx_all_len[i] = 0;
    x->bme_enabled_for_us = 0;
    x->bme_off_at_end = 0;
#ifndef CK_GEM_MUTANT_NOT_STICKY
    if (g->halted || ck_dmaw_halted(w))
        return CK_GEM_E_HALTED;
#endif
    if (x->mode < CK_GEMX_LLB || x->mode > CK_GEMX_WIRE || (x->speed != 10 && x->speed != 100 && x->speed != 1000) ||
        x->rx_count > CK_GEM_RXMAX || (x->rx_count && (x->rx_buf_len < 64u || x->rx_buf_len > CK_DMAW_MAX_FRAME || (x->rx_buf_len % 64u))))
        return CK_GEM_E_ARG;
#ifdef CK_GEM_MUTANT_NO_FRAMECAP
    if (x->frame_len < 60u)
#else
    if (x->frame_len < 60u || x->frame_len > 1514u)
#endif
        return CK_GEM_E_ARG;
    if (!g->caps.is_gem)
        return CK_GEM_E_NOGEM;
#ifndef CK_GEM_MUTANT_NO_HWWIN
    if (!g->hw_window_ok)
        return CK_GEM_E_HWWIN; /* no root-complex backstop confirmed: bus-master stays off, nothing touched */
#endif
#ifdef CK_GEM_MUTANT_BME_EARLY
    if (g->bme)
        (void)g->bme(g->bme_ctx, 1); /* mutant: bus-master before anything is validated */
#endif
    dsz = g->caps.desc_bytes;
    next0 = w->next;

    /* ---- 1. layout and validation: memory only, no register is touched, bus-master stays off ---- */
    if (ck_dmaw_alloc(w, CK_GEM_TXRING * dsz, 64, &txring) || ck_dmaw_alloc(w, CK_GEM_RXRING * dsz, 64, &rxring) ||
        ck_dmaw_alloc(w, dsz, 64, &tie_tx) || ck_dmaw_alloc(w, dsz, 64, &tie_rx) ||
        ck_dmaw_alloc(w, 64, 64, &sentbuf) || ck_dmaw_alloc(w, (uint64_t)((x->frame_len + 63u) & ~63u), 64, &txbuf)) {
        w->next = next0;
        return CK_GEM_E_WINDOW;
    }
    for (i = 0; i < x->rx_count; i++)
        if (ck_dmaw_alloc(w, x->rx_buf_len, 64, &rxbuf[i])) {
            w->next = next0;
            return CK_GEM_E_WINDOW;
        }
    if (x->tx_addr)
        txbuf = x->tx_addr;
    if (x->rx_count && x->rx_addr)
        rxbuf[0] = x->rx_addr;
    if (!g->caps.daw64 && ((w->base + w->size - 1) >> 32))
        return refuse(g, CK_GEM_E_WINDOW); /* 32-bit descriptors cannot name this window */
    /* the four tables the device reads or writes, and every buffer, through the ADR 0020 validator */
    if (ck_dmaw_validate_aligned(w, txring, CK_GEM_TXRING * dsz, 64) || ck_dmaw_validate_aligned(w, rxring, CK_GEM_RXRING * dsz, 64) ||
        ck_dmaw_validate_aligned(w, tie_tx, dsz, 64) || ck_dmaw_validate_aligned(w, tie_rx, dsz, 64))
        return refuse(g, CK_GEM_E_WINDOW);
    if (ck_dmaw_ring_init(&txr, txs, CK_GEM_TXRING) || ck_dmaw_ring_init(&rxr, rxs, CK_GEM_RXRING))
        return refuse(g, CK_GEM_E_ARG);
    prc = post(&txr, w, txbuf, x->frame_len, CK_GEMD_TX_LAST, &idx, x->tx_addr != 0);
    if (prc)
        return refuse(g, CK_GEM_E_WINDOW);
    prc = post(&txr, w, sentbuf, 64, CK_GEMD_TX_USED | CK_GEMD_TX_WRAP, &idx, 0); /* sentinel: never executed (USED) */
    if (prc)
        return refuse(g, CK_GEM_E_WINDOW);
    for (i = 0; i < CK_GEM_RXRING; i++) {
        if (i < x->rx_count)
            prc = post(&rxr, w, rxbuf[i], x->rx_buf_len, 0, &idx, i == 0 && x->rx_addr != 0);
        else
            prc = post(&rxr, w, sentbuf, 64, CK_GEMD_RX_USED, &idx, 0);
        if (prc)
            return refuse(g, CK_GEM_E_WINDOW);
    }

    /* ---- 2. write the descriptors from the shadow only ---- */
    m_zero(g, txring, CK_GEM_TXRING * dsz);
    m_zero(g, rxring, CK_GEM_RXRING * dsz);
    m_zero(g, tie_tx, dsz);
    m_zero(g, tie_rx, dsz);
    m_zero(g, sentbuf, 64);
    m_zero(g, txbuf, (uint64_t)((x->frame_len + 63u) & ~63u));
    for (i = 0; i < x->rx_count; i++)
        m_zero(g, rxbuf[i], x->rx_buf_len);
    if (in_win(g, txbuf, x->frame_len))
        for (i = 0; i < x->frame_len; i++)
            g->va[(txbuf - w->base) + i] = x->frame[i]; /* copy only (ADR 0020 rule 5) */
    x->tx_bus = txbuf;
    x->rx_bus = x->rx_count ? rxbuf[0] : 0;

    for (i = 0; i < CK_GEM_TXRING; i++) {
        if (emit(&txr, w, i, &d))
            return refuse(g, CK_GEM_E_WINDOW);
        flags = i == 0 ? 0 : (CK_GEMD_TX_USED | CK_GEMD_TX_WRAP);
#ifdef CK_GEM_MUTANT_NO_SENTINEL_USED
        flags = i == 0 ? 0 : CK_GEMD_TX_WRAP;
#endif
        if (i == 0) {
#ifndef CK_GEM_MUTANT_NO_TX_LAST
            flags |= CK_GEMD_TX_LAST;
#endif
        }
        if (i == 0 && x->raw_tx_outside)
            d.addr = x->raw_tx_outside; /* confinement test only */
        tx_desc_write(g, txring + (uint64_t)i * dsz, d.addr, d.len, flags);
    }
    for (i = 0; i < CK_GEM_RXRING; i++) {
        if (emit(&rxr, w, i, &d))
            return refuse(g, CK_GEM_E_WINDOW);
        flags = (i >= x->rx_count) ? CK_GEMD_RX_USED : 0;
#ifndef CK_GEM_MUTANT_NO_WRAP
        if (i == CK_GEM_RXRING - 1)
            flags |= CK_GEMD_RX_WRAP;
#endif
        if (i == 0 && x->raw_rx_outside)
            d.addr = x->raw_rx_outside; /* confinement test only */
        rx_desc_write(g, rxring + (uint64_t)i * dsz, d.addr, flags);
    }
    /* tie-off descriptors for every other hardware queue [M2 macb_init_tieoff]: USED so the device never uses them */
    tx_desc_write(g, tie_tx, sentbuf, 64, CK_GEMD_TX_USED | CK_GEMD_TX_WRAP);
    rx_desc_write(g, tie_rx, sentbuf, CK_GEMD_RX_USED | CK_GEMD_RX_WRAP);
    gbarrier(g);

    /* ---- 3. program the MAC (bus-master still off) ---- */
    gem_reset_hw(g);
    gem_set_mac(g, x->mac);
    gem_config(g, x);
    gwr(g, CK_GEM_RBQP, (uint32_t)rxring);
    gwr(g, CK_GEM_TBQP, (uint32_t)txring);
    if (g->caps.daw64) {
        gwr(g, CK_GEM_RBQPH, (uint32_t)(rxring >> 32));
        gwr(g, CK_GEM_TBQPH, (uint32_t)(txring >> 32));
    }
#ifndef CK_GEM_MUTANT_NO_TIEOFF
    for (q = 1; q < CK_GEM_MAX_QUEUES; q++) {
        if (!(g->caps.queue_mask & (1u << q)))
            continue;
        gwr(g, CK_GEM_QN_TBQP(q), (uint32_t)tie_tx);
        gwr(g, CK_GEM_QN_RBQP(q), (uint32_t)tie_rx);
        gwr(g, CK_GEM_QN_RBQS(q), (x->rx_buf_len ? x->rx_buf_len : 64u) / CK_GEM_RXBS_UNIT);
    }
#else
    (void)q; (void)tie_tx; (void)tie_rx;
#endif
    x->dmacfg_run = grd(g, CK_GEM_DMACFG);
    x->ncfgr_run = grd(g, CK_GEM_NCFGR);
    gbarrier(g);

    /* ---- 4. bus-master on ONLY now, after every descriptor passed the validator ---- */
    if (!g->bme)
        return refuse(g, CK_GEM_E_BME);
#ifndef CK_GEM_MUTANT_BME_EARLY
    if (g->bme(g->bme_ctx, 1)) {
        (void)g->bme(g->bme_ctx, 0);
        return refuse(g, CK_GEM_E_BME);
    }
#endif
    g->bme_on = 1;
    x->bme_enabled_for_us = 1;
    ncr = grd(g, CK_GEM_NCR);
    ncr |= CK_GEM_NCR_TE;
    if (x->rx_count)
        ncr |= CK_GEM_NCR_RE;
    if (x->mode == CK_GEMX_LLB)
        ncr |= CK_GEM_NCR_LLB;
    gwr(g, CK_GEM_NCR, ncr);
    gbarrier(g);
    gwr(g, CK_GEM_NCR, ncr | CK_GEM_NCR_TSTART);
    x->ncr_run = grd(g, CK_GEM_NCR);
    g->xfers++;

    /* ---- 5. poll (bounded) ---- */
    for (polls = 0; polls <= x->tx_wait_us / 10u; polls++) {
        if (m_rd(g, txring + 4) & CK_GEMD_TX_USED) {
            x->tx_done = 1;
            break;
        }
        gdelay(g, 10);
    }
    if (x->rx_count) {
        for (polls = 0; polls <= x->rx_wait_us / 10u; polls++) {
            if (x->rx_probe) {
                /* Cut 4: look at every completed slot in ring order. Reads only; the slot is accounted for after the
                 * quiesce. A length above the buffer ends the wait here and is refused (and latched) below. */
                while (scan < x->rx_count && !probe_stop) {
                    uint64_t sa = rxring + (uint64_t)scan * dsz;
                    uint32_t sw0 = m_rd(g, sa), sc = m_rd(g, sa + 4), slen, sn;
                    uint8_t sd[CK_GEM_RXKEEP];
                    if (!((sw0 & CK_GEMD_RX_USED) && (sc & (CK_GEMD_RX_SOF | CK_GEMD_RX_EOF)) == (CK_GEMD_RX_SOF | CK_GEMD_RX_EOF)))
                        break;
                    slen = sc & CK_GEMD_RX_FRMLEN_MASK;
                    if (slen > x->rx_buf_len) {
                        probe_stop = 1;
                        break;
                    }
                    {
                        uint8_t *pd = x->rx_probe_buf ? x->rx_probe_buf : sd;
                        uint32_t pc = x->rx_probe_buf ? x->rx_probe_cap : CK_GEM_RXKEEP;
                        sn = slen < pc ? slen : pc;
                        if (!in_win(g, rxbuf[scan], sn)) {
                            probe_stop = 1;
                            break;
                        }
                        for (i = 0; i < sn; i++)
                            pd[i] = g->va[(rxbuf[scan] - w->base) + i];
                        prc = x->rx_probe(x->rx_probe_ctx, pd, sn, slen);
                    }
                    if (prc) {
                        x->rx_probe_hit = 1;
                        probe_stop = 1;
                    }
                    scan++;
                }
                if (probe_stop || scan >= x->rx_count)
                    break;
            } else if ((m_rd(g, rxring) & CK_GEMD_RX_USED) && (m_rd(g, rxring + 4) & (CK_GEMD_RX_SOF | CK_GEMD_RX_EOF)) ==
                                                         (CK_GEMD_RX_SOF | CK_GEMD_RX_EOF))
                break;
            gdelay(g, 10);
        }
    }
    x->tsr = grd(g, CK_GEM_TSR);
    x->rsr = grd(g, CK_GEM_RSR);
    x->isr = grd(g, CK_GEM_ISR);

    /* ---- 6. quiesce: stop RE/TE (and loopback), wait for the transmitter, settle, THEN bus-master off ---- */
    ncr = grd(g, CK_GEM_NCR);
#ifndef CK_GEM_MUTANT_NO_RETE_OFF
    ncr &= ~(CK_GEM_NCR_RE | CK_GEM_NCR_TE | CK_GEM_NCR_LLB | CK_GEM_NCR_TSTART);
    gwr(g, CK_GEM_NCR, ncr);
#endif
#ifndef CK_GEM_MUTANT_NO_QUIESCE_WAIT
    for (polls = 0; polls < 1000u; polls++) {
        if (!(grd(g, CK_GEM_TSR) & CK_GEM_TSR_TGO))
            break;
        gdelay(g, 10);
    }
    if (grd(g, CK_GEM_TSR) & CK_GEM_TSR_TGO)
        qrc = CK_GEM_E_QUIESCE;
#endif
    gdelay(g, 2000); /* let anything already on the bus finish (same margin as the window change) */
    gwr(g, CK_GEM_TSR, 0xffffffffu);
    gwr(g, CK_GEM_RSR, 0xffffffffu);
    x->ncr_end = grd(g, CK_GEM_NCR);
    x->tsr_end = grd(g, CK_GEM_TSR);
#ifndef CK_GEM_MUTANT_BME_LEFT_ON
    if (g->bme(g->bme_ctx, 0)) {
        g->halted = 1;
        return CK_GEM_E_BME;
    }
    g->bme_on = 0;
    x->bme_off_at_end = 1;
#endif
    if (qrc) {
        g->halted = 1;
        rc = qrc;
    }

    /* ---- 7. results: device-written words are read as data only; lengths are checked against the shadow ---- */
    x->tx_ctrl = m_rd(g, txring + 4);
    x->tx_done = (x->tx_ctrl & CK_GEMD_TX_USED) != 0;
    for (i = 0; i < x->rx_count; i++) {
        uint32_t w0 = m_rd(g, rxring + (uint64_t)i * dsz), c = m_rd(g, rxring + (uint64_t)i * dsz + 4);
        if ((w0 & CK_GEMD_RX_USED) && (c & (CK_GEMD_RX_SOF | CK_GEMD_RX_EOF)) == (CK_GEMD_RX_SOF | CK_GEMD_RX_EOF)) {
            uint32_t len = c & CK_GEMD_RX_FRMLEN_MASK, n;
#ifndef CK_GEM_MUTANT_NO_RXLEN_CHECK
            if (ck_dmaw_ring_complete(&rxr, w, i, len)) {
                g->halted = 1; /* the device claims more bytes than the buffer holds */
                return CK_GEM_E_DEVICE;
            }
#endif
            if (x->rx_all_n < CK_GEM_RXMAX) {
                uint32_t kn = len < CK_GEM_RXKEEP ? len : CK_GEM_RXKEEP, k;
                if (kn > x->rx_buf_len)
                    kn = x->rx_buf_len;
                if (in_win(g, rxbuf[i], kn))
                    for (k = 0; k < kn; k++)
                        x->rx_all[x->rx_all_n][k] = g->va[(rxbuf[i] - w->base) + k];
                else
                    kn = 0;
                x->rx_all_len[x->rx_all_n] = len;
                x->rx_all_n++;
            }
            if (x->rx_frames++ == 0) {
                x->rx_slot = i;
                x->rx_done = 1;
                x->rx_word0 = w0;
                x->rx_ctrl = c;
                x->rx_len = len;
                n = len < CK_GEM_RXCOPY ? len : CK_GEM_RXCOPY;
                if (n > x->rx_buf_len)
                    n = x->rx_buf_len;
                if (in_win(g, rxbuf[i], n)) {
                    uint32_t k;
                    for (k = 0; k < n; k++)
                        x->rx_data[k] = g->va[(rxbuf[i] - w->base) + k];
                } else {
                    n = 0;
                }
                x->rx_copied = n;
            }
        }
    }
    if (x->tx_done && ck_dmaw_ring_complete(&txr, w, 0, x->frame_len)) {
        g->halted = 1;
        return CK_GEM_E_DEVICE;
    }
    /* give the window space back, zeroed, so nothing the device left is read later as if it were new */
    m_zero(g, w->base + next0, w->next - next0);
    w->next = next0;
    return rc;
}

void ck_gem_park(struct ck_gem *g)
{
    uint32_t ncr = grd(g, CK_GEM_NCR);
    ncr &= ~(CK_GEM_NCR_RE | CK_GEM_NCR_TE | CK_GEM_NCR_LLB | CK_GEM_NCR_TSTART);
    gwr(g, CK_GEM_NCR, ncr);
}

/* ---------------- Cut 4: ARP request and reply (RFC 826) ---------------- */

uint32_t ck_arp_request_build(const uint8_t mac[6], const uint8_t sip[4], const uint8_t tip[4], uint8_t *out, uint32_t cap)
{
    static const uint8_t hdr[6] = {0x00, 0x01, 0x08, 0x00, 0x06, 0x04}; /* htype Ethernet, ptype IPv4, hlen 6, plen 4 */
    uint32_t i;
    if (!mac || !sip || !tip || !out || cap < CK_ARP_FRAME_LEN)
        return 0;
    for (i = 0; i < CK_ARP_FRAME_LEN; i++)
        out[i] = 0;
    for (i = 0; i < 6; i++) {
        out[i] = 0xff;      /* Ethernet destination: broadcast */
        out[6 + i] = mac[i];
        out[22 + i] = mac[i]; /* ARP sender hardware address */
    }
    out[12] = 0x08;
    out[13] = 0x06;
    for (i = 0; i < 6; i++)
        out[14 + i] = hdr[i];
    out[20] = 0x00;
#ifdef CK_GEM_MUTANT_ARP_OP
    out[21] = 0x02; /* mutant: a reply, not a request */
#else
    out[21] = 0x01; /* request */
#endif
    for (i = 0; i < 4; i++) {
#ifdef CK_GEM_MUTANT_ARP_SIP_ZERO
        out[28 + i] = 0; /* mutant: an ARP probe, not a request */
#else
        out[28 + i] = sip[i];
#endif
        out[38 + i] = tip[i];
    }
    return CK_ARP_FRAME_LEN; /* target hardware address 32..37 stays zero; 42..59 zero padding */
}

static int mac_eq(const uint8_t *a, const uint8_t *b)
{
    uint32_t i;
    for (i = 0; i < 6; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}
static int ip_eq(const uint8_t *a, const uint8_t *b)
{
    uint32_t i;
    for (i = 0; i < 4; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

int ck_arp_reply_check(const uint8_t *d, uint32_t n, uint32_t len, const uint8_t our_mac[6], const uint8_t our_ip[4],
                       const uint8_t asked_ip[4], uint8_t sender_mac[6], uint8_t sender_ip[4])
{
    uint32_t i;
    (void)n;
    if (!d || !our_mac || !our_ip || !asked_ip)
        return CK_ARP_E_ARG;
#ifndef CK_GEM_MUTANT_ARP_NO_LEN
    if (len < CK_ARP_MINLEN || n < CK_ARP_MINLEN || n < (len < CK_GEM_RXKEEP ? len : CK_GEM_RXKEEP))
        return CK_ARP_E_LEN;
#endif
#ifndef CK_GEM_MUTANT_ARP_NO_MAXLEN
    if (len > CK_ARP_MAXLEN)
        return CK_ARP_E_LEN;
#endif
    if (d[12] != 0x08 || d[13] != 0x06)
        return CK_ARP_E_ETYPE;
#ifndef CK_GEM_MUTANT_ARP_NO_DST
    if (!mac_eq(d, our_mac))
        return CK_ARP_E_DST;
#endif
#ifndef CK_GEM_MUTANT_ARP_NO_HDR
    if (d[14] != 0x00 || d[15] != 0x01 || d[16] != 0x08 || d[17] != 0x00 || d[18] != 6 || d[19] != 4)
        return CK_ARP_E_HDR;
#endif
#ifndef CK_GEM_MUTANT_ARP_NO_OP
    if (d[20] != 0x00 || d[21] != 0x02)
        return CK_ARP_E_OP;
#endif
#ifndef CK_GEM_MUTANT_ARP_NO_SRC
    if (!mac_eq(d + 6, d + 22))
        return CK_ARP_E_SRC;
#endif
#ifndef CK_GEM_MUTANT_ARP_NO_THA
    if (!mac_eq(d + 32, our_mac))
        return CK_ARP_E_THA;
#endif
#ifndef CK_GEM_MUTANT_ARP_NO_TPA
    if (!ip_eq(d + 38, our_ip))
        return CK_ARP_E_TPA;
#endif
#ifndef CK_GEM_MUTANT_ARP_NO_SPA
    if (!ip_eq(d + 28, asked_ip))
        return CK_ARP_E_SPA;
#endif
    if (sender_mac)
        for (i = 0; i < 6; i++)
            sender_mac[i] = d[22 + i];
    if (sender_ip)
        for (i = 0; i < 4; i++)
            sender_ip[i] = d[28 + i];
    return CK_ARP_OK;
}

int ck_frame_class(const uint8_t *d, uint32_t n, const uint8_t our_mac[6])
{
    uint32_t i, ff = 0;
    if (!d || !our_mac || n < 14u)
        return CK_FC_RUNT;
    for (i = 0; i < 6; i++)
        ff += d[i] == 0xff;
    if (ff == 6)
        return CK_FC_BROADCAST;
    if (d[0] & 1u)
        return CK_FC_MULTICAST;
    return mac_eq(d, our_mac) ? CK_FC_UNICAST_US : CK_FC_UNICAST_OTHER;
}

static int arp_probe_cb(void *ctx, const uint8_t *d, uint32_t n, uint32_t len)
{
    const struct ck_arp_exchange *e = (const struct ck_arp_exchange *)ctx;
#ifdef CK_GEM_MUTANT_ARP_ACCEPT_ANY
    (void)d; (void)n; (void)len; (void)e;
    return 1; /* mutant: the first frame of any kind ends the wait as a match */
#else
    return ck_arp_reply_check(d, n, len, e->mac, e->our_ip, e->target_ip, 0, 0) == CK_ARP_OK;
#endif
}

int ck_gem_arp_exchange(struct ck_gem *g, struct ck_arp_exchange *e)
{
    uint8_t fr[CK_ARP_FRAME_LEN];
    uint32_t a, i, r;
    if (!g || !e || e->attempts < 1u || e->attempts > CK_ARP_MAXATT || e->wait_us > 10000000u ||
        (e->speed != 10 && e->speed != 100 && e->speed != 1000))
        return CK_GEM_E_ARG;
    e->matched = 0;
    e->attempts_used = e->tx_ok = 0;
    e->reply_len = 0;
    e->frames_seen = e->n_broadcast = e->n_multicast = e->n_unicast_us = e->n_unicast_other = e->n_runt = 0;
    for (i = 0; i < CK_ARP_NREASONS; i++)
        e->n_arp_refused[i] = 0;
    e->seen_n = 0;
    for (i = 0; i < CK_ARP_MAXATT; i++)
        e->attempt_start_us[i] = 0;
    e->last_rc = 0;
    e->last_tsr = e->last_rsr = e->last_isr = 0;
    e->bme_off_at_end = 0;
    if (ck_arp_request_build(e->mac, e->our_ip, e->target_ip, fr, sizeof fr) != CK_ARP_FRAME_LEN)
        return CK_GEM_E_ARG;
    for (a = 0; a < e->attempts && !e->matched; a++) {
        struct ck_gem_xfer x = {0};
        int rc;
        if (e->pre_attempt)
            e->pre_attempt(e->pre_ctx, a);
        e->attempt_start_us[a] = e->now_us ? e->now_us() : 0;
        x.mode = CK_GEMX_WIRE;
        x.speed = e->speed;
        x.full_duplex = e->full_duplex;
        for (i = 0; i < 6; i++)
            x.mac[i] = e->mac[i];
        x.frame = fr;
        x.frame_len = CK_ARP_FRAME_LEN;
        x.rx_count = CK_GEM_RXMAX;
        x.rx_buf_len = 1536;
        x.tx_wait_us = 50000;
        x.rx_wait_us = e->wait_us;
        x.rx_probe = arp_probe_cb;
        x.rx_probe_ctx = e;
        rc = ck_gem_xfer(g, &x);
        e->attempts_used = a + 1u;
        e->last_rc = rc;
        e->last_tsr = x.tsr;
        e->last_rsr = x.rsr;
        e->last_isr = x.isr;
        e->bme_off_at_end = x.bme_off_at_end;
        if (x.tx_done && !(x.tx_ctrl & (CK_GEMD_TX_ERROR | CK_GEMD_TX_UNDERRUN | CK_GEMD_TX_BUF_EXHAUSTED)))
            e->tx_ok++;
        if (rc < 0)
            return rc;
        for (r = 0; r < x.rx_all_n; r++) {
            const uint8_t *d = x.rx_all[r];
            uint32_t len = x.rx_all_len[r], n = len < CK_GEM_RXKEEP ? len : CK_GEM_RXKEEP;
            int cls = ck_frame_class(d, n, e->mac);
            uint8_t sm[6] = {0}, si[4] = {0};
            int ck = (cls == CK_FC_UNICAST_US) ? ck_arp_reply_check(d, n, len, e->mac, e->our_ip, e->target_ip, sm, si) : CK_ARP_E_DST;
#ifdef CK_GEM_MUTANT_ARP_ACCEPT_ANY
            ck = CK_ARP_OK;
#endif
            e->frames_seen++;
            if (e->seen_n < CK_ARP_SEEN) {
                uint32_t hk;
                for (hk = 0; hk < CK_ARP_HDRKEEP; hk++)
                    e->seen_hdr[e->seen_n][hk] = hk < n ? d[hk] : 0;
                e->seen_len[e->seen_n] = len;
                e->seen_attempt[e->seen_n] = a + 1u;
                e->seen_n++;
            }
            if (ck == CK_ARP_OK && !e->matched) {
                e->matched = 1;
                e->reply_len = len;
                for (i = 0; i < 6; i++)
                    e->reply_mac[i] = sm[i];
                for (i = 0; i < 4; i++)
                    e->reply_ip[i] = si[i];
                for (i = 0; i < n && i < CK_ARP_MAXLEN; i++)
                    e->reply_frame[i] = d[i];
                continue;
            }
            switch (cls) {
            case CK_FC_BROADCAST: e->n_broadcast++; break;
            case CK_FC_MULTICAST: e->n_multicast++; break;
            case CK_FC_UNICAST_US: e->n_unicast_us++; break;
            case CK_FC_UNICAST_OTHER: e->n_unicast_other++; break;
            default: e->n_runt++; break;
            }
            if (cls == CK_FC_UNICAST_US && ck != CK_ARP_OK && d[12] == 0x08 && d[13] == 0x06 && (uint32_t)(-ck) < CK_ARP_NREASONS)
                e->n_arp_refused[-ck]++;
        }
    }
    return 0;
}

/* ---------------- PHY ---------------- */

uint32_t ck_gem_mdio_frame_write(uint32_t phy, uint32_t reg, uint16_t val)
{
#ifdef CK_GEM_MUTANT_MDIO_OP
    uint32_t op = 2u; /* mutant: the READ op code */
#else
    uint32_t op = 1u; /* MACB_MAN_C22_WRITE */
#endif
    return (1u << 30) | (op << 28) | ((phy & 31u) << 23) | ((reg & 31u) << 18) | (2u << 16) | val;
}

int ck_gem_mdio_write(const struct ck_rp1_io *io, uint32_t phy, uint32_t reg, uint16_t val)
{
    uint32_t i;
    if (!io || phy > 31u || reg > 31u)
        return CK_GEM_E_ARG;
    for (i = 0; i < CK_GEM_MDIO_POLLS; i++) {
        if (io->rd(io->ctx, CK_RP1_ETH + CK_GEM_NSR) & CK_GEM_NSR_IDLE)
            break;
        if (io->delay_us)
            io->delay_us(io->ctx, CK_GEM_MDIO_POLL_US);
    }
    if (i == CK_GEM_MDIO_POLLS)
        return CK_GEM_E_TIMEOUT;
    io->wr(io->ctx, CK_RP1_ETH + CK_GEM_MAN, ck_gem_mdio_frame_write(phy, reg, val));
    for (i = 0; i < CK_GEM_MDIO_POLLS; i++) {
        if (io->rd(io->ctx, CK_RP1_ETH + CK_GEM_NSR) & CK_GEM_NSR_IDLE)
            return 0;
        if (io->delay_us)
            io->delay_us(io->ctx, CK_GEM_MDIO_POLL_US);
    }
    return CK_GEM_E_TIMEOUT;
}

void ck_eth_cfg_status_decode(uint32_t st, int *link, uint32_t *speed, int *full_duplex)
{
    uint32_t s = (st >> 1) & 3u;
    if (link)
        *link = (st & 1u) != 0;
    if (speed)
        *speed = s == 0 ? 10u : s == 1 ? 100u : s == 2 ? 1000u : 0u;
    if (full_duplex)
        *full_duplex = (st & 8u) != 0;
}

int ck_phy_link_read(const struct ck_rp1_io *io, uint32_t phy, struct ck_phy_link *l)
{
    int e = 0;
    if (!io || !l)
        return CK_GEM_E_ARG;
    *l = (struct ck_phy_link){0};
    e |= ck_gem_mdio_read(io, phy, CK_MII_BMCR, &l->bmcr);
    e |= ck_gem_mdio_read(io, phy, CK_MII_BMSR, &l->bmsr_first); /* latched low: the first read reports a past drop */
    e |= ck_gem_mdio_read(io, phy, CK_MII_BMSR, &l->bmsr);
    e |= ck_gem_mdio_read(io, phy, CK_MII_ADVERTISE, &l->adv);
    e |= ck_gem_mdio_read(io, phy, CK_MII_LPA, &l->lpa);
    e |= ck_gem_mdio_read(io, phy, CK_MII_CTRL1000, &l->ctrl1000);
    e |= ck_gem_mdio_read(io, phy, CK_MII_STAT1000, &l->stat1000);
    if (e)
        return CK_GEM_E_TIMEOUT;
    l->link = (l->bmsr & CK_MII_BMSR_LSTATUS) != 0;
    l->aneg_done = (l->bmsr & CK_MII_BMSR_ANEGCOMPLETE) != 0;
    if (!l->link)
        return 0;
    if (l->bmcr & CK_MII_BMCR_ANENABLE) {
        uint16_t gb, com;
        if (!l->aneg_done)
            return 0;
        gb = (uint16_t)(l->stat1000 & (l->ctrl1000 << 2)); /* LPA_1000FULL = ADVERTISE_1000FULL << 2 */
        com = (uint16_t)(l->adv & l->lpa);
        if (gb & 0x0800u) { l->speed = 1000; l->full_duplex = 1; }
        else if (gb & 0x0400u) { l->speed = 1000; l->full_duplex = 0; }
        else if (com & 0x0100u) { l->speed = 100; l->full_duplex = 1; }
        else if (com & 0x0080u) { l->speed = 100; l->full_duplex = 0; }
        else if (com & 0x0040u) { l->speed = 10; l->full_duplex = 1; }
        else if (com & 0x0020u) { l->speed = 10; l->full_duplex = 0; }
    } else {
        l->speed = (l->bmcr & CK_MII_BMCR_SPEED1000) ? 1000u : (l->bmcr & CK_MII_BMCR_SPEED100) ? 100u : 10u;
        l->full_duplex = (l->bmcr & CK_MII_BMCR_FULLDPLX) != 0;
    }
    return 0;
}

int ck_phy_rgmii_id_config(const struct ck_rp1_io *io, uint32_t phy, uint16_t *auxctl_before, uint16_t *clk_before)
{
    /* Linux bcm54xx_config_clock_delay for PHY_INTERFACE_MODE_RGMII_ID [P1]:
     * RX skew: AUXCTL shadow MISC (0x07) read-modify-write with WREN (0x8000) and RGMII_SKEW_EN (0x0100).
     * TX delay: shadow CLK_CTL (0x03) |= GTXCLK_EN (1 << 9). */
    uint16_t v = 0, s = 0;
    int e;
    if (!io)
        return CK_GEM_E_ARG;
    e = ck_gem_mdio_write(io, phy, 0x18u, (uint16_t)(0x0007u | (0x0007u << 12)));      /* select + read-select MISC */
    e |= ck_gem_mdio_read(io, phy, 0x18u, &v);
    if (e)
        return CK_GEM_E_TIMEOUT;
    if (auxctl_before)
        *auxctl_before = v;
    v = (uint16_t)(v | 0x8000u | 0x0100u);
    e = ck_gem_mdio_write(io, phy, 0x18u, (uint16_t)(0x0007u | v));
    e |= ck_gem_mdio_write(io, phy, 0x1cu, (uint16_t)(0x03u << 10));                   /* shadow read select CLK_CTL */
    e |= ck_gem_mdio_read(io, phy, 0x1cu, &s);
    if (e)
        return CK_GEM_E_TIMEOUT;
    s &= 0x3ffu;
    if (clk_before)
        *clk_before = s;
    s |= (uint16_t)(1u << 9);
    e = ck_gem_mdio_write(io, phy, 0x1cu, (uint16_t)(0x8000u | (0x03u << 10) | s));
    return e ? CK_GEM_E_TIMEOUT : 0;
}

int ck_phy_loopback_enter(const struct ck_rp1_io *io, uint32_t phy, uint32_t speed, uint16_t *bmcr_saved, uint16_t *bmsr_out)
{
    uint16_t cur = 0, bmsr = 0;
    uint32_t i;
    uint16_t want;
    if (!io || (speed != 10 && speed != 100 && speed != 1000))
        return CK_GEM_E_ARG;
    if (ck_gem_mdio_read(io, phy, CK_MII_BMCR, &cur))
        return CK_GEM_E_TIMEOUT;
    if (bmcr_saved)
        *bmcr_saved = cur;
#ifdef CK_GEM_MUTANT_PHYLB_BIT
    want = CK_MII_BMCR_FULLDPLX | 0x2000u;
#else
    want = (uint16_t)(CK_MII_BMCR_LOOPBACK | CK_MII_BMCR_FULLDPLX);
#endif
    if (speed == 1000)
        want |= CK_MII_BMCR_SPEED1000;
    else if (speed == 100)
        want |= CK_MII_BMCR_SPEED100;
    if (ck_gem_mdio_write(io, phy, CK_MII_BMCR, want))
        return CK_GEM_E_TIMEOUT;
    for (i = 0; i < 100u; i++) { /* genphy_loopback waits up to 500 ms for link, 5 ms apart */
        (void)ck_gem_mdio_read(io, phy, CK_MII_BMSR, &bmsr);
        (void)ck_gem_mdio_read(io, phy, CK_MII_BMSR, &bmsr);
        if (bmsr & CK_MII_BMSR_LSTATUS)
            break;
        if (io->delay_us)
            io->delay_us(io->ctx, 5000u);
    }
    if (bmsr_out)
        *bmsr_out = bmsr;
    return (bmsr & CK_MII_BMSR_LSTATUS) ? 0 : CK_GEM_E_TIMEOUT;
}

int ck_phy_loopback_exit(const struct ck_rp1_io *io, uint32_t phy, uint16_t bmcr_saved)
{
    uint16_t v = (uint16_t)(bmcr_saved & ~CK_MII_BMCR_LOOPBACK);
    if (!io)
        return CK_GEM_E_ARG;
    if (v & CK_MII_BMCR_ANENABLE)
        v |= CK_MII_BMCR_ANRESTART;
    return ck_gem_mdio_write(io, phy, CK_MII_BMCR, v) ? CK_GEM_E_TIMEOUT : 0;
}

/* ================= image glue (CK_PI5_NET only) ================= */
#ifdef CK_PI5_NET
#include "ck.h"
/* dev/pi5_peer.c (aienos-mv#398); declared here so this file does not need the net headers */
void ck_pi5peer_run(struct ck_gem *g, uint32_t speed, int full_duplex, const uint8_t mac[6], const uint8_t ip[4]);

static int glue_bme(void *ctx, int on) { return ck_pi5gem_bme_set((const struct ck_pi5pcie_io *)ctx, on); }
static void glue_barrier(void) { ck_mb(); }

/* Locally administered unicast (bit 1 of the first octet set), not the board's own address: this cut does not read
 * the OTP MAC (UNVERIFIED which one the board owns). */
static const uint8_t k_mac[6] = {0x02, 0x41, 0x49, 0x45, 0x4e, 0x03};

/* PI5-NET-4 qualification constants (compile-time; no DHCP yet). They are the Raspberry Pi 5's OWN identity on this LAN,
 * read from the same board while it ran Linux (hub, eth0): MAC = device-tree local-mac-address of the RP1 Ethernet node
 * (the firmware puts the board's MAC there; same bytes as `ip link` eth0 and as the Pi firmware mailbox tag 0x00010003
 * "get board MAC address" would return, UNVERIFIED here because this kernel has no mailbox driver), IP = the DHCP
 * address that board holds on 192.168.1.0/24, gateway = the LAN router (default route). The Pi runs no Linux during an
 * AIENOS run, so nothing else answers to this MAC or IP. If the board or LAN changes, change these three lines. */
static const uint8_t k_lan_mac[6] = {0x2c, 0xcf, 0x67, 0x3f, 0x54, 0x76};
static const uint8_t k_lan_ip[4] = {192, 168, 1, 192};
static const uint8_t k_lan_gw[4] = {192, 168, 1, 1};

static const char *verdict(int ok) { return ok ? "PASS" : "FAIL"; }

static void print_xfer(const char *tag, int rc, const struct ck_gem_xfer *x, const uint8_t *tx)
{
    uint32_t diff = x->rx_done ? ck_gem_frame_diff(tx, x->frame_len, x->rx_data, x->rx_copied) : 0xffffffffu;
    ck_printf("pi5_gem: %s rc=%d tx_done=%u rx_done=%u rx_frames=%u tx_ctrl=0x%08x rx_word0=0x%08x rx_ctrl=0x%08x rx_len=%u frame_diff=%u\n",
              tag, rc, x->tx_done, x->rx_done, x->rx_frames, x->tx_ctrl, x->rx_word0, x->rx_ctrl, x->rx_len,
              x->rx_done ? diff : 0xffffffffu);
    ck_printf("pi5_gem: %s regs: tsr=0x%08x rsr=0x%08x isr=0x%08x ncr_run=0x%08x ncfgr=0x%08x dmacfg=0x%08x ncr_end=0x%08x tsr_end=0x%08x bme_on_for_us=%u bme_off_at_end=%u tx_bus=0x%llx rx_bus=0x%llx\n",
              tag, x->tsr, x->rsr, x->isr, x->ncr_run, x->ncfgr_run, x->dmacfg_run, x->ncr_end, x->tsr_end,
              x->bme_enabled_for_us, x->bme_off_at_end, (unsigned long long)x->tx_bus, (unsigned long long)x->rx_bus);
}

static void print_link(const char *tag, const struct ck_phy_link *l, uint32_t eth_status)
{
    int el = 0, efd = 0;
    uint32_t es = 0;
    ck_eth_cfg_status_decode(eth_status, &el, &es, &efd);
    ck_printf("pi5_gem: %s: BMCR=0x%04x BMSR=0x%04x/0x%04x ADV=0x%04x LPA=0x%04x CTRL1000=0x%04x STAT1000=0x%04x link=%u aneg_done=%u speed=%u duplex=%s | eth_cfg STATUS=0x%08x link=%u speed=%u duplex=%s\n",
              tag, l->bmcr, l->bmsr_first, l->bmsr, l->adv, l->lpa, l->ctrl1000, l->stat1000, l->link, l->aneg_done, l->speed,
              l->full_duplex ? "full" : "half", eth_status, el, es, efd ? "full" : "half");
}

static uint32_t pci_cmd(const struct ck_pi5gem_ctx *c)
{
    uint32_t v = 0;
    (void)ck_pi5pcie_cfg_read32(c->rc, 1, 0, 0, 4, &v);
    return v;
}

static uint32_t eth_status(const struct ck_rp1_io *rio) { return rio->rd(rio->ctx, CK_RP1_ETH_CFG + 0x04u); }

/* Wait (bounded) for the PHY to report link with autonegotiation complete. */
static int wait_link(const struct ck_pi5gem_ctx *c, uint32_t max_ms, struct ck_phy_link *l)
{
    uint32_t t;
    for (t = 0; t <= max_ms; t += 100) {
        if (ck_phy_link_read(c->rio, c->phy_addr, l) == 0 && l->link && l->aneg_done)
            return 1;
        if (t < max_ms)
            c->rio->delay_us(c->rio->ctx, 100000u);
    }
    return 0;
}

/* Wait for a link that has settled: link up, autonegotiation complete and, at 1000 Mbit, BOTH receivers reported OK
 * (STAT1000 bit 13 local, bit 12 partner) for 5 consecutive reads 100 ms apart. A link that just came back from a
 * loopback or a restart can read "up" for a moment before it drops; the partner's receiver is the one that matters for
 * frames we send. Bounded; returns 1 when settled. */
static int wait_link_stable(const struct ck_pi5gem_ctx *c, uint32_t max_ms, struct ck_phy_link *l, uint32_t *waited_ms)
{
    uint32_t t, good = 0;
    for (t = 0; t <= max_ms; t += 100) {
        int ok = ck_phy_link_read(c->rio, c->phy_addr, l) == 0 && l->link && l->aneg_done && l->speed != 0;
        if (ok && l->speed == 1000)
            ok = (l->stat1000 & 0x3000u) == 0x3000u;
        good = ok ? good + 1 : 0;
        if (good >= 5) {
            *waited_ms = t;
            return 1;
        }
        if (t < max_ms)
            c->rio->delay_us(c->rio->ctx, 100000u);
    }
    *waited_ms = max_ms;
    return 0;
}

static uint32_t canary_changed(volatile const uint8_t *p, uint32_t n, uint8_t pat, uint32_t *first)
{
    uint32_t i, c = 0;
    *first = 0xffffffffu;
    for (i = 0; i < n; i++)
        if (p[i] != pat) {
            if (c == 0)
                *first = i;
            c++;
        }
    return c;
}

void ck_pi5gem_run(const struct ck_pi5gem_ctx *c)
{
    struct ck_gem g;
    struct ck_gem_xfer x;
    struct ck_phy_link lk;
    uint8_t txf[64], tpa[4] = {192, 168, 1, 1};
    uint16_t aux0 = 0, clk0 = 0, bmcr0 = 0, bmsr_lb = 0;
    int rc, link_up, llb_ok = 0, phylb_ok = 0, ok;
    uint32_t n;
    volatile uint8_t *va = (volatile uint8_t *)c->bounce;

    ck_printf("pi5_gem: cut 4: loopbacks, then one ARP exchange with the LAN gateway, descriptors only inside the bounce window; software-only DMA confinement (Pi 5, weaker than the Spark)\n");
    if (!c->hw_window_confined) {
        ck_printf("pi5_gem: STOP root-complex inbound window is not CONFINED; bus-master stays off, no GEM DMA\n");
        return;
    }
    ck_gem_init(&g, c->rio, c->dw, va, glue_bme, (void *)c->rc, glue_barrier, 1);
    rc = ck_gem_probe(c->rio, &g.caps);
    ck_printf("pi5_gem: caps rc=%d MID=0x%08x DCFG1=0x%08x DCFG2=0x%08x DCFG6=0x%08x gem=%u daw64=%u queue_mask=0x%x queues=%u desc_bytes=%u dbw_code=%u\n",
              rc, g.caps.mid, g.caps.dcfg1, g.caps.dcfg2, g.caps.dcfg6, g.caps.is_gem, g.caps.daw64, g.caps.queue_mask,
              g.caps.nqueues, g.caps.desc_bytes, g.caps.dbw);
    if (rc) {
        ck_printf("pi5_gem: STOP not a GEM; nothing programmed\n");
        return;
    }
    ck_printf("pi5_gem: state before: NCR=0x%08x NCFGR=0x%08x USRIO=0x%08x DMACFG=0x%08x AMP=0x%08x TBQP=0x%08x RBQP=0x%08x eth_cfg STATUS=0x%08x CLKGEN=0x%08x rp1 pci cmd=0x%08x\n",
              c->rio->rd(0, CK_RP1_ETH + CK_GEM_NCR), c->rio->rd(0, CK_RP1_ETH + CK_GEM_NCFGR),
              c->rio->rd(0, CK_RP1_ETH + CK_GEM_USRIO), c->rio->rd(0, CK_RP1_ETH + CK_GEM_DMACFG),
              c->rio->rd(0, CK_RP1_ETH + CK_GEM_AMP), c->rio->rd(0, CK_RP1_ETH + CK_GEM_TBQP),
              c->rio->rd(0, CK_RP1_ETH + CK_GEM_RBQP), eth_status(c->rio), c->rio->rd(0, CK_RP1_ETH_CFG + 0x14u),
              pci_cmd(c));

    /* ---- 1. link status first, nothing assumed ---- */
    rc = ck_phy_link_read(c->rio, c->phy_addr, &lk);
    print_link("link (first read, read-only)", &lk, eth_status(c->rio));
    link_up = wait_link(c, 6000, &lk);
    print_link(link_up ? "link UP" : "link NOT UP after 6000 ms (no cable or no partner, or autoneg not complete)", &lk, eth_status(c->rio));
    ck_printf("pi5_gem: LINK %s\n", link_up ? "FOUND" : "ABSENT");

    /* ---- PHY RGMII-ID delays as Linux sets them (broadcom.c), then read back ---- */
    rc = ck_phy_rgmii_id_config(c->rio, c->phy_addr, &aux0, &clk0);
    {
        uint16_t a1 = 0, s1 = 0;
        (void)ck_gem_mdio_write(c->rio, c->phy_addr, 0x18u, (uint16_t)(0x0007u | (0x0007u << 12)));
        (void)ck_gem_mdio_read(c->rio, c->phy_addr, 0x18u, &a1);
        (void)ck_gem_mdio_write(c->rio, c->phy_addr, 0x1cu, (uint16_t)(0x03u << 10));
        (void)ck_gem_mdio_read(c->rio, c->phy_addr, 0x1cu, &s1);
        ck_printf("pi5_gem: phy rgmii-id delays rc=%d AUXCTL_MISC before=0x%04x after=0x%04x CLK_CTL before=0x%03x after=0x%03x\n",
                  rc, aux0, a1, clk0, s1 & 0x3ff);
    }

    /* ---- 2. GEM local loopback ---- */
    ck_gem_frame_build(CK_GEMF_RAW, k_mac, 1, tpa, txf, sizeof txf);
    x = (struct ck_gem_xfer){0};
    x.mode = CK_GEMX_LLB; x.speed = 1000; x.full_duplex = 1; x.frame = txf; x.frame_len = 60; x.rx_count = 2; x.rx_buf_len = 1536;
    x.tx_wait_us = 20000; x.rx_wait_us = 20000;
    for (n = 0; n < 6; n++) x.mac[n] = k_mac[n];
    rc = ck_gem_xfer(&g, &x);
    print_xfer("loopback gem-local(NCR.LLB) seq=1", rc, &x, txf);
    llb_ok = rc == 0 && x.tx_done && x.rx_done && x.rx_frames == 1 && ck_gem_frame_diff(txf, 60, x.rx_data, x.rx_copied) == 0 &&
             x.rx_len == 60;
    ck_printf("pi5_gem: LOOPBACK gem-local %s (sent seq=1 60 bytes, received %u bytes, byte compare %s)\n", verdict(llb_ok), x.rx_len,
              x.rx_done && ck_gem_frame_diff(txf, 60, x.rx_data, x.rx_copied) == 0 ? "identical" : "different or none");

    /* ---- 3. PHY loopback (BMCR bit 14) ---- */
    ck_gem_frame_build(CK_GEMF_RAW, k_mac, 2, tpa, txf, sizeof txf);
    rc = ck_phy_loopback_enter(c->rio, c->phy_addr, 1000, &bmcr0, &bmsr_lb);
    ck_printf("pi5_gem: phy loopback enter rc=%d BMCR_was=0x%04x BMSR_now=0x%04x (link in loopback=%u)\n", rc, bmcr0, bmsr_lb,
              (bmsr_lb & CK_MII_BMSR_LSTATUS) != 0);
    x = (struct ck_gem_xfer){0};
    x.mode = CK_GEMX_PHYLB; x.speed = 1000; x.full_duplex = 1; x.frame = txf; x.frame_len = 60; x.rx_count = 2; x.rx_buf_len = 1536;
    x.tx_wait_us = 20000; x.rx_wait_us = 50000;
    for (n = 0; n < 6; n++) x.mac[n] = k_mac[n];
    rc = ck_gem_xfer(&g, &x);
    print_xfer("loopback phy(BMCR.LOOPBACK) seq=2", rc, &x, txf);
    phylb_ok = rc == 0 && x.tx_done && x.rx_done && x.rx_frames == 1 && ck_gem_frame_diff(txf, 60, x.rx_data, x.rx_copied) == 0 &&
               x.rx_len == 60;
    ck_printf("pi5_gem: LOOPBACK phy %s (sent seq=2 60 bytes, received %u bytes, byte compare %s)\n", verdict(phylb_ok), x.rx_len,
              x.rx_done && ck_gem_frame_diff(txf, 60, x.rx_data, x.rx_copied) == 0 ? "identical" : "different or none");
    rc = ck_phy_loopback_exit(c->rio, c->phy_addr, bmcr0);
    ck_printf("pi5_gem: phy loopback exit rc=%d BMCR restored to 0x%04x\n", rc, (uint16_t)(bmcr0 & ~CK_MII_BMCR_LOOPBACK));

    /* ---- 4. one frame on the wire, only if a link exists ---- */
    if (link_up) {
        struct ck_phy_link l2;
        uint32_t speed, fd, es = 0;
        int el = 0, efd = 0;
        uint32_t waited = 0;
        link_up = wait_link_stable(c, 15000, &l2, &waited);
        print_link(link_up ? "link after loopback (UP, settled)" : "link after loopback (NOT settled within 15000 ms)", &l2, eth_status(c->rio));
        ck_printf("pi5_gem: link settle wait: %u ms, settled=%u (needs link, aneg done and, at 1000 Mbit, local+partner receiver OK for 500 ms)\n", waited, link_up);
        if (link_up) {
            speed = l2.speed;
            fd = (uint32_t)l2.full_duplex;
            ck_eth_cfg_status_decode(eth_status(c->rio), &el, &es, &efd);
            if (!speed) { speed = es; fd = (uint32_t)efd; }
            struct ck_arp_exchange ae = {0};
            uint32_t k;
            uint32_t es_before, es_after;
            for (k = 0; k < 6; k++) ae.mac[k] = k_lan_mac[k];
            for (k = 0; k < 4; k++) { ae.our_ip[k] = k_lan_ip[k]; ae.target_ip[k] = k_lan_gw[k]; }
            ae.speed = speed ? speed : 100; ae.full_duplex = (int)fd; ae.attempts = 2; ae.wait_us = 1000000; ae.pre_attempt = 0; ae.pre_ctx = 0; /* r2 clock state, unchanged (one variable at a time) */
            ae.now_us = ck_time_us;
            rc = ck_gem_set_link(&g, ae.speed, ae.full_duplex);
            ck_printf("pi5_gem: MAC speed set at link up (as Linux macb_mac_link_up): rc=%d speed=%u %s NCFGR=0x%08x eth_cfg CLKGEN=0x%08x\n", rc, ae.speed,
                      ae.full_duplex ? "full" : "half", c->rio->rd(0, CK_RP1_ETH + CK_GEM_NCFGR), c->rio->rd(0, CK_RP1_ETH_CFG + 0x14u));
            es_before = eth_status(c->rio);
            ck_printf("pi5_gem: ARP exchange: our mac=%02x:%02x:%02x:%02x:%02x:%02x ip=%u.%u.%u.%u asks who-has %u.%u.%u.%u at %u Mbit %s duplex, up to %u attempts of %u ms (bus-master on only inside each attempt)\n",
                      ae.mac[0], ae.mac[1], ae.mac[2], ae.mac[3], ae.mac[4], ae.mac[5], ae.our_ip[0], ae.our_ip[1], ae.our_ip[2], ae.our_ip[3],
                      ae.target_ip[0], ae.target_ip[1], ae.target_ip[2], ae.target_ip[3], ae.speed, ae.full_duplex ? "full" : "half",
                      ae.attempts, ae.wait_us / 1000u);
            rc = ck_gem_arp_exchange(&g, &ae);
            es_after = eth_status(c->rio);
            {
                struct ck_phy_link l3;
                uint32_t pi, ph[6] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x19};
                uint16_t pv[6] = {0};
                (void)ck_phy_link_read(c->rio, c->phy_addr, &l3);
                for (pi = 0; pi < 6; pi++)
                    (void)ck_gem_mdio_read(c->rio, c->phy_addr, ph[pi], &pv[pi]);
                print_link("link after the exchange", &l3, es_after);
                ck_printf("pi5_gem: PHY after the exchange: reg0x10=0x%04x reg0x11=0x%04x rxerr_cnt(0x12)=0x%04x false_carrier_cnt(0x13)=0x%04x rx_notok_cnt(0x14)=0x%04x aux_status(0x19)=0x%04x | eth_cfg CLKGEN=0x%08x CONTROL=0x%08x\n",
                          pv[0], pv[1], pv[2], pv[3], pv[4], pv[5], c->rio->rd(0, CK_RP1_ETH_CFG + 0x14u), c->rio->rd(0, CK_RP1_ETH_CFG));
            }
            {
                uint32_t si;
                for (si = 0; si < ae.attempts_used && si < 5; si++)
                    ck_printf("pi5_gem: ARP attempt %u started at uptime %llu us\n", si + 1, (unsigned long long)ae.attempt_start_us[si]);
                for (si = 0; si < ae.seen_n; si++) {
                    const uint8_t *h = ae.seen_hdr[si];
                    ck_printf("pi5_gem: ARP seen[%u] attempt %u len=%u dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x type=%02x%02x payload=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                              si, ae.seen_attempt[si], ae.seen_len[si], h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9], h[10], h[11],
                              h[12], h[13], h[14], h[15], h[16], h[17], h[18], h[19], h[20], h[21], h[22], h[23]);
                }
            }
            ck_printf("pi5_gem: ARP exchange rc=%d last_rc=%d attempts_used=%u tx_ok=%u last tsr=0x%08x rsr=0x%08x isr=0x%08x bme_off_at_end=%u | eth_cfg STATUS before=0x%08x after=0x%08x\n",
                      rc, ae.last_rc, ae.attempts_used, ae.tx_ok, ae.last_tsr, ae.last_rsr, ae.last_isr, ae.bme_off_at_end, es_before, es_after);
            {
                /* GEM statistics registers [M1 macb.h], read only; CLRSTAT ran at the start of the last attempt, so these
                 * are that attempt's counts. */
                uint32_t sb = CK_RP1_ETH;
                ck_printf("pi5_gem: GEM stats (last attempt): tx_frames=%u tx_bcast=%u tx_octets=%u tx_underrun=%u tx_carrier_sense_err=%u | rx_frames=%u rx_bcast=%u rx_mcast=%u rx_octets=%u rx_fcs_err=%u rx_align_err=%u rx_symbol_err=%u rx_undersize=%u rx_oversize=%u rx_resource_err=%u rx_overrun=%u\n",
                          c->rio->rd(0, sb + 0x108u), c->rio->rd(0, sb + 0x10cu), c->rio->rd(0, sb + 0x100u), c->rio->rd(0, sb + 0x134u),
                          c->rio->rd(0, sb + 0x14cu), c->rio->rd(0, sb + 0x158u), c->rio->rd(0, sb + 0x15cu), c->rio->rd(0, sb + 0x160u),
                          c->rio->rd(0, sb + 0x150u), c->rio->rd(0, sb + 0x190u), c->rio->rd(0, sb + 0x19cu), c->rio->rd(0, sb + 0x198u),
                          c->rio->rd(0, sb + 0x184u), c->rio->rd(0, sb + 0x188u), c->rio->rd(0, sb + 0x1a0u), c->rio->rd(0, sb + 0x1a4u));
            }
            ck_printf("pi5_gem: ARP frames seen=%u (reply included): broadcast=%u multicast=%u unicast_to_us_other=%u unicast_other=%u runt=%u; refused-as-reply by reason (len,etype,dst,hdr,op,src,tha,tpa,spa)=%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                      ae.frames_seen, ae.n_broadcast, ae.n_multicast, ae.n_unicast_us, ae.n_unicast_other, ae.n_runt,
                      ae.n_arp_refused[2], ae.n_arp_refused[3], ae.n_arp_refused[4], ae.n_arp_refused[5], ae.n_arp_refused[6],
                      ae.n_arp_refused[7], ae.n_arp_refused[8], ae.n_arp_refused[9], ae.n_arp_refused[10]);
            ck_printf("pi5_gem: WIRE tx %s (ARP request 60 bytes, %u of %u attempts completed in the MAC without an error bit)\n",
                      verdict(ae.tx_ok > 0), ae.tx_ok, ae.attempts_used);
            if (ae.matched) {
                ck_printf("pi5_gem: ARP REPLY from %u.%u.%u.%u is-at %02x:%02x:%02x:%02x:%02x:%02x to %02x:%02x:%02x:%02x:%02x:%02x (frame %u bytes, request for %u.%u.%u.%u, we are %u.%u.%u.%u, attempt %u)\n",
                          ae.reply_ip[0], ae.reply_ip[1], ae.reply_ip[2], ae.reply_ip[3], ae.reply_mac[0], ae.reply_mac[1], ae.reply_mac[2],
                          ae.reply_mac[3], ae.reply_mac[4], ae.reply_mac[5], ae.reply_frame[0], ae.reply_frame[1], ae.reply_frame[2],
                          ae.reply_frame[3], ae.reply_frame[4], ae.reply_frame[5], ae.reply_len, ae.target_ip[0], ae.target_ip[1],
                          ae.target_ip[2], ae.target_ip[3], ae.our_ip[0], ae.our_ip[1], ae.our_ip[2], ae.our_ip[3], ae.attempts_used);
                ck_printf("pi5_gem: ARP EXCHANGE PASS: the request left the board and the gateway's reply addressed to our MAC came back (RX on the wire works)\n");
            } else {
                ck_printf("pi5_gem: ARP EXCHANGE FAIL: no accepted ARP reply within %u attempts\n", ae.attempts_used);
            }
            /* aienos-mv#398: the authenticated Pi <-> Spark session on the same link (dev/pi5_peer.c) */
            ck_pi5peer_run(&g, ae.speed, ae.full_duplex, k_lan_mac, k_lan_ip);
        }
    } else {
        ck_printf("pi5_gem: WIRE SKIPPED (no link partner found)\n");
    }

    /* ---- 5. the confinement proof (ADR 0020 test 5): descriptors deliberately OUTSIDE the window ---- */
    {
        int mode = llb_ok ? CK_GEMX_LLB : phylb_ok ? CK_GEMX_PHYLB : 0;
        uint64_t cphys = 0, cphys2 = 0;
        volatile uint8_t *can = 0, *can2 = 0;
        struct ck_dmaw scratch = *c->dw;
        int vr;
        if (!mode) {
            ck_printf("pi5_gem: CONFINEMENT SKIPPED: neither loopback delivered a frame, so there is no frame to push at an outside address; UNVERIFIED\n");
        } else {
            if (mode == CK_GEMX_PHYLB) {
                rc = ck_phy_loopback_enter(c->rio, c->phy_addr, 1000, &bmcr0, &bmsr_lb);
                ck_printf("pi5_gem: confinement: phy loopback re-entered rc=%d\n", rc);
            }
            can = (volatile uint8_t *)ck_dma_alloc(4096, 4096, &cphys);
            can2 = (volatile uint8_t *)ck_dma_alloc(4096, 4096, &cphys2);
            ok = can && can2 && cphys && cphys2 && cphys != cphys2 &&
                 (cphys + 4096 <= c->bounce_phys || cphys >= c->bounce_phys + c->dw->size) &&
                 (cphys2 + 4096 <= c->bounce_phys || cphys2 >= c->bounce_phys + c->dw->size);
            if (!ok) {
                ck_printf("pi5_gem: CONFINEMENT SKIPPED: could not get two private pages outside the window (can=%llx can2=%llx); UNVERIFIED\n",
                          (unsigned long long)cphys, (unsigned long long)cphys2);
            } else {
                uint32_t i, changed, first;
                for (i = 0; i < 4096; i++) { can[i] = 0xa5; can2[i] = 0; }
                vr = ck_dmaw_validate(&scratch, cphys, 64);
                ck_printf("pi5_gem: confinement setup: canary page cpu=0x%llx (outside window 0x%llx..+0x%llx); validator on a scratch copy of the window says %d (2 = E_RANGE, refused) for a 64 byte buffer there\n",
                          (unsigned long long)cphys, (unsigned long long)c->bounce_phys, (unsigned long long)c->dw->size, vr);
                if (vr != CK_DMAW_E_RANGE) {
                    ck_printf("pi5_gem: CONFINEMENT STOP validator did not refuse the outside address; nothing run\n");
                } else {
                    /* A. device WRITE outside: RX buffer 64 bytes, one slot, the page is ours and bounded at 64 bytes */
                    ck_gem_frame_build(CK_GEMF_RAW, k_mac, 4, tpa, txf, sizeof txf);
                    x = (struct ck_gem_xfer){0};
                    x.mode = mode; x.speed = 1000; x.full_duplex = 1; x.frame = txf; x.frame_len = 60; x.rx_count = 1; x.rx_buf_len = 64;
                    x.tx_wait_us = 20000; x.rx_wait_us = 50000; x.raw_rx_outside = cphys;
                    for (n = 0; n < 6; n++) x.mac[n] = k_mac[n];
                    ck_printf("pi5_gem: CONFINEMENT A: the RX descriptor points at 0x%llx outside the window, buffer size 64 bytes (the device can write at most 64 bytes there)\n",
                              (unsigned long long)cphys);
                    rc = ck_gem_xfer(&g, &x);
                    print_xfer("confinement A (rx write outside) seq=4", rc, &x, txf);
                    changed = canary_changed(can, 4096, 0xa5, &first);
                    ck_printf("pi5_gem: CONFINEMENT A result: canary bytes changed=%u first_changed_offset=%d device_says_rx_done=%u hresp=%u\n",
                              changed, (int)first, x.rx_done, (x.isr & CK_GEM_ISR_HRESP) != 0);
                    ck_printf("pi5_gem: CONFINEMENT A %s\n", changed == 0 ? "BLOCKED: the write did not reach the outside page" :
                                                           "NOT BLOCKED: the device wrote outside the window (hardware backstop NOT effective)");
                    /* B. device READ outside: TX descriptor points at a page holding a different frame */
                    ck_gem_frame_build(CK_GEMF_RAW, k_mac, 0xdead0005u, tpa, (uint8_t *)can2, 64);
                    ck_gem_frame_build(CK_GEMF_RAW, k_mac, 5, tpa, txf, sizeof txf);
                    x = (struct ck_gem_xfer){0};
                    x.mode = mode; x.speed = 1000; x.full_duplex = 1; x.frame = txf; x.frame_len = 60; x.rx_count = 1; x.rx_buf_len = 1536;
                    x.tx_wait_us = 20000; x.rx_wait_us = 50000; x.raw_tx_outside = cphys2;
                    for (n = 0; n < 6; n++) x.mac[n] = k_mac[n];
                    ck_printf("pi5_gem: CONFINEMENT B: the TX descriptor points at 0x%llx outside the window, which holds a different frame (marker seq=0xdead0005)\n",
                              (unsigned long long)cphys2);
                    rc = ck_gem_xfer(&g, &x);
                    print_xfer("confinement B (tx read outside) seq=5", rc, &x, txf);
                    {
                        uint8_t outf[64];
                        uint32_t dm = 1, dt = 1;
                        ck_gem_frame_build(CK_GEMF_RAW, k_mac, 0xdead0005u, tpa, outf, sizeof outf);
                        if (x.rx_done) {
                            dm = ck_gem_frame_diff(outf, 60, x.rx_data, x.rx_copied);
                            dt = ck_gem_frame_diff(txf, 60, x.rx_data, x.rx_copied);
                        }
                        ck_printf("pi5_gem: CONFINEMENT B result: rx_done=%u diff_vs_outside_marker=%u diff_vs_inside_frame=%u tsr_bex=%u hresp=%u\n",
                                  x.rx_done, x.rx_done ? dm : 0xffffffffu, x.rx_done ? dt : 0xffffffffu, (x.tsr & CK_GEM_TSR_BEX) != 0,
                                  (x.isr & CK_GEM_ISR_HRESP) != 0);
                        ck_printf("pi5_gem: CONFINEMENT B %s\n", (x.rx_done && dm == 0) ? "NOT BLOCKED: the device read the outside page (hardware backstop NOT effective for reads)" :
                                                                                     "BLOCKED: the outside page was not delivered as the frame");
                    }
                }
            }
        }
        if (mode == CK_GEMX_PHYLB) {
            rc = ck_phy_loopback_exit(c->rio, c->phy_addr, bmcr0);
            ck_printf("pi5_gem: phy loopback exit rc=%d\n", rc);
        }
    }

    ck_gem_park(&g);
    ck_printf("pi5_gem: done: GEM RE/TE/LLB off, halted=%u bme_on=%u rp1 pci cmd=0x%08x NCR=0x%08x\n", g.halted, g.bme_on,
              pci_cmd(c), c->rio->rd(0, CK_RP1_ETH + CK_GEM_NCR));
}
#endif
