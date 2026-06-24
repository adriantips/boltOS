/* ===========================================================================
 *  dg_libc.c  -  a tiny freestanding C library for the BoltOS DOOM port.
 *
 *  DOOM was written against a hosted libc; BoltOS's kernel is freestanding.
 *  This file supplies the slice of <stdio.h>/<stdlib.h>/<string.h>/<math.h>
 *  the engine actually touches, on top of the kernel's heap and serial log.
 *
 *  Files live in RAM: the shareware IWAD is registered as a read-only blob and
 *  any file DOOM opens for writing (savegames, default.cfg) becomes a growable
 *  in-memory file. No real disk is involved.
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "kheap.h"
#include "kprintf.h"

int errno = 0;

/* fatal-exit hook: longjmp back into the app (defined in doomgeneric_boltos.c) */
void dg_panic(void) __attribute__((noreturn));

/* ---------------------------------------------------------------------------
 *  raw serial text output (DOOM's stdout/stderr -> COM1 via the kernel)
 * ------------------------------------------------------------------------- */
static void sputs(const char *s) { while (*s) kputc(*s++); }
static void sputn(const char *s, int n) { while (n-- > 0) kputc(*s++); }

/* ===========================================================================
 *  In-RAM file table
 * ===========================================================================*/
typedef struct {
    char     name[64];
    uint8_t *data;
    uint32_t size;
    uint32_t cap;
    int      writable;     /* 1 = growable scratch file, 0 = const blob */
    int      used;
} memfile_t;

#define DG_MAXFILES 24
static memfile_t g_files[DG_MAXFILES];

struct _DG_FILE {
    int      kind;         /* 0 = memfile, 1 = stdout, 2 = stderr, 3 = stdin */
    int      idx;
    uint32_t pos;
    int      eof, err;
    int      writable;
};

static FILE g_stdin  = { 3, -1, 0, 0, 0, 0 };
static FILE g_stdout = { 1, -1, 0, 0, 0, 0 };
static FILE g_stderr = { 2, -1, 0, 0, 0, 0 };
FILE *stdin  = &g_stdin;
FILE *stdout = &g_stdout;
FILE *stderr = &g_stderr;

static const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *s = p; *s; s++) if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

static int ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static memfile_t *find_file(const char *path) {
    const char *bn = basename_of(path);
    for (int i = 0; i < DG_MAXFILES; i++)
        if (g_files[i].used && ci_eq(g_files[i].name, bn)) return &g_files[i];
    return NULL;
}

static memfile_t *new_file(const char *path) {
    const char *bn = basename_of(path);
    for (int i = 0; i < DG_MAXFILES; i++) {
        if (!g_files[i].used) {
            memfile_t *m = &g_files[i];
            int j = 0; while (bn[j] && j < 63) { m->name[j] = bn[j]; j++; } m->name[j] = 0;
            m->data = NULL; m->size = 0; m->cap = 0; m->writable = 1; m->used = 1;
            return m;
        }
    }
    return NULL;
}

/* register a read-only blob (the embedded IWAD) under a name */
void dg_fs_register(const char *name, const uint8_t *data, uint32_t size) {
    memfile_t *m = find_file(name);
    if (!m) {
        for (int i = 0; i < DG_MAXFILES; i++) if (!g_files[i].used) { m = &g_files[i]; break; }
    }
    if (!m) return;
    int j = 0; while (name[j] && j < 63) { m->name[j] = name[j]; j++; } m->name[j] = 0;
    m->data = (uint8_t *)data; m->size = size; m->cap = size; m->writable = 0; m->used = 1;
}

static int memfile_ensure(memfile_t *m, uint32_t need) {
    if (need <= m->cap) return 1;
    uint32_t ncap = m->cap ? m->cap : 4096;
    while (ncap < need) ncap *= 2;
    uint8_t *nd = (uint8_t *)kmalloc(ncap);
    if (!nd) return 0;
    if (m->data && m->size) memcpy(nd, m->data, m->size);
    if (m->data && m->writable) kfree(m->data);
    m->data = nd; m->cap = ncap;
    return 1;
}

