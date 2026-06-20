#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Minimal PE32+ (Windows x86-64) executable loader.
 *
 *  Loads a real Windows console .exe into a heap image, copies its sections by
 *  RVA, resolves its kernel32 imports against an in-kernel shim (GetStdHandle /
 *  WriteConsoleA / WriteFile / ExitProcess / Heap*), and calls the entry point
 *  using the Microsoft x64 calling convention. Output goes through kputc(), so
 *  the Terminal/IDE capture it. Returns the process exit code, or -1 on a load
 *  error. (No base relocations are needed: the toolchain emits RIP-relative
 *  code, so the image is position-independent once imports are bound.)
 * ===========================================================================*/
int pe_run(const uint8_t *image, uint32_t size);

/* Build a real Windows PE32+ .exe from BoltOS source by patching the embedded
 * boltrt template (BoltCC/BoltPython baked in). lang: 0=C 1=C++ 2=C# 3=Python.
 * Returns a freshly kmalloc'd image (caller kfree) and sets *out_sz, or 0 on
 * failure. The result runs on Windows 11 and under pe_run(). */
uint8_t *pe_build_exe(int lang, const uint8_t *src, uint32_t len, uint32_t *out_sz);
