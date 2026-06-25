/* ===========================================================================
 *  oldbrowser/ns_html.c -- real libdom + libcss render pipeline.
 *
 *  HTML -> libdom DOM (real hubbub binding) -> libcss cascade/selection (the
 *  ns_select.c handler) -> a style tree -> block+inline layout driven by the
 *  genuine computed styles -> a flat display list -> framebuffer paint.
 *
 *  Deliberately self-contained: it includes libdom/libcss public headers and so
 *  must NOT pull BoltOS's own <dom.h> (the kernel/dom.c API) -- the two define a
 *  clashing `dom_node`/`dom_document`. The few kernel services it needs (heap,
 *  framebuffer, URL join, fetch, image decode) are forward-declared below.
 * ===========================================================================*/
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <libcss/libcss.h>
#include <dom/dom.h>

#include "parser.h"          /* libdom hubbub binding (bindings/hubbub) */
#include "ns_select.h"
#include "ns_html.h"
#include "image.h"           /* image_t, image_decode, image_free (standalone) */

/* --- kernel services (forward-declared to avoid including boltos <dom.h>) --- */
void *kmalloc(uint64_t size);
void  kfree(void *p);
int   g_text_pn(int x, int y, const char *s, int len, uint32_t color, int scale, int italic);
int   g_text_width_pn(const char *s, int len, int scale);
void  g_fill(int x, int y, int w, int h, uint32_t color);
void  g_rect(int x, int y, int w, int h, uint32_t color);
void  g_hline(int x, int y, int w, uint32_t color);
void  g_blit(int dx, int dy, int dw, int dh, const uint32_t *src, int sw, int sh);
int   layout_line_height(int scale);
/* URL + fetch (oldbrowser nsurl/llcache) */
typedef struct nsurl nsurl;
nsurl      *nsurl_create(const char *s);
nsurl      *nsurl_join(nsurl *base, const char *rel);
void        nsurl_unref(nsurl *u);
int         llcache_fetch(nsurl *url, char **out, int *status, nsurl **final);

#define NS_TEXT_DEFAULT 0x202124
#define NS_LINK_COLOR   0x1A73E8
#define NS_MAXITEMS     40000
#define NS_MAXDEPTH     80

/* ----------------------------- display list ----------------------------- */
enum { IT_TEXT = 1, IT_RECT, IT_IMG };
typedef struct {
    uint8_t  kind;
    int      x, y, w, h;
    uint32_t color;          /* text colour / rect fill (0x1RRGGBB or 0) */
    uint32_t border;         /* rect border colour (0 = none) */
    uint8_t  scale, bold, italic, underline;
    char    *text;           /* IT_TEXT (heap) */
    char    *href;           /* IT_TEXT/IT_IMG: link target (borrowed) */
    image_t *img;            /* IT_IMG */
} ns_item;

struct ns_page {
    ns_item *items;
    int      n, cap;
    int      doc_h;
    char   **strings;        /* owned heap strings (hrefs) to free */
    int      nstr, capstr;
    image_t **imgs; int nimg, capimg;
    nsurl   *base;
};

/* ----------------------------- style tree ------------------------------- */
typedef struct snode {
    int      is_text;
    char    *text;                       /* text node content (heap) */
    char     tag[16];
    char    *href;                       /* link target (borrowed from strings) */
    char    *src, *alt;                  /* <img> (heap via page strings) */
    int      display;                    /* 0 inline, 1 block, 2 none, 4 list */
    uint32_t color, bg;                  /* 0x1RRGGBB / 0 */
    int      fscale, bold, italic, talign;
    int      mt, mr, mb, ml, pt, pr, pb, pl, bw;
    uint32_t bcolor;
    int      has_w, wpx;
    struct snode *kids, *last, *next, *parent;
} snode;

/* ------------------------------- helpers -------------------------------- */
static char *page_strdup(ns_page *p, const char *s, int len) {
    if (!s) return 0;
    if (len < 0) len = (int)strlen(s);
    char *d = (char *)kmalloc((uint64_t)len + 1);
    if (!d) return 0;
    memcpy(d, s, len); d[len] = 0;
    if (p->nstr >= p->capstr) {
        int nc = p->capstr ? p->capstr * 2 : 64;
        char **na = (char **)kmalloc(sizeof(char *) * (uint64_t)nc);
        if (!na) { kfree(d); return 0; }
        for (int i = 0; i < p->nstr; i++) na[i] = p->strings[i];
        if (p->strings) kfree(p->strings);
        p->strings = na; p->capstr = nc;
    }
    p->strings[p->nstr++] = d;
    return d;
}

