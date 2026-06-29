/* ===========================================================================
 *  BoltOS  -  kernel/app_contacts.c
 *  Contacts: a simple address book backed by the filesystem. Type
 *  "Name, phone or email" in the field and press Enter (or "Add") to create a
 *  contact; click a card to select it; Del (or "Delete") removes it. Each
 *  contact is one line in /contacts.txt as "Name\tinfo", and the FS autosaves,
 *  so the address book survives reboots.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "fs.h"
#include "keyboard.h"
#include "pit.h"
#include "string.h"

#define MAXC   128
#define NLEN   48
#define ILEN   64
#define ROWH   40
#define TOOLH  40
#define PAD    10

typedef struct { char name[NLEN]; char info[ILEN]; } contact_t;

static contact_t cts[MAXC];
static int   nct;
static int   sel = -1;
static int   top;
static char  input[NLEN + ILEN];
static int   ilen;
static int   saved_flash;

/* ---- file I/O ----------------------------------------------------------- */
static void contacts_save(void) {
    static char out[MAXC * (NLEN + ILEN + 2)];
    int n = 0;
    for (int i = 0; i < nct; i++) {
        for (int j = 0; cts[i].name[j] && n < (int)sizeof(out) - 2; j++) out[n++] = cts[i].name[j];
        out[n++] = '\t';
        for (int j = 0; cts[i].info[j] && n < (int)sizeof(out) - 2; j++) out[n++] = cts[i].info[j];
        out[n++] = '\n';
    }
    fs_node *node = fs_lookup("/contacts.txt");
    if (!node) node = fs_create("/contacts.txt", 0);
    if (node && fs_write(node, out, (uint32_t)n) == 0)
        saved_flash = (int)pit_ticks() + 1500;
}

static void contacts_load(void) {
    nct = 0;
    fs_node *node = fs_lookup("/contacts.txt");
    if (node && !node->is_dir && node->data) {
        const char *p = (const char *)node->data;
        int len = (int)node->size, i = 0;
        while (i < len && nct < MAXC) {
            int k = 0;
            while (i < len && p[i] != '\t' && p[i] != '\n' && k < NLEN - 1) cts[nct].name[k++] = p[i++];
            cts[nct].name[k] = 0;
            int m = 0;
            if (i < len && p[i] == '\t') {
                i++;
                while (i < len && p[i] != '\n' && m < ILEN - 1) cts[nct].info[m++] = p[i++];
            }
            cts[nct].info[m] = 0;
            if (cts[nct].name[0]) nct++;
            while (i < len && p[i] != '\n') i++;
            if (i < len) i++;
        }
    }
    if (nct == 0) {
        strncpy(cts[0].name, "BoltOS Support", NLEN - 1);
        strncpy(cts[0].info, "support@boltos.local", ILEN - 1);
        nct = 1;
    }
}

/* ---- editing ------------------------------------------------------------ */
/* split the input on the first comma: "Name, info" */
static void contacts_add(void) {
    if (ilen == 0 || nct >= MAXC) return;
    int comma = -1;
    for (int i = 0; input[i]; i++) if (input[i] == ',') { comma = i; break; }
    contact_t *c = &cts[nct];
    int k = 0;
    int end = comma >= 0 ? comma : ilen;
    for (int i = 0; i < end && k < NLEN - 1; i++) c->name[k++] = input[i];
    c->name[k] = 0;
    int m = 0;
    if (comma >= 0) {
        int i = comma + 1;
        while (input[i] == ' ') i++;             /* trim a leading space */
        for (; input[i] && m < ILEN - 1; i++) c->info[m++] = input[i];
    }
    c->info[m] = 0;
    nct++;
    ilen = 0; input[0] = 0;
    contacts_save();
}
static void contacts_remove(int idx) {
    if (idx < 0 || idx >= nct) return;
    for (int i = idx; i < nct - 1; i++) cts[i] = cts[i + 1];
    nct--;
    if (sel >= nct) sel = nct - 1;
    contacts_save();
}

/* ---- input -------------------------------------------------------------- */
static void contacts_key(window_t *w, char c) {
    (void)w;
    switch ((unsigned char)c) {
    case KEY_UP:   if (nct) sel = (sel <= 0) ? 0 : sel - 1; break;
    case KEY_DOWN: if (nct) sel = (sel < 0) ? 0 : (sel + 1 >= nct ? nct - 1 : sel + 1); break;
    case KEY_DEL:  contacts_remove(sel); break;
    case '\n':     contacts_add(); break;
    case '\b':     if (ilen > 0) input[--ilen] = 0; break;
    default:
        if ((unsigned char)c >= 32 && ilen < (int)sizeof(input) - 1) { input[ilen++] = c; input[ilen] = 0; }
        break;
    }
}

enum { B_ADD = 1, B_DEL };
typedef struct { int x, y, w, h, id; } hot_t;
static hot_t hots[3];
static int   nhot;
static int   row0_y, list_h;

static int btn(int x, int y, const char *label, int id, uint32_t bg, int ox, int oy) {
    int w = g_text_width(label, 1) + 20, h = 26;
    g_round(x, y, w, h, 6, bg, 255);
    g_text(x + 10, y + 6, label, 0xFFFFFF, 1);
    if (nhot < 3) { hots[nhot].x = x - ox; hots[nhot].y = y - oy;
                    hots[nhot].w = w; hots[nhot].h = h; hots[nhot].id = id; nhot++; }
    return w;
}

