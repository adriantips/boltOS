/* ===========================================================================
 *  libc/printf.c -- the printf family for BoltOS's freestanding libc.
 *
 *  One engine (emit) drives every variant through a small "sink" abstraction:
 *  snprintf/vsnprintf fill a caller buffer, printf/vprintf push to the kernel
 *  console (kputc), asprintf allocates. Supports the C99 conversion set incl.
 *  flags (-+ #0), '*' width/precision, length modifiers (hh h l ll z j t L) and
 *  floating point (%f/%e/%g and upper variants) computed on SSE doubles -- no
 *  x87, since the kernel builds -mno-80387.
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "kprintf.h"   /* kputc */
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

typedef struct sink {
    char  *buf;        /* destination (NULL => console)                         */
    size_t cap;        /* capacity incl. NUL (0 => unbounded count only)        */
    size_t len;        /* chars that *would* be written (return value)          */
    int    console;    /* push to kputc instead of buf                          */
} sink;

static void s_put(sink *s, char c) {
    if (s->console) kputc(c);
    else if (s->buf && s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len++;
}
static void s_str(sink *s, const char *p, size_t n) { while (n--) s_put(s, *p++); }

/* unsigned -> ASCII in the given base; returns length, fills tmp (not NUL-term) */
static int u_to_str(uint64_t v, unsigned base, int upper, char *tmp) {
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char rev[32]; int n = 0;
    if (v == 0) rev[n++] = '0';
    while (v) { rev[n++] = dig[v % base]; v /= base; }
    for (int i = 0; i < n; i++) tmp[i] = rev[n - 1 - i];
    return n;
}

/* ---- double -> string (no x87). Handles f / e / g, returns length. ---- */
static double pow10i(int e) { double r = 1.0; double b = (e < 0) ? 0.1 : 10.0;
    int n = e < 0 ? -e : e; while (n--) r *= b; return r; }

static int fmt_fixed(char *out, double x, int prec) {
    /* x >= 0, finite. Emit integer part + '.' + prec fractional digits, rounded. */
    int n = 0;
    /* round into the last fractional digit up front */
    double scale = pow10i(prec);
    /* integer and fractional split with rounding */
    double xr = x * scale + 0.5;
    /* split xr into integer using uint64 when possible */
    char ibuf[32]; int il;
    if (xr < 1.8446744073709552e19) {
        uint64_t whole = (uint64_t)xr;          /* scaled, rounded integer */
        uint64_t ip = prec ? whole / (uint64_t)scale : whole;
        uint64_t fp = prec ? whole % (uint64_t)scale : 0;
        il = u_to_str(ip, 10, 0, ibuf);
        for (int i = 0; i < il; i++) out[n++] = ibuf[i];
        if (prec) {
            out[n++] = '.';
            char fbuf[24]; int fl = u_to_str(fp, 10, 0, fbuf);
            for (int i = 0; i < prec - fl; i++) out[n++] = '0';   /* leading zeros */
            for (int i = 0; i < fl; i++) out[n++] = fbuf[i];
        }
    } else {
        /* magnitude beyond uint64: fall back to crude scientific-ish print */
        il = u_to_str((uint64_t)x, 10, 0, ibuf);
        for (int i = 0; i < il; i++) out[n++] = ibuf[i];
        if (prec) { out[n++] = '.'; for (int i = 0; i < prec; i++) out[n++] = '0'; }
    }
    return n;
}

static int fmt_double(char *out, double x, int prec, char conv) {
    int n = 0, upper = (conv >= 'A' && conv <= 'Z');
    char c = upper ? conv + 32 : conv;
    if (prec < 0) prec = 6;
    if (__builtin_isnan(x)) { memcpy(out, upper ? "NAN" : "nan", 3); return 3; }
    int neg = __builtin_signbit(x);
    if (neg) { out[n++] = '-'; x = -x; }
    if (__builtin_isinf(x)) { memcpy(out + n, upper ? "INF" : "inf", 3); return n + 3; }

    if (c == 'e') {
        int exp = 0;
        if (x != 0.0) { while (x >= 10.0) { x /= 10.0; exp++; } while (x < 1.0) { x *= 10.0; exp--; } }
        n += fmt_fixed(out + n, x, prec);
        out[n++] = upper ? 'E' : 'e';
        out[n++] = exp < 0 ? '-' : '+';
        int ae = exp < 0 ? -exp : exp;
        char eb[8]; int el = u_to_str((uint64_t)ae, 10, 0, eb);
        if (el < 2) out[n++] = '0';
        for (int i = 0; i < el; i++) out[n++] = eb[i];
        return n;
    }
    if (c == 'g') {
        /* choose %e if exponent < -4 or >= precision; trim trailing zeros */
        int P = prec ? prec : 1;
        int exp = 0; double t = x;
        if (t != 0.0) { while (t >= 10.0) { t /= 10.0; exp++; } while (t < 1.0) { t *= 10.0; exp--; } }
        char tmp[64]; int tl;
        if (exp < -4 || exp >= P) tl = fmt_double(tmp, neg ? -x : x, P - 1, upper ? 'E' : 'e');
        else                      tl = fmt_double(tmp, neg ? -x : x, P - 1 - exp, 'f');
        /* fmt_double re-added sign; copy from after our already-emitted sign */
        int start = (tmp[0] == '-') ? 1 : 0;
        /* trim trailing zeros in the fractional part (and a dangling '.') */
        int dot = -1, ee = tl;
        for (int i = start; i < tl; i++) { if (tmp[i] == '.') dot = i; if (tmp[i]=='e'||tmp[i]=='E'){ ee=i; break; } }
        if (dot >= 0) {
            int last = ee - 1;
            while (last > dot && tmp[last] == '0') last--;
            if (last == dot) last--;
            int w = n;
            for (int i = start; i <= last; i++) out[w++] = tmp[i];
            for (int i = ee; i < tl; i++) out[w++] = tmp[i];
            return w;
        }
        for (int i = start; i < tl; i++) out[n++] = tmp[i];
        return n;
    }
    /* default 'f' */
    n += fmt_fixed(out + n, x, prec);
    return n;
}

/* ---- the engine ---- */
static int emit(sink *s, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { s_put(s, *fmt); continue; }
        fmt++;
        /* flags */
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1; else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1; else if (*fmt == '#') alt = 1;
            else if (*fmt == '0') zero = 1; else break;
        }
        /* width */
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; if (width < 0) { left = 1; width = -width; } }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        /* precision */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; if (prec < 0) prec = -1; }
            else { prec = 0; while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
        }
        /* length modifiers */
        int lng = 0;  /* 0=int 1=long 2=longlong, plus size_t/ptr via 1 on LP64 */
        if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }          /* hh,h -> int */
        else if (*fmt == 'l') { lng = 1; fmt++; if (*fmt == 'l') { lng = 2; fmt++; } }
        else if (*fmt == 'L') { lng = 2; fmt++; }
        else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { lng = 1; fmt++; }

        char conv = *fmt;
        char tmp[64]; int tn = 0; const char *body = tmp;
        char sign = 0; const char *prefix = ""; int numeric = 0;

        switch (conv) {
        case 'd': case 'i': {
            long long v = (lng == 2) ? va_arg(ap, long long)
                       : (lng == 1) ? va_arg(ap, long) : va_arg(ap, int);
            numeric = 1; unsigned long long uv;
            if (v < 0) { sign = '-'; uv = (unsigned long long)(-v); }
            else { sign = plus ? '+' : (space ? ' ' : 0); uv = (unsigned long long)v; }
            tn = u_to_str(uv, 10, 0, tmp);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'b': {
            unsigned long long uv = (lng == 2) ? va_arg(ap, unsigned long long)
                       : (lng == 1) ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            numeric = 1;
            unsigned base = (conv=='x'||conv=='X') ? 16 : (conv=='o') ? 8 : (conv=='b') ? 2 : 10;
            tn = u_to_str(uv, base, conv == 'X', tmp);
            if (alt && uv) { if (conv=='x') prefix = "0x"; else if (conv=='X') prefix = "0X"; else if (conv=='o') prefix = "0"; }
            break;
        }
        case 'p': {
            uintptr_t pv = (uintptr_t)va_arg(ap, void *);
            prefix = "0x"; numeric = 1;
            tn = u_to_str((uint64_t)pv, 16, 0, tmp);
            break;
        }
        case 'c': { tmp[0] = (char)va_arg(ap, int); tn = 1; break; }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int sl = 0; while (str[sl] && (prec < 0 || sl < prec)) sl++;
            /* width pad */
            int pad = width - sl;
            if (!left) while (pad-- > 0) s_put(s, ' ');
            s_str(s, str, sl);
            if (left)  while (pad-- > 0) s_put(s, ' ');
            continue;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double dv = va_arg(ap, double);
            char db[80];
            int dl = fmt_double(db, dv, prec, conv);
            /* sign already inside db; honour +/space on non-negative */
            int dstart = 0;
            if (db[0] != '-' && (plus || space)) { s_put(s, plus ? '+' : ' '); }
            int pad = width - dl;
            if (!left && !zero) while (pad-- > 0) s_put(s, ' ');
            if (!left &&  zero) {
                if (db[0]=='-') { s_put(s,'-'); dstart=1; }
                while (pad-- > 0) s_put(s, '0');
            }
            s_str(s, db + dstart, dl - dstart);
            if (left) while (pad-- > 0) s_put(s, ' ');
            continue;
        }
        case 'n': { int *np = va_arg(ap, int *); if (np) *np = (int)s->len; continue; }
        case '%': { s_put(s, '%'); continue; }
        case 0:   { s_put(s, '%'); return (int)s->len; }
        default:  { s_put(s, '%'); s_put(s, conv); continue; }
        }

        /* assemble numeric/char field with sign, prefix, padding */
        int plen = 0; while (prefix[plen]) plen++;
        int signw = sign ? 1 : 0;
        int digits = tn;
        int zeros = 0;
        if (numeric && prec >= 0 && prec > digits) zeros = prec - digits;  /* int precision = min digits */
        int bodyw = signw + plen + zeros + digits;
        int pad = width - bodyw;
        char padc = (zero && left == 0 && prec < 0 && numeric) ? '0' : ' ';

        if (!left && padc == ' ') while (pad-- > 0) s_put(s, ' ');
        if (sign) s_put(s, sign);
        for (int i = 0; i < plen; i++) s_put(s, prefix[i]);
        if (!left && padc == '0') while (pad-- > 0) s_put(s, '0');
        for (int i = 0; i < zeros; i++) s_put(s, '0');
        s_str(s, body, digits);
        if (left) while (pad-- > 0) s_put(s, ' ');
    }
    return (int)s->len;
}

/* ---- public API ---- */
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    sink s = { str, size, 0, 0 };
    int r = emit(&s, fmt, ap);
    if (str && size) s.buf[s.len < size ? s.len : size - 1] = 0;
    return r;
}
int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vsnprintf(str, size, fmt, ap); va_end(ap); return r;
}
int vsprintf(char *str, const char *fmt, va_list ap) { return vsnprintf(str, (size_t)-1, fmt, ap); }
int sprintf(char *str, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vsnprintf(str, (size_t)-1, fmt, ap); va_end(ap); return r;
}
int vprintf(const char *fmt, va_list ap) { sink s = { 0, 0, 0, 1 }; return emit(&s, fmt, ap); }
int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vprintf(fmt, ap); va_end(ap); return r;
}
int vasprintf(char **out, const char *fmt, va_list ap) {
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(0, 0, fmt, ap2); va_end(ap2);
    if (n < 0) { *out = 0; return -1; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { *out = 0; return -1; }
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    *out = buf;
    return n;
}
int asprintf(char **out, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vasprintf(out, fmt, ap); va_end(ap); return r;
}
