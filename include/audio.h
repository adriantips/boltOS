#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Audio output. Drives an AC'97 codec (QEMU's -device AC97) with a real DMA
 *  buffer-descriptor list for 48 kHz / 16-bit / stereo PCM. Detects Intel HDA
 *  as well. Falls back to the PC speaker when no PCI audio device is present so
 *  audio_tone() always makes a sound.
 * ===========================================================================*/

void        audio_init(void);
int         audio_present(void);          /* 1 if a PCI codec was claimed */
const char *audio_name(void);             /* "AC97" / "Intel HDA" / "PC speaker" */

/* Play a square-wave tone of `freq` Hz for `ms` milliseconds (blocking). */
void audio_tone(uint32_t freq, uint32_t ms);

/* Play raw 16-bit signed stereo PCM at 48 kHz (frames = L/R pairs). Blocking. */
void audio_play_pcm(const int16_t *interleaved, uint32_t frames);

void audio_set_volume(int percent);       /* 0..100 master volume */
int  audio_volume(void);
