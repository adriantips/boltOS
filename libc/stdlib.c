/* ===========================================================================
 *  libc/stdlib.c -- conversions, sort/search, PRNG, process control.
 *  Floating conversions (strtod) run on SSE doubles. exit()/abort() funnel into
 *  the kernel panic path since libc code lives inside the kernel address space.
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include "stdlib.h"
#include "string.h"
#include "strings.h"
#include "inttypes.h"
#include "ctype.h"
#include "errno.h"
#include "kprintf.h"

/* ----------------------------- string -> integer ----------------------------- */
static int digit_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}
static unsigned long long strtoull_core(const char *s, char **end, int base, int *neg) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    *neg = 0;
    if (*p == '+') p++; else if (*p == '-') { *neg = 1; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
    else if ((base == 0 || base == 2) && p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) { p += 2; base = 2; }
    else if (base == 0 && p[0] == '0') { base = 8; }
    else if (base == 0) base = 10;
    unsigned long long v = 0; int any = 0;
    for (;; p++) {
        int d = digit_val((unsigned char)*p);
        if (d >= base) break;
        v = v * (unsigned)base + (unsigned)d; any = 1;
    }
    if (end) *end = (char *)(any ? p : s);
    return v;
}
unsigned long long strtoull(const char *s, char **end, int base) {
    int neg; unsigned long long v = strtoull_core(s, end, base, &neg);
    return neg ? (unsigned long long)(-(long long)v) : v;
}
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtoull(s, end, base); }
long long strtoll(const char *s, char **end, int base) {
    int neg; unsigned long long v = strtoull_core(s, end, base, &neg);
    return neg ? -(long long)v : (long long)v;
}
long strtol(const char *s, char **end, int base) { return (long)strtoll(s, end, base); }
intmax_t  strtoimax(const char *s, char **e, int b) { return (intmax_t)strtoll(s, e, b); }
uintmax_t strtoumax(const char *s, char **e, int b) { return (uintmax_t)strtoull(s, e, b); }

/* atoi lives in libc/string.c (shared with the kernel); not redefined here. */
long      atol(const char *s) { return strtol(s, 0, 10); }
long long atoll(const char *s){ return strtoll(s, 0, 10); }

/* ----------------------------- string -> double ----------------------------- */
double strtod(const char *s, char **end) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+') p++; else if (*p == '-') { neg = 1; p++; }
    /* nan / inf */
    if ((p[0]=='n'||p[0]=='N') && (p[1]=='a'||p[1]=='A') && (p[2]=='n'||p[2]=='N')) {
        if (end) *end = (char *)(p + 3); return neg ? -__builtin_nan("") : __builtin_nan("");
    }
    if ((p[0]=='i'||p[0]=='I') && (p[1]=='n'||p[1]=='N') && (p[2]=='f'||p[2]=='F')) {
        p += 3; if (!strncasecmp(p, "inity", 5)) p += 5;
        if (end) *end = (char *)p; return neg ? -__builtin_inf() : __builtin_inf();
    }
    double mant = 0.0; int any = 0, frac = 0;
    for (; isdigit((unsigned char)*p); p++) { mant = mant * 10.0 + (*p - '0'); any = 1; }
    if (*p == '.') { p++; for (; isdigit((unsigned char)*p); p++) { mant = mant * 10.0 + (*p - '0'); frac++; any = 1; } }
    int exp = 0;
    if (any && (*p == 'e' || *p == 'E')) {
        const char *e2 = p + 1; int es = 0, en = 0, eany = 0;
        if (*e2 == '+') e2++; else if (*e2 == '-') { es = 1; e2++; }
        for (; isdigit((unsigned char)*e2); e2++) { en = en * 10 + (*e2 - '0'); eany = 1; }
        if (eany) { exp = es ? -en : en; p = e2; }
    }
    if (!any) { if (end) *end = (char *)s; return 0.0; }
    exp -= frac;
    double r = mant;
    /* scale by 10^exp using repeated squaring on a double base */
    if (exp != 0) {
        double base = 10.0; int e = exp < 0 ? -exp : exp; double f = 1.0;
        while (e) { if (e & 1) f *= base; base *= base; e >>= 1; }
        r = exp < 0 ? r / f : r * f;
    }
    if (end) *end = (char *)p;
    return neg ? -r : r;
}
float  strtof(const char *s, char **end) { return (float)strtod(s, end); }
double atof(const char *s) { return strtod(s, 0); }

/* ----------------------------- integer arithmetic ----------------------------- */
int       abs(int v)        { return v < 0 ? -v : v; }
long      labs(long v)      { return v < 0 ? -v : v; }
long long llabs(long long v){ return v < 0 ? -v : v; }
intmax_t  imaxabs(intmax_t v){ return v < 0 ? -v : v; }
div_t   div(int n, int d)   { div_t r = { n / d, n % d }; return r; }
ldiv_t  ldiv(long n, long d){ ldiv_t r = { n / d, n % d }; return r; }
lldiv_t lldiv(long long n, long long d) { lldiv_t r = { n / d, n % d }; return r; }
imaxdiv_t imaxdiv(intmax_t n, intmax_t d) { imaxdiv_t r = { n / d, n % d }; return r; }

