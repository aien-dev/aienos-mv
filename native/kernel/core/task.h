/* task.h -- native pipeline task lifecycle: TASK_SPAWN / TASK_WAIT / TASK_STATE / TASK_CANCEL and pipelines of stages
 * connected by native pipes (aienos-mv#429, on top of the pipe primitive of aienos-mv#428).
 *
 * Contract: aien-protocols-mv specs/osh-platform/OSH_PLATFORM_ABI.md read at commit fb5fca98e667 (v1 DRAFT, unfrozen;
 * the file last changed in 37a215904ff2): sections 4.4 (epoch, STALE), 7.2 (commands start concurrently with pipes
 * connected left to right; pipeline status is the last command's status), 8.4 (TASK_SPAWN(resolved, argv, env,
 * stdio{0,1,2}, binding), TASK_WAIT(task, deadline), TASK_CANCEL(task, interrupt | terminate), TASK_STATE(task); spawn
 * without a valid binding is DENIED), 8.5 (8 concurrent tasks per pipeline, LIMIT never truncation), 8.6 (error codes,
 * osh_err.h), 9 (binding checked at the moment of the effect; domains never mixed), 10 (PARTIAL_LAUNCH after the
 * started commands are cancelled and reaped) and 12 (cleanup: interrupt, then terminate after a bounded grace, reap,
 * close every end, release ids). No branch feat/osh-stream-abi-43 existed in aien-protocols-mv when this was written.
 * Everything marked UNVERIFIED is this kernel's own choice where the draft names a thing but does not pin it.
 *
 * WHAT RUNS. A task is one pipeline stage: an admitted unit's step function, run as a bounded, resumable step through a
 * runner (the kernel's runner is core/task_osc.c: every step is one ck_osc_launch, so the realization receipt is
 * verified, the code re-hashed and the step entered at EL0 in its own address space, every time). The stage never
 * touches a pipe and has no system call: the kernel reads its stdin pipe into the step's read-only input window and
 * writes the step's output cells to its stdout pipe.
 *
 * HOW STAGES PROGRESS (be precise about this). One CPU, cooperative at step granularity: ck_task_run picks the next
 * READY task round robin (sched.h run queue), runs ONE step of it to completion, moves its bytes, and goes on to the
 * next. Stages therefore progress interleaved, both alive with bytes in flight between them, NOT serially to
 * completion. A step is never preempted and resumed: a step that overruns its tick budget is ended by the launcher
 * (OUTCOME_UNKNOWN TICK_OVERRUN) and the task with it. A task that would wait (empty stdin, full stdout) leaves the run
 * queue (BLOCKED) and watches the pipe end (ck_pipe_watch); the pipe's wake hook puts it back on readable, writable,
 * closed or revoked, and TASK_CANCEL wakes it directly. When no task is READY, ck_task_run returns at once: nothing is
 * polled and nothing spins.
 *
 * LEAST PRIVILEGE. Each task has its own capability table holding only its stdio ends, duplicated from the spawner's
 * table with exactly the data right (stdin READ, stdout and stderr WRITE; no DERIVE, GRANT or REVOKE), so the kernel
 * acting for a stage can do nothing else with them. The stage itself holds no handle at all (no system calls): this
 * is kernel bookkeeping that bounds what the scheduler does on the stage's behalf, not a sandbox claim beyond the EL0
 * isolation of osc_task.c.
 *
 * STAGE ABI v1 (UNVERIFIED, this kernel's own; the draft has no stage contract yet). Entry `fn X(inp: bytes, w: cells)
 * -> u64`, the shape of the OSH core entries. `inp` is the bytes read from stdin for this step (0 to in_chunk); a step
 * must consume all of it. `w` is the task's private workspace, CK_STG_CELLS cells, mapped read-write only while that
 * task's step runs; the kernel writes the header before every step and reads it back after, and treats every cell the
 * stage wrote as untrusted:
 *   w[0] magic 0x4F535447 ("OSTG")  w[1] abi version 1            (kernel, at spawn; a stage refuses other values)
 *   w[2] flags: bit 0 stdin EOF (no stdin, or every writer closed and drained), bit 1 interrupt requested (kernel)
 *   w[3] out_len: output bytes this step, 0..CK_TASK_OUT_MAX                                            (stage)
 *   w[4] exit status, 0..255, read when the step returns DONE                                           (stage)
 *   w[8..15]  spawn parameters (argv stand-in, UNVERIFIED: the draft's argv slices are not passed yet)  (kernel, spawn)
 *   w[16..63] stage state, kept across steps                                                            (stage)
 *   w[64..64+CK_TASK_OUT_MAX) output bytes, one byte per cell (values 0..255)                            (stage)
 * Return 0 MORE (call again) or 1 DONE. Anything else, out_len over the maximum, an output cell over 255, an exit
 * status over 255, or a MORE step that consumed no input and produced no output without an interrupt pending (no
 * progress: re-stepping it would be busy polling) is a protocol violation: the task ends with status 139.
 *
 * STATUS (UNVERIFIED mapping, POSIX-shaped as section 8.4 suggests): DONE -> its exit status; TRAPPED -> 134;
 * OUTCOME_UNKNOWN -> 137 for a tick overrun or the task's step limit, else 139; a step REFUSED by the launcher -> 126;
 * protocol violation -> 139; BROKEN_PIPE on stdout -> 141; REVOKED / STALE / DENIED on a stdio end -> 137;
 * interrupt not honoured within the grace -> 130; terminate -> 143.
 *
 * BUFFERING BOUND. Bytes in flight in a pipeline are at most: CK_PIPE_CAPACITY per pipe, plus CK_TASK_OUT_MAX pending
 * output and CK_TASK_IN_MAX input per task. Nothing grows; everything is static.
 *
 * Pure logic over pipe.c, ipc.c and sched.c, no hardware: host-tested in tests/test_task.c with C stand-in stages
 * through the same runner seam, and driven in QEMU with three real Omega stages by core/task_test.c (CK_TASK_TEST).
 * Host and QEMU evidence qualify nothing physical. */
