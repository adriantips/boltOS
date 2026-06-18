/* ===========================================================================
 *  BoltOS  -  kernel/app_mines.c
 *  Minesweeper. Classic reveal/flag grid. The window framework only delivers
 *  left clicks, so flagging is a header toggle (Reveal / Flag mode) rather than
 *  a right click. Mines are placed lazily on the first reveal (excluding that
 *  cell and its neighbours, so the first click is always safe) using a small LCG
 *  seeded from the PIT tick count.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "pit.h"
#include "string.h"
#include "commands.h"     /* sh_utoa */

#define ROWS  10
#define COLS  12
#define MINES 18
#define CELL  30
#define HEADH 44
#define PAD   12

enum { HID = 0, REV, FLAG };

typedef struct {
    uint8_t  mine[ROWS][COLS];
    uint8_t  adj[ROWS][COLS];
    uint8_t  state[ROWS][COLS];
    int      placed, over, won, flag_mode;
    int      revealed_count, flags;
    uint32_t rng;
} mines_t;
static mines_t ms;

/* header hot rects (client-local) */
enum { HB_RESET = 1, HB_MODE };
static struct { int x, y, w, h, id; } mhots[4];
static int nmhot;

static uint32_t rnd(void) { ms.rng = ms.rng * 1103515245u + 12345u; return ms.rng >> 8; }

static void new_game(void) {
    memset(ms.mine, 0, sizeof(ms.mine));
    memset(ms.adj, 0, sizeof(ms.adj));
    memset(ms.state, 0, sizeof(ms.state));
    ms.placed = ms.over = ms.won = 0;
    ms.revealed_count = ms.flags = 0;
    ms.rng ^= (uint32_t)pit_ticks() * 2654435761u + 1u;
}

static int in_bounds(int r, int c) { return r >= 0 && r < ROWS && c >= 0 && c < COLS; }

/* place mines avoiding (sr,sc) and its 8 neighbours; then compute adjacency */
static void place_mines(int sr, int sc) {
    int placed = 0;
    while (placed < MINES) {
        int r = rnd() % ROWS, c = rnd() % COLS;
        if (ms.mine[r][c]) continue;
        int near = (r >= sr - 1 && r <= sr + 1 && c >= sc - 1 && c <= sc + 1);
        if (near) continue;
        ms.mine[r][c] = 1; placed++;
    }
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int n = 0;
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++)
                    if (in_bounds(r + dr, c + dc) && ms.mine[r + dr][c + dc]) n++;
            ms.adj[r][c] = (uint8_t)n;
        }
    ms.placed = 1;
}

static void reveal(int r, int c) {
    if (!in_bounds(r, c) || ms.state[r][c] != HID) return;
    ms.state[r][c] = REV; ms.revealed_count++;
    if (ms.mine[r][c]) {                       /* boom */
        ms.over = 1;
        for (int i = 0; i < ROWS; i++)
            for (int j = 0; j < COLS; j++)
                if (ms.mine[i][j]) ms.state[i][j] = REV;
        return;
    }
    if (ms.adj[r][c] == 0)                     /* flood empty region */
        for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++)
                if (dr || dc) reveal(r + dr, c + dc);
}

static void check_win(void) {
    if (ms.over) return;
    if (ms.revealed_count >= ROWS * COLS - MINES) { ms.won = 1; ms.over = 1; }
}

/* ---- input -------------------------------------------------------------- */
static void mines_click(window_t *w, int lx, int ly) {
    (void)w;
    for (int i = 0; i < nmhot; i++) {
        if (lx >= mhots[i].x && lx < mhots[i].x + mhots[i].w &&
            ly >= mhots[i].y && ly < mhots[i].y + mhots[i].h) {
            if (mhots[i].id == HB_RESET) new_game();
            else if (mhots[i].id == HB_MODE) ms.flag_mode = !ms.flag_mode;
            return;
        }
    }
    if (ms.over) return;

    int gx = PAD, gy = HEADH + PAD;
    int c = (lx - gx) / CELL, r = (ly - gy) / CELL;
    if (!in_bounds(r, c)) return;

    if (ms.flag_mode) {
        if (ms.state[r][c] == FLAG) { ms.state[r][c] = HID; ms.flags--; }
        else if (ms.state[r][c] == HID) { ms.state[r][c] = FLAG; ms.flags++; }
        return;
    }
    if (ms.state[r][c] == FLAG) return;
    if (!ms.placed) place_mines(r, c);
    reveal(r, c);
    check_win();
}

/* right-click toggles a flag on a hidden cell (no need for Flag mode) */
static void mines_rclick(window_t *w, int lx, int ly) {
    (void)w;
    if (ms.over) return;
    int gx = PAD, gy = HEADH + PAD;
    int c = (lx - gx) / CELL, r = (ly - gy) / CELL;
    if (!in_bounds(r, c)) return;
    if (ms.state[r][c] == FLAG) { ms.state[r][c] = HID; ms.flags--; }
    else if (ms.state[r][c] == HID) { ms.state[r][c] = FLAG; ms.flags++; }
}

