/* ===========================================================================
 *  BoltOS  -  kernel/app_ide.c
 *  Code: a multi-language IDE that replaces the old Python REPL window. It has
 *  a real text editor (caret, arrow/Home/End/Del navigation, click-to-place,
 *  vertical scroll, syntax highlighting and line numbers), a language switcher
 *  (C / C++ / C# / Python) and a Run button. Running compiles the buffer with
 *  BoltCC (C/C++/C#) or BoltPython (Python) and shows the program's output in a
 *  console pane underneath, captured through console_set_sink() just like the
 *  Terminal app. Each language keeps its own editor buffer and caret.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "console.h"
#include "boltcc.h"
#include "boltpy.h"
#include "keyboard.h"
#include "pit.h"
#include "string.h"
#include "kprintf.h"
#include "fs.h"
#include "vfs.h"

#define IDE_DIR "/home/Documents"

#define BUFCAP 16384
#define CELLW  8
#define CELLH  16
#define PAD    8
#define TOOLH  38
#define GUTTER 38            /* line-number gutter width                         */
#define OCOLS  256
#define OROWS  400

enum { LANG_C = 0, LANG_CPP, LANG_CS, LANG_PY, NLANG };

typedef struct {
    char buf[BUFCAP];
    int  len, cur, top;      /* caret index, first visible editor line          */
} editor_t;

enum { PR_NONE = 0, PR_SAVE, PR_OPEN };

typedef struct {
    editor_t ed[NLANG];
    int      lang;
    /* output console grid */
    char     og[OROWS][OCOLS];
    int      ox, oy;         /* output write cursor                             */
    int      otop;           /* first visible output row                        */
    int      running;
    /* file dialogs */
    int      prompt;         /* PR_NONE / PR_SAVE / PR_OPEN                      */
    char     fname[80];      /* save-as filename being typed                    */
    int      fnlen;
    int      osel;           /* highlighted entry in the open list              */
    char     status[96];     /* one-line status under the toolbar               */
} ide_t;

static ide_t S;
static ide_t *out_active;    /* grid currently capturing kputc                  */

static const char *lang_label(int l) {
    return l == LANG_C ? "C" : l == LANG_CPP ? "C++" : l == LANG_CS ? "C#" : "Python";
}

/* --------------------------------------------------------- editor buffer --- */
static editor_t *ED(void) { return &S.ed[S.lang]; }

static void ed_insert(char c) {
    editor_t *e = ED();
    if (e->len >= BUFCAP - 1) return;
    for (int i = e->len; i > e->cur; i--) e->buf[i] = e->buf[i - 1];
    e->buf[e->cur++] = c; e->len++; e->buf[e->len] = 0;
}
static void ed_backspace(void) {
    editor_t *e = ED();
    if (e->cur <= 0) return;
    for (int i = e->cur - 1; i < e->len - 1; i++) e->buf[i] = e->buf[i + 1];
    e->len--; e->cur--; e->buf[e->len] = 0;
}
static void ed_delete(void) {
    editor_t *e = ED();
    if (e->cur >= e->len) return;
    for (int i = e->cur; i < e->len - 1; i++) e->buf[i] = e->buf[i + 1];
    e->len--; e->buf[e->len] = 0;
}
static int caret_col(void) {
    editor_t *e = ED(); int col = 0;
    for (int i = e->cur - 1; i >= 0 && e->buf[i] != '\n'; i--) col++;
    return col;
}
static int line_start(int pos) { editor_t *e = ED(); int i = pos; while (i > 0 && e->buf[i-1] != '\n') i--; return i; }
static int line_end(int pos)   { editor_t *e = ED(); int i = pos; while (i < e->len && e->buf[i] != '\n') i++; return i; }
static void ed_up(void) {
    editor_t *e = ED(); int col = caret_col(); int ls = line_start(e->cur);
    if (ls == 0) { e->cur = 0; return; }
    int prev = line_start(ls - 1); int plen = (ls - 1) - prev;
    e->cur = prev + (col < plen ? col : plen);
}
static void ed_down(void) {
    editor_t *e = ED(); int col = caret_col(); int le = line_end(e->cur);
    if (le >= e->len) { e->cur = e->len; return; }
    int next = le + 1; int nlen = line_end(next) - next;
    e->cur = next + (col < nlen ? col : nlen);
}

