/* ===========================================================================
 *  BoltOS  -  kernel/app_sheets.c
 *  Sheets: a spreadsheet. A 26 x 100 grid of text cells with a formula engine
 *  (=A1+B2, SUM/AVG/MIN/MAX/COUNT over ranges, + - * / and parentheses). All
 *  arithmetic is fixed-point int64 scaled by 1000 -- no hardware float, matching
 *  the rest of the kernel. Saves to /sheet.csv (standard CSV with quoting) and
 *  reloads on boot; the FS autosaves to disk so a workbook survives reboots.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "fs.h"
#include "keyboard.h"
#include "pit.h"
#include "string.h"

#define COLS    26
#define ROWS    100
#define CELLTXT 24            /* max chars stored per cell (incl. NUL)         */
#define SCALE   1000          /* fixed-point: 3 decimal places                 */

#define TOOLH   34            /* toolbar height                                */
#define FBH     22            /* formula-bar height                            */
#define HH      18            /* column-header row height                      */
#define RHW     38            /* row-number column width                       */
#define CW      72            /* data column width                             */
#define RH      18            /* data row height                               */

static char cells[ROWS][COLS][CELLTXT];
static char        sp_path[FS_PATH_MAX] = "/sheet.csv";  /* current workbook    */
static window_t   *sp_win;                                /* our window, for open */

static int  sel_r, sel_c;     /* active cell                                   */
static int  top_row, left_col;/* scroll origin                                 */
static int  editing;          /* 1 while the active cell is being edited       */
static char ebuf[CELLTXT];    /* edit buffer                                   */
static int  elen, ecur;       /* edit length + caret                           */
static int  s_dirty;
static int  s_saved_flash;

/* toolbar hot rects (client-local), rebuilt each draw */
enum { B_SAVE = 1, B_NEW, B_SAVEAS };
typedef struct { int x, y, w, h, id; } shot_t;
static shot_t shots[6];
static int    nshots;

/* ===========================================================================
 *  Fixed-point formula engine
 * ===========================================================================*/
static int eval_err;

static int64_t eval_cell(int r, int c, int depth);
static int64_t parse_expr(const char **p, int depth);

static void skipws(const char **p) { while (**p == ' ') (*p)++; }

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* "[ddd][.ddd]" -> scaled int64. Advances *p. */
static int64_t parse_number(const char **p) {
    const char *s = *p;
    int64_t ip = 0;
    while (is_digit(*s)) { ip = ip * 10 + (*s - '0'); s++; }
    int64_t fp = 0, place = SCALE;
    if (*s == '.') {
        s++;
        while (is_digit(*s) && place > 1) { place /= 10; fp += (*s - '0') * place; s++; }
        while (is_digit(*s)) s++;                 /* drop excess precision     */
    }
    *p = s;
    return ip * SCALE + fp;
}

/* parse a cell reference like "B12" -> row/col (0-based). returns 1 on success */
static int parse_cref(const char **p, int *pr, int *pc) {
    skipws(p);
    const char *s = *p;
    int col = 0, nl = 0;
    while (is_alpha(*s)) { col = col * 26 + (up(*s) - 'A' + 1); s++; nl++; }
    if (!nl) return 0;
    int row = 0, got = 0;
    while (is_digit(*s)) { row = row * 10 + (*s - '0'); s++; got = 1; }
    if (!got) return 0;
    *p = s; *pr = row - 1; *pc = col - 1;
    return 1;
}

