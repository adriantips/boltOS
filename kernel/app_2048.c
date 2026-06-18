/* ===========================================================================
 *  BoltOS  -  kernel/app_2048.c
 *  2048: slide the 4x4 board with the arrow keys (or WASD); equal tiles merge
 *  and the score climbs. A new 2 or 4 spawns after every move that changes the
 *  board. Reach 2048 to win; fill the board with no moves left to lose. Pure
 *  integer logic; random spawns from an LCG seeded off the PIT.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "pit.h"
#include "keyboard.h"
#include "string.h"
#include "commands.h"     /* sh_utoa */

#define N     4
#define HEADH 56
#define PAD   14
#define GAP   10

typedef struct {
    int      g[N][N];
    int      score, won, over;
    uint32_t rng;
} g2048_t;
static g2048_t G;

/* header hot rect (New Game) */
static int nb_x, nb_y, nb_w, nb_h;

static uint32_t rnd(void) { G.rng = G.rng * 1103515245u + 12345u; return G.rng >> 8; }

static void spawn(void) {
    int empty = 0;
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) if (!G.g[r][c]) empty++;
    if (!empty) return;
    int k = rnd() % empty;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            if (!G.g[r][c] && k-- == 0) { G.g[r][c] = (rnd() % 10 == 0) ? 4 : 2; return; }
}

static void new_game(void) {
    memset(G.g, 0, sizeof(G.g));
    G.score = 0; G.won = 0; G.over = 0;
    G.rng ^= (uint32_t)pit_ticks() * 2654435761u + 3u;
    spawn(); spawn();
}

/* slide/merge the four cells pointed to by p[] toward p[0]; returns 1 if moved */
static int slide(int *p0, int *p1, int *p2, int *p3) {
    int *p[N] = { p0, p1, p2, p3 };
    int tmp[N], n = 0;
    for (int i = 0; i < N; i++) if (*p[i]) tmp[n++] = *p[i];
    for (int i = n; i < N; i++) tmp[i] = 0;
    for (int i = 0; i < N - 1; i++)
        if (tmp[i] && tmp[i] == tmp[i + 1]) {
            tmp[i] *= 2; G.score += tmp[i];
            if (tmp[i] == 2048) G.won = 1;
            tmp[i + 1] = 0;
        }
    int out[N], m = 0;
    for (int i = 0; i < N; i++) if (tmp[i]) out[m++] = tmp[i];
    for (int i = m; i < N; i++) out[i] = 0;
    int changed = 0;
    for (int i = 0; i < N; i++) { if (*p[i] != out[i]) changed = 1; *p[i] = out[i]; }
    return changed;
}

static int any_moves(void) {
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            if (!G.g[r][c]) return 1;
            if (c < N - 1 && G.g[r][c] == G.g[r][c + 1]) return 1;
            if (r < N - 1 && G.g[r][c] == G.g[r + 1][c]) return 1;
        }
    return 0;
}

static void move(int dir) {   /* 0 left, 1 right, 2 up, 3 down */
    if (G.over) return;
    int changed = 0;
    for (int i = 0; i < N; i++) {
        if (dir == 0)      changed |= slide(&G.g[i][0], &G.g[i][1], &G.g[i][2], &G.g[i][3]);
        else if (dir == 1) changed |= slide(&G.g[i][3], &G.g[i][2], &G.g[i][1], &G.g[i][0]);
        else if (dir == 2) changed |= slide(&G.g[0][i], &G.g[1][i], &G.g[2][i], &G.g[3][i]);
        else               changed |= slide(&G.g[3][i], &G.g[2][i], &G.g[1][i], &G.g[0][i]);
    }
    if (changed) { spawn(); if (!any_moves()) G.over = 1; }
}

