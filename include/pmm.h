#pragma once
#include <stdint.h>
#include "boot.h"

void     pmm_init(struct bootinfo *bi);
uint64_t pmm_alloc_frame(void);              /* returns physical addr, 0 = OOM */
void     pmm_free_frame(uint64_t addr);

/* Allocate n physically-contiguous 4 KiB frames; returns base phys addr (0 =
 * none). Used for DMA descriptor rings and buffers. */
uint64_t pmm_alloc_contig(uint64_t n);
void     pmm_free_contig(uint64_t base, uint64_t n);
void     pmm_reserve_range(uint64_t base, uint64_t len);
uint64_t pmm_free_count(void);               /* free frames */
uint64_t pmm_total_count(void);
