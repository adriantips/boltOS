/* ===========================================================================
 *  libc/scanf.c -- formatted input for BoltOS's freestanding libc.
 *  Supports %d %i %u %o %x %c %s %f/%e/%g %[..] %n %% with optional field width
 *  and assignment suppression (%*). Length modifiers h/l/ll/z map width onto the
 *  destination. Drives off a tiny pull-character reader so sscanf and fscanf
 *  share one engine.
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "stdio.h"
#include "stdlib.h"
#include "ctype.h"
#include "string.h"

typedef struct { const char *s; FILE *f; int last; int consumed; } src;
static int s_get(src *r) {
    int c;
    if (r->s) { c = (unsigned char)*r->s; if (c == 0) c = -1; else r->s++; }
    else      c = fgetc(r->f);
    if (c >= 0) r->consumed++;
    r->last = c;
    return c;
}
/* simple one-char pushback */
static void s_unget(src *r, int c) {
    if (c < 0) return;
    if (r->s) { r->s--; }
    else ungetc(c, r->f);
    r->consumed--;
}

int vsscanf_impl(src *r, const char *fmt, va_list ap) {
    int assigned = 0;
    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) {
            int c; do { c = s_get(r); } while (c >= 0 && isspace(c));
            s_unget(r, c);
            continue;
        }
        if (*fmt != '%') {
            int c = s_get(r);
            if (c != *fmt) { s_unget(r, c); return assigned; }
            continue;
        }
        fmt++;
        int suppress = 0; if (*fmt == '*') { suppress = 1; fmt++; }
        int width = 0; while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        if (width == 0) width = 1 << 30;
        int lng = 0;
        if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'l') { lng = 1; fmt++; if (*fmt == 'l') { lng = 2; fmt++; } }
        else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'L') { lng = 1; fmt++; }

        char conv = *fmt;
        int c;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'o': case 'p': {
            do { c = s_get(r); } while (c >= 0 && isspace(c));
            char buf[64]; int n = 0;
            if (c == '+' || c == '-') { if (n < (int)sizeof buf - 1) buf[n++] = c; c = s_get(r); width--; }
            int base = conv == 'x' || conv == 'p' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
            while (c >= 0 && width-- > 0 && n < (int)sizeof buf - 1) {
                int ok = conv == 'u' || conv == 'd' || conv == 'i' ? isdigit(c)
                       : conv == 'o' ? (c >= '0' && c <= '7')
                       : isxdigit(c) || ((n == 1) && (c == 'x' || c == 'X'));
                if (!ok) break;
                buf[n++] = c; c = s_get(r);
            }
            s_unget(r, c);
            if (n == 0) return assigned;
            buf[n] = 0;
            if (!suppress) {
                if (conv == 'u' || conv == 'x' || conv == 'o' || conv == 'p') {
                    unsigned long long v = strtoull(buf, 0, base);
                    if (lng == 2) *va_arg(ap, unsigned long long *) = v;
                    else if (lng == 1) *va_arg(ap, unsigned long *) = (unsigned long)v;
                    else *va_arg(ap, unsigned *) = (unsigned)v;
                } else {
                    long long v = strtoll(buf, 0, base);
                    if (lng == 2) *va_arg(ap, long long *) = v;
                    else if (lng == 1) *va_arg(ap, long *) = (long)v;
                    else *va_arg(ap, int *) = (int)v;
                }
                assigned++;
            }
            break;
        }
        case 'f': case 'e': case 'g': case 'a': {
            do { c = s_get(r); } while (c >= 0 && isspace(c));
            char buf[80]; int n = 0;
            while (c >= 0 && width-- > 0 && n < (int)sizeof buf - 1 &&
                   (isdigit(c) || c=='+' || c=='-' || c=='.' || c=='e' || c=='E')) { buf[n++] = c; c = s_get(r); }
            s_unget(r, c);
            if (n == 0) return assigned;
            buf[n] = 0;
            if (!suppress) {
                double v = strtod(buf, 0);
                if (lng) *va_arg(ap, double *) = v; else *va_arg(ap, float *) = (float)v;
                assigned++;
            }
            break;
        }
        case 'c': {
            int cnt = width == (1 << 30) ? 1 : width;
            char *out = suppress ? 0 : va_arg(ap, char *);
            int got = 0;
            while (cnt-- > 0) { c = s_get(r); if (c < 0) break; if (out) *out++ = (char)c; got++; }
            if (got == 0) return assigned;
            if (!suppress) assigned++;
            break;
        }
        case 's': {
            do { c = s_get(r); } while (c >= 0 && isspace(c));
            char *out = suppress ? 0 : va_arg(ap, char *);
            int n = 0;
            while (c >= 0 && !isspace(c) && width-- > 0) { if (out) out[n] = (char)c; n++; c = s_get(r); }
            s_unget(r, c);
            if (n == 0) return assigned;
            if (out) out[n] = 0;
            if (!suppress) assigned++;
            break;
        }
        case '[': {
            fmt++;
            int negate = 0; if (*fmt == '^') { negate = 1; fmt++; }
            const char *set = fmt;
            if (*fmt == ']') fmt++;            /* leading ] is literal */
            while (*fmt && *fmt != ']') fmt++;
            int setlen = (int)(fmt - set);
            char *out = suppress ? 0 : va_arg(ap, char *);
            int n = 0;
            c = s_get(r);
            while (c >= 0 && width-- > 0) {
                int in = 0; for (int i = 0; i < setlen; i++) if (set[i] == c) { in = 1; break; }
                if (in == negate) break;
                if (out) out[n] = (char)c; n++; c = s_get(r);
            }
            s_unget(r, c);
            if (n == 0) return assigned;
            if (out) out[n] = 0;
            if (!suppress) assigned++;
            break;
        }
        case 'n': { if (!suppress) *va_arg(ap, int *) = r->consumed; break; }
        case '%': { do { c = s_get(r); } while (c >= 0 && isspace(c));
                    if (c != '%') { s_unget(r, c); return assigned; } break; }
        default: return assigned;
        }
    }
    return assigned;
}

int vsscanf(const char *str, const char *fmt, va_list ap) {
    src r = { str, 0, 0, 0 };
    return vsscanf_impl(&r, fmt, ap);
}
int sscanf(const char *str, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int n = vsscanf(str, fmt, ap); va_end(ap); return n;
}
int fscanf(FILE *f, const char *fmt, ...) {
    src r = { 0, f, 0, 0 };
    va_list ap; va_start(ap, fmt); int n = vsscanf_impl(&r, fmt, ap); va_end(ap); return n;
}
int scanf(const char *fmt, ...) {
    src r = { 0, stdin, 0, 0 };
    va_list ap; va_start(ap, fmt); int n = vsscanf_impl(&r, fmt, ap); va_end(ap); return n;
}