static int64_t parse_func(const char *id, const char **p, int depth) {
    int r1, c1, r2, c2;
    if (!parse_cref(p, &r1, &c1)) { eval_err = 1; return 0; }
    skipws(p);
    if (**p == ':') { (*p)++; if (!parse_cref(p, &r2, &c2)) { eval_err = 1; return 0; } }
    else { r2 = r1; c2 = c1; }
    skipws(p);
    if (**p == ')') (*p)++;
    if (r1 > r2) { int t = r1; r1 = r2; r2 = t; }
    if (c1 > c2) { int t = c1; c1 = c2; c2 = t; }

    int64_t sum = 0, mn = 0, mx = 0;
    int cnt = 0;
    for (int r = r1; r <= r2; r++)
        for (int c = c1; c <= c2; c++) {
            if (r < 0 || r >= ROWS || c < 0 || c >= COLS) continue;
            if (cells[r][c][0] == 0) continue;
            int64_t v = eval_cell(r, c, depth + 1);
            if (cnt == 0) { mn = mx = v; } else { if (v < mn) mn = v; if (v > mx) mx = v; }
            sum += v; cnt++;
        }
    if (!strcmp(id, "SUM"))   return sum;
    if (!strcmp(id, "COUNT")) return (int64_t)cnt * SCALE;
    if (!strcmp(id, "AVG") || !strcmp(id, "AVERAGE")) return cnt ? sum / cnt : 0;
    if (!strcmp(id, "MIN"))   return cnt ? mn : 0;
    if (!strcmp(id, "MAX"))   return cnt ? mx : 0;
    eval_err = 1;
    return 0;
}

static int64_t parse_factor(const char **p, int depth) {
    skipws(p);
    char ch = **p;
    if (ch == '-') { (*p)++; return -parse_factor(p, depth); }
    if (ch == '+') { (*p)++; return  parse_factor(p, depth); }
    if (ch == '(') { (*p)++; int64_t v = parse_expr(p, depth); skipws(p); if (**p == ')') (*p)++; return v; }
    if (is_alpha(ch)) {
        const char *s = *p;
        char id[12]; int n = 0;
        while (is_alpha(*s) && n < 11) { id[n++] = up(*s); s++; }
        id[n] = 0;
        if (*s == '(') { *p = s + 1; return parse_func(id, p, depth); }
        /* a cell reference: letters form the column, digits the row */
        int col = 0;
        for (int i = 0; i < n; i++) col = col * 26 + (id[i] - 'A' + 1);
        col -= 1;
        int row = 0, got = 0;
        while (is_digit(*s)) { row = row * 10 + (*s - '0'); s++; got = 1; }
        *p = s;
        if (!got || col < 0 || col >= COLS || row < 1 || row > ROWS) { eval_err = 1; return 0; }
        return eval_cell(row - 1, col, depth + 1);
    }
    if (is_digit(ch) || ch == '.') return parse_number(p);
    eval_err = 1;
    return 0;
}

static int64_t parse_term(const char **p, int depth) {
    int64_t v = parse_factor(p, depth);
    for (;;) {
        skipws(p);
        char op = **p;
        if (op == '*' || op == '/') {
            (*p)++;
            int64_t b = parse_factor(p, depth);
            if (op == '*') v = (v * b) / SCALE;
            else { if (b == 0) { eval_err = 1; v = 0; } else v = (v * SCALE) / b; }
        } else break;
    }
    return v;
}

static int64_t parse_expr(const char **p, int depth) {
    int64_t v = parse_term(p, depth);
    for (;;) {
        skipws(p);
        char op = **p;
        if (op == '+' || op == '-') { (*p)++; int64_t b = parse_term(p, depth); v = (op == '+') ? v + b : v - b; }
        else break;
    }
    return v;
}

/* is the raw cell text a plain number (optionally signed/decimal)? */
static int looks_numeric(const char *t) {
    const char *s = t;
    if (*s == '-' || *s == '+') s++;
    int dig = 0, dot = 0;
    for (; *s; s++) {
        if (is_digit(*s)) dig = 1;
        else if (*s == '.' && !dot) dot = 1;
        else return 0;
    }
    return dig;
}

static int64_t eval_cell(int r, int c, int depth) {
    if (depth > 40) { eval_err = 1; return 0; }       /* circular reference    */
    const char *t = cells[r][c];
    if (t[0] == 0) return 0;
    if (t[0] == '=') { const char *p = t + 1; return parse_expr(&p, depth); }
    if (looks_numeric(t)) {
        const char *p = t;
        int neg = 0;
        if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
        int64_t v = parse_number(&p);
        return neg ? -v : v;
    }
    return 0;                                          /* text label = 0 in math */
}

