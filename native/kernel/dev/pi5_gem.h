/* pi5_gem.h -- Raspberry Pi 5 network lane, cut 3: the RP1 Ethernet MAC (Cadence GEM_GXL) sends and
 * receives ONE frame through DMA descriptors that live only inside the ADR 0020 bounce window and pass the
 * dma_window.c validator, plus the PHY link/loopback helpers the proof needs.
 *
 * LABEL: software-only DMA confinement (Pi 5, weaker than the Spark). Pi-only; not linked into the Spark or QEMU.
 *
 * Hosted-testable over fake registers and a fake GEM (tests/test_pi5_gem.c, with mutants). A host test proves
 * the decisions and the ORDER of register writes, NOT the hardware. The MMIO glue at the bottom of pi5_gem.c
 * is the CK_PI5_NET image only. Every number below cites its source; anything not read is UNVERIFIED.
 *
 * SOURCES (all read in this cut; Linux = raspberrypi/linux branch rpi-6.12.y):
 *  [M1] drivers/net/ethernet/cadence/macb.h: register offsets (MACB_NCR/NCFGR/NSR/TSR/RBQP/TBQP/RSR/ISR/IDR,
 *       GEM_DMACFG/USRIO/PBUFRXCUT/AMP/HRB/HRT/SA1B..SA4T/DCFG1/DCFG2/DCFG6, GEM_ISR/TBQP/RBQP/RBQS/IDR(hw_q),
 *       GEM_TBQPH/RBQPH), bit fields (NCR LLB/RE/TE/MPE/CLRSTAT/TSTART, NCFGR SPD/FD/GBE/DBW/DRFCS/CLK, DMACFG
 *       FBLDO/RXBMS/TXPBMS/TXCOEN/RXBS/ADDR64/RXEXT/TXEXT, TSR UBR/TGO/BEX/COMP/UND, RSR BNA/REC/OVR, DCFG6 DAW64),
 *       descriptor layout (struct macb_dma_desc addr/ctrl, macb_dma_desc_64 addrh/resvd; RX USED/WRAP/FRMLEN/SOF/EOF,
 *       TX FRMLEN(14 bits on GEM)/LAST/NOCRC/BUF_EXHAUSTED/UNDERRUN/ERROR/WRAP/USED).
 *  [M2] drivers/net/ethernet/cadence/macb_main.c: macb_reset_hw, macb_init_hw, macb_configure_dma, gem_init_axi,
 *       macb_set_hwaddr, macb_init_buffers (per-queue RBQP/TBQP, shared RBQPH/TBQPH), macb_probe_queues (queue mask
 *       from DCFG6), macb_dbw (DCFG1.DBWDEF), macb_init (USRIO rgmii), macb_mac_link_up (SPD/FD/GBE, then RE|TE),
 *       macb_set_addr / gem_rx_refill (RX address word: USED clears last, WRAP on the last slot), macb_tx_map (TX ctrl
 *       written last, USED clear hands the slot over), raspberrypi_rp1_config (dma_burst_length 16, jumbo caps),
 *       macb_init_tieoff (unused RX queue: WRAP|USED descriptor), rp1.dtsi rp1_eth (phy-mode rgmii-id,
 *       cdns,aw2w-max-pipe/ar2r-max-pipe 8, use-aw2b-fill), bcm2712-rpi-5-b.dts (phy reg 1).
 *  [P1] drivers/net/phy/broadcom.c bcm54xx_config_clock_delay (RGMII-ID: AUXCTL misc shadow skew enable, shadow 0x03
 *       GTXCLK_EN), drivers/net/phy/bcm-phy-lib.c bcm54xx_auxctl_read/write, bcm_phy_read/write_shadow,
 *       include/linux/brcmphy.h shadow numbers, include/uapi/linux/mii.h BMCR/BMSR/ADVERTISE/CTRL1000/STAT1000 bits,
 *       drivers/net/phy/phy_device.c genphy_loopback (BMCR LOOPBACK + fixed speed/duplex, then wait BMSR link).
 *  [D1] RP1 peripherals datasheet RP-008370-DS-1 chapter 7: GEM_GXL (64-bit address DMA, internal loopback, RGMII),
 *       ETH_CFG STATUS (RGMII link/speed/duplex), CLKGEN (tx clock follows the MAC speed).
 *  [I1] IEEE 802.3 clause 22 (MII registers, BMCR loopback bit 14), IEEE 802 Local Experimental Ethertype 1 = 0x88b5.
 * Not read: the Cadence GEM_GXL user guide and the BCM54213PE datasheet (not public). Anything only they would
 * say (what exactly local loopback does at the RGMII pins) is UNVERIFIED and is measured by the Pi run. */
