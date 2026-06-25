/* ===========================================================================
 *  kernel/cmd_libctest.c -- `libctest`: exercise the freestanding libc so the
 *  NetSurf port has a verified C runtime under it. Prints one line per check.
 * ===========================================================================*/
#include <stdint.h>
#include "commands.h"
#include "kprintf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <errno.h>
#include <time.h>

static int pass, fail;
static void ck(const char *what, int ok) {
    kprintf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) pass++; else fail++;
}
static int feq(double a, double b) { double d = a - b; if (d < 0) d = -d; return d < 1e-6; }

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static jmp_buf jb;
static void deep(int n) { if (n == 0) longjmp(jb, 42); deep(n - 1); }

int cmd_libctest(int argc, char **argv) {
    (void)argc; (void)argv;
    pass = fail = 0;
    char buf[128];

    kprintf("libc: formatted output\n");
    snprintf(buf, sizeof buf, "%d %05d %+d %x %o", 42, 42, 7, 255, 8);
    ck("snprintf int/flags", !strcmp(buf, "42 00042 +7 ff 10"));
    snprintf(buf, sizeof buf, "[%8.3f][%-8.3f][%g]", 3.14159, 3.14159, 0.0001);
    ck("snprintf float", !strcmp(buf, "[   3.142][3.142   ][0.0001]"));
    snprintf(buf, sizeof buf, "%s/%c/%.3s", "hello", 'X', "abcdef");
    ck("snprintf str/char/prec", !strcmp(buf, "hello/X/abc"));
    int n = snprintf(buf, sizeof buf, "%lld", -1234567890123LL);
    ck("snprintf long long", n == 14 && !strcmp(buf, "-1234567890123"));

    kprintf("libc: formatted input\n");
    int a = 0, b = 0; double d = 0;
    int got = sscanf("17 -3 2.5", "%d %d %lf", &a, &b, &d);
    ck("sscanf d/d/lf", got == 3 && a == 17 && b == -3 && feq(d, 2.5));
    unsigned u = 0; sscanf("deadBEEF", "%x", &u);
    ck("sscanf hex", u == 0xdeadbeef);

    kprintf("libc: string\n");
    ck("strstr", strstr("a needle here", "needle") != 0 && strstr("abc", "z") == 0);
    ck("strchr/rchr", strrchr("a/b/c", '/') - (char *)"a/b/c" == 3);
    ck("strspn/cspn", strspn("123abc", "0123456789") == 3 && strcspn("abc=1", "=") == 3);
    ck("strcasecmp", strcasecmp("HeLLo", "hello") == 0 && strncasecmp("ABCx", "abcY", 3) == 0);
    char tk[32]; strcpy(tk, "a,bb,,c"); char *sv = 0;
    char *t1 = strtok_r(tk, ",", &sv), *t2 = strtok_r(0, ",", &sv), *t3 = strtok_r(0, ",", &sv);
    ck("strtok_r", t1 && !strcmp(t1, "a") && t2 && !strcmp(t2, "bb") && t3 && !strcmp(t3, "c"));
    char *dup = strdup("copy me");
    ck("strdup", dup && !strcmp(dup, "copy me")); free(dup);
    ck("memchr", memchr("abcde", 'c', 5) != 0 && memchr("abcde", 'z', 5) == 0);

    kprintf("libc: stdlib conversions\n");
    ck("strtol bases", strtol("-100", 0, 10) == -100 && strtol("0x1F", 0, 0) == 31 && strtol("777", 0, 8) == 511);
    ck("strtoul", strtoul("4294967295", 0, 10) == 4294967295UL);
    ck("strtod", feq(strtod("3.14159", 0), 3.14159) && feq(strtod("1.5e3", 0), 1500.0));
    ck("atoi/atol", atoi("  -42xyz") == -42 && atol("9999") == 9999);

    kprintf("libc: qsort / bsearch\n");
    int arr[] = { 5, 3, 9, 1, 7, 2, 8, 4, 6, 0 };
    qsort(arr, 10, sizeof(int), cmp_int);
    int sorted = 1; for (int i = 0; i < 10; i++) if (arr[i] != i) sorted = 0;
    ck("qsort", sorted);
    int key = 7; int *fnd = bsearch(&key, arr, 10, sizeof(int), cmp_int);
    ck("bsearch", fnd && *fnd == 7);

    kprintf("libc: heap\n");
    void *p = malloc(64); int ok = p != 0;
    memset(p, 0xAB, 64);
    p = realloc(p, 256); ok = ok && p && ((uint8_t *)p)[0] == 0xAB && ((uint8_t *)p)[63] == 0xAB;
    free(p);
    int *cz = calloc(16, sizeof(int)); ok = ok && cz && cz[0] == 0 && cz[15] == 0; free(cz);
    void *al = aligned_alloc(64, 100); ok = ok && al && ((uintptr_t)al & 63) == 0; free(al);
    ck("malloc/realloc/calloc/aligned_alloc", ok);

    kprintf("libc: math (SSE double)\n");
    ck("sqrt/pow", feq(sqrt(2.0), 1.41421356) && feq(pow(2.0, 10.0), 1024.0));
    ck("floor/ceil/round/trunc", floor(2.7) == 2 && ceil(2.1) == 3 && round(2.5) == 3 && trunc(-2.7) == -2);
    ck("sin/cos", feq(sin(0.0), 0.0) && feq(cos(0.0), 1.0) && feq(sin(M_PI / 2), 1.0));
    ck("exp/log", feq(log(exp(1.0)), 1.0) && feq(exp(0.0), 1.0));
    ck("fmod", feq(fmod(10.0, 3.0), 1.0));

    kprintf("libc: setjmp / longjmp\n");
    int v = setjmp(jb);
    if (v == 0) deep(20);
    ck("setjmp/longjmp", v == 42);

    kprintf("libc: time\n");
    struct tm tm = { .tm_year = 100, .tm_mon = 0, .tm_mday = 1, .tm_hour = 0, .tm_min = 0, .tm_sec = 0 };
    time_t e = timegm(&tm);                  /* 2000-01-01 00:00:00 UTC */
    ck("mktime epoch", e == 946684800);
    struct tm out; gmtime_r(&e, &out);
    ck("gmtime roundtrip", out.tm_year == 100 && out.tm_mon == 0 && out.tm_mday == 1);
    strftime(buf, sizeof buf, "%Y-%m-%d", &out);
    ck("strftime", !strcmp(buf, "2000-01-01"));

    kprintf("libc: in-memory streams\n");
    char *ms = 0; size_t msz = 0; FILE *mf = open_memstream(&ms, &msz);
    fprintf(mf, "x=%d,y=%s", 5, "hi"); fclose(mf);
    ck("open_memstream+fprintf", ms && !strcmp(ms, "x=5,y=hi") && msz == 8);

    kprintf("\nlibc self-test: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
