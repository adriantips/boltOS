/* ===========================================================================
 *  BoltOS  -  kernel/app_browser.c
 *  A small web browser window. Loads http:// pages over the BoltOS TCP/DNS/HTTP
 *  stack (no TLS, so http only) and local files from the ramfs, flattens the
 *  HTML with html_parse(), then word-wraps and paints the run list. Links are
 *  clickable; an address bar drives navigation; Back walks history.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "html.h"
#include "http.h"
#include "net.h"
#include "netif.h"
#include "fs.h"
#include "string.h"
#include "kheap.h"
#include "pit.h"
#include "mouse.h"             /* mouse_x / mouse_y for hover */
#include "commands.h"          /* sh_utoa */

#define TOOLBAR_H  36
#define STATUS_H   20
#define SBW        12
#define MAXHIST    24
#define HTTP_CAP   (256 * 1024)

typedef struct { int x, y, w, h, link; } hitrect;

typedef struct {
    char      url[256];
    char      addr[256];
    int       addr_len;
    int       editing;
    html_doc *doc;
    int       scroll, content_h;
    char      status[120];
    int       cw, ch;
    hitrect   hits[600];
    int       nhits;
    char      history[MAXHIST][256];
    int       histlen;
} browser_t;

static browser_t B;
static window_t *BW;

/* ---- small string helpers ---------------------------------------------- */
static void scopy(char *d, const char *s, uint32_t cap) { strncpy(d, s, cap); d[cap - 1] = 0; }
static void sappend(char *d, const char *s, uint32_t cap) {
    uint32_t n = (uint32_t)strlen(d);
    while (*s && n < cap - 1) d[n++] = *s++;
    d[n] = 0;
}
static void set_status(browser_t *st, const char *s) { scopy(st->status, s, sizeof(st->status)); }

/* ---- style metrics ----------------------------------------------------- */
static void metrics(uint8_t style, int *scale, int *lh, uint32_t *col) {
    switch (style) {
    case HSTYLE_H1: *scale = 2; *lh = 34; *col = 0xFFFFFF; break;
    case HSTYLE_H2: *scale = 2; *lh = 28; *col = 0xF0F0F4; break;
    case HSTYLE_H3: *scale = 1; *lh = 22; *col = 0xFFFFFF; break;
    case HSTYLE_BOLD:*scale = 1; *lh = 16; *col = 0xFFFFFF; break;
    case HSTYLE_LINK:*scale = 1; *lh = 16; *col = COL_ACCENT; break;
    case HSTYLE_PRE: *scale = 1; *lh = 16; *col = 0x9FE0A0; break;
    default:         *scale = 1; *lh = 16; *col = 0xC8C8D0; break;
    }
}

/* ---- layout + paint of the page body ----------------------------------- *
 * One pass over the run list, word-wrapping into the content rect. When draw
 * is 0 it only measures (to learn total height); when 1 it paints and records
 * link hit rects. Coordinates are client-relative; cx/cy add the window origin. */
static void layout(browser_t *st, int cx, int cy, int draw) {
    int X0 = 10, Xr = st->cw - SBW - 12;
    int Y0 = TOOLBAR_H + 4, Yb = st->ch - STATUS_H - 2;
    if (Xr < X0 + 40) Xr = X0 + 40;

    if (draw) st->nhits = 0;
    int pen_x = X0, pen_y = Y0 - st->scroll, line_h = 16;

    html_doc *d = st->doc;
    for (int ri = 0; d && ri < d->nruns; ri++) {
        html_run *r = &d->runs[ri];
        int scale, lh; uint32_t col;
        metrics(r->style, &scale, &lh, &col);
        int sw = 8 * scale;

        if (r->brk) {
            pen_y += line_h;
            if (r->brk >= 2) pen_y += line_h / 2;
            pen_x = X0; line_h = lh;
        }
        if (lh > line_h) line_h = lh;

        const char *s = r->text;
        while (*s) {
            while (*s == ' ') { pen_x += sw; s++; if (pen_x > Xr) { pen_y += line_h; pen_x = X0; line_h = lh; } }
            if (!*s) break;
            const char *w = s; int wl = 0;
            while (*s && *s != ' ') { s++; wl++; }
            int ww = wl * sw;
            if (pen_x + ww > Xr && pen_x > X0) { pen_y += line_h; pen_x = X0; line_h = lh; }
            if (lh > line_h) line_h = lh;

            if (draw && pen_y + lh >= Y0 && pen_y <= Yb) {
                int ax = cx + pen_x, ay = cy + pen_y;
                for (int k = 0; k < wl; k++) g_char(ax + k * sw, ay, w[k], col, scale);
                if (r->style == HSTYLE_LINK) {
                    g_hline(ax, ay + 8 * scale + 1, ww, col);
                    if (st->nhits < (int)(sizeof(st->hits) / sizeof(st->hits[0])))
                        st->hits[st->nhits++] = (hitrect){ pen_x, pen_y, ww, lh, r->link };
                }
            }
            pen_x += ww;
        }
    }
    st->content_h = (pen_y + st->scroll + line_h) - Y0;   /* total, scroll-independent */
}