/* ---- draw --------------------------------------------------------------- */
static uint32_t num_color(int n) {
    switch (n) {
    case 1: return 0x4F8DF7; case 2: return 0x34C759; case 3: return 0xE0556B;
    case 4: return 0x9B6CF2; case 5: return 0xC8932E; case 6: return 0x4FC3F7;
    case 7: return 0xCAD6EE; default: return 0x9AA7C2;
    }
}

static void mines_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w; (void)cw; (void)ch;
    nmhot = 0;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    /* header: mines-left counter, reset, mode toggle, status */
    g_fill(cx, cy, cw, HEADH, COL_PANEL_2);
    char cnt[8]; sh_utoa((uint64_t)(MINES - ms.flags >= 0 ? MINES - ms.flags : 0), cnt);
    g_text(cx + PAD, cy + 14, "Mines", COL_TEXT_DIM, 1);
    g_text(cx + PAD + 52, cy + 12, cnt, COL_BAD, 2);

    /* reset button (status face) */
    int rbw = 34, rbx = cx + cw / 2 - rbw / 2, rby = cy + 5;
    g_round(rbx, rby, rbw, 34, 8, COL_PANEL_3, 255);
    const char *face = ms.over ? (ms.won ? ":D" : ":(") : ":)";
    g_text(rbx + 6, rby + 9, face, ms.won ? COL_GOOD : (ms.over ? COL_BAD : COL_TEXT), 2);
    mhots[nmhot].x = rbx - cx; mhots[nmhot].y = rby - cy; mhots[nmhot].w = rbw; mhots[nmhot].h = 34; mhots[nmhot].id = HB_RESET; nmhot++;

    /* mode toggle */
    int mbw = g_text_width(ms.flag_mode ? "Flag" : "Dig", 1) + 22;
    int mbx = cx + cw - PAD - mbw, mby = cy + 9;
    g_round(mbx, mby, mbw, 26, 6, ms.flag_mode ? COL_WARN : COL_ACCENT, 255);
    g_text(mbx + 11, mby + 5, ms.flag_mode ? "Flag" : "Dig", 0xFFFFFF, 1);
    mhots[nmhot].x = mbx - cx; mhots[nmhot].y = mby - cy; mhots[nmhot].w = mbw; mhots[nmhot].h = 26; mhots[nmhot].id = HB_MODE; nmhot++;

    /* grid */
    int gx = cx + PAD, gy = cy + HEADH + PAD;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int x = gx + c * CELL, y = gy + r * CELL;
            uint8_t st = ms.state[r][c];
            if (st == HID || st == FLAG) {
                g_round(x + 1, y + 1, CELL - 2, CELL - 2, 5, COL_PANEL_3, 255);
                g_hline(x + 3, y + 2, CELL - 6, 0x3A3A48);          /* top highlight */
                if (st == FLAG) {
                    g_fill(x + CELL / 2 - 1, y + 7, 2, 12, COL_TEXT);
                    g_round(x + CELL / 2 + 1, y + 7, 8, 6, 2, COL_BAD, 255);
                }
            } else {                                                /* revealed */
                g_round(x + 1, y + 1, CELL - 2, CELL - 2, 5, 0x12121A, 255);
                if (ms.mine[r][c]) {
                    g_round(x + CELL / 2 - 5, y + CELL / 2 - 5, 10, 10, 5,
                            ms.over && !ms.won ? COL_BAD : COL_TEXT, 255);
                } else if (ms.adj[r][c]) {
                    char d[2] = { (char)('0' + ms.adj[r][c]), 0 };
                    g_text(x + CELL / 2 - 4, y + CELL / 2 - 7, d, num_color(ms.adj[r][c]), 2);
                }
            }
        }

    /* footer status */
    const char *msg = ms.won ? "You cleared the field!" :
                      ms.over ? "Boom. Press the face to retry." :
                      ms.flag_mode ? "Flag mode: tap cells to mark mines" :
                                     "Dig: left-click reveals, right-click flags";
    g_text(cx + PAD, cy + ch - 18, msg, COL_TEXT_DIM, 1);
}

void mines_app_init(void) {
    memset(&ms, 0, sizeof(ms));
    ms.rng = 0x2545F491;
    new_game();
    int w = COLS * CELL + 2 * PAD + 4;
    int h = HEADH + ROWS * CELL + 2 * PAD + 28;
    window_t *win = gui_add_window("Minesweeper", w, h, 0x34C759, ICON_MINES);
    if (!win) return;
    win->draw   = mines_draw;
    win->click  = mines_click;
    win->rclick = mines_rclick;
    win->x = 200; win->y = 80;
}
