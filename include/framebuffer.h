#pragma once
#include <stdint.h>
#include "boot.h"

void     fb_init(struct bootinfo *bi);
int      fb_present(void);
/* Switch the physical panel to w*h 32bpp via the Bochs DISPI interface (QEMU
 * std VGA). Returns 0 on success, -1 if unsupported -- caller keeps old mode. */
int      fb_set_mode(uint32_t w, uint32_t h);
uint32_t fb_width(void);
uint32_t fb_height(void);
uint64_t fb_phys(void);      /* physical base of the linear framebuffer */
uint32_t fb_pitch_px(void);  /* scanline stride in pixels (>= width) */

/* Set while a ring-3 program owns the panel (maps the fb via SYS_FBINFO); the
 * GUI compositor skips its flip so it doesn't fight the userland renderer. */
extern volatile int g_fb_exclusive;
void     fb_pixel(uint32_t x, uint32_t y, uint32_t color);  /* color = 0x00RRGGBB */
void     fb_fill(uint32_t color);
void     fb_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void     fb_blit(const uint32_t *src);   /* copy a packed fw*fh xRGB image to VRAM */
/* nearest-neighbour scale a packed sw*sh xRGB image into panel rect (dx,dy,dw,dh);
 * the rest of the panel is cleared to black (letterbox bars). */
void     fb_blit_scaled(const uint32_t *src, uint32_t sw, uint32_t sh,
                        uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh);
