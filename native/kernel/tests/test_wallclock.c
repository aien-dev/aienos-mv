/* test_wallclock.c -- host tests of core/wallclock.c (L200): EFI_TIME to Unix seconds
 * against reference values from GNU `date -u -d ... +%s` (generated when the test was
 * written, not computed by the code under test), leap-year edges, every field refusal,
 * the time zone rule, the round trip back to a civil date, and the counter-advanced clock. */
#include "ck_test.h"
#include "wallclock.h"

static struct ck_efi_time T(unsigned y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s)
{
    struct ck_efi_time t = {.year = (uint16_t)y, .month = (uint8_t)mo, .day = (uint8_t)d, .hour = (uint8_t)h,
                            .minute = (uint8_t)mi, .second = (uint8_t)s, .timezone = CK_EFI_UNSPECIFIED_TIMEZONE};
    return t;
}

static int64_t conv(struct ck_efi_time t, int *known)
{
    int64_t u = -1;
    int k = -1;
    int rc = ck_efi_time_to_unix(&t, &u, &k);
    if (known)
        *known = k;
    return rc ? INT64_MIN : u;
}

int main(void)
{
    static const struct { unsigned y, mo, d, h, mi, s; int64_t u; } v[] = {
        {1970, 1, 1, 0, 0, 0, 0},
        {1970, 1, 1, 0, 0, 1, 1},
        {1999, 12, 31, 23, 59, 59, 946684799},
        {2000, 2, 29, 12, 0, 0, 951825600},
        {2000, 3, 1, 0, 0, 0, 951868800},
        {2024, 2, 29, 23, 59, 59, 1709251199},
        {2026, 10, 20, 12, 34, 56, 1792499696},
        {2038, 1, 19, 3, 14, 8, 2147483648LL},
        {2100, 2, 28, 23, 59, 59, 4107542399LL},
        {2100, 3, 1, 0, 0, 0, 4107542400LL},
        {9999, 12, 31, 23, 59, 59, 253402300799LL},
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        int k = -1;
        int64_t u = conv(T(v[i].y, v[i].mo, v[i].d, v[i].h, v[i].mi, v[i].s), &k);
        CHECK(u == v[i].u && k == 0);
        struct ck_civil c;
        ck_unix_to_civil(v[i].u, &c);
        CHECK(c.year == (int)v[i].y && c.month == v[i].mo && c.day == v[i].d && c.hour == v[i].h &&
              c.minute == v[i].mi && c.second == v[i].s);
    }
    /* refusals: every field, the century leap rule, before 1970 */
    CHECK(conv(T(2100, 2, 29, 0, 0, 0), 0) == INT64_MIN);  /* 2100 is not a leap year */
    CHECK(conv(T(2000, 2, 30, 0, 0, 0), 0) == INT64_MIN);
    CHECK(conv(T(2023, 2, 29, 0, 0, 0), 0) == INT64_MIN);
    CHECK(conv(T(2026, 0, 1, 0, 0, 0), 0) == INT64_MIN);
    CHECK(conv(T(2026, 13, 1, 0, 0, 0), 0) == INT64_MIN);
    CHECK(conv(T(2026, 4, 31, 0, 0, 0), 0) == INT64_MIN);
    CHECK(conv(T(2026, 4, 0, 0, 0, 0), 0) == INT64_MIN);
    CHECK(conv(T(2026, 4, 1, 24, 0, 0), 0) == INT64_MIN);
    CHECK(conv(T(2026, 4, 1, 0, 60, 0), 0) == INT64_MIN);
    CHECK(conv(T(2026, 4, 1, 0, 0, 60), 0) == INT64_MIN);
    CHECK(conv(T(1969, 12, 31, 23, 59, 59), 0) == INT64_MIN);
    CHECK(conv(T(1900, 3, 1, 0, 0, 0), 0) == INT64_MIN);
    CHECK(conv(T(10000, 1, 1, 0, 0, 0), 0) == INT64_MIN);
    CHECK(conv(T(0, 0, 0, 0, 0, 0), 0) == INT64_MIN); /* all-zero reading from a dead RTC */
    {
        struct ck_efi_time t = T(2026, 10, 20, 12, 34, 56);
        t.nanosecond = 1000000000u;
        CHECK(conv(t, 0) == INT64_MIN);
        t.nanosecond = 999999999u;
        CHECK(conv(t, 0) == 1792499696);
        /* time zone: unspecified is read as UTC; a given zone is applied as UTC = reading + minutes */
        int k = -1;
        t.timezone = 0;
        CHECK(conv(t, &k) == 1792499696 && k == 1);
        t.timezone = 60;
        CHECK(conv(t, &k) == 1792499696 + 3600 && k == 1);
        t.timezone = -300;
        CHECK(conv(t, &k) == 1792499696 - 18000 && k == 1);
        t.timezone = 1440;
        CHECK(conv(t, 0) == 1792499696 + 86400);
        t.timezone = 1441;
        CHECK(conv(t, 0) == INT64_MIN);
        t.timezone = -1441;
        CHECK(conv(t, 0) == INT64_MIN);
        /* a zone that pushes before the epoch is refused */
        struct ck_efi_time e = T(1970, 1, 1, 0, 0, 0);
        e.timezone = -60;
        CHECK(conv(e, 0) == INT64_MIN);
    }
    CHECK(ck_efi_time_to_unix(NULL, &(int64_t){0}, NULL) == -1);
    CHECK(ck_efi_time_to_unix(&(struct ck_efi_time){.year = 2026, .month = 1, .day = 1, .timezone = 2047}, NULL, NULL) == -1);
    /* round trip over every day boundary for a few years, including 2100 */
    for (int64_t s = 951782400; s < 951782400 + 400LL * 86400; s += 86399) {
        struct ck_civil c;
        ck_unix_to_civil(s, &c);
        struct ck_efi_time t = T((unsigned)c.year, c.month, c.day, c.hour, c.minute, c.second);
        if (conv(t, 0) != s) {
            CHECK(0);
            break;
        }
    }
    /* the counter-advanced clock */
    {
        struct ck_wallclock w = {0};
        struct ck_efi_time t = T(2026, 10, 20, 12, 34, 56);
        int64_t now = -1;
        CHECK(ck_wallclock_now(&w, 100, &now) == -1); /* nothing set */
        ck_wallclock_set(&w, &t, 1000, 0);
        CHECK(!w.valid);                               /* counter frequency 0 refused */
        t.month = 13;
        ck_wallclock_set(&w, &t, 1000, 1000);
        CHECK(!w.valid);                               /* invalid reading refused */
        t.month = 10;
        ck_wallclock_set(&w, &t, 1000, 1000000);       /* 1 MHz counter read at tick 1000 */
        CHECK(w.valid && !w.tz_known && w.unix0 == 1792499696);
        CHECK(ck_wallclock_now(&w, 1000, &now) == 0 && now == 1792499696);
        CHECK(ck_wallclock_now(&w, 1000 + 999999, &now) == 0 && now == 1792499696);       /* just under 1 s */
        CHECK(ck_wallclock_now(&w, 1000 + 1000000, &now) == 0 && now == 1792499697);
        CHECK(ck_wallclock_now(&w, 1000 + 3600ull * 1000000, &now) == 0 && now == 1792499696 + 3600);
        CHECK(ck_wallclock_now(&w, 999, &now) == -1);  /* counter went backwards */
        CHECK(ck_wallclock_now(&w, 5000, NULL) == -1);
    }
    return ck_t_verdict("test_wallclock");
}