static ns_item *page_add(ns_page *p) {
    if (p->n >= NS_MAXITEMS) return 0;
    if (p->n >= p->cap) {
        int nc = p->cap ? p->cap * 2 : 256;
        ns_item *ni = (ns_item *)kmalloc(sizeof(ns_item) * (uint64_t)nc);
        if (!ni) return 0;
        for (int i = 0; i < p->n; i++) ni[i] = p->items[i];
        if (p->items) kfree(p->items);
        p->items = ni; p->cap = nc;
    }
    ns_item *it = &p->items[p->n++];
    memset(it, 0, sizeof(*it));
    return it;
}

/* libcss colour (0xAARRGGBB) -> boltos 0x1RRGGBB (bit24 = "set"). */
static uint32_t cv_color(uint8_t type, css_color c, uint32_t fallback, int allow_none) {
    if (type == CSS_COLOR_COLOR) {
        uint8_t a = (c >> 24) & 0xff;
        if (a == 0 && allow_none) return 0;        /* transparent */
        return 0x1000000u | (c & 0xFFFFFF);
    }
    return fallback;
}

/* (css_fixed,unit) -> integer device px for a given style. */
static int cv_px(const css_computed_style *st, css_fixed len, css_unit unit) {
    css_fixed dp = css_unit_len2device_px(st, &ns_css_unit_ctx, len, unit);
    return (int)FIXTOINT(dp);
}

static int map_display(uint8_t d) {
    switch (d) {
    case CSS_DISPLAY_NONE:      return 2;
    case CSS_DISPLAY_INLINE:    return 0;
    case CSS_DISPLAY_LIST_ITEM: return 4;
    default:                    return 1;   /* block / inline-block / table / flex / grid */
    }
}

/* lowercase an element name into buf */
static void tag_of(dom_node *n, char *buf, int cap) {
    dom_string *nm = 0;
    buf[0] = 0;
    if (dom_node_get_node_name(n, &nm) == DOM_NO_ERR && nm) {
        const char *d = (const char *)dom_string_data(nm);
        int ln = (int)dom_string_length(nm), i = 0;
        for (; i < ln && i < cap - 1; i++) {
            char c = d[i];
            buf[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        }
        buf[i] = 0;
        dom_string_unref(nm);
    }
}

/* fetch an attribute value into a page string (or NULL) */
static char *attr_dup(ns_page *p, dom_node *n, const char *name) {
    dom_string *dn = 0, *val = 0;
    char *out = 0;
    if (dom_string_create_interned((const uint8_t *)name, strlen(name), &dn)
            != DOM_NO_ERR)
        return 0;
    if (dom_element_get_attribute(n, dn, &val) == DOM_NO_ERR && val) {
        out = page_strdup(p, (const char *)dom_string_data(val),
                          (int)dom_string_byte_length(val));
        dom_string_unref(val);
    }
    dom_string_unref(dn);
    return out;
}

/* ------------------------- build the style tree ------------------------- */
static snode *snew(void) {
    snode *s = (snode *)kmalloc(sizeof(snode));
    if (s) memset(s, 0, sizeof(*s));
    return s;
}
static void sappend(snode *parent, snode *child) {
    child->parent = parent;
    if (parent->last) parent->last->next = child; else parent->kids = child;
    parent->last = child;
}

static void fill_style(snode *s, const css_computed_style *cs) {
    css_color col; css_fixed len; css_unit unit;
    s->color  = cv_color(css_computed_color(cs, &col), col, 0x1000000u | NS_TEXT_DEFAULT, 0);
    s->bg     = cv_color(css_computed_background_color(cs, &col), col, 0, 1);
    s->display = map_display(css_computed_display(cs, false));

    uint8_t fs = css_computed_font_size(cs, &len, &unit);
    int px = (fs == CSS_FONT_SIZE_DIMENSION) ? cv_px(cs, len, unit) : 16;
    s->fscale = px >= 26 ? 3 : px >= 15 ? 2 : 1;

    uint8_t w = css_computed_font_weight(cs);
    s->bold = (w == CSS_FONT_WEIGHT_BOLD ||
               (w >= CSS_FONT_WEIGHT_700 && w <= CSS_FONT_WEIGHT_900));
    uint8_t fst = css_computed_font_style(cs);
    s->italic = (fst == CSS_FONT_STYLE_ITALIC || fst == CSS_FONT_STYLE_OBLIQUE);

    switch (css_computed_text_align(cs)) {
    case CSS_TEXT_ALIGN_CENTER: s->talign = 1; break;
    case CSS_TEXT_ALIGN_RIGHT:  s->talign = 2; break;
    default:                    s->talign = 0; break;
    }

#define EDGE(getter, field) do { \
        if (getter(cs, &len, &unit) == CSS_MARGIN_SET) s->field = cv_px(cs, len, unit); \
    } while (0)
    EDGE(css_computed_margin_top,    mt); EDGE(css_computed_margin_right,  mr);
    EDGE(css_computed_margin_bottom, mb); EDGE(css_computed_margin_left,   ml);
    EDGE(css_computed_padding_top,   pt); EDGE(css_computed_padding_right, pr);
    EDGE(css_computed_padding_bottom,pb); EDGE(css_computed_padding_left,  pl);
#undef EDGE
    /* border: use the top width as a uniform approximation if a colour is set */
    if (css_computed_border_top_width(cs, &len, &unit) == CSS_BORDER_WIDTH_WIDTH) {
        int bw = cv_px(cs, len, unit);
        if (bw > 0) { s->bw = bw; s->bcolor = cv_color(
                css_computed_border_top_color(cs, &col), col, 0x1000000u | 0xC0C0C8, 0)
                & 0xFFFFFF; }
    }
    uint8_t wt = css_computed_width(cs, &len, &unit);
    if (wt == CSS_WIDTH_SET && unit == CSS_UNIT_PX) { s->has_w = 1; s->wpx = cv_px(cs, len, unit); }
}

