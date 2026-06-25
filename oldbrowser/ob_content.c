/* ===========================================================================
 *  ob_content.c  --  NetSurf content/content.c + content/handlers/{html,
 *                    textplain,image}.c
 *
 *  The content abstraction and its three core handlers. content_create sniffs
 *  the source (MIME-by-magic, like NetSurf's content_factory), then dispatches:
 *
 *    text/html   -> dom_parse + CSS gather + layout_build (libdom + libcss role)
 *    text/plain  -> monospace reflow
 *    image       -> image_decode (libnsgif / libnspng role)
 *
 *  content_reformat re-runs layout for a new viewport width; content_redraw
 *  walks the resulting box tree and paints it through the BoltOS compositor,
 *  mirroring NetSurf's html_redraw / textplain_redraw / image_redraw.
 * ===========================================================================*/
#include "oldbrowser.h"
#include "ns_html.h"     /* REAL libdom + libcss render path */
#include "string.h"
#include "kheap.h"
#include "gui.h"

#define WEB_TEXT   0x202124
#define WEB_DIM    0x5F6368
#define WEB_LINK   0x1A73E8
#define WEB_PLACE  0xE8E8EC
#define WEB_BG     0xFFFFFF

#define OB_MAX_IMG   64
#define OB_CSS_CAP   (96u * 1024)

/* per-content inline-image cache (src string -> decoded pixels) */
typedef struct { char src[256]; image_t *img; } imgcache_t;
static imgcache_t g_imgc[OB_MAX_IMG];     /* one page in front at a time */
static int        g_nimgc;
static content   *g_imgc_owner;

