#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltDOM -- a real HTML Document Object Model tree.
 *
 *  The legacy html.c flattens HTML into a linear run list; that cannot support
 *  querySelector, mutations, the event system, or a proper CSS cascade with
 *  descendant combinators, because none of those exist without a parent/child
 *  tree. BoltDOM builds the actual tree: a tokenizer + tree builder with
 *  implicit-close rules, attribute lists, and a CSS-subset selector engine
 *  (type/.class/#id/universal, descendant ' ' and child '>' combinators, selector
 *  lists). Everything is bump-allocated and freed wholesale with the document.
 * ===========================================================================*/

enum { DOM_ELEMENT = 0, DOM_TEXT };

typedef struct dom_attr {
    char *name;                 /* lowercased attribute name */
    char *value;                /* attribute value (entities not decoded) */
    struct dom_attr *next;
} dom_attr;

typedef struct dom_node {
    int   type;                 /* DOM_ELEMENT / DOM_TEXT */
    char *tag;                  /* lowercased tag name (elements), else NULL */
    char *text;                 /* text content (text nodes), else NULL */
    dom_attr *attrs;
    struct dom_node *parent, *first_child, *last_child, *prev, *next;
} dom_node;

typedef struct dom_document {
    dom_node *root;             /* synthetic document root; children incl <html> */
    void     *arena;            /* private bump arena */
} dom_document;

/* Parse len bytes of HTML into a document tree, or NULL on OOM. */
dom_document *dom_parse(const char *src, uint32_t len);
void          dom_free(dom_document *d);

/* ---- queries ----------------------------------------------------------- */
dom_node *dom_get_by_id(dom_document *d, const char *id);
/* first element (document order, within `root`'s subtree) matching `selector` */
dom_node *dom_query(dom_node *root, const char *selector);
/* up to `cap` matches into out[]; returns the count */
int       dom_query_all(dom_node *root, const char *selector, dom_node **out, int cap);
int       dom_by_tag(dom_node *root, const char *tag, dom_node **out, int cap);
int       dom_by_class(dom_node *root, const char *cls, dom_node **out, int cap);
/* Element.matches(): does node match the (comma-separated) selector list? */
int       dom_matches(dom_node *n, const char *selector);
/* CSS specificity of a selector list = max over its selectors of
 * id*10000 + (class/attr/pseudo)*100 + type. Combinators/universal add 0. */
int       dom_specificity(const char *selector);

/* ---- attributes -------------------------------------------------------- */
const char *dom_attr_get(dom_node *n, const char *name);
void        dom_attr_set(dom_document *d, dom_node *n, const char *name, const char *value);
int         dom_has_class(dom_node *n, const char *cls);

/* ---- mutation ---------------------------------------------------------- */
dom_node *dom_create_element(dom_document *d, const char *tag);
dom_node *dom_create_text(dom_document *d, const char *text);
void      dom_append_child(dom_node *parent, dom_node *child);
void      dom_insert_before(dom_node *parent, dom_node *child, dom_node *ref);
void      dom_remove(dom_node *n);
void      dom_set_text(dom_document *d, dom_node *n, const char *text);

/* ---- serialization ----------------------------------------------------- */
/* concatenated text of all descendant text nodes (textContent) */
int dom_inner_text(dom_node *n, char *out, uint32_t cap);
/* serialized child markup (innerHTML) */
int dom_inner_html(dom_node *n, char *out, uint32_t cap);