#ifndef AIENOS_CK_PI5_GEM_H
#define AIENOS_CK_PI5_GEM_H
#include <stddef.h>
#include <stdint.h>
#include "pi5_net.h"
#include "dma_window.h"

enum {
    CK_GEM_OK = 0,
    CK_GEM_E_ARG = -1,
    CK_GEM_E_HALTED = -2,   /* latched earlier: no more DMA until reboot */
    CK_GEM_E_WINDOW = -3,   /* the validator refused a descriptor or buffer (window latched, BME never enabled) */
    CK_GEM_E_HWWIN = -4,    /* the root-complex window is not confirmed confined: refuse to enable bus-master */
    CK_GEM_E_BME = -5,      /* bus-master enable/disable did not read back */
    CK_GEM_E_QUIESCE = -6,  /* the transmitter did not stop (halted) */
    CK_GEM_E_DEVICE = -7,   /* the device reported an impossible index or length (window latched) */
    CK_GEM_E_NOGEM = -8,    /* MID says this is not a GEM */
    CK_GEM_E_TIMEOUT = -9,
    CK_GEM_E_READBACK = -10
};

/* ============ GEM registers: offset from the GEM base (CK_RP1_ETH in BAR1) [M1] ============ */
#define CK_GEM_USRIO 0x00cu
#define CK_GEM_DMACFG 0x010u
#define CK_GEM_TSR 0x014u
#define CK_GEM_RBQP 0x018u
#define CK_GEM_TBQP 0x01cu
#define CK_GEM_RSR 0x020u
#define CK_GEM_ISR 0x024u
#define CK_GEM_IDR 0x02cu
#define CK_GEM_PBUFRXCUT 0x044u
#define CK_GEM_AMP 0x054u
#define CK_GEM_HRB 0x080u
#define CK_GEM_HRT 0x084u
#define CK_GEM_SA1B 0x088u
#define CK_GEM_SA1T 0x08cu
#define CK_GEM_SA2B 0x090u /* SA2B..SA4T = 0x90..0xa4 */
#define CK_GEM_DCFG1 0x280u
#define CK_GEM_DCFG2 0x284u
#define CK_GEM_DCFG6 0x294u
#define CK_GEM_TBQPH 0x4c8u /* shared by every queue */
#define CK_GEM_RBQPH 0x4d4u
/* queues 1.. (hw_q - 1 = n): GEM_ISR(n) 0x400, GEM_TBQP(n) 0x440, GEM_RBQP(n) 0x480, GEM_RBQS(n) 0x4a0, GEM_IDR(n) 0x620 */
#define CK_GEM_QN_TBQP(hwq) (0x440u + (((hwq) - 1u) << 2))
#define CK_GEM_QN_RBQP(hwq) (0x480u + (((hwq) - 1u) << 2))
#define CK_GEM_QN_RBQS(hwq) (0x4a0u + (((hwq) - 1u) << 2))
#define CK_GEM_QN_IDR(hwq) (0x620u + (((hwq) - 1u) << 2))
#define CK_GEM_QN_ISR(hwq) (0x400u + (((hwq) - 1u) << 2))

