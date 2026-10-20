/*
 * disk_nvme.h -- AIENOS freestanding NVMe driver (C, no libc, no heap).
 *
 * Ported from the audited Rust path (crates/aienos-kernel/src/nvme.rs,
 * nvme/driver.rs, nvme/atomicity.rs; docs/P3_NVME_*.md). Polled, one admin
 * queue pair and one I/O queue pair, no interrupts. The I/O queue keeps up to
 * io_depth - 1 commands in flight (NVMe 1.4 section 4.1.2: a queue is full
 * when it holds one less entry than its size).
 *
 * The caller provides:
 *   - nvme_ops: 32-bit MMIO access to BAR0 (64-bit optional), a full memory
 *     barrier, a bounded delay in microseconds, and optionally dma_map, which
 *     says whether a caller buffer is already device visible (see below).
 *   - nvme_dma: ONE physically contiguous, 4 KiB aligned region with its CPU
 *     pointer and its bus (physical) address. The driver carves it into
 *     NVME_QUEUE_PAGES fixed pages (admin SQ, admin CQ, identify buffer, I/O
 *     SQ, I/O CQ), one PRP list page per in-flight slot, and a bounce area
 *     for the rest. NVME_DMA_BYTES_FOR(depth, bounce) gives the size. The
 *     region must be coherent with the device (or the barrier op must make it
 *     so).
 *
 * Data path. A request whose buffer dma_map accepts (the whole range is
 * device visible, for example inside the SMMU window the kernel granted,
 * and contiguous at a dword aligned bus address: NVMe 1.4 Figure 108) is
 * transferred directly into or out of the caller buffer, split into commands
 * of at most max_xfer_bytes. Any other buffer (dma_map NULL or refusing,
 * odd alignment) goes through the bounce area in commands of at most
 * bounce_slot_bytes. Both paths keep up to `slots` commands in flight and
 * ring the SQ doorbell once per batch. The synchronous calls (nvme_read,
 * nvme_write, nvme_flush) return only when every command they issued has
 * completed. nvme_submit / nvme_poll are the asynchronous form; nvme_quiesce
 * ends them cleanly (NVMe 1.4 section 7.6.2: let outstanding commands
 * complete, delete the I/O SQ, which aborts what is left, then the I/O CQ).
 *
 * Checks performed by nvme_init (each refusal returns its own NVME_E* code;
 * docs/P3_NVME_UNSAFE_ASSUMPTIONS.md items marked [UA]):
 *   [UA] CAP.MPSMIN (bits 51:48) must be 0: the driver uses 4 KiB pages only
 *        (CC.MPS = 0). CAP.MPSMAX (bits 55:52) must be >= MPSMIN.  NVME_EMPS
 *   [UA] CAP.MQES (bits 15:0, 0-based): admin depth NVME_ADMIN_DEPTH and the
 *        configured I/O depth must each be <= MQES + 1.         NVME_EMQES
 *   [UA] CAP.CSS bit 37 (NVM command set) must be set; CC.CSS = 0.  NVME_ECSS
 *   [UA] CAP.AMS: documented limit, not a refusal. Round robin (CC.AMS = 0) is
 *        mandatory for every controller (NVMe base spec, CC.AMS); CAP.AMS
 *        bits 18:17 only advertise the optional WRR and vendor schemes. The
 *        audit text reading "bit 0" as round robin is wrong; the driver always
 *        programs CC.AMS = 0 (asserted by the test model).
 *   [UA] CAP.TO (bits 31:24, 500 ms units; 0 treated as 500 ms): readiness
 *        waits poll CSTS every 1 ms and give up after exactly TO*500 ms of
 *        delay (no extra delay after the final read).     NVME_ETIMEOUT
 *        CSTS.CFS seen during any wait or command poll.        NVME_ECFS
 *   [UA] VS (0x08): major version must be 1 or 2.               NVME_EVS
 *   [UA] NSID: never hard-coded. cfg.nsid == 0 picks the first active NSID
 *        from Identify CNS=02h; a caller NSID must be in 1..NN (Identify
 *        Controller NN) and identify with NSZE != 0.          NVME_ENSID
 *   [UA] LBADS: only 9 (512 B) or 12 (4096 B) accepted (disk_dev contract;
 *        10, 11 and 13..31 are valid NVMe but unsupported here).  NVME_ELBADS
 *        LBA format metadata size must be 0.                   NVME_EMETA
 *        FLBAS index must be <= NLBAF.                         NVME_EFLBAS
 *        NSZE must be non-zero.                                NVME_ENSZE
 *   [UA] Completion SQID must equal the queue the command went to (0 admin,
 *        1 I/O) and CID must name a command in flight. NVME_ESQID / NVME_ECID
 *        Identify Controller SQES/CQES must allow 64 B / 16 B entries. NVME_EQES
 *        DMA region: non-NULL, 4 KiB aligned phys, size >=
 *        NVME_DMA_BYTES_FOR(io_depth, one page), no address wrap.  NVME_EARG
 *
 * Limits (documented, not checks):
 *   - Per command: direct min(2 MiB, 4 KiB << MDTS) (MDTS 0 = no device
 *     limit), bounce min(that, 128 KiB, bounce area). 2 MiB needs at most 512
 *     PRP list entries, so one list page per slot is enough; nvme_build_prps
 *     still implements list chaining (ported from Rust build_prps) and is
 *     tested. One request may be any length inside the namespace; disk_dev
 *     callers see max_blocks_per_io = 256 MiB / block_size.
 *   - Command poll budget: max(1000 ms, CAP.TO*500 ms) in 10 us steps, reset
 *     by each completion.
 *   - Any timeout, CFS, CID/SQID mismatch or failed queue creation puts the
 *     controller in the failed state; with commands still in flight the
 *     driver then clears CC.EN (a Controller Reset deletes the I/O queues and
 *     brings the hardware to an idle state, NVMe 1.4 CC.EN) so no DMA lands in
 *     a caller buffer after the call returns. Every later call returns
 *     NVME_ESTATE until nvme_init runs again (fail-stop). A command that
 *     completes with a non-zero status returns NVME_ESTATUS and the queue
 *     stays usable; the other commands of that request are still reaped
 *     before the call returns.
 *   - Doorbell offsets use CAP.DSTRD (4 << DSTRD stride).
 */
