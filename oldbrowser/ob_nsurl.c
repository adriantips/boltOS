/* ===========================================================================
 *  ob_nsurl.c  --  NetSurf utils/nsurl.c + utils/url.c
 *
 *  The nsurl object: a parsed, normalised URL with cheap component access and
 *  an RFC 3986 §5 relative-reference resolver (nsurl_join). NetSurf threads a
 *  refcounted nsurl through every fetch, history entry and hyperlink; we keep
 *  the same contract (ref/unref) so ownership matches the upstream core.
 * ===========================================================================*/
#include "oldbrowser.h"
#include "string.h"
#include "kheap.h"

static void scopy(char *d, const char *s, uint32_t cap) {
    if (!cap) return;
    uint32_t i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static void sapp(char *d, const char *s, uint32_t cap) {
    uint32_t n = strlen(d);
    if (n >= cap) return;
    scopy(d + n, s, cap - n);
}
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* known schemes that take a "scheme:opaque" form (no //authority) */
static int opaque_scheme(const char *s, uint32_t n) {
    static const char *k[] = { "about", "data", "mailto", "javascript", "tel", 0 };
    for (int i = 0; k[i]; i++)
        if (strlen(k[i]) == n && strncmp(s, k[i], n) == 0) return 1;
    return 0;
}

/* In-place RFC 3986 §5.2.4 remove_dot_segments over an absolute path. */
static void remove_dots(char *path) {
    char out[1024]; out[0] = 0;
    const char *in = path;
    uint32_t ol = 0;
    while (*in) {
        if (strncmp(in, "../", 3) == 0)      in += 3;
        else if (strncmp(in, "./", 2) == 0)  in += 2;
        else if (strncmp(in, "/./", 3) == 0) in += 2;            /* -> "/"   */
        else if (strcmp(in, "/.") == 0)      { in += 1; out[ol++] = '/'; out[ol] = 0; break; }
        else if (strncmp(in, "/../", 4) == 0 || strcmp(in, "/..") == 0) {
            /* pop last segment from out */
            while (ol > 0 && out[ol - 1] != '/') ol--;
            if (ol > 0) ol--;            /* drop the '/' too, re-added below   */
            out[ol] = 0;
            if (strcmp(in, "/..") == 0) { out[ol++] = '/'; out[ol] = 0; break; }
            in += 3;
        } else if (strcmp(in, ".") == 0 || strcmp(in, "..") == 0) {
            break;
        } else {
            /* copy the leading '/' (if any) then up to the next '/' */
            if (*in == '/') { out[ol++] = '/'; in++; }
            while (*in && *in != '/') { if (ol < sizeof(out) - 1) out[ol++] = *in; in++; }
            out[ol] = 0;
        }
        if (ol >= sizeof(out) - 2) break;
    }
    out[ol] = 0;
    scopy(path, out, 1024);
}

/* Rebuild u->full from components and apply normalisation. */
static void nsurl_rebuild(nsurl *u) {
    for (char *p = u->scheme; *p; p++) *p = lc(*p);
    for (char *p = u->host;   *p; p++) *p = lc(*p);

    int http = (strcmp(u->scheme, "http") == 0 || strcmp(u->scheme, "https") == 0);
    if (http) {
        if (u->path[0] == 0) scopy(u->path, "/", sizeof(u->path));
        remove_dots(u->path);
        int dflt = (strcmp(u->scheme, "http") == 0) ? 80 : 443;
        if (u->port == dflt) u->port = 0;
    }

    u->full[0] = 0;
    sapp(u->full, u->scheme, sizeof(u->full));
    if (u->host[0] || http) {
        sapp(u->full, "://", sizeof(u->full));
        sapp(u->full, u->host, sizeof(u->full));
        if (u->port) {
            char pb[8]; int n = u->port, i = 0; char tmp[8];
            if (n == 0) tmp[i++] = '0';
            while (n) { tmp[i++] = (char)('0' + n % 10); n /= 10; }
            int j = 0; for (int k = i - 1; k >= 0; k--) pb[j++] = tmp[k]; pb[j] = 0;
            sapp(u->full, ":", sizeof(u->full));
            sapp(u->full, pb, sizeof(u->full));
        }
    } else {
        sapp(u->full, ":", sizeof(u->full));
    }
    sapp(u->full, u->path, sizeof(u->full));
    if (u->query[0])    { sapp(u->full, "?", sizeof(u->full)); sapp(u->full, u->query, sizeof(u->full)); }
    if (u->fragment[0]) { sapp(u->full, "#", sizeof(u->full)); sapp(u->full, u->fragment, sizeof(u->full)); }
}

nsurl *nsurl_create(const char *s) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    nsurl *u = (nsurl *)kmalloc(sizeof(nsurl));
    if (!u) return 0;
    memset(u, 0, sizeof(*u));
    u->refcount = 1;

    const char *p = s;
    const char *colon = 0;
    for (const char *q = s; *q && *q != '/' && *q != '?' && *q != '#'; q++)
        if (*q == ':') { colon = q; break; }

    int authority = 0;
    if (colon && strncmp(colon, "://", 3) == 0) {
        uint32_t n = (uint32_t)(colon - s); if (n >= sizeof(u->scheme)) n = sizeof(u->scheme) - 1;
        memcpy(u->scheme, s, n); u->scheme[n] = 0;
        p = colon + 3; authority = 1;
    } else if (colon && opaque_scheme(s, (uint32_t)(colon - s))) {
        uint32_t n = (uint32_t)(colon - s); if (n >= sizeof(u->scheme)) n = sizeof(u->scheme) - 1;
        memcpy(u->scheme, s, n); u->scheme[n] = 0;
        scopy(u->path, colon + 1, sizeof(u->path));   /* opaque remainder */
        nsurl_rebuild(u);
        return u;
    } else {
        scopy(u->scheme, "http", sizeof(u->scheme));  /* schemeless -> http */
        p = s; authority = 1;
    }

    if (authority) {
        char host[200]; uint32_t hi = 0;
        while (*p && *p != '/' && *p != '?' && *p != '#' && hi < sizeof(host) - 1)
            host[hi++] = *p++;
        host[hi] = 0;
        char *cp = strchr(host, ':');
        if (cp) { *cp = 0; u->port = atoi(cp + 1); }
        scopy(u->host, host, sizeof(u->host));
    }

    /* path */
    char *pp = u->path; uint32_t pi = 0;
    while (*p && *p != '?' && *p != '#' && pi < sizeof(u->path) - 1) pp[pi++] = *p++;
    pp[pi] = 0;
    if (*p == '?') { p++; char *qp = u->query; uint32_t qi = 0;
        while (*p && *p != '#' && qi < sizeof(u->query) - 1) qp[qi++] = *p++; qp[qi] = 0; }
    if (*p == '#') { p++; scopy(u->fragment, p, sizeof(u->fragment)); }

    nsurl_rebuild(u);
    return u;
}

/* Does rel begin with a usable scheme ("x://" or a known opaque scheme)? */
static int rel_is_absolute(const char *rel) {
    const char *colon = 0;
    for (const char *q = rel; *q && *q != '/' && *q != '?' && *q != '#'; q++)
        if (*q == ':') { colon = q; break; }
    if (!colon) return 0;
    if (strncmp(colon, "://", 3) == 0) return 1;
    return opaque_scheme(rel, (uint32_t)(colon - rel));
}

nsurl *nsurl_join(nsurl *base, const char *rel) {
    if (!rel) return base ? nsurl_ref(base) : 0;
    while (*rel == ' ' || *rel == '\t') rel++;
    if (!base) return nsurl_create(rel);
    if (rel[0] == 0) return nsurl_ref(base);
    if (rel_is_absolute(rel)) return nsurl_create(rel);

    nsurl *u = (nsurl *)kmalloc(sizeof(nsurl));
    if (!u) return 0;
    memset(u, 0, sizeof(*u)); u->refcount = 1;
    scopy(u->scheme, base->scheme, sizeof(u->scheme));
    scopy(u->host,   base->host,   sizeof(u->host));
    u->port = base->port;

    if (rel[0] == '/' && rel[1] == '/') {                 /* network-path ref */
        char tmp[2048]; scopy(tmp, base->scheme, sizeof(tmp));
        sapp(tmp, ":", sizeof(tmp)); sapp(tmp, rel, sizeof(tmp));
        kfree(u); return nsurl_create(tmp);
    }
    if (rel[0] == '#') {                                  /* fragment only    */
        scopy(u->path, base->path, sizeof(u->path));
        scopy(u->query, base->query, sizeof(u->query));
        scopy(u->fragment, rel + 1, sizeof(u->fragment));
        nsurl_rebuild(u); return u;
    }
    if (rel[0] == '?') {                                  /* query only       */
        scopy(u->path, base->path, sizeof(u->path));
        const char *h = strchr(rel, '#');
        if (h) { uint32_t n = (uint32_t)(h - (rel + 1)); if (n >= sizeof(u->query)) n = sizeof(u->query) - 1;
                 memcpy(u->query, rel + 1, n); u->query[n] = 0; scopy(u->fragment, h + 1, sizeof(u->fragment)); }
        else scopy(u->query, rel + 1, sizeof(u->query));
        nsurl_rebuild(u); return u;
    }

    /* split rel into path / query / fragment */
    char rpath[1024]; uint32_t i = 0;
    const char *r = rel;
    while (*r && *r != '?' && *r != '#' && i < sizeof(rpath) - 1) rpath[i++] = *r++;
    rpath[i] = 0;
    if (*r == '?') { r++; char *q = u->query; uint32_t qi = 0;
        while (*r && *r != '#' && qi < sizeof(u->query) - 1) q[qi++] = *r++; q[qi] = 0; }
    if (*r == '#') { r++; scopy(u->fragment, r, sizeof(u->fragment)); }

    if (rpath[0] == '/') {
        scopy(u->path, rpath, sizeof(u->path));           /* absolute path    */
    } else {
        /* merge: base dir + rel path */
        char merged[1024]; scopy(merged, base->path, sizeof(merged));
        char *slash = strrchr(merged, '/');
        if (slash) slash[1] = 0; else scopy(merged, "/", sizeof(merged));
        sapp(merged, rpath, sizeof(merged));
        scopy(u->path, merged, sizeof(u->path));
    }
    nsurl_rebuild(u);
    return u;
}

nsurl *nsurl_ref(nsurl *u) { if (u) u->refcount++; return u; }
void   nsurl_unref(nsurl *u) { if (u && --u->refcount <= 0) kfree(u); }
const char *nsurl_access(nsurl *u) { return u ? u->full : ""; }

int nsurl_get_component(nsurl *u, enum nsurl_component c, char *out, uint32_t cap) {
    if (!u || !out || !cap) return 0;
    const char *src = "";
    switch (c) {
        case NSURL_SCHEME:   src = u->scheme;   break;
        case NSURL_HOST:     src = u->host;     break;
        case NSURL_PATH:     src = u->path;     break;
        case NSURL_QUERY:    src = u->query;    break;
        case NSURL_FRAGMENT: src = u->fragment; break;
        case NSURL_COMPLETE: src = u->full;     break;
    }
    scopy(out, src, cap);
    return (int)strlen(out);
}

int nsurl_compare(nsurl *a, nsurl *b) {
    if (!a || !b) return 0;
    return strcmp(a->full, b->full) == 0;
}
