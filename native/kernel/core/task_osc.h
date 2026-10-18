/* task_osc.h -- the kernel's runner for pipeline tasks (core/task.h, aienos-mv#429): a stage is an admitted OSC unit
 * (osc_admit.c) and one function in it with the stage signature `fn X(inp: bytes, w: cells) -> u64`.
 *
 * admit: the realization receipt is verified against the unit and the kernel's own policy (osc_receipt_verify, as
 * ck_osc_launch does first), then the entry is looked up and its shape checked (4 registers: bytes, u64, cells, u64;
 * returns u64). Refusals: DENIED (no or bad receipt), NOT_FOUND (no such function), INVALID_ARG (not a stage).
 * step: exactly one ck_osc_launch (osc_task.h): receipt verified again, code re-hashed, the step entered at EL0 in its
 * own address space with the stdin bytes in the read-only input window and the task's workspace (CK_TASK_WS_PAGES,
 * page aligned, owned by the task record) mapped read-write, never executable, then torn down. Nothing here bypasses
 * those checks: there is no other way into EL0. now: the 10 ms timer tick count (CNTPCT / (CNTFRQ / 100)).
 * QEMU (aarch64 virt) so far; nothing here is physical. */
#ifndef AIENOS_CK_TASK_OSC_H
#define AIENOS_CK_TASK_OSC_H
#include <stddef.h>
#include <stdint.h>
#include "osc_unit.h"
#include "task.h"

struct ck_task_osc_image {
    const uint8_t *unit; /* the container bytes osc_unit_admit accepted */
    size_t unit_len;
    const struct osc_accept *acc;
    const uint8_t *rcpt; /* its realization receipt, or NULL (then refused: RECEIPT_MISSING) */
    size_t rcpt_len;
    const char *entry;   /* the stage function's exact name */
};

struct ck_task_osc_stats {
    uint64_t launches;       /* ck_osc_launch calls (one per step) */
    uint64_t not_torn_down;  /* launches after which the slot was not free or a page-table entry was left: must be 0 */
    uint64_t admit_refused;  /* spawns refused by the receipt or entry check */
    uint32_t last_refusal;   /* the launcher code of the last admission refusal (receipt or entry) */
};

/* The runner (ctx = st, which must outlive the task table). */
void ck_task_osc_runner(struct ck_task_runner *run, struct ck_task_osc_stats *st);

#endif
