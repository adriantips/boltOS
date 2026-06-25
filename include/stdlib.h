#pragma once
#include <stddef.h>

/* ===========================================================================
 *  <stdlib.h> -- BoltOS freestanding C library
 *    malloc family  -> libc/malloc.c   (size-prefixed wrapper over kmalloc)
 *    conversions/sort/rand/exit -> libc/stdlib.c
 * ===========================================================================*/

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff
#define MB_CUR_MAX 1

/* --- memory --- */
void  *malloc(size_t n);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *p, size_t n);
void  *reallocarray(void *p, size_t nmemb, size_t size);
void   free(void *p);
void  *aligned_alloc(size_t align, size_t size);
int    posix_memalign(void **out, size_t align, size_t size);

/* --- string -> number --- */
int                 atoi(const char *s);
long                atol(const char *s);
long long           atoll(const char *s);
double              atof(const char *s);
long                strtol(const char *s, char **end, int base);
unsigned long       strtoul(const char *s, char **end, int base);
long long           strtoll(const char *s, char **end, int base);
unsigned long long  strtoull(const char *s, char **end, int base);
double              strtod(const char *s, char **end);
float               strtof(const char *s, char **end);

/* --- integer arithmetic --- */
int       abs(int v);
long      labs(long v);
long long llabs(long long v);
typedef struct { int quot, rem; }             div_t;
typedef struct { long quot, rem; }            ldiv_t;
typedef struct { long long quot, rem; }       lldiv_t;
div_t     div(int num, int den);
ldiv_t    ldiv(long num, long den);
lldiv_t   lldiv(long long num, long long den);

/* --- sort / search --- */
void  qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t sz,
              int (*cmp)(const void *, const void *));

/* --- prng --- */
int   rand(void);
void  srand(unsigned seed);
long  random(void);
void  srandom(unsigned seed);

/* --- environment / process --- */
char *getenv(const char *name);
int   setenv(const char *name, const char *val, int overwrite);
int   unsetenv(const char *name);
int   atexit(void (*fn)(void));
void  exit(int code) __attribute__((noreturn));
void  _Exit(int code) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));
int   system(const char *cmd);
int   mkstemp(char *tmpl);

/* --- multibyte (C-locale, ASCII passthrough) --- */
int   mblen(const char *s, size_t n);
int   mbtowc(int *pwc, const char *s, size_t n);
int   wctomb(char *s, int wc);
size_t mbstowcs(int *dst, const char *src, size_t n);
size_t wcstombs(char *dst, const int *src, size_t n);
