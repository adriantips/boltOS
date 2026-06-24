#pragma once
#include <stdint.h>
/* ===========================================================================
 *  Minimal TrueType font rasterizer. Parses an embedded .ttf (glyf/loca/cmap/
 *  head/hmtx), and rasterizes glyph outlines (quadratic beziers) to an 8-bit
 *  coverage bitmap with anti-aliasing via vertical supersampling + horizontal
 *  fractional coverage. Integer/fixed-point only (no FPU reliance). Enough to
 *  render crisp scalable Latin text — a real step up from the 8x16 bitmap face.
 * ===========================================================================*/

int  ttf_init(void);                 /* parse the embedded font; 0 ok, -1 fail */
int  ttf_ready(void);

/* Render `cp` at `px` pixels tall into an internal 8-bit coverage buffer.
 * Returns the bitmap via *bmp/*w/*h (top-left origin), the pen advance in
 * pixels, and the left/top bearings. Returns 0 on success (-1 if no glyph). */
int  ttf_glyph(uint32_t cp, int px, const uint8_t **bmp, int *w, int *h,
               int *advance, int *bearing_x, int *bearing_y, int *ascent_px);

/* Pixel-plot callback: the rasterizer hands back (x,y,coverage 0..255). */
typedef void (*ttf_plot_fn)(int x, int y, uint8_t cov, void *ctx);

/* Draw a UTF-8/ASCII string at baseline (x,y_baseline), `px` tall, calling
 * plot for each covered pixel. Returns the total advance width in pixels. */
int  ttf_draw(const char *s, int x, int y_baseline, int px, ttf_plot_fn plot, void *ctx);
int  ttf_text_width(const char *s, int px);
int  ttf_line_height(int px);
