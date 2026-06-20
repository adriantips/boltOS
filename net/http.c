#include <stdint.h>
#include "http.h"
#include "net.h"
#include "tls.h"
#include "string.h"
#include "kprintf.h"
#include "kheap.h"

/* split "http[s]://host:port/path" -> host, port, path; *secure set for https.
 * Returns 0 on success. */
static int parse_url(const char *url, char *host, uint32_t hcap, uint16_t *port,
                     char *path, uint32_t pcap, int *secure) {
    const char *s = url;
    *secure = 0;
    if (strncmp(s, "http://", 7) == 0) s += 7;
    else if (strncmp(s, "https://", 8) == 0) { s += 8; *secure = 1; }

    uint32_t i = 0;
    while (*s && *s != '/' && *s != ':' && i < hcap - 1) host[i++] = *s++;
    host[i] = 0;
    if (i == 0) return -1;

    *port = *secure ? 443 : 80;
    if (*s == ':') {
        s++; int p = 0;
        while (*s >= '0' && *s <= '9') p = p * 10 + (*s++ - '0');
        if (p > 0 && p < 65536) *port = (uint16_t)p;
    }

    i = 0;
    if (*s != '/') path[i++] = '/';
    while (*s && i < pcap - 1) path[i++] = *s++;
    path[i] = 0;
    return 0;
}

static int parse_status(const char *resp) {       /* "HTTP/1.x NNN ..." */
    const char *p = resp;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    int code = 0;
    while (*p >= '0' && *p <= '9') code = code * 10 + (*p++ - '0');
    return code;
}

/* case-insensitive header lookup within the header block; copies value to out */
static void find_header(const char *hdr, const char *name, char *out, uint32_t cap) {
    if (cap) out[0] = 0;
    uint32_t nl = strlen(name);
    for (const char *p = hdr; *p; ) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        /* compare name: prefix of line, case-insensitive, followed by ':' */
        uint32_t k = 0;
        while (k < nl && line[k]) {
            char a = line[k], b = name[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            k++;
        }
        if (k == nl && line[k] == ':') {
            const char *v = line + k + 1;
            while (*v == ' ') v++;
            uint32_t o = 0;
            while (*v && *v != '\r' && *v != '\n' && o < cap - 1) out[o++] = *v++;
            out[o] = 0;
            return;
        }
        if (*p == '\n') p++;
    }
}

/* in-place decode of an HTTP/1.1 "Transfer-Encoding: chunked" body. Returns the
 * decoded length. Tolerant of chunk extensions and a missing final CRLF. */
static int dechunk(char *buf, uint32_t len) {
    uint32_t r = 0, w = 0;
    while (r < len) {
        uint32_t sz = 0; int any = 0;
        while (r < len) {
            char ch = buf[r];
            int dv = (ch >= '0' && ch <= '9') ? ch - '0'
                   : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
                   : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
            if (dv < 0) break;
            sz = sz * 16 + (uint32_t)dv; r++; any = 1;
        }
        while (r < len && buf[r] != '\n') r++;     /* skip extensions + CR */
        if (r < len) r++;                          /* the \n */
        if (!any || sz == 0) break;                /* malformed or last chunk */
        if (r + sz > len) sz = len - r;
        memmove(buf + w, buf + r, sz); w += sz; r += sz;
        if (r < len && buf[r] == '\r') r++;
        if (r < len && buf[r] == '\n') r++;
    }
    buf[w] = 0;
    return (int)w;
}

/* ===========================================================================
 *  Keep-alive HTTP/1.1 session. http_get() opens one connection per call and
 *  reads to EOF; that costs a fresh TLS handshake every time, which is painful
 *  when a page pulls dozens of images. A session keeps a single TCP/TLS
 *  connection open and reads one length-delimited response per http_fetch(),
 *  so a batch of same-host fetches (e.g. all of a page's images) pays for just
 *  one handshake. Falls back to closing the connection when the server does.
 * ===========================================================================*/
struct http_conn {
    int      secure;
    struct tcp_conn *tc;
    struct tls_conn *sc;
    uint16_t port;
    char     host[128];
    int      dead;          /* connection no longer reusable */
};

static int conn_send(struct http_conn *c, const void *d, uint32_t n) {
    return c->secure ? tls_send(c->sc, d, n) : tcp_send(c->tc, d, n);
}
static int conn_recv(struct http_conn *c, void *b, uint32_t cap, uint32_t to) {
    return c->secure ? tls_recv(c->sc, b, cap, to) : tcp_recv(c->tc, b, cap, to);
}

