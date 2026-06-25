#pragma once
#include <stddef.h>
#include <stdarg.h>

/* ===========================================================================
 *  <stdio.h> -- BoltOS freestanding C library
 *    formatted output  -> libc/printf.c   (full vsnprintf engine, SSE doubles)
 *    formatted input    -> libc/scanf.c
 *    streams (FILE)     -> libc/stdio_file.c  (VFS-backed + in-memory streams)
 * ===========================================================================*/

#define EOF (-1)
#define BUFSIZ 1024
#define FOPEN_MAX 32
#define FILENAME_MAX 256
#define L_tmpnam 32
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct __FILE FILE;
typedef long fpos_t;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* --- formatted output --- */
int snprintf(char *str, size_t size, const char *fmt, ...) __attribute__((format(printf,3,4)));
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
int sprintf(char *str, const char *fmt, ...) __attribute__((format(printf,2,3)));
int vsprintf(char *str, const char *fmt, va_list ap);
int printf(const char *fmt, ...) __attribute__((format(printf,1,2)));
int vprintf(const char *fmt, va_list ap);
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf,2,3)));
int vfprintf(FILE *f, const char *fmt, va_list ap);
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf,2,3)));
int asprintf(char **out, const char *fmt, ...) __attribute__((format(printf,2,3)));
int vasprintf(char **out, const char *fmt, va_list ap);

/* --- formatted input --- */
int sscanf(const char *str, const char *fmt, ...) __attribute__((format(scanf,2,3)));
int vsscanf(const char *str, const char *fmt, va_list ap);
int fscanf(FILE *f, const char *fmt, ...) __attribute__((format(scanf,2,3)));
int scanf(const char *fmt, ...) __attribute__((format(scanf,1,2)));

/* --- streams --- */
FILE  *fopen(const char *path, const char *mode);
FILE  *fdopen(int fd, const char *mode);
FILE  *freopen(const char *path, const char *mode, FILE *f);
FILE  *fmemopen(void *buf, size_t size, const char *mode);
FILE  *open_memstream(char **bufp, size_t *sizep);
int    fclose(FILE *f);
int    fflush(FILE *f);
size_t fread(void *ptr, size_t size, size_t n, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f);
int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
int    fgetpos(FILE *f, fpos_t *pos);
int    fsetpos(FILE *f, const fpos_t *pos);
void   rewind(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);
int    fileno(FILE *f);
void   setbuf(FILE *f, char *buf);
int    setvbuf(FILE *f, char *buf, int mode, size_t size);

/* --- character / line I/O --- */
int    fgetc(FILE *f);
int    getc(FILE *f);
int    getchar(void);
int    ungetc(int c, FILE *f);
char  *fgets(char *s, int size, FILE *f);
int    fputc(int c, FILE *f);
int    putc(int c, FILE *f);
int    putchar(int c);
int    fputs(const char *s, FILE *f);
int    puts(const char *s);

/* --- misc --- */
int    remove(const char *path);
int    rename(const char *from, const char *to);
void   perror(const char *s);
FILE  *tmpfile(void);
char  *tmpnam(char *s);
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
