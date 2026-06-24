/* ===========================================================================
 *  BoltOS  -  kernel/app_notes.c
 *  Notepad: a plain-text editor backed by the filesystem. Edits a flat text
 *  buffer with a movable caret (arrow keys / Home / End / Del, courtesy of the
 *  extended-key support in the PS/2 driver), and saves to /notes.txt with the
 *  Save button on the toolbar. The file is loaded back on boot, and because the
 *  FS autosaves to disk, notes survive reboots.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "fs.h"
#include "keyboard.h"
#include "clipboard.h"
#include "pit.h"
#include "string.h"

#define BUFCAP 16384
#define NPATH  "/notes.txt"
#define CELLW  8
#define CELLH  16
#define PAD    10
#define TOOLH  36

typedef struct {
    char buf[BUFCAP];
    int  len;
    int  cur;            /* caret position, 0..len */
    int  sel;            /* selection anchor, or -1 if no selection           */
    int  top;            /* first visible line (vertical scroll)              */
    int  dirty;          /* unsaved changes                                   */
    int  saved_flash;    /* pit tick until which to show "Saved"              */
} notes_t;

static notes_t nt;

/* toolbar button hot rects (client-local), rebuilt each draw */
enum { B_SAVE = 1, B_NEW };
typedef struct { int x, y, w, h, id; } nhot_t;
static nhot_t nhots[4];
static int    nnhot;

/* ---- buffer editing ----------------------------------------------------- */
static void nt_insert(char c) {
    if (nt.len >= BUFCAP - 1) return;
    for (int i = nt.len; i > nt.cur; i--) nt.buf[i] = nt.buf[i - 1];
    nt.buf[nt.cur] = c;
    nt.len++; nt.cur++; nt.buf[nt.len] = 0;
    nt.dirty = 1;
}
static void nt_backspace(void) {
    if (nt.cur <= 0) return;
    for (int i = nt.cur - 1; i < nt.len - 1; i++) nt.buf[i] = nt.buf[i + 1];
    nt.len--; nt.cur--; nt.buf[nt.len] = 0;
    nt.dirty = 1;
}
static void nt_delete(void) {
    if (nt.cur >= nt.len) return;
    for (int i = nt.cur; i < nt.len - 1; i++) nt.buf[i] = nt.buf[i + 1];
    nt.len--; nt.buf[nt.len] = 0;
    nt.dirty = 1;
}

/* ---- selection + clipboard --------------------------------------------- */
static int  sel_active(void) { return nt.sel >= 0 && nt.sel != nt.cur; }
static int  sel_lo(void)     { return nt.sel < nt.cur ? nt.sel : nt.cur; }
static int  sel_hi(void)     { return nt.sel < nt.cur ? nt.cur : nt.sel; }

/* Delete the selected span; leaves the caret at its start and clears selection.
 * Returns 1 if anything was removed. */
static int sel_erase(void) {
    if (!sel_active()) { nt.sel = -1; return 0; }
    int a = sel_lo(), b = sel_hi(), n = b - a;
    for (int i = a; i + n <= nt.len; i++) nt.buf[i] = nt.buf[i + n];
    nt.len -= n; nt.cur = a; nt.sel = -1; nt.dirty = 1;
    return 1;
}
static void sel_copy(void) {
    if (sel_active()) clip_set(nt.buf + sel_lo(), sel_hi() - sel_lo());
}
static void nt_paste(void) {
    sel_erase();
    const char *p = clip_get();
    for (int i = 0; p[i]; i++) {
        char c = p[i];
        if (c == '\r') continue;                 /* normalise CRLF -> LF */
        if (c != '\n' && c != '\t' && (unsigned char)c < 32) continue;
        nt_insert(c);
    }
}
/* Begin/extend a shift-selection: anchor on first extend, then caller moves cur. */
static void sel_start(void) { if (nt.sel < 0) nt.sel = nt.cur; }

/* column of the caret within its line (chars since the previous '\n') */
static int caret_col(void) {
    int col = 0;
    for (int i = nt.cur - 1; i >= 0 && nt.buf[i] != '\n'; i--) col++;
    return col;
}
static int line_start(int pos) {
    int i = pos; while (i > 0 && nt.buf[i - 1] != '\n') i--; return i;
}
static int line_end(int pos) {
    int i = pos; while (i < nt.len && nt.buf[i] != '\n') i++; return i;
}

