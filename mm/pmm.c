#include <stdint.h>
#include "pmm.h"
#include "mm.h"
#include "kprintf.h"

#define FRAME 4096ull

struct e820 {
    uint64_t base;
    uint64_t len;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed));

extern char __kernel_end[];        /* from linker.ld */

static uint64_t *bitmap;
static uint64_t  nframes;
static uint64_t  used_frames;

static void set_used(uint64_t f) { bitmap[f >> 6] |=  (1ull << (f & 63)); }
static void set_free(uint64_t f) { bitmap[f >> 6] &= ~(1ull << (f & 63)); }
static int  is_used(uint64_t f)  { return (bitmap[f >> 6] >> (f & 63)) & 1; }

uint64_t pmm_total_count(void) { return nframes; }
uint64_t pmm_free_count(void)  { return nframes - used_frames; }

void pmm_reserve_range(uint64_t base, uint64_t len) {
    uint64_t s  = base / FRAME;
    uint64_t en = (base + len + FRAME - 1) / FRAME;
    for (uint64_t f = s; f < en && f < nframes; f++)
        if (!is_used(f)) { set_used(f); used_frames++; }
}

void pmm_init(struct bootinfo *bi) {
    struct e820 *e = (struct e820 *)P2V((uint64_t)bi->e820_addr);
    uint32_t n = bi->e820_count;

    uint64_t highest = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t top = e[i].base + e[i].len;
        if (e[i].type == 1 && top > highest) highest = top;
    }
    if (highest > 0x100000000ull) highest = 0x100000000ull;   /* identity map = 4GiB */
    nframes = highest / FRAME;

    /* place the bitmap on the first frame past the (physical) kernel image */
    uint64_t bm = (KV2P(__kernel_end) + FRAME - 1) & ~(FRAME - 1);
    bitmap = (uint64_t *)P2V(bm);
    uint64_t bm_bytes = ((nframes + 7) / 8 + 7) & ~7ull;

    /* start with everything reserved... */
    for (uint64_t i = 0; i < (nframes + 63) / 64; i++) bitmap[i] = ~0ull;
    used_frames = nframes;

    /* ...then free the usable E820 regions... */
    for (uint32_t i = 0; i < n; i++) {
        if (e[i].type != 1) continue;
        uint64_t s  = (e[i].base + FRAME - 1) / FRAME;
        uint64_t en = (e[i].base + e[i].len) / FRAME;
        for (uint64_t f = s; f < en && f < nframes; f++)
            if (is_used(f)) { set_free(f); used_frames--; }
    }

    /* ...and reserve everything from address 0 up to the end of the bitmap in
     * one contiguous range: low memory (IVT, bootloader, page tables), the whole
     * kernel image incl. .bss, and the bitmap itself (which sits just past the
     * kernel). This scales with the kernel size -- a hardcoded 2 MiB low reserve
     * would let .bss past 2 MiB be handed out once the kernel grows large (e.g.
     * with big embedded blobs), silently corrupting kernel state. */
    uint64_t reserved_top = (bm + bm_bytes + FRAME - 1) & ~(FRAME - 1);
    if (reserved_top < 0x200000) reserved_top = 0x200000;
    pmm_reserve_range(0, reserved_top);

    kprintf("[pmm] E820 map (%u entries):\n", n);
    for (uint32_t i = 0; i < n; i++)
        kprintf("      base=0x%lx len=0x%lx type=%u\n", e[i].base, e[i].len, e[i].type);
    kprintf("[pmm] %lu frames managed (%lu MiB), %lu free (%lu MiB), bitmap @0x%lx\n",
            nframes, (nframes * FRAME) >> 20,
            pmm_free_count(), (pmm_free_count() * FRAME) >> 20, bm);
}

uint64_t pmm_alloc_frame(void) {
    uint64_t words = (nframes + 63) / 64;
    for (uint64_t i = 0; i < words; i++) {
        if (bitmap[i] == ~0ull) continue;
        for (int b = 0; b < 64; b++) {
            uint64_t f = i * 64 + b;
            if (f >= nframes) break;
            if (!is_used(f)) { set_used(f); used_frames++; return f * FRAME; }
        }
    }
    return 0;
}

void pmm_free_frame(uint64_t addr) {
    uint64_t f = addr / FRAME;
    if (f < nframes && is_used(f)) { set_free(f); used_frames--; }
}

uint64_t pmm_alloc_contig(uint64_t n) {
    if (n == 0) return 0;
    /* linear scan for a run of n free frames; bounded by nframes (small n) */
    uint64_t run = 0, start = 0;
    for (uint64_t f = 0; f < nframes; f++) {
        if (is_used(f)) { run = 0; continue; }
        if (run == 0) start = f;
        if (++run == n) {
            for (uint64_t g = start; g < start + n; g++) { set_used(g); used_frames++; }
            return start * FRAME;
        }
    }
    return 0;
}

void pmm_free_contig(uint64_t base, uint64_t n) {
    uint64_t s = base / FRAME;
    for (uint64_t f = s; f < s + n && f < nframes; f++)
        if (is_used(f)) { set_free(f); used_frames--; }
}
