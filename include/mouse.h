#pragma once
#include <stdint.h>

/* PS/2 mouse driver. Enables the 8042 auxiliary device + IRQ12, decodes the
 * 3-byte movement packets into an absolute cursor position clamped to the
 * screen. The GUI polls the accessors below each loop iteration. */

#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

void    mouse_init(int screen_w, int screen_h);
void    mouse_set_bounds(int screen_w, int screen_h);   /* re-clamp after a res change */
int     mouse_x(void);
int     mouse_y(void);
uint8_t mouse_buttons(void);
/* Inject a movement/button update from the USB HID boot-mouse path. */
void    mouse_inject(int dx, int dy, uint8_t buttons, int wheel);
/* Returns 1 (and clears the flag) if a packet arrived since the last call. */
int     mouse_poll_event(void);
/* Accumulated scroll-wheel delta since last call (+ up / - down), then clears.
 * Always 0 on a plain 3-byte mouse with no wheel. */
int     mouse_wheel(void);