static void g2048_key(window_t *w, char c) {
    (void)w;
    switch ((unsigned char)c) {
    case KEY_LEFT:  case 'a': move(0); break;
    case KEY_RIGHT: case 'd': move(1); break;
    case KEY_UP:    case 'w': move(2); break;
    case KEY_DOWN:  case 's': move(3); break;
    case 'n': case 'r': new_game(); break;
    default: break;
    }
}

static void g2048_click(window_t *w, int lx, int ly) {
    (void)w;
    if (lx >= nb_x && lx < nb_x + nb_w && ly >= nb_y && ly < nb_y + nb_h) new_game();
}

/* tile face + text colour by value */
static uint32_t tile_color(int v) {
    switch (v) {
    case 2:    return 0x3A3A48; case 4:    return 0x45506B; case 8:    return 0x4F8DF7;
    case 16:   return 0x4FC3F7; case 32:   return 0x34C759; case 64:   return 0x8FD14F;
    case 128:  return 0xF6D32D; case 256:  return 0xFFB454; case 512:  return 0xFF8A3D;
    case 1024: return 0xE0556B; case 2048: return 0x9B6CF2; default:   return 0x6A4FC2;
    }
}

static void g2048_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    /* header: score + new game */
    g_text(cx + PAD, cy + 10, "Score", COL_TEXT_DIM, 1);
    char sc[12]; sh_utoa((uint64_t)G.score, sc);
    g_text(cx + PAD, cy + 26, sc, COL_TEXT, 2);

    const char *nb = "New Game";
    nb_w = g_text_width(nb, 1) + 24; nb_h = 28;
    nb_x = cx + cw - PAD - nb_w; nb_y = cy + 14;
    g_round(nb_x, nb_y, nb_w, nb_h, 7, COL_ACCENT, 255);
    g_text(nb_x + 12, nb_y + 7, nb, 0xFFFFFF, 1);
    nb_x -= cx; nb_y -= cy;                       /* store client-local for hit-test */

    /* board */
    int bx = cx + PAD, by = cy + HEADH;
    int board = (cw - 2 * PAD < ch - HEADH - PAD ? cw - 2 * PAD : ch - HEADH - PAD);
    int ts = (board - (N + 1) * GAP) / N;
    g_round(bx, by, board, board, 12, COL_PANEL_3, 255);

    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            int x = bx + GAP + c * (ts + GAP), y = by + GAP + r * (ts + GAP);
            int v = G.g[r][c];
            if (!v) { g_round(x, y, ts, ts, 8, 0x20202C, 255); continue; }
            g_round(x, y, ts, ts, 8, tile_color(v), 255);
            char t[8]; sh_utoa((uint64_t)v, t);
            int scale = v >= 1000 ? 2 : 3;
            int tw = g_text_width(t, scale);
            uint32_t tc = (v <= 4) ? COL_TEXT : 0xFFFFFF;
            g_text(x + (ts - tw) / 2, y + (ts - 8 * scale) / 2, t, tc, scale);
        }

    /* overlay status */
    if (G.over || G.won) {
        const char *msg = G.won ? "You made 2048!" : "Game over";
        int tw = g_text_width(msg, 2);
        g_blend(bx, by, board, board, 0x000000, 150);
        g_text(bx + (board - tw) / 2, by + board / 2 - 18, msg, G.won ? COL_GOOD : COL_BAD, 2);
        const char *hint = "Press New Game or R";
        int hw = g_text_width(hint, 1);
        g_text(bx + (board - hw) / 2, by + board / 2 + 10, hint, COL_TEXT, 1);
    }
}

void g2048_app_init(void) {
    memset(&G, 0, sizeof(G));
    G.rng = 0x9E3779B1;
    new_game();
    window_t *win = gui_add_window("2048", 360, 430, 0xF6D32D, ICON_2048);
    if (!win) return;
    win->draw  = g2048_draw;
    win->key   = g2048_key;
    win->click = g2048_click;
    win->min_w = 300; win->min_h = 380;
    win->x = 320; win->y = 90;
}
