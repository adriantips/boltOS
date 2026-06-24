/* ===========================================================================
 *  BoltOS userland: <stdlib.h> extras + a thin buffered <stdio.h> FILE layer
 *  over the raw fd syscalls in ulibc.c.
 * ========================================================================= */
#include "ulibc.h"
#include "stdio.h"
#include "string.h"

/* ----------------------------------------------------------- stdlib -------- */
long strtol(const char *s, char **end, int base) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    int neg = 0;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0 && s[0] == '0') { base = 8; }
    else if (base == 0) base = 10;
    long v = 0;
    for (;;) {
        int c = *s, d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d; s++;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtol(s, end, base); }

double strtod(const char *s, char **end) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    int neg = 0;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    double v = 0.0;
    while (*s >= '0' && *s <= '9') v = v * 10.0 + (*s++ - '0');
    if (*s == '.') {
        s++; double f = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s++ - '0') * f; f *= 0.1; }
    }
    if (*s == 'e' || *s == 'E') {
        s++; int eneg = 0, e = 0;
        if (*s == '+' || *s == '-') { eneg = (*s == '-'); s++; }
        while (*s >= '0' && *s <= '9') e = e * 10 + (*s++ - '0');
        double p = 1.0; while (e--) p *= 10.0;
        v = eneg ? v / p : v * p;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
double atof(const char *s) { return strtod(s, 0); }
long   atol(const char *s) { return strtol(s, 0, 10); }
int    abs(int v)  { return v < 0 ? -v : v; }
long   labs(long v){ return v < 0 ? -v : v; }

static unsigned long rng = 0x2545F4914F6CDD1DUL;
void srand(unsigned s) { rng = s ? s : 1; }
int  rand(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (int)(rng & 0x7fffffff); }

void qsort(void *base, unsigned long n, unsigned long sz, int (*cmp)(const void *, const void *)) {
    /* insertion sort: simple, stable enough for the small arrays apps pass. */
    char *a = base, tmp[256];
    if (sz > sizeof tmp) return;
    for (unsigned long i = 1; i < n; i++) {
        memcpy(tmp, a + i * sz, sz);
        long j = (long)i - 1;
        while (j >= 0 && cmp(a + j * sz, tmp) > 0) { memcpy(a + (j + 1) * sz, a + j * sz, sz); j--; }
        memcpy(a + (j + 1) * sz, tmp, sz);
    }
}
void *bsearch(const void *key, const void *base, unsigned long n, unsigned long sz,
              int (*cmp)(const void *, const void *)) {
    const char *a = base;
    unsigned long lo = 0, hi = n;
    while (lo < hi) {
        unsigned long mid = (lo + hi) / 2;
        int c = cmp(key, a + mid * sz);
        if (c == 0) return (void *)(a + mid * sz);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}
void  abort(void) { exit(134); }
char *getenv(const char *name) { (void)name; return 0; }   /* no environment yet */

/* ----------------------------------------------------------- FILE ---------- */
static FILE _files[16];
static FILE _std[3] = { {0,0,0,0}, {1,0,0,0}, {2,0,0,0} };
FILE *stdin  = &_std[0];
FILE *stdout = &_std[1];
FILE *stderr = &_std[2];

FILE *fopen(const char *path, const char *mode) {
    int flags = O_RDONLY;
    if (mode[0] == 'w') flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mode[0] == 'a') flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (mode[0] == 'r' && mode[1] == '+') flags = O_RDWR;
    int fd = open(path, flags);
    if (fd < 0) return 0;
    for (int i = 0; i < 16; i++) if (!_files[i].used) {
        _files[i] = (FILE){ fd, 0, 1, 0 };
        return &_files[i];
    }
    close(fd);
    return 0;
}
int fclose(FILE *f) { if (!f) return -1; int fd = f->fd; if (f->used) { f->used = 0; } return close(fd); }
unsigned long fread(void *p, unsigned long sz, unsigned long n, FILE *f) {
    long got = read(f->fd, p, sz * n); return got <= 0 ? 0 : (unsigned long)got / sz;
}
unsigned long fwrite(const void *p, unsigned long sz, unsigned long n, FILE *f) {
    long put = write(f->fd, p, sz * n); return put <= 0 ? 0 : (unsigned long)put / sz;
}
int fputc(int c, FILE *f) { char ch = (char)c; return write(f->fd, &ch, 1) == 1 ? c : -1; }
int fputs(const char *s, FILE *f) { unsigned long n = strlen(s); return write(f->fd, s, n) == (long)n ? 0 : -1; }
int fgetc(FILE *f) { unsigned char c; long r = read(f->fd, &c, 1); if (r <= 0) { f->eof = 1; return -1; } return c; }
char *fgets(char *buf, int cap, FILE *f) {
    int i = 0;
    while (i < cap - 1) { int c = fgetc(f); if (c < 0) break; buf[i++] = (char)c; if (c == '\n') break; }
    if (i == 0) return 0;
    buf[i] = 0;
    return buf;
}
int   fflush(FILE *f) { (void)f; return 0; }
int   feof(FILE *f)   { return f->eof; }
long  ftell(FILE *f)  { return lseek(f->fd, 0, SEEK_CUR); }
int   fseek(FILE *f, long off, int whence) { return lseek(f->fd, off, whence) < 0 ? -1 : 0; }
