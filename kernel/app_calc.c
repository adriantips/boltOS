/* ===========================================================================
 *  BoltOS  -  kernel/app_calc.c
 *  Calculator window: a modern button-grid calculator. The kernel is built
 *  with -mno-sse/-mno-387 (no hardware FP and no soft-float runtime linked),
 *  so all arithmetic is fixed-point: every value is an int64 scaled by 1000,
 *  giving 3 decimal places without ever touching a float.
 *
 *  Input comes from both the mouse (each button registers a client-local hot
 *  rect while drawing; the click handler hit-tests them) and the keyboard
 *  (digits, . + - * / , Enter/=, Backspace, Esc/c).
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "string.h"

#define SCALE 1000               /* fixed-point: 3 decimal places */

typedef struct {
    char    entry[24];           /* number currently being typed, e.g. "12.5"   */
    int64_t acc;                 /* stored operand (scaled)                      */
    char    op;                  /* pending operator: 0 '+' '-' '*' '/'          */
    int     fresh;               /* next digit starts a new entry                */
    int     error;              /* divide-by-zero etc. -> show "Error"           */
} calc_t;

static calc_t calc;

/* ---- fixed-point helpers ------------------------------------------------- */

/* parse the entry string ("[-]ddd[.ddd]") into a scaled int64 */
static int64_t parse_entry(const char *s) {
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    int64_t ip = 0;
    while (*s >= '0' && *s <= '9') { ip = ip * 10 + (*s - '0'); s++; }
    int64_t fp = 0, place = SCALE;
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9' && place > 1) {
            place /= 10;
            fp += (*s - '0') * place;
            s++;
        }
    }
    int64_t v = ip * SCALE + fp;
    return neg ? -v : v;
}

