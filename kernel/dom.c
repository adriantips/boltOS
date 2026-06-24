/* ===========================================================================
 *  BoltDOM -- HTML -> Document Object Model tree (see include/dom.h).
 *  Tokenizer + tree builder (implicit-close rules) -> node tree; a CSS-subset
 *  selector engine matches against the tree using parent links, so descendant
 *  and child combinators and real specificity become possible.
 * ===========================================================================*/
#include <stdint.h>
#include "dom.h"
#include "string.h"
#include "kheap.h"

/* ----------------------------- arena ----------------------------------- */
#define DBLK (64 * 1024)
#define DMAX (12 * 1024 * 1024)          /* runaway backstop */
typedef struct DBlk { struct DBlk *next; uint32_t used; uint8_t mem[DBLK]; } DBlk;
typedef struct { DBlk *head; uint32_t total; int oom; } DArena;

static void *da_alloc(DArena *a, uint32_t n) {
    n = (n + 7) & ~7u;
    if (n > DBLK) { a->oom = 1; return 0; }
    if (!a->head || a->head->used + n > DBLK) {
        if (a->total + DBLK > DMAX) { a->oom = 1; return 0; }
        DBlk *b = (DBlk *)kmalloc(sizeof(DBlk));
        if (!b) { a->oom = 1; return 0; }
        b->next = a->head; b->used = 0; a->head = b; a->total += DBLK;
    }
    void *p = a->head->mem + a->head->used; a->head->used += n; return p;
}
static void da_free(DArena *a) { for (DBlk *b = a->head; b; ) { DBlk *n = b->next; kfree(b); b = n; } a->head = 0; }
static char *da_strn(DArena *a, const char *s, uint32_t n) {
    char *p = (char *)da_alloc(a, n + 1); if (!p) return 0;
    for (uint32_t i = 0; i < n; i++) p[i] = s[i]; p[n] = 0; return p;
}
static char *da_str(DArena *a, const char *s) { return da_strn(a, s, (uint32_t)strlen(s)); }

/* the document carries its arena inline so dom_free can reach it */
typedef struct { dom_document doc; DArena arena; } DomImpl;

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  ieq(const char *a, const char *b) {
    while (*a && *b) { if (lc(*a) != lc(*b)) return 0; a++; b++; }
    return *a == *b;
}

/* ----------------------------- nodes ----------------------------------- */
static dom_node *new_node(DArena *a, int type) {
    dom_node *n = (dom_node *)da_alloc(a, sizeof(dom_node));
    if (!n) return 0;
    n->type = type; n->tag = 0; n->text = 0; n->attrs = 0;
    n->parent = n->first_child = n->last_child = n->prev = n->next = 0;
    return n;
}
void dom_append_child(dom_node *parent, dom_node *child) {
    if (!parent || !child) return;
    child->parent = parent; child->next = 0; child->prev = parent->last_child;
    if (parent->last_child) parent->last_child->next = child; else parent->first_child = child;
    parent->last_child = child;
}
void dom_insert_before(dom_node *parent, dom_node *child, dom_node *ref) {
    if (!ref) { dom_append_child(parent, child); return; }
    child->parent = parent; child->next = ref; child->prev = ref->prev;
    if (ref->prev) ref->prev->next = child; else parent->first_child = child;
    ref->prev = child;
}
void dom_remove(dom_node *n) {
    if (!n || !n->parent) return;
    if (n->prev) n->prev->next = n->next; else n->parent->first_child = n->next;
    if (n->next) n->next->prev = n->prev; else n->parent->last_child = n->prev;
    n->parent = n->prev = n->next = 0;
}

