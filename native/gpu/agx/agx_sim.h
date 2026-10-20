/*
 * Simulated Apple GPU (AGX G14, firmware 13.5 interface) for AienOS.
 *
 * A host-side model that accepts a compute job the way the real GPU does and
 * executes it on the CPU by interpreting Apple's shader machine code:
 *
 *   RunWorkQueueMsg -> QueueInfo -> RingState -> ring[] -> RunCompute
 *   -> microsequence (StartCompute ... FinalizeCompute, RetireStamp)
 *   -> JobParameters1/2 -> CDM control stream -> USC word list
 *   -> uniforms, shared memory size, register count, shader code
 *   -> threadgroups of 32-lane SIMD groups running the ISA subset
 *   -> stamps, ring pointers and a completion event.
 *
 * Every structure is read from simulated physical memory through the UAT
 * (agx_uat.h): firmware structures through context 0 with firmware rights,
 * the command stream, USC words, uniforms, code and buffers through the job's
 * VM slot with GPU rights. So the driver code that builds a job for this model
 * (agx_job.c) writes exactly the bytes it would write for the real chip.
 *
 * Not modelled (the firmware is a black box here, see
 * docs/AGX_FIRMWARE_INTERFACE.md): firmware boot and InitData, the RTKit
 * mailbox and channel rings (the message is handed over directly), power
 * states, TLB maintenance, preemption, timestamps, events beyond a log.
 *
 * Semantics of the ISA subset: control flow per applegpu's pseudocode
 * (BSD-3, dougallj); operand encodings per Mesa AGX2.xml (MIT); the rest is
 * OBSERVED or INFERRED as noted in agx_sim.c. Anything outside the subset, or
 * an encoding whose meaning is UNKNOWN, is refused with a fault rather than
 * guessed.
 *
 * Freestanding: no heap, no clock, no I/O. struct agx_sim is large (about
 * 1.2 MiB); allocate it statically or on the heap, not on the stack.
 */
#ifndef AIENOS_AGX_SIM_H
#define AIENOS_AGX_SIM_H

#include <stdint.h>

#include "agx_fw.h"
#include "agx_isa.h"
#include "agx_uat.h"

#define AGX_SIM_SIMD 32
#define AGX_SIM_MAX_TG_THREADS 1024
#define AGX_SIM_MAX_SG (AGX_SIM_MAX_TG_THREADS / AGX_SIM_SIMD)
#define AGX_SIM_GPR_HALVES 512     /* 256 32-bit GPRs, the USC Registers maximum */
#define AGX_SIM_UNIFORM_HALVES 512 /* CDM launch uniform count maximum */
#define AGX_SIM_SHARED_MAX 65536   /* USC Shared maximum (256 groups of 256 bytes) */
#define AGX_SIM_MICROSEQ_MAX 0x400
#define AGX_SIM_EVENTS 64
#define AGX_SIM_DEFAULT_STEP_LIMIT (1ull << 24)

enum agx_sim_fault_kind {
	AGX_SIM_FAULT_NONE = 0,
	AGX_SIM_FAULT_MMU,         /* translation or permission failure (va, slot, access, uat_err) */
	AGX_SIM_FAULT_DECODE,      /* shader bytes do not decode */
	AGX_SIM_FAULT_UNSUPPORTED, /* outside the subset, or UNKNOWN meaning */
	AGX_SIM_FAULT_BOUNDS,      /* register, uniform or shared memory index past its allocation */
	AGX_SIM_FAULT_WATCHDOG,    /* step limit reached */
	AGX_SIM_FAULT_COMMAND,     /* malformed queue, command, microsequence, CDM or USC words */
};

struct agx_sim_fault {
	int kind;          /* enum agx_sim_fault_kind */
	int uat_err;       /* MMU: enum agx_uat_err */
	unsigned slot;     /* MMU: context */
	unsigned access;   /* MMU: enum agx_access */
	uint64_t va;       /* MMU: address; shader faults: code VA of the instruction */
	uint64_t pc;       /* shader faults: byte offset of the instruction */
	int op;            /* shader faults: agx_ins_table index, or -1 */
	const char *what;  /* static description */
};

struct agx_sim_event {
	uint32_t slot;  /* RunWorkQueueMsg event_slot */
	uint32_t value; /* stamp value written */
	uint32_t error; /* 1: the job faulted, no stamp written */
};

struct agx_sim_sg {
	uint16_t r[AGX_SIM_SIMD][AGX_SIM_GPR_HALVES];
	uint16_t lid[AGX_SIM_SIMD][3];
	uint16_t tid[AGX_SIM_SIMD]; /* thread index in threadgroup */
	uint32_t alive;             /* lanes that exist */
	uint32_t exec;              /* execution mask; control flow recomputes it from r0l */
	uint64_t pc;
	uint8_t state;
};

struct agx_sim {
	struct agx_mem mem;
	uint64_t ttbat_pa;
	uint64_t step_limit; /* SIMD-group instructions per job */

	/* Statistics. */
	uint64_t jobs, jobs_faulted, threadgroups, simdgroups, instructions;
	uint32_t op_hits[256]; /* per agx_ins_table entry */

	struct agx_sim_fault fault; /* first fault of the most recent faulted job */
	struct agx_sim_event events[AGX_SIM_EVENTS];
	uint32_t nevents; /* total; events[i % AGX_SIM_EVENTS] */

	/* Per-job state. */
	unsigned slot;
	uint64_t code_va;
	uint32_t gpr_limit, uni_limit, shared_size;
	uint32_t grid[3], local[3], tg[3];
	uint64_t steps;
	int faulted;
	uint16_t uni[AGX_SIM_UNIFORM_HALVES];
	uint8_t shared[AGX_SIM_SHARED_MAX];
	uint8_t kind[256]; /* agx_ins_table index -> internal kind */
	struct agx_sim_sg sg[AGX_SIM_MAX_SG];
};

/* mem: the simulated DRAM; ttbat_pa: the TTBAT (64 x 16 bytes) inside it. */
int agx_sim_init(struct agx_sim *s, const struct agx_mem *mem, uint64_t ttbat_pa);

/* The firmware's handling of one RunWorkQueueMsg (AGX_RWQ_SIZE bytes): run
 * every command from the queue's gpu_rptr up to its cpu_wptr. Returns the
 * number of commands retired (faulted ones included), or -1 when the queue
 * itself is unreadable (s->fault says why). */
int agx_sim_run_work_queue(struct agx_sim *s, const uint8_t *msg);

const char *agx_sim_fault_name(int kind);

#endif
