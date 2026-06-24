/* <math.h> for the BoltOS DOOM port (dg_libc.c). DOOM only really uses
 * sin/tan/atan at table-build time; the rest are provided for completeness. */
#pragma once

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double sqrt(double x);
double pow(double b, double e);
double exp(double x);
double log(double x);
double fmod(double a, double b);

float  fabsf(float x);
float  sqrtf(float x);