/* --------------------------------------------------------- output console -- */
static void out_clear(void) {
    memset(S.og, 0, sizeof(S.og));
    S.ox = S.oy = S.otop = 0;
}
static void out_scroll(void) {
    for (int r = 1; r < OROWS; r++) memcpy(S.og[r-1], S.og[r], OCOLS);
    memset(S.og[OROWS-1], 0, OCOLS);
    S.oy = OROWS - 1;
}
static void out_putc(char c) {
    if (c == '\r') { S.ox = 0; return; }
    if (c == '\n') { S.ox = 0; if (++S.oy >= OROWS) out_scroll(); return; }
    if (c == '\t') { int n = 4 - (S.ox & 3); while (n--) out_putc(' '); return; }
    if (c == '\b') { if (S.ox > 0) { S.ox--; S.og[S.oy][S.ox] = 0; } return; }
    if ((unsigned char)c < 32) return;
    if (S.ox < OCOLS - 1) S.og[S.oy][S.ox++] = c;
    else { S.ox = 0; if (++S.oy >= OROWS) out_scroll(); S.og[S.oy][S.ox++] = c; }
}
static void out_puts(const char *s) { while (*s) out_putc(*s++); }
static void out_sink(char c) { if (out_active) out_putc(c); }

/* --------------------------------------------------------- run the code ---- */
static void ide_run(void) {
    editor_t *e = ED();
    e->buf[e->len] = 0;
    out_clear();
    out_puts("[ "); out_puts(lang_label(S.lang)); out_puts(" ]  building + running...\n\n");

    S.running = 1;
    out_active = &S;
    console_set_sink(out_sink);
    int rc;
    if (S.lang == LANG_PY) rc = bpy_run(e->buf);
    else                   rc = boltcc_run(S.lang == LANG_CPP ? BCC_CPP :
                                           S.lang == LANG_CS  ? BCC_CSHARP : BCC_C, e->buf);
    console_set_sink(0);
    out_active = 0;
    S.running = 0;

    out_puts(rc ? "\n\n[ program exited with errors ]\n" : "\n\n[ program finished ]\n");
    /* keep the tail visible */
    S.otop = S.oy;
}

/* ----------------------------------------------------------- file dialogs - */
static const char *lang_ext(int l) {
    return l == LANG_CPP ? ".cpp" : l == LANG_CS ? ".cs" : l == LANG_PY ? ".py" : ".c";
}
/* pick the editor language that matches a filename's extension */
static int lang_for_name(const char *name) {
    int n = (int)strlen(name);
    if (n >= 4 && strcmp(name + n - 4, ".cpp") == 0) return LANG_CPP;
    if (n >= 3 && strcmp(name + n - 3, ".cs")  == 0) return LANG_CS;
    if (n >= 3 && strcmp(name + n - 3, ".py")  == 0) return LANG_PY;
    return LANG_C;
}
static void set_status(const char *s) {
    int i = 0; for (; s[i] && i < (int)sizeof(S.status) - 1; i++) S.status[i] = s[i];
    S.status[i] = 0;
}

static void do_save(void) {
    if (S.fnlen == 0) { set_status("Save cancelled: empty name"); S.prompt = PR_NONE; return; }
    if (!fs_lookup(IDE_DIR)) fs_create(IDE_DIR, 1);
    char path[160];
    int p = 0;
    for (const char *d = IDE_DIR; *d; d++) path[p++] = *d;
    path[p++] = '/';
    for (int i = 0; i < S.fnlen && p < (int)sizeof(path) - 1; i++) path[p++] = S.fname[i];
    path[p] = 0;

    file *f = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) { set_status("Save failed: cannot open file"); S.prompt = PR_NONE; return; }
    editor_t *e = ED();
    vfs_write(f, e->buf, (uint64_t)e->len);
    vfs_close(f);
    fs_sync();

    char msg[120]; int m = 0;
    const char *pre = "Saved ";
    for (const char *q = pre; *q; q++) msg[m++] = *q;
    for (int i = 0; i < S.fnlen && m < (int)sizeof(msg) - 1; i++) msg[m++] = S.fname[i];
    msg[m] = 0;
    set_status(msg);
    S.prompt = PR_NONE;
}

