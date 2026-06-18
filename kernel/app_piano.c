/* ===========================================================================
 *  BoltOS  -  kernel/app_piano.c
 *  Piano window: a multi-octave keyboard played through the PC speaker driver.
 *  Click a key (or use the home-row keys) to sound a note; the note auto-stops
 *  a short time later, polled from the window tick. White and black keys each
 *  register client-local hot rects, black keys tested first as they sit on top.
 *  Zoom out (the [-] button or the '-' key) widens the view to show more
 *  octaves at once; zoom in ([+] / '=') narrows back toward a single octave.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "pcspk.h"
#include "pit.h"
#include "string.h"

/* Equal-tempered frequencies for the 4 octaves C3..B6 (48 semitones). */
static const uint32_t FREQ48[48] = {
    131, 139, 147, 156, 165, 175, 185, 196, 208, 220, 233, 247,   /* octave 3 */
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494,   /* octave 4 */
    523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988,   /* octave 5 */
    1047,1109,1175,1245,1319,1397,1480,1568,1661,1760,1865,1976   /* octave 6 */
};
#define NOCT 4

/* semitone offset of each white key within an octave, and its label */
static const int   WSEMI[7]  = { 0, 2, 4, 5, 7, 9, 11 };
static const char *WLABEL[7] = { "C", "D", "E", "F", "G", "A", "B" };
/* black keys sit after white index {0,1,3,4,5}; semitone offset within octave */
static const int   BAFTER[5] = { 0, 1, 3, 4, 5 };
static const int   BSEMI[5]  = { 1, 3, 6, 8, 10 };

/* home-row keys play the lowest visible octave */
static const char  WKEY[7] = { 'a','s','d','f','g','h','j' };
static const char  BKEY[5] = { 'w','e','t','y','u' };

typedef struct {
    int      sounding;
    uint64_t off_at;          /* pit tick to silence the note */
    int      active_sem;      /* absolute semitone (0..47) currently lit, -1 none */
    int      zoom;            /* octaves shown, 1..NOCT */
    int      vstart;          /* first visible octave index, 0..NOCT-1 */
} piano_t;
static piano_t pn = { 0, 0, -1, 1, 1 };   /* default: 1 octave, octave 4 */

/* hot rects rebuilt each draw */
typedef struct { int x, y, w, h; int sem; int btn; } khot_t;  /* btn: 0 key, -1 zoom-out, 1 zoom-in */
static khot_t khots[64];
static int    nkhot;

static void play(int sem) {
    if (sem < 0 || sem >= 48) return;
    pcspk_tone(FREQ48[sem]);
    pn.sounding = 1;
    pn.off_at = pit_ticks() + 320;
    pn.active_sem = sem;
}

static void zoom_out(void) {
    if (pn.zoom >= NOCT) return;
    if (pn.vstart + pn.zoom < NOCT) pn.zoom++;        /* room on the right */
    else if (pn.vstart > 0) { pn.vstart--; pn.zoom++; } /* grow to the left */
    gui_request_redraw();
}
static void zoom_in(void) {
    if (pn.zoom > 1) pn.zoom--;
    gui_request_redraw();
}

static void piano_tick(window_t *w) {
    (void)w;
    if (pn.sounding && pit_ticks() >= pn.off_at) {
        pcspk_off();
        pn.sounding = 0; pn.active_sem = -1;
        gui_request_redraw();
    }
}

static void piano_key(window_t *w, char c) {
    (void)w;
    if (c == '-' || c == '_') { zoom_out(); return; }
    if (c == '=' || c == '+') { zoom_in();  return; }
    int base = pn.vstart * 12;
    for (int i = 0; i < 7; i++) if (c == WKEY[i]) { play(base + WSEMI[i]); return; }
    for (int i = 0; i < 5; i++) if (c == BKEY[i]) { play(base + BSEMI[i]); return; }
}

