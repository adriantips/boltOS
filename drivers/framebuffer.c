#include <stdint.h>
#include "framebuffer.h"
#include "io.h"
#include "mm.h"

/* The VESA linear framebuffer the bootloader selected. It lives below 4 GiB
 * and is already identity-mapped, so we write straight to it. 32 bpp / xRGB. */
static volatile uint32_t *fb;
static uint64_t fb_pa;                  /* physical base of the LFB */
static uint32_t fw, fh, pitch_px;

/* Set while a ring-3 program (the userland browser) owns the screen: the GUI
 * compositor checks it and stops touching VRAM so the two never fight over the
 * panel. Cleared by SYS_FBEND or when that process exits. */
volatile int g_fb_exclusive;

void fb_init(struct bootinfo *bi) {
    if (!bi->fb_addr || !bi->fb_width || bi->fb_bpp != 32) { fb = 0; return; }
    fb_pa    = (uint64_t)bi->fb_addr;
    fb       = (volatile uint32_t *)P2V(fb_pa);
    fw       = bi->fb_width;
    fh       = bi->fb_height;
    pitch_px = bi->fb_pitch ? bi->fb_pitch / 4 : bi->fb_width;
}

uint64_t fb_phys(void)     { return fb_pa; }
uint32_t fb_pitch_px(void) { return pitch_px; }

/* ---- Bochs/QEMU DISPI runtime mode switch ------------------------------- *
 * QEMU's default "std" (Bochs) VGA exposes the DISPI register interface on I/O
 * ports 0x01CE (index) / 0x01CF (data). Writing XRES/YRES/BPP and re-enabling
 * with the LFB bit gives a real native-resolution linear framebuffer at the
 * same BAR (so the base pointer never moves). No BIOS, no real-mode trip, no
 * upscaling and no letterbox bars -- the panel itself becomes the new size. */
#define DISPI_INDEX   0x01CE
#define DISPI_DATA    0x01CF
#define DI_XRES       1
#define DI_YRES       2
#define DI_BPP        3
#define DI_ENABLE     4
#define DI_VIRT_W     6
#define DI_BANK       5
#define DI_X_OFF      8
#define DI_Y_OFF      9
#define DI_EN_ENABLED 0x01
#define DI_EN_LFB     0x40

static void dispi_w(uint16_t idx, uint16_t val) { outw(DISPI_INDEX, idx); outw(DISPI_DATA, val); }
static uint16_t dispi_r(uint16_t idx) { outw(DISPI_INDEX, idx); return inw(DISPI_DATA); }

/* Reprogram the panel to w*h, 32bpp, linear. Returns 0 on success. The LFB BAR
 * is unchanged so `fb` stays valid; we only update the cached geometry. */
int fb_set_mode(uint32_t w, uint32_t h) {
    if (!fb || !w || !h) return -1;
    dispi_w(DI_ENABLE, 0);                 /* disable while reprogramming      */
    dispi_w(DI_XRES,   (uint16_t)w);
    dispi_w(DI_YRES,   (uint16_t)h);
    dispi_w(DI_BPP,    32);
    dispi_w(DI_VIRT_W, (uint16_t)w);       /* pitch = width (no padding)       */
    dispi_w(DI_BANK,   0);
    dispi_w(DI_X_OFF,  0);
    dispi_w(DI_Y_OFF,  0);
    dispi_w(DI_ENABLE, DI_EN_ENABLED | DI_EN_LFB);
    /* verify the card accepted it; on a non-DISPI adapter the readback differs */
    if (dispi_r(DI_XRES) != (uint16_t)w || dispi_r(DI_YRES) != (uint16_t)h) return -1;
    fw = w; fh = h; pitch_px = w;
    return 0;
}

int      fb_present(void) { return fb != 0; }
uint32_t fb_width(void)   { return fw; }
uint32_t fb_height(void)  { return fh; }

void fb_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x < fw && y < fh) fb[(uint64_t)y * pitch_px + x] = color;
}

void fb_fill(uint32_t color) {
    for (uint32_t y = 0; y < fh; y++)
        for (uint32_t x = 0; x < fw; x++)
            fb[(uint64_t)y * pitch_px + x] = color;
}

void fb_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t j = 0; j < h; j++)
        for (uint32_t i = 0; i < w; i++)
            fb_pixel(x + i, y + j, color);
}

/* Blit a tightly-packed fw*fh xRGB image (the GUI compositor's backbuffer)
 * straight onto the visible framebuffer, one scanline at a time so a pitch
 * wider than the width is handled. 32-bit stores keep it fast in a freestanding
 * build with no SSE memcpy. */
void fb_blit(const uint32_t *src) {
    if (!fb) return;
    for (uint32_t y = 0; y < fh; y++) {
        volatile uint32_t *d = fb  + (uint64_t)y * pitch_px;
        const uint32_t    *s = src + (uint64_t)y * fw;
        for (uint32_t x = 0; x < fw; x++) d[x] = s[x];
    }
}

/* Scale a logical sw*sh desktop into the (dx,dy,dw,dh) rect of the panel and
 * paint black bars around it. A per-column source-index map is built once so
 * the inner loop is array lookups, not a divide per pixel. */
void fb_blit_scaled(const uint32_t *src, uint32_t sw, uint32_t sh,
                    uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh) {
    if (!fb) return;
    for (uint32_t y = 0; y < fh; y++) {           /* clear panel -> letterbox bars */
        volatile uint32_t *d = fb + (uint64_t)y * pitch_px;
        for (uint32_t x = 0; x < fw; x++) d[x] = 0;
    }
    if (!sw || !sh || !dw || !dh || dx >= fw || dy >= fh) return;

    static uint16_t colmap[4096];
    uint32_t w = dw;
    if (dx + w > fw) w = fw - dx;
    if (w > 4096)    w = 4096;
    for (uint32_t i = 0; i < w; i++) colmap[i] = (uint16_t)((uint64_t)i * sw / dw);

    for (uint32_t j = 0; j < dh; j++) {
        uint32_t py = dy + j;
        if (py >= fh) break;
        uint32_t sy = (uint32_t)((uint64_t)j * sh / dh);
        const uint32_t    *s = src + (uint64_t)sy * sw;
        volatile uint32_t *d = fb + (uint64_t)py * pitch_px + dx;
        for (uint32_t i = 0; i < w; i++) d[i] = s[colmap[i]];
    }
}
