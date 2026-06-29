#pragma once
/* Userland shim for kernel/kheap.h. Code shared with the kernel (e.g. the
 * BoltJS interpreter, kernel/js.c) calls kmalloc/kfree; in ring 3 those map
 * onto the userland heap (SYS_BRK-backed malloc in user/ulibc.c). This header
 * is found first because UCFLAGS lists -Iuser before -Iinclude. */
#include "ulibc.h"

static inline void *kmalloc(unsigned long n) { return malloc(n); }
static inline void  kfree(void *p)           { free(p); }