#ifndef AIENOS_DISK_NVME_H
#define AIENOS_DISK_NVME_H

#include <stdint.h>
#include "disk.h"

#define NVME_PAGE 4096u
#define NVME_ADMIN_DEPTH 8u
#define NVME_IO_DEPTH_DEFAULT 32u /* QEMU saturation, evidence/nvme_queue_depth_qemu_2026-10-20.md */
#define NVME_IO_DEPTH_MAX 64u     /* 64 entries x 64 B = one 4 KiB page */
#define NVME_SLOTS_MAX (NVME_IO_DEPTH_MAX - 1u)
#define NVME_MAX_XFER (2u * 1024u * 1024u) /* one PRP list page per command */
#define NVME_BOUNCE_SLOT_MAX (128u * 1024u)
#define NVME_REQUEST_MAX_BYTES (256u * 1024u * 1024u) /* disk_dev max_blocks_per_io */
#define NVME_QUEUE_PAGES 5u /* asq, acq, identify, iosq, iocq */
#define NVME_FIXED_PAGES 6u /* queue pages + one PRP list page (io_depth 2) */
#define NVME_DMA_MIN_BYTES ((NVME_FIXED_PAGES + 1u) * NVME_PAGE)
/* Region for io_depth (2..NVME_IO_DEPTH_MAX) with bounce_bytes of bounce area. */
#define NVME_DMA_BYTES_FOR(depth, bounce_bytes) \
    ((NVME_QUEUE_PAGES + (uint64_t)(depth) - 1u) * NVME_PAGE + (uint64_t)(bounce_bytes))

/* Store v1 superblock: bytes 0..171 carry every non-zero byte, 172..4095 are
 * frozen zero and covered by the slot CRC (ADR 0015; native/store/store_v1.h
 * SV1_SB_RESERVED_OFFSET). A root write therefore needs only the blocks that
 * hold bytes 0..171 to be power-fail atomic: a tear elsewhere leaves a
 * non-zero tail only on top of a slot that was already invalid, and the CRC
 * refuses it. */
#define NVME_STORE_ROOT_UNIT_BYTES 4096u
#define NVME_STORE_ROOT_LIVE_BYTES 172u

/* Register offsets (NVMe base spec). */
#define NVME_REG_CAP 0x00u
#define NVME_REG_VS 0x08u
#define NVME_REG_INTMS 0x0cu
#define NVME_REG_CC 0x14u
#define NVME_REG_CSTS 0x1cu
#define NVME_REG_AQA 0x24u
#define NVME_REG_ASQ 0x28u
#define NVME_REG_ACQ 0x30u
#define NVME_REG_DOORBELL 0x1000u

