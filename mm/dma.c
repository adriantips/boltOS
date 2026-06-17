#include <stdint.h>
#include "dma.h"
#include "pmm.h"
#include "mm.h"

static uint64_t pages_for(uint64_t size) {
    return (size + PAGE_SIZE - 1) / PAGE_SIZE;
}

int dma_alloc(uint64_t size, struct dma_buf *out) {
    uint64_t npages = pages_for(size);
    if (npages == 0) npages = 1;
    uint64_t phys = pmm_alloc_contig(npages);
    if (!phys) return -1;

    out->phys = phys;
    out->size = npages * PAGE_SIZE;
    out->virt = P2V(phys);

    uint8_t *p = (uint8_t *)out->virt;
    for (uint64_t i = 0; i < out->size; i++) p[i] = 0;
    return 0;
}

void dma_free(struct dma_buf *buf) {
    if (!buf || !buf->phys) return;
    pmm_free_contig(buf->phys, pages_for(buf->size));
    buf->virt = 0; buf->phys = 0; buf->size = 0;
}
