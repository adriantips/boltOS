/* ===========================================================================
 *  BoltOS  -  kernel/app_browser.c
 *  A small web browser window. Loads http:// and https:// pages over the BoltOS
 *  TCP/DNS/TLS/HTTP stack and local files from the ramfs, flattens the
 *  HTML with html_parse(), then word-wraps and paints the run list. Links are
 *  clickable; an address bar drives navigation; Back walks history.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "html.h"
#include "js.h"
#include "image.h"
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
#define IMG_FETCH_CAP (1536 * 1024)   /* per-image download buffer            */
#define IMG_BUDGET    (7 * 1024 * 1024) /* total resident decoded-image budget */
#define IMG_TIME_BUDGET 14000           /* ms (PIT@1kHz) max spent fetching images */
#define MAXIMG     256
#define INDENT_STEP 22

typedef struct { int x, y, w, h, link, run, kind; } hitrect;  /* kind: 0 link, 1 input */

#define MAXFIELDS 32

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
    image_t  *owned[MAXIMG];   /* decoded images backing the current doc */
    int       nowned;
    /* form-field editing: a buffer per text input/textarea, keyed by run index */
    char      fld[MAXFIELDS][96];
    int       fld_run[MAXFIELDS];
    int       nfld;
    int       focus_run;       /* run index of the focused field, or -1 */
    char     *js_str[96];      /* strings allocated by page scripts (innerHTML=...) */
    int       njs;
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

/* ---- form field edit buffers ------------------------------------------- */
static void fields_reset(browser_t *st) { st->nfld = 0; st->focus_run = -1; }
/* buffer for a field run, or 0 if none exists yet */
static char *field_find(browser_t *st, int run) {
    for (int i = 0; i < st->nfld; i++) if (st->fld_run[i] == run) return st->fld[i];
    return 0;
}
/* buffer for a field run, allocating one (empty) on first use, or 0 if full */
static char *field_get(browser_t *st, int run) {
    char *b = field_find(st, run);
    if (b) return b;
    if (st->nfld >= MAXFIELDS) return 0;
    st->fld_run[st->nfld] = run; st->fld[st->nfld][0] = 0;
    return st->fld[st->nfld++];
}

/* ---- page palette ------------------------------------------------------ *
 * Web pages are authored for a white canvas with dark text, so the body is
 * painted light regardless of the (dark) OS window chrome. */
#define WEB_BG    0xFFFFFF      /* page background                          */
#define WEB_TEXT  0x202124      /* body text (Google near-black grey)       */
#define WEB_DIM   0x5F6368      /* secondary / italic grey                  */
#define WEB_LINK  0x1A73E8      /* anchor blue                              */
#define WEB_CODE  0x0B7D3E      /* monospace / preformatted green           */
#define WEB_PLACE 0xE8E8EC      /* missing-image placeholder fill           */

/* ---- style metrics ----------------------------------------------------- */
static void metrics(uint8_t style, int *scale, int *lh, uint32_t *col, int *italic) {
    *italic = 0;
    switch (style) {
    case HSTYLE_H1: *scale = 2; *lh = 36; *col = WEB_TEXT; break;
    case HSTYLE_H2: *scale = 2; *lh = 30; *col = WEB_TEXT; break;
    case HSTYLE_H3: *scale = 1; *lh = 24; *col = WEB_TEXT; break;
    case HSTYLE_BOLD:*scale = 1; *lh = 18; *col = WEB_TEXT; break;
    case HSTYLE_LINK:*scale = 1; *lh = 18; *col = WEB_LINK; break;
    case HSTYLE_PRE: *scale = 1; *lh = 17; *col = WEB_CODE; break;
    case HSTYLE_CODE:*scale = 1; *lh = 18; *col = WEB_CODE; break;
    case HSTYLE_ITALIC:*scale = 1; *lh = 18; *col = WEB_DIM; *italic = 1; break;
    default:         *scale = 1; *lh = 18; *col = WEB_TEXT; break;
    }
}

/* ---- layout + paint of the page body ----------------------------------- *
 * Line-based: words and images accumulate into a line buffer, then each line is
 * flushed with its alignment (left/center/right) and indent applied. Supports
 * per-run colour, headings, links and inline/block images. When draw is 0 it
 * only measures (to learn total height); when 1 it paints + records hit rects.
 * Coordinates are client-relative; cx/cy add the window origin. */
typedef struct {
    int      x, w, scale, lh;
    uint32_t color;
    int      link;
    const char *text; int len;     /* text item */
    int      is_img; image_t *pix; int dw, dh; /* image item */
    int      italic;
    int      is_input, subtype;    /* form control: 0 field,1 button,2 area,3 select */
    uint32_t bg;                   /* element background: HCOL_NONE or 0x1RRGGBB */
    int      run;                  /* source run index (for input focus/hit) */
} litem;

#define MAXLINE 320
static litem LINE[MAXLINE];