/* ----------------------------- attributes ------------------------------ */
static void attr_add(DArena *a, dom_node *n, const char *name, uint32_t nl,
                     const char *val, uint32_t vl) {
    dom_attr *at = (dom_attr *)da_alloc(a, sizeof(dom_attr));
    if (!at) return;
    at->name = da_strn(a, name, nl); for (char *p = at->name; p && *p; p++) *p = lc(*p);
    at->value = da_strn(a, val, vl);
    at->next = n->attrs; n->attrs = at;
}
const char *dom_attr_get(dom_node *n, const char *name) {
    if (!n) return 0;
    for (dom_attr *a = n->attrs; a; a = a->next) if (ieq(a->name, name)) return a->value;
    return 0;
}
void dom_attr_set(dom_document *d, dom_node *n, const char *name, const char *value) {
    DArena *a = &((DomImpl *)d)->arena;
    for (dom_attr *at = n->attrs; at; at = at->next)
        if (ieq(at->name, name)) { at->value = da_str(a, value); return; }
    attr_add(a, n, name, (uint32_t)strlen(name), value, (uint32_t)strlen(value));
}
int dom_has_class(dom_node *n, const char *cls) {
    const char *c = dom_attr_get(n, "class"); if (!c) return 0;
    uint32_t nl = (uint32_t)strlen(cls);
    for (const char *p = c; *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        const char *q = p; while (*q && *q != ' ' && *q != '\t') q++;
        if ((uint32_t)(q - p) == nl) { uint32_t k = 0; for (; k < nl; k++) if (lc(p[k]) != lc(cls[k])) break; if (k == nl) return 1; }
        p = q;
    }
    return 0;
}

/* parse attributes out of a tag's interior (after the tag name) into node n */
static void parse_attrs(DArena *a, dom_node *n, const char *s, const char *end) {
    while (s < end) {
        while (s < end && (*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='/')) s++;
        if (s >= end) break;
        const char *ns = s;
        while (s < end && *s!='='&&*s!=' '&&*s!='\t'&&*s!='\n'&&*s!='\r'&&*s!='/'&&*s!='>') s++;
        uint32_t nl = (uint32_t)(s - ns);
        if (!nl) { s++; continue; }
        const char *vs = ""; uint32_t vl = 0;
        while (s < end && (*s==' '||*s=='\t'||*s=='\n'||*s=='\r')) s++;
        if (s < end && *s == '=') {
            s++;
            while (s < end && (*s==' '||*s=='\t'||*s=='\n'||*s=='\r')) s++;
            if (s < end && (*s=='"' || *s=='\'')) {
                char q = *s++; vs = s; while (s < end && *s != q) s++; vl = (uint32_t)(s - vs); if (s < end) s++;
            } else { vs = s; while (s < end && *s!=' '&&*s!='\t'&&*s!='\n'&&*s!='\r'&&*s!='>') s++; vl = (uint32_t)(s - vs); }
        }
        attr_add(a, n, ns, nl, vs, vl);
    }
}

/* ----------------------------- tree builder ---------------------------- */
static int is_void(const char *t) {
    return ieq(t,"br")||ieq(t,"hr")||ieq(t,"img")||ieq(t,"meta")||ieq(t,"link")||
           ieq(t,"input")||ieq(t,"area")||ieq(t,"base")||ieq(t,"col")||ieq(t,"wbr")||
           ieq(t,"source")||ieq(t,"embed")||ieq(t,"track")||ieq(t,"param");
}
static int is_block(const char *t) {
    return ieq(t,"p")||ieq(t,"div")||ieq(t,"ul")||ieq(t,"ol")||ieq(t,"table")||
           ieq(t,"blockquote")||ieq(t,"section")||ieq(t,"article")||ieq(t,"header")||
           ieq(t,"footer")||ieq(t,"nav")||ieq(t,"pre")||ieq(t,"figure")||ieq(t,"form")||
           ieq(t,"h1")||ieq(t,"h2")||ieq(t,"h3")||ieq(t,"h4")||ieq(t,"h5")||ieq(t,"h6");
}

