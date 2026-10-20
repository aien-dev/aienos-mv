/*
 * AienOS driver side of a first AGX compute job (G14, firmware 13.5): device
 * memory and GPU page tables, the compute work queue, and building one
 * RunCompute with its microsequence, CDM control stream, USC words, uniforms
 * and shader code. The bytes written here are the bytes the firmware reads on
 * the real chip; the simulated AGX (agx_sim.h) is the device they are tested
 * against first, the way the GB10 driver is written against its simulator
 * (ledger L193).
 *
 * Freestanding: no heap, no clock, no I/O. Memory comes from a bump allocator
 * over one physical window; nothing is freed (the kernel port retires job
 * memory when the stamp lands, docs/AGX_SIMULATOR.md section 6).
 *
 * Layouts: agx_fw.h. Argument convention for Metal-compiled kernels (buffer k's
 * address in uniforms 4k..4k+3 in 16-bit halves, constants after the table)
 * is INFERRED from Apple's compiler output and OBSERVED to produce the real
 * GPU's results (docs/AGX_SIMULATOR.md section 3).
 */
#ifndef AIENOS_AGX_JOB_H
#define AIENOS_AGX_JOB_H

#include <stdint.h>

#include "agx_fw.h"
#include "agx_uat.h"

enum agx_job_err {
	AGX_JOB_OK = 0,
	/* -1 .. -7: enum agx_uat_err from memory and mapping */
	AGX_JOB_E_INVAL = -20,     /* bad argument or size */
	AGX_JOB_E_RING_FULL = -21, /* every ring slot holds an unretired command */
	AGX_JOB_E_SHADER = -22,    /* code does not decode, or needs what the job lacks */
	AGX_JOB_E_SUBMIT = -23,    /* the submit hook refused the message */
};

#define AGX_JOB_MAX_UNIFORM_HALVES 512
#define AGX_JOB_RING_MAX 64

/* Sends one RunWorkQueueMsg (AGX_RWQ_SIZE bytes) to the firmware. On the chip
 * this is the RTKit channel; in tests it calls agx_sim_run_work_queue. */
typedef int (*agx_submit_fn)(void *ctx, const uint8_t *msg);

struct agx_dev {
	struct agx_mem mem;
	struct agx_pages pages;
	uint64_t ttbat_pa;
	struct agx_uat kern; /* firmware/kernel half, context 0 and every ttb1 */
	uint64_t kva_next;

	/* Compute work queue. */
	uint64_t qi_va, rs_va, ring_va, cmd_va, stamp_va, fw_stamp_va;
	uint8_t *qi, *rs, *ring, *cmd, *stamp, *fw_stamp; /* CPU views */
	uint32_t ring_size, wptr, next_stamp, event_slot;

	agx_submit_fn submit;
	void *submit_ctx;
};

struct agx_vm {
	struct agx_uat uat; /* user half (ttb0) */
	unsigned slot;      /* TTBAT context, 1..63 */
	uint64_t va_next;
};

int agx_dev_init(struct agx_dev *d, const struct agx_mem *mem, uint64_t pa_start, uint64_t pa_end,
		 uint32_t ring_size, agx_submit_fn submit, void *submit_ctx);

/* Kernel (firmware-shared) memory, mapped FW_SHARED_RW in context 0. */
int agx_kalloc(struct agx_dev *d, uint64_t size, uint64_t *va, uint8_t **cpu);

int agx_vm_create(struct agx_dev *d, struct agx_vm *vm, unsigned slot);
/* User memory in vm, size rounded up to 16 KiB, with prot (AGX_PROT_*). */
int agx_vm_alloc(struct agx_dev *d, struct agx_vm *vm, uint64_t size, uint64_t prot, uint64_t *va, uint8_t **cpu);

struct agx_shader_info {
	uint32_t gpr_halves;  /* one past the highest register half used */
	uint32_t code_bytes;  /* through the last stop */
	uint8_t uses_shared;  /* lload or lstore present */
};

/* Decode code[0, len) instruction by instruction; fails on undecodable bytes
 * or when no stop is found. */
int agx_shader_scan(const uint8_t *code, uint32_t len, struct agx_shader_info *info);

struct agx_compute {
	const uint8_t *code;
	uint32_t code_len;
	const uint64_t *buf_va;  /* buffer(k) GPU addresses, Metal argument table */
	unsigned nbuf;
	const uint32_t *consts;  /* constant-buffer values after the table, in order */
	unsigned nconst;
	uint32_t shared_bytes;   /* threadgroup memory (Metal staticThreadgroupMemoryLength) */
	uint32_t grid[3];        /* threads */
	uint32_t local[3];       /* threads per threadgroup */
};

/* Build and submit one compute job in vm. *stamp_value is what the firmware
 * writes to the queue stamp when the job completes. */
int agx_compute_submit(struct agx_dev *d, struct agx_vm *vm, const struct agx_compute *job, uint32_t *stamp_value);

/* 1 once the stamp for stamp_value has landed. */
int agx_compute_done(const struct agx_dev *d, uint32_t stamp_value);

/* Jobs the firmware reported as faulted on this queue (QueueInfo error_count). */
uint32_t agx_queue_errors(const struct agx_dev *d);

#endif