enum {
    NVME_OK = 0,
    NVME_EARG = -100,     /* bad ops / dma / config */
    NVME_EMPS = -101,     /* CAP.MPSMIN != 0 or MPSMAX < MPSMIN */
    NVME_EMQES = -102,    /* queue depth above CAP.MQES + 1 */
    NVME_ECSS = -103,     /* NVM command set not supported */
    NVME_EVS = -104,      /* unsupported major version */
    NVME_ETIMEOUT = -105, /* readiness or command poll budget exhausted */
    NVME_ECFS = -106,     /* CSTS.CFS: controller fatal status */
    NVME_ESTATUS = -107,  /* completion with non-zero status */
    NVME_ECID = -108,     /* completion CID names no command in flight */
    NVME_ESQID = -109,    /* completion SQID mismatch */
    NVME_ENSID = -110,    /* no usable namespace */
    NVME_ELBADS = -111,   /* LBA data size not 512 or 4096 */
    NVME_EMETA = -112,    /* LBA format carries metadata */
    NVME_EFLBAS = -113,   /* FLBAS index above NLBAF */
    NVME_ENSZE = -114,    /* namespace size zero */
    NVME_EQES = -115,     /* SQES/CQES do not allow 64/16 byte entries */
    NVME_ERANGE = -116,   /* lba/count out of range */
    NVME_ESTATE = -117,   /* not initialized, failed (fail-stop) or quiesced */
    NVME_EBUSY = -118,    /* no free slot, or a synchronous call while async commands are in flight */
    NVME_EABORT = -119,   /* command aborted by its SQ being deleted (nvme_quiesce) */
};

typedef struct {
    void *ctx;
    uint32_t (*read32)(void *ctx, uint32_t off);
    void (*write32)(void *ctx, uint32_t off, uint32_t val);
    /* Optional: when NULL, 64-bit registers are accessed as two 32-bit
     * halves (low dword first), as the Rust driver does. */
    uint64_t (*read64)(void *ctx, uint32_t off);
    void (*write64)(void *ctx, uint32_t off, uint64_t val);
    void (*barrier)(void *ctx);                /* full memory + DMA barrier */
    void (*delay_us)(void *ctx, uint32_t us);  /* bounded busy wait */
    /* Optional: return 0 and the bus address when all of [buf, buf+len) is
     * already granted to the device (SMMU window) and contiguous in bus
     * space; anything else refuses. NULL = every request uses the bounce
     * area. The buffer must stay valid until its commands complete. */
    int (*dma_map)(void *ctx, const void *buf, uint64_t len, uint64_t *bus);
} nvme_ops;

typedef struct {
    uint8_t *virt;  /* CPU pointer */
    uint64_t phys;  /* device-visible address, 4 KiB aligned */
    uint64_t bytes; /* >= NVME_DMA_BYTES_FOR(io_depth, NVME_PAGE) */
} nvme_dma;

typedef struct {
    uint32_t nsid;     /* 0 = first active namespace */
    uint16_t io_depth; /* 0 = NVME_IO_DEPTH_DEFAULT; 2..NVME_IO_DEPTH_MAX */
} nvme_config;

typedef struct {
    uint16_t tail, head, depth, sqid;
    uint8_t phase;
    uint8_t *sq, *cq;     /* CPU pointers into the DMA region */
    uint64_t sq_phys, cq_phys;
    uint32_t sq_db, cq_db; /* doorbell register offsets */
} nvme_qpair;

/* One in-flight I/O command. */
typedef struct {
    uint8_t busy, bounce, write, async;
    uint16_t cid;
    uint8_t *buf;   /* caller buffer (a bounce read copies here on success) */
    uint32_t bytes;
    uint64_t *prp;  /* this slot's PRP list page */
    uint64_t prp_phys;
    uint8_t *bnc;   /* this slot's bounce area (slots < nbounce only) */
    uint64_t bnc_phys;
} nvme_slot;

typedef struct {
    uint64_t direct_cmds, bounce_cmds; /* data commands issued per path */
    uint64_t sq_doorbells;             /* I/O SQ tail writes */
    uint32_t max_inflight;             /* highest number in flight at once */
    uint32_t fail_resets;              /* CC.EN cleared by fail-stop with commands in flight */
} nvme_stats;

typedef struct {
    nvme_ops ops;
    nvme_dma dma;
    uint64_t cap;
    uint32_t vs;
    uint32_t timeout_ms;     /* CAP.TO * 500, min 500 */
    uint32_t cmd_timeout_ms; /* max(1000, timeout_ms) */
    uint32_t dstrd_stride;
    nvme_qpair admin, io;
    uint16_t next_cid, io_seq;
    int ready, failed, quiesced;
    int last_err; /* last NVME_E* code, for diagnostics */
    /* Identify results */
    uint8_t mdts, vwc;
    uint32_t nn, nsid;
    uint32_t block_size;
    uint64_t block_count;
    uint32_t max_xfer_bytes;    /* per direct command */
    uint32_t bounce_slot_bytes; /* per bounce command */
    uint8_t *identify, *bounce;
    uint64_t identify_phys, bounce_phys, bounce_bytes;
    /* In-flight slots: slots = io_depth - 1, nbounce of them own a bounce area. */
    nvme_slot slot[NVME_SLOTS_MAX];
    uint32_t slots, nbounce, inflight;
    nvme_stats stats;
    /* Power-fail atomicity fields (ported from nvme/atomicity.rs). */
    uint16_t awupf_raw, nawupf_raw, nabspf_raw, nabo_blocks;
} nvme_ctrl;