#ifndef AIENOS_CK_TASK_H
#define AIENOS_CK_TASK_H

#include <stddef.h>
#include <stdint.h>
#include "ipc.h"
#include "osh_err.h"
#include "pipe.h"
#include "sched.h"

#define CK_TASK_ABI_VERSION 1u /* abi_version of the task platform frame (the same "OSHF" frame as pipe.h) */
#define CK_TASK_MAX 8u         /* section 8.5: 8 concurrent tasks per pipeline */
#define CK_TASK_IMAGES 16u     /* resolved images a table can name (UNVERIFIED: own count) */
#define CK_TASK_IN_MAX 4096u   /* stdin bytes per step: the launcher's input window */
#define CK_TASK_OUT_MAX 4096u  /* output bytes per step */
#define CK_TASK_PARAMS 8u
#define CK_TASK_GRACE_STEPS 4u /* steps an interrupted task gets to end itself before it is terminated (UNVERIFIED) */
_Static_assert(CK_TASK_MAX <= CK_SCHED_MAX_TASKS, "every task fits the run queue");

/* stage ABI v1 (see above) */
#define CK_STG_MAGIC 0x4F535447u
#define CK_STG_ABI 1u
enum { CK_STG_W_MAGIC = 0, CK_STG_W_ABI = 1, CK_STG_W_FLAGS = 2, CK_STG_W_OUT_LEN = 3, CK_STG_W_EXIT = 4,
       CK_STG_W_PARAM = 8, CK_STG_W_STATE = 16, CK_STG_W_OUT = 64 };
#define CK_STG_F_EOF 0x1u
#define CK_STG_F_INTR 0x2u
enum { CK_STG_MORE = 0, CK_STG_DONE = 1 };
#define CK_STG_CELLS (CK_STG_W_OUT + CK_TASK_OUT_MAX) /* cells handed to a step */
#define CK_TASK_WS_PAGES 9u                           /* the workspace the runner maps: 9 pages = 4608 cells */
_Static_assert((uint64_t)CK_STG_CELLS * 8u <= (uint64_t)CK_TASK_WS_PAGES * 4096u, "the stage cells fit the workspace");

/* Task id: tag 0x54 ("T") in bits 63..56, slot in 39..32, slot epoch in 31..0. Layout UNVERIFIED (own). */
#define CK_TASK_ID_TAG 0x54u
/* The spawn capability (section 9 binding): a kernel IPC table capability (domain 1) on this resource with the WRITE
 * right. Resource, class and operation numbers are UNVERIFIED (own). */
