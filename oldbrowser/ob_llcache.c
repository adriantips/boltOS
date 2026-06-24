/* ===========================================================================
 *  ob_llcache.c  --  NetSurf content/llcache.c (low-level cache / fetch)
 *
 *  Pulls the raw source for an nsurl over the kernel HTTP/HTTPS stack
 *  (net/http.c), transparently following 3xx redirects (resolving each Location
 *  against the current URL via nsurl_join, exactly as NetSurf's fetch layer
 *  does), and synthesising the about: scheme pages. The high-level cache
 *  (content_create in ob_content.c) consumes whatever bytes this returns.
 * ===========================================================================*/
#include "oldbrowser.h"
#include "string.h"
#include "kheap.h"
#include "kprintf.h"
#include "http.h"
#include <stdarg.h>

#define OB_FETCH_CAP   (256u * 1024)
#define OB_MAX_REDIR   6

void ob_log(const char *fmt, ...) {
    /* minimal logger: %s and %d, prefixed -- utils/log.h analogue */
    char buf[256]; uint32_t o = 0;
    va_list ap; va_start(ap, fmt);
    for (const char *f = fmt; *f && o < sizeof(buf) - 1; f++) {
        if (*f != '%') { buf[o++] = *f; continue; }
        f++;
        if (*f == 's') { const char *s = va_arg(ap, const char *); if (!s) s = "(null)";
                         while (*s && o < sizeof(buf) - 1) buf[o++] = *s++; }
        else if (*f == 'd') { int v = va_arg(ap, int); char t[12]; int n = 0, neg = v < 0; unsigned uv = neg ? -v : v;
                         if (!uv) t[n++] = '0'; while (uv) { t[n++] = '0' + uv % 10; uv /= 10; }
                         if (neg && o < sizeof(buf) - 1) buf[o++] = '-';
                         while (n && o < sizeof(buf) - 1) buf[o++] = t[--n]; }
        else if (*f) buf[o++] = *f;
    }
    va_end(ap);
    buf[o] = 0;
    kprintf("[oldbrowser] %s\n", buf);
}

/* ------------------------- about: scheme pages -------------------------- */
static const char *about_page(const char *which) {
    if (strcmp(which, "blank") == 0) return "<html><body></body></html>";
    if (strcmp(which, "credits") == 0)
        return "<html><head><title>About OldBrowser</title></head><body>"
               "<h1>OldBrowser</h1>"
               "<p>A <b>NetSurf</b> port for BoltOS - a small, fast browser "
               "built on the NetSurf core architecture (content cache, handler "
               "pipeline and browser_window state machine), driving BoltOS's own "
               "DOM tree, CSS cascade and box-model layout engine over the "
               "kernel's TCP/TLS network stack.</p>"
               "<h3>Credits</h3>"
               "<p>NetSurf is (c) The NetSurf Developers, GPLv2. This port "
               "reimplements its architecture for BoltOS.</p></body></html>";
    /* welcome / home */
    return "<html><head><title>OldBrowser</title></head><body>"
           "<h1>OldBrowser</h1>"
           "<p><b>NetSurf</b>, ported to BoltOS.</p>"
           "<p>Type a URL or a search term in the address bar and press Enter. "
           "Use the toolbar to go Back, Forward, Stop, Reload or Home, and the "
           "star to bookmark the current page.</p>"
           "<h3>Try these</h3>"
           "<ul>"
           "<li><a href=\"http://example.com/\">example.com</a></li>"
           "<li><a href=\"http://info.cern.ch/\">info.cern.ch - the first website</a></li>"
           "<li><a href=\"about:credits\">about:credits</a></li>"
           "</ul>"
           "<p>OldBrowser renders with the same box / flex / grid layout engine "
           "that powers the BoltOS layout core, so real CSS pages lay out with "
           "margins, widths and inline flow.</p>"
           "</body></html>";
}

static char *dup_str(const char *s, uint32_t *lenout) {
    uint32_t n = strlen(s);
    char *p = (char *)kmalloc(n + 1);
    if (!p) return 0;
    memcpy(p, s, n + 1);
    if (lenout) *lenout = n;
    return p;
}

int llcache_fetch(nsurl *url, char **out, int *status, nsurl **final) {
    if (out)   *out = 0;
    if (final) *final = 0;
    if (status) *status = 0;
    if (!url) return -1;

    if (strcmp(url->scheme, "about") == 0) {
        const char *which = url->path[0] ? url->path : "welcome";
        uint32_t n = 0;
        char *p = dup_str(about_page(which), &n);
        if (!p) return -1;
        if (out) *out = p;
        if (status) *status = 200;
        if (final) *final = nsurl_ref(url);
        return (int)n;
    }

    if (strcmp(url->scheme, "http") != 0 && strcmp(url->scheme, "https") != 0) {
        if (status) *status = -1;
        return -1;
    }

    nsurl *cur = nsurl_ref(url);
    for (int hop = 0; hop < OB_MAX_REDIR; hop++) {
        char *buf = (char *)kmalloc(OB_FETCH_CAP);
        if (!buf) { nsurl_unref(cur); if (status) *status = -2; return -1; }
        int code = 0; char loc[1024]; loc[0] = 0;
        int n = http_get(cur->full, buf, OB_FETCH_CAP, &code, loc, sizeof(loc));
        if (n < 0) {
            kfree(buf); nsurl_unref(cur);
            if (status) *status = code;
            return -1;
        }
        if (code >= 300 && code < 400 && loc[0]) {
            kfree(buf);
            nsurl *nu = nsurl_join(cur, loc);
            nsurl_unref(cur);
            if (!nu) { if (status) *status = code; return -1; }
            cur = nu;
            ob_log("redirect -> %s", cur->full);
            continue;
        }
        if (out) *out = buf; else kfree(buf);
        if (status) *status = code;
        if (final) *final = cur; else nsurl_unref(cur);
        return n;
    }
    nsurl_unref(cur);
    if (status) *status = -6;     /* too many redirects */
    return -1;
}