/* format a scaled int64 into "[-]ddd[.ddd]" trimming trailing zeros */
static void fmt_scaled(int64_t v, char *out, int cap) {
    char tmp[32]; int n = 0;
    int neg = v < 0;
    uint64_t u = (uint64_t)(neg ? -v : v);
    uint64_t ip = u / SCALE, fp = u % SCALE;
    char frac[4];
    frac[0] = '0' + (char)((fp / 100) % 10);
    frac[1] = '0' + (char)((fp / 10) % 10);
    frac[2] = '0' + (char)(fp % 10);
    frac[3] = 0;
    int fl = 3;
    while (fl > 0 && frac[fl - 1] == '0') frac[--fl] = 0;
    if (ip == 0) tmp[n++] = '0';
    while (ip > 0 && n < (int)sizeof(tmp)) { tmp[n++] = '0' + (char)(ip % 10); ip /= 10; }
    int o = 0;
    if (neg && o < cap - 1) out[o++] = '-';
    for (int i = n - 1; i >= 0 && o < cap - 1; i--) out[o++] = tmp[i];
    if (fl > 0 && o < cap - 1) {
        out[o++] = '.';
        for (int i = 0; i < fl && o < cap - 1; i++) out[o++] = frac[i];
    }
    out[o] = 0;
}

/* displayed text of a cell (evaluates formulas). right = numeric, for align. */
static void cell_disp(int r, int c, char *out, int cap, int *right) {
    *right = 0;
    const char *t = cells[r][c];
    if (t[0] == 0) { out[0] = 0; return; }
    if (t[0] == '=') {
        eval_err = 0;
        const char *p = t + 1;
        int64_t v = parse_expr(&p, 0);
        if (eval_err) { strcpy(out, "#ERR"); return; }
        fmt_scaled(v, out, cap);
        *right = 1;
        return;
    }
    int i = 0;
    for (; t[i] && i < cap - 1; i++) out[i] = t[i];
    out[i] = 0;
    if (looks_numeric(t)) *right = 1;
}

/* ===========================================================================
 *  Column labels
 * ===========================================================================*/
static void col_label(int c, char *out) {
    /* single-letter A..Z covers all 26 columns */
    out[0] = (char)('A' + c);
    out[1] = 0;
}

/* ===========================================================================
 *  Editing
 * ===========================================================================*/
static void edit_begin(int preserve) {
    editing = 1;
    if (preserve) {
        int i = 0;
        const char *t = cells[sel_r][sel_c];
        for (; t[i] && i < CELLTXT - 1; i++) ebuf[i] = t[i];
        ebuf[i] = 0; elen = i; ecur = i;
    } else {
        ebuf[0] = 0; elen = 0; ecur = 0;
    }
}
static void edit_commit(void) {
    if (!editing) return;
    int i = 0;
    for (; i < elen && i < CELLTXT - 1; i++) cells[sel_r][sel_c][i] = ebuf[i];
    cells[sel_r][sel_c][i] = 0;
    editing = 0; s_dirty = 1;
}
static void edit_insert(char c) {
    if (elen >= CELLTXT - 1) return;
    for (int i = elen; i > ecur; i--) ebuf[i] = ebuf[i - 1];
    ebuf[ecur] = c; elen++; ecur++; ebuf[elen] = 0;
}
static void edit_backspace(void) {
    if (ecur <= 0) return;
    for (int i = ecur - 1; i < elen - 1; i++) ebuf[i] = ebuf[i + 1];
    elen--; ecur--; ebuf[elen] = 0;
}
static void edit_del(void) {
    if (ecur >= elen) return;
    for (int i = ecur; i < elen - 1; i++) ebuf[i] = ebuf[i + 1];
    elen--; ebuf[elen] = 0;
}

static void move_sel(int dr, int dc) {
    sel_r += dr; sel_c += dc;
    if (sel_r < 0) sel_r = 0; if (sel_r >= ROWS) sel_r = ROWS - 1;
    if (sel_c < 0) sel_c = 0; if (sel_c >= COLS) sel_c = COLS - 1;
}

