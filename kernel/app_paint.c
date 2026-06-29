/* ===========================================================================
 *  BoltOS  -  kernel/app_paint.c
 *  Paint: a simple raster drawing app. The app owns a fixed ARGB canvas in the
 *  kernel heap; the new window `drag` callback streams held-button motion in
 *  client-local coordinates, which we map onto the canvas and stamp with a round
 *  brush. A toolbar offers a colour palette, three brush sizes, and Clear.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "kheap.h"
#include "string.h"
#include "fs.h"

#define CANVW 520
#define CANVH 340
#define TOOLH 44
#define PAD   10

static uint32_t *canvas;           /* CANVW*CANVH ARGB, owned by this app */
static uint32_t  brush_color = 0x000000;
static int       brush_size  = 4;
static int       last_x = -1, last_y = -1;   /* for line interpolation while dragging */

static const uint32_t PAL[10] = {
    0x000000, 0xFFFFFF, 0xE0556B, 0xFFB454, 0xF6D32D,
    0x34C759, 0x4FC3F7, 0x9B6CF2, 0x8B5A2B, 0x7C7C8A
};
static const int SIZES[3] = { 2, 6, 12 };

/* toolbar hot rects (client-local) */
enum { H_COLOR = 1, H_SIZE, H_CLEAR, H_SAVE };
typedef struct { int x, y, w, h, kind, val; } phot_t;
static phot_t phots[20];
static int    nphot;

/* canvas pixels are stored opaque (alpha 0xFF) because g_blit treats the top
 * byte as alpha and skips fully-transparent (alpha 0) source pixels. */
static void canvas_clear(void) {
    if (canvas) for (int i = 0; i < CANVW * CANVH; i++) canvas[i] = 0xFFFFFFFF;
}

/* stamp a filled circle of radius r at canvas pixel (cxp,cyp) */
static void stamp(int cxp, int cyp, int r, uint32_t col) {
    if (!canvas) return;
    for (int dy = -r; dy <= r; dy++) {
        int yy = cyp + dy;
        if (yy < 0 || yy >= CANVH) continue;
        for (int dx = -r; dx <= r; dx++) {
            int xx = cxp + dx;
            if (xx < 0 || xx >= CANVW) continue;
            if (dx * dx + dy * dy <= r * r) canvas[yy * CANVW + xx] = col | 0xFF000000;
        }
    }
}

/* draw an interpolated brush stroke from (x0,y0) to (x1,y1) in canvas coords */
static void stroke(int x0, int y0, int x1, int y1) {
    int r = brush_size;
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = (adx > ady ? adx : ady) / (r > 1 ? r / 2 + 1 : 1);
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++)
        stamp(x0 + dx * i / steps, y0 + dy * i / steps, r, brush_color);
}

static void paint_drag(window_t *w, int lx, int ly) {
    (void)w;
    int cxp = lx - PAD, cyp = ly - TOOLH - PAD;     /* client -> canvas */
    if (cxp < 0 || cxp >= CANVW || cyp < 0 || cyp >= CANVH) { last_x = last_y = -1; return; }
    if (last_x < 0) stamp(cxp, cyp, brush_size, brush_color);
    else            stroke(last_x, last_y, cxp, cyp);
    last_x = cxp; last_y = cyp;
}

/* Export the canvas as a 24-bit BMP. Bare names go to /home/Pictures with a
 * .bmp default; an absolute path is used verbatim. */
static void paint_saveas_cb(const char *name) {
    if (!canvas || !name || !name[0]) return;
    char path[FS_PATH_MAX];
    if (name[0] == '/') {
        strncpy(path, name, sizeof(path) - 1); path[sizeof(path) - 1] = 0;
    } else {
        path[0] = 0;
        kstrlcat(path, "/home/Pictures/", sizeof(path));
        kstrlcat(path, name, sizeof(path));
        /* append .bmp if the name has no extension */
        int hasdot = 0; for (int i = 0; path[i]; i++) if (path[i] == '.') hasdot = 1;
        if (!hasdot) kstrlcat(path, ".bmp", sizeof(path));
    }
    int w = CANVW, h = CANVH;
    uint32_t row = (uint32_t)w * 3, pad = (4 - (row & 3)) & 3, stride = row + pad;
    uint32_t sz = 54 + stride * (uint32_t)h;
    uint8_t *bmp = (uint8_t *)kmalloc(sz);
    if (!bmp) return;
    memset(bmp, 0, 54);
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = (uint8_t)sz; bmp[3] = (uint8_t)(sz >> 8); bmp[4] = (uint8_t)(sz >> 16); bmp[5] = (uint8_t)(sz >> 24);
    bmp[10] = 54; bmp[14] = 40;
    bmp[18] = (uint8_t)w; bmp[19] = (uint8_t)(w >> 8); bmp[20] = (uint8_t)(w >> 16); bmp[21] = (uint8_t)(w >> 24);
    bmp[22] = (uint8_t)h; bmp[23] = (uint8_t)(h >> 8); bmp[24] = (uint8_t)(h >> 16); bmp[25] = (uint8_t)(h >> 24);
    bmp[26] = 1; bmp[28] = 24;
    for (int y = 0; y < h; y++) {
        uint8_t  *dst = bmp + 54 + (uint32_t)(h - 1 - y) * stride;   /* bottom-up */
        uint32_t *src = &canvas[(uint32_t)y * w];
        for (int x = 0; x < w; x++) {
            uint32_t p = src[x];
            dst[x * 3 + 0] = (uint8_t)(p);          /* B */
            dst[x * 3 + 1] = (uint8_t)(p >> 8);     /* G */
            dst[x * 3 + 2] = (uint8_t)(p >> 16);    /* R */
        }
    }
    if (!fs_lookup("/home/Pictures")) fs_create("/home/Pictures", 1);
    fs_node *f = fs_lookup(path);
    if (!f) f = fs_create(path, 0);
    if (f && fs_write(f, bmp, sz) == 0) gui_toast("Image saved");
    kfree(bmp);
}

