/* ===========================================================================
 *  BoltRT  -  the runtime stub baked into every executable that BoltOS's
 *  `compile` command emits.
 *
 *  It is a genuine, freestanding Windows x86-64 console program (no CRT, imports
 *  kernel32) that *contains the real BoltCC / BoltPython front-ends* compiled in.
 *  The program's source code is embedded as a patchable payload (g_payload). At
 *  `compile` time, BoltOS finds the magic marker in this .exe and writes the
 *  user's source after it. At run time the stub hands that source to boltcc_run()
 *  or bpy_run() and prints the output.
 *
 *  Because it imports only the standard kernel32 entry points, the resulting
 *  .exe runs natively on Windows 11 *and* under BoltOS's own PE loader (whose
 *  kernel32 shim binds the same imports).  -- see kernel/pe.c, kernel/cmd_proc.c
 *
 *  This file provides freestanding implementations of the handful of kernel
 *  symbols that boltcc.c / boltpy.c reference (kmalloc/kfree, kputc/kprintf,
 *  panic, the kbd_* input stubs) on top of kernel32.
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include "boltcc.h"
#include "boltpy.h"

/* ------------------------------------------------------------- kernel32 ----- */
typedef uint64_t HANDLE;
typedef uint32_t DWORD;
typedef int      BOOL;

__declspec(dllimport) HANDLE GetStdHandle(DWORD nStdHandle);
__declspec(dllimport) BOOL   WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov);
__declspec(dllimport) void   ExitProcess(DWORD code);
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void  *HeapAlloc(HANDLE heap, DWORD flags, uint64_t bytes);
__declspec(dllimport) BOOL   HeapFree(HANDLE heap, DWORD flags, void *mem);

/* --------------------------------------------------------- console output --- */
static HANDLE g_out;
static char   g_buf[4096];
static int    g_bn;

static void flush_out(void) {
    if (g_bn) { DWORD w; WriteFile(g_out, g_buf, (DWORD)g_bn, &w, 0); g_bn = 0; }
}
void kputc(char c) {
    if (g_bn >= (int)sizeof(g_buf)) flush_out();
    g_buf[g_bn++] = c;
    if (c == '\n') flush_out();
}
static void kputs_raw(const char *s) { while (*s) kputc(*s++); }

/* a small printf: enough for boltcc/boltpy diagnostics (%s %d %u %x %c %p %%) */
static void put_unum(uint64_t v, int base, int upper) {
    char tmp[24]; int n = 0;
    const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = d[v % base]; v /= base; }
    while (n) kputc(tmp[--n]);
}
static void put_snum(int64_t v) {
    if (v < 0) { kputc('-'); put_unum((uint64_t)(-(v + 1)) + 1, 10, 0); }
    else put_unum((uint64_t)v, 10, 0);
}
void kprintf(const char *fmt, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { kputc(*p); continue; }
        p++;
        int lng = 0;
        while (*p == 'l' || *p == 'h' || *p == 'z') { if (*p == 'l') lng++; p++; }
        switch (*p) {
        case 's': { const char *s = __builtin_va_arg(ap, const char *); kputs_raw(s ? s : "(null)"); break; }
        case 'c': kputc((char)__builtin_va_arg(ap, int)); break;
        case 'd': case 'i':
            if (lng) put_snum(__builtin_va_arg(ap, int64_t));
            else     put_snum(__builtin_va_arg(ap, int));
            break;
        case 'u':
            if (lng) put_unum(__builtin_va_arg(ap, uint64_t), 10, 0);
            else     put_unum(__builtin_va_arg(ap, unsigned), 10, 0);
            break;
        case 'x':
            if (lng) put_unum(__builtin_va_arg(ap, uint64_t), 16, 0);
            else     put_unum(__builtin_va_arg(ap, unsigned), 16, 0);
            break;
        case 'X':
            if (lng) put_unum(__builtin_va_arg(ap, uint64_t), 16, 1);
            else     put_unum(__builtin_va_arg(ap, unsigned), 16, 1);
            break;
        case 'p': kputs_raw("0x"); put_unum(__builtin_va_arg(ap, uint64_t), 16, 0); break;
        case '%': kputc('%'); break;
        default: kputc('%'); if (*p) kputc(*p); break;
        }
    }
    __builtin_va_end(ap);
}
void panic(const char *msg) { kputs_raw("panic: "); kputs_raw(msg); kputc('\n'); flush_out(); ExitProcess(1); }

/* --------------------------------------------------------------- heap ------- */
void  kheap_init(void) {}
void *kmalloc(uint64_t size) { return HeapAlloc(GetProcessHeap(), 0, size ? size : 1); }
void  kfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
void  kheap_usage(uint64_t *used, uint64_t *total) { if (used) *used = 0; if (total) *total = 0; }

/* ------------------------------------------------------------- keyboard ----- */
/* No interactive stdin in a baked program: input() yields empty lines. */
void keyboard_init(void) {}
int  kbd_trygetc(void) { return -1; }
char kbd_getc(void) { return '\n'; }

/* --------------------------------------------------------------- payload ---- */
/* 16-byte magic, then [lang:1][len:4 LE][source...]. BoltOS's `compile` command
 * locates the magic in this .exe and writes the program's source here. The array
 * carries an initializer so it lands in .data (present in the file, patchable). */
#define PAYLOAD_CAP 65536
volatile unsigned char g_payload[PAYLOAD_CAP] = {
    0x42, 0x6F, 0x4C, 0x74, 0x50, 0x41, 0x59, 0x31,   /* "BoLtPAY1" */
    0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x13, 0x37,
    0x00,                                             /* lang (C) */
    0x00, 0x00, 0x00, 0x00,                           /* length   */
};

void entry(void) {
    g_out = GetStdHandle((DWORD)-11);                 /* STD_OUTPUT_HANDLE */

    static const unsigned char MAGIC[16] = {
        0x42, 0x6F, 0x4C, 0x74, 0x50, 0x41, 0x59, 0x31,
        0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x13, 0x37
    };
    for (int i = 0; i < 16; i++) if (g_payload[i] != MAGIC[i]) {
        kputs_raw("boltrt: no program payload (this stub was not patched).\n");
        flush_out(); ExitProcess(2);
    }

    int lang = g_payload[16];
    uint32_t len = (uint32_t)g_payload[17] | ((uint32_t)g_payload[18] << 8) |
                   ((uint32_t)g_payload[19] << 16) | ((uint32_t)g_payload[20] << 24);
    if (len == 0 || 21 + len >= PAYLOAD_CAP) {
        kputs_raw("boltrt: empty or oversized payload.\n");
        flush_out(); ExitProcess(2);
    }

    /* copy source out of the volatile payload into a NUL-terminated buffer */
    char *src = (char *)kmalloc(len + 1);
    if (!src) { kputs_raw("boltrt: out of memory.\n"); flush_out(); ExitProcess(2); }
    for (uint32_t i = 0; i < len; i++) src[i] = (char)g_payload[21 + i];
    src[len] = 0;

    int rc;
    if (lang == 3) rc = bpy_run(src);                 /* Python */
    else           rc = boltcc_run(lang, src);        /* C / C++ / C# */

    flush_out();
    ExitProcess((DWORD)rc);
}
