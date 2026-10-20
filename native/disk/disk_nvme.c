/*
 * disk_nvme.c -- freestanding polled NVMe driver (see disk_nvme.h).
 * No libc, no heap, no clock: platform access only through nvme_ops.
 * Assumes a little-endian CPU (NVMe structures are little-endian).
 */
#include "disk_nvme.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "disk_nvme.c assumes a little-endian CPU"
#endif

#define CC_EN 1u
#define CC_IOSQES_64 (6u << 16)
#define CC_IOCQES_16 (4u << 20)
#define CSTS_RDY 1u
#define CSTS_CFS 2u
#define POLL_STEP_US 10u
#define SQE_BYTES 64u
#define CQE_BYTES 16u

/* ---- small freestanding helpers (volatile so the compiler cannot turn them
 * into memset/memcpy calls) ---- */

static void zero_bytes(uint8_t *p, uint64_t n)
{
    volatile uint8_t *q = p;
    while (n--)
        *q++ = 0;
}

static void copy_bytes(uint8_t *d, const uint8_t *s, uint64_t n)
{
    volatile uint8_t *q = d;
    const volatile uint8_t *r = s;
    while (n--)
        *q++ = *r++;
}

static uint16_t ld16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t ld64(const uint8_t *p)
{
    return (uint64_t)ld32(p) | ((uint64_t)ld32(p + 4) << 32);
}

static uint32_t rd32(nvme_ctrl *c, uint32_t off)
{
    return c->ops.read32(c->ops.ctx, off);
}

static void wr32(nvme_ctrl *c, uint32_t off, uint32_t v)
{
    c->ops.write32(c->ops.ctx, off, v);
}

static uint64_t rd64(nvme_ctrl *c, uint32_t off)
{
    if (c->ops.read64)
        return c->ops.read64(c->ops.ctx, off);
    uint64_t lo = rd32(c, off);
    return lo | ((uint64_t)rd32(c, off + 4) << 32);
}

static void wr64(nvme_ctrl *c, uint32_t off, uint64_t v)
{
    if (c->ops.write64) {
        c->ops.write64(c->ops.ctx, off, v);
        return;
    }
    wr32(c, off, (uint32_t)v);
    wr32(c, off + 4, (uint32_t)(v >> 32));
}

static void barrier(nvme_ctrl *c)
{
    c->ops.barrier(c->ops.ctx);
}

static void release_slots(nvme_ctrl *c)
{
    for (uint32_t i = 0; i < NVME_SLOTS_MAX; i++)
        c->slot[i].busy = 0;
    c->inflight = 0;
}

static int wait_ready(nvme_ctrl *c, int want);

/* Fail-stop. With I/O commands still in flight the controller is reset
 * (CC.EN 1 -> 0 deletes the I/O queues and idles the hardware, NVMe 1.4
 * CC.EN), so nothing DMAs into a caller buffer after the call returns. */
static int fail(nvme_ctrl *c, int err)
{
    c->failed = 1;
    c->last_err = err;
    if (c->inflight) {
        c->stats.fail_resets++;
        wr32(c, NVME_REG_CC, rd32(c, NVME_REG_CC) & ~CC_EN); /* GUARD:nvme-fail-reset */
        (void)wait_ready(c, 0);
        release_slots(c);
    }
    return err;
}

/* Wait for CSTS.RDY == want. Reads CSTS, then delays 1 ms, at most
 * timeout_ms delays in total: the bound is exactly CAP.TO (P2 audit fix). */
static int wait_ready(nvme_ctrl *c, int want)
{
    for (uint32_t i = 0;; i++) {
        uint32_t s = rd32(c, NVME_REG_CSTS);
        if (s & CSTS_CFS) return NVME_ECFS; /* GUARD:nvme-cfs-ready */
        if (((s & CSTS_RDY) != 0) == (want != 0))
            return NVME_OK;
        if (i >= c->timeout_ms) return NVME_ETIMEOUT; /* GUARD:nvme-ready-bound */
        c->ops.delay_us(c->ops.ctx, 1000);
    }
}

/* ---- queue pair submission and polled completion ---- */

static void qpair_setup(nvme_ctrl *c, nvme_qpair *q, uint16_t sqid, uint16_t depth, uint8_t *sq,
                        uint64_t sq_phys, uint8_t *cq, uint64_t cq_phys)
{
    q->tail = 0;
    q->head = 0;
    q->depth = depth;
    q->sqid = sqid;
    q->phase = 1;
    q->sq = sq;
    q->cq = cq;
    q->sq_phys = sq_phys;
    q->cq_phys = cq_phys;
    q->sq_db = NVME_REG_DOORBELL + (2u * sqid) * c->dstrd_stride;
    q->cq_db = NVME_REG_DOORBELL + (2u * sqid + 1u) * c->dstrd_stride;
}

