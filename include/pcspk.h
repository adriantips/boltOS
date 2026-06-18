#pragma once
#include <stdint.h>

/* PC speaker (PIT channel 2 gated through port 0x61). Square-wave beeper. */
void pcspk_tone(uint32_t hz);   /* start a continuous tone at `hz` (0 = silence) */
void pcspk_off(void);           /* stop the tone */
void pcspk_set_enabled(int on); /* master mute gate (Settings audio device)      */
int  pcspk_enabled(void);
