/* ===========================================================================
 *  libc/math.c -- double math on SSE2 (the kernel keeps SSE; only x87 is off).
 *
 *  __builtin_{fabs,copysign,sqrt} lower to single SSE instructions, so they are
 *  safe to lean on. floor/ceil/trunc/round are written with integer casts rather
 *  than __builtin_floor (which would emit a call to floor() -- SSE4.1 roundsd is
 *  not in the x86-64 baseline -- and recurse). Transcendentals use range
 *  reduction + series; browser-grade precision, not libm-grade.
 * ===========================================================================*/
#include <stdint.h>
#include "math.h"

typedef union { double d; uint64_t u; } du;

int __fpclassifyd(double x) {
    du v = { x };
    uint64_t exp = (v.u >> 52) & 0x7ff, man = v.u & 0xfffffffffffffull;
    if (exp == 0)     return man ? FP_SUBNORMAL : FP_ZERO;
    if (exp == 0x7ff) return man ? FP_NAN : FP_INFINITE;
    return FP_NORMAL;
}

double fabs(double x)              { return __builtin_fabs(x); }
double copysign(double x, double y){ return __builtin_copysign(x, y); }
double sqrt(double x)             { return __builtin_sqrt(x); }

double trunc(double x) {
    if (!__builtin_isfinite(x)) return x;
    if (x < 9.2233720368547758e18 && x > -9.2233720368547758e18)
        return (double)(long long)x;          /* cvttsd2si: toward zero */
    return x;
}
double floor(double x) { double t = trunc(x); return (t > x) ? t - 1.0 : t; }
double ceil(double x)  { double t = trunc(x); return (t < x) ? t + 1.0 : t; }
double round(double x) { return x < 0 ? ceil(x - 0.5) : floor(x + 0.5); }
double rint(double x)  { /* round-half-to-even */
    double f = floor(x), d = x - f;
    if (d < 0.5) return f; if (d > 0.5) return f + 1.0;
    return ((long long)f & 1) ? f + 1.0 : f;
}
double nearbyint(double x) { return rint(x); }

double fmod(double x, double y) {
    if (y == 0.0 || !__builtin_isfinite(x)) return __builtin_nan("");
    if (!__builtin_isfinite(y)) return x;
    double r = x - trunc(x / y) * y;
    return r;
}
double remainder(double x, double y) {
    if (y == 0.0) return __builtin_nan("");
    double n = rint(x / y);
    return x - n * y;
}
double modf(double x, double *ip) { double i = trunc(x); *ip = i; return x - i; }

double ldexp(double x, int e) {
    /* multiply by 2^e via the exponent field; chunk to stay in range */
    while (e > 1023) { x *= 0x1p1023; e -= 1023; }
    while (e < -1022) { x *= 0x1p-1022; e += 1022; }
    du v; v.u = (uint64_t)(e + 1023) << 52;
    return x * v.d;
}
double scalbn(double x, int e) { return ldexp(x, e); }
double frexp(double x, int *e) {
    if (x == 0.0 || !__builtin_isfinite(x)) { *e = 0; return x; }
    du v = { x };
    int ex = (int)((v.u >> 52) & 0x7ff);
    if (ex == 0) { x *= 0x1p54; v.d = x; ex = (int)((v.u >> 52) & 0x7ff) - 54; }
    *e = ex - 1022;
    v.u = (v.u & ~(0x7ffull << 52)) | (1022ull << 52);
    return v.d;
}

double fmax(double a, double b) { return (a > b || __builtin_isnan(b)) ? a : b; }
double fmin(double a, double b) { return (a < b || __builtin_isnan(b)) ? a : b; }
double fdim(double a, double b) { return a > b ? a - b : 0.0; }
double nextafter(double x, double y) {
    if (x == y) return y;
    du v = { x };
    if (x == 0.0) { v.u = 1; return y > 0 ? v.d : -v.d; }
    if ((x < y) == (x > 0)) v.u++; else v.u--;
    return v.d;
}

