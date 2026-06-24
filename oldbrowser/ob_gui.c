/* ===========================================================================
 *  ob_gui.c  --  NetSurf frontends/framebuffer/gui.c (the framebuffer frontend)
 *
 *  Assembles the classic NetSurf chrome on top of a browser_window: a toolbar
 *  of Back / Forward / Reload / Stop / Home buttons, an address entry, a
 *  bookmark toggle and an animated throbber, plus the content viewport with a
 *  scrollbar and a status bar. The BoltOS window callbacks in
 *  kernel/app_oldbrowser.c funnel straight into the ob_gui_* entry points here,
 *  which is the role NetSurf's framebuffer gui.c plays between fbtk and core.
 * ===========================================================================*/
#include "oldbrowser.h"
#include "string.h"
#include "gui.h"
#include "keyboard.h"

#define TOOLBAR_H   40
#define STATUS_H    22
#define CONTENT_PAD 12
#define SBW         10
#define BTN_H       26
#define BTN_Y       7

enum { ACT_BACK = 1, ACT_FWD, ACT_RELOAD, ACT_STOP, ACT_HOME, ACT_URL, ACT_STAR, ACT_THROB };

static browser_window *g_bw;
static fbtk_widget *w_back, *w_fwd, *w_reload, *w_stop, *w_home, *w_url, *w_star, *w_throb;
static int  g_cx, g_cy, g_cw, g_ch;     /* last client rect (screen coords)  */
static int  url_editing;
static char urlbuf[1024];
static int  throb_frame;
static int  blink;

window_t *ob_active_window;             /* set by the app; title is synced    */

static void scopy(char *d, const char *s, uint32_t cap) {
    if (!cap) return; uint32_t i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0;
}
static void wset(fbtk_widget *wd, int x, int y, int w, int h) { if (wd){wd->x=x;wd->y=y;wd->w=w;wd->h=h;} }

void ob_gui_init(void) {
    hotlist_init();
    g_bw = browser_window_create();
    w_back   = fbtk_create(FBTK_BUTTON,  ACT_BACK,   0, BTN_Y, 44, BTN_H, "Back");
    w_fwd    = fbtk_create(FBTK_BUTTON,  ACT_FWD,    0, BTN_Y, 40, BTN_H, "Fwd");
    w_reload = fbtk_create(FBTK_BUTTON,  ACT_RELOAD, 0, BTN_Y, 56, BTN_H, "Reload");
    w_stop   = fbtk_create(FBTK_BUTTON,  ACT_STOP,   0, BTN_Y, 44, BTN_H, "Stop");
    w_home   = fbtk_create(FBTK_BUTTON,  ACT_HOME,   0, BTN_Y, 50, BTN_H, "Home");
    w_url    = fbtk_create(FBTK_TEXT,    ACT_URL,    0, BTN_Y, 200, BTN_H, "");
    w_star   = fbtk_create(FBTK_BUTTON,  ACT_STAR,   0, BTN_Y, 48, BTN_H, "Book");
    w_throb  = fbtk_create(FBTK_THROBBER,ACT_THROB,  0, BTN_Y, 24, BTN_H, "");
    browser_window_home(g_bw);
}

static void layout_widgets(int cw) {
    int x = 8;
    wset(w_back,   x, BTN_Y, 44, BTN_H); x += 44 + 4;
    wset(w_fwd,    x, BTN_Y, 40, BTN_H); x += 40 + 4;
    wset(w_reload, x, BTN_Y, 56, BTN_H); x += 56 + 4;
    wset(w_stop,   x, BTN_Y, 44, BTN_H); x += 44 + 4;
    wset(w_home,   x, BTN_Y, 50, BTN_H); x += 50 + 6;
    int throb_x = cw - 8 - 24;
    int star_w  = 48;
    int star_x  = throb_x - 6 - star_w;
    int ux = x;
    int uw = star_x - 6 - ux; if (uw < 90) uw = 90;
    wset(w_url,   ux, BTN_Y, uw, BTN_H);
    wset(w_star,  star_x, BTN_Y, star_w, BTN_H);
    wset(w_throb, throb_x, BTN_Y, 24, BTN_H);
}

