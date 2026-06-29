/* ===========================================================================
 *  BoltOS  -  user/browser_main.c
 *  A web browser that runs entirely in RING 3.
 *
 *  Everything privilege-sensitive about rendering a web page -- the HTML
 *  tokeniser + DOM tree builder (kernel/dom.c), the CSS cascade + box/flex/grid
 *  layout engine (kernel/layout.c), and the BoltJS interpreter (kernel/js.c) --
 *  is compiled into THIS userland ELF and executes in ring 3. The kernel keeps
 *  only the privileged primitives behind syscalls:
 *      SYS_FBINFO  - map the framebuffer + pause the compositor
 *      SYS_GETKEY  - read the keyboard
 *      SYS_HTTPGET - fetch a URL over the network stack (TCP/TLS in ring 0)
 *      SYS_FBEND   - hand the screen back to the desktop
 *  No DOM construction, layout, or script execution happens in kernel mode.
 *
 *      usage:  browser [url-or-path]
 *
 *  Keys:  arrows / PgUp / PgDn / Space  scroll      Tab    next link
 *         Enter   follow link                       g      edit address
 *         r       reload                            q/Esc  quit
 * ===========================================================================*/
#include "ulibc.h"
#include "string.h"
#include "dom.h"
#include "layout.h"
#include "js.h"

/* the public-domain 8x8 bitmap font, compiled in from kernel/font8x8.c */
extern const unsigned char font8x8_basic[128][8];

/* ----------------------------------------------------------------- screen --*/
/* The renderer paints into VRAM (a packed FB.w*FB.h RAM backbuffer) and hands it
 * to the kernel with fb_present() each frame; the kernel performs the actual
 * panel write. No framebuffer memory is mapped into this ring-3 process. */
static struct user_fbinfo FB;
static uint32_t *VRAM;

#define TOPBAR 28                       /* address/status chrome height */
#define COL_BG     0x00FFFFFF
#define COL_TEXT   0x00101015
#define COL_CHROME 0x00202833
#define COL_BAR    0x00F0F0F4
#define COL_LINK   0x000A3AD0
#define COL_SEL    0x00FF6A00

static void px(int x, int y, uint32_t c) {
    if ((unsigned)x < FB.w && (unsigned)y < FB.h) VRAM[(unsigned)y * FB.pitch_px + x] = c;
}
static void fill(int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++) {
        int yy = y + j; if ((unsigned)yy >= FB.h) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i; if ((unsigned)xx >= FB.w) continue;
            VRAM[(unsigned)yy * FB.pitch_px + xx] = c;
        }
    }
}
static void frame(int x, int y, int w, int h, uint32_t c) {
    for (int i = 0; i < w; i++) { px(x + i, y, c); px(x + i, y + h - 1, c); }
    for (int j = 0; j < h; j++) { px(x, y + j, c); px(x + w - 1, y + j, c); }
}
static void glyph(int x, int y, char ch, uint32_t c, int s) {
    unsigned uc = (unsigned char)ch; if (uc > 127) uc = '?';
    for (int row = 0; row < 8; row++) {
        unsigned bits = font8x8_basic[uc][row];
        for (int col = 0; col < 8; col++)
            if (bits & (1u << col))
                for (int dy = 0; dy < s; dy++)
                    for (int dx = 0; dx < s; dx++) px(x + col * s + dx, y + row * s + dy, c);
    }
}
static void text(int x, int y, const char *str, uint32_t c, int s) {
    for (; *str; str++) { if (*str != ' ') glyph(x, y, *str, c, s); x += 8 * s; }
}

/* ------------------------------------------------------------- page state --*/
#define PAGE_CAP  (768 * 1024)
#define CSS_CAP   (96  * 1024)
#define MAX_LINKS 512

static char  *pagebuf;                  /* raw HTML */
static char  *cssbuf;                   /* concatenated <style> + linked CSS */
static uint32_t csslen;
static char   cur_url[1024];
static char   title[256];

static dom_document *doc;
static layout_tree  *lt;
static int    scroll;
static int    nlink, sel_link;
static struct { int x, y, w, h; char href[512]; } links[MAX_LINKS];

/* log lines from console.log(), shown along the bottom of the chrome */
static char logbuf[256];

