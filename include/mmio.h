#pragma once
#include <stdint.h>

/* Memory-mapped I/O accessors. Device registers must be touched with explicit
 * width and never cached/reordered by the compiler, hence volatile + a compiler
 * barrier. (On real hardware the BAR pages should also be mapped uncached; under
 * QEMU caching is not modelled so the direct map suffices — see pci_map_bar.) */

static inline uint32_t mmio_read32(volatile void *base, uint32_t off) {
    uint32_t v = *(volatile uint32_t *)((volatile uint8_t *)base + off);
    __asm__ volatile("" ::: "memory");
    return v;
}
static inline void mmio_write32(volatile void *base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)((volatile uint8_t *)base + off) = v;
    __asm__ volatile("" ::: "memory");
}
static inline uint16_t mmio_read16(volatile void *base, uint32_t off) {
    uint16_t v = *(volatile uint16_t *)((volatile uint8_t *)base + off);
    __asm__ volatile("" ::: "memory");
    return v;
}
static inline void mmio_write16(volatile void *base, uint32_t off, uint16_t v) {
    *(volatile uint16_t *)((volatile uint8_t *)base + off) = v;
    __asm__ volatile("" ::: "memory");
}
static inline uint8_t mmio_read8(volatile void *base, uint32_t off) {
    uint8_t v = *(volatile uint8_t *)((volatile uint8_t *)base + off);
    __asm__ volatile("" ::: "memory");
    return v;
}
static inline void mmio_write8(volatile void *base, uint32_t off, uint8_t v) {
    *(volatile uint8_t *)((volatile uint8_t *)base + off) = v;
    __asm__ volatile("" ::: "memory");
}