static void scopy(char *d, const char *s, uint32_t cap) {
    if (!cap) return; uint32_t i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

/* ------------------------------ sniffing -------------------------------- */
static int looks_html(const char *d, uint32_t n) {
    uint32_t lim = n < 1024 ? n : 1024;
    for (uint32_t i = 0; i + 1 < lim; i++) {
        if (d[i] == '<') {
            char c = d[i + 1] | 0x20;
            if (c == '!' || c == 'h' || c == 'b' || c == 'a' || c == 'p' ||
                c == 'd' || c == 't' || c == 'u' || c == 'i' || c == 's')
                return 1;
        }
    }
    return 0;
}
static enum content_type sniff(nsurl *url, const char *d, uint32_t n) {
    const unsigned char *u = (const unsigned char *)d;
    if (n >= 4 && u[0] == 0x89 && u[1] == 'P' && u[2] == 'N' && u[3] == 'G') return CONTENT_IMAGE;
    if (n >= 3 && u[0] == 0xFF && u[1] == 0xD8 && u[2] == 0xFF)             return CONTENT_IMAGE;
    if (n >= 4 && u[0] == 'G' && u[1] == 'I' && u[2] == 'F' && u[3] == '8') return CONTENT_IMAGE;
    if (n >= 2 && u[0] == 'B' && u[1] == 'M')                              return CONTENT_IMAGE;
    const char *p = url ? url->path : "";
    int L = (int)strlen(p);
    if (L > 4) {
        const char *e = p + L;
        if (!strncmp(e - 4, ".txt", 4) || !strncmp(e - 3, ".md", 3) ||
            !strncmp(e - 2, ".c", 2)   || !strncmp(e - 2, ".h", 2)) return CONTENT_TEXTPLAIN;
    }
    if (looks_html(d, n)) return CONTENT_HTML;
    return CONTENT_TEXTPLAIN;
}

/* ------------------------------ CSS gather ------------------------------ */
static void css_gather(content *c) {
    c->css = (char *)kmalloc(OB_CSS_CAP);
    if (!c->css) { c->css_len = 0; return; }
    c->css[0] = 0; c->css_len = 0;
    dom_node *nodes[64];

    int ns = dom_by_tag(c->dom->root, "style", nodes, 64);
    for (int i = 0; i < ns; i++) {
        char buf[8192];
        int n = dom_inner_text(nodes[i], buf, sizeof(buf));
        if (n > 0 && c->css_len + (uint32_t)n + 1 < OB_CSS_CAP) {
            memcpy(c->css + c->css_len, buf, n); c->css_len += n;
            c->css[c->css_len++] = '\n'; c->css[c->css_len] = 0;
        }
    }
    /* external <link rel=stylesheet href=...> (bounded) */
    int nl = dom_by_tag(c->dom->root, "link", nodes, 64);
    int fetched = 0;
    for (int i = 0; i < nl && fetched < 6; i++) {
        const char *rel = dom_attr_get(nodes[i], "rel");
        const char *href = dom_attr_get(nodes[i], "href");
        if (!href || !href[0]) continue;
        if (rel && !(ob_strstr(rel, "stylesheet") || ob_strstr(rel, "STYLESHEET"))) continue;
        nsurl *lu = nsurl_join(c->url, href);
        if (!lu) continue;
        char *src = 0; int st = 0; nsurl *fin = 0;
        int got = llcache_fetch(lu, &src, &st, &fin);
        if (got > 0 && src) {
            if (c->css_len + (uint32_t)got + 1 < OB_CSS_CAP) {
                memcpy(c->css + c->css_len, src, got); c->css_len += got;
                c->css[c->css_len++] = '\n'; c->css[c->css_len] = 0;
            }
            fetched++;
        }
        if (src) kfree(src);
        if (fin) nsurl_unref(fin);
        nsurl_unref(lu);
    }
}

static void extract_title(content *c) {
    c->title[0] = 0;
    dom_node *t = dom_query(c->dom->root, "title");
    if (t) { char buf[160]; dom_inner_text(t, buf, sizeof(buf));
             /* trim */ char *s = buf; while (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r') s++;
             scopy(c->title, s, sizeof(c->title));
             int L = (int)strlen(c->title);
             while (L > 0 && (c->title[L-1]==' '||c->title[L-1]=='\n'||c->title[L-1]=='\t'||c->title[L-1]=='\r')) c->title[--L]=0; }
    if (!c->title[0]) {
        if (c->url) nsurl_get_component(c->url, NSURL_HOST, c->title, sizeof(c->title));
        if (!c->title[0]) scopy(c->title, "Untitled", sizeof(c->title));
    }
}

/* ------------------------------ create ---------------------------------- */
content *content_create(nsurl *url, const char *data, uint32_t len, int status) {
    content *c = (content *)kmalloc(sizeof(content));
    if (!c) return 0;
    memset(c, 0, sizeof(*c));
    c->status = status;
    c->url = url ? nsurl_ref(url) : 0;
    c->source = (char *)kmalloc(len + 1);
    if (c->source) { memcpy(c->source, data, len); c->source[len] = 0; c->source_len = len; }

    c->type = sniff(url, data, len);
    if (c->type == CONTENT_IMAGE) {
        c->img = image_decode((const uint8_t *)data, len);
        if (!c->img) c->type = CONTENT_TEXTPLAIN;
        else { c->width = c->img->w; c->height = c->img->h;
               scopy(c->title, "Image", sizeof(c->title));
               if (url) nsurl_get_component(url, NSURL_HOST, c->title, sizeof(c->title)); }
    }
    if (c->type == CONTENT_HTML) {
        /* Real NetSurf core: libdom DOM + libcss cascade/selection + layout.
         * Built at a provisional width here to recover the <title>; reformat
         * rebuilds at the true viewport width. */
        const char *base = c->url ? nsurl_access(c->url) : 0;
        c->nspage = ns_html_build(c->source, c->source_len, base, 1000,
                                  c->title, sizeof c->title);
        c->laid_w = 1000;
        if (!c->nspage) c->type = CONTENT_TEXTPLAIN;
        else if (!c->title[0] && c->url)
            nsurl_get_component(c->url, NSURL_HOST, c->title, sizeof c->title);
    }
    if (c->type == CONTENT_TEXTPLAIN) {
        if (url) nsurl_get_component(url, NSURL_HOST, c->title, sizeof(c->title));
        if (!c->title[0]) scopy(c->title, "Text", sizeof(c->title));
    }
    return c;
}

content *content_create_error(nsurl *url, const char *msg) {
    content *c = (content *)kmalloc(sizeof(content));
    if (!c) return 0;
    memset(c, 0, sizeof(*c));
    c->type = CONTENT_ERROR;
    c->url = url ? nsurl_ref(url) : 0;
    scopy(c->errmsg, msg, sizeof(c->errmsg));
    scopy(c->title, "Error", sizeof(c->title));
    return c;
}

/* ------------------------------ images ---------------------------------- */
static void imgc_reset(content *c) {
    if (g_imgc_owner != c) return;
    for (int i = 0; i < g_nimgc; i++) if (g_imgc[i].img) image_free(g_imgc[i].img);
    g_nimgc = 0; g_imgc_owner = 0;
}
static image_t *img_for(content *c, const char *src) {
    if (g_imgc_owner != c) { imgc_reset(c); g_imgc_owner = c; g_nimgc = 0; }
    for (int i = 0; i < g_nimgc; i++) if (strcmp(g_imgc[i].src, src) == 0) return g_imgc[i].img;
    if (g_nimgc >= OB_MAX_IMG) return 0;
    nsurl *iu = nsurl_join(c->url, src);
    if (!iu) return 0;
    char *data = 0; int st = 0; nsurl *fin = 0;
    int n = llcache_fetch(iu, &data, &st, &fin);
    image_t *im = 0;
    if (n > 0 && data) im = image_decode((const uint8_t *)data, n);
    if (data) kfree(data);
    if (fin) nsurl_unref(fin);
    nsurl_unref(iu);
    scopy(g_imgc[g_nimgc].src, src, sizeof(g_imgc[g_nimgc].src));
    g_imgc[g_nimgc].img = im; g_nimgc++;
    return im;
}
static void assign_images(content *c, layout_box *b) {
    if (!b) return;
    if (b->node && b->node->tag && strcmp(b->node->tag, "img") == 0) {
        const char *src = dom_attr_get(b->node, "src");
        if (src && src[0]) b->pix = img_for(c, src);
    }
    for (layout_box *k = b->first_child; k; k = k->next) assign_images(c, k);
}

/* ------------------------------ reformat -------------------------------- */
static int text_height(content *c, int width);
void content_reformat(content *c, int width) {
    if (!c) return;
    if (width < 80) width = 80;
    if (c->type == CONTENT_HTML) {
        if (c->nspage && c->laid_w == width) return;
        if (c->nspage) { ns_page_free(c->nspage); c->nspage = 0; }
        const char *base = c->url ? nsurl_access(c->url) : 0;
        c->nspage = ns_html_build(c->source, c->source_len, base, width,
                                  c->title, sizeof c->title);
        c->laid_w = width;
        c->width = width;
        c->height = c->nspage ? ns_page_height(c->nspage) + 8 : 0;
    } else if (c->type == CONTENT_TEXTPLAIN) {
        c->width = width;
        c->height = text_height(c, width);
    } else if (c->type == CONTENT_IMAGE && c->img) {
        c->width = c->img->w; c->height = c->img->h;
    } else if (c->type == CONTENT_ERROR) {
        c->width = width; c->height = 120;
    }
}

/* ------------------------------ redraw: html ---------------------------- */
static const char *box_link(layout_box *b) {
    for (layout_box *p = b; p; p = p->parent)
        if (p->node && p->node->tag && strcmp(p->node->tag, "a") == 0) {
            const char *h = dom_attr_get(p->node, "href"); if (h && h[0]) return h;
        }
    return 0;
}
static int box_lh(int fs) { return layout_line_height(fs ? fs : 1); }

static void paint_text_box(layout_box *b, int ox, int oy, int cl, int cr, int clipT, int clipB) {
    computed_style *s = &b->st;
    int scale = s->fscale ? s->fscale : 1, lh = box_lh(scale);
    uint32_t col = (s->color & 0x1000000) ? (s->color & 0xFFFFFF) : WEB_TEXT;
    const char *href = box_link(b);
    if (href) col = WEB_LINK;
    int pen_x = ox + b->x, pen_y = oy + b->y, x0 = cl;
    int sp = g_text_width_pn(" ", 1, scale);
    const char *p = b->text;
    while (p && *p) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;
        const char *w = p; int wl = 0;
        while (*p && *p != ' ' && *p != '\n' && *p != '\t' && *p != '\r') { p++; wl++; }
        int ww = g_text_width_pn(w, wl, scale);
        int space = (pen_x > x0) ? sp : 0;
        if (pen_x > x0 && pen_x + space + ww > cr) { pen_x = x0; pen_y += lh; space = 0; }
        pen_x += space;
        if (pen_y + lh >= clipT && pen_y <= clipB) {
            g_text_pn(pen_x, pen_y, w, wl, col, scale, s->italic);
            if (href) g_hline(pen_x, pen_y + 8 * scale + 1, ww, col);
        }
        pen_x += ww;
    }
}

static void paint_box(content *c, layout_box *b, int ox, int oy, int cl, int cr, int clipT, int clipB) {
    if (!b) return;
    if (b->is_text) { paint_text_box(b, ox, oy, cl, cr, clipT, clipB); return; }
    computed_style *s = &b->st;
    int sx = ox + b->x, sy = oy + b->y;
    int vis = (sy + b->h >= clipT) && (sy <= clipB);
    if (vis) {
        if (s->bg & 0x1000000) g_fill(sx, sy, b->w, b->h, s->bg & 0xFFFFFF);
        if (s->border[0] || s->border[1] || s->border[2] || s->border[3]) g_rect(sx, sy, b->w, b->h, 0xC0C0C8);
    }
    const char *tag = b->node ? b->node->tag : 0;
    if (tag && strcmp(tag, "img") == 0) {
        if (vis) {
            if (b->pix) { image_t *im = (image_t *)b->pix; g_blit(sx, sy, b->w, b->h, im->px, im->w, im->h); }
            else { g_fill(sx, sy, b->w, b->h, WEB_PLACE); g_rect(sx, sy, b->w, b->h, 0xC0C0C8);
                   const char *alt = dom_attr_get(b->node, "alt");
                   g_text(sx + 4, sy + b->h / 2 - 4, alt && alt[0] ? alt : "[img]", WEB_DIM, 1); }
        }
        return;
    }
    if (tag && (strcmp(tag, "input") == 0 || strcmp(tag, "button") == 0 || strcmp(tag, "select") == 0 || strcmp(tag, "textarea") == 0)) {
        if (vis) {
            g_round(sx, sy, b->w, b->h, 5, 0xFFFFFF, 255); g_rect(sx, sy, b->w, b->h, 0xC0C0C8);
            const char *ph = dom_attr_get(b->node, "placeholder"); const char *val = dom_attr_get(b->node, "value");
            const char *lbl = val && val[0] ? val : ph ? ph : "";
            if (lbl[0]) g_text_pn(sx + 6, sy + (b->h - 8) / 2, lbl, (int)strlen(lbl), WEB_DIM, 1, 0);
        }
        return;
    }
    int content_l = ox + b->cx, content_r = ox + b->cx + b->cw;
    for (layout_box *k = b->first_child; k; k = k->next)
        paint_box(c, k, ox, oy, content_l, content_r, clipT, clipB);
}

/* ------------------------------ redraw: text ---------------------------- */
static int text_render(content *c, int ox, int oy, int cl, int cr, int clipT, int clipB, int draw) {
    int x = cl, y = oy, lh = box_lh(1), maxw = cr - cl;
    int spw = g_text_width_pn(" ", 1, 1);
    const char *p = c->source;
    int startx = cl;
    (void)ox;
    while (p && *p) {
        if (*p == '\n') { x = startx; y += lh; p++; continue; }
        if (*p == '\r') { p++; continue; }
        if (*p == ' ' || *p == '\t') { x += (*p == '\t' ? spw * 4 : spw); p++; continue; }
        const char *w = p; int wl = 0;
        while (*p && *p != ' ' && *p != '\n' && *p != '\t' && *p != '\r') { p++; wl++; }
        int ww = g_text_width_pn(w, wl, 1);
        if (x > startx && x + ww > cl + maxw) { x = startx; y += lh; }
        if (draw && y + lh >= clipT && y <= clipB)
            g_text_pn(x, y, w, wl, WEB_TEXT, 1, 0);
        x += ww;
    }
    return (y + lh) - oy;
}
static int text_height(content *c, int width) {
    return text_render(c, 0, 0, 0, width, 0, 0x7fffffff, 0) + 8;
}

/* ------------------------------ redraw dispatch ------------------------- */
void content_redraw(content *c, browser_window *bw, int ox, int oy, int cl, int cr, int clipT, int clipB) {
    (void)bw;
    if (!c) return;
    if (c->type == CONTENT_HTML && c->nspage) {
        ns_page_paint(c->nspage, ox, oy, cl, cr, clipT, clipB);
    } else if (c->type == CONTENT_TEXTPLAIN) {
        text_render(c, ox, oy, cl + 4, cr - 4, clipT, clipB, 1);
    } else if (c->type == CONTENT_IMAGE && c->img) {
        g_blit(ox, oy, c->img->w, c->img->h, c->img->px, c->img->w, c->img->h);
    } else if (c->type == CONTENT_ERROR) {
        g_text(cl + 8, oy + 8, "Unable to display page", 0xC0392B, 2);
        g_text(cl + 8, oy + 40, c->errmsg, WEB_DIM, 1);
        if (c->url) g_text(cl + 8, oy + 60, c->url->full, WEB_LINK, 1);
    }
}

/* ------------------------------ hit testing ----------------------------- */
static layout_box *box_at(layout_box *b, int x, int y) {
    if (!b) return 0;
    layout_box *hit = 0;
    if (!b->is_text && x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) hit = b;
    for (layout_box *k = b->first_child; k; k = k->next) {
        layout_box *d = box_at(k, x, y);
        if (d) hit = d;
    }
    return hit;
}
const char *content_href_at(content *c, int cx, int cy) {
    if (!c || c->type != CONTENT_HTML || !c->nspage) return 0;
    return ns_page_href_at(c->nspage, cx, cy);
}

const char *content_get_title(content *c) { return c ? c->title : ""; }
int content_get_height(content *c) { return c ? c->height : 0; }

void content_destroy(content *c) {
    if (!c) return;
    imgc_reset(c);
    if (c->nspage) ns_page_free(c->nspage);
    if (c->box) layout_free(c->box);
    if (c->dom) dom_free(c->dom);
    if (c->css) kfree(c->css);
    if (c->source) kfree(c->source);
    if (c->img) image_free(c->img);
    if (c->url) nsurl_unref(c->url);
    kfree(c);
}
