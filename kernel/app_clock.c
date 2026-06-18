/* ===========================================================================
 *  BoltOS  -  kernel/app_clock.c
 *  Clock window: a live analog clock face with hour/minute/second hands, plus
 *  a big digital readout and the date underneath. Time comes from the CMOS RTC.
 *
 *  No hardware FP in the kernel, so the hands are placed from a 60-entry integer
 *  sine table (scaled by 1000); one tick = 6 degrees. cos(t) = sin(t + 15 ticks).
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "hw.h"
#include "pit.h"
#include "string.h"

/* sin(tick * 6 deg) * 1000, tick 0..59 (0 = 12 o'clock, increasing clockwise) */
static const int16_t SIN60[60] = {
    0,105,208,309,407,500,588,669,743,809,866,914,951,978,995,1000,995,978,951,914,
    866,809,743,669,588,500,407,309,208,105,0,-105,-208,-309,-407,-500,-588,-669,-743,-809,
    -866,-914,-951,-978,-995,-1000,-995,-978,-951,-914,-866,-809,-743,-669,-588,-500,-407,-309,-208,-105
};
static int sinv(int t) { return SIN60[((t % 60) + 60) % 60]; }
static int cosv(int t) { return sinv(t + 15); }

/* draw a thick line from (x0,y0) to (x1,y1) as a chain of small filled dots */
static void thick_line(int x0, int y0, int x1, int y1, int thick, uint32_t color) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (steps == 0) steps = 1;
    int half = thick / 2;
    for (int i = 0; i <= steps; i++) {
        int px = x0 + dx * i / steps;
        int py = y0 + dy * i / steps;
        g_fill(px - half, py - half, thick, thick, color);
    }
}

/* place a point at radius `r` along clock tick `t` from centre (cx,cy) */
static void polar(int cx, int cy, int t, int r, int *ox, int *oy) {
    *ox = cx + r * sinv(t) / 1000;
    *oy = cy - r * cosv(t) / 1000;
}

static void clk_tick(window_t *w) { (void)w; gui_request_redraw(); }

static const char *MONTHS[13] = { "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void two(char *o, int v) { o[0] = '0' + (v / 10) % 10; o[1] = '0' + v % 10; o[2] = 0; }

static void clk_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    struct rtc_time t; rtc_now(&t);

    /* clock face: centred circle in the upper portion of the window */
    int face = (cw < ch - 96 ? cw : ch - 96) - 40;
    if (face < 80) face = 80;
    int ccx = cx + cw / 2;
    int ccy = cy + 24 + face / 2;
    int r = face / 2;

    /* dial */
    g_round(ccx - r - 8, ccy - r - 8, 2 * (r + 8), 2 * (r + 8), r + 8, COL_PANEL_2, 255);
    g_round(ccx - r, ccy - r, 2 * r, 2 * r, r, 0x0E0E16, 255);

    /* tick marks: bold at the 12 hour positions, faint between */
    for (int i = 0; i < 60; i++) {
        int inner = (i % 5 == 0) ? r - 12 : r - 6;
        int x0, y0, x1, y1;
        polar(ccx, ccy, i, inner, &x0, &y0);
        polar(ccx, ccy, i, r - 2, &x1, &y1);
        uint32_t c = (i % 5 == 0) ? COL_TEXT : COL_PANEL_3;
        thick_line(x0, y0, x1, y1, (i % 5 == 0) ? 3 : 1, c);
    }

    /* hands: hour (12h -> 60 ticks), minute, second */
    int sec = t.sec % 60;
    int min = t.min % 60;
    int hr  = t.hour % 12;
    int hour_tick = hr * 5 + min / 12;          /* smooth hour hand */

    int hx, hy;
    polar(ccx, ccy, hour_tick, r * 1 / 2, &hx, &hy);
    thick_line(ccx, ccy, hx, hy, 5, COL_TEXT);

    polar(ccx, ccy, min, r * 3 / 4, &hx, &hy);
    thick_line(ccx, ccy, hx, hy, 3, COL_TEXT);

    polar(ccx, ccy, sec, r * 4 / 5, &hx, &hy);
    thick_line(ccx, ccy, hx, hy, 1, COL_ACCENT);

    /* centre hub */
    g_round(ccx - 5, ccy - 5, 10, 10, 5, COL_ACCENT, 255);

    /* digital readout + date below the face */
    char hh[3], mm[3], ss[3];
    two(hh, t.hour); two(mm, t.min); two(ss, t.sec);
    char dig[12];
    dig[0] = 0;
    kstrlcat(dig, hh, sizeof(dig)); kstrlcat(dig, ":", sizeof(dig));
    kstrlcat(dig, mm, sizeof(dig)); kstrlcat(dig, ":", sizeof(dig));
    kstrlcat(dig, ss, sizeof(dig));

    int dy = ccy + r + 18;
    int dw = g_text_width(dig, 3);
    g_text(ccx - dw / 2, dy, dig, COL_TEXT, 3);

    char dd[3]; two(dd, t.day);
    char date[24];
    date[0] = 0;
    kstrlcat(date, MONTHS[t.mon <= 12 ? t.mon : 0], sizeof(date));
    kstrlcat(date, " ", sizeof(date));
    kstrlcat(date, dd, sizeof(date));
    int dw2 = g_text_width(date, 1);
    g_text(ccx - dw2 / 2, dy + 32, date, COL_TEXT_DIM, 1);
}

void clock_app_init(void) {
    window_t *win = gui_add_window("Clock", 300, 380, 0x4FC3F7, ICON_CLOCK);
    if (!win) return;
    win->draw = clk_draw;
    win->tick = clk_tick;
    win->min_w = 220; win->min_h = 300;
    win->x = 660; win->y = 140;
}
