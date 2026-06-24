#include <stdint.h>
#include "console.h"
#include "kprintf.h"
#include "serial.h"
#include "framebuffer.h"
#include "pit.h"
#include "io.h"

extern const unsigned char font8x8_basic[128][8];

#define SCALE   2
#define CELL    (8 * SCALE)        /* 16x16 px per character cell */
#define MARGIN  16
#define MAXCOLS 200
#define MAXROWS 80

static uint32_t W, H, bottom;      /* screen size; text clips above the taskbar */
static uint32_t cols, rows;        /* text grid size, in cells */
static uint32_t ccol, crow;        /* cursor position, in cells */
static uint32_t fg = 0xF0F0F5;     /* BoltUI --text-primary */
static int on_fb = 0;

/* When the GUI takes over the screen it detaches the text console (so stray
 * kprintf / console_clear never scribble on the compositor's framebuffer) and
 * installs a sink so command output can be captured into a terminal window. */
static void (*sink)(char) = 0;
void console_set_sink(void (*fn)(char)) { sink = fn; }
void (*console_get_sink(void))(char) { return sink; }
void console_detach(void) { on_fb = 0; }

static char grid[MAXROWS][MAXCOLS];

/* cursor blink */
static int      cur_on = 0;
static uint64_t cur_last = 0;

static uint32_t lerp_color(uint32_t a, uint32_t b, int n, int d) {
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ar + (br - ar) * n / d, g = ag + (bg - ag) * n / d, c = ab + (bb - ab) * n / d;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)c;
}

static void draw_bolt(uint32_t x, uint32_t y, uint32_t col) {
    for (int i = 0; i < 22; i++) fb_rect(x + 8 - i / 3, y + i,       5, 1, col);
    fb_rect(x - 6, y + 20, 14, 4, col);
    for (int i = 0; i < 22; i++) fb_rect(x + 2 - i / 3, y + 24 + i,  5, 1, col);
}

/* Background tint for a cell, sampled at its vertical center (one color per
 * cell keeps scroll-repaint cheap; the gradient is subtle enough not to band). */
static uint32_t bg_at(uint32_t py) {
    return lerp_color(0x0F0C29, 0x24243E, (int)(py + CELL / 2), (int)H);
}

/* Repaint one cell: restore its background, then stamp its glyph (if any). */
static void draw_cell(uint32_t col, uint32_t row) {
    uint32_t px = MARGIN + col * CELL, py = MARGIN + row * CELL;
    fb_rect(px, py, CELL, CELL, bg_at(py));
    unsigned char ch = (unsigned char)grid[row][col];
    if (ch >= 32 && ch < 128) {
        const unsigned char *g = font8x8_basic[ch & 0x7F];
        for (int ry = 0; ry < 8; ry++) {
            unsigned char bits = g[ry];
            for (int rx = 0; rx < 8; rx++)
                if (bits & (1u << rx))
                    fb_rect(px + rx * SCALE, py + ry * SCALE, SCALE, SCALE, fg);
        }
    }
}

static void cursor_erase(void) {
    if (cur_on) { cur_on = 0; if (on_fb) draw_cell(ccol, crow); }
}
static void cursor_draw(void) {
    if (!on_fb) return;
    uint32_t px = MARGIN + ccol * CELL, py = MARGIN + crow * CELL;
    fb_rect(px, py + CELL - 3, CELL - 2, 2, fg);     /* underline */
    cur_on = 1;
}

void console_cursor_tick(void) {
    if (!on_fb) return;
    uint64_t t = pit_ticks();                        /* PIT @ 1000 Hz -> ms */
    if (t - cur_last >= 500) {
        cur_last = t;
        if (cur_on) cursor_erase(); else cursor_draw();
    }
}

static void repaint(void) {
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++)
            draw_cell(c, r);
}

static void scroll(void) {
    for (uint32_t r = 1; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++)
            grid[r - 1][c] = grid[r][c];
    for (uint32_t c = 0; c < cols; c++) grid[rows - 1][c] = 0;
    if (on_fb) repaint();
    crow = rows - 1;
}

static void newline(void) {
    ccol = 0;
    if (++crow >= rows) scroll();
}

void console_clear(void) {
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++)
            grid[r][c] = 0;
    ccol = crow = 0;
    if (on_fb) repaint();
    cur_on = 0;
    cur_last = pit_ticks();
}

/* Ping the Bochs VBE DISPI enable register to force VirtualBox (under NEM) to
 * re-examine the framebuffer content. Also on plain QEMU this is a harmless
 * no-op that re-asserts the same state. */
static void vbe_ping(void) {
    outw(0x01CE, 4);               /* DI_ENABLE */
    outw(0x01CF, 0);               /* disable   */
    outw(0x01CE, 4);
    outw(0x01CF, 0x41);           /* enable + LFB */
}

void console_init(void) {
    if (!fb_present()) { on_fb = 0; return; }
    on_fb = 1;
    W = fb_width();
    H = fb_height();

    for (uint32_t y = 0; y < H; y++) {
        uint32_t c = lerp_color(0x0F0C29, 0x24243E, (int)y, (int)H);
        fb_rect(0, y, W, 1, c);
    }
    fb_rect(0, H - 52, W, 52, 0x0D0D0F);     /* taskbar */
    fb_rect(0, H - 52, W, 1,  0x2E2E38);
    draw_bolt(30, H - 44, 0x5E6EFF);          /* bolt logo */
    vbe_ping();                                /* flush to VirtualBox display */

    bottom = H - 60;
    cols = (W - 2 * MARGIN) / CELL; if (cols > MAXCOLS) cols = MAXCOLS;
    rows = (bottom - MARGIN) / CELL; if (rows > MAXROWS) rows = MAXROWS;
    ccol = crow = 0;
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++)
            grid[r][c] = 0;
    cur_on = 0;
    cur_last = 0;
}

static void putc_fb(char c) {
    cursor_erase();                          /* never render over the cursor */
    if (c == '\r') { ccol = 0; return; }
    if (c == '\n') { newline(); return; }
    if (c == '\b') {
        if (ccol > 0)      ccol--;
        else if (crow > 0) { crow--; ccol = cols - 1; }
        grid[crow][ccol] = 0;
        draw_cell(ccol, crow);
        return;
    }
    if (c == '\t') {
        uint32_t n = 4 - (ccol & 3);
        while (n--) {
            grid[crow][ccol] = ' ';
            draw_cell(ccol, crow);
            if (++ccol >= cols) newline();
        }
        return;
    }
    if ((unsigned char)c < 32) return;       /* drop remaining control chars */
    grid[crow][ccol] = c;
    draw_cell(ccol, crow);
    if (++ccol >= cols) newline();
}

/* kprintf's sink: serial always; then a redirect sink if one is installed
 * (GUI terminal), otherwise the framebuffer text console when present. */
void kputc(char c) {
    if (c == '\n') serial_putc('\r');
    serial_putc(c);
    if (sink)       sink(c);
    else if (on_fb) putc_fb(c);
}
