#pragma once
#include <stdint.h>
#include "dom.h"
#include "layout.h"
#include "image.h"

/* ===========================================================================
 *  OldBrowser -- a NetSurf port for BoltOS.
 *
 *  NetSurf is a small, fast, multi-platform web browser written in C, designed
 *  around a clean separation between a portable *core* (content cache + handler
 *  pipeline + browser_window state machine) and per-platform *frontends*. This
 *  is a faithful port of that architecture to BoltOS: the module layout below
 *  mirrors NetSurf's real source tree, and each ob_*.c file corresponds to a
 *  NetSurf subsystem (named in its banner). The heavy lifting that NetSurf gives
 *  to libdom / libcss is provided here by BoltOS's own DOM tree (kernel/dom.c)
 *  and CSS-cascade + box-model layout engine (kernel/layout.c); fetching rides
 *  the kernel's IPv4/TCP/TLS stack (net/http.c).
 *
 *      ob_nsurl.c    <- utils/nsurl.c, utils/url.c   (URL object + RFC3986 join)
 *      ob_llcache.c  <- content/llcache.c            (low-level fetch + redirect)
 *      ob_content.c  <- content/content.c + handlers (html / textplain / image)
 *      ob_window.c   <- desktop/browser_window.c,
 *                       desktop/browser_history.c     (navigation + back/forward)
 *      ob_hotlist.c  <- desktop/hotlist.c            (bookmarks, persisted to FS)
 *      ob_fbtk.c     <- frontends/framebuffer/fbtk   (widget toolkit)
 *      ob_gui.c      <- frontends/framebuffer/gui.c  (toolbar/throbber/viewport)
 * ===========================================================================*/

struct browser_window;