/* ===========================================================================
 *  File I/O  (CSV)
 * ===========================================================================*/
static char iobuf[ROWS * COLS * CELLTXT];

static int field_needs_quote(const char *s) {
    for (; *s; s++) if (*s == ',' || *s == '"' || *s == '\n') return 1;
    return 0;
}

static void s_save(void) {
    /* find the used bounds so the file stays small */
    int maxr = -1, maxc = -1;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (cells[r][c][0]) { if (r > maxr) maxr = r; if (c > maxc) maxc = c; }

    int o = 0;
    for (int r = 0; r <= maxr; r++) {
        for (int c = 0; c <= maxc; c++) {
            const char *t = cells[r][c];
            if (field_needs_quote(t)) {
                if (o < (int)sizeof(iobuf) - 1) iobuf[o++] = '"';
                for (const char *s = t; *s && o < (int)sizeof(iobuf) - 2; s++) {
                    if (*s == '"') iobuf[o++] = '"';     /* escape by doubling */
                    iobuf[o++] = *s;
                }
                if (o < (int)sizeof(iobuf) - 1) iobuf[o++] = '"';
            } else {
                for (const char *s = t; *s && o < (int)sizeof(iobuf) - 1; s++) iobuf[o++] = *s;
            }
            if (c < maxc && o < (int)sizeof(iobuf) - 1) iobuf[o++] = ',';
        }
        if (o < (int)sizeof(iobuf) - 1) iobuf[o++] = '\n';
    }

    fs_node *n = fs_lookup(sp_path);
    if (!n) n = fs_create(sp_path, 0);
    if (n && fs_write(n, iobuf, (uint32_t)o) == 0) {
        s_dirty = 0;
        s_saved_flash = (int)pit_ticks() + 1500;
    }
}

static void s_load(void) {
    memset(cells, 0, sizeof(cells));
    fs_node *n = fs_lookup(sp_path);
    if (!n || n->is_dir || !n->data) return;
    const char *d = (const char *)n->data;
    int len = (int)n->size;
    int r = 0, c = 0, fl = 0;
    char field[CELLTXT];
    int inq = 0;
    for (int i = 0; i < len; i++) {
        char ch = d[i];
        if (inq) {
            if (ch == '"') {
                if (i + 1 < len && d[i + 1] == '"') { if (fl < CELLTXT - 1) field[fl++] = '"'; i++; }
                else inq = 0;
            } else if (fl < CELLTXT - 1) field[fl++] = ch;
            continue;
        }
        if (ch == '"') { inq = 1; continue; }
        if (ch == ',') {
            field[fl] = 0;
            if (r < ROWS && c < COLS) memcpy(cells[r][c], field, fl + 1);
            c++; fl = 0; continue;
        }
        if (ch == '\n' || ch == '\r') {
            if (ch == '\r' && i + 1 < len && d[i + 1] == '\n') i++;
            field[fl] = 0;
            if (r < ROWS && c < COLS) memcpy(cells[r][c], field, fl + 1);
            r++; c = 0; fl = 0; continue;
        }
        if (fl < CELLTXT - 1) field[fl++] = ch;
    }
    if (fl > 0 && r < ROWS && c < COLS) { field[fl] = 0; memcpy(cells[r][c], field, fl + 1); }
}

static void s_new(void) {
    memset(cells, 0, sizeof(cells));
    sel_r = sel_c = top_row = left_col = 0;
    editing = 0; s_dirty = 1;
}

/* Save As: bare names land in /home/Documents (with a .csv default); an
 * absolute path is used verbatim. */
static void s_saveas_cb(const char *name) {
    if (!name || !name[0]) return;
    if (name[0] == '/') {
        strncpy(sp_path, name, sizeof(sp_path) - 1);
    } else {
        sp_path[0] = 0;
        kstrlcat(sp_path, "/home/Documents/", sizeof(sp_path));
        kstrlcat(sp_path, name, sizeof(sp_path));
    }
    sp_path[sizeof(sp_path) - 1] = 0;
    s_save();
}

