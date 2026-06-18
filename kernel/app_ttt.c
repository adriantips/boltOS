/* ===========================================================================
 *  BoltOS  -  kernel/app_ttt.c
 *  Tic-Tac-Toe vs an unbeatable AI. You are X; the computer is O and plays a
 *  full minimax search (3x3 is tiny, so it explores the whole game tree every
 *  move and never loses). Click an empty square to play; the AI replies. A
 *  running tally of wins/losses/draws sits at the bottom.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "string.h"
#include "commands.h"     /* sh_utoa */

#define EMPTY 0
#define X 1
#define O 2

typedef struct {
    int b[9];
    int turn;            /* whose move: X (player) or O (AI) */
    int over, winner;    /* winner: 0 none/draw */
    int win_line;        /* index 0..7 of the winning line, or -1 */
    int wins, losses, draws;
} ttt_t;
static ttt_t T;

static int nb_x, nb_y, nb_w, nb_h;   /* New Game button (client-local) */

static const int LINES[8][3] = {
    {0,1,2},{3,4,5},{6,7,8},        /* rows */
    {0,3,6},{1,4,7},{2,5,8},        /* cols */
    {0,4,8},{2,4,6}                 /* diagonals */
};

/* returns X or O if that player has a line, else 0; sets *line to its index */
static int check_winner(const int *b, int *line) {
    for (int i = 0; i < 8; i++) {
        int a = b[LINES[i][0]];
        if (a && a == b[LINES[i][1]] && a == b[LINES[i][2]]) { if (line) *line = i; return a; }
    }
    if (line) *line = -1;
    return 0;
}

static int board_full(const int *b) {
    for (int i = 0; i < 9; i++) if (!b[i]) return 0;
    return 1;
}

/* minimax from O's perspective; `turn` is whose move it is now */
static int minimax(int *b, int turn, int depth) {
    int w = check_winner(b, 0);
    if (w == O) return 10 - depth;
    if (w == X) return depth - 10;
    if (board_full(b)) return 0;

    int best = (turn == O) ? -1000 : 1000;
    for (int i = 0; i < 9; i++) {
        if (b[i]) continue;
        b[i] = turn;
        int s = minimax(b, turn == X ? O : X, depth + 1);
        b[i] = EMPTY;
        if (turn == O) { if (s > best) best = s; }
        else           { if (s < best) best = s; }
    }
    return best;
}

static void settle(void) {
    T.winner = check_winner(T.b, &T.win_line);
    if (T.winner == X) { T.over = 1; T.wins++; }
    else if (T.winner == O) { T.over = 1; T.losses++; }
    else if (board_full(T.b)) { T.over = 1; T.draws++; }
}

static void ai_move(void) {
    int best = -1000, bi = -1;
    for (int i = 0; i < 9; i++) {
        if (T.b[i]) continue;
        T.b[i] = O;
        int s = minimax(T.b, X, 0);
        T.b[i] = EMPTY;
        if (s > best) { best = s; bi = i; }
    }
    if (bi >= 0) T.b[bi] = O;
    T.turn = X;
}

static void new_game(void) {
    for (int i = 0; i < 9; i++) T.b[i] = EMPTY;
    T.turn = X; T.over = 0; T.winner = 0; T.win_line = -1;
}

static void ttt_click(window_t *w, int lx, int ly) {
    (void)w;
    if (lx >= nb_x && lx < nb_x + nb_w && ly >= nb_y && ly < nb_y + nb_h) { new_game(); return; }
    if (T.over || T.turn != X) return;

    /* grid hit-test (must match draw geometry) */
    int gx = 16, gy = 56, gsz = w->w - 32;
    if (gsz > w->h - 56 - 56) gsz = w->h - 56 - 56;
    int cell = gsz / 3;
    int c = (lx - gx) / cell, r = (ly - gy) / cell;
    if (c < 0 || c > 2 || r < 0 || r > 2) return;
    int idx = r * 3 + c;
    if (T.b[idx]) return;

    T.b[idx] = X; T.turn = O;
    settle();
    if (!T.over) { ai_move(); settle(); }
}

