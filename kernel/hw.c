#include <stdint.h>
#include "hw.h"
#include "io.h"

/* ===========================================================================
 *  CPUID
 * ===========================================================================*/
uint32_t hw_cpu_max_leaf(void) {
    uint32_t a, b, c, d;
    cpuidx(0, 0, &a, &b, &c, &d);
    return a;
}

void hw_cpu_vendor(char out[13]) {
    uint32_t a, b, c, d;
    cpuidx(0, 0, &a, &b, &c, &d);
    *(uint32_t *)(out + 0) = b;
    *(uint32_t *)(out + 4) = d;
    *(uint32_t *)(out + 8) = c;
    out[12] = 0;
}

void hw_cpu_brand(char out[49]) {
    uint32_t a, b, c, d;
    cpuidx(0x80000000u, 0, &a, &b, &c, &d);
    if (a < 0x80000004u) { out[0] = 0; return; }
    uint32_t *o = (uint32_t *)out;
    for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
        cpuidx(leaf, 0, &a, &b, &c, &d);
        *o++ = a; *o++ = b; *o++ = c; *o++ = d;
    }
    out[48] = 0;
}

/* ===========================================================================
 *  CMOS real-time clock
 * ===========================================================================*/
static uint8_t cmos(uint8_t reg) {
    outb(0x70, reg);
    io_wait();
    return inb(0x71);
}
static int rtc_updating(void) {
    outb(0x70, 0x0A);
    return inb(0x71) & 0x80;
}
static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v & 0x0F) + ((v >> 4) * 10)); }

/* ---- timezone: US Pacific (western US) ---------------------------------- *
 * QEMU is launched with `-rtc base=utc`, so the CMOS holds UTC. We shift it to
 * America/Los_Angeles: PST = UTC-8, PDT = UTC-7 from the 2nd Sunday of March to
 * the 1st Sunday of November. Date arithmetic goes through a day count so the
 * offset rolls the day/month/year correctly across midnight and month ends. */

/* days since 1970-01-01 for a proleptic-Gregorian y/m/d (Howard Hinnant) */
static long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}
static void civil_from_days(long z, int *y, unsigned *m, unsigned *d) {
    z += 719468L;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yy = (int)yoe + (int)era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yy + (*m <= 2);
}
/* weekday 0=Sun..6=Sat (1970-01-01 was a Thursday = 4) */
static int weekday_from_days(long z) { int w = (int)((z % 7 + 4) % 7); return w < 0 ? w + 7 : w; }

/* Pacific UTC offset in hours for a given UTC date/time (-8 or -7) */
static int pacific_offset(int y, unsigned mo, unsigned d, unsigned h) {
    if (mo < 3 || mo > 11) return -8;          /* Dec-Feb: PST              */
    if (mo > 3 && mo < 11) return -7;          /* Apr-Oct: PDT              */
    long md = days_from_civil(y, mo, 1);
    int  dow1 = weekday_from_days(md);          /* weekday of the 1st        */
    if (mo == 3) {                              /* DST starts 2nd Sun 10:00Z */
        unsigned sun2 = (unsigned)(1 + ((7 - dow1) % 7) + 7);
        return (d > sun2 || (d == sun2 && h >= 10)) ? -7 : -8;
    }
    /* November: DST ends 1st Sun 09:00Z */
    unsigned sun1 = (unsigned)(1 + ((7 - dow1) % 7));
    return (d > sun1 || (d == sun1 && h >= 9)) ? -8 : -7;
}

static void apply_pacific(struct rtc_time *t) {
    int off = pacific_offset(t->year, t->mon, t->day, t->hour);
    long day = days_from_civil(t->year, t->mon, t->day);
    long sec = ((long)t->hour * 60 + t->min) * 60 + t->sec + (long)off * 3600;
    while (sec < 0)      { sec += 86400; day--; }
    while (sec >= 86400) { sec -= 86400; day++; }
    int y; unsigned m, d;
    civil_from_days(day, &y, &m, &d);
    t->year = (uint16_t)y; t->mon = (uint8_t)m; t->day = (uint8_t)d;
    t->hour = (uint8_t)(sec / 3600);
    t->min  = (uint8_t)((sec / 60) % 60);
    t->sec  = (uint8_t)(sec % 60);
}

void rtc_now(struct rtc_time *t) {
    int guard = 100000;
    while (rtc_updating() && guard--) { /* wait out the update window */ }

    uint8_t s  = cmos(0x00);
    uint8_t mi = cmos(0x02);
    uint8_t h  = cmos(0x04);
    uint8_t d  = cmos(0x07);
    uint8_t mo = cmos(0x08);
    uint8_t y  = cmos(0x09);
    uint8_t status_b = cmos(0x0B);

    if (!(status_b & 0x04)) {                 /* values are BCD */
        s  = bcd2bin(s);
        mi = bcd2bin(mi);
        d  = bcd2bin(d);
        mo = bcd2bin(mo);
        y  = bcd2bin(y);
        uint8_t pm = h & 0x80;
        h = (uint8_t)(bcd2bin(h & 0x7F) | pm);
    }
    if (!(status_b & 0x02) && (h & 0x80)) {   /* 12-hour mode, PM set */
        h = (uint8_t)(((h & 0x7F) + 12) % 24);
    } else {
        h &= 0x7F;
    }

    t->sec = s; t->min = mi; t->hour = h;
    t->day = d; t->mon = mo; t->year = (uint16_t)(2000 + y);

    apply_pacific(t);            /* CMOS is UTC -> shift to US Pacific (PST/PDT) */
}

/* PCI configuration space + BAR/MMIO helpers now live in kernel/pci.c. */