/* whitespace-collapse src into a page string; returns NULL if all-whitespace */
static char *collapse_text(ns_page *p, const char *src, int len) {
    char *buf = (char *)kmalloc((uint64_t)len + 1);
    if (!buf) return 0;
    int o = 0, sp = 0, any = 0;
    for (int i = 0; i < len; i++) {
        char c = src[i];
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f') {
            sp = 1;
        } else {
            if (sp && o) buf[o++] = ' ';
            sp = 0; buf[o++] = c; any = 1;
        }
    }
    buf[o] = 0;
    if (!any) { kfree(buf); return 0; }
    /* re-home into a tracked page string and free the temp */
    char *out = page_strdup(p, buf, o);
    kfree(buf);
    return out;
}

static int is_skip_tag(const char *t) {
    return strcmp(t, "script") == 0 || strcmp(t, "style") == 0 ||
           strcmp(t, "head") == 0   || strcmp(t, "title") == 0 ||
           strcmp(t, "meta") == 0   || strcmp(t, "link") == 0  ||
           strcmp(t, "noscript") == 0;
}

static void build_children(ns_page *pg, css_select_ctx *sctx, dom_node *el,
                           const css_computed_style *parent_cs,
                           snode *out, int depth) {
    dom_node *child = 0;
    if (depth > NS_MAXDEPTH) return;
    if (dom_node_get_first_child(el, &child) != DOM_NO_ERR) return;
    while (child) {
        dom_node *next = 0;
        dom_node_type type = 0;
        dom_node_get_node_type(child, &type);

        if (type == DOM_ELEMENT_NODE) {
            css_select_results *res = ns_css_select_style(sctx, child, parent_cs);
            const css_computed_style *cs = res ? res->styles[CSS_PSEUDO_ELEMENT_NONE] : 0;
            char tg[16]; tag_of(child, tg, sizeof tg);
            if (cs && !is_skip_tag(tg)) {
                snode *s = snew();
                if (s) {
                    fill_style(s, cs);
                    { int i = 0; for (; tg[i] && i < 15; i++) s->tag[i] = tg[i]; s->tag[i] = 0; }
                    /* link inheritance */
                    if (strcmp(tg, "a") == 0) {
                        s->href = attr_dup(pg, child, "href");
                        if (s->href) s->color = 0x1000000u | NS_LINK_COLOR;
                    } else {
                        s->href = out->href;   /* inherit enclosing link */
                    }
                    if (strcmp(tg, "img") == 0) {
                        s->src = attr_dup(pg, child, "src");
                        s->alt = attr_dup(pg, child, "alt");
                    }
                    if (s->display != 2) {     /* not display:none */
                        sappend(out, s);
                        build_children(pg, sctx, child, cs, s, depth + 1);
                    }
                }
            }
            if (res) css_select_results_destroy(res);
        } else if (type == DOM_TEXT_NODE) {
            dom_string *val = 0;
            if (dom_node_get_node_value(child, &val) == DOM_NO_ERR && val) {
                char *t = collapse_text(pg, (const char *)dom_string_data(val),
                                        (int)dom_string_byte_length(val));
                if (t) {
                    snode *s = snew();
                    if (s) {
                        s->is_text = 1; s->text = t;
                        s->color = out->color; s->fscale = out->fscale ? out->fscale : 2;
                        s->bold = out->bold; s->italic = out->italic;
                        s->talign = out->talign; s->href = out->href;
                        sappend(out, s);
                    }
                }
                dom_string_unref(val);
            }
        }

        if (dom_node_get_next_sibling(child, &next) != DOM_NO_ERR) {
            dom_node_unref(child); break;
        }
        dom_node_unref(child);
        child = next;
    }
}

