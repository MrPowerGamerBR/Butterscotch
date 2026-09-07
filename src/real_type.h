#ifndef _BS_REAL_TYPE_H_
#define _BS_REAL_TYPE_H_

#include "common.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "log.h"

#ifndef INFINITY
#define INFINITY ((float)1e39)
#endif

#undef isnan
#undef isinf
#define isnan(x) ((x) != (x))
#define isinf(x) (((double)x) == (double)INFINITY || ((double)x) == (double)-INFINITY)

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

#ifdef USE_FIXED_REALS

#ifndef __cplusplus
#error USE_FIXED_REALS requires compiling as C++
#endif

#include <type_traits>

template <typename T>
struct is_gml_integral : std::integral_constant<bool, std::is_integral<T>::value || std::is_enum<T>::value> {};

template <typename T>
struct is_gml_numeric : std::integral_constant<bool, std::is_arithmetic<T>::value || std::is_enum<T>::value> {};

typedef int64_t realint_t;

class GMLReal {
public:
    static const int FRAC_BITS = 12;
    static const realint_t REALINT_MIN = INT64_MIN;
    static const realint_t REALINT_MAX = INT64_MAX;

    GMLReal() = default;

    template <typename T, typename std::enable_if<is_gml_integral<T>::value, int>::type = 0>
    GMLReal(T i) : raw_((realint_t)i << FRAC_BITS) {}

    // Covers float and double. `v != v` detects NaN for either type.
    template <typename T, typename std::enable_if<std::is_floating_point<T>::value, int>::type = 0>
    GMLReal(T v) {
        if      (v != v)         raw_ =  REALINT_MIN;
        else if (v >=  INFINITY) raw_ =  REALINT_MAX;
        else if (v <= -INFINITY) raw_ = -REALINT_MAX;
        else raw_ = (realint_t)(v * (T)(((realint_t)1) << FRAC_BITS));
    }

    static GMLReal from_raw(realint_t r) {
        GMLReal f;
        f.raw_ = r;
        return f;
    }

    static GMLReal infinity()     { return from_raw(REALINT_MAX);  }
    static GMLReal neg_infinity() { return from_raw(-REALINT_MAX); }
    static GMLReal nan()          { return from_raw(REALINT_MIN);  }

    bool is_nan()          const { return raw_ ==  REALINT_MIN; }
    bool is_pos_infinite() const { return raw_ ==  REALINT_MAX; }
    bool is_neg_infinite() const { return raw_ == -REALINT_MAX; }
    bool is_infinite()     const { return is_pos_infinite() || is_neg_infinite(); }

    template <typename T, typename std::enable_if<is_gml_integral<T>::value, int>::type = 0>
    operator T() const { return (T)(raw_ >> FRAC_BITS); }

    template <typename T, typename std::enable_if<std::is_floating_point<T>::value, int>::type = 0>
    operator T() const {
        if (is_nan())          return NAN;
        if (is_pos_infinite()) return INFINITY;
        if (is_neg_infinite()) return -INFINITY;
        return (T)raw_ / (T)(((realint_t)1) << FRAC_BITS);
    }

    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    GMLReal operator+(T d) const { return *this + GMLReal(d); }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    GMLReal operator-(T d) const { return *this - GMLReal(d); }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    GMLReal operator*(T d) const { return *this * GMLReal(d); }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    GMLReal operator/(T d) const { return *this / GMLReal(d); }

    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    bool operator==(T d) const { return *this == GMLReal(d); }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    bool operator!=(T d) const { return *this != GMLReal(d); }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    bool operator< (T d) const { return *this <  GMLReal(d); }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    bool operator<=(T d) const { return *this <= GMLReal(d); }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    bool operator> (T d) const { return *this >  GMLReal(d); }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    bool operator>=(T d) const { return *this >= GMLReal(d); }

    GMLReal operator-() const {
        if (is_nan()) return *this;
        return from_raw(-raw_);
    }

    GMLReal operator+(const GMLReal& o) const {
        if (is_nan() || o.is_nan()) return nan();
        if (is_infinite() || o.is_infinite()) {
            if (is_infinite() && o.is_infinite() && raw_ != o.raw_)
                return nan(); // inf + -inf
            return is_infinite() ? *this : o;
        }
        realint_t sum = raw_ + o.raw_;
        if ((raw_ > 0 && o.raw_ > 0 && sum <= 0) || (raw_ < 0 && o.raw_ < 0 && sum >= 0))
            return raw_ > 0 ? infinity() : neg_infinity();
        return from_raw(sum);
    }

    GMLReal operator-(const GMLReal& o) const { return *this + (-o); }

    GMLReal operator*(const GMLReal& o) const {
        if (is_nan() || o.is_nan()) return nan();
        if (is_infinite() || o.is_infinite()) {
            if ((raw_ == 0 && o.is_infinite()) || (o.raw_ == 0 && is_infinite()))
                return nan(); // 0 * inf
            bool neg = (raw_ < 0) != (o.raw_ < 0);
            return neg ? neg_infinity() : infinity();
        }
        return from_raw((raw_ * o.raw_) >> FRAC_BITS);
    }