/* substring search (the kernel libc lacks strstr) */
static inline char *ob_strstr(const char *h, const char *n) {
    if (!n || !*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return 0;
}

/* ============================ nsurl (utils/nsurl) ======================== */
typedef struct nsurl {
    int   refcount;
    char  scheme[16];
    char  host[160];
    int   port;            /* 0 = scheme default                            */
    char  path[1024];      /* always begins with '/' for http(s); may be ""  */
    char  query[1024];     /* without leading '?'  ("" if none)              */
    char  fragment[256];   /* without leading '#'  ("" if none)              */
    char  full[2048];      /* complete normalised URL string                 */
} nsurl;

enum nsurl_component { NSURL_SCHEME, NSURL_HOST, NSURL_PATH, NSURL_QUERY,
                       NSURL_FRAGMENT, NSURL_COMPLETE };

nsurl      *nsurl_create(const char *s);              /* parse absolute URL    */
nsurl      *nsurl_join(nsurl *base, const char *rel); /* RFC3986 §5 resolve    */
nsurl      *nsurl_ref(nsurl *u);
void        nsurl_unref(nsurl *u);
const char *nsurl_access(nsurl *u);                   /* complete string       */
int         nsurl_get_component(nsurl *u, enum nsurl_component c,
                                char *out, uint32_t cap);
int         nsurl_compare(nsurl *a, nsurl *b);        /* 1 if equal            */

/* ============================ content (content/content) ================== */
enum content_type { CONTENT_NONE = 0, CONTENT_HTML, CONTENT_TEXTPLAIN,
                    CONTENT_IMAGE, CONTENT_ERROR };

typedef struct content {
    enum content_type type;
    char    title[160];
    int     status;                 /* HTTP status (or synthetic)            */
    int     width, height;          /* laid-out / natural content size       */
    nsurl  *url;                    /* base URL for relative resolution      */
    char   *source; uint32_t source_len;   /* retained source bytes          */
    /* --- text/html (content/handlers/html.c) --- */
    dom_document *dom;
    layout_tree  *box;
    char   *css; uint32_t css_len;
    int     laid_w;
    /* --- image (content/handlers/image.c) --- */
    image_t *img;
    /* --- error --- */
    char    errmsg[200];
} content;

content    *content_create(nsurl *url, const char *data, uint32_t len, int status);
content    *content_create_error(nsurl *url, const char *msg);
void        content_destroy(content *c);
void        content_reformat(content *c, int width);
void        content_redraw(content *c, struct browser_window *bw,
                           int ox, int oy, int cl, int cr, int clipT, int clipB);
const char *content_get_title(content *c);
int         content_get_height(content *c);
/* raw href (unresolved) of the <a> under content-space point (cx,cy), or NULL */
const char *content_href_at(content *c, int cx, int cy);

/* ============================ llcache (content/llcache) ================== */
/* Fetch url's source into *out (kmalloc'd; caller kfree's). Returns byte count
 * and sets *status; *final (caller nsurl_unref's) is the post-redirect URL.
 * Returns -1 on hard failure (status carries a negative reason). */
int  llcache_fetch(nsurl *url, char **out, int *status, nsurl **final);

/* ============================ browser_window (desktop) =================== */
typedef struct browser_window browser_window;

browser_window *browser_window_create(void);
void   browser_window_destroy(browser_window *bw);
int    browser_window_navigate(browser_window *bw, nsurl *url, int add_to_hist);
void   browser_window_go(browser_window *bw, const char *addr);  /* user typed */
void   browser_window_reformat(browser_window *bw, int width);
void   browser_window_redraw(browser_window *bw, int ox, int oy,
                             int cl, int cr, int clipT, int clipB);
void   browser_window_mouse_click(browser_window *bw, int cx, int cy);
void   browser_window_key(browser_window *bw, char c);
void   browser_window_scroll(browser_window *bw, int delta, int viewport_h);
int    browser_window_back_available(browser_window *bw);
int    browser_window_forward_available(browser_window *bw);
void   browser_window_history_back(browser_window *bw);
void   browser_window_history_forward(browser_window *bw);
void   browser_window_reload(browser_window *bw);
void   browser_window_stop(browser_window *bw);
void   browser_window_home(browser_window *bw);
nsurl *browser_window_get_url(browser_window *bw);
const char *browser_window_get_title(browser_window *bw);
const char *browser_window_get_status(browser_window *bw);
int    browser_window_get_scroll(browser_window *bw);
int    browser_window_content_height(browser_window *bw);
int    browser_window_is_loading(browser_window *bw);
content *browser_window_get_content(browser_window *bw);
/* hover: raw href under content-space point, resolved into out (absolute) */
int    browser_window_link_at(browser_window *bw, int cx, int cy,
                              char *out, uint32_t cap);

/* ============================ hotlist (desktop/hotlist) ================== */
void hotlist_init(void);
int  hotlist_add(const char *url, const char *title);
int  hotlist_remove(const char *url);
int  hotlist_has(const char *url);
int  hotlist_count(void);
int  hotlist_get(int i, char *url, uint32_t ucap, char *title, uint32_t tcap);

/* ============================ fbtk (framebuffer toolkit) ================= */
enum fbtk_type { FBTK_FILL = 0, FBTK_LABEL, FBTK_BUTTON, FBTK_TEXT, FBTK_THROBBER };

typedef struct fbtk_widget {
    int   type;
    int   x, y, w, h;          /* relative to the toolbar origin            */
    char  text[256];           /* label / button glyph / text-entry buffer  */
    int   caret;               /* text-entry caret index                    */
    int   enabled;             /* greyed out when 0                         */
    int   pressed;             /* visual press state                        */
    int   id;                  /* caller tag (toolbar action id)            */
    uint32_t accent;
} fbtk_widget;

fbtk_widget *fbtk_create(int type, int id, int x, int y, int w, int h,
                         const char *text);
void         fbtk_set_text(fbtk_widget *wd, const char *s);
void         fbtk_set_enabled(fbtk_widget *wd, int en);
void         fbtk_redraw(fbtk_widget *wd, int ox, int oy, int throb_frame);
int          fbtk_hit(fbtk_widget *wd, int ox, int oy, int px, int py);

/* ============================ frontend entry points ===================== */
/* Driven by kernel/app_oldbrowser.c window callbacks. */
void ob_gui_init(void);
void ob_gui_draw(int cx, int cy, int cw, int ch);
void ob_gui_click(int lx, int ly);     /* client-local                      */
void ob_gui_key(char c);
void ob_gui_scroll(int delta);
void ob_gui_tick(void);

/* small shared logging shim (utils/log.h) */
void ob_log(const char *fmt, ...);