/* Admin queue: submit one command (16 dwords; CID is filled in) and poll for
 * its completion. Returns NVME_OK, NVME_ESTATUS (queue still usable), or a
 * fail-stop error. */
static int submit(nvme_ctrl *c, nvme_qpair *q, uint32_t *sqe, uint32_t *result)
{
    if (c->failed)
        return NVME_ESTATE;
    uint16_t cid = c->next_cid++;
    sqe[0] = (sqe[0] & 0xffffu) | ((uint32_t)cid << 16);
    volatile uint32_t *slot = (volatile uint32_t *)(q->sq + (uint64_t)q->tail * SQE_BYTES);
    for (int i = 0; i < 16; i++)
        slot[i] = sqe[i];
    barrier(c); /* command and data visible before the doorbell */
    q->tail = (uint16_t)((q->tail + 1u) % q->depth);
    wr32(c, q->sq_db, q->tail);

    volatile uint32_t *cqe = (volatile uint32_t *)(q->cq + (uint64_t)q->head * CQE_BYTES);
    uint64_t budget = (uint64_t)c->cmd_timeout_ms * (1000u / POLL_STEP_US);
    uint32_t dw3;
    for (uint64_t i = 0;; i++) {
        barrier(c);
        dw3 = cqe[3];
        if (((dw3 >> 16) & 1u) == q->phase)
            break;
        if (rd32(c, NVME_REG_CSTS) & CSTS_CFS) return fail(c, NVME_ECFS); /* GUARD:nvme-cfs-cmd */
        if (i >= budget) return fail(c, NVME_ETIMEOUT); /* GUARD:nvme-cmd-bound */
        c->ops.delay_us(c->ops.ctx, POLL_STEP_US);
    }
    barrier(c); /* completion seen before reading its fields or data */
    uint32_t dw0 = cqe[0];
    uint32_t dw2 = cqe[2];
    q->head = (uint16_t)(q->head + 1u);
    if (q->head == q->depth) {
        q->head = 0;
        q->phase ^= 1u;
    }
    wr32(c, q->cq_db, q->head);
    if ((dw3 & 0xffffu) != cid) return fail(c, NVME_ECID); /* GUARD:nvme-cid */
    if ((dw2 >> 16) != q->sqid) return fail(c, NVME_ESQID); /* GUARD:nvme-sqid */
    if ((dw3 >> 17) & 0x7ffu) { /* SC (bits 24:17) and SCT (27:25) */
        c->last_err = NVME_ESTATUS;
        return NVME_ESTATUS;
    }
    if (result)
        *result = dw0;
    return NVME_OK;
}

static void sqe_clear(uint32_t *sqe)
{
    for (int i = 0; i < 16; i++)
        sqe[i] = 0;
}

static int identify(nvme_ctrl *c, uint32_t cns, uint32_t nsid)
{
    uint32_t sqe[16];
    zero_bytes(c->identify, NVME_PAGE);
    sqe_clear(sqe);
    sqe[0] = 0x06;
    sqe[1] = nsid;
    sqe[6] = (uint32_t)c->identify_phys;
    sqe[7] = (uint32_t)(c->identify_phys >> 32);
    sqe[10] = cns;
    return submit(c, &c->admin, sqe, 0);
}

/* ---- init ---- */