    GMLReal operator/(const GMLReal& o) const {
        if (is_nan() || o.is_nan()) return nan();
        if (is_infinite() && o.is_infinite())
            return nan(); // inf / inf
        if (o.is_infinite())
            return from_raw(0);
        if (is_infinite()) {
            bool neg = (raw_ < 0) != (o.raw_ < 0);
            return neg ? neg_infinity() : infinity();
        }
        if (o.raw_ == 0) {
            if (raw_ == 0) return nan(); // 0 / 0
            return raw_ < 0 ? neg_infinity() : infinity();
        }
        return from_raw((raw_ << FRAC_BITS) / o.raw_);
    }

    GMLReal& operator+=(const GMLReal& o) { *this = *this + o; return *this; }
    GMLReal& operator-=(const GMLReal& o) { *this = *this - o; return *this; }
    GMLReal& operator*=(const GMLReal& o) { *this = *this * o; return *this; }
    GMLReal& operator/=(const GMLReal& o) { *this = *this / o; return *this; }

    GMLReal& operator++()    { *this += GMLReal(1); return *this; }
    GMLReal& operator--()    { *this -= GMLReal(1); return *this; }
    GMLReal  operator++(int) { GMLReal tmp = *this; *this += GMLReal(1); return tmp; }
    GMLReal  operator--(int) { GMLReal tmp = *this; *this -= GMLReal(1); return tmp; }

    bool operator==(const GMLReal& o) const { return !is_nan() && !o.is_nan() && raw_ == o.raw_; }
    bool operator!=(const GMLReal& o) const { return  is_nan() ||  o.is_nan() || raw_ != o.raw_; }
    bool operator< (const GMLReal& o) const { return !is_nan() && !o.is_nan() && raw_ <  o.raw_; }
    bool operator<=(const GMLReal& o) const { return !is_nan() && !o.is_nan() && raw_ <= o.raw_; }
    bool operator> (const GMLReal& o) const { return !is_nan() && !o.is_nan() && raw_ >  o.raw_; }
    bool operator>=(const GMLReal& o) const { return !is_nan() && !o.is_nan() && raw_ >= o.raw_; }

    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    friend GMLReal operator+(T lhs, const GMLReal& rhs) { return GMLReal(lhs) + rhs; }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    friend GMLReal operator-(T lhs, const GMLReal& rhs) { return GMLReal(lhs) - rhs; }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    friend GMLReal operator*(T lhs, const GMLReal& rhs) { return GMLReal(lhs) * rhs; }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    friend GMLReal operator/(T lhs, const GMLReal& rhs) { return GMLReal(lhs) / rhs; }

    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    friend bool operator==(T lhs, const GMLReal& rhs) { return rhs == GMLReal(lhs); }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    friend bool operator!=(T lhs, const GMLReal& rhs) { return rhs != GMLReal(lhs); }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    friend bool operator< (T lhs, const GMLReal& rhs) { return rhs >  GMLReal(lhs); }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    friend bool operator<=(T lhs, const GMLReal& rhs) { return rhs >= GMLReal(lhs); }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    friend bool operator> (T lhs, const GMLReal& rhs) { return rhs <  GMLReal(lhs); }
    template <typename T, typename std::enable_if<is_gml_numeric<T>::value, int>::type = 0>
    friend bool operator>=(T lhs, const GMLReal& rhs) { return rhs <= GMLReal(lhs); }

private:
    realint_t raw_;
};

#else

typedef double GMLReal;

#endif

#define GMLReal_sin(x) sin((double)(x))
#define GMLReal_cos(x) cos((double)(x))
#define GMLReal_tan(x) tan((double)(x))
#define GMLReal_acos(x) acos((double)(x))
#define GMLReal_asin(x) asin((double)(x))
#define GMLReal_atan(x) atan((double)(x))
#define GMLReal_atan2(x,y) atan2((double)(x),(double)(y))
#define GMLReal_sqrt(x) sqrt((double)(x))
#define GMLReal_fabs(x) fabs((double)(x))
#define GMLReal_fmod(x,y) fmod((double)(x),(double)(y))
#define GMLReal_floor(x) floor((double)(x))
#define GMLReal_ceil(x) ceil((double)(x))
#define GMLReal_round(x) round((double)(x))
#define GMLReal_pow(x,y) pow((double)(x),(double)(y))
#define GMLReal_log(x) log((double)(x))
#define GMLReal_log2(x) log2((double)(x))
#define GMLReal_log10(x) log10((double)(x))
#define GMLReal_fmax(x,y) fmax((double)(x),(double)(y))
#define GMLReal_fmin(x,y) fmin((double)(x),(double)(y))
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

#endif /* _BS_REAL_TYPE_H_ */
