/* ===========================================================================
 *  BoltOS  -  kernel/app_calendar.c
 *  Calendar window: a month grid with the current day highlighted and prev/next
 *  month navigation. The weekday of the 1st is found with Zeller's congruence
 *  (pure integer math - the kernel has no hardware FP). Date source: CMOS RTC.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "hw.h"
#include "fs.h"
#include "pit.h"
#include "string.h"
#include "commands.h"     /* sh_utoa */

typedef struct { int year, month; int inited; } cal_t;   /* month: 1..12 */
static cal_t cal;

/* prev/next hot rects (client-local) */
static struct { int x, y, w, h, d; } chots[2];
static int nchot;

/* ---- events: persisted to /home/.calendar as "Y-M-D\ttext" lines --------- */
#define MAXEV 256
typedef struct { int y, m, d; char text[60]; } cevent_t;
static cevent_t evs[MAXEV];
static int  nev;
static int  ev_loaded;
static int  sel_day;            /* day-of-month selected in the current view, 0 = none */
static int  pend_y, pend_m, pend_d;             /* date awaiting event text from the prompt */
static int  gx0, gy0, gcolw, growh, gfirst, gdim;  /* grid geometry, client-local */
static int  add_bx, add_by, add_bw, add_bh;     /* "Add event" button, client-local */
static int  ev_saved_flash;

