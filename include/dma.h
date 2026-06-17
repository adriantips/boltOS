#pragma once
#include <stdint.h>

/* Coherent DMA buffers for device descriptor rings and packet storage.
 *
 * BoltOS manages only physical RAM below 4 GiB (the PMM caps the map at 4 GiB),
 * so every buffer is addressable by a 32-bit DMA engine -- no bounce buffers
 * needed. Buffers are physically contiguous (page-granular) and reachable in
 * kernel space through the shared direct map, so .virt is valid under any CR3.
 *
 * Coherency note: under QEMU, device and CPU views are coherent. On real
 * hardware these pages should be mapped uncached/write-combining; that remap is
 * deferred to the real-HW bring-up (P5) and isolated here. */
struct dma_buf {
    void    *virt;   /* CPU pointer (direct map)            */
    uint64_t phys;   /* bus/physical address for the device */
    uint64_t size;   /* rounded up to a page multiple       */
};

/* Allocate >= size bytes, zeroed. Returns 0 on success, -1 on OOM. */
int  dma_alloc(uint64_t size, struct dma_buf *out);
void dma_free(struct dma_buf *buf);
