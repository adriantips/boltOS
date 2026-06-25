/* ===========================================================================
 *  kernel/cmd_nstest.c -- `nstest`: exercise the real upstream NetSurf support
 *  libraries now compiled into BoltOS against the freestanding libc. Grows one
 *  library at a time as the port climbs the dependency stack.
 *
 *  Compiled with the NetSurf include path (see build.sh NS_SRCS) so it can pull
 *  the genuine library headers; still links the kernel console (kprintf).
 * ===========================================================================*/
#include <stdint.h>
#include <string.h>
#include "commands.h"
#include "kprintf.h"
#include "libwapcaplet/libwapcaplet.h"
#include "parserutils/errors.h"
#include "parserutils/charset/mibenum.h"
#include "parserutils/utils/buffer.h"
#include "parserutils/input/inputstream.h"
#include "hubbub/hubbub.h"
#include "hubbub/types.h"
#include "tokeniser/tokeniser.h"
#include <libcss/libcss.h>
#include <dom/dom.h>
#include "parser.h"               /* libdom hubbub binding (bindings/hubbub) */
#include "../oldbrowser/ns_select.h"

static int pass, fail;
static void ck(const char *what, int ok) {
    kprintf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) pass++; else fail++;
}

static int test_libwapcaplet(void) {
    kprintf("libwapcaplet: string interning\n");
    lwc_string *a = 0, *b = 0, *c = 0, *up = 0, *low = 0;
    lwc_error e1 = lwc_intern_string("hello", 5, &a);
    lwc_error e2 = lwc_intern_string("hello", 5, &b);
    lwc_error e3 = lwc_intern_string("world", 5, &c);
    ck("intern ok", e1 == lwc_error_ok && e2 == lwc_error_ok && e3 == lwc_error_ok && a && b && c);
    /* interning identity: equal strings collapse to the same object */
    ck("intern identity (a==b)", a == b);
    ck("distinct strings differ", a != c);

    bool eq = false;
    lwc_string_isequal(a, b, &eq);
    ck("isequal(a,b)", eq == true);
    lwc_string_isequal(a, c, &eq);
    ck("!isequal(a,c)", eq == false);

    /* caseless compare against a different-case intern */
    lwc_intern_string("HELLO", 5, &up);
    bool ceq = false;
    lwc_string_caseless_isequal(a, up, &ceq);
    ck("caseless isequal(hello,HELLO)", ceq == true);

    /* data + length accessors */
    ck("data/length", lwc_string_length(a) == 5 &&
                      lwc_string_data(a)[0] == 'h' && lwc_string_data(a)[4] == 'o');

    /* tolower derivation */
    lwc_string_tolower(up, &low);
    ck("tolower(HELLO)->hello", low && lwc_string_length(low) == 5 &&
                                lwc_string_data(low)[0] == 'h');
    /* lowercased form of an already-lower string interns to itself */
    ck("tolower identity", low == a);

    /* refcount: ref then unref must not destroy a still-referenced string */
    lwc_string *r = lwc_string_ref(a);
    ck("ref returns same", r == a);
    lwc_string_unref(a);   /* drop the extra ref; a still held by b/low */

    lwc_string_unref(b); lwc_string_unref(c); lwc_string_unref(up); lwc_string_unref(low);
    /* note: 'a' aliases b/low interns; the interner reference-counts correctly */
    return 0;
}