static void emit_line(browser_t *st, int cx, int cy, int draw, int *pen_y,
                      int n, int line_h, int xstart, int avail, uint8_t align, int Y0, int Yb) {
    int py = *pen_y;
    if (n > 0) {
        int linew = LINE[n-1].x + LINE[n-1].w;
        int off = 0;
        if (align == HALIGN_CENTER)      off = (avail - linew) / 2;
        else if (align == HALIGN_RIGHT)  off = avail - linew;
        if (off < 0) off = 0;
        if (draw && py + line_h >= Y0 && py <= Yb) {
            uint32_t lbg = LINE[0].bg;          /* block background band for the line */
            if (lbg & 0x1000000) g_fill(cx + xstart, cy + py, avail, line_h, lbg & 0xFFFFFF);
            for (int k = 0; k < n; k++) {
                litem *it = &LINE[k];
                int ax = cx + xstart + off + it->x;
                if (it->is_input) {
                    int focused = (st->focus_run == it->run);
                    uint32_t fill = it->subtype == 1 ? 0xE8F0FE : 0xFFFFFF;
                    uint32_t brd  = focused ? WEB_LINK : (it->subtype == 1 ? WEB_LINK : 0xC0C0C8);
                    g_round(ax, cy + py, it->dw, it->dh, 5, fill, 255);
                    g_rect(ax, cy + py, it->dw, it->dh, brd);
                    int ty = cy + py + (it->dh - 8) / 2;
                    if (it->subtype == 1) {                    /* button: centred label */
                        int tw = g_text_width_pn(it->text, it->len, 1);
                        g_text_pn(ax + (it->dw - tw) / 2, ty, it->text, it->len, WEB_LINK, 1, 0);
                    } else {                                   /* field / textarea / select */
                        int tyo = it->subtype == 2 ? cy + py + 5 : ty;
                        char *val = field_find(st, it->run);   /* typed text, if any */
                        if (val && val[0]) {
                            int vl = (int)strlen(val);
                            g_text_pn(ax + 6, tyo, val, vl, WEB_TEXT, 1, 0);
                            if (focused && (pit_ticks() / 500) % 2 == 0)
                                g_fill(ax + 6 + g_text_width_pn(val, vl, 1), tyo - 1, 2, 11, WEB_LINK);
                        } else {
                            if (it->len) g_text_pn(ax + 6, tyo, it->text, it->len, WEB_DIM, 1, 0);
                            if (focused && (pit_ticks() / 500) % 2 == 0)
                                g_fill(ax + 6, tyo - 1, 2, 11, WEB_LINK);
                        }
                        if (it->subtype == 3) g_text(ax + it->dw - 12, ty, "v", WEB_DIM, 1);
                    }
                    if (st->nhits < (int)(sizeof(st->hits)/sizeof(st->hits[0])))
                        st->hits[st->nhits++] = (hitrect){ xstart+off+it->x, py, it->dw, it->dh, it->link, it->run, 1 };
                } else if (it->is_img) {
                    if (it->pix) g_blit(ax, cy + py, it->dw, it->dh, it->pix->px, it->pix->w, it->pix->h);
                    else {
                        g_fill(ax, cy + py, it->dw, it->dh, WEB_PLACE);
                        g_rect(ax, cy + py, it->dw, it->dh, 0xC0C0C8);
                        g_text(ax + 4, cy + py + it->dh/2 - 4, it->text, WEB_DIM, 1);
                    }
                    if (it->link >= 0 && st->nhits < (int)(sizeof(st->hits)/sizeof(st->hits[0])))
                        st->hits[st->nhits++] = (hitrect){ xstart+off+it->x, py, it->dw, it->dh, it->link, it->run, 0 };
                } else {
                    /* inline background highlight (e.g. <code>, <mark>) when the
                     * line has no full-width block band of the same colour */
                    if ((it->bg & 0x1000000) && it->bg != lbg)
                        g_fill(ax - 2, cy + py, it->w + 4, line_h, it->bg & 0xFFFFFF);
                    g_text_pn(ax, cy + py, it->text, it->len, it->color, it->scale, it->italic);
                    if (it->link >= 0) {
                        g_hline(ax, cy + py + 8 * it->scale + 1, it->w, it->color);
                        if (st->nhits < (int)(sizeof(st->hits)/sizeof(st->hits[0])))
                            st->hits[st->nhits++] = (hitrect){ xstart+off+it->x, py, it->w, line_h, it->link, it->run, 0 };
                    }
                }
            }
        }
        *pen_y = py + line_h;
    }
}

static void layout(browser_t *st, int cx, int cy, int draw) {
    int X0 = 10, Xr = st->cw - SBW - 12;
    int Y0 = TOOLBAR_H + 4, Yb = st->ch - STATUS_H - 2;
    if (Xr < X0 + 60) Xr = X0 + 60;
    if (draw) st->nhits = 0;

    int pen_y = Y0 - st->scroll;
    int n = 0, cur_x = 0, line_h = 0;
    int xstart = X0, avail = Xr - X0;
    uint8_t align = HALIGN_LEFT;

    html_doc *d = st->doc;
    for (int ri = 0; d && ri < d->nruns; ri++) {
        html_run *r = &d->runs[ri];
        int scale, lh, italic; uint32_t dcol;
        metrics(r->style, &scale, &lh, &dcol, &italic);
        /* dark-theme support: default body text follows the page text colour */
        if (dcol == WEB_TEXT && st->doc && (st->doc->page_fg & 0x1000000)) dcol = st->doc->page_fg & 0xFFFFFF;
        uint32_t color = (r->color & 0x1000000) ? (r->color & 0xFFFFFF) : dcol;

        if (r->brk) {
            if (n > 0) emit_line(st, cx, cy, draw, &pen_y, n, line_h, xstart, avail, align, Y0, Yb);
            else       pen_y += 16;
            if (r->brk >= 2) pen_y += 8;
            n = 0; cur_x = 0; line_h = 0;
        }

        align  = r->align;
        xstart = X0 + r->indent * INDENT_STEP;
        avail  = Xr - xstart; if (avail < 60) avail = 60;

        if (r->kind == HRUN_IMG) {
            image_t *im = r->pix;
            int dw = im ? im->w : (r->iw > 0 ? r->iw : 140);
            int dh = im ? im->h : (r->ih > 0 ? r->ih : 90);
            if (dw > avail) { dh = dh * avail / (dw ? dw : 1); dw = avail; }
            if (dw < 1) dw = 1; if (dh < 1) dh = 1;
            if (cur_x > 0 && cur_x + dw > avail) {
                emit_line(st, cx, cy, draw, &pen_y, n, line_h, xstart, avail, align, Y0, Yb);
                n = 0; cur_x = 0; line_h = 0;
            }
            if (n < MAXLINE) {
                LINE[n] = (litem){ cur_x, dw, 1, dh, color, r->link, r->text, (int)strlen(r->text), 1, im, dw, dh, 0, 0, 0, r->bg, ri };
                n++;
            }
            cur_x += dw + 4;
            if (dh > line_h) line_h = dh;
            continue;
        }

        if (r->kind == HRUN_INPUT) {
            int sub = r->img;
            int tlen = (int)strlen(r->text);
            int dw = r->iw > 0 ? r->iw
                   : sub == 1 ? g_text_width_pn(r->text, tlen, 1) + 24
                   : sub == 2 ? 240 : 160;
            int dh = r->ih > 0 ? r->ih : 22;
            if (dw > avail) dw = avail;
            if (dw < 1) dw = 1; if (dh < 1) dh = 1;
            if (cur_x > 0 && cur_x + dw > avail) {
                emit_line(st, cx, cy, draw, &pen_y, n, line_h, xstart, avail, align, Y0, Yb);
                n = 0; cur_x = 0; line_h = 0;
            }
            if (n < MAXLINE) {
                LINE[n] = (litem){ cur_x, dw, 1, dh, color, r->link, r->text, tlen, 0, 0, dw, dh, 0, 1, sub, r->bg, ri };
                n++;
            }
            cur_x += dw + 6;
            if (dh > line_h) line_h = dh;
            continue;
        }

        int spacew = g_glyph_adv(' ', scale);
        const char *s = r->text;
        while (*s) {
            while (*s == ' ') s++;
            if (!*s) break;
            const char *w = s; int wl = 0;
            while (*s && *s != ' ') { s++; wl++; }
            int ww = g_text_width_pn(w, wl, scale);
            int space = cur_x > 0 ? spacew : 0;
            if (cur_x > 0 && cur_x + space + ww > avail) {
                emit_line(st, cx, cy, draw, &pen_y, n, line_h, xstart, avail, align, Y0, Yb);
                n = 0; cur_x = 0; line_h = 0; space = 0;
            }
            cur_x += space;
            if (n < MAXLINE) {
                LINE[n] = (litem){ cur_x, ww, scale, lh, color, r->link, w, wl, 0, 0, 0, 0, italic, 0, 0, r->bg, ri };
                n++;
            }
            cur_x += ww;
            if (lh > line_h) line_h = lh;
        }
    }
    if (n > 0) emit_line(st, cx, cy, draw, &pen_y, n, line_h, xstart, avail, align, Y0, Yb);
    st->content_h = (pen_y + st->scroll) - Y0;
}

