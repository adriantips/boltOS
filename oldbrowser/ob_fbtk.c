/* ===========================================================================
 *  ob_fbtk.c  --  NetSurf frontends/framebuffer/fbtk (widget toolkit)
 *
 *  A tiny retained-mode widget toolkit, the analogue of NetSurf's fbtk: the
 *  framebuffer frontend builds a list of widgets (buttons, a text entry, a
 *  throbber, labels) once, then redraws and hit-tests them each frame. Widgets
 *  live in a fixed pool (the toolbar never needs more than a handful) and draw
 *  through the BoltOS compositor primitives. The chrome uses a deliberately
 *  light, classic palette independent of the desktop theme.
 * ===========================================================================*/
#include "oldbrowser.h"
#include "string.h"
#include "gui.h"

#define FBTK_POOL 24
static fbtk_widget pool[FBTK_POOL];
static int         npool;

/* classic browser-chrome palette */
#define CH_BTN     0xF4F4F6
#define CH_BTN_HOT 0xE2E6F0
#define CH_BTN_DN  0xCAD2E6
#define CH_BORDER  0xB8BAC2
#define CH_TXT     0x303338
#define CH_TXT_DIS 0xAEB0B8
#define CH_FIELD   0xFFFFFF

static void scopy(char *d, const char *s, uint32_t cap) {
    if (!cap) return; uint32_t i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0;
}

fbtk_widget *fbtk_create(int type, int id, int x, int y, int w, int h, const char *text) {
    if (npool >= FBTK_POOL) return 0;
    fbtk_widget *wd = &pool[npool++];
    memset(wd, 0, sizeof(*wd));
    wd->type = type; wd->id = id;
    wd->x = x; wd->y = y; wd->w = w; wd->h = h;
    wd->enabled = 1;
    if (text) scopy(wd->text, text, sizeof(wd->text));
    return wd;
}
void fbtk_set_text(fbtk_widget *wd, const char *s) { if (wd) scopy(wd->text, s, sizeof(wd->text)); }
void fbtk_set_enabled(fbtk_widget *wd, int en)     { if (wd) wd->enabled = en; }

int fbtk_hit(fbtk_widget *wd, int ox, int oy, int px, int py) {
    if (!wd) return 0;
    int x = ox + wd->x, y = oy + wd->y;
    return px >= x && px < x + wd->w && py >= y && py < y + wd->h;
}

/* spinner ring -- 8 dots, the active one (by frame) lit in the accent colour */
static void draw_throbber(fbtk_widget *wd, int x, int y, int frame) {
    static const int dx[8] = { 0,  3,  4,  3,  0, -3, -4, -3 };
    static const int dy[8] = {-4, -3,  0,  3,  4,  3,  0, -3 };
    int cx = x + wd->w / 2, cy = y + wd->h / 2;
    for (int i = 0; i < 8; i++) {
        int lit = ((frame % 8) == i);
        int near = (((frame % 8) + 7) % 8) == i;
        uint32_t col = lit ? wd->accent : near ? 0x9AA4C0 : 0xCBCED8;
        g_fill(cx + dx[i] - 1, cy + dy[i] - 1, 3, 3, col);
    }
}

void fbtk_redraw(fbtk_widget *wd, int ox, int oy, int throb_frame) {
    if (!wd) return;
    int x = ox + wd->x, y = oy + wd->y;
    switch (wd->type) {
    case FBTK_FILL:
        g_fill(x, y, wd->w, wd->h, wd->accent);
        break;
    case FBTK_LABEL: {
        int tw = g_text_width(wd->text, 1);
        g_text(x, y + (wd->h - 8) / 2, wd->text, CH_TXT, 1);
        (void)tw;
        break;
    }
    case FBTK_BUTTON: {
        uint32_t bg = wd->pressed ? CH_BTN_DN : CH_BTN;
        g_round(x, y, wd->w, wd->h, 5, bg, 255);
        g_rect(x, y, wd->w, wd->h, CH_BORDER);
        uint32_t tc = wd->enabled ? CH_TXT : CH_TXT_DIS;
        int tw = g_text_width(wd->text, 1);
        g_text(x + (wd->w - tw) / 2, y + (wd->h - 8) / 2, wd->text, tc, 1);
        break;
    }
    case FBTK_TEXT: {
        g_round(x, y, wd->w, wd->h, 5, CH_FIELD, 255);
        g_rect(x, y, wd->w, wd->h, CH_BORDER);
        g_set_clip(x + 4, y, wd->w - 8, wd->h);
        int tw = g_text_width(wd->text, 1);
        int tx = x + 6;
        if (tw > wd->w - 14) tx = x + wd->w - 8 - tw;   /* scroll to caret end */
        g_text(tx, y + (wd->h - 8) / 2, wd->text, CH_TXT, 1);
        g_clear_clip();
        break;
    }
    case FBTK_THROBBER:
        draw_throbber(wd, x, y, throb_frame);
        break;
    }
}
