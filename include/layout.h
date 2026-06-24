#pragma once
#include <stdint.h>
#include "dom.h"

/* ===========================================================================
 *  BoltLayout -- a CSS cascade + box-model layout engine over the BoltDOM tree.
 *
 *  The legacy renderer (app_browser layout()) is a single-column word wrapper:
 *  no boxes, no margins/padding/borders, no widths, no real cascade. This engine
 *  builds, from a DOM tree + a stylesheet:
 *    1. a CSSOM rule set (selector + declarations + specificity),
 *    2. a computed style per element (cascade + inheritance + inline style),
 *    3. a box/layout tree, and runs block + inline layout (reflow) into a
 *       viewport width, producing real x/y/w/h geometry.
 *  Integer-only (no FPU); text is measured on the 8px bitmap grid (8*scale).
 * ===========================================================================*/

enum { DISP_INLINE = 0, DISP_BLOCK, DISP_INLINE_BLOCK, DISP_LIST_ITEM, DISP_FLEX, DISP_GRID, DISP_NONE };
#define GRID_MAX_COLS 12
enum { TA_LEFT = 0, TA_CENTER, TA_RIGHT };
enum { S_TOP = 0, S_RIGHT, S_BOTTOM, S_LEFT };   /* edge order: TRBL */
enum { FLEX_ROW = 0, FLEX_COLUMN };
enum { JUST_START = 0, JUST_CENTER, JUST_END, JUST_BETWEEN, JUST_AROUND };
enum { ALIGN_STRETCH = 0, ALIGN_START, ALIGN_CENTER, ALIGN_END };

typedef struct {
    uint32_t color;          /* 0x1RRGGBB or 0 (none -> inherit/default) */
    uint32_t bg;             /* 0x1RRGGBB or 0 (transparent) */
    uint8_t  display;        /* DISP_* */
    uint8_t  talign;         /* TA_* */
    uint8_t  fscale;         /* 1..3 */
    uint8_t  bold, italic;
    int16_t  margin[4], padding[4], border[4];   /* px, TRBL */
    int32_t  width, height;  /* px, -1 = auto */
    uint8_t  flex_dir;       /* FLEX_ROW / FLEX_COLUMN (display:flex containers) */
    uint8_t  justify;        /* JUST_*  (justify-content, main axis)  */
    uint8_t  align;          /* ALIGN_* (align-items, cross axis)     */
    int16_t  gap;            /* flex/grid gap, px */
    int16_t  flex_grow;      /* flex-grow factor (flex item)          */
    int16_t  gcols[GRID_MAX_COLS]; /* grid-template-columns tracks: >=0 px, <0 = (-w) fr */
    uint8_t  ngcols;         /* number of grid column tracks          */
} computed_style;

typedef struct layout_box {
    dom_node       *node;        /* source element (NULL for anonymous/text) */
    computed_style  st;
    int             x, y, w, h;  /* border-box geometry in viewport px */
    int             cx, cy, cw, ch; /* content-box geometry */
    int             is_text;     /* 1 = text box */
    const char     *text;        /* text content (is_text) */
    void           *pix;         /* decoded image_t* (img boxes), filled by host */
    int             link;        /* host link id (-1 none), filled by host */
    struct layout_box *first_child, *last_child, *next, *parent;
} layout_box;

typedef struct {
    layout_box *root;
    void       *arena;
    int         doc_w, doc_h;    /* total laid-out content size */
} layout_tree;

/* Build computed styles + box tree and run layout into viewport_w pixels.
 * `css` is the concatenated author stylesheet(s) (may be NULL). */
layout_tree *layout_build(dom_document *doc, const char *css, uint32_t csslen, int viewport_w);
void         layout_free(layout_tree *t);

/* Parse a CSS colour token (#hex, rgb()/rgba(), hsl()/hsla(), named,
 * transparent) -> 0x1RRGGBB or 0. Exposed for reuse/tests. */
uint32_t css_color(const char *v);

/* Override text measurement (default: 8px*scale monospace grid). The GUI sets
 * this to its proportional font metric so layout wrapping matches painting.
 * fn(text, len, scale) -> pixel width. */
void layout_set_text_measurer(int (*fn)(const char *, int, int));
int  layout_line_height(int scale);   /* line box height for a scale (1..3) */
