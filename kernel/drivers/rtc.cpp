#include "drivers/rtc.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"

static uint64_t g_boot_unix;   // UTC unix time at g_boot_ms
static uint64_t g_boot_ms;
static int g_offset_min;

static uint8_t cmos(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint64_t to_unix(int y, int mo, int d, int h, int mi, int s) {
    // days since epoch
    int64_t days = 0;
    for (int yr = 1970; yr < y; yr++) days += (yr % 4 == 0 && (yr % 100 != 0 || yr % 400 == 0)) ? 366 : 365;
    for (int m = 1; m < mo; m++) days += days_in_month(y, m);
    days += d - 1;
    return (uint64_t)days * 86400 + h * 3600 + mi * 60 + s;
}

void rtc_init() {
    while (cmos(0x0A) & 0x80) {}
    uint8_t s = cmos(0x00), mi = cmos(0x02), h = cmos(0x04), d = cmos(0x07), mo = cmos(0x08), y = cmos(0x09);
    uint8_t century = cmos(0x32);
    uint8_t b = cmos(0x0B);
    auto bcd = [](uint8_t v) { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); };
    if (!(b & 0x04)) {
        s = bcd(s); mi = bcd(mi); d = bcd(d); mo = bcd(mo); y = bcd(y);
        h = ((h & 0x0F) + (((h & 0x70) >> 4) * 10)) | (h & 0x80);
        century = bcd(century);
    }
    if (!(b & 0x02) && (h & 0x80)) h = ((h & 0x7F) + 12) % 24;
    int year = (century >= 19 && century <= 30) ? century * 100 + y : 2000 + y;
    g_boot_unix = to_unix(year, mo, d, h, mi, s);
    g_boot_ms = timer_ms();
}

uint64_t rtc_unix_time() { return g_boot_unix + (timer_ms() - g_boot_ms) / 1000; }
void rtc_set_utc_offset_minutes(int m) { g_offset_min = m; }
int rtc_utc_offset_minutes() { return g_offset_min; }

int days_in_month(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 29;
    return d[(m - 1) % 12];
}

int day_of_week(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

DateTime unix_to_datetime(uint64_t t) {
    DateTime dt{};
    uint64_t days = t / 86400, rem = t % 86400;
    dt.hour = rem / 3600;
    dt.minute = (rem % 3600) / 60;
    dt.second = rem % 60;
    int y = 1970;
    for (;;) {
        int yd = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
        if (days < (uint64_t)yd) break;
        days -= yd;
        y++;
    }
    int m = 1;
    while (days >= (uint64_t)days_in_month(y, m)) { days -= days_in_month(y, m); m++; }
    dt.year = y;
    dt.month = m;
    dt.day = (int)days + 1;
    dt.weekday = day_of_week(y, m, dt.day);
    return dt;
}

DateTime rtc_now() { return unix_to_datetime(rtc_unix_time() + (int64_t)g_offset_min * 60); }

const char* month_name(int m) {
    static const char* n[] = {"January", "February", "March", "April", "May", "June", "July",
                              "August", "September", "October", "November", "December"};
    return n[(m - 1) % 12];
}
const char* weekday_name(int w) {
    static const char* n[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    return n[w % 7];
}
