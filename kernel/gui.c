/* ===========================================================================
 *  BoltOS  -  kernel/gui.c
 *  Compositing window manager: graphics primitives, window framing, focus and
 *  stacking, dragging, minimise/maximise/close, a centred taskbar with a start
 *  menu and clock, and the mouse cursor. One backbuffer, one blit per frame.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "xhci.h"
#include "ttf.h"
#include "settings.h"
#include "framebuffer.h"
#include "console.h"
#include "keyboard.h"
#include "kprintf.h"
#include "mouse.h"
#include "kheap.h"
#include "pmm.h"
#include "mm.h"
#include "pcspk.h"
#include "pit.h"
#include "hw.h"
#include "shell.h"
#include "string.h"
#include "fs.h"
#include "image.h"
#include "users.h"
#include "audio.h"
#include "netif.h"
#include "net.h"

extern const unsigned char font8x8_basic[128][8];   /* 8x8 retro face   */
extern const unsigned char font_8x16[95][16];        /* 8x16 Arial face  */

/* Active GUI font face (FONT_RETRO / FONT_ARIAL). Arial is the default; the
 * Settings app flips this via g_set_font. Both faces are 8px wide so every
 * x-advance and width calc is identical -- only the glyph height differs. */
static int g_font = FONT_ARIAL;

#define TITLE_H    32
#define TASKBAR_H  48
#define RADIUS     9
#define MAX_WIN    40
#define DI_W       80              /* desktop icon cell width  */
#define DI_H       86              /* desktop icon cell height */
#define MAX_PW     3840            /* largest panel we allocate backbuffers for */
#define MAX_PH     2160            /* (4K) -- res changes reuse these buffers   */
#define RS_BORDER  6               /* window edge resize grab band, in px       */
enum { RS_L = 1, RS_R = 2, RS_T = 4, RS_B = 8 };   /* resize-edge bitmask       */

static int hires_ok;               /* big backbuffers allocated -> res switch allowed */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- backbuffer + clip -------------------------------------------------- */
static uint32_t *BB;            /* compositor backbuffer (alloc PW*PH, use W*H) */
static uint32_t *BG;            /* prerendered desktop wallpaper (W*H) or 0 */
static image_t  *wall_img;      /* user-chosen image wallpaper, or 0 (procedural) */
static char      wall_img_path[FS_PATH_MAX];
static int       dirty = 1;     /* a frame needs recompositing                  */
static char      toast_msg[72];          /* transient bottom-right notification  */
static uint64_t  toast_until;            /* pit tick the toast disappears at      */

/* Modal text prompt (Save As, rename, ...). One at a time; apps call gui_prompt
 * with a callback that receives the entered string when the user confirms. */
static int        prompt_active;
static char       prompt_title[40];
static char       prompt_buf[FS_PATH_MAX];
static int        prompt_len;
static void     (*prompt_cb)(const char *);
static int       PW, PH;        /* physical panel size (fixed by the bootloader) */
static int       W, H;          /* logical desktop size (the resolution setting)  */
static int       out_x, out_y, out_w, out_h;   /* where the logical desktop lands  */
static int       clx0, cly0, clx1, cly1;   /* active clip rect (exclusive hi) */

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iabs(int v) { return v < 0 ? -v : v; }
static int isqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }

void g_set_clip(int x, int y, int w, int h) {
    clx0 = imax(0, x);     cly0 = imax(0, y);
    clx1 = imin(W, x + w); cly1 = imin(H, y + h);
}
void g_clear_clip(void) { clx0 = 0; cly0 = 0; clx1 = W; cly1 = H; }

static inline void px(int x, int y, uint32_t c) {
    if (x >= clx0 && x < clx1 && y >= cly0 && y < cly1) BB[y * W + x] = c;
}
static inline void px_blend(int x, int y, uint32_t c, uint8_t a) {
    if (x < clx0 || x >= clx1 || y < cly0 || y >= cly1) return;
    uint32_t d = BB[y * W + x];
    uint32_t r = (((c >> 16 & 0xFF) * a + (d >> 16 & 0xFF) * (255 - a)) / 255) & 0xFF;
    uint32_t g = (((c >> 8  & 0xFF) * a + (d >> 8  & 0xFF) * (255 - a)) / 255) & 0xFF;
    uint32_t b = (((c       & 0xFF) * a + (d       & 0xFF) * (255 - a)) / 255) & 0xFF;
    BB[y * W + x] = (r << 16) | (g << 8) | b;
}

void g_fill(int x, int y, int w, int h, uint32_t c) {
    int x0 = imax(x, clx0), y0 = imax(y, cly0);
    int x1 = imin(x + w, clx1), y1 = imin(y + h, cly1);
    for (int j = y0; j < y1; j++) { uint32_t *row = BB + j * W; for (int i = x0; i < x1; i++) row[i] = c; }
}
void g_blend(int x, int y, int w, int h, uint32_t c, uint8_t a) {
    int x0 = imax(x, clx0), y0 = imax(y, cly0);
    int x1 = imin(x + w, clx1), y1 = imin(y + h, cly1);
    for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) px_blend(i, j, c, a);
}
void g_hline(int x, int y, int w, uint32_t c) { g_fill(x, y, w, 1, c); }
void g_vline(int x, int y, int h, uint32_t c) { g_fill(x, y, 1, h, c); }
void g_rect(int x, int y, int w, int h, uint32_t c) {
    g_hline(x, y, w, c); g_hline(x, y + h - 1, w, c);
    g_vline(x, y, h, c); g_vline(x + w - 1, y, h, c);
}

/* horizontal inset of a rounded corner at distance `dy` from the rounded edge */
static int corner_dx(int r, int dy) {
    if (dy >= r) return 0;
    return r - isqrt(r * r - (r - 1 - dy) * (r - 1 - dy));
}

void g_round(int x, int y, int w, int h, int r, uint32_t c, uint8_t a) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        int dx = 0;
        if (j < r)            dx = corner_dx(r, j);
        else if (j >= h - r)  dx = corner_dx(r, h - 1 - j);
        if (a >= 255) g_fill(x + dx, y + j, w - 2 * dx, 1, c);
        else for (int i = x + dx; i < x + w - dx; i++) px_blend(i, y + j, c, a);
    }
}

/* rounded only on the top two corners (window title bars) */
static void g_round_top(int x, int y, int w, int h, int r, uint32_t c) {
    if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        int dx = (j < r) ? corner_dx(r, j) : 0;
        g_fill(x + dx, y + j, w - 2 * dx, 1, c);
    }
}

/* ---- text --------------------------------------------------------------- */
/* Fetch the bitmap rows + row count of one glyph in the active face. Returns
 * NULL for a blank/uncovered glyph (nothing to draw). Both faces are 8px wide.
 * The 8x8 retro face packs bit 0 = leftmost pixel; the 8x16 Arial face packs
 * bit 7 = leftmost. glyph_bit() hides that so the draw loops stay uniform. */
static const unsigned char *glyph_rows(unsigned char u, int *rows) {
    if (g_font == FONT_ARIAL) {
        *rows = 16;
        if (u >= 32 && u <= 126) return font_8x16[u - 32];
        return 0;                                  /* outside Arial range -> blank */
    }
    *rows = 8;
    if (u < 128) return font8x8_basic[u];
    return 0;
}
/* is column rx (0 = leftmost) lit in this glyph row? */
static inline int glyph_bit(unsigned char bits, int rx) {
    return (g_font == FONT_ARIAL) ? (bits & (0x80u >> rx)) : (bits & (1u << rx));
}

int g_font_height(int scale) { return (g_font == FONT_ARIAL ? 16 : 8) * scale; }