#define DSTACK 160
dom_document *dom_parse(const char *src, uint32_t len) {
    DomImpl *im = (DomImpl *)kmalloc(sizeof(DomImpl));
    if (!im) return 0;
    im->arena.head = 0; im->arena.total = 0; im->arena.oom = 0;
    DArena *a = &im->arena;
    dom_node *root = new_node(a, DOM_ELEMENT);
    if (!root) { kfree(im); return 0; }
    root->tag = da_str(a, "#document");
    im->doc.root = root; im->doc.arena = a;

    dom_node *stack[DSTACK]; int sp = 0; stack[sp++] = root;
    #define TOP (stack[sp-1])

    for (uint32_t i = 0; i < len && !a->oom; ) {
        char c = src[i];
        if (c == '<') {
            /* comment / doctype / CDATA */
            if (i+3 < len && src[i+1]=='!' && src[i+2]=='-' && src[i+3]=='-') {
                i += 4; while (i+2 < len && !(src[i]=='-'&&src[i+1]=='-'&&src[i+2]=='>')) i++;
                i = (i+3 <= len) ? i+3 : len; continue;
            }
            if (i+1 < len && src[i+1]=='!') { while (i < len && src[i] != '>') i++; if (i<len) i++; continue; }

            int closing = 0; uint32_t j = i+1;
            if (j < len && src[j]=='/') { closing = 1; j++; }
            const char *ns = src + j;
            while (j < len && ((lc(src[j])>='a'&&lc(src[j])<='z')||(src[j]>='0'&&src[j]<='9'))) j++;
            uint32_t nl = (uint32_t)((src + j) - ns);
            if (nl == 0) { i++; continue; }                 /* stray '<' */
            char tag[24]; uint32_t tn = nl < 23 ? nl : 23;
            for (uint32_t k = 0; k < tn; k++) tag[k] = lc(ns[k]); tag[tn] = 0;

            /* tag interior up to '>' */
            const char *as = src + j;
            while (j < len && src[j] != '>') j++;
            const char *ae = src + j;
            uint32_t next = (j < len) ? j+1 : len;

            if (closing) {
                int found = -1;
                for (int s = sp-1; s >= 1; s--) if (ieq(stack[s]->tag, tag)) { found = s; break; }
                if (found >= 0) sp = found;                  /* pop through match */
                i = next; continue;
            }

            /* raw-text elements: capture body verbatim as a text child */
            if (ieq(tag,"script") || ieq(tag,"style") || ieq(tag,"textarea") || ieq(tag,"title")) {
                dom_node *el = new_node(a, DOM_ELEMENT); if (!el) break;
                el->tag = da_strn(a, tag, tn); parse_attrs(a, el, as, ae);
                dom_append_child(TOP, el);
                uint32_t b0 = next;
                while (next < len) {
                    if (src[next]=='<' && next+1<len && src[next+1]=='/') {
                        uint32_t k = next+2, z = 0; char nm[12];
                        while (k < len && z < 11 && src[k]!='>' && src[k]!=' ') nm[z++] = lc(src[k++]);
                        nm[z] = 0; if (ieq(nm, tag)) break;
                    }
                    next++;
                }
                if (next > b0) { dom_node *tx = new_node(a, DOM_TEXT); if (tx) { tx->text = da_strn(a, src+b0, next-b0); dom_append_child(el, tx); } }
                while (next < len && src[next] != '>') next++; if (next < len) next++;
                i = next; continue;
            }

            /* implicit-close rules before opening */
            if (ieq(tag,"li"))                    { while (sp>1 && ieq(TOP->tag,"li")) sp--; }
            else if (ieq(tag,"option"))           { while (sp>1 && ieq(TOP->tag,"option")) sp--; }
            else if (ieq(tag,"tr"))               { while (sp>1 && ieq(TOP->tag,"tr")) sp--; }
            else if (ieq(tag,"td")||ieq(tag,"th")){ while (sp>1 && (ieq(TOP->tag,"td")||ieq(TOP->tag,"th"))) sp--; }
            else if (ieq(tag,"dd")||ieq(tag,"dt")){ while (sp>1 && (ieq(TOP->tag,"dd")||ieq(TOP->tag,"dt"))) sp--; }
            if (is_block(tag))                    { while (sp>1 && ieq(TOP->tag,"p")) sp--; }

            dom_node *el = new_node(a, DOM_ELEMENT); if (!el) break;
            el->tag = da_strn(a, tag, tn); parse_attrs(a, el, as, ae);
            dom_append_child(TOP, el);
            int selfclose = (ae > as && ae[-1] == '/');
            if (!is_void(tag) && !selfclose && sp < DSTACK) stack[sp++] = el;
            i = next; continue;
        }

        /* text run up to next '<' */
        uint32_t t0 = i; while (i < len && src[i] != '<') i++;
        /* skip all-whitespace runs to avoid a tree full of blank text nodes */
        int nonws = 0; for (uint32_t k = t0; k < i; k++) { char x = src[k]; if (x!=' '&&x!='\t'&&x!='\n'&&x!='\r') { nonws = 1; break; } }
        if (nonws) { dom_node *tx = new_node(a, DOM_TEXT); if (tx) { tx->text = da_strn(a, src+t0, i-t0); dom_append_child(TOP, tx); } }
    }
    if (a->oom) { da_free(a); kfree(im); return 0; }
    return &im->doc;
}

