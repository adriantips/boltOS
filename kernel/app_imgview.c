/* ===========================================================================
 *  BoltOS  -  kernel/app_imgview.c
 *  Photos: a simple image viewer. Decodes PNG / JPEG / GIF / BMP / TGA from the
 *  filesystem (via image_decode) and paints it scaled to fit the window, on a
 *  checkerboard backdrop so transparency reads. Opened from the file manager by
 *  double-clicking an image, or from its desktop icon (shows a hint).
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "fs.h"
#include "image.h"
#include "string.h"

#define TOOLH 30

static image_t  *iv_img;
static char      iv_path[FS_PATH_MAX];
static char      iv_name[FS_NAME_MAX];
static int       iv_err;                 /* 1 = decode failed                  */
static window_t *iv_win;

static void iv_clear(void) {
    if (iv_img) { image_free(iv_img); iv_img = 0; }
    iv_err = 0;
}

/* compute the largest integer-fit rectangle preserving aspect ratio */
static void fit_rect(int iw, int ih, int aw, int ah, int *ox, int *oy, int *ow, int *oh) {
    if (iw <= 0 || ih <= 0 || aw <= 0 || ah <= 0) { *ox = *oy = 0; *ow = *oh = 0; return; }
    /* scale = min(aw/iw, ah/ih) in rationals; compare aw*ih vs ah*iw */
    int w, h;
    if ((int64_t)aw * ih <= (int64_t)ah * iw) { w = aw; h = (int)((int64_t)aw * ih / iw); }
    else                                      { h = ah; w = (int)((int64_t)ah * iw / ih); }
    if (w < 1) w = 1; if (h < 1) h = 1;
    *ow = w; *oh = h;
    *ox = (aw - w) / 2; *oy = (ah - h) / 2;
}

/* "Set as wallpaper" button hot-rect, client-local, rebuilt each draw */
static int wp_bx, wp_by, wp_bw, wp_bh;

static void imgview_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    /* toolbar with the file name */
    g_fill(cx, cy, cw, TOOLH, COL_PANEL_2);
    g_hline(cx, cy + TOOLH, cw, COL_PANEL_3);
    g_text(cx + 10, cy + 9, iv_name[0] ? iv_name : "Photos", COL_TEXT, 1);

    /* "Set as wallpaper" button (only meaningful with an image loaded) */
    wp_bw = 0;
    if (iv_img) {
        const char *lbl = "Set as wallpaper";
        int bw = g_text_width(lbl, 1) + 20, bh = 20;
        int bx = cx + cw - bw - 90, by = cy + 5;
        g_round(bx, by, bw, bh, 6, COL_ACCENT, 255);
        g_text(bx + 10, by + 4, lbl, 0xFFFFFF, 1);
        wp_bx = bx - cx; wp_by = by - cy; wp_bw = bw; wp_bh = bh;   /* client-local */
    }
    if (iv_img) {
        char dim[40]; int p = 0;
        char tmp[12]; int t;
        unsigned v = (unsigned)iv_img->w;
        t = 0; char r1[12]; if (!v) r1[t++]='0'; while (v){ r1[t++]='0'+v%10; v/=10; }
        while (t) tmp[p++] = r1[--t];
        dim[0]=0; /* assemble "WxH" */
        for (int i=0;i<p;i++) dim[i]=tmp[i]; dim[p++]='x';
        v = (unsigned)iv_img->h; t=0; char r2[12]; if(!v) r2[t++]='0'; while(v){ r2[t++]='0'+v%10; v/=10; }
        while (t) dim[p++]=r2[--t]; dim[p]=0;
        int tw = g_text_width(dim, 1);
        g_text(cx + cw - tw - 10, cy + 9, dim, COL_TEXT_DIM, 1);
    }

    int ay = cy + TOOLH + 1, ah = ch - TOOLH - 1, aw = cw;
    /* checkerboard backdrop */
    for (int yy = 0; yy < ah; yy += 16)
        for (int xx = 0; xx < aw; xx += 16) {
            uint32_t cellc = ((xx / 16 + yy / 16) & 1) ? 0x1A1A22 : 0x12121A;
            int bw = (xx + 16 <= aw) ? 16 : aw - xx;
            int bh = (yy + 16 <= ah) ? 16 : ah - yy;
            g_fill(cx + xx, ay + yy, bw, bh, cellc);
        }

    if (iv_err) {
        const char *m = "Could not open this image.";
        g_text(cx + (aw - g_text_width(m, 1)) / 2, ay + ah / 2 - 4, m, COL_BAD, 1);
        return;
    }
    if (!iv_img) {
        const char *m = "Open an image from Files to view it here.";
        g_text(cx + (aw - g_text_width(m, 1)) / 2, ay + ah / 2 - 4, m, COL_TEXT_DIM, 1);
        return;
    }
    int ox, oy, ow, oh;
    fit_rect(iv_img->w, iv_img->h, aw, ah, &ox, &oy, &ow, &oh);
    g_blit(cx + ox, ay + oy, ow, oh, iv_img->px, iv_img->w, iv_img->h);
}

static void imgview_click(window_t *w, int lx, int ly) {
    (void)w;
    if (iv_img && wp_bw && lx >= wp_bx && lx < wp_bx + wp_bw &&
        ly >= wp_by && ly < wp_by + wp_bh)
        gui_set_wallpaper_image(iv_path);
}

void imgview_app_init(void) {
    iv_img = 0; iv_path[0] = 0; iv_name[0] = 0; iv_err = 0;
    window_t *win = gui_add_window("Photos", 600, 480, 0x5AB0FF, ICON_IMAGE);
    if (!win) return;
    win->draw  = imgview_draw;
    win->click = imgview_click;
    win->min_w = 280; win->min_h = 200;
    win->x = 260; win->y = 70;
    iv_win = win;
}

/* Open an image file (called by the file manager via file association). */
void imgview_open_path(const char *path) {
    if (!path || !path[0]) return;
    strncpy(iv_path, path, sizeof(iv_path) - 1);
    iv_path[sizeof(iv_path) - 1] = 0;
    /* extract the basename for the title */
    const char *base = path;
    for (const char *s = path; *s; s++) if (*s == '/') base = s + 1;
    strncpy(iv_name, base, sizeof(iv_name) - 1);
    iv_name[sizeof(iv_name) - 1] = 0;

    iv_clear();
    fs_node *n = fs_lookup(path);
    if (n && !n->is_dir && n->data && n->size)
        iv_img = image_decode(n->data, n->size);
    if (!iv_img) iv_err = 1;
    if (iv_win) gui_open(iv_win);
}