/* ------------------------------- layout --------------------------------- */
typedef struct { ns_page *pg; int pen_x, pen_y, line_h, line_top, cl, cr, talign; } flow_t;

static void flow_newline(flow_t *f) {
    f->pen_x = f->cl;
    f->pen_y = f->line_top + (f->line_h ? f->line_h : layout_line_height(2));
    f->line_top = f->pen_y;
    f->line_h = 0;
}

/* place one word, wrapping at f->cr */
static void flow_word(flow_t *f, const char *w, int wl, snode *st) {
    int scale = st->fscale ? st->fscale : 2;
    int lh = layout_line_height(scale);
    int ww = g_text_width_pn(w, wl, scale);
    int sp = (f->pen_x > f->cl) ? g_text_width_pn(" ", 1, scale) : 0;
    if (f->pen_x > f->cl && f->pen_x + sp + ww > f->cr) { flow_newline(f); sp = 0; }
    f->pen_x += sp;
    if (lh > f->line_h) f->line_h = lh;
    ns_item *it = page_add(f->pg);
    if (it) {
        it->kind = IT_TEXT; it->x = f->pen_x; it->y = f->line_top;
        it->w = ww; it->h = lh;
        it->color = (st->color & 0x1000000) ? (st->color & 0xFFFFFF) : NS_TEXT_DEFAULT;
        it->scale = scale; it->bold = st->bold; it->italic = st->italic;
        it->href = st->href; it->underline = st->href ? 1 : 0;
        it->text = page_strdup(f->pg, w, wl);   /* word as a tracked string */
    }
    f->pen_x += ww;
}

static void flow_text(flow_t *f, snode *st) {
    const char *p = st->text;
    while (p && *p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *w = p; int wl = 0;
        while (*p && *p != ' ') { p++; wl++; }
        flow_word(f, w, wl, st);
    }
}

/* recursively flow an inline subtree */
static void flow_inline(flow_t *f, snode *n) {
    if (n->is_text) { flow_text(f, n); return; }
    for (snode *k = n->kids; k; k = k->next) flow_inline(f, k);
}

static int has_block_child(snode *n) {
    for (snode *k = n->kids; k; k = k->next)
        if (!k->is_text && (k->display == 1 || k->display == 4)) return 1;
    return 0;
}

