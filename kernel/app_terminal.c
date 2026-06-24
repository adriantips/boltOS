/* ===========================================================================
 *  BoltOS  -  kernel/app_terminal.c
 *  A terminal window. Keeps its own character grid, edits a command line from
 *  the focused-window key stream, and runs each line through the real BoltShell
 *  dispatcher with kputc redirected into this grid (see console_set_sink).
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "console.h"
#include "shell.h"
#include "string.h"
#include "pit.h"
#include "fs.h"
#include "keyboard.h"
#include "clipboard.h"

#define TCOLS 220
#define TROWS 80
#define PAD   8
#define CELLW 8
#define CELLH 14

typedef struct {
    char    ch [TROWS][TCOLS];
    uint8_t col[TROWS][TCOLS];
    int     cols, rows;
    int     cx, cy;
    uint8_t curcol;
    char    line[256];
    int     len;
} term_t;

static term_t  term;
static term_t *active;                 /* terminal currently capturing kputc */

/* palette index -> colour */
static uint32_t pal(uint8_t i) {
    switch (i) {
    case 1:  return 0x4FC3F7;          /* prompt / accent */
    case 2:  return 0x7C7C8A;          /* dim             */
    case 3:  return COL_GOOD;
    case 4:  return COL_BAD;
    default: return 0xD8D8E0;          /* normal          */
    }
}

static void term_scroll(term_t *t) {
    for (int r = 1; r < t->rows; r++)
        for (int c = 0; c < TCOLS; c++) { t->ch[r - 1][c] = t->ch[r][c]; t->col[r - 1][c] = t->col[r][c]; }
    for (int c = 0; c < TCOLS; c++) { t->ch[t->rows - 1][c] = 0; t->col[t->rows - 1][c] = 0; }
    t->cy = t->rows - 1;
}
static void term_newline(term_t *t) { t->cx = 0; if (++t->cy >= t->rows) term_scroll(t); }

static void term_putc(term_t *t, char c) {
    if (t->cols <= 0 || t->rows <= 0) return;
    if (c == '\n') { term_newline(t); return; }
    if (c == '\r') { t->cx = 0; return; }
    if (c == '\t') { int n = 4 - (t->cx & 3); while (n--) term_putc(t, ' '); return; }
    if (c == '\b') {
        if (t->cx > 0) t->cx--;
        else if (t->cy > 0) { t->cy--; t->cx = t->cols - 1; }
        t->ch[t->cy][t->cx] = 0;
        return;
    }
    if ((unsigned char)c < 32) return;
    t->ch[t->cy][t->cx]  = c;
    t->col[t->cy][t->cx] = t->curcol;
    if (++t->cx >= t->cols) term_newline(t);
}

static void term_puts(term_t *t, const char *s, uint8_t color) {
    uint8_t save = t->curcol; t->curcol = color;
    while (*s) term_putc(t, *s++);
    t->curcol = save;
}

static void term_prompt(term_t *t) {
    char path[FS_PATH_MAX];
    fs_abspath(fs_cwd(), path, sizeof(path));
    term_puts(t, "bolt:", 1);
    term_puts(t, path, 1);
    term_puts(t, "> ", 1);
}

/* kputc sink while a command runs */
static void term_sink(char c) { if (active) term_putc(active, c); }

static void term_exec(term_t *t) {
    t->line[t->len] = 0;
    if (t->len == 0) { term_prompt(t); return; }
    active = t; t->curcol = 0;
    console_set_sink(term_sink);
    shell_exec_line(t->line);
    console_set_sink(0);
    active = 0;
    term_prompt(t);
}

static void term_key(window_t *w, char c) {
    (void)w;
    term_t *t = &term;
    if (c == '\n') {
        term_putc(t, '\n');
        term_exec(t);
        t->len = 0;
    } else if (c == '\b') {
        if (t->len > 0) { t->len--; term_putc(t, '\b'); }
    } else if ((unsigned char)c == KEY_COPY) {
        clip_set(t->line, t->len);               /* copy the current input line */
    } else if ((unsigned char)c == KEY_PASTE) {
        const char *p = clip_get();              /* paste: replay each char (\n runs it) */
        for (int i = 0; p[i]; i++)
            if (p[i] != '\r') term_key(w, p[i]);
    } else if ((unsigned char)c >= 32 && t->len < (int)sizeof(t->line) - 1) {
        t->line[t->len++] = c;
        term_putc(t, c);
    }
}

static void term_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    term_t *t = &term;
    g_fill(cx, cy, cw, ch, 0x0C0C12);            /* terminal background */

    t->cols = (cw - 2 * PAD) / CELLW; if (t->cols > TCOLS) t->cols = TCOLS; if (t->cols < 1) t->cols = 1;
    t->rows = (ch - 2 * PAD) / CELLH; if (t->rows > TROWS) t->rows = TROWS; if (t->rows < 1) t->rows = 1;

    for (int r = 0; r < t->rows; r++) {
        int py = cy + PAD + r * CELLH;
        for (int c = 0; c < t->cols; c++) {
            char g = t->ch[r][c];
            if (g) g_char(cx + PAD + c * CELLW, py, g, pal(t->col[r][c]), 1);
        }
    }

    /* blinking caret (only when focused) */
    if (gui_window_focused(w) && (pit_ticks() / 500) % 2 == 0) {
        int px = cx + PAD + t->cx * CELLW;
        int py = cy + PAD + t->cy * CELLH;
        g_fill(px, py, 2, CELLH - 2, 0x4FC3F7);
    }
}

void terminal_app_init(void) {
    memset(&term, 0, sizeof(term));
    term.cols = 88; term.rows = 29;              /* default grid until first draw */
    window_t *w = gui_add_window("Terminal", 720, 460, COL_ACCENT, ICON_TERMINAL);
    if (!w) return;
    w->draw = term_draw;
    w->key  = term_key;

    term_puts(&term, "BoltOS Terminal  -  BoltShell\n", 1);
    term_puts(&term, "Type 'help' for the full command list.\n\n", 2);
    term_prompt(&term);
}
