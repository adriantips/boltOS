#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS GPU / display-adapter driver.
 *
 *  Probes the PCI bus for a display-class (0x03) controller, identifies it,
 *  decodes its framebuffer BAR to learn the VRAM aperture size, and detects the
 *  Bochs DISPI (VBE) interface QEMU's std-VGA exposes for runtime mode setting.
 *  The linear framebuffer itself is owned by drivers/framebuffer.c; this driver
 *  is the device-management layer on top of it (identify, VRAM size, mode list,
 *  mode switch) and the source of the GPU line in System Info.
 * ===========================================================================*/

struct gpu_mode { uint16_t w, h; };

int          gpu_init(void);                 /* probe PCI; returns 1 if a GPU was found */
int          gpu_present(void);
const char  *gpu_name(void);                 /* human adapter name                       */
uint16_t     gpu_vendor(void);
uint16_t     gpu_device(void);
uint64_t     gpu_vram_bytes(void);           /* framebuffer aperture size (BAR0)         */
uint64_t     gpu_fb_phys(void);              /* physical base of the LFB aperture        */
int          gpu_accel(void);                /* 1 = DISPI runtime mode setting available */
uint16_t     gpu_dispi_id(void);             /* VBE DISPI version id (0xB0Cx) or 0       */

/* A short table of standard resolutions the adapter can switch to via DISPI. */
int          gpu_mode_count(void);
const struct gpu_mode *gpu_modes(void);
int          gpu_set_mode(uint32_t w, uint32_t h);   /* -> framebuffer modeset; 0 ok */
