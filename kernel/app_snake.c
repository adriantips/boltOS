/* ===========================================================================
 *  BoltOS  -  kernel/app_snake.c
 *  Snake. The window tick (~6.6 Hz) advances the snake; arrow keys (or WASD)
 *  steer it via the extended-key support in the PS/2 driver. Food placement uses
 *  a small LCG seeded from the PIT. Eat food to grow and score; hitting a wall
 *  or yourself ends the game - any key restarts.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "pit.h"
#include "keyboard.h"
#include "string.h"
#include "commands.h"     /* sh_utoa */

#define GW   20           /* grid width  (cells) */
#define GH   18           /* grid height (cells) */
#define CELL 18
#define HEADH 36
#define PAD  10
#define MAXLEN (GW * GH)

typedef struct { uint8_t x, y; } cell_t;

typedef struct {
    cell_t   body[MAXLEN];
    int      len;
    int      dx, dy;          /* current heading            */
    int      ndx, ndy;        /* queued heading (applied on step) */
    cell_t   food;
    int      score, over, started;
    uint32_t rng;
    uint64_t last_step;
    int      step_ms;
} snake_t;
static snake_t sn;

static uint32_t rnd(void) { sn.rng = sn.rng * 1103515245u + 12345u; return sn.rng >> 8; }

static int on_snake(int x, int y) {
    for (int i = 0; i < sn.len; i++) if (sn.body[i].x == x && sn.body[i].y == y) return 1;
    return 0;
}

static void place_food(void) {
    for (int tries = 0; tries < 500; tries++) {
        int x = rnd() % GW, y = rnd() % GH;
        if (!on_snake(x, y)) { sn.food.x = (uint8_t)x; sn.food.y = (uint8_t)y; return; }
    }
}

static void new_game(void) {
    sn.len = 4;
    for (int i = 0; i < sn.len; i++) { sn.body[i].x = (uint8_t)(6 - i); sn.body[i].y = GH / 2; }
    sn.dx = 1; sn.dy = 0; sn.ndx = 1; sn.ndy = 0;
    sn.score = 0; sn.over = 0; sn.started = 0;
    sn.step_ms = 130;
    sn.rng ^= (uint32_t)pit_ticks() * 2654435761u + 7u;
    place_food();
}

static void step(void) {
    sn.dx = sn.ndx; sn.dy = sn.ndy;
    int nx = sn.body[0].x + sn.dx, ny = sn.body[0].y + sn.dy;

    if (nx < 0 || nx >= GW || ny < 0 || ny >= GH) { sn.over = 1; return; }
    /* self-collision (ignore the tail cell, which moves away this step) */
    for (int i = 0; i < sn.len - 1; i++) if (sn.body[i].x == nx && sn.body[i].y == ny) { sn.over = 1; return; }

    int grow = (nx == sn.food.x && ny == sn.food.y);
    int newlen = sn.len + (grow ? 1 : 0);
    if (newlen > MAXLEN) newlen = MAXLEN;
    for (int i = newlen - 1; i > 0; i--) sn.body[i] = sn.body[i - 1];
    sn.body[0].x = (uint8_t)nx; sn.body[0].y = (uint8_t)ny;
    sn.len = newlen;

    if (grow) {
        sn.score += 10;
        if (sn.step_ms > 60) sn.step_ms -= 4;     /* speed up as you grow */
        place_food();
    }
}

static void snake_tick(window_t *w) {
    (void)w;
    if (sn.over || !sn.started) return;
    uint64_t now = pit_ticks();
    if (now - sn.last_step < (uint64_t)sn.step_ms) return;
    sn.last_step = now;
    step();
}

static void set_dir(int dx, int dy) {
    if (sn.dx == -dx && sn.dy == -dy) return;     /* no 180-degree reversal */
    sn.ndx = dx; sn.ndy = dy;
    sn.started = 1;
}

static void snake_key(window_t *w, char c) {
    (void)w;
    if (sn.over) { new_game(); return; }
    switch ((unsigned char)c) {
    case KEY_LEFT:  case 'a': set_dir(-1, 0); break;
    case KEY_RIGHT: case 'd': set_dir(1, 0);  break;
    case KEY_UP:    case 'w': set_dir(0, -1); break;
    case KEY_DOWN:  case 's': set_dir(0, 1);  break;
    default: break;
    }
}

static void snake_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w; (void)cw; (void)ch;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    /* header: score + hint */
    g_fill(cx, cy, cw, HEADH, COL_PANEL_2);
    g_text(cx + PAD, cy + 11, "Score", COL_TEXT_DIM, 1);
    char sc[8]; sh_utoa((uint64_t)sn.score, sc);
    g_text(cx + PAD + 48, cy + 9, sc, COL_ACCENT, 2);
    const char *hint = sn.over ? "Game over - press any key" :
                       !sn.started ? "Arrow keys / WASD to start" : "";
    if (hint[0]) g_text(cx + cw - g_text_width(hint, 1) - PAD, cy + 12, hint, COL_TEXT_DIM, 1);

    /* play field */
    int fx = cx + PAD, fy = cy + HEADH + PAD;
    int fw = GW * CELL, fh = GH * CELL;
    g_round(fx - 2, fy - 2, fw + 4, fh + 4, 6, 0x0C0C12, 255);

    /* food */
    g_round(fx + sn.food.x * CELL + 3, fy + sn.food.y * CELL + 3, CELL - 6, CELL - 6,
            (CELL - 6) / 2, COL_BAD, 255);

    /* snake: brighter head, gradient body */
    for (int i = 0; i < sn.len; i++) {
        int x = fx + sn.body[i].x * CELL, y = fy + sn.body[i].y * CELL;
        uint32_t col = (i == 0) ? 0x6FE38A : COL_GOOD;
        g_round(x + 1, y + 1, CELL - 2, CELL - 2, 4, col, 255);
    }
}

void snake_app_init(void) {
    memset(&sn, 0, sizeof(sn));
    sn.rng = 0x1F123BB5;
    new_game();
    int w = GW * CELL + 2 * PAD + 4;
    int h = HEADH + GH * CELL + 2 * PAD + 4;
    window_t *win = gui_add_window("Snake", w, h, 0x34C759, ICON_SNAKE);
    if (!win) return;
    win->draw = snake_draw;
    win->key  = snake_key;
    win->tick = snake_tick;
    win->x = 260; win->y = 70;
}
