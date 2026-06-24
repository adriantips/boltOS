/* Minimal <string.h> for the BoltOS DOOM port. The core mem/str routines are
 * provided by the kernel's libc/string.c; the extras live in dg_libc.c. */
#pragma once
#include <stddef.h>

void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
/* NB: the kernel's libc strncpy is strlcpy-flavoured (forces a NUL at d[n-1]),
 * which corrupts DOOM's exactly-8-char lump names. Redirect to a standard one. */
char  *dg_strncpy(char *d, const char *s, size_t n);
#define strncpy dg_strncpy
char  *strcat(char *d, const char *s);
char  *strncat(char *d, const char *s, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
char  *strdup(const char *s);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strerror(int errnum);