int nvme_init(nvme_ctrl *c, const nvme_ops *ops, const nvme_dma *dma, const nvme_config *cfg)
{
    if (!c || !ops || !dma || !ops->read32 || !ops->write32 || !ops->barrier || !ops->delay_us)
        return NVME_EARG;
    zero_bytes((uint8_t *)c, sizeof *c);
    /* field by field: a struct copy may become a memcpy call (e.g. -mstrict-align) */
    c->ops.ctx = ops->ctx;
    c->ops.read32 = ops->read32;
    c->ops.write32 = ops->write32;
    c->ops.read64 = ops->read64;
    c->ops.write64 = ops->write64;
    c->ops.barrier = ops->barrier;
    c->ops.delay_us = ops->delay_us;
    c->ops.dma_map = ops->dma_map;
    c->last_err = NVME_EARG;
    uint32_t want_nsid = cfg ? cfg->nsid : 0;
    int depth_given = cfg && cfg->io_depth;
    uint16_t io_depth = depth_given ? cfg->io_depth : NVME_IO_DEPTH_DEFAULT;
    if (io_depth < 2 || io_depth > NVME_IO_DEPTH_MAX)
        return NVME_EARG;
    if (!dma->virt || ((uintptr_t)dma->virt & (NVME_PAGE - 1)) || (dma->phys & (NVME_PAGE - 1)) || dma->bytes < NVME_DMA_MIN_BYTES)
        return NVME_EARG;
    if (dma->phys + dma->bytes < dma->phys) /* bus address wraps */
        return NVME_EARG;
    /* The default depth shrinks to fit a small region (one PRP page per
     * slot plus one bounce page); a caller depth that does not fit is refused. */
    uint64_t pages = dma->bytes / NVME_PAGE;
    if (!depth_given && pages < NVME_QUEUE_PAGES + (uint64_t)io_depth)
        io_depth = (uint16_t)(pages - NVME_QUEUE_PAGES);
    c->dma.virt = dma->virt;
    c->dma.phys = dma->phys;
    c->dma.bytes = dma->bytes;

    c->cap = rd64(c, NVME_REG_CAP);
    c->vs = rd32(c, NVME_REG_VS);
    uint32_t major = c->vs >> 16;
    if (major < 1 || major > 2) return c->last_err = NVME_EVS; /* GUARD:nvme-vs */
    /* MPSMIN == 0 means 4 KiB is the minimum page; MPSMAX >= 0 always holds,
     * so 4 KiB is then within [MPSMIN, MPSMAX]. */
    if ((c->cap >> 48) & 0xfu) return c->last_err = NVME_EMPS; /* GUARD:nvme-mpsmin */
    uint32_t mqes1 = (uint32_t)(c->cap & 0xffffu) + 1u;
    if (NVME_ADMIN_DEPTH > mqes1) return c->last_err = NVME_EMQES; /* GUARD:nvme-mqes-admin */
    if (!depth_given && io_depth > mqes1)
        io_depth = (uint16_t)mqes1; /* the default never asks for more than MQES allows */
    if (io_depth > mqes1) return c->last_err = NVME_EMQES; /* GUARD:nvme-mqes-io */
    if (!((c->cap >> 37) & 1u)) return c->last_err = NVME_ECSS; /* GUARD:nvme-css */
    /* In flight at most depth - 1: a full queue holds one less entry than
     * its size (NVMe 1.4 section 4.1.2), and the CQ is the same size. */
    uint32_t slots = NVME_SLOTS_MAX;
    if (slots > io_depth - 1u) slots = io_depth - 1u; /* GUARD:nvme-sq-full */
    uint64_t fixed = (uint64_t)(NVME_QUEUE_PAGES + slots) * NVME_PAGE;
    if (dma->bytes < fixed + NVME_PAGE)
        return c->last_err = NVME_EARG;
    uint32_t to = (uint32_t)((c->cap >> 24) & 0xffu);
    c->timeout_ms = (to ? to : 1u) * 500u;
    c->cmd_timeout_ms = c->timeout_ms > 1000u ? c->timeout_ms : 1000u;
    c->dstrd_stride = 4u << ((c->cap >> 32) & 0xfu);

    /* Reset: clear EN, wait for RDY = 0. */
    uint32_t cc = rd32(c, NVME_REG_CC);
    wr32(c, NVME_REG_CC, cc & ~CC_EN);
    int rc = wait_ready(c, 0);
    if (rc)
        return c->last_err = rc;

    /* Carve the DMA region. */
    uint8_t *v = dma->virt;
    uint64_t p = dma->phys;
    uint8_t *asq = v, *acq = v + NVME_PAGE, *iosq = v + 3 * NVME_PAGE, *iocq = v + 4 * NVME_PAGE;
    c->identify = v + 2 * NVME_PAGE;
    c->identify_phys = p + 2 * NVME_PAGE;
    c->slots = slots;
    for (uint32_t i = 0; i < slots; i++) {
        uint64_t off = (uint64_t)(NVME_QUEUE_PAGES + i) * NVME_PAGE;
        c->slot[i].prp = (uint64_t *)(void *)(v + off);
        c->slot[i].prp_phys = p + off;
    }
    c->bounce = v + fixed;
    c->bounce_phys = p + fixed;
    c->bounce_bytes = (dma->bytes - fixed) & ~(uint64_t)(NVME_PAGE - 1);
    zero_bytes(v, fixed);
    barrier(c);

    qpair_setup(c, &c->admin, 0, NVME_ADMIN_DEPTH, asq, p, acq, p + NVME_PAGE);
    wr32(c, NVME_REG_AQA, ((NVME_ADMIN_DEPTH - 1u) << 16) | (NVME_ADMIN_DEPTH - 1u));
    wr64(c, NVME_REG_ASQ, c->admin.sq_phys);
    wr64(c, NVME_REG_ACQ, c->admin.cq_phys);
    /* CSS = 0 (NVM), MPS = 0 (4 KiB), AMS = 0 (round robin, mandatory). */
    wr32(c, NVME_REG_CC, CC_IOSQES_64 | CC_IOCQES_16 | CC_EN);
    rc = wait_ready(c, 1);
    if (rc)
        return fail(c, rc);
    c->next_cid = 1;

    /* Identify Controller (CNS 01h). */
    if ((rc = identify(c, 1, 0)) != NVME_OK)
        return fail(c, rc);
    const uint8_t *id = c->identify;
    barrier(c);
    uint8_t sqes = id[512], cqes = id[513];
    if ((sqes & 0xf) > 6 || (sqes >> 4) < 6) return fail(c, NVME_EQES); /* GUARD:nvme-sqes */
    if ((cqes & 0xf) > 4 || (cqes >> 4) < 4) return fail(c, NVME_EQES); /* GUARD:nvme-cqes */
    c->mdts = id[77];
    c->nn = ld32(id + 516);
    c->vwc = id[525] & 1u;
    c->awupf_raw = ld16(id + 528);

    /* Namespace: caller's, or the first active one (CNS 02h). */
    if (want_nsid == 0) {
        if ((rc = identify(c, 2, 0)) != NVME_OK)
            return fail(c, rc == NVME_ESTATUS ? NVME_ENSID : rc);
        barrier(c);
        want_nsid = ld32(c->identify);
    }
    if (want_nsid == 0 || want_nsid > c->nn) return fail(c, NVME_ENSID); /* GUARD:nvme-nsid */
    c->nsid = want_nsid;

    /* Identify Namespace (CNS 00h). */
    if ((rc = identify(c, 0, c->nsid)) != NVME_OK)
        return fail(c, rc == NVME_ESTATUS ? NVME_ENSID : rc);
    barrier(c);
    uint64_t nsze = ld64(id);
    uint8_t nlbaf = id[25], flbas = id[26];
    if (nsze == 0) return fail(c, NVME_ENSZE); /* GUARD:nvme-nsze */
    if ((flbas & 0x60) || (flbas & 0xf) > nlbaf) return fail(c, NVME_EFLBAS); /* GUARD:nvme-flbas */
    uint32_t lbaf = ld32(id + 128 + (flbas & 0xfu) * 4u);
    uint32_t lbads = (lbaf >> 16) & 0xffu;
    if (lbaf & 0xffffu) return fail(c, NVME_EMETA); /* GUARD:nvme-meta */
    if (lbads != 9 && lbads != 12) return fail(c, NVME_ELBADS); /* GUARD:nvme-lbads */
    c->block_size = 1u << lbads;
    c->block_count = nsze;
    c->nawupf_raw = ld16(id + 36);
    c->nabo_blocks = ld16(id + 42);
    c->nabspf_raw = ld16(id + 44);

    /* Transfer limits: direct commands up to MDTS (2 MiB cap), bounce
     * commands also up to one bounce slot (page multiple, so every slot
     * starts page aligned). */
    uint64_t lim = NVME_MAX_XFER;
    if (c->mdts && c->mdts < 20 && ((uint64_t)NVME_PAGE << c->mdts) < lim)
        lim = (uint64_t)NVME_PAGE << c->mdts;
    lim = lim / c->block_size * c->block_size;
    if (lim < c->block_size)
        return fail(c, NVME_EARG);
    c->max_xfer_bytes = (uint32_t)lim;
    uint64_t bl = lim < NVME_BOUNCE_SLOT_MAX ? lim : NVME_BOUNCE_SLOT_MAX;
    if (c->bounce_bytes < bl)
        bl = c->bounce_bytes;
    bl &= ~(uint64_t)(NVME_PAGE - 1);
    c->bounce_slot_bytes = (uint32_t)bl;
    uint64_t nb = c->bounce_bytes / bl;
    c->nbounce = nb < slots ? (uint32_t)nb : slots;
    for (uint32_t i = 0; i < c->nbounce; i++) {
        c->slot[i].bnc = c->bounce + (uint64_t)i * bl;
        c->slot[i].bnc_phys = c->bounce_phys + (uint64_t)i * bl;
    }

    /* I/O queue pair, QID 1: completion queue first (polled: PC=1, IEN=0). */
    qpair_setup(c, &c->io, 1, io_depth, iosq, p + 3 * NVME_PAGE, iocq, p + 4 * NVME_PAGE);
    uint32_t sqe[16];
    sqe_clear(sqe);
    sqe[0] = 0x05;
    sqe[6] = (uint32_t)c->io.cq_phys;
    sqe[7] = (uint32_t)(c->io.cq_phys >> 32);
    sqe[10] = 1u | ((uint32_t)(io_depth - 1u) << 16);
    sqe[11] = 1u;
    if ((rc = submit(c, &c->admin, sqe, 0)) != NVME_OK)
        return fail(c, rc);
    sqe_clear(sqe);
    sqe[0] = 0x01;
    sqe[6] = (uint32_t)c->io.sq_phys;
    sqe[7] = (uint32_t)(c->io.sq_phys >> 32);
    sqe[10] = 1u | ((uint32_t)(io_depth - 1u) << 16);
    sqe[11] = 1u | (1u << 16);
    if ((rc = submit(c, &c->admin, sqe, 0)) != NVME_OK)
        return fail(c, rc);
    c->ready = 1;
    c->last_err = NVME_OK;
    return NVME_OK;
}

