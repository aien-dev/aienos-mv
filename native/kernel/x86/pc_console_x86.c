/* pc_console_x86.c -- AienOS PC edition (CK_X86_PLATFORM_PC): the console
 * that stays up after the boot report, on a real PC or in a virtual machine.
 *
 * Output goes where every kernel line goes (console_x86.c: the GOP screen and
 * COM1 when a 16550 answers). Input is polled 100 times a second from the
 * LAPIC tick, the core halting in between: COM1 receive, the virtio keyboard
 * (virtio_x86.c), and a PS/2 keyboard controller when one answers at ports
 * 0x60/0x64 (QEMU q35 has one; many PC firmwares emulate one for a USB
 * keyboard; Hyper-V generation 2 and the iMac have none, where status reads
 * 0xff and the port is left alone). Lines are assembled by core/line_rx.c (the
 * same rules as the AArch64 service console).
 *
 * The serial contract the Windows launcher (aienos-run, ledger L261) relies on:
 * the line "aienos: console ready" once, then the prompt "aienos> "; CR or LF ends a
 * line; "poweroff" powers off through ACPI S5 (a VM exits by itself);
 * "reboot" resets through the FADT reset register, then port 0xcf9, then the
 * keyboard controller; the ACPI power button (a VM's shut down request: QEMU
 * system_powerdown, Hyper-V Stop) powers off the same way as "poweroff".
 * Nothing here writes a disk or a firmware variable. */
#include "../core/ck_internal.h"
#include "../core/line_rx.h"
#include "x86.h"

extern const char ck_x86_keymap_lo[58], ck_x86_keymap_hi[58]; /* virtio_x86.c */

static const struct ck_x86_pc_status *st;
static uint64_t g_rsdp;
static void (*g_poweroff)(void);
static struct ck_line_rx line;
static int last_cr;
static int ps2_on, ps2_shift, ps2_e0;

