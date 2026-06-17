#include <stdint.h>
#include "http.h"
#include "net.h"
#include "string.h"
#include "kprintf.h"

/* split "http://host:port/path" -> host, port, path. Returns 0 on success. */
static int parse_url(const char *url, char *host, uint32_t hcap,
                     uint16_t *port, char *path, uint32_t pcap) {
    const char *s = url;
    if (strncmp(s, "http://", 7) == 0) s += 7;
    else if (strncmp(s, "https://", 8) == 0) return -2;     /* no TLS */

    uint32_t i = 0;
    while (*s && *s != '/' && *s != ':' && i < hcap - 1) host[i++] = *s++;
    host[i] = 0;
    if (i == 0) return -1;

    *port = 80;
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

int http_get(const char *url, char *out, uint32_t cap,
             int *status, char *location, uint32_t loc_cap) {
    char host[128], path[512];
    uint16_t port;
    int pr = parse_url(url, host, sizeof(host), &port, path, sizeof(path));
    if (pr == -2) { if (status) *status = -2; return -1; }   /* https unsupported */
    if (pr != 0)  { if (status) *status = 0;  return -1; }

    uint32_t ip;
    if (!dns_resolve(host, &ip, 3000)) { if (status) *status = -3; return -1; } /* DNS fail */

    struct tcp_conn *c = tcp_connect(ip, port, 5000);
    if (!c) { if (status) *status = -4; return -1; }         /* connect fail */

    /* build and send the request */
    char req[800];
    int n = 0;
    const char *parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                            "\r\nUser-Agent: BoltOS/1.0\r\nConnection: close\r\n\r\n" };
    for (uint32_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
        for (const char *q = parts[i]; *q && n < (int)sizeof(req) - 1; ) req[n++] = *q++;
    if (tcp_send(c, req, (uint32_t)n) < 0) { tcp_close(c); if (status) *status = -4; return -1; }

    /* read the whole response into out */
    uint32_t total = 0;
    for (;;) {
        if (total >= cap - 1) break;
        int r = tcp_recv(c, out + total, cap - 1 - total, 8000);
        if (r <= 0) break;
        total += (uint32_t)r;
    }
    tcp_close(c);
    out[total] = 0;

    if (status) *status = parse_status(out);

    /* split headers / body at the blank line */
    char *body = out;
    for (uint32_t i = 0; i + 3 < total; i++) {
        if (out[i] == '\r' && out[i + 1] == '\n' && out[i + 2] == '\r' && out[i + 3] == '\n') {
            out[i] = 0;                 /* terminate header block for find_header */
            body = out + i + 4;
            if (location) find_header(out, "Location", location, loc_cap);
            break;
        }
    }
    uint32_t blen = total - (uint32_t)(body - out);
    memmove(out, body, blen);
    out[blen] = 0;
    return (int)blen;
}
