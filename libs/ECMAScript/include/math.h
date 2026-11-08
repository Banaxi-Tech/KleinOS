#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#define NAN __builtin_nan("")
#define INFINITY __builtin_inf()
#define HUGE_VAL __builtin_huge_val()
#define M_PI 3.14159265358979323846
#define M_E 2.71828182845904523536
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_LOG2E 1.4426950408889634074
#define M_LOG10E 0.43429448190325182765
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(0, 1, 4, 3, 2, x)
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define isnormal(x) __builtin_isnormal(x)
double sqrt(double); double fabs(double); double floor(double); double ceil(double); double round(double); double trunc(double);
double fmod(double, double); double sin(double); double cos(double); double tan(double); double asin(double); double acos(double);
double atan(double); double atan2(double, double); double exp(double); double log(double); double log10(double); double log2(double);
double pow(double, double); double sinh(double); double cosh(double); double tanh(double); double asinh(double); double acosh(double);
double atanh(double); double expm1(double); double log1p(double); double cbrt(double); double hypot(double, double); double exp2(double);
double fmin(double, double); double fmax(double, double); double copysign(double, double); double rint(double); double nearbyint(double);
long lrint(double); long long llrint(double); double frexp(double, int*); double ldexp(double, int); double modf(double, double*);
double scalbn(double, int); float fabsf(float); float sqrtf(float); double remainder(double, double);
#ifdef __cplusplus
}
#endif