/* ------------------------------------------------------- JS <-> DOM host ----*/
static void  h_log  (void *u, const char *m){ (void)u; strncpy(logbuf, m, sizeof logbuf - 1); logbuf[sizeof logbuf - 1] = 0; }
static void  h_write(void *u, const char *h){ (void)u; (void)h; }
static js_dom_node h_byid(void *u, const char *i){ return dom_get_by_id((dom_document *)u, i); }
static js_dom_node h_bytag(void *u, const char *t, int i){
    dom_node *o[64]; int n = dom_by_tag(((dom_document *)u)->root, t, o, 64);
    return (i >= 0 && i < n) ? (js_dom_node)o[i] : 0;
}
static void  h_sinner(void *u, js_dom_node n, const char *h){ dom_set_text((dom_document *)u, (dom_node *)n, h); }
static int   h_ginner(void *u, js_dom_node n, char *o, uint32_t c){ (void)u; return dom_inner_html((dom_node *)n, o, c); }
static void  h_stext(void *u, js_dom_node n, const char *t){ dom_set_text((dom_document *)u, (dom_node *)n, t); }
static void  h_title(void *u, const char *t){ (void)u; strncpy(title, t, sizeof title - 1); title[sizeof title - 1] = 0; }
static js_dom_node h_query(void *u, const char *s){ return dom_query(((dom_document *)u)->root, s); }
static js_dom_node h_create(void *u, const char *t){ return dom_create_element((dom_document *)u, t); }
static void  h_append(void *u, js_dom_node p, js_dom_node c){ (void)u; dom_append_child((dom_node *)p, (dom_node *)c); }
static void  h_setattr(void *u, js_dom_node n, const char *k, const char *v){ dom_attr_set((dom_document *)u, (dom_node *)n, k, v); }
static int   h_getattr(void *u, js_dom_node n, const char *k, char *o, uint32_t c){
    (void)u; const char *v = dom_attr_get((dom_node *)n, k);
    if (!v) { if (c) o[0] = 0; return 0; }
    uint32_t l = (uint32_t)strlen(v); if (l > c - 1) l = c - 1;
    memcpy(o, v, l); o[l] = 0; return (int)l;
}
/* fetch(): the ring-3 engine reaches the kernel network stack through SYS_HTTPGET */
static int h_fetch(void *u, const char *url, char *out, uint32_t cap, int *status){
    (void)u; return (int)http_get_u(url, out, cap, status);
}

static js_host host;
static void host_init(void) {
    memset(&host, 0, sizeof host);
    host.ud = 0;                    /* set to `doc` before each run */
    host.get_by_id = h_byid;  host.get_by_tag = h_bytag;
    host.set_inner = h_sinner; host.get_inner = h_ginner;
    host.set_text = h_stext;   host.doc_write = h_write;
    host.set_title = h_title;  host.log = h_log;
    host.query = h_query;      host.create_el = h_create;
    host.append_child = h_append;
    host.set_attr = h_setattr; host.get_attr = h_getattr;
    host.fetch = h_fetch;
}

/* ----------------------------------------------------------- page loading --*/
/* Pull every <style>...</style> block (and inline <link rel=stylesheet> is left
 * to the kernel build; here we honour embedded CSS, which covers most pages). */
static void collect_css(const char *html, int hlen) {
    csslen = 0;
    for (int i = 0; i + 6 < hlen; i++) {
        if ((html[i] == '<') &&
            (html[i+1] == 's' || html[i+1] == 'S') &&
            !strncasecmp(html + i + 1, "style", 5)) {
            int j = i + 6; while (j < hlen && html[j] != '>') j++;   /* skip to '>' */
            j++;
            int s0 = j;
            while (j + 7 < hlen && !(html[j] == '<' && html[j+1] == '/' &&
                   !strncasecmp(html + j + 2, "style", 5))) j++;
            int n = j - s0;
            if (n > 0 && csslen + (uint32_t)n + 1 < CSS_CAP) {
                memcpy(cssbuf + csslen, html + s0, n); csslen += n;
                cssbuf[csslen++] = '\n';
            }
            i = j;
        }
    }
    cssbuf[csslen] = 0;
}

/* Run each <script>...</script> in ring 3 against the live DOM. */
static void run_scripts(const char *html, int hlen) {
    char err[160];
    host.ud = doc;
    for (int i = 0; i + 7 < hlen; i++) {
        if (html[i] == '<' && !strncasecmp(html + i + 1, "script", 6)) {
            int j = i + 7; while (j < hlen && html[j] != '>') j++;     /* skip attrs */
            /* a <script src=...> has no inline body to run; skip to its close */
            j++;
            int s0 = j;
            while (j + 8 < hlen && !(html[j] == '<' && html[j+1] == '/' &&
                   !strncasecmp(html + j + 2, "script", 6))) j++;
            int n = j - s0;
            if (n > 0) js_run(html + s0, (uint32_t)n, &host, err, sizeof err);
            i = j;
        }
    }
}