/* NCR (CK_GEM_NCR_MPE is in pi5_net.h) */
#define CK_GEM_NCR_LLB (1u << 1)
#define CK_GEM_NCR_RE (1u << 2)
#define CK_GEM_NCR_TE (1u << 3)
#define CK_GEM_NCR_CLRSTAT (1u << 5)
#define CK_GEM_NCR_TSTART (1u << 9)
/* NCFGR (the MDC divider CK_GEM_NCFGR_CLK_* is in pi5_net.h) */
#define CK_GEM_NCFGR_SPD (1u << 0) /* 100 Mbit when set (GBE clear) */
#define CK_GEM_NCFGR_FD (1u << 1)
#define CK_GEM_NCFGR_CAF (1u << 4)
#define CK_GEM_NCFGR_NBC (1u << 5)
#define CK_GEM_NCFGR_GBE (1u << 10)
#define CK_GEM_NCFGR_DRFCS (1u << 17)
#define CK_GEM_NCFGR_DBW_SHIFT 21
#define CK_GEM_NCFGR_DBW_MASK (3u << 21)
/* DMACFG */
#define CK_GEM_DMACFG_FBLDO_MASK 0x1fu
#define CK_GEM_DMACFG_ENDIA_DESC (1u << 6)
#define CK_GEM_DMACFG_ENDIA_PKT (1u << 7)
#define CK_GEM_DMACFG_RXBMS_MASK (3u << 8)
#define CK_GEM_DMACFG_TXPBMS (1u << 10)
#define CK_GEM_DMACFG_TXCOEN (1u << 11)
#define CK_GEM_DMACFG_RXBS_SHIFT 16
#define CK_GEM_DMACFG_RXBS_MASK (0xffu << 16)
#define CK_GEM_DMACFG_RXEXT (1u << 28)
#define CK_GEM_DMACFG_TXEXT (1u << 29)
#define CK_GEM_DMACFG_ADDR64 (1u << 30)
#define CK_GEM_BURST 16u        /* macb dma_burst_length for raspberrypi_rp1_config */
#define CK_GEM_RXBS_UNIT 64u    /* RX_BUFFER_MULTIPLE */
/* TSR / RSR / ISR */
#define CK_GEM_TSR_UBR (1u << 0)
#define CK_GEM_TSR_RLE (1u << 2)
#define CK_GEM_TSR_TGO (1u << 3)
#define CK_GEM_TSR_BEX (1u << 4)
#define CK_GEM_TSR_COMP (1u << 5)
#define CK_GEM_TSR_UND (1u << 6)
#define CK_GEM_RSR_BNA (1u << 0)
#define CK_GEM_RSR_REC (1u << 1)
#define CK_GEM_RSR_OVR (1u << 2)
#define CK_GEM_ISR_HRESP (1u << 11)
/* USRIO */
#define CK_GEM_USRIO_RGMII (1u << 0)
/* AMP */
#define CK_GEM_AMP_AR2R_MASK 0xffu
#define CK_GEM_AMP_AW2W_SHIFT 8
#define CK_GEM_AMP_AW2W_MASK (0xffu << 8)
#define CK_GEM_AMP_AW2B_FILL (1u << 16)
#define CK_GEM_AXI_PIPE 8u /* rp1.dtsi cdns,aw2w-max-pipe = <8>, cdns,ar2r-max-pipe = <8> */
/* DCFG */
#define CK_GEM_DCFG1_DBWDEF_SHIFT 25
#define CK_GEM_DCFG6_DAW64 (1u << 23)
#define CK_GEM_MID_IDNUM_SHIFT 16

/* Descriptors [M1]: word0 = address (RX: bits 31:2, bit1 WRAP, bit0 USED), word1 = ctrl, then (64-bit address
 * mode) word2 = address high, word3 reserved. */
#define CK_GEMD_RX_USED (1u << 0)
#define CK_GEMD_RX_WRAP (1u << 1)
#define CK_GEMD_RX_FRMLEN_MASK 0xfffu
#define CK_GEMD_RX_SOF (1u << 14)
#define CK_GEMD_RX_EOF (1u << 15)
#define CK_GEMD_TX_FRMLEN_MASK 0x3fffu /* GEM_TX_FRMLEN_SIZE 14 */
#define CK_GEMD_TX_LAST (1u << 15)
#define CK_GEMD_TX_NOCRC (1u << 16)
#define CK_GEMD_TX_BUF_EXHAUSTED (1u << 27)
#define CK_GEMD_TX_UNDERRUN (1u << 28)
#define CK_GEMD_TX_ERROR (1u << 29)
#define CK_GEMD_TX_WRAP (1u << 30)
#define CK_GEMD_TX_USED (1u << 31)