void g_char(int x, int y, char ch, uint32_t color, int s) {
    int rows;
    const unsigned char *g = glyph_rows((unsigned char)ch, &rows);
    if (!g) return;
    for (int ry = 0; ry < rows; ry++) {
        unsigned char bits = g[ry];
        for (int rx = 0; rx < 8; rx++)
            if (glyph_bit(bits, rx)) g_fill(x + rx * s, y + ry * s, s, s, color);
    }
}
void g_text(int x, int y, const char *str, uint32_t color, int s) {
    for (; *str; str++) { g_char(x, y, *str, color, s); x += 8 * s; }
}
int g_text_width(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

/* ---- scalable anti-aliased text via the TrueType rasterizer ------------- *
 * Renders to the backbuffer with per-pixel alpha. y is the text TOP; px is the
 * cap/em pixel size. Falls back to nothing if no font was embedded. */
static uint32_t tt_color;
static void tt_plot(int x, int y, uint8_t cov, void *ctx) { (void)ctx; px_blend(x, y, tt_color, cov); }
void g_text_tt(int x, int y_top, const char *s, uint32_t color, int px) {
    if (!ttf_ready()) { g_text(x, y_top, s, color, px >= 24 ? 2 : 1); return; }
    int asc = (px * 80) / 100;                 /* approx baseline from top */
    tt_color = color;
    ttf_draw(s, x, y_top + asc, px, tt_plot, 0);
}
int g_text_tt_width(const char *s, int px) {
    return ttf_ready() ? ttf_text_width(s, px) : (int)strlen(s) * 8;
}

/* ---- proportional text -------------------------------------------------- *
 * Per-glyph metrics are derived once from the active bitmap font: the ink box
 * (first and last lit column) gives a left bearing and a tight advance; blanks
 * get a fixed word-space. Glyphs are drawn left-trimmed to their ink so spacing
 * is even. Used by the browser body for prose; the rest of the UI stays
 * monospace. Rebuilt whenever the font face changes (gp_ready cleared).        */
static int gp_adv[128], gp_minc[128], gp_ready = 0;
static void gp_build(void) {
    for (int u = 0; u < 128; u++) {
        int rows;
        const unsigned char *g = glyph_rows((unsigned char)u, &rows);
        int minc = 8, maxc = -1;
        if (g) for (int ry = 0; ry < rows; ry++) {
            unsigned char b = g[ry];
            for (int rx = 0; rx < 8; rx++) if (glyph_bit(b, rx)) { if (rx < minc) minc = rx; if (rx > maxc) maxc = rx; }
        }
        if (maxc < 0) { gp_adv[u] = 4; gp_minc[u] = 0; }      /* space / blank glyph */
        else          { gp_adv[u] = (maxc - minc + 1) + 1; gp_minc[u] = minc; }
    }
    gp_ready = 1;
}
int g_glyph_adv(char c, int scale) {
    unsigned char u = (unsigned char)c; if (u >= 128) u = '?';
    if (!gp_ready) gp_build();
    return gp_adv[u] * scale;
}
void g_char_p(int x, int y, char ch, uint32_t color, int s, int italic) {
    unsigned char u = (unsigned char)ch; if (u >= 128) u = '?';
    if (!gp_ready) gp_build();
    int rows;
    const unsigned char *g = glyph_rows(u, &rows);
    if (!g) return;
    int minc = gp_minc[u];
    for (int ry = 0; ry < rows; ry++) {
        unsigned char bits = g[ry];
        int sk = italic ? ((rows - 1 - ry) * s) / 3 : 0;     /* fake-italic shear */
        for (int rx = 0; rx < 8; rx++)
            if (glyph_bit(bits, rx)) g_fill(x + (rx - minc) * s + sk, y + ry * s, s, s, color);
    }
}

/* swap the active GUI font face; clears proportional metrics so they rebuild */
void g_set_font(int face) {
    int f = (face == FONT_ARIAL) ? FONT_ARIAL : FONT_RETRO;
    if (f == g_font) return;
    g_font = f;
    gp_ready = 0;
    gui_request_redraw();
}
int g_get_font(void) { return g_font; }
int g_text_width_pn(const char *s, int len, int scale) {
    int w = 0; for (int i = 0; i < len; i++) w += g_glyph_adv(s[i], scale); return w;
}
int g_text_pn(int x, int y, const char *s, int len, uint32_t color, int scale, int italic) {
    for (int i = 0; i < len; i++) { g_char_p(x, y, s[i], color, scale, italic); x += g_glyph_adv(s[i], scale); }
    return x;
}

/* Blit a w*h ARGB image into the dst rectangle, nearest-neighbour scaled and
 * alpha-blended against the backbuffer. Honours the active clip rect. */
void g_blit(int dx, int dy, int dw, int dh, const uint32_t *src, int sw, int sh) {
    if (!src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int x0 = imax(dx, clx0), y0 = imax(dy, cly0);
    int x1 = imin(dx + dw, clx1), y1 = imin(dy + dh, cly1);
    for (int y = y0; y < y1; y++) {
        int sy = (y - dy) * sh / dh; if (sy >= sh) sy = sh - 1;
        const uint32_t *srow = src + sy * sw;
        for (int x = x0; x < x1; x++) {
            int sx = (x - dx) * sw / dw; if (sx >= sw) sx = sw - 1;
            uint32_t c = srow[sx];
            uint8_t a = c >> 24;
            if (a == 255)      BB[y * W + x] = c & 0xFFFFFF;
            else if (a == 0)   continue;
            else               px_blend(x, y, c & 0xFFFFFF, a);
        }
    }
}

/* ===========================================================================
 *  Desktop wallpaper
 * ===========================================================================*/
static uint32_t lerp(uint32_t a, uint32_t b, int n, int d) {
    int ar = a >> 16 & 0xFF, ag = a >> 8 & 0xFF, ab = a & 0xFF;
    int br = b >> 16 & 0xFF, bg = b >> 8 & 0xFF, bb = b & 0xFF;
    int r = ar + (br - ar) * n / d, g = ag + (bg - ag) * n / d, c = ab + (bb - ab) * n / d;
    return (uint32_t)(r << 16 | g << 8 | c);
}
static uint32_t add_clamp(uint32_t c, int dr, int dg, int db) {
    int r = (int)(c >> 16 & 0xFF) + dr, g = (int)(c >> 8 & 0xFF) + dg, b = (int)(c & 0xFF) + db;
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    if (r < 0) r = 0;     if (g < 0) g = 0;     if (b < 0) b = 0;
    return (uint32_t)(r << 16 | g << 8 | b);
}

/* Render the desktop background per the current wallpaper style + base colour.
 * dst is a packed W*H image (the BG buffer or, as a fallback, BB). */
static void render_wallpaper(uint32_t *dst) {
    if (wall_img && wall_img->px && wall_img->w > 0 && wall_img->h > 0) {
        int iw = wall_img->w, ih = wall_img->h;        /* stretch image to fill */
        for (int y = 0; y < H; y++) {
            const uint32_t *srow = &wall_img->px[(y * ih / H) * iw];
            for (int x = 0; x < W; x++) dst[x] = srow[x * iw / W] & 0x00FFFFFF;
            dst += W;
        }
        return;
    }
    uint32_t base   = g_settings.wall_color;
    uint32_t bottom = add_clamp(base, 16, 26, 46);     /* gradient foot   */
    uint32_t glow   = add_clamp(base, 34, 52, 92);     /* radial highlight*/
    uint32_t grid   = lerp(base, g_theme.accent, 26, 100);
    int gx = W / 2, gy = H / 3, gr = (W > H ? W : H) * 2 / 3;
    int style = g_settings.wall_style;

    for (int y = 0; y < H; y++) {
        uint32_t grad = lerp(base, bottom, y, H);
        for (int x = 0; x < W; x++) {
            uint32_t c;
            if (style == WALL_SOLID) {
                c = base;
            } else if (style == WALL_GRID) {
                c = ((x % 28) == 0 || (y % 28) == 0) ? grid : base;
            } else {
                int dx = x - gx, dy = y - gy;
                int dist = isqrt(dx * dx + dy * dy);
                uint32_t fld = (style == WALL_GLOW) ? base : grad;
                c = (dist < gr) ? lerp(glow, fld, dist, gr) : fld;
            }
            dst[x] = c;
        }
        dst += W;
    }
}

#define WALL_PREF "/home/.wallpaper"   /* remembers the image path across reboots */

/* Use the image at `path` as the desktop wallpaper (decoded once, then stretched
 * to fill). Persists the choice so it survives a reboot. */
void gui_set_wallpaper_image(const char *path) {
    if (wall_img) { image_free(wall_img); wall_img = 0; }
    wall_img_path[0] = 0;
    if (path && path[0]) {
        fs_node *n = fs_lookup(path);
        if (n && !n->is_dir && n->data && n->size) {
            wall_img = image_decode(n->data, n->size);
            if (wall_img) {
                strncpy(wall_img_path, path, sizeof(wall_img_path) - 1);
                wall_img_path[sizeof(wall_img_path) - 1] = 0;
            }
        }
    }
    fs_node *w = fs_lookup(WALL_PREF);
    if (!w) w = fs_create(WALL_PREF, 0);
    if (w) fs_write(w, wall_img_path, (uint32_t)strlen(wall_img_path));
    if (BG) render_wallpaper(BG);
    if (wall_img_path[0]) gui_toast("Wallpaper updated");
    dirty = 1;
}

void gui_clear_wallpaper_image(void) { gui_set_wallpaper_image(0); }

/* On boot, restore a previously chosen image wallpaper (if any). */
static void wallpaper_restore(void) {
    fs_node *n = fs_lookup(WALL_PREF);
    if (!n || n->is_dir || !n->data || !n->size) return;
    char path[FS_PATH_MAX];
    uint32_t len = n->size; if (len > sizeof(path) - 1) len = sizeof(path) - 1;
    memcpy(path, n->data, len); path[len] = 0;
    if (path[0]) gui_set_wallpaper_image(path);
}

/* ===========================================================================
 *  Window registry + stacking
 * ===========================================================================*/
static window_t wins[MAX_WIN];
static int      nwin;
static int      ztop;
static int      focus_id = -1;
static int      drag_id  = -1, drag_dx, drag_dy;
static int      title_last_id = -1; static uint64_t title_last_tick;  /* title double-click */
static int      resize_id = -1, resize_edge;    /* window being edge-resized + its edge mask */
static int      rs_x0, rs_y0, rs_w0, rs_h0, rs_mx, rs_my;  /* rect + mouse at resize start */
static int      client_drag_id = -1;            /* window receiving held-button drag */
static uint8_t  prev_btns;
static int      menu_open;
static int      vol_open;                        /* tray volume popup visible */
static int      net_open;                        /* tray network popup visible */
static int      clock_open;                       /* tray calendar flyout visible */
static int      cpu_load;

/* right-click context menu. ctx_mode 0 = taskbar/window (targets ctx_win,
 * pops upward); 1 = desktop (app shortcuts, pops downward from the cursor). */
static int      ctx_open, ctx_win, ctx_x, ctx_y, ctx_mode;
#define CTX_W   168
#define CTX_ITEM 30
#define CTX_N    2          /* window menu: Close + Pin */
#define CTX_DESK_N 3        /* desktop menu: 3 shortcuts */

int  gui_screen_w(void) { return W; }
int  gui_screen_h(void) { return H; }
int  gui_panel_w(void)  { return PW; }
int  gui_panel_h(void)  { return PH; }
int  gui_cpu_load(void) { return cpu_load; }

/* The pointer reports physical panel pixels; map it into the logical desktop so
 * every hit-test and the cursor stay aligned when the desktop is scaled. */
static int mlx(void) {
    int v = mouse_x() - out_x;
    if (out_w > 0) v = v * W / out_w;
    if (v < 0) v = 0; if (v >= W) v = W - 1;
    return v;
}
static int mly(void) {
    int v = mouse_y() - out_y;
    if (out_h > 0) v = v * H / out_h;
    if (v < 0) v = 0; if (v >= H) v = H - 1;
    return v;
}
void gui_request_redraw(void) { dirty = 1; }
int  gui_window_focused(window_t *win) { return focus_id >= 0 && &wins[focus_id] == win; }

window_t *gui_add_window(const char *title, int w, int h, uint32_t accent, int icon) {
    if (nwin >= MAX_WIN) return 0;
    window_t *win = &wins[nwin];
    memset(win, 0, sizeof(*win));
    strncpy(win->title, title, sizeof(win->title));
    win->w = w; win->h = h;
    win->min_w = 260; win->min_h = 160;
    win->accent = accent; win->icon = icon;
    /* cascade initial placement so windows don't perfectly overlap */
    win->x = 80 + nwin * 48;
    win->y = 60 + nwin * 40;
    win->z = ++ztop;
    nwin++;
    return win;
}

void gui_focus(window_t *win) {
    for (int i = 0; i < nwin; i++)
        if (&wins[i] == win) { win->z = ++ztop; focus_id = i; dirty = 1; return; }
}
void gui_open(window_t *win) {
    win->open = 1; win->minimized = 0;
    gui_focus(win);
}

/* Alt+Tab: cycle focus through open, non-minimised windows in a stable index
 * order starting from the current focus (dir > 0 forward, < 0 backward). */
static void gui_cycle_window(int dir) {
    int list[MAX_WIN], n = 0, cur = -1;
    for (int i = 0; i < nwin; i++)
        if (wins[i].open && !wins[i].minimized) { if (i == focus_id) cur = n; list[n++] = i; }
    if (n == 0) return;
    int start = cur < 0 ? 0 : cur;
    int nx = (start + (dir > 0 ? 1 : n - 1)) % n;
    gui_focus(&wins[list[nx]]);
}

/* Win+D: minimise every open window; press again to restore the ones we hid. */
static void gui_show_desktop(void) {
    static uint8_t hidden[MAX_WIN];
    int any_visible = 0;
    for (int i = 0; i < nwin; i++) if (wins[i].open && !wins[i].minimized) { any_visible = 1; break; }
    if (any_visible) {
        for (int i = 0; i < nwin; i++) {
            hidden[i] = (wins[i].open && !wins[i].minimized) ? 1 : 0;
            if (hidden[i]) wins[i].minimized = 1;
        }
    } else {                                   /* restore what we hid */
        for (int i = 0; i < nwin; i++) if (hidden[i] && wins[i].open) { wins[i].minimized = 0; hidden[i] = 0; }
    }
    dirty = 1;
}

/* Open (and focus) the first window whose title matches, if any. */
static void gui_open_by_title(const char *title) {
    for (int i = 0; i < nwin; i++)
        if (!strcmp(wins[i].title, title)) { gui_open(&wins[i]); return; }
}

/* topmost open, non-minimised window index, or -1 */
static int topmost(void) {
    int best = -1, bz = -1;
    for (int i = 0; i < nwin; i++)
        if (wins[i].open && !wins[i].minimized && wins[i].z > bz) { bz = wins[i].z; best = i; }
    return best;
}

/* ===========================================================================
 *  Icons (drawn vectorially; no asset files)
 * ===========================================================================*/
static void draw_bolt(int x, int y, int s, uint32_t c) {     /* lightning glyph */
    for (int i = 0; i < 7 * s; i++) g_fill(x + 4 * s - i / 3, y + i, 2 * s, s, c);
    g_fill(x - 2 * s, y + 6 * s, 5 * s, s, c);
    for (int i = 0; i < 7 * s; i++) g_fill(x + 1 * s - i / 3, y + 7 * s + i, 2 * s, s, c);
}
static void draw_icon(int id, int x, int y, int s, uint32_t c) {
    switch (id) {
    case ICON_TERMINAL:
        g_round(x, y, 16 * s, 13 * s, 2 * s, 0x101018, 255);
        g_rect(x, y, 16 * s, 13 * s, c);
        g_text(x + 2 * s, y + 3 * s, ">", c, s);
        g_fill(x + 7 * s, y + 8 * s, 5 * s, s, c);
        break;
    case ICON_CODE:                              /* IDE: code brackets </> */
        g_round(x, y, 16 * s, 14 * s, 3 * s, 0x101018, 255);
        g_rect(x, y, 16 * s, 14 * s, c);
        g_text(x + 2 * s, y + 3 * s, "<", c, s);
        g_text(x + 10 * s, y + 3 * s, ">", c, s);
        g_fill(x + 7 * s, y + 3 * s, s, 8 * s, c);       /* the slash (vertical hint) */
        g_fill(x + 8 * s, y + 3 * s, s, 8 * s, COL_ACCENT);
        break;
    case ICON_DOOM:                              /* a snarling demon face */
        g_round(x, y, 16 * s, 16 * s, 4 * s, 0x3A0A08, 255);
        g_round(x + s, y + s, 14 * s, 13 * s, 3 * s, 0x8A1A12, 255);
        g_fill(x + 3 * s,  y + 5 * s, 3 * s, 3 * s, c);          /* eyes */
        g_fill(x + 10 * s, y + 5 * s, 3 * s, 3 * s, c);
        g_fill(x + 6 * s,  y + 3 * s, 4 * s, 2 * s, 0x3A0A08);   /* angry brow */
        for (int t = 0; t < 5; t++)                             /* teeth */
            g_fill(x + (3 + t * 2) * s, y + 11 * s, s, 2 * s, 0xE8D2B0);
        break;
    case ICON_TASKMGR:
        g_fill(x + 1 * s, y + 7 * s, 2 * s, 5 * s, c);
        g_fill(x + 5 * s, y + 4 * s, 2 * s, 8 * s, c);
        g_fill(x + 9 * s,  y + 1 * s, 2 * s, 11 * s, c);
        g_fill(x + 13 * s, y + 5 * s, 2 * s, 7 * s, c);
        break;
    case ICON_START:
        draw_bolt(x + 7 * s, y + 1 * s, s, c);
        break;
    case ICON_SETTINGS: {                       /* gear */
        int cxx = x + 8 * s, cyy = y + 7 * s, r = 5 * s;
        g_fill(cxx - s, y + 1 * s, 2 * s, 12 * s, c);        /* N-S teeth   */
        g_fill(x + 2 * s, cyy - s, 12 * s, 2 * s, c);        /* E-W teeth   */
        g_fill(cxx - 4 * s, cyy - 4 * s, 8 * s, 2 * s, c);   /* diagonal hint */
        g_fill(cxx - 4 * s, cyy + 3 * s, 8 * s, 2 * s, c);
        g_round(cxx - r, cyy - r, 2 * r, 2 * r, r, c, 255);  /* body disc   */
        uint32_t hub = (c >> 1) & 0x7F7F7F;
        g_round(cxx - 2 * s, cyy - 2 * s, 4 * s, 4 * s, 2 * s, hub, 255); /* hub */
        break;
    }
    case ICON_BROWSER: {                        /* globe */
        int cxx = x + 8 * s, cyy = y + 7 * s, r = 6 * s;
        g_round(cxx - r, cyy - r, 2 * r, 2 * r, r, c, 255);     /* disc          */
        uint32_t in = (c >> 1) & 0x7F7F7F;
        g_vline(cxx, cyy - r, 2 * r, in);                       /* meridian      */
        g_hline(cxx - r, cyy, 2 * r, in);                       /* equator       */
        g_hline(cxx - r + s, cyy - 3 * s, 2 * (r - s), in);     /* latitude lines*/
        g_hline(cxx - r + s, cyy + 3 * s, 2 * (r - s), in);
        break;
    }
    case ICON_OLDBROWSER: {                     /* antique brass globe (NetSurf) */
        int cxx = x + 8 * s, cyy = y + 8 * s, r = 7 * s;
        g_round(cxx - r, cyy - r, 2 * r, 2 * r, r, 0xC8A867, 255); /* brass disc */
        uint32_t in = 0x6B4F2A;                                  /* engraved sepia */
        g_vline(cxx, cyy - r, 2 * r, in);                       /* meridian      */
        g_hline(cxx - r, cyy, 2 * r, in);                       /* equator       */
        g_hline(cxx - r + s, cyy - 3 * s, 2 * (r - s), in);     /* latitudes     */
        g_hline(cxx - r + s, cyy + 3 * s, 2 * (r - s), in);
        g_vline(cxx - 3 * s, cyy - r + 2 * s, 2 * r - 4 * s, in); /* meridians   */
        g_vline(cxx + 3 * s, cyy - r + 2 * s, 2 * r - 4 * s, in);
        (void)c;
        break;
    }
    case ICON_FOLDER: {                         /* manila folder (Windows-ish) */
        uint32_t body = 0xE7B24A, lid = 0xF3C766, lip = 0xC8932E;
        g_round(x, y + 2 * s, 8 * s, 4 * s, 2 * s, lid, 255);    /* back tab     */
        g_round(x, y + 4 * s, 16 * s, 9 * s, 2 * s, lip, 255);   /* rear panel   */
        g_round(x, y + 5 * s, 16 * s, 8 * s, 2 * s, body, 255);  /* front flap   */
        g_fill(x + 2 * s, y + 6 * s, 12 * s, s, lid);            /* highlight     */
        (void)c;
        break;
    }
    case ICON_FILE: {                           /* document page with folded corner */
        uint32_t pg = 0xF5F7FC, fold = 0xCAD6EE, ln = 0x9AA7C2;
        g_round(x + 2 * s, y, 11 * s, 14 * s, 2 * s, pg, 255);
        g_fill(x + 2 * s + 7 * s, y, 4 * s, 4 * s, fold);        /* dog-ear       */
        for (int i = 0; i < 4; i++)                              /* text lines    */
            g_fill(x + 4 * s, y + 6 * s + i * 2 * s, 7 * s, s, ln);
        (void)c;
        break;
    }
    case ICON_TRASH: {                          /* recycle bin */
        uint32_t metal = 0x9AA7BE, dark = 0x5E6A82;
        g_fill(x + 6 * s, y, 4 * s, s, metal);                   /* handle        */
        g_fill(x + 2 * s, y + s, 12 * s, 2 * s, metal);          /* lid           */
        g_round(x + 3 * s, y + 3 * s, 10 * s, 11 * s, 2 * s, metal, 255); /* body  */
        g_fill(x + 5 * s,  y + 5 * s, s, 7 * s, dark);           /* slots         */
        g_fill(x + 8 * s,  y + 5 * s, s, 7 * s, dark);
        g_fill(x + 11 * s, y + 5 * s, s, 7 * s, dark);
        (void)c;
        break;
    }
    case ICON_CALC: {                           /* pocket calculator */
        g_round(x, y, 16 * s, 14 * s, 2 * s, c, 255);                 /* body      */
        g_round(x + 2 * s, y + 2 * s, 12 * s, 3 * s, s, 0x0E0E16, 255); /* screen  */
        uint32_t key = (c >> 1) & 0x7F7F7F;
        for (int ky = 0; ky < 3; ky++)                               /* keypad    */
            for (int kx = 0; kx < 3; kx++)
                g_fill(x + (3 + kx * 4) * s, y + (7 + ky * 2) * s, 2 * s, s, key);
        break;
    }
    case ICON_MATRIX: {                          /* matrix rain: dotted columns */
        for (int cc = 0; cc < 4; cc++) {
            int gx = x + cc * 4 * s;
            for (int k = 0; k < 3; k++)
                g_fill(gx, y + ((cc + k) % 4) * 4 * s, 2 * s, 2 * s, c);
        }
        break;
    }
    case ICON_MEMORY: {                          /* memory: two cards */
        g_round(x,        y + 2 * s, 7 * s, 11 * s, 2 * s, c, 255);
        g_round(x + 9 * s, y,        7 * s, 11 * s, 2 * s, (c >> 1) & 0x7F7F7F, 255);
        break;
    }
    case ICON_COLOR: {                           /* color picker: swatches */
        g_round(x,        y,        7 * s, 7 * s, 2 * s, 0xE0556B, 255);
        g_round(x + 9 * s, y,        7 * s, 7 * s, 2 * s, 0x34C759, 255);
        g_round(x,        y + 9 * s, 7 * s, 7 * s, 2 * s, 0x4F8DF7, 255);
        g_round(x + 9 * s, y + 9 * s, 7 * s, 7 * s, 2 * s, 0xF6D32D, 255);
        (void)c;
        break;
    }
    case ICON_TTT: {                             /* tic-tac-toe: grid + X O */
        for (int i = 1; i < 3; i++) { g_fill(x + i * 5 * s, y, s, 15 * s, c); g_fill(x, y + i * 5 * s, 15 * s, s, c); }
        g_fill(x + s, y + s, 3 * s, s, c); g_fill(x + s, y + 3 * s, 3 * s, s, c); /* X-ish */
        g_round(x + 11 * s, y + 11 * s, 3 * s, 3 * s, s, c, 255);                  /* O-ish */
        break;
    }
    case ICON_LIFE: {                            /* life: glider */
        g_round(x + 5 * s, y + 1 * s, 3 * s, 3 * s, s, c, 255);
        g_round(x + 9 * s, y + 5 * s, 3 * s, 3 * s, s, c, 255);
        g_round(x + 1 * s, y + 9 * s, 3 * s, 3 * s, s, c, 255);
        g_round(x + 5 * s, y + 9 * s, 3 * s, 3 * s, s, c, 255);
        g_round(x + 9 * s, y + 9 * s, 3 * s, 3 * s, s, c, 255);
        break;
    }
    case ICON_SYSINFO: {                         /* info "i" in a disc */
        int cxx = x + 8 * s, cyy = y + 7 * s, rr = 7 * s;
        g_round(cxx - rr, cyy - rr, 2 * rr, 2 * rr, rr, c, 255);
        uint32_t in = 0x0E0E16;
        g_fill(cxx - s, cyy - 4 * s, 2 * s, 2 * s, in);          /* dot  */
        g_fill(cxx - s, cyy - s, 2 * s, 5 * s, in);             /* stem */
        break;
    }
    case ICON_STOPWATCH: {                       /* stopwatch */
        int cxx = x + 8 * s, cyy = y + 9 * s, rr = 6 * s;
        g_fill(cxx - 2 * s, y, 4 * s, 2 * s, c);                 /* top button */
        g_round(cxx - rr, cyy - rr, 2 * rr, 2 * rr, rr, c, 255); /* case       */
        uint32_t in = 0x0E0E16;
        g_round(cxx - rr + s, cyy - rr + s, 2 * (rr - s), 2 * (rr - s), rr - s, in, 255);
        g_fill(cxx - s / 2, cyy - 4 * s, s, 4 * s, c);           /* hand        */
        break;
    }
    case ICON_2048: {                           /* 2048: four tiles */
        uint32_t a = 0x4F8DF7, b = 0xF6D32D;
        g_round(x,        y,        7 * s, 7 * s, 2 * s, a, 255);
        g_round(x + 9 * s, y,        7 * s, 7 * s, 2 * s, b, 255);
        g_round(x,        y + 9 * s, 7 * s, 7 * s, 2 * s, b, 255);
        g_round(x + 9 * s, y + 9 * s, 7 * s, 7 * s, 2 * s, a, 255);
        (void)c;
        break;
    }
    case ICON_SNAKE: {                          /* snake: body segments + food */
        g_round(x + 1 * s, y + 9 * s, 4 * s, 4 * s, s, COL_GOOD, 255);
        g_round(x + 4 * s, y + 6 * s, 4 * s, 4 * s, s, COL_GOOD, 255);
        g_round(x + 7 * s, y + 6 * s, 4 * s, 4 * s, s, 0x6FE38A, 255);   /* head */
        g_round(x + 12 * s, y + 2 * s, 3 * s, 3 * s, s, COL_BAD, 255);   /* food */
        (void)c;
        break;
    }
    case ICON_MINES: {                          /* minesweeper: grid + a mine */
        g_round(x, y, 16 * s, 14 * s, 2 * s, COL_PANEL_3, 255);
        for (int i = 1; i < 4; i++) { g_vline(x + i * 4 * s, y, 14 * s, 0x12121A); g_hline(x, y + i * 4 * s - s, 16 * s, 0x12121A); }
        g_round(x + 9 * s, y + 8 * s, 5 * s, 5 * s, 2 * s, c, 255);  /* a mine */
        break;
    }
    case ICON_PAINT: {                          /* paint: palette + brush */
        g_round(x, y, 13 * s, 13 * s, 5 * s, 0xF2F2F5, 255);     /* palette disc */
        g_round(x + 2 * s, y + 2 * s, 2 * s, 2 * s, s, 0xE0556B, 255);
        g_round(x + 7 * s, y + 2 * s, 2 * s, 2 * s, s, 0x4FC3F7, 255);
        g_round(x + 2 * s, y + 7 * s, 2 * s, 2 * s, s, 0xF6D32D, 255);
        g_round(x + 7 * s, y + 7 * s, 2 * s, 2 * s, s, 0x34C759, 255);
        g_fill(x + 11 * s, y + 11 * s, 5 * s, 2 * s, c);         /* brush handle */
        break;
    }
    case ICON_PIANO: {                          /* piano keys */
        g_round(x, y + 2 * s, 16 * s, 11 * s, 2 * s, 0xF2F2F5, 255);   /* white keys */
        for (int i = 1; i < 5; i++) g_vline(x + i * 3 * s, y + 2 * s, 11 * s, 0x9AA7C2);
        for (int i = 0; i < 4; i++) g_fill(x + (2 + i * 3) * s, y + 2 * s, 2 * s, 7 * s, 0x101018); /* black */
        (void)c;
        break;
    }
    case ICON_CALENDAR: {                       /* calendar: page with header band */
        uint32_t pg = 0xF5F7FC, grid = 0x9AA7C2;
        g_round(x, y + s, 16 * s, 13 * s, 2 * s, pg, 255);
        g_round(x, y + s, 16 * s, 4 * s, 2 * s, c, 255);      /* red header  */
        g_fill(x + 3 * s, y, s, 3 * s, c);                    /* rings       */
        g_fill(x + 12 * s, y, s, 3 * s, c);
        for (int gy = 0; gy < 3; gy++)
            for (int gx = 0; gx < 4; gx++)
                g_fill(x + (2 + gx * 4) * s, y + (7 + gy * 2) * s, 2 * s, s, grid);
        break;
    }
    case ICON_NOTES: {                          /* notepad: page with a pencil */
        uint32_t pg = 0xF5F7FC, ln = 0x9AA7C2;
        g_round(x + s, y, 12 * s, 15 * s, 2 * s, pg, 255);
        for (int i = 0; i < 4; i++)
            g_fill(x + 3 * s, y + 3 * s + i * 3 * s, 8 * s, s, ln);
        g_fill(x + 10 * s, y + 9 * s, 5 * s, 2 * s, c);       /* pencil body */
        g_fill(x + 14 * s, y + 8 * s, 2 * s, 2 * s, 0xF3C766); /* pencil tip  */
        break;
    }
    case ICON_MUSIC: {                          /* eighth note */
        uint32_t note = 0xE85AB0;
        g_fill(x + 5 * s, y + 2 * s, 2 * s, 9 * s, note);            /* stem      */
        g_fill(x + 5 * s, y + 2 * s, 7 * s, 2 * s, note);            /* flag      */
        g_fill(x + 11 * s, y + 2 * s, 2 * s, 4 * s, note);
        g_round(x + s, y + 9 * s, 6 * s, 5 * s, 2 * s, note, 255);   /* note head */
        break;
    }
    case ICON_IMAGE: {                          /* photo: sky, sun, mountain    */
        uint32_t frame = 0xF5F7FC, sky = 0x5AB0FF, hill = 0x33C481, sun = 0xFFD54A;
        g_round(x, y + s, 16 * s, 13 * s, 2 * s, frame, 255);
        g_fill(x + 2 * s, y + 3 * s, 12 * s, 9 * s, sky);
        g_round(x + 9 * s, y + 4 * s, 3 * s, 3 * s, s, sun, 255);          /* sun  */
        for (int i = 0; i < 6; i++)                                        /* hill */
            g_fill(x + (2 + i) * s, y + (9 - (i < 3 ? i : 5 - i)) * s, s, (4 + (i < 3 ? i : 5 - i)) * s, hill);
        for (int i = 0; i < 6; i++)
            g_fill(x + (8 + i) * s, y + (8 - (i < 3 ? i : 5 - i)) * s, s, (5 + (i < 3 ? i : 5 - i)) * s, hill);
        break;
    }
    case ICON_SHEETS: {                         /* spreadsheet: green grid page */
        uint32_t pg = 0xF5F7FC, grid = 0x33C481;
        g_round(x + s, y, 14 * s, 16 * s, 2 * s, pg, 255);
        g_fill(x + s, y, 14 * s, 4 * s, grid);                /* header band   */
        for (int gx2 = 1; gx2 < 4; gx2++)
            g_fill(x + (s) + gx2 * 4 * s, y + 4 * s, s, 12 * s, grid);   /* cols */
        for (int gy2 = 1; gy2 < 4; gy2++)
            g_fill(x + s, y + 4 * s + gy2 * 3 * s, 14 * s, s, grid);     /* rows */
        break;
    }
    case ICON_CLOCK: {                          /* analog clock */
        int cxx = x + 8 * s, cyy = y + 7 * s, rr = 6 * s;
        g_round(cxx - rr, cyy - rr, 2 * rr, 2 * rr, rr, c, 255);
        uint32_t in = 0x0E0E16;
        g_round(cxx - rr + s, cyy - rr + s, 2 * (rr - s), 2 * (rr - s), rr - s, in, 255);
        g_fill(cxx - s / 2, cyy - 4 * s, s, 4 * s, c);          /* hour hand  */
        g_fill(cxx, cyy - s / 2, 4 * s, s, c);                  /* minute hand*/
        break;
    }
    case ICON_CONTACTS: {                       /* person: head + shoulders */
        uint32_t pc = 0xF5F7FC;
        g_round(x + 5 * s, y + s, 6 * s, 6 * s, 3 * s, pc, 255);          /* head */
        g_round(x + 2 * s, y + 8 * s, 12 * s, 7 * s, 3 * s, c, 255);      /* body */
        break;
    }
    case ICON_TASKS: {                          /* checklist: page with ticks */
        uint32_t pg = 0xF5F7FC, bx2 = 0x33C481;
        g_round(x + s, y, 14 * s, 16 * s, 2 * s, pg, 255);
        for (int i = 0; i < 3; i++) {
            int ry = y + (3 + i * 4) * s;
            g_round(x + 3 * s, ry, 3 * s, 3 * s, s, bx2, 255);   /* check box */
            g_fill(x + 7 * s, ry + s, 6 * s, s, 0x9AA7C2);       /* task line */
        }
        break;
    }
    default:
        g_round(x, y, 14 * s, 12 * s, 2 * s, c, 255);
        break;
    }
}

void gui_icon(int id, int x, int y, int scale, uint32_t color) { draw_icon(id, x, y, scale, color); }

/* ===========================================================================
 *  Window frame
 * ===========================================================================*/
/* caption button rects: slot 0 = close, 1 = maximise, 2 = minimise (from right) */
static void caption_btn(window_t *w, int slot, int *bx, int *by, int *bw, int *bh) {
    *bw = 46; *bh = TITLE_H;
    *bx = w->x + w->w - (slot + 1) * 46;
    *by = w->y;
}

static void draw_window(window_t *w) {
    int focused = gui_window_focused(w);
    int mxp = mlx(), myp = mly();

    /* soft drop shadow */
    g_round(w->x - 6, w->y - 4, w->w + 12, w->h + 14, RADIUS + 6, 0x000000, 28);
    g_round(w->x - 2, w->y - 1, w->w + 4,  w->h + 7,  RADIUS + 3, 0x000000, 45);

    /* body + title bar */
    g_round(w->x, w->y, w->w, w->h, RADIUS, COL_PANEL, 255);
    g_round_top(w->x, w->y, w->w, TITLE_H, RADIUS, focused ? COL_PANEL_2 : 0x202028);
    if (focused) g_fill(w->x, w->y, w->w, 2, w->accent);     /* accent strip */

    /* title icon + text */
    draw_icon(w->icon, w->x + 12, w->y + 9, 1, focused ? w->accent : COL_TEXT_DIM);
    g_text(w->x + 34, w->y + 12, w->title, focused ? COL_TEXT : COL_TEXT_DIM, 1);

    /* caption buttons */
    for (int slot = 0; slot < 3; slot++) {
        int bx, by, bw, bh; caption_btn(w, slot, &bx, &by, &bw, &bh);
        int hot = mxp >= bx && mxp < bx + bw && myp >= by && myp < by + bh;
        uint32_t hl = (slot == 0) ? COL_BAD : 0x3A3A48;
        if (hot) { if (slot == 0) g_round_top(bx, by, bw, bh, 0, hl); else g_fill(bx, by, bw, bh, hl); }
        uint32_t gc = (hot && slot == 0) ? 0xFFFFFF : COL_TEXT;
        int cx = bx + bw / 2, cy = by + bh / 2;
        if (slot == 0) {                                     /* close: X */
            for (int i = -4; i <= 4; i++) { px(cx + i, cy + i, gc); px(cx + i, cy - i, gc); }
        } else if (slot == 1) {                              /* maximise: box */
            g_rect(cx - 4, cy - 4, 9, 9, gc);
        } else {                                             /* minimise: dash */
            g_hline(cx - 4, cy + 3, 9, gc);
        }
    }

    /* client area drawn by the app, clipped to it */
    int cx = w->x, cy = w->y + TITLE_H, cw = w->w, ch = w->h - TITLE_H;
    g_fill(cx, cy, cw, ch, COL_PANEL);
    if (w->draw) { g_set_clip(cx, cy, cw, ch); w->draw(w, cx, cy, cw, ch); g_clear_clip(); }
}

/* ===========================================================================
 *  Taskbar + start menu
 * ===========================================================================*/
#define TB_BTN 44
/* The taskbar only carries apps that are open OR pinned -- launchers live on
 * the desktop now. Build that ordered list (window indices) each frame. */
static int taskbar_list(int *out) {
    int n = 0;
    for (int i = 0; i < nwin; i++)
        if (wins[i].open || wins[i].pinned) out[n++] = i;
    return n;
}
static void taskbar_layout(int *startx, int *y) {
    int idx[MAX_WIN]; int vis = taskbar_list(idx);
    int count = 1 + vis;                        /* start button + visible apps */
    int total = count * TB_BTN + (count - 1) * 8;
    *startx = (W - total) / 2;
    *y = H - TASKBAR_H + (TASKBAR_H - TB_BTN) / 2;
}
/* slot -1 = start button; 0..vis-1 = visible-list position */
static void taskbar_btn_rect(int slot, int *bx, int *by) {
    int sx, y; taskbar_layout(&sx, &y);
    *bx = sx + (slot + 1) * (TB_BTN + 8);
    *by = y;
}

static void draw_clock(void) {
    struct rtc_time t; rtc_now(&t);
    char hm[6];
    hm[0] = '0' + t.hour / 10; hm[1] = '0' + t.hour % 10; hm[2] = ':';
    hm[3] = '0' + t.min / 10;  hm[4] = '0' + t.min % 10;  hm[5] = 0;
    g_text(W - g_text_width(hm, 1) - 18, H - TASKBAR_H + 12, hm, COL_TEXT, 1);
    char md[6];
    md[0] = '0' + t.mon / 10; md[1] = '0' + t.mon % 10; md[2] = '/';
    md[3] = '0' + t.day / 10; md[4] = '0' + t.day % 10; md[5] = 0;
    g_text(W - g_text_width(md, 1) - 18, H - TASKBAR_H + 27, md, COL_TEXT_DIM, 1);
}

/* ---- tray volume control ------------------------------------------------ */
static void tray_vol_rect(int *bx, int *by) {
    *bx = W - 96;                                /* just left of the clock */
    *by = H - TASKBAR_H + (TASKBAR_H - 28) / 2;
}
static void vol_popup_rect(int *x, int *y, int *w, int *h) {
    *w = 188; *h = 56;
    int bx, by; tray_vol_rect(&bx, &by);
    *x = bx + 14 - *w / 2;
    if (*x + *w > W - 6) *x = W - 6 - *w;
    if (*x < 6) *x = 6;
    *y = H - TASKBAR_H - *h - 8;
}
/* speaker glyph + sound waves scaled by the current volume */
static void draw_speaker(int x, int y, int vol, uint32_t col) {
    g_fill(x, y + 5, 4, 6, col);                 /* speaker box */
    for (int i = 0; i < 5; i++)                  /* cone */
        g_fill(x + 4 + i, y + 5 - i, 1, 6 + 2 * i, col);
    if (vol > 0)  g_fill(x + 11, y + 6, 2, 4, col);          /* wave 1 */
    if (vol > 40) g_fill(x + 14, y + 4, 2, 8, col);          /* wave 2 */
    if (vol > 75) g_fill(x + 17, y + 2, 2, 12, col);         /* wave 3 */
    if (vol == 0) { g_fill(x + 12, y + 4, 8, 2, 0xFF6B6B); } /* mute slash hint */
}
static void draw_volume_tray(void) {
    int bx, by; tray_vol_rect(&bx, &by);
    int mxp = mlx(), myp = mly();
    int hot = mxp >= bx && mxp < bx + 28 && myp >= by && myp < by + 28;
    if (vol_open || hot) g_round(bx, by, 28, 28, 7, 0x2A2A38, 255);
    draw_speaker(bx + 5, by + 7, audio_volume(), COL_TEXT);

    if (!vol_open) return;
    int px, py, pw, ph; vol_popup_rect(&px, &py, &pw, &ph);
    g_round(px - 2, py - 2, pw + 4, ph + 4, 12, 0x000000, 70);
    g_round(px, py, pw, ph, 10, 0x16161E, 250);
    int vol = audio_volume();
    draw_speaker(px + 12, py + ph / 2 - 8, vol, COL_TEXT);
    /* slider track */
    int tx = px + 38, tw = pw - 38 - 44, ty = py + ph / 2 - 3;
    g_round(tx, ty, tw, 6, 3, 0x33333F, 255);
    g_round(tx, ty, tw * vol / 100, 6, 3, COL_ACCENT, 255);
    g_round(tx + tw * vol / 100 - 4, ty - 4, 9, 14, 4, COL_TEXT, 255);  /* knob */
    char pct[5]; int n = 0;                       /* "NN%" */
    if (vol >= 100) { pct[n++]='1'; pct[n++]='0'; pct[n++]='0'; }
    else { if (vol >= 10) pct[n++] = '0' + vol / 10; pct[n++] = '0' + vol % 10; }
    pct[n++] = '%'; pct[n] = 0;
    g_text(px + pw - 38, py + ph / 2 - 4, pct, COL_TEXT, 1);
}
/* click inside the volume popup -> set level from the slider, or toggle mute on
 * the speaker. Returns 1 if the click was consumed by the popup. */
static int volume_popup_click(int x, int y) {
    int px, py, pw, ph; vol_popup_rect(&px, &py, &pw, &ph);
    if (x < px || x >= px + pw || y < py || y >= py + ph) return 0;
    if (x < px + 32) { audio_set_volume(audio_volume() > 0 ? 0 : 80); return 1; }  /* mute toggle */
    int tx = px + 38, tw = pw - 38 - 44;
    int v = (x - tx) * 100 / (tw > 0 ? tw : 1);
    if (v < 0) v = 0; if (v > 100) v = 100;
    audio_set_volume(v);
    return 1;
}

/* ---- tray network indicator --------------------------------------------- */
static int net_is_up(void) {
    struct netif *n = netif_default();
    return (n && n->link_up && net_ip != 0);
}
static void tray_net_rect(int *bx, int *by) {
    *bx = W - 132;                               /* left of the volume button */
    *by = H - TASKBAR_H + (TASKBAR_H - 28) / 2;
}
/* wifi-style arcs; bright when connected, dim+slash when not */
static void draw_net_glyph(int x, int y, int up, uint32_t col) {
    uint32_t c = up ? col : COL_TEXT_DIM;
    g_fill(x + 8, y + 13, 3, 3, c);                          /* base dot */
    g_fill(x + 4, y + 9, 11, 2, c);                          /* arc 1 */
    g_fill(x + 2, y + 5, 15, 2, c);                          /* arc 2 */
    if (!up) { g_fill(x + 3, y + 3, 13, 2, 0xFF6B6B); }      /* offline slash */
}
static void ipv4_str(uint32_t ip, char *out) {               /* host order -> "a.b.c.d" */
    int n = 0;
    for (int s = 24; s >= 0; s -= 8) {
        int v = (ip >> s) & 0xFF, d = 0; char t[4];
        if (v == 0) t[d++] = '0'; else { int x = v; char r[4]; int ri = 0; while (x) { r[ri++] = '0' + x % 10; x /= 10; } while (ri) t[d++] = r[--ri]; }
        for (int i = 0; i < d; i++) out[n++] = t[i];
        if (s) out[n++] = '.';
    }
    out[n] = 0;
}
static void draw_net_tray(void) {
    int bx, by; tray_net_rect(&bx, &by);
    int mxp = mlx(), myp = mly();
    int hot = mxp >= bx && mxp < bx + 28 && myp >= by && myp < by + 28;
    int up = net_is_up();
    if (net_open || hot) g_round(bx, by, 28, 28, 7, 0x2A2A38, 255);
    draw_net_glyph(bx + 4, by + 5, up, COL_ACCENT);

    if (!net_open) return;
    int pw = 200, ph = 56, px = bx + 14 - pw / 2, py = H - TASKBAR_H - ph - 8;
    if (px + pw > W - 6) px = W - 6 - pw;
    if (px < 6) px = 6;
    g_round(px - 2, py - 2, pw + 4, ph + 4, 12, 0x000000, 70);
    g_round(px, py, pw, ph, 10, 0x16161E, 250);
    draw_net_glyph(px + 10, py + ph / 2 - 9, up, COL_ACCENT);
    if (up) {
        char ip[16]; ipv4_str(net_ip, ip);
        g_text(px + 40, py + 12, "Connected", COL_GOOD, 1);
        g_text(px + 40, py + 30, ip, COL_TEXT_DIM, 1);
    } else {
        g_text(px + 40, py + ph / 2 - 4, "No network", COL_TEXT_DIM, 1);
    }
}

/* ---- clock calendar flyout ---------------------------------------------- */
static int cal_is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }
static int cal_days_in(int y, int m) {
    static const int d[13] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
    return (m == 2 && cal_is_leap(y)) ? 29 : d[m];
}
static int cal_weekday(int y, int m, int d) {       /* 0=Sun..6=Sat */
    if (m < 3) { m += 12; y--; }
    int K = y % 100, J = y / 100;
    int h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    return (h + 6) % 7;
}
static void clock_rect(int *bx, int *by, int *bw, int *bh) {
    *bw = 72; *bx = W - *bw; *by = H - TASKBAR_H; *bh = TASKBAR_H;
}
static void draw_clock_flyout(void) {
    if (!clock_open) return;
    static const char *MON[13] = { "", "January","February","March","April","May","June",
        "July","August","September","October","November","December" };
    struct rtc_time t; rtc_now(&t);
    int yr = t.year ? t.year : 2026, mo = (t.mon >= 1 && t.mon <= 12) ? t.mon : 1, today = t.day;
    int pw = 210, ph = 196, px = W - pw - 6, py = H - TASKBAR_H - ph - 8;
    g_round(px - 2, py - 2, pw + 4, ph + 4, 12, 0x000000, 70);
    g_round(px, py, pw, ph, 10, 0x16161E, 250);
    /* header: Month Year */
    char hdr[24]; int n = 0;
    for (const char *p = MON[mo]; *p; p++) hdr[n++] = *p;
    hdr[n++] = ' ';
    int y4 = yr; char yb[6]; int yi = 0; while (y4) { yb[yi++] = '0' + y4 % 10; y4 /= 10; }
    while (yi) hdr[n++] = yb[--yi];
    hdr[n] = 0;
    g_text(px + 14, py + 12, hdr, COL_TEXT, 1);
    /* weekday header */
    static const char *WD[7] = { "S","M","T","W","T","F","S" };
    int cw = (pw - 20) / 7, gx = px + 12, gy = py + 36;
    for (int i = 0; i < 7; i++)
        g_text(gx + i * cw + cw / 2 - 3, gy, WD[i], (i == 0 || i == 6) ? COL_ACCENT : COL_TEXT_DIM, 1);
    /* day grid */
    int first = cal_weekday(yr, mo, 1), dim = cal_days_in(yr, mo);
    int row = 0;
    for (int d = 1; d <= dim; d++) {
        int slot = first + d - 1; row = slot / 7; int col = slot % 7;
        int cxp = gx + col * cw, cyp = gy + 16 + row * 22;
        if (d == today) g_round(cxp + cw / 2 - 11, cyp - 3, 22, 20, 5, COL_ACCENT, 255);
        char db[3]; int dn = 0; if (d >= 10) db[dn++] = '0' + d / 10; db[dn++] = '0' + d % 10; db[dn] = 0;
        int dw = g_text_width(db, 1);
        g_text(cxp + cw / 2 - dw / 2, cyp, db, d == today ? 0xFFFFFF : COL_TEXT, 1);
    }
}

static void draw_taskbar(void) {
    g_blend(0, H - TASKBAR_H, W, TASKBAR_H, 0x0E0E16, 224);
    g_hline(0, H - TASKBAR_H, W, 0x33333F);

    int top = topmost();
    /* start button */
    int bx, by; taskbar_btn_rect(-1, &bx, &by);
    int mxp = mlx(), myp = mly();
    int hot = mxp >= bx && mxp < bx + TB_BTN && myp >= by && myp < by + TB_BTN;
    if (menu_open || hot) g_round(bx, by, TB_BTN, TB_BTN, 8, 0x2A2A38, 255);
    draw_icon(ICON_START, bx + 8, by + 8, 1, COL_ACCENT);

    /* one button per open-or-pinned window */
    int idx[MAX_WIN]; int vis = taskbar_list(idx);
    for (int s = 0; s < vis; s++) {
        int i = idx[s];
        taskbar_btn_rect(s, &bx, &by);
        window_t *w = &wins[i];
        int active = (i == top) && w->open && !w->minimized;
        int h2 = mxp >= bx && mxp < bx + TB_BTN && myp >= by && myp < by + TB_BTN;
        if (active)      g_round(bx, by, TB_BTN, TB_BTN, 8, 0x33334A, 255);
        else if (h2)     g_round(bx, by, TB_BTN, TB_BTN, 8, 0x262632, 255);
        draw_icon(w->icon, bx + 8, by + 8, 1, w->open ? w->accent : COL_TEXT_DIM);
        if (w->open) {   /* running indicator underline */
            int iw = active ? 18 : 8;
            g_round(bx + TB_BTN / 2 - iw / 2, by + TB_BTN - 3, iw, 3, 1, active ? w->accent : COL_TEXT_DIM, 255);
        } else if (w->pinned) {   /* pinned-but-closed: small dot */
            g_round(bx + TB_BTN / 2 - 1, by + TB_BTN - 3, 3, 3, 1, COL_TEXT_DIM, 200);
        }
    }
    draw_net_tray();
    draw_volume_tray();
    draw_clock();
    draw_clock_flyout();
}

/* ---- right-click context menu (window or desktop) ----------------------- */
static int ctx_count(void) { return ctx_mode == 1 ? CTX_DESK_N : CTX_N; }
static const char *ctx_label(int item) {
    if (ctx_mode == 1) {                        /* desktop shortcuts */
        switch (item) {
            case 0:  return "Display Settings";
            case 1:  return "File Explorer";
            default: return "Open Terminal";
        }
    }
    if (item == 0) return "Close window";
    return (ctx_win >= 0 && wins[ctx_win].pinned) ? "Unpin from taskbar"
                                                   : "Pin to taskbar";
}
static void ctx_menu_rect(int *x, int *y, int *w, int *h) {
    *w = CTX_W; *h = CTX_ITEM * ctx_count() + 8;
    int mx = ctx_x;
    int my = ctx_mode == 1 ? ctx_y : ctx_y - *h;  /* desktop: down; taskbar: up */
    if (mx + *w > W - 6) mx = W - 6 - *w;
    if (mx < 6) mx = 6;
    if (my + *h > H - 6) my = H - 6 - *h;
    if (my < 6) my = 6;
    *x = mx; *y = my;
}
static void draw_context_menu(void) {
    if (!ctx_open) return;
    int x, y, w, h; ctx_menu_rect(&x, &y, &w, &h);
    g_round(x - 2, y - 2, w + 4, h + 4, 12, 0x000000, 70);
    g_round(x, y, w, h, 10, 0x16161E, 250);
    int mxp = mlx(), myp = mly();
    for (int it = 0; it < ctx_count(); it++) {
        int iy = y + 4 + it * CTX_ITEM;
        int hot = mxp >= x && mxp < x + w && myp >= iy && myp < iy + CTX_ITEM;
        if (hot) g_round(x + 4, iy, w - 8, CTX_ITEM, 7, 0x2A2A3A, 255);
        uint32_t col = (ctx_mode == 0 && it == 0) ? COL_BAD : COL_TEXT;  /* "Close" in red */
        g_text(x + 14, iy + 9, ctx_label(it), hot ? 0xFFFFFF : col, 1);
    }
}
/* returns clicked item index, -1 (between items), or -2 (outside) */
static int ctx_menu_hit(int x, int y) {
    int mx, my, w, h; ctx_menu_rect(&mx, &my, &w, &h);
    if (x < mx || x >= mx + w || y < my || y >= my + h) return -2;   /* outside */
    for (int it = 0; it < ctx_count(); it++) {
        int iy = my + 4 + it * CTX_ITEM;
        if (y >= iy && y < iy + CTX_ITEM) return it;
    }
    return -1;
}

/* ---- start menu: a modern icon grid (scales as apps are added) ---------- */
#define SM_COLS 4
#define SM_TW   86
#define SM_TH   80
#define SM_PAD  16
#define SM_HEAD 86      /* title + subtitle + search box */
#define SM_FOOT 40

/* Type-to-search: the Start menu filters its app grid by a query the user types
 * while it's open. The filtered window indices are rebuilt on demand. */
static char sm_query[32];
static int  sm_qlen;
static int  sm_filt[MAX_WIN];
static int  sm_nfilt;

/* case-insensitive substring test */
static int sm_ci_contains(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j]) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            j++;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}
static void sm_build_filter(void) {
    sm_nfilt = 0;
    for (int i = 0; i < nwin; i++)
        if (sm_ci_contains(wins[i].title, sm_query)) sm_filt[sm_nfilt++] = i;
}

