/* KleinOS libc bits for QuickJS that the kernel does not export. */
#include "include/klibc.h"
#include "include/math.h"
#include "include/time.h"
#include "include/sys/time.h"

/* kernel C++ functions, bound by their mangled names */
extern void k_serial_write(const char* s) __asm__("_Z12serial_writePKc");
extern uint64_t k_unix_time(void) __asm__("_Z13rtc_unix_timev");
extern uint64_t k_timer_ms(void) __asm__("_Z8timer_msv");
extern int k_utc_offset(void) __asm__("_Z22rtc_utc_offset_minutesv");

int errno;

/* kernel heap block header (kernel/lib/heap.cpp): magic, size, next, prev; 16-byte footer */
size_t malloc_usable_size(void* p) {
    if (!p) return 0;
    uint64_t size = ((uint64_t*)p)[-3];
    return size > 48 ? (size_t)(size - 48) : 0;
}
struct KFILE { int fd; };
static struct KFILE f_in = {0}, f_out = {1}, f_err = {2};
FILE* stdin = &f_in;
FILE* stdout = &f_out;
FILE* stderr = &f_err;

size_t strnlen(const char* s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
long long strtoll(const char* s, char** e, int b) { return strtol(s, e, b); }
unsigned long long strtoull(const char* s, char** e, int b) { return strtoul(s, e, b); }
long labs(long v) { return v < 0 ? -v : v; }
long long llabs(long long v) { return v < 0 ? -v : v; }
char* getenv(const char* n) { (void)n; return 0; }
void exit(int code) { (void)code; abort(); }

int sprintf(char* b, const char* f, ...) { va_list ap; va_start(ap, f); int n = vsnprintf(b, 1 << 20, f, ap); va_end(ap); return n; }
int vfprintf(FILE* fp, const char* f, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), f, ap);
    (void)fp;
    k_serial_write(buf);
    return n;
}
int fprintf(FILE* fp, const char* f, ...) { va_list ap; va_start(ap, f); int n = vfprintf(fp, f, ap); va_end(ap); return n; }
int printf(const char* f, ...) { va_list ap; va_start(ap, f); int n = vfprintf(stdout, f, ap); va_end(ap); return n; }
int fputs(const char* s, FILE* fp) { (void)fp; k_serial_write(s); return 0; }
int fputc(int c, FILE* fp) { char b[2] = {(char)c, 0}; (void)fp; k_serial_write(b); return c; }
int putchar(int c) { return fputc(c, stdout); }
int puts(const char* s) { k_serial_write(s); k_serial_write("\n"); return 0; }
int fflush(FILE* fp) { (void)fp; return 0; }
size_t fwrite(const void* p, size_t s, size_t n, FILE* fp) {
    const char* c = (const char*)p;
    char buf[257];
    size_t total = s * n, off = 0;
    (void)fp;
    while (off < total) {
        size_t k = total - off > 256 ? 256 : total - off;
        memcpy(buf, c + off, k);
        buf[k] = 0;
        k_serial_write(buf);
        off += k;
    }
    return n;
}

