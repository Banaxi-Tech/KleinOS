#include "net/timesync.h"
#include "net/http.h"
#include "net/net.h"
#include "drivers/rtc.h"
#include "sys/system.h"
#include "arch/interrupts.h"
#include "lib/log.h"
#include "lib/string.h"

namespace net {

namespace {

constexpr uint64_t kCheckMs = 1000, kRetryMs = 5 * 60 * 1000, kResyncMs = 6 * 60 * 60 * 1000;
constexpr int kMinYear = 2024;   // anything earlier is a broken answer

// Small responses from servers that are rarely blocked; the second is the fallback.
const char* const kSources[] = {"https://www.google.com/generate_204", "https://www.cloudflare.com/cdn-cgi/trace"};
constexpr int kSourceCount = sizeof(kSources) / sizeof(kSources[0]);

HttpRequest* g_req;
int g_source;
uint64_t g_next_check;

// "Mon, 09 Nov 2026 12:34:56 GMT"
bool parse_http_date(const char* s, uint64_t* out) {
    static const char* const kMonths = "janfebmaraprmayjunjulaugsepoctnovdec";
    const char* p = strchr(s, ',');
    if (!p) return false;
    p++;
    while (*p == ' ') p++;
    int day = atoi(p);
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (strlen(p) < 3) return false;
    char mon[4] = {(char)tolower(p[0]), (char)tolower(p[1]), (char)tolower(p[2]), 0};
    const char* found = strstr(kMonths, mon);
    if (!found || (found - kMonths) % 3) return false;
    int month = (int)(found - kMonths) / 3 + 1;
    while (*p && *p != ' ') p++;
    int year = atoi(p);
    while (*p == ' ') p++;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    int hour = atoi(p);
    p = strchr(p, ':');
    if (!p) return false;
    int minute = atoi(++p);
    p = strchr(p, ':');
    if (!p) return false;
    int second = atoi(p + 1);
    if (year < kMinYear || day < 1 || day > days_in_month(year, month) || hour > 23 || minute > 59 || second > 60) return false;
    *out = rtc_make_unix(year, month, day, hour, minute, second);
    return true;
}

void poll_timesync() {
    uint64_t now = timer_ms();
    if (!g_req) {
        if (now < g_next_check) return;
        g_next_check = now + kCheckMs;
        if (is_up()) g_req = http_get(kSources[g_source]);
        return;
    }
    HttpState st = http_state(g_req);
    if (st != HttpState::Done && st != HttpState::Failed) return;
    const char* date = st == HttpState::Done ? http_header(g_req, "date") : nullptr;
    uint64_t t;
    if (date && parse_http_date(date, &t)) {
        int64_t off = (int64_t)t - (int64_t)rtc_unix_time();
        rtc_set_unix_time(t);
        klog("time: set from the web, clock was off by %d s", (int)off);
        g_next_check = now + kResyncMs;
        g_source = 0;
    } else if (++g_source >= kSourceCount) {
        klog("time: could not get the time from the web, keeping the clock");
        g_source = 0;
        g_next_check = now + kRetryMs;
    } else {
        g_next_check = now;
    }
    http_free(g_req);
    g_req = nullptr;
}

}  // namespace

void timesync_init() { system_add_poll_hook(poll_timesync); }

}  // namespace net
