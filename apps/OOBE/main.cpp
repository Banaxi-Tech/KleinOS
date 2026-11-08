// Setup Assistant: first-boot setup. Walks through language, region, keyboard,
// network, account, privacy and appearance, then writes the first user into the
// system configuration, applies the choices and enters the desktop.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "gfx/wallpaper.h"
#include "drivers/input.h"
#include "drivers/rtc.h"
#include "net/net.h"
#include "sys/session.h"
#include "sys/sysconfig.h"
#include "arch/interrupts.h"
#include "lib/string.h"
#include "lib/math.h"

namespace {

constexpr int kCardW = 620, kCardH = 460, kCardRadius = 26, kShadowPad = 60;
constexpr float kPi = 3.14159265f;
constexpr int kFooterTag = -1;
constexpr int kAvatarCount = 6;
// Same palette as Login (UserAccount::avatar_color indexes it).
constexpr Color kAvatarColors[kAvatarCount] = {hex(0x0A84FF), hex(0xA550F7), hex(0xF74F9E),
                                               hex(0xFF9F0A), hex(0x30D158), hex(0x8E8E93)};
constexpr Color kAccents[8] = {kAccentBlue,   kAccentPurple, kAccentPink,   kAccentRed,
                               kAccentOrange, kAccentYellow, kAccentGreen,  kAccentGraphite};

enum Page { kHello, kRegion, kKeyboard, kNetwork, kAccount, kPrivacy, kAppearance, kSetup, kPageCount };
constexpr int kDotCount = kSetup;   // step dots shown before the spinner page

struct Region { const char* name; int utc_min; };
const Region kRegions[] = {
    {"United States (Eastern)", -300}, {"United States (Pacific)", -480}, {"United Kingdom", 0},
    {"Ireland", 0},                    {"France", 60},                    {"Germany", 60},
    {"Austria", 60},                   {"Spain", 60},                     {"India", 330},
    {"China", 480},                    {"Japan", 540},                    {"Australia (Sydney)", 600},
};
const int kRegionCount = sizeof(kRegions) / sizeof(kRegions[0]);

const char* const kLayoutNames[4] = {"U.S.", "British", "German", "French"};
const char* const kLayoutDetails[4] = {"English (US)", "English (UK)", "German (QWERTZ)", "French (AZERTY)"};
const char* const kHellos[] = {"hello", "hallo", "bonjour", "hola", "ciao", "hej"};
const int kHelloCount = sizeof(kHellos) / sizeof(kHellos[0]);

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// "Jane Doe" -> "janedoe"
void derive_login(const char* full, char* out, int n) {
    int k = 0;
    for (const char* p = full; *p && k < n - 1; p++) {
        char ch = *p;
        if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) out[k++] = ch;
    }
    out[k] = 0;
}