/* Build (or rebuild) the DOM + layout tree for the current viewport width. */
static void build(int hlen) {
    if (lt)  { layout_free(lt);  lt = 0; }
    if (doc) { dom_free(doc);    doc = 0; }
    title[0] = 0;
    collect_css(pagebuf, hlen);
    doc = dom_parse(pagebuf, (uint32_t)hlen);
    if (!doc) return;
    run_scripts(pagebuf, hlen);                 /* scripts may mutate the DOM */
    lt = layout_build(doc, cssbuf, csslen, (int)FB.w);
}

/* Fetch over http(s) via the kernel, or read a local file via the VFS. */
static int load(const char *url) {
    strncpy(cur_url, url, sizeof cur_url - 1); cur_url[sizeof cur_url - 1] = 0;
    logbuf[0] = 0;
    if (!strncasecmp(url, "http://", 7) || !strncasecmp(url, "https://", 8)) {
        int status = 0;
        long n = http_get_u(url, pagebuf, PAGE_CAP, &status);
        if (n < 0) { strcpy(logbuf, "network error"); return -1; }
        pagebuf[n] = 0; return (int)n;
    }
    int fd = open(url, O_RDONLY);
    if (fd < 0) { strcpy(logbuf, "cannot open"); return -1; }
    long n = read(fd, pagebuf, PAGE_CAP - 1); close(fd);
    if (n < 0) n = 0;
    pagebuf[n] = 0; return (int)n;
}

/* ----------------------------------------------------------- rendering -----*/
static const char *box_href(layout_box *b) {
    for (dom_node *n = b->node; n; n = n->parent)
        if (n->tag && n->tag[0] == 'a' && n->tag[1] == 0) {
            const char *h = dom_attr_get(n, "href");
            if (h && h[0]) return h;
        }
    return 0;
}

static void paint_box(layout_box *b, int ox, int oy) {
    if (!b) return;
    computed_style *s = &b->st;
    int sx = ox + b->x, sy = oy + b->y;

    if (b->is_text) {
        if (b->text && sy + b->h > TOPBAR && sy < (int)FB.h) {
            uint32_t c = (s->color & 0x1000000) ? (s->color & 0xFFFFFF) : COL_TEXT;
            const char *href = box_href(b);
            if (href) c = COL_LINK;
            int scale = s->fscale ? s->fscale : 1;
            text(sx, sy, b->text, c, scale);
            if (href && nlink < MAX_LINKS) {
                int w = (int)strlen(b->text) * 8 * scale;
                links[nlink].x = sx; links[nlink].y = sy;
                links[nlink].w = w;  links[nlink].h = 8 * scale;
                strncpy(links[nlink].href, href, sizeof links[0].href - 1);
                links[nlink].href[sizeof links[0].href - 1] = 0;
                nlink++;
            }
        }
        return;
    }

    int vis = (sy + b->h > TOPBAR) && (sy < (int)FB.h);
    if (vis) {
        if (s->bg & 0x1000000) fill(sx, sy, b->w, b->h, s->bg & 0xFFFFFF);
        if (s->border[0] || s->border[1] || s->border[2] || s->border[3])
            frame(sx, sy, b->w, b->h, 0x00C0C0C8);
    }
    for (layout_box *c = b->first_child; c; c = c->next) paint_box(c, ox, oy);
}

static int url_edit;                    /* 1 while typing in the address bar */
static char url_buf[1024];

static void paint_chrome(void) {
    fill(0, 0, (int)FB.w, TOPBAR, COL_CHROME);
    /* address bar */
    fill(6, 4, (int)FB.w - 12, TOPBAR - 8, COL_BAR);
    const char *shown = url_edit ? url_buf : cur_url;
    int max = ((int)FB.w - 24) / 8;
    char line[256]; int n = (int)strlen(shown);
    int start = n > max ? n - max : 0;
    strncpy(line, shown + start, sizeof line - 1); line[sizeof line - 1] = 0;
    text(12, 10, line, 0x00101015, 1);
    if (url_edit) frame(6, 4, (int)FB.w - 12, TOPBAR - 8, COL_SEL);
    /* status / title / log along the very top-right, drawn small */
    if (title[0]) {
        int tw = (int)strlen(title) * 8;
        if (tw < (int)FB.w - 16) text((int)FB.w - tw - 8, 2, title, 0x00A0A8B4, 1);
    }
}

static void render(void) {
    fill(0, TOPBAR, (int)FB.w, (int)FB.h - TOPBAR, COL_BG);
    nlink = 0;
    if (lt && lt->root) paint_box(lt->root, 0, TOPBAR - scroll);
    if (sel_link >= nlink) sel_link = nlink ? nlink - 1 : 0;
    if (nlink && sel_link < nlink)                     /* highlight the focused link */
        frame(links[sel_link].x - 1, links[sel_link].y - 1,
              links[sel_link].w + 2, links[sel_link].h + 2, COL_SEL);
    paint_chrome();
    if (logbuf[0]) {
        int lw = (int)strlen(logbuf);
        fill(0, (int)FB.h - 14, lw * 8 + 8, 14, 0x00202020);
        text(4, (int)FB.h - 12, logbuf, 0x0040FF60, 1);
    }
}