int nvme_disable(nvme_ctrl *c)
{
    if (!c || !c->ops.read32)
        return NVME_EARG;
    c->ready = 0;
    wr32(c, NVME_REG_CC, rd32(c, NVME_REG_CC) & ~CC_EN);
    int rc = wait_ready(c, 0);
    release_slots(c); /* the Controller Reset dropped whatever was in flight */
    return rc;
}

/* ---- PRP construction (port of Rust build_prps) ---- */

int nvme_build_prps(uint64_t addr, uint64_t len, uint64_t page_size, uint64_t list_addr,
                    uint64_t *list, uint32_t list_cap, uint64_t *prp1, uint64_t *prp2,
                    uint32_t *used)
{
    if (!prp1 || !prp2 || !used)
        return NVME_EARG;
    if (len == 0 || page_size < 8 || (page_size & (page_size - 1)))
        return NVME_EARG;
    uint64_t off = addr & (page_size - 1);
    uint64_t covered = off + len;
    if (covered < off) return NVME_EARG; /* GUARD:prp-len-wrap */
    uint64_t pages = covered / page_size + ((covered % page_size) != 0);
    *prp1 = addr;
    *prp2 = 0;
    *used = 0;
    if (pages == 1)
        return NVME_OK;
    uint64_t second = (addr & ~(page_size - 1)) + page_size;
    if (second < addr) return NVME_EARG; /* GUARD:prp-addr-wrap */
    if (pages == 2) {
        *prp2 = second;
        return NVME_OK;
    }
    if (list_addr == 0 || (list_addr & (page_size - 1)) || !list)
        return NVME_EARG;
    uint64_t per = page_size / 8, count = pages - 1, data = 0, slot = 0;
    while (data < count) {
        if (slot >= list_cap) return NVME_ERANGE; /* GUARD:prp-list-cap */
        uint64_t e;
        if (slot % per == per - 1 && count - data > 1) {
            e = list_addr + (slot / per + 1) * page_size; /* chain to next list page */
            if (e < list_addr)
                return NVME_EARG;
        } else {
            e = second + data * page_size;
            if (e < second)
                return NVME_EARG;
            data++;
        }
        list[slot++] = e;
    }
    *prp2 = list_addr;
    *used = (uint32_t)slot;
    return NVME_OK;
}