static void cal_save(void) {
    static char out[MAXEV * 72];
    int n = 0;
    for (int i = 0; i < nev; i++) {
        char nb[12];
        sh_utoa((uint64_t)evs[i].y, nb); for (int j = 0; nb[j]; j++) out[n++] = nb[j]; out[n++] = '-';
        sh_utoa((uint64_t)evs[i].m, nb); for (int j = 0; nb[j]; j++) out[n++] = nb[j]; out[n++] = '-';
        sh_utoa((uint64_t)evs[i].d, nb); for (int j = 0; nb[j]; j++) out[n++] = nb[j]; out[n++] = '\t';
        for (int j = 0; evs[i].text[j] && n < (int)sizeof(out) - 2; j++) out[n++] = evs[i].text[j];
        out[n++] = '\n';
    }
    fs_node *f = fs_lookup("/home/.calendar");
    if (!f) f = fs_create("/home/.calendar", 0);
    if (f && fs_write(f, out, (uint32_t)n) == 0) ev_saved_flash = (int)pit_ticks() + 1500;
}
static void cal_load(void) {
    nev = 0; ev_loaded = 1;
    fs_node *f = fs_lookup("/home/.calendar");
    if (!f || f->is_dir || !f->data) return;
    const char *p = (const char *)f->data; int len = (int)f->size, i = 0;
    while (i < len && nev < MAXEV) {
        int y = 0, m = 0, d = 0;
        while (i < len && p[i] >= '0' && p[i] <= '9') y = y * 10 + (p[i++] - '0');
        if (i < len && p[i] == '-') i++;
        while (i < len && p[i] >= '0' && p[i] <= '9') m = m * 10 + (p[i++] - '0');
        if (i < len && p[i] == '-') i++;
        while (i < len && p[i] >= '0' && p[i] <= '9') d = d * 10 + (p[i++] - '0');
        if (i < len && p[i] == '\t') i++;
        int k = 0;
        while (i < len && p[i] != '\n' && k < 59) evs[nev].text[k++] = p[i++];
        evs[nev].text[k] = 0;
        evs[nev].y = y; evs[nev].m = m; evs[nev].d = d;
        if (y && m && d && k > 0) nev++;
        while (i < len && p[i] != '\n') i++;
        if (i < len) i++;
    }
}
static int day_has_event(int y, int m, int d) {
    for (int i = 0; i < nev; i++) if (evs[i].y == y && evs[i].m == m && evs[i].d == d) return 1;
    return 0;
}
static void ev_add_cb(const char *text) {
    if (!text || !text[0] || nev >= MAXEV) return;
    evs[nev].y = pend_y; evs[nev].m = pend_m; evs[nev].d = pend_d;
    strncpy(evs[nev].text, text, sizeof(evs[nev].text) - 1);
    evs[nev].text[sizeof(evs[nev].text) - 1] = 0;
    nev++;
    cal_save();
}

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
    if (!ev_loaded) cal_load();
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

    /* day cells (reserve a bottom panel for the selected day's events) */
    int evpanel = 92;
    int rowh = (cy + ch - pad - evpanel - y) / 6;
    if (rowh < 18) rowh = 18;
    int first = weekday(cal.year, cal.month, 1);
    int dim = days_in(cal.year, cal.month);
    int today = (cal.year == now.year && cal.month == now.mon) ? now.day : 0;

    /* stash grid geometry (client-local) for the click handler */
    gx0 = x0 - cx; gy0 = y - cy; gcolw = colw; growh = rowh; gfirst = first; gdim = dim;

    for (int d = 1; d <= dim; d++) {
        int cell = first + (d - 1);
        int row = cell / 7, col = cell % 7;
        int cxp = x0 + col * colw;
        int cyp = y + row * rowh;
        char db[4]; sh_utoa((uint64_t)d, db);
        int dw = g_text_width(db, 1);

        if (d == sel_day)                     /* selection ring */
            g_rect(cxp + 2, cyp + 2, colw - 4, rowh - 4, COL_ACCENT);
        if (d == today) {                     /* today: filled accent pill */
            int r = 13;
            g_round(cxp + colw / 2 - r, cyp + rowh / 2 - r, 2 * r, 2 * r, r, COL_ACCENT, 255);
            g_text(cxp + (colw - dw) / 2, cyp + rowh / 2 - 7, db, 0xFFFFFF, 1);
        } else {
            uint32_t c = (col == 0 || col == 6) ? COL_TEXT_DIM : COL_TEXT;
            g_text(cxp + (colw - dw) / 2, cyp + rowh / 2 - 7, db, c, 1);
        }
        if (day_has_event(cal.year, cal.month, d))   /* event dot */
            g_round(cxp + colw / 2 - 2, cyp + rowh - 7, 4, 4, 2, d == today ? 0xFFFFFF : COL_GOOD, 255);
    }

    /* ---- selected-day events panel ------------------------------------- */
    int py = cy + ch - evpanel + 4;
    g_hline(x0, py - 6, gridw, COL_PANEL_3);
    if (sel_day == 0) {
        g_text(x0, py, "Click a day to see its events", COL_TEXT_DIM, 1);
        add_bw = 0;
        return;
    }
    char hdr[24]; int hn = 0;
    const char *dl = "Day "; for (int i = 0; dl[i]; i++) hdr[hn++] = dl[i];
    char nb[8]; sh_utoa((uint64_t)sel_day, nb); for (int i = 0; nb[i]; i++) hdr[hn++] = nb[i];
    hdr[hn] = 0;
    g_text(x0, py, hdr, COL_TEXT, 1);
    /* "Add event" button on the right */
    add_bw = g_text_width("+ Add event", 1) + 16; add_bh = 20;
    int abx = cx + cw - pad - add_bw, aby = py - 4;
    g_round(abx, aby, add_bw, add_bh, 5, COL_ACCENT, 255);
    g_text(abx + 8, aby + 5, "+ Add event", 0xFFFFFF, 1);
    add_bx = abx - cx; add_by = aby - cy;
    /* list this day's events */
    int ey = py + 18, shown = 0;
    for (int i = 0; i < nev && shown < 3; i++) {
        if (evs[i].y != cal.year || evs[i].m != cal.month || evs[i].d != sel_day) continue;
        g_round(x0, ey + 4, 4, 4, 2, COL_GOOD, 255);
        g_text(x0 + 10, ey, evs[i].text, COL_TEXT_DIM, 1);
        ey += 18; shown++;
    }
    if (shown == 0) g_text(x0, ey, "No events", COL_TEXT_DIM, 1);
    if (ev_saved_flash && (int)pit_ticks() < ev_saved_flash)
        g_text(cx + cw - pad - 50, cy + ch - 16, "Saved", COL_GOOD, 1);
}

static void cal_click(window_t *w, int lx, int ly) {
    (void)w;
    for (int i = 0; i < nchot; i++) {
        if (lx >= chots[i].x && lx < chots[i].x + chots[i].w &&
            ly >= chots[i].y && ly < chots[i].y + chots[i].h) {
            shift_month(chots[i].d); sel_day = 0;
            return;
        }
    }
    /* "Add event" button -> prompt for the selected day */
    if (add_bw && sel_day && lx >= add_bx && lx < add_bx + add_bw &&
        ly >= add_by && ly < add_by + add_bh) {
        pend_y = cal.year; pend_m = cal.month; pend_d = sel_day;
        gui_prompt("New event", "", ev_add_cb);
        return;
    }
    /* a day cell -> select it */
    if (gcolw > 0 && growh > 0 && lx >= gx0 && lx < gx0 + 7 * gcolw &&
        ly >= gy0 && ly < gy0 + 6 * growh) {
        int col = (lx - gx0) / gcolw, row = (ly - gy0) / growh;
        int day = row * 7 + col - gfirst + 1;
        if (day >= 1 && day <= gdim) sel_day = day;
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