static void do_open(fs_node *n) {
    if (!n || n->is_dir) return;
    int l = lang_for_name(n->name);
    S.lang = l;
    editor_t *e = ED();
    int len = (int)n->size; if (len > BUFCAP - 1) len = BUFCAP - 1;
    if (n->data && len > 0) memcpy(e->buf, n->data, len);
    e->len = len; e->buf[len] = 0; e->cur = 0; e->top = 0;
    /* remember the name for a quick re-save */
    int i = 0; for (; n->name[i] && i < (int)sizeof(S.fname) - 1; i++) S.fname[i] = n->name[i];
    S.fname[i] = 0; S.fnlen = i;

    char msg[120]; int m = 0;
    const char *pre = "Opened ";
    for (const char *q = pre; *q; q++) msg[m++] = *q;
    for (int j = 0; n->name[j] && m < (int)sizeof(msg) - 1; j++) msg[m++] = n->name[j];
    msg[m] = 0;
    set_status(msg);
    S.prompt = PR_NONE;
}

/* prefill the save-as box with the remembered name, or a language default */
static void begin_save(void) {
    if (S.fnlen == 0) {
        const char *base = "prog";
        const char *ext  = lang_ext(S.lang);
        int p = 0;
        for (const char *q = base; *q; q++) S.fname[p++] = *q;
        for (const char *q = ext;  *q; q++) S.fname[p++] = *q;
        S.fname[p] = 0; S.fnlen = p;
    }
    S.prompt = PR_SAVE;
}

