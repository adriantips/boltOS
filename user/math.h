#pragma once
/* BoltOS userland <math.h> — hardware double math (see user/libm.c). */

#define M_PI    3.14159265358979323846
#define M_E     2.71828182845904523536
#define M_SQRT2 1.41421356237309504880
#define NAN     (__builtin_nanf(""))
#define INFINITY (__builtin_inff())
#define HUGE_VAL (__builtin_huge_val())

double fabs(double);
double copysign(double, double);
double sqrt(double);
double trunc(double);
double floor(double);
double ceil(double);
double round(double);
double fmod(double, double);
double sin(double);
double cos(double);
double tan(double);
double asin(double);
double acos(double);
double atan(double);
double atan2(double, double);
double exp(double);
double log(double);
double log2(double);
double log10(double);
double pow(double, double);
int    isnan(double);
int    isinf(double);

float  sqrtf(float);
float  sinf(float);
float  cosf(float);
float  fabsf(float);
float  powf(float, float);
float  expf(float);
float  logf(float);
float  floorf(float);
float  ceilf(float);