#define CK_TASK_SPAWN_RESOURCE 0x5453504Eu /* "TSPN" */
#define CK_TASK_RC_TASK 2u                 /* binding.resource_class (osh_op.h uses 1 for the console log) */
#define CK_TASK_DOMAIN_KERNEL 1u
#define CK_TASK_DOMAIN_HOSTED 2u

/* Platform op ids, continuing pipe.h's 1..5. UNVERIFIED (own). */
enum ck_task_op { CK_TASK_OP_SPAWN = 6, CK_TASK_OP_WAIT = 7, CK_TASK_OP_STATE = 8, CK_TASK_OP_CANCEL = 9 };
enum ck_task_cancel_mode { CK_TASK_CANCEL_INTERRUPT = 1, CK_TASK_CANCEL_TERMINATE = 2 };
#define CK_TASK_FOREVER UINT64_MAX /* TASK_WAIT deadline: no time limit (it still returns when nothing can run) */

enum ck_task_state { CK_TS_FREE = 0, CK_TS_READY = 1, CK_TS_BLOCKED = 2, CK_TS_EXITED = 3 };
#define CK_TW_READABLE 0x1u /* blocked on an empty stdin */
#define CK_TW_WRITABLE 0x2u /* blocked on a full stdout */
/* how a task ended */
enum ck_task_end { CK_TE_NONE = 0, CK_TE_EXIT = 1, CK_TE_TRAPPED = 2, CK_TE_FAULT = 3, CK_TE_REFUSED = 4,
                   CK_TE_PROTOCOL = 5, CK_TE_BROKEN_PIPE = 6, CK_TE_STREAM = 7, CK_TE_INTERRUPTED = 8,
                   CK_TE_TERMINATED = 9, CK_TE_LIMIT = 10 };

/* section 9 rule 2: the authority record of an effect */
struct ck_task_binding {
    uint8_t principal_id[32];
    uint8_t domain; /* 1 = kernel IPC table (32-bit generation); 2 = hosted capability authority (64-bit) */
    uint32_t cap_index;
    uint64_t cap_generation;
    uint16_t resource_class;
    uint16_t operation;
};

/* What one step did, in the launcher's four classes (artifact/osc_launch.h OSC_RES_*, same numbers). */
enum { CK_TASK_STEP_RETURNED = 1, CK_TASK_STEP_TRAPPED = 2, CK_TASK_STEP_REFUSED = 3, CK_TASK_STEP_UNKNOWN = 4 };
#define CK_TASK_UNK_TICK_OVERRUN 2u /* osc_launch.h OSC_UNK_TICK_OVERRUN */
struct ck_task_step {
    uint8_t cls;    /* CK_TASK_STEP_* */
    uint8_t code;   /* trap code, refused code or unknown reason */
    uint64_t value; /* RETURNED: x0 */
    uint64_t ticks; /* timer ticks the step ran */
};

/* The seam to the thing that runs a step. The kernel's (core/task_osc.c) launches an admitted OSC unit; the host test
 * runs C stand-ins. */
struct ck_task_runner {
    void *ctx;
    /* Spawn-time admission of image `img` (opaque): OSH_E_OK, or the refusal (DENIED: receipt refused; NOT_FOUND:
     * unknown entry; INVALID_ARG: not a stage entry). It must refuse whatever a step would refuse, so a spawn never
     * succeeds on an image no step can run. */
    unsigned (*admit)(void *ctx, const void *img);
    /* One step of `img` over (in, in_len) and ws (cells, CK_STG_CELLS of them; the runner may map more). */
    void (*step)(void *ctx, const void *img, const uint8_t *in, size_t in_len, uint64_t *ws, uint64_t max_ticks,
                 struct ck_task_step *r);
    /* Monotonic ticks for deadlines (kernel: 10 ms timer ticks). */
    uint64_t (*now)(void *ctx);
};

struct ck_task_spawn_req {
    unsigned image;                /* index of a registered image (ck_task_image_set): the resolved target */
    struct ck_handle stdio[3];     /* ends in the spawner's table; generation 0 = none (stdin then reads as EOF) */
    uint64_t params[CK_TASK_PARAMS];
    unsigned nparams;
    uint32_t in_chunk;             /* stdin bytes per step, 1..CK_TASK_IN_MAX (0 = CK_TASK_IN_MAX) */
    uint64_t max_ticks;            /* per-step tick cap handed to the launcher (0 = the unit's own budget) */
    uint64_t max_steps;            /* steps the task may take in all (0 = no limit); over it the task ends 137 */
    uint64_t max_task_ticks;       /* ticks the task may run in all (0 = no limit); over it the task ends 137 */
    struct ck_task_binding binding;
};

