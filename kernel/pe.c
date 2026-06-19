/* ===========================================================================
 *  BoltOS  -  kernel/pe.c   (PE32+ loader + kernel32 shim, see include/pe.h)
 * ===========================================================================*/
#include <stdint.h>
#include "pe.h"
#include "string.h"
#include "kheap.h"
#include "kprintf.h"

/* unaligned reads from the raw image */
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* ----------------------------------------------------------- kernel32 shim - */
/* All exports use the Microsoft x64 ABI so PE code can call them directly. */
#define WINAPI __attribute__((ms_abi))

static void  *g_pejmp_set;       /* non-zero once setjmp armed   */
static void  *g_pejmp[5];
static int    g_exitcode;

static void   pe_emit(const char *buf, uint32_t n) { for (uint32_t i = 0; i < n; i++) kputc(buf[i]); }

static uint64_t WINAPI sh_GetStdHandle(uint32_t which) { (void)which; return 2; }
static int      WINAPI sh_WriteConsoleA(uint64_t h, const void *buf, uint32_t n, uint32_t *written, void *resv) {
    (void)h; (void)resv; pe_emit((const char *)buf, n); if (written) *written = n; return 1;
}
static int      WINAPI sh_WriteFile(uint64_t h, const void *buf, uint32_t n, uint32_t *written, void *ov) {
    (void)h; (void)ov; pe_emit((const char *)buf, n); if (written) *written = n; return 1;
}
static void     WINAPI sh_ExitProcess(uint32_t code) { g_exitcode = (int)code; __builtin_longjmp(g_pejmp, 1); }
static uint32_t WINAPI sh_GetLastError(void) { return 0; }
static void     WINAPI sh_SetLastError(uint32_t e) { (void)e; }
static char    *WINAPI sh_GetCommandLineA(void) { static char cl[] = "program.exe"; return cl; }
static uint64_t WINAPI sh_GetModuleHandleA(const char *m) { (void)m; return 0x10000; }
static uint64_t WINAPI sh_GetProcessHeap(void) { return 0x00010001; }
static void    *WINAPI sh_HeapAlloc(uint64_t h, uint32_t flags, uint64_t n) { (void)h; void *p = kmalloc(n ? n : 1); if (p && (flags & 8)) memset(p, 0, n); return p; }
static int      WINAPI sh_HeapFree(uint64_t h, uint32_t flags, void *p) { (void)h; (void)flags; if (p) kfree(p); return 1; }
static void     WINAPI sh_Sleep(uint32_t ms) { (void)ms; }
static uint32_t WINAPI sh_GetTickCount(void) { return 0; }

/* fallback for any unresolved import: print a note and return 0 */
static uint64_t WINAPI sh_missing(void) { return 0; }

typedef struct { const char *name; void *fn; } shim_t;
static const shim_t SHIMS[] = {
    { "GetStdHandle",    (void *)sh_GetStdHandle },
    { "WriteConsoleA",   (void *)sh_WriteConsoleA },
    { "WriteConsoleW",   (void *)sh_WriteConsoleA },
    { "WriteFile",       (void *)sh_WriteFile },
    { "ExitProcess",     (void *)sh_ExitProcess },
    { "GetLastError",    (void *)sh_GetLastError },
    { "SetLastError",    (void *)sh_SetLastError },
    { "GetCommandLineA", (void *)sh_GetCommandLineA },
    { "GetModuleHandleA",(void *)sh_GetModuleHandleA },
    { "GetProcessHeap",  (void *)sh_GetProcessHeap },
    { "HeapAlloc",       (void *)sh_HeapAlloc },
    { "HeapFree",        (void *)sh_HeapFree },
    { "Sleep",           (void *)sh_Sleep },
    { "GetTickCount",    (void *)sh_GetTickCount },
    { 0, 0 },
};
static void *resolve_import(const char *name) {
    for (int i = 0; SHIMS[i].name; i++) if (strcmp(SHIMS[i].name, name) == 0) return SHIMS[i].fn;
    kprintf("[pe] unresolved import: %s (stubbed)\n", name);
    return (void *)sh_missing;
}

/* --------------------------------------------------------------- loader ---- */
typedef __attribute__((ms_abi)) void (*pe_entry_t)(void);