static void start_menu_rect(int *ox, int *oy, int *mw, int *mh) {
    sm_build_filter();
    int n = sm_nfilt > 0 ? sm_nfilt : 1;
    int rows = (n + SM_COLS - 1) / SM_COLS; if (rows < 1) rows = 1;
    *mw = SM_PAD * 2 + SM_COLS * SM_TW;
    *mh = SM_HEAD + rows * SM_TH + SM_PAD - 8 + SM_FOOT;
    int sx, sy; taskbar_layout(&sx, &sy);
    int x = sx; if (x + *mw > W - 8) x = W - 8 - *mw; if (x < 8) x = 8;
    int y = H - TASKBAR_H - *mh - 10; if (y < 8) y = 8;
    *ox = x; *oy = y;
}
/* slot = grid position (0-based) of a tile within the filtered list */
static void start_menu_tile(int slot, int ox, int oy, int *tx, int *ty) {
    *tx = ox + SM_PAD + (slot % SM_COLS) * SM_TW;
    *ty = oy + SM_HEAD + (slot / SM_COLS) * SM_TH;
}

/* sign-out button rect inside the start menu footer */
static void signout_rect(int x, int y, int mw, int mh, int *bx, int *by, int *bw, int *bh) {
    *bw = 80; *bh = 24;
    *bx = x + mw - *bw - 14;
    *by = y + mh - SM_FOOT + 8;
}

