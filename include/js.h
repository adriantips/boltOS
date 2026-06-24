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
    /* ---- DOM tree bindings (querySelector/createElement/mutation) ---- */
    js_dom_node (*query)(void *ud, const char *selector);          /* querySelector */
    js_dom_node (*create_el)(void *ud, const char *tag);           /* createElement */
    void        (*append_child)(void *ud, js_dom_node parent, js_dom_node child);
    void        (*set_attr)(void *ud, js_dom_node n, const char *name, const char *value);
    int         (*get_attr)(void *ud, js_dom_node n, const char *name, char *out, uint32_t cap);
    /* ---- Web Storage (localStorage) ---- */
    int         (*ls_get)(void *ud, const char *key, char *out, uint32_t cap);
    void        (*ls_set)(void *ud, const char *key, const char *val);
    void        (*ls_remove)(void *ud, const char *key);
    /* ---- document.cookie ---- */
    int         (*cookie_get)(void *ud, char *out, uint32_t cap);
    void        (*cookie_set)(void *ud, const char *kv);
    /* ---- fetch(): synchronous HTTP GET (resolves the Promise immediately).
     *      Writes the body into out (NUL-terminated), returns its length (>=0)
     *      or -1 on failure, and sets *status to the HTTP status code.       */
    int         (*fetch)(void *ud, const char *url, char *out, uint32_t cap, int *status);
} js_host;

/* Run a script. Returns 0 on success, -1 on a parse/runtime error; on error
 * err (if non-null) gets a short message. The engine allocates from the kernel
 * heap and frees everything on return. */
int js_run(const char *src, uint32_t len, js_host *host, char *err, uint32_t errcap);

/* ---- persistent VM (keeps the heap + registered handlers alive across calls)
 * so addEventListener/setTimeout/setInterval can fire after the script ends.  */
typedef struct js_vm js_vm;
js_vm *js_vm_create(js_host *host);
int    js_vm_eval(js_vm *vm, const char *src, uint32_t len, char *err, uint32_t errcap);
/* fire any timers due at now_ms; returns the number fired */
int    js_vm_pump(js_vm *vm, uint64_t now_ms);
/* dispatch an event of `type` (e.g. "click") to listeners on `target`; returns
 * the number of handlers invoked */
int    js_vm_dispatch(js_vm *vm, js_dom_node target, const char *type);
int    js_vm_has_work(js_vm *vm);     /* 1 if any active timers/listeners exist */
void   js_vm_destroy(js_vm *vm);