/* ---- block I/O: up to c->slots commands in flight on the I/O queue ---- */

#define OPC_FLUSH 0x00u
#define OPC_WRITE 0x01u
#define OPC_READ 0x02u
#define SF_ABORTED_SQ_DELETION 0x08u /* generic status (SCT 0), SC 08h */

static int check_io(nvme_ctrl *c, uint64_t lba, uint32_t count, const void *buf)
{
    if (!c)
        return NVME_EARG;
    if (!c->ready || c->failed) return NVME_ESTATE; /* GUARD:nvme-failstop */
    if (!buf || count == 0)
        return NVME_EARG;
    if (lba >= c->block_count || count > c->block_count - lba) return NVME_ERANGE; /* GUARD:nvme-range */
    return NVME_OK;
}

/* Direct DMA only into a buffer the platform says is already device
 * visible and contiguous, at a dword aligned bus address (NVMe 1.4 Figure
 * 108: the PRP offset shall be dword aligned). */
static int direct_ok(nvme_ctrl *c, const uint8_t *buf, uint64_t len, uint64_t *bus)
{
    *bus = 0;
    if (!c->ops.dma_map || c->ops.dma_map(c->ops.ctx, buf, len, bus))
        return 0;
    if (*bus & 3u) return 0; /* GUARD:nvme-direct-align */
    if (*bus + len < *bus)
        return 0;
    return 1;
}

static int slot_free(const nvme_ctrl *c, int bounce)
{
    uint32_t n = bounce ? c->nbounce : c->slots;
    for (uint32_t i = 0; i < n; i++)
        if (!c->slot[i].busy)
            return (int)i;
    return -1;
}

/* Write one command into the next SQ entry (no doorbell). A bounce write
 * copies the caller data into the slot's bounce area first. */
