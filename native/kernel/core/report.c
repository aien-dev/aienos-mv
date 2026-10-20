/* report.c -- report headers, panic, fault report, PSCI reset.
 * Report keys follow the Rust handoff (report_version, aienos_commit,
 * report_kind, last_stage) so the same scripts read both kernels. */
#include "arch.h"
#include "ck_internal.h"
#include "fmt.h"
#include "vcon.h"

#ifndef AIENOS_COMMIT
#define AIENOS_COMMIT "unknown"
#endif

static const char *stage = "uefi_entry";
static int psci_hvc;
static int in_fatal;

const char *ck_commit(void) { return AIENOS_COMMIT; }
uint32_t ck_boot_count_hint(void) { return 0; }
/* Boot timeline: every stage change and every ck_boot_mark is stamped from
 * the virtual counter (CNTVCT: readable at EL1 and EL2 on every machine this
 * kernel boots, and the counter VZ and HVF start near zero at VM creation),
 * and ck_boot_timeline_report prints them as one line. No allocation, no
 * output until the report. */
#define CK_BOOT_MARKS 48u
static struct {
    const char *name;
    uint64_t cnt;
} marks[CK_BOOT_MARKS];
static unsigned n_marks, marks_lost;

void ck_boot_mark(const char *name)
{
    if (n_marks < CK_BOOT_MARKS) {
        marks[n_marks].name = name;
        marks[n_marks].cnt = ck_rd(cntvct_el0);
        n_marks++;
    } else
        marks_lost++;
}

/* "boot_timeline: unit=us counter_at_entry=<since counter 0> <name>=<us spent
 * from this mark to the next> ... total=<first mark to now>". */
void ck_boot_timeline_report(void)
{
    static int done;
    uint64_t f = ck_rd(cntfrq_el0), now = ck_rd(cntvct_el0);
    if (done || !n_marks || !f)
        return;
    done = 1;
    char line[1024];
    size_t k = 0;
    k += (size_t)ck_snprintf(line + k, sizeof line - k, "boot_timeline: unit=us counter_at_entry=%llu",
                             (unsigned long long)ck_counter_to_us(marks[0].cnt, f));
    for (unsigned i = 0; i < n_marks && k < sizeof line - 48; i++) {
        uint64_t end = i + 1 < n_marks ? marks[i + 1].cnt : now;
        k += (size_t)ck_snprintf(line + k, sizeof line - k, " %s=%llu", marks[i].name,
                                 (unsigned long long)ck_counter_to_us(end - marks[i].cnt, f));
        if (k >= sizeof line)
            k = sizeof line - 1;
    }
    ck_printf("%s total=%llu lost=%u\n", line, (unsigned long long)ck_counter_to_us(now - marks[0].cnt, f),
              marks_lost);
    if (ck_vcon_on()) {
        uint64_t chunks = 0, full = 0;
        unsigned to = 0;
        ck_vcon_tx_stats(&chunks, &full, &to);
        ck_printf("console_tx: virtio-console chunks=%llu ring_full_waits=%llu wait_timeouts=%u\n",
                  (unsigned long long)chunks, (unsigned long long)full, to);
    }
}

void ck_set_stage(const char *name)
{
    stage = name;
    ck_boot_mark(name);
    if (ck_pi5_stage)
        ck_pi5_stage(name);
}
const char *ck_stage_name(void) { return stage; }

void ck_report_header(const char *kind)
{
    ck_printf("report_version: 1\naienos_commit: %s\nreport_kind: %s\nlast_stage: %s\n",
              ck_commit(), kind, stage);
}

void ck_psci_configure(const void *fadt)
{
    uint16_t flags = 0;
    if (fadt && ck_fadt_arm_boot_arch(fadt, &flags) == 0)
        psci_hvc = (flags & 2) != 0;
}

/* SMCCC v1.0: x4-x17 may be corrupted by the callee. */
#define PSCI_CLOBBERS "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", \
                      "x15", "x16", "x17", "memory"

int64_t ck_psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    /* HVC would land in our own EL2 vectors while still at EL2. */
    if (psci_hvc && ck_current_el() == 1)
        __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : PSCI_CLOBBERS);
    else
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : PSCI_CLOBBERS);
    return (int64_t)x0;
}

const char *ck_psci_conduit(void)
{
    return (psci_hvc && ck_current_el() == 1) ? "hvc" : "smc";
}

static void psci_call(uint64_t fn)
{
    (void)ck_psci_call(fn, 0, 0, 0);
}

void ck_reset(void)
{
    static int quiescing;
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    if (ck_stage_quiesce && !quiescing) {
        quiescing = 1; /* a panic inside the hook resets without it */
        ck_stage_quiesce();
    }
    if (ck_pi5_before_reset)
        ck_pi5_before_reset();
    ck_console_fb_hold();
    ck_vcon_flush(); /* the last lines reach the host before the reset */
    ck_mb();
    psci_call(0x84000009u); /* PSCI SYSTEM_RESET */
    psci_call(0x84000008u); /* SYSTEM_OFF if reset was refused */
    for (;;)
        __asm__ volatile("wfi");
}

void ck_mb(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

void ck_panic(const char *fmt, ...)
{
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    if (in_fatal++)
        ck_reset();
    ck_puts("\n");
    ck_report_header("panic");
    ck_puts("panic: ");
    va_list ap;
    va_start(ap, fmt);
    ck_vprintf(fmt, ap);
    va_end(ap);
    ck_puts("\n");
    ck_reset();
}

void ck_fault_report(const char *what, uint64_t esr, uint64_t far, uint64_t elr, unsigned el)
{
    if (in_fatal++)
        ck_reset();
    ck_puts("\n");
    ck_report_header("fault");
    ck_printf("fault: vector=%s el=%u esr=0x%llx ec=0x%llx far=0x%llx elr=0x%llx\n", what, el,
              (unsigned long long)esr, (unsigned long long)((esr >> 26) & 0x3f),
              (unsigned long long)far, (unsigned long long)elr);
    ck_reset();
}
