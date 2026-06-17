#include <stdint.h>
#include "html.h"
#include "kheap.h"
#include "string.h"

/* ===========================================================================
 *  HTML -> run-list flattener. A single forward pass: text is decoded and
 *  whitespace-collapsed into styled runs; tags toggle style / link / line-break
 *  state; <script>/<style> bodies and comments are dropped. Not a real parser
 *  (no tree, no error recovery) -- a pragmatic reader for basic pages.
 * ===========================================================================*/

#define SEG_MAX   1024
#define TITLE_MAX 96

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static char *arena_push(html_doc *d, const char *s, uint32_t n) {
    if (d->arena_len + n + 1 > d->arena_cap) return 0;
    char *p = d->arena + d->arena_len;
    for (uint32_t i = 0; i < n; i++) p[i] = s[i];
    p[n] = 0;
    d->arena_len += n + 1;
    return p;
}

static void push_run(html_doc *d, const char *text, uint32_t n,
                     uint8_t style, int link, uint8_t brk) {
    if (n == 0) return;
    if (d->nruns >= d->runs_cap) return;
    char *t = arena_push(d, text, n);
    if (!t) return;
    html_run *r = &d->runs[d->nruns++];
    r->text = t; r->style = style; r->link = link; r->brk = brk;
}

/* decode an entity starting at src[*i]=='&'; return decoded char, advance *i */
static int decode_entity(const char *src, uint32_t *i, uint32_t len) {
    uint32_t j = *i + 1, start = j;
    if (j < len && src[j] == '#') {                 /* numeric */
        j++;
        int base = 10, val = 0;
        if (j < len && (src[j] == 'x' || src[j] == 'X')) { base = 16; j++; }
        while (j < len && src[j] != ';' && j - start < 8) {
            char c = src[j];
            int dv = (c >= '0' && c <= '9') ? c - '0'
                   : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                   : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (dv < 0 || dv >= base) { val = -1; break; }
            val = val * base + dv; j++;
        }
        if (val >= 0 && j < len && src[j] == ';') { *i = j + 1; return val < 128 ? val : '?'; }
        return -1;
    }
    /* named */
    char name[8]; uint32_t k = 0;
    while (j < len && src[j] != ';' && k < sizeof(name) - 1) name[k++] = lc(src[j++]);
    name[k] = 0;
    if (j >= len || src[j] != ';') return -1;
    int out = -1;
    if      (strcmp(name, "amp") == 0)  out = '&';
    else if (strcmp(name, "lt") == 0)   out = '<';
    else if (strcmp(name, "gt") == 0)   out = '>';
    else if (strcmp(name, "quot") == 0) out = '"';
    else if (strcmp(name, "apos") == 0) out = '\'';
    else if (strcmp(name, "nbsp") == 0) out = ' ';
    else if (strcmp(name, "copy") == 0) out = '?';
    else if (strcmp(name, "mdash") == 0 || strcmp(name, "ndash") == 0) out = '-';
    if (out >= 0) { *i = j + 1; return out; }
    return -1;
}

/* extract the href="..." value from a raw <a ...> tag body into out */
static void extract_href(const char *tag, char *out, uint32_t cap) {
    out[0] = 0;
    for (const char *p = tag; *p; p++) {
        if ((lc(p[0]) == 'h' && lc(p[1]) == 'r' && lc(p[2]) == 'e' && lc(p[3]) == 'f')) {
            const char *q = p + 4;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != '=') continue;
            q++;
            while (*q == ' ' || *q == '\t') q++;
            char quote = 0;
            if (*q == '"' || *q == '\'') quote = *q++;
            uint32_t o = 0;
            while (*q && o < cap - 1) {
                if (quote && *q == quote) break;
                if (!quote && (*q == ' ' || *q == '>' || *q == '\t')) break;
                out[o++] = *q++;
            }
            out[o] = 0;
            return;
        }
    }
}

