/* ===========================================================================
 *  BoltOS  -  drivers/pcspk.c
 *  PC speaker driver. The speaker is driven by PIT channel 2 configured as a
 *  square-wave generator (mode 3); its output is AND-gated onto the speaker
 *  line by the low two bits of port 0x61. To make a tone we program channel 2's
 *  reload value to 1193182 / freq and raise those gate bits.
 * ===========================================================================*/
#include <stdint.h>
#include "io.h"
#include "pcspk.h"

#define PIT_FREQ 1193182u

/* Master output gate the Settings "Audio" panel flips. When muted, tone
 * requests are dropped so the desktop stays silent without touching call sites
 * (piano, stopwatch, the UI test beep all route through here). */
static int spk_enabled = 1;
void pcspk_set_enabled(int on) { spk_enabled = on ? 1 : 0; if (!spk_enabled) pcspk_off(); }
int  pcspk_enabled(void)       { return spk_enabled; }

void pcspk_tone(uint32_t hz) {
    if (!spk_enabled) return;
    if (hz == 0) { pcspk_off(); return; }
    uint32_t div = PIT_FREQ / hz;
    if (div == 0) div = 1;

    outb(0x43, 0xB6);                     /* ch2, lobyte/hibyte, mode 3 (square) */
    outb(0x42, (uint8_t)(div & 0xFF));
    outb(0x42, (uint8_t)((div >> 8) & 0xFF));

    uint8_t g = inb(0x61);
    if ((g & 3) != 3) outb(0x61, g | 3);  /* enable gate + speaker data */
}

void pcspk_off(void) {
    outb(0x61, inb(0x61) & ~3);
}
