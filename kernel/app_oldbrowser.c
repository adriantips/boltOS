/* ===========================================================================
 *  app_oldbrowser.c  --  BoltOS window glue for the OldBrowser (NetSurf) port
 *
 *  Registers the OldBrowser desktop window and forwards the compositor's
 *  draw / key / click / scroll / tick callbacks into the NetSurf framebuffer
 *  frontend (oldbrowser/ob_gui.c). All browser state lives in the port; this
 *  file is just the thin BoltOS-side adapter, the same role a platform's
 *  netsurf main() plays.
 * ===========================================================================*/
#include "gui.h"
#include "../oldbrowser/oldbrowser.h"

extern window_t *ob_active_window;

static void ob_draw (window_t *w, int cx, int cy, int cw, int ch) { (void)w; ob_gui_draw(cx, cy, cw, ch); }
static void ob_key  (window_t *w, char c)              { (void)w; ob_gui_key(c); }
static void ob_click(window_t *w, int lx, int ly)      { (void)w; ob_gui_click(lx, ly); }
static void ob_scrl (window_t *w, int delta)           { (void)w; ob_gui_scroll(delta); }
static void ob_tick (window_t *w)                      { (void)w; ob_gui_tick(); }

void oldbrowser_app_init(void) {
    window_t *w = gui_add_window("OldBrowser", 820, 600, 0x1A73E8, ICON_OLDBROWSER);
    if (!w) return;
    w->min_w = 460; w->min_h = 320;
    w->draw   = ob_draw;
    w->key    = ob_key;
    w->click  = ob_click;
    w->scroll = ob_scrl;
    w->tick   = ob_tick;
    ob_active_window = w;
    ob_gui_init();
}
