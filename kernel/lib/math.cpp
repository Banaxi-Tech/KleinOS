#include "lib/math.h"
#include <stdint.h>

extern "C" {

double sqrt(double x) { double r; asm("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
float sqrtf(float x) { float r; asm("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }
double fabs(double x) { return x < 0 ? -x : x; }
float fabsf(float x) { return x < 0 ? -x : x; }

double trunc(double x) {
    if (x > 9.2e18 || x < -9.2e18 || x != x) return x;
    return (double)(long long)x;
}
double floor(double x) {
    double t = trunc(x);
    return (t > x) ? t - 1 : t;
}
float floorf(float x) { return (float)floor(x); }
double ceil(double x) {
    double t = trunc(x);
    return (t < x) ? t + 1 : t;
}
float ceilf(float x) { return (float)ceil(x); }
double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }
float roundf(float x) { return (float)round(x); }

double fmod(double x, double y) {
    if (y == 0) return 0;
    double q = trunc(x / y);
    return x - q * y;
}
float fmodf(float x, float y) { return (float)fmod(x, y); }

// sin/cos via range reduction + Taylor polynomial on [-pi/4, pi/4]
static double sin_poly(double x) {
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156))))));
}
static double cos_poly(double x) {
    double x2 = x * x;
    return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 * (1 - x2 / 132)))));
}

double sin(double x) {
    const double half_pi = M_PI / 2;
    double k = round(x / half_pi);
    double r = x - k * half_pi;
    long long q = ((long long)k) & 3;
    switch (q) {
    case 0: return sin_poly(r);
    case 1: return cos_poly(r);
    case 2: return -sin_poly(r);
    default: return -cos_poly(r);
    }
}
double cos(double x) { return sin(x + M_PI / 2); }
double tan(double x) { double c = cos(x); return c == 0 ? 1e300 : sin(x) / c; }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }

double atan(double x) {
    // reduce to [0,1], then use a rational approximation refined with series
    bool neg = x < 0;
    if (neg) x = -x;
    bool inv = x > 1;
    if (inv) x = 1 / x;
    // further reduce: atan(x) = atan(c) + atan((x-c)/(1+xc)) with c = 0.5 when x > 0.4
    double base = 0;
    if (x > 0.4) {
        const double c = 0.5;
        base = 0.4636476090008061;   // atan(0.5)
        x = (x - c) / (1 + x * c);
    }
    double x2 = x * x, term = x, sum = x;
    for (int n = 1; n < 18; n++) {
        term *= -x2;
        sum += term / (2 * n + 1);
    }
    double r = base + sum;
    if (inv) r = M_PI / 2 - r;
    return neg ? -r : r;
}

double atan2(double y, double x) {
    if (x > 0) return atan(y / x);
    if (x < 0) return y >= 0 ? atan(y / x) + M_PI : atan(y / x) - M_PI;
    if (y > 0) return M_PI / 2;
    if (y < 0) return -M_PI / 2;
    return 0;
}
float atan2f(float y, float x) { return (float)atan2(y, x); }

double asin(double x) {
    if (x >= 1) return M_PI / 2;
    if (x <= -1) return -M_PI / 2;
    return atan(x / sqrt(1 - x * x));
}
double acos(double x) { return M_PI / 2 - asin(x); }

double exp(double x) {
    if (x > 700) return 1e300;
    if (x < -700) return 0;
    // e^x = 2^k * e^r, r in [-ln2/2, ln2/2]
    const double ln2 = 0.6931471805599453;
    double k = round(x / ln2);
    double r = x - k * ln2;
    double term = 1, sum = 1;
    for (int n = 1; n < 20; n++) {
        term *= r / n;
        sum += term;
    }
    long long ki = (long long)k;
    union { double d; uint64_t u; } u;
    u.u = (uint64_t)(ki + 1023) << 52;
    return sum * u.d;
}
float expf(float x) { return (float)exp(x); }

double log(double x) {
    if (x <= 0) return -1e300;
    union { double d; uint64_t u; } u{x};
    long long e = (long long)((u.u >> 52) & 0x7FF) - 1023;
    u.u = (u.u & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;   // mantissa in [1,2)
    double m = u.d;
    if (m > 1.41421356) { m /= 2; e++; }
    // log(m) = 2 atanh((m-1)/(m+1))
    double s = (m - 1) / (m + 1), s2 = s * s, term = s, sum = 0;
    for (int n = 0; n < 30; n++) {
        sum += term / (2 * n + 1);
        term *= s2;
    }
    return 2 * sum + e * 0.6931471805599453;
}
double log10(double x) { return log(x) / 2.302585092994046; }
double log2(double x) { return log(x) / 0.6931471805599453; }

double pow(double x, double y) {
    if (y == 0) return 1;
    if (x == 0) return 0;
    if (y == trunc(y) && fabs(y) < 1e9) {
        long long n = (long long)y;
        bool neg = n < 0;
        if (neg) n = -n;
        double r = 1, b = x;
        while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
        return neg ? 1 / r : r;
    }
    if (x < 0) return 0;   // undefined for non-integer powers
    return exp(y * log(x));
}
float powf(float x, float y) { return (float)pow(x, y); }

double tanh(double x) {
    if (x > 20) return 1;
    if (x < -20) return -1;
    double e = exp(2 * x);
    return (e - 1) / (e + 1);
}
float tanhf(float x) { return (float)tanh(x); }

double hypot(double x, double y) { return sqrt(x * x + y * y); }

}  // extern "C"