FILE *fopen(const char *path, const char *mode) {
    int wr = 0, ap = 0;
    for (const char *p = mode; *p; p++) { if (*p == 'w') wr = 1; else if (*p == 'a') ap = 1; }

    memfile_t *m = find_file(path);
    if (wr) {
        if (!m) m = new_file(path);
        if (!m) { errno = ENOMEM; return NULL; }
        if (!m->writable) { errno = EACCES; return NULL; }
        m->size = 0;                          /* truncate */
    } else if (ap) {
        if (!m) m = new_file(path);
        if (!m) { errno = ENOMEM; return NULL; }
    } else {                                  /* read */
        if (!m) { errno = ENOENT; return NULL; }
    }

    FILE *f = (FILE *)kmalloc(sizeof(FILE));
    if (!f) { errno = ENOMEM; return NULL; }
    f->kind = 0; f->idx = (int)(m - g_files);
    f->pos = ap ? m->size : 0;
    f->eof = 0; f->err = 0; f->writable = (wr || ap);
    return f;
}

int fclose(FILE *f) { if (f && f->kind == 0) kfree(f); return 0; }

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || f->kind != 0 || size == 0 || nmemb == 0) return 0;
    memfile_t *m = &g_files[f->idx];
    uint64_t want = (uint64_t)size * nmemb;
    uint64_t avail = m->size > f->pos ? (uint64_t)(m->size - f->pos) : 0;
    if (want > avail) want = avail;
    if (want) memcpy(ptr, m->data + f->pos, (size_t)want);
    f->pos += (uint32_t)want;
    if (f->pos >= m->size) f->eof = 1;
    return (size_t)(want / size);
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || size == 0 || nmemb == 0) return 0;
    uint64_t want = (uint64_t)size * nmemb;
    if (f->kind == 1 || f->kind == 2) { sputn((const char *)ptr, (int)want); return nmemb; }
    if (f->kind != 0 || !f->writable) return 0;
    memfile_t *m = &g_files[f->idx];
    if (!memfile_ensure(m, f->pos + (uint32_t)want)) return 0;
    memcpy(m->data + f->pos, ptr, (size_t)want);
    f->pos += (uint32_t)want;
    if (f->pos > m->size) m->size = f->pos;
    return nmemb;
}

int fseek(FILE *f, long off, int whence) {
    if (!f || f->kind != 0) return -1;
    memfile_t *m = &g_files[f->idx];
    long base = (whence == SEEK_CUR) ? (long)f->pos : (whence == SEEK_END) ? (long)m->size : 0;
    long np = base + off;
    if (np < 0) np = 0;
    f->pos = (uint32_t)np; f->eof = 0;
    return 0;
}

long ftell(FILE *f) { return (f && f->kind == 0) ? (long)f->pos : -1; }
void rewind(FILE *f) { if (f) { f->pos = 0; f->eof = 0; } }
int  fflush(FILE *f) { (void)f; return 0; }
int  feof(FILE *f)   { return f ? f->eof : 1; }
int  ferror(FILE *f) { return f ? f->err : 0; }
int  fileno(FILE *f) { return f ? f->kind : -1; }

int fgetc(FILE *f) {
    unsigned char c;
    if (fread(&c, 1, 1, f) != 1) return EOF;
    return c;
}
char *fgets(char *s, int size, FILE *f) {
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = 0;
    return s;
}
int fputc(int c, FILE *f) {
    char ch = (char)c;
    if (f && (f->kind == 1 || f->kind == 2)) { kputc(ch); return c; }
    return fwrite(&ch, 1, 1, f) == 1 ? c : EOF;
}
int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n ? (int)n : EOF;
}

int remove(const char *path)  { memfile_t *m = find_file(path); if (m && m->writable) { if (m->data) kfree(m->data); m->used = 0; m->data = NULL; m->size = m->cap = 0; } return 0; }
int rename(const char *o, const char *n) {
    memfile_t *m = find_file(o); if (!m) return -1;
    const char *bn = basename_of(n); int j = 0; while (bn[j] && j < 63) { m->name[j] = bn[j]; j++; } m->name[j] = 0;
    return 0;
}

/* ===========================================================================
 *  printf-family formatting
 * ===========================================================================*/
typedef struct { char *buf; size_t cap; size_t len; int to_serial; } fmtsink;

