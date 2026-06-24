/* ===========================================================================
 *  ob_window.c  --  NetSurf desktop/browser_window.c + desktop/browser_history.c
 *
 *  The browser_window: the per-tab state machine that ties a URL to a rendered
 *  content, owns the scroll offset and the back/forward history list, and turns
 *  user gestures (typed address, link click, scroll, nav buttons) into
 *  navigations. browser_window_navigate runs the fetch -> create -> reformat
 *  pipeline, exactly as NetSurf's core does (minus the async scheduler: BoltOS
 *  fetches synchronously on the GUI thread).
 * ===========================================================================*/
#include "oldbrowser.h"
#include "string.h"
#include "kheap.h"
#include "keyboard.h"

#define HIST_MAX 32

struct browser_window {
    content *current;
    nsurl   *url;
    int      scroll_y;
    int      view_w, view_h;
    int      loading;
    char     status[160];
    char     title[160];
    /* local history (browser_history) */
    nsurl   *hist[HIST_MAX];
    char     hist_title[HIST_MAX][120];
    int      hist_len;
    int      hist_pos;       /* index of the current entry, -1 if empty */
};

static void scopy(char *d, const char *s, uint32_t cap) {
    if (!cap) return; uint32_t i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0;
}

browser_window *browser_window_create(void) {
    browser_window *bw = (browser_window *)kmalloc(sizeof(browser_window));
    if (!bw) return 0;
    memset(bw, 0, sizeof(*bw));
    bw->hist_pos = -1;
    bw->view_w = 760;
    scopy(bw->status, "Ready", sizeof(bw->status));
    return bw;
}

/* ------------------------------ history --------------------------------- */
static void history_push(browser_window *bw, nsurl *url, const char *title) {
    /* drop any forward entries */
    for (int i = bw->hist_pos + 1; i < bw->hist_len; i++) { nsurl_unref(bw->hist[i]); bw->hist[i] = 0; }
    bw->hist_len = bw->hist_pos + 1;
    if (bw->hist_len >= HIST_MAX) {
        nsurl_unref(bw->hist[0]);
        for (int i = 1; i < bw->hist_len; i++) { bw->hist[i-1] = bw->hist[i];
            scopy(bw->hist_title[i-1], bw->hist_title[i], 120); }
        bw->hist_len--; bw->hist_pos--;
    }
    bw->hist[bw->hist_len] = nsurl_ref(url);
    scopy(bw->hist_title[bw->hist_len], title ? title : "", 120);
    bw->hist_pos = bw->hist_len;
    bw->hist_len++;
}

/* ------------------------------ navigate -------------------------------- */
int browser_window_navigate(browser_window *bw, nsurl *url, int add_to_hist) {
    if (!bw || !url) return -1;
    bw->loading = 1;
    scopy(bw->status, "Fetching ", sizeof(bw->status));
    { uint32_t n = strlen(bw->status); scopy(bw->status + n, url->full, sizeof(bw->status) - n); }

    char *data = 0; int status = 0; nsurl *fin = 0;
    int n = llcache_fetch(url, &data, &status, &fin);
    nsurl *eff = fin ? fin : url;

    content *nc;
    if (n < 0 || !data) {
        const char *why = "Connection failed";
        if (status == -3) why = "Could not resolve host (DNS)";
        else if (status == -4) why = "Could not connect (TCP)";
        else if (status == -5) why = "TLS handshake failed";
        else if (status == -6) why = "Too many redirects";
        else if (status >= 400) why = "Server returned an error";
        nc = content_create_error(eff, why);
    } else {
        nc = content_create(eff, data, (uint32_t)n, status);
    }
    if (data) kfree(data);

    if (!nc) { bw->loading = 0; scopy(bw->status, "Out of memory", sizeof(bw->status)); if (fin) nsurl_unref(fin); return -1; }

    if (bw->current) content_destroy(bw->current);
    bw->current = nc;
    if (bw->url) nsurl_unref(bw->url);
    bw->url = nsurl_ref(eff);
    bw->scroll_y = 0;
    scopy(bw->title, content_get_title(nc), sizeof(bw->title));

    content_reformat(nc, bw->view_w);

    if (add_to_hist) history_push(bw, eff, bw->title);

    if (nc->type == CONTENT_ERROR) scopy(bw->status, "Error", sizeof(bw->status));
    else {
        char st[160]; scopy(st, "Done", sizeof(st));
        if (status > 0) { char num[8]; int v = status, i = 0, j; char t[8];
            if (!v) t[i++]='0'; while (v){t[i++]='0'+v%10;v/=10;} for (j=0;i>0;) num[j++]=t[--i]; num[j]=0;
            scopy(st, "HTTP ", sizeof(st)); uint32_t L = strlen(st); scopy(st+L, num, sizeof(st)-L); }
        scopy(bw->status, st, sizeof(bw->status));
    }
    if (fin) nsurl_unref(fin);
    bw->loading = 0;
    return 0;
}

