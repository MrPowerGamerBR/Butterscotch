#ifndef _BS_MATH_COMPAT_H_
#define _BS_MATH_COMPAT_H_

#include <math.h>
#include "real_type.h"

/* This header is for compatibility with systems that don't have full C99 math libraries. */

/*
 * This fmin and fmax don't handle some semantics with negative zero correctly.
 * Handling them correctly would add complexity and hurt performance and it probably doesn't matter anyway.
 */
#if defined(NO_FMIN) || defined(USE_FIXED_REALS)

#undef GMLReal_fmin
static inline GMLReal GMLReal_fmin(GMLReal a, GMLReal b) {
    if (a != a) return b;
    if (b != b) return a;
    return a < b ? a : b;
}

#endif

#if defined(NO_FMAX) || defined(USE_FIXED_REALS)

#undef GMLReal_fmax
static inline GMLReal GMLReal_fmax(GMLReal a, GMLReal b) {
    if (a != a) return b;
    if (b != b) return a;
    return a > b ? a : b;
}

#endif

#ifdef USE_FIXED_REALS

#undef GMLReal_fabs
static inline GMLReal GMLReal_fabs(GMLReal x) {
    if (x < 0)
        return -x;
    return x;
}

#endif

// TODO: make this compatible with fixed reals
#ifdef NO_ROUND

#undef GMLReal_round
static inline GMLReal GMLReal_round(GMLReal x) {
    if (x >= 9007199254740992.0 || x <= -9007199254740992.0) return x;
    if (x >= 0.0) return (GMLReal)((int64_t)(x + 0.5));
    else          return (GMLReal)((int64_t)(x - 0.5));
}

#endif

#ifdef NO_LOG2

#undef GMLReal_log2
static inline GMLReal GMLReal_log2(GMLReal x) { return log(x) * 1.4426950408889634; }

#endif

// TODO: make this compatible with fixed reals
#ifdef NO_LROUND

static inline long lround(double x) {
    if (x >= 9007199254740992.0 || x <= -9007199254740992.0) return (long)x;
    if (x >= 0.0) return (long)((int64_t)(x + 0.5));
    else          return (long)((int64_t)(x - 0.5));
}

#endif

#ifdef NO_SQRTF

static inline float sqrtf(float x) {
    return sqrt(x);
}

#endif

#ifdef NO_FABSF

static inline float fabsf(float x) {
    return fabs(x);
}

#endif

#ifdef NO_FMODF

static inline float fmodf(float x, float y) {
    return fmod(x, y);
}

#endif

#ifdef NO_SINF

static inline float sinf(float x) { return sin(x); }

#endif

#ifdef NO_COSF

static inline float cosf(float x) { return cos(x); }

#endif

#ifdef NO_FLOORF

static inline float floorf(float x) { return floor(x); }

#endif

#ifdef NO_ROUNDF

static inline float roundf(float x) {
    if (x >= 2147483648.0f || x <= -2147483648.0f) return x;
    if (x >= 0.0f) return (float)((int32_t)(x + 0.5f));
    else           return (float)((int32_t)(x - 0.5f));
}

#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#endif /* _BS_MATH_COMPAT_H_ */