static int io_place(nvme_ctrl *c, int idx, uint32_t opc, uint64_t lba, uint32_t count, uint8_t *buf,
                    uint64_t bus, int bounce, int async)
{
    nvme_slot *s = &c->slot[idx];
    uint64_t bytes = (uint64_t)count * c->block_size, prp1 = 0, prp2 = 0;
    if (opc != OPC_FLUSH) {
        uint32_t used;
        if (bounce && opc == OPC_WRITE)
            copy_bytes(s->bnc, buf, bytes);
        int rc = nvme_build_prps(bounce ? s->bnc_phys : bus, bytes, NVME_PAGE, s->prp_phys, s->prp,
                                 NVME_PAGE / 8, &prp1, &prp2, &used);
        if (rc)
            return rc;
    }
    /* CID = slot index (bits 5:0) + a sequence number, so a completion for
     * a slot's previous command cannot be taken for its current one. */
    uint16_t cid = (uint16_t)((uint32_t)idx | ((uint32_t)(c->io_seq++ & 0x3ffu) << 6));
    s->busy = 1;
    s->bounce = (uint8_t)(bounce != 0);
    s->write = (uint8_t)(opc == OPC_WRITE);
    s->async = (uint8_t)(async != 0);
    s->cid = cid;
    s->buf = buf;
    s->bytes = (uint32_t)bytes;
    volatile uint32_t *e = (volatile uint32_t *)(c->io.sq + (uint64_t)c->io.tail * SQE_BYTES);
    e[0] = opc | ((uint32_t)cid << 16);
    e[1] = c->nsid;
    e[2] = e[3] = e[4] = e[5] = 0;
    e[6] = (uint32_t)prp1;
    e[7] = (uint32_t)(prp1 >> 32);
    e[8] = (uint32_t)prp2;
    e[9] = (uint32_t)(prp2 >> 32);
    e[10] = opc == OPC_FLUSH ? 0 : (uint32_t)lba;
    e[11] = opc == OPC_FLUSH ? 0 : (uint32_t)(lba >> 32);
    e[12] = opc == OPC_FLUSH ? 0 : count - 1u; /* NLB is 0-based */
    e[13] = e[14] = e[15] = 0;
    c->io.tail = (uint16_t)((c->io.tail + 1u) % c->io.depth);
    if (++c->inflight > c->stats.max_inflight)
        c->stats.max_inflight = c->inflight;
    if (opc != OPC_FLUSH) {
        if (bounce)
            c->stats.bounce_cmds++;
        else
            c->stats.direct_cmds++;
    }
    return NVME_OK;
}

static void io_ring_sq(nvme_ctrl *c)
{
    barrier(c); /* commands and data visible before the doorbell */
    wr32(c, c->io.sq_db, c->io.tail);
    c->stats.sq_doorbells++;
}

/* Reap one I/O completion if one is posted: 1 (tag, status filled), 0, or
 * a fail-stop error. The CQ head doorbell is left to the caller. */
static int io_reap(nvme_ctrl *c, int *tag, int *status)
{
    nvme_qpair *q = &c->io;
    volatile uint32_t *cqe = (volatile uint32_t *)(q->cq + (uint64_t)q->head * CQE_BYTES);
    barrier(c);
    uint32_t dw3 = cqe[3];
    if (((dw3 >> 16) & 1u) != q->phase)
        return 0;
    barrier(c); /* completion seen before reading its fields or data */
    uint32_t dw2 = cqe[2];
    q->head = (uint16_t)(q->head + 1u);
    if (q->head == q->depth) {
        q->head = 0;
        q->phase ^= 1u;
    }
    uint16_t cid = (uint16_t)(dw3 & 0xffffu);
    uint32_t idx = cid & 0x3fu;
    if (idx >= c->slots || !c->slot[idx].busy || c->slot[idx].cid != cid) return fail(c, NVME_ECID); /* GUARD:nvme-io-cid */
    if ((dw2 >> 16) != q->sqid) return fail(c, NVME_ESQID); /* GUARD:nvme-io-sqid */
    nvme_slot *s = &c->slot[idx];
    uint32_t sf = (dw3 >> 17) & 0x7ffu; /* SC (bits 24:17) and SCT (27:25) */
    int st = sf == SF_ABORTED_SQ_DELETION ? NVME_EABORT : sf ? NVME_ESTATUS : NVME_OK;
    if (st == NVME_OK && s->bounce && !s->write && s->buf) {
        barrier(c);
        copy_bytes(s->buf, s->bnc, s->bytes);
    }
    s->busy = 0;
    c->inflight--;
    if (st)
        c->last_err = st;
    *tag = (int)idx;
    *status = st;
    return 1;
}

static void io_ring_cq(nvme_ctrl *c)
{
    wr32(c, c->io.cq_db, c->io.head);
}

/* Block until at least one completion, reap every posted one, ring the CQ
 * doorbell once. Returns the number reaped (first error in *first) or a
 * fail-stop error. The budget restarts with every call. */
static int io_wait(nvme_ctrl *c, int *first)
{
    uint64_t budget = (uint64_t)c->cmd_timeout_ms * (1000u / POLL_STEP_US);
    for (uint64_t i = 0;; i++) {
        int n = 0, tag, st, r;
        while ((r = io_reap(c, &tag, &st)) == 1) {
            n++;
            if (st && *first == NVME_OK)
                *first = st;
        }
        if (r < 0)
            return r;
        if (n) {
            io_ring_cq(c);
            return n;
        }
        if (rd32(c, NVME_REG_CSTS) & CSTS_CFS) return fail(c, NVME_ECFS); /* GUARD:nvme-io-cfs */
        if (i >= budget) return fail(c, NVME_ETIMEOUT); /* GUARD:nvme-io-bound */
        c->ops.delay_us(c->ops.ctx, POLL_STEP_US);
    }
}

/* Synchronous request: split into commands, keep every free slot busy, ring
 * once per batch, return after the last completion. After a command error
 * nothing new is issued, but what is in flight is still reaped. */