static void nt_up(void) {
    int col = caret_col();
    int ls = line_start(nt.cur);
    if (ls == 0) { nt.cur = 0; return; }
    int prev = line_start(ls - 1);
    int plen = (ls - 1) - prev;          /* chars on previous line */
    nt.cur = prev + (col < plen ? col : plen);
}
static void nt_down(void) {
    int col = caret_col();
    int le = line_end(nt.cur);
    if (le >= nt.len) { nt.cur = nt.len; return; }
    int next = le + 1;
    int nlen = line_end(next) - next;
    nt.cur = next + (col < nlen ? col : nlen);
}

/* ---- file I/O ----------------------------------------------------------- */
static void nt_load(void) {
    fs_node *n = fs_lookup(NPATH);
    if (n && !n->is_dir && n->data) {
        int len = (int)n->size; if (len > BUFCAP - 1) len = BUFCAP - 1;
        memcpy(nt.buf, n->data, len);
        nt.len = len; nt.buf[len] = 0;
    } else {
        const char *seed = "Welcome to BoltOS Notepad.\n\nType freely; use the arrow keys to move the\ncaret, and press Save to write to /notes.txt.\n";
        int len = (int)strlen(seed);
        memcpy(nt.buf, seed, len); nt.len = len; nt.buf[len] = 0;
    }
    nt.cur = 0; nt.top = 0; nt.dirty = 0;
}
static void nt_save(void) {
    fs_node *n = fs_lookup(NPATH);
    if (!n) n = fs_create(NPATH, 0);
    if (n && fs_write(n, nt.buf, (uint32_t)nt.len) == 0) {
        nt.dirty = 0;
        nt.saved_flash = (int)pit_ticks() + 1500;
    }
}
static void nt_new(void) {
    nt.len = 0; nt.cur = 0; nt.sel = -1; nt.top = 0; nt.buf[0] = 0; nt.dirty = 1;
}

/* ---- input -------------------------------------------------------------- */
static void notes_key(window_t *w, char c) {
    (void)w;
    switch ((unsigned char)c) {
    /* plain navigation: clears any selection */
    case KEY_LEFT:  nt.sel = -1; if (nt.cur > 0) nt.cur--; break;
    case KEY_RIGHT: nt.sel = -1; if (nt.cur < nt.len) nt.cur++; break;
    case KEY_UP:    nt.sel = -1; nt_up();   break;
    case KEY_DOWN:  nt.sel = -1; nt_down(); break;
    case KEY_HOME:  nt.sel = -1; nt.cur = line_start(nt.cur); break;
    case KEY_END:   nt.sel = -1; nt.cur = line_end(nt.cur);   break;
    /* shift+navigation: extend selection */
    case KEY_SLEFT:  sel_start(); if (nt.cur > 0) nt.cur--; break;
    case KEY_SRIGHT: sel_start(); if (nt.cur < nt.len) nt.cur++; break;
    case KEY_SUP:    sel_start(); nt_up();   break;
    case KEY_SDOWN:  sel_start(); nt_down(); break;
    case KEY_SHOME:  sel_start(); nt.cur = line_start(nt.cur); break;
    case KEY_SEND:   sel_start(); nt.cur = line_end(nt.cur);   break;
    /* clipboard / editing shortcuts */
    case KEY_SELALL: nt.sel = 0; nt.cur = nt.len; break;
    case KEY_COPY:   sel_copy(); break;
    case KEY_CUT:    sel_copy(); sel_erase(); break;
    case KEY_PASTE:  nt_paste(); break;
    case KEY_SAVE:   nt_save();  break;
    case KEY_DEL:    if (!sel_erase()) nt_delete(); break;
    case '\b':       if (!sel_erase()) nt_backspace(); break;
    case '\n': case '\t':
    default:
        if (c == '\n' || c == '\t' || (unsigned char)c >= 32) {
            sel_erase();
            nt_insert(c);
        }
        break;
    }
}

static void notes_click(window_t *w, int lx, int ly) {
    (void)w;
    for (int i = 0; i < nnhot; i++) {
        nhot_t *h = &nhots[i];
        if (lx < h->x || lx >= h->x + h->w || ly < h->y || ly >= h->y + h->h) continue;
        if (h->id == B_SAVE) nt_save();
        else if (h->id == B_NEW) nt_new();
        return;
    }
}

/* ---- draw --------------------------------------------------------------- */
/* origin of the client area, captured each draw so hot rects can be stored
 * client-local (the click handler receives client-local coordinates). */
static int note_ox, note_oy;