#define CK_GEM_MAX_QUEUES 8u
#define CK_GEM_TXRING 2u  /* slot 0 = the frame, slot 1 = a USED|WRAP sentinel */
#define CK_GEM_RXRING 8u  /* power of two for the ring helper; unused slots are USED sentinels */
#define CK_GEM_RXMAX 7u   /* real RX buffers per transfer (one ring slot is always a sentinel) */
#define CK_GEM_RXKEEP 64u /* bytes of each received frame kept for the caller (Ethernet+ARP is 42) */
#define CK_GEM_RXCOPY 256u

/* What the hardware says it is (read from the registers, never assumed). */
struct ck_gem_caps {
    uint32_t mid, dcfg1, dcfg2, dcfg6;
    int is_gem;           /* MID IDNUM >= 2 (hw_is_gem) */
    int daw64;            /* DCFG6 bit 23 */
    uint32_t queue_mask;  /* bit q set = hardware queue q exists; bit 0 always */
    uint32_t nqueues;
    uint32_t desc_bytes;  /* 16 with 64-bit descriptors, else 8 */
    uint32_t dbw;         /* NCFGR DBW field value (macb_dbw) */
};
int ck_gem_probe(const struct ck_rp1_io *io, struct ck_gem_caps *c);

/* The driver context. All memory the device may touch is the bounce window `w`, mapped at `va`. */
struct ck_gem {
    const struct ck_rp1_io *rio;
    struct ck_gem_caps caps;
    struct ck_dmaw *w;        /* the ADR 0020 validator and allocator */
    volatile uint8_t *va;     /* CPU mapping of the window; bus address w->base */
    int (*bme)(void *ctx, int on); /* RP1 PCI bus-master enable: 0 = done AND read back */
    void *bme_ctx;
    void (*barrier)(void);    /* data barrier between descriptor stores and the register kick */
    int hw_window_ok;         /* the root-complex window read back CONFINED (cut 2 verdict); else no bus-master */
    int halted;               /* latched until reboot */
    int bme_on;
    uint32_t xfers;
};
void ck_gem_init(struct ck_gem *g, const struct ck_rp1_io *rio, struct ck_dmaw *w, volatile uint8_t *va,
                 int (*bme)(void *, int), void *bme_ctx, void (*barrier)(void), int hw_window_ok);

enum { CK_GEMX_LLB = 1, CK_GEMX_PHYLB = 2, CK_GEMX_WIRE = 3 };

