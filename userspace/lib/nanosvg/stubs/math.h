/* Stub math.h for NanoSVG freestanding build.
 * Math functions are provided by nanosvg_xos.c. */

#ifndef _XOS_STUB_MATH_H
#define _XOS_STUB_MATH_H

static inline int isnan(double x) { return x != x; }
static inline int isnanf(float x) { return x != x; }

float powf(float base, float exp);
float floorf(float x);
float ceilf(float x);
float roundf(float x);
float fabsf(float x);
float fmodf(float a, float b);
float cosf(float x);
float sinf(float x);
float tanf(float x);
float atan2f(float y, float x);
float acosf(float x);
float sqrtf(float x);

double pow(double base, double exp);
double floor(double x);
double ceil(double x);
double fabs(double x);
double fmod(double a, double b);
double cos(double x);
double sin(double x);
double atan2(double y, double x);
double acos(double x);
double sqrt(double x);
double round(double x);

#endif
