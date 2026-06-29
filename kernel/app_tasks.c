/* ===========================================================================
 *  BoltOS  -  kernel/app_tasks.c
 *  Tasks: a to-do list backed by the filesystem. Type in the input field and
 *  press Enter (or "Add") to append a task; click a task's checkbox to mark it
 *  done; select a row and press Del (or "Delete") to remove it. Tasks persist
 *  to /tasks.txt -- one line per task, "x <text>" if done, " <text>" if not --
 *  and the FS autosaves, so the list survives reboots.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "fs.h"
#include "keyboard.h"
#include "pit.h"
#include "string.h"

#define MAXT   128
#define TLEN   120
#define ROWH   24
#define TOOLH  40
#define PAD    10

typedef struct { char text[TLEN]; int done; } task_t;

static task_t tasks[MAXT];
static int    ntask;
static int    sel = -1;          /* selected row, or -1 */
static int    top;               /* first visible row (scroll) */
static char   input[TLEN];       /* new-task input field */
static int    ilen;
static int    saved_flash;       /* pit tick until which to show "Saved" */
static window_t *tw_win;

/* ---- file I/O ----------------------------------------------------------- */
static void tasks_save(void) {
    char out[MAXT * (TLEN + 2)];
    int  n = 0;
    for (int i = 0; i < ntask; i++) {
        out[n++] = tasks[i].done ? 'x' : ' ';
        out[n++] = ' ';
        for (int j = 0; tasks[i].text[j] && n < (int)sizeof(out) - 2; j++)
            out[n++] = tasks[i].text[j];
        out[n++] = '\n';
    }
    fs_node *node = fs_lookup("/tasks.txt");
    if (!node) node = fs_create("/tasks.txt", 0);
    if (node && fs_write(node, out, (uint32_t)n) == 0)
        saved_flash = (int)pit_ticks() + 1500;
}

static void tasks_load(void) {
    ntask = 0;
    fs_node *node = fs_lookup("/tasks.txt");
    if (node && !node->is_dir && node->data) {
        const char *p = (const char *)node->data;
        int len = (int)node->size, i = 0;
        while (i < len && ntask < MAXT) {
            int done = (p[i] == 'x' || p[i] == 'X');
            /* skip the status char + a single following space */
            if (p[i] == 'x' || p[i] == 'X' || p[i] == ' ') i++;
            if (i < len && p[i] == ' ') i++;
            int k = 0;
            while (i < len && p[i] != '\n' && k < TLEN - 1) tasks[ntask].text[k++] = p[i++];
            tasks[ntask].text[k] = 0;
            tasks[ntask].done = done;
            if (k > 0) ntask++;
            while (i < len && p[i] != '\n') i++;   /* drop overflow */
            if (i < len) i++;                        /* eat '\n' */
        }
    }
    if (ntask == 0) {
        strncpy(tasks[0].text, "Welcome -- type a task and press Enter", TLEN - 1);
        tasks[0].done = 0; ntask = 1;
    }
}

/* ---- editing ------------------------------------------------------------ */
static void tasks_add(void) {
    if (ilen == 0 || ntask >= MAXT) return;
    strncpy(tasks[ntask].text, input, TLEN - 1);
    tasks[ntask].text[TLEN - 1] = 0;
    tasks[ntask].done = 0;
    ntask++;
    ilen = 0; input[0] = 0;
    tasks_save();
}
static void tasks_remove(int idx) {
    if (idx < 0 || idx >= ntask) return;
    for (int i = idx; i < ntask - 1; i++) tasks[i] = tasks[i + 1];
    ntask--;
    if (sel >= ntask) sel = ntask - 1;
    tasks_save();
}
static void tasks_clear_done(void) {
    int w = 0;
    for (int i = 0; i < ntask; i++) if (!tasks[i].done) tasks[w++] = tasks[i];
    ntask = w; sel = -1;
    tasks_save();
}