static uint32_t rd32(const uint8_t *p, unsigned off)
{
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

static void prompt(void) { ck_puts("aienos> "); }

static void reboot(void)
{
    const uint8_t *fadt = g_rsdp ? ck_acpi_lookup(g_rsdp, "FACP") : 0;
    if (fadt && rd32(fadt, 4) >= 129 && (rd32(fadt, 112) & 1u << 10)) { /* RESET_REG_SUP */
        uint8_t space = fadt[116], val = fadt[128];
        uint64_t addr;
        memcpy(&addr, fadt + 120, 8);
        ck_printf("power: reset via the FADT reset register space=%u addr=0x%llx\n", space, (unsigned long long)addr);
        if (space == 1 && addr && addr <= 0xffff)
            ck_outb((uint16_t)addr, val);
        else if (space == 0 && addr)
            *(volatile uint8_t *)(uintptr_t)addr = val;
    }
    for (volatile unsigned i = 0; i < 10000000u; i++)
        ;
    ck_puts("power: reset via port 0xcf9\n");
    ck_outb(0xcf9, 0x02);
    ck_outb(0xcf9, 0x06);
    for (volatile unsigned i = 0; i < 10000000u; i++)
        ;
    if (ps2_on) {
        ck_puts("power: reset via the keyboard controller\n");
        ck_outb(0x64, 0xfe);
    }
    for (volatile unsigned i = 0; i < 10000000u; i++)
        ;
    ck_puts("power: reset did not happen; turn the machine or the VM off\n");
}

static void status(void)
{
    uint64_t up = st->tsc_hz ? ck_rdtsc() / st->tsc_hz : 0;
    const struct ck_x86_virtio_result *v = ck_x86_virtio_result();
    ck_printf("status: kernel=" CK_X86_TITLE " commit=%s cores=%u/%u apic=%s timebase=%s tsc_mhz=%llu\n",
              ck_commit(), st->cores_up, st->cores_listed, st->apic_mode, st->timebase,
              (unsigned long long)(st->tsc_hz / 1000000));
    ck_printf("status: free_mib=%llu tsc_seconds=%llu virtio_disks=%u net=%s tx_frames=%u rx_frames=%u "
              "keyboard=%s%s writes=none\n",
              (unsigned long long)(st->free_bytes >> 20), (unsigned long long)up, v->blk_disks,
              v->net_up ? "virtio" : "none", v->net_tx_frames, ck_x86_virtio_net_rx_frames(),
              v->kbd_up ? "virtio" : "", ps2_on ? (v->kbd_up ? "+ps2" : "ps2") : (v->kbd_up ? "" : "serial-only"));
}

static void run(const char *s)
{
    while (*s == ' ')
        s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ')
        n--;
    if (!n)
        return;
    if (n == 4 && !memcmp(s, "help", 4)) {
        ck_puts("commands: help, status, poweroff, reboot\n"
                "This is the first AienOS PC edition: the Aienux kernel, its console and read-only disk checks.\n"
                "It does not change any disk. There is no graphics driver and no AI model on the PC yet.\n");
    } else if (n == 6 && !memcmp(s, "status", 6)) {
        status();
    } else if (n == 8 && !memcmp(s, "poweroff", 8)) {
        g_poweroff();
        ck_puts("power: ACPI S5 did not power off; turn the machine or the VM off\n");
    } else if (n == 6 && !memcmp(s, "reboot", 6)) {
        reboot();
    } else {
        char w[41];
        size_t k = n < sizeof w - 1 ? n : sizeof w - 1;
        memcpy(w, s, k);
        w[k] = 0;
        ck_printf("unknown command: %s (type help)\n", w);
    }
}

static void feed(int b)
{
    if (b == '\n' && last_cr) { /* CR LF is one Enter */
        last_cr = 0;
        return;
    }
    last_cr = b == '\r';
    switch (ck_line_rx_feed(&line, b)) {
    case CK_LINE_ECHO: {
        char e[2] = {line.buf[line.len - 1], 0};
        ck_puts(e);
        break;
    }
    case CK_LINE_ERASE:
        ck_puts("\b \b");
        break;
    case CK_LINE_DONE:
        ck_puts("\n");
        run(line.buf);
        prompt();
        break;
    case CK_LINE_OVERLONG:
        ck_puts("\nrefused: line too long\n");
        prompt();
        break;
    case CK_LINE_DAMAGED:
        ck_puts("\nrefused: a byte was lost on the line\n");
        prompt();
        break;
    case CK_LINE_NONE:
        break;
    }
}

/* PS/2 controller, scan code set 1 as the firmware's translation delivers it. */
static void ps2_poll(void)
{
    for (unsigned k = 0; ps2_on && k < 32; k++) {
        uint8_t s = ck_inb(0x64);
        if (s == 0xff || !(s & 1))
            return;
        uint8_t c = ck_inb(0x60);
        if (s & 0x20)
            continue; /* a mouse byte */
        if (c == 0xe0) {
            ps2_e0 = 1;
            continue;
        }
        int e0 = ps2_e0, up = c & 0x80;
        ps2_e0 = 0;
        c &= 0x7f;
        if (c == 0x2a || c == 0x36) {
            ps2_shift = !up;
            continue;
        }
        if (up)
            continue;
        if (e0) {
            if (c == 0x1c)
                feed('\n'); /* keypad Enter */
            continue;
        }
        if (c < sizeof ck_x86_keymap_lo) {
            char ch = (ps2_shift ? ck_x86_keymap_hi : ck_x86_keymap_lo)[c];
            if (ch)
                feed(ch == '\b' ? 0x08 : ch);
        }
    }
}

void ck_x86_pc_console(const struct ck_x86_pc_status *s, uint64_t rsdp, void (*arm_tick)(void),
                       void (*poweroff)(void))
{
    st = s;
    g_rsdp = rsdp;
    g_poweroff = poweroff;
    ck_line_rx_init(&line);
    uint8_t ps = ck_inb(0x64);
    if (ps != 0xff) {
        while (ck_inb(0x64) & 1) /* drop what the firmware left */
            (void)ck_inb(0x60);
        ps2_on = 1;
    }
    ck_printf("keyboard: ps2=%s virtio=%s serial=%s\n", ps2_on ? "present" : "absent",
              ck_x86_virtio_result()->kbd_up ? "present" : "absent", "com1-if-present");
    ck_puts("note: QEMU qualifies nothing physical\n");
    ck_puts("aienos: console ready\n");
    prompt();
    arm_tick();
    for (;;) {
        __asm__ volatile("sti; hlt" ::: "memory"); /* the 100 Hz tick wakes the core */
        int c;
        for (unsigned k = 0; k < CK_LINE_RX_BURST && (c = ck_x86_com1_getc()) >= 0; k++)
            feed(c);
        while ((c = ck_x86_virtio_poll_key()) >= 0)
            feed(c);
        ps2_poll();
        if (ck_x86_power_button_pressed()) {
            ck_puts("\npower: power button pressed (the VM was asked to shut down)\n");
            g_poweroff();
            ck_puts("power: ACPI S5 did not power off; turn the machine or the VM off\n");
        }
    }
}
