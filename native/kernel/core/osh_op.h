/* osh_op.h -- the AIENOS adapter's platform-operation layer for OSH (aien-architecture#158): one executor for every
 * request record, whatever route built it (a script or a line typed at a prompt through the shell core, or AIEN's
 * direct-argv record with flags bit 0).
 *
 * Contract: aien-protocols-mv specs/osh-platform/OSH_PLATFORM_ABI.md v1 DRAFT (unfrozen; read at main fb5fca98e667),
 * sections 7 to 13, and its stream/task revision 2, STREAM_TASK.md on branch feat/osh-stream-abi-43 at
 * 181855c9ff3e (ST6 tasks, ST7 launch order, ST8 effects and outcomes), whose semantics win where the two differ.
 * Reference behaviour: omega src/osh/host/osh_exec.c run_pipeline at a8ffd2b912c8 (the Linux host adapter).
 * Everything marked UNVERIFIED is this adapter's own choice where the drafts name a thing but do not pin it.
 *
 * WHAT RUNS (aienos-mv#430). A command name is a lookup key only (section 9 rule 1). RESOLVE (section 8.3) knows:
 *   - `klog <words...>`: class 1, an operation the adapter hosts itself. It writes one line to the kernel console
 *     ("osh_op_out: <words>") and needs WRITE on the console-log capability. Single command, no redirections: that is
 *     the only form this operation has (anything else is NOT_SUPPORTED before any effect).
 *   - the stage operations the session's registry names (struct osh_op_def): class 1 OMEGA_NATIVE_OP, each an
 *     admitted Omega unit run as a native task (core/task.h, stage ABI v1) and connected to its neighbours by native
 *     pipes (core/pipe.h). Every spawn needs a binding on the session's spawn capability, re-checked at the moment of
 *     that spawn. argv[1..] become the stage's parameters (decimal u64 each: stage ABI v1 has no argv slices yet,
 *     UNVERIFIED stand-in).
 *   - a registry entry of class 2, 3 or 4, and any name containing '/' (a path: class 4 LINUX_EXECUTABLE by the
 *     PATH rule, UNVERIFIED reading), resolve to a class this adapter does not provide: the whole pipeline is
 *     refused UNAVAILABLE, status 126, before any effect. Classes never fall back to each other and nothing is ever
 *     handed to a host shell or another machine.
 *   - any other name is not found: that command gets status 127 and the others still run (ST7 step 5).
 *
 * LAUNCH ORDER (ST7). 1 shape (every command block; INVALID_ARG, status 70); 2 RESOLVE every command (UNAVAILABLE
 * 126; NOT_SUPPORTED 126 for builtins, assignments and klog in a pipeline or with redirections; INVALID_ARG 70 for a
 * parameter that is not a decimal u64; LIMIT for more parameters than the stage takes); 3 PERM_CHECK of the spawn
 * binding (its error, 126); 4 the resources the pipeline needs: pipes in the pool, intent records (LIMIT, status 1).
 * Nothing before this point has an effect or writes an intent. 5 start the commands left to right; for each, at
 * the moment of its effect: the pipe to its right neighbour, then its redirections in record order, then the spawn.
 * A name not found (127), a redirection refused (1) or a binding that fails now (126: revoked between stages) ends
 * only that command: its pipe ends are closed so its neighbours see end of stream or BROKEN_PIPE. 6 a resource
 * failure of the spawn itself (LIMIT, IO; or the test hook) is a launch failure: nothing started yet -> that error,
 * status 1; otherwise the started stages are cancelled and reaped (section 12) and the result is PARTIAL_LAUNCH,
 * status 1. 7 run every stage to its end, reap all; the pipeline status is the last command's status.
 *
 * REDIRECTIONS (section 7.1, 7.2). After pipe setup each command has a descriptor table {0, 1, 2}: 0 is the pipe
 * from its left neighbour (the first command: nothing, end of stream), 1 the pipe to its right neighbour (the last:
 * the session's stdout, env->stdout_bytes), 2 nothing (discarded; stage ABI v1 cannot write stderr). Each
 * redirection then applies in record order, so `>o 2>&1` and `2>&1 >o` differ as in POSIX shells: kind 4 copies
 * descriptor M into N; kinds 1 (read), 2 (create or truncate) and 3 (append) open a location. AIENOS has no POSIX
 * path scheme, so in this TEST adapter a location is a named RAM buffer of the session (OSH_LOC_MAX of them,
 * OSH_LOC_BYTES each), reached only through the session's location capability: READ to open for reading, WRITE to
 * open for writing, checked at the moment of the open. UNVERIFIED stand-in for a Store-backed location (section 8.2,
 * R1.1). The adapter moves the bytes between the location and a pipe end it holds; the stage only ever sees a pipe.
 * More bytes than a location holds is LIMIT (the write is cut at the limit, the writer gets BROKEN_PIPE, and the
 * command's error says LIMIT): never a silent truncation.
 *
 * EFFECTS AND RECORDS (section 10, ST8). An effect is one spawn, one write-class open or one console write. Before
 * each the adapter writes an intent record (RAM here; the integration point for Omega's durable records is struct
 * osh_intent and osh_op_recover), after it an outcome. Every grant used and every refusal at an effect boundary
 * writes a security receipt (section 13). An adapter lost after an effect leaves its intent open: a restart reports
 * it OUTCOME_UNKNOWN with the request digest and never replays it; the kernel reaps what the lost adapter left
 * running (osh_session_close).
 *
 * UNVERIFIED CHOICES (the drafts at the commits above do not pin these; each is this adapter's own reading):
 *   U1  klog is single-command only: in a pipeline or with a redirection it is NOT_SUPPORTED (126).
 *   U2  a name containing '/' is class 4 (path rule) and so UNAVAILABLE.
 *   U3  locations are TEST RAM buffers behind one session location capability (READ to read, WRITE to create,
 *       truncate or append), 4 of 16 KiB, names of 1 to 32 bytes; a longer name is LIMIT.
 *   U4  argv[1..] are decimal u64 stage parameters; anything else is INVALID_ARG.
 *   U5  a failed redirection gives that command status 1 and it does not run (Omega osh_exec.c and POSIX shells; the
 *       drafts are silent).
 *   U6  the name and the binding are decided before any redirection opens (bash opens redirections first, so
 *       `nosuch >o` truncates o in bash and leaves it untouched here).
 *   U7  a spawn refused by admission (receipt, entry) or by the binding at that moment is that command's 126, not a
 *       launch failure; only LIMIT and IO from the spawn are launch failures (ST7 step 6).
 *   U8  step 4 checks that the pool and the session table can hold every pipe, pump and intent the pipeline needs,
 *       then the pipes are created one at a time during step 5.
 *   U9  a location that overflows is LIMIT for that command and its writer sees BROKEN_PIPE.
 *   U10 the session deadline ends a pipeline like an interrupt (section 12 cleanup, INTERRUPTED, status 130); a
 *       pipeline in which nothing can move any more ends as IO.
 *   U11 security receipts for stage effects carry `cmd=<k>`; the pipeline error is the first command error that is not
 *       NOT_FOUND (Omega's rule).
 *   U12 in a request record every argv, assignment and redirection entry beyond its count must be zero (INVALID_ARG).
 *   U13 statuses: LIMIT and PARTIAL_LAUNCH 1 (the single-command adapter before #430 gave LIMIT 70), INVALID_ARG and IO
 *       70, refusals 126, not found 127, interrupt 130; a refusal at PERM_CHECK is FAILED_NO_EFFECT, a refusal at
 *       RESOLVE NOT_STARTED.
 *   U14 task.c reports terminate as 143 where STREAM_TASK ST6 says 137; this adapter passes task.c's status through.
 *   U15 task.c's own permission check says STALE for a removed capability; the adapter checks first so a capability
 *       revoked between stages is REVOKED as the drafts name it.
 *   U16 at most 8 commands, 4 pipes in the pool and 8 slots in the session table: below the ST9 minimums.
 *   U17 descriptor 2 of a stage goes nowhere (stage ABI v1 cannot write stderr), so `2>file` creates an empty file.
 *
 * Pure logic over ipc.c, pipe.c, task.c and sched.c, no hardware: host-tested in tests/test_osh_op.c with C stand-in
 * stages through the task runner seam, driven in QEMU by core/osh_test.c (CK_OSH_OP_TEST) with the real shell units
 * and three admitted Omega stage units. Host and QEMU evidence qualify nothing physical. */