static void emit(fmtsink *s, char c) {
    if (s->to_serial) { kputc(c); s->len++; return; }
    if (s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len++;
}
static void emit_str(fmtsink *s, const char *p, int n) { while (n-- > 0) emit(s, *p++); }

static int u64_to_str(char *out, uint64_t v, int base, int upper) {
    static const char *lo = "0123456789abcdef", *hi = "0123456789ABCDEF";
    const char *d = upper ? hi : lo;
    char tmp[32]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = d[v % base]; v /= base; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

static void fmt_double(fmtsink *s, double v, int prec, int width, int left, int zero, int plus, int space) {
    char tmp[64]; int n = 0;
    if (v != v) { emit_str(s, "nan", 3); return; }
    int neg = 0;
    union { double d; uint64_t u; } bz; bz.d = v;
    if (bz.u >> 63) { neg = 1; v = -v; }
    if (prec < 0) prec = 6;
    /* rounding */
    double scale = 1.0; for (int i = 0; i < prec; i++) scale *= 10.0;
    double rounded = v * scale + 0.5;
    uint64_t scaled = (uint64_t)rounded;
    uint64_t ip = prec ? scaled / (uint64_t)scale : scaled;
    uint64_t fp = prec ? scaled % (uint64_t)scale : 0;
    char ib[32]; int in = u64_to_str(ib, ip, 10, 0);
    char sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
    if (sign) tmp[n++] = sign;
    for (int i = 0; i < in; i++) tmp[n++] = ib[i];
    if (prec) {
        tmp[n++] = '.';
        char fb[32]; int fn = u64_to_str(fb, fp, 10, 0);
        for (int i = 0; i < prec - fn; i++) tmp[n++] = '0';
        for (int i = 0; i < fn; i++) tmp[n++] = fb[i];
    }
    int pad = width - n;
    if (!left) for (int i = 0; i < pad; i++) emit(s, zero ? '0' : ' ');
    emit_str(s, tmp, n);
    if (left) for (int i = 0; i < pad; i++) emit(s, ' ');
}

static int do_format(fmtsink *s, const char *fmt, va_list ap) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { emit(s, *p); continue; }
        p++;
        if (*p == '%') { emit(s, '%'); continue; }
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; p++) {
            if (*p == '-') left = 1; else if (*p == '0') zero = 1;
            else if (*p == '+') plus = 1; else if (*p == ' ') space = 1;
            else if (*p == '#') alt = 1; else break;
        }
        int width = 0;
        if (*p == '*') { width = va_arg(ap, int); p++; if (width < 0) { left = 1; width = -width; } }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int prec = -1;
        if (*p == '.') { p++; prec = 0; if (*p == '*') { prec = va_arg(ap, int); p++; }
                         else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0'); }
        int lng = 0;
        while (*p == 'l') { lng++; p++; }
        while (*p == 'h' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'L') p++;

        char c = *p;
        char numbuf[40]; int nn;
        switch (c) {
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
            int neg = v < 0; uint64_t uv = neg ? (uint64_t)(-v) : (uint64_t)v;
            nn = u64_to_str(numbuf, uv, 10, 0);
            if (prec == 0 && uv == 0) nn = 0;                       /* %.0d of 0 -> "" */
            int pz = (prec > nn) ? prec - nn : 0;                   /* precision zeros */
            int zpad = (zero && prec < 0);                         /* '0' flag void if prec given */
            char sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
            int pad = width - (nn + pz + (sign ? 1 : 0));
            if (!left && !zpad) for (int i = 0; i < pad; i++) emit(s, ' ');
            if (sign) emit(s, sign);
            if (!left && zpad) for (int i = 0; i < pad; i++) emit(s, '0');
            for (int i = 0; i < pz; i++) emit(s, '0');
            emit_str(s, numbuf, nn);
            if (left) for (int i = 0; i < pad; i++) emit(s, ' ');
            break; }
        case 'u': case 'x': case 'X': case 'o': case 'p': {
            int base = (c == 'o') ? 8 : (c == 'u') ? 10 : 16;
            int upper = (c == 'X');
            uint64_t v;
            if (c == 'p') { v = (uint64_t)(uintptr_t)va_arg(ap, void *); }
            else v = lng ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
            nn = u64_to_str(numbuf, v, base, upper);
            if (prec == 0 && v == 0) nn = 0;
            int pz = (prec > nn) ? prec - nn : 0;
            int zpad = (zero && prec < 0);
            int pfx = (alt && (c == 'x' || c == 'X') && v) ? 2 : (c == 'p') ? 2 : 0;
            int pad = width - (nn + pz + pfx);
            if (!left && !zpad) for (int i = 0; i < pad; i++) emit(s, ' ');
            if (pfx) { emit(s, '0'); emit(s, upper ? 'X' : 'x'); }
            if (!left && zpad) for (int i = 0; i < pad; i++) emit(s, '0');
            for (int i = 0; i < pz; i++) emit(s, '0');
            emit_str(s, numbuf, nn);
            if (left) for (int i = 0; i < pad; i++) emit(s, ' ');
            break; }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            int pad = width - 1;
            if (!left) for (int i = 0; i < pad; i++) emit(s, ' ');
            emit(s, ch);
            if (left) for (int i = 0; i < pad; i++) emit(s, ' ');
            break; }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int len = 0; while (str[len] && (prec < 0 || len < prec)) len++;
            int pad = width - len;
            if (!left) for (int i = 0; i < pad; i++) emit(s, ' ');
            emit_str(s, str, len);
            if (left) for (int i = 0; i < pad; i++) emit(s, ' ');
            break; }
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': {
            double v = va_arg(ap, double);
            fmt_double(s, v, prec, width, left, zero, plus, space);
            break; }
        case 0: return (int)s->len;
        default: emit(s, '%'); emit(s, c); break;
        }
    }
    return (int)s->len;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    fmtsink s = { buf, n, 0, 0 };
    int r = do_format(&s, fmt, ap);
    if (n) buf[s.len < n ? s.len : n - 1] = 0;
    return r;
}
int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, (size_t)-1, fmt, ap); }
int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, n, fmt, ap); va_end(ap); return r;
}
int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, (size_t)-1, fmt, ap); va_end(ap); return r;
}
int vfprintf(FILE *f, const char *fmt, va_list ap) {
    (void)f; fmtsink s = { 0, 0, 0, 1 }; return do_format(&s, fmt, ap);
}
int fprintf(FILE *f, const char *fmt, ...) {
    (void)f; va_list ap; va_start(ap, fmt);
    fmtsink s = { 0, 0, 0, 1 }; int r = do_format(&s, fmt, ap); va_end(ap); return r;
}
int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fmtsink s = { 0, 0, 0, 1 }; int r = do_format(&s, fmt, ap); va_end(ap); return r;
}
int vprintf(const char *fmt, va_list ap) { fmtsink s = { 0, 0, 0, 1 }; return do_format(&s, fmt, ap); }
int puts(const char *s) { sputs(s); kputc('\n'); return 0; }
int putchar(int c) { kputc((char)c); return c; }