static int test_libparserutils(void) {
    kprintf("libparserutils: buffer\n");
    parserutils_buffer *buf = 0;
    parserutils_error e = parserutils_buffer_create(&buf);
    parserutils_buffer_append(buf, (const uint8_t *)"abc", 3);
    parserutils_buffer_append(buf, (const uint8_t *)"def", 3);
    ck("buffer append", e == PARSERUTILS_OK && buf && buf->length == 6 &&
                        memcmp(buf->data, "abcdef", 6) == 0);
    parserutils_buffer_discard(buf, 0, 3);                  /* drop "abc" */
    ck("buffer discard", buf->length == 3 && memcmp(buf->data, "def", 3) == 0);
    parserutils_buffer_destroy(buf);

    kprintf("libparserutils: charset aliases -> MIB\n");
    uint16_t utf8 = parserutils_charset_mibenum_from_name("UTF-8", 5);
    uint16_t latin1 = parserutils_charset_mibenum_from_name("latin1", 6);   /* alias */
    uint16_t iso = parserutils_charset_mibenum_from_name("ISO-8859-1", 10);
    ck("mibenum utf-8 != 0", utf8 != 0);
    ck("alias latin1 == ISO-8859-1", latin1 != 0 && latin1 == iso);
    ck("mibenum is_unicode(utf-8)", parserutils_charset_mibenum_is_unicode(utf8));
    const char *back = parserutils_charset_mibenum_to_name(utf8);
    ck("mibenum->name roundtrip", back && strcmp(back, "UTF-8") == 0);

    kprintf("libparserutils: UTF-8 input stream\n");
    parserutils_inputstream *s = 0;
    e = parserutils_inputstream_create("UTF-8", 0, 0, &s);
    /* "Hi" + U+00E9 (é = 0xC3 0xA9) */
    const uint8_t in[] = { 'H', 'i', 0xC3, 0xA9 };
    parserutils_inputstream_append(s, in, sizeof in);
    const uint8_t *p = 0; size_t len = 0;
    parserutils_error pe = parserutils_inputstream_peek(s, 0, &p, &len);
    ck("peek 'H'", pe == PARSERUTILS_OK && len == 1 && p[0] == 'H');
    parserutils_inputstream_advance(s, len);
    pe = parserutils_inputstream_peek(s, 0, &p, &len);
    ck("peek 'i'", pe == PARSERUTILS_OK && len == 1 && p[0] == 'i');
    parserutils_inputstream_advance(s, len);
    pe = parserutils_inputstream_peek(s, 0, &p, &len);   /* multibyte é */
    ck("peek U+00E9 (2 bytes)", pe == PARSERUTILS_OK && len == 2 && p[0] == 0xC3 && p[1] == 0xA9);
    parserutils_inputstream_destroy(s);
    return 0;
}

/* token-stream accumulator for the libhubbub tokeniser test */
struct tok_acc { int start, end, chars; char first_tag[16]; char text[32]; int tlen; };
static hubbub_error tok_cb(const hubbub_token *t, void *pw) {
    struct tok_acc *a = pw;
    switch (t->type) {
    case HUBBUB_TOKEN_START_TAG:
        if (a->start == 0 && t->data.tag.name.len < sizeof a->first_tag) {
            memcpy(a->first_tag, t->data.tag.name.ptr, t->data.tag.name.len);
            a->first_tag[t->data.tag.name.len] = 0;
        }
        a->start++;
        break;
    case HUBBUB_TOKEN_END_TAG: a->end++; break;
    case HUBBUB_TOKEN_CHARACTER:
        for (size_t i = 0; i < t->data.character.len && a->tlen < (int)sizeof a->text - 1; i++)
            a->text[a->tlen++] = t->data.character.ptr[i];
        a->chars++;
        break;
    default: break;
    }
    return HUBBUB_OK;
}

static int test_libhubbub(void) {
    kprintf("libhubbub: HTML5 tokeniser\n");
    const char *html = "<html><body><p class=\"x\">Hi</p></body></html>";
    parserutils_inputstream *in = 0;
    parserutils_inputstream_create("UTF-8", 0, 0, &in);
    parserutils_inputstream_append(in, (const uint8_t *)html, strlen(html));
    parserutils_inputstream_append(in, 0, 0);                 /* signal EOF */

    hubbub_tokeniser *tok = 0;
    hubbub_error he = hubbub_tokeniser_create(in, &tok);
    ck("tokeniser create", he == HUBBUB_OK && tok != 0);

    struct tok_acc acc; memset(&acc, 0, sizeof acc);
    hubbub_tokeniser_optparams params; memset(&params, 0, sizeof params);
    params.token_handler.handler = tok_cb;
    params.token_handler.pw = &acc;
    hubbub_tokeniser_setopt(tok, HUBBUB_TOKENISER_TOKEN_HANDLER, &params);

    he = hubbub_tokeniser_run(tok);
    acc.text[acc.tlen] = 0;
    ck("tokeniser run", he == HUBBUB_OK);
    ck("start tags (html,body,p)", acc.start == 3);
    ck("end tags (p,body,html)", acc.end == 3);
    ck("first start tag = html", strcmp(acc.first_tag, "html") == 0);
    ck("character data = 'Hi'", strstr(acc.text, "Hi") != 0);

    hubbub_tokeniser_destroy(tok);
    parserutils_inputstream_destroy(in);
    return 0;
}