static void contacts_click(window_t *w, int lx, int ly) {
    (void)w;
    for (int i = 0; i < nhot; i++) {
        hot_t *h = &hots[i];
        if (lx < h->x || lx >= h->x + h->w || ly < h->y || ly >= h->y + h->h) continue;
        if (h->id == B_ADD) contacts_add();
        else if (h->id == B_DEL) contacts_remove(sel);
        return;
    }
    if (ly < row0_y || ly >= row0_y + list_h) return;
    int row = top + (ly - row0_y) / ROWH;
    if (row >= 0 && row < nct) sel = row;
}

/* ---- draw --------------------------------------------------------------- */
static void contacts_draw(window_t *w, int cx, int cy, int cw, int ch) {
    nhot = 0;

    g_fill(cx, cy, cw, TOOLH, COL_PANEL_2);
    g_hline(cx, cy + TOOLH, cw, COL_PANEL_3);

    int bx = cx + cw, by = cy + 7;
    bx -= 10 + g_text_width("Delete", 1) + 20; btn(bx, by, "Delete", B_DEL, COL_PANEL_3, cx, cy);
    bx -= 8  + g_text_width("Add", 1) + 20;     btn(bx, by, "Add", B_ADD, COL_ACCENT, cx, cy);

    int ifx = cx + PAD, ify = cy + 7, ifw = bx - 8 - ifx, ifh = 26;
    if (ifw < 40) ifw = 40;
    g_round(ifx, ify, ifw, ifh, 6, 0x0C0C12, 255);
    int focused = gui_window_focused(w);
    if (ilen) g_text(ifx + 8, ify + 6, input, COL_TEXT, 1);
    else      g_text(ifx + 8, ify + 6, "Name, phone or email", COL_TEXT_DIM, 1);
    if (focused && (pit_ticks() / 500) % 2 == 0) {
        int cxp = ifx + 8 + g_text_width(input, 1);
        if (cxp < ifx + ifw - 4) g_fill(cxp, ify + 5, 2, ifh - 10, COL_ACCENT);
    }

    int ly0 = cy + TOOLH + 1;
    g_fill(cx, ly0, cw, ch - TOOLH - 1, 0x0C0C12);
    row0_y = TOOLH + 1;
    list_h = ch - TOOLH - 1 - 24;
    int vis = list_h / ROWH; if (vis < 1) vis = 1;

    if (sel >= 0) {
        if (sel < top) top = sel;
        if (sel >= top + vis) top = sel - vis + 1;
    }
    if (top > nct - vis) top = nct - vis;
    if (top < 0) top = 0;

    static const uint32_t avc[6] = { 0x33C481, 0x3A8DDE, 0xE85AB0, 0xFFB454, 0x9B6BFF, 0xF3C766 };
    for (int r = 0; r < vis && top + r < nct; r++) {
        int i = top + r;
        int ry = ly0 + r * ROWH;
        if (i == sel) g_fill(cx, ry, cw, ROWH, COL_PANEL_3);
        /* avatar circle with the first initial */
        int ax = cx + PAD + 12, ay = ry + ROWH / 2;
        g_round(ax - 12, ay - 12, 24, 24, 12, avc[i % 6], 255);
        char init[2]; init[0] = cts[i].name[0]; init[1] = 0;
        if (init[0] >= 'a' && init[0] <= 'z') init[0] -= 32;
        g_text(ax - 4, ay - 7, init, 0xFFFFFF, 1);
        int tx = cx + PAD + 34;
        g_text(tx, ry + 6, cts[i].name, COL_TEXT, 1);
        if (cts[i].info[0]) g_text(tx, ry + 22, cts[i].info, COL_TEXT_DIM, 1);
    }

    int fy = cy + ch - 20;
    g_hline(cx, fy - 2, cw, COL_PANEL_3);
    char foot[40]; int n = 0;
    int v = nct; char tmp[8]; int tn = 0;
    if (v == 0) tmp[tn++] = '0'; else { char s[8]; int si = 0; while (v) { s[si++] = '0' + v % 10; v /= 10; } while (si) tmp[tn++] = s[--si]; }
    for (int k = 0; k < tn; k++) foot[n++] = tmp[k];
    const char *suf = " contacts";
    for (int k = 0; suf[k]; k++) foot[n++] = suf[k];
    foot[n] = 0;
    g_text(cx + PAD, fy, foot, COL_TEXT_DIM, 1);
    if (saved_flash && (int)pit_ticks() < saved_flash)
        g_text(cx + cw - 60, fy, "Saved", COL_GOOD, 1);
}

void contacts_app_init(void) {
    contacts_load();
    window_t *win = gui_add_window("Contacts", 460, 460, 0x3A8DDE, ICON_CONTACTS);
    if (!win) return;
    win->draw  = contacts_draw;
    win->key   = contacts_key;
    win->click = contacts_click;
    win->min_w = 320; win->min_h = 240;
    win->x = 320; win->y = 110;
}
