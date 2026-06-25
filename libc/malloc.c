/* ===========================================================================
 *  libc/malloc.c -- C heap allocator for BoltOS's freestanding libc.
 *
 *  The kernel heap (mm/kheap.c) exposes only kmalloc/kfree with no size query
 *  and no realloc, so this layer stores a 16-byte bookkeeping prefix in front of
 *  every block: a magic tag and the payload size (for realloc/calloc). The user
 *  pointer therefore stays 16-byte aligned, satisfying max_align_t.
 *
 *  Over-aligned blocks (aligned_alloc, align>16) additionally stash the original
 *  kmalloc base pointer in the 8 bytes just below the header so free() can route
 *  back to kfree() regardless of how far the payload was bumped up for alignment.
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include "kheap.h"
#include "string.h"

#define HDR     16u
#define MAGIC_N 0xA110C8EDu     /* normal block: header sits at the kmalloc base */
#define MAGIC_A 0xA11C9EDu      /* aligned block: real base stored at hdr-8       */

typedef struct { uint32_t magic; uint32_t pad; uint64_t size; } hdr_t;

void *malloc(size_t n) {
    if (n == 0) n = 1;
    if (n > 0xF0000000ull) return 0;
    uint8_t *raw = (uint8_t *)kmalloc(n + HDR);
    if (!raw) return 0;
    hdr_t *h = (hdr_t *)raw;
    h->magic = MAGIC_N;
    h->size  = n;
    return raw + HDR;
}

void free(void *p) {
    if (!p) return;
    hdr_t *h = (hdr_t *)((uint8_t *)p - HDR);
    if (h->magic == MAGIC_N) {
        h->magic = 0;
        kfree(h);
    } else if (h->magic == MAGIC_A) {
        void *base = *(void **)((uint8_t *)p - HDR - sizeof(void *));
        h->magic = 0;
        kfree(base);
    }
    /* unknown magic: not ours -> ignore (double-free / foreign pointer guard) */
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb && size > (size_t)-1 / nmemb) return 0;
    size_t total = nmemb * size;
    void *p = malloc(total ? total : 1);
    if (p) memset(p, 0, total ? total : 1);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return 0; }
    hdr_t *h = (hdr_t *)((uint8_t *)p - HDR);
    if (h->magic != MAGIC_N && h->magic != MAGIC_A) return 0;
    uint64_t old = h->size;
    if (n <= old) return p;                     /* shrink/refit in place        */
    void *np = malloc(n);
    if (!np) return 0;
    memcpy(np, p, (size_t)old);
    free(p);
    return np;
}

void *reallocarray(void *p, size_t nmemb, size_t size) {
    if (nmemb && size > (size_t)-1 / nmemb) return 0;
    return realloc(p, nmemb * size);
}

void *aligned_alloc(size_t align, size_t size) {
    if (align <= HDR) return malloc(size);
    if (align & (align - 1)) return 0;          /* power of two required        */
    if (size == 0) size = 1;
    /* worst-case bump: align-1; plus header + base word below it */
    uint8_t *raw = (uint8_t *)kmalloc(size + align + HDR + sizeof(void *));
    if (!raw) return 0;
    uintptr_t lowest = (uintptr_t)raw + HDR + sizeof(void *);
    uintptr_t a = (lowest + (align - 1)) & ~(uintptr_t)(align - 1);
    hdr_t *h = (hdr_t *)(a - HDR);
    h->magic = MAGIC_A;
    h->size  = size;
    *(void **)(a - HDR - sizeof(void *)) = raw;
    return (void *)a;
}

int posix_memalign(void **out, size_t align, size_t size) {
    if (align < sizeof(void *) || (align & (align - 1))) return 22 /*EINVAL*/;
    void *p = aligned_alloc(align, size);
    if (!p) return 12 /*ENOMEM*/;
    *out = p;
    return 0;
}
