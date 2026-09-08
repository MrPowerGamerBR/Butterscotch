#ifndef _BS_REAL_TYPE_H_
#define _BS_REAL_TYPE_H_

#include "common.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef INFINITY
#define INFINITY ((float)1e39)
#endif

#ifdef NO_ISNAN
#define isnan(x) (x != x)
#endif
#ifdef NO_ISINF
#define isinf(x) ((x) == INFINITY || (x) == -INFINITY)
#endif

#ifdef USE_FLOAT_REALS

typedef float GMLReal;

#define GMLReal_sin sinf
#define GMLReal_cos cosf
#define GMLReal_tan tanf
#define GMLReal_acos acosf
#define GMLReal_asin asinf
#define GMLReal_atan atanf
#define GMLReal_atan2 atan2f
#define GMLReal_sqrt sqrtf
#define GMLReal_fabs fabsf
#define GMLReal_fmod fmodf
#define GMLReal_floor floorf
#define GMLReal_ceil ceilf
#define GMLReal_round roundf
#define GMLReal_pow powf
#define GMLReal_log logf
#define GMLReal_log2 log2f
#define GMLReal_log10 log10f
#define GMLReal_fmax fmaxf
#define GMLReal_fmin fminf
#define GMLReal_nextafter nextafterf
#define GMLReal_strtod(str, endptr) strtof(str, endptr)

#else

typedef double GMLReal;

#define GMLReal_sin sin
#define GMLReal_cos cos
#define GMLReal_tan tan
#define GMLReal_acos acos
#define GMLReal_asin asin
#define GMLReal_atan atan
#define GMLReal_atan2 atan2
#define GMLReal_sqrt sqrt
#define GMLReal_fabs fabs
#define GMLReal_fmod fmod
#define GMLReal_floor floor
#define GMLReal_ceil ceil
#define GMLReal_round round
#define GMLReal_pow pow
#define GMLReal_log log
#define GMLReal_log2 log2
#define GMLReal_log10 log10
#define GMLReal_fmax fmax
#define GMLReal_fmin fmin
#define GMLReal_nextafter nextafter
#define GMLReal_strtod(str, endptr) strtod(str, endptr)

#endif

// Round-half-to-even (banker's rounding).
// While the original runner uses "llrint(double)", we use our own banker's rounding implementation to avoid quirks in specific platforms (like the PlayStation 2) having different llrint rounding implementations.
static inline GMLReal GMLReal_bankersRound(GMLReal v) {
    if (isnan(v) || isinf(v)) return v;
    GMLReal f = GMLReal_floor(v);
    GMLReal frac = v - f;
    if (0.5 > frac) return f;
    if (frac > 0.5) return f + 1.0;
    // Exactly halfway: round to the even neighbor.
    int64_t fi = (int64_t) f;
    return (fi & 1) == 0 ? f : f + 1.0;
}

static inline GMLReal GMLReal_clamp(GMLReal val, GMLReal min, GMLReal max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}
#endif /* _BS_REAL_TYPE_H_ */