static void piano_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    nkhot = 0;
    g_fill(cx, cy, cw, ch, 0x14141C);

    /* title strip */
    g_text(cx + 14, cy + 12, "BoltOS Piano", COL_TEXT, 2);
    g_text(cx + 14, cy + 36, "Click keys or use a s d f g h j", COL_TEXT_DIM, 1);

    /* zoom buttons, top-right */
    int bw0 = 26, bh0 = 24, by = cy + 12, bxp = cx + cw - 14 - bw0;
    int bxm = bxp - 6 - bw0;
    g_round(bxm, by, bw0, bh0, 5, 0x2A2A38, 255);
    g_text(bxm + 9, by + 5, "-", COL_TEXT, 2);
    g_round(bxp, by, bw0, bh0, 5, 0x2A2A38, 255);
    g_text(bxp + 8, by + 5, "+", COL_TEXT, 2);
    if (nkhot < 64) khots[nkhot++] = (khot_t){ bxm - cx, by - cy, bw0, bh0, -1, -1 };
    if (nkhot < 64) khots[nkhot++] = (khot_t){ bxp - cx, by - cy, bw0, bh0, -1,  1 };

    int top = cy + 60, pad = 12;
    int kx0 = cx + pad, kw_area = cw - 2 * pad, kh = ch - 60 - pad;
    int nwhite = pn.zoom * 7;
    int ww = kw_area / nwhite;
    if (ww < 1) ww = 1;

    /* white keys */
    for (int o = 0; o < pn.zoom; o++) {
        int oct = pn.vstart + o;
        for (int i = 0; i < 7; i++) {
            int wi = o * 7 + i;
            int x = kx0 + wi * ww;
            int sem = oct * 12 + WSEMI[i];
            int pressed = (pn.active_sem == sem);
            uint32_t face = pressed ? COL_ACCENT : 0xF2F2F5;
            g_round(x + 1, top, ww - 2, kh, 6, face, 255);
            if (ww >= 18)
                g_text(x + ww / 2 - 4, top + kh - 22, WLABEL[i], pressed ? 0xFFFFFF : 0x44444C, 1);
            if (nkhot < 64) khots[nkhot++] = (khot_t){ x - cx, top - cy, ww, kh, sem, 0 };
        }
    }

    /* black keys on top */
    int bw = ww * 3 / 5, bh = kh * 3 / 5;
    for (int o = 0; o < pn.zoom; o++) {
        int oct = pn.vstart + o;
        for (int i = 0; i < 5; i++) {
            int wi = o * 7 + BAFTER[i];
            int x = kx0 + (wi + 1) * ww - bw / 2;
            int sem = oct * 12 + BSEMI[i];
            int pressed = (pn.active_sem == sem);
            uint32_t face = pressed ? COL_ACCENT : 0x101018;
            g_round(x, top, bw, bh, 5, face, 255);
            if (nkhot < 64) khots[nkhot++] = (khot_t){ x - cx, top - cy, bw, bh, sem, 0 };
        }
    }
}

static void piano_click(window_t *w, int lx, int ly) {
    (void)w;
    /* black keys are appended after the white keys; test them first (on top) */
    for (int i = nkhot - 1; i >= 0; i--) {
        khot_t *k = &khots[i];
        if (lx >= k->x && lx < k->x + k->w && ly >= k->y && ly < k->y + k->h) {
            if (k->btn == -1) zoom_out();
            else if (k->btn == 1) zoom_in();
            else play(k->sem);
            return;
        }
    }
}

void piano_app_init(void) {
    window_t *win = gui_add_window("Piano", 460, 300, 0x9B6CF2, ICON_PIANO);
    if (!win) return;
    win->draw  = piano_draw;
    win->key   = piano_key;
    win->click = piano_click;
    win->tick  = piano_tick;
    win->min_w = 360; win->min_h = 240;
    win->x = 300; win->y = 200;
}