/* ---- input -------------------------------------------------------------- */
static void tasks_key(window_t *w, char c) {
    (void)w;
    switch ((unsigned char)c) {
    case KEY_UP:   if (ntask) { sel = (sel <= 0) ? 0 : sel - 1; } break;
    case KEY_DOWN: if (ntask) { sel = (sel < 0) ? 0 : (sel + 1 >= ntask ? ntask - 1 : sel + 1); } break;
    case KEY_DEL:  tasks_remove(sel); break;
    case '\n':     tasks_add(); break;
    case '\b':     if (ilen > 0) input[--ilen] = 0; break;
    case ' ':
        /* space toggles the selected task only when the input field is empty */
        if (ilen == 0 && sel >= 0) { tasks[sel].done = !tasks[sel].done; tasks_save(); break; }
        /* else fall through to typing */
        /* fallthrough */
    default:
        if ((unsigned char)c >= 32 && ilen < TLEN - 1) { input[ilen++] = c; input[ilen] = 0; }
        break;
    }
}

/* toolbar button hot rects (client-local) */
enum { B_ADD = 1, B_DEL, B_CLR };
typedef struct { int x, y, w, h, id; } hot_t;
static hot_t hots[4];
static int   nhot;
static int   row0_y;          /* client-local y of the first task row */
static int   list_h;

static int btn(int x, int y, const char *label, int id, uint32_t bg, int ox, int oy) {
    int w = g_text_width(label, 1) + 20, h = 26;
    g_round(x, y, w, h, 6, bg, 255);
    g_text(x + 10, y + 6, label, 0xFFFFFF, 1);
    if (nhot < 4) { hots[nhot].x = x - ox; hots[nhot].y = y - oy;
                    hots[nhot].w = w; hots[nhot].h = h; hots[nhot].id = id; nhot++; }
    return w;
}

static void tasks_click(window_t *w, int lx, int ly) {
    (void)w;
    for (int i = 0; i < nhot; i++) {
        hot_t *h = &hots[i];
        if (lx < h->x || lx >= h->x + h->w || ly < h->y || ly >= h->y + h->h) continue;
        if (h->id == B_ADD) tasks_add();
        else if (h->id == B_DEL) tasks_remove(sel);
        else if (h->id == B_CLR) tasks_clear_done();
        return;
    }
    /* task rows */
    if (ly < row0_y || ly >= row0_y + list_h) return;
    int row = top + (ly - row0_y) / ROWH;
    if (row < 0 || row >= ntask) return;
    sel = row;
    /* checkbox column toggles done */
    if (lx >= PAD && lx < PAD + ROWH) { tasks[row].done = !tasks[row].done; tasks_save(); }
}