/* Reset, configure and enable the controller, identify it and the namespace,
 * create the I/O queue pair. Calling it again is a full reset ("reopen"). */
int nvme_init(nvme_ctrl *c, const nvme_ops *ops, const nvme_dma *dma, const nvme_config *cfg);

/* Clear CC.EN and wait for CSTS.RDY = 0 (bounded by CAP.TO). Commands still
 * in flight are dropped by the reset; their slots are released. */
int nvme_disable(nvme_ctrl *c);

/* Block I/O in logical blocks, any count inside the namespace. Returns when
 * every command of the request has completed. NVME_EBUSY while async
 * commands are in flight. */
int nvme_read(nvme_ctrl *c, uint64_t lba, uint32_t count, uint8_t *buf);
int nvme_write(nvme_ctrl *c, uint64_t lba, uint32_t count, const uint8_t *buf);
int nvme_flush(nvme_ctrl *c);

/* Asynchronous I/O: one command of at most max_xfer_bytes (direct buffer)
 * or bounce_slot_bytes (bounce). Returns the tag (>= 0) or NVME_E*. The
 * doorbell is rung at once. */
int nvme_submit(nvme_ctrl *c, int write, uint64_t lba, uint32_t count, uint8_t *buf);
/* Reap at most one completion without waiting: 1 (tag and status filled),
 * 0 (nothing posted yet), or a fail-stop NVME_E*. */
int nvme_poll(nvme_ctrl *c, int *tag, int *status);
uint32_t nvme_inflight(const nvme_ctrl *c);

/* Clean stop with commands in flight (NVMe 1.4 section 7.6.2 steps 1-3):
 * refuse new submissions, reap completions for up to drain_us, delete the
 * I/O SQ (the controller completes or aborts what is left; an entry with no
 * completion is reported as aborted), delete the I/O CQ. Afterwards no
 * command is in flight and I/O returns NVME_ESTATE until nvme_init; the
 * caller then sets CC.SHN (kernel: dev/nvme_shutdown.c) or nvme_disable.
 * status[tag] (may be NULL; NVME_SLOTS_MAX entries) gets each async
 * command's final status. */
typedef struct {
    uint32_t inflight_at_start, completed_ok, completed_err, aborted;
    uint32_t drained_us; /* time spent waiting for completions */
    int sq_deleted, cq_deleted;
} nvme_quiesce_report;
int nvme_quiesce(nvme_ctrl *c, uint32_t drain_us, nvme_quiesce_report *r, int *status);

/* Fill a disk_dev backed by an initialized controller. */
int nvme_disk(nvme_ctrl *c, disk_dev *out);

/* PRP construction (port of Rust build_prps). list holds list_cap u64
 * entries that live at bus address list_addr (page aligned, contiguous).
 * On success *used is the number of list entries written. */
int nvme_build_prps(uint64_t addr, uint64_t len, uint64_t page_size, uint64_t list_addr,
                    uint64_t *list, uint32_t list_cap, uint64_t *prp1, uint64_t *prp2,
                    uint32_t *used);

/* Power-fail atomicity (port of atomicity.rs). Returns NVME_ATOMIC,
 * NVME_NOT_ATOMIC or NVME_ATOMIC_UNKNOWN for a write of lba_count blocks. */
enum { NVME_ATOMIC = 1, NVME_NOT_ATOMIC = 0, NVME_ATOMIC_UNKNOWN = -1 };
int nvme_write_is_power_fail_atomic(const nvme_ctrl *c, uint64_t start_lba, uint64_t lba_count);

/* The same verdict for a Store v1 root (superblock) write of the 4096-byte
 * unit starting at start_lba: only the blocks holding bytes
 * 0..NVME_STORE_ROOT_LIVE_BYTES-1 must be atomic (1 block at 512 B and at
 * 4096 B). *required_blocks (may be NULL) gets that count. */
int nvme_store_root_is_power_fail_atomic(const nvme_ctrl *c, uint64_t start_lba, uint32_t *required_blocks);

#endif /* AIENOS_DISK_NVME_H */
