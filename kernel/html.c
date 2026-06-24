#include <stdint.h>
#include "html.h"
#include "kheap.h"
#include "string.h"

/* ===========================================================================
 *  HTML -> run-list flattener. A single forward pass: text is decoded and
 *  whitespace-collapsed into styled runs; tags toggle style / link / colour /
 *  alignment / indent state and emit line breaks; <img> emits an image run;
 *  <script>/<style> bodies and comments are dropped. Not a real parser (no
 *  tree, no error recovery) -- a pragmatic reader that copes with real pages.
 * ===========================================================================*/

#define SEG_MAX   1024
#define TITLE_MAX 96
#define FSTACK_MAX 48

/* entity sentinels: characters with no single ASCII glyph that expand to a
 * short ASCII string in the parse loop (the bitmap fonts are ASCII-only). */
#define ENT_COPY  (-2)   /* (c) */
#define ENT_REG   (-3)   /* (r) */
#define ENT_TRADE (-4)   /* (tm) */

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* decode one UTF-8 sequence at s[0..n); return bytes consumed, codepoint in *cp */
static int utf8_decode(const unsigned char *s, uint32_t n, uint32_t *cp) {
    unsigned char c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int len; uint32_t v;
    if      ((c & 0xE0) == 0xC0) { len = 2; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; v = c & 0x07; }
    else { *cp = 0xFFFD; return 1; }
    if ((uint32_t)len > n) { *cp = 0xFFFD; return 1; }
    for (int k = 1; k < len; k++) { if ((s[k] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; } v = (v << 6) | (s[k] & 0x3F); }
    *cp = v; return len;
}

/* fold a Unicode codepoint to a single ASCII byte: accents -> base letter,
 * smart punctuation -> its ASCII twin. Returns -1 to drop the char entirely
 * (CJK, symbols and anything we can't sensibly render with an 8x8 ASCII font). */
static int fold_cp(uint32_t cp) {
    if (cp < 0x80) return (int)cp;
    switch (cp) {
        case 0x00A0: return ' ';                                   /* nbsp */
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
        case 0x2013: case 0x2014: case 0x2212: case 0x2010: case 0x2011: return '-';
        case 0x2022: case 0x00B7: case 0x25AA: case 0x2023: return '*';
        case 0x2026: return '.';
    }
    /* Latin-1 Supplement + Latin Extended-A: strip the diacritic */
    static const char *acc =
        /* C0..FF */ "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
    if (cp >= 0x00C0 && cp <= 0x00FF) return acc[cp - 0x00C0];
    if (cp == 0x0152) return 'O'; if (cp == 0x0153) return 'o';    /* OE / oe */
    if (cp == 0x0160 || cp == 0x015A) return 'S'; if (cp == 0x0161 || cp == 0x015B) return 's';
    if (cp == 0x017D || cp == 0x0179) return 'Z'; if (cp == 0x017E || cp == 0x017A) return 'z';
    return -1;
}

static char *arena_push(html_doc *d, const char *s, uint32_t n) {
    if (d->arena_len + n + 1 > d->arena_cap) return 0;
    char *p = d->arena + d->arena_len;
    for (uint32_t i = 0; i < n; i++) p[i] = s[i];
    p[n] = 0;
    d->arena_len += n + 1;
    return p;
}

/* full run record so callers can set the extended fields */
static void push_full(html_doc *d, const char *text, uint32_t n, uint8_t style,
                      int link, uint8_t brk, uint32_t color, uint8_t align, uint8_t indent,
                      uint32_t bg, uint8_t fscale) {
    if (d->nruns >= d->runs_cap) return;
    char *t = arena_push(d, text, n);
    if (!t) return;
    html_run *r = &d->runs[d->nruns++];
    r->text = t; r->style = style; r->link = link; r->brk = brk;
    r->kind = HRUN_TEXT; r->align = align; r->indent = indent; r->color = color;
    r->bg = bg; r->fscale = fscale; r->img = -1; r->iw = r->ih = 0; r->pix = 0; r->name = 0;
    r->elid = d->cur_id;
}

static void cpy(char *d, const char *s, uint32_t cap) {
    uint32_t i = 0; for (; s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0;
}

/* emit a form-control run (input / button / textarea / select) */
static void push_input(html_doc *d, const char *label, const char *name, int link,
                       int subtype, int w, int h,
                       uint8_t brk, uint32_t color, uint8_t align, uint8_t indent) {
    if (d->nruns >= d->runs_cap) return;
    char *t = arena_push(d, label, (uint32_t)strlen(label));
    html_run *r = &d->runs[d->nruns++];
    r->text = t ? t : ""; r->style = HSTYLE_NORMAL; r->link = link; r->brk = brk;
    r->kind = HRUN_INPUT; r->align = align; r->indent = indent; r->color = color;
    r->bg = HCOL_NONE; r->fscale = 0; r->img = subtype; r->iw = w; r->ih = h; r->pix = 0;
    r->name = (name && name[0]) ? arena_push(d, name, (uint32_t)strlen(name)) : 0;
    r->elid = d->cur_id;
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
        if (val >= 0 && j < len && src[j] == ';') {
            *i = j + 1;
            if (val == 169) return ENT_COPY;
            if (val == 174) return ENT_REG;
            if (val == 8482) return ENT_TRADE;
            return val < 128 ? val : '?';
        }
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
    else if (strcmp(name, "copy") == 0) { *i = j + 1; return ENT_COPY; }
    else if (strcmp(name, "reg")  == 0) { *i = j + 1; return ENT_REG; }
    else if (strcmp(name, "trade") == 0) { *i = j + 1; return ENT_TRADE; }
    else if (strcmp(name, "mdash") == 0 || strcmp(name, "ndash") == 0 ||
             strcmp(name, "minus") == 0) out = '-';
    else if (strcmp(name, "ldquo") == 0 || strcmp(name, "rdquo") == 0 ||
             strcmp(name, "bdquo") == 0) out = '"';
    else if (strcmp(name, "lsquo") == 0 || strcmp(name, "rsquo") == 0 ||
             strcmp(name, "sbquo") == 0) out = '\'';
    else if (strcmp(name, "hellip") == 0) out = '.';
    else if (strcmp(name, "bull") == 0 || strcmp(name, "middot") == 0) out = '*';
    else if (strcmp(name, "times") == 0) out = 'x';
    else if (strcmp(name, "deg") == 0)   out = '?';
    else if (name[0] && name[1]) {
        /* accented latin names: <base><accent>, e.g. eacute, uuml, ntilde */
        char b = name[0];
        if ((b>='a'&&b<='z') &&
            (strcmp(name+1,"acute")==0 || strcmp(name+1,"grave")==0 ||
             strcmp(name+1,"circ")==0  || strcmp(name+1,"uml")==0   ||
             strcmp(name+1,"tilde")==0 || strcmp(name+1,"ring")==0  ||
             strcmp(name+1,"cedil")==0 || strcmp(name+1,"slash")==0))
            out = b;
    }
    if (out >= 0) { *i = j + 1; return out; }
    return -1;
}

/* Decode HTML entities in-place in a URL/attribute string. Only ASCII
 * expansions are applied (e.g. &amp; -> &, &#x2F; -> /); a non-entity '&' or a
 * non-ASCII sentinel is left as the literal '&'. Without this, hrefs such as
 * DuckDuckGo's "...&amp;rut=..." stay malformed and the server 400s the click. */
static void decode_url_entities(char *s) {
    uint32_t len = (uint32_t)strlen(s), r = 0, w = 0;
    while (r < len) {
        if (s[r] == '&') {
            uint32_t i = r;
            int e = decode_entity(s, &i, len);
            if (e >= 0) { s[w++] = (char)e; r = i; continue; }
        }
        s[w++] = s[r++];
    }
    s[w] = 0;
}

/* extract a quoted/bare attribute value (case-insensitive name) into out */
static int get_attr(const char *tag, const char *attr, char *out, uint32_t cap) {
    uint32_t al = strlen(attr);
    out[0] = 0;
    for (const char *p = tag; *p; p++) {
        uint32_t k = 0;
        while (k < al && lc(p[k]) == attr[k]) k++;
        if (k == al) {
            /* must be a token boundary before the name (not mid-identifier) */
            char pc = p == tag ? ' ' : p[-1];
            if ((pc>='a'&&pc<='z')||(pc>='A'&&pc<='Z')||(pc>='0'&&pc<='9')||pc=='-') continue;
            const char *q = p + al;
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
            return 1;
        }
    }
    return 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lc(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int iabs(int x) { return x < 0 ? -x : x; }
static int clamp255(int x) { return x < 0 ? 0 : x > 255 ? 255 : x; }

/* HSL (h:deg, s/l:percent) -> 8-bit RGB, integer math only (no FPU). */
static void hsl_to_rgb(int h, int s, int l, int *R, int *G, int *B) {
    h = ((h % 360) + 360) % 360;
    if (s < 0) s = 0; if (s > 100) s = 100;
    if (l < 0) l = 0; if (l > 100) l = 100;
    int c = (255 * (100 - iabs(2*l - 100)) * s) / 10000;     /* chroma 0..255 */
    int x = c * (60 - iabs((h % 120) - 60)) / 60;
    int m = 255 * l / 100 - c / 2;
    int r, g, b;
    if      (h <  60) { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else              { r = c; g = 0; b = x; }
    *R = clamp255(r + m); *G = clamp255(g + m); *B = clamp255(b + m);
}

/* read an int; supports trailing '%' (scaled 0..100 -> 0..255 when pct!=0). p
 * advanced past the number. */
static int read_num(const char **pp, int as_pct_of_255) {
    const char *p = *pp;
    while (*p == ' ' || *p == ',' || *p == '\t') p++;
    int neg = 0; if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    int v = 0, any = 0;
    while (*p >= '0' && *p <= '9') { v = v*10 + (*p - '0'); p++; any = 1; }
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }   /* drop fraction */
    int pct = 0; if (*p == '%') { pct = 1; p++; }
    *pp = p;
    if (!any) return -1;
    if (neg) v = -v;
    if (pct && as_pct_of_255) v = v * 255 / 100;
    return v;
}

/* parse a CSS/HTML colour token -> 0x1RRGGBB (set bit) or HCOL_NONE */
static uint32_t parse_color_token(const char *v) {
    while (*v == ' ') v++;
    if (v[0] == '#') {
        const char *h = v + 1;
        int n = 0; while (hexval(h[n]) >= 0) n++;
        if (n >= 6) return 0x1000000u | (uint32_t)(hexval(h[0])*16+hexval(h[1]))<<16
                                       | (uint32_t)(hexval(h[2])*16+hexval(h[3]))<<8
                                       | (uint32_t)(hexval(h[4])*16+hexval(h[5]));
        if (n >= 3) return 0x1000000u | (uint32_t)(hexval(h[0])*17)<<16
                                       | (uint32_t)(hexval(h[1])*17)<<8
                                       | (uint32_t)(hexval(h[2])*17);
        return HCOL_NONE;
    }
    /* functional notation: rgb()/rgba()/hsl()/hsla(), space- or comma-separated */
    if ((lc(v[0])=='r'&&lc(v[1])=='g'&&lc(v[2])=='b') ||
        (lc(v[0])=='h'&&lc(v[1])=='s'&&lc(v[2])=='l')) {
        int is_hsl = lc(v[0]) == 'h';
        const char *p = v + 3;
        if (lc(*p) == 'a') p++;            /* rgba/hsla */
        while (*p == ' ') p++;
        if (*p != '(') return HCOL_NONE;
        p++;
        if (is_hsl) {
            int h = read_num(&p, 0);       /* hue: degrees */
            int s = read_num(&p, 0);       /* % */
            int l = read_num(&p, 0);       /* % */
            if (h < 0 && s < 0) return HCOL_NONE;
            int R, G, B; hsl_to_rgb(h < 0 ? 0 : h, s, l, &R, &G, &B);
            return 0x1000000u | ((uint32_t)R<<16) | ((uint32_t)G<<8) | (uint32_t)B;
        } else {
            int r = read_num(&p, 1), g = read_num(&p, 1), b = read_num(&p, 1);
            if (r < 0 || g < 0 || b < 0) return HCOL_NONE;
            return 0x1000000u | ((uint32_t)clamp255(r)<<16)
                              | ((uint32_t)clamp255(g)<<8) | (uint32_t)clamp255(b);
        }
    }
    char low[20]; uint32_t i = 0; while (v[i] && v[i] != ' ' && v[i] != ';' && i < sizeof(low)-1) { low[i] = lc(v[i]); i++; } low[i] = 0;
    if (strcmp(low,"transparent")==0 || strcmp(low,"none")==0 || strcmp(low,"inherit")==0 ||
        strcmp(low,"initial")==0 || strcmp(low,"currentcolor")==0) return HCOL_NONE;
    struct { const char *n; uint32_t c; } named[] = {
        {"black",0x000000},{"white",0xFFFFFF},{"red",0xE04040},{"green",0x008000},
        {"blue",0x5070E0},{"yellow",0xE0E040},{"orange",0xE0A040},{"purple",0x800080},
        {"gray",0x808080},{"grey",0x808080},{"silver",0xC0C0C0},{"navy",0x000080},
        {"teal",0x008080},{"maroon",0x800000},{"lime",0x00FF00},{"cyan",0x40D0E0},
        {"aqua",0x00FFFF},{"fuchsia",0xFF00FF},{"olive",0x808000},
        {"magenta",0xE040E0},{"pink",0xFFC0CB},{"gold",0xFFD700},{"brown",0xA07040},
        {"indigo",0x4B0082},{"violet",0xEE82EE},{"crimson",0xDC143C},{"coral",0xFF7F50},
        {"salmon",0xFA8072},{"khaki",0xF0E68C},{"orchid",0xDA70D6},{"plum",0xDDA0DD},
        {"tan",0xD2B48C},{"beige",0xF5F5DC},{"ivory",0xFFFFF0},{"azure",0xF0FFFF},
        {"lavender",0xE6E6FA},{"turquoise",0x40E0D0},{"tomato",0xFF6347},{"chocolate",0xD2691E},
        {"darkblue",0x00008B},{"darkgreen",0x006400},{"darkred",0x8B0000},{"darkgray",0xA9A9A9},
        {"darkgrey",0xA9A9A9},{"lightgray",0xD3D3D3},{"lightgrey",0xD3D3D3},{"lightblue",0xADD8E6},
        {"lightgreen",0x90EE90},{"steelblue",0x4682B4},{"royalblue",0x4169E1},{"skyblue",0x87CEEB},
        {"dodgerblue",0x1E90FF},{"cornflowerblue",0x6495ED},{"slategray",0x708090},{"slategrey",0x708090},
        {"dimgray",0x696969},{"dimgrey",0x696969},{"gainsboro",0xDCDCDC},{"whitesmoke",0xF5F5F5},
        {"firebrick",0xB22222},{"forestgreen",0x228B22},{"seagreen",0x2E8B57},{"goldenrod",0xDAA520},
        {"hotpink",0xFF69B4},{"deeppink",0xFF1493},{"midnightblue",0x191970},{"rebeccapurple",0x663399},
    };
    for (uint32_t k = 0; k < sizeof(named)/sizeof(named[0]); k++)
        if (strcmp(low, named[k].n) == 0) return 0x1000000u | named[k].c;
    return HCOL_NONE;
}

/* pull colour from color="" / style="color:..." in a tag body */
static uint32_t tag_color(const char *tag) {
    char val[64];
    if (get_attr(tag, "color", val, sizeof(val))) { uint32_t c = parse_color_token(val); if (c) return c; }
    if (get_attr(tag, "style", val, sizeof(val))) {
        for (char *p = val; *p; p++)
            if (lc(p[0])=='c'&&lc(p[1])=='o'&&lc(p[2])=='l'&&lc(p[3])=='o'&&lc(p[4])=='r'
                && (p == val || p[-1] == ' ' || p[-1] == ';')) {   /* not background-color */
                const char *q = p + 5; while (*q == ' ') q++;
                if (*q == ':') return parse_color_token(q + 1);
            }
    }
    return HCOL_NONE;
}

/* ===========================================================================
 *  Tiny CSS layer. <style> blocks and inline style="" are parsed into a flat
 *  property set (colour / text-align / bold / italic / hidden). Selectors are
 *  reduced to their right-most simple selector -- a tag, .class, #id or * --
 *  so `nav ul li a` matches as `a`. No cascade weighting beyond source order
 *  and a tag < class < id pass order. Enough to colour and hide real pages.
 * ===========================================================================*/
#define CSS_MAX 320
#define ALIGN_UNSET 0xFF
typedef struct {
    char     sel[24];   /* bare ident, lower-cased ('*' for universal) */
    uint8_t  kind;      /* 0 tag, 1 class, 2 id, 3 universal           */
    uint32_t color;     /* HCOL_NONE or 0x1RRGGBB                       */
    uint32_t bg;        /* background colour: HCOL_NONE or 0x1RRGGBB    */
    uint8_t  align;     /* HALIGN_* or ALIGN_UNSET                      */
    int8_t   bold;      /* -1 unset, else 0/1                           */
    int8_t   italic;    /* -1 unset, else 0/1                           */
    uint8_t  hidden;    /* display:none / visibility:hidden             */
    uint8_t  fscale;    /* font-size scale 1..3, 0 unset                */
} css_rule;

static int streqi(const char *a, const char *b) {
    while (*a && *b) { if (lc(*a) != lc(*b)) return 0; a++; b++; }
    return *a == *b;
}

/* is `name` one of the space-separated class names in `cls`? */
static int cls_has(const char *cls, const char *name) {
    uint32_t nl = strlen(name);
    const char *p = cls;
    while (*p) {
        while (*p == ' ') p++;
        const char *q = p; while (*q && *q != ' ') q++;
        if ((uint32_t)(q - p) == nl) {
            uint32_t k = 0; for (; k < nl; k++) if (lc(p[k]) != lc(name[k])) break;
            if (k == nl) return 1;
        }
        p = q;
    }
    return 0;
}

/* CSS font-size token -> text scale 1..3 (0 = leave unset). Integer only;
 * px/pt treated as px, em/rem/% relative to a 16px base, keywords mapped. */
static uint8_t css_fontsize_scale(const char *v) {
    while (*v == ' ') v++;
    if ((*v >= '0' && *v <= '9') || *v == '.') {
        int num = 0, any = 0; const char *p = v;
        while (*p >= '0' && *p <= '9') { num = num*10 + (*p - '0'); p++; any = 1; }
        if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }   /* drop fraction */
        if (!any && *v != '.') return 0;
        if (*p == '%')                              return num >= 200 ? 3 : num >= 125 ? 2 : 1;
        if ((p[0]|32) == 'e' && (p[1]|32) == 'm')   return num >= 2   ? 3 : num >= 1   ? 2 : 1;
        return num >= 30 ? 3 : num >= 19 ? 2 : 1;   /* px / pt / unitless */
    }
    char a = v[0]|32;
    if (a == 'x') return 3;                          /* x-large / xx-large */
    if (a == 'l') return 2;                          /* large / larger */
    if (a == 's' || a == 'm') return 1;              /* small / smaller / medium */
    return 0;
}

/* apply a `prop:val; prop:val` declaration list onto the field set */
static void css_decl_apply(const char *d, uint32_t n, uint32_t *color, uint8_t *align,
                           int8_t *bold, int8_t *italic, uint8_t *hidden, uint32_t *bg,
                           uint8_t *fscale) {
    uint32_t i = 0;
    while (i < n) {
        while (i < n && (d[i]==';'||d[i]==' '||d[i]=='\t'||d[i]=='\n'||d[i]=='\r')) i++;
        char prop[20]; uint32_t pl = 0;
        while (i < n && d[i] != ':' && d[i] != ';' && pl < sizeof(prop)-1) {
            char c = lc(d[i]); if (c != ' ') prop[pl++] = c; i++;
        }
        prop[pl] = 0;
        if (i >= n || d[i] != ':') { while (i < n && d[i] != ';') i++; continue; }
        i++;
        char val[48]; uint32_t vl = 0;
        while (i < n && d[i] != ';' && vl < sizeof(val)-1) val[vl++] = d[i++];
        val[vl] = 0;
        char *v = val; while (*v == ' ') v++;
        if      (strcmp(prop,"color")==0)       { uint32_t c = parse_color_token(v); if (c) *color = c; }
        else if (strcmp(prop,"background-color")==0 ||
                 strcmp(prop,"background")==0)   { uint32_t c = parse_color_token(v); if (c) *bg = c; }
        else if (strcmp(prop,"text-align")==0)  { char a = lc(v[0]); *align = a=='c'?HALIGN_CENTER : a=='r'?HALIGN_RIGHT : HALIGN_LEFT; }
        else if (strcmp(prop,"font-weight")==0) { *bold   = (lc(v[0])=='b' || (v[0]>='6'&&v[0]<='9')) ? 1 : 0; }
        else if (strcmp(prop,"font-style")==0)  { *italic = (lc(v[0])=='i' || lc(v[0])=='o') ? 1 : 0; }
        else if (strcmp(prop,"font-size")==0)   { if (fscale) { uint8_t s = css_fontsize_scale(v); if (s) *fscale = s; } }
        else if (strcmp(prop,"display")==0)     { if (lc(v[0])=='n') *hidden = 1; }
        else if (strcmp(prop,"visibility")==0)  { if (lc(v[0])=='h') *hidden = 1; }
    }
}

/* reduce one selector (s[a..b)) to its right-most simple selector */
static void sel_extract(const char *s, uint32_t a, uint32_t b, char *out, uint8_t *kind) {
    out[0] = 0; *kind = 0;
    while (a < b && (s[a]==' '||s[a]=='\t'||s[a]=='\n'||s[a]=='\r')) a++;
    while (b > a && (s[b-1]==' '||s[b-1]=='\t'||s[b-1]=='\n'||s[b-1]=='\r')) b--;
    uint32_t r = a;
    for (uint32_t k = a; k < b; k++) { char c = s[k]; if (c==' '||c=='>'||c=='+'||c=='~') r = k+1; }
    if (r < b && s[r] == '*') { out[0] = '*'; out[1] = 0; *kind = 3; return; }
    int hashp = -1, dotp = -1;
    for (uint32_t k = r; k < b; k++) { char c = s[k]; if (c==':'||c=='['||c==' ') break;
        if (c=='#' && hashp<0) hashp = (int)k; if (c=='.' && dotp<0) dotp = (int)k; }
    uint32_t is;
    if (hashp >= 0)      { *kind = 2; is = (uint32_t)hashp + 1; }
    else if (dotp >= 0)  { *kind = 1; is = (uint32_t)dotp + 1; }
    else                 { *kind = 0; is = r; }
    uint32_t ie = is, o = 0;
    while (ie < b) { char c = s[ie]; if (c==':'||c=='['||c==' '||c=='.'||c=='#') break; ie++; }
    for (uint32_t k = is; k < ie && o < 23; k++) out[o++] = lc(s[k]);
    out[o] = 0;
}

/* parse a <style> body into rules[*nr..] */
static void parse_css(css_rule *rules, int *nr, const char *src, uint32_t len) {
    uint32_t i = 0;
    while (i < len && *nr < CSS_MAX) {
        while (i < len && (src[i]==' '||src[i]=='\n'||src[i]=='\t'||src[i]=='\r')) i++;
        if (i >= len) break;
        if (src[i] == '@') {                 /* skip @media / @font-face etc. */
            int depth = 0;
            while (i < len) { char c = src[i++];
                if (c==';' && depth==0) break;
                if (c=='{') depth++;
                if (c=='}') { if (--depth <= 0) break; } }
            continue;
        }
        uint32_t s0 = i; while (i < len && src[i] != '{' && src[i] != '}') i++;
        if (i >= len || src[i] == '}') break;
        uint32_t s1 = i; i++;
        uint32_t b0 = i; while (i < len && src[i] != '}') i++;
        uint32_t b1 = i; if (i < len) i++;

        uint32_t color = HCOL_NONE, bg = HCOL_NONE; uint8_t align = ALIGN_UNSET;
        int8_t bold = -1, italic = -1; uint8_t hidden = 0, fscale = 0;
        css_decl_apply(src + b0, b1 - b0, &color, &align, &bold, &italic, &hidden, &bg, &fscale);
        if (color==HCOL_NONE && bg==HCOL_NONE && align==ALIGN_UNSET && bold<0 && italic<0 && !hidden && !fscale) continue;

        uint32_t p = s0;
        while (p < s1 && *nr < CSS_MAX) {
            uint32_t q = p; while (q < s1 && src[q] != ',') q++;
            char sel[24]; uint8_t kind;
            sel_extract(src, p, q, sel, &kind);
            if (sel[0]) {
                css_rule *r = &rules[(*nr)++];
                cpy(r->sel, sel, sizeof(r->sel));
                r->kind = kind; r->color = color; r->bg = bg; r->align = align;
                r->bold = bold; r->italic = italic; r->hidden = hidden; r->fscale = fscale;
            }
            p = q + 1;
        }
    }
}

/* overlay every rule matching (tag, class list, id) onto the field set,
 * in tag < class < id specificity order */
static void css_match(const css_rule *rules, int n, const char *tag, const char *cls,
                      const char *id, uint32_t *color, uint8_t *align,
                      int8_t *bold, int8_t *italic, uint8_t *hidden, uint32_t *bg,
                      uint8_t *fscale) {
    static const uint8_t order[4] = { 3, 0, 1, 2 };
    for (int pass = 0; pass < 4; pass++) {
        uint8_t want = order[pass];
        for (int k = 0; k < n; k++) {
            if (rules[k].kind != want) continue;
            int m = want==3 ? 1
                  : want==0 ? streqi(rules[k].sel, tag)
                  : want==1 ? (cls[0] && cls_has(cls, rules[k].sel))
                  :           (id[0]  && streqi(rules[k].sel, id));
            if (!m) continue;
            if (rules[k].color != HCOL_NONE)  *color  = rules[k].color;
            if (rules[k].bg    != HCOL_NONE)  *bg     = rules[k].bg;
            if (rules[k].align != ALIGN_UNSET) *align = rules[k].align;
            if (rules[k].bold   >= 0)         *bold   = rules[k].bold;
            if (rules[k].italic >= 0)         *italic = rules[k].italic;
            if (rules[k].hidden)              *hidden = 1;
            if (rules[k].fscale && fscale)    *fscale = rules[k].fscale;
        }
    }
}

/* explicit align="" / inline text-align, or ALIGN_UNSET when absent */
static uint8_t tag_align(const char *tag) {
    char val[48];
    if (get_attr(tag, "align", val, sizeof(val))) {
        char a = lc(val[0]);
        return a=='c'?HALIGN_CENTER : a=='r'?HALIGN_RIGHT : HALIGN_LEFT;
    }
    if (get_attr(tag, "style", val, sizeof(val))) {
        uint32_t c = HCOL_NONE, bg = HCOL_NONE; uint8_t al = ALIGN_UNSET; int8_t b=-1,it=-1; uint8_t h=0;
        css_decl_apply(val, (uint32_t)strlen(val), &c, &al, &b, &it, &h, &bg, 0);
        return al;
    }
    return ALIGN_UNSET;
}

static html_doc *doc_alloc(uint32_t len) {
    html_doc *d = (html_doc *)kmalloc(sizeof(*d));
    if (!d) return 0;
    memset(d, 0, sizeof(*d));
    d->arena_cap = len + 1024;
    d->runs_cap  = (int)(len / 3 + 64); if (d->runs_cap > 24000) d->runs_cap = 24000;
    d->hrefs_cap = (int)(len / 40 + 32); if (d->hrefs_cap > 4000) d->hrefs_cap = 4000;
    d->imgs_cap  = (int)(len / 80 + 16); if (d->imgs_cap > 1024) d->imgs_cap = 1024;
    d->scripts_cap = 16;
    d->csslinks_cap = 32;
    d->arena = (char *)kmalloc(d->arena_cap);
    d->runs  = (html_run *)kmalloc((uint32_t)d->runs_cap * sizeof(html_run));
    d->hrefs = (char **)kmalloc((uint32_t)d->hrefs_cap * sizeof(char *));
    d->imgs  = (char **)kmalloc((uint32_t)d->imgs_cap * sizeof(char *));
    d->scripts = (char **)kmalloc((uint32_t)d->scripts_cap * sizeof(char *));
    d->csslinks = (char **)kmalloc((uint32_t)d->csslinks_cap * sizeof(char *));
    if (!d->arena || !d->runs || !d->hrefs || !d->imgs || !d->scripts || !d->csslinks) { html_free(d); return 0; }
    return d;
}

/* a tag that never has a matching close and so must not push a format frame */
static int is_void_tag(const char *name) {
    return strcmp(name,"br")==0 || strcmp(name,"hr")==0 || strcmp(name,"img")==0 ||
           strcmp(name,"meta")==0 || strcmp(name,"link")==0 || strcmp(name,"input")==0 ||
           strcmp(name,"area")==0 || strcmp(name,"base")==0 || strcmp(name,"col")==0 ||
           strcmp(name,"wbr")==0 || strcmp(name,"source")==0 || strcmp(name,"embed")==0;
}

static html_doc *html_parse_impl(const char *src, uint32_t len, const char *xcss, uint32_t xcsslen) {
    html_doc *d = doc_alloc(len);
    if (!d) return 0;

    char    seg[SEG_MAX]; uint32_t seglen = 0;
    char    title[TITLE_MAX]; uint32_t titlelen = 0;
    uint8_t style = HSTYLE_NORMAL;
    int     bold = 0, italic = 0, mono = 0, heading = 0, pre = 0, link = -1;
    int     pending_brk = 0, space_pending = 0, line_started = 0;
    int     skip = 0, in_title = 0;
    int     list_depth = 0, bq_depth = 0;
    int     form_link = -1;          /* href index of the enclosing <form action>  */
    char    skipname[12] = "";       /* tag whose body is being dropped */

    /* <style> rules collected up front, applied per opening tag */
    css_rule *rules = (css_rule *)kmalloc(sizeof(css_rule) * CSS_MAX);
    int nrules = 0;
    if (rules && xcss && xcsslen) parse_css(rules, &nrules, xcss, xcsslen);

    /* format stack: open colour/align/css tags push a frame, popped on close */
    struct { char name[12]; uint32_t color; uint32_t bg; uint8_t align; int8_t bold, italic; uint8_t hidden; uint8_t fscale; } fst[FSTACK_MAX];
    int fdepth = 0;

    /* id stack: tracks the innermost element id so runs can be tagged for
     * getElementById()/innerHTML. Pushed on an id-bearing open tag, popped on
     * the matching close. */
    struct { char *id; char name[12]; } idst[32];
    int idsp = 0;

    #define CUR_COLOR  (fdepth ? fst[fdepth-1].color : HCOL_NONE)
    #define CUR_BG     (fdepth ? fst[fdepth-1].bg : HCOL_NONE)
    #define CUR_ALIGN  (fdepth ? fst[fdepth-1].align : HALIGN_LEFT)
    #define CUR_BOLD   (fdepth && fst[fdepth-1].bold   > 0)
    #define CUR_ITALIC (fdepth && fst[fdepth-1].italic > 0)
    #define CUR_HIDDEN (fdepth ? fst[fdepth-1].hidden : 0)
    #define CUR_FSCALE (fdepth ? fst[fdepth-1].fscale : 0)
    #define CUR_INDENT ((uint8_t)((list_depth + bq_depth) > 12 ? 12 : (list_depth + bq_depth)))

    #define EFF_STYLE() (pre ? HSTYLE_PRE : heading == 1 ? HSTYLE_H1 : heading == 2 ? HSTYLE_H2 \
                        : heading >= 3 ? HSTYLE_H3 : link >= 0 ? HSTYLE_LINK : mono ? HSTYLE_CODE \
                        : (italic || CUR_ITALIC) ? HSTYLE_ITALIC : (bold || CUR_BOLD) ? HSTYLE_BOLD : HSTYLE_NORMAL)

    #define FLUSH() do { if (seglen) { push_full(d, seg, seglen, style, link, (uint8_t)pending_brk, \
                                CUR_COLOR, CUR_ALIGN, CUR_INDENT, CUR_BG, CUR_FSCALE); seglen = 0; pending_brk = 0; } } while (0)

    for (uint32_t i = 0; i < len; ) {
        char c = src[i];

        if (c == '<') {
            if (i + 3 < len && src[i+1] == '!' && src[i+2] == '-' && src[i+3] == '-') {
                i += 4;
                while (i + 2 < len && !(src[i] == '-' && src[i+1] == '-' && src[i+2] == '>')) i++;
                i = (i + 3 < len) ? i + 3 : len;
                continue;
            }
            char tag[512]; uint32_t tl = 0;
            i++;
            while (i < len && src[i] != '>') { if (tl < sizeof(tag) - 1) tag[tl++] = src[i]; i++; }
            tag[tl] = 0;
            if (i < len) i++;

            const char *t = tag;
            int closing = 0;
            while (*t == ' ' || *t == '\t') t++;
            if (*t == '/') { closing = 1; t++; }
            char name[16]; uint32_t nl = 0;
            while (*t && ((lc(*t) >= 'a' && lc(*t) <= 'z') || (*t >= '0' && *t <= '9')) && nl < sizeof(name)-1)
                name[nl++] = lc(*t++);
            name[nl] = 0;

            if (skip) {
                if (closing && strcmp(name, skipname) == 0) {
                    skip = 0;
                    if (fdepth > 0 && strcmp(fst[fdepth-1].name, name) == 0) fdepth--;
                }
                continue;
            }

            /* format stack: push on opening fmt/css tags, pop on close */
            int fchanged = 0;
            if (!closing && !is_void_tag(name)) {
                char cls[96]; cls[0] = 0; get_attr(tag, "class", cls, sizeof(cls));
                char idv[64]; idv[0] = 0; get_attr(tag, "id",    idv, sizeof(idv));
                uint32_t col = HCOL_NONE, bgc = HCOL_NONE; uint8_t al = ALIGN_UNSET;
                int8_t cb = -1, ci = -1; uint8_t hid = 0, fsz = 0;
                if (rules) css_match(rules, nrules, name, cls, idv, &col, &al, &cb, &ci, &hid, &bgc, &fsz);
                /* inline style / presentational attrs override the rule set */
                char sv[160];
                if (get_attr(tag, "style", sv, sizeof(sv)))
                    css_decl_apply(sv, (uint32_t)strlen(sv), &col, &al, &cb, &ci, &hid, &bgc, &fsz);
                { uint32_t ic = tag_color(tag); if (ic != HCOL_NONE) col = ic; }
                { char bgv[40]; if (get_attr(tag, "bgcolor", bgv, sizeof(bgv))) { uint32_t bc = parse_color_token(bgv); if (bc) bgc = bc; } }
                if (strcmp(name,"body")==0 || strcmp(name,"html")==0) {     /* page-level defaults */
                    if (bgc != HCOL_NONE) d->page_bg = bgc;
                    if (col != HCOL_NONE) d->page_fg = col;
                }
                { uint8_t ia = tag_align(tag); if (ia != ALIGN_UNSET) al = ia; }
                if (strcmp(name,"center")==0 && al == ALIGN_UNSET) al = HALIGN_CENTER;
                int has = (col!=HCOL_NONE) || (bgc!=HCOL_NONE) || (al!=ALIGN_UNSET) || (cb>=0) || (ci>=0) || hid || fsz;
                int is_fmt = has || strcmp(name,"center")==0 || strcmp(name,"font")==0 || strcmp(name,"span")==0;
                if (is_fmt && fdepth < FSTACK_MAX) {
                    FLUSH();                       /* preceding text is the parent's */
                    fst[fdepth].color  = (col != HCOL_NONE)   ? col : CUR_COLOR;
                    fst[fdepth].bg     = (bgc != HCOL_NONE)   ? bgc : CUR_BG;
                    fst[fdepth].align  = (al  != ALIGN_UNSET) ? al  : CUR_ALIGN;
                    fst[fdepth].bold   = (cb >= 0) ? cb : (fdepth ? fst[fdepth-1].bold   : -1);
                    fst[fdepth].italic = (ci >= 0) ? ci : (fdepth ? fst[fdepth-1].italic : -1);
                    fst[fdepth].hidden = hid ? 1 : (uint8_t)CUR_HIDDEN;
                    fst[fdepth].fscale = fsz ? fsz : (uint8_t)CUR_FSCALE;
                    uint32_t k = 0; while (name[k] && k < 11) { fst[fdepth].name[k] = name[k]; k++; } fst[fdepth].name[k] = 0;
                    fdepth++; fchanged = 1;
                }
            } else if (closing && fdepth > 0) {
                int target = -1;
                if (strcmp(fst[fdepth-1].name, name) == 0) target = fdepth - 1;
                else for (int s = fdepth - 1; s >= 0; s--) if (strcmp(fst[s].name, name) == 0) { target = s; break; }
                if (target >= 0) { FLUSH(); fdepth = target; fchanged = 1; }  /* flush child text first */
            }
            if (fchanged) style = EFF_STYLE();

            /* id stack maintenance (independent of the format stack) */
            if (!closing && !is_void_tag(name)) {
                char idv2[64]; idv2[0] = 0;
                if (get_attr(tag, "id", idv2, sizeof(idv2)) && idv2[0] && idsp < 32) {
                    FLUSH();                               /* text so far belongs to the parent id */
                    idst[idsp].id = arena_push(d, idv2, (uint32_t)strlen(idv2));
                    cpy(idst[idsp].name, name, sizeof(idst[idsp].name));
                    idsp++;
                    d->cur_id = idst[idsp-1].id;
                }
            } else if (closing && idsp > 0 && strcmp(idst[idsp-1].name, name) == 0) {
                FLUSH();                                   /* flush this element's text first */
                idsp--;
                d->cur_id = idsp ? idst[idsp-1].id : 0;
            }

            if (strcmp(name, "style") == 0) {
                if (!closing && rules) {
                    uint32_t s0 = i;
                    while (i < len) {                  /* scan to </style> */
                        if (src[i]=='<' && i+1<len && src[i+1]=='/') {
                            uint32_t k = i+2, z = 0; char nm[8];
                            while (k < len && z < 7 && src[k] != '>' && src[k] != ' ') nm[z++] = lc(src[k++]);
                            nm[z] = 0;
                            if (strcmp(nm, "style") == 0) break;
                        }
                        i++;
                    }
                    parse_css(rules, &nrules, src + s0, i - s0);
                    while (i < len && src[i] != '>') i++; if (i < len) i++;
                }
                continue;
            }
            if (strcmp(name, "link") == 0) {
                /* collect <link rel="stylesheet" href="..."> for external CSS */
                char rel[32]; rel[0] = 0; get_attr(tag, "rel", rel, sizeof(rel));
                int is_css = 0;
                for (char *p = rel; *p; p++)            /* "stylesheet" substring, case-insensitive */
                    if (lc(p[0])=='s'&&lc(p[1])=='t'&&lc(p[2])=='y'&&lc(p[3])=='l'&&lc(p[4])=='e') { is_css = 1; break; }
                if (is_css && d->ncsslinks < d->csslinks_cap) {
                    char href[256]; href[0] = 0; get_attr(tag, "href", href, sizeof(href));
                    if (href[0]) { decode_url_entities(href);
                        char *h = arena_push(d, href, (uint32_t)strlen(href));
                        if (h) d->csslinks[d->ncsslinks++] = h; }
                }
                continue;
            }
            if (strcmp(name, "script") == 0) {
                if (!closing) {
                    uint32_t s0 = i;                       /* content begins after '>' */
                    while (i < len) {                      /* scan to </script> */
                        if (src[i]=='<' && i+1<len && src[i+1]=='/') {
                            uint32_t k=i+2, z=0; char nm[8];
                            while (k<len && z<7 && src[k]!='>' && src[k]!=' ') nm[z++]=lc(src[k++]);
                            nm[z]=0;
                            if (strcmp(nm,"script")==0) break;
                        }
                        i++;
                    }
                    if (d->scripts && d->nscripts < d->scripts_cap && i > s0) {
                        uint32_t n = i - s0;
                        char *sc = (char *)kmalloc(n + 1);
                        if (sc) { for (uint32_t z=0; z<n; z++) sc[z]=src[s0+z]; sc[n]=0; d->scripts[d->nscripts++]=sc; }
                    }
                    while (i < len && src[i] != '>') i++; if (i < len) i++;   /* eat </script> */
                }
                continue;
            }
            if (strcmp(name, "form") == 0) {
                FLUSH();
                if (closing) form_link = -1;
                else {
                    form_link = -1;
                    char act[256]; act[0] = 0;
                    if (get_attr(tag, "action", act, sizeof(act)) && act[0] && d->nhrefs < d->hrefs_cap) {
                        decode_url_entities(act);
                        char *h = arena_push(d, act, (uint32_t)strlen(act));
                        if (h) { d->hrefs[d->nhrefs] = h; form_link = d->nhrefs; d->nhrefs++; }
                    }
                }
                if (pending_brk < 1) pending_brk = 1;
                continue;
            }
            if (strcmp(name, "input") == 0) {
                FLUSH();
                if (CUR_HIDDEN) continue;
                char ty[16]; ty[0] = 0; if (get_attr(tag, "type", ty, sizeof(ty))) for (int z = 0; ty[z]; z++) ty[z] = lc(ty[z]);
                if (strcmp(ty, "hidden") == 0) continue;
                char nm[64]; nm[0] = 0; get_attr(tag, "name", nm, sizeof(nm));
                if (strcmp(ty,"checkbox")==0 || strcmp(ty,"radio")==0) {
                    push_input(d, strcmp(ty,"radio")==0 ? "( )" : "[ ]", nm, form_link, 0, 18, 18,
                               (uint8_t)pending_brk, CUR_COLOR, CUR_ALIGN, CUR_INDENT);
                    pending_brk = 0; continue;
                }
                int sub = 0; char lbl[96]; lbl[0] = 0;
                if (strcmp(ty,"submit")==0 || strcmp(ty,"button")==0 || strcmp(ty,"reset")==0) {
                    sub = 1;
                    if (!get_attr(tag, "value", lbl, sizeof(lbl)) || !lbl[0])
                        cpy(lbl, strcmp(ty,"reset")==0 ? "Reset" : "Submit", sizeof(lbl));
                } else if (!get_attr(tag, "placeholder", lbl, sizeof(lbl))) {
                    get_attr(tag, "value", lbl, sizeof(lbl));
                }
                char sz[8]; int w = get_attr(tag, "size", sz, sizeof(sz)) ? atoi(sz) * 9 : 0;
                push_input(d, lbl, nm, form_link, sub, w, sub ? 26 : 22, (uint8_t)pending_brk, CUR_COLOR, CUR_ALIGN, CUR_INDENT);
                pending_brk = 0; continue;
            }
            if (strcmp(name, "textarea") == 0) {
                if (!closing) {
                    FLUSH();
                    if (!CUR_HIDDEN) {
                        char ph[96]; ph[0] = 0; get_attr(tag, "placeholder", ph, sizeof(ph));
                        char nm[64]; nm[0] = 0; get_attr(tag, "name", nm, sizeof(nm));
                        push_input(d, ph, nm, form_link, 2, 0, 60, (uint8_t)pending_brk, CUR_COLOR, CUR_ALIGN, CUR_INDENT);
                        pending_brk = 0;
                    }
                    skip = 1; cpy(skipname, "textarea", sizeof(skipname));
                }
                continue;
            }
            if (strcmp(name, "select") == 0) {
                if (!closing) {
                    FLUSH();
                    if (!CUR_HIDDEN) {
                        char nm[64]; nm[0] = 0; get_attr(tag, "name", nm, sizeof(nm));
                        push_input(d, "", nm, form_link, 3, 160, 22, (uint8_t)pending_brk, CUR_COLOR, CUR_ALIGN, CUR_INDENT);
                        pending_brk = 0;
                    }
                    skip = 1; cpy(skipname, "select", sizeof(skipname));
                }
                continue;
            }
            if (strcmp(name, "title") == 0) { FLUSH(); in_title = !closing; if (closing && titlelen) {
                    title[titlelen] = 0; d->title = arena_push(d, title, titlelen); titlelen = 0; } continue; }

            if (strcmp(name, "b") == 0 || strcmp(name, "strong") == 0) { FLUSH(); bold = !closing; style = EFF_STYLE(); continue; }
            if (strcmp(name, "i") == 0 || strcmp(name, "em") == 0 || strcmp(name, "cite") == 0 ||
                strcmp(name, "var") == 0 || strcmp(name, "dfn") == 0) { FLUSH(); italic = !closing; style = EFF_STYLE(); continue; }
            if (strcmp(name, "code") == 0 || strcmp(name, "kbd") == 0 || strcmp(name, "samp") == 0 ||
                strcmp(name, "tt") == 0) { FLUSH(); mono = !closing; style = EFF_STYLE(); continue; }
            if (strcmp(name, "a") == 0) {
                FLUSH();
                if (!closing) {
                    char href[256]; get_attr(tag, "href", href, sizeof(href));
                    if (href[0] && d->nhrefs < d->hrefs_cap) {
                        decode_url_entities(href);
                        char *h = arena_push(d, href, (uint32_t)strlen(href));
                        if (h) { d->hrefs[d->nhrefs] = h; link = d->nhrefs; d->nhrefs++; }
                    }
                } else link = -1;
                style = EFF_STYLE();
                continue;
            }
            if (strcmp(name, "img") == 0) {
                FLUSH();
                if (CUR_HIDDEN) continue;
                char srcv[256]; get_attr(tag, "src", srcv, sizeof(srcv));
                if (srcv[0] && d->nimgs < d->imgs_cap && d->nruns < d->runs_cap) {
                    char *s = arena_push(d, srcv, (uint32_t)strlen(srcv));
                    if (s) {
                        char wv[12], hv[12];
                        int iw = get_attr(tag, "width", wv, sizeof(wv)) ? atoi(wv) : 0;
                        int ih = get_attr(tag, "height", hv, sizeof(hv)) ? atoi(hv) : 0;
                        char alt[64]; alt[0] = 0; get_attr(tag, "alt", alt, sizeof(alt));
                        const char *lbl = alt[0] ? alt : "[image]";
                        char *at = arena_push(d, lbl, (uint32_t)strlen(lbl));
                        d->imgs[d->nimgs] = s;
                        html_run *r = &d->runs[d->nruns++];
                        r->text = at ? at : s; r->style = HSTYLE_NORMAL; r->link = link;
                        r->brk = (uint8_t)pending_brk; r->kind = HRUN_IMG; r->align = CUR_ALIGN;
                        r->indent = CUR_INDENT; r->color = CUR_COLOR; r->bg = CUR_BG; r->img = d->nimgs;
                        r->iw = iw; r->ih = ih; r->pix = 0; r->name = 0; r->fscale = 0;
                        d->nimgs++; pending_brk = 0;
                    }
                }
                continue;
            }
            if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == 0) {
                FLUSH(); heading = closing ? 0 : (name[1] - '0'); style = EFF_STYLE();
                if (pending_brk < 2) pending_brk = 2; continue;
            }
            if (strcmp(name, "pre") == 0) { FLUSH(); pre = !closing; style = EFF_STYLE(); if (pending_brk < 2) pending_brk = 2; continue; }
            if (strcmp(name, "br") == 0)  { FLUSH(); if (pending_brk < 1) pending_brk = 1; continue; }
            if (strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0) {
                FLUSH(); if (closing) { if (list_depth) list_depth--; } else list_depth++;
                if (pending_brk < 1) pending_brk = 1; continue;
            }
            if (strcmp(name, "blockquote") == 0) {
                FLUSH(); if (closing) { if (bq_depth) bq_depth--; } else bq_depth++;
                if (pending_brk < 2) pending_brk = 2; continue;
            }
            if (strcmp(name, "li") == 0 && !closing) {
                FLUSH(); pending_brk = pending_brk < 1 ? 1 : pending_brk;
                push_full(d, "- ", 2, HSTYLE_NORMAL, -1, (uint8_t)pending_brk, CUR_COLOR, CUR_ALIGN, CUR_INDENT, CUR_BG, 0);
                pending_brk = 0; continue;
            }
            if (strcmp(name, "hr") == 0) { FLUSH(); pending_brk = 2;
                push_full(d, "--------------------------------", 32, HSTYLE_NORMAL, -1, 2, CUR_COLOR, HALIGN_LEFT, CUR_INDENT, CUR_BG, 0);
                pending_brk = 1; continue; }
            if (strcmp(name, "p") == 0 || strcmp(name, "div") == 0 ||
                strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 ||
                strcmp(name, "section") == 0 || strcmp(name, "article") == 0 || strcmp(name, "header") == 0 ||
                strcmp(name, "footer") == 0 || strcmp(name, "nav") == 0 ||
                strcmp(name, "center") == 0 || strcmp(name, "figure") == 0 || strcmp(name, "figcaption") == 0) {
                FLUSH(); int w = (strcmp(name, "p") == 0) ? 2 : 1;
                if (pending_brk < w) pending_brk = w;
                continue;
            }
            if ((strcmp(name, "td") == 0 || strcmp(name, "th") == 0) && !closing) {
                FLUSH(); /* a couple of spaces to separate table cells on a line */
                push_full(d, "  ", 2, HSTYLE_NORMAL, -1, 0, CUR_COLOR, CUR_ALIGN, CUR_INDENT, CUR_BG, 0);
                continue;
            }
            continue;   /* unknown / inline tag: ignored, text flows through */
        }

        if (skip) { i++; continue; }

        if (c == '&') {
            int e = decode_entity(src, &i, len);
            if (CUR_HIDDEN && !in_title) continue;
            if (e <= ENT_COPY) {                            /* multi-char ASCII expansion */
                const char *rep = (e == ENT_COPY) ? "(c)" : (e == ENT_REG) ? "(r)" : "(tm)";
                if (!in_title && !pre) {
                    if (space_pending && (seglen > 0 || (pending_brk == 0 && line_started)) && seglen < SEG_MAX - 1)
                        seg[seglen++] = ' ';
                    space_pending = 0;
                    for (const char *q = rep; *q && seglen < SEG_MAX - 1; q++) seg[seglen++] = *q;
                    line_started = 1;
                } else if (in_title) {
                    for (const char *q = rep; *q && titlelen < TITLE_MAX - 1; q++) title[titlelen++] = *q;
                } else { /* pre */
                    for (const char *q = rep; *q && seglen < SEG_MAX - 1; q++) seg[seglen++] = *q;
                }
                continue;
            }
            if (e >= 0) c = (char)e; else { i++; }
        } else if ((unsigned char)c >= 0x80) {              /* UTF-8 multibyte */
            uint32_t cp; int adv = utf8_decode((const unsigned char *)(src + i), len - i, &cp);
            i += adv > 0 ? (uint32_t)adv : 1;
            int m = fold_cp(cp);
            if (m < 0) continue;                            /* unrenderable: drop */
            c = (char)m;
        } else i++;

        if (in_title) {
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
                if (titlelen && title[titlelen-1] != ' ' && titlelen < TITLE_MAX-1) title[titlelen++] = ' ';
            } else if (titlelen < TITLE_MAX - 1) title[titlelen++] = c;
            continue;
        }

        if (CUR_HIDDEN) continue;                          /* display:none subtree */

        if (pre) {
            if (c == '\n') { FLUSH(); pending_brk = 1; }
            else if (c == '\r') { }
            else if (seglen < SEG_MAX - 1) seg[seglen++] = c;
            continue;
        }

        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            space_pending = 1;
        } else {
            if (space_pending && seglen < SEG_MAX - 1) {
                if (seglen > 0)                            seg[seglen++] = ' ';
                else if (pending_brk == 0 && line_started) seg[seglen++] = ' ';
            }
            space_pending = 0;
            if (seglen >= SEG_MAX - 1) { push_full(d, seg, seglen, style, link, (uint8_t)pending_brk, CUR_COLOR, CUR_ALIGN, CUR_INDENT, CUR_BG, 0); seglen = 0; pending_brk = 0; }
            seg[seglen++] = c;
            line_started = 1;
        }
    }
    if (seglen) push_full(d, seg, seglen, style, link, (uint8_t)pending_brk, CUR_COLOR, CUR_ALIGN, CUR_INDENT, CUR_BG, 0);
    if (rules) kfree(rules);
    return d;
}