static void draw_start_menu(void) {
    if (!menu_open) return;
    int x, y, mw, mh; start_menu_rect(&x, &y, &mw, &mh);
    g_round(x, y, mw, mh, 14, 0x16161E, 248);
    g_fill(x, y, mw, 3, COL_ACCENT);
    g_text(x + 18, y + 12, "BoltOS", COL_TEXT, 2);

    /* search box */
    int qbx = x + 16, qby = y + 40, qbw = mw - 32, qbh = 26;
    g_round(qbx, qby, qbw, qbh, 6, 0x0C0C12, 255);
    if (sm_qlen) {
        g_text(qbx + 10, qby + 6, sm_query, COL_TEXT, 1);
        if ((pit_ticks() / 500) % 2 == 0) {
            int cxp = qbx + 10 + g_text_width(sm_query, 1);
            if (cxp < qbx + qbw - 4) g_fill(cxp, qby + 5, 2, qbh - 10, COL_ACCENT);
        }
    } else {
        g_text(qbx + 10, qby + 6, "Search apps...", COL_TEXT_DIM, 1);
    }

    int mxp = mlx(), myp = mly();
    sm_build_filter();
    if (sm_nfilt == 0)
        g_text(x + 18, y + SM_HEAD + 10, "No matching apps", COL_TEXT_DIM, 1);
    for (int k = 0; k < sm_nfilt; k++) {
        int i = sm_filt[k];
        int tx, ty; start_menu_tile(k, x, y, &tx, &ty);
        int hot = mxp >= tx && mxp < tx + SM_TW && myp >= ty && myp < ty + SM_TH;
        if (hot) g_round(tx + 3, ty + 2, SM_TW - 6, SM_TH - 6, 10, 0x2A2A3A, 255);
        draw_icon(wins[i].icon, tx + SM_TW / 2 - 16, ty + 12, 2, wins[i].accent);
        /* centred label, truncated to the tile width */
        char lbl[24]; strncpy(lbl, wins[i].title, sizeof(lbl));
        while (strlen(lbl) > 1 && g_text_width(lbl, 1) > SM_TW - 12) lbl[strlen(lbl) - 1] = 0;
        int lw = g_text_width(lbl, 1);
        g_text(tx + (SM_TW - lw) / 2, ty + 52, lbl, hot ? COL_TEXT : COL_TEXT_DIM, 1);
    }

    /* footer: current user + a sign-out button */
    int fy = y + mh - SM_FOOT;
    g_hline(x + 12, fy, mw - 24, 0x2A2A3A);
    const char *who = users_current();
    char line[40]; line[0] = 0;
    kstrlcat(line, "Signed in: ", sizeof(line));
    kstrlcat(line, who[0] ? who : "guest", sizeof(line));
    g_text(x + 18, fy + 14, line, COL_TEXT_DIM, 1);
    int sx, sy, sw, sh; signout_rect(x, y, mw, mh, &sx, &sy, &sw, &sh);
    int sh_hot = mxp >= sx && mxp < sx + sw && myp >= sy && myp < sy + sh;
    g_round(sx, sy, sw, sh, 6, sh_hot ? 0xC0392B : 0x3A2A2A, 255);
    g_text(sx + 10, sy + 6, "Sign out", COL_TEXT, 1);
}

