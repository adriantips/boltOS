#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS image decoder. Turns an in-memory PNG / JPEG (baseline) / GIF / BMP
 *  / TGA byte stream into a 32-bit ARGB pixel buffer. Everything is integer-only (the
 *  kernel is built with no FPU), and decoded images are box-downscaled so a
 *  single image never pins more than a few hundred KiB of the 16 MiB heap.
 * ===========================================================================*/

typedef struct {
    int       w, h;       /* dimensions in pixels                    */
    uint32_t *px;         /* w*h pixels, 0xAARRGGBB, row-major        */
} image_t;

/* Decode buf[0..len) (format detected from the magic bytes). Returns a heap
 * image on success (free with image_free) or NULL on any failure / OOM. */
image_t *image_decode(const uint8_t *buf, uint32_t len);
void     image_free(image_t *im);