void ob_gui_draw(int cx, int cy, int cw, int ch) {
    g_cx = cx; g_cy = cy; g_cw = cw; g_ch = ch;
    layout_widgets(cw);

    /* ---- toolbar chrome ---- */
    g_fill(cx, cy, cw, TOOLBAR_H, 0xE9EAEE);
    g_hline(cx, cy + TOOLBAR_H - 1, cw, 0xB8BAC2);

    fbtk_set_enabled(w_back, browser_window_back_available(g_bw));
    fbtk_set_enabled(w_fwd,  browser_window_forward_available(g_bw));
    fbtk_set_enabled(w_stop, browser_window_is_loading(g_bw));
    w_throb->accent = COL_ACCENT;

    nsurl *cur = browser_window_get_url(g_bw);
    if (url_editing) fbtk_set_text(w_url, urlbuf);
    else fbtk_set_text(w_url, cur ? nsurl_access(cur) : "");
    w_star->pressed = (cur && hotlist_has(nsurl_access(cur)));

    fbtk_redraw(w_back,   cx, cy, throb_frame);
    fbtk_redraw(w_fwd,    cx, cy, throb_frame);
    fbtk_redraw(w_reload, cx, cy, throb_frame);
    fbtk_redraw(w_stop,   cx, cy, throb_frame);
    fbtk_redraw(w_home,   cx, cy, throb_frame);
    fbtk_redraw(w_url,    cx, cy, throb_frame);
    fbtk_redraw(w_star,   cx, cy, throb_frame);
    if (browser_window_is_loading(g_bw)) fbtk_redraw(w_throb, cx, cy, throb_frame);

    /* caret in the address bar */
    if (url_editing && (blink & 1)) {
        int tw = g_text_width(urlbuf, 1);
        int fx = cx + w_url->x, fw = w_url->w;
        int caretx = fx + 6 + tw;
        if (caretx > fx + fw - 6) caretx = fx + fw - 6;
        g_fill(caretx, cy + w_url->y + 5, 1, BTN_H - 10, 0x303338);
    }

    /* ---- content viewport ---- */
    int vy0 = cy + TOOLBAR_H;
    int vh  = ch - TOOLBAR_H - STATUS_H; if (vh < 16) vh = 16;
    int W   = cw - 2 * CONTENT_PAD - SBW; if (W < 90) W = 90;
    browser_window_reformat(g_bw, W);
    browser_window_scroll(g_bw, 0, vh);          /* set view_h + clamp scroll  */
    int scroll = browser_window_get_scroll(g_bw);

    g_fill(cx, vy0, cw, vh, 0xFFFFFF);
    g_set_clip(cx, vy0, cw - SBW, vh);
    int ox = cx + CONTENT_PAD;
    int oy = vy0 + 6 - scroll;
    browser_window_redraw(g_bw, ox, oy, cx + CONTENT_PAD, cx + CONTENT_PAD + W, vy0, vy0 + vh);
    g_clear_clip();

    /* ---- scrollbar ---- */
    int content_h = browser_window_content_height(g_bw);
    if (content_h > vh) {
        int track_x = cx + cw - SBW;
        g_fill(track_x, vy0, SBW, vh, 0xE4E4E8);
        int th = vh * vh / content_h; if (th < 24) th = 24;
        int maxs = content_h - vh; if (maxs < 1) maxs = 1;
        int ty = vy0 + (vh - th) * scroll / maxs;
        g_round(track_x + 1, ty, SBW - 2, th, 3, 0xA8AAB4, 255);
    }

    /* ---- status bar ---- */
    int sy = cy + ch - STATUS_H;
    g_fill(cx, sy, cw, STATUS_H, 0xE9EAEE);
    g_hline(cx, sy, cw, 0xB8BAC2);
    const char *status = browser_window_get_status(g_bw);
    g_set_clip(cx, sy, cw - 120, STATUS_H);
    g_text(cx + 8, sy + 7, status, 0x4A4D55, 1);
    g_clear_clip();
    /* loading marker on the right */
    if (browser_window_is_loading(g_bw))
        g_text(cx + cw - 70, sy + 7, "loading", COL_ACCENT, 1);
    else {
        content *c = browser_window_get_content(g_bw);
        const char *tlabel = c && c->type == CONTENT_HTML ? "html" :
                             c && c->type == CONTENT_TEXTPLAIN ? "text" :
                             c && c->type == CONTENT_IMAGE ? "image" : "";
        g_text(cx + cw - 50, sy + 7, tlabel, 0x8A8D95, 1);
    }
}