/* fetch + decode an <img>, cached on the page */
static image_t *fetch_img(ns_page *pg, const char *src) {
    if (!src || !src[0] || !pg->base) return 0;
    nsurl *iu = nsurl_join(pg->base, src);
    if (!iu) return 0;
    char *data = 0; int st = 0; nsurl *fin = 0;
    int n = llcache_fetch(iu, &data, &st, &fin);
    image_t *im = 0;
    if (n > 0 && data) im = image_decode((const uint8_t *)data, n);
    if (data) kfree(data);
    if (fin) nsurl_unref(fin);
    nsurl_unref(iu);
    if (im) {
        if (pg->nimg >= pg->capimg) {
            int nc = pg->capimg ? pg->capimg * 2 : 16;
            image_t **na = (image_t **)kmalloc(sizeof(image_t *) * (uint64_t)nc);
            for (int i = 0; i < pg->nimg; i++) na[i] = pg->imgs[i];
            if (pg->imgs) kfree(pg->imgs);
            pg->imgs = na; pg->capimg = nc;
        }
        pg->imgs[pg->nimg++] = im;
    }
    return im;
}

/* lay out a block box; returns total vertical advance incl margins. */
static int layout_block(ns_page *pg, snode *n, int x, int y, int avail_w) {
    int bx = x + n->ml, by = y + n->mt;
    int bw = avail_w - n->ml - n->mr; if (bw < 0) bw = 0;
    if (n->has_w && n->wpx > 0 && n->wpx < bw) bw = n->wpx;

    /* reserve a background/border rect (filled once height is known) */
    int bg_idx = -1;
    if ((n->bg & 0x1000000) || n->bw) { ns_item *bgi = page_add(pg); bg_idx = bgi ? pg->n - 1 : -1; }

    int cx = bx + n->bw + n->pl;
    int cy = by + n->bw + n->pt;
    int cw = bw - 2 * n->bw - n->pl - n->pr; if (cw < 1) cw = 1;

    /* list bullet */
    if (n->display == 4) {
        ns_item *b = page_add(pg);
        if (b) { b->kind = IT_TEXT; b->x = cx; b->y = cy; b->w = 8; b->h = layout_line_height(2);
                 b->color = NS_TEXT_DEFAULT; b->scale = 2; b->text = page_strdup(pg, "\x07", 1); }
        cx += 14; cw -= 14; if (cw < 1) cw = 1;
    }

    /* <img> block */
    if (n->tag[0] == 'i' && strcmp(n->tag, "img") == 0) {
        image_t *im = fetch_img(pg, n->src);
        int iw = im ? im->w : 0, ih = im ? im->h : 0;
        if (n->has_w && n->wpx > 0) { if (im && iw) { ih = ih * n->wpx / iw; iw = n->wpx; } else iw = n->wpx; }
        if (iw > cw) { if (im && iw) ih = ih * cw / iw; iw = cw; }
        if (iw <= 0) iw = 64; if (ih <= 0) ih = 64;
        ns_item *it = page_add(pg);
        if (it) {
            it->kind = IT_IMG; it->x = cx; it->y = cy; it->w = iw; it->h = ih;
            it->img = im; it->href = n->href; it->color = 0x1000000u | 0xE8E8EC;
            it->text = n->alt;
        }
        cy += ih;
    }

    int childy = cy;
    if (n->kids) {
        if (has_block_child(n)) {
            /* block formatting: flush runs of inline children between blocks */
            flow_t f; snode *k = n->kids;
            while (k) {
                if (!k->is_text && (k->display == 1 || k->display == 4)) {
                    childy += layout_block(pg, k, cx, childy, cw);
                    k = k->next;
                } else {
                    memset(&f, 0, sizeof f);
                    f.pg = pg; f.cl = cx; f.cr = cx + cw;
                    f.pen_x = cx; f.pen_y = childy; f.line_top = childy; f.line_h = 0;
                    while (k && (k->is_text || k->display == 0)) { flow_inline(&f, k); k = k->next; }
                    childy = f.line_top + (f.line_h ? f.line_h : 0);
                }
            }
        } else {
            /* inline formatting context */
            flow_t f; memset(&f, 0, sizeof f);
            f.pg = pg; f.cl = cx; f.cr = cx + cw;
            f.pen_x = cx; f.pen_y = cy; f.line_top = cy; f.line_h = 0;
            for (snode *k = n->kids; k; k = k->next) flow_inline(&f, k);
            childy = f.line_top + (f.line_h ? f.line_h : 0);
        }
    }

    int contenth = childy - cy;
    int border_h = n->bw * 2 + n->pt + n->pb + contenth;
    if (n->display == 4 && border_h < layout_line_height(2)) border_h = layout_line_height(2);

    if (bg_idx >= 0) {
        ns_item *bgi = &pg->items[bg_idx];
        bgi->kind = IT_RECT; bgi->x = bx; bgi->y = by; bgi->w = bw; bgi->h = border_h;
        bgi->color = n->bg; bgi->border = n->bw ? (0x1000000u | n->bcolor) : 0;
    }
    return n->mt + border_h + n->mb;
}

