// System Settings: macOS Ventura-style preference panes.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "gfx/wallpaper.h"
#include "drivers/framebuffer.h"
#include "drivers/input.h"
#include "drivers/rtc.h"
#include "drivers/ata.h"
#include "net/net.h"
#include "sys/productkey.h"
#include "sys/session.h"
#include "sys/system.h"
#include "sys/dl.h"
#include "boot/multiboot2.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "fs/vfs.h"
#include "lib/math.h"

namespace {

// ---------------------------------------------------------------- layout
constexpr int kSideW = 220;          // glass sidebar
constexpr int kListY = 116;          // first pane row in the sidebar
constexpr int kListStep = 30;
constexpr int kX0 = 244;             // content left edge
constexpr int kX1 = 736;             // content right edge
constexpr int kW = kX1 - kX0;        // 492
constexpr int kRowH = 44;            // one row in a grouped box
constexpr int kInR = kX1 - 16;       // right edge inside a group

enum PaneId {
    PN_General, PN_Appearance, PN_Wallpaper, PN_Dock, PN_Displays, PN_Network, PN_Keyboard,
    PN_DateTime, PN_Users, PN_Activation, PN_Update, PN_Storage, PN_About, PN_Count
};

struct PaneDef { const char* name; Symbol sym; Color top; Color bottom; };
const PaneDef kPanes[PN_Count] = {
    {"General", Symbol::Gear, hex(0x9A9AA0), hex(0x5E5E66)},
    {"Appearance", Symbol::Moon, hex(0x4A4A52), hex(0x1C1C22)},
    {"Wallpaper", Symbol::Image, hex(0x5AD2F0), hex(0x0A7EC0)},
    {"Dock & Desktop", Symbol::Grid, hex(0x7D7AFF), hex(0x3E3AB8)},
    {"Displays", Symbol::Display, hex(0x3D9BFF), hex(0x0055D4)},
    {"Network", Symbol::Globe, hex(0x40C8FF), hex(0x1E6FE0)},
    {"Keyboard", Symbol::Keyboard, hex(0xE6E6EB), hex(0x9A9AA2)},
    {"Date & Time", Symbol::Clock, hex(0xFF6B61), hex(0xD42A20)},
    {"Users", Symbol::User, hex(0xFFA43A), hex(0xD9620A)},
    {"Activation", Symbol::Lock, hex(0xFFD84A), hex(0xE0A200)},
    {"Software Update", Symbol::Refresh, hex(0xC47CFF), hex(0x7A3ACC)},
    {"Storage", Symbol::Download, hex(0x4CD97A), hex(0x1E8F44)},
    {"About", Symbol::Info, hex(0x8E9AAF), hex(0x4A5568)},
};

const Color kAccents[8] = {kAccentBlue, kAccentPurple, kAccentPink, kAccentRed,
                           kAccentOrange, kAccentYellow, kAccentGreen, kAccentGraphite};

const char* kLayoutNames[4] = {"English (US)", "English (UK)", "German", "French"};
// Top, home and bottom letter rows for the keyboard preview.
const char* kKeyRows[4][3] = {
    {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
    {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
    {"QWERTZUIOP", "ASDFGHJKL", "YXCVBNM"},
    {"AZERTYUIOP", "QSDFGHJKLM", "WXCVBN"},
};

struct Zone { int minutes; const char* name; };
const Zone kZones[] = {
    {-480, "UTC-8  Los Angeles"}, {-420, "UTC-7  Denver"}, {-360, "UTC-6  Chicago"},
    {-300, "UTC-5  New York"},    {-180, "UTC-3  Sao Paulo"}, {0, "UTC+0  London"},
    {60, "UTC+1  Berlin"},        {120, "UTC+2  Athens"},  {180, "UTC+3  Istanbul"},
    {240, "UTC+4  Dubai"},        {330, "UTC+5:30  Mumbai"}, {480, "UTC+8  Singapore"},
    {540, "UTC+9  Tokyo"},
};
constexpr int kZoneCount = sizeof(kZones) / sizeof(kZones[0]);

const char* kDriveLocations[4] = {"Primary master", "Primary slave", "Secondary master", "Secondary slave"};

// ---------------------------------------------------------------- state
int g_appearance = -1;       // 0 light, 1 dark, 2 auto (-1 = not loaded yet)
bool g_autoupdate = true;

String prefs_dir() { return String("/Users/") + session_user() + "/Library/Settings"; }
String prefs_path() { return prefs_dir() + "/prefs.txt"; }
const char* activation_path() { return "/System/Config/activation.txt"; }

// Reads "key=value" from newline-separated text. Returns def if absent.
int pref_int(const char* text, const char* key, int def) {
    char pat[40];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char* p = text;
    while ((p = strstr(p, pat)) != nullptr) {
        if (p == text || p[-1] == '\n') return atoi(p + strlen(pat));
        p += 1;
    }
    return def;
}

void load_prefs() {
    String text;
    if (!vfs::read_text(prefs_path().c_str(), text)) return;
    g_appearance = pref_int(text.c_str(), "appearance", -1);
    g_autoupdate = pref_int(text.c_str(), "autoupdate", 1) != 0;
}

int current_accent_index() {
    for (int i = 0; i < 8; i++)
        if (theme().accent == kAccents[i]) return i;
    return -1;
}

// Saves every preference from the live system state.
void save_prefs() {
    String dir = prefs_dir();
    vfs::mkdir(dir.c_str());
    char buf[320];
    int n = snprintf(buf, sizeof(buf),
                     "appearance=%d\nwallpaper=%d\naccent=%d\nglass=%d\nreduce=%d\n"
                     "dock_magnification=%d\nkeyboard=%d\ntimezone=%d\nautoupdate=%d\n",
                     g_appearance, wm::wallpaper(), current_accent_index(),
                     (int)(wm::glass_intensity() * 1000 + 0.5f), wm::reduce_transparency() ? 1 : 0,
                     wm::dock_magnification() ? 1 : 0, (int)input_layout(), rtc_utc_offset_minutes(),
                     g_autoupdate ? 1 : 0);
    vfs::write_file(prefs_path().c_str(), buf, (size_t)n);
}

// Automatic appearance: dark between 19:00 and 07:00.
bool auto_wants_dark() {
    int h = rtc_now().hour;
    return h < 7 || h >= 19;
}

void apply_appearance(int mode) {
    g_appearance = mode;
    if (mode == 2) theme_set_dark(auto_wants_dark());
    else theme_set_dark(mode == 1);
    save_prefs();
}

// ---------------------------------------------------------------- helpers
bool has_ci(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    if (n == 0) return true;
    for (; *hay; hay++) {
        size_t k = 0;
        while (k < n && hay[k] && tolower((unsigned char)hay[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == n) return true;
    }
    return false;
}

// Up to two capital initials from a full name ("Ada Lovelace" -> "AL").
void initials_of(const char* name, char out[3]) {
    int n = 0;
    bool start = true;
    for (const char* p = name; *p && n < 2; p++) {
        if (*p == ' ') { start = true; continue; }
        if (start && isalpha((unsigned char)*p)) out[n++] = (char)toupper((unsigned char)*p);
        start = false;
    }
    out[n] = 0;
}

void fmt_bytes(uint64_t b, char* out, size_t n) {
    unsigned long long v = b;
    if (b < 1024) snprintf(out, n, "%llu B", v);
    else if (b < (1ull << 20)) snprintf(out, n, "%llu KB", v >> 10);
    else if (b < (1ull << 30)) snprintf(out, n, "%llu MB", v >> 20);
    else snprintf(out, n, "%llu GB", v >> 30);
}

void fmt_offset(int m, char* out, size_t n) {
    int a = m < 0 ? -m : m;
    if (a % 60) snprintf(out, n, "UTC%c%d:%02d", m < 0 ? '-' : '+', a / 60, a % 60);
    else snprintf(out, n, "UTC%c%d", m < 0 ? '-' : '+', a / 60);
}

int zone_index_for(int minutes) {
    for (int i = 0; i < kZoneCount; i++)
        if (kZones[i].minutes == minutes) return i;
    return 5;   // UTC+0
}

void draw_avatar(Canvas& c, int x, int y, int d, const char* name, int size) {
    c.fill_gradient(Rect(x, y, d, d), hex(0x9EC2FF), hex(0x5A7BE0), d / 2.0f);
    c.fill_gradient(Rect(x + 2, y + 2, d - 4, d / 2), rgba(255, 255, 255, 80), rgba(255, 255, 255, 0), d / 2.0f);
    char ini[3];
    initials_of(name, ini);
    c.draw_text_in(Rect(x, y, d, d), ini, hex(0xFFFFFF), size, Font::Bold, Align::Center);
}

// Small squircle icon used in the sidebar and pane headers.
void mini_icon(Canvas& c, int x, int y, int s, Symbol sym, Color top, Color bottom) {
    Rect r(x, y, s, s);
    c.fill_gradient(r, top, bottom, s * 0.25f);
    c.fill_gradient(Rect(x + 1, y + 1, s - 2, s / 2), rgba(255, 255, 255, 70), rgba(255, 255, 255, 0), s * 0.22f);
    draw_symbol(c, r.inset(s / 5), sym, hex(0xFFFFFF), s / 12.0f);
}

// Clears the corners of a thumbnail so it can be drawn with rounded corners.
void round_mask(Canvas& c, int rad) {
    int w = c.width(), h = c.height();
    for (int y = 0; y < rad; y++)
        for (int x = 0; x < rad; x++) {
            float dx = rad - x - 0.5f, dy = rad - y - 0.5f;
            if (dx * dx + dy * dy <= rad * (float)rad) continue;
            c.pixels()[y * c.stride() + x] = 0;
            c.pixels()[y * c.stride() + (w - 1 - x)] = 0;
            c.pixels()[(h - 1 - y) * c.stride() + x] = 0;
            c.pixels()[(h - 1 - y) * c.stride() + (w - 1 - x)] = 0;
        }
}

void draw_logo_mark(Canvas& c, Rect r) {
    float rad = r.w * 0.24f;
    c.draw_shadow(r, rad, r.w * 0.08f, rgba(0, 0, 0, 80), 0, r.w / 20);
    c.fill_gradient(r, hex(0x5E8BFF), hex(0x7A3CFF), rad);
    c.fill_gradient(Rect(r.x + 2, r.y + 2, r.w - 4, r.h / 2), rgba(255, 255, 255, 90), rgba(255, 255, 255, 0), rad - 2);
    c.stroke_round_rect(r, rad, rgba(255, 255, 255, 120), 1.5f);
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, u = r.w / 96.0f;
    c.draw_line(cx - 15 * u, cy - 24 * u, cx - 15 * u, cy + 24 * u, hex(0xFFFFFF), 8 * u);
    c.draw_line(cx - 11 * u, cy + 3 * u, cx + 17 * u, cy - 24 * u, hex(0xFFFFFF), 8 * u);
    c.draw_line(cx - 4 * u, cy - 3 * u, cx + 18 * u, cy + 24 * u, hex(0xFFFFFF), 8 * u);
}

// Grouped rounded box with thin separators between rows.
void group(Canvas& c, int top, int rows) {
    const Theme& t = theme();
    draw_group_box(c, Rect(kX0, top, kW, rows * kRowH));
    for (int i = 1; i < rows; i++) c.fill_rect(Rect(kX0 + 16, top + i * kRowH, kW - 32, 1), t.separator);
}

void row_title(Canvas& c, int top, int i, const char* label, const char* sub = nullptr) {
    const Theme& t = theme();
    int y = top + i * kRowH;
    if (sub) {
        c.draw_text_in(Rect(kX0 + 16, y + 6, 214, 18), label, t.text, 13, Font::Medium);
        c.draw_text_in(Rect(kX0 + 16, y + 23, 214, 16), sub, t.text_secondary, 11, Font::Regular);
    } else {
        c.draw_text_in(Rect(kX0 + 16, y, 214, kRowH), label, t.text, 13, Font::Medium);
    }
}

void row_value(Canvas& c, int top, int i, const char* value, Color col = 0) {
    const Theme& t = theme();
    int y = top + i * kRowH;
    c.draw_text_in(Rect(kX0 + 240, y, kInR - (kX0 + 240), kRowH), value, col ? col : t.text_secondary, 13,
                   Font::Regular, Align::Right);
}

// Title (and optional subtitle) at the top of a pane.
void pane_title(Canvas& c, const char* title, const char* sub) {
    const Theme& t = theme();
    c.draw_text(kX0, 18, title, t.text, 22, Font::Bold);
    if (sub) c.draw_text(kX0, 50, sub, t.text_secondary, 13);
}

// Light/dark picture tile for the appearance pane.
void paint_mode_tile(Canvas& c, Rect r, int mode, bool selected) {
    const Theme& t = theme();
    Color light_bg = hex(0xEDEDF2), dark_bg = hex(0x2A2A30);
    Color light_win = hex(0xFFFFFF), dark_win = hex(0x1E1E22);
    auto draw_side = [&](Rect area, bool dark) {
        c.save();
        c.clip_to(area);
        c.fill_round_rect(r, 14, dark ? dark_bg : light_bg);
        Rect w(r.x + 16, r.y + 14, r.w - 32, r.h - 28);
        c.fill_round_rect(w, 7, dark ? dark_win : light_win);
        c.fill_rect(Rect(w.x + 8, w.y + 9, 22, 4), t.accent);
        c.fill_rect(Rect(w.x + 8, w.y + 22, w.w - 40, 4), dark ? rgba(255, 255, 255, 60) : rgba(0, 0, 0, 40));
        c.fill_rect(Rect(w.x + 8, w.y + 32, w.w - 56, 4), dark ? rgba(255, 255, 255, 40) : rgba(0, 0, 0, 25));
        c.restore();
    };
    if (mode == 0) draw_side(r, false);
    else if (mode == 1) draw_side(r, true);
    else {
        draw_side(Rect(r.x, r.y, r.w / 2, r.h), false);
        draw_side(Rect(r.x + r.w / 2, r.y, r.w - r.w / 2, r.h), true);
    }
    c.stroke_round_rect(r, 14, t.separator, 1);
    if (selected) {
        c.stroke_round_rect(Rect(r.x - 4, r.y - 4, r.w + 8, r.h + 8), 18, t.accent, 2.5f);
        c.fill_circle(r.right() - 12, r.y + 12, 9, t.accent);
        draw_symbol(c, Rect(r.right() - 18, r.y + 6, 12, 12), Symbol::Check, hex(0xFFFFFF), 1.8f);
    }
}

// Wallpaper grid geometry (shared by paint and hit testing).
Rect wallpaper_rect(int i) {
    int col = i % 3, row = i / 3;
    return Rect(kX0 + col * 169, 84 + row * 136, 154, 96);
}

Rect tile_rect(int i) { return Rect(kX0 + i * 170, 80, 150, 92); }

constexpr int kAccentRowY = 212;
constexpr int kGlassRowY = 256;
constexpr int kReduceRowY = 300;

Rect accent_circle(int i) { return Rect(488 + i * 26 - 12, kAccentRowY + 22 - 12, 24, 24); }

// ---------------------------------------------------------------- window
class SettingsWindow : public Window {
public:
    SettingsWindow() : Window("Settings", 760, 540, WF_NO_ZOOM) {
        set_sidebar_width(kSideW);
        load_prefs();
        if (g_appearance < 0) g_appearance = theme_is_dark() ? 1 : 0;
        load_activation();

        search = (TextField*)add_tagged(new TextField(Rect(12, 12, 196, 28), "Search"), -1, true);
        search->rounded = true;
        search->search_icon = true;
        search->font_size = 13;
        search->on_change = [this] { rebuild_shown(); invalidate(); };

        glass = (Slider*)add_tagged(new Slider(Rect(482, kGlassRowY, 150, kRowH), 0), PN_Appearance, false);
        glass->on_change = [this](float v) { wm::set_glass_intensity(v * 1.5f); save_prefs(); invalidate(); };

        reduce = (Toggle*)add_tagged(new Toggle(Rect(670, kReduceRowY, 50, kRowH)), PN_Appearance, false);
        reduce->on_change = [this](bool on) { wm::set_reduce_transparency(on); save_prefs(); invalidate(); };

        dock_mag = (Toggle*)add_tagged(new Toggle(Rect(670, 80, 50, kRowH)), PN_Dock, false);
        dock_mag->on_change = [this](bool on) { wm::set_dock_magnification(on); save_prefs(); invalidate(); };

        layout = (Dropdown*)add_tagged(new Dropdown(Rect(kInR - 200, 80 + 7, 200, 30)), PN_Keyboard, false);
        for (const char* s : kLayoutNames) layout->options.push(String(s));
        layout->on_change = [this](int i) { input_set_layout((KeyboardLayout)i); save_prefs(); invalidate(); };

        keytest = (TextField*)add_tagged(new TextField(Rect(kInR - 250, 80 + kRowH + 7, 250, 30), "Type to test"),
                                         PN_Keyboard, false);

        timezone = (Dropdown*)add_tagged(new Dropdown(Rect(kInR - 238, 80 + 7, 238, 30)), PN_DateTime, false);
        for (int i = 0; i < kZoneCount; i++) timezone->options.push(String(kZones[i].name));
        timezone->on_change = [this](int i) { rtc_set_utc_offset_minutes(kZones[i].minutes); save_prefs(); invalidate(); };

        key = (TextField*)add_tagged(new TextField(Rect(kInR - 280, 196 + 7, 280, 30), "XXXXX-XXXXX-XXXXX-XXXXX-XXXXX"),
                                     PN_Activation, false);
        key->max_length = 29;
        key->font_size = 13;
        key->on_change = [this] {
            char buf[32];
            productkey::format_partial(key->get_text(), buf);
            if (strcmp(buf, key->get_text()) != 0) key->set_text(buf);
            key_error = "";
            invalidate();
        };
        key->on_submit = [this] { activate_key(); };

        activate_btn = (Button*)add_tagged(new Button(Rect(kInR - 110, 196 + kRowH + 7, 110, 30), "Activate", Button::Primary),
                                           PN_Activation, false);
        activate_btn->on_click = [this] { activate_key(); };

        check_btn = (Button*)add_tagged(new Button(Rect(400, 204, 180, 30), "Check for Updates", Button::Primary),
                                        PN_Update, false);
        check_btn->on_click = [this] { start_check(); };

        autoupd = (Toggle*)add_tagged(new Toggle(Rect(670, 276, 50, kRowH), g_autoupdate), PN_Update, false);
        autoupd->on_change = [this](bool on) { g_autoupdate = on; save_prefs(); invalidate(); };

        rebuild_shown();
        select_pane(PN_General);
        set_ticking(true);
    }

    ~SettingsWindow() override {
        for (int i = 0; i < 6; i++) delete thumbs[i];
    }

    void on_paint(Canvas& c) override {
        const Theme& t = theme();
        c.clear(t.window_bg);
        c.save();
        c.clip_to(Rect(0, 0, kSideW, height()));
        c.clear(rgba(0, 0, 0, 0));
        c.restore();
        c.fill_rect(Rect(kSideW - 1, 0, 1, height()), t.separator);

        paint_sidebar(c);
        switch (pane) {
            case PN_General: paint_general(c); break;
            case PN_Appearance: paint_appearance(c); break;
            case PN_Wallpaper: paint_wallpaper(c); break;
            case PN_Dock: paint_dock(c); break;
            case PN_Displays: paint_displays(c); break;
            case PN_Network: paint_network(c); break;
            case PN_Keyboard: paint_keyboard(c); break;
            case PN_DateTime: paint_datetime(c); break;
            case PN_Users: paint_users(c); break;
            case PN_Activation: paint_activation(c); break;
            case PN_Update: paint_update(c); break;
            case PN_Storage: paint_storage(c); break;
            case PN_About: paint_about(c); break;
        }
        // Widgets go last so they sit above the grouped boxes.
        for (Widget* w : widgets())
            if (w->visible) {
                c.save();
                w->paint(c);
                c.restore();
            }
    }

    void on_mouse_down(int x, int y, int button) override {
        if (x < kSideW) {
            if (y >= kListY) {
                int k = (y - kListY) / kListStep;
                if ((y - kListY) % kListStep < 28 && k >= 0 && k < (int)shown.size()) select_pane(shown[k]);
                return;
            }
            Window::on_mouse_down(x, y, button);
            return;
        }
        if (pane == PN_Appearance) {
            for (int i = 0; i < 3; i++)
                if (tile_rect(i).contains(x, y)) { focus_widget(nullptr); apply_appearance(i); sync_values(); invalidate(); return; }
            for (int i = 0; i < 8; i++) {
                Rect r = accent_circle(i);
                int dx = x - (r.x + 12), dy = y - (r.y + 12);
                if (dx * dx + dy * dy <= 144) {
                    focus_widget(nullptr);
                    theme_set_accent(kAccents[i]);
                    save_prefs();
                    invalidate();
                    return;
                }
            }
        }
        if (pane == PN_Wallpaper) {
            for (int i = 0; i < wallpaper_count(); i++)
                if (wallpaper_rect(i).contains(x, y)) {
                    focus_widget(nullptr);
                    wm::set_wallpaper(i);
                    save_prefs();
                    invalidate();
                    return;
                }
        }
        Window::on_mouse_down(x, y, button);
    }

    void on_tick(uint64_t now) override {
        uint64_t sec = now / 1000;
        if (sec != last_sec) {
            last_sec = sec;
            if (g_appearance == 2 && auto_wants_dark() != theme_is_dark()) theme_set_dark(auto_wants_dark());
            if (pane == PN_Network || pane == PN_DateTime || pane == PN_General) invalidate();
        }
        if (checking) {
            if (now - check_start >= 2600) {
                checking = false;
                DateTime d = rtc_now();
                check_h = d.hour;
                check_m = d.minute;
                checked_once = true;
                sync_values();
            }
            invalidate();
        }
    }

private:
    // ----- state
    int pane = PN_General;
    Vector<int> shown;
    TextField* search = nullptr;
    Slider* glass = nullptr;
    Toggle* reduce = nullptr;
    Toggle* dock_mag = nullptr;
    Dropdown* layout = nullptr;
    TextField* keytest = nullptr;
    Dropdown* timezone = nullptr;
    TextField* key = nullptr;
    Button* activate_btn = nullptr;
    Button* check_btn = nullptr;
    Toggle* autoupd = nullptr;
    Canvas* thumbs[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    uint64_t last_sec = 0;
    bool checking = false;
    uint64_t check_start = 0;
    bool checked_once = false;
    int check_h = 0, check_m = 0;
    bool activated = false;
    productkey::KeyInfo info{productkey::Edition::Invalid, productkey::Channel::Retail, 0};
    String key_text;
    String key_error;

    // Widgets are created once and shown or hidden per pane (tag = pane, -1 = always).
    Widget* add_tagged(Widget* w, int tag, bool vis) {
        w->tag = tag;
        w->visible = vis;
        add(w);
        return w;
    }

    void load_activation() {
        String text;
        if (!vfs::read_text(activation_path(), text)) return;
        if (productkey::validate(text.c_str(), &info)) {
            activated = true;
            key_text = text.trim();
        }
    }

    void activate_key() {
        if (activated) return;
        productkey::KeyInfo found{productkey::Edition::Invalid, productkey::Channel::Retail, 0};
        if (!productkey::validate(key->get_text(), &found)) {
            key_error = "That product key isn't valid. Check it and try again.";
            invalidate();
            return;
        }
        char formatted[32];
        productkey::format_partial(key->get_text(), formatted);
        vfs::mkdir("/System/Config");
        if (!vfs::write_text(activation_path(), formatted)) {
            key_error = "Could not save the activation.";
            invalidate();
            return;
        }
        info = found;
        key_text = formatted;
        activated = true;
        key_error = "";
        key->set_text("");
        sync_visibility();
        invalidate();
    }

    void start_check() {
        if (checking) return;
        checking = true;
        check_start = timer_ms();
        sync_values();
        invalidate();
    }

    void rebuild_shown() {
        shown.clear();
        const char* q = search ? search->get_text() : "";
        for (int i = 0; i < PN_Count; i++)
            if (has_ci(kPanes[i].name, q)) shown.push(i);
    }

    void select_pane(int p) {
        pane = p;
        focus_widget(nullptr);
        sync_values();
        sync_visibility();
        invalidate();
    }

    void sync_visibility() {
        for (Widget* w : widgets()) {
            bool v = w->tag == -1 || w->tag == pane;
            if (w == key || w == activate_btn) v = v && !activated;
            w->visible = v;
        }
    }

    void sync_values() {
        glass->value = wm::glass_intensity() / 1.5f;
        reduce->on = wm::reduce_transparency();
        dock_mag->on = wm::dock_magnification();
        layout->selected = (int)input_layout();
        timezone->selected = zone_index_for(rtc_utc_offset_minutes());
        autoupd->on = g_autoupdate;
        check_btn->enabled = !checking;
        check_btn->text = checking ? "Checking..." : "Check for Updates";
        activate_btn->enabled = !activated;
    }

    // ----- sidebar
    void paint_sidebar(Canvas& c) {
        const Theme& t = theme();
        Rect card(10, 50, 200, 56);
        c.fill_round_rect(card, 12, t.dark ? rgba(255, 255, 255, 20) : rgba(255, 255, 255, 150));
        draw_avatar(c, 20, 58, 40, session_full_name(), 15);
        c.draw_text_in(Rect(70, 62, 136, 20), session_full_name(), t.text, 13, Font::Bold);
        c.draw_text_in(Rect(70, 82, 136, 16), "KleinOS Account", t.text_secondary, 11, Font::Regular);

        for (int k = 0; k < (int)shown.size(); k++) {
            int p = shown[k];
            int y = kListY + k * kListStep;
            bool sel = p == pane;
            if (sel) c.fill_round_rect(Rect(10, y, 200, 28), 8, t.accent);
            mini_icon(c, 16, y + 4, 20, kPanes[p].sym, kPanes[p].top, kPanes[p].bottom);
            c.draw_text_in(Rect(44, y, 164, 28), kPanes[p].name, sel ? t.accent_text : t.text, 13,
                           sel ? Font::Medium : Font::Regular);
        }
    }

    // ----- panes
    void paint_general(Canvas& c) {
        pane_title(c, "General", "Version, language and region");
        group(c, 80, 4);
        char build[24], tz[24];
        snprintf(build, sizeof(build), "%d", system_build_number());
        row_title(c, 80, 0, "Version");
        char version[48];
        snprintf(version, sizeof(version), "KleinOS %s.%d %s", KLEIN_VERSION, system_build_number(), KLEIN_CODENAME);
        row_value(c, 80, 0, version);
        row_title(c, 80, 1, "Build");
        row_value(c, 80, 1, build);
        row_title(c, 80, 2, "Edition");
        row_value(c, 80, 2, activated ? productkey::edition_name(info.edition) : "Not activated");
        row_title(c, 80, 3, "Boot mode");
        row_value(c, 80, 3, system_boot_mode_name());

        group(c, 280, 2);
        row_title(c, 280, 0, "Keyboard");
        row_value(c, 280, 0, kLayoutNames[(int)input_layout()]);
        row_title(c, 280, 1, "Time zone");
        fmt_offset(rtc_utc_offset_minutes(), tz, sizeof(tz));
        row_value(c, 280, 1, tz);
    }

    void paint_appearance(Canvas& c) {
        const Theme& t = theme();
        pane_title(c, "Appearance", "Light or dark mode, accent color and glass");
        const char* names[3] = {"Light", "Dark", "Auto"};
        for (int i = 0; i < 3; i++) {
            Rect r = tile_rect(i);
            paint_mode_tile(c, r, i, g_appearance == i);
            c.draw_text_in(Rect(r.x, r.bottom() + 8, r.w, 18), names[i], t.text, 13, Font::Medium, Align::Center);
        }
        group(c, 212, 3);
        row_title(c, 212, 0, "Accent color");
        int cur = current_accent_index();
        for (int i = 0; i < 8; i++) {
            Rect r = accent_circle(i);
            c.fill_circle(r.x + 12, r.y + 12, 10, kAccents[i]);
            if (i == cur) {
                c.stroke_circle(r.x + 12, r.y + 12, 13, t.text_secondary, 2.0f);
                draw_symbol(c, Rect(r.x + 7, r.y + 7, 10, 10), Symbol::Check, hex(0xFFFFFF), 1.8f);
            }
        }

        row_title(c, 212, 1, "Liquid Glass", "Strength of glass effects");
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", (int)(wm::glass_intensity() * 100.0f / 1.5f + 0.5f));
        c.draw_text_in(Rect(636, kGlassRowY, 84, kRowH), pct, t.text_secondary, 13, Font::Regular, Align::Right);

        row_title(c, 212, 2, "Reduce transparency", "Solid backgrounds instead of glass");
    }

    void ensure_thumbs() {
        for (int i = 0; i < 6 && i < wallpaper_count(); i++) {
            if (thumbs[i]) continue;
            thumbs[i] = new Canvas(154, 96);
            wallpaper_render(*thumbs[i], i);
            round_mask(*thumbs[i], 10);
        }
    }

    void paint_wallpaper(Canvas& c) {
        const Theme& t = theme();
        pane_title(c, "Wallpaper", "Click a wallpaper to use it on your desktop");
        ensure_thumbs();
        int cur = wm::wallpaper();
        for (int i = 0; i < wallpaper_count() && i < 6; i++) {
            Rect r = wallpaper_rect(i);
            c.draw(*thumbs[i], r.x, r.y);
            c.stroke_round_rect(r, 10, t.separator, 1);
            if (i == cur) c.stroke_round_rect(Rect(r.x - 4, r.y - 4, r.w + 8, r.h + 8), 13, t.accent, 2.5f);
            c.draw_text_in(Rect(r.x, r.bottom() + 8, r.w, 18), wallpaper_name(i), i == cur ? t.accent : t.text, 13,
                           i == cur ? Font::Medium : Font::Regular, Align::Center);
        }
    }

    void paint_dock(Canvas& c) {
        const Theme& t = theme();
        pane_title(c, "Dock & Desktop", "Dock magnification and desktop preview");
        group(c, 80, 1);
        row_title(c, 80, 0, "Magnification", "Enlarge icons under the pointer");

        // desktop preview with a Dock on top
        Rect box(kX0, 176, kW, 210);
        ensure_thumbs();
        c.draw_scaled(*thumbs[wm::wallpaper()], box);
        c.stroke_round_rect(box, 14, t.separator, 1);
        bool mag = wm::dock_magnification();
        int n = 6, base = 40, gap = 14;
        int pill_w = n * base + (n - 1) * gap + 40, pill_h = 62;
        Rect pill(box.x + (box.w - pill_w) / 2, box.bottom() - pill_h - 14, pill_w, pill_h);
        c.fill_round_rect(pill, 22, rgba(255, 255, 255, t.dark ? 50 : 110));
        c.stroke_round_rect(pill, 22, rgba(255, 255, 255, 150), 1);
        for (int i = 0; i < n; i++) {
            int d = base;
            if (mag) {
                int dist = i - (n / 2);
                d = dist == 0 ? 56 : (dist == 1 || dist == -1 ? 46 : base);
            }
            int cx = pill.x + 20 + i * (base + gap) + base / 2;
            int y = pill.y + pill.h / 2 - d / 2;
            const PaneDef& p = kPanes[(i * 3 + 2) % PN_Count];
            mini_icon(c, cx - d / 2, y, d, p.sym, p.top, p.bottom);
        }
    }

    void paint_displays(Canvas& c) {
        const Theme& t = theme();
        FramebufferInfo& fb = framebuffer();
        pane_title(c, "Displays", "Resolution and arrangement");
        char res[32], depth[16];
        snprintf(res, sizeof(res), "%u x %u", fb.width, fb.height);
        snprintf(depth, sizeof(depth), "%u-bit", fb.bpp);
        group(c, 80, 3);
        row_title(c, 80, 0, "Resolution");
        row_value(c, 80, 0, res);
        row_title(c, 80, 1, "Color depth");
        row_value(c, 80, 1, depth);
        row_title(c, 80, 2, "Display");
        row_value(c, 80, 2, "Built-in (main)");

        if (fb.width == 0 || fb.height == 0) return;
        int pw = 320;
        int ph = (int)(fb.height * (uint32_t)pw / fb.width);
        if (ph > 200) { ph = 200; pw = (int)(fb.width * (uint32_t)ph / fb.height); }
        Rect scr(kX0, 80 + 3 * kRowH + 28, pw, ph);
        c.fill_round_rect(scr, 10, hex(0x111114));
        ensure_thumbs();
        c.draw_scaled(*thumbs[wm::wallpaper()], Rect(scr.x + 3, scr.y + 3, scr.w - 6, scr.h - 6));
        c.fill_rect(Rect(scr.x + 3, scr.y + 3, scr.w - 6, 6), rgba(0, 0, 0, 90));
        c.stroke_round_rect(scr, 10, t.separator, 1);
        c.draw_text_in(Rect(scr.x, scr.bottom() + 8, scr.w, 18), "Main display", t.text_secondary, 12,
                       Font::Regular, Align::Center);
    }

    void paint_network(Canvas& c) {
        const Theme& t = theme();
        net::Status s = net::status();
        pane_title(c, "Network", "Adapter and address information");
        group(c, 80, 9);
        const char* state;
        Color state_col;
        if (!s.link) { state = "Not connected"; state_col = t.destructive; }
        else if (!s.configured) { state = "Acquiring address..."; state_col = t.warning; }
        else { state = "Connected"; state_col = t.success; }
        row_title(c, 80, 0, "Status");
        row_value(c, 80, 0, state, state_col);
        row_title(c, 80, 1, "Adapter");
        row_value(c, 80, 1, net::nic_name());

        char ip[20], mask[20], gw[20], dns[20], mac[24], rx[64], tx[64];
        if (s.link && s.configured) {
            net::ip_to_string(s.ip, ip);
            net::ip_to_string(s.netmask, mask);
            net::ip_to_string(s.gateway, gw);
            net::ip_to_string(s.dns, dns);
        } else {
            snprintf(ip, sizeof(ip), "Not assigned");
            snprintf(mask, sizeof(mask), "Not assigned");
            snprintf(gw, sizeof(gw), "Not assigned");
            snprintf(dns, sizeof(dns), "Not assigned");
        }
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4],
                 s.mac[5]);
        char rxb[24], txb[24];
        fmt_bytes(s.rx_bytes, rxb, sizeof(rxb));
        fmt_bytes(s.tx_bytes, txb, sizeof(txb));
        snprintf(rx, sizeof(rx), "%llu packets, %s", (unsigned long long)s.rx_packets, rxb);
        snprintf(tx, sizeof(tx), "%llu packets, %s", (unsigned long long)s.tx_packets, txb);

        row_title(c, 80, 2, "IP address");     row_value(c, 80, 2, ip);
        row_title(c, 80, 3, "Subnet mask");    row_value(c, 80, 3, mask);
        row_title(c, 80, 4, "Router");         row_value(c, 80, 4, gw);
        row_title(c, 80, 5, "DNS server");     row_value(c, 80, 5, dns);
        row_title(c, 80, 6, "MAC address");    row_value(c, 80, 6, mac);
        row_title(c, 80, 7, "Received");       row_value(c, 80, 7, rx);
        row_title(c, 80, 8, "Sent");           row_value(c, 80, 8, tx);
    }

    void paint_keyboard(Canvas& c) {
        const Theme& t = theme();
        pane_title(c, "Keyboard", "Input source and layout preview");
        group(c, 80, 2);
        row_title(c, 80, 0, "Input source");
        row_title(c, 80, 1, "Try it");

        int li = (int)input_layout();
        Rect board(kX0, 184, kW, 150);
        c.fill_round_rect(board, 14, t.dark ? rgba(255, 255, 255, 12) : rgba(0, 0, 0, 10));
        c.stroke_round_rect(board, 14, t.separator, 1);
        for (int r = 0; r < 3; r++) {
            const char* row = kKeyRows[li][r];
            int n = (int)strlen(row);
            int keyw = 38, gap = 5;
            int total = n * keyw + (n - 1) * gap;
            int x = board.x + (board.w - total) / 2;
            int y = board.y + 20 + r * 46;
            for (int k = 0; k < n; k++) {
                Rect kr(x + k * (keyw + gap), y, keyw, 38);
                c.draw_shadow(kr, 7, 2, rgba(0, 0, 0, 40), 0, 1);
                c.fill_round_rect(kr, 7, t.control_bg);
                c.stroke_round_rect(kr, 7, t.control_border, 1);
                char ch[2] = {row[k], 0};
                c.draw_text_in(kr, ch, t.text, 14, Font::Medium, Align::Center);
            }
        }
    }

    void paint_datetime(Canvas& c) {
        const Theme& t = theme();
        pane_title(c, "Date & Time", "Time zone and clock");
        group(c, 80, 3);
        row_title(c, 80, 0, "Time zone");
        row_title(c, 80, 1, "Local time");
        row_title(c, 80, 2, "Date");
        DateTime d = rtc_now();
        char tm[16], dt[64];
        snprintf(tm, sizeof(tm), "%02d:%02d", d.hour, d.minute);
        snprintf(dt, sizeof(dt), "%s, %s %d, %d", weekday_name(d.weekday), month_name(d.month), d.day, d.year);
        row_value(c, 80, 1, tm);
        row_value(c, 80, 2, dt);

        Rect card(kX0, 236, kW, 150);
        draw_group_box(c, card);
        char big[16];
        snprintf(big, sizeof(big), "%02d:%02d:%02d", d.hour, d.minute, d.second);
        c.draw_text_in(Rect(card.x, card.y + 24, card.w, 56), big, t.text, 46, Font::Bold, Align::Center);
        c.draw_text_in(Rect(card.x, card.y + 92, card.w, 20), dt, t.text_secondary, 14, Font::Regular, Align::Center);
        c.draw_text_in(Rect(card.x, card.y + 116, card.w, 16), kZones[zone_index_for(rtc_utc_offset_minutes())].name,
                       t.text_tertiary, 12, Font::Regular, Align::Center);
    }

    void paint_users(Canvas& c) {
        const Theme& t = theme();
        pane_title(c, "Users", "Your account on this computer");
        draw_avatar(c, kX0 + 4, 84, 64, session_full_name(), 24);
        c.draw_text(kX0 + 84, 92, session_full_name(), t.text, 18, Font::Bold);
        c.draw_text(kX0 + 84, 118, "KleinOS Account", t.text_secondary, 13);

        group(c, 176, 4);
        char home[64];
        snprintf(home, sizeof(home), "/Users/%s", session_user());
        row_title(c, 176, 0, "Full name");     row_value(c, 176, 0, session_full_name());
        row_title(c, 176, 1, "Account name");  row_value(c, 176, 1, session_user());
        row_title(c, 176, 2, "Home folder");   row_value(c, 176, 2, home);
        row_title(c, 176, 3, "Account type");  row_value(c, 176, 3, "Administrator");
    }

    void paint_activation(Canvas& c) {
        const Theme& t = theme();
        pane_title(c, "Activation", "Product key and license");
        draw_group_box(c, Rect(kX0, 80, kW, 96));
        int cx = kX0 + 42, cy = 128;
        if (activated) {
            c.fill_circle(cx, cy, 22, t.success);
            draw_symbol(c, Rect(cx - 11, cy - 11, 22, 22), Symbol::Check, hex(0xFFFFFF), 2.6f);
        } else {
            c.fill_circle(cx, cy, 22, t.warning);
            draw_symbol(c, Rect(cx - 11, cy - 11, 22, 22), Symbol::Warning, hex(0xFFFFFF), 2.2f);
        }
        c.draw_text_in(Rect(kX0 + 80, 100, 380, 22),
                       activated ? "KleinOS is activated" : "KleinOS is not activated", t.text, 16, Font::Bold);
        if (activated) {
            char sub[96];
            snprintf(sub, sizeof(sub), "%s  -  %s  -  Serial %u", productkey::edition_name(info.edition),
                     productkey::channel_name(info.channel), info.serial);
            c.draw_text_in(Rect(kX0 + 80, 126, 380, 18), sub, t.text_secondary, 12, Font::Regular);
        } else {
            c.draw_text_in(Rect(kX0 + 80, 126, 380, 18), "Enter a product key to activate this copy.",
                           t.text_secondary, 12, Font::Regular);
        }

        group(c, 196, 2);
        if (activated) {
            row_title(c, 196, 0, "Product key");
            row_value(c, 196, 0, key_text.c_str());
            row_title(c, 196, 1, "Edition");
            row_value(c, 196, 1, productkey::edition_name(info.edition));
        } else {
            row_title(c, 196, 0, "Product key");
            if (key_error.empty())
                row_title(c, 196, 1, "Enter 25 characters");
            else
                c.draw_text_in(Rect(kX0 + 16, 196 + kRowH, kW - 32, kRowH), key_error.c_str(), t.destructive, 13,
                               Font::Medium);
        }
    }

    void paint_update(Canvas& c) {
        const Theme& t = theme();
        pane_title(c, "Software Update", "KleinOS 1.0 Aurora");
        draw_group_box(c, Rect(kX0, 80, kW, 176));
        int cx = kX0 + kW / 2, cy = 118;
        if (checking) {
            c.fill_arc(cx, cy, 24, 18, 0, 6.2831853f, rgba(128, 128, 128, 60));
            float ph = (float)((timer_ms() % 900) / 900.0 * 6.2831853);
            c.fill_arc(cx, cy, 24, 18, ph, ph + 1.6f, t.accent);
            c.draw_text_in(Rect(kX0, 152, kW, 22), "Checking for updates...", t.text, 16, Font::Bold, Align::Center);
            c.draw_text_in(Rect(kX0, 176, kW, 16), "This only takes a moment.", t.text_secondary, 12, Font::Regular,
                           Align::Center);
        } else {
            c.fill_circle(cx, cy, 24, t.success);
            draw_symbol(c, Rect(cx - 12, cy - 12, 24, 24), Symbol::Check, hex(0xFFFFFF), 2.8f);
            c.draw_text_in(Rect(kX0, 152, kW, 22), "KleinOS 1.0 is up to date", t.text, 16, Font::Bold, Align::Center);
            char sub[96];
            if (checked_once) snprintf(sub, sizeof(sub), "Last checked today at %02d:%02d", check_h, check_m);
            else snprintf(sub, sizeof(sub), "%s.%d is the latest release for this channel", KLEIN_VERSION, system_build_number());
            c.draw_text_in(Rect(kX0, 176, kW, 16), sub, t.text_secondary, 12, Font::Regular, Align::Center);
        }

        group(c, 276, 2);
        row_title(c, 276, 0, "Install updates automatically");
        row_title(c, 276, 1, "Installed version");
        char build[64];
        snprintf(build, sizeof(build), "%s.%d", KLEIN_VERSION, system_build_number());
        row_value(c, 276, 1, build);
    }

    void paint_storage(Canvas& c) {
        const Theme& t = theme();
        pane_title(c, "Storage", "Drives detected on this computer");
        unsigned long long max_sectors = 0;
        int count = 0;
        for (int i = 0; i < ata_drive_count(); i++) {
            AtaDrive* d = ata_drive(i);
            if (!d || !d->present) continue;
            count++;
            if (!d->atapi && d->sectors > max_sectors) max_sectors = d->sectors;
        }
        if (count == 0) {
            draw_group_box(c, Rect(kX0, 80, kW, 80));
            c.draw_text_in(Rect(kX0, 80, kW, 80), "No drives detected", t.text_secondary, 14, Font::Regular, Align::Center);
            return;
        }
        int y = 80;
        for (int i = 0; i < ata_drive_count(); i++) {
            AtaDrive* d = ata_drive(i);
            if (!d || !d->present) continue;
            Rect card(kX0, y, kW, 78);
            draw_group_box(c, card);
            // drive glyph
            c.fill_gradient(Rect(card.x + 16, card.y + 20, 46, 32), hex(0xF2F2F7), hex(0xA9AAB2), 7);
            c.stroke_round_rect(Rect(card.x + 16, card.y + 20, 46, 32), 7, rgba(0, 0, 0, 50), 1);
            c.fill_circle(card.x + 52, card.y + 42, 3, hex(0x30D158));
            c.fill_rect(Rect(card.x + 24, card.y + 40, 20, 3), rgba(0, 0, 0, 60));

            String model = String(d->model).trim();
            if (model.empty()) model = "Unnamed drive";
            c.draw_text_in(Rect(card.x + 76, card.y + 12, 300, 20), model.c_str(), t.text, 14, Font::Bold);
            c.draw_text_in(Rect(card.x + 76, card.y + 32, 300, 16), kDriveLocations[i], t.text_secondary, 12,
                           Font::Regular);
            char cap[32];
            if (d->atapi) {
                snprintf(cap, sizeof(cap), "Optical");
                c.draw_text_in(Rect(card.x + 260, card.y + 12, kW - 276, 20), cap, t.text_secondary, 13,
                               Font::Regular, Align::Right);
            } else {
                unsigned long long gb = d->sectors / 2097152ull;
                if (gb == 0) snprintf(cap, sizeof(cap), "%llu MB", d->sectors / 2048ull);
                else snprintf(cap, sizeof(cap), "%llu GB", gb);
                c.draw_text_in(Rect(card.x + 260, card.y + 12, kW - 276, 20), cap, t.text_secondary, 13,
                               Font::Regular, Align::Right);
                int bx = card.x + 76, bw = kW - 76 - 16;
                c.fill_round_rect(Rect(bx, card.y + 56, bw, 8), 4, t.dark ? rgba(255, 255, 255, 30) : rgba(0, 0, 0, 24));
                float frac = max_sectors ? (float)((double)d->sectors / (double)max_sectors) : 0.0f;
                int fw = (int)(bw * kclamp(frac, 0.02f, 1.0f));
                c.fill_round_rect(Rect(bx, card.y + 56, fw, 8), 4, t.accent);
            }
            y += 78 + 12;
        }
    }

    void paint_about(Canvas& c) {
        const Theme& t = theme();
        draw_logo_mark(c, Rect(kX0, 80, 72, 72));
        c.draw_text(kX0 + 92, 84, "KleinOS", t.text, 26, Font::Bold);
        char ver[96];
        snprintf(ver, sizeof(ver), "Version %s.%d %s", KLEIN_VERSION, system_build_number(), KLEIN_CODENAME);
        c.draw_text(kX0 + 92, 118, ver, t.text_secondary, 13);

        FramebufferInfo& fb = framebuffer();
        char mem[32], disp[40], up[32], mods[48];
        snprintf(mem, sizeof(mem), "%llu MB", (unsigned long long)(boot_info().mem_total >> 20));
        snprintf(disp, sizeof(disp), "%u x %u, %u-bit", fb.width, fb.height, fb.bpp);
        uint64_t s = timer_ms() / 1000;
        snprintf(up, sizeof(up), "%lluh %02llum %02llus", (unsigned long long)(s / 3600),
                 (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
        snprintf(mods, sizeof(mods), "%zu .dl modules, %d apps", dl_modules().size(), app_count());
        group(c, 176, 6);
        row_title(c, 176, 0, "Processor");   row_value(c, 176, 0, cpu_brand());
        row_title(c, 176, 1, "Memory");      row_value(c, 176, 1, mem);
        row_title(c, 176, 2, "Display");     row_value(c, 176, 2, disp);
        row_title(c, 176, 3, "Modules");     row_value(c, 176, 3, mods);
        row_title(c, 176, 4, "Uptime");      row_value(c, 176, 4, up);
        row_title(c, 176, 5, "Boot mode");   row_value(c, 176, 5, system_boot_mode_name());
        set_ticking(true);
    }

};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x9AA0A9), hex(0x4B5260));
    draw_symbol(c, r.inset(r.w / 4), Symbol::Gear, hex(0xFFFFFF), r.w / 20.0f);
}

Window* create() { return new SettingsWindow(); }

AppRegistrar reg({"settings", "Settings", icon, create, "System", true, true, 90,
                  "Appearance, wallpaper, network, keyboard, date and more"});

}  // namespace