#ifndef AIENOS_CK_OSH_OP_H
#define AIENOS_CK_OSH_OP_H

#include <stddef.h>
#include <stdint.h>
#include "ipc.h"

/* This layer is a QEMU/host TEST adapter (RAM-only records and locations, injection hooks that can revoke a capability or
 * lose the adapter). It is built only with CK_OSH_OP_TEST and never into a production or hardware-staging image. */
#ifndef CK_OSH_OP_TEST
#error "osh_op is test-only: define CK_OSH_OP_TEST (make CK_OSH_TEST=1 CK_OSH_OP_TEST=1, or the host test rules)"
#endif
#ifdef CK_HARDWARE_STAGING
#error "CK_OSH_OP_TEST cannot be combined with CK_HARDWARE_STAGING"
#endif

/* section 8.6 platform error codes: one shared numbering (osh_err.h), also used by the kernel pipe primitive (pipe.h) */
#include "osh_err.h"
#include "pipe.h"
#include "task.h"
/* section 10 outcomes */
enum osh_outcome { OSH_O_NOT_STARTED = 0, OSH_O_COMPLETED, OSH_O_FAILED_NO_EFFECT, OSH_O_CANCELLED, OSH_O_OUTCOME_UNKNOWN };

/* The resources and operations this layer knows. The numeric ids are this adapter's own (the draft names the fields and
 * leaves the numbering open: UNVERIFIED against a frozen contract). The spawn capability is task.h's. */