static void free_snode(snode *n) {
    snode *k = n->kids;
    while (k) { snode *nx = k->next; free_snode(k); k = nx; }
    kfree(n);
}

/* find an element by tag (depth-first) */
static dom_node *find_tag(dom_node *node, const char *tag) {
    dom_node *child = 0, *found = 0;
    if (!node) return 0;
    if (dom_node_get_first_child(node, &child) != DOM_NO_ERR) return 0;
    while (child) {
        dom_node *next = 0; dom_node_type type = 0;
        dom_node_get_node_type(child, &type);
        if (type == DOM_ELEMENT_NODE) {
            char t[16]; tag_of(child, t, sizeof t);
            if (strcmp(t, tag) == 0) { found = child; break; }
        }
        found = find_tag(child, tag);
        if (found) { dom_node_unref(child); break; }
        if (dom_node_get_next_sibling(child, &next) != DOM_NO_ERR) { dom_node_unref(child); break; }
        dom_node_unref(child); child = next;
    }
    return found;
}

/* gather <style> text + a minimal UA sheet into a select context */
static const char *UA_CSS =
    "html,body,div,p,h1,h2,h3,h4,h5,h6,ul,ol,li,table,tr,form,header,footer,"
    "section,article,nav,aside,main,blockquote,pre,hr,figure{display:block}"
    "head,title,style,script,meta,link{display:none}"
    "body{margin:8px;color:#202124}"
    "h1{font-size:32px;font-weight:bold;margin-top:16px;margin-bottom:12px}"
    "h2{font-size:26px;font-weight:bold;margin-top:14px;margin-bottom:10px}"
    "h3{font-size:20px;font-weight:bold;margin-top:12px;margin-bottom:8px}"
    "h4,h5,h6{font-size:16px;font-weight:bold;margin-top:10px;margin-bottom:6px}"
    "p{margin-top:8px;margin-bottom:8px}"
    "ul,ol{margin-top:8px;margin-bottom:8px;padding-left:28px}"
    "li{display:list-item;margin-top:2px;margin-bottom:2px}"
    "a{color:#1a73e8}b,strong{font-weight:bold}i,em{font-style:italic}"
    "pre{font-size:13px}blockquote{padding-left:16px}"
    "th,td{padding-left:4px;padding-right:4px}";

static css_stylesheet *make_sheet(const char *css, int len, const char *url, int inl) {
    css_stylesheet_params sp; memset(&sp, 0, sizeof sp);
    sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    sp.level = CSS_LEVEL_DEFAULT; sp.charset = "UTF-8"; sp.url = url ? url : "boltos://";
    sp.inline_style = inl;
    /* resolve: treat rel as absolute (we don't follow @import here) */
    extern css_error ns_resolve_url_cb(void *, const char *, lwc_string *, lwc_string **);
    sp.resolve = ns_resolve_url_cb;
    css_stylesheet *sh = 0;
    if (css_stylesheet_create(&sp, &sh) != CSS_OK) return 0;
    css_stylesheet_append_data(sh, (const uint8_t *)css, len);
    css_stylesheet_data_done(sh);
    return sh;
}

css_error ns_resolve_url_cb(void *pw, const char *base, lwc_string *rel, lwc_string **abs) {
    (void)pw; (void)base; *abs = lwc_string_ref(rel); return CSS_OK;
}

