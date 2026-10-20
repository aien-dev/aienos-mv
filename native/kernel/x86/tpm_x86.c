/* tpm_x86.c -- TPM 2.0 CRB probe for the x86_64 kernel (L200). QEMU q35 has the
 * tpm-crb device (the aarch64 virt machine has none), so this is where the
 * driver in ../dev/tpm_crb.c is exercised inside a booted kernel with a real
 * swtpm behind it (scripts/qemu_ck_tpm_crb_test.sh). It runs only when the
 * firmware lists an ACPI TPM2 table with start method 7; with no table the
 * kernel prints "tpm: none" and touches nothing. QEMU qualifies nothing
 * physical. */
#include "../core/ck_internal.h"
#include "../core/acpi.h"
#include "../dev/tpm_crb.h"
#include "x86.h"

#define NV_INDEX 0x01500016u

static uint64_t hz;

static uint32_t m_rd32(void *c, uint32_t off) { return *(volatile uint32_t *)((uintptr_t)c + off); }
static void m_wr32(void *c, uint32_t off, uint32_t v) { *(volatile uint32_t *)((uintptr_t)c + off) = v; }
static void m_rd_buf(void *c, uint32_t off, uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++)
        d[i] = *(volatile uint8_t *)((uintptr_t)c + off + i);
}
static void m_wr_buf(void *c, uint32_t off, const uint8_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        *(volatile uint8_t *)((uintptr_t)c + off + i) = s[i];
}
static void m_delay(void *c, uint32_t us)
{
    (void)c;
    uint64_t t0 = ck_rdtsc(), n = hz / 1000000 * us;
    while (ck_rdtsc() - t0 < n)
        ck_pause();
}

static void hex(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ck_printf("%02x", p[i]);
}

void ck_x86_tpm_probe(uint64_t rsdp, uint64_t tsc_hz)
{
    const uint8_t *tbl = rsdp ? ck_acpi_lookup(rsdp, "TPM2") : 0;
    uint64_t base = 0;
    uint32_t method = 0;
    if (!tbl) {
        ck_puts("tpm: none (no ACPI TPM2 table)\n");
        return;
    }
    hz = tsc_hz;
    size_t len = (size_t)tbl[4] | (size_t)tbl[5] << 8 | (size_t)tbl[6] << 16 | (size_t)tbl[7] << 24;
    int rc = ck_tpm2_acpi_parse(tbl, len, &base, &method);
    if (rc) {
        ck_printf("tpm: refused acpi rc=%d start_method=%u\n", rc, method);
        return;
    }
    ck_printf("tpm: acpi TPM2 start_method=%u crb_base=0x%llx\n", method, (unsigned long long)base);
    if (ck_x86_map_mmio(base, 0x1000)) {
        ck_puts("tpm: refused (cannot map the CRB window)\n");
        return;
    }
    struct ck_tpm_crb t;
    struct ck_tpm_crb_ops ops = {m_rd32, m_wr32, m_rd_buf, m_wr_buf, m_delay, (void *)(uintptr_t)base};
    rc = ck_tpm_crb_open(&t, &ops, base);
    if (rc) {
        ck_printf("tpm: open FAILED rc=%d\n", rc);
        return;
    }
    ck_printf("tpm: crb open vid=0x%04x did=0x%04x cmd=0x%x+0x%x rsp=0x%x+0x%x\n", t.vid, t.did, t.cmd_off,
              t.cmd_size, t.rsp_off, t.rsp_size);
    rc = ck_tpm2_startup(&t, CK_TPM_SU_CLEAR);
    ck_printf("tpm: startup rc=0x%x%s\n", (unsigned)rc, rc == (int)CK_TPM_RC_INITIALIZE ? " (already started by firmware)" : "");
    uint8_t r[16], d[32], nv[32];
    int zero = 1;
    rc = ck_tpm2_get_random(&t, r, sizeof r);
    for (unsigned i = 0; i < sizeof r; i++)
        if (r[i])
            zero = 0;
    ck_printf("tpm: getrandom rc=%d bytes=%u allzero=%s\n", rc, (unsigned)sizeof r, zero ? "yes" : "no");
    for (unsigned pcr = 0; pcr < 8; pcr += 7) {
        uint32_t cnt = 0;
        rc = ck_tpm2_pcr_read_sha256(&t, pcr, d, &cnt);
        ck_printf("tpm: pcr%u rc=%d sha256=", pcr, rc);
        if (rc == 0)
            hex(d, 32);
        ck_puts("\n");
    }
    rc = ck_tpm2_nv_read(&t, NV_INDEX, NV_INDEX, 0, nv, sizeof nv);
    ck_printf("tpm: nv_read 0x%x rc=0x%x data=", NV_INDEX, (unsigned)rc);
    if (rc == 0)
        hex(nv, sizeof nv);
    ck_puts("\n");
    ck_tpm_crb_close(&t);
    ck_puts("tpm: probe done\n");
}