bool valid_login(const char* s) {
    int len = (int)strlen(s);
    if (len < 1 || len > 31) return false;
    for (const char* p = s; *p; p++) {
        char ch = *p;
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                  ch == '.' || ch == '_' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

bool has_text(const char* s) {
    for (; *s; s++)
        if (*s != ' ') return true;
    return false;
}

void format_utc(char* out, int n, int minutes) {
    if (minutes == 0) { snprintf(out, n, "UTC"); return; }
    int a = minutes < 0 ? -minutes : minutes;
    char sign = minutes < 0 ? '-' : '+';
    if (a % 60) snprintf(out, n, "UTC%c%d:%02d", sign, a / 60, a % 60);
    else snprintf(out, n, "UTC%c%d", sign, a / 60);
}

// Soft radial light, premultiplied, drawn with Canvas::draw.
Canvas* make_glow(int d, int r, int g, int b, int peak) {
    Canvas* s = new Canvas(d, d);
    int rad = d / 2, r2 = rad * rad;
    for (int y = 0; y < d; y++) {
        uint32_t* row = s->row(y);
        for (int x = 0; x < d; x++) {
            int dx = x - rad, dy = y - rad, d2 = dx * dx + dy * dy;
            if (d2 >= r2) { row[x] = 0; continue; }
            int t = 255 - (d2 * 255) / r2;
            int a = (peak * t * t) / (255 * 255);
            row[x] = premul(rgba(r, g, b, a));
        }
    }
    return s;
}

void draw_logo(Canvas& c, Rect r) {
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

void draw_swatch(Canvas& c, Rect r, Color col, bool selected) {
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
    c.fill_circle(cx, cy, r.w / 2.0f, col);
    if (selected) {
        c.stroke_circle(cx, cy, r.w / 2.0f + 3.5f, col, 2.0f);
        draw_symbol(c, r.inset(7), Symbol::Check, hex(0xFFFFFF), 2.2f);
    }
}

void draw_theme_tile(Canvas& c, Rect r, bool dark_tile, bool selected, Color accent) {
    c.fill_round_rect(r, 16, dark_tile ? hex(0x1C1C1E) : hex(0xEFEFF4));
    Rect win(r.x + 22, r.y + 22, r.w - 44, r.h - 44);
    c.fill_round_rect(win, 10, dark_tile ? hex(0x2C2C2E) : hex(0xFFFFFF));
    for (int i = 0; i < 3; i++) c.fill_circle(win.x + 12 + i * 10.0f, win.y + 12.0f, 3.0f, rgba(128, 128, 128, 140));
    Color bar = dark_tile ? hex(0x3A3A3C) : hex(0xE5E5EA);
    c.fill_round_rect(Rect(win.x + 12, win.y + 30, (win.w * 3) / 5, 8), 4, accent);
    c.fill_round_rect(Rect(win.x + 12, win.y + 48, win.w - 24, 8), 4, bar);
    c.fill_round_rect(Rect(win.x + 12, win.y + 62, (win.w * 2) / 5, 8), 4, bar);
    c.stroke_round_rect(r, 16, selected ? accent : rgba(0, 0, 0, 30), selected ? 3.0f : 1.0f);
}

// Custom-drawn control: paints through a callback and reports clicks.
class Hot : public Widget {
public:
    Hot(Rect r, Func<void(Canvas&, Rect)> fn) : Widget(r), draw_(fn) {}
    void paint(Canvas& c) override {
        if (draw_) draw_(c, bounds);
    }
    void mouse_up(int x, int y, int) override {
        if (on_click && bounds.contains(x, y)) on_click();
    }
    Func<void()> on_click;

private:
    Func<void(Canvas&, Rect)> draw_;
};

class SetupWindow : public Window {
public:
    SetupWindow() : Window("", 640, 480, WF_FULLSCREEN | WF_NO_ZOOM | WF_NO_MINIMIZE) {
        wm::set_shell_visible(false);
        sw_ = width();
        sh_ = height();
        cx_ = (sw_ - kCardW) / 2;
        cy_ = (sh_ - kCardH) / 2;
        accent_ = theme().accent;
        dark_ = theme_is_dark();

        // Background: wallpaper rendered once; a light drifts across it in on_paint.
        bg_ = new Canvas(sw_, sh_);
        wallpaper_render(*bg_, 0);
        light_ = make_glow(560, 255, 255, 255, 70);
        shadow_ = new Canvas(kCardW + 2 * kShadowPad, kCardH + 2 * kShadowPad);
        shadow_->draw_shadow(Rect(kShadowPad, kShadowPad, kCardW, kCardH), kCardRadius, 36, rgba(0, 0, 0, 120), 0, 14);

        build();
        show(kHello);
        set_ticking(true);
    }

    ~SetupWindow() override {
        delete bg_;
        delete light_;
        delete shadow_;
    }

    void on_paint(Canvas& c) override {
        c.blit(*bg_, 0, 0);
        float t = now_ / 1000.0f;
        int d = light_->width();
        c.draw(*light_, (int)(sw_ * 0.5f + cosf(t * 0.11f) * sw_ * 0.30f) - d / 2,
               (int)(sh_ * 0.5f + sinf(t * 0.17f) * sh_ * 0.26f) - d / 2);
        c.draw(*shadow_, cx_ - kShadowPad, cy_ - kShadowPad);

        bool dark = theme_is_dark();
        Color ink = dark ? hex(0xF5F5F7) : hex(0x1D1D1F);
        Color ink2 = dark ? hex(0x9A9AA0) : hex(0x6E6E73);
        Color panel = dark ? rgba(255, 255, 255, 14) : rgba(0, 0, 0, 9);
        c.fill_round_rect(at(0, 0, kCardW, kCardH), kCardRadius, dark ? rgba(30, 30, 34, 200) : rgba(255, 255, 255, 200));
        c.stroke_round_rect(at(0, 0, kCardW, kCardH), kCardRadius, dark ? rgba(255, 255, 255, 36) : rgba(255, 255, 255, 255), 1.0f);

        if (page_ != kSetup) {
            int x0 = cx_ + kCardW / 2 - (kDotCount * 14) / 2;
            for (int i = 0; i < kDotCount; i++)
                c.fill_circle(x0 + i * 14 + 3.0f, cy_ + 18.0f, 3.0f, i == page_ ? ink : with_alpha(ink, 60));
        }

        switch (page_) {
        case kHello: {
            draw_logo(c, at(kCardW / 2 - 36, 56, 72, 72));
            int idx = (int)((now_ / 1500) % kHelloCount);
            uint32_t phase = (uint32_t)(now_ % 1500);
            int a = 255;
            if (phase < 300) a = (int)(phase * 255 / 300);
            else if (phase > 1200) a = (int)((1500 - phase) * 255 / 300);
            c.draw_text_in(at(0, 150, kCardW, 92), kHellos[idx], with_alpha(ink, a), 72, Font::Bold, Align::Center);
            c.draw_text_in(at(0, 258, kCardW, 24), "Let's set up your computer.", ink2, 18, Font::Regular, Align::Center);
            break;
        }
        case kRegion:
            header(c, "Select your country or region", "This sets the time zone for your computer.", ink, ink2);
            c.fill_round_rect(at(60, 112, 500, 262), 14, panel);
            break;
        case kKeyboard:
            header(c, "Choose your keyboard layout", "You can change this later in Settings.", ink, ink2);
            c.fill_round_rect(at(60, 112, 500, 152), 14, panel);
            break;
        case kNetwork: {
            header(c, "Connect to a network", "KleinOS uses Ethernet to get online.", ink, ink2);
            net::Status st = net::status();
            bool online = st.link && st.configured;
            float cx = cx_ + kCardW / 2.0f, cy = cy_ + 186.0f;
            c.fill_circle(cx, cy, 44, online ? hex(0x30D158) : with_alpha(ink2, 70));
            Rect icon(cx_ + kCardW / 2 - 22, cy_ + 164, 44, 44);
            if (online) draw_symbol(c, icon, Symbol::Check, hex(0xFFFFFF), 3.0f);
            else if (st.link) draw_symbol(c, icon, Symbol::Refresh, ink, 2.5f);
            else draw_symbol(c, icon, Symbol::WifiOff, ink, 2.5f);
            char msg[96];
            if (online) snprintf(msg, sizeof(msg), "Connected via Ethernet, IP %s", net::ip_string());
            else if (st.link) snprintf(msg, sizeof(msg), "Cable connected, getting an address...");
            else snprintf(msg, sizeof(msg), "Not connected, you can continue offline");
            c.draw_text_in(at(0, 258, kCardW, 26), msg, ink, 17, Font::Medium, Align::Center);
            if (!st.link)
                c.draw_text_in(at(0, 288, kCardW, 20), "Plug in an Ethernet cable to get online later.", ink2, 14,
                               Font::Regular, Align::Center);
            break;
        }
        case kAccount:
            header(c, "Create your account", "This will be an administrator account on this computer.", ink, ink2);
            c.draw_text_in(at(60, 300, 300, 16), "Avatar color", ink2, 12, Font::Medium);
            if (!error_.empty()) c.draw_text_in(at(60, 372, 500, 18), error_.c_str(), hex(0xFF453A), 13, Font::Medium);
            break;
        case kPrivacy: {
            header(c, "Privacy", "Choose what this computer may share. You can change these later.", ink, ink2);
            static const char* const titles[3] = {"Share analytics", "Location services", "Personalized ads"};
            static const char* const descs[3] = {"Send anonymous usage data to help improve KleinOS.",
                                                 "Let apps use your general location for time and weather.",
                                                 "Show suggestions based on how you use KleinOS."};
            for (int i = 0; i < 3; i++) {
                int y = 112 + i * 78;
                c.draw_text_in(at(60, y + 10, 420, 20), titles[i], ink, 15, Font::Medium);
                c.draw_text_in(at(60, y + 32, 440, 18), descs[i], ink2, 13, Font::Regular);
                if (i < 2) c.fill_rect(at(60, y + 70, 500, 1), with_alpha(ink, 30));
            }
            c.draw_text_in(at(60, 358, 500, 18), "You can change these at any time in Settings.", ink2, 12, Font::Regular);
            break;
        }
        case kAppearance:
            header(c, "Choose your look", "Light or dark, and an accent color.", ink, ink2);
            c.draw_text_in(at(90, 274, 200, 18), "Light", dark ? ink2 : ink, 13, Font::Medium, Align::Center);
            c.draw_text_in(at(330, 274, 200, 18), "Dark", dark ? ink : ink2, 13, Font::Medium, Align::Center);
            c.draw_text_in(at(0, 318, kCardW, 16), "Accent color", ink2, 12, Font::Medium, Align::Center);
            break;
        case kSetup: {
            float cx = cx_ + kCardW / 2.0f, cy = cy_ + 186.0f;
            c.stroke_circle(cx, cy, 34, with_alpha(ink, 36), 5.0f);
            float a0 = (float)(now_ % 1257) * 0.005f;   // about 5 rad/s
            c.fill_arc(cx, cy, 37, 31, a0, a0 + 1.2f * kPi, theme().accent);
            c.draw_text_in(at(0, 258, kCardW, 26), "Setting up your account\xE2\x80\xA6", ink, 17, Font::Medium, Align::Center);
            break;
        }
        default: break;
        }

        const Vector<Widget*>& ws = widgets();
        for (size_t i = 0; i < ws.size(); i++) {
            Widget* w = ws[i];
            if (!w->visible) continue;
            c.save();
            w->paint(c);
            c.restore();
        }
    }

    void on_tick(uint64_t now) override {
        now_ = now;
        if (now - last_paint_ < 100) return;   // repaint at most about 10 times a second
        last_paint_ = now;
        if (page_ == kSetup) {
            if (setup_t0_ == 0) setup_t0_ = now;   // start the spinner timer on its first tick
            else if (now - setup_t0_ >= 3000) {
                finish();
                return;
            }
        }
        invalidate();
    }

    void on_key(const KeyEvent& e) override {
        if (e.key == Key::Enter && timer_ms() - page_shown_ < 800) return;   // ignore stray keys left over from boot
        if (e.key == Key::Enter && focused_widget() == nullptr && page_ != kSetup && page_ != kAccount) {
            advance();
            return;
        }
        Window::on_key(e);
    }

private:
    Rect at(int x, int y, int w, int h) const { return Rect(cx_ + x, cy_ + y, w, h); }

    void header(Canvas& c, const char* title, const char* sub, Color ink, Color ink2) {
        c.draw_text_in(at(0, 34, kCardW, 34), title, ink, 26, Font::Bold, Align::Center);
        c.draw_text_in(at(0, 72, kCardW, 20), sub, ink2, 14, Font::Regular, Align::Center);
    }

    Widget* place(Widget* w, int page) {
        add(w);
        w->tag = page;
        return w;
    }

    void build() {
        // Region
        int pick = 0;
        for (int i = 0; i < kRegionCount; i++)
            if (kRegions[i].utc_min == rtc_utc_offset_minutes()) { pick = i; break; }
        region_ = new ListView(at(60, 112, 500, 262));
        region_->transparent = true;
        region_->row_height = 33;
        for (int i = 0; i < kRegionCount; i++) {
            char detail[16];
            format_utc(detail, sizeof(detail), kRegions[i].utc_min);
            region_->add(kRegions[i].name, detail);
        }
        region_->selected = pick;
        region_->ensure_visible(pick);
        region_->on_activate = [this](int) { advance(); };
        place(region_, kRegion);

        // Keyboard
        kb_ = new ListView(at(60, 112, 500, 152));
        kb_->transparent = true;
        kb_->row_height = 38;
        for (int i = 0; i < 4; i++) kb_->add(kLayoutNames[i], kLayoutDetails[i]);
        kb_->selected = clampi((int)input_layout(), 0, 3);
        kb_->on_activate = [this](int) { advance(); };
        place(kb_, kKeyboard);

        // Account
        name_ = new TextField(at(60, 112, 500, 36), "Full name");
        name_->font_size = 15;
        name_->max_length = 63;
        name_->on_change = [this] {
            error_ = "";
            if (!login_touched_) sync_login();
        };
        place(name_, kAccount);

        login_ = new TextField(at(60, 158, 500, 36), "Account name");
        login_->font_size = 15;
        login_->max_length = 31;
        login_->accept_char = [](uint32_t ch) -> bool {
            return ch < 128 && (isalnum((int)ch) || ch == '.' || ch == '_' || ch == '-');
        };
        login_->on_change = [this] {
            login_touched_ = true;
            error_ = "";
        };
        place(login_, kAccount);

        pass_ = new TextField(at(60, 204, 500, 36), "Password");
        pass_->font_size = 15;
        pass_->password = true;
        pass_->max_length = 63;
        pass_->on_change = [this] { error_ = ""; };
        place(pass_, kAccount);

        confirm_ = new TextField(at(60, 250, 500, 36), "Confirm password");
        confirm_->font_size = 15;
        confirm_->password = true;
        confirm_->max_length = 63;
        confirm_->on_change = [this] { error_ = ""; };
        confirm_->on_submit = [this] { advance(); };
        place(confirm_, kAccount);

        for (int i = 0; i < kAvatarCount; i++) {
            Hot* h = new Hot(at(60 + i * 56, 322, 40, 40), [this, i](Canvas& c, Rect r) {
                draw_swatch(c, r, kAvatarColors[i], avatar_ == i);
            });
            h->on_click = [this, i] { avatar_ = i; invalidate(); };
            place(h, kAccount);
        }

        // Privacy (display only, KleinOS keeps no setting for these yet)
        for (int i = 0; i < 3; i++) place(new Toggle(at(kCardW - 60 - 50, 112 + i * 78 + 12, 50, 28), false), kPrivacy);

        // Appearance
        for (int i = 0; i < 2; i++) {
            Hot* h = new Hot(at(90 + i * 240, 116, 200, 150), [this, i](Canvas& c, Rect r) {
                draw_theme_tile(c, r, i == 1, dark_ == (i == 1), accent_);
            });
            h->on_click = [this, i] {
                dark_ = (i == 1);
                theme_set_dark(dark_);
                invalidate();
            };
            place(h, kAppearance);
        }
        for (int i = 0; i < 8; i++) {
            Color col = kAccents[i];
            Hot* h = new Hot(at(156 + i * 40, 338, 28, 28), [this, col](Canvas& c, Rect r) {
                draw_swatch(c, r, col, accent_ == col);
            });
            h->on_click = [this, col] {
                accent_ = col;
                theme_set_accent(col);
                invalidate();
            };
            place(h, kAppearance);
        }

        // Footer buttons
        back_ = new Button(at(40, 394, 110, 36), "Back");
        back_->on_click = [this] { back(); };
        add(back_);
        back_->tag = kFooterTag;
        continue_ = new Button(at(kCardW - 40 - 140, 394, 140, 36), "Continue", Button::Primary);
        continue_->on_click = [this] { advance(); };
        add(continue_);
        continue_->tag = kFooterTag;
    }

    void show(int page) {
        page_ = page;
        page_shown_ = timer_ms();
        const Vector<Widget*>& ws = widgets();
        for (size_t i = 0; i < ws.size(); i++) {
            Widget* w = ws[i];
            if (w != back_ && w != continue_) w->visible = (w->tag == page);
        }
        back_->visible = page != kHello && page != kSetup;
        continue_->visible = page != kSetup;
        setup_t0_ = 0;   // set on the next tick, see on_tick
        Widget* f = nullptr;
        if (page == kRegion) f = region_;
        else if (page == kKeyboard) f = kb_;
        else if (page == kAccount) f = name_;
        focus_widget(f);
        invalidate();
    }

    void advance() {
        if (page_ == kSetup) return;
        if (page_ == kAccount && !validate_account()) {
            invalidate();
            return;
        }
        show(page_ + 1);
    }

    void back() {
        if (page_ > kRegion && page_ != kSetup) show(page_ - 1);
    }

    void sync_login() {
        char buf[32];
        derive_login(name_->get_text(), buf, sizeof(buf));
        login_->set_text(buf);
    }

    bool validate_account() {
        if (!has_text(name_->get_text())) {
            error_ = "Enter your full name.";
            return false;
        }
        const char* login = login_->get_text();
        if (!valid_login(login)) {
            error_ = login[0] ? "Account names use letters, numbers, dots, _ and -." : "Choose an account name.";
            return false;
        }
        if (strcmp(pass_->get_text(), confirm_->get_text()) != 0) {
            error_ = "The passwords don't match.";
            return false;
        }
        error_ = "";
        return true;
    }

    // Writes the first account and settings, applies them live, then enters the desktop.
    void finish() {
        int region = region_->selected;
        if (region < 0 || region >= kRegionCount) region = 0;
        int utc = kRegions[region].utc_min;
        int layout = clampi(kb_->selected, 0, 3);
        bool dark = dark_;
        Color accent = accent_;

        String full(name_->get_text());
        char login[32];
        lower_copy(login_->get_text(), login, sizeof(login));
        char first[64];
        int n = 0;
        for (const char* p = full.c_str(); *p && *p != ' ' && n < 63; p++) first[n++] = *p;
        first[n] = 0;

        if (sysconfig_present()) {
            SysConfig& cfg = sysconfig();
            UserAccount& u = cfg.users[0];
            memset(&u, 0, sizeof(u));
            strncpy(u.full_name, full.c_str(), sizeof(u.full_name) - 1);
            strncpy(u.login, login, sizeof(u.login) - 1);
            sysconfig_hash_password(u.login, pass_->get_text(), u.password_hash);
            u.avatar_color = (uint8_t)avatar_;
            u.is_admin = 1;
            cfg.user_count = 1;
            cfg.utc_offset_min = utc;
            cfg.keyboard_layout = (uint32_t)layout;
            cfg.dark_mode = dark ? 1 : 0;
            cfg.accent = accent;
            snprintf(cfg.computer_name, sizeof(cfg.computer_name), "%s's KleinOS", first);
            cfg.setup_complete = 1;
            sysconfig_save();
        }

        input_set_layout((KeyboardLayout)layout);
        rtc_set_utc_offset_minutes(utc);
        theme_set_dark(dark);
        theme_set_accent(accent);
        session_enter_desktop(full.c_str(), login);
        close();
    }

    static void lower_copy(const char* src, char* out, int n) {
        int k = 0;
        for (const char* p = src; *p && k < n - 1; p++) {
            char ch = *p;
            if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
            out[k++] = ch;
        }
        out[k] = 0;
    }

    int sw_ = 0, sh_ = 0, cx_ = 0, cy_ = 0;
    Canvas* bg_ = nullptr;
    Canvas* light_ = nullptr;
    Canvas* shadow_ = nullptr;
    int page_ = kHello;
    uint64_t page_shown_ = 0;
    uint64_t now_ = 0, last_paint_ = 0, setup_t0_ = 0;

    ListView* region_ = nullptr;
    ListView* kb_ = nullptr;
    TextField* name_ = nullptr;
    TextField* login_ = nullptr;
    TextField* pass_ = nullptr;
    TextField* confirm_ = nullptr;
    Button* back_ = nullptr;
    Button* continue_ = nullptr;
    bool login_touched_ = false;
    int avatar_ = 0;
    bool dark_ = false;
    Color accent_ = 0;
    String error_;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0xFF9F0A), hex(0xFF375F));
    draw_symbol(c, r.inset(r.w / 4), Symbol::Star, hex(0xFFFFFF), r.w / 20.0f);
}

Window* create() { return new SetupWindow(); }

AppRegistrar reg({"oobe", "Setup Assistant", icon, create, "System", true, false, 100, "Set up your KleinOS computer"});

}  // namespace