/* ===========================================================================
 *  sscanf - enough for DOOM's config/number parsing
 * ===========================================================================*/
static int dval(int c, int base) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return -1;
    return v < base ? v : -1;
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    const char *s = str; int assigned = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n') { while (*s == ' ' || *s == '\t' || *s == '\n') s++; continue; }
        if (*p != '%') { if (*s == *p) s++; else break; continue; }
        p++;
        int suppress = 0; if (*p == '*') { suppress = 1; p++; }
        int width = 0; while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        if (width == 0) width = 1 << 30;
        while (*p == 'l' || *p == 'h' || *p == 'z') p++;
        char c = *p;
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'o') {
            int base = (c == 'x' || c == 'X') ? 16 : (c == 'o') ? 8 : 10;
            int neg = 0;
            if ((c == 'd' || c == 'i') && (*s == '-' || *s == '+')) { neg = (*s == '-'); s++; }
            if (c == 'i') { if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; } }
            long val = 0; int got = 0;
            while (width-- > 0) { int dv = dval(*s, base); if (dv < 0) break; val = val * base + dv; s++; got = 1; }
            if (!got) break;
            if (neg) val = -val;
            if (!suppress) { *va_arg(ap, int *) = (int)val; assigned++; }
        } else if (c == 's') {
            char *out = suppress ? NULL : va_arg(ap, char *);
            int got = 0;
            while (width-- > 0 && *s && *s != ' ' && *s != '\t' && *s != '\n') { if (out) *out++ = *s; s++; got = 1; }
            if (out) *out = 0;
            if (!got) break;
            if (!suppress) assigned++;
        } else if (c == 'c') {
            if (!*s) break;
            if (!suppress) { *va_arg(ap, char *) = *s; assigned++; }
            s++;
        } else if (c == 'f' || c == 'g' || c == 'e') {
            char *end; double d = strtod(s, &end);
            if (end == s) break;
            s = end;
            if (!suppress) { *va_arg(ap, float *) = (float)d; assigned++; }
        } else break;
    }
    va_end(ap);
    return assigned;
}

/* ===========================================================================
 *  stdlib: heap, conversions, misc
 * ===========================================================================*/