/* ---- exp / log ---- */
double exp(double x) {
    if (__builtin_isnan(x)) return x;
    if (x > 709.0) return INFINITY;
    if (x < -745.0) return 0.0;
    int k = (int)round(x * 1.4426950408889634);     /* x / ln2 */
    double r = x - k * 0.6931471805599453;
    double term = 1.0, sum = 1.0;
    for (int n = 1; n < 18; n++) { term *= r / n; sum += term; }
    return ldexp(sum, k);
}
double exp2(double x) { return exp(x * 0.6931471805599453); }
double expm1(double x){ return (x > -0.5 && x < 0.5) ? exp(x) - 1.0 : exp(x) - 1.0; }
double log(double x) {
    if (x < 0.0) return __builtin_nan("");
    if (x == 0.0) return -INFINITY;
    if (!__builtin_isfinite(x)) return x;
    int e; double m = frexp(x, &e);                  /* m in [0.5,1) */
    if (m < 0.7071067811865476) { m *= 2.0; e--; }   /* center around 1 */
    double t = (m - 1.0) / (m + 1.0), t2 = t * t, s = 0.0, p = t;
    for (int n = 1; n < 30; n += 2) { s += p / n; p *= t2; }
    return 2.0 * s + e * 0.6931471805599453;
}
double log2(double x)  { return log(x) * 1.4426950408889634; }
double log10(double x) { return log(x) * 0.4342944819032518; }
double log1p(double x) { return log(1.0 + x); }
double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 1.0) return 1.0;
    if (x == 0.0) return y > 0 ? 0.0 : INFINITY;
    /* exact integer exponent (preserves sign for negative bases) */
    if (y == trunc(y) && y > -1024 && y < 1024) {
        long long n = (long long)y; int neg = n < 0; if (neg) n = -n;
        double r = 1.0, b = x; while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
        return neg ? 1.0 / r : r;
    }
    if (x < 0.0) return __builtin_nan("");
    return exp(y * log(x));
}
double cbrt(double x) {
    if (x == 0.0) return 0.0;
    double a = fabs(x), g = exp(log(a) / 3.0);
    for (int i = 0; i < 3; i++) g = (2.0 * g + a / (g * g)) / 3.0;   /* Newton */
    return x < 0 ? -g : g;
}
double hypot(double a, double b) { a = fabs(a); b = fabs(b);
    if (a < b) { double t = a; a = b; b = t; }
    if (a == 0.0) return 0.0; double r = b / a; return a * sqrt(1.0 + r * r); }

/* ---- trig ---- */
static double sin_core(double x) {            /* x already in [-pi,pi] */
    double x2 = x * x, term = x, sum = x;
    for (int n = 1; n < 11; n++) { term *= -x2 / ((2*n) * (2*n + 1)); sum += term; }
    return sum;
}
double sin(double x) {
    double t = fmod(x, 6.283185307179586);
    if (t > 3.141592653589793) t -= 6.283185307179586;
    if (t < -3.141592653589793) t += 6.283185307179586;
    return sin_core(t);
}
double cos(double x) { return sin(x + 1.5707963267948966); }
double tan(double x) { double c = cos(x); return c == 0.0 ? INFINITY : sin(x) / c; }
double atan(double x) {
    int neg = x < 0; if (neg) x = -x;
    int inv = x > 1.0; if (inv) x = 1.0 / x;
    double x2 = x * x, term = x, sum = x;
    for (int n = 1; n < 40; n++) { term *= -x2; sum += term / (2*n + 1); }
    if (inv) sum = 1.5707963267948966 - sum;
    return neg ? -sum : sum;
}
double atan2(double y, double x) {
    if (x > 0) return atan(y / x);
    if (x < 0) return atan(y / x) + (y >= 0 ? 3.141592653589793 : -3.141592653589793);
    if (y > 0) return 1.5707963267948966;
    if (y < 0) return -1.5707963267948966;
    return 0.0;
}
double asin(double x) { if (x <= -1.0) return -1.5707963267948966; if (x >= 1.0) return 1.5707963267948966;
    return atan(x / sqrt(1.0 - x * x)); }
double acos(double x) { return 1.5707963267948966 - asin(x); }
double sinh(double x) { double e = exp(x); return (e - 1.0 / e) * 0.5; }
double cosh(double x) { double e = exp(x); return (e + 1.0 / e) * 0.5; }
double tanh(double x) { if (x > 20) return 1.0; if (x < -20) return -1.0; double e = exp(2.0 * x); return (e - 1.0) / (e + 1.0); }

/* ---- float aliases ---- */
float fabsf(float x){ return __builtin_fabsf(x); }
float sqrtf(float x){ return (float)__builtin_sqrt((double)x); }
float floorf(float x){ return (float)floor(x); }
float ceilf(float x){ return (float)ceil(x); }
float roundf(float x){ return (float)round(x); }
float truncf(float x){ return (float)trunc(x); }
float fmodf(float x, float y){ return (float)fmod(x, y); }
float powf(float x, float y){ return (float)pow(x, y); }
float expf(float x){ return (float)exp(x); }
float logf(float x){ return (float)log(x); }
float sinf(float x){ return (float)sin(x); }
float cosf(float x){ return (float)cos(x); }
float tanf(float x){ return (float)tan(x); }
float atan2f(float y, float x){ return (float)atan2(y, x); }
float ldexpf(float x, int e){ return (float)ldexp(x, e); }
float copysignf(float x, float y){ return __builtin_copysignf(x, y); }
