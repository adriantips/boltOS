/* ===========================================================================
 *  libc/time.c -- calendar/clock for BoltOS's freestanding libc.
 *  Wall-clock comes from the CMOS RTC (rtc_now); a monotonic millisecond clock
 *  comes from the PIT tick counter. Epoch math uses Howard Hinnant's civil-day
 *  algorithm so the full 1970.. range round-trips.
 * ===========================================================================*/
#include <stdint.h>
#include "time.h"
#include "string.h"
#include "stdio.h"
#include "hw.h"
#include "pit.h"

/* days since 1970-01-01 for a proleptic-Gregorian y/m/d (m in 1..12) */
static long days_from_civil(long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
static void civil_from_days(long z, int *y, int *m, int *d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp + (mp < 10 ? 3 : -9));
    *y = (int)(yy + (*m <= 2));
}

time_t timegm(struct tm *tm) {
    long days = days_from_civil(tm->tm_year + 1900, (unsigned)(tm->tm_mon + 1), (unsigned)tm->tm_mday);
    return (time_t)days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}
time_t mktime(struct tm *tm) { return timegm(tm); }   /* C locale, UTC */

struct tm *gmtime_r(const time_t *tp, struct tm *out) {
    time_t t = *tp;
    long days = (long)(t / 86400); long rem = (long)(t % 86400);
    if (rem < 0) { rem += 86400; days--; }
    out->tm_hour = (int)(rem / 3600); out->tm_min = (int)((rem % 3600) / 60); out->tm_sec = (int)(rem % 60);
    int y, m, d; civil_from_days(days, &y, &m, &d);
    out->tm_year = y - 1900; out->tm_mon = m - 1; out->tm_mday = d;
    int wd = (int)((days % 7 + 4) % 7); if (wd < 0) wd += 7;   /* 1970-01-01 = Thu */
    out->tm_wday = wd;
    out->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    out->tm_isdst = 0; out->tm_gmtoff = 0; out->tm_zone = "UTC";
    return out;
}
static struct tm tmbuf;
struct tm *gmtime(const time_t *t)    { return gmtime_r(t, &tmbuf); }
struct tm *localtime(const time_t *t) { return gmtime_r(t, &tmbuf); }
struct tm *localtime_r(const time_t *t, struct tm *o) { return gmtime_r(t, o); }

time_t time(time_t *t) {
    struct rtc_time r; rtc_now(&r);
    struct tm tm = {0};
    tm.tm_year = r.year - 1900; tm.tm_mon = r.mon - 1; tm.tm_mday = r.day;
    tm.tm_hour = r.hour; tm.tm_min = r.min; tm.tm_sec = r.sec;
    time_t v = timegm(&tm);
    if (t) *t = v;
    return v;
}
clock_t clock(void) {
    uint32_t hz = pit_hz(); if (!hz) hz = 1000;
    return (clock_t)(pit_ticks() * 1000 / hz);          /* CLOCKS_PER_SEC = 1000 */
}
int clock_gettime(int clk, struct timespec *ts) {
    (void)clk;
    if (!ts) return -1;
    uint32_t hz = pit_hz(); if (!hz) hz = 1000;
    uint64_t tk = pit_ticks();
    ts->tv_sec = (time_t)(tk / hz);
    ts->tv_nsec = (long)((tk % hz) * (1000000000ull / hz));
    return 0;
}
double difftime(time_t a, time_t b) { return (double)(a - b); }

static const char *wdays[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *mons[]  = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
char *asctime(const struct tm *tm) {
    static char b[32];
    snprintf(b, sizeof b, "%s %s %2d %02d:%02d:%02d %d\n",
             wdays[tm->tm_wday % 7], mons[tm->tm_mon % 12], tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    return b;
}
char *ctime(const time_t *t) { return asctime(gmtime(t)); }

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    size_t n = 0;
    #define PUT(c) do { if (n + 1 < max) s[n] = (c); n++; } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        char tmp[16]; int tl = 0;
        switch (*fmt) {
        case 'Y': tl = snprintf(tmp, sizeof tmp, "%d", tm->tm_year + 1900); break;
        case 'm': tl = snprintf(tmp, sizeof tmp, "%02d", tm->tm_mon + 1); break;
        case 'd': tl = snprintf(tmp, sizeof tmp, "%02d", tm->tm_mday); break;
        case 'H': tl = snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour); break;
        case 'M': tl = snprintf(tmp, sizeof tmp, "%02d", tm->tm_min); break;
        case 'S': tl = snprintf(tmp, sizeof tmp, "%02d", tm->tm_sec); break;
        case 'a': tl = snprintf(tmp, sizeof tmp, "%s", wdays[tm->tm_wday % 7]); break;
        case 'b': case 'h': tl = snprintf(tmp, sizeof tmp, "%s", mons[tm->tm_mon % 12]); break;
        case 'j': tl = snprintf(tmp, sizeof tmp, "%03d", tm->tm_yday + 1); break;
        case '%': tmp[0] = '%'; tl = 1; break;
        default:  tmp[0] = '%'; tmp[1] = *fmt; tl = 2; break;
        }
        for (int i = 0; i < tl; i++) PUT(tmp[i]);
    }
    if (max) s[n < max ? n : max - 1] = 0;
    #undef PUT
    return n;
}
