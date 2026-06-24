#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Minimal HTML -> layout model. The parser flattens a document into a list of
 *  styled inline "runs"; the browser app word-wraps and paints them. Block-level
 *  tags set a line break before the next run. Good enough for headings, links,
 *  paragraphs, lists and preformatted text -- "basic websites", not the DOM.
 * ===========================================================================*/

enum {
    HSTYLE_NORMAL = 0,
    HSTYLE_H1, HSTYLE_H2, HSTYLE_H3,
    HSTYLE_BOLD,
    HSTYLE_LINK,
    HSTYLE_PRE,
    HSTYLE_ITALIC,
    HSTYLE_CODE,
};

enum { HRUN_TEXT = 0, HRUN_IMG, HRUN_INPUT };
/* HRUN_INPUT subtype is carried in html_run.img: 0 text field, 1 button,
 * 2 textarea, 3 select. html_run.iw/ih give a box size hint (0 = auto). */
enum { HALIGN_LEFT = 0, HALIGN_CENTER, HALIGN_RIGHT };

#define HCOL_NONE 0u            /* run uses its style's default colour */

typedef struct {
    char    *text;       /* NUL-terminated, points into doc->arena (text runs) */
    uint8_t  style;      /* HSTYLE_*                                */
    int      link;       /* index into doc->hrefs, or -1            */
    uint8_t  brk;        /* 1 = line break before this run, 2 = paragraph gap */
    uint8_t  kind;       /* HRUN_TEXT / HRUN_IMG                    */
    uint8_t  align;      /* HALIGN_*                                */
    uint8_t  indent;     /* indent level (lists / blockquote)       */
    uint32_t color;      /* HCOL_NONE, or 0x1RRGGBB (high bit = set)*/
    uint32_t bg;         /* element background: HCOL_NONE or 0x1RRGGBB */
    uint8_t  fscale;     /* CSS font-size -> text scale 1..3, 0 = use style default */
    int      img;        /* index into doc->imgs, or -1 (image runs)*/
    int      iw, ih;     /* width/height hints from attrs, 0 = none */
    void    *pix;        /* decoded image_t*, filled by the browser */
    char    *name;       /* form-field name (HRUN_INPUT), else NULL  */
    char    *elid;       /* id of the enclosing element, or NULL (for getElementById) */
} html_run;

typedef struct {
    html_run *runs;   int nruns;
    char    **hrefs;  int nhrefs;
    char    **imgs;   int nimgs;       /* <img src> values            */
    char     *title;            /* page <title>, or NULL */
    uint32_t  page_bg;          /* <body>/<html> background: HCOL_NONE or 0x1RRGGBB */
    uint32_t  page_fg;          /* <body>/<html> text colour: HCOL_NONE or 0x1RRGGBB */
    char    **scripts; int nscripts, scripts_cap;  /* captured <script> bodies (kmalloc'd) */
    char    **csslinks; int ncsslinks, csslinks_cap; /* <link rel=stylesheet href> (arena ptrs) */
    /* private storage */
    char     *arena;  uint32_t arena_len, arena_cap;
    int       runs_cap, hrefs_cap, imgs_cap;
    char     *cur_id;           /* transient: id of element being parsed (for run->elid) */
} html_doc;

/* Parse len bytes of HTML into a freshly allocated doc, or NULL on OOM. */
html_doc *html_parse(const char *src, uint32_t len);
/* Like html_parse, but pre-seeds the CSS rule set with `css` (concatenated
 * external stylesheets fetched via <link rel=stylesheet>) so author styles
 * from .css files apply, not just inline <style> blocks. */
html_doc *html_parse_ext(const char *src, uint32_t len, const char *css, uint32_t csslen);
/* Wrap raw text (no markup) into a doc: each newline becomes a line break. */
html_doc *html_parse_text(const char *src, uint32_t len);
void      html_free(html_doc *d);
