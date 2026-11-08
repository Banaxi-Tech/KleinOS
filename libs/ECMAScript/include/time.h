#pragma once
#include <stdint.h>
#include <stddef.h>
typedef long time_t;
typedef long clock_t;
struct timespec { time_t tv_sec; long tv_nsec; };
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; long tm_gmtoff; const char* tm_zone; };
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCKS_PER_SEC 1000
#ifdef __cplusplus
extern "C" {
#endif
time_t time(time_t* t);
int clock_gettime(int clk, struct timespec* ts);
struct tm* localtime_r(const time_t* t, struct tm* out);
struct tm* gmtime_r(const time_t* t, struct tm* out);
struct tm* localtime(const time_t* t);
time_t mktime(struct tm* tm);
clock_t clock(void);
#ifdef __cplusplus
}
#endif