/* ----------------------------- qsort / bsearch ----------------------------- */
static void swp(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
static void qsort_r(char *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    while (n > 1) {
        /* median-of-three pivot to base[0], partition (Lomuto on the tail) */
        size_t mid = n / 2;
        if (cmp(base + mid * sz, base) < 0) swp(base, base + mid * sz, sz);
        if (cmp(base + (n - 1) * sz, base) < 0) swp(base, base + (n - 1) * sz, sz);
        if (cmp(base + mid * sz, base) < 0) swp(base, base + mid * sz, sz);
        char *pivot = base;
        size_t lt = 0;
        for (size_t i = 1; i < n; i++)
            if (cmp(base + i * sz, pivot) < 0) { lt++; swp(base + lt * sz, base + i * sz, sz); }
        swp(base, base + lt * sz, sz);
        /* recurse into the smaller side, loop on the larger (bounded stack) */
        size_t ln = lt, rn = n - lt - 1;
        if (ln < rn) { qsort_r(base, ln, sz, cmp); base += (lt + 1) * sz; n = rn; }
        else         { qsort_r(base + (lt + 1) * sz, rn, sz, cmp); n = ln; }
    }
}
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    if (n > 1 && sz) qsort_r((char *)base, n, sz, cmp);
}
void *bsearch(const void *key, const void *base, size_t n, size_t sz,
              int (*cmp)(const void *, const void *)) {
    size_t lo = 0, hi = n;
    const char *b = base;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp(key, b + mid * sz);
        if (c < 0) hi = mid;
        else if (c > 0) lo = mid + 1;
        else return (void *)(b + mid * sz);
    }
    return 0;
}

/* ----------------------------- PRNG (xorshift) ----------------------------- */
static uint64_t rng = 0x2545F4914F6CDD1Dull;
int  rand(void)        { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (int)(rng & 0x7fffffff); }
void srand(unsigned s) { rng = s ? ((uint64_t)s << 1) | 1 : 1; }
long random(void)      { return rand(); }
void srandom(unsigned s){ srand(s); }

/* ----------------------------- environment ----------------------------- */
static struct { char key[48]; char val[160]; int used; } envtab[32];
char *getenv(const char *name) {
    for (unsigned i = 0; i < 32; i++)
        if (envtab[i].used && !strcmp(envtab[i].key, name)) return envtab[i].val;
    return 0;
}
int setenv(const char *name, const char *val, int overwrite) {
    for (unsigned i = 0; i < 32; i++)
        if (envtab[i].used && !strcmp(envtab[i].key, name)) {
            if (overwrite) strlcpy(envtab[i].val, val, sizeof envtab[i].val);
            return 0;
        }
    for (unsigned i = 0; i < 32; i++)
        if (!envtab[i].used) { envtab[i].used = 1; strlcpy(envtab[i].key, name, sizeof envtab[i].key);
                               strlcpy(envtab[i].val, val, sizeof envtab[i].val); return 0; }
    return -1;
}
int unsetenv(const char *name) {
    for (unsigned i = 0; i < 32; i++) if (envtab[i].used && !strcmp(envtab[i].key, name)) envtab[i].used = 0;
    return 0;
}

/* ----------------------------- process control ----------------------------- */
static void (*atexit_fns[16])(void); static int atexit_n;
int  atexit(void (*fn)(void)) { if (atexit_n < 16) { atexit_fns[atexit_n++] = fn; return 0; } return -1; }
void exit(int code) {
    for (int i = atexit_n - 1; i >= 0; i--) atexit_fns[i]();
    kprintf("\n[libc] exit(%d) inside kernel context\n", code);
    panic("libc exit()");
    for (;;) __asm__ volatile("hlt");
}
void _Exit(int code) { (void)code; panic("libc _Exit()"); for (;;) __asm__ volatile("hlt"); }
void abort(void) { panic("libc abort()"); for (;;) __asm__ volatile("hlt"); }
int  system(const char *cmd) { (void)cmd; errno = ENOSYS; return -1; }
int  mkstemp(char *tmpl) { (void)tmpl; errno = ENOSYS; return -1; }

/* ----------------------------- multibyte (C locale) ----------------------------- */
int    mblen(const char *s, size_t n)  { (void)n; return (s && *s) ? 1 : 0; }
int    mbtowc(int *pw, const char *s, size_t n) { (void)n; if (!s) return 0; if (pw) *pw = (unsigned char)*s; return *s ? 1 : 0; }
int    wctomb(char *s, int wc) { if (!s) return 0; *s = (char)wc; return 1; }
size_t mbstowcs(int *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) if (d) d[i] = (unsigned char)s[i]; if (i < n && d) d[i] = 0; return i; }
size_t wcstombs(char *d, const int *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) if (d) d[i] = (char)s[i]; if (i < n && d) d[i] = 0; return i; }
