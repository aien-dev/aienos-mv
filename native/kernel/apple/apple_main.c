/* apple_main.c -- first C code of the CK_APPLE=1 raw Image (apple_entry.S
 * calls it at EL1h, MMU off, DAIF masked, x0 = device tree).
 *
 * What it does, in order, each step one serial line:
 *   device tree check (ck_fdt_open), console from /chosen stdout-path,
 *   EL2 -> EL1 result, memory and reservations, EL1 vectors self-test (a BRK
 *   taken and returned), EL1 physical timer reached (polled with its
 *   interrupt masked: no interrupt controller is touched here), the FIQ
 *   source decode on this CPU, the Apple platform probe (family table), the
 *   idle policy in force, then AIENOS_APPLE_IMAGE: PASS|FAIL.
 * It ends with PSCI SYSTEM_OFF when the tree has a /psci node (QEMU), and
 * otherwise (every Apple chip: no EL3, no PSCI) stops with interrupts masked
 * in the idle policy the family table allows. No float, every wait bounded.
 * QEMU runs of this image qualify nothing physical. */
#include <stdarg.h>
#include <stdint.h>

#include "aic.h"
#include "apple_console.h"
#include "apple_family.h"
#include "apple_smp.h"
#include "fdt.h"
#include "fmt.h"

struct apple_frame {
    uint64_t x[31];
    uint64_t elr, spsr, pad;
};
_Static_assert(sizeof(struct apple_frame) == 272, "apple_frame matches apple_entry.S");

extern char __image_base[], __image_end[], apple_vectors[], apple_secondary_entry[];
__attribute__((noreturn)) void apple_stop(void);
__attribute__((noreturn)) void apple_main(uint64_t dtb, uint64_t entry_el, uint64_t e2h);
void apple_exception(uint64_t idx, struct apple_frame *fr);
__attribute__((noreturn)) void apple_secondary_main(uint64_t idx, uint64_t entry_el, uint64_t e2h);

/* Secondary CPU table (apple_entry.S apple_secondary_entry reads the first
 * two): MPIDR affinity per cpu index, count, and the check-in word each
 * secondary writes: 1 | entry EL << 8 | E2H << 16 | CurrentEL << 24. */
uint64_t apple_smp_mpidr[CK_APPLE_MAX_CPUS];
uint32_t apple_smp_n;
static volatile uint32_t apple_smp_checkin[CK_APPLE_MAX_CPUS];

#define rd(reg) ({ uint64_t v_; __asm__ volatile("mrs %0, " #reg : "=r"(v_)); v_; })
#define wr(reg, v) __asm__ volatile("msr " #reg ", %0" ::"r"((uint64_t)(v)) : "memory")

static struct ck_apple_uart uart;
static struct ck_fdt fdt;
static struct ck_apple_platform plat;
static volatile uint32_t brk_taken;
static struct ck_aic aic;
static int aic_up;

static void emit(void *ctx, char c)
{
    (void)ctx;
    if (c == '\n') ck_apple_uart_putc(&uart, '\r');
    ck_apple_uart_putc(&uart, c);
}

static void pr(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void pr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ck_vformat(emit, 0, fmt, ap);
    va_end(ap);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static unsigned current_el(void) { return (unsigned)(rd(CurrentEL) >> 2) & 3u; }

void apple_exception(uint64_t idx, struct apple_frame *fr)
{
    uint64_t esr = rd(esr_el1);
    /* Current EL with SP_ELx, synchronous, EC 0x3c (BRK): the self-test. */
    if (idx == 4 && (esr >> 26) == 0x3cu && (esr & 0xffffu) == 0x50u) {
        brk_taken++;
        fr->elr += 4;
        return;
    }
    /* IRQ (5) and FIQ (6) from the current EL: only once the AIC is up on a
     * family-table chip; nothing unmasks them yet (DAIF stays masked). */
    if (aic_up && idx == 6) {
        ck_aic_handle_fiq(&aic, &ck_aic_io_live, 0);
        return;
    }
    if (aic_up && idx == 5) {
        ck_aic_handle_irq(&aic, 0, 0, 64u);
        return;
    }
    pr("apple: FAULT vector=%llu esr=0x%llx elr=0x%llx far=0x%llx spsr=0x%llx\n", (unsigned long long)idx,
       (unsigned long long)esr, (unsigned long long)fr->elr, (unsigned long long)rd(far_el1),
       (unsigned long long)fr->spsr);
    pr("AIENOS_APPLE_IMAGE: FAIL\n");
    apple_stop();
}

/* EL1 physical timer, interrupt masked (CNTP_CTL_EL0 ENABLE | IMASK): wait
 * for ISTATUS with a bound of 100 ms of counter time plus a loop cap. */
static int timer_check(uint64_t *freq_out, uint64_t *ticks_out)
{
    uint64_t freq = rd(cntfrq_el0);
    *freq_out = freq;
    if (freq == 0) return -1;
    uint64_t start = rd(cntpct_el0);
    wr(cntp_tval_el0, freq / 1000u); /* 1 ms */
    wr(cntp_ctl_el0, 3u);           /* ENABLE | IMASK */
    __asm__ volatile("isb");
    for (uint32_t spins = 0; spins < 50000000u; spins++) {
        uint64_t now = rd(cntpct_el0);
        if (rd(cntp_ctl_el0) & 4u) { /* ISTATUS */
            *ticks_out = now - start;
            wr(cntp_ctl_el0, 2u);    /* off, masked */
            return 0;
        }
        if (now - start > freq / 10u) break;
    }
    wr(cntp_ctl_el0, 2u);
    return -1;
}


static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* PSCI call (DEN 0022) through the tree's conduit: "smc" or "hvc". */
static uint64_t psci_call(const char *method, uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    if (method[0] == 's')
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)::"memory");
    else
        __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)::"memory");
    return x0;
}
#define PSCI_SYSTEM_OFF 0x84000008u
#define PSCI_CPU_ON_64 0xc4000003u

