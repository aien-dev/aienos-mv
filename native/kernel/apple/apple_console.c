/* apple_console.c -- see apple_console.h. */
#include "apple_console.h"

#define UART_SPIN_MAX 200000u /* polls per character before it is dropped */

const char *ck_apple_uart_name(uint32_t kind)
{
    return kind == CK_APPLE_UART_PL011 ? "pl011" : kind == CK_APPLE_UART_S5L ? "s5l" : "none";
}

static void copy_bounded(char *dst, uint32_t cap, const char *src, uint32_t len)
{
    uint32_t i = 0;
    for (; i < len && i + 1u < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

int ck_apple_uart_find(const struct ck_fdt *f, struct ck_apple_uart *u)
{
    u->kind = CK_APPLE_UART_NONE;
    u->base = 0;
    u->node = CK_FDT_E_NOTFOUND;
    u->path[0] = 0;
    const char *sp = ck_fdt_string(f, ck_fdt_path(f, "/chosen"), "stdout-path");
    if (!sp) return CK_FDT_E_NOTFOUND;
    uint32_t len = 0;
    while (sp[len] && sp[len] != ':') len++; /* "serial0:115200n8" -> "serial0" */
    char name[96];
    copy_bounded(name, sizeof(name), sp, len);
    const char *path = name;
    if (name[0] != '/') {
        path = ck_fdt_string(f, ck_fdt_path(f, "/aliases"), name);
        if (!path) return CK_FDT_E_NOTFOUND;
    }
    copy_bounded(u->path, sizeof(u->path), path, sizeof(u->path));
    int node = ck_fdt_path(f, path);
    if (node < 0) return CK_FDT_E_NOTFOUND;
    uint32_t kind = ck_fdt_compatible(f, node, "arm,pl011")        ? CK_APPLE_UART_PL011
                    : ck_fdt_compatible(f, node, "apple,s5l-uart") ? CK_APPLE_UART_S5L
                                                                   : CK_APPLE_UART_NONE;
    if (kind == CK_APPLE_UART_NONE) return CK_FDT_E_NOTFOUND;
    uint64_t base, size;
    int rc = ck_fdt_reg_cpu(f, node, 0, &base, &size);
    if (rc) return rc;
    if (size < 0x24u) return CK_FDT_E_RANGE; /* both kinds use registers up to 0x20 */
    u->kind = kind;
    u->base = base;
    u->node = node;
    return 0;
}

static uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static void wr32(uint64_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }

void ck_apple_uart_putc(const struct ck_apple_uart *u, char c)
{
    if (u->kind == CK_APPLE_UART_PL011) {
        for (uint32_t i = 0; i < UART_SPIN_MAX && (rd32(u->base + 0x18u) & (1u << 5)); i++) {
        }
        wr32(u->base, (uint8_t)c);
    } else if (u->kind == CK_APPLE_UART_S5L) {
        for (uint32_t i = 0; i < UART_SPIN_MAX && !(rd32(u->base + 0x10u) & (1u << 1)); i++) {
        }
        wr32(u->base + 0x20u, (uint8_t)c);
    }
}

void ck_apple_uart_puts(const struct ck_apple_uart *u, const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') ck_apple_uart_putc(u, '\r');
        ck_apple_uart_putc(u, *s);
    }
}
