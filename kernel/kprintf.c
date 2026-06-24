#include <stdint.h>
#include <stdarg.h>
#include "kprintf.h"
#include "io.h"

/* render an unsigned value into buf (no NUL); returns the digit count */
static int u_to_dec(char *buf, uint64_t v) {
    char t[24]; int i = 0;
    if (!v) { buf[0] = '0'; return 1; }
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    for (int k = 0; k < i; k++) buf[k] = t[i - 1 - k];
    return i;
}
static int u_to_hex(char *buf, uint64_t v) {
    static const char *h = "0123456789abcdef";
    char t[16]; int i = 0;
    if (!v) { buf[0] = '0'; return 1; }
    while (v) { t[i++] = h[v & 0xF]; v >>= 4; }
    for (int k = 0; k < i; k++) buf[k] = t[i - 1 - k];
    return i;
}

/* printf with flags/width: supports %[-][0][width][l..]{s,c,d,i,u,x,p,%}.
 * `-` left-justifies, `0` zero-pads numerics, width is the minimum field
 * width. Precision is not supported. */
void kprintf(const char *f, ...) {
    va_list ap; va_start(ap, f);
    for (; *f; f++) {
        if (*f != '%') { kputc(*f); continue; }
        f++;
        int left = 0, zero = 0, width = 0;
        for (;;) { if (*f == '-') { left = 1; f++; } else if (*f == '0') { zero = 1; f++; } else break; }
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        int l = 0; while (*f == 'l') { l++; f++; }

        char tmp[24]; const char *out = tmp; int n = 0, numeric = 0, neg = 0;
        switch (*f) {
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; out = s; while (out[n]) n++; } break;
        case 'c':   tmp[0] = (char)va_arg(ap, int); n = 1; break;
        case 'd': case 'i': {
            long v = l ? va_arg(ap, long) : va_arg(ap, int);
            numeric = 1; if (v < 0) { neg = 1; v = -v; }
            n = u_to_dec(tmp, (uint64_t)v);
        } break;
        case 'u': { unsigned long v = l ? va_arg(ap, unsigned long) : va_arg(ap, unsigned); numeric = 1; n = u_to_dec(tmp, v); } break;
        case 'x': { unsigned long v = l ? va_arg(ap, unsigned long) : va_arg(ap, unsigned); numeric = 1; n = u_to_hex(tmp, v); } break;
        case 'p': { uint64_t v = (uint64_t)va_arg(ap, void *); char hb[16]; int hn = u_to_hex(hb, v);
                    kputc('0'); kputc('x'); for (int i = hn; i < 16; i++) kputc('0'); for (int i = 0; i < hn; i++) kputc(hb[i]); } continue;
        case '%': kputc('%'); continue;
        default:  kputc('%'); kputc(*f); continue;
        }

        int total = n + neg;
        int pad = width > total ? width - total : 0;
        if (neg && zero && !left) { kputc('-'); neg = 0; }     /* sign before zero-pad */
        if (!left) { char pc = (zero && numeric) ? '0' : ' '; for (int i = 0; i < pad; i++) kputc(pc); }
        if (neg) kputc('-');
        for (int i = 0; i < n; i++) kputc(out[i]);
        if (left) for (int i = 0; i < pad; i++) kputc(' ');
    }
    va_end(ap);
}

void panic(const char *msg) {
    kprintf("\n*** KERNEL PANIC: %s ***\n", msg);
    for (;;) __asm__ volatile("cli; hlt");
}