/* ---- real DOM (libdom) + real CSS (libcss) + real selection end-to-end ---- */

/* libcss url resolver: trivial -- treat rel as already absolute. */
static css_error nst_resolve_url(void *pw, const char *base,
                                 lwc_string *rel, lwc_string **abs) {
    (void)pw; (void)base;
    *abs = lwc_string_ref(rel);
    return CSS_OK;
}

/* libdom parser message sink (no-op). */
static void nst_dom_msg(uint32_t severity, void *ctx, const char *msg, ...) {
    (void)severity; (void)ctx; (void)msg;
}

/* Depth-first search for the first element node whose name equals `tag`. */
static dom_node *nst_find_tag(dom_node *node, const char *tag) {
    dom_node *child = 0, *found = 0;
    if (!node) return 0;
    if (dom_node_get_first_child(node, &child) != DOM_NO_ERR) return 0;
    while (child) {
        dom_node *next = 0;
        dom_node_type type;
        if (dom_node_get_node_type(child, &type) == DOM_NO_ERR &&
            type == DOM_ELEMENT_NODE) {
            dom_string *nm = 0;
            if (dom_node_get_node_name(child, &nm) == DOM_NO_ERR && nm) {
                int hit = (dom_string_length(nm) == (int)strlen(tag));
                if (hit) {
                    const char *d = (const char *)dom_string_data(nm);
                    for (size_t i = 0; i < strlen(tag); i++)
                        if ((d[i] | 0x20) != (tag[i] | 0x20)) { hit = 0; break; }
                }
                dom_string_unref(nm);
                if (hit) { found = child; break; }
            }
        }
        found = nst_find_tag(child, tag);
        if (found) { dom_node_unref(child); break; }
        if (dom_node_get_next_sibling(child, &next) != DOM_NO_ERR) {
            dom_node_unref(child); break;
        }
        dom_node_unref(child);
        child = next;
    }
    return found;
}

