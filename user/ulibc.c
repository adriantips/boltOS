#include "ulibc.h"
#include <stdarg.h>
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

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
 * First-fit free list backed by SYS_BRK. Each block carries a header with its
 * usable size and a free flag; free() coalesces with the following block, and
 * the allocator splits oversized blocks. Good enough for real programs. */
typedef struct blk { unsigned long size; struct blk *next; int free_; } blk_t;
#define BLK_HDR ((unsigned long)sizeof(blk_t))

static unsigned char *heap_cur, *heap_end;
static blk_t *free_head;

static void heap_init(void) {
    if (!heap_cur) heap_cur = heap_end = (unsigned char *)sbrk_brk(0);
}
static void *heap_grow(unsigned long n) {        /* extend brk by >= n bytes */
    unsigned long chunk = (n + 65535) & ~65535ul;     /* round to 64 KiB */
    unsigned char *want = heap_end + chunk;
    unsigned char *got  = (unsigned char *)sbrk_brk(want);
    if (got < want) return 0;
    void *p = heap_end;
    heap_end = got;
    return p;
}

void *malloc(unsigned long n) {
    heap_init();
    if (n == 0) return 0;
    n = (n + 15) & ~15ul;
    for (blk_t *b = free_head; b; b = b->next) {       /* first fit */
        if (b->free_ && b->size >= n) {
            if (b->size >= n + BLK_HDR + 16) {         /* split */
                blk_t *s = (blk_t *)((unsigned char *)b + BLK_HDR + n);
                s->size = b->size - n - BLK_HDR;
                s->free_ = 1; s->next = b->next;
                b->size = n; b->next = s;
            }
            b->free_ = 0;
            return (unsigned char *)b + BLK_HDR;
        }
    }
    blk_t *b = (blk_t *)heap_grow(n + BLK_HDR);          /* new block */
    if (!b) return 0;
    b->size = n; b->free_ = 0;
    b->next = free_head; free_head = b;
    return (unsigned char *)b + BLK_HDR;
}
void free(void *p) {
    if (!p) return;
    blk_t *b = (blk_t *)((unsigned char *)p - BLK_HDR);
    b->free_ = 1;
    if (b->next && b->next->free_) {                    /* coalesce forward */
        b->size += BLK_HDR + b->next->size;
        b->next = b->next->next;
    }
}
void *calloc(unsigned long nm, unsigned long sz) {
    unsigned long n = nm * sz;
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
void *realloc(void *p, unsigned long n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return 0; }
    blk_t *b = (blk_t *)((unsigned char *)p - BLK_HDR);
    if (b->size >= n) return p;
    void *q = malloc(n);
    if (q) memcpy(q, p, b->size);
    free(p);
    return q;
}

/* -------------------------------------------------------------- stdio ------
 * One sink-based formatter (fmt_core) drives printf (batched to fd 1),
 * snprintf/vsnprintf (into a caller buffer), and fprintf (any fd). */
typedef struct {
    char  *buf;      /* destination buffer, or 0 for direct-to-fd */
    unsigned long cap, len;
    int    fd;       /* used when buf == 0 */
    char   fbuf[256]; unsigned fn;   /* fd batching window */
} sink_t;

static void s_flush(sink_t *s) { if (s->fn) { sys_write(s->fd, s->fbuf, s->fn); s->fn = 0; } }
static void s_put(sink_t *s, char c) {
    s->len++;
    if (s->buf) { if (s->len <= s->cap) s->buf[s->len - 1] = c; return; }
    s->fbuf[s->fn++] = c;
    if (s->fn == sizeof s->fbuf) s_flush(s);
}
static void s_str(sink_t *s, const char *p, unsigned long n) { while (n--) s_put(s, *p++); }