/* introsort-free simple qsort: heap sort (O(n log n), no recursion) */
static void swap_bytes(char* a, char* b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
static void sift(char* base, size_t start, size_t end, size_t size, int (*cmp)(const void*, const void*)) {
    size_t root = start;
    while (root * 2 + 1 <= end) {
        size_t child = root * 2 + 1, sw = root;
        if (cmp(base + sw * size, base + child * size) < 0) sw = child;
        if (child + 1 <= end && cmp(base + sw * size, base + (child + 1) * size) < 0) sw = child + 1;
        if (sw == root) return;
        swap_bytes(base + root * size, base + sw * size, size);
        root = sw;
    }
}
void qsort(void* b, size_t n, size_t size, int (*cmp)(const void*, const void*)) {
    char* base = (char*)b;
    if (n < 2) return;
    if (n <= 16) {   /* insertion sort, stable for small arrays */
        for (size_t i = 1; i < n; i++)
            for (size_t j = i; j > 0 && cmp(base + (j - 1) * size, base + j * size) > 0; j--)
                swap_bytes(base + (j - 1) * size, base + j * size, size);
        return;
    }
    for (size_t s = (n - 2) / 2 + 1; s-- > 0;) sift(base, s, n - 1, size, cmp);
    for (size_t end = n - 1; end > 0; end--) {
        swap_bytes(base, base + end * size, size);
        sift(base, 0, end - 1, size, cmp);
    }
}

/* ---- math (complements kernel/lib/math.cpp) */
double sinh(double x) { double e = exp(x); return (e - 1 / e) / 2; }
double cosh(double x) { double e = exp(x); return (e + 1 / e) / 2; }
double asinh(double x) { return x < 0 ? -asinh(-x) : log(x + sqrt(x * x + 1)); }
double acosh(double x) { return log(x + sqrt(x * x - 1)); }
double atanh(double x) { return 0.5 * log((1 + x) / (1 - x)); }
double expm1(double x) { if (fabs(x) < 1e-5) return x + x * x / 2 + x * x * x / 6; return exp(x) - 1; }
double log1p(double x) { if (fabs(x) < 1e-4) return x - x * x / 2 + x * x * x / 3; return log(1 + x); }
double cbrt(double x) {
    if (x == 0 || isnan(x) || isinf(x)) return x;
    double y = x < 0 ? -pow(-x, 1.0 / 3) : pow(x, 1.0 / 3);
    y = y - (y * y * y - x) / (3 * y * y);   /* Newton refine */
    return y;
}
double exp2(double x) { return pow(2.0, x); }
double fmin(double a, double b) { if (isnan(a)) return b; if (isnan(b)) return a; return a < b ? a : b; }
double fmax(double a, double b) { if (isnan(a)) return b; if (isnan(b)) return a; return a > b ? a : b; }
double copysign(double a, double b) { return __builtin_copysign(a, b); }
double rint(double x) {
    if (!isfinite(x) || fabs(x) >= 4503599627370496.0) return x;
    double r = floor(x + 0.5);
    if (r - x == 0.5 && fmod(r, 2.0) != 0) r -= 1;   /* ties to even */
    return copysign(r, x);
}
double nearbyint(double x) { return rint(x); }
long lrint(double x) { return (long)rint(x); }
long long llrint(double x) { return (long long)rint(x); }
double frexp(double x, int* e) {
    union { double d; uint64_t u; } u = {x};
    int ex = (int)((u.u >> 52) & 0x7FF);
    if (x == 0 || isnan(x) || isinf(x)) { *e = 0; return x; }
    if (!ex) { x *= 18014398509481984.0; u.d = x; ex = (int)((u.u >> 52) & 0x7FF) - 54; }
    *e = ex - 1022;
    u.u = (u.u & 0x800FFFFFFFFFFFFFull) | (1022ull << 52);
    return u.d;
}
double ldexp(double x, int e) {
    while (e > 1000) { x *= 1.0715086071862673e301; e -= 1000; }
    while (e < -1000) { x *= 9.332636185032189e-302; e += 1000; }
    union { double d; uint64_t u; } u;
    u.u = (uint64_t)(e + 1023) << 52;
    return x * u.d;
}
double scalbn(double x, int e) { return ldexp(x, e); }
double modf(double x, double* ip) { double t = trunc(x); *ip = t; return x - t; }
double remainder(double x, double y) { double q = rint(x / y); return x - q * y; }

/* ---- time */
time_t time(time_t* t) { time_t v = (time_t)k_unix_time(); if (t) *t = v; return v; }
int gettimeofday(struct timeval* tv, void* tz) {
    (void)tz;
    uint64_t ms = k_timer_ms();
    static uint64_t base_unix, base_ms;
    if (!base_unix) { base_unix = k_unix_time(); base_ms = ms; }
    uint64_t now = base_unix * 1000 + (ms - base_ms);
    tv->tv_sec = (time_t)(now / 1000);
    tv->tv_usec = (long)(now % 1000) * 1000;
    return 0;
}
int clock_gettime(int clk, struct timespec* ts) {
    if (clk == CLOCK_MONOTONIC) {
        uint64_t ms = k_timer_ms();
        ts->tv_sec = (time_t)(ms / 1000);
        ts->tv_nsec = (long)(ms % 1000) * 1000000;
        return 0;
    }
    struct timeval tv;
    gettimeofday(&tv, 0);
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000;
    return 0;
}
clock_t clock(void) { return (clock_t)k_timer_ms(); }

static int days_in(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m];
}
struct tm* gmtime_r(const time_t* tp, struct tm* o) {
    long long t = *tp;
    long long days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    o->tm_hour = (int)(rem / 3600);
    o->tm_min = (int)(rem % 3600 / 60);
    o->tm_sec = (int)(rem % 60);
    o->tm_wday = (int)((days + 4) % 7);
    if (o->tm_wday < 0) o->tm_wday += 7;
    int y = 1970;
    while (days < 0) { y--; days += ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365; }
    for (;;) {
        int yd = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
        if (days < yd) break;
        days -= yd;
        y++;
    }
    o->tm_year = y - 1900;
    o->tm_yday = (int)days;
    int m = 0;
    while (days >= days_in(y, m)) { days -= days_in(y, m); m++; }
    o->tm_mon = m;
    o->tm_mday = (int)days + 1;
    o->tm_isdst = 0;
    o->tm_gmtoff = 0;
    o->tm_zone = "UTC";
    return o;
}
struct tm* localtime_r(const time_t* tp, struct tm* o) {
    long off = (long)k_utc_offset() * 60;
    time_t t = *tp + off;
    gmtime_r(&t, o);
    o->tm_gmtoff = off;
    o->tm_zone = "LOCAL";
    return o;
}
struct tm* localtime(const time_t* t) { static struct tm tmv; return localtime_r(t, &tmv); }
time_t mktime(struct tm* tm) {
    long long days = 0;
    int y = tm->tm_year + 1900;
    for (int yr = 1970; yr < y; yr++) days += ((yr % 4 == 0 && yr % 100 != 0) || yr % 400 == 0) ? 366 : 365;
    for (int m = 0; m < tm->tm_mon; m++) days += days_in(y, m);
    days += tm->tm_mday - 1;
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec) - (long)k_utc_offset() * 60;
}