/* ===========================================================================
 *  Layout helpers (client-local geometry, recomputed by draw)
 * ===========================================================================*/
static int gl_cw, gl_ch;          /* last client size                          */

static int grid_y0(void) { return TOOLH + FBH + HH; }   /* first data row top   */
static int grid_x0(void) { return RHW; }                /* first data col left  */
static int vis_rows(void) { int v = (gl_ch - grid_y0()) / RH; return v < 1 ? 1 : v; }
static int vis_cols(void) { int v = (gl_cw - grid_x0()) / CW; return v < 1 ? 1 : v; }

static void ensure_visible(void) {
    if (sel_r < top_row) top_row = sel_r;
    if (sel_r >= top_row + vis_rows()) top_row = sel_r - vis_rows() + 1;
    if (sel_c < left_col) left_col = sel_c;
    if (sel_c >= left_col + vis_cols()) left_col = sel_c - vis_cols() + 1;
    if (top_row < 0) top_row = 0;
    if (left_col < 0) left_col = 0;
}

/* ===========================================================================
 *  Input
 * ===========================================================================*/
static void sheets_key(window_t *w, char c) {
    (void)w;
    if (editing) {
        switch ((unsigned char)c) {
        case '\n':           edit_commit(); move_sel(1, 0);  break;
        case '\t':           edit_commit(); move_sel(0, 1);  break;
        case 27:             editing = 0;                    break;  /* Esc: cancel */
        case '\b':           edit_backspace();               break;
        case KEY_DEL:        edit_del();                     break;
        case KEY_LEFT:       if (ecur > 0) ecur--;           break;
        case KEY_RIGHT:      if (ecur < elen) ecur++;        break;
        case KEY_HOME:       ecur = 0;                       break;
        case KEY_END:        ecur = elen;                    break;
        case KEY_UP:         edit_commit(); move_sel(-1, 0); break;
        case KEY_DOWN:       edit_commit(); move_sel(1, 0);  break;
        default:
            if ((unsigned char)c >= 32 && (unsigned char)c < 127) edit_insert(c);
            break;
        }
        ensure_visible();
        return;
    }

    switch ((unsigned char)c) {
    case KEY_LEFT:  move_sel(0, -1); break;
    case KEY_RIGHT: move_sel(0,  1); break;
    case KEY_UP:    move_sel(-1, 0); break;
    case KEY_DOWN:  move_sel(1,  0); break;
    case '\t':      move_sel(0,  1); break;
    case '\n':      move_sel(1,  0); break;
    case KEY_HOME:  sel_c = 0;        break;
    case KEY_END:   sel_c = COLS - 1; break;
    case KEY_PGUP:  sel_r -= vis_rows(); if (sel_r < 0) sel_r = 0; break;
    case KEY_PGDN:  sel_r += vis_rows(); if (sel_r >= ROWS) sel_r = ROWS - 1; break;
    case KEY_SAVE:  s_save(); break;
    case '\b': case KEY_DEL:
        cells[sel_r][sel_c][0] = 0; s_dirty = 1; break;
    default:
        if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
            edit_begin(0);
            edit_insert(c);
        }
        break;
    }
    ensure_visible();
}

static void sheets_click(window_t *w, int lx, int ly) {
    (void)w;
    /* toolbar buttons */
    for (int i = 0; i < nshots; i++) {
        shot_t *h = &shots[i];
        if (lx >= h->x && lx < h->x + h->w && ly >= h->y && ly < h->y + h->h) {
            if (editing) edit_commit();
            if (h->id == B_SAVE) s_save();
            else if (h->id == B_NEW) s_new();
            else if (h->id == B_SAVEAS) gui_prompt("Save As (filename.csv)", "", s_saveas_cb);
            return;
        }
    }
    /* formula bar: edit active cell preserving its content */
    if (ly >= TOOLH && ly < TOOLH + FBH && lx >= RHW) {
        edit_begin(1);
        return;
    }
    /* grid cells */
    int gx = grid_x0(), gy = grid_y0();
    if (lx >= gx && ly >= gy) {
        int c = left_col + (lx - gx) / CW;
        int r = top_row  + (ly - gy) / RH;
        if (r >= 0 && r < ROWS && c >= 0 && c < COLS) {
            if (editing) edit_commit();
            sel_r = r; sel_c = c;
            ensure_visible();
        }
    }
}

