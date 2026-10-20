/* xhci_pipe.h -- generic polled xHCI host pipes for the AIENOS C kernel
 * (L207): control, bulk and interrupt transfers to one device at a time,
 * inside the xHCI DMA fence (xhci_fence.c). usb_kbd.c is the keyboard-only
 * driver and stays as it is; this is the same ring and context code in the
 * same style, made reusable for Bluetooth (dev/usb_bt.c).
 *
 * Reference: xHCI 1.2c (~/docs/imac-hardware/txt/xhci-spec-1.2c.txt) sections
 * 5.3-5.6 registers, 6.2 contexts, 6.4 TRBs, 4.3 device slot bring-up; USB 2.0
 * chapter 9 standard requests.
 *
 * Rules shared with the keyboard driver:
 *   - every ring, context and buffer lives in the fence's DMA region, the
 *     controller stream's only SMMU window; no SMMU confinement means this code
 *     never runs (the fence decides, not this file);
 *   - polled only: no interrupt, no scheduler; every wait is bounded;
 *   - the caller revokes DMA afterwards whatever this returns.
 *
 * One device slot at a time: attach() enables a slot for a port, detach()
 * frees it. Devices behind a hub are not reached (hubs are not driven).
 * QEMU is not hardware: nothing here is physically qualified. */
#ifndef AIENOS_CK_XHCI_PIPE_H
#define AIENOS_CK_XHCI_PIPE_H
#include <stddef.h>
#include <stdint.h>

/* Pages of the fence's DMA region used (4 KiB each). The fence's region is
 * 64 KiB = 16 pages, so this fits without enlarging it. */
#define CK_XP_PAGES 16u
#define CK_XP_MAX_SCRATCHPADS 7u
#define CK_XP_BUF_BYTES 4096u    /* bounce buffer for control, bulk-in and bulk-out */
#define CK_XP_MAX_EPS 3u         /* non-control endpoints configured at once */
#define CK_XP_INTR_BYTES 1024u   /* interrupt IN bounce buffer */

enum {
    CK_XP_OK = 0,
    CK_XP_E_ARG = -951,     /* bad region, registers or argument */
    CK_XP_E_UNSUP = -952,   /* controller or device needs something this driver lacks */
    CK_XP_E_TIMEOUT = -953, /* a bounded wait ran out */
    CK_XP_E_FAILED = -954,  /* a command or transfer completed with an error */
    CK_XP_E_STALL = -955,   /* the endpoint answered STALL */
    CK_XP_E_STATE = -956,   /* no slot / endpoint not configured */
};

enum { CK_XP_EP_BULK = 2, CK_XP_EP_INTR = 3 }; /* USB bmAttributes transfer type */

typedef struct {
    uint8_t addr;       /* bEndpointAddress, bit 7 = IN */
    uint8_t type;       /* CK_XP_EP_BULK or CK_XP_EP_INTR */
    uint16_t max_packet;
    uint8_t interval;   /* bInterval (interrupt) */
} ck_xp_ep;

typedef struct {
    uint64_t ptr;
    uint32_t resid;     /* bytes not transferred, from the event */
    uint8_t valid, code;
} ck_xp_done;

typedef struct {
    volatile uint32_t *t;
    uint64_t phys;
    uint32_t i, c, n; /* next TRB, producer cycle, TRBs in the ring (last one is the Link TRB) */
} ck_xp_ring;

typedef struct {
    volatile uint8_t *base;
    uint32_t op, rt, db;
    uint8_t *mem;
    uint64_t phys;
    unsigned ctx, max_ports;
    ck_xp_ring cmd, ep0, ring[CK_XP_MAX_EPS];
    uint32_t ev_i, ev_c;
    uint8_t slot, port, speed;
    uint16_t mps0;
    uint8_t dev_desc[18];
    uint8_t ep_dci[CK_XP_MAX_EPS], ep_addr[CK_XP_MAX_EPS], ep_n;
    uint16_t ep_mps[CK_XP_MAX_EPS];
    uint32_t armed[CK_XP_MAX_EPS]; /* length of the queued interrupt IN transfer */
    ck_xp_done done[32];  /* per device context index: the last transfer event */
    ck_xp_done cmd_done;
    uint32_t last_code;
    const char *why;
    int started;
} ck_xp;

/* Take the controller from halted or running, reset it, set up rings and run.
 * mem/phys/bytes: the fence's DMA region, CK_XP_PAGES pages, 4 KiB aligned. */
int ck_xp_start(ck_xp *x, volatile uint8_t *bar0, uint8_t *mem, uint64_t phys, size_t bytes);
/* Run/Stop = 0 and wait for halted; safe to call repeatedly. */
void ck_xp_stop(ck_xp *x);

unsigned ck_xp_max_ports(const ck_xp *x);
int ck_xp_port_present(ck_xp *x, unsigned port);

/* Reset the port, enable a slot, address the device, read its device
 * descriptor into x->dev_desc (18 bytes). */
int ck_xp_attach(ck_xp *x, unsigned port);
/* Free the slot (Disable Slot). Idempotent. */
void ck_xp_detach(ck_xp *x);
/* Whole configuration descriptor (9-byte header first, then wTotalLength). */
int ck_xp_config_desc(ck_xp *x, uint8_t configuration_index, uint8_t *out, size_t cap, size_t *len);
/* Configure Endpoint for eps[0..n) (n <= CK_XP_MAX_EPS), then SET_CONFIGURATION. */
int ck_xp_configure(ck_xp *x, uint8_t configuration_value, const ck_xp_ep *eps, unsigned n);

/* Control transfer on endpoint 0. Data direction from bit 7 of rtype; data is
 * len bytes (0 for none, at most CK_XP_BUF_BYTES - 64). got = bytes received. */
int ck_xp_control(ck_xp *x, uint8_t rtype, uint8_t req, uint16_t val, uint16_t idx, void *data, uint16_t len,
                  uint32_t *got, uint32_t timeout_ms);
int ck_xp_bulk_out(ck_xp *x, uint8_t ep_addr, const void *data, uint32_t len, uint32_t timeout_ms);
int ck_xp_bulk_in(ck_xp *x, uint8_t ep_addr, void *out, uint32_t cap, uint32_t *got, uint32_t timeout_ms);
/* Interrupt IN: queue one transfer of up to len (<= CK_XP_INTR_BYTES) bytes,
 * then poll. poll returns 0 with *got filled, 1 if nothing arrived yet,
 * a negative value on failure. */
int ck_xp_intr_arm(ck_xp *x, uint8_t ep_addr, uint32_t len);
int ck_xp_intr_poll(ck_xp *x, uint8_t ep_addr, void *out, uint32_t cap, uint32_t *got);
#endif
