/* ===========================================================================
 *  BoltOS  -  kernel/app_life.c
 *  Conway's Game of Life. A double-buffered cell grid evolved from the window
 *  tick (throttled to ~7 generations/sec while running). Paint live cells by
 *  dragging across the grid (using the window drag callback); Play/Pause, Step,
 *  Random and Clear sit on the toolbar. Edges are dead (bounded universe).
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "pit.h"
#include "string.h"
#include "commands.h"     /* sh_utoa */

#define GW   46
#define GH   28
#define CELL 12
#define TOOLH 40
#define PAD   10

typedef struct {
    uint8_t  cur[GH][GW];
    uint8_t  nxt[GH][GW];
    int      running;
    uint64_t gen, last_step;
    uint32_t rng;
} life_t;
static life_t L;

enum { LB_PLAY = 1, LB_STEP, LB_RAND, LB_CLEAR };
typedef struct { int x, y, w, h, id; } lhot_t;
static lhot_t lhots[8];
static int    nlhot, lorg_x, lorg_y;

static uint32_t rnd(void) { L.rng = L.rng * 1103515245u + 12345u; return L.rng >> 8; }

static int population(void) {
    int n = 0;
    for (int r = 0; r < GH; r++) for (int c = 0; c < GW; c++) n += L.cur[r][c];
    return n;
}

static void randomize(void) {
    L.rng ^= (uint32_t)pit_ticks() * 2654435761u + 5u;
    for (int r = 0; r < GH; r++) for (int c = 0; c < GW; c++) L.cur[r][c] = (rnd() % 100 < 28);
    L.gen = 0;
}
static void clear_grid(void) { memset(L.cur, 0, sizeof(L.cur)); L.gen = 0; }

static void step(void) {
    for (int r = 0; r < GH; r++)
        for (int c = 0; c < GW; c++) {
            int n = 0;
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++) {
                    if (!dr && !dc) continue;
                    int rr = r + dr, cc = c + dc;
                    if (rr >= 0 && rr < GH && cc >= 0 && cc < GW) n += L.cur[rr][cc];
                }
            L.nxt[r][c] = (L.cur[r][c] ? (n == 2 || n == 3) : (n == 3)) ? 1 : 0;
        }
    memcpy(L.cur, L.nxt, sizeof(L.cur));
    L.gen++;
}

static void life_tick(window_t *w) {
    (void)w;
    if (!L.running) return;
    uint64_t now = pit_ticks();
    if (now - L.last_step < 140) return;
    L.last_step = now;
    step();
}

/* ---- input -------------------------------------------------------------- */
static void lhot(int x, int y, int w, int h, int id) {
    if (nlhot >= 8) return;
    lhots[nlhot].x = x - lorg_x; lhots[nlhot].y = y - lorg_y;
    lhots[nlhot].w = w; lhots[nlhot].h = h; lhots[nlhot].id = id; nlhot++;
}

static void life_click(window_t *w, int lx, int ly) {
    (void)w;
    for (int i = 0; i < nlhot; i++) {
        lhot_t *h = &lhots[i];
        if (lx < h->x || lx >= h->x + h->w || ly < h->y || ly >= h->y + h->h) continue;
        switch (h->id) {
        case LB_PLAY:  L.running = !L.running; break;
        case LB_STEP:  L.running = 0; step(); break;
        case LB_RAND:  randomize(); break;
        case LB_CLEAR: clear_grid(); break;
        }
        return;
    }
}

/* paint live cells while dragging across the grid */
static void life_drag(window_t *w, int lx, int ly) {
    (void)w;
    int gx = PAD, gy = TOOLH + PAD;
    int c = (lx - gx) / CELL, r = (ly - gy) / CELL;
    if (r >= 0 && r < GH && c >= 0 && c < GW) L.cur[r][c] = 1;
}

/* ---- draw --------------------------------------------------------------- */
static int lbtn(int x, int y, const char *label, int id, uint32_t bg) {
    int w = g_text_width(label, 1) + 22, h = 26;
    g_round(x, y, w, h, 7, bg, 255);
    g_text(x + 11, y + 5, label, 0xFFFFFF, 1);
    lhot(x, y, w, h, id);
    return w;
}

static void life_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w; (void)cw; (void)ch;
    nlhot = 0; lorg_x = cx; lorg_y = cy;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    /* toolbar */
    g_fill(cx, cy, cw, TOOLH, COL_PANEL_2);
    int bx = cx + PAD, by = cy + 7;
    bx += lbtn(bx, by, L.running ? "Pause" : "Play", LB_PLAY, L.running ? COL_WARN : COL_GOOD) + 8;
    bx += lbtn(bx, by, "Step",  LB_STEP,  COL_PANEL_3) + 8;
    bx += lbtn(bx, by, "Random", LB_RAND, COL_ACCENT) + 8;
    bx += lbtn(bx, by, "Clear", LB_CLEAR, COL_BAD) + 8;

    char info[32], t[12];
    info[0] = 0;
    kstrlcat(info, "Gen ", sizeof(info)); sh_utoa(L.gen, t); kstrlcat(info, t, sizeof(info));
    kstrlcat(info, "  Pop ", sizeof(info)); sh_utoa((uint64_t)population(), t); kstrlcat(info, t, sizeof(info));
    g_text(cx + cw - g_text_width(info, 1) - PAD, cy + 14, info, COL_TEXT_DIM, 1);

    /* grid */
    int gx = cx + PAD, gy = cy + TOOLH + PAD;
    g_round(gx - 2, gy - 2, GW * CELL + 4, GH * CELL + 4, 6, 0x0C0C12, 255);
    for (int r = 0; r < GH; r++)
        for (int c = 0; c < GW; c++)
            if (L.cur[r][c])
                g_round(gx + c * CELL + 1, gy + r * CELL + 1, CELL - 2, CELL - 2, 3, COL_ACCENT, 255);
}

void life_app_init(void) {
    memset(&L, 0, sizeof(L));
    L.rng = 0x7F4A7C15;
    randomize();
    int w = GW * CELL + 2 * PAD + 4;
    int h = TOOLH + GH * CELL + 2 * PAD + 4;
    window_t *win = gui_add_window("Life", w, h, 0x34C759, ICON_LIFE);
    if (!win) return;
    win->draw  = life_draw;
    win->click = life_click;
    win->drag  = life_drag;
    win->tick  = life_tick;
    win->x = 150; win->y = 80;
}
