#pragma once
#include <stddef.h>
/* <time.h> -- BoltOS freestanding C library (libc/time.c). Backed by the RTC. */

typedef long time_t;
typedef long clock_t;
#define CLOCKS_PER_SEC 1000

struct tm {
    int tm_sec;    /* 0-60 */
    int tm_min;    /* 0-59 */
    int tm_hour;   /* 0-23 */
    int tm_mday;   /* 1-31 */
    int tm_mon;    /* 0-11 */
    int tm_year;   /* years since 1900 */
    int tm_wday;   /* 0-6, Sunday=0 */
    int tm_yday;   /* 0-365 */
    int tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

struct timespec { time_t tv_sec; long tv_nsec; };
struct timeval  { time_t tv_sec; long tv_usec; };

time_t      time(time_t *t);
clock_t     clock(void);
double      difftime(time_t a, time_t b);
time_t      mktime(struct tm *tm);
time_t      timegm(struct tm *tm);
struct tm  *gmtime(const time_t *t);
struct tm  *gmtime_r(const time_t *t, struct tm *out);
struct tm  *localtime(const time_t *t);
struct tm  *localtime_r(const time_t *t, struct tm *out);
char       *asctime(const struct tm *tm);
char       *ctime(const time_t *t);
size_t      strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
int         clock_gettime(int clk, struct timespec *ts);
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