/* Secondary, at EL1 on its own stack: check in, then park with interrupts
 * masked (spinning: the idle policy for parked cores is not decided yet). */
void apple_secondary_main(uint64_t idx, uint64_t entry_el, uint64_t e2h)
{
    if (idx < CK_APPLE_MAX_CPUS) {
        apple_smp_checkin[idx] = 1u | (uint32_t)entry_el << 8 | (uint32_t)e2h << 16 | current_el() << 24;
        __asm__ volatile("dsb sy\n dc civac, %0\n dsb sy\n sev" ::"r"(&apple_smp_checkin[idx]) : "memory");
    }
    for (;;) __asm__ volatile("yield");
}

/* Live spin-table accessors for ck_smp_spin_release. */
static uint64_t smp_rd64(void *ctx, uint64_t pa)
{
    (void)ctx;
    return *(volatile uint64_t *)(uintptr_t)pa;
}
static void smp_wr64(void *ctx, uint64_t pa, uint64_t v)
{
    (void)ctx;
    *(volatile uint64_t *)(uintptr_t)pa = v;
}
static void smp_clean(void *ctx, uint64_t pa)
{
    (void)ctx;
    __asm__ volatile("dsb sy\n dc civac, %0\n dsb sy" ::"r"(pa) : "memory");
}
static void smp_sev(void *ctx)
{
    (void)ctx;
    __asm__ volatile("sev" ::: "memory");
}
/* One poll: the check-in word, then about 10 us of counter time. */
static uint32_t smp_checkin(void *ctx, uint32_t idx)
{
    (void)ctx;
    __asm__ volatile("dmb sy" ::: "memory");
    if (idx < CK_APPLE_MAX_CPUS && apple_smp_checkin[idx]) return 1;
    uint64_t t0 = rd(cntpct_el0), wait = rd(cntfrq_el0) / 100000u;
    for (uint32_t i = 0; i < 1000000u && rd(cntpct_el0) - t0 < wait; i++) __asm__ volatile("yield");
    return 0;
}
static const struct ck_smp_io smp_io = {smp_rd64, smp_wr64, smp_clean, smp_sev, smp_checkin, 0};
#define SMP_POLLS 100000u

static void smp_report(uint32_t i, const char *how, int rc)
{
    uint32_t w = apple_smp_checkin[i];
    if (rc == 0)
        pr("apple: cpu%u mpidr=0x%llx checked in at EL%u (entered EL%u, E2H=%u) via %s\n", i,
           (unsigned long long)apple_smp_mpidr[i], w >> 24, (w >> 8) & 0xffu, (w >> 16) & 0xffu, how);
    else
        pr("apple: cpu%u mpidr=0x%llx not started via %s: %s\n", i, (unsigned long long)apple_smp_mpidr[i], how,
           ck_smp_strerror(rc));
}

/* Starts every secondary. Family-table chip: the spin table (refusals are a
 * safe outcome, never a failure). Otherwise (the QEMU gate), PSCI CPU_ON
 * into the same apple_secondary_entry, so the gate runs the secondary EL2 ->
 * EL1 path; there every secondary must check in. Returns 0 on a failure. */
