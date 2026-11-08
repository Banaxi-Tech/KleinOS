#pragma once
// Freestanding math library (float + double). SSE-backed where possible.

#define M_PI 3.14159265358979323846
#define M_PI_F 3.14159265f

extern "C" {
double sqrt(double x);
float sqrtf(float x);
double fabs(double x);
float fabsf(float x);
double floor(double x);
float floorf(float x);
double ceil(double x);
float ceilf(float x);
double round(double x);
float roundf(float x);
double trunc(double x);
double fmod(double x, double y);
float fmodf(float x, float y);
double sin(double x);
double cos(double x);
double tan(double x);
float sinf(float x);
float cosf(float x);
double atan(double x);
double atan2(double y, double x);
float atan2f(float y, float x);
double asin(double x);
double acos(double x);
double exp(double x);
float expf(float x);
double log(double x);
double log10(double x);
double log2(double x);
double pow(double x, double y);
float powf(float x, float y);
double tanh(double x);
float tanhf(float x);
double hypot(double x, double y);
}

template <typename T> static inline T kmin(T a, T b) { return a < b ? a : b; }
template <typename T> static inline T kmax(T a, T b) { return a > b ? a : b; }
template <typename T> static inline T kclamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smoothstepf(float e0, float e1, float x) {
    float t = kclamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
