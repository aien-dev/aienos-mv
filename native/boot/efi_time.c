/* efi_time.c -- read the firmware wall clock once, before ExitBootServices (L200).
 * UEFI GetTime (UEFI 2.x section 8.3) is the same service Linux's rtc-efi driver
 * wraps, and the Spark's rtc0 is that driver (OBSERVED in sysfs), so this is the
 * one clock the Spark exposes; it works whatever RTC chip sits behind it. The
 * kernel never calls a runtime service: the reading lands in ck_wallclock_boot
 * (core/wallclock.c, a static in the shared image) and the kernel advances it
 * with the generic counter. Read-only: SetTime is never called. */
#include "efi.h"
#include "../kernel/arch/arch.h"
#include "../kernel/core/ck_internal.h"
#include "../kernel/core/wallclock.h"

void ck_boot_wallclock_read(EFI_SYSTEM_TABLE *st)
{
    EFI_RUNTIME_SERVICES *rt = st ? st->RuntimeServices : 0;
    struct ck_wallclock *w = &ck_wallclock_boot;
    struct ck_efi_time t;
    EFI_TIME_CAPABILITIES caps;
    w->valid = 0;
    w->status = 0x8000000000000003ull; /* EFI_UNSUPPORTED: no GetTime */
    if (!rt || !rt->GetTime)
        return;
    uint64_t cnt = ck_rd(cntpct_el0);
    EFI_STATUS s = rt->GetTime((EFI_TIME *)&t, &caps);
    if (s != EFI_SUCCESS) {
        w->status = s;
        return;
    }
    ck_wallclock_set(w, &t, cnt, ck_rd(cntfrq_el0));
    if (!w->valid)
        w->status = 0x8000000000000002ull; /* EFI_INVALID_PARAMETER: reading failed validation */
}