/* ---------------------- typed-address handling -------------------------- */
static int is_url_like(const char *s) {
    if (ob_strstr(s, "://")) return 1;
    if (strncmp(s, "about:", 6) == 0) return 1;
    /* must have no spaces, a dot before any slash, and look host-ish */
    for (const char *p = s; *p; p++) if (*p == ' ') return 0;
    const char *slash = strchr(s, '/');
    const char *dot = strchr(s, '.');
    if (dot && (!slash || dot < slash)) return 1;
    if (strcmp(s, "localhost") == 0) return 1;
    return 0;
}
static void pct_encode(const char *in, char *out, uint32_t cap) {
    static const char *hex = "0123456789ABCDEF";
    uint32_t o = 0;
    for (const char *p = in; *p && o < cap - 4; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') out[o++] = c;
        else if (c == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

void browser_window_go(browser_window *bw, const char *addr) {
    if (!bw || !addr) return;
    while (*addr == ' ') addr++;
    if (!*addr) return;
    nsurl *u;
    if (is_url_like(addr)) {
        u = nsurl_create(addr);
    } else {
        char q[1024]; pct_encode(addr, q, sizeof(q));
        char full[1100]; scopy(full, "https://duckduckgo.com/html/?q=", sizeof(full));
        uint32_t L = strlen(full); scopy(full + L, q, sizeof(full) - L);
        u = nsurl_create(full);
    }
    if (!u) return;
    browser_window_navigate(bw, u, 1);
    nsurl_unref(u);
}

/* ------------------------------ gestures -------------------------------- */
void browser_window_reformat(browser_window *bw, int width) {
    if (!bw) return;
    bw->view_w = width;
    if (bw->current) content_reformat(bw->current, width);
}

void browser_window_redraw(browser_window *bw, int ox, int oy, int cl, int cr, int clipT, int clipB) {
    if (bw && bw->current) content_redraw(bw->current, bw, ox, oy, cl, cr, clipT, clipB);
}

int browser_window_link_at(browser_window *bw, int cx, int cy, char *out, uint32_t cap) {
    if (!bw || !bw->current) return 0;
    const char *href = content_href_at(bw->current, cx, cy);
    if (!href) return 0;
    nsurl *u = nsurl_join(bw->url, href);
    if (!u) return 0;
    scopy(out, u->full, cap);
    nsurl_unref(u);
    return 1;
}

void browser_window_mouse_click(browser_window *bw, int cx, int cy) {
    if (!bw || !bw->current) return;
    const char *href = content_href_at(bw->current, cx, cy);
    if (!href) return;
    nsurl *u = nsurl_join(bw->url, href);
    if (!u) return;
    browser_window_navigate(bw, u, 1);
    nsurl_unref(u);
}

void browser_window_scroll(browser_window *bw, int delta, int viewport_h) {
    if (!bw) return;
    bw->view_h = viewport_h;
    bw->scroll_y += delta;
    int ch = browser_window_content_height(bw);
    int maxs = ch - viewport_h; if (maxs < 0) maxs = 0;
    if (bw->scroll_y < 0) bw->scroll_y = 0;
    if (bw->scroll_y > maxs) bw->scroll_y = maxs;
}

void browser_window_key(browser_window *bw, char c) {
    if (!bw) return;
    int vh = bw->view_h > 0 ? bw->view_h : 400;
    switch ((unsigned char)c) {
        case ' ':       browser_window_scroll(bw, vh - 32, vh); break;
        case 'b':       browser_window_scroll(bw, -(vh - 32), vh); break;
        case 'j': case KEY_DOWN: browser_window_scroll(bw, 40, vh); break;
        case 'k': case KEY_UP:   browser_window_scroll(bw, -40, vh); break;
        case KEY_PGDN:  browser_window_scroll(bw, vh - 32, vh); break;
        case KEY_PGUP:  browser_window_scroll(bw, -(vh - 32), vh); break;
        case KEY_HOME:  bw->scroll_y = 0; break;
        case KEY_END:   browser_window_scroll(bw, browser_window_content_height(bw), vh); break;
    }
}

int  browser_window_back_available(browser_window *bw)    { return bw && bw->hist_pos > 0; }
int  browser_window_forward_available(browser_window *bw) { return bw && bw->hist_pos >= 0 && bw->hist_pos < bw->hist_len - 1; }

void browser_window_history_back(browser_window *bw) {
    if (!browser_window_back_available(bw)) return;
    bw->hist_pos--;
    browser_window_navigate(bw, bw->hist[bw->hist_pos], 0);
}
void browser_window_history_forward(browser_window *bw) {
    if (!browser_window_forward_available(bw)) return;
    bw->hist_pos++;
    browser_window_navigate(bw, bw->hist[bw->hist_pos], 0);
}
void browser_window_reload(browser_window *bw) {
    if (bw && bw->url) { nsurl *u = nsurl_ref(bw->url); browser_window_navigate(bw, u, 0); nsurl_unref(u); }
}
void browser_window_stop(browser_window *bw) { if (bw) { bw->loading = 0; scopy(bw->status, "Stopped", sizeof(bw->status)); } }
void browser_window_home(browser_window *bw) {
    nsurl *u = nsurl_create("about:welcome");
    if (u) { browser_window_navigate(bw, u, 1); nsurl_unref(u); }
}

nsurl      *browser_window_get_url(browser_window *bw)    { return bw ? bw->url : 0; }
const char *browser_window_get_title(browser_window *bw)  { return bw ? bw->title : ""; }
const char *browser_window_get_status(browser_window *bw) { return bw ? bw->status : ""; }
int  browser_window_get_scroll(browser_window *bw)        { return bw ? bw->scroll_y : 0; }
int  browser_window_is_loading(browser_window *bw)        { return bw ? bw->loading : 0; }
content *browser_window_get_content(browser_window *bw)   { return bw ? bw->current : 0; }
int  browser_window_content_height(browser_window *bw) {
    return (bw && bw->current) ? content_get_height(bw->current) : 0;
}

void browser_window_destroy(browser_window *bw) {
    if (!bw) return;
    if (bw->current) content_destroy(bw->current);
    if (bw->url) nsurl_unref(bw->url);
    for (int i = 0; i < bw->hist_len; i++) if (bw->hist[i]) nsurl_unref(bw->hist[i]);
    kfree(bw);
}
