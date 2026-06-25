#pragma once
/* ===========================================================================
 *  oldbrowser/ns_select.h -- the real libcss <-> libdom selection glue.
 *
 *  libcss never touches the DOM itself: it drives selection through a vtable of
 *  ~40 callbacks (css_select_handler) that answer questions about a node ("what
 *  is your name", "do you have class X", "who is your parent"). This module
 *  implements that vtable against a genuine libdom tree -- i.e. it is BoltOS's
 *  port of NetSurf's content/handlers/css/select.c, the canonical libdom binding
 *  for libcss. With it, real CSS cascade + selection runs over a real DOM.
 * ===========================================================================*/
#include <libcss/libcss.h>
#include <dom/dom.h>

/* One-time init: interns the handful of strings the handler needs. Idempotent;
 * returns 0 on success, -1 on failure. Call before any selection. */
int ns_css_select_init(void);

/* The libcss selection handler, bound to libdom. Pass &ns_select_handler to
 * css_select_style(). */
extern css_select_handler ns_select_handler;

/* Shared unit context (16px default font, 96 dpi). */
extern css_unit_ctx ns_css_unit_ctx;

/* Run selection for one element node against `select` (a css_select_ctx holding
 * the author + UA sheets), composing the result with `parent` (the element's
 * parent's already-computed style, or NULL for the root) so inheritance is
 * resolved. Returns a css_select_results whose styles[CSS_PSEUDO_ELEMENT_NONE]
 * is the final computed style, or NULL on error. Caller owns the results and
 * must css_select_results_destroy() it. `parent` is borrowed, not consumed. */
css_select_results *ns_css_select_style(css_select_ctx *select, dom_node *node,
		const css_computed_style *parent);