/* ---- window edge resize: hit-testing ----------------------------------- *
 * Which edges of window w is (x,y) within the grab band of? Returns an RS_*
 * bitmask (a corner sets two bits). The band straddles each edge by RS_BORDER
 * px, inside and out, so the very rim grabs like every other OS. Maximised
 * windows don't resize. */
static int resize_zone(window_t *w, int x, int y) {
    if (w->maximized) return 0;
    int x0 = w->x, y0 = w->y, x1 = w->x + w->w, y1 = w->y + w->h;
    if (x < x0 - RS_BORDER || x >= x1 + RS_BORDER ||
        y < y0 - RS_BORDER || y >= y1 + RS_BORDER) return 0;
    int e = 0;
    if (iabs(x - x0) <= RS_BORDER) e |= RS_L;
    if (iabs(x - x1) <= RS_BORDER) e |= RS_R;
    if (iabs(y - y0) <= RS_BORDER) e |= RS_T;
    if (iabs(y - y1) <= RS_BORDER) e |= RS_B;
    return e;
}
static int over_caption_btn(window_t *w, int x, int y) {
    for (int s = 0; s < 3; s++) {
        int bx, by, bw, bh; caption_btn(w, s, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) return 1;
    }
    return 0;
}
/* Resize edge mask for the cursor at (x,y): the topmost window whose grab band
 * the point lands in wins. Returns 0 (and sets *out_i to that window, or -1) if
 * the point is over a window interior / caption button / nothing -- i.e. the
 * normal arrow cursor applies. */
static int cursor_resize(int x, int y, int *out_i) {
    int best = -1, bz = -1;
    for (int i = 0; i < nwin; i++) {
        window_t *w = &wins[i];
        if (!w->open || w->minimized || w->maximized) continue;
        if (x < w->x - RS_BORDER || x >= w->x + w->w + RS_BORDER ||
            y < w->y - RS_BORDER || y >= w->y + w->h + RS_BORDER) continue;
        if (w->z > bz) { bz = w->z; best = i; }
    }
    *out_i = best;
    if (best < 0) return 0;
    window_t *w = &wins[best];
    int e = resize_zone(w, x, y);
    if (e && over_caption_btn(w, x, y)) e = 0;       /* buttons win over the rim */
    return e;
}

/* ---- mouse cursor (arrow + resize arrows) ------------------------------ */
static const char *CURSOR[19] = {
    "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ",
    "X....X     ", "X.....X    ", "X......X   ", "X.......X  ", "X........X ",
    "X.....XXXXX", "X..X..X    ", "X.X X..X   ", "XX  X..X   ", "X    X..X  ",
    "     X..X  ", "      X..X ", "      X..X ", "       XX  ",
};
/* A double-headed resize arrow, drawn centred on the pointer. kind: 1 = <->,
 * 2 = vertical, 3 = NW-SE diagonal, 4 = NE-SW diagonal. The shape is defined as
 * a white "ink" predicate; any empty pixel touching ink becomes the black
 * outline, so it reads on any background like the plain arrow does. */
static int resize_ink(int kind, int dx, int dy) {
    switch (kind) {
    case 1:
        if (iabs(dx) <= 5 && iabs(dy) <= 1) return 1;
        if (dx <= -4 && dx >= -7 && iabs(dy) <= dx + 7) return 1;
        if (dx >=  4 && dx <=  7 && iabs(dy) <= 7 - dx) return 1;
        return 0;
    case 2:
        if (iabs(dy) <= 5 && iabs(dx) <= 1) return 1;
        if (dy <= -4 && dy >= -7 && iabs(dx) <= dy + 7) return 1;
        if (dy >=  4 && dy <=  7 && iabs(dx) <= 7 - dy) return 1;
        return 0;
    case 3:
        if (iabs(dx - dy) <= 1 && iabs(dx) <= 6 && iabs(dy) <= 6) return 1;
        if (dx + dy <= -8 && dx >= -7 && dy >= -7 && dx <= 0 && dy <= 0) return 1;
        if (dx + dy >=  8 && dx <=  7 && dy <=  7 && dx >= 0 && dy >= 0) return 1;
        return 0;
    default:
        if (iabs(dx + dy) <= 1 && iabs(dx) <= 6 && iabs(dy) <= 6) return 1;
        if (dy - dx <= -8 && dx >= 0 && dx <= 7 && dy >= -7 && dy <= 0) return 1;
        if (dy - dx >=  8 && dx >= -7 && dx <= 0 && dy >= 0 && dy <= 7) return 1;
        return 0;
    }
}
static void draw_resize_cursor(int cx, int cy, int kind) {
    for (int dy = -8; dy <= 8; dy++)
        for (int dx = -8; dx <= 8; dx++) {
            if (resize_ink(kind, dx, dy)) { px(cx + dx, cy + dy, 0xFFFFFF); continue; }
            int o = 0;
            for (int ny = -1; ny <= 1 && !o; ny++)
                for (int nx = -1; nx <= 1; nx++)
                    if (resize_ink(kind, dx + nx, dy + ny)) { o = 1; break; }
            if (o) px(cx + dx, cy + dy, 0x000000);
        }
}
static void draw_cursor(void) {
    g_clear_clip();
    int ox = mlx(), oy = mly();
    int wi, e = (resize_id >= 0) ? resize_edge : cursor_resize(ox, oy, &wi);
    if (e) {                                   /* over (or dragging) a resize edge */
        int kind;
        if ((e & (RS_L | RS_R)) && (e & (RS_T | RS_B)))
            kind = (((e & RS_L) && (e & RS_T)) || ((e & RS_R) && (e & RS_B))) ? 3 : 4;
        else if (e & (RS_L | RS_R)) kind = 1;
        else                        kind = 2;
        draw_resize_cursor(ox, oy, kind);
        return;
    }
    for (int j = 0; j < 19; j++)
        for (int i = 0; CURSOR[j][i]; i++) {
            char c = CURSOR[j][i];
            if (c == 'X') px(ox + i, oy + j, 0x000000);
            else if (c == '.') px(ox + i, oy + j, 0xFFFFFF);
        }
}

/* ===========================================================================
 *  Desktop icons + drag-and-drop
 * ===========================================================================*/
#define MAX_DESKICON 48
/* A desktop shortcut is either an app launcher (win != 0, double-click opens the
 * window) or a file/folder shortcut (node != 0, opened via the explorer). */
typedef struct { int used, x, y, icon; char label[40]; fs_node *node; window_t *win; } deskicon_t;
static deskicon_t dicons[MAX_DESKICON];
static int      desk_sel = -1;
static int      desk_drag = -1, desk_dx, desk_dy;
static uint64_t desk_last_tick;
static int      desk_last_idx = -1;