struct ck_task_info {
    unsigned state, wait, end, err; /* ck_task_state, CK_TW_*, ck_task_end, the osh_err that ended it */
    uint32_t status;                /* valid when EXITED */
    uint64_t steps, ticks, bytes_in, bytes_out, blocks, wakes;
};

struct ck_task_rec {
    uint64_t ws[CK_TASK_WS_PAGES * 512u]; /* first: page aligned with the record */
    uint8_t in[CK_TASK_IN_MAX];
    uint8_t out[CK_TASK_OUT_MAX];        /* bytes packed from the output cells, pending a write */
    struct ck_cap_table caps;            /* this task's own table: its stdio ends only */
    struct ck_handle stdio[3];
    struct ck_handle watch_h;            /* the end being watched while BLOCKED (generation 0 = none) */
    unsigned watch_pipe, watch_end;
    unsigned image, state, wait, end, err, done, eof, intr_left;
    uint32_t epoch, status, in_chunk, out_off, out_len;
    uint64_t max_ticks, max_steps, max_task_ticks;
    uint64_t steps, ticks, bytes_in, bytes_out, blocks, wakes;
} __attribute__((aligned(4096)));

struct ck_task_stats {
    uint64_t spawned, reaped, steps, turns, idle_returns, wakeups, blocks;
    uint64_t blocked_runs;   /* a BLOCKED or EXITED task picked by the run queue: must stay 0 (no polling) */
    uint64_t partial_launches, cancels, refused;
};

struct ck_task_tab {
    struct ck_task_rec tasks[CK_TASK_MAX];
    struct ck_pipe_pool *pool;
    struct ck_cap_table *session; /* the spawner's table: the spawn capability and the ends it hands out */
    uint8_t principal[32];        /* the principal the session's spawn capability was granted to */
    const void *images[CK_TASK_IMAGES];
    struct ck_task_runner run;
    struct ck_pipe_hooks hooks;   /* installed as the pool's hooks: wake only (a stage never parks a stack) */
    struct ck_sched sched;
    struct ck_task_stats stats;
};

/* Takes over the pool's hooks. `session` holds the spawn capability; `principal` is who it was granted to. */
void ck_task_init(struct ck_task_tab *tab, struct ck_pipe_pool *pool, struct ck_cap_table *session,
                  const uint8_t principal[32], const struct ck_task_runner *run);
/* Register a resolved image under index i (< CK_TASK_IMAGES). */
void ck_task_image_set(struct ck_task_tab *tab, unsigned i, const void *img);
/* 1 if no task slot is in use (every task reaped). */
int ck_task_tab_empty(const struct ck_task_tab *tab);
/* Section 9: OSH_E_OK, or why the binding cannot spawn (checked again at every spawn, at the moment of the spawn). */
unsigned ck_task_perm_check(const struct ck_task_tab *tab, const struct ck_task_binding *b);

/* TASK_SPAWN. Every check comes before any effect: request shape (INVALID_ARG), binding (DENIED, STALE,
 * CAP_DOMAIN_MISMATCH, CAP_GEN_NARROW), image (NOT_FOUND, or the runner's admission refusal), a free slot (LIMIT), the
 * stdio ends (each must be the right end and grantable: DENIED, STALE, REVOKED; LIMIT). On OK the task is READY and *id
 * names it; nothing has run yet. On any error nothing is left allocated. */
unsigned ck_task_spawn(struct ck_task_tab *tab, const struct ck_task_spawn_req *rq, uint64_t *id);
/* TASK_WAIT. Runs the scheduler until the task has ended, then reaps it (its id becomes STALE) and returns OK with
 * *status. WOULD_BLOCK (task still alive, nothing reaped) when `deadline` ticks have passed, or at once when no task can
 * run (every live task is waiting for something only the caller can do: drain, feed, close, cancel). deadline 0 runs
 * nothing; CK_TASK_FOREVER has no time limit. STALE for an unknown or reaped id. */
