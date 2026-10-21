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
#include "fdt.h"
#include "fmt.h"

struct apple_frame {
    uint64_t x[31];
    uint64_t elr, spsr, pad;
};
_Static_assert(sizeof(struct apple_frame) == 272, "apple_frame matches apple_entry.S");

extern char __image_base[], __image_end[], apple_vectors[];
__attribute__((noreturn)) void apple_stop(void);
__attribute__((noreturn)) void apple_main(uint64_t dtb, uint64_t entry_el, uint64_t e2h);
void apple_exception(uint64_t idx, struct apple_frame *fr);

#define rd(reg) ({ uint64_t v_; __asm__ volatile("mrs %0, " #reg : "=r"(v_)); v_; })
#define wr(reg, v) __asm__ volatile("msr " #reg ", %0" ::"r"((uint64_t)(v)) : "memory")

static struct ck_apple_uart uart;
static struct ck_fdt fdt;
static struct ck_apple_platform plat;
static volatile uint32_t brk_taken;

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

static void psci_off(const char *method)
{
    register uint64_t x0 __asm__("x0") = 0x84000008u; /* PSCI SYSTEM_OFF (DEN 0022) */
    if (method[0] == 's')
        __asm__ volatile("smc #0" : "+r"(x0)::"memory");
    else
        __asm__ volatile("hvc #0" : "+r"(x0)::"memory");
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
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
    uint8_t idle = ck_apple_idle_effective(verdict == CK_APPLE_OK ? plat.soc : 0, 0);
    pr("apple: idle policy in force=%s (no state-retention setup built yet)\n", ck_apple_idle_name(idle));
    pr("AIENOS_APPLE_IMAGE: %s\n", ok ? "PASS" : "FAIL");

    int psci = ck_fdt_path(&fdt, "/psci");
    const char *method = psci >= 0 ? ck_fdt_string(&fdt, psci, "method") : 0;
    if (method && (str_eq(method, "smc") || str_eq(method, "hvc"))) {
        pr("apple: psci SYSTEM_OFF via %s\n", method);
        psci_off(method);
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