/* concatenate all <style> element text under root */
static void gather_style_text(dom_node *root, char *buf, int cap, int *len) {
    dom_node *stack_dummy = 0; (void)stack_dummy;
    /* simple recursive walk */
    dom_node *child = 0;
    if (!root || dom_node_get_first_child(root, &child) != DOM_NO_ERR) return;
    while (child) {
        dom_node *next = 0; dom_node_type type = 0;
        dom_node_get_node_type(child, &type);
        if (type == DOM_ELEMENT_NODE) {
            char t[16]; tag_of(child, t, sizeof t);
            if (strcmp(t, "style") == 0) {
                /* concatenate text children */
                dom_node *tn = 0;
                if (dom_node_get_first_child(child, &tn) == DOM_NO_ERR) {
                    while (tn) {
                        dom_node *tnx = 0; dom_string *val = 0;
                        if (dom_node_get_node_value(tn, &val) == DOM_NO_ERR && val) {
                            int vl = (int)dom_string_byte_length(val);
                            const char *vd = (const char *)dom_string_data(val);
                            for (int i = 0; i < vl && *len < cap - 1; i++) buf[(*len)++] = vd[i];
                            dom_string_unref(val);
                        }
                        if (dom_node_get_next_sibling(tn, &tnx) != DOM_NO_ERR) { dom_node_unref(tn); break; }
                        dom_node_unref(tn); tn = tnx;
                    }
                }
            } else {
                gather_style_text(child, buf, cap, len);
            }
        }
        if (dom_node_get_next_sibling(child, &next) != DOM_NO_ERR) { dom_node_unref(child); break; }
        dom_node_unref(child); child = next;
    }
    buf[*len] = 0;
}

static void nst_msg(uint32_t sev, void *ctx, const char *msg, ...) { (void)sev; (void)ctx; (void)msg; }

/* ------------------------------- public --------------------------------- */
ns_page *ns_html_build(const char *html, uint32_t len, const char *base_url,
                       int width, char *title_out, int title_cap) {
    if (title_out && title_cap) title_out[0] = 0;
    if (ns_css_select_init() != 0) return 0;

    /* 1. HTML -> real libdom document */
    dom_hubbub_parser_params pp; memset(&pp, 0, sizeof pp);
    pp.enc = "UTF-8"; pp.fix_enc = true; pp.msg = nst_msg;
    dom_hubbub_parser *parser = 0; dom_document *doc = 0;
    if (dom_hubbub_parser_create(&pp, &parser, &doc) != DOM_HUBBUB_OK) return 0;
    dom_hubbub_parser_parse_chunk(parser, (const uint8_t *)html, len);
    dom_hubbub_parser_completed(parser);
    dom_hubbub_parser_destroy(parser);
    if (!doc) return 0;

    dom_element *root = 0;
    dom_document_get_document_element(doc, &root);

    ns_page *pg = (ns_page *)kmalloc(sizeof(ns_page));
    if (!pg) { dom_node_unref((dom_node *)doc); return 0; }
    memset(pg, 0, sizeof(*pg));
    if (base_url) pg->base = nsurl_create(base_url);

    /* title */
    if (title_out && title_cap && root) {
        dom_node *tn = find_tag((dom_node *)root, "title");
        if (tn) {
            dom_node *tt = 0;
            if (dom_node_get_first_child(tn, &tt) == DOM_NO_ERR && tt) {
                dom_string *val = 0;
                if (dom_node_get_node_value(tt, &val) == DOM_NO_ERR && val) {
                    int vl = (int)dom_string_byte_length(val);
                    const char *vd = (const char *)dom_string_data(val);
                    int o = 0; int i = 0;
                    while (i < vl && (vd[i]==' '||vd[i]=='\n'||vd[i]=='\t'||vd[i]=='\r')) i++;
                    for (; i < vl && o < title_cap - 1; i++) title_out[o++] = vd[i];
                    while (o > 0 && (title_out[o-1]==' '||title_out[o-1]=='\n'||title_out[o-1]=='\t'||title_out[o-1]=='\r')) o--;
                    title_out[o] = 0;
                    dom_string_unref(val);
                }
                dom_node_unref(tt);
            }
            dom_node_unref(tn);
        }
    }

    /* 2. CSS: UA sheet + author <style> blocks -> selection context */
    css_select_ctx *sctx = 0;
    css_select_ctx_create(&sctx);
    css_stylesheet *ua = make_sheet(UA_CSS, (int)strlen(UA_CSS), "boltos://ua", 0);
    if (ua) css_select_ctx_append_sheet(sctx, ua, CSS_ORIGIN_UA, 0);
    char *cssbuf = (char *)kmalloc(64 * 1024); int csslen = 0;
    css_stylesheet *author = 0;
    if (cssbuf) {
        gather_style_text((dom_node *)(root ? (dom_node *)root : (dom_node *)doc), cssbuf, 64 * 1024, &csslen);
        if (csslen > 0) {
            author = make_sheet(cssbuf, csslen, base_url, 0);
            if (author) css_select_ctx_append_sheet(sctx, author, CSS_ORIGIN_AUTHOR, 0);
        }
    }

    /* 3. style tree from <body> (fallback: document element) */
    dom_node *body = root ? find_tag((dom_node *)root, "body") : 0;
    dom_node *start = body ? body : (dom_node *)root;
    snode *sroot = snew();
    if (sroot && start) {
        sroot->display = 1; sroot->color = 0x1000000u | NS_TEXT_DEFAULT; sroot->fscale = 2;
        /* select the start element's own style as the parent for its children */
        css_select_results *res = ns_css_select_style(sctx, start, 0);
        const css_computed_style *bcs = res ? res->styles[CSS_PSEUDO_ELEMENT_NONE] : 0;
        if (bcs) { fill_style(sroot, bcs); sroot->display = 1; }
        build_children(pg, sctx, start, bcs, sroot, 0);
        if (res) css_select_results_destroy(res);
    }

    /* 4. layout into width */
    int doc_h = 0;
    if (sroot) doc_h = layout_block(pg, sroot, 0, 0, width);
    pg->doc_h = doc_h + 8;

    /* cleanup parse-time resources (display list is self-contained) */
    if (sroot) free_snode(sroot);
    if (cssbuf) kfree(cssbuf);
    if (author) css_stylesheet_destroy(author);
    if (ua) css_stylesheet_destroy(ua);
    if (sctx) css_select_ctx_destroy(sctx);
    if (body) dom_node_unref(body);
    if (root) dom_node_unref((dom_node *)root);
    dom_node_unref((dom_node *)doc);
    return pg;
}