html_doc *html_parse(const char *src, uint32_t len) {
    return html_parse_impl(src, len, 0, 0);
}
html_doc *html_parse_ext(const char *src, uint32_t len, const char *css, uint32_t csslen) {
    return html_parse_impl(src, len, css, csslen);
}

html_doc *html_parse_text(const char *src, uint32_t len) {
    html_doc *d = doc_alloc(len);
    if (!d) return 0;
    char seg[SEG_MAX]; uint32_t seglen = 0; uint8_t brk = 0;
    for (uint32_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '\r') continue;
        if (c == '\n') {
            push_full(d, seglen ? seg : " ", seglen ? seglen : 1, HSTYLE_PRE, -1, brk, HCOL_NONE, HALIGN_LEFT, 0, HCOL_NONE, 0);
            seglen = 0; brk = 1; continue;
        }
        if (seglen < SEG_MAX - 1) seg[seglen++] = c;
    }
    if (seglen) push_full(d, seg, seglen, HSTYLE_PRE, -1, brk, HCOL_NONE, HALIGN_LEFT, 0, HCOL_NONE, 0);
    return d;
}

void html_free(html_doc *d) {
    if (!d) return;
    if (d->arena) kfree(d->arena);
    if (d->runs)  kfree(d->runs);
    if (d->hrefs) kfree(d->hrefs);
    if (d->imgs)  kfree(d->imgs);
    if (d->scripts) { for (int i = 0; i < d->nscripts; i++) if (d->scripts[i]) kfree(d->scripts[i]); kfree(d->scripts); }
    if (d->csslinks) kfree(d->csslinks);   /* entries are arena ptrs, freed with arena */
    kfree(d);
}
