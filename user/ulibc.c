#include "ulibc.h"
#include <stdarg.h>
#include "string.h"

/* syscall numbers (subset of include/syscall.h) */
#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_FSTAT 5
#define SYS_LSEEK 8
#define SYS_MMAP 9
#define SYS_BRK 12
#define SYS_YIELD 24
#define SYS_GETPID 39
#define SYS_EXIT 60

static long __syscall(long n, long a, long b, long c, long d, long e) {
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}

long sys_read (int fd, void *buf, unsigned long len)       { return __syscall(SYS_READ,  fd, (long)buf, (long)len, 0, 0); }
long sys_write(int fd, const void *buf, unsigned long len) { return __syscall(SYS_WRITE, fd, (long)buf, (long)len, 0, 0); }
long read (int fd, void *buf, unsigned long len)           { return sys_read(fd, buf, len); }
long write(int fd, const void *buf, unsigned long len)     { return sys_write(fd, buf, len); }
int  open (const char *path, int flags)                    { return (int)__syscall(SYS_OPEN, (long)path, flags, 0, 0, 0); }
int  close(int fd)                                         { return (int)__syscall(SYS_CLOSE, fd, 0, 0, 0, 0); }
long lseek(int fd, long off, int whence)                   { return __syscall(SYS_LSEEK, fd, off, whence, 0, 0); }
int  fstat(int fd, stat_t *st)                             { return (int)__syscall(SYS_FSTAT, fd, (long)st, 0, 0, 0); }
void *mmap(void *addr, unsigned long len, int prot, int flags, int fd)
                                                           { return (void *)__syscall(SYS_MMAP, (long)addr, (long)len, prot, flags, fd); }
void *sbrk_brk(void *addr)                                 { return (void *)__syscall(SYS_BRK, (long)addr, 0, 0, 0, 0); }
int  getpid(void)                                          { return (int)__syscall(SYS_GETPID, 0, 0, 0, 0, 0); }
void yield(void)                                           { __syscall(SYS_YIELD, 0, 0, 0, 0, 0); }
void exit(int code) { __syscall(SYS_EXIT, code, 0, 0, 0, 0); for (;;) {} }

/* --------------------------------------------------------------- heap ------
 * Bump allocator backed by SYS_BRK. free() is a no-op (sufficient for the
 * tools we run today); a real free list can replace this later. */
static unsigned char *heap_cur, *heap_end;

static void heap_init(void) {
    if (!heap_cur) heap_cur = heap_end = (unsigned char *)sbrk_brk(0);
}
void *malloc(unsigned long n) {
    heap_init();
    n = (n + 15) & ~15ul;
    if (heap_cur + n > heap_end) {
        unsigned char *want = heap_cur + n;
        unsigned char *got  = (unsigned char *)sbrk_brk(want);
        if (got < want) return 0;          /* OOM */
        heap_end = got;
    }
    void *p = heap_cur;
    heap_cur += n;
    return p;
}
void free(void *p) { (void)p; }

/* -------------------------------------------------------------- stdio ------*/
int putchar(int c) { char ch = (char)c; sys_write(1, &ch, 1); return c; }
int puts(const char *s) { sys_write(1, s, strlen(s)); putchar('\n'); return 0; }

static void put_unsigned(unsigned long v, int base, int upper) {
    char tmp[32]; int i = 0;
    const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = d[v % base]; v /= base; }
    while (i) putchar(tmp[--i]);
}
static void put_signed(long v) {
    if (v < 0) { putchar('-'); put_unsigned((unsigned long)(-v), 10, 0); }
    else put_unsigned((unsigned long)v, 10, 0);
}

int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { putchar(*p); continue; }
        p++;
        int lng = 0;
        while (*p == 'l') { lng = 1; p++; }
        switch (*p) {
        case 'd': case 'i': lng ? put_signed(va_arg(ap, long)) : put_signed(va_arg(ap, int)); break;
        case 'u': put_unsigned(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 10, 0); break;
        case 'x': put_unsigned(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 16, 0); break;
        case 'X': put_unsigned(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 16, 1); break;
        case 'p': putchar('0'); putchar('x'); put_unsigned((unsigned long)va_arg(ap, void *), 16, 0); break;
        case 'c': putchar((char)va_arg(ap, int)); break;
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; sys_write(1, s, strlen(s)); break; }
        case '%': putchar('%'); break;
        default:  putchar('%'); putchar(*p); break;
        }
    }
    va_end(ap);
    return 0;
}
