/* ===========================================================================
 *  BoltOS  -  kernel/app_memory.c
 *  Memory / concentration game. A 4x4 board of face-down cards hides 8 pairs of
 *  coloured letters. Flip two: a match stays revealed, otherwise both flip back
 *  after a short delay (timed from the window tick). Clear all pairs to win.
 *  Cards are shuffled with a PIT-seeded LCG.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "pit.h"
#include "string.h"
#include "commands.h"     /* sh_utoa */

#define ROWS 4
#define COLS 4
#define NCARD (ROWS * COLS)
#define HEADH 44
#define PAD   12
#define GAP   10

enum { DOWN = 0, UP, MATCHED };

typedef struct {
    int      val[NCARD];      /* 0..7 symbol id */
    int      st[NCARD];
    int      first, second;   /* currently flipped card indices, -1 if none */
    uint64_t flip_back_at;    /* tick to hide a non-matching pair */
    int      moves, pairs, won;
    uint32_t rng;
} mem_t;
static mem_t M;

static int nb_x, nb_y, nb_w, nb_h;

static const char  SYM[8]    = { 'A','B','C','D','E','F','G','H' };
static const uint32_t SCOL[8] = {
    0xE0556B, 0xFFB454, 0xF6D32D, 0x34C759, 0x4FC3F7, 0x4F8DF7, 0x9B6CF2, 0xFF8AC4
};

static uint32_t rnd(void) { M.rng = M.rng * 1103515245u + 12345u; return M.rng >> 8; }

static void new_game(void) {
    for (int i = 0; i < NCARD; i++) { M.val[i] = i / 2; M.st[i] = DOWN; }
    M.rng ^= (uint32_t)pit_ticks() * 2654435761u + 9u;
    for (int i = NCARD - 1; i > 0; i--) {        /* Fisher-Yates shuffle */
        int j = rnd() % (i + 1);
        int t = M.val[i]; M.val[i] = M.val[j]; M.val[j] = t;
    }
    M.first = M.second = -1;
    M.moves = M.pairs = M.won = 0;
    M.flip_back_at = 0;
}

static void mem_tick(window_t *w) {
    (void)w;
    if (M.second >= 0 && M.flip_back_at && pit_ticks() >= M.flip_back_at) {
        M.st[M.first] = DOWN; M.st[M.second] = DOWN;
        M.first = M.second = -1; M.flip_back_at = 0;
    }
}

static void mem_click(window_t *w, int lx, int ly) {
    (void)w;
    if (lx >= nb_x && lx < nb_x + nb_w && ly >= nb_y && ly < nb_y + nb_h) { new_game(); return; }
    if (M.second >= 0) return;                    /* waiting for the flip-back */

    int gx = PAD, gy = HEADH + PAD;
    int avail = (w->w - 2 * PAD < w->h - HEADH - 2 * PAD ? w->w - 2 * PAD : w->h - HEADH - 2 * PAD);
    int cs = (avail - (COLS - 1) * GAP) / COLS;
    int c = (lx - gx) / (cs + GAP), r = (ly - gy) / (cs + GAP);
    if (c < 0 || c >= COLS || r < 0 || r >= ROWS) return;
    int idx = r * COLS + c;
    if (M.st[idx] != DOWN) return;

    M.st[idx] = UP;
    if (M.first < 0) { M.first = idx; return; }

    M.second = idx; M.moves++;
    if (M.val[M.first] == M.val[M.second]) {      /* match */
        M.st[M.first] = MATCHED; M.st[M.second] = MATCHED;
        M.first = M.second = -1;
        if (++M.pairs == NCARD / 2) M.won = 1;
    } else {
        M.flip_back_at = pit_ticks() + 700;       /* show briefly, then hide */
    }
}

static void mem_draw(window_t *win, int cx, int cy, int cw, int ch) {
    (void)win;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    /* header */
    g_fill(cx, cy, cw, HEADH, COL_PANEL_2);
    char info[32], t[8];
    info[0] = 0;
    kstrlcat(info, "Moves ", sizeof(info)); sh_utoa((uint64_t)M.moves, t); kstrlcat(info, t, sizeof(info));
    kstrlcat(info, "   Pairs ", sizeof(info)); sh_utoa((uint64_t)M.pairs, t); kstrlcat(info, t, sizeof(info));
    kstrlcat(info, "/8", sizeof(info));
    g_text(cx + PAD, cy + 15, info, COL_TEXT, 1);

    const char *nb = M.won ? "You win! New?" : "New Game";
    nb_w = g_text_width(nb, 1) + 22; nb_h = 26;
    nb_x = cx + cw - PAD - nb_w; nb_y = cy + 9;
    g_round(nb_x, nb_y, nb_w, nb_h, 7, M.won ? COL_GOOD : COL_ACCENT, 255);
    g_text(nb_x + 11, nb_y + 5, nb, 0xFFFFFF, 1);
    nb_x -= cx; nb_y -= cy;

    /* board */
    int gx = cx + PAD, gy = cy + HEADH + PAD;
    int avail = (cw - 2 * PAD < ch - HEADH - 2 * PAD ? cw - 2 * PAD : ch - HEADH - 2 * PAD);
    int cs = (avail - (COLS - 1) * GAP) / COLS;
    for (int i = 0; i < NCARD; i++) {
        int r = i / COLS, c = i % COLS;
        int x = gx + c * (cs + GAP), y = gy + r * (cs + GAP);
        if (M.st[i] == DOWN) {
            g_round(x, y, cs, cs, 10, COL_PANEL_3, 255);
            gui_icon(ICON_START, x + cs / 2 - 8, y + cs / 2 - 8, 1, COL_ACCENT_DIM);  /* bolt back */
        } else {
            int matched = (M.st[i] == MATCHED);
            uint32_t col = SCOL[M.val[i]];
            g_round(x, y, cs, cs, 10, matched ? COL_PANEL_2 : 0x1A1A24, 255);
            g_rect(x, y, cs, cs, col);
            char s[2] = { SYM[M.val[i]], 0 };
            int scale = 4;
            g_text(x + (cs - g_text_width(s, scale)) / 2, y + (cs - 8 * scale) / 2, s,
                   matched ? COL_TEXT_DIM : col, scale);
        }
    }
}

void memory_app_init(void) {
    memset(&M, 0, sizeof(M));
    M.rng = 0x51ED2701;
    new_game();
    window_t *win = gui_add_window("Memory", 360, 420, 0xFF8AC4, ICON_MEMORY);
    if (!win) return;
    win->draw  = mem_draw;
    win->click = mem_click;
    win->tick  = mem_tick;
    win->min_w = 300; win->min_h = 360;
    win->x = 340; win->y = 90;
}