static void sheets_scroll(window_t *w, int delta) {
    (void)w;
    top_row -= delta * 3;
    if (top_row < 0) top_row = 0;
    if (top_row > ROWS - 1) top_row = ROWS - 1;
}

/* ===========================================================================
 *  Draw
 * ===========================================================================*/
static int tool_btn(int x, int y, const char *label, int id, uint32_t bg, int ox, int oy) {
    int w = g_text_width(label, 1) + 24, h = 22;
    g_round(x, y, w, h, 6, bg, 255);
    g_text(x + 12, y + 4, label, 0xFFFFFF, 1);
    if (nshots < 6) { shots[nshots].x = x - ox; shots[nshots].y = y - oy;
                      shots[nshots].w = w; shots[nshots].h = h; shots[nshots].id = id; nshots++; }
    return w;
}

static void sheets_draw(window_t *w, int cx, int cy, int cw, int ch) {
    nshots = 0;
    gl_cw = cw; gl_ch = ch;
    ensure_visible();

    /* toolbar */
    g_fill(cx, cy, cw, TOOLH, COL_PANEL_2);
    g_hline(cx, cy + TOOLH, cw, COL_PANEL_3);
    int bx = cx + 10, by = cy + 6;
    bx += tool_btn(bx, by, "Save", B_SAVE, COL_ACCENT,  cx, cy) + 8;
    bx += tool_btn(bx, by, "Save As", B_SAVEAS, COL_PANEL_3, cx, cy) + 8;
    bx += tool_btn(bx, by, "New",  B_NEW,  COL_PANEL_3, cx, cy) + 8;
    if (s_saved_flash && (int)pit_ticks() < s_saved_flash)
        g_text(bx + 6, cy + 11, "Saved", COL_GOOD, 1);
    else if (s_dirty)
        g_text(bx + 6, cy + 11, "Unsaved", COL_WARN, 1);

    /* formula bar: cell ref tag + active cell content (or live edit buffer) */
    int fby = cy + TOOLH;
    g_fill(cx, fby, cw, FBH, COL_PANEL);
    g_hline(cx, fby + FBH, cw, COL_PANEL_3);
    char tag[8]; col_label(sel_c, tag);
    char tagn[12]; int tl = 0;
    tagn[tl++] = tag[0];
    int rn = sel_r + 1; char digs[6]; int dn = 0;
    while (rn > 0) { digs[dn++] = '0' + rn % 10; rn /= 10; }
    while (dn > 0) tagn[tl++] = digs[--dn];
    tagn[tl] = 0;
    g_fill(cx, fby, RHW, FBH, COL_PANEL_2);
    g_text(cx + 6, fby + 6, tagn, COL_ACCENT, 1);
    g_vline(cx + RHW, fby, FBH, COL_PANEL_3);
    const char *fbtext = editing ? ebuf : cells[sel_r][sel_c];
    g_text(cx + RHW + 8, fby + 6, fbtext, COL_TEXT, 1);
    if (editing && (pit_ticks() / 400) % 2 == 0) {
        int caret_px = cx + RHW + 8 + g_text_width_pn(ebuf, ecur, 1);
        g_fill(caret_px, fby + 4, 1, FBH - 8, COL_ACCENT);
    }

    /* grid background */
    int gx = cx + grid_x0(), gy = cy + grid_y0();
    g_fill(cx, gy - HH, cw, ch - (grid_y0() - HH), 0x14141C);

    int vr = vis_rows(), vc = vis_cols();

    /* column headers */
    g_fill(cx, cy + TOOLH + FBH, cw, HH, COL_PANEL_2);
    for (int j = 0; j < vc && left_col + j < COLS; j++) {
        int c = left_col + j;
        int x = gx + j * CW;
        char lbl[8]; col_label(c, lbl);
        uint32_t hc = (c == sel_c) ? COL_ACCENT : COL_TEXT_DIM;
        g_text(x + CW / 2 - 4, cy + TOOLH + FBH + 5, lbl, hc, 1);
        g_vline(x, cy + TOOLH + FBH, HH, COL_PANEL_3);
    }
    /* corner box */
    g_fill(cx, cy + TOOLH + FBH, RHW, HH, COL_PANEL_2);

    /* rows */
    for (int i = 0; i < vr && top_row + i < ROWS; i++) {
        int r = top_row + i;
        int y = gy + i * RH;
        /* row number gutter */
        g_fill(cx, y, RHW, RH, COL_PANEL_2);
        char num[6]; int nn = 0, v = r + 1;
        char rev[6]; int rl = 0;
        while (v > 0) { rev[rl++] = '0' + v % 10; v /= 10; }
        while (rl > 0) num[nn++] = rev[--rl];
        num[nn] = 0;
        uint32_t rc = (r == sel_r) ? COL_ACCENT : COL_TEXT_DIM;
        g_text(cx + 6, y + 4, num, rc, 1);

        for (int j = 0; j < vc && left_col + j < COLS; j++) {
            int c = left_col + j;
            int x = gx + j * CW;
            int is_sel = (r == sel_r && c == sel_c);
            if (is_sel) g_fill(x, y, CW, RH, 0x223046);
            char disp[40]; int right;
            if (editing && is_sel) { /* show live buffer while editing in-cell */
                int k = 0; for (; ebuf[k] && k < 39; k++) disp[k] = ebuf[k]; disp[k] = 0; right = 0;
            } else {
                cell_disp(r, c, disp, sizeof(disp), &right);
            }
            if (disp[0]) {
                int tw = g_text_width(disp, 1);
                int tx = right ? (x + CW - 6 - tw) : (x + 5);
                if (tx < x + 3) tx = x + 3;
                g_text(tx, y + 4, disp, COL_TEXT, 1);
            }
            g_vline(x, y, RH, 0x20202C);
        }
        g_hline(gx, y + RH, vc * CW, 0x20202C);
    }
    /* selection outline */
    if (sel_r >= top_row && sel_r < top_row + vr && sel_c >= left_col && sel_c < left_col + vc) {
        int sx = gx + (sel_c - left_col) * CW;
        int sy = gy + (sel_r - top_row) * RH;
        g_hline(sx, sy, CW, COL_ACCENT);
        g_hline(sx, sy + RH, CW, COL_ACCENT);
        g_vline(sx, sy, RH, COL_ACCENT);
        g_vline(sx + CW, sy, RH, COL_ACCENT);
    }
    g_vline(gx, gy - HH, ch, COL_PANEL_3);
}

void sheets_app_init(void) {
    memset(cells, 0, sizeof(cells));
    sel_r = sel_c = top_row = left_col = 0;
    editing = 0; s_dirty = 0;
    s_load();
    window_t *win = gui_add_window("Sheets", 640, 460, 0x33C481, ICON_SHEETS);
    if (!win) return;
    win->draw   = sheets_draw;
    win->key    = sheets_key;
    win->click  = sheets_click;
    win->scroll = sheets_scroll;
    win->min_w  = 360; win->min_h = 260;
    win->x = 200; win->y = 80;
    sp_win = win;
}

/* Open an arbitrary CSV file in Sheets (called by the file manager). */
void sheets_open_path(const char *path) {
    if (!path || !path[0]) return;
    strncpy(sp_path, path, sizeof(sp_path) - 1);
    sp_path[sizeof(sp_path) - 1] = 0;
    sel_r = sel_c = top_row = left_col = 0;
    editing = 0; s_dirty = 0;
    s_load();
    if (sp_win) gui_open(sp_win);
}