/* drag payload (a file/folder being dragged out of a window onto the desktop) */
static fs_node *dnd_node;
static char     dnd_label[40];
static int      dnd_icon, dnd_armed, dnd_active, dnd_px, dnd_py, press_x, press_y;

static int deskicon_find(fs_node *n) {
    for (int i = 0; i < MAX_DESKICON; i++) if (dicons[i].used && dicons[i].node == n) return i;
    return -1;
}
static void deskicon_autopos(int *ox, int *oy) {
    int used = 0;
    for (int i = 0; i < MAX_DESKICON; i++) if (dicons[i].used) used++;
    int per_col = (H - TASKBAR_H - 16) / DI_H; if (per_col < 1) per_col = 1;
    *ox = 16 + (used / per_col) * DI_W;
    *oy = 16 + (used % per_col) * DI_H;
}
static void deskicon_clamp(deskicon_t *d) {
    if (d->x < 0) d->x = 0; if (d->x > W - DI_W) d->x = W - DI_W;
    if (d->y < 0) d->y = 0; if (d->y > H - TASKBAR_H - DI_H) d->y = H - TASKBAR_H - DI_H;
}

void gui_desktop_add(fs_node *node, const char *label, int icon) {
    if (!node || deskicon_find(node) >= 0) return;
    for (int i = 0; i < MAX_DESKICON; i++) {
        if (dicons[i].used) continue;
        dicons[i].used = 1; dicons[i].node = node; dicons[i].icon = icon;
        strncpy(dicons[i].label, label ? label : "item", sizeof(dicons[i].label));
        deskicon_autopos(&dicons[i].x, &dicons[i].y);
        dirty = 1;
        return;
    }
}

/* Seed an app launcher onto the desktop (double-click opens the window). */
void gui_desktop_add_app(window_t *win, const char *label, int icon) {
    if (!win) return;
    for (int i = 0; i < MAX_DESKICON; i++) {
        if (dicons[i].used) continue;
        dicons[i].used = 1; dicons[i].win = win; dicons[i].icon = icon;
        strncpy(dicons[i].label, label ? label : "app", sizeof(dicons[i].label));
        deskicon_autopos(&dicons[i].x, &dicons[i].y);
        dirty = 1;
        return;
    }
}

/* place (or move) a dropped item's shortcut centred on the drop point */
static void deskicon_drop(fs_node *n, const char *label, int icon, int x, int y) {
    int idx = deskicon_find(n);
    if (idx < 0) {
        for (int i = 0; i < MAX_DESKICON && idx < 0; i++) if (!dicons[i].used) idx = i;
        if (idx < 0) return;
        dicons[idx].used = 1; dicons[idx].node = n; dicons[idx].icon = icon;
        strncpy(dicons[idx].label, label ? label : "item", sizeof(dicons[idx].label));
    }
    dicons[idx].x = x - DI_W / 2; dicons[idx].y = y - 28;
    deskicon_clamp(&dicons[idx]);
    desk_sel = idx; dirty = 1;
}

void gui_begin_item_drag(fs_node *node, const char *label, int icon) {
    if (!node) return;
    dnd_node = node; dnd_icon = icon; dnd_armed = 1; dnd_active = 0;
    strncpy(dnd_label, label ? label : "item", sizeof(dnd_label));
    press_x = mlx(); press_y = mly();
}

static void deskicon_label(const char *src, char *out, int cap) {
    int maxc = (DI_W - 6) / 8; if (maxc > cap - 1) maxc = cap - 1;
    int len = (int)strlen(src);
    if (len <= maxc) { strncpy(out, src, cap); return; }
    int k = 0; for (; k < maxc - 2; k++) out[k] = src[k];
    out[k++] = '.'; out[k++] = '.'; out[k] = 0;
}

static void draw_desktop_icons(void) {
    int mxp = mlx(), myp = mly();
    for (int i = 0; i < MAX_DESKICON; i++) {
        if (!dicons[i].used) continue;
        deskicon_t *d = &dicons[i];
        int hot = mxp >= d->x && mxp < d->x + DI_W && myp >= d->y && myp < d->y + DI_H;
        if (i == desk_sel) g_round(d->x, d->y, DI_W, DI_H, 8, COL_ACCENT, 70);
        else if (hot)      g_round(d->x, d->y, DI_W, DI_H, 8, 0xFFFFFF, 26);
        int s = 3, iw = 16 * s;
        uint32_t ic = d->win ? d->win->accent : COL_TEXT;
        draw_icon(d->icon, d->x + (DI_W - iw) / 2, d->y + 8, s, ic);
        char lb[18]; deskicon_label(d->label, lb, sizeof(lb));
        int tw = g_text_width(lb, 1), tx = d->x + (DI_W - tw) / 2, ty = d->y + 8 + iw + 6;
        g_text(tx + 1, ty + 1, lb, 0x000000, 1);        /* drop shadow for legibility */
        g_text(tx, ty, lb, 0xFFFFFF, 1);
    }
}

static void draw_drag_ghost(void) {
    int s = 2, iw = 16 * s, x = dnd_px - iw / 2, y = dnd_py - iw / 2;
    g_round(x - 6, y - 4, iw + 12, iw + 24, 8, 0x000000, 70);
    draw_icon(dnd_icon, x, y, s, COL_TEXT);
    int tw = g_text_width(dnd_label, 1);
    g_text(dnd_px - tw / 2, y + iw + 4, dnd_label, 0xFFFFFF, 1);
}

static int deskicon_hit(int x, int y) {
    for (int i = MAX_DESKICON - 1; i >= 0; i--) {
        if (!dicons[i].used) continue;
        deskicon_t *d = &dicons[i];
        if (x >= d->x && x < d->x + DI_W && y >= d->y && y < d->y + DI_H) return i;
    }
    return -1;
}

static void deskicon_open(int i) {
    if (i < 0 || i >= MAX_DESKICON || !dicons[i].used) return;
    if (dicons[i].win)       gui_open(dicons[i].win);
    else if (dicons[i].node) files_open_node(dicons[i].node);
}

/* drop point over the wallpaper (not a window, taskbar or menu) -> make shortcut */
static void dnd_drop(int x, int y) {
    if (!dnd_node) return;
    if (y >= H - TASKBAR_H) return;
    for (int i = 0; i < nwin; i++) {
        window_t *w = &wins[i];
        if (w->open && !w->minimized && x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) return;
    }
    deskicon_drop(dnd_node, dnd_label, dnd_icon, x, y);
}

/* ===========================================================================
 *  Compositor
 * ===========================================================================*/
/* A transient notification toast, bottom-right above the taskbar. */
static void draw_toast(void) {
    if (!toast_msg[0] || pit_ticks() >= toast_until) return;
    int tw = g_text_width(toast_msg, 1);
    int bw = tw + 28, bh = 30;
    int bx = W - bw - 18, by = H - TASKBAR_H - bh - 14;
    g_round(bx, by, bw, bh, 8, COL_PANEL_2, 245);
    g_round(bx, by, 4, bh, 8, COL_ACCENT, 255);          /* accent edge */
    g_text(bx + 16, by + 11, toast_msg, COL_TEXT, 1);
}

/* Show `msg` as a ~2.5 s toast (called by apps + the GUI for user feedback). */
void gui_toast(const char *msg) {
    if (!msg) return;
    int i = 0; for (; msg[i] && i < (int)sizeof(toast_msg) - 1; i++) toast_msg[i] = msg[i];
    toast_msg[i] = 0;
    toast_until = pit_ticks() + 2500;
    dirty = 1;
}

/* prompt button rects (screen coords), rebuilt each draw */
static int pr_ok_x, pr_ok_y, pr_ok_w, pr_ok_h, pr_ca_x, pr_ca_w;
static void draw_prompt(void) {
    if (!prompt_active) return;
    int pw = 360, ph = 150, px = (W - pw) / 2, py = (H - ph) / 2;
    g_blend(0, 0, W, H, 0x000000, 120);                  /* dim the desktop */
    g_round(px - 2, py - 2, pw + 4, ph + 4, 14, 0x000000, 80);
    g_round(px, py, pw, ph, 12, 0x16161E, 255);
    g_fill(px, py, pw, 3, COL_ACCENT);
    g_text(px + 20, py + 16, prompt_title, COL_TEXT, 2);
    /* text field */
    int fx = px + 20, fy = py + 52, fw = pw - 40, fh = 30;
    g_round(fx, fy, fw, fh, 6, 0x0C0C12, 255);
    g_rect(fx, fy, fw, fh, COL_ACCENT);
    g_text(fx + 10, fy + 8, prompt_buf, COL_TEXT, 1);
    if ((pit_ticks() / 500) % 2 == 0) {
        int cxp = fx + 10 + g_text_width(prompt_buf, 1);
        if (cxp < fx + fw - 4) g_fill(cxp, fy + 6, 2, fh - 12, COL_ACCENT);
    }
    /* OK / Cancel buttons */
    pr_ok_w = 80; pr_ok_h = 28; pr_ok_y = py + ph - pr_ok_h - 16;
    pr_ok_x = px + pw - pr_ok_w - 20;
    pr_ca_w = 80; pr_ca_x = pr_ok_x - pr_ca_w - 10;
    g_round(pr_ca_x, pr_ok_y, pr_ca_w, pr_ok_h, 6, COL_PANEL_3, 255);
    g_text(pr_ca_x + 22, pr_ok_y + 7, "Cancel", COL_TEXT, 1);
    g_round(pr_ok_x, pr_ok_y, pr_ok_w, pr_ok_h, 6, COL_ACCENT, 255);
    g_text(pr_ok_x + 30, pr_ok_y + 7, "OK", 0xFFFFFF, 1);
}

/* Open a modal text prompt. `cb` is called with the entered string on OK/Enter
 * (not called on Cancel/Esc). Only one prompt is shown at a time. */
void gui_prompt(const char *title, const char *initial, void (*cb)(const char *)) {
    prompt_active = 1;
    prompt_cb = cb;
    int i = 0; for (; title && title[i] && i < (int)sizeof(prompt_title) - 1; i++) prompt_title[i] = title[i];
    prompt_title[i] = 0;
    prompt_len = 0;
    if (initial) for (; initial[prompt_len] && prompt_len < (int)sizeof(prompt_buf) - 1; prompt_len++)
        prompt_buf[prompt_len] = initial[prompt_len];
    prompt_buf[prompt_len] = 0;
    dirty = 1;
}
static void prompt_confirm(void) {
    prompt_active = 0;
    void (*cb)(const char *) = prompt_cb;
    prompt_cb = 0; dirty = 1;
    if (cb && prompt_len > 0) cb(prompt_buf);
}
static void prompt_cancel(void) { prompt_active = 0; prompt_cb = 0; dirty = 1; }

static void composite(void) {
    g_clear_clip();
    if (BG) { for (int i = 0; i < W * H; i++) BB[i] = BG[i]; }
    else    render_wallpaper(BB);

    draw_desktop_icons();

    /* windows back-to-front by z */
    for (int pass = 0; pass <= ztop; pass++)
        for (int i = 0; i < nwin; i++)
            if (wins[i].open && !wins[i].minimized && wins[i].z == pass)
                draw_window(&wins[i]);

    draw_start_menu();
    draw_taskbar();
    draw_context_menu();
    if (dnd_active) draw_drag_ghost();
    draw_toast();
    draw_prompt();
    draw_cursor();
}

/* ===========================================================================
 *  Input
 * ===========================================================================*/
static void clamp_window(window_t *w) {
    if (w->x > W - 60)        w->x = W - 60;
    if (w->x < 60 - w->w)     w->x = 60 - w->w;
    if (w->y < 0)             w->y = 0;
    if (w->y > H - TASKBAR_H - TITLE_H) w->y = H - TASKBAR_H - TITLE_H;
}

static void toggle_window_from_taskbar(int i) {
    window_t *w = &wins[i];
    int top = topmost();
    if (!w->open)            gui_open(w);
    else if (w->minimized) { w->minimized = 0; gui_focus(w); }
    else if (i == top)       w->minimized = 1;     /* focused -> minimise */
    else                     gui_focus(w);
    dirty = 1;
}

static void do_maximize(window_t *w) {
    if (w->maximized || w->snapped) {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
        w->maximized = 0; w->snapped = 0;
    } else {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->x = 0; w->y = 0; w->w = W; w->h = H - TASKBAR_H; w->maximized = 1;
    }
}

/* Win+arrow window management: left/right half, maximise, restore-or-minimise. */
static void gui_keysnap(window_t *w, char c) {
    int avail = H - TASKBAR_H;
    if (!w->maximized && !w->snapped) { w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h; }
    switch ((unsigned char)c) {
    case KEY_LEFT:  w->x = 0; w->y = 0; w->w = W / 2; w->h = avail; w->maximized = 0; w->snapped = 1; break;
    case KEY_RIGHT: w->x = W / 2; w->y = 0; w->w = W - W / 2; w->h = avail; w->maximized = 0; w->snapped = 1; break;
    case KEY_UP:    w->x = 0; w->y = 0; w->w = W; w->h = avail; w->maximized = 1; w->snapped = 0; break;
    case KEY_DOWN:
        if (w->maximized || w->snapped) {              /* restore */
            w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
            w->maximized = 0; w->snapped = 0;
        } else w->minimized = 1;                        /* else minimise */
        break;
    }
    dirty = 1;
}

/* Aero-snap on drag release: pointer at the top edge maximises; at the left or
 * right edge fills that half of the screen. The pre-snap rect is saved so the
 * next drag (or the maximise button) restores it. */
static void snap_window(window_t *w, int mx, int my) {
    int avail = H - TASKBAR_H;
    if (my > 2 && mx > 2 && mx < W - 3) return;        /* not at any edge */
    if (!w->maximized && !w->snapped) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
    }
    if (my <= 2) {                                     /* top -> maximise */
        w->x = 0; w->y = 0; w->w = W; w->h = avail; w->maximized = 1; w->snapped = 0;
    } else if (mx <= 2) {                              /* left half */
        w->x = 0; w->y = 0; w->w = W / 2; w->h = avail; w->maximized = 0; w->snapped = 1;
    } else {                                           /* right half */
        w->x = W / 2; w->y = 0; w->w = W - W / 2; w->h = avail; w->maximized = 0; w->snapped = 1;
    }
}