/* X drawn as two diagonals from small squares */
static void draw_x(int x, int y, int s, uint32_t col) {
    int steps = s;
    for (int i = 0; i <= steps; i++) {
        g_fill(x + i - 2, y + i - 2, 4, 4, col);
        g_fill(x + (s - i) - 2, y + i - 2, 4, 4, col);
    }
}

static void ttt_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    /* status line */
    const char *msg = T.over ? (T.winner == X ? "You win!" : T.winner == O ? "AI wins" : "Draw")
                             : (T.turn == X ? "Your move (X)" : "AI thinking...");
    uint32_t mc = T.over ? (T.winner == X ? COL_GOOD : T.winner == O ? COL_BAD : COL_TEXT) : COL_TEXT;
    g_text(cx + 16, cy + 18, msg, mc, 2);

    /* board geometry */
    int gx = cx + 16, gy = cy + 56, gsz = cw - 32;
    if (gsz > ch - 56 - 56) gsz = ch - 56 - 56;
    int cell = gsz / 3;
    g_round(gx, gy, cell * 3, cell * 3, 12, COL_PANEL_2, 255);
    for (int i = 1; i < 3; i++) {
        g_fill(gx + i * cell, gy + 8, 2, cell * 3 - 16, COL_PANEL_3);
        g_fill(gx + 8, gy + i * cell, cell * 3 - 16, 2, COL_PANEL_3);
    }

    /* winning line highlight */
    if (T.winner && T.win_line >= 0) {
        for (int k = 0; k < 3; k++) {
            int idx = LINES[T.win_line][k];
            int r = idx / 3, c = idx % 3;
            g_round(gx + c * cell + 6, gy + r * cell + 6, cell - 12, cell - 12, 10,
                    T.winner == X ? COL_GOOD : COL_BAD, 60);
        }
    }

    /* marks */
    for (int i = 0; i < 9; i++) {
        int r = i / 3, c = i % 3;
        int x = gx + c * cell, y = gy + r * cell;
        int pad = cell / 4, m = cell - 2 * pad;
        if (T.b[i] == X) draw_x(x + pad, y + pad, m, COL_ACCENT);
        else if (T.b[i] == O) {
            int rad = m / 2;
            g_round(x + pad, y + pad, m, m, rad, 0xF6D32D, 255);
            g_round(x + pad + 6, y + pad + 6, m - 12, m - 12, rad - 6, COL_PANEL_2, 255);
        }
    }

    /* score + new game */
    char sc[40], t[8];
    sc[0] = 0;
    kstrlcat(sc, "W ", sizeof(sc)); sh_utoa((uint64_t)T.wins, t); kstrlcat(sc, t, sizeof(sc));
    kstrlcat(sc, "  L ", sizeof(sc)); sh_utoa((uint64_t)T.losses, t); kstrlcat(sc, t, sizeof(sc));
    kstrlcat(sc, "  D ", sizeof(sc)); sh_utoa((uint64_t)T.draws, t); kstrlcat(sc, t, sizeof(sc));
    g_text(cx + 16, cy + ch - 28, sc, COL_TEXT_DIM, 1);

    const char *nb = "New Game";
    nb_w = g_text_width(nb, 1) + 24; nb_h = 28;
    nb_x = cx + cw - 16 - nb_w; nb_y = cy + ch - 36;
    g_round(nb_x, nb_y, nb_w, nb_h, 7, COL_ACCENT, 255);
    g_text(nb_x + 12, nb_y + 7, nb, 0xFFFFFF, 1);
    nb_x -= cx; nb_y -= cy;
}

void ttt_app_init(void) {
    memset(&T, 0, sizeof(T));
    new_game();
    window_t *win = gui_add_window("Tic-Tac-Toe", 300, 380, 0xF6D32D, ICON_TTT);
    if (!win) return;
    win->draw  = ttt_draw;
    win->click = ttt_click;
    win->min_w = 260; win->min_h = 340;
    win->x = 360; win->y = 110;
}
