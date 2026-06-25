#pragma once
#include <stdint.h>
/* ===========================================================================
 *  oldbrowser/ns_html.h -- HTML rendering on the REAL NetSurf core.
 *
 *  ns_html_build() parses HTML with libdom (via the real hubbub binding),
 *  cascades + selects styles with libcss (ns_select.c handler over the libdom
 *  tree), then runs a block+inline layout that consumes the genuine libcss
 *  computed styles and produces a flat display list painted with the BoltOS
 *  framebuffer primitives. This is the libdom+libcss render path wired into the
 *  browser, replacing the from-scratch kernel/dom.c + kernel/layout.c path.
 * ===========================================================================*/
typedef struct ns_page ns_page;

/* Parse + style + lay out `html` into `width` px. Writes the document <title>
 * into title_out. Returns an opaque page (paint/hit-test/free below) or NULL. */
ns_page *ns_html_build(const char *html, uint32_t len, const char *base_url,
                       int width, char *title_out, int title_cap);

int  ns_page_height(ns_page *p);
void ns_page_paint(ns_page *p, int ox, int oy, int cl, int cr,
                   int clipT, int clipB);
/* Raw (unresolved) href of the link under content-space point, or NULL. */
const char *ns_page_href_at(ns_page *p, int cx, int cy);
void ns_page_free(ns_page *p);
