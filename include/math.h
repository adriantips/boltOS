#pragma once
/* ===========================================================================
 *  <math.h> -- BoltOS freestanding C library (libc/math.c)
 *  double math runs on SSE2 (the kernel builds -msse -msse2; only x87/-m80387
 *  is disabled). sqrt uses the hardware sqrtsd; transcendentals are portable
 *  series/range-reduction implementations -- no x87, no long double.
 * ===========================================================================*/

#define M_PI     3.14159265358979323846
#define M_PI_2   1.57079632679489661923
#define M_PI_4   0.78539816339744830962
#define M_E      2.7182818284590452354
#define M_SQRT2  1.41421356237309504880
#define M_LN2    0.69314718055994530942
#define M_LN10   2.30258509299404568402
#define M_LOG2E  1.44269504088896340736

#define HUGE_VAL  __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define INFINITY  __builtin_inff()
#define NAN       __builtin_nanf("")
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4

int    __fpclassifyd(double x);
#define fpclassify(x) __fpclassifyd((double)(x))
#define isnan(x)   __builtin_isnan(x)
#define isinf(x)   __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) (fpclassify(x) == FP_NORMAL)
#define signbit(x) __builtin_signbit(x)

double fabs(double x);
double sqrt(double x);
double cbrt(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
double rint(double x);
double nearbyint(double x);
double fmod(double x, double y);
double remainder(double x, double y);
double modf(double x, double *iptr);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double scalbn(double x, int exp);
double copysign(double x, double y);
double nextafter(double x, double y);
double fdim(double x, double y);
double fmax(double x, double y);
double fmin(double x, double y);
double hypot(double x, double y);

double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double pow(double x, double y);

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);

/* float aliases (NetSurf image/SVG code occasionally uses the f-suffixed set) */
float  fabsf(float x);
float  sqrtf(float x);
float  floorf(float x);
float  ceilf(float x);
float  roundf(float x);
float  truncf(float x);
float  fmodf(float x, float y);
float  powf(float x, float y);
float  expf(float x);
float  logf(float x);
float  sinf(float x);
float  cosf(float x);
float  tanf(float x);
float  atan2f(float y, float x);
float  ldexpf(float x, int e);
float  copysignf(float x, float y);
