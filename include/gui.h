#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS GUI  -  a small compositing window manager over the framebuffer.
 *
 *  Everything is drawn into an off-screen backbuffer and blitted once per
 *  frame (flicker-free dragging). Apps are windows: they register a draw and
 *  optional input callbacks and the manager handles framing, focus, dragging,
 *  minimise/maximise/close, the taskbar and the mouse cursor.
 * ===========================================================================*/

/* ---- theme -------------------------------------------------------------- *
 *  Colours are runtime now (the Settings app retints the whole desktop). The
 *  COL_* names are kept as accessors into the live theme so the ~50 existing
 *  call sites compile unchanged; they just read the current palette each use.
 *  NOTE: because these expand to a struct read they are NOT compile-time
 *  constants -- never use a COL_* in a static/global initialiser.            */
typedef struct {
    uint32_t accent, accent_dim;
    uint32_t text, text_dim;
    uint32_t panel, panel2, panel3;
    uint32_t good, warn, bad;
} theme_t;
extern theme_t g_theme;

#define COL_ACCENT      (g_theme.accent)      /* signature accent          */
#define COL_ACCENT_DIM  (g_theme.accent_dim)
#define COL_TEXT        (g_theme.text)
#define COL_TEXT_DIM    (g_theme.text_dim)
#define COL_PANEL       (g_theme.panel)       /* window client background  */
#define COL_PANEL_2     (g_theme.panel2)
#define COL_PANEL_3     (g_theme.panel3)
#define COL_GOOD        (g_theme.good)
#define COL_WARN        (g_theme.warn)
#define COL_BAD         (g_theme.bad)

/* ---- graphics primitives (operate on the compositor backbuffer) -------- */
void g_fill (int x, int y, int w, int h, uint32_t color);
void g_blend(int x, int y, int w, int h, uint32_t color, uint8_t alpha);   /* alpha 0..255 */
void g_rect (int x, int y, int w, int h, uint32_t color);                  /* 1px outline   */
void g_hline(int x, int y, int w, uint32_t color);
void g_vline(int x, int y, int h, uint32_t color);
void g_round(int x, int y, int w, int h, int r, uint32_t color, uint8_t alpha); /* rounded rect */
void g_char (int x, int y, char c, uint32_t color, int scale);
void g_text (int x, int y, const char *s, uint32_t color, int scale);
int  g_text_width(const char *s, int scale);
void g_set_clip(int x, int y, int w, int h);
void g_clear_clip(void);

/* ---- windows ----------------------------------------------------------- */
typedef struct window window_t;
struct window {
    char     title[40];
    int      x, y, w, h;            /* outer rectangle (includes title bar)   */
    int      min_w, min_h;
    int      open;                  /* registered + visible                   */
    int      minimized, maximized;
    int      rx, ry, rw, rh;        /* saved rect for un-maximise             */
    int      z;                     /* stacking order (higher = nearer front) */
    uint32_t accent;                /* per-app accent / icon colour           */
    int      icon;                  /* taskbar/title icon id (see ICON_*)      */
    /* client area is (x, y+TITLE_H, w, h-TITLE_H); callbacks get it as cx..ch */
    void (*draw) (window_t *, int cx, int cy, int cw, int ch);
    void (*key)  (window_t *, char c);
    void (*click)(window_t *, int lx, int ly);   /* client-local mouse click  */
    void (*tick) (window_t *);                    /* periodic update (~2 Hz)   */
    void  *st;                       /* app private state                      */
};

enum { ICON_NONE = 0, ICON_TERMINAL, ICON_TASKMGR, ICON_START, ICON_FILES, ICON_SETTINGS, ICON_BROWSER };

window_t *gui_add_window(const char *title, int w, int h, uint32_t accent, int icon);
void      gui_open(window_t *win);          /* show + focus + raise           */
void      gui_focus(window_t *win);
void      gui_request_redraw(void);
int       gui_cpu_load(void);               /* 0..100, real busy/idle fraction*/
int       gui_screen_w(void);               /* logical desktop width           */
int       gui_screen_h(void);               /* logical desktop height          */
int       gui_panel_w(void);                /* physical panel width (fixed)    */
int       gui_panel_h(void);                /* physical panel height (fixed)   */
int       gui_window_focused(window_t *win);
void      gui_run(void);                    /* enter the desktop; never returns*/

/* Re-read the live settings: relayout the logical desktop / letterbox, retint
 * the theme and re-render the wallpaper. Called by the Settings app on change. */
void      gui_apply_display(void);

/* ---- apps -------------------------------------------------------------- */
void terminal_app_init(void);
void taskmgr_app_init(void);
void settings_app_init(void);
void browser_app_init(void);
