/* ===========================================================================
 *  BoltOS  -  kernel/app_calendar.c
 *  Calendar window: a month grid with the current day highlighted and prev/next
 *  month navigation. The weekday of the 1st is found with Zeller's congruence
 *  (pure integer math - the kernel has no hardware FP). Date source: CMOS RTC.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "hw.h"
#include "string.h"
#include "commands.h"     /* sh_utoa */

typedef struct { int year, month; int inited; } cal_t;   /* month: 1..12 */
static cal_t cal;

/* prev/next hot rects (client-local) */
static struct { int x, y, w, h, d; } chots[2];
static int nchot;

static const char *MON[13] = { "", "January", "February", "March", "April", "May",
    "June", "July", "August", "September", "October", "November", "December" };
static const char *WD[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }
static int days_in(int y, int m) {
    static const int d[13] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && is_leap(y)) return 29;
    return d[m];
}

/* weekday of (y,m,d): 0 = Sunday .. 6 = Saturday */
static int weekday(int y, int m, int d) {
    if (m < 3) { m += 12; y--; }
    int K = y % 100, J = y / 100;
    int h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;  /* 0=Sat */
    return (h + 6) % 7;                                               /* ->0=Sun */
}

static void ensure_init(void) {
    if (cal.inited) return;
    struct rtc_time t; rtc_now(&t);
    cal.year = t.year ? t.year : 2026;
    cal.month = (t.mon >= 1 && t.mon <= 12) ? t.mon : 1;
    cal.inited = 1;
}

static void shift_month(int delta) {
    cal.month += delta;
    while (cal.month > 12) { cal.month -= 12; cal.year++; }
    while (cal.month < 1)  { cal.month += 12; cal.year--; }
}

static void cal_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    ensure_init();
    nchot = 0;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    struct rtc_time now; rtc_now(&now);

    int pad = 16, x0 = cx + pad, y = cy + pad;

    /* header: "< Month Year >" */
    char title[24];
    title[0] = 0;
    kstrlcat(title, MON[cal.month], sizeof(title));
    kstrlcat(title, " ", sizeof(title));
    char yb[8]; sh_utoa((uint64_t)cal.year, yb);
    kstrlcat(title, yb, sizeof(title));
    int tw = g_text_width(title, 2);
    g_text(cx + (cw - tw) / 2, y, title, COL_TEXT, 2);

    /* nav arrows */
    int aw = 28, ay = y - 4;
    g_round(x0, ay, aw, 26, 6, COL_PANEL_3, 255);          g_text(x0 + 10, y, "<", COL_TEXT, 2);
    g_round(cx + cw - pad - aw, ay, aw, 26, 6, COL_PANEL_3, 255); g_text(cx + cw - pad - aw + 9, y, ">", COL_TEXT, 2);
    chots[0].x = x0 - cx; chots[0].y = ay - cy; chots[0].w = aw; chots[0].h = 26; chots[0].d = -1;
    chots[1].x = (cx + cw - pad - aw) - cx; chots[1].y = ay - cy; chots[1].w = aw; chots[1].h = 26; chots[1].d = +1;
    nchot = 2;

    y += 40;

    /* grid geometry: 7 columns */
    int gridw = cw - 2 * pad;
    int colw = gridw / 7;
    int x = x0;

    /* weekday header row */
    for (int i = 0; i < 7; i++) {
        uint32_t c = (i == 0 || i == 6) ? COL_ACCENT : COL_TEXT_DIM;
        g_text(x + i * colw + (colw - g_text_width(WD[i], 1)) / 2, y, WD[i], c, 1);
    }
    y += 22;
    g_hline(x0, y - 4, gridw, COL_PANEL_3);

    /* day cells */
    int rowh = (cy + ch - pad - y) / 6;
    if (rowh < 18) rowh = 18;
    int first = weekday(cal.year, cal.month, 1);
    int dim = days_in(cal.year, cal.month);
    int today = (cal.year == now.year && cal.month == now.mon) ? now.day : 0;

    for (int d = 1; d <= dim; d++) {
        int cell = first + (d - 1);
        int row = cell / 7, col = cell % 7;
        int cxp = x0 + col * colw;
        int cyp = y + row * rowh;
        char db[4]; sh_utoa((uint64_t)d, db);
        int dw = g_text_width(db, 1);

        if (d == today) {                     /* today: filled accent pill */
            int r = 13;
            g_round(cxp + colw / 2 - r, cyp + rowh / 2 - r, 2 * r, 2 * r, r, COL_ACCENT, 255);
            g_text(cxp + (colw - dw) / 2, cyp + rowh / 2 - 7, db, 0xFFFFFF, 1);
        } else {
            uint32_t c = (col == 0 || col == 6) ? COL_TEXT_DIM : COL_TEXT;
            g_text(cxp + (colw - dw) / 2, cyp + rowh / 2 - 7, db, c, 1);
        }
    }
}

static void cal_click(window_t *w, int lx, int ly) {
    (void)w;
    for (int i = 0; i < nchot; i++) {
        if (lx >= chots[i].x && lx < chots[i].x + chots[i].w &&
            ly >= chots[i].y && ly < chots[i].y + chots[i].h) {
            shift_month(chots[i].d);
            return;
        }
    }
}

void calendar_app_init(void) {
    memset(&cal, 0, sizeof(cal));
    window_t *win = gui_add_window("Calendar", 360, 360, 0xE0556B, ICON_CALENDAR);
    if (!win) return;
    win->draw  = cal_draw;
    win->click = cal_click;
    win->min_w = 300; win->min_h = 300;
    win->x = 420; win->y = 160;
}