static void emit_uint(sink_t *s, unsigned long v, int base, int upper) {
    char tmp[32]; int i = 0;
    const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = d[v % base]; v /= base; }
    while (i) s_put(s, tmp[--i]);
}
static void emit_int(sink_t *s, long v) {
    if (v < 0) { s_put(s, '-'); emit_uint(s, (unsigned long)(-v), 10, 0); }
    else emit_uint(s, (unsigned long)v, 10, 0);
}
static void emit_float(sink_t *s, double v, int prec) {
    if (v != v) { s_str(s, "nan", 3); return; }
    if (prec < 0) prec = 6;
    if (v < 0) { s_put(s, '-'); v = -v; }
    if (v > 1.7e308) { s_str(s, "inf", 3); return; }
    /* round at the requested precision */
    double scale = 1.0; for (int i = 0; i < prec; i++) scale *= 10.0;
    double r = v * scale + 0.5;
    unsigned long long scaled = (unsigned long long)r;
    unsigned long long ip = scaled / (unsigned long long)scale;
    unsigned long long fp = scaled % (unsigned long long)scale;
    emit_uint(s, (unsigned long)ip, 10, 0);
    if (prec > 0) {
        s_put(s, '.');
        /* leading zeros of the fractional part */
        double t = scale / 10.0;
        for (int i = 0; i < prec; i++) { if (fp < (unsigned long long)t && t >= 1.0) s_put(s, '0'); t /= 10.0; }
        if (fp) emit_uint(s, (unsigned long)fp, 10, 0);
    }
}

static int fmt_core(sink_t *s, const char *fmt, va_list ap) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { s_put(s, *p); continue; }
        p++;
        int width = 0, prec = -1, zero = 0, left = 0, lng = 0;
        while (*p == '-' || *p == '0') { if (*p == '0') zero = 1; else left = 1; p++; }
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        if (*p == '.') { p++; prec = 0; while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0'); }
        while (*p == 'l') { lng++; p++; }
        if (*p == 'z') { lng = 1; p++; }
        /* render the conversion into a scratch sink to know its width */
        char scratch[64]; sink_t sc = { scratch, sizeof scratch, 0, 0, {0}, 0 };
        sink_t *o = (width > 0) ? &sc : s;
        switch (*p) {
        case 'd': case 'i': emit_int(o, lng ? va_arg(ap, long) : va_arg(ap, int)); break;
        case 'u': emit_uint(o, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 10, 0); break;
        case 'x': emit_uint(o, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 16, 0); break;
        case 'X': emit_uint(o, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 16, 1); break;
        case 'o': emit_uint(o, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 8, 0); break;
        case 'p': s_str(o, "0x", 2); emit_uint(o, (unsigned long)va_arg(ap, void *), 16, 0); break;
        case 'f': case 'F': case 'g': emit_float(o, va_arg(ap, double), prec); break;
        case 'c': s_put(o, (char)va_arg(ap, int)); break;
        case 's': { const char *a = va_arg(ap, const char *); if (!a) a = "(null)";
                    unsigned long n = strlen(a); if (prec >= 0 && (unsigned long)prec < n) n = prec;
                    s_str(o, a, n); break; }
        case '%': s_put(o, '%'); break;
        default:  s_put(o, '%'); if (*p) s_put(o, *p); break;
        }
        if (width > 0) {                                /* apply field width padding */
            int pad = width - (int)sc.len;
            char fill = (zero && !left) ? '0' : ' ';
            if (!left) while (pad-- > 0) s_put(s, fill);
            s_str(s, scratch, sc.len);
            if (left)  while (pad-- > 0) s_put(s, ' ');
        }
    }
    return (int)s->len;
}

int vsnprintf(char *buf, unsigned long cap, const char *fmt, va_list ap) {
    sink_t s = { buf, cap, 0, 0, {0}, 0 };
    int n = fmt_core(&s, fmt, ap);
    if (cap) buf[(s.len < cap) ? s.len : cap - 1] = 0;
    return n;
}
int snprintf(char *buf, unsigned long cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}
int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, 0x7fffffff, fmt, ap);
    va_end(ap);
    return n;
}
int vprintf(const char *fmt, va_list ap) {
    sink_t s = { 0, 0, 0, 1, {0}, 0 };
    int n = fmt_core(&s, fmt, ap);
    s_flush(&s);
    return n;
}
int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}
int fprintf(int fd, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    sink_t s = { 0, 0, 0, fd, {0}, 0 };
    int n = fmt_core(&s, fmt, ap);
    s_flush(&s);
    va_end(ap);
    return n;
}
int putchar(int c) { char ch = (char)c; sys_write(1, &ch, 1); return c; }
int puts(const char *s) { sys_write(1, s, strlen(s)); putchar('\n'); return 0; }
