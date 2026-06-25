#pragma once
#include <stddef.h>
/* <strings.h> -- BSD/POSIX case-insensitive + bit helpers (libc/string_ext.c) */
int   strcasecmp(const char *a, const char *b);
int   strncasecmp(const char *a, const char *b, size_t n);
void  bzero(void *s, size_t n);
void  bcopy(const void *src, void *dst, size_t n);
int   bcmp(const void *a, const void *b, size_t n);
char *index(const char *s, int c);
char *rindex(const char *s, int c);
int   ffs(int v);