static int tool_btn(int x, int y, const char *label, int id, uint32_t bg) {
    int w = g_text_width(label, 1) + 24, h = 24;
    g_round(x, y, w, h, 6, bg, 255);
    g_text(x + 12, y + 5, label, 0xFFFFFF, 1);
    if (nnhot < 4) { nhots[nnhot].x = x - note_ox; nhots[nnhot].y = y - note_oy;
                     nhots[nnhot].w = w; nhots[nnhot].h = h; nhots[nnhot].id = id; nnhot++; }
    return w;
}

static void notes_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    nnhot = 0;
    note_ox = cx; note_oy = cy;

    /* toolbar */
    g_fill(cx, cy, cw, TOOLH, COL_PANEL_2);
    g_hline(cx, cy + TOOLH, cw, COL_PANEL_3);
    int bx = cx + 10, by = cy + 6;
    bx += tool_btn(bx, by, "Save", B_SAVE, COL_ACCENT) + 8;
    bx += tool_btn(bx, by, "New",  B_NEW,  COL_PANEL_3) + 8;

    /* status: filename + dirty/saved indicator */
    const char *status = NPATH;
    g_text(bx + 6, cy + 11, status, COL_TEXT_DIM, 1);
    int sx = cx + cw - 80;
    if (nt.saved_flash && (int)pit_ticks() < nt.saved_flash)
        g_text(sx, cy + 11, "Saved", COL_GOOD, 1);
    else if (nt.dirty)
        g_text(sx + 30, cy + 11, "*", COL_WARN, 2);

    /* text area */
    int tx = cx + PAD, ty = cy + TOOLH + PAD;
    int area_h = ch - TOOLH - 2 * PAD;
    int vis_rows = area_h / CELLH; if (vis_rows < 1) vis_rows = 1;
    g_fill(cx, cy + TOOLH + 1, cw, ch - TOOLH - 1, 0x0C0C12);

    /* keep the caret on screen: figure out its line, adjust nt.top */
    int caret_line = 0;
    for (int i = 0; i < nt.cur; i++) if (nt.buf[i] == '\n') caret_line++;
    if (caret_line < nt.top) nt.top = caret_line;
    if (caret_line >= nt.top + vis_rows) nt.top = caret_line - vis_rows + 1;

    int line = 0, col = 0;
    int caret_px = tx, caret_py = ty;
    int focused = gui_window_focused(w);
    int slo = sel_active() ? sel_lo() : -1;
    int shi = sel_active() ? sel_hi() : -1;

    for (int i = 0; i <= nt.len; i++) {
        if (i == nt.cur) {
            caret_px = tx + col * CELLW;
            caret_py = ty + (line - nt.top) * CELLH;
        }
        if (i == nt.len) break;
        char c = nt.buf[i];
        int seld = (i >= slo && i < shi);
        if (c == '\n') {
            /* show selection running through the line break as a stub */
            if (seld && line >= nt.top && line < nt.top + vis_rows) {
                int px = tx + col * CELLW;
                if (px < cx + cw - CELLW)
                    g_fill(px, ty + (line - nt.top) * CELLH, CELLW / 2, CELLH, COL_ACCENT);
            }
            line++; col = 0; continue;
        }
        if (c == '\t') {
            int ncol = (col + 4) & ~3;
            if (seld && line >= nt.top && line < nt.top + vis_rows) {
                int px = tx + col * CELLW;
                g_fill(px, ty + (line - nt.top) * CELLH, (ncol - col) * CELLW, CELLH, COL_ACCENT);
            }
            col = ncol; continue;
        }
        if (line >= nt.top && line < nt.top + vis_rows) {
            int px = tx + col * CELLW;
            if (px < cx + cw - CELLW) {
                int py = ty + (line - nt.top) * CELLH;
                if (seld) g_fill(px, py, CELLW, CELLH, COL_ACCENT);
                g_char(px, py, c, seld ? 0xFFFFFF : COL_TEXT, 1);
            }
        }
        col++;
    }

    /* blinking caret */
    if (focused && (pit_ticks() / 500) % 2 == 0 &&
        caret_py >= ty - 2 && caret_py < cy + ch)
        g_fill(caret_px, caret_py, 2, CELLH - 2, COL_ACCENT);
}

void notes_app_init(void) {
    memset(&nt, 0, sizeof(nt));
    nt.sel = -1;
    nt_load();
    window_t *win = gui_add_window("Notepad", 560, 460, 0xFFB454, ICON_NOTES);
    if (!win) return;
    win->draw  = notes_draw;
    win->key   = notes_key;
    win->click = notes_click;
    win->min_w = 320; win->min_h = 240;
    win->x = 240; win->y = 90;
}
