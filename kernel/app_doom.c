/* ===========================================================================
 *  BoltOS  -  kernel/app_doom.c
 *  DOOM, running on the from-scratch doomgeneric port (see doom/). This file is
 *  the GUI side: it owns the window, blits the engine's 640x400 framebuffer,
 *  and drives the game forward ~35x a second from the desktop's main loop.
 *
 *  The engine itself (under doom/) is built against a tiny in-house libc
 *  (doom/dg_libc.c) and a platform shim (doom/doomgeneric_boltos.c). We only
 *  talk to it through the small C ABI declared below. The game boots straight
 *  into E1M1 (the platform passes "-warp 1 1") so opening the app drops you in.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "pit.h"
#include "keyboard.h"

/* ---- doomgeneric bridge (doom/doomgeneric_boltos.c) -----------------------*/
extern uint32_t *DG_ScreenBuffer;        /* 640x400 ARGB, painted by the engine */
extern void  dg_boltos_create(void);     /* one-time init (loads WAD, builds tables) */
extern void  dg_boltos_tick(void);       /* advance + render one frame */
extern int   dg_boltos_status(void);     /* 0 = idle, 1 = running, 2 = failed */
extern void  dg_boltos_key(int pressed, int scancode, int ext);

#define DOOM_W 640
#define DOOM_H 400
#define FRAME_MS 28                       /* ~35 fps render cap (game logic is real-time) */

static window_t *g_win;
static int       g_loading;               /* showing the "Loading" notice */
static int       g_created;               /* engine init has been attempted */
static uint64_t  g_last_frame;

static int doom_active(void) {
    return g_win && g_win->open && !g_win->minimized && gui_window_focused(g_win);
}

/* Called every iteration of the desktop loop (see gui.c). Pulls raw PS/2
 * scancodes for real key up/down, then steps the engine at the frame cap. */
void doom_pump(void) {
    if (!g_win) return;

    /* drain raw scancodes: low7 = code, 0x100 = release, 0x200 = extended */
    int ev;
    while ((ev = kbd_raw_get()) >= 0) {
        if (!doom_active()) continue;                  /* discard when not in focus */
        dg_boltos_key(!(ev & 0x100), ev & 0x7F, (ev & 0x200) != 0);
    }

    if (!doom_active()) return;

    if (!g_created) {
        /* First activation: paint the loading notice for one frame, then boot
         * the engine -- a multi-second, blocking WAD load + table build. */
        if (!g_loading) { g_loading = 1; gui_request_redraw(); return; }
        g_created = 1;
        dg_boltos_create();
        g_last_frame = pit_ticks();
        gui_request_redraw();
        return;
    }
    if (dg_boltos_status() != 1) return;               /* failed or not running */

    uint64_t now = pit_ticks();
    if (now - g_last_frame < FRAME_MS) return;
    g_last_frame = now;
    dg_boltos_tick();
    gui_request_redraw();
}

static void doom_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    int st = g_created ? dg_boltos_status() : 0;

    if (st == 1 && DG_ScreenBuffer) {
        /* paint the engine framebuffer, stretched to fill the client area */
        g_blit(cx, cy, cw, ch, DG_ScreenBuffer, DOOM_W, DOOM_H);
        return;
    }

    g_fill(cx, cy, cw, ch, 0x000000);
    const char *msg = (st == 2)   ? "DOOM failed to start (see serial log)"
                    : g_loading    ? "Loading DOOM  -  first run builds the level..."
                    :                "Opening DOOM...";
    g_text(cx + (cw - g_text_width(msg, 2)) / 2, cy + ch / 2 - 8, msg,
           st == 2 ? COL_BAD : COL_ACCENT, 2);
}

void doom_app_init(void) {
    /* a touch of breathing room around the 640x400 image + title bar */
    window_t *win = gui_add_window("DOOM", DOOM_W, DOOM_H + 32, 0xB31312, ICON_DOOM);
    if (!win) return;
    win->draw  = doom_draw;
    win->min_w = 320;
    win->min_h = 240;
    win->x = 150; win->y = 40;
    g_win = win;

    kbd_raw_enable(1);     /* start capturing real make/break scancodes */
}