static int xfer(nvme_ctrl *c, uint32_t opc, uint64_t lba, uint32_t count, uint8_t *buf)
{
    int rc = check_io(c, lba, count, buf);
    if (rc)
        return rc;
    if (c->inflight)
        return NVME_EBUSY;
    uint64_t bus;
    int bounce = !direct_ok(c, buf, (uint64_t)count * c->block_size, &bus);
    uint32_t per = (bounce ? c->bounce_slot_bytes : c->max_xfer_bytes) / c->block_size;
    uint32_t done = 0;
    int first = NVME_OK;
    for (;;) {
        int placed = 0;
        while (first == NVME_OK && done < count) {
            int idx = slot_free(c, bounce);
            if (idx < 0)
                break;
            uint32_t n = count - done;
            if (n > per) n = per; /* GUARD:nvme-xfer */
            uint64_t off = (uint64_t)done * c->block_size;
            if ((rc = io_place(c, idx, opc, lba + done, n, buf + off, bus + off, bounce, 0)) != NVME_OK) {
                first = rc;
                break;
            }
            done += n;
            placed = 1;
        }
        if (placed)
            io_ring_sq(c);
        if (!c->inflight)
            return first;
        if ((rc = io_wait(c, &first)) < 0)
            return rc;
    }
}

int nvme_read(nvme_ctrl *c, uint64_t lba, uint32_t count, uint8_t *buf)
{
    return xfer(c, OPC_READ, lba, count, buf);
}

int nvme_write(nvme_ctrl *c, uint64_t lba, uint32_t count, const uint8_t *buf)
{
    /* never written through: the device reads it, a bounce copies from it */
    return xfer(c, OPC_WRITE, lba, count, (uint8_t *)(uintptr_t)buf);
}

int nvme_flush(nvme_ctrl *c)
{
    if (!c)
        return NVME_EARG;
    if (!c->ready || c->failed)
        return NVME_ESTATE;
    if (c->inflight)
        return NVME_EBUSY;
    int rc = io_place(c, 0, OPC_FLUSH, 0, 0, 0, 0, 0, 0), first = NVME_OK;
    if (rc)
        return rc;
    io_ring_sq(c);
    if ((rc = io_wait(c, &first)) < 0)
        return rc;
    return first;
}

/* ---- asynchronous I/O and clean stop ---- */

int nvme_submit(nvme_ctrl *c, int write, uint64_t lba, uint32_t count, uint8_t *buf)
{
    int rc = check_io(c, lba, count, buf);
    if (rc)
        return rc;
    uint64_t bytes = (uint64_t)count * c->block_size, bus;
    int bounce = !direct_ok(c, buf, bytes, &bus);
    if (bytes > (bounce ? c->bounce_slot_bytes : c->max_xfer_bytes)) return NVME_ERANGE; /* GUARD:nvme-async-xfer */
    int idx = slot_free(c, bounce);
    if (idx < 0)
        return NVME_EBUSY;
    if ((rc = io_place(c, idx, write ? OPC_WRITE : OPC_READ, lba, count, buf, bus, bounce, 1)) != NVME_OK)
        return rc;
    io_ring_sq(c);
    return idx;
}

int nvme_poll(nvme_ctrl *c, int *tag, int *status)
{
    if (!c || !tag || !status)
        return NVME_EARG;
    if (c->failed)
        return NVME_ESTATE;
    int r = io_reap(c, tag, status);
    if (r == 1)
        io_ring_cq(c);
    return r;
}

uint32_t nvme_inflight(const nvme_ctrl *c)
{
    return c ? c->inflight : 0;
}

static void q_account(nvme_quiesce_report *r, int *status, int tag, int st)
{
    if (st == NVME_OK)
        r->completed_ok++;
    else if (st == NVME_EABORT)
        r->aborted++;
    else
        r->completed_err++;
    if (status)
        status[tag] = st;
}

static int delete_queue(nvme_ctrl *c, uint32_t opc)
{
    uint32_t sqe[16];
    sqe_clear(sqe);
    sqe[0] = opc;
    sqe[10] = c->io.sqid;
    return submit(c, &c->admin, sqe, 0);
}