#define OSH_RES_CONSOLE_LOG 0x4f534843u /* "OSHC" */
#define OSH_RC_CONSOLE_LOG 1u           /* binding.resource_class */
#define OSH_OP_WRITE 1u                 /* binding.operation */
#define OSH_RES_LOCATIONS 0x4f53484cu   /* "OSHL": the session's TEST locations */
#define OSH_RC_LOCATION 3u              /* binding.resource_class (task.h uses 2 for spawning) */
#define OSH_OP_OPEN_READ 2u
#define OSH_OP_OPEN_WRITE 3u

/* section 8.3 resolution classes */
enum osh_class { OSH_CLASS_NATIVE_OP = 1, OSH_CLASS_ARTIFACT = 2, OSH_CLASS_SERVICE = 3, OSH_CLASS_LINUX = 4 };

/* section 7.1 request record */
#define OSH_REQ_MAGIC 0x4F524551ull
#define OSH_REQ_HDR 8u
#ifndef OSH_REQ_CMD_CELLS /* also defined by the shell layout (tests/fixtures/osh/osh_layout.h), with the same value */
#define OSH_REQ_CMD_CELLS 164u
#endif
#define OSH_MAX_CMDS 8u
#define OSH_MAX_ARGV 32u
#define OSH_MAX_ASSIGN 16u
#define OSH_MAX_REDIR 8u
#define OSH_REQ_F_DIRECT 0x1ull /* section 11: AIEN direct-argv submission */
_Static_assert(OSH_REQ_CMD_CELLS == 4u + 2u * OSH_MAX_ARGV + 4u * OSH_MAX_ASSIGN + 4u * OSH_MAX_REDIR, "section 7.1 command block");
enum { OSH_RK_IN = 1, OSH_RK_OUT = 2, OSH_RK_APPEND = 3, OSH_RK_DUP = 4 };

/* section 9 rule 2: the authority record carried by every effect */
struct osh_binding {
    uint8_t principal_id[32];
    uint8_t domain; /* 1 = kernel IPC table (32-bit generation) */
    uint32_t cap_index;
    uint64_t cap_generation;
    uint16_t resource_class;
    uint16_t operation;
};

#define OSH_INTENT_MAX 16
enum osh_effect { OSH_EF_CONSOLE = 0, OSH_EF_SPAWN = 1, OSH_EF_OPEN_WRITE = 2 };
struct osh_intent {
    uint8_t used;      /* 1 once the intent is written */
    uint8_t closed;    /* 1 once an outcome is written */
    uint8_t outcome;   /* enum osh_outcome, valid when closed */
    uint8_t err;       /* enum osh_err, valid when closed */
    uint8_t effect;    /* enum osh_effect */
    uint8_t cmd;       /* 0-based command of the pipeline */
    uint32_t seq;      /* 1-based pipeline number in the session */
    uint32_t status;   /* shell status when COMPLETED */
    uint8_t digest[32];
};

