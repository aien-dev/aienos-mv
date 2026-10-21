/* apple_console.h -- serial console of the CK_APPLE=1 image, found from the
 * device tree's /chosen stdout-path (a path, or an /aliases name, with an
 * optional ":options" tail). Two UART kinds:
 *   - "arm,pl011" (QEMU virt): UARTDR 0x000, UARTFR 0x018 bit 5 TXFF
 *     (Arm PrimeCell UART PL011 TRM DDI 0183, sections 3.3.1 and 3.3.3);
 *   - "apple,s5l-uart" (Apple SoCs, Samsung-style): UTRSTAT 0x10 bit 1 TXFE
 *     (transmit buffer empty), UTXH 0x20 (register facts read from Linux
 *     include/linux/serial_s3c.h S3C2410_UTRSTAT, S3C2410_UTRSTAT_TXFE,
 *     S3C2410_UTXH; no code copied). NOT TESTED on Apple hardware.
 * Every wait is bounded: a UART that never drains drops characters, it never
 * stops the kernel. */
#ifndef AIENOS_CK_APPLE_CONSOLE_H
#define AIENOS_CK_APPLE_CONSOLE_H
#include <stdint.h>
#include "fdt.h"

#define CK_APPLE_UART_NONE 0u
#define CK_APPLE_UART_PL011 1u
#define CK_APPLE_UART_S5L 2u

struct ck_apple_uart {
    uint32_t kind;     /* CK_APPLE_UART_* */
    uint64_t base;     /* CPU physical address of the registers */
    int node;          /* device tree node, or a CK_FDT_E_* code */
    char path[96];     /* resolved node path as written in the tree, for the report */
};

/* Resolve /chosen stdout-path into *u. 0 ok; CK_FDT_E_NOTFOUND when there is
 * no stdout-path, the alias or node is missing, or the node is neither UART
 * kind (u->kind stays NONE). */
int ck_apple_uart_find(const struct ck_fdt *f, struct ck_apple_uart *u);

const char *ck_apple_uart_name(uint32_t kind);

/* MMIO output (kernel only; never called by the host tests). */
void ck_apple_uart_putc(const struct ck_apple_uart *u, char c);
void ck_apple_uart_puts(const struct ck_apple_uart *u, const char *s);

#endif
