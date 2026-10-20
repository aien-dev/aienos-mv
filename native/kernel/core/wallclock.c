/* wallclock.c -- see wallclock.h. */
#include "wallclock.h"

struct ck_wallclock ck_wallclock_boot;

static int leap(unsigned y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static unsigned mdays(unsigned y, unsigned m)
{
    static const unsigned d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29u : d[m - 1];
}

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian, civil-from-days family). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int ck_efi_time_to_unix(const struct ck_efi_time *t, int64_t *unix_s, int *tz_known)
{
    if (!t || !unix_s)
        return -1;
    if (t->year < 1970 || t->year > 9999 || t->month < 1 || t->month > 12 || t->day < 1 ||
        t->day > mdays(t->year, t->month) || t->hour > 23 || t->minute > 59 || t->second > 59 ||
        t->nanosecond > 999999999u)
        return -1;
    int known = t->timezone != CK_EFI_UNSPECIFIED_TIMEZONE;
    if (known && (t->timezone < -1440 || t->timezone > 1440))
        return -1;
    int64_t s = days_from_civil(t->year, t->month, t->day) * 86400 + (int64_t)t->hour * 3600 +
                (int64_t)t->minute * 60 + t->second;
    if (known)
        s += (int64_t)t->timezone * 60;
    if (s < 0)
        return -1;
    *unix_s = s;
    if (tz_known)
        *tz_known = known;
    return 0;
}

void ck_unix_to_civil(int64_t unix_s, struct ck_civil *c)
{
    int64_t days = unix_s / 86400;
    int64_t rem = unix_s % 86400;
    c->hour = (unsigned)(rem / 3600);
    c->minute = (unsigned)(rem % 3600 / 60);
    c->second = (unsigned)(rem % 60);
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    c->day = doy - (153 * mp + 2) / 5 + 1;
    c->month = mp < 10 ? mp + 3 : mp - 9;
    c->year = (int)(y + (c->month <= 2));
}

void ck_wallclock_set(struct ck_wallclock *w, const struct ck_efi_time *t, uint64_t cnt, uint64_t freq)
{
    int64_t s;
    int known = 0;
    w->valid = 0;
    if (!freq || ck_efi_time_to_unix(t, &s, &known))
        return;
    w->valid = 1;
    w->tz_known = known;
    w->timezone = t->timezone;
    w->daylight = t->daylight;
    w->unix0 = s;
    w->cnt0 = cnt;
    w->freq = freq;
    w->status = 0;
}

int ck_wallclock_now(const struct ck_wallclock *w, uint64_t cnt_now, int64_t *unix_s)
{
    if (!w || !w->valid || !unix_s || cnt_now < w->cnt0)
        return -1;
    *unix_s = w->unix0 + (int64_t)((cnt_now - w->cnt0) / w->freq);
    return 0;
}