static void on_left_down(int x, int y) {
    /* a modal prompt swallows all clicks; only OK/Cancel act */
    if (prompt_active) {
        if (x >= pr_ok_x && x < pr_ok_x + pr_ok_w && y >= pr_ok_y && y < pr_ok_y + pr_ok_h) prompt_confirm();
        else if (x >= pr_ca_x && x < pr_ca_x + pr_ca_w && y >= pr_ok_y && y < pr_ok_y + pr_ok_h) prompt_cancel();
        return;
    }
    /* an open context menu eats the next click */
    if (ctx_open) {
        int it = ctx_menu_hit(x, y);
        if (ctx_mode == 1) {                                       /* desktop shortcuts */
            if (it == 0)      gui_open_by_title("Settings");
            else if (it == 1) gui_open_by_title("File Explorer");
            else if (it == 2) gui_open_by_title("Terminal");
        } else {
            if (it == 0)      { if (ctx_win >= 0) wins[ctx_win].open = 0; }      /* close   */
            else if (it == 1) { if (ctx_win >= 0) wins[ctx_win].pinned = !wins[ctx_win].pinned; }
        }
        ctx_open = 0; dirty = 1;
        if (it >= 0) return;                  /* clicked an item -> done       */
        /* it == -2 (outside) falls through so the click also lands normally   */
    }

    /* tray volume: popup slider, then the speaker button toggles it */
    if (vol_open && volume_popup_click(x, y)) { dirty = 1; return; }
    { int vbx, vby; tray_vol_rect(&vbx, &vby);
      if (x >= vbx && x < vbx + 28 && y >= vby && y < vby + 28) { vol_open = !vol_open; net_open = clock_open = 0; dirty = 1; return; } }
    if (vol_open) { vol_open = 0; dirty = 1; }   /* click elsewhere dismisses it */

    /* tray network: button toggles an info popup */
    { int nbx, nby; tray_net_rect(&nbx, &nby);
      if (x >= nbx && x < nbx + 28 && y >= nby && y < nby + 28) { net_open = !net_open; vol_open = 0; clock_open = 0; dirty = 1; return; } }
    if (net_open) { net_open = 0; dirty = 1; }    /* click elsewhere dismisses it */

    /* tray clock: toggles a calendar flyout */
    { int qbx, qby, qbw, qbh; clock_rect(&qbx, &qby, &qbw, &qbh);
      if (x >= qbx && x < qbx + qbw && y >= qby && y < qby + qbh) { clock_open = !clock_open; vol_open = net_open = 0; dirty = 1; return; } }
    if (clock_open) { clock_open = 0; dirty = 1; } /* click elsewhere dismisses it */

    /* start button */
    int bx, by; taskbar_btn_rect(-1, &bx, &by);
    if (x >= bx && x < bx + TB_BTN && y >= by && y < by + TB_BTN) { menu_open = !menu_open; sm_query[0] = 0; sm_qlen = 0; dirty = 1; return; }

    /* taskbar app buttons (open-or-pinned list) */
    if (y >= H - TASKBAR_H) {
        int idx[MAX_WIN]; int vis = taskbar_list(idx);
        for (int s = 0; s < vis; s++) {
            taskbar_btn_rect(s, &bx, &by);
            if (x >= bx && x < bx + TB_BTN && y >= by && y < by + TB_BTN) { toggle_window_from_taskbar(idx[s]); menu_open = 0; return; }
        }
        return;
    }

    /* start menu interaction (icon grid) */
    if (menu_open) {
        int mxx, myy, mw, mh; start_menu_rect(&mxx, &myy, &mw, &mh);
        if (x >= mxx && x < mxx + mw && y >= myy && y < myy + mh) {
            int sx, sy, sw, sh; signout_rect(mxx, myy, mw, mh, &sx, &sy, &sw, &sh);
            if (x >= sx && x < sx + sw && y >= sy && y < sy + sh) { menu_open = 0; gui_logout(); return; }
            sm_build_filter();
            for (int k = 0; k < sm_nfilt; k++) {
                int tx, ty; start_menu_tile(k, mxx, myy, &tx, &ty);
                if (x >= tx && x < tx + SM_TW && y >= ty && y < ty + SM_TH) { gui_open(&wins[sm_filt[k]]); menu_open = 0; return; }
            }
            return;
        }
        menu_open = 0; dirty = 1;                 /* click outside menu closes it */
    }

    /* window edge -> start an edge/corner resize (topmost window wins) */
    {
        int rwin, re = cursor_resize(x, y, &rwin);
        if (re && rwin >= 0) {
            gui_focus(&wins[rwin]);
            resize_id = rwin; resize_edge = re;
            rs_x0 = wins[rwin].x; rs_y0 = wins[rwin].y;
            rs_w0 = wins[rwin].w; rs_h0 = wins[rwin].h;
            rs_mx = x; rs_my = y;
            return;
        }
    }

    /* windows, front to back */
    for (int z = ztop; z >= 0; z--) {
        for (int i = 0; i < nwin; i++) {
            window_t *w = &wins[i];
            if (!w->open || w->minimized || w->z != z) continue;
            if (x < w->x || x >= w->x + w->w || y < w->y || y >= w->y + w->h) continue;
            gui_focus(w);
            if (y < w->y + TITLE_H) {              /* title bar */
                for (int slot = 0; slot < 3; slot++) {
                    int qx, qy, qw, qh; caption_btn(w, slot, &qx, &qy, &qw, &qh);
                    if (x >= qx && x < qx + qw && y >= qy && y < qy + qh) {
                        if (slot == 0)      w->open = 0;            /* close   */
                        else if (slot == 1) do_maximize(w);         /* maximise*/
                        else                w->minimized = 1;       /* minimise*/
                        return;
                    }
                }
                /* double-click the title bar toggles maximise */
                uint64_t now = pit_ticks();
                if (title_last_id == i && now - title_last_tick < 400) {
                    do_maximize(w); title_last_id = -1; return;
                }
                title_last_id = i; title_last_tick = now;
                /* drag a maximised/snapped window: restore its size first and
                 * recenter it under the cursor so it follows naturally */
                if (w->maximized || w->snapped) {
                    int gw = w->rw > 0 ? w->rw : w->w;
                    w->maximized = 0; w->snapped = 0;
                    w->w = gw; w->h = w->rh > 0 ? w->rh : w->h;
                    w->x = x - gw / 2; w->y = y - TITLE_H / 2;
                    clamp_window(w);
                }
                drag_id = i; drag_dx = x - w->x; drag_dy = y - w->y;
            } else {                                /* client area */
                if (w->click) w->click(w, x - w->x, y - (w->y + TITLE_H));
                if (w->drag) {                       /* begin a held-button drag stream */
                    client_drag_id = i;
                    w->drag(w, x - w->x, y - (w->y + TITLE_H));
                }
            }
            return;
        }
    }

    /* nothing else hit: the desktop. Select / double-click-open / arm reposition. */
    int di = deskicon_hit(x, y);
    if (di >= 0) {
        uint64_t now = pit_ticks();
        if (desk_last_idx == di && now - desk_last_tick < 400) {   /* double-click */
            desk_sel = di; desk_last_idx = -1; deskicon_open(di); return;
        }
        desk_sel = di; desk_last_idx = di; desk_last_tick = now;
        desk_drag = di; desk_dx = x - dicons[di].x; desk_dy = y - dicons[di].y;
        dirty = 1;
        return;
    }
    desk_sel = -1;                                   /* click on empty wallpaper */
}

/* right-press over a window's client area -> its rclick handler */
static void on_right_down(int x, int y) {
    /* right-click a taskbar app button -> close/pin context menu */
    if (y >= H - TASKBAR_H) {
        int idx[MAX_WIN]; int vis = taskbar_list(idx);
        for (int s = 0; s < vis; s++) {
            int bx, by; taskbar_btn_rect(s, &bx, &by);
            if (x >= bx && x < bx + TB_BTN && y >= by && y < by + TB_BTN) {
                ctx_open = 1; ctx_mode = 0; ctx_win = idx[s];
                ctx_x = bx; ctx_y = by; dirty = 1;
                return;
            }
        }
        ctx_open = 0; dirty = 1;
        return;
    }
    ctx_open = 0;                              /* any other right-click closes it */

    for (int z = ztop; z >= 0; z--)
        for (int i = 0; i < nwin; i++) {
            window_t *w = &wins[i];
            if (!w->open || w->minimized || w->z != z) continue;
            if (x < w->x || x >= w->x + w->w || y < w->y || y >= w->y + w->h) continue;
            if (y >= w->y + TITLE_H && w->rclick) { gui_focus(w); w->rclick(w, x - w->x, y - (w->y + TITLE_H)); }
            return;
        }

    /* empty desktop -> shortcut context menu at the cursor */
    if (!menu_open) { ctx_open = 1; ctx_mode = 1; ctx_x = x; ctx_y = y; dirty = 1; }
}

/* scroll-wheel over a window's client area -> its scroll handler */
static void handle_wheel(int dz) {
    int x = mlx(), y = mly();
    /* wheel over the tray speaker (or its open popup) nudges the volume */
    int vbx, vby; tray_vol_rect(&vbx, &vby);
    int over_tray = (x >= vbx && x < vbx + 28 && y >= vby && y < vby + 28);
    if (!over_tray && vol_open) {
        int px, py, pw, ph; vol_popup_rect(&px, &py, &pw, &ph);
        over_tray = (x >= px && x < px + pw && y >= py && y < py + ph);
    }
    if (over_tray) {
        int v = audio_volume() + dz * 5;
        if (v < 0) v = 0; if (v > 100) v = 100;
        audio_set_volume(v); dirty = 1; return;
    }
    for (int z = ztop; z >= 0; z--)
        for (int i = 0; i < nwin; i++) {
            window_t *w = &wins[i];
            if (!w->open || w->minimized || w->z != z) continue;
            if (x < w->x || x >= w->x + w->w || y < w->y + TITLE_H || y >= w->y + w->h) continue;
            if (w->scroll) { w->scroll(w, dz); dirty = 1; }
            return;
        }
}

static void handle_mouse(void) {
    int x = mlx(), y = mly();
    uint8_t b = mouse_buttons();
    uint8_t down = b & ~prev_btns;
    uint8_t up   = prev_btns & ~b;

    if (resize_id >= 0) {                            /* resizing a window by an edge/corner */
        window_t *w = &wins[resize_id];
        if (b & MOUSE_LEFT) {
            int dx = x - rs_mx, dy = y - rs_my;
            int r_edge = rs_x0 + rs_w0, b_edge = rs_y0 + rs_h0;   /* fixed opposite edges */
            int nx = rs_x0, ny = rs_y0, nw = rs_w0, nh = rs_h0;
            if (resize_edge & RS_R) nw = rs_w0 + dx;
            if (resize_edge & RS_L) nw = rs_w0 - dx;
            if (resize_edge & RS_B) nh = rs_h0 + dy;
            if (resize_edge & RS_T) nh = rs_h0 - dy;
            if (nw < w->min_w) nw = w->min_w;
            if (nh < w->min_h) nh = w->min_h;
            if (nw > W)            nw = W;
            if (nh > H - TITLE_H)  nh = H - TITLE_H;
            if (resize_edge & RS_L) nx = r_edge - nw;            /* grow left: keep right edge */
            if (resize_edge & RS_T) ny = b_edge - nh;            /* grow up: keep bottom edge   */
            if (ny < 0) { ny = 0; if (resize_edge & RS_T) nh = b_edge; }
            w->x = nx; w->y = ny; w->w = nw; w->h = nh;
            clamp_window(w);
        }
        if (up & MOUSE_LEFT) resize_id = -1;
    } else if (drag_id >= 0) {                       /* dragging a window */
        if (b & MOUSE_LEFT) { wins[drag_id].x = x - drag_dx; wins[drag_id].y = y - drag_dy; clamp_window(&wins[drag_id]); }
        if (up & MOUSE_LEFT) { snap_window(&wins[drag_id], x, y); drag_id = -1; }
    } else if (client_drag_id >= 0) {                /* held-button drag inside an app's client area */
        window_t *w = &wins[client_drag_id];
        if ((b & MOUSE_LEFT) && w->drag) w->drag(w, x - w->x, y - (w->y + TITLE_H));
        if (up & MOUSE_LEFT) client_drag_id = -1;
    } else if (desk_drag >= 0) {                     /* repositioning a desktop icon */
        if (b & MOUSE_LEFT) { dicons[desk_drag].x = x - desk_dx; dicons[desk_drag].y = y - desk_dy; deskicon_clamp(&dicons[desk_drag]); }
        if (up & MOUSE_LEFT)  desk_drag = -1;
    } else if (dnd_active) {                          /* dragging an item to the desktop */
        dnd_px = x; dnd_py = y;
        if (up & MOUSE_LEFT) { dnd_drop(x, y); dnd_active = 0; dnd_armed = 0; dnd_node = 0; }
    } else if (dnd_armed && (b & MOUSE_LEFT)) {       /* armed: promote to a drag once it moves */
        if (iabs(x - press_x) > 5 || iabs(y - press_y) > 5) { dnd_active = 1; dnd_px = x; dnd_py = y; }
        if (up & MOUSE_LEFT) { dnd_armed = 0; dnd_node = 0; }
    } else if (down & MOUSE_LEFT) {
        on_left_down(x, y);
    }
    if ((up & MOUSE_LEFT) && !dnd_active) { dnd_armed = 0; dnd_node = 0; }  /* armed click ended w/o a drag */
    if (down & MOUSE_RIGHT) on_right_down(x, y);
    prev_btns = b;
    dirty = 1;                                      /* cursor moved -> repaint */
}

/* Capture the composited desktop (the BB backbuffer) to a 24-bit BMP under
 * /home/Pictures and open it in Photos. Triggered globally by F12. */
static void gui_screenshot(void) {
    int w = W, h = H;
    if (w <= 0 || h <= 0) return;
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
        uint32_t *src = &BB[(uint32_t)y * w];
        for (int x = 0; x < w; x++) {
            uint32_t p = src[x];
            dst[x * 3 + 0] = (uint8_t)(p);          /* B */
            dst[x * 3 + 1] = (uint8_t)(p >> 8);     /* G */
            dst[x * 3 + 2] = (uint8_t)(p >> 16);    /* R */
        }
    }
    /* pick the first free /home/Pictures/screenshot-N.bmp */
    if (!fs_lookup("/home/Pictures")) fs_create("/home/Pictures", 1);
    char path[FS_PATH_MAX];
    for (int n = 1; n <= 999; n++) {
        const char *pre = "/home/Pictures/screenshot-";
        int i = 0; for (; pre[i]; i++) path[i] = pre[i];
        char num[8]; int t = 0, v = n; while (v) { num[t++] = (char)('0' + v % 10); v /= 10; }
        while (t) path[i++] = num[--t];
        const char *suf = ".bmp"; for (int j = 0; suf[j]; j++) path[i++] = suf[j];
        path[i] = 0;
        if (!fs_lookup(path)) break;
    }
    fs_node *f = fs_create(path, 0);
    if (f) fs_write(f, bmp, sz);
    kfree(bmp);
    gui_toast("Screenshot saved to Pictures");
    imgview_open_path(path);
}

