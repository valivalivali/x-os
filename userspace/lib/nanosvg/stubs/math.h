/* Stub math.h for NanoSVG freestanding build.
 * Math functions are provided by nanosvg_xos.c. */

#ifndef _XOS_STUB_MATH_H
#define _XOS_STUB_MATH_H

static inline int isnan(double x) {
    return x != x;
}

static inline int isnanf(float x) {
    return x != x;
}

#endif