static int hosts_eq(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        a++; b++;
    }
    return *a == *b;
}

struct http_conn *http_open(const char *url, int *status) {
    char host[128], path[512]; uint16_t port; int secure;
    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path), &secure) != 0) {
        if (status) *status = -1; return 0;
    }
    uint32_t ip;
    if (!dns_resolve(host, &ip, 3000)) { if (status) *status = -3; return 0; }

    struct http_conn *c = (struct http_conn *)kmalloc(sizeof(*c));
    if (!c) { if (status) *status = -1; return 0; }
    memset(c, 0, sizeof(*c));
    c->secure = secure; c->port = port;
    uint32_t i = 0; while (host[i] && i < sizeof(c->host) - 1) { c->host[i] = host[i]; i++; } c->host[i] = 0;

    if (secure) { c->sc = tls_connect(ip, port, host, 8000);
                  if (!c->sc) { kfree(c); if (status) *status = -5; return 0; } }
    else        { c->tc = tcp_connect(ip, port, 5000);
                  if (!c->tc) { kfree(c); if (status) *status = -4; return 0; } }
    if (status) *status = 0;
    return c;
}

int http_conn_can_reuse(struct http_conn *c, const char *url) {
    if (!c || c->dead) return 0;
    char host[128], path[512]; uint16_t port; int secure;
    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path), &secure) != 0) return 0;
    return secure == c->secure && port == c->port && hosts_eq(host, c->host);
}

void http_close_conn(struct http_conn *c) {
    if (!c) return;
    if (c->secure) { if (c->sc) tls_close(c->sc); }
    else           { if (c->tc) tcp_close(c->tc); }
    kfree(c);
}

/* find the 4-byte CRLFCRLF header/body split in buf[0..len); -1 if absent */
static int find_body(const char *buf, uint32_t len) {
    for (uint32_t i = 0; i + 3 < len; i++)
        if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n') return (int)(i + 4);
    return -1;
}

/* Send one keep-alive GET and read exactly one response. Returns body length,
 * or -1 on error. Marks the connection dead if it cannot be reused after. */
int http_fetch(struct http_conn *c, const char *url, char *out, uint32_t cap,
               int *status, char *location, uint32_t loc_cap) {
    if (!c || c->dead) return -1;
    char host[128], path[512]; uint16_t port; int secure;
    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path), &secure) != 0) return -1;

    char req[800]; int n = 0;
    const char *parts[] = { "GET ", path, " HTTP/1.1\r\nHost: ", host,
                            "\r\nUser-Agent: BoltOS/1.0\r\nAccept: */*\r\nConnection: keep-alive\r\n\r\n" };
    for (uint32_t i = 0; i < sizeof(parts)/sizeof(parts[0]); i++)
        for (const char *q = parts[i]; *q && n < (int)sizeof(req) - 1; ) req[n++] = *q++;
    if (conn_send(c, req, (uint32_t)n) < 0) { c->dead = 1; return -1; }

    uint32_t total = 0;
    int body_off = -1; long clen = -1; int chunked = 0, want_close = 0;
    for (;;) {
        if (total >= cap - 1) break;
        int r = conn_recv(c, out + total, cap - 1 - total, 4000);
        if (r <= 0) { c->dead = 1; if (body_off < 0) return -1; break; }
        total += (uint32_t)r;

        if (body_off < 0) {
            int bo = find_body(out, total);
            if (bo >= 0) {
                body_off = bo;
                char saved = out[bo]; out[bo] = 0;       /* terminate header block */
                if (status) *status = parse_status(out);
                if (location) find_header(out, "Location", location, loc_cap);
                char cl[24]; find_header(out, "Content-Length", cl, sizeof(cl));
                if (cl[0]) { clen = 0; for (char *p = cl; *p >= '0' && *p <= '9'; p++) clen = clen*10 + (*p - '0'); }
                char te[32]; find_header(out, "Transfer-Encoding", te, sizeof(te));
                for (char *p = te; *p; p++) { char a = *p; if (a>='A'&&a<='Z') a += 32;
                    if (a=='c' && (p==te || p[-1]==' ' || p[-1]==',')) { chunked = 1; break; } }
                char cn[24]; find_header(out, "Connection", cn, sizeof(cn));
                for (char *p = cn; *p; p++) { char a = *p; if (a>='A'&&a<='Z') a += 32;
                    if (a=='c' && (p==cn || p[-1]==' ')) { want_close = 1; break; } }
                out[bo] = saved;
            }
        }
        if (body_off >= 0) {
            if (clen >= 0 && (long)(total - (uint32_t)body_off) >= clen) break;
            if (chunked) {                               /* done at the 0-length chunk */
                if (total >= (uint32_t)body_off + 5) {
                    uint32_t s = (uint32_t)body_off;
                    for (uint32_t i = s; i + 4 < total; i++)
                        if (out[i]=='0' && out[i+1]=='\r' && out[i+2]=='\n' &&
                            out[i+3]=='\r' && out[i+4]=='\n' &&
                            (i==s || out[i-1]=='\n')) { goto done; }
                }
            }
        }
    }
