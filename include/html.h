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
};

typedef struct {
    char    *text;       /* NUL-terminated, points into doc->arena */
    uint8_t  style;      /* HSTYLE_*                                */
    int      link;       /* index into doc->hrefs, or -1            */
    uint8_t  brk;        /* 1 = start a new line before this run    */
} html_run;

typedef struct {
    html_run *runs;   int nruns;
    char    **hrefs;  int nhrefs;
    char     *title;            /* page <title>, or NULL */
    /* private storage */
    char     *arena;  uint32_t arena_len, arena_cap;
    int       runs_cap, hrefs_cap;
} html_doc;

/* Parse len bytes of HTML into a freshly allocated doc, or NULL on OOM. */
html_doc *html_parse(const char *src, uint32_t len);
/* Wrap raw text (no markup) into a doc: each newline becomes a line break. */
html_doc *html_parse_text(const char *src, uint32_t len);
void      html_free(html_doc *d);
