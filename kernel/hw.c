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
}

/* PCI configuration space + BAR/MMIO helpers now live in kernel/pci.c. */