void *malloc(size_t n) {
    uint64_t *p = (uint64_t *)kmalloc(n + 16);
    if (!p) return NULL;
    p[0] = n;
    return (void *)((uint8_t *)p + 16);
}
void free(void *q) { if (q) kfree((uint8_t *)q - 16); }
void *calloc(size_t a, size_t b) {
    size_t n = a * b; void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
void *realloc(void *q, size_t n) {
    if (!q) return malloc(n);
    if (n == 0) { free(q); return NULL; }
    uint64_t old = ((uint64_t *)((uint8_t *)q - 16))[0];
    void *nw = malloc(n);
    if (!nw) return NULL;
    memcpy(nw, q, old < n ? old : n);
    free(q);
    return nw;
}

long strtol(const char *s, char **end, int base) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0; if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    else if (base == 0 && s[0] == '0') { base = 8; }
    else if (base == 0) base = 10;
    long v = 0; int got = 0;
    for (;;) { int d = dval(*s, base); if (d < 0) break; v = v * base + d; s++; got = 1; }
    (void)got;
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtol(s, end, base); }
long atol(const char *s) { return strtol(s, NULL, 10); }

double strtod(const char *s, char **end) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0; if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    double v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    if (*s == '.') {
        s++; double f = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s - '0') * f; f *= 0.1; s++; }
    }
    if (*s == 'e' || *s == 'E') {
        s++; int eneg = 0; if (*s == '-') { eneg = 1; s++; } else if (*s == '+') s++;
        int e = 0; while (*s >= '0' && *s <= '9') { e = e * 10 + (*s - '0'); s++; }
        double p = 1; while (e--) p *= 10;
        if (eneg) v /= p; else v *= p;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
double atof(const char *s) { return strtod(s, NULL); }

int  abs(int x)   { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

static unsigned long g_rng = 1;
int  rand(void) { g_rng = g_rng * 1103515245u + 12345u; return (int)((g_rng >> 16) & 0x7fffffff); }
void srand(unsigned s) { g_rng = s; }

void exit(int code)  { (void)code; dg_panic(); }
void abort(void)     { dg_panic(); }
char *getenv(const char *n) { (void)n; return NULL; }
int  system(const char *c)  { (void)c; return 0; }

void qsort(void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *)) {
    /* simple insertion sort - DOOM's qsort inputs are tiny */
    char *a = (char *)base, tmp[256];
    if (size > sizeof(tmp)) return;
    for (size_t i = 1; i < nmemb; i++) {
        memcpy(tmp, a + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(a + (j - 1) * size, tmp) > 0) { memcpy(a + j * size, a + (j - 1) * size, size); j--; }
        memcpy(a + j * size, tmp, size);
    }
}

/* ===========================================================================
 *  POSIX-ish stubs over the RAM file table
 * ===========================================================================*/
int access(const char *path, int mode) { (void)mode; return find_file(path) ? 0 : -1; }
int unlink(const char *path) { return remove(path); }
int mkdir(const char *path, mode_t m) { (void)path; (void)m; return 0; }
int isatty(int fd) { return fd <= 2; }
int usleep(unsigned us) { (void)us; return 0; }
unsigned sleep(unsigned s) { (void)s; return 0; }
char *getcwd(char *buf, size_t size) { if (buf && size) buf[0] = 0; return buf; }
int close(int fd) { (void)fd; return 0; }
ssize_t read(int fd, void *b, size_t n) { (void)fd; (void)b; (void)n; return 0; }
ssize_t write(int fd, const void *b, size_t n) { if (fd == 1 || fd == 2) { sputn((const char *)b, (int)n); return (ssize_t)n; } return -1; }
long lseek(int fd, long off, int whence) { (void)fd; (void)off; (void)whence; return -1; }
int open(const char *path, int flags, ...) { (void)flags; return find_file(path) ? 100 : -1; }

int stat(const char *path, struct stat *st) {
    memfile_t *m = find_file(path);
    if (!m) return -1;
    if (st) { st->st_mode = S_IFREG; st->st_size = m->size; st->st_dev = 0; st->st_ino = 0; }
    return 0;
}
int fstat(int fd, struct stat *st) { (void)fd; if (st) { st->st_mode = S_IFREG; st->st_size = 0; } return 0; }

/* ===========================================================================
 *  string extras the kernel libc doesn't provide
 * ===========================================================================*/
void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; }
    return NULL;
}
char *strncat(char *d, const char *s, size_t n) {
    char *e = d + strlen(d);
    while (n-- && *s) *e++ = *s++;
    *e = 0; return d;
}
/* Standard strncpy: copy up to n bytes, NUL-pad the remainder, and (unlike the
 * kernel's strlcpy-style strncpy) do NOT force a terminator when src fills n. */
