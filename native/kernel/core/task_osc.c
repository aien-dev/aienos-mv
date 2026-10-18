/* task_osc.c -- the kernel's runner for pipeline tasks: admission and one EL0 step per call. See task_osc.h. */
#include "arch.h"
#include "ck_internal.h"
#include "osc_admit.h"
#include "osc_launch.h"
#include "osc_receipt.h"
#include "osc_task.h"
#include "task_osc.h"

_Static_assert(CK_TASK_IN_MAX <= CK_OSC_IN_MAX_BYTES, "a step's stdin fits the launcher's input window");
_Static_assert(CK_TASK_WS_PAGES <= OSC_WS_MAX_PAGES, "the task workspace fits the launcher's cap");
_Static_assert((int)OSC_RES_RETURNED == (int)CK_TASK_STEP_RETURNED && (int)OSC_RES_TRAPPED == (int)CK_TASK_STEP_TRAPPED &&
                   (int)OSC_RES_REFUSED == (int)CK_TASK_STEP_REFUSED && (int)OSC_RES_UNKNOWN == (int)CK_TASK_STEP_UNKNOWN,
               "step classes are the launcher's");
_Static_assert((int)OSC_UNK_TICK_OVERRUN == (int)CK_TASK_UNK_TICK_OVERRUN, "tick overrun reason");

/* reg_kind codes of the stage signature (OSC_UNIT_ARTIFACT.md, as oscc writes them in .entries): bytes ptr 11,
 * u64 5, cells ptr 12, u64 5; ret_kind u64 5 */
static const uint8_t stage_kinds[4] = { 11, 5, 12, 5 };

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s && s[n])
        n++;
    return n;
}

static unsigned osc_admit_stage(void *ctx, const void *im)
{
    struct ck_task_osc_stats *st = ctx;
    const struct ck_task_osc_image *g = im;
    struct osc_policy pol;
    struct osc_receipt_info ri;
    ck_osc_policy(&pol);
    unsigned rc = osc_receipt_verify(g->rcpt, g->rcpt_len, g->acc, &pol, &ri);
    if (rc != 0) {
        st->admit_refused++;
        st->last_refusal = rc;
        return OSH_E_DENIED;
    }
    int fi = osc_unit_lookup(g->acc, g->entry, slen(g->entry));
    if (fi < 0) {
        st->admit_refused++;
        st->last_refusal = OSC_LAUNCH_BAD_ENTRY;
        return OSH_E_NOT_FOUND;
    }
    const struct osc_entry *e = &g->acc->entry[fi];
    if (e->nregs != 4 || e->ret_kind != 5 || memcmp(e->reg_kind, stage_kinds, 4) != 0) {
        st->admit_refused++;
        st->last_refusal = OSC_LAUNCH_ARG_SHAPE;
        return OSH_E_INVALID_ARG;
    }
    return OSH_E_OK;
}

static void osc_step(void *ctx, const void *im, const uint8_t *in, size_t in_len, uint64_t *ws, uint64_t max_ticks,
                     struct ck_task_step *r)
{
    struct ck_task_osc_stats *st = ctx;
    const struct ck_task_osc_image *g = im;
    struct ck_osc_ws w = { (uint8_t *)ws, CK_TASK_WS_PAGES };
    struct ck_osc_launch_req rq;
    memset(&rq, 0, sizeof rq);
    rq.name = g->entry;
    rq.name_len = slen(g->entry);
    rq.nargs = 4;
    rq.args[0] = ck_osc_va_in();
    rq.args[1] = in_len;
    rq.args[2] = ck_osc_va_ws();
    rq.args[3] = CK_STG_CELLS;
    rq.in = in_len ? in : 0;
    rq.in_len = in_len;
    rq.ws = &w;
    rq.max_ticks = max_ticks;
    struct osc_result res;
    struct ck_osc_launch_info info;
    ck_osc_launch(g->unit, g->unit_len, g->acc, g->rcpt, g->rcpt_len, &rq, &res, &info);
    st->launches++;
    if (!info.slot_free_after || !info.tables_zeroed)
        st->not_torn_down++;
    r->cls = res.cls;
    r->code = res.cls == OSC_RES_TRAPPED ? res.trap_code
              : res.cls == OSC_RES_UNKNOWN ? res.unknown_reason
              : res.cls == OSC_RES_REFUSED ? res.refused_code
                                           : 0;
    r->value = res.value;
    r->ticks = res.ticks;
}

static uint64_t osc_now(void *ctx)
{
    (void)ctx;
    uint64_t hz = ck_rd(cntfrq_el0);
    return ck_rd(cntpct_el0) / (hz >= 100 ? hz / 100 : 1);
}

void ck_task_osc_runner(struct ck_task_runner *run, struct ck_task_osc_stats *st)
{
    memset(st, 0, sizeof *st);
    run->ctx = st;
    run->admit = osc_admit_stage;
    run->step = osc_step;
    run->now = osc_now;
}