/* Test injection points (all zero in normal use). Each `_at` names the 1-based pipeline number of the session. */
struct osh_hooks {
    uint32_t revoke_before_perm_at;  /* the console capability is revoked after the binding is built, before the effect */
    uint32_t interrupt_before_effect_at; /* an interrupt arrives after the permission check, before the effect (klog) */
    uint32_t crash_after_effect_at;  /* the adapter is lost after the effect (klog), or after spawn `crash_cmd` (stages) */
    uint32_t sink_fail_at;           /* the console write reports failure */
    /* stage pipelines (aienos-mv#430) */
    uint32_t revoke_spawn_at, revoke_spawn_cmd;   /* the spawn capability is revoked just before command k's effect */
    uint32_t revoke_loc_at, revoke_loc_cmd;       /* the location capability is revoked just before command k's effect */
    uint32_t fail_spawn_at, fail_spawn_cmd, fail_spawn_err; /* command k's spawn fails with that error (fault-spawn) */
    uint32_t crash_cmd;                           /* with crash_after_effect_at: lost right after this command's spawn */
    uint32_t interrupt_at, interrupt_after_turns; /* an interrupt (section 12 cleanup) after that many scheduler turns */
    uint32_t snapshot_at, snapshot_after_turns;   /* record a mid-run snapshot after that many turns */
};

struct osh_env {
    void *ctx;
    int (*out)(void *ctx, const char *line, size_t len); /* the console effect: 0 = written */
    void (*log)(void *ctx, const char *line);            /* receipts and records, one line, no newline */
    /* the session's stdout: bytes the last command writes when not redirected; 0 = accepted. NULL: none (discarded). */
    int (*stdout_bytes)(void *ctx, const uint8_t *b, size_t n);
};

/* ---- the stage registry and the native task machinery a session may run on ---- */
#define OSH_OPS_MAX 16u
struct osh_op_def {
    const char *name;  /* exact bytes of argv[0] (no NUL inside) */
    uint8_t cls;       /* enum osh_class; only OSH_CLASS_NATIVE_OP is provided by this adapter */
    uint8_t max_args;  /* argv[1..] accepted, each a decimal u64 -> params[k-1]; at most CK_TASK_PARAMS */
    unsigned image;    /* task image index (struct osh_native images[]) */
    uint32_t in_chunk; /* stdin bytes per step (0 = CK_TASK_IN_MAX) */
};

struct osh_native {
    struct ck_task_tab *tab;     /* storage only: osh_session_attach initialises it over the session's table */
    struct ck_pipe_pool *pool;   /* likewise */
    struct ck_task_runner run;
    const void *images[CK_TASK_IMAGES];
    const struct osh_op_def *ops;
    unsigned nops;
    uint64_t step_ticks;         /* per-step tick cap handed to the launcher (0 = the unit's own budget) */
    uint64_t max_steps;          /* steps a stage may take in all (0 = no limit) */
    uint64_t deadline_ticks;     /* ticks a pipeline may run before the session interrupts it (0 = no deadline) */
};

/* ---- TEST locations (see REDIRECTIONS above) ---- */
#define OSH_LOC_MAX 4u
#define OSH_LOC_BYTES 16384u
#define OSH_LOC_NAME 32u
struct osh_loc {
    uint8_t used, name_len;
    uint8_t name[OSH_LOC_NAME];
    uint32_t len;
    uint8_t bytes[OSH_LOC_BYTES];
};

/* the pipeline in flight (kept in the session so a lost adapter's leftovers can be reaped) */
#define OSH_PUMPS_MAX CK_PIPE_POOL_PIPES
struct osh_pump {
    uint8_t used, feed, done, overflow; /* feed: the adapter writes into the pipe (input redirection) */
    int8_t loc;                         /* location index, -1 = the session's stdout */
    struct ck_handle h;                 /* the end the adapter holds, in the session table */
    uint32_t off;                       /* feed: next byte of the location to write */
    uint64_t bytes;
    uint8_t cmd;                        /* the command whose stream this is */
    int8_t intent;                      /* write-class open: its intent record, -1 none */
};
struct osh_live {
    uint8_t active;
    unsigned n;
    uint64_t ids[OSH_MAX_CMDS];   /* 0 = not started or reaped */
    int8_t intent[OSH_MAX_CMDS];  /* spawn intents, -1 none */
    struct osh_pump pumps[OSH_PUMPS_MAX];
};

/* One principal per session is the current limit: `principal` is the task running the shell, `cap_holder` is the principal
 * every capability was granted to (recorded at grant). Each binding carries `principal`; PERM_CHECK requires it to equal
 * `cap_holder`, so a capability is only usable by the principal it was granted to. */
