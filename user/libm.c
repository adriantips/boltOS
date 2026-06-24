/* ===========================================================================
 *  BoltOS userland libm  -  hardware double-precision math (SSE2).
 *  Enabled now that the kernel runs with CR4.OSFXSR and the scheduler
 *  FXSAVE/FXRSTORs FPU state across switches. Accuracy is "good enough for
 *  applications" (range-reduced minimax / Taylor), not bit-exact libm.
 * ========================================================================= */
#include "math.h"

union du { double d; unsigned long u; };

double fabs(double x)  { union du v = { x }; v.u &= 0x7fffffffffffffffUL; return v.d; }
double copysign(double x, double y) {
    union du a = { x }, b = { y };
    a.u = (a.u & 0x7fffffffffffffffUL) | (b.u & 0x8000000000000000UL);
    return a.d;
}
double sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }

int    isnan(double x) { return x != x; }
int    isinf(double x) { union du v = { x }; return (v.u & 0x7fffffffffffffffUL) == 0x7ff0000000000000UL; }

double trunc(double x) {
    if (fabs(x) >= 4503599627370496.0 || x != x) return x;   /* >= 2^52: already integral */
    long long i = (long long)x;
    return (double)i;
}
double floor(double x) { double t = trunc(x); return (t > x) ? t - 1.0 : t; }
double ceil (double x) { double t = trunc(x); return (t < x) ? t + 1.0 : t; }
double round(double x) { return (x < 0.0) ? ceil(x - 0.5) : floor(x + 0.5); }
double fmod(double x, double y) {
    if (y == 0.0 || x != x || y != y || isinf(x)) return x != x ? x : (x - x) / (x - x);
    double r = x - trunc(x / y) * y;
    return r;
}

/* --- trig: reduce to [-pi/4, pi/4] then minimax-ish Taylor ----------------- */
#define PI      3.14159265358979323846
#define TWO_PI  6.28318530717958647692
#define HALF_PI 1.57079632679489661923

static double sin_kernel(double x) {            /* |x| <= pi/4 */
    double x2 = x * x;
    /* Taylor to x^11 */
    return x * (1.0 + x2 * (-1.0/6 + x2 * (1.0/120 + x2 * (-1.0/5040
             + x2 * (1.0/362880 - x2 * (1.0/39916800))))));
}
static double cos_kernel(double x) {            /* |x| <= pi/4 */
    double x2 = x * x;
    return 1.0 + x2 * (-0.5 + x2 * (1.0/24 + x2 * (-1.0/720 + x2 * (1.0/40320
             + x2 * (-1.0/3628800)))));
}
double sin(double x) {
    if (x != x || isinf(x)) return (x - x) / (x - x);
    double q = floor(x / HALF_PI + 0.5);            /* nearest multiple of pi/2 */
    double r = x - q * HALF_PI;
    int k = ((int)((long long)q)) & 3;
    if (k < 0) k += 4;
    switch (k) {
        case 0: return  sin_kernel(r);
        case 1: return  cos_kernel(r);
        case 2: return -sin_kernel(r);
        default:return -cos_kernel(r);
    }
}
double cos(double x) { return sin(x + HALF_PI); }
double tan(double x) { double c = cos(x); return (c == 0.0) ? copysign(1e308, sin(x)) : sin(x) / c; }

/* --- exp / log / pow ------------------------------------------------------- */
double exp(double x) {
    if (x != x) return x;
    if (x >  709.0) return 1e308 * 1e308;          /* +inf */
    if (x < -745.0) return 0.0;
    /* exp(x) = 2^k * exp(r), k = round(x/ln2), r in [-ln2/2, ln2/2] */
    double k = floor(x / 0.6931471805599453 + 0.5);
    double r = x - k * 0.6931471805599453;
    double t = 1.0, term = 1.0;
    for (int n = 1; n <= 14; n++) { term *= r / n; t += term; }
    /* scale by 2^k via integer exponent assembly into a double */
    union du s; long e = (long)k + 1023;
    if (e <= 0)    return 0.0;
    if (e >= 2047) return 1e308 * 1e308;
    s.u = (unsigned long)e << 52;
    return t * s.d;
}
double log(double x) {
    if (x < 0.0 || x != x) return (x - x) / (x - x);   /* nan */
    if (x == 0.0) return -1e308 * 1e308;               /* -inf */
    /* x = m * 2^e, m in [1,2); log = e*ln2 + log(m) */
    union du v = { x };
    long e = (long)((v.u >> 52) & 0x7ff) - 1023;
    v.u = (v.u & 0x000fffffffffffffUL) | 0x3ff0000000000000UL;   /* m in [1,2) */
    double m = v.d;
    /* atanh series around 1: log(m) = 2*(s + s^3/3 + s^5/5 ...), s=(m-1)/(m+1) */
    double s = (m - 1.0) / (m + 1.0), s2 = s * s, sum = 0.0, p = s;
    for (int n = 1; n <= 19; n += 2) { sum += p / n; p *= s2; }
    return e * 0.6931471805599453 + 2.0 * sum;
}
double log2(double x)  { return log(x) * 1.4426950408889634; }
double log10(double x) { return log(x) * 0.4342944819032518; }
double pow(double b, double e) {
    if (e == 0.0) return 1.0;
    if (b == 0.0) return (e > 0.0) ? 0.0 : 1e308 * 1e308;
    /* integer exponent: exact-ish by squaring */
    if (e == trunc(e) && fabs(e) < 1024.0) {
        long n = (long)e; double r = 1.0, base = b;
        unsigned long m = (n < 0) ? (unsigned long)(-n) : (unsigned long)n;
        while (m) { if (m & 1) r *= base; base *= base; m >>= 1; }
        return (n < 0) ? 1.0 / r : r;
    }
    if (b < 0.0) return (b - b) / (b - b);          /* nan: non-integer pow of negative */
    return exp(e * log(b));
}

/* --- inverse trig ---------------------------------------------------------- */
double atan(double x) {
    int neg = x < 0.0; if (neg) x = -x;
    /* Halve the argument k times via atan(x)=2*atan(x/(1+sqrt(1+x^2))) so the
     * series converges fast even at x=1 (where the naive Taylor crawls). */
    int k = 0;
    while (x > 0.2) { x = x / (1.0 + sqrt(1.0 + x * x)); k++; }
    double x2 = x * x, sum = 0.0, p = x;
    for (int n = 1; n <= 15; n += 2) { sum += (((n / 2) & 1) ? -1.0 : 1.0) * p / n; p *= x2; }
    sum *= (double)(1 << k);
    return neg ? -sum : sum;
}
double atan2(double y, double x) {
    if (x > 0.0) return atan(y / x);
    if (x < 0.0) return atan(y / x) + (y >= 0.0 ? PI : -PI);
    if (y > 0.0) return HALF_PI;
    if (y < 0.0) return -HALF_PI;
    return 0.0;
}
double asin(double x) { if (x <= -1.0) return -HALF_PI; if (x >= 1.0) return HALF_PI; return atan(x / sqrt(1.0 - x * x)); }
double acos(double x) { return HALF_PI - asin(x); }

/* float wrappers */
float  sqrtf(float x) { return (float)sqrt(x); }
float  sinf (float x) { return (float)sin(x);  }
float  cosf (float x) { return (float)cos(x);  }
float  fabsf(float x) { return (float)fabs(x); }
float  powf (float b, float e) { return (float)pow(b, e); }
float  expf (float x) { return (float)exp(x); }
float  logf (float x) { return (float)log(x); }
float  floorf(float x){ return (float)floor(x);}
float  ceilf (float x){ return (float)ceil(x); }