/* ---- URL / link resolution --------------------------------------------- */
static int is_http(const char *u) { return strncmp(u, "http://", 7) == 0; }

/* extract "host" (and optional port) from an http url into out */
static void url_host(const char *url, char *out, uint32_t cap) {
    const char *s = url; if (is_http(s)) s += 7;
    uint32_t i = 0; while (*s && *s != '/' && i < cap - 1) out[i++] = *s++;
    out[i] = 0;
}

/* resolve href (possibly relative) against the current page url */
static void resolve_link(browser_t *st, const char *href, char *out, uint32_t cap) {
    if (is_http(href) || strncmp(href, "https://", 8) == 0) { scopy(out, href, cap); return; }
    if (href[0] == '#') { scopy(out, st->url, cap); return; }

    if (is_http(st->url)) {
        char host[160]; url_host(st->url, host, sizeof(host));
        out[0] = 0; sappend(out, "http://", cap); sappend(out, host, cap);
        if (href[0] == '/') { sappend(out, href, cap); return; }
        /* relative to current directory */
        const char *path = st->url + 7; const char *slash = strrchr(path, '/');
        if (slash && slash > path) { /* copy dir part after host */
            char dir[256]; uint32_t n = 0; const char *p = strchr(path, '/');
            for (; p && p <= slash && n < sizeof(dir) - 1; p++) dir[n++] = *p;
            dir[n] = 0; sappend(out, dir, cap);
        } else sappend(out, "/", cap);
        sappend(out, href, cap);
        return;
    }
    /* local file base */
    if (href[0] == '/') { scopy(out, href, cap); return; }
    char dir[256]; scopy(dir, st->url, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else dir[0] = 0;
    out[0] = 0; sappend(out, dir, cap); sappend(out, href, cap);
}

/* ---- loading ----------------------------------------------------------- */
static int ends_with(const char *s, const char *suf) {
    uint32_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

static void load_url(browser_t *st, const char *url, int depth);

static int load_http(browser_t *st, const char *url, int depth) {
    set_status(st, "Connecting...");
    char *buf = (char *)kmalloc(HTTP_CAP);
    if (!buf) { set_status(st, "out of memory"); return 0; }

    int code = 0; char loc[256];
    int blen = http_get(url, buf, HTTP_CAP, &code, loc, sizeof(loc));
    if (blen < 0) {
        if (code == -2)      set_status(st, "https is not supported (http only)");
        else if (code == -3) set_status(st, "DNS lookup failed");
        else if (code == -4) set_status(st, "connection failed / timed out");
        else                 set_status(st, "request failed");
        kfree(buf);
        return 0;
    }
    if (code >= 300 && code < 400 && loc[0] && depth < 4) {
        char rurl[300]; resolve_link(st, loc, rurl, sizeof(rurl));
        kfree(buf);
        load_url(st, rurl, depth + 1);
        return 1;
    }

    if (st->doc) { html_free(st->doc); st->doc = 0; }
    st->doc = html_parse(buf, (uint32_t)blen);
    kfree(buf);
    st->scroll = 0;
    scopy(st->url, url, sizeof(st->url));

    char num[16]; sh_utoa((uint64_t)blen, num);
    char s[120]; s[0] = 0;
    char cs[12]; sh_utoa((uint64_t)code, cs);
    sappend(s, "HTTP ", sizeof(s)); sappend(s, cs, sizeof(s));
    sappend(s, "  -  ", sizeof(s)); sappend(s, num, sizeof(s)); sappend(s, " bytes", sizeof(s));
    if (st->doc && st->doc->title) { sappend(s, "  -  ", sizeof(s)); sappend(s, st->doc->title, sizeof(s)); }
    set_status(st, s);
    if (st->doc && st->doc->title) scopy(BW->title, st->doc->title, sizeof(BW->title));
    return 1;
}

static int load_fs(browser_t *st, const char *path) {
    const char *p = path;
    if (strncmp(p, "file:", 5) == 0) p += 5;
    fs_node *n = fs_lookup(p);
    if (!n) { char s[120]; s[0]=0; sappend(s,"not found: ",sizeof(s)); sappend(s,p,sizeof(s)); set_status(st, s); return 0; }
    if (n->is_dir) { set_status(st, "that is a directory, not a file"); return 0; }

    if (st->doc) { html_free(st->doc); st->doc = 0; }
    if (ends_with(n->name, ".html") || ends_with(n->name, ".htm"))
        st->doc = html_parse((const char *)n->data, n->size);
    else
        st->doc = html_parse_text((const char *)n->data, n->size);
    st->scroll = 0;
    scopy(st->url, p, sizeof(st->url));
    scopy(BW->title, n->name, sizeof(BW->title));

    char num[16]; sh_utoa((uint64_t)n->size, num);
    char s[120]; s[0] = 0;
    sappend(s, "local file  -  ", sizeof(s)); sappend(s, num, sizeof(s)); sappend(s, " bytes", sizeof(s));
    set_status(st, s);
    return 1;
}

static void load_url(browser_t *st, const char *url, int depth) {
    /* trim leading spaces */
    while (*url == ' ') url++;
    if (!*url) return;

    int ok;
    if (is_http(url) || strncmp(url, "https://", 8) == 0) {
        ok = load_http(st, url, depth);
    } else if (url[0] == '/' || strncmp(url, "file:", 5) == 0) {
        ok = load_fs(st, url);
    } else {
        /* host-looking (a dot before any slash) -> http, else local */
        const char *slash = strchr(url, '/');
        const char *dot   = strchr(url, '.');
        if (dot && (!slash || dot < slash)) {
            char full[300]; full[0] = 0; sappend(full, "http://", sizeof(full)); sappend(full, url, sizeof(full));
            ok = load_http(st, full, depth);
        } else {
            ok = load_fs(st, url);
        }
    }
    if (ok) { scopy(st->addr, st->url, sizeof(st->addr)); st->addr_len = (int)strlen(st->addr); st->editing = 0; }
}

static void navigate(browser_t *st, const char *url) {
    if (st->url[0] && st->histlen < MAXHIST) scopy(st->history[st->histlen++], st->url, 256);
    load_url(st, url, 0);
}
static void go_back(browser_t *st) {
    if (st->histlen <= 0) { set_status(st, "no history"); return; }
    char prev[256]; scopy(prev, st->history[--st->histlen], sizeof(prev));
    load_url(st, prev, 0);
}

/* ---- drawing ----------------------------------------------------------- */
static void draw_button(int x, int y, int w, int h, const char *label, int hot, uint32_t accent) {
    g_round(x, y, w, h, 5, hot ? 0x33334A : 0x24242E, 255);
    g_rect(x, y, w, h, hot ? accent : 0x3A3A48);
    int tw = g_text_width(label, 1);
    g_text(x + (w - tw) / 2, y + (h - 8) / 2, label, COL_TEXT, 1);
}

static void browser_draw(window_t *w, int cx, int cy, int cw, int ch) {
    browser_t *st = &B;
    st->cw = cw; st->ch = ch;

    g_fill(cx, cy, cw, ch, 0x0F0F16);
    /* toolbar */
    g_fill(cx, cy, cw, TOOLBAR_H, COL_PANEL_2);
    g_hline(cx, cy + TOOLBAR_H, cw, 0x33333F);

    int mxp = mouse_x() - cx, myp = mouse_y() - cy;
    int back_hot = mxp >= 8 && mxp < 38 && myp >= 7 && myp < 29;
    draw_button(cx + 8, cy + 7, 30, 22, "<", back_hot, w->accent);

    int addr_x = 44, addr_w = cw - 44 - 64 - 8;
    if (addr_w < 60) addr_w = 60;
    g_round(cx + addr_x, cy + 7, addr_w, 22, 5, 0x16161E, 255);
    g_rect(cx + addr_x, cy + 7, addr_w, 22, st->editing ? w->accent : 0x33333F);
    const char *shown = st->editing ? st->addr : st->url;
    /* clip the url to the bar width */
    int maxchars = (addr_w - 12) / 8;
    int slen = (int)strlen(shown);
    int off = (st->editing && slen > maxchars) ? slen - maxchars : 0;
    g_text(cx + addr_x + 6, cy + 14, shown + off, st->editing ? COL_TEXT : COL_TEXT_DIM, 1);
    if (st->editing && (pit_ticks() / 500) % 2 == 0) {
        int cxp = cx + addr_x + 6 + (slen - off) * 8;
        g_fill(cxp, cy + 11, 2, 14, COL_ACCENT);
    }

    int go_x = cw - 64;
    int go_hot = mxp >= go_x && mxp < go_x + 56 && myp >= 7 && myp < 29;
    draw_button(cx + go_x, cy + 7, 56, 22, "Go", go_hot, w->accent);

    /* page body (clipped to the content rect) */
    int Y0 = TOOLBAR_H, Yb = ch - STATUS_H;
    g_set_clip(cx, cy + Y0, cw, Yb - Y0);
    if (st->doc) layout(st, cx, cy, 1);
    else g_text(cx + 14, cy + Y0 + 12, "No page loaded.", COL_TEXT_DIM, 1);
    g_set_clip(cx, cy, cw, ch);

    /* scrollbar */
    int track_y = cy + Y0, track_h = Yb - Y0;
    int vh = Yb - Y0;
    g_fill(cx + cw - SBW - 4, track_y, SBW, track_h, 0x16161E);
    if (st->content_h > vh) {
        int thumb_h = vh * vh / st->content_h; if (thumb_h < 20) thumb_h = 20;
        int maxscroll = st->content_h - vh;
        int thumb_y = track_y + (track_h - thumb_h) * st->scroll / (maxscroll ? maxscroll : 1);
        g_round(cx + cw - SBW - 3, thumb_y, SBW - 2, thumb_h, 3, COL_ACCENT_DIM, 255);
    }

    /* status bar */
    g_fill(cx, cy + ch - STATUS_H, cw, STATUS_H, COL_PANEL_2);
    g_hline(cx, cy + ch - STATUS_H, cw, 0x33333F);
    struct netif *nif = netif_default();
    char link[40]; link[0] = 0;
    sappend(link, nif ? (nif->link_up ? "online" : "link down") : "no NIC", sizeof(link));
    g_text(cx + 8, cy + ch - STATUS_H + 6, st->status, COL_TEXT_DIM, 1);
    g_text(cx + cw - g_text_width(link, 1) - 10, cy + ch - STATUS_H + 6, link, nif && nif->link_up ? COL_GOOD : COL_BAD, 1);
}

/* ---- input ------------------------------------------------------------- */
static void browser_key(window_t *w, char c) {
    (void)w;
    browser_t *st = &B;
    if (st->editing) {
        if (c == '\n')      { navigate(st, st->addr); }
        else if (c == 27)   { st->editing = 0; }
        else if (c == '\b') { if (st->addr_len > 0) st->addr[--st->addr_len] = 0; }
        else if ((unsigned char)c >= 32 && st->addr_len < (int)sizeof(st->addr) - 1) {
            st->addr[st->addr_len++] = c; st->addr[st->addr_len] = 0;
        }
        return;
    }
    int vh = st->ch - TOOLBAR_H - STATUS_H;
    int maxscroll = st->content_h - vh; if (maxscroll < 0) maxscroll = 0;
    if      (c == ' ')  st->scroll += vh - 24;
    else if (c == 'b')  st->scroll -= vh - 24;
    else if (c == 'j')  st->scroll += 32;
    else if (c == 'k')  st->scroll -= 32;
    else if (c == '/')  { st->editing = 1; st->addr_len = 0; st->addr[0] = 0; }
    if (st->scroll < 0) st->scroll = 0;
    if (st->scroll > maxscroll) st->scroll = maxscroll;
}

static void browser_click(window_t *w, int lx, int ly) {
    (void)w;
    browser_t *st = &B;
    int cw = st->cw, ch = st->ch;

    if (ly < TOOLBAR_H) {                              /* toolbar */
        if (lx >= 8 && lx < 38) { go_back(st); return; }
        int go_x = cw - 64;
        if (lx >= go_x && lx < go_x + 56) { navigate(st, st->addr); return; }
        int addr_x = 44, addr_w = cw - 44 - 64 - 8;
        if (lx >= addr_x && lx < addr_x + addr_w) {
            st->editing = 1; scopy(st->addr, st->url, sizeof(st->addr)); st->addr_len = (int)strlen(st->addr);
        }
        return;
    }

    int Y0 = TOOLBAR_H, Yb = ch - STATUS_H, vh = Yb - Y0;
    if (lx >= cw - SBW - 4) {                          /* scrollbar: jump */
        int maxscroll = st->content_h - vh; if (maxscroll < 0) maxscroll = 0;
        int rel = ly - Y0; if (rel < 0) rel = 0; if (rel > vh) rel = vh;
        st->scroll = maxscroll * rel / (vh ? vh : 1);
        return;
    }

    st->editing = 0;                                   /* click in body */
    for (int i = 0; i < st->nhits; i++) {
        hitrect *h = &st->hits[i];
        if (lx >= h->x && lx < h->x + h->w && ly >= h->y && ly < h->y + h->h) {
            if (st->doc && h->link >= 0 && h->link < st->doc->nhrefs) {
                char rurl[300]; resolve_link(st, st->doc->hrefs[h->link], rurl, sizeof(rurl));
                navigate(st, rurl);
            }
            return;
        }
    }
}

/* ---- a built-in start page + a sample local file ----------------------- */
static const char WELCOME[] =
    "<title>BoltOS Browser</title>"
    "<h1>BoltOS Browser</h1>"
    "<p>A tiny web browser running on a from-scratch kernel. It speaks HTTP over "
    "the BoltOS TCP/IP stack (no TLS yet, so <b>http://</b> only) and can open "
    "local HTML files from the ramfs.</p>"
    "<h2>Try it</h2>"
    "<ul>"
    "<li>Type a URL in the bar and press <b>Go</b> (e.g. <a href=\"http://example.com\">http://example.com</a>)</li>"
    "<li>Open the bundled page: <a href=\"/web/index.html\">/web/index.html</a></li>"
    "<li>Scroll with the scrollbar, or Space / b, or the j / k keys</li>"
    "<li>Click <b>&lt;</b> to go back</li>"
    "</ul>"
    "<h2>Notes</h2>"
    "<p>The data path uses the e1000 NIC (the link QEMU/VirtualBox NAT exposes). "
    "Wi-Fi association is scaffolded in the kernel but needs a radio driver; see "
    "the <b>wifi</b> shell command for status.</p>";

static const char SAMPLE[] =
    "<title>Sample Page</title>"
    "<h1>Hello from the ramfs</h1>"
    "<p>This file lives at <b>/web/index.html</b> and is rendered by the BoltOS "
    "HTML engine without touching the network.</p>"
    "<h3>A short list</h3>"
    "<ul><li>headings</li><li>paragraphs</li><li>links and <b>bold</b> text</li></ul>"
    "<p>Back to the <a href=\"/web/index.html\">start</a> or visit "
    "<a href=\"http://example.com\">example.com</a>.</p>";

static void seed_sample_file(void) {
    if (fs_lookup("/web")) return;
    fs_node *dir = fs_create("/web", 1);
    if (!dir) return;
    fs_node *f = fs_create("/web/index.html", 0);
    if (f) fs_write(f, SAMPLE, (uint32_t)(sizeof(SAMPLE) - 1));
}

void browser_app_init(void) {
    memset(&B, 0, sizeof(B));
    seed_sample_file();

    window_t *w = gui_add_window("Browser", 780, 540, 0x66BB6A, ICON_BROWSER);
    if (!w) return;
    BW = w;
    w->draw  = browser_draw;
    w->key   = browser_key;
    w->click = browser_click;

    B.doc = html_parse(WELCOME, (uint32_t)(sizeof(WELCOME) - 1));
    scopy(B.url, "about:welcome", sizeof(B.url));
    scopy(B.addr, "", sizeof(B.addr));
    set_status(&B, "Welcome - enter a URL or open a local file");
}
