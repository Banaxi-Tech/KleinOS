#include "net/wifi.h"
#include "arch/interrupts.h"
#include "boot/multiboot2.h"
#include "drivers/rtc.h"
#include "fs/fat32.h"
#include "lib/heap.h"
#include "lib/log.h"
#include "lib/string.h"
#include "net/crypto.h"
#include "sys/system.h"

namespace net {

static const WifiDriver* g_driver;
void wifi_register(const WifiDriver* driver) { g_driver = driver; }
const WifiDriver* wifi_driver() { return g_driver; }

// ---- remembered networks: one "<hex ssid> <hex key, or - when open>" line each, newest first
static const char kSavedPath[] = "/KleinOS/wifi.cfg";
static constexpr int kMaxSaved = 16;
static constexpr uint64_t kRescanMs = 30000, kUserGraceMs = 60000;
struct SavedNetwork { char ssid[33]; char key[65]; };
static SavedNetwork g_saved[kMaxSaved], g_pending;
static int g_saved_count;
static bool g_loaded, g_has_pending, g_demo, g_panel_open;
static bool g_tried[kMaxSaved], g_rejected[kMaxSaved];   // this boot only

enum class Auto { Wait, Scanning, Joining };
static Auto g_auto = Auto::Wait;
static uint64_t g_next_scan;
static int g_joining = -1;

static void to_hex(const uint8_t* p, size_t n, char* out) {
    static const char kDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i * 2] = kDigits[p[i] >> 4]; out[i * 2 + 1] = kDigits[p[i] & 15]; }
    out[n * 2] = 0;
}
static int hex_digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
static bool from_hex(const char* s, size_t len, char* out, size_t max) {
    if ((len & 1) || len / 2 >= max) return false;
    for (size_t i = 0; i < len; i += 2) {
        int a = hex_digit(s[i]), b = hex_digit(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out[i / 2] = (char)(a * 16 + b);
    }
    out[len / 2] = 0;
    return true;
}

static int find_saved(const char* ssid) {
    for (int i = 0; i < g_saved_count; i++)
        if (!strcmp(g_saved[i].ssid, ssid)) return i;
    return -1;
}

static void load_saved() {
    g_loaded = true;
    uint8_t* data = nullptr;
    size_t size = 0;
    if (!fat32::read_file(kSavedPath, &data, &size)) return;
    const char* p = (const char*)data, *end = p + size;
    while (p < end && g_saved_count < kMaxSaved) {
        const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        const char* sp = (const char*)memchr(p, ' ', (size_t)(nl - p));
        if (sp) {
            SavedNetwork& n = g_saved[g_saved_count];
            size_t key_len = (size_t)(nl - sp - 1);
            bool open = key_len == 1 && sp[1] == '-';
            if (from_hex(p, (size_t)(sp - p), n.ssid, sizeof(n.ssid)) && n.ssid[0] && (open || key_len == 64)) {
                n.key[0] = 0;
                if (!open) { memcpy(n.key, sp + 1, 64); n.key[64] = 0; }
                g_saved_count++;
            }
        }
        p = nl + 1;
    }
    kfree(data);
    klog("wifi: %d remembered networks", g_saved_count);
}

static void write_saved() {
    static char text[kMaxSaved * 134];
    size_t n = 0;
    for (int i = 0; i < g_saved_count; i++) {
        to_hex((const uint8_t*)g_saved[i].ssid, strlen(g_saved[i].ssid), text + n);
        n += strlen(text + n);
        text[n++] = ' ';
        if (g_saved[i].key[0]) { memcpy(text + n, g_saved[i].key, 64); n += 64; }
        else text[n++] = '-';
        text[n++] = '\n';
    }
    if (!fat32::write_file(kSavedPath, text, n, rtc_unix_time()) || !fat32::flush()) klog("wifi: could not save networks");
}

static void remove_saved(int i) {
    for (int j = i; j + 1 < g_saved_count; j++) {
        g_saved[j] = g_saved[j + 1];
        g_tried[j] = g_tried[j + 1];
        g_rejected[j] = g_rejected[j + 1];
    }
    g_saved_count--;
    g_tried[g_saved_count] = g_rejected[g_saved_count] = false;
}

static void remember(const SavedNetwork& net) {
    int i = find_saved(net.ssid);
    if (i >= 0) remove_saved(i);
    if (g_saved_count == kMaxSaved) g_saved_count--;
    for (int j = g_saved_count; j > 0; j--) {
        g_saved[j] = g_saved[j - 1];
        g_tried[j] = g_tried[j - 1];
        g_rejected[j] = g_rejected[j - 1];
    }
    g_saved[0] = net;
    g_tried[0] = g_rejected[0] = false;
    g_saved_count++;
    write_saved();
}

static void user_is_connecting() {
    g_auto = Auto::Wait;
    g_joining = -1;
    g_next_scan = timer_ms() + kUserGraceMs;
}

void wifi_connect(const char* ssid, const char* password) {
    if (!g_driver) return;
    user_is_connecting();
    if (g_demo) { g_driver->connect(ssid, password); return; }
    strlcpy(g_pending.ssid, ssid, sizeof(g_pending.ssid));
    g_pending.key[0] = 0;
    if (password[0]) {
        uint8_t pmk[32];
        crypto::wpa_psk(password, (const uint8_t*)ssid, strlen(ssid), pmk);
        to_hex(pmk, sizeof(pmk), g_pending.key);
    }
    g_has_pending = true;
    g_driver->connect(ssid, g_pending.key);
}

bool wifi_connect_saved(const char* ssid) {
    int i = g_driver ? find_saved(ssid) : -1;
    if (i < 0) return false;
    user_is_connecting();
    g_driver->connect(ssid, g_saved[i].key);
    return true;
}

void wifi_forget(const char* ssid) {
    int i = find_saved(ssid);
    if (i < 0) return;
    remove_saved(i);
    write_saved();
}

void wifi_set_panel_open(bool open) { g_panel_open = open; }

static void pick_network() {
    static WifiNetwork found[32];
    int n = g_driver->results(found, 32);
    for (int i = 0; i < n; i++) {                       // strongest first
        int s = find_saved(found[i].ssid);
        if (s < 0 || g_tried[s] || g_rejected[s]) continue;
        g_tried[s] = true;
        g_joining = s;
        g_auto = Auto::Joining;
        klog("wifi: joining remembered network %s", g_saved[s].ssid);
        g_driver->connect(g_saved[s].ssid, g_saved[s].key);
        return;
    }
    for (bool& t : g_tried) t = false;
    g_auto = Auto::Wait;
    g_next_scan = timer_ms() + kRescanMs;
}

static void wifi_poll() {
    if (!g_loaded) {
        if (!fat32::mounted()) return;
        load_saved();
    }
    WifiStatus s = g_driver->status();
    if (g_has_pending && s != WifiStatus::Connecting) {
        g_has_pending = false;
        if (s == WifiStatus::Connected) remember(g_pending);
    }
    switch (g_auto) {
        case Auto::Wait:
            if (g_panel_open || !g_saved_count || s == WifiStatus::Connected || s == WifiStatus::Connecting || g_driver->scanning()) return;
            if (timer_ms() < g_next_scan) return;
            g_driver->scan();
            g_auto = Auto::Scanning;
            break;
        case Auto::Scanning:
            if (g_panel_open) { g_auto = Auto::Wait; return; }
            if (!g_driver->scanning()) pick_network();
            break;
        case Auto::Joining:
            if (s == WifiStatus::Connecting) return;
            if (s == WifiStatus::Connected) {
                for (bool& t : g_tried) t = false;
            } else if (s == WifiStatus::WrongPassword && g_joining >= 0) {
                g_rejected[g_joining] = true;      // not deleted: a weak signal can look the same
            }
            g_joining = -1;
            g_auto = Auto::Wait;
            g_next_scan = timer_ms() + 1000;
            break;
    }
}

void wifi_autoconnect_init() {
    if (g_driver && !g_demo) system_add_poll_hook(wifi_poll);
}

// ---- demo: the password for every secured network is "password"
static uint64_t g_scan_until, g_connect_at;
static WifiStatus g_status = WifiStatus::Idle;
static char g_ssid[33];

static const WifiNetwork kDemo[] = {
    {"Demo Home Network", 4, true}, {"Demo Open Cafe", 3, false}, {"Demo Neighbour", 2, true},
    {"Demo Office", 1, true},       {"Demo Far Away", 1, true},   {"Demo Guest", 3, false},
    {"Demo Garage", 2, true},       {"Demo Upstairs", 1, true},
};
static const char kDemoPassword[] = "password";
static char g_try[64];

static void demo_scan() { g_scan_until = timer_ms() + 1200; }
static bool demo_scanning() { return timer_ms() < g_scan_until; }
static int demo_results(WifiNetwork* out, int max) {
    int n = 0;
    for (const WifiNetwork& w : kDemo)
        if (n < max) out[n++] = w;
    return n;
}
static void demo_connect(const char* ssid, const char* password) {
    strlcpy(g_ssid, ssid, sizeof(g_ssid));
    strlcpy(g_try, password, sizeof(g_try));
    g_status = WifiStatus::Connecting;
    g_connect_at = timer_ms() + 1500;
}
static void demo_disconnect() { g_status = WifiStatus::Idle; g_ssid[0] = 0; }
static WifiStatus demo_status() {
    if (g_status == WifiStatus::Connecting && timer_ms() >= g_connect_at)
        g_status = !g_try[0] || !strcmp(g_try, kDemoPassword) ? WifiStatus::Connected : WifiStatus::WrongPassword;
    return g_status;
}
static const char* demo_connected() { return g_status == WifiStatus::Connected ? g_ssid : ""; }

void wifi_init_demo() {
    char v[4];
    if (!boot_arg("wifidemo", v, sizeof(v)) || v[0] != '1') return;
    g_demo = true;
    static const WifiDriver demo = {demo_scan, demo_scanning, demo_results, demo_connect, demo_disconnect, demo_status, demo_connected};
    wifi_register(&demo);
}

}  // namespace net