void dom_free(dom_document *d) {
    if (!d) return;
    DomImpl *im = (DomImpl *)d;
    da_free(&im->arena);
    kfree(im);
}

/* ----------------------------- selectors ------------------------------- */
typedef struct { char tag[24]; char id[48]; char cls[4][32]; int ncls; } scomp;
typedef struct { scomp parts[6]; char comb[6]; int n; } ssel;   /* comb[k]: combinator before parts[k] */

/* parse one compound (no spaces) like "div.a.b#id" */
static void parse_compound(const char *s, const char *e, scomp *c) {
    c->tag[0] = 0; c->id[0] = 0; c->ncls = 0;
    const char *p = s;
    /* leading type / universal */
    if (p < e && *p != '.' && *p != '#' && *p != ':' && *p != '[') {
        uint32_t o = 0; while (p < e && *p!='.'&&*p!='#'&&*p!=':'&&*p!='[' && o < 23) c->tag[o++] = lc(*p++);
        c->tag[o] = 0;
    }
    while (p < e) {
        char k = *p++;
        if (k == '.') { uint32_t o = 0; if (c->ncls < 4) { while (p<e && *p!='.'&&*p!='#'&&*p!=':'&&*p!='[' && o<31) c->cls[c->ncls][o++]=lc(*p++); c->cls[c->ncls][o]=0; c->ncls++; } else while (p<e&&*p!='.'&&*p!='#'&&*p!=':'&&*p!='[') p++; }
        else if (k == '#') { uint32_t o = 0; while (p<e && *p!='.'&&*p!='#'&&*p!=':'&&*p!='[' && o<47) c->id[o++]=*p++; c->id[o]=0; }
        else if (k == ':' || k == '[') { while (p<e && *p!='.'&&*p!='#') p++; }  /* skip pseudo / attr selectors */
    }
}

/* parse one complex selector (single, no commas) into ssel */
static int parse_selector(const char *s, const char *e, ssel *out) {
    out->n = 0;
    char pend_comb = ' ';
    while (s < e && out->n < 6) {
        while (s < e && (*s==' '||*s=='\t'||*s=='\n'||*s=='\r')) s++;
        if (s >= e) break;
        if (*s == '>' || *s == '+' || *s == '~') { pend_comb = *s=='>' ? '>' : ' '; s++; continue; }
        const char *cs = s;
        while (s < e && *s!=' '&&*s!='\t'&&*s!='\n'&&*s!='\r'&&*s!='>'&&*s!='+'&&*s!='~') s++;
        parse_compound(cs, s, &out->parts[out->n]);
        out->comb[out->n] = pend_comb; out->n++; pend_comb = ' ';
    }
    return out->n;
}