int pe_run(const uint8_t *image, uint32_t size) {
    if (size < 0x40 || image[0] != 'M' || image[1] != 'Z') { kprintf("[pe] not an MZ image\n"); return -1; }
    uint32_t lfa = rd32(image + 0x3C);
    if (lfa + 0x108 > size) { kprintf("[pe] bad e_lfanew\n"); return -1; }
    const uint8_t *pe = image + lfa;
    if (pe[0] != 'P' || pe[1] != 'E' || pe[2] || pe[3]) { kprintf("[pe] no PE signature\n"); return -1; }

    const uint8_t *coff = pe + 4;
    uint16_t machine  = rd16(coff + 0);
    uint16_t nsec     = rd16(coff + 2);
    uint16_t optsz    = rd16(coff + 16);
    if (machine != 0x8664) { kprintf("[pe] not x86-64 (machine %x)\n", machine); return -1; }

    const uint8_t *opt = coff + 20;
    if (rd16(opt) != 0x20B) { kprintf("[pe] not PE32+\n"); return -1; }
    uint32_t entry_rva = rd32(opt + 16);
    uint32_t sizeimg   = rd32(opt + 56);
    uint32_t sizehdr   = rd32(opt + 60);
    uint32_t numrva    = rd32(opt + 108);
    uint32_t imp_rva = 0, imp_sz = 0;
    if (numrva > 1) { imp_rva = rd32(opt + 112 + 8 * 1); imp_sz = rd32(opt + 112 + 8 * 1 + 4); }
    (void)imp_sz;
    if (!sizeimg || sizeimg > 64u * 1024 * 1024) { kprintf("[pe] bad SizeOfImage\n"); return -1; }

    uint8_t *img = (uint8_t *)kmalloc(sizeimg);
    if (!img) { kprintf("[pe] out of memory\n"); return -1; }
    memset(img, 0, sizeimg);
    if (sizehdr > size) sizehdr = size;
    memcpy(img, image, sizehdr);

    /* copy each section to its RVA */
    const uint8_t *sec = opt + optsz;
    for (int i = 0; i < nsec; i++) {
        const uint8_t *s = sec + i * 40;
        uint32_t vaddr = rd32(s + 12);
        uint32_t rawsz = rd32(s + 16);
        uint32_t rawpt = rd32(s + 20);
        if ((uint64_t)vaddr + rawsz > sizeimg || (uint64_t)rawpt + rawsz > size) continue;
        if (rawsz) memcpy(img + vaddr, image + rawpt, rawsz);
    }

    /* bind imports: write resolved shim pointers into the IAT (FirstThunk) */
    if (imp_rva) {
        for (uint32_t d = 0; ; d += 20) {
            const uint8_t *desc = img + imp_rva + d;
            if (imp_rva + d + 20 > sizeimg) break;
            uint32_t oft  = rd32(desc + 0);
            uint32_t name = rd32(desc + 12);
            uint32_t ft   = rd32(desc + 16);
            if (!oft && !name && !ft) break;
            uint32_t ilt = oft ? oft : ft;
            for (uint32_t k = 0; ; k++) {
                uint32_t to = ilt + k * 8, io = ft + k * 8;
                if (to + 8 > sizeimg || io + 8 > sizeimg) break;
                uint64_t thunk = rd64(img + to);
                if (!thunk) break;
                void *fn;
                if (thunk & 0x8000000000000000ull) fn = (void *)sh_missing;   /* by ordinal */
                else {
                    const char *fname = (const char *)(img + (uint32_t)(thunk & 0x7FFFFFFF) + 2); /* skip hint */
                    fn = resolve_import(fname);
                }
                memcpy(img + io, &fn, 8);
            }
        }
    }

    if (entry_rva >= sizeimg) { kprintf("[pe] bad entry\n"); kfree(img); return -1; }
    pe_entry_t entry = (pe_entry_t)(void *)(img + entry_rva);

    g_exitcode = 0;
    if (__builtin_setjmp(g_pejmp)) {        /* ExitProcess longjmps back here */
        kfree(img);
        return g_exitcode;
    }
    g_pejmp_set = (void *)1;
    entry();                                /* MS x64 ABI call into the program */
    g_pejmp_set = 0;
    kfree(img);
    return g_exitcode;
}