static html_doc *doc_alloc(uint32_t len) {
    html_doc *d = (html_doc *)kmalloc(sizeof(*d));
    if (!d) return 0;
    memset(d, 0, sizeof(*d));
    d->arena_cap = len + 256;
    d->runs_cap  = (int)(len / 3 + 64); if (d->runs_cap > 24000) d->runs_cap = 24000;
    d->hrefs_cap = (int)(len / 40 + 32); if (d->hrefs_cap > 4000) d->hrefs_cap = 4000;
    d->arena = (char *)kmalloc(d->arena_cap);
    d->runs  = (html_run *)kmalloc((uint32_t)d->runs_cap * sizeof(html_run));
    d->hrefs = (char **)kmalloc((uint32_t)d->hrefs_cap * sizeof(char *));
    if (!d->arena || !d->runs || !d->hrefs) { html_free(d); return 0; }
    return d;
}

html_doc *html_parse(const char *src, uint32_t len) {
    html_doc *d = doc_alloc(len);
    if (!d) return 0;

    char    seg[SEG_MAX]; uint32_t seglen = 0;
    char    title[TITLE_MAX]; uint32_t titlelen = 0;
    uint8_t style = HSTYLE_NORMAL;
    int     bold = 0, heading = 0, pre = 0, link = -1;
    int     pending_brk = 0, space_pending = 0, line_started = 0;
    int     skip = 0;                /* inside <script>/<style> */
    int     in_title = 0;

    /* recompute effective style from the toggles */
    #define EFF_STYLE() (pre ? HSTYLE_PRE : heading == 1 ? HSTYLE_H1 : heading == 2 ? HSTYLE_H2 \
                        : heading >= 3 ? HSTYLE_H3 : link >= 0 ? HSTYLE_LINK \
                        : bold ? HSTYLE_BOLD : HSTYLE_NORMAL)

    for (uint32_t i = 0; i < len; ) {
        char c = src[i];

        if (c == '<') {
            /* comment? skip to --> */
            if (i + 3 < len && src[i+1] == '!' && src[i+2] == '-' && src[i+3] == '-') {
                i += 4;
                while (i + 2 < len && !(src[i] == '-' && src[i+1] == '-' && src[i+2] == '>')) i++;
                i = (i + 3 < len) ? i + 3 : len;
                continue;
            }
            /* read the tag body into tag[] */
            char tag[512]; uint32_t tl = 0;
            i++;
            while (i < len && src[i] != '>') { if (tl < sizeof(tag) - 1) tag[tl++] = src[i]; i++; }
            tag[tl] = 0;
            if (i < len) i++;        /* skip '>' */

            /* tag name (lowercased), note if it's a closing tag */
            const char *t = tag;
            int closing = 0;
            while (*t == ' ' || *t == '\t') t++;
            if (*t == '/') { closing = 1; t++; }
            char name[16]; uint32_t nl = 0;
            while (*t && ((lc(*t) >= 'a' && lc(*t) <= 'z') || (*t >= '0' && *t <= '9')) && nl < sizeof(name)-1)
                name[nl++] = lc(*t++);
            name[nl] = 0;

            /* flush current text before acting on structural tags */
            /* keep space_pending across a flush so a space straddling an inline
             * tag (e.g. "x <b>y</b>") survives as a single separating space */
            #define FLUSH() do { if (seglen) { push_run(d, seg, seglen, style, link, (uint8_t)pending_brk); \
                                               seglen = 0; pending_brk = 0; } } while (0)

            if (skip) {
                if (closing && (strcmp(name, "script") == 0 || strcmp(name, "style") == 0)) skip = 0;
                continue;
            }

            if (strcmp(name, "script") == 0 || strcmp(name, "style") == 0) { if (!closing) skip = 1; continue; }
            if (strcmp(name, "title") == 0) { FLUSH(); in_title = !closing; if (closing && titlelen) {
                    title[titlelen] = 0; d->title = arena_push(d, title, titlelen); titlelen = 0; } continue; }

            if (strcmp(name, "b") == 0 || strcmp(name, "strong") == 0) { FLUSH(); bold = !closing; style = EFF_STYLE(); continue; }
            if (strcmp(name, "a") == 0) {
                FLUSH();
                if (!closing) {
                    char href[256]; extract_href(tag, href, sizeof(href));
                    if (href[0] && d->nhrefs < d->hrefs_cap) {
                        char *h = arena_push(d, href, (uint32_t)strlen(href));
                        if (h) { d->hrefs[d->nhrefs] = h; link = d->nhrefs; d->nhrefs++; }
                    }
                } else link = -1;
                style = EFF_STYLE();
                continue;
            }
            if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == 0) {
                FLUSH(); heading = closing ? 0 : (name[1] - '0'); style = EFF_STYLE();
                if (pending_brk < 2) pending_brk = 2; continue;
            }
            if (strcmp(name, "pre") == 0) { FLUSH(); pre = !closing; style = EFF_STYLE(); if (pending_brk < 2) pending_brk = 2; continue; }
            if (strcmp(name, "br") == 0)  { FLUSH(); if (pending_brk < 1) pending_brk = 1; continue; }
            if (strcmp(name, "li") == 0 && !closing) {
                FLUSH(); pending_brk = pending_brk < 1 ? 1 : pending_brk;
                push_run(d, "- ", 2, HSTYLE_NORMAL, -1, (uint8_t)pending_brk);  /* bullet marker */
                pending_brk = 0; continue;
            }
            if (strcmp(name, "hr") == 0) { FLUSH(); pending_brk = 2;
                push_run(d, "--------------------------------", 32, HSTYLE_NORMAL, -1, 2);
                pending_brk = 1; continue; }
            /* generic block elements -> ensure a line break */
            if (strcmp(name, "p") == 0 || strcmp(name, "div") == 0 || strcmp(name, "ul") == 0 ||
                strcmp(name, "ol") == 0 || strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 ||
                strcmp(name, "section") == 0 || strcmp(name, "article") == 0 || strcmp(name, "header") == 0 ||
                strcmp(name, "footer") == 0 || strcmp(name, "nav") == 0 || strcmp(name, "blockquote") == 0 ||
                strcmp(name, "h7") == 0) {
                FLUSH(); int w = (strcmp(name, "p") == 0 || strcmp(name, "blockquote") == 0) ? 2 : 1;
                if (pending_brk < w) pending_brk = w;
                continue;
            }
            continue;   /* unknown / inline tag: ignored, text flows through */
        }

        if (skip) { i++; continue; }

        /* a text character */
        if (c == '&') {
            int e = decode_entity(src, &i, len);
            if (e >= 0) c = (char)e; else { i++; /* literal & */ }
        } else i++;

        if (in_title) {
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
                if (titlelen && title[titlelen-1] != ' ' && titlelen < TITLE_MAX-1) title[titlelen++] = ' ';
            } else if (titlelen < TITLE_MAX - 1) title[titlelen++] = c;
            continue;
        }

        if (pre) {
            if (c == '\n') { FLUSH(); pending_brk = 1; }
            else if (c == '\r') { /* drop */ }
            else if (seglen < SEG_MAX - 1) seg[seglen++] = c;
            continue;
        }

        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            space_pending = 1;
        } else {
            if (space_pending && seglen < SEG_MAX - 1) {
                if (seglen > 0)                            seg[seglen++] = ' ';   /* word gap   */
                else if (pending_brk == 0 && line_started) seg[seglen++] = ' ';   /* inline join*/
            }
            space_pending = 0;
            if (seglen >= SEG_MAX - 1) { push_run(d, seg, seglen, style, link, (uint8_t)pending_brk); seglen = 0; pending_brk = 0; }
            seg[seglen++] = c;
            line_started = 1;
        }
    }
    if (seglen) push_run(d, seg, seglen, style, link, (uint8_t)pending_brk);
    return d;
}

html_doc *html_parse_text(const char *src, uint32_t len) {
    html_doc *d = doc_alloc(len);
    if (!d) return 0;
    char seg[SEG_MAX]; uint32_t seglen = 0; uint8_t brk = 0;
    for (uint32_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '\r') continue;
        if (c == '\n') {
            push_run(d, seglen ? seg : " ", seglen ? seglen : 1, HSTYLE_PRE, -1, brk);
            seglen = 0; brk = 1; continue;
        }
        if (seglen < SEG_MAX - 1) seg[seglen++] = c;
    }
    if (seglen) push_run(d, seg, seglen, HSTYLE_PRE, -1, brk);
    return d;
}

void html_free(html_doc *d) {
    if (!d) return;
    if (d->arena) kfree(d->arena);
    if (d->runs)  kfree(d->runs);
    if (d->hrefs) kfree(d->hrefs);
    kfree(d);
}