struct osh_session {
    struct ck_cap_table caps;     /* console and spawn capabilities, and the pipe ends the adapter holds */
    struct ck_cap_table loc_caps; /* the location capability: its own table, so the 8 slots above stay for pipe ends */
    struct ck_handle console_cap; /* generation 0 = none held */
    struct ck_handle spawn_cap;
    struct ck_handle loc_cap;     /* in loc_caps */
    uint32_t epoch;               /* advanced by every restart (section 12, ST8.3) */
    uint8_t principal[32];
    uint8_t cap_holder[32]; /* zero when no capability was granted */
    struct osh_intent intents[OSH_INTENT_MAX];
    uint32_t pipelines;
    struct osh_hooks hooks;
    struct osh_native *nat; /* NULL: no stage operations (klog only) */
    struct osh_live live;
    struct osh_loc locs[OSH_LOC_MAX];
};

struct osh_result {
    int status;      /* shell status for $?: the last command's (ST7 step 7), or the whole-pipeline refusal's */
    unsigned error;  /* enum osh_err */
    unsigned outcome; /* enum osh_outcome of the pipeline */
    unsigned launch_error; /* OK once every command was started or refused one by one (ST7 step 5); else the whole
                            * pipeline's refusal (steps 1 to 4) or launch failure (step 6, PARTIAL_LAUNCH) */
    int stop;        /* 1: interrupted or lost, the list must stop */
    int crashed;     /* 1: lost after an effect, outcome open */
    uint32_t seq;
    uint8_t digest[32];
    /* per command (aienos-mv#430) */
    unsigned ncmds, started;
    int cmd_status[OSH_MAX_CMDS];  /* -1: never started and no status (NOT_STARTED) */
    uint8_t cmd_outcome[OSH_MAX_CMDS], cmd_error[OSH_MAX_CMDS];
    uint8_t cmd_fd[OSH_MAX_CMDS][3]; /* where each descriptor went: enum osh_fd_kind << 4 | index */
    /* evidence */
    uint64_t stdout_bytes;           /* bytes delivered to the session's stdout */
    uint64_t steps, blocks, wakeups, turns, max_resident, pumped;
    unsigned mid_alive;              /* snapshot: stages alive */
    uint64_t mid_in_flight;          /* snapshot: bytes written by stages minus bytes read by stages and the adapter */
};
enum osh_fd_kind { OSH_FD_NONE = 0, OSH_FD_PIPE_IN = 1, OSH_FD_PIPE_OUT = 2, OSH_FD_STDOUT = 3, OSH_FD_LOC_IN = 4,
                   OSH_FD_LOC_OUT = 5 };

/* A fresh session. grant_rights = rights of the console capability inserted (0 = no capability held at all). */
void osh_session_init(struct osh_session *s, unsigned grant_rights);
/* The capability root grants the session a spawn capability (CK_R_WRITE needed to spawn; CK_R_GRANT is added so the
 * stage ends can be handed out) or a location capability (CK_R_READ, CK_R_WRITE). 0 on success. */
int osh_session_grant_spawn(struct osh_session *s, unsigned rights);
int osh_session_grant_loc(struct osh_session *s, unsigned rights);
/* Run stage operations on `nat` (its tab and pool are initialised here over the session's table). */
void osh_session_attach(struct osh_session *s, struct osh_native *nat);
/* Task end (or the adapter is lost): section 12 cleanup of an in-flight pipeline (interrupt, grace, terminate, reap),
 * every pipe end closed, every capability the session holds dropped. Returns 1 if the session table, the task table and
 * the pipe pool are all empty afterwards. */
int osh_session_close(struct osh_session *s);
/* A restart: a new session that inherits only the intent records (what a durable log would give back) and the
 * locations (state outside the adapter), holding no capability and no stage machinery. Intents without an outcome
 * are then reported by osh_op_recover. */
void osh_session_restart(struct osh_session *fresh, const struct osh_session *old);
/* Marks every open intent OUTCOME_UNKNOWN and logs it with its request digest. Never re-runs anything.
 * Returns how many were open. */
unsigned osh_op_recover(struct osh_session *s, const struct osh_env *env);

/* Run one PIPELINE_READY request record. req = the REQUEST cells, out = the OUT cells (one byte per cell, n_out cells). */
void osh_op_exec(struct osh_session *s, const struct osh_env *env, const uint64_t *req, const uint64_t *out, size_t n_out,
                 struct osh_result *r);

/* section 9 PERM_CHECK of the console binding, exposed for the tests: OSH_E_OK or the denial reason */
unsigned osh_op_perm_check(const struct osh_session *s, const struct osh_binding *b);
/* A location's bytes (NULL if no location has that name). */
const struct osh_loc *osh_loc_find(const struct osh_session *s, const uint8_t *name, size_t len);

const char *osh_err_name(unsigned e);
const char *osh_outcome_name(unsigned o);

#endif