/* ---- draw --------------------------------------------------------------- */
static void tasks_draw(window_t *w, int cx, int cy, int cw, int ch) {
    nhot = 0;

    /* toolbar with the input field */
    g_fill(cx, cy, cw, TOOLH, COL_PANEL_2);
    g_hline(cx, cy + TOOLH, cw, COL_PANEL_3);

    int bx = cx + cw, by = cy + 7;
    bx -= 10 + g_text_width("Clear Done", 1) + 20; btn(bx, by, "Clear Done", B_CLR, COL_PANEL_3, cx, cy);
    bx -= 8  + g_text_width("Delete", 1) + 20;     btn(bx, by, "Delete", B_DEL, COL_PANEL_3, cx, cy);
    bx -= 8  + g_text_width("Add", 1) + 20;        btn(bx, by, "Add", B_ADD, COL_ACCENT, cx, cy);

    /* input field fills the space left of the buttons */
    int ifx = cx + PAD, ify = cy + 7, ifw = bx - 8 - ifx, ifh = 26;
    if (ifw < 40) ifw = 40;
    g_round(ifx, ify, ifw, ifh, 6, 0x0C0C12, 255);
    int focused = gui_window_focused(w);
    if (ilen)
        g_text(ifx + 8, ify + 6, input, COL_TEXT, 1);
    else
        g_text(ifx + 8, ify + 6, "Add a task...", COL_TEXT_DIM, 1);
    if (focused && (pit_ticks() / 500) % 2 == 0) {
        int cxp = ifx + 8 + g_text_width(input, 1);
        if (cxp < ifx + ifw - 4) g_fill(cxp, ify + 5, 2, ifh - 10, COL_ACCENT);
    }

    /* list area */
    int ly0 = cy + TOOLH + 1;
    g_fill(cx, ly0, cw, ch - TOOLH - 1, 0x0C0C12);
    row0_y = TOOLH + 1;          /* client-local */
    list_h = ch - TOOLH - 1 - 26; /* leave room for footer */
    int vis = list_h / ROWH; if (vis < 1) vis = 1;

    if (sel >= 0) {
        if (sel < top) top = sel;
        if (sel >= top + vis) top = sel - vis + 1;
    }
    if (top > ntask - vis) top = ntask - vis;
    if (top < 0) top = 0;

    int done_cnt = 0;
    for (int i = 0; i < ntask; i++) if (tasks[i].done) done_cnt++;

    for (int r = 0; r < vis && top + r < ntask; r++) {
        int i = top + r;
        int ry = ly0 + r * ROWH;
        if (i == sel) g_fill(cx, ry, cw, ROWH, COL_PANEL_3);
        /* checkbox */
        int bxx = cx + PAD, byy = ry + 4, bs = ROWH - 8;
        g_round(bxx, byy, bs, bs, 4, tasks[i].done ? COL_GOOD : 0x222230, 255);
        if (tasks[i].done) {
            /* check mark */
            g_fill(bxx + 4, byy + bs / 2, 3, 3, 0xFFFFFF);
            g_fill(bxx + 6, byy + bs - 7, 3, 3, 0xFFFFFF);
            g_fill(bxx + 8, byy + 4, 3, bs - 9, 0xFFFFFF);
        }
        uint32_t tc = tasks[i].done ? COL_TEXT_DIM : COL_TEXT;
        int tx = bxx + bs + 10;
        g_text(tx, ry + 4, tasks[i].text, tc, 1);
        if (tasks[i].done)   /* strike-through */
            g_hline(tx, ry + ROWH / 2, g_text_width(tasks[i].text, 1), COL_TEXT_DIM);
    }

    /* footer: counts + saved flash */
    int fy = cy + ch - 22;
    g_hline(cx, fy - 2, cw, COL_PANEL_3);
    char foot[64];
    int n = 0;
    /* small manual int->str: "%d tasks, %d done" */
    const char *p1 = " tasks, ", *p2 = " done";
    int t1 = ntask, d1 = done_cnt;
    char tmp[12]; int tn = 0;
    if (t1 == 0) tmp[tn++] = '0'; else { int v = t1; char s[12]; int si = 0; while (v) { s[si++] = '0' + v % 10; v /= 10; } while (si) tmp[tn++] = s[--si]; }
    for (int k = 0; k < tn; k++) foot[n++] = tmp[k];
    for (int k = 0; p1[k]; k++) foot[n++] = p1[k];
    tn = 0; if (d1 == 0) tmp[tn++] = '0'; else { int v = d1; char s[12]; int si = 0; while (v) { s[si++] = '0' + v % 10; v /= 10; } while (si) tmp[tn++] = s[--si]; }
    for (int k = 0; k < tn; k++) foot[n++] = tmp[k];
    for (int k = 0; p2[k]; k++) foot[n++] = p2[k];
    foot[n] = 0;
    g_text(cx + PAD, fy, foot, COL_TEXT_DIM, 1);
    if (saved_flash && (int)pit_ticks() < saved_flash)
        g_text(cx + cw - 60, fy, "Saved", COL_GOOD, 1);
}

void tasks_app_init(void) {
    tasks_load();
    window_t *win = gui_add_window("Tasks", 480, 440, 0x33C481, ICON_TASKS);
    if (!win) return;
    win->draw  = tasks_draw;
    win->key   = tasks_key;
    win->click = tasks_click;
    win->min_w = 320; win->min_h = 240;
    win->x = 300; win->y = 110;
    tw_win = win;
}