/* ------------------------------------------------------------- highlight --- */
/* colour ids: 0 normal, 1 keyword, 2 string, 3 comment, 4 number, 5 builtin */
static uint32_t hl_color(int id) {
    switch (id) {
    case 1: return 0x569CD6;      /* keyword  - blue   */
    case 2: return 0xCE9178;      /* string   - salmon */
    case 3: return 0x6A9955;      /* comment  - green  */
    case 4: return 0xB5CEA8;      /* number   - mint   */
    case 5: return 0xDCDCAA;      /* builtin  - yellow */
    default: return 0xD4D4D4;     /* text             */
    }
}
static int is_kw(const char *s, int n) {
    static const char *kw[] = {
        "int","long","short","char","unsigned","signed","void","bool","float","double",
        "string","var","auto","const","static","struct","class","return","if","else",
        "while","for","do","break","continue","new","delete","public","private","protected",
        "using","namespace","true","false","null","def","print","import","from","in","range",
        "switch","case","default","sizeof","this","null","None","True","False","elif","not",
        "and","or","lambda","try","except","foreach","var", 0 };
    for (int i = 0; kw[i]; i++) {
        const char *k = kw[i]; int j = 0;
        while (j < n && k[j] && k[j] == s[j]) j++;
        if (j == n && k[j] == 0) return 1;
    }
    return 0;
}
static int is_builtin(const char *s, int n) {
    static const char *bi[] = { "printf","puts","putchar","cout","cin","endl","len","abs",
        "min","max","chr","ord","Console","WriteLine","Write","Math","main","Main", 0 };
    for (int i = 0; bi[i]; i++) {
        const char *k = bi[i]; int j = 0;
        while (j < n && k[j] && k[j] == s[j]) j++;
        if (j == n && k[j] == 0) return 1;
    }
    return 0;
}
static int isal(char c)  { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int isaln(char c) { return isal(c)||(c>='0'&&c<='9'); }
static int isdg(char c)  { return c>='0'&&c<='9'; }

/* fill colr[] for the whole buffer in one pass */
static void compute_colors(editor_t *e, uint8_t *colr) {
    const char *b = e->buf; int n = e->len; int i = 0;
    int hashcmt = (S.lang == LANG_PY);      /* python uses # line comments */
    while (i < n) {
        char c = b[i];
        if (c == '/' && i+1 < n && b[i+1] == '/') { while (i < n && b[i] != '\n') colr[i++] = 3; continue; }
        if (hashcmt && c == '#') { while (i < n && b[i] != '\n') colr[i++] = 3; continue; }
        if (c == '/' && i+1 < n && b[i+1] == '*') { colr[i++]=3; colr[i++]=3; while (i < n && !(b[i]=='*'&&i+1<n&&b[i+1]=='/')) colr[i++]=3; if (i<n){colr[i++]=3;} if(i<n)colr[i++]=3; continue; }
        if (c == '"' || c == '\'') { char q=c; colr[i++]=2; while (i<n && b[i]!=q) { if (b[i]=='\\'&&i+1<n) colr[i++]=2; if(i<n)colr[i++]=2; } if (i<n) colr[i++]=2; continue; }
        if (isdg(c)) { while (i<n && (isaln(b[i])||b[i]=='.')) colr[i++]=4; continue; }
        if (isal(c)) { int s=i; while (i<n && isaln(b[i])) i++; int w=i-s;
                       int col = is_kw(b+s,w) ? 1 : is_builtin(b+s,w) ? 5 : 0;
                       for (int k=s;k<i;k++) colr[k]=col; continue; }
        colr[i++] = 0;
    }
}

/* ------------------------------------------------------------- toolbar ----- */
typedef struct { int x, y, w, h, id; } hot_t;
static hot_t hots[12]; static int nhot;
static int ide_ox, ide_oy;       /* client origin for client-local hot rects   */
enum { H_TABBASE = 100, H_RUN = 1, H_NEW = 2, H_OPEN = 3, H_SAVE = 4 };

/* open-dialog file list hit rects */
static struct { int x, y, w, h; fs_node *n; } olist[64];
static int nolist;

static int btn(int x, int y, const char *label, int id, uint32_t bg, uint32_t fg) {
    int w = g_text_width(label, 1) + 22, h = 24;
    g_round(x, y, w, h, 6, bg, 255);
    g_text(x + 11, y + 5, label, fg, 1);
    if (nhot < 12) { hots[nhot].x = x - ide_ox; hots[nhot].y = y - ide_oy;
                     hots[nhot].w = w; hots[nhot].h = h; hots[nhot].id = id; nhot++; }
    return w;
}

/* editor text-area geometry (filled each draw, used by click handler) */
static int area_tx, area_ty, area_rows, area_cols;

static void ide_draw(window_t *w, int cx, int cy, int cw, int ch) {
    nhot = 0; nolist = 0; ide_ox = cx; ide_oy = cy;

    /* toolbar */
    g_fill(cx, cy, cw, TOOLH, COL_PANEL_2);
    g_hline(cx, cy + TOOLH, cw, COL_PANEL_3);
    int bx = cx + 8, by = cy + 7;
    for (int l = 0; l < NLANG; l++) {
        int active = (l == S.lang);
        bx += btn(bx, by, lang_label(l), H_TABBASE + l,
                  active ? COL_ACCENT : COL_PANEL_3,
                  active ? 0xFFFFFF : COL_TEXT_DIM) + 6;
    }
    /* Run, then Open/Save/New on the right */
    int rx = cx + cw - 8;
    int rw = g_text_width("Run", 1) + 22;
    rx -= rw; btn(rx, by, "Run", H_RUN, 0x2EA043, 0xFFFFFF);
    int nw = g_text_width("New", 1) + 22;
    rx -= nw + 6; btn(rx, by, "New", H_NEW, COL_PANEL_3, COL_TEXT);
    int sw = g_text_width("Save", 1) + 22;
    rx -= sw + 6; btn(rx, by, "Save", H_SAVE, COL_PANEL_3, COL_TEXT);
    int ow = g_text_width("Open", 1) + 22;
    rx -= ow + 6; btn(rx, by, "Open", H_OPEN, COL_PANEL_3, COL_TEXT);

    /* split: editor on top, output console at the bottom */
    int body_y = cy + TOOLH + 1;
    int body_h = ch - TOOLH - 1;
    int out_h  = body_h / 3; if (out_h < 90) out_h = 90; if (out_h > body_h - 80) out_h = body_h - 80;
    int ed_h   = body_h - out_h;

    /* ---- editor ---- */
    g_fill(cx, body_y, cw, ed_h, 0x1E1E1E);
    g_fill(cx, body_y, GUTTER, ed_h, 0x181818);
    editor_t *e = ED();

    int tx = cx + GUTTER + 6, ty = body_y + 6;
    int rows = (ed_h - 12) / CELLH; if (rows < 1) rows = 1;
    int cols = (cx + cw - tx) / CELLW; if (cols < 1) cols = 1;
    area_tx = tx - cx; area_ty = ty - cy; area_rows = rows; area_cols = cols;

    /* track caret line, keep on screen */
    int caret_line = 0;
    for (int i = 0; i < e->cur; i++) if (e->buf[i] == '\n') caret_line++;
    if (caret_line < e->top) e->top = caret_line;
    if (caret_line >= e->top + rows) e->top = caret_line - rows + 1;

    static uint8_t colr[BUFCAP];
    compute_colors(e, colr);

    int line = 0, col = 0, caret_px = tx, caret_py = ty;
    /* line numbers for visible lines */
    for (int r = 0; r < rows; r++) {
        int ln = e->top + r + 1;
        char num[8]; int v = ln, p = 0; char t[8]; int tn = 0;
        if (v == 0) t[tn++] = '0'; while (v) { t[tn++] = (char)('0'+v%10); v/=10; }
        while (tn) num[p++] = t[--tn]; num[p] = 0;
        g_text(cx + GUTTER - 6 - g_text_width(num,1), ty + r*CELLH, num, 0x5A5A5A, 1);
    }
    for (int i = 0; i <= e->len; i++) {
        if (i == e->cur) { caret_px = tx + col*CELLW; caret_py = ty + (line - e->top)*CELLH; }
        if (i == e->len) break;
        char c = e->buf[i];
        if (c == '\n') { line++; col = 0; continue; }
        if (c == '\t') { col = (col + 4) & ~3; continue; }
        if (line >= e->top && line < e->top + rows && col < cols) {
            g_char(tx + col*CELLW, ty + (line - e->top)*CELLH, c, hl_color(colr[i]), 1);
        }
        col++;
    }
    if (gui_window_focused(w) && (pit_ticks()/500)%2 == 0 &&
        caret_py >= ty - 2 && caret_py < body_y + ed_h)
        g_fill(caret_px, caret_py, 2, CELLH - 2, 0xFFFFFF);

    /* ---- output console ---- */
    int oy0 = body_y + ed_h;
    g_hline(cx, oy0, cw, COL_PANEL_3);
    g_fill(cx, oy0 + 1, cw, out_h - 1, 0x0C0C12);
    g_text(cx + 8, oy0 + 5, S.running ? "Output  (running)" : "Output", COL_TEXT_DIM, 1);
    int otx = cx + 8, oty = oy0 + 22;
    int orows = (out_h - 26) / CELLH; if (orows < 1) orows = 1;
    int ocols = (cw - 16) / CELLW;    if (ocols > OCOLS) ocols = OCOLS;
    int start = S.oy - orows + 1; if (start < 0) start = 0;
    for (int r = 0; r < orows; r++) {
        int row = start + r; if (row >= OROWS) break;
        for (int c = 0; c < ocols; c++) {
            char g = S.og[row][c];
            if (g) g_char(otx + c*CELLW, oty + r*CELLH, g, 0xCDCDCD, 1);
        }
    }

    /* ---- status line (right side of toolbar area, under the tabs) ---- */
    if (S.status[0] && S.prompt == PR_NONE)
        g_text(cx + 8, cy + TOOLH + 4, S.status, 0x6A9955, 1);

    /* ---- save-as input strip ---- */
    if (S.prompt == PR_SAVE) {
        int dw = 360, dh = 64;
        int dx = cx + (cw - dw) / 2, dy = cy + TOOLH + 8;
        g_round(dx, dy, dw, dh, 8, COL_PANEL_2, 255);
        g_text(dx + 12, dy + 8, "Save as  (Enter to save, Esc to cancel)", COL_TEXT_DIM, 1);
        int fx = dx + 12, fy = dy + 30, fw = dw - 24, fh = 22;
        g_round(fx, fy, fw, fh, 5, 0x1E1E1E, 255);
        S.fname[S.fnlen] = 0;
        g_text(fx + 6, fy + 4, S.fname, COL_TEXT, 1);
        if ((pit_ticks()/500)%2 == 0)
            g_fill(fx + 6 + g_text_width(S.fname, 1), fy + 3, 2, fh - 6, 0xFFFFFF);
    }

    /* ---- open file list ---- */
    if (S.prompt == PR_OPEN) {
        int dw = 320, dh = 280;
        int dx = cx + (cw - dw) / 2, dy = cy + TOOLH + 8;
        if (dy + dh > cy + ch) dh = cy + ch - dy - 8;
        g_round(dx, dy, dw, dh, 8, COL_PANEL_2, 255);
        g_text(dx + 12, dy + 8, "Open  (" IDE_DIR ")", COL_TEXT_DIM, 1);
        int ly = dy + 30, lh = 20;
        fs_node *dir = fs_lookup(IDE_DIR);
        int idx = 0;
        for (fs_node *c = dir ? dir->child : 0; c; c = c->next) {
            if (c->is_dir) continue;
            if (ly + lh > dy + dh - 6) break;
            if (idx == S.osel) g_fill(dx + 6, ly - 1, dw - 12, lh, COL_ACCENT);
            uint32_t fg = (idx == S.osel) ? 0xFFFFFF : COL_TEXT;
            gui_icon(ICON_FILE, dx + 10, ly, 1, fg);
            g_text(dx + 30, ly + 2, c->name, fg, 1);
            if (nolist < 64) { olist[nolist].x = dx - cx; olist[nolist].y = ly - cy;
                               olist[nolist].w = dw - 12; olist[nolist].h = lh;
                               olist[nolist].n = c; nolist++; }
            ly += lh; idx++;
        }
        if (idx == 0) g_text(dx + 12, dy + 34, "(no files yet)", COL_TEXT_DIM, 1);
    }
}

/* --------------------------------------------------------------- input ----- */
static void ide_key(window_t *w, char c) {
    (void)w;
    if (S.running) return;

    /* save-as input box has keyboard focus */
    if (S.prompt == PR_SAVE) {
        unsigned char u = (unsigned char)c;
        if (u == 27) { S.prompt = PR_NONE; }
        else if (c == '\n') { do_save(); }
        else if (c == '\b') { if (S.fnlen > 0) S.fname[--S.fnlen] = 0; }
        else if (u >= 32 && u < 127 && S.fnlen < (int)sizeof(S.fname) - 1) {
            S.fname[S.fnlen++] = c; S.fname[S.fnlen] = 0;
        }
        return;
    }
    /* open list navigation */
    if (S.prompt == PR_OPEN) {
        unsigned char u = (unsigned char)c;
        if (u == 27) { S.prompt = PR_NONE; }
        else if (u == KEY_UP)   { if (S.osel > 0) S.osel--; }
        else if (u == KEY_DOWN) { S.osel++; if (S.osel >= nolist && nolist > 0) S.osel = nolist - 1; }
        else if (c == '\n') {
            if (S.osel >= 0 && S.osel < nolist) do_open(olist[S.osel].n);
        }
        return;
    }

    switch ((unsigned char)c) {
    case KEY_LEFT:  { editor_t *e = ED(); if (e->cur > 0) e->cur--; break; }
    case KEY_RIGHT: { editor_t *e = ED(); if (e->cur < e->len) e->cur++; break; }
    case KEY_UP:    ed_up();   break;
    case KEY_DOWN:  ed_down(); break;
    case KEY_HOME:  { editor_t *e = ED(); e->cur = line_start(e->cur); break; }
    case KEY_END:   { editor_t *e = ED(); e->cur = line_end(e->cur);   break; }
    case KEY_DEL:   ed_delete(); break;
    case '\b':      ed_backspace(); break;
    default:
        if (c == '\n' || c == '\t' || (unsigned char)c >= 32) ed_insert(c);
        break;
    }
}

static void ide_click(window_t *w, int lx, int ly) {
    (void)w;
    /* clicks inside an open dialog: pick a file or dismiss */
    if (S.prompt == PR_OPEN) {
        for (int i = 0; i < nolist; i++) {
            if (lx >= olist[i].x && lx < olist[i].x + olist[i].w &&
                ly >= olist[i].y && ly < olist[i].y + olist[i].h) { do_open(olist[i].n); return; }
        }
        S.prompt = PR_NONE; return;
    }
    if (S.prompt == PR_SAVE) { return; }  /* type a name / Enter / Esc */

    for (int i = 0; i < nhot; i++) {
        hot_t *h = &hots[i];
        if (lx < h->x || lx >= h->x + h->w || ly < h->y || ly >= h->y + h->h) continue;
        if (h->id == H_RUN) ide_run();
        else if (h->id == H_NEW) { editor_t *e = ED(); e->len = 0; e->cur = 0; e->top = 0; e->buf[0] = 0;
                                   S.fname[0] = 0; S.fnlen = 0; set_status("New buffer"); }
        else if (h->id == H_SAVE) begin_save();
        else if (h->id == H_OPEN) { S.prompt = PR_OPEN; S.osel = 0; }
        else if (h->id >= H_TABBASE) S.lang = h->id - H_TABBASE;
        return;
    }
    /* click in the editor text area places the caret */
    if (lx >= area_tx && ly >= area_ty) {
        editor_t *e = ED();
        int row = (ly - area_ty) / CELLH; if (row < 0) row = 0;
        int colc = (lx - area_tx) / CELLW; if (colc < 0) colc = 0;
        int want = e->top + row;
        /* walk to the wanted line + column */
        int li = 0, idx = 0;
        while (idx < e->len && li < want) { if (e->buf[idx] == '\n') li++; idx++; }
        int c = 0;
        while (idx < e->len && e->buf[idx] != '\n' && c < colc) { idx++; c++; }
        e->cur = idx;
    }
}

static void ide_scroll(window_t *w, int delta) {
    (void)w; editor_t *e = ED();
    e->top -= delta * 3;
    if (e->top < 0) e->top = 0;
    int lines = 0; for (int i = 0; i < e->len; i++) if (e->buf[i] == '\n') lines++;
    if (e->top > lines) e->top = lines;
}

static void ide_tick(window_t *w) { (void)w; gui_request_redraw(); }

/* --------------------------------------------------------- sample programs - */
static const char *SAMPLE[NLANG] = {
/* C */
"#include <stdio.h>\n\n"
"int fib(int n) {\n"
"    if (n < 2) return n;\n"
"    return fib(n - 1) + fib(n - 2);\n"
"}\n\n"
"int main() {\n"
"    printf(\"BoltCC: a real C compiler\\n\");\n"
"    for (int i = 0; i < 10; i++)\n"
"        printf(\"fib(%d) = %d\\n\", i, fib(i));\n"
"    return 0;\n"
"}\n",
/* C++ */
"#include <iostream>\n"
"using namespace std;\n\n"
"int main() {\n"
"    cout << \"Hello from C++\" << endl;\n"
"    int sum = 0;\n"
"    for (int i = 1; i <= 100; i++) sum += i;\n"
"    cout << \"sum 1..100 = \" << sum << endl;\n"
"    return 0;\n"
"}\n",
/* C# */
"using System;\n\n"
"class Program {\n"
"    static int Factorial(int n) {\n"
"        int r = 1;\n"
"        for (int i = 2; i <= n; i++) r *= i;\n"
"        return r;\n"
"    }\n\n"
"    static void Main() {\n"
"        Console.WriteLine(\"Hello from C#\");\n"
"        for (int i = 1; i <= 6; i++)\n"
"            Console.WriteLine(\"{0}! = {1}\", i, Factorial(i));\n"
"    }\n"
"}\n",
/* Python */
"print(\"Hello from BoltPython\")\n\n"
"def fib(n):\n"
"    return n if n < 2 else fib(n - 1) + fib(n - 2)\n\n"
"for i in range(10):\n"
"    print(\"fib(\", i, \") =\", fib(i))\n",
};

void ide_app_init(void) {
    memset(&S, 0, sizeof(S));
    S.lang = LANG_C;
    for (int l = 0; l < NLANG; l++) {
        editor_t *e = &S.ed[l];
        int n = (int)strlen(SAMPLE[l]); if (n > BUFCAP - 1) n = BUFCAP - 1;
        memcpy(e->buf, SAMPLE[l], n); e->len = n; e->buf[n] = 0; e->cur = 0; e->top = 0;
    }
    out_clear();
    out_puts("BoltCC ready.  Pick a language, edit, and press Run.\n");

    window_t *win = gui_add_window("Code", 860, 600, 0x569CD6, ICON_CODE);
    if (!win) return;
    win->draw   = ide_draw;
    win->key    = ide_key;
    win->click  = ide_click;
    win->scroll = ide_scroll;
    win->tick   = ide_tick;
    win->min_w = 520; win->min_h = 360;
    win->x = 150; win->y = 60;
}