/* ---- URL / link resolution --------------------------------------------- */
static int is_http(const char *u)  { return strncmp(u, "http://", 7) == 0; }
static int is_https(const char *u) { return strncmp(u, "https://", 8) == 0; }
static int is_remote(const char *u) { return is_http(u) || is_https(u); }
static uint32_t scheme_len(const char *u) { return is_https(u) ? 8 : (is_http(u) ? 7 : 0); }

/* extract "host" (and optional port) from a remote url into out */
static void url_host(const char *url, char *out, uint32_t cap) {
    const char *s = url + scheme_len(url);
    uint32_t i = 0; while (*s && *s != '/' && i < cap - 1) out[i++] = *s++;
    out[i] = 0;
}

/* resolve href (possibly relative) against the current page url */
static void resolve_link(browser_t *st, const char *href, char *out, uint32_t cap) {
    if (is_remote(href)) { scopy(out, href, cap); return; }
    if (href[0] == '/' && href[1] == '/') {            /* protocol-relative //host/path */
        out[0] = 0;
        sappend(out, is_https(st->url) ? "https:" : "http:", cap);
        sappend(out, href, cap);
        return;
    }
    if (href[0] == '#') { scopy(out, st->url, cap); return; }

    if (is_remote(st->url)) {
        char host[160]; url_host(st->url, host, sizeof(host));
        out[0] = 0; sappend(out, is_https(st->url) ? "https://" : "http://", cap); sappend(out, host, cap);
        if (href[0] == '/') { sappend(out, href, cap); return; }
        /* relative to current directory */
        const char *path = st->url + scheme_len(st->url); const char *slash = strrchr(path, '/');
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

/* ---- image fetching ---------------------------------------------------- */
static void free_images(browser_t *st) {
    for (int i = 0; i < st->nowned; i++) if (st->owned[i]) image_free(st->owned[i]);
    st->nowned = 0;
}

/* Fetch+decode one image into the shared buffer. `psess` holds a keep-alive
 * connection reused across same-host images so a page's images cost a single
 * TLS handshake instead of one per image. */
static image_t *fetch_one(browser_t *st, const char *src,
                          uint8_t *buf, uint32_t bufcap, struct http_conn **psess) {
    char url[300]; resolve_link(st, src, url, sizeof(url));
    const uint8_t *data = 0; uint32_t dlen = 0; image_t *im = 0;
    if (is_remote(url)) {
        if (*psess && !http_conn_can_reuse(*psess, url)) { http_close_conn(*psess); *psess = 0; }
        if (!*psess) { int code; *psess = http_open(url, &code); }
        if (!*psess) return 0;
        int code = 0; char loc[256];
        int blen = http_fetch(*psess, url, (char *)buf, bufcap, &code, loc, sizeof(loc));
        if (blen < 0) {                                /* stale connection: one retry */
            http_close_conn(*psess); *psess = 0;
            int c2; *psess = http_open(url, &c2);
            if (*psess) blen = http_fetch(*psess, url, (char *)buf, bufcap, &code, loc, sizeof(loc));
        }
        if (code >= 300 && code < 400 && loc[0]) {     /* one redirect hop */
            char rurl[300]; resolve_link(st, loc, rurl, sizeof(rurl));
            if (!*psess || !http_conn_can_reuse(*psess, rurl)) {
                if (*psess) http_close_conn(*psess);
                int c3; *psess = http_open(rurl, &c3);
            }
            if (*psess) blen = http_fetch(*psess, rurl, (char *)buf, bufcap, &code, loc, sizeof(loc));
        }
        if (blen > 0) { data = buf; dlen = (uint32_t)blen; }
    } else {
        const char *p = url; if (strncmp(p, "file:", 5) == 0) p += 5;
        fs_node *nf = fs_lookup(p);
        if (nf && !nf->is_dir) { data = nf->data; dlen = nf->size; }
    }
    if (data) im = image_decode(data, dlen);
    return im;
}

/* ===========================================================================
 *  Page scripts: run captured <script> bodies through the BoltJS engine with a
 *  DOM host bound to this doc. Scripts can read/rewrite element text by id,
 *  set the title, and console.log to the status line. A DOM node handle is the
 *  1-based index of the first run carrying that id.
 * ===========================================================================*/
static void js_free_strings(browser_t *st) {
    for (int i = 0; i < st->njs; i++) if (st->js_str[i]) kfree(st->js_str[i]);
    st->njs = 0;
}
static char *js_keep(browser_t *st, const char *s) {        /* own a copy for a run->text */
    uint32_t n = (uint32_t)strlen(s);
    char *p = (char *)kmalloc(n + 1);
    if (!p) return 0;
    memcpy(p, s, n); p[n] = 0;
    if (st->njs < 96) st->js_str[st->njs++] = p;
    return p;
}
/* strip HTML tags + decode nothing fancy: innerHTML -> visible plain text */
static void strip_tags(const char *in, char *out, uint32_t cap) {
    uint32_t o = 0; int intag = 0;
    for (const char *p = in; *p && o < cap - 1; p++) {
        if (*p == '<') { intag = 1; continue; }
        if (*p == '>') { intag = 0; continue; }
        if (!intag) out[o++] = *p;
    }
    out[o] = 0;
}
static int jh_find_id(browser_t *st, const char *id) {
    html_doc *d = st->doc; if (!d) return -1;
    for (int i = 0; i < d->nruns; i++)
        if (d->runs[i].elid && strcmp(d->runs[i].elid, id) == 0) return i;
    return -1;
}
static js_dom_node jh_by_id(void *ud, const char *id) {
    browser_t *st = (browser_t *)ud; int idx = jh_find_id(st, id);
    return idx < 0 ? 0 : (js_dom_node)(uint64_t)(idx + 1);
}
static js_dom_node jh_by_tag(void *ud, const char *tag, int index) {
    (void)ud; (void)tag; (void)index; return 0;
}
static int jh_get_inner(void *ud, js_dom_node n, char *out, uint32_t cap) {
    browser_t *st = (browser_t *)ud; html_doc *d = st->doc;
    int idx = (int)(uint64_t)n - 1; if (!d || idx < 0 || idx >= d->nruns) { if(cap)out[0]=0; return 0; }
    const char *eid = d->runs[idx].elid; uint32_t o = 0; out[0] = 0;
    for (int i = idx; i < d->nruns && o < cap - 1; i++)
        if (d->runs[i].elid && eid && strcmp(d->runs[i].elid, eid) == 0)
            for (const char *p = d->runs[i].text; *p && o < cap - 1; p++) out[o++] = *p;
    out[o] = 0; return 1;
}
static void jh_set_node(browser_t *st, js_dom_node n, const char *text) {
    html_doc *d = st->doc; int idx = (int)(uint64_t)n - 1;
    if (!d || idx < 0 || idx >= d->nruns) return;
    const char *eid = d->runs[idx].elid;
    char *keep = js_keep(st, text); if (!keep) return;
    d->runs[idx].text = keep;                       /* first run gets the new text */
    if (eid) for (int i = idx + 1; i < d->nruns; i++)/* blank the rest of the element */
        if (d->runs[i].elid && strcmp(d->runs[i].elid, eid) == 0) d->runs[i].text = "";
}
static void jh_set_inner(void *ud, js_dom_node n, const char *html) {
    char plain[4096]; strip_tags(html, plain, sizeof plain);
    jh_set_node((browser_t *)ud, n, plain);
}
static void jh_set_text(void *ud, js_dom_node n, const char *text) {
    jh_set_node((browser_t *)ud, n, text);
}
static void jh_write(void *ud, const char *html) {          /* document.write -> append a run */
    browser_t *st = (browser_t *)ud; html_doc *d = st->doc;
    if (!d || d->nruns >= d->runs_cap) return;
    char plain[2048]; strip_tags(html, plain, sizeof plain);
    if (!plain[0]) return;
    char *keep = js_keep(st, plain); if (!keep) return;
    html_run *r = &d->runs[d->nruns++];
    memset(r, 0, sizeof(*r));
    r->text = keep; r->style = HSTYLE_NORMAL; r->link = -1; r->img = -1; r->brk = 1;
}
static void jh_title(void *ud, const char *t) {
    (void)ud; if (BW) scopy(BW->title, t, sizeof(BW->title));
}
static void jh_log(void *ud, const char *m) {
    browser_t *st = (browser_t *)ud; char s[120]; s[0]=0;
    sappend(s, "console: ", sizeof s); sappend(s, m, sizeof s); set_status(st, s);
}
static void run_scripts(browser_t *st) {
    html_doc *d = st->doc;
    if (!d || d->nscripts == 0) return;
    js_host host = { st, jh_by_id, jh_by_tag, jh_set_inner, jh_get_inner,
                     jh_set_text, jh_write, jh_title, jh_log };
    char err[160];
    for (int i = 0; i < d->nscripts; i++) {
        const char *src = d->scripts[i]; if (!src) continue;
        js_run(src, (uint32_t)strlen(src), &host, err, sizeof err);
    }
}

/* Resolve, download and decode every <img> in the current doc; wire decoded
 * pixels back onto the runs. Bounded by a count, a resident-memory budget and
 * a wall-clock budget so an image-heavy page can't freeze the UI for minutes.
 * Same-host images share one keep-alive connection. */
static void fetch_images(browser_t *st) {
    html_doc *d = st->doc;
    if (!d || d->nimgs == 0) return;
    image_t **cache = (image_t **)kmalloc((uint32_t)d->nimgs * sizeof(image_t *));
    if (!cache) return;
    for (int i = 0; i < d->nimgs; i++) cache[i] = 0;
    uint8_t *buf = (uint8_t *)kmalloc(IMG_FETCH_CAP);
    if (!buf) { kfree(cache); return; }

    struct http_conn *sess = 0;
    uint32_t budget = 0;
    uint64_t t0 = pit_ticks();
    for (int i = 0; i < d->nimgs; i++) {
        if (st->nowned >= MAXIMG || budget >= IMG_BUDGET) break;
        if (pit_ticks() - t0 > IMG_TIME_BUDGET) break;   /* keep the page responsive */
        set_status(st, "Loading images...");
        gui_pump();                                   /* keep the UI alive */
        image_t *im = fetch_one(st, d->imgs[i], buf, IMG_FETCH_CAP, &sess);
        if (im) {
            cache[i] = im;
            st->owned[st->nowned++] = im;
            budget += (uint32_t)im->w * im->h * 4;
        }
    }
    if (sess) http_close_conn(sess);
    kfree(buf);
    for (int ri = 0; ri < d->nruns; ri++)
        if (d->runs[ri].kind == HRUN_IMG && d->runs[ri].img >= 0 && d->runs[ri].img < d->nimgs)
            d->runs[ri].pix = cache[d->runs[ri].img];
    kfree(cache);
}

/* ===========================================================================
 *  YouTube renderer. YouTube builds its pages with JavaScript, but it also
 *  embeds the data in an `ytInitialData` JSON blob inside a <script>. We can't
 *  run that app (it needs a full browser engine + video codecs), but we CAN
 *  scrape the embedded data: pull the videoId + title for each result and emit
 *  a plain HTML list whose links point at the real /watch pages. That makes
 *  YouTube *search* show real YouTube videos; playback stays out of reach.
 * ===========================================================================*/
static int is_youtube(const char *url) {
    const char *h = url;
    if (strncmp(h,"http://",7)==0) h+=7; else if (strncmp(h,"https://",8)==0) h+=8;
    return strncmp(h,"www.youtube.com",15)==0 || strncmp(h,"youtube.com",11)==0 ||
           strncmp(h,"m.youtube.com",13)==0   || strncmp(h,"youtu.be",8)==0;
}
static const char *find_sub(const char *h, const char *end, const char *needle) {
    uint32_t nl = (uint32_t)strlen(needle);
    for (const char *p=h; p+nl<=end; p++) { uint32_t k=0; for(;k<nl;k++) if(p[k]!=needle[k])break; if(k==nl)return p; }
    return 0;
}
/* copy a JSON string value (p at first char after the opening quote) into out */
static const char *json_str(const char *p, const char *end, char *out, uint32_t cap) {
    uint32_t o=0;
    while (p<end && *p!='"' && o<cap-1) {
        if (*p=='\\' && p+1<end) {
            char e=p[1];
            if(e=='n'||e=='t'){ out[o++]=' '; p+=2; continue; }
            if(e=='"'||e=='\\'||e=='/'){ out[o++]=e; p+=2; continue; }
            if(e=='u' && p+5<end){ int v=0; for(int k=2;k<6;k++){ char c=p[k];
                    int hv=(c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:0; v=v*16+hv; }
                out[o++] = (v>=32 && v<127) ? (char)v : (v==0x26?'&':' '); p+=6; continue; }
            out[o++]=e; p+=2; continue;
        }
        out[o++]=*p++;
    }
    out[o]=0;
    return p;
}
/* Build a plain-HTML page from YouTube's embedded JSON. Returns length. */
static int youtube_build(const char *raw, uint32_t len, char *out, uint32_t outcap, const char *url) {
    const char *end = raw + len; uint32_t o = 0;
    #define YPUT(s) do { for (const char *q=(s); *q && o<outcap-1; ) out[o++]=*q++; } while(0)
    int is_watch = (find_sub(url, url+strlen(url), "/watch") != 0);

    YPUT("<html><head><title>YouTube (BoltOS)</title></head><body>");
    YPUT("<h1>YouTube</h1>");

    if (is_watch) {
        /* watch page: show title + description from videoDetails */
        const char *vd = find_sub(raw, end, "\"videoDetails\":{");
        char title[256]=""; char desc[1024]="";
        if (vd) {
            const char *tt = find_sub(vd, vd+4000<end?vd+4000:end, "\"title\":\"");
            if (tt) json_str(tt+9, end, title, sizeof title);
            const char *dd = find_sub(vd, vd+8000<end?vd+8000:end, "\"shortDescription\":\"");
            if (dd) json_str(dd+20, end, desc, sizeof desc);
        }
        YPUT("<h2>"); for(char*c=title;*c&&o<outcap-300;c++) if(*c!='<'&&*c!='>') out[o++]=*c; YPUT("</h2>");
        YPUT("<p><b>Note:</b> BoltOS shows YouTube's page data; video playback needs codecs not present.</p>");
        YPUT("<p>"); for(char*c=desc;*c&&o<outcap-300;c++) if(*c!='<'&&*c!='>') out[o++]=*c; YPUT("</p>");
        YPUT("</body></html>"); out[o]=0; return (int)o;
    }

    YPUT("<p>Real YouTube results (titles link to the watch page; playback needs codecs not present).</p>");
    char seen[48][12]; int nseen=0;
    const char *p = raw;
    while (o < outcap-400 && nseen < 40) {
        const char *vid = find_sub(p, end, "\"videoId\":\"");
        if (!vid) break;
        const char *idp = vid + 11;
        char id[12]; int k=0; while (idp+k<end && idp[k]!='"' && k<11){ id[k]=idp[k]; k++; } id[k]=0;
        p = idp + (k>0?k:1);
        if (k != 11) continue;
        int dup=0; for(int i=0;i<nseen;i++) if(strcmp(seen[i],id)==0){dup=1;break;}
        if (dup) continue;
        const char *win = idp+2500<end?idp+2500:end;
        const char *titlestart;
        const char *tt = find_sub(idp, win, "\"title\":{\"runs\":[{\"text\":\"");
        if (tt) titlestart = tt + strlen("\"title\":{\"runs\":[{\"text\":\"");
        else { tt = find_sub(idp, win, "\"text\":\""); if (!tt) continue; titlestart = tt + 8; }
        char title[200]; json_str(titlestart, end, title, sizeof title);
        if (!title[0]) continue;
        if (nseen<48){ for(int z=0;z<12;z++)seen[nseen][z]=id[z]; nseen++; }
        YPUT("<p><a href=\"https://www.youtube.com/watch?v=");
        YPUT(id); YPUT("\">");
        for (char *c=title; *c && o<outcap-200; c++) if (*c!='<' && *c!='>') out[o++]=*c;
        YPUT("</a></p>");
    }
    if (nseen==0) YPUT("<p>(no video data found)</p>");
    YPUT("</body></html>");
    out[o]=0;
    return (int)o;
}

static int load_http(browser_t *st, const char *url, int depth) {
    set_status(st, "Connecting...");
    int yt = is_youtube(url);
    /* YouTube pages are ~2 MB; the ytInitialData result list runs through the
     * first ~1.2 MB (≈20 videos). Cap the read there so a slow link finishes
     * in reasonable time instead of pulling the whole bundle. */
    uint32_t cap = yt ? (1200u*1024) : HTTP_CAP;
    char *buf = (char *)kmalloc(cap);
    if (!buf) { set_status(st, "out of memory"); return 0; }

    int code = 0; char loc[256];
    int blen = http_get(url, buf, cap, &code, loc, sizeof(loc));
    if (blen < 0) {
        if (code == -3)      set_status(st, "DNS lookup failed");
        else if (code == -4) set_status(st, "connection failed / timed out");
        else if (code == -5) set_status(st, "TLS handshake failed");
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

    free_images(st);
    js_free_strings(st);
    if (st->doc) { html_free(st->doc); st->doc = 0; }
    if (yt) {                                  /* scrape YouTube's embedded data */
        char *page = (char *)kmalloc(128 * 1024);
        if (page) {
            int n = youtube_build(buf, (uint32_t)blen, page, 128 * 1024, url);
            st->doc = html_parse(page, (uint32_t)n);
            kfree(page);
        } else st->doc = html_parse(buf, (uint32_t)blen);
    } else {
        st->doc = html_parse(buf, (uint32_t)blen);
    }
    kfree(buf);
    st->scroll = 0;
    fields_reset(st);
    scopy(st->url, url, sizeof(st->url));
    run_scripts(st);
    fetch_images(st);

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

    free_images(st);
    js_free_strings(st);
    if (st->doc) { html_free(st->doc); st->doc = 0; }
    if (ends_with(n->name, ".html") || ends_with(n->name, ".htm"))
        st->doc = html_parse((const char *)n->data, n->size);
    else
        st->doc = html_parse_text((const char *)n->data, n->size);
    st->scroll = 0;
    fields_reset(st);
    scopy(st->url, p, sizeof(st->url));
    run_scripts(st);
    fetch_images(st);
    scopy(BW->title, n->name, sizeof(BW->title));

    char num[16]; sh_utoa((uint64_t)n->size, num);
    char s[120]; s[0] = 0;
    sappend(s, "local file  -  ", sizeof(s)); sappend(s, num, sizeof(s)); sappend(s, " bytes", sizeof(s));
    set_status(st, s);
    return 1;
}

/* Percent-encode a query into a DuckDuckGo HTML search URL. DDG's html
 * endpoint returns plain markup (no JS), so its results render natively --
 * this is what makes typing a search term in the address bar actually work. */
static void build_search_url(const char *q, char *out, uint32_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    const char *pre = "https://html.duckduckgo.com/html/?q=";
    uint32_t o = 0;
    for (const char *p = pre; *p && o < cap - 1; ) out[o++] = *p++;
    for (const char *p = q; *p && o + 4 < cap; p++) {
        unsigned char ch = (unsigned char)*p;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            out[o++] = (char)ch;
        else if (ch == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[ch >> 4]; out[o++] = hex[ch & 15]; }
    }
    out[o] = 0;
}

/* Google and Bing build their results with JavaScript: a non-JS client gets a
 * "please enable JavaScript" shell with no result links. So when the user
 * submits the Google/Bing search box (or navigates such a results URL), pull
 * the q= term out and re-issue the search against DuckDuckGo's html endpoint,
 * which returns plain markup we render. This is what makes "Google search"
 * actually produce results here -- same query, a backend that works without JS. */
static int rewrite_serp(const char *url, char *out, uint32_t cap) {
    const char *h = url;
    if (strncmp(h, "http://", 7) == 0)  h += 7;
    else if (strncmp(h, "https://", 8) == 0) h += 8;
    /* host must be a google.* or bing.* results path */
    int is_g = (strncmp(h, "www.google.", 11) == 0 || strncmp(h, "google.", 7) == 0);
    int is_b = (strncmp(h, "www.bing.", 9) == 0 || strncmp(h, "bing.", 5) == 0);
    if (!is_g && !is_b) return 0;
    const char *slash = strchr(h, '/');
    if (!slash || strncmp(slash, "/search", 7) != 0) return 0;
    const char *qs = strchr(slash, '?');
    if (!qs) return 0;
    /* find the q= parameter */
    const char *q = 0;
    for (const char *p = qs + 1; *p; ) {
        if ((p[0]=='q'||p[0]=='Q') && p[1]=='=') { q = p + 2; break; }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    if (!q) return 0;
    uint32_t o = 0;
    const char *pre = "https://html.duckduckgo.com/html/?q=";
    for (const char *p = pre; *p && o < cap - 1; ) out[o++] = *p++;
    for (const char *p = q; *p && *p != '&' && o < cap - 1; p++) out[o++] = *p;   /* q is already URL-encoded */
    out[o] = 0;
    return 1;
}

/* DuckDuckGo wraps every result link as duckduckgo.com/l/?uddg=<encoded-target>
 * &rut=... -- a click-tracking redirect that answers with a 200 + client-side
 * redirect (not an HTTP 30x we follow), so it renders blank. Unwrap it: pull
 * uddg=, percent-decode it, and navigate straight to the real destination. */
static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int unwrap_ddg_link(const char *url, char *out, uint32_t cap) {
    const char *h = url;
    if (strncmp(h, "http://", 7) == 0)  h += 7;
    else if (strncmp(h, "https://", 8) == 0) h += 8;
    if (strncmp(h, "duckduckgo.com/l/", 17) != 0 &&
        strncmp(h, "www.duckduckgo.com/l/", 21) != 0 &&
        strncmp(h, "links.duckduckgo.com/l/", 23) != 0) return 0;
    const char *q = 0;
    for (const char *p = h; *p; p++)
        if (p[0]=='u' && p[1]=='d' && p[2]=='d' && p[3]=='g' && p[4]=='=') { q = p + 5; break; }
    if (!q) return 0;
    uint32_t o = 0;
    for (const char *p = q; *p && *p != '&' && o < cap - 1; ) {
        if (*p == '%' && p[1] && p[2]) {
            int hi = hexv(p[1]), lo = hexv(p[2]);
            if (hi >= 0 && lo >= 0) { out[o++] = (char)(hi * 16 + lo); p += 3; continue; }
        }
        if (*p == '+') { out[o++] = ' '; p++; continue; }
        out[o++] = *p++;
    }
    out[o] = 0;
    return o > 0;
}

static void load_url(browser_t *st, const char *url, int depth) {
    /* trim leading spaces */
    while (*url == ' ') url++;
    if (!*url) return;

    /* transparent Google/Bing -> renderable-search rewrite (see rewrite_serp) */
    char serp[420];
    if (depth == 0 && rewrite_serp(url, serp, sizeof(serp))) url = serp;

    /* unwrap a DuckDuckGo result-link redirect to its real destination */
    char unwrapped[420];
    if (depth < 4 && unwrap_ddg_link(url, unwrapped, sizeof(unwrapped))) url = unwrapped;

    int ok;
    if (is_remote(url)) {
        ok = load_http(st, url, depth);
    } else if (url[0] == '/' || strncmp(url, "file:", 5) == 0) {
        ok = load_fs(st, url);
    } else {
        /* Decide between a host to open, a local file, and a web search.
         * host-looking = a dot before any slash and no spaces. A bare token
         * that names an existing local file opens it; anything else (a phrase,
         * or a word with no matching file) becomes a web search. */
        const char *slash = strchr(url, '/');
        const char *dot   = strchr(url, '.');
        int spaced = (strchr(url, ' ') != 0);
        fs_node *local = spaced ? 0 : fs_lookup(url);
        if (!spaced && dot && (!slash || dot < slash)) {
            char full[300]; full[0] = 0; sappend(full, "http://", sizeof(full)); sappend(full, url, sizeof(full));
            ok = load_http(st, full, depth);
        } else if (local && !local->is_dir) {
            ok = load_fs(st, url);
        } else {
            char surl[400]; build_search_url(url, surl, sizeof(surl));
            ok = load_http(st, surl, depth);
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

/* Build a GET query from the form's text fields and navigate to it. `run` is
 * the focused field or the clicked submit button; its run->link points at the
 * enclosing <form action> (or -1 to submit to the current page). */
static void submit_form(browser_t *st, int run) {
    html_doc *d = st->doc;
    if (!d || run < 0 || run >= d->nruns) return;
    int action_link = d->runs[run].link;
    char base[300];
    if (action_link >= 0 && action_link < d->nhrefs)
        resolve_link(st, d->hrefs[action_link], base, sizeof(base));
    else
        scopy(base, st->url, sizeof(base));
    for (char *p = base; *p; p++) if (*p == '?') { *p = 0; break; }   /* drop old query */

    char url[512]; scopy(url, base, sizeof(url));
    int first = 1;
    for (int i = 0; i < d->nruns; i++) {
        html_run *r = &d->runs[i];
        if (r->kind != HRUN_INPUT || !r->name) continue;
        if (r->img != 0 && r->img != 2) continue;       /* text fields / textareas only */
        char *val = field_find(st, i);
        sappend(url, first ? "?" : "&", sizeof(url)); first = 0;
        sappend(url, r->name, sizeof(url));
        sappend(url, "=", sizeof(url));
        for (char *v = val; v && *v; v++) {             /* minimal urlencode */
            char c = *v;
            if (c == ' ') sappend(url, "+", sizeof(url));
            else if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') {
                char one[2] = { c, 0 }; sappend(url, one, sizeof(url));
            } else {
                const char *H = "0123456789ABCDEF";
                char hx[4] = { '%', H[(c>>4)&0xF], H[c&0xF], 0 }; sappend(url, hx, sizeof(url));
            }
        }
    }
    st->focus_run = -1;
    navigate(st, url);
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

    uint32_t pbg = (st->doc && (st->doc->page_bg & 0x1000000)) ? st->doc->page_bg & 0xFFFFFF : WEB_BG;
    g_fill(cx, cy, cw, ch, pbg);
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
    else g_text(cx + 14, cy + Y0 + 12, "No page loaded.", WEB_DIM, 1);
    g_set_clip(cx, cy, cw, ch);

    /* scrollbar */
    int track_y = cy + Y0, track_h = Yb - Y0;
    int vh = Yb - Y0;
    g_fill(cx + cw - SBW - 4, track_y, SBW, track_h, 0xF1F1F3);
    if (st->content_h > vh) {
        int thumb_h = vh * vh / st->content_h; if (thumb_h < 20) thumb_h = 20;
        int maxscroll = st->content_h - vh;
        int thumb_y = track_y + (track_h - thumb_h) * st->scroll / (maxscroll ? maxscroll : 1);
        g_round(cx + cw - SBW - 3, thumb_y, SBW - 2, thumb_h, 3, 0xC1C1C7, 255);
    }

    /* status bar */
    g_fill(cx, cy + ch - STATUS_H, cw, STATUS_H, COL_PANEL_2);
    g_hline(cx, cy + ch - STATUS_H, cw, 0x33333F);
    struct netif *nif = netif_default();
    char link[40]; link[0] = 0;
    sappend(link, nif ? (nif->link_up ? "online" : "link down") : "no NIC", sizeof(link));

    /* hovering a link previews its resolved target on the left, like a browser */
    char hov[300]; hov[0] = 0;
    if (st->doc && myp >= Y0 && myp < Yb && mxp < cw - SBW - 4) {
        for (int i = 0; i < st->nhits; i++) {
            hitrect *h = &st->hits[i];
            if (h->kind == 0 && h->link >= 0 && h->link < st->doc->nhrefs &&
                mxp >= h->x && mxp < h->x + h->w && myp >= h->y && myp < h->y + h->h) {
                resolve_link(st, st->doc->hrefs[h->link], hov, sizeof(hov));
                break;
            }
        }
    }
    g_text(cx + 8, cy + ch - STATUS_H + 6, hov[0] ? hov : st->status,
           hov[0] ? COL_ACCENT : COL_TEXT_DIM, 1);
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
    if (st->focus_run >= 0) {                          /* typing into a form field */
        char *b = field_get(st, st->focus_run);
        if (!b)             st->focus_run = -1;
        else if (c == '\n') submit_form(st, st->focus_run);
        else if (c == 27)   st->focus_run = -1;
        else if (c == '\b') { int l = (int)strlen(b); if (l > 0) b[l-1] = 0; }
        else if ((unsigned char)c >= 32) { int l = (int)strlen(b); if (l < 94) { b[l] = c; b[l+1] = 0; } }
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

static void browser_scroll(window_t *w, int delta) {
    (void)w;
    browser_t *st = &B;
    int vh = st->ch - TOOLBAR_H - STATUS_H;
    int maxscroll = st->content_h - vh; if (maxscroll < 0) maxscroll = 0;
    st->scroll += delta * 40;          /* wheel down (positive) moves the page down */
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
            if (h->kind == 1) {                        /* form control */
                int sub = (st->doc && h->run < st->doc->nruns) ? st->doc->runs[h->run].img : 0;
                if (sub == 1) submit_form(st, h->run);       /* button: submit */
                else { st->focus_run = h->run; field_get(st, h->run); }  /* field: focus */
                return;
            }
            if (st->doc && h->link >= 0 && h->link < st->doc->nhrefs) {
                char rurl[300]; resolve_link(st, st->doc->hrefs[h->link], rurl, sizeof(rurl));
                navigate(st, rurl);
            }
            return;
        }
    }
    st->focus_run = -1;                                /* clicked empty space: unfocus */
}

/* ---- a built-in start page + a sample local file ----------------------- */
static const char WELCOME[] =
    "<title>BoltOS Browser</title>"
    "<style>"
    "  .bar { background: #1a73e8; color: #ffffff; text-align: center; font-weight: bold; }"
    "  h1 { color: #1a73e8; text-align: center; }"
    "  .tag { color: #5f6368; font-style: italic; text-align: center; }"
    "  .note { color: #0b7d3e; font-weight: bold; }"
    "  .card { background: #f1f3f4; }"
    "  code { background: #eef1f5; color: #0b7d3e; }"
    "  .hidden { display: none; }"
    "</style>"
    "<p class=\"bar\">BoltOS &mdash; from-scratch x86-64 OS</p>"
    "<h1>BoltOS Browser</h1>"
    "<p class=\"tag\">now with CSS styling, backgrounds and form controls</p>"
    "<p class=\"hidden\">This line is display:none and must not render.</p>"
    "<form action=\"https://html.duckduckgo.com/html/\">"
    "<p>Search the web: <input type=\"text\" name=\"q\" placeholder=\"Click here, type, press Enter\" size=\"26\"> "
    "<input type=\"submit\" value=\"Search\"></p>"
    "</form>"
    "<p class=\"tag\">Click a text field to focus it, type, then press Enter or Search to submit a GET query.</p>"
    "<p>A tiny web browser running on a from-scratch kernel. It speaks HTTP and "
    "HTTPS (TLS 1.2: ECDHE-X25519 / AES-128-GCM) over the BoltOS TCP/IP stack, and "
    "can open local HTML files from the ramfs. The server certificate is not "
    "verified, so treat <b>https://</b> here as eavesdrop-resistant, not trusted.</p>"
    "<h2>Try it</h2>"
    "<ul>"
    "<li>Type a URL in the bar and press <b>Go</b> (e.g. <a href=\"https://example.com\">https://example.com</a>)</li>"
    "<li>Open the bundled page: <a href=\"/web/index.html\">/web/index.html</a></li>"
    "<li>Scroll with the scrollbar, or Space / b, or the j / k keys</li>"
    "<li>Click <b>&lt;</b> to go back</li>"
    "</ul>"
    "<h2>Rendering</h2>"
    "<p>The engine decodes <b>PNG</b>, <b>JPEG</b>, <b>GIF</b> and <b>BMP</b> images, "
    "and lays out proportional text with <b>bold</b>, <i>italic</i>, "
    "<code>monospace code</code> and "
    "<font color=\"#e06060\">in</font><font color=\"#60c060\">line</font> "
    "<font color=\"#6090e0\">colour</font>, headings, lists, blockquotes and "
    "alignment. UTF-8 is decoded too &mdash; accents fold to ASCII "
    "(caf&eacute;, na&iuml;ve) and &ldquo;smart quotes&rdquo; render.</p>"
    "<center><p>This paragraph is centred.</p></center>"
    "<p><img src=\"/web/logo.bmp\" alt=\"a generated gradient\"></p>"
    "<h2>Notes</h2>"
    "<div class=\"card\"><p>Sections, cards and code blocks can now carry a "
    "<b>background colour</b> from CSS &mdash; <code>background-color</code> and the "
    "<code>background</code> shorthand both work, as do <code>bgcolor</code> attributes.</p></div>"
    "<p>The data path uses the e1000 NIC (the link QEMU/VirtualBox NAT exposes). "
    "Wi-Fi association is scaffolded in the kernel but needs a radio driver; see "
    "the <b>wifi</b> shell command for status.</p>";

static const char SAMPLE[] =
    "<title>Sample Page</title>"
    "<h1>Hello from the ramfs</h1>"
    "<p>This file lives at <b>/web/index.html</b> and is rendered by the BoltOS "
    "HTML engine without touching the network.</p>"
    "<p><img src=\"/web/logo.bmp\" alt=\"a generated gradient\"></p>"
    "<h3>A short list</h3>"
    "<ul><li>headings</li><li>paragraphs</li>"
    "<li><font color=\"red\">coloured</font> links and <b>bold</b> text</li></ul>"
    "<blockquote>Block quotes indent their text from the left margin.</blockquote>"
    "<p>Back to the <a href=\"/web/index.html\">start</a> or visit "
    "<a href=\"http://example.com\">example.com</a>.</p>";

/* generate a small 24-bit BMP so the sample page has a real image to decode */
static void seed_logo(void) {
    if (fs_lookup("/web/logo.bmp")) return;
    int w = 160, h = 70;
    uint32_t rowsz = ((uint32_t)w * 3 + 3) & ~3u, dsz = rowsz * h, fsz = 54 + dsz;
    uint8_t *bmp = (uint8_t *)kmalloc(fsz);
    if (!bmp) return;
    memset(bmp, 0, fsz);
    bmp[0] = 'B'; bmp[1] = 'M';
    *(uint32_t *)(bmp + 2)  = fsz;
    *(uint32_t *)(bmp + 10) = 54;
    *(uint32_t *)(bmp + 14) = 40;
    *(int32_t  *)(bmp + 18) = w;
    *(int32_t  *)(bmp + 22) = h;
    *(uint16_t *)(bmp + 26) = 1;
    *(uint16_t *)(bmp + 28) = 24;
    for (int y = 0; y < h; y++) {
        uint8_t *row = bmp + 54 + (uint32_t)y * rowsz;
        for (int x = 0; x < w; x++) {
            uint8_t *p = row + x * 3;        /* BGR, bottom-up */
            p[0] = (uint8_t)(60 + x * 160 / w);   /* B */
            p[1] = (uint8_t)(y * 200 / h);        /* G */
            p[2] = (uint8_t)(200 - x * 120 / w);  /* R */
        }
    }
    fs_node *f = fs_create("/web/logo.bmp", 0);
    if (f) fs_write(f, bmp, fsz);
    kfree(bmp);
}

static void seed_sample_file(void) {
    if (!fs_lookup("/web")) {
        fs_node *dir = fs_create("/web", 1);
        if (!dir) return;
    }
    if (!fs_lookup("/web/index.html")) {
        fs_node *f = fs_create("/web/index.html", 0);
        if (f) fs_write(f, SAMPLE, (uint32_t)(sizeof(SAMPLE) - 1));
    }
    seed_logo();
}

void browser_app_init(void) {
    memset(&B, 0, sizeof(B));
    seed_sample_file();

    window_t *w = gui_add_window("Browser", 780, 540, 0x66BB6A, ICON_BROWSER);
    if (!w) return;
    BW = w;
    w->draw   = browser_draw;
    w->key    = browser_key;
    w->click  = browser_click;
    w->scroll = browser_scroll;

    scopy(B.url, "about:welcome", sizeof(B.url));
    fields_reset(&B);
    B.doc = html_parse(WELCOME, (uint32_t)(sizeof(WELCOME) - 1));
    fetch_images(&B);
    scopy(B.addr, "", sizeof(B.addr));
    set_status(&B, "Welcome - enter a URL or open a local file");
}