done:;
    if (body_off < 0) { out[total] = 0; return 0; }
    if (want_close) c->dead = 1;
    uint32_t blen = total - (uint32_t)body_off;
    memmove(out, out + body_off, blen);
    out[blen] = 0;
    if (chunked) blen = (uint32_t)dechunk(out, blen);
    else if (clen >= 0 && (uint32_t)clen < blen) { blen = (uint32_t)clen; out[blen] = 0; }
    return (int)blen;
}

int http_get(const char *url, char *out, uint32_t cap,
             int *status, char *location, uint32_t loc_cap) {
    char host[128], path[512];
    uint16_t port; int secure;
    int pr = parse_url(url, host, sizeof(host), &port, path, sizeof(path), &secure);
    if (pr != 0)  { if (status) *status = 0;  return -1; }

    uint32_t ip;
    if (!dns_resolve(host, &ip, 3000)) { if (status) *status = -3; return -1; } /* DNS fail */

    /* one transport, two backends: plain TCP for http, TLS for https */
    struct tcp_conn *tc = 0; struct tls_conn *sc = 0;
    if (secure) { sc = tls_connect(ip, port, host, 8000);
                  if (!sc) { if (status) *status = -5; return -1; } }   /* TLS fail */
    else        { tc = tcp_connect(ip, port, 5000);
                  if (!tc) { if (status) *status = -4; return -1; } }   /* connect fail */

    /* build and send the request */
    char req[800];
    int n = 0;
    const char *parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                            "\r\nUser-Agent: BoltOS/1.0\r\nConnection: close\r\n\r\n" };
    for (uint32_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
        for (const char *q = parts[i]; *q && n < (int)sizeof(req) - 1; ) req[n++] = *q++;
    int sret = secure ? tls_send(sc, req, (uint32_t)n) : tcp_send(tc, req, (uint32_t)n);
    if (sret < 0) {
        if (secure) tls_close(sc); else tcp_close(tc);
        if (status) *status = secure ? -5 : -4;
        return -1;
    }

    /* read the whole response into out */
    uint32_t total = 0;
    for (;;) {
        if (total >= cap - 1) break;
        int r = secure ? tls_recv(sc, out + total, cap - 1 - total, 8000)
                       : tcp_recv(tc, out + total, cap - 1 - total, 8000);
        if (r <= 0) break;
        total += (uint32_t)r;
    }
    if (secure) tls_close(sc); else tcp_close(tc);
    out[total] = 0;

    if (status) *status = parse_status(out);

    /* split headers / body at the blank line */
    char *body = out;
    int chunked = 0;
    for (uint32_t i = 0; i + 3 < total; i++) {
        if (out[i] == '\r' && out[i + 1] == '\n' && out[i + 2] == '\r' && out[i + 3] == '\n') {
            out[i] = 0;                 /* terminate header block for find_header */
            body = out + i + 4;
            if (location) find_header(out, "Location", location, loc_cap);
            char te[32]; find_header(out, "Transfer-Encoding", te, sizeof(te));
            for (char *p = te; *p; p++) { char a = *p; if (a >= 'A' && a <= 'Z') a += 32;
                if (a == 'c' && (p == te || p[-1] == ' ' || p[-1] == ',')) { chunked = 1; break; } }
            break;
        }
    }
    uint32_t blen = total - (uint32_t)(body - out);
    memmove(out, body, blen);
    out[blen] = 0;
    if (chunked) blen = (uint32_t)dechunk(out, blen);
    return (int)blen;
}
