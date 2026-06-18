/* ===========================================================================
 *  BoltOS  -  kernel/app_matrix.c
 *  "Matrix rain" - columns of glyphs cascading down with bright heads and
 *  fading green trails. Driven entirely by the window tick; no input. A small
 *  LCG produces the glyphs and per-column speeds.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "pit.h"
#include "string.h"

#define MAXCOL 160
#define CW     8
#define CH     13

typedef struct {
    int      pos[MAXCOL];      /* head row of each column (can be negative) */
    int      spd[MAXCOL];      /* fall speed, rows per tick */
    int      len[MAXCOL];      /* trail length */
    int      cols, rows;
    uint32_t rng;
    int      inited;
} mtx_t;
static mtx_t MX;

static uint32_t rnd(void) { MX.rng = MX.rng * 1103515245u + 12345u; return MX.rng >> 8; }
static char glyph(void) { return (char)(33 + rnd() % 94); }   /* printable ASCII */

static void col_reset(int c) {
    MX.pos[c] = -(int)(rnd() % (MX.rows ? MX.rows : 20));
    MX.spd[c] = 1 + (int)(rnd() % 2);
    MX.len[c] = 6 + (int)(rnd() % 14);
}

static void mtx_tick(window_t *w) {
    (void)w;
    if (!MX.inited) return;
    for (int c = 0; c < MX.cols; c++) {
        MX.pos[c] += MX.spd[c];
        if (MX.pos[c] - MX.len[c] > MX.rows) col_reset(c);
    }
}

/* trail colour: bright near the head, fading to dark green down the tail */
static uint32_t trail_color(int k, int len) {
    if (k == 0) return 0xD8FFE0;                 /* head: near-white */
    int g = 255 - (k * 200 / (len > 1 ? len : 1));
    if (g < 40) g = 40;
    return ((uint32_t)(g / 4) << 16) | ((uint32_t)g << 8) | (uint32_t)(g / 5);
}

static void mtx_draw(window_t *win, int cx, int cy, int cw, int ch) {
    (void)win;
    g_fill(cx, cy, cw, ch, 0x05060A);

    int cols = cw / CW, rows = ch / CH;
    if (cols > MAXCOL) cols = MAXCOL;
    if (!MX.inited || cols != MX.cols || rows != MX.rows) {
        MX.cols = cols; MX.rows = rows;
        for (int c = 0; c < cols; c++) col_reset(c);
        MX.inited = 1;
    }

    for (int c = 0; c < cols; c++) {
        int x = cx + c * CW;
        for (int k = 0; k < MX.len[c]; k++) {
            int row = MX.pos[c] - k;
            if (row < 0 || row >= rows) continue;
            g_char(x, cy + row * CH, glyph(), trail_color(k, MX.len[c]), 1);
        }
    }
}

void matrix_app_init(void) {
    memset(&MX, 0, sizeof(MX));
    MX.rng = 0x1234ABCD;
    window_t *win = gui_add_window("Matrix", 440, 320, 0x34C759, ICON_MATRIX);
    if (!win) return;
    win->draw = mtx_draw;
    win->tick = mtx_tick;
    win->x = 280; win->y = 110;
}