unsigned ck_task_wait(struct ck_task_tab *tab, uint64_t id, uint64_t deadline, uint32_t *status);
/* TASK_STATE: never runs anything, never reaps. */
unsigned ck_task_state(const struct ck_task_tab *tab, uint64_t id, struct ck_task_info *info);
/* TASK_CANCEL. TERMINATE ends the task now (status 143) and closes its ends. INTERRUPT sets the interrupt flag and
 * gives the stage CK_TASK_GRACE_STEPS steps (no stdin read) to end itself; if it does not, it ends with status 130.
 * Cancelling an ended task is OK and changes nothing. */
unsigned ck_task_cancel(struct ck_task_tab *tab, uint64_t id, unsigned mode);
/* Run at most `turns` scheduler turns; returns how many ran (fewer when nothing is READY). */
unsigned ck_task_run(struct ck_task_tab *tab, unsigned turns);

/* ---- pipelines (section 7.2, 10, 12) ---- */
struct ck_task_pipeline {
    uint64_t ids[CK_TASK_MAX];
    uint32_t status[CK_TASK_MAX];
    unsigned n, started;
    unsigned cause; /* PARTIAL_LAUNCH: the error of the stage that could not start */
};
/* Start n stages (1..CK_TASK_MAX) connected left to right by new pipes: stage 0 reads `in`, stage n-1 writes `out`
 * (both in the session table, generation 0 = none), stage k's stderr is stages[k].stdio[2]. stages[k].stdio[0..1] are
 * ignored. The session never keeps an end of an inner pipe: each is closed as soon as its stage holds its own copy,
 * so EOF and BROKEN_PIPE reach the stages. If stage 0 cannot start: its error, nothing allocated. If a later stage
 * cannot start: the started stages are terminated and reaped, every end is closed, pl->cause is the error and the
 * result is PARTIAL_LAUNCH. Pipes come from the pool (CK_PIPE_POOL_PIPES), so n is at most CK_PIPE_POOL_PIPES + 1
 * (LIMIT above that, never a silent cut). */
unsigned ck_task_pipeline_spawn(struct ck_task_tab *tab, const struct ck_task_spawn_req *stages, unsigned n,
                                struct ck_handle in, struct ck_handle out, struct ck_task_pipeline *pl);
/* Wait for every stage (TASK_WAIT on each, same deadline rule). OK: all reaped, pl->status filled, *last = the last
 * stage's status (the pipeline status). WOULD_BLOCK: something is still alive (nothing reaped). */
unsigned ck_task_pipeline_wait(struct ck_task_tab *tab, struct ck_task_pipeline *pl, uint64_t deadline, uint32_t *last);
/* Section 12 cleanup: interrupt every live stage, run up to CK_TASK_GRACE_STEPS turns per stage, terminate what is
 * left, reap all. Afterwards no stage of pl is alive and every end they held is closed. Returns how many it terminated. */
unsigned ck_task_pipeline_cancel(struct ck_task_tab *tab, struct ck_task_pipeline *pl);

/* ---- versioned op table ----
 * Frame cells: 0 magic CK_PIPE_FRAME_MAGIC ("OSHF"), 1 abi_version, 2 op, 3 reserved (0), then the arguments:
 *   SPAWN  image, stdin, stdout, stderr (ck_handle_raw, 0 = none), in_chunk, max_ticks, max_steps, max_task_ticks,
 *          principal_id (4 cells, bytes 0..31 little-endian), domain, cap_index, cap_generation, resource_class,
 *          operation, nparams, params[8]                                        (27 cells)
 *   WAIT   task, deadline          STATE  task          CANCEL  task, mode
 * Check order, first failure wins, all before any effect: shorter than 4 cells, bad magic or nonzero reserved ->
 * INVALID_ARG; abi_version not CK_TASK_ABI_VERSION or an unknown op -> NOT_SUPPORTED; wrong cell count, a field out of
 * its width -> INVALID_ARG. Result {error, value}: SPAWN the task id, WAIT the status, STATE state | wait << 8 |
 * end << 16 | status << 32 (UNVERIFIED packing), CANCEL 0. */
struct ck_task_ret {
    uint32_t error;
    uint64_t value;
};
unsigned ck_task_dispatch(struct ck_task_tab *tab, const uint64_t *frame, size_t ncells, struct ck_task_ret *ret);

#endif