static int test_libdom_libcss(void) {
    kprintf("libdom + libcss: HTML->DOM->CSS->selection\n");
    ck("ns_css_select_init", ns_css_select_init() == 0);

    /* --- parse real HTML into a real libdom tree via the hubbub binding --- */
    /* title (RCDATA) + style (RAWTEXT/CDATA) exercise libhubbub's close-tag
     * matching -- the path that the BoltOS strncpy fix repairs. If RCDATA/
     * RAWTEXT close tags don't match, <title>/<style> swallow the document and
     * <body> ends up empty. */
    const char *html =
        "<html><head><title>Hello</title><style>div{color:#0000ff}</style></head>"
        "<body><div><p id=\"intro\" class=\"lead\">Hi</p></div></body></html>";
    dom_hubbub_parser_params pp;
    memset(&pp, 0, sizeof pp);
    pp.enc = "UTF-8"; pp.fix_enc = true;
    pp.enable_script = false; pp.script = 0;
    pp.msg = nst_dom_msg; pp.ctx = 0; pp.daf = 0;

    dom_hubbub_parser *parser = 0;
    dom_document *doc = 0;
    dom_hubbub_error de = dom_hubbub_parser_create(&pp, &parser, &doc);
    ck("dom_hubbub_parser_create", de == DOM_HUBBUB_OK && parser && doc);
    if (parser) {
        de = dom_hubbub_parser_parse_chunk(parser,
                (const uint8_t *)html, strlen(html));
        ck("parse_chunk", de == DOM_HUBBUB_OK);
        de = dom_hubbub_parser_completed(parser);
        ck("parse completed", de == DOM_HUBBUB_OK);
        dom_hubbub_parser_destroy(parser);   /* ownership of doc -> us */
    }

    /* --- walk the real DOM: find <p> and read its name/id --- */
    dom_element *root = 0;
    dom_document_get_document_element(doc, &root);
    ck("document element (<html>) present", root != 0);
    /* <body> populated only if <title>/<style> closed correctly (RCDATA fix) */
    dom_node *body = root ? nst_find_tag((dom_node *)root, "body") : 0;
    ck("<body> present", body != 0);
    dom_node *p = nst_find_tag((dom_node *)root, "p");
    ck("found <p> below <head> RCDATA/RAWTEXT (strncpy fix)", p != 0);
    if (p) {
        dom_string *id = 0;
        dom_html_element_get_id(p, &id);
        ck("<p> id == 'intro'", id && dom_string_length(id) == 5 &&
           ((const char *)dom_string_data(id))[0] == 'i');
        if (id) dom_string_unref(id);
    }

    /* --- parse a real stylesheet with libcss --- */
    css_stylesheet_params sp;
    memset(&sp, 0, sizeof sp);
    sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    sp.level = CSS_LEVEL_DEFAULT;
    sp.charset = "UTF-8"; sp.url = "boltos://test"; sp.title = 0;
    sp.allow_quirks = false; sp.inline_style = false;
    sp.resolve = nst_resolve_url;
    css_stylesheet *sheet = 0;
    css_error ce = css_stylesheet_create(&sp, &sheet);
    ck("css_stylesheet_create", ce == CSS_OK && sheet);
    const char *css = "p { color: #ff0000; display: block; }"
                      ".lead { color: #00ff00; }";
    ce = css_stylesheet_append_data(sheet, (const uint8_t *)css, strlen(css));
    ck("append_data", ce == CSS_OK || ce == CSS_NEEDDATA);
    ce = css_stylesheet_data_done(sheet);
    ck("data_done (parsed CSS)", ce == CSS_OK);

    /* --- build a selection context and run REAL selection over the DOM --- */
    css_select_ctx *sctx = 0;
    ck("select_ctx_create", css_select_ctx_create(&sctx) == CSS_OK && sctx);
    ck("append_sheet", css_select_ctx_append_sheet(sctx, sheet,
            CSS_ORIGIN_AUTHOR, 0) == CSS_OK);

    if (p && sctx) {
        css_select_results *res = ns_css_select_style(sctx, p, 0);
        ck("css_select_style on <p>", res != 0 &&
           res->styles[CSS_PSEUDO_ELEMENT_NONE] != 0);
        if (res && res->styles[CSS_PSEUDO_ELEMENT_NONE]) {
            const css_computed_style *st = res->styles[CSS_PSEUDO_ELEMENT_NONE];
            css_color col = 0;
            uint8_t ct = css_computed_color(st, &col);
            uint8_t disp = css_computed_display(st, false);
            kprintf("    computed color=0x%08x (type %d), display=%d\n",
                    (unsigned)col, ct, disp);
            /* class .lead (spec 0,1,0) beats type p (spec 0,0,1): green wins.
             * css_color is 0xAARRGGBB, so green #00ff00 -> 0xFF00FF00. */
            ck("color resolved (green from .lead beats red from p)",
               ct == CSS_COLOR_COLOR &&
               ((col >> 16) & 0xff) == 0x00 &&   /* R */
               ((col >> 8)  & 0xff) == 0xff &&   /* G */
               ((col)       & 0xff) == 0x00);    /* B */
            ck("display == block", disp == CSS_DISPLAY_BLOCK);
        }
        if (res) css_select_results_destroy(res);
    }

    if (sctx) css_select_ctx_destroy(sctx);
    if (sheet) css_stylesheet_destroy(sheet);
    if (doc) dom_node_unref((dom_node *)doc);
    return 0;
}

int cmd_nstest(int argc, char **argv) {
    (void)argc; (void)argv;
    pass = fail = 0;
    kprintf("=== NetSurf library self-test ===\n");
    test_libwapcaplet();
    test_libparserutils();
    test_libhubbub();
    test_libdom_libcss();
    kprintf("\nNetSurf self-test: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
