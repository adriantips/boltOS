/* ===========================================================================
 *  libc/string_ext.c -- the rest of <string.h>/<strings.h> beyond the handful
 *  of primitives the kernel already shipped in libc/string.c. Kept separate so
 *  the original file (memset/memcpy/strlen/strcmp/...) stays untouched.
 * ===========================================================================*/
#include <stddef.h>
#include <stdint.h>
#include "string.h"
#include "strings.h"
#include "stdlib.h"
#include "ctype.h"

/* --- memory --- */
void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = s;
    for (; n; n--, p++) if (*p == (uint8_t)c) return (void *)p;
    return 0;
}
void *memrchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *)s + n;
    while (n--) { if (*--p == (uint8_t)c) return (void *)p; }
    return 0;
}
void *memmem(const void *h, size_t hl, const void *n, size_t nl) {
    if (nl == 0) return (void *)h;
    if (hl < nl) return 0;
    const uint8_t *hp = h, *np = n;
    for (size_t i = 0; i + nl <= hl; i++)
        if (hp[i] == np[0] && memcmp(hp + i, np, nl) == 0) return (void *)(hp + i);
    return 0;
}

/* --- length / compare --- */
size_t strnlen(const char *s, size_t m) { size_t n = 0; while (n < m && s[n]) n++; return n; }
int    strcoll(const char *a, const char *b) { return strcmp(a, b); }
size_t strxfrm(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t k = l < n - 1 ? l : n - 1; memcpy(d, s, k); d[k] = 0; }
    return l;
}

/* --- copy / concat --- */
char *strncat(char *d, const char *s, size_t n) {
    char *r = d; while (*d) d++;
    while (n-- && *s) *d++ = *s++;
    *d = 0; return r;
}
size_t strlcpy(char *d, const char *s, size_t cap) {
    size_t sl = strlen(s);
    if (cap) { size_t k = sl < cap - 1 ? sl : cap - 1; memcpy(d, s, k); d[k] = 0; }
    return sl;
}
size_t strlcat(char *d, const char *s, size_t cap) {
    size_t dl = strnlen(d, cap), sl = strlen(s);
    if (dl == cap) return cap + sl;
    size_t room = cap - dl - 1, k = sl < room ? sl : room;
    memcpy(d + dl, s, k); d[dl + k] = 0;
    return dl + sl;
}

/* --- search --- */
char *strstr(const char *h, const char *n) {
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return 0;
}
char *strcasestr(const char *h, const char *n) {
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return 0;
}
size_t strspn(const char *s, const char *acc) {
    const char *p = s;
    for (; *p; p++) { if (!strchr(acc, *p)) break; }
    return (size_t)(p - s);
}
size_t strcspn(const char *s, const char *rej) {
    const char *p = s;
    for (; *p; p++) { if (strchr(rej, *p)) break; }
    return (size_t)(p - s);
}
char *strpbrk(const char *s, const char *acc) {
    for (; *s; s++) if (strchr(acc, *s)) return (char *)s;
    return 0;
}
char *strtok_r(char *s, const char *delim, char **save) {
    char *p = s ? s : *save;
    if (!p) return 0;
    p += strspn(p, delim);
    if (!*p) { *save = 0; return 0; }
    char *tok = p;
    p += strcspn(p, delim);
    if (*p) { *p = 0; *save = p + 1; } else { *save = 0; }
    return tok;
}
char *strtok(char *s, const char *delim) {
    static char *save;
    return strtok_r(s, delim, &save);
}

/* --- heap duplicate --- */
char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
char *strndup(const char *s, size_t n) {
    size_t l = strnlen(s, n);
    char *p = malloc(l + 1);
    if (p) { memcpy(p, s, l); p[l] = 0; }
    return p;
}

/* --- diagnostics --- */
char *strerror(int e) {
    switch (e) {
    case 0:  return "Success";
    case 1:  return "Operation not permitted";
    case 2:  return "No such file or directory";
    case 5:  return "I/O error";
    case 9:  return "Bad file descriptor";
    case 11: return "Try again";
    case 12: return "Out of memory";
    case 13: return "Permission denied";
    case 14: return "Bad address";
    case 17: return "File exists";
    case 22: return "Invalid argument";
    case 28: return "No space left on device";
    case 34: return "Numerical result out of range";
    case 38: return "Function not implemented";
    case 84: return "Invalid or incomplete multibyte sequence";
    case 110:return "Connection timed out";
    case 111:return "Connection refused";
    default: return "Unknown error";
    }
}

/* --- <strings.h> --- */
int strcasecmp(const char *a, const char *b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; n--; }
    if (!n) return 0;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
void  bzero(void *s, size_t n)             { memset(s, 0, n); }
void  bcopy(const void *s, void *d, size_t n) { memmove(d, s, n); }
int   bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
char *index(const char *s, int c)          { return strchr(s, c); }
char *rindex(const char *s, int c)         { return strrchr(s, c); }
int   ffs(int v) { for (int i = 0; i < 32; i++) if (v & (1 << i)) return i + 1; return 0; }