int ns_page_height(ns_page *p) { return p ? p->doc_h : 0; }

void ns_page_paint(ns_page *p, int ox, int oy, int cl, int cr, int clipT, int clipB) {
    if (!p) return;
    for (int i = 0; i < p->n; i++) {
        ns_item *it = &p->items[i];
        int sx = ox + it->x, sy = oy + it->y;
        if (sy + it->h < clipT || sy > clipB) continue;
        if (it->kind == IT_RECT) {
            if (it->color & 0x1000000) g_fill(sx, sy, it->w, it->h, it->color & 0xFFFFFF);
            if (it->border & 0x1000000) g_rect(sx, sy, it->w, it->h, it->border & 0xFFFFFF);
        } else if (it->kind == IT_TEXT) {
            if (it->text) {
                g_text_pn(sx, sy, it->text, (int)strlen(it->text), it->color, it->scale ? it->scale : 2, it->italic);
                if (it->underline) g_hline(sx, sy + 8 * (it->scale ? it->scale : 2) + 1, it->w, it->color);
            }
        } else if (it->kind == IT_IMG) {
            if (it->img) g_blit(sx, sy, it->w, it->h, it->img->px, it->img->w, it->img->h);
            else { g_fill(sx, sy, it->w, it->h, it->color & 0xFFFFFF); g_rect(sx, sy, it->w, it->h, 0xC0C0C8);
                   if (it->text) g_text_pn(sx + 4, sy + it->h / 2 - 4, it->text, (int)strlen(it->text), 0x5F6368, 1, 0); }
        }
    }
}

const char *ns_page_href_at(ns_page *p, int cx, int cy) {
    if (!p) return 0;
    for (int i = p->n - 1; i >= 0; i--) {
        ns_item *it = &p->items[i];
        if (!it->href) continue;
        if (cx >= it->x && cx < it->x + it->w && cy >= it->y && cy < it->y + it->h)
            return it->href;
    }
    return 0;
}

void ns_page_free(ns_page *p) {
    if (!p) return;
    for (int i = 0; i < p->nstr; i++) kfree(p->strings[i]);
    if (p->strings) kfree(p->strings);
    for (int i = 0; i < p->nimg; i++) image_free(p->imgs[i]);
    if (p->imgs) kfree(p->imgs);
    if (p->items) kfree(p->items);
    if (p->base) nsurl_unref(p->base);
    kfree(p);
}
