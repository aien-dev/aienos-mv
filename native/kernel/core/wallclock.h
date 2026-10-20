/* wallclock.h -- the wall-clock date for AienOS (L200). Pure functions over
 * values, no I/O: the boot stub reads the firmware clock once through the UEFI
 * GetTime service before ExitBootServices (native/boot/efi_time.c; the Spark's
 * rtc0 is Linux's rtc-efi driver, i.e. this same firmware service, OBSERVED
 * in sysfs), the kernel never calls a runtime service. The stub leaves the
 * reading in a static in the shared image (the way the console state travels,
 * docs/BOOT_HANDOFF_CONTRACT.md), and the kernel advances it with the generic
 * counter, so no handoff layout changes. */
#ifndef AIENOS_CK_WALLCLOCK_H
#define AIENOS_CK_WALLCLOCK_H

#include <stdint.h>

#define CK_EFI_UNSPECIFIED_TIMEZONE 2047 /* UEFI EFI_UNSPECIFIED_TIMEZONE (0x7FF) */

/* EFI_TIME, UEFI 2.x section 8.3 (16 bytes). */
struct ck_efi_time {
    uint16_t year;       /* 1900 - 9999 */
    uint8_t month;       /* 1 - 12 */
    uint8_t day;         /* 1 - 31 */
    uint8_t hour;        /* 0 - 23 */
    uint8_t minute;      /* 0 - 59 */
    uint8_t second;      /* 0 - 59 */
    uint8_t pad1;
    uint32_t nanosecond; /* 0 - 999,999,999 */
    int16_t timezone;    /* -1440 to 1440 or CK_EFI_UNSPECIFIED_TIMEZONE */
    uint8_t daylight;
    uint8_t pad2;
};

struct ck_civil {
    int year;
    unsigned month, day, hour, minute, second;
};

/* Validates every field and converts to Unix seconds (UTC). An unspecified
 * time zone is taken as UTC (what Linux's rtc-efi does); *tz_known says which.
 * A specified zone is applied as UTC = reading + timezone minutes (the EDK2
 * EfiTimeToEpoch rule). Years before 1970 are refused. 0 ok, -1 invalid. */
int ck_efi_time_to_unix(const struct ck_efi_time *t, int64_t *unix_s, int *tz_known);
/* Unix seconds (>= 0) to a civil UTC date. */
void ck_unix_to_civil(int64_t unix_s, struct ck_civil *c);

/* The boot reading: valid only when the stub got and validated a time. */
struct ck_wallclock {
    int valid;
    int tz_known;
    int16_t timezone;
    uint8_t daylight;
    int64_t unix0;      /* UTC seconds at the reading */
    uint64_t cnt0;      /* generic counter ticks at the reading */
    uint64_t freq;      /* counter frequency, Hz */
    uint64_t status;    /* GetTime status when not valid (0 when valid) */
};

/* Fill from a firmware reading; valid stays 0 on an invalid time or freq 0. */
void ck_wallclock_set(struct ck_wallclock *w, const struct ck_efi_time *t, uint64_t cnt, uint64_t freq);
/* UTC seconds now, given the counter now. 0 ok, -1 not valid or counter went back. */
int ck_wallclock_now(const struct ck_wallclock *w, uint64_t cnt_now, int64_t *unix_s);

/* The one reading taken by the stub (zero-initialized). */
extern struct ck_wallclock ck_wallclock_boot;

#endif