static int match_compound(dom_node *n, scomp *c) {
    if (!n || n->type != DOM_ELEMENT) return 0;
    if (c->tag[0] && c->tag[0] != '*' && !ieq(n->tag, c->tag)) return 0;
    if (c->id[0]) { const char *id = dom_attr_get(n, "id"); if (!id || !ieq(id, c->id)) return 0; }
    for (int i = 0; i < c->ncls; i++) if (!dom_has_class(n, c->cls[i])) return 0;
    return 1;
}
static int match_ssel(dom_node *n, ssel *s) {
    int pi = s->n - 1;
    if (!match_compound(n, &s->parts[pi])) return 0;
    dom_node *cur = n;
    for (pi = s->n - 2; pi >= 0; pi--) {
        char comb = s->comb[pi+1];
        if (comb == '>') { cur = cur->parent; if (!match_compound(cur, &s->parts[pi])) return 0; }
        else { cur = cur ? cur->parent : 0; int ok = 0; while (cur) { if (match_compound(cur, &s->parts[pi])) { ok = 1; break; } cur = cur->parent; } if (!ok) return 0; }
    }
    return 1;
}
/* match against a comma-separated selector list */
static int match_list(dom_node *n, const char *sel) {
    const char *p = sel;
    while (*p) {
        const char *q = p; while (*q && *q != ',') q++;
        ssel s; if (parse_selector(p, q, &s) > 0 && match_ssel(n, &s)) return 1;
        p = (*q == ',') ? q+1 : q;
    }
    return 0;
}

int dom_matches(dom_node *n, const char *selector) {
    return (n && selector) ? match_list(n, selector) : 0;
}
int dom_specificity(const char *selector) {
    int best = 0;
    const char *p = selector;
    while (*p) {
        const char *q = p; while (*q && *q != ',') q++;
        ssel s;
        if (parse_selector(p, q, &s) > 0) {
            int a = 0, b = 0, c = 0;
            for (int i = 0; i < s.n; i++) {
                if (s.parts[i].id[0]) a++;
                b += s.parts[i].ncls;
                if (s.parts[i].tag[0] && s.parts[i].tag[0] != '*') c++;
            }
            int spec = a*10000 + b*100 + c;
            if (spec > best) best = spec;
        }
        p = (*q == ',') ? q+1 : q;
    }
    return best;
}

/* DFS, calling match against the list; collect into out (cap), return count.
 * If out is NULL, returns the first match via *first instead. */
static int walk(dom_node *n, const char *sel, dom_node **out, int cap, int *cnt, dom_node **first) {
    for (dom_node *c = n->first_child; c; c = c->next) {
        if (c->type == DOM_ELEMENT && match_list(c, sel)) {
            if (first) { *first = c; return 1; }
            if (out && *cnt < cap) out[(*cnt)++] = c;
        }
        if (walk(c, sel, out, cap, cnt, first)) return 1;
    }
    return 0;
}
dom_node *dom_query(dom_node *root, const char *selector) {
    if (!root || !selector) return 0;
    dom_node *first = 0; walk(root, selector, 0, 0, 0, &first); return first;
}
int dom_query_all(dom_node *root, const char *selector, dom_node **out, int cap) {
    int cnt = 0; if (root && selector) walk(root, selector, out, cap, &cnt, 0); return cnt;
}
static int walk_tag(dom_node *n, const char *tag, dom_node **out, int cap, int *cnt) {
    for (dom_node *c = n->first_child; c; c = c->next) {
        if (c->type == DOM_ELEMENT && (ieq(tag,"*") || ieq(c->tag, tag)) && *cnt < cap) out[(*cnt)++] = c;
        walk_tag(c, tag, out, cap, cnt);
    }
    return *cnt;
}
int dom_by_tag(dom_node *root, const char *tag, dom_node **out, int cap) {
    int cnt = 0; if (root) walk_tag(root, tag, out, cap, &cnt); return cnt;
}
static int walk_cls(dom_node *n, const char *cls, dom_node **out, int cap, int *cnt) {
    for (dom_node *c = n->first_child; c; c = c->next) {
        if (c->type == DOM_ELEMENT && dom_has_class(c, cls) && *cnt < cap) out[(*cnt)++] = c;
        walk_cls(c, cls, out, cap, cnt);
    }
    return *cnt;
}
int dom_by_class(dom_node *root, const char *cls, dom_node **out, int cap) {
    int cnt = 0; if (root) walk_cls(root, cls, out, cap, &cnt); return cnt;
}
static dom_node *find_id(dom_node *n, const char *id) {
    for (dom_node *c = n->first_child; c; c = c->next) {
        if (c->type == DOM_ELEMENT) { const char *v = dom_attr_get(c, "id"); if (v && ieq(v, id)) return c; }
        dom_node *r = find_id(c, id); if (r) return r;
    }
    return 0;
}
dom_node *dom_get_by_id(dom_document *d, const char *id) { return d ? find_id(d->root, id) : 0; }