static void handle_key(char c) {
    /* a modal prompt grabs all keys until confirmed/cancelled */
    if (prompt_active) {
        unsigned char u = (unsigned char)c;
        if (u == 27)      prompt_cancel();
        else if (u == '\n' || u == '\r') prompt_confirm();
        else if (u == '\b') { if (prompt_len > 0) prompt_buf[--prompt_len] = 0; dirty = 1; }
        else if (u >= 32 && u < 127 && prompt_len < (int)sizeof(prompt_buf) - 1) {
            prompt_buf[prompt_len++] = c; prompt_buf[prompt_len] = 0; dirty = 1;
        }
        return;
    }
    if (c == 27) { menu_open = 0; ctx_open = 0; dirty = 1; return; }   /* Esc closes menus */
    if ((unsigned char)c == KEY_SHOT) { gui_screenshot(); dirty = 1; return; }
    if ((unsigned char)c == KEY_ALTTAB)  { gui_cycle_window(+1); menu_open = 0; return; }
    if ((unsigned char)c == KEY_ALTTABR) { gui_cycle_window(-1); menu_open = 0; return; }
    if ((unsigned char)c == KEY_ALTF4)   { int t = topmost(); if (t >= 0) wins[t].open = 0; menu_open = 0; dirty = 1; return; }
    if ((unsigned char)c == KEY_WINTAP)  { menu_open = !menu_open; sm_query[0] = 0; sm_qlen = 0; dirty = 1; return; }
    /* Win+arrow snaps the focused window; Win+D shows/restores the desktop. */
    if (kbd_win_down()) {
        unsigned char u = (unsigned char)c;
        if (u == KEY_LEFT || u == KEY_RIGHT || u == KEY_UP || u == KEY_DOWN) {
            int t = topmost(); if (t >= 0) gui_keysnap(&wins[t], c);
            return;
        }
        if (u == 'd' || u == 'D') { gui_show_desktop(); return; }
    }
    /* While the Start menu is open, keystrokes drive its search box. */
    if (menu_open) {
        unsigned char u = (unsigned char)c;
        if (u == '\b') { if (sm_qlen) sm_query[--sm_qlen] = 0; }
        else if (u == '\n') {                       /* Enter opens the first match */
            sm_build_filter();
            if (sm_nfilt > 0) { gui_open(&wins[sm_filt[0]]); menu_open = 0; }
        } else if (u >= 32 && u < 127 && sm_qlen < (int)sizeof(sm_query) - 1) {
            sm_query[sm_qlen++] = c; sm_query[sm_qlen] = 0;
        }
        dirty = 1; return;
    }
    int f = topmost();
    if (f >= 0 && wins[f].key) wins[f].key(&wins[f], c);
    dirty = 1;
}

/* ===========================================================================
 *  Display configuration  (resolution, aspect letterbox, wallpaper)
 *  Reads the live g_settings and recomputes the logical desktop size, the
 *  letterboxed output rectangle on the physical panel, and the wallpaper.
 * ===========================================================================*/
void gui_apply_display(void) {
    /* Real native resolution: reprogram the GPU to the picked mode so the panel
     * itself becomes that size. Logical desktop == physical panel, output fills
     * it edge to edge -- no upscaling, no letterbox bars, always crisp. */
    int lw, lh; settings_res_dims(g_settings.res_index, &lw, &lh);
    if (lw > MAX_PW) lw = MAX_PW;
    if (lh > MAX_PH) lh = MAX_PH;

    if (hires_ok && (lw != PW || lh != PH)) {
        if (fb_set_mode((uint32_t)lw, (uint32_t)lh) == 0) {
            PW = lw; PH = lh;
            mouse_set_bounds(PW, PH);
        }
        /* on failure the old mode stands; fall through with current PW,PH */
    }
    W = PW; H = PH;
    out_x = 0; out_y = 0; out_w = PW; out_h = PH;

    g_clear_clip();                          /* clip now spans the new W,H */

    /* keep every window inside the (possibly smaller) logical desktop */
    for (int i = 0; i < nwin; i++) {
        window_t *w = &wins[i];
        if (w->maximized) { w->x = 0; w->y = 0; w->w = W; w->h = H - TASKBAR_H; }
        else {
            if (w->w > W)             w->w = W;
            if (w->h > H - TASKBAR_H) w->h = H - TASKBAR_H;
            clamp_window(w);
        }
    }

    if (BG) render_wallpaper(BG);
    dirty = 1;
}

static int output_is_native(void) {
    return out_x == 0 && out_y == 0 && out_w == PW && out_h == PH && W == PW && H == PH;
}

/* Force one composite + blit right now. Lets a blocking command (e.g. the
 * `python` REPL waiting on keyboard input) keep the desktop on screen instead
 * of freezing it. No-op before the desktop is up (BB not yet allocated). */
void gui_pump(void) {
    if (!BB) return;
    composite();
    if (output_is_native()) fb_blit(BB);
    else fb_blit_scaled(BB, (uint32_t)W, (uint32_t)H,
                        (uint32_t)out_x, (uint32_t)out_y, (uint32_t)out_w, (uint32_t)out_h);
    dirty = 0;
}

/* ===========================================================================
 *  Login screen. Shown full-screen before the desktop; keyboard-driven. On a
 *  correct username/password the current user is set and the desktop appears.
 * ===========================================================================*/
static int  login_active;
static char login_user[USER_NAME_MAX];
static int  login_ulen;
static char login_pass[40];
static int  login_plen;
static int  login_field;          /* 0 = username, 1 = password */
static int  login_err;

static void login_begin(void) {
    login_active = 1; login_ulen = login_plen = 0; login_field = 0; login_err = 0;
    login_user[0] = login_pass[0] = 0;
}
void gui_logout(void) { users_logout(); login_begin(); dirty = 1; }
static void login_submit(void) {
    login_user[login_ulen] = 0; login_pass[login_plen] = 0;
    int uid = users_auth(login_user, login_pass);
    if (uid >= 0) { users_login(uid); login_active = 0; }
    else { login_err = 1; login_plen = 0; login_pass[0] = 0; login_field = 1; }
}
static void login_key(char c) {
    unsigned char u = (unsigned char)c;
    if (u == '\t') { login_field ^= 1; login_err = 0; return; }
    if (u == '\n') { if (login_field == 0 && login_ulen > 0) login_field = 1; else login_submit(); return; }
    if (u == '\b') {
        if (login_field == 0) { if (login_ulen) login_user[--login_ulen] = 0; }
        else                  { if (login_plen) login_pass[--login_plen] = 0; }
        return;
    }
    if (u >= 32 && u < 127) {
        if (login_field == 0) { if (login_ulen < USER_NAME_MAX - 1) login_user[login_ulen++] = c; }
        else                  { if (login_plen < 39)               login_pass[login_plen++] = c; }
    }
}
static void draw_login(void) {
    g_clear_clip();
    g_fill(0, 0, W, H, 0x0A0E1A);
    int cw = 380, chh = 256;
    int cx = (W - cw) / 2, cy = (H - chh) / 2;
    g_round(cx, cy, cw, chh, 12, 0x161A28, 255);
    /* TrueType title (scalable, anti-aliased) with a bitmap fallback */
    g_text_tt(cx + cw/2 - g_text_tt_width("BoltOS", 34)/2, cy + 14, "BoltOS", 0xFFFFFF, 34);
    g_text_tt(cx + cw/2 - g_text_tt_width("Sign in to continue", 14)/2, cy + 56, "Sign in to continue", 0x8893A8, 14);

    int fx = cx + 30, fw = cw - 60, fh = 30;
    int uy = cy + 96, py = cy + 152;
    g_text(fx, uy - 16, "Username", 0x8893A8, 1);
    g_round(fx, uy, fw, fh, 6, login_field == 0 ? 0x223052 : 0x10131F, 255);
    g_rect(fx, uy, fw, fh, login_field == 0 ? 0x4FC3F7 : 0x33333F);
    g_text(fx + 10, uy + 8, login_user, 0xFFFFFF, 1);

    g_text(fx, py - 16, "Password", 0x8893A8, 1);
    g_round(fx, py, fw, fh, 6, login_field == 1 ? 0x223052 : 0x10131F, 255);
    g_rect(fx, py, fw, fh, login_field == 1 ? 0x4FC3F7 : 0x33333F);
    char stars[40]; int i; for (i = 0; i < login_plen && i < 39; i++) stars[i] = '*'; stars[i] = 0;
    g_text(fx + 10, py + 8, stars, 0xFFFFFF, 1);

    if (login_err)
        g_text(cx + cw/2 - g_text_width("Incorrect credentials", 1)/2, cy + chh - 32, "Incorrect credentials", 0xFF6B6B, 1);
    else
        g_text(fx, cy + chh - 32, "Tab switches field . default: user / bolt", 0x55607A, 1);
}

/* ===========================================================================
 *  Boot screen + main loop
 * ===========================================================================*/
void gui_run(void) {
    PW = (int)fb_width();
    PH = (int)fb_height();
    W = PW; H = PH;
    out_x = out_y = 0; out_w = PW; out_h = PH;
    g_clear_clip();

    /* Allocate the compositor backbuffers at the largest panel we support (4K)
     * so a later resolution change just reuses them -- no realloc, no contig
     * fragmentation mid-session. From the PMM (direct-mapped), since the 16 MiB
     * kheap can't hold a 33 MiB buffer. Fall back to a boot-sized kheap buffer
     * if that much contiguous RAM isn't available (hi-res then disabled). */
    uint64_t maxpx    = (uint64_t)MAX_PW * MAX_PH;
    uint64_t maxframes = (maxpx * 4 + 4095) / 4096;
    uint64_t bbp = pmm_alloc_contig(maxframes);
    uint64_t bgp = bbp ? pmm_alloc_contig(maxframes) : 0;
    if (bbp && bgp) {
        BB = (uint32_t *)P2V(bbp);
        BG = (uint32_t *)P2V(bgp);
        hires_ok = 1;
    } else {
        if (bbp) pmm_free_contig(bbp, maxframes);
        BB = (uint32_t *)kmalloc((uint64_t)PW * PH * 4);
        if (!BB) { console_detach(); shell_run(); return; }   /* no memory -> shell */
        BG = (uint32_t *)kmalloc((uint64_t)PW * PH * 4);
        hires_ok = 0;
    }

    settings_init();           /* theme + logical size + letterbox + wallpaper */

    console_detach();                                /* GUI owns the framebuffer now */

    terminal_app_init();
    files_app_init();
    browser_app_init();
    oldbrowser_app_init();
    ide_app_init();
    taskmgr_app_init();
    calc_app_init();
    clock_app_init();
    notes_app_init();
    tasks_app_init();
    contacts_app_init();
    sheets_app_init();
    imgview_app_init();
    music_app_init();
    calendar_app_init();
    piano_app_init();
    paint_app_init();
    mines_app_init();
    snake_app_init();
    g2048_app_init();
    stopwatch_app_init();
    sysinfo_app_init();
    life_app_init();
    ttt_app_init();
    colorpick_app_init();
    memory_app_init();
    matrix_app_init();
    doom_app_init();
    settings_app_init();
    /* Every app gets a launcher icon on the desktop; double-click opens it. The
     * taskbar starts empty and only fills with apps as they are opened (or that
     * the user pins via the taskbar right-click menu). */
    for (int i = 0; i < nwin; i++)
        gui_desktop_add_app(&wins[i], wins[i].title, wins[i].icon);

    uint64_t tick_pit = pit_ticks();
    uint64_t sec_pit  = pit_ticks();
    uint64_t busy = 0, total = 0;
    prev_btns = 0; drag_id = -1; dirty = 1;

    wallpaper_restore();       /* restore a saved image wallpaper, if any */

    login_begin();             /* gate the desktop behind a sign-in */

    for (;;) {
        uint64_t t0 = rdtsc();

        /* A ring-3 program (the userland browser) has grabbed the panel: don't
         * touch VRAM or drain the keyboard -- it reads input via SYS_GETKEY and
         * paints the screen itself. Park until the timer tick reschedules; the
         * scheduler runs that ring-3 thread. Repaint the desktop once it lets go. */
        if (g_fb_exclusive) { __asm__ volatile("hlt"); dirty = 1; continue; }

        if (login_active) {                          /* sign-in: keyboard only */
            xhci_poll();
            int ci; while ((ci = kbd_trygetc()) >= 0) { login_key((char)ci); dirty = 1; }
            if (dirty) {
                draw_login();
                if (output_is_native()) fb_blit(BB);
                else fb_blit_scaled(BB, (uint32_t)W, (uint32_t)H,
                                    (uint32_t)out_x, (uint32_t)out_y, (uint32_t)out_w, (uint32_t)out_h);
                dirty = 0;
            }
            continue;
        }

        xhci_poll();                                 /* drain USB HID keyboard reports */
        if (mouse_poll_event()) handle_mouse();
        { int dz = mouse_wheel(); if (dz) handle_wheel(dz); }
        int ci; while ((ci = kbd_trygetc()) >= 0) handle_key((char)ci);

        doom_pump();        /* DOOM self-throttles to ~35 fps and only when focused */

        uint64_t now = pit_ticks();
        if (now - tick_pit >= 150) {                 /* ~6.6 Hz: app ticks, animation, sampling
                                                      * (apps self-throttle slower work internally) */
            tick_pit = now;
            for (int i = 0; i < nwin; i++) if (wins[i].open && wins[i].tick) wins[i].tick(&wins[i]);
            dirty = 1;
        }

        if (dirty) {
            composite();
            /* Re-check after the (non-atomic) composite: a ring-3 renderer may
             * have grabbed the panel mid-frame. Don't blit over it -- that was
             * the stale-frame race that left the desktop on screen. */
            if (!g_fb_exclusive) {
                if (output_is_native()) fb_blit(BB);
                else fb_blit_scaled(BB, (uint32_t)W, (uint32_t)H,
                                     (uint32_t)out_x, (uint32_t)out_y,
                                     (uint32_t)out_w, (uint32_t)out_h);
            }
            dirty = 0;
        }

        uint64_t t1 = rdtsc();
        busy += t1 - t0;
        __asm__ volatile("hlt");                     /* sleep until next IRQ */
        total += rdtsc() - t0;

        if (now - sec_pit >= 1000) {                 /* recompute CPU load each second */
            sec_pit = now;
            cpu_load = total ? (int)((busy * 100) / total) : 0;
            if (cpu_load > 100) cpu_load = 100;
            busy = total = 0;
        }
    }
}
