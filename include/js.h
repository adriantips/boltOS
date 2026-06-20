#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltJS -- a small tree-walking JavaScript interpreter.
 *  Enough of the language to run hand-written page scripts: variables,
 *  functions/closures, objects, arrays, control flow, the usual operators,
 *  and a DOM binding so a script can read and rewrite the page. It is NOT a
 *  modern-bundle engine (no ES modules, async, or the thousands of Web APIs a
 *  site like YouTube depends on) -- it targets the scripts a small browser can
 *  realistically host.
 *
 *  The host (the browser) supplies a js_host with DOM callbacks; the engine
 *  calls back into it for document.getElementById / innerHTML / write etc.
 * ===========================================================================*/

/* Opaque DOM element handle, defined by the host. */
typedef void *js_dom_node;

typedef struct js_host {
    void *ud;                                   /* host user-data (browser_t*) */
    js_dom_node (*get_by_id)(void *ud, const char *id);
    js_dom_node (*get_by_tag)(void *ud, const char *tag, int index);
    void        (*set_inner)(void *ud, js_dom_node n, const char *html);
    int         (*get_inner)(void *ud, js_dom_node n, char *out, uint32_t cap);
    void        (*set_text)(void *ud, js_dom_node n, const char *text);
    void        (*doc_write)(void *ud, const char *html);
    void        (*set_title)(void *ud, const char *title);
    void        (*log)(void *ud, const char *msg);   /* console.log sink */
} js_host;

/* Run a script. Returns 0 on success, -1 on a parse/runtime error; on error
 * err (if non-null) gets a short message. The engine allocates from the kernel
 * heap and frees everything on return. */
int js_run(const char *src, uint32_t len, js_host *host, char *err, uint32_t errcap);