/* format a scaled int64 into "[-]ddd[.ddd]" with trailing zeros trimmed */
static void fmt_scaled(int64_t v, char *out, int cap) {
    char tmp[32]; int n = 0;
    int neg = v < 0;
    uint64_t u = (uint64_t)(neg ? -v : v);
    uint64_t ip = u / SCALE, fp = u % SCALE;

    /* fractional part: exactly 3 digits, then trim trailing zeros */
    char frac[4];
    frac[0] = '0' + (char)((fp / 100) % 10);
    frac[1] = '0' + (char)((fp / 10) % 10);
    frac[2] = '0' + (char)(fp % 10);
    frac[3] = 0;
    int fl = 3;
    while (fl > 0 && frac[fl - 1] == '0') frac[--fl] = 0;

    /* integer part (reversed into tmp) */
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

/* a op b, all scaled. returns 0 on ok, -1 on error (divide by zero) */
static int apply_op(int64_t a, char op, int64_t b, int64_t *res) {
    switch (op) {
    case '+': *res = a + b; return 0;
    case '-': *res = a - b; return 0;
    case '*': *res = (a * b) / SCALE; return 0;
    case '/':
        if (b == 0) return -1;
        *res = (a * SCALE) / b;
        return 0;
    default:  *res = b; return 0;
    }
}

/* current display text: the live entry, or "Error" */
static const char *display_text(void) {
    return calc.error ? "Error" : calc.entry;
}

/* ---- input actions ------------------------------------------------------- */

static void clear_all(void) {
    memset(&calc, 0, sizeof(calc));
    strcpy(calc.entry, "0");
    calc.fresh = 1;
}

static void input_digit(char d) {
    if (calc.error) clear_all();
    if (calc.fresh) { calc.entry[0] = 0; calc.fresh = 0; }
    int len = (int)strlen(calc.entry);
    if (len >= (int)sizeof(calc.entry) - 1) return;
    if (strcmp(calc.entry, "0") == 0) len = calc.entry[0] = 0;   /* drop leading 0 */
    calc.entry[len] = d; calc.entry[len + 1] = 0;
    if (calc.entry[0] == 0) strcpy(calc.entry, "0");
}

static void input_dot(void) {
    if (calc.error) clear_all();
    if (calc.fresh) { strcpy(calc.entry, "0"); calc.fresh = 0; }
    if (strchr(calc.entry, '.')) return;                 /* one dot only */
    kstrlcat(calc.entry, ".", sizeof(calc.entry));
}

static void input_op(char op) {
    if (calc.error) return;
    int64_t cur = parse_entry(calc.entry);
    if (calc.op && !calc.fresh) {
        int64_t r;
        if (apply_op(calc.acc, calc.op, cur, &r) < 0) { calc.error = 1; return; }
        calc.acc = r;
        fmt_scaled(calc.acc, calc.entry, sizeof(calc.entry));
    } else {
        calc.acc = cur;
    }
    calc.op = op;
    calc.fresh = 1;
}

static void input_equals(void) {
    if (calc.error || !calc.op) return;
    int64_t cur = parse_entry(calc.entry);
    int64_t r;
    if (apply_op(calc.acc, calc.op, cur, &r) < 0) { calc.error = 1; return; }
    fmt_scaled(r, calc.entry, sizeof(calc.entry));
    calc.acc = r;
    calc.op = 0;
    calc.fresh = 1;
}

static void input_neg(void) {
    if (calc.error) return;
    if (calc.entry[0] == '-') memmove(calc.entry, calc.entry + 1, strlen(calc.entry));
    else if (strcmp(calc.entry, "0") != 0) {
        char t[24]; t[0] = '-'; strncpy(t + 1, calc.entry, sizeof(t) - 1);
        strncpy(calc.entry, t, sizeof(calc.entry));
    }
}

static void input_percent(void) {
    if (calc.error) return;
    int64_t cur = parse_entry(calc.entry) / 100;
    fmt_scaled(cur, calc.entry, sizeof(calc.entry));
    calc.fresh = 1;
}

static void input_backspace(void) {
    if (calc.error) { clear_all(); return; }
    if (calc.fresh) return;
    int len = (int)strlen(calc.entry);
    if (len <= 1 || (len == 2 && calc.entry[0] == '-')) { strcpy(calc.entry, "0"); calc.fresh = 1; return; }
    calc.entry[len - 1] = 0;
}

/* dispatch a logical key (used by both mouse buttons and the keyboard) */
static void calc_input(char k) {
    if (k >= '0' && k <= '9') { input_digit(k); return; }
    switch (k) {
    case '.':            input_dot();       break;
    case '+': case '-':
    case '*': case '/':  input_op(k);        break;
    case '=':            input_equals();     break;
    case 'c': case 'C':  clear_all();        break;
    case 'n':            input_neg();        break;   /* +/- */
    case '%':            input_percent();    break;
    case 'b':            input_backspace();  break;
    default: break;
    }
}

/* ---- button grid --------------------------------------------------------- */
/* kind: 0 normal digit, 1 operator, 2 function, 3 equals */
typedef struct { const char *label; char key; uint8_t kind; uint8_t span; } btn_t;

static const btn_t BTNS[] = {
    {"C", 'c', 2, 1}, {"+/-", 'n', 2, 1}, {"%", '%', 2, 1}, {"/", '/', 1, 1},
    {"7", '7', 0, 1}, {"8",   '8', 0, 1}, {"9", '9', 0, 1}, {"x", '*', 1, 1},
    {"4", '4', 0, 1}, {"5",   '5', 0, 1}, {"6", '6', 0, 1}, {"-", '-', 1, 1},
    {"1", '1', 0, 1}, {"2",   '2', 0, 1}, {"3", '3', 0, 1}, {"+", '+', 1, 1},
    {"0", '0', 0, 2},                     {".", '.', 0, 1}, {"=", '=', 3, 1},
};
#define NBTN ((int)(sizeof(BTNS) / sizeof(BTNS[0])))

/* hot rects rebuilt every draw (client-local) */
typedef struct { int x, y, w, h; char key; } chot_t;
static chot_t chots[NBTN];
static int    nchot;

static uint32_t btn_color(uint8_t kind) {
    switch (kind) {
    case 1: return COL_ACCENT;          /* operators */
    case 2: return COL_PANEL_3;         /* functions */
    case 3: return COL_ACCENT;          /* equals    */
    default: return COL_PANEL_2;        /* digits    */
    }
}
static uint32_t btn_text(uint8_t kind) {
    return (kind == 1 || kind == 3) ? 0xFFFFFF : COL_TEXT;
}

static void calc_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    g_fill(cx, cy, cw, ch, COL_PANEL);
    nchot = 0;

    int pad = 14;
    int x0 = cx + pad, y0 = cy + pad, gw = cw - 2 * pad;

    /* ---- display ---- */
    int disp_h = 92;
    g_round(x0, y0, gw, disp_h, 12, 0x0E0E16, 255);
    const char *s = display_text();

    /* pending-operator hint, top-left of the display */
    if (calc.op && !calc.error) {
        char ohint[2] = { calc.op == '*' ? 'x' : calc.op, 0 };
        g_text(x0 + 16, y0 + 12, ohint, COL_ACCENT, 2);
    }

    /* right-align the number; shrink the scale if it would overflow */
    int scale = 4;
    while (scale > 1 && g_text_width(s, scale) > gw - 32) scale--;
    int tw = g_text_width(s, scale);
    int ty = y0 + disp_h - 16 - 8 * scale;
    g_text(x0 + gw - tw - 16, ty, s, calc.error ? COL_BAD : COL_TEXT, scale);

    /* ---- button grid ---- */
    int grid_y = y0 + disp_h + 12;
    int cols = 4, gap = 8;
    int avail_h = (cy + ch - pad) - grid_y;
    int rows = 5;
    int bw = (gw - (cols - 1) * gap) / cols;
    int bh = (avail_h - (rows - 1) * gap) / rows;

    int bx = x0, by = grid_y, col = 0;
    for (int i = 0; i < NBTN; i++) {
        const btn_t *b = &BTNS[i];
        int span = b->span;
        int w2 = bw * span + (span - 1) * gap;

        g_round(bx, by, w2, bh, 10, btn_color(b->kind), 255);
        int lw = g_text_width(b->label, 2);
        g_text(bx + (w2 - lw) / 2, by + (bh - 16) / 2, b->label, btn_text(b->kind), 2);

        if (nchot < NBTN) {
            chots[nchot].x = bx - cx; chots[nchot].y = by - cy;
            chots[nchot].w = w2; chots[nchot].h = bh; chots[nchot].key = b->key;
            nchot++;
        }

        col += span;
        bx += w2 + gap;
        if (col >= cols) { col = 0; bx = x0; by += bh + gap; }
    }
}

static void calc_click(window_t *w, int lx, int ly) {
    (void)w;
    for (int i = 0; i < nchot; i++) {
        chot_t *c = &chots[i];
        if (lx >= c->x && lx < c->x + c->w && ly >= c->y && ly < c->y + c->h) {
            calc_input(c->key);
            return;
        }
    }
}

static void calc_key(window_t *w, char c) {
    (void)w;
    if (c == '\n' || c == '\r') { calc_input('='); return; }
    if (c == '\b')              { calc_input('b'); return; }
    if (c == 27)                { calc_input('c'); return; }   /* Esc */
    calc_input(c);
}

void calc_app_init(void) {
    clear_all();
    window_t *win = gui_add_window("Calculator", 300, 440, 0x34C759, ICON_CALC);
    if (!win) return;
    win->draw  = calc_draw;
    win->click = calc_click;
    win->key   = calc_key;
    win->min_w = 240; win->min_h = 360;
    win->x = 360; win->y = 120;
}
