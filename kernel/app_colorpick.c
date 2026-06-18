/* ===========================================================================
 *  BoltOS  -  kernel/app_colorpick.c
 *  Colour Picker: three R/G/B sliders (drag to set, using the window drag
 *  callback), a live preview swatch, the resulting #RRGGBB hex code, and a row
 *  of preset swatches. Pure integer maths.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "string.h"
#include "commands.h"     /* sh_utoa */

static int rgb[3] = { 79, 195, 247 };     /* default: a nice cyan */

/* slider + preset hit rects (client-local) */
typedef struct { int x, y, w, h, kind, val; } chit_t;
static chit_t hits[16];
static int    nhit;

enum { K_SLIDER = 1, K_PRESET };
static const uint32_t PRESETS[8] = {
    0xE0556B, 0xFFB454, 0xF6D32D, 0x34C759,
    0x4FC3F7, 0x4F8DF7, 0x9B6CF2, 0xFFFFFF
};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void hex2(uint8_t v, char *out) {
    const char *H = "0123456789ABCDEF";
    out[0] = H[(v >> 4) & 0xF]; out[1] = H[v & 0xF]; out[2] = 0;
}

/* map a press/drag at (lx,ly) onto whichever slider/preset it falls on */
static void apply_at(int lx, int ly) {
    for (int i = 0; i < nhit; i++) {
        chit_t *h = &hits[i];
        if (h->kind == K_SLIDER) {
            /* generous vertical band so dragging off the track still tracks */
            if (ly < h->y - 10 || ly >= h->y + h->h + 10) continue;
            if (lx < h->x || lx >= h->x + h->w) {
                if (lx < h->x) { rgb[h->val] = 0; return; }
                if (lx >= h->x + h->w) { rgb[h->val] = 255; return; }
            }
            rgb[h->val] = clampi((lx - h->x) * 255 / (h->w - 1), 0, 255);
            return;
        }
    }
}

static void cp_drag(window_t *w, int lx, int ly) { (void)w; apply_at(lx, ly); }

static void cp_click(window_t *w, int lx, int ly) {
    (void)w;
    for (int i = 0; i < nhit; i++) {
        chit_t *h = &hits[i];
        if (h->kind != K_PRESET) continue;
        if (lx >= h->x && lx < h->x + h->w && ly >= h->y && ly < h->y + h->h) {
            uint32_t c = PRESETS[h->val];
            rgb[0] = (c >> 16) & 0xFF; rgb[1] = (c >> 8) & 0xFF; rgb[2] = c & 0xFF;
            return;
        }
    }
    apply_at(lx, ly);                 /* a plain click on a track also sets it */
}

static void slider(int x, int y, int w, int ch, int idx, const char *label, uint32_t accent) {
    g_text(x, y, label, COL_TEXT_DIM, 1);
    char vb[8]; sh_utoa((uint64_t)rgb[idx], vb);
    g_text(x + w - g_text_width(vb, 1), y, vb, COL_TEXT, 1);
    int ty = y + 16, th = 10;
    g_round(x, ty, w, th, 5, COL_PANEL_3, 255);
    int fill = rgb[idx] * (w - 1) / 255;
    g_round(x, ty, fill + 1, th, 5, accent, 255);
    int kx = x + fill;                                  /* knob */
    g_round(kx - 7, ty - 4, 14, 18, 7, 0xFFFFFF, 255);
    if (nhit < 16) { hits[nhit].x = x; hits[nhit].y = ty; hits[nhit].w = w; hits[nhit].h = th;
                     hits[nhit].kind = K_SLIDER; hits[nhit].val = idx; nhit++; }
    (void)ch;
}

static void cp_draw(window_t *win, int cx, int cy, int cw, int ch) {
    (void)win;
    nhit = 0;
    g_fill(cx, cy, cw, ch, COL_PANEL);
    int pad = 16, x = cx + pad, w = cw - 2 * pad, y = cy + pad;

    uint32_t color = ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | (uint32_t)rgb[2];

    /* preview swatch + hex */
    g_round(x, y, w, 88, 12, color, 255);
    char hex[8]; hex[0] = '#';
    char t[3];
    hex2((uint8_t)rgb[0], t); hex[1] = t[0]; hex[2] = t[1];
    hex2((uint8_t)rgb[1], t); hex[3] = t[0]; hex[4] = t[1];
    hex2((uint8_t)rgb[2], t); hex[5] = t[0]; hex[6] = t[1]; hex[7] = 0;
    /* contrast-aware label colour */
    int lum = (rgb[0] * 30 + rgb[1] * 59 + rgb[2] * 11) / 100;
    uint32_t hc = lum > 140 ? 0x101018 : 0xFFFFFF;
    g_text(x + 16, y + 34, hex, hc, 3);
    y += 104;

    slider(x, y, w, ch, 0, "Red",   0xE0556B); y += 40;
    slider(x, y, w, ch, 1, "Green", 0x34C759); y += 40;
    slider(x, y, w, ch, 2, "Blue",  0x4F8DF7); y += 48;

    /* presets */
    g_text(x, y, "Presets", COL_TEXT_DIM, 1); y += 18;
    int sw = (w - 7 * 8) / 8;
    for (int i = 0; i < 8; i++) {
        int px = x + i * (sw + 8);
        g_round(px, y, sw, sw, 6, PRESETS[i], 255);
        g_rect(px, y, sw, sw, COL_PANEL_3);
        if (nhit < 16) { hits[nhit].x = px - cx; hits[nhit].y = y - cy; hits[nhit].w = sw; hits[nhit].h = sw;
                         hits[nhit].kind = K_PRESET; hits[nhit].val = i; nhit++; }
    }

    /* slider hits were stored absolute (track uses cx-based x); convert to client-local */
    for (int i = 0; i < nhit; i++)
        if (hits[i].kind == K_SLIDER) { hits[i].x -= cx; hits[i].y -= cy; }
}

void colorpick_app_init(void) {
    window_t *win = gui_add_window("Color Picker", 320, 400, 0x9B6CF2, ICON_COLOR);
    if (!win) return;
    win->draw  = cp_draw;
    win->click = cp_click;
    win->drag  = cp_drag;
    win->min_w = 300; win->min_h = 380;
    win->x = 360; win->y = 100;
}
