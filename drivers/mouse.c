#include <stdint.h>
#include "mouse.h"
#include "interrupts.h"
#include "io.h"

/* ---------------------------------------------------------------------------
 *  8042 PS/2 controller + mouse. The controller multiplexes the keyboard and
 *  the "auxiliary" (mouse) device over ports 0x60 (data) / 0x64 (cmd+status).
 *  Status bit0 = output buffer full (data to read), bit1 = input buffer full
 *  (don't write yet), bit5 = the byte came from the mouse, not the keyboard.
 * ------------------------------------------------------------------------- */
#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

static volatile int     mx, my;          /* absolute cursor position */
static volatile int     sw, sh;          /* screen bounds for clamping */
static volatile uint8_t btns;            /* current button bitmask */
static volatile int     evt;             /* set on every decoded packet */

static volatile uint8_t pkt[4];
static volatile uint8_t phase;           /* which byte of the packet is next */
static volatile uint8_t pktsz = 3;       /* 3 (plain) or 4 (IntelliMouse wheel) */
static volatile int     wheelacc;        /* accumulated wheel delta (+up / -down) */

/* Bounded spin so a missing/!ready controller can never wedge the kernel. */
static void wait_write(void) { for (int i = 0; i < 100000; i++) if (!(inb(PS2_STATUS) & 2)) return; }
static void wait_read(void)  { for (int i = 0; i < 100000; i++) if (inb(PS2_STATUS) & 1)   return; }

static uint8_t ctl_read(void)        { wait_read(); return inb(PS2_DATA); }
static void    ctl_cmd(uint8_t c)    { wait_write(); outb(PS2_CMD, c); }
static void    ctl_write(uint8_t v)  { wait_write(); outb(PS2_DATA, v); }

/* Send a byte to the mouse (prefix 0xD4) and swallow its 0xFA acknowledge. */
static void mouse_cmd(uint8_t v) {
    ctl_cmd(0xD4);
    ctl_write(v);
    ctl_read();                          /* ACK */
}

/* finished a full packet (3 or 4 bytes): apply movement, buttons and wheel */
static void decode_packet(void) {
    uint8_t f = pkt[0];
    if (f & 0xC0) return;                      /* X/Y overflow -> drop packet */
    int dx = (int)pkt[1] - (int)((f << 4) & 0x100);   /* sign-extend 9-bit */
    int dy = (int)pkt[2] - (int)((f << 3) & 0x100);
    mx += dx;
    my -= dy;                                  /* screen Y grows downward */
    if (mx < 0)      mx = 0;
    if (my < 0)      my = 0;
    if (mx > sw - 1) mx = sw - 1;
    if (my > sh - 1) my = sh - 1;
    btns = f & 0x07;
    if (pktsz == 4) {                          /* low nibble of byte3 is signed Z */
        int z = pkt[3] & 0x0F;
        if (z & 0x08) z -= 16;
        wheelacc += z;
    }
    evt = 1;
}

/* Apply a movement/button update from an external transport (USB HID boot
 * mouse). Deltas are already sign-extended; USB Y grows downward, matching the
 * screen, so it is added directly (unlike the PS/2 path which inverts it). */
void mouse_inject(int dx, int dy, uint8_t buttons, int wheel) {
    mx += dx; my += dy;
    if (mx < 0)      mx = 0;
    if (my < 0)      my = 0;
    if (mx > sw - 1) mx = sw - 1;
    if (my > sh - 1) my = sh - 1;
    btns = buttons & 0x07;
    wheelacc += wheel;
    evt = 1;
}

static void on_irq(struct registers *r) {
    (void)r;
    uint8_t st = inb(PS2_STATUS);
    if (!(st & 1) || !(st & 0x20)) return;     /* nothing, or it's keyboard data */
    uint8_t data = inb(PS2_DATA);

    switch (phase) {
    case 0:
        if (!(data & 0x08)) return;            /* resync: bit3 of byte0 is always 1 */
        pkt[0] = data; phase = 1; break;
    case 1:
        pkt[1] = data; phase = 2; break;
    case 2:
        pkt[2] = data;
        if (pktsz == 4) { phase = 3; break; }
        phase = 0; decode_packet(); break;
    case 3:
        pkt[3] = data; phase = 0; decode_packet(); break;
    }
}

/* Re-clamp the cursor to a new panel size after a resolution change. Keeps the
 * pointer on screen instead of stranded off the new bottom-right edge. */
void mouse_set_bounds(int screen_w, int screen_h) {
    sw = screen_w; sh = screen_h;
    if (mx > sw - 1) mx = sw - 1;
    if (my > sh - 1) my = sh - 1;
}

void mouse_init(int screen_w, int screen_h) {
    sw = screen_w; sh = screen_h;
    mx = screen_w / 2; my = screen_h / 2;
    btns = 0; phase = 0; evt = 0;

    ctl_cmd(0xA8);                             /* enable auxiliary device */

    ctl_cmd(0x20);                             /* read controller config byte */
    uint8_t cfg = ctl_read();
    cfg |=  0x02;                              /* enable IRQ12 (mouse interrupt) */
    cfg &= ~0x20;                              /* enable the mouse clock */
    ctl_cmd(0x60);                             /* write controller config byte */
    ctl_write(cfg);

    mouse_cmd(0xF6);                           /* set defaults */

    /* IntelliMouse "knock": rates 200/100/80 then read device id; id 3 means a
     * scroll wheel is present and the mouse will stream 4-byte packets. */
    mouse_cmd(0xF3); mouse_cmd(200);
    mouse_cmd(0xF3); mouse_cmd(100);
    mouse_cmd(0xF3); mouse_cmd(80);
    ctl_cmd(0xD4); ctl_write(0xF2); ctl_read();   /* get-id cmd + ACK */
    uint8_t id = ctl_read();
    if (id == 0x03) pktsz = 4;
    mouse_cmd(0xF3); mouse_cmd(100);           /* restore a sane sample rate */

    mouse_cmd(0xF4);                           /* enable packet streaming */

    irq_install(12, on_irq);
}

int     mouse_x(void)        { return mx; }
int     mouse_y(void)        { return my; }
uint8_t mouse_buttons(void)  { return btns; }
int     mouse_poll_event(void) { int e = evt; evt = 0; return e; }
int     mouse_wheel(void)      { int w = wheelacc; wheelacc = 0; return w; }
