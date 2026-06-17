#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Firmware blob source (P4).
 *
 *  WiFi parts (ath9k-htc and friends) require a vendor firmware image to be
 *  uploaded before the MAC runs. BoltOS has no on-disk firmware store yet, so
 *  blobs are registered into an in-RAM table -- by a linked-in .rodata array, or
 *  later by the filesystem. A driver calls firmware_request(name) at probe time
 *  and DMA-uploads the bytes. This isolates "where firmware comes from" from the
 *  driver, so the driver code is identical whether the blob is built in or loaded.
 * ===========================================================================*/
struct firmware { const uint8_t *data; uint32_t size; };

/* Register a named blob. data must stay live for the kernel's lifetime. */
void firmware_provide(const char *name, const uint8_t *data, uint32_t size);

/* Look up a blob by name. Returns 0 and fills *fw on hit, -1 if absent. */
int  firmware_request(const char *name, struct firmware *fw);