struct ck_gem_xfer {
    /* in */
    int mode;                      /* LLB: NCR.LLB; PHYLB/WIRE: GEM normal (the PHY loops or the wire does) */
    uint32_t speed;                /* 1000, 100 or 10 */
    int full_duplex;
    uint8_t mac[6];
    const uint8_t *frame;          /* sent as given (the MAC appends the FCS) */
    uint32_t frame_len;            /* 60..1514 */
    uint32_t rx_count;             /* real RX buffers, 0..CK_GEM_RXMAX (0 = TX only); the rest are USED sentinels */
    uint32_t rx_buf_len;           /* multiple of 64, 64..1536 */
    uint32_t tx_wait_us, rx_wait_us;
    uint64_t tx_addr, rx_addr;     /* 0 = allocate inside the window; non-zero = caller-chosen, STILL validated */
    /* Confinement test ONLY (ADR 0020 test 5). After the validated slot is written, the hardware-visible address
     * word of TX slot 0 / RX slot 0 is replaced by this raw address. The shadow still holds the valid one.
     * Callers must have proven the target is a page they own and that a write there is harmless. */
    uint64_t raw_tx_outside, raw_rx_outside;
    /* out */
    int tx_done, rx_done;
    uint32_t tx_ctrl, rx_word0, rx_ctrl, rx_len;
    uint32_t tsr, rsr, isr, ncr_run, ncfgr_run, dmacfg_run, ncr_end, tsr_end;
    uint64_t tx_bus, rx_bus;       /* where the buffers were */
    uint8_t rx_data[CK_GEM_RXCOPY];
    uint32_t rx_copied;
    uint32_t rx_frames;            /* RX slots the device handed back (USED, SOF and EOF) */
    uint32_t rx_slot;              /* the first such slot; rx_data/rx_len/rx_ctrl/rx_word0 are its */
    int bme_enabled_for_us;        /* 1 once bus-master was turned on for this transfer */
    /* Cut 4: every RX slot the device handed back, in ring order (the first CK_GEM_RXKEEP bytes, copied out of the
     * window; the frame length is the device's word, already checked against the buffer size). */
    uint32_t rx_all_n;
    uint32_t rx_all_len[CK_GEM_RXMAX];
    uint8_t rx_all[CK_GEM_RXMAX][CK_GEM_RXKEEP];
    /* Optional early stop: called from the poll with each completed slot (data = first min(len,CK_GEM_RXKEEP) bytes).
     * Non-zero ends the wait. It must not touch the device. Without it the wait ends on the first completed slot. */
    int (*rx_probe)(void *ctx, const uint8_t *data, uint32_t n, uint32_t len);
    void *rx_probe_ctx;
    /* Optional: hand the probe up to rx_probe_cap bytes of each frame (copied out of the window into this caller
     * buffer) instead of the first CK_GEM_RXKEEP. NULL = the CK_GEM_RXKEEP stack copy. */
    uint8_t *rx_probe_buf;
    uint32_t rx_probe_cap;
    int rx_probe_hit;              /* the probe returned non-zero */
    int bme_off_at_end;
};
/* One transfer: validate everything, THEN program the GEM, THEN enable RP1 bus-master, kick, poll,
 * quiesce (RE/TE off, wait TGO clear, 2 ms), THEN disable bus-master and read the results. 0 = the procedure
 * ran and the device is stopped (tx_done/rx_done say what happened); negative = refused or failed. */
int ck_gem_xfer(struct ck_gem *g, struct ck_gem_xfer *x);
/* Stop the MAC (RE/TE/LLB off, MPE kept) with bus-master already off; for the end of a run. */
void ck_gem_park(struct ck_gem *g);

/* ============ frames ============ */
enum { CK_GEMF_RAW = 1, CK_GEMF_ARP = 2 };
#define CK_GEMF_ETHERTYPE_EXP 0x88b5u
/* A 60 byte frame (MAC pads nothing, appends FCS). Raw: broadcast, EtherType 0x88b5, magic, sequence number,
 * pattern. ARP: broadcast ARP probe (sender IP 0.0.0.0) for tpa, trailer carries magic + sequence number. */
uint32_t ck_gem_frame_build(int kind, const uint8_t mac[6], uint32_t seq, const uint8_t tpa[4], uint8_t *out, uint32_t cap);
/* 0 = identical; else 1 + index of the first difference (a length difference counts at min(len)). */
uint32_t ck_gem_frame_diff(const uint8_t *a, uint32_t alen, const uint8_t *b, uint32_t blen);


/* ============ Cut 4: one ARP exchange on the real LAN ============
 * We send ONE broadcast ARP request (RFC 826) "who has <target>? tell <our ip>" and accept ONLY an ARP reply
 * (opcode 2) that is addressed to our MAC, answers our request (sender IP = the address we asked about, target IP
 * and target MAC = ours) and is internally consistent (Ethernet source = ARP sender MAC). Every other frame the MAC
 * hands up (broadcasts, multicast, unrelated unicast) is only counted and classified, never parsed further.
 * Sources: RFC 826 (packet format, opcodes 1 and 2), IEEE 802.3 (60 byte minimum frame without FCS; DRFCS strips the
 * FCS on receive). */
