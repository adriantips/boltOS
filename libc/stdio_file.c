/* ===========================================================================
 *  libc/stdio_file.c -- FILE streams for BoltOS's freestanding libc.
 *
 *  Three stream flavours behind one FILE:
 *    - FD     : a kernel vfs file* (fopen of a real path)
 *    - MEMR   : a fixed read-only byte buffer (fmemopen)
 *    - MEMW   : a growable heap buffer that publishes to *bufp/*sizep
 *               (open_memstream) -- libcss/libdom use these to assemble output
 *    - CONS   : stdout/stderr, writes funnel to the kernel console (kputc)
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"
#include "vfs.h"
#include "kprintf.h"

enum { K_FD, K_MEMR, K_MEMW, K_CONS };

struct __FILE {
    int     kind;
    file   *vf;                 /* K_FD                                        */
    uint8_t *mem; size_t mlen, mcap, mpos;  /* K_MEMR/K_MEMW                   */
    char  **ubuf; size_t *usize;            /* K_MEMW publish targets          */
    int     err, eof, can_read, can_write, append;
    int     ungot;              /* pushed-back char, or -1                     */
};

static FILE f_stdin  = { K_CONS, 0,0,0,0,0, 0,0, 0,0, 0,0,1,0, -1 };
static FILE f_stdout = { K_CONS, 0,0,0,0,0, 0,0, 0,0, 0,0,0,1, -1 };
static FILE f_stderr = { K_CONS, 0,0,0,0,0, 0,0, 0,0, 0,0,0,1, -1 };
FILE *stdin  = &f_stdin;
FILE *stdout = &f_stdout;
FILE *stderr = &f_stderr;

/* ---- mode string -> capabilities + vfs flags ---- */
static int parse_mode(const char *m, int *rd, int *wr, int *ap, int *vflags) {
    *rd = *wr = *ap = 0;
    switch (m[0]) {
    case 'r': *rd = 1; *vflags = O_RDONLY; break;
    case 'w': *wr = 1; *vflags = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': *wr = 1; *ap = 1; *vflags = O_WRONLY | O_CREAT | O_APPEND; break;
    default: return -1;
    }
    for (const char *p = m + 1; *p; p++)
        if (*p == '+') { *rd = *wr = 1; *vflags = (*vflags & ~(O_RDONLY|O_WRONLY)) | O_RDWR; }
    return 0;
}

FILE *fopen(const char *path, const char *mode) {
    int rd, wr, ap, vf;
    if (parse_mode(mode, &rd, &wr, &ap, &vf) < 0) { errno = EINVAL; return 0; }
    file *h = vfs_open(path, vf);
    if (!h) { errno = ENOENT; return 0; }
    FILE *f = calloc(1, sizeof *f);
    if (!f) { vfs_close(h); errno = ENOMEM; return 0; }
    f->kind = K_FD; f->vf = h; f->can_read = rd; f->can_write = wr; f->append = ap; f->ungot = -1;
    return f;
}
FILE *freopen(const char *path, const char *mode, FILE *f) {
    if (f && f->kind == K_FD && f->vf) vfs_close(f->vf);
    FILE *n = fopen(path, mode);
    if (!n) return 0;
    if (f) { *f = *n; free(n); return f; }
    return n;
}
FILE *fdopen(int fd, const char *mode) { (void)fd; (void)mode; errno = ENOSYS; return 0; }

FILE *fmemopen(void *buf, size_t size, const char *mode) {
    int rd, wr, ap, vf;
    if (parse_mode(mode, &rd, &wr, &ap, &vf) < 0) { errno = EINVAL; return 0; }
    FILE *f = calloc(1, sizeof *f);
    if (!f) return 0;
    f->kind = K_MEMR; f->mem = buf; f->mlen = size; f->mcap = size;
    f->can_read = rd; f->can_write = wr; f->ungot = -1;
    return f;
}
FILE *open_memstream(char **bufp, size_t *sizep) {
    FILE *f = calloc(1, sizeof *f);
    if (!f) return 0;
    f->kind = K_MEMW; f->mcap = 128; f->mem = malloc(f->mcap);
    if (!f->mem) { free(f); return 0; }
    f->mem[0] = 0; f->mlen = 0; f->ubuf = bufp; f->usize = sizep;
    f->can_write = 1; f->ungot = -1;
    if (bufp) *bufp = (char *)f->mem; if (sizep) *sizep = 0;
    return f;
}

static int memw_grow(FILE *f, size_t need) {
    if (need <= f->mcap) return 0;
    size_t nc = f->mcap ? f->mcap : 128;
    while (nc < need) nc *= 2;
    uint8_t *nb = realloc(f->mem, nc);
    if (!nb) { f->err = 1; return -1; }
    f->mem = nb; f->mcap = nc;
    if (f->ubuf) *f->ubuf = (char *)f->mem;
    return 0;
}

size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f) {
    if (!f || !f->can_write) { if (f) f->err = 1; return 0; }
    size_t total = size * n;
    if (total == 0) return 0;
    const uint8_t *p = ptr;
    switch (f->kind) {
    case K_CONS: for (size_t i = 0; i < total; i++) kputc(p[i]); return n;
    case K_FD: { int64_t w = vfs_write(f->vf, ptr, total); if (w < 0) { f->err = 1; return 0; } return (size_t)w / size; }
    case K_MEMW:
        if (memw_grow(f, f->mpos + total + 1) < 0) return 0;
        memcpy(f->mem + f->mpos, ptr, total);
        f->mpos += total;
        if (f->mpos > f->mlen) { f->mlen = f->mpos; f->mem[f->mlen] = 0; if (f->usize) *f->usize = f->mlen; }
        return n;
    case K_MEMR: {
        if (!f->can_write) { f->err = 1; return 0; }
        size_t room = f->mcap - f->mpos, k = total < room ? total : room;
        memcpy(f->mem + f->mpos, ptr, k); f->mpos += k; if (f->mpos > f->mlen) f->mlen = f->mpos;
        return k / size;
    }
    }
    return 0;
}