/* Resolve a possibly-relative href against the current URL (scheme/host kept). */
static void resolve(const char *href, char *out, int cap) {
    if (!strncasecmp(href, "http://", 7) || !strncasecmp(href, "https://", 8)) {
        strncpy(out, href, cap - 1); out[cap - 1] = 0; return;
    }
    /* find scheme://host end of cur_url */
    const char *p = strstr(cur_url, "://");
    if (!p) { strncpy(out, href, cap - 1); out[cap - 1] = 0; return; }
    const char *hoststart = p + 3;
    const char *slash = strchr(hoststart, '/');
    int rootlen = slash ? (int)(slash - cur_url) : (int)strlen(cur_url);
    if (href[0] == '/') {                       /* root-relative */
        int k = rootlen; if (k > cap - 1) k = cap - 1;
        memcpy(out, cur_url, k);
        strncpy(out + k, href, cap - 1 - k); out[cap - 1] = 0;
    } else {                                    /* doc-relative: append after last '/' */
        int base = rootlen;
        for (int i = rootlen; cur_url[i]; i++) if (cur_url[i] == '/') base = i + 1;
        if (base > cap - 1) base = cap - 1;
        memcpy(out, cur_url, base);
        strncpy(out + base, href, cap - 1 - base); out[cap - 1] = 0;
    }
}

static void navigate(const char *url) {
    int n = load(url);
    scroll = 0; sel_link = 0;
    if (n >= 0) build(n);
}

/* ----------------------------------------------------------------- main ----*/
int main(int argc, char **argv) {
    pagebuf = (char *)malloc(PAGE_CAP);
    cssbuf  = (char *)malloc(CSS_CAP);
    if (!pagebuf || !cssbuf) { printf("browser: out of memory\n"); return 1; }

    if (fb_map(&FB) != 0) { printf("browser: no framebuffer available\n"); return 1; }
    VRAM = (uint32_t *)malloc((uint64_t)FB.w * FB.h * 4);
    if (!VRAM) { fb_release(); printf("browser: backbuffer alloc failed\n"); return 1; }
    host_init();

    const char *start = (argc > 1) ? argv[1] : "/web/index.html";
    navigate(start);
    render();

    int step = 60;                              /* scroll amount per key */
    for (;;) {
        fb_present(VRAM);                        /* push the backbuffer to the panel */
        int k = getkey();
        if (k < 0) { yield(); continue; }       /* idle: let other threads run */

        if (url_edit) {                          /* ---- address-bar editing ---- */
            if (k == 27) { url_edit = 0; }                       /* Esc cancel */
            else if (k == '\n' || k == '\r') {
                url_edit = 0;
                if (url_buf[0]) navigate(url_buf);
            } else if (k == 8 || k == 0x7F) {
                int l = (int)strlen(url_buf); if (l) url_buf[l - 1] = 0;
            } else if (k >= 32 && k < 127) {
                int l = (int)strlen(url_buf);
                if (l < (int)sizeof url_buf - 1) { url_buf[l] = (char)k; url_buf[l + 1] = 0; }
            }
            render();
            continue;
        }

        switch (k) {                             /* ---- normal mode ---- */
        case 'q': case 27:
            fb_release(); return 0;
        case 'g': case '/':
            url_edit = 1; url_buf[0] = 0; break;
        case 'r':
            navigate(cur_url); break;
        case '\t':
            if (nlink) sel_link = (sel_link + 1) % nlink; break;
        case '\n': case '\r':
            if (nlink && sel_link < nlink) {
                char u[1024]; resolve(links[sel_link].href, u, sizeof u); navigate(u);
            }
            break;
        case 0x11: /* KEY_UP   */ scroll -= step; break;
        case 0x12: /* KEY_DOWN */ scroll += step; break;
        case 0x10: /* KEY_PGUP */ scroll -= (int)FB.h - TOPBAR - 20; break;
        case 0x0E: /* KEY_PGDN */
        case ' ':                 scroll += (int)FB.h - TOPBAR - 20; break;
        case 0x02: /* KEY_HOME */ scroll = 0; break;
        default: break;
        }
        int maxs = lt ? lt->doc_h - ((int)FB.h - TOPBAR) : 0;
        if (maxs < 0) maxs = 0;
        if (scroll < 0) scroll = 0;
        if (scroll > maxs) scroll = maxs;
        render();
    }
}
