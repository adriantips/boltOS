#include <stdint.h>
#include "kheap.h"
#include "pmm.h"
#include "mm.h"

/* A simple first-fit free-list heap over a fixed window starting at physical
 * 16 MiB (identity-mapped, reserved from the PMM). Good enough to back the rest
 * of the kernel; a slab allocator can replace it later. Sized at 96 MiB so the
 * DOOM port has room for its ~6 MiB zone, the 640x400 framebuffer and WAD lump
 * cache on top of the desktop's compositor buffers. */
#define KHEAP_START 0x1000000ull   /* 16 MiB */
#define KHEAP_SIZE  0x6000000ull   /* 96 MiB */

typedef struct block {
    uint64_t       size;           /* payload size, excluding header */
    struct block  *next;
    int            free;
} block_t;

static block_t *head;

void kheap_init(void) {
    pmm_reserve_range(KHEAP_START, KHEAP_SIZE);
    head = (block_t *)P2V(KHEAP_START);    /* direct map: valid under any CR3 */
    head->size = KHEAP_SIZE - sizeof(block_t);
    head->next = 0;
    head->free = 1;
}

void *kmalloc(uint64_t size) {
    size = (size + 15) & ~15ull;
    for (block_t *b = head; b; b = b->next) {
        if (!b->free || b->size < size) continue;
        if (b->size >= size + sizeof(block_t) + 16) {     /* split */
            block_t *nb = (block_t *)((uint8_t *)b + sizeof(block_t) + size);
            nb->size = b->size - size - sizeof(block_t);
            nb->next = b->next;
            nb->free = 1;
            b->size = size;
            b->next = nb;
        }
        b->free = 0;
        return (uint8_t *)b + sizeof(block_t);
    }
    return 0;
}

void kfree(void *p) {
    if (!p) return;
    block_t *b = (block_t *)((uint8_t *)p - sizeof(block_t));
    b->free = 1;
    if (b->next && b->next->free) {                       /* coalesce forward */
        b->size += sizeof(block_t) + b->next->size;
        b->next  = b->next->next;
    }
}

/* Walk the free list to report live allocation. *used = payload bytes handed
 * out, *total = bytes under management. Backs the Task Manager heap readout. */
void kheap_usage(uint64_t *used, uint64_t *total) {
    uint64_t u = 0, t = 0;
    for (block_t *b = head; b; b = b->next) {
        t += b->size + sizeof(block_t);
        if (!b->free) u += b->size;
    }
    if (used)  *used  = u;
    if (total) *total = t;
}
