/* Minimal <stdio.h> for the BoltOS DOOM port. Backed by dg_libc.c, which keeps
 * an in-RAM file table (the embedded WAD + writable scratch files). */
#pragma once
#include <stddef.h>
#include <stdarg.h>

typedef struct _DG_FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF        (-1)
#define SEEK_SET   0
#define SEEK_CUR   1
#define SEEK_END   2
#define BUFSIZ     8192

FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
void   rewind(FILE *f);
int    fflush(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
int    fgetc(FILE *f);
char  *fgets(char *s, int size, FILE *f);
int    fputc(int c, FILE *f);
int    fputs(const char *s, FILE *f);
int     fileno(FILE *f);
int    remove(const char *path);
int    rename(const char *oldp, const char *newp);

int    printf(const char *fmt, ...)                       __attribute__((format(printf,1,2)));
int    fprintf(FILE *f, const char *fmt, ...)             __attribute__((format(printf,2,3)));
int    sprintf(char *buf, const char *fmt, ...)           __attribute__((format(printf,2,3)));
int    snprintf(char *buf, size_t n, const char *fmt, ...)__attribute__((format(printf,3,4)));
int    vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int    vsprintf(char *buf, const char *fmt, va_list ap);
int    vfprintf(FILE *f, const char *fmt, va_list ap);
int    vprintf(const char *fmt, va_list ap);
int    puts(const char *s);
int    putchar(int c);
int    sscanf(const char *str, const char *fmt, ...);