static int start_secondaries(int verdict, const char *psci)
{
    uint64_t me = rd(mpidr_el1) & CK_SMP_AFF_MASK;
    uint32_t n = 0, started = 0, boot = CK_APPLE_MAX_CPUS;
    if (verdict == CK_APPLE_OK) {
        for (uint32_t i = 0; i < plat.ncpus && n < CK_APPLE_MAX_CPUS; i++)
            apple_smp_mpidr[n++] = plat.cpu[i].mpidr & CK_SMP_AFF_MASK;
    } else {
        int cpus = ck_fdt_path(&fdt, "/cpus");
        for (int c = cpus >= 0 ? ck_fdt_first_child(&fdt, cpus) : -1; c >= 0 && n < CK_APPLE_MAX_CPUS;
             c = ck_fdt_next_sibling(&fdt, c)) {
            const char *dt = ck_fdt_string(&fdt, c, "device_type");
            uint64_t a, s;
            if (dt && str_eq(dt, "cpu") && ck_fdt_reg(&fdt, c, 0, &a, &s) == 0) apple_smp_mpidr[n++] = a & CK_SMP_AFF_MASK;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        apple_smp_checkin[i] = 0;
        if (apple_smp_mpidr[i] == me) boot = i;
    }
    apple_smp_n = n;
    __asm__ volatile("dsb sy" ::: "memory");
    if (boot == CK_APPLE_MAX_CPUS) {
        pr("apple: secondaries not started: boot cpu mpidr=0x%llx not among %u cpu nodes\n", (unsigned long long)me, n);
        return verdict == CK_APPLE_OK;
    }
    if (verdict == CK_APPLE_OK) {
        for (uint32_t i = 0; i < n; i++) {
            if (i == boot) continue;
            int rc = ck_smp_spin_release(&fdt, &plat, i, me, (uint64_t)(uintptr_t)apple_secondary_entry,
                                         (uint64_t)(uintptr_t)__image_base, (uint64_t)(uintptr_t)__image_end, &smp_io,
                                         SMP_POLLS);
            started += rc == 0;
            smp_report(i, "spin-table", rc);
        }
        pr("apple: secondaries started=%u of %u (spin-table)\n", started, n - 1u);
        return 1;
    }
    if (!psci) {
        pr("apple: secondaries not started: no family-table row and no PSCI\n");
        return 1;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (i == boot) continue;
        uint64_t r = psci_call(psci, PSCI_CPU_ON_64, apple_smp_mpidr[i], (uint64_t)(uintptr_t)apple_secondary_entry, 0);
        int rc = (int64_t)r == 0 ? CK_SMP_E_TIMEOUT : CK_SMP_E_METHOD;
        for (uint32_t p = 0; (int64_t)r == 0 && p < SMP_POLLS; p++)
            if (smp_checkin(0, i)) {
                rc = 0;
                break;
            }
        started += rc == 0;
        smp_report(i, "psci CPU_ON", rc);
    }
    pr("apple: secondaries started=%u of %u (psci CPU_ON, QEMU path: the spin table needs an Apple loader)\n",
       started, n - 1u);
    return started == n - 1u;
}
void apple_main(uint64_t dtb, uint64_t entry_el, uint64_t e2h)
{
    unsigned el = current_el();
    if (el == 1) {
        wr(vbar_el1, (uint64_t)(uintptr_t)apple_vectors);
        __asm__ volatile("isb");
    }
    int rc = CK_FDT_E_MAGIC;
    const uint8_t *b = (const uint8_t *)(uintptr_t)dtb;
    if (b && be32(b) == CK_FDT_MAGIC) {
        uint32_t total = be32(b + 4);
        rc = ck_fdt_open(&fdt, b, total <= CK_FDT_MAX_SIZE ? total : 0);
    }
    if (rc != 0 || ck_apple_uart_find(&fdt, &uart) != 0) apple_stop(); /* nowhere to report */

    int ok = 1;
    pr("\nAIENOS_APPLE_IMAGE: entry el=%llu e2h=%llu dtb=0x%llx image=0x%llx-0x%llx\n",
       (unsigned long long)entry_el, (unsigned long long)e2h, (unsigned long long)dtb,
       (unsigned long long)(uintptr_t)__image_base, (unsigned long long)(uintptr_t)__image_end);
    if (entry_el == 2) {
        pr("apple: el2->el1 %s (CurrentEL=%u, HCR_EL2.E2H=%llu%s)\n", el == 1 ? "done" : "FAILED", el,
           (unsigned long long)e2h, e2h ? ", SCTLR_EL12 path" : "");
        ok &= el == 1;
    } else {
        pr("apple: entered at EL%llu (CurrentEL=%u)\n", (unsigned long long)entry_el, el);
        ok &= entry_el == 1 && el == 1;
    }
    pr("apple: fdt ok version=%u nodes=%u props=%u bytes=%u boot_cpuid=%u\n", fdt.version, fdt.nodes, fdt.props,
       fdt.total, fdt.boot_cpuid);
    pr("apple: console %s at 0x%llx (stdout-path -> %s)\n", ck_apple_uart_name(uart.kind),
       (unsigned long long)uart.base, uart.path);
    for (int m = ck_fdt_next_node(&fdt, -1); m >= 0; m = ck_fdt_next_node(&fdt, m)) {
        const char *dt = ck_fdt_string(&fdt, m, "device_type");
        uint64_t a, s;
        if (dt && str_eq(dt, "memory") && ck_fdt_reg_cpu(&fdt, m, 0, &a, &s) == 0)
            pr("apple: memory 0x%llx+0x%llx (%s)\n", (unsigned long long)a, (unsigned long long)s,
               ck_fdt_name(&fdt, m));
    }
    uint32_t nrsv = 0;
    for (uint64_t a, s; ck_fdt_rsv(&fdt, nrsv, &a, &s) == 0 && nrsv < 64u;) nrsv++;
    pr("apple: memreserve entries=%u\n", nrsv);

    if (el == 1) {
        __asm__ volatile("brk #0x50" ::: "memory");
        pr("apple: vectors %s (brk at EL1 taken=%u, returned)\n", brk_taken == 1 ? "ok" : "FAIL", brk_taken);
        ok &= brk_taken == 1;
        uint64_t freq = 0, ticks = 0;
        int t = timer_check(&freq, &ticks);
        pr("apple: timer %s cntfrq=%llu el1 physical timer fired after %llu ticks (polled, interrupt masked)\n",
           t == 0 ? "ok" : "FAIL", (unsigned long long)freq, (unsigned long long)ticks);
        ok &= t == 0;
    }

    int verdict = ck_apple_probe(&fdt, &plat);
    char line[320];
    ck_apple_report(&plat, line, sizeof(line));
    pr("%s\n", line);
    uint32_t rows = 0;
    while (ck_apple_soc_at(rows)) rows++;
    pr("apple: family table rows=%u\n", rows);
    /* FIQ sources this CPU reports right now (architected timers only on a
     * non-Apple CPU: the Apple IMP-DEF registers are read only on a table row). */
    uint32_t src = ck_aic_fiq_pending_live(verdict == CK_APPLE_OK ? plat.soc : 0);
    pr("apple: fiq sources now=0x%x (timer bits only off-Apple)\n", src);
    if (verdict == CK_APPLE_OK) {
        int arc = ck_aic_init(&aic, &ck_aic_io_live, plat.soc->aic, plat.aic_base, plat.aic_size, plat.aic_event,
                              plat.aic_event_size);
        aic_up = arc == 0;
        pr("apple: aic v%u %s nr_irq=%u dies=%u (every IRQ masked)\n", plat.soc->aic, arc == 0 ? "up" : "REFUSED",
           aic.nr_irq, aic.nr_die);
    }
    int psci = ck_fdt_path(&fdt, "/psci");
    const char *method = psci >= 0 ? ck_fdt_string(&fdt, psci, "method") : 0;
    if (method && !str_eq(method, "smc") && !str_eq(method, "hvc")) method = 0;
    ok &= start_secondaries(verdict, method);
    uint8_t idle = ck_apple_idle_effective(verdict == CK_APPLE_OK ? plat.soc : 0, 0);
    pr("apple: idle policy in force=%s (no state-retention setup built yet)\n", ck_apple_idle_name(idle));
    pr("AIENOS_APPLE_IMAGE: %s\n", ok ? "PASS" : "FAIL");

    if (method) {
        pr("apple: psci SYSTEM_OFF via %s\n", method);
        psci_call(method, PSCI_SYSTEM_OFF, 0, 0, 0);
        pr("apple: psci SYSTEM_OFF returned\n");
    } else {
        pr("apple: no PSCI in the tree (Apple: no EL3); stopping with interrupts masked\n");
    }
    if (idle == CK_APPLE_IDLE_SPIN) {
        __asm__ volatile("msr daifset, #0xf" ::: "memory");
        for (;;) __asm__ volatile("yield");
    }
    apple_stop();
}