size_t fread(void *ptr, size_t size, size_t n, FILE *f) {
    if (!f || !f->can_read) { if (f) f->err = 1; return 0; }
    size_t total = size * n;
    if (total == 0) return 0;
    switch (f->kind) {
    case K_FD: { int64_t r = vfs_read(f->vf, ptr, total); if (r <= 0) { f->eof = 1; return 0; } return (size_t)r / size; }
    case K_MEMR: case K_MEMW: {
        size_t avail = f->mlen - f->mpos, k = total < avail ? total : avail;
        if (k == 0) { f->eof = 1; return 0; }
        memcpy(ptr, f->mem + f->mpos, k); f->mpos += k;
        return k / size;
    }
    case K_CONS: f->eof = 1; return 0;
    }
    return 0;
}

int fseek(FILE *f, long off, int whence) {
    if (!f) return -1;
    f->eof = 0; f->ungot = -1;
    if (f->kind == K_FD) { int64_t r = vfs_lseek(f->vf, off, whence); return r < 0 ? -1 : 0; }
    if (f->kind == K_MEMR || f->kind == K_MEMW) {
        long base = whence == SEEK_CUR ? (long)f->mpos : whence == SEEK_END ? (long)f->mlen : 0;
        long np = base + off; if (np < 0) return -1;
        f->mpos = (size_t)np; return 0;
    }
    return -1;
}
long ftell(FILE *f) {
    if (!f) return -1;
    if (f->kind == K_FD) return (long)f->vf->pos;
    if (f->kind == K_MEMR || f->kind == K_MEMW) return (long)f->mpos;
    return -1;
}
void rewind(FILE *f) { fseek(f, 0, SEEK_SET); if (f) f->err = 0; }
int  fgetpos(FILE *f, fpos_t *pos) { long t = ftell(f); if (t < 0) return -1; *pos = t; return 0; }
int  fsetpos(FILE *f, const fpos_t *pos) { return fseek(f, (long)*pos, SEEK_SET); }

int fflush(FILE *f) { (void)f; return 0; }       /* writes are unbuffered      */
int fclose(FILE *f) {
    if (!f) return -1;
    if (f == stdin || f == stdout || f == stderr) return 0;
    if (f->kind == K_FD && f->vf) vfs_close(f->vf);
    if (f->kind == K_MEMW) {                       /* publish final buffer      */
        if (f->ubuf) *f->ubuf = (char *)f->mem;
        if (f->usize) *f->usize = f->mlen;
        /* memstream buffer is owned by the caller now; do not free f->mem */
    }
    free(f);
    return 0;
}

int feof(FILE *f)   { return f ? f->eof : 1; }
int ferror(FILE *f) { return f ? f->err : 1; }
void clearerr(FILE *f) { if (f) { f->err = 0; f->eof = 0; } }
int fileno(FILE *f) { return f == stdin ? 0 : f == stdout ? 1 : f == stderr ? 2 : 3; }
void setbuf(FILE *f, char *b) { (void)f; (void)b; }
int  setvbuf(FILE *f, char *b, int m, size_t s) { (void)f; (void)b; (void)m; (void)s; return 0; }

/* ---- character / line I/O ---- */
int fgetc(FILE *f) {
    if (!f) return EOF;
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    unsigned char c;
    if (fread(&c, 1, 1, f) != 1) return EOF;
    return c;
}
int getc(FILE *f)   { return fgetc(f); }
int getchar(void)   { return fgetc(stdin); }
int ungetc(int c, FILE *f) { if (!f || c == EOF) return EOF; f->ungot = (unsigned char)c; f->eof = 0; return c; }
char *fgets(char *s, int size, FILE *f) {
    if (size <= 0) return 0;
    int i = 0;
    while (i < size - 1) { int c = fgetc(f); if (c == EOF) break; s[i++] = (char)c; if (c == '\n') break; }
    if (i == 0) return 0;
    s[i] = 0; return s;
}
int fputc(int c, FILE *f) { unsigned char ch = (unsigned char)c; return fwrite(&ch, 1, 1, f) == 1 ? c : EOF; }
int putc(int c, FILE *f)  { return fputc(c, f); }
int putchar(int c)        { return fputc(c, stdout); }
int fputs(const char *s, FILE *f) { size_t n = strlen(s); return fwrite(s, 1, n, f) == n ? (int)n : EOF; }
int puts(const char *s)   { int r = fputs(s, stdout); if (r >= 0) putchar('\n'); return r; }

/* ---- formatted output to a stream ---- */
int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char *buf; int n = vasprintf(&buf, fmt, ap);
    if (n < 0) return -1;
    fwrite(buf, 1, (size_t)n, f);
    free(buf);
    return n;
}
int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r;
}
int dprintf(int fd, const char *fmt, ...) {
    FILE *f = fd == 2 ? stderr : stdout;
    va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r;
}

/* ---- misc filesystem ---- */
int remove(const char *path) { (void)path; errno = ENOSYS; return -1; }
int rename(const char *a, const char *b) { (void)a; (void)b; errno = ENOSYS; return -1; }
void perror(const char *s) { if (s && *s) kprintf("%s: ", s); kprintf("%s\n", strerror(errno)); }
FILE *tmpfile(void) { errno = ENOSYS; return 0; }
char *tmpnam(char *s) { static char b[L_tmpnam]; char *o = s ? s : b; strcpy(o, "/tmp/t0"); return o; }