int nvme_quiesce(nvme_ctrl *c, uint32_t drain_us, nvme_quiesce_report *r, int *status)
{
    if (!c || !r)
        return NVME_EARG;
    zero_bytes((uint8_t *)r, sizeof *r);
    if (!c->ready || c->failed)
        return NVME_ESTATE;
    /* 1. No new commands; let the outstanding ones complete. */
    c->ready = 0;
    c->quiesced = 1;
    r->inflight_at_start = c->inflight;
    int tag, st, got;
    for (;;) {
        int n = 0;
        while ((got = io_reap(c, &tag, &st)) == 1) {
            q_account(r, status, tag, st);
            n++;
        }
        if (got < 0)
            return got;
        if (n)
            io_ring_cq(c);
        if (!c->inflight || r->drained_us >= drain_us)
            break;
        if (rd32(c, NVME_REG_CSTS) & CSTS_CFS)
            return fail(c, NVME_ECFS);
        c->ops.delay_us(c->ops.ctx, POLL_STEP_US);
        r->drained_us += POLL_STEP_US;
    }
    /* 2. Delete the I/O SQ: what is left is completed or aborted before the
     * delete completes (NVMe 1.4 section 5.6). */
    int rc = delete_queue(c, 0x00u);
    if (rc) return fail(c, rc); /* GUARD:nvme-quiesce-sq */
    r->sq_deleted = 1;
    int n = 0;
    while ((got = io_reap(c, &tag, &st)) == 1) {
        q_account(r, status, tag, st);
        n++;
    }
    if (got < 0)
        return got;
    if (n)
        io_ring_cq(c);
    /* A command with no completion entry was completed implicitly. */
    for (uint32_t i = 0; i < c->slots; i++)
        if (c->slot[i].busy)
            q_account(r, status, (int)i, NVME_EABORT);
    release_slots(c);
    /* 3. Delete the I/O CQ. */
    if ((rc = delete_queue(c, 0x04u)) != NVME_OK)
        return fail(c, rc);
    r->cq_deleted = 1;
    return NVME_OK;
}

/* ---- disk_dev backend ---- */

static int to_disk(int rc)
{
    switch (rc) {
    case NVME_OK: return DISK_OK;
    case NVME_ERANGE: return DISK_ERANGE;
    case NVME_EARG: return DISK_EARG;
    case NVME_ESTATE: return DISK_ESTATE;
    case NVME_EBUSY: return DISK_ESTATE;
    default: return DISK_EIO;
    }
}

static int d_read(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf)
{
    return to_disk(nvme_read((nvme_ctrl *)ctx, lba, count, buf));
}

static int d_write(void *ctx, uint64_t lba, uint32_t count, const uint8_t *buf)
{
    return to_disk(nvme_write((nvme_ctrl *)ctx, lba, count, buf));
}

static int d_flush(void *ctx)
{
    return to_disk(nvme_flush((nvme_ctrl *)ctx));
}

int nvme_disk(nvme_ctrl *c, disk_dev *out)
{
    if (!c || !out)
        return DISK_EARG;
    if (!c->ready || c->failed)
        return DISK_ESTATE;
    out->ctx = c;
    out->block_size = c->block_size;
    out->block_count = c->block_count;
    out->read = d_read;
    out->write = d_write;
    out->flush = d_flush;
    out->max_blocks_per_io = NVME_REQUEST_MAX_BYTES / c->block_size; /* split and pipelined in xfer() */
    return disk_check(out);
}

/* ---- power-fail atomicity (port of atomicity.rs) ---- */

int nvme_write_is_power_fail_atomic(const nvme_ctrl *c, uint64_t start_lba, uint64_t lba_count)
{
    if (!c || !c->ready)
        return NVME_ATOMIC_UNKNOWN;
    if (lba_count == 0)
        return NVME_NOT_ATOMIC;
    uint64_t unit = c->nawupf_raw ? (uint64_t)c->nawupf_raw + 1 : (uint64_t)c->awupf_raw + 1;
    if (lba_count > unit) return NVME_NOT_ATOMIC; /* GUARD:atomic-unit */
    if (c->nabspf_raw) {
        uint64_t size = (uint64_t)c->nabspf_raw + 1, off = c->nabo_blocks % size;
        uint64_t end = start_lba + lba_count - 1;
        if (end < start_lba)
            return NVME_NOT_ATOMIC;
        /* region index shifted by one so blocks before the offset are region 0 */
        uint64_t r0 = start_lba < off ? 0 : (start_lba - off) / size + 1;
        uint64_t r1 = end < off ? 0 : (end - off) / size + 1;
        if (r0 != r1) return NVME_NOT_ATOMIC; /* GUARD:atomic-boundary */
    }
    return NVME_ATOMIC;
}

int nvme_store_root_is_power_fail_atomic(const nvme_ctrl *c, uint64_t start_lba, uint32_t *required_blocks)
{
    if (required_blocks)
        *required_blocks = 0;
    if (!c || !c->ready || !c->block_size || NVME_STORE_ROOT_UNIT_BYTES % c->block_size)
        return NVME_ATOMIC_UNKNOWN;
    /* The whole unit is written, but only the blocks holding the live bytes
     * must survive a power cut whole (see NVME_STORE_ROOT_LIVE_BYTES). */
    uint32_t need = NVME_STORE_ROOT_UNIT_BYTES / c->block_size;
    need = (NVME_STORE_ROOT_LIVE_BYTES + c->block_size - 1u) / c->block_size; /* GUARD:root-live */
    if (required_blocks)
        *required_blocks = need;
    return nvme_write_is_power_fail_atomic(c, start_lba, need);
}