char *dg_strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)hay;
    }
    return NULL;
}
char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
int strcasecmp(const char *a, const char *b) {
    for (;;) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
        a++; b++;
    }
}
int strncasecmp(const char *a, const char *b, size_t n) {
    while (n--) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
        a++; b++;
    }
    return 0;
}
char *strerror(int e) { (void)e; return "error"; }

/* ===========================================================================
 *  math (SSE2 is enabled in the kernel, so doubles work natively)
 * ===========================================================================*/
double fabs(double x)  { union { double d; uint64_t u; } v; v.d = x; v.u &= ~(1ull << 63); return v.d; }
float  fabsf(float x)  { union { float f; uint32_t u; } v; v.f = x; v.u &= ~(1u << 31); return v.f; }
double sqrt(double x)  { return __builtin_sqrt(x); }
float  sqrtf(float x)  { return __builtin_sqrtf(x); }

double floor(double x) { double t = (double)(long long)x; if (t > x) t -= 1.0; return t; }
double ceil(double x)  { double t = (double)(long long)x; if (t < x) t += 1.0; return t; }

double sin(double x) {
    const double twopi = 6.283185307179586;
    double k = x / twopi;
    long long n = (long long)(k >= 0 ? k + 0.5 : k - 0.5);
    x -= (double)n * twopi;                 /* x now in [-pi, pi] */
    double x2 = x * x;
    return x * (1.0 + x2 * (-1.0/6 + x2 * (1.0/120 + x2 * (-1.0/5040 +
               x2 * (1.0/362880 - x2 * (1.0/39916800))))));
}
double cos(double x) { return sin(x + 1.5707963267948966); }
double tan(double x) { double c = cos(x); return c == 0 ? 0 : sin(x) / c; }

double atan(double x) {
    int inv = 0, neg = 0;
    double a = x;
    if (a < 0) { a = -a; neg = 1; }
    if (a > 1.0) { a = 1.0 / a; inv = 1; }
    double a2 = a * a;
    double r = a * (0.9998660 + a2 * (-0.3302995 + a2 * (0.1801410 +
               a2 * (-0.0851330 + a2 * 0.0208351))));
    if (inv) r = 1.5707963267948966 - r;
    return neg ? -r : r;
}
double atan2(double y, double x) {
    if (x > 0) return atan(y / x);
    if (x < 0) return atan(y / x) + (y >= 0 ? M_PI : -M_PI);
    if (y > 0) return  1.5707963267948966;
    if (y < 0) return -1.5707963267948966;
    return 0;
}
static double dg_ldexp(double m, int k) {
    union { double d; uint64_t u; } v; v.d = m;
    int e = (int)((v.u >> 52) & 0x7ff) + k;
    if (e <= 0) return 0.0;
    if (e >= 2047) return m < 0 ? -1e308 : 1e308;
    v.u = (v.u & ~(0x7ffull << 52)) | ((uint64_t)e << 52);
    return v.d;
}
double exp(double x) {
    const double ln2 = 0.6931471805599453;
    int k = (int)(x / ln2 + (x >= 0 ? 0.5 : -0.5));
    double r = x - k * ln2;
    double term = 1, sum = 1;
    for (int i = 1; i < 14; i++) { term *= r / i; sum += term; }
    return dg_ldexp(sum, k);
}
double log(double x) {
    if (x <= 0) return -1e308;
    union { double d; uint64_t u; } v; v.d = x;
    int e = (int)((v.u >> 52) & 0x7ff) - 1023;
    v.u = (v.u & ~(0x7ffull << 52)) | (1023ull << 52);   /* mantissa in [1,2) */
    double m = v.d;
    double t = (m - 1) / (m + 1), t2 = t * t, s = 0, tp = t;
    for (int i = 1; i <= 13; i += 2) { s += tp / i; tp *= t2; }
    return e * 0.6931471805599453 + 2 * s;
}
double pow(double b, double e) {
    if (b == 0) return e == 0 ? 1 : 0;
    if (b < 0) { long long ei = (long long)e; double r = exp(ei * log(-b)); return (ei & 1) ? -r : r; }
    return exp(e * log(b));
}
double fmod(double a, double b) {
    if (b == 0) return 0;
    double q = a / b;
    long long qi = (long long)q;
    return a - (double)qi * b;
}
