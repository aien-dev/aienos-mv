/* efi_secureboot.c -- the PC edition's Secure Boot record (x86_64 PCKERN.EFI).
 *
 * Before ExitBootServices the stub reads two UEFI global variables and prints
 * them, so the boot report says what state the firmware was in:
 *   secure_boot: SecureBoot=<0|1|absent> SetupMode=<0|1|absent>
 * (UEFI 2.10 section 3.3, EFI_GLOBAL_VARIABLE 8BE4DF61-93CA-11D2-AA0D-00E098032B8C;
 * both are one byte, read-only to the OS). This is the only runtime-service
 * call in the PC edition and it is GetVariable: nothing is ever written, no
 * boot entry, no key, no variable (the Makefile refuses SetVariable here). */
#include "efi.h"
#include "../kernel/core/ck_internal.h"

typedef EFI_STATUS (*get_variable_fn)(CHAR16 *name, EFI_GUID *vendor, uint32_t *attributes, uint64_t *size,
                                      void *data);

static const char *one_byte(get_variable_fn get, const char *ascii, char out[8])
{
    static EFI_GUID global = {0x8be4df61, 0x93ca, 0x11d2, {0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c}};
    CHAR16 name[16];
    unsigned n = 0;
    for (; ascii[n] && n < 15; n++)
        name[n] = (CHAR16)ascii[n];
    name[n] = 0;
    uint8_t v[8] = {0};
    uint64_t size = sizeof v;
    uint32_t attr = 0;
    EFI_STATUS s = get(name, &global, &attr, &size, v);
    if (s != EFI_SUCCESS || size != 1)
        return "absent";
    out[0] = (char)('0' + (v[0] > 9 ? 9 : v[0]));
    out[1] = 0;
    return out;
}

void ck_boot_secureboot_report(EFI_SYSTEM_TABLE *st)
{
    const EFI_RUNTIME_SERVICES *rs = st ? (const EFI_RUNTIME_SERVICES *)st->RuntimeServices : 0;
    if (!rs || !rs->GetVariable) {
        ck_puts("secure_boot: SecureBoot=unknown SetupMode=unknown (no runtime services table)\n");
        return;
    }
    get_variable_fn get = (get_variable_fn)rs->GetVariable;
    char a[8], b[8];
    const char *sb = one_byte(get, "SecureBoot", a);
    const char *sm = one_byte(get, "SetupMode", b);
    ck_printf("secure_boot: SecureBoot=%s SetupMode=%s\n", sb, sm);
}
