#pragma once
#include <stdint.h>

struct DateTime {
    int year, month, day;       // month 1..12
    int hour, minute, second;
    int weekday;                // 0 = Sunday
};

void rtc_init();
// Current local time (RTC + configured UTC offset, advanced with the PIT).
DateTime rtc_now();
// Seconds since 1970-01-01 UTC.
uint64_t rtc_unix_time();
void rtc_set_utc_offset_minutes(int minutes);
int rtc_utc_offset_minutes();
// Helpers
int days_in_month(int year, int month);
int day_of_week(int year, int month, int day);
DateTime unix_to_datetime(uint64_t t);
const char* month_name(int month);      // "January"
const char* weekday_name(int weekday);  // "Sunday"