static void paint_click(window_t *w, int lx, int ly) {
    (void)w;
    last_x = last_y = -1;                            /* reset stroke on a fresh press */
    for (int i = 0; i < nphot; i++) {
        phot_t *h = &phots[i];
        if (lx < h->x || lx >= h->x + h->w || ly < h->y || ly >= h->y + h->h) continue;
        if (h->kind == H_COLOR) brush_color = PAL[h->val];
        else if (h->kind == H_SIZE) brush_size = SIZES[h->val];
        else if (h->kind == H_CLEAR) canvas_clear();
        else if (h->kind == H_SAVE) gui_prompt("Save image (filename.bmp)", "", paint_saveas_cb);
        return;
    }
}

static void paint_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w; (void)cw; (void)ch;
    nphot = 0;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    /* toolbar */
    g_fill(cx, cy, cw, TOOLH, COL_PANEL_2);
    g_hline(cx, cy + TOOLH, cw, COL_PANEL_3);

    int sw = 22, x = cx + PAD, y = cy + 11;
    for (int i = 0; i < 10; i++) {
        int sel = (PAL[i] == brush_color);
        if (sel) { g_round(x - 2, y - 2, sw + 4, sw + 4, 6, COL_ACCENT, 255); }
        g_round(x, y, sw, sw, 5, PAL[i], 255);
        g_rect(x, y, sw, sw, COL_PANEL_3);
        phots[nphot++] = (phot_t){ x - cx, y - cy, sw, sw, H_COLOR, i };
        x += sw + 6;
    }

    /* brush sizes */
    x += 10;
    for (int i = 0; i < 3; i++) {
        int sel = (SIZES[i] == brush_size);
        g_round(x, y, sw, sw, 5, sel ? COL_ACCENT : COL_PANEL_3, 255);
        int r = SIZES[i] / 2 + 1;
        g_round(x + sw / 2 - r, y + sw / 2 - r, 2 * r, 2 * r, r, sel ? 0xFFFFFF : COL_TEXT, 255);
        phots[nphot++] = (phot_t){ x - cx, y - cy, sw, sw, H_SIZE, i };
        x += sw + 6;
    }

    /* clear button */
    x += 10;
    int cwd = g_text_width("Clear", 1) + 20;
    g_round(x, y, cwd, sw, 6, COL_BAD, 255);
    g_text(x + 10, y + 4, "Clear", 0xFFFFFF, 1);
    phots[nphot++] = (phot_t){ x - cx, y - cy, cwd, sw, H_CLEAR, 0 };

    /* save button */
    x += cwd + 8;
    int swd = g_text_width("Save", 1) + 20;
    g_round(x, y, swd, sw, 6, COL_ACCENT, 255);
    g_text(x + 10, y + 4, "Save", 0xFFFFFF, 1);
    phots[nphot++] = (phot_t){ x - cx, y - cy, swd, sw, H_SAVE, 0 };

    /* canvas */
    int dx = cx + PAD, dy = cy + TOOLH + PAD;
    if (canvas) {
        g_rect(dx - 1, dy - 1, CANVW + 2, CANVH + 2, COL_PANEL_3);
        g_blit(dx, dy, CANVW, CANVH, canvas, CANVW, CANVH);
    }
}

void paint_app_init(void) {
    canvas = (uint32_t *)kmalloc((uint64_t)CANVW * CANVH * 4);
    canvas_clear();
    window_t *win = gui_add_window("Paint", CANVW + 2 * PAD + 4, CANVH + TOOLH + 2 * PAD + 4,
                                   0xE0556B, ICON_PAINT);
    if (!win) return;
    win->draw  = paint_draw;
    win->click = paint_click;
    win->drag  = paint_drag;
    win->min_w = 360; win->min_h = 300;
    win->x = 120; win->y = 60;
}