#define CK_ARP_FRAME_LEN 60u
#define CK_ARP_MINLEN 42u  /* 14 Ethernet + 28 ARP */
#define CK_ARP_MAXLEN 64u  /* a reply is 42..60 bytes (padded); anything longer is not ours to parse */
enum {
    CK_ARP_OK = 0,
    CK_ARP_E_ARG = -1,
    CK_ARP_E_LEN = -2,     /* shorter than 42, longer than 64, or the kept bytes do not cover the frame */
    CK_ARP_E_ETYPE = -3,   /* not EtherType 0x0806 */
    CK_ARP_E_DST = -4,     /* Ethernet destination is not our MAC (broadcast and other hosts are refused) */
    CK_ARP_E_HDR = -5,     /* htype/ptype/hlen/plen are not Ethernet/IPv4/6/4 */
    CK_ARP_E_OP = -6,      /* opcode is not 2 (reply) */
    CK_ARP_E_SRC = -7,     /* Ethernet source differs from the ARP sender hardware address */
    CK_ARP_E_THA = -8,     /* ARP target hardware address is not ours */
    CK_ARP_E_TPA = -9,     /* ARP target protocol address is not our IP */
    CK_ARP_E_SPA = -10     /* ARP sender IP is not the address we asked about */
};
#define CK_ARP_NREASONS 11u
#define CK_ARP_MAXATT 8u
#define CK_ARP_SEEN 8u     /* frames whose header is recorded for the log */
#define CK_ARP_HDRKEEP 24u
/* Build the 60 byte request (broadcast dst, our MAC, sender IP = our IP, target hardware zero, zero padding). */
uint32_t ck_arp_request_build(const uint8_t mac[6], const uint8_t sip[4], const uint8_t tip[4], uint8_t *out, uint32_t cap);
/* data = the kept bytes (n), len = the frame length the device reported. 0 = accepted; sender_mac/sender_ip filled. */
int ck_arp_reply_check(const uint8_t *data, uint32_t n, uint32_t len, const uint8_t our_mac[6], const uint8_t our_ip[4],
                       const uint8_t asked_ip[4], uint8_t sender_mac[6], uint8_t sender_ip[4]);
enum { CK_FC_RUNT = 0, CK_FC_BROADCAST = 1, CK_FC_MULTICAST = 2, CK_FC_UNICAST_US = 3, CK_FC_UNICAST_OTHER = 4 };
/* Classify by the Ethernet destination only (14 bytes needed); nothing else is looked at. */
int ck_frame_class(const uint8_t *data, uint32_t n, const uint8_t our_mac[6]);

struct ck_arp_exchange {
    /* in */
    uint8_t mac[6], our_ip[4], target_ip[4];
    uint32_t speed;           /* 10, 100 or 1000 (from the PHY) */
    int full_duplex;
    uint32_t attempts;        /* 1..CK_ARP_MAXATT transfers, stop at the first accepted reply */
    uint32_t wait_us;         /* receive wait per attempt, up to 10 s */
    /* out */
    int matched;
    uint32_t attempts_used, tx_ok;     /* tx_ok = transmissions the MAC completed without an error bit */
    uint8_t reply_mac[6], reply_ip[4];
    uint8_t reply_frame[CK_ARP_MAXLEN];
    uint32_t reply_len;
    uint32_t frames_seen;     /* every frame the MAC handed up, the accepted reply included */
    uint32_t n_broadcast, n_multicast, n_unicast_us, n_unicast_other, n_runt;
    uint64_t (*now_us)(void);     /* optional clock for attempt_start_us */
    uint64_t attempt_start_us[CK_ARP_MAXATT];
    /* Optional: called before each attempt (qualification: try another documented clock-delay setting). */
    void (*pre_attempt)(void *ctx, uint32_t attempt);
    void *pre_ctx;
    uint32_t seen_n;              /* frames recorded below (the first CK_ARP_SEEN of them) */
    uint32_t seen_len[CK_ARP_SEEN];
    uint32_t seen_attempt[CK_ARP_SEEN];
    uint8_t seen_hdr[CK_ARP_SEEN][CK_ARP_HDRKEEP]; /* first CK_ARP_HDRKEEP bytes: Ethernet header + start of the ARP packet */
    uint32_t n_arp_refused[CK_ARP_NREASONS]; /* frames that looked like ARP replies to us but failed a check, by -reason */
    int last_rc;
    uint32_t last_tsr, last_rsr, last_isr;
    int bme_off_at_end;       /* of the last transfer */
};
/* Set the MAC speed/duplex when the PHY link comes up (RE/TE must be off; no DMA). Like Linux macb_mac_link_up. */
int ck_gem_set_link(struct ck_gem *g, uint32_t speed, int full_duplex);
/* Up to `attempts` transfers (each: validate, program, bus-master on, send, wait, quiesce, bus-master off). */
int ck_gem_arp_exchange(struct ck_gem *g, struct ck_arp_exchange *e);