/* ----------------------------- mutation -------------------------------- */
dom_node *dom_create_element(dom_document *d, const char *tag) {
    DArena *a = &((DomImpl *)d)->arena;
    dom_node *n = new_node(a, DOM_ELEMENT); if (n) { n->tag = da_str(a, tag); for (char *p=n->tag; p&&*p; p++) *p=lc(*p); } return n;
}
dom_node *dom_create_text(dom_document *d, const char *text) {
    DArena *a = &((DomImpl *)d)->arena;
    dom_node *n = new_node(a, DOM_TEXT); if (n) n->text = da_str(a, text); return n;
}
void dom_set_text(dom_document *d, dom_node *n, const char *text) {
    if (!n) return;
    DArena *a = &((DomImpl *)d)->arena;
    n->first_child = n->last_child = 0;
    dom_node *tx = new_node(a, DOM_TEXT); if (tx) { tx->text = da_str(a, text); dom_append_child(n, tx); }
}

/* ----------------------------- serialization --------------------------- */
static void emit(char *out, uint32_t cap, uint32_t *o, const char *s) {
    while (*s && *o < cap-1) out[(*o)++] = *s++;
}
static void inner_text_rec(dom_node *n, char *out, uint32_t cap, uint32_t *o) {
    for (dom_node *c = n->first_child; c; c = c->next) {
        if (c->type == DOM_TEXT) emit(out, cap, o, c->text);
        else if (!ieq(c->tag,"script") && !ieq(c->tag,"style")) inner_text_rec(c, out, cap, o);
    }
}
int dom_inner_text(dom_node *n, char *out, uint32_t cap) {
    uint32_t o = 0; if (n && cap) inner_text_rec(n, out, cap, &o); if (cap) out[o] = 0; return (int)o;
}
static void inner_html_rec(dom_node *n, char *out, uint32_t cap, uint32_t *o) {
    for (dom_node *c = n->first_child; c; c = c->next) {
        if (c->type == DOM_TEXT) { emit(out, cap, o, c->text); continue; }
        emit(out, cap, o, "<"); emit(out, cap, o, c->tag);
        for (dom_attr *a = c->attrs; a; a = a->next) {
            emit(out, cap, o, " "); emit(out, cap, o, a->name);
            emit(out, cap, o, "=\""); emit(out, cap, o, a->value); emit(out, cap, o, "\"");
        }
        emit(out, cap, o, ">");
        if (!is_void(c->tag)) { inner_html_rec(c, out, cap, o); emit(out, cap, o, "</"); emit(out, cap, o, c->tag); emit(out, cap, o, ">"); }
    }
}
int dom_inner_html(dom_node *n, char *out, uint32_t cap) {
    uint32_t o = 0; if (n && cap) inner_html_rec(n, out, cap, &o); if (cap) out[o] = 0; return (int)o;
}
