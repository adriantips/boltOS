#pragma once
#include <stddef.h>

/* ===========================================================================
 *  <string.h> -- BoltOS freestanding C library (libc/string.c, libc/string_ext.c)
 *  A real, standards-shaped string/memory surface so portable C (NetSurf and
 *  its support libraries) compiles unmodified against the kernel.
 * ===========================================================================*/

/* --- memory --- */
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);
void  *memrchr(const void *s, int c, size_t n);
void  *memmem(const void *h, size_t hl, const void *n, size_t nl);

/* --- length / compare --- */
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcoll(const char *a, const char *b);          /* C locale == strcmp  */

/* --- copy / concat --- */
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);      /* always NUL-terminates d[n-1] */
char  *strcat(char *d, const char *s);
char  *strncat(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t cap);    /* BSD: returns srclen */
size_t strlcat(char *d, const char *s, size_t cap);
char  *kstrlcat(char *d, const char *s, size_t cap);   /* legacy kernel alias */

/* --- search --- */
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *h, const char *n);
char  *strcasestr(const char *h, const char *n);
char  *strpbrk(const char *s, const char *accept);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strtok(char *s, const char *delim);
char  *strtok_r(char *s, const char *delim, char **saveptr);

/* --- duplicate (heap) --- */
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);

/* --- case-insensitive compare (BSD/GNU extension; defined in string_ext.c).
 *     Also declared in <strings.h>; mirrored here because much portable C
 *     (e.g. libcss) reaches for them with only <string.h> included.        */
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);

/* --- misc --- */
char  *strerror(int errnum);
size_t strxfrm(char *dst, const char *src, size_t n);
int    atoi(const char *s);                            /* (also in stdlib.h)  */