/* ============ PHY (clause 22) ============ */
int ck_gem_mdio_write(const struct ck_rp1_io *io, uint32_t phy, uint32_t reg, uint16_t val);
uint32_t ck_gem_mdio_frame_write(uint32_t phy, uint32_t reg, uint16_t val);
#define CK_MII_BMCR 0u
#define CK_MII_BMSR 1u
#define CK_MII_ADVERTISE 4u
#define CK_MII_LPA 5u
#define CK_MII_CTRL1000 9u
#define CK_MII_STAT1000 10u
#define CK_MII_BMCR_SPEED1000 0x0040u
#define CK_MII_BMCR_FULLDPLX 0x0100u
#define CK_MII_BMCR_ANRESTART 0x0200u
#define CK_MII_BMCR_ANENABLE 0x1000u
#define CK_MII_BMCR_SPEED100 0x2000u
#define CK_MII_BMCR_LOOPBACK 0x4000u
#define CK_MII_BMSR_LSTATUS 0x0004u
#define CK_MII_BMSR_ANEGCOMPLETE 0x0020u

struct ck_phy_link {
    uint16_t bmcr, bmsr_first, bmsr, adv, lpa, ctrl1000, stat1000;
    int link;              /* BMSR link status (read twice: the bit is latched low) */
    int aneg_done;
    uint32_t speed;        /* resolved 1000/100/10; 0 = unresolved */
    int full_duplex;
};
/* Read the registers and resolve speed/duplex from the advertisement and partner ability (IEEE 802.3 clause 28/40;
 * the same resolution as Linux genphy_read_status). Does not write. */
int ck_phy_link_read(const struct ck_rp1_io *io, uint32_t phy, struct ck_phy_link *l);
/* ETH_CFG STATUS decode [D1]: bit0 link, bits2:1 speed (0=10,1=100,2=1000), bit3 duplex. speed 0 if reserved. */
void ck_eth_cfg_status_decode(uint32_t st, int *link, uint32_t *speed, int *full_duplex);
/* Linux bcm54xx_config_clock_delay for RGMII-ID [P1]. before/after = the two registers read before the write. */
int ck_phy_rgmii_id_config(const struct ck_rp1_io *io, uint32_t phy, uint16_t *auxctl_before, uint16_t *clk_before);
/* genphy_loopback [P1]: BMCR = LOOPBACK | duplex | speed, then wait (bounded) for BMSR link. *bmcr_saved = BMCR found. */
int ck_phy_loopback_enter(const struct ck_rp1_io *io, uint32_t phy, uint32_t speed, uint16_t *bmcr_saved,
                          uint16_t *bmsr_out);
/* Leave loopback: BMCR = saved (autoneg restarted if it was enabled). */
int ck_phy_loopback_exit(const struct ck_rp1_io *io, uint32_t phy, uint16_t bmcr_saved);

/* RP1 PCI bus-master through the root complex config window (needs a read back). */
int ck_pi5gem_bme_set(const struct ck_pi5pcie_io *io, int on);

#ifdef CK_PI5_NET
/* Image glue called by ck_pi5net_run after the PHY id is read. */
struct ck_pi5gem_ctx {
    const struct ck_pi5pcie_io *rc;
    const struct ck_rp1_io *rio;
    struct ck_dmaw *dw;
    void *bounce;
    uint64_t bounce_phys;
    int hw_window_confined;
    uint32_t phy_addr;
    uint32_t pclk_hz;
};
void ck_pi5gem_run(const struct ck_pi5gem_ctx *c);
#endif

#endif