/* ----------------------------- input ------------------------------------ */
static int hit(fbtk_widget *wd, int lx, int ly) {
    return wd && lx >= wd->x && lx < wd->x + wd->w && ly >= wd->y && ly < wd->y + wd->h;
}

void ob_gui_click(int lx, int ly) {
    if (ly < TOOLBAR_H) {
        url_editing = 0;
        if (hit(w_back, lx, ly))        browser_window_history_back(g_bw);
        else if (hit(w_fwd, lx, ly))    browser_window_history_forward(g_bw);
        else if (hit(w_reload, lx, ly)) browser_window_reload(g_bw);
        else if (hit(w_stop, lx, ly))   browser_window_stop(g_bw);
        else if (hit(w_home, lx, ly))   browser_window_home(g_bw);
        else if (hit(w_star, lx, ly)) {
            nsurl *cur = browser_window_get_url(g_bw);
            if (cur) { const char *u = nsurl_access(cur);
                if (hotlist_has(u)) hotlist_remove(u);
                else hotlist_add(u, browser_window_get_title(g_bw)); }
        } else if (hit(w_url, lx, ly)) {
            url_editing = 1;
            nsurl *cur = browser_window_get_url(g_bw);
            scopy(urlbuf, cur ? nsurl_access(cur) : "", sizeof(urlbuf));
        }
        return;
    }
    int sy = g_ch - STATUS_H;
    if (ly >= sy) return;                /* status bar: ignore               */

    /* content click -> hyperlink hit test (content-space coords) */
    url_editing = 0;
    int scroll = browser_window_get_scroll(g_bw);
    int cxp = lx - CONTENT_PAD;
    int cyp = ly - TOOLBAR_H - 6 + scroll;
    browser_window_mouse_click(g_bw, cxp, cyp);
}

void ob_gui_key(char c) {
    if (url_editing) {
        unsigned char u = (unsigned char)c;
        if (c == '\n' || c == '\r') { url_editing = 0; browser_window_go(g_bw, urlbuf); }
        else if (c == 27)           { url_editing = 0; }
        else if (u == 8 || u == 0x7F) { int n = strlen(urlbuf); if (n > 0) urlbuf[n-1] = 0; }
        else if (u >= 32 && u < 127) { int n = strlen(urlbuf); if (n < (int)sizeof(urlbuf)-1) { urlbuf[n]=c; urlbuf[n+1]=0; } }
        return;
    }
    browser_window_key(g_bw, c);
}

void ob_gui_scroll(int delta) {
    int vh = g_ch - TOOLBAR_H - STATUS_H; if (vh < 16) vh = 16;
    browser_window_scroll(g_bw, -delta * 48, vh);
}

void ob_gui_tick(void) {
    blink++;
    if (browser_window_is_loading(g_bw)) throb_frame++;
    if (ob_active_window) {
        char t[40]; scopy(t, "OldBrowser", sizeof(t));
        const char *pt = browser_window_get_title(g_bw);
        if (pt && pt[0]) {
            scopy(t, pt, sizeof(t));
            int n = strlen(t);
            const char *suf = " - OldBrowser";
            if (n < (int)sizeof(t) - 1) scopy(t + n, suf, sizeof(t) - n);
        }
        scopy(ob_active_window->title, t, sizeof(ob_active_window->title));
    }
}
