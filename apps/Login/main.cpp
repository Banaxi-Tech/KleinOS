// Login window: shown on every boot after setup. Blurred wallpaper, a large
// clock, user avatars, a password field and power buttons.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "gfx/wallpaper.h"
#include "gfx/glass.h"
#include "drivers/power.h"
#include "drivers/rtc.h"
#include "sys/session.h"
#include "sys/sysconfig.h"
#include "sys/update.h"
#include "arch/interrupts.h"
#include "lib/string.h"
#include "lib/math.h"

namespace {

constexpr int kAvatarCount = 6;
// Same palette as Setup Assistant (UserAccount::avatar_color indexes it).
constexpr Color kAvatarColors[kAvatarCount] = {hex(0x0A84FF), hex(0xA550F7), hex(0xF74F9E),
                                               hex(0xFF9F0A), hex(0x30D158), hex(0x8E8E93)};

String initials_of(const char* name) {
    char out[3] = {0, 0, 0};
    int n = 0;
    bool word_start = true;
    for (const char* p = name; *p && n < 2; p++) {
        if (*p == ' ') { word_start = true; continue; }
        if (word_start && (uint8_t)*p < 0x80) {
            char ch = *p;
            if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
            out[n++] = ch;
            word_start = false;
        }
    }
    return String(out);
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

// Light translucent pill with masked (dotted) input.
class PillField : public TextField {
public:
    explicit PillField(Rect r) : TextField(r, "Password") {
        password = true;
        rounded = true;
        font_size = 16;
        max_length = 63;
    }

    void paint(Canvas& c) override {
        Rect r = bounds;
        float rad = r.h / 2.0f;
        bool f = focused();
        c.fill_round_rect(r, rad, f ? rgba(255, 255, 255, 70) : rgba(255, 255, 255, 42));
        c.stroke_round_rect(r, rad, rgba(255, 255, 255, f ? 160 : 90), 1.0f);

        const int pad = 12;   // matches the hit-test padding of TextField
        int ty = r.y + (r.h - line_height(font_size)) / 2;
        int dot_y = r.y + r.h / 2;
        c.save();
        c.clip_to(Rect(r.x + pad, r.y, r.w - 2 * pad, r.h));
        int chars = 0;
        for (const char* p = text.c_str(); *p; p++)
            if (((uint8_t)*p & 0xC0) != 0x80) chars++;
        if (chars == 0) {
            if (!placeholder.empty())
                c.draw_text(r.x + pad, ty, placeholder.c_str(), rgba(255, 255, 255, 140), font_size);
        } else {
            for (int i = 0; i < chars; i++)
                c.fill_circle(r.x + pad + 5.0f + i * 12.0f, dot_y, 4.0f, rgba(255, 255, 255, 240));
        }
        // caret, placed after the last dot (the field is always typed at the end here)
        if (f && (timer_ms() / 530) % 2 == 0)
            c.fill_rect(Rect(r.x + pad + chars * 12, r.y + (r.h - 18) / 2, 2, 18), rgba(255, 255, 255, 240));
        c.restore();
        if (f && window) window->set_ticking(true);
    }
};

void draw_arrow_button(Canvas& c, Rect r) {
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
    c.fill_circle(cx, cy, r.w / 2.0f, rgba(255, 255, 255, 236));
    Color ink = hex(0x1C1C1E);
    c.draw_line(cx - 7, cy, cx + 7, cy, ink, 2.2f);
    c.draw_line(cx + 1, cy - 6, cx + 7, cy, ink, 2.2f);
    c.draw_line(cx + 1, cy + 6, cx + 7, cy, ink, 2.2f);
}

// Glass round button with a caption underneath. r: 120 wide, 74 tall.
void draw_power(Canvas& c, Rect r, Symbol sym, const char* label) {
    Rect b(r.x + r.w / 2 - 24, r.y, 48, 48);
    GlassStyle gs = glass_style_control(true);
    gs.radius = 24;
    glass_render(c, b, gs, c.clip());
    draw_symbol(c, b.inset(14), sym, rgba(255, 255, 255, 235), 2.0f);
    c.draw_text_in(Rect(r.x, r.y + 56, r.w, 18), label, rgba(255, 255, 255, 210), 13, Font::Medium, Align::Center);
}

// Rounded button on the dark login backdrop: primary is solid white, the other translucent.
void draw_card_button(Canvas& c, Rect r, const char* label, bool primary) {
    float rad = r.h / 2.0f;
    c.fill_round_rect(r, rad, primary ? rgba(255, 255, 255, 236) : rgba(255, 255, 255, 46));
    if (!primary) c.stroke_round_rect(r, rad, rgba(255, 255, 255, 90), 1.0f);
    c.draw_text_in(r, label, primary ? hex(0x1C1C1E) : rgba(255, 255, 255, 240), 15, Font::Medium, Align::Center);
}

class LoginWindow : public Window {
public:
    LoginWindow() : Window("", 640, 480, WF_FULLSCREEN | WF_NO_ZOOM | WF_NO_MINIMIZE) {
        wm::set_shell_visible(false);
        sw_ = width();
        sh_ = height();

        // Blurred wallpaper, rendered once and darkened.
        bg_ = new Canvas(sw_, sh_);
        wallpaper_render(*bg_, wm::wallpaper());
        bg_->blur(Rect(0, 0, sw_, sh_), 30, 3);
        bg_->fill_rect(Rect(0, 0, sw_, sh_), rgba(0, 0, 0, 70));

        memset(&guest_, 0, sizeof(guest_));
        strncpy(guest_.full_name, "Guest", sizeof(guest_.full_name) - 1);
        strncpy(guest_.login, "guest", sizeof(guest_.login) - 1);
        guest_.avatar_color = 4;
        use_guest_ = !sysconfig_present() || sysconfig().user_count == 0;
        count_ = use_guest_ ? 1 : (int)(sysconfig().user_count > 4 ? 4 : sysconfig().user_count);

        int top = sh_ / 2 - 150;
        for (int i = 0; i < count_; i++) {
            int cxi = sw_ / 2 + (2 * i - (count_ - 1)) * 80;
            Hot* h = new Hot(Rect(cxi - 80, top, 160, 152), [this, i](Canvas& c, Rect r) {
                draw_user(c, r, i, i == selected_);
            });
            h->on_click = [this, i] { select(i); };
            add(h);
            user_hots_.push(h);
        }

        int px0 = sw_ / 2 - 177, py = sh_ / 2 + 36;
        pill_ = new PillField(Rect(px0, py, 300, 44));
        pill_x_ = px0;
        add(pill_);
        pill_->on_submit = [this] { if (mode_ == Mode::Reset) focus_widget(verify_); else attempt(); };

        Hot* go = new Hot(Rect(px0 + 310, py, 44, 44), [](Canvas& c, Rect r) { draw_arrow_button(c, r); });
        go->on_click = [this] { attempt(); };
        add(go);
        go_ = go;

        Hot* restart = new Hot(Rect(sw_ / 2 - 120, sh_ - 150, 120, 74), [](Canvas& c, Rect r) {
            draw_power(c, r, Symbol::Restart, "Restart");
        });
        restart->on_click = [] { update::confirm_by_user(); power_reboot(); };
        add(restart);
        restart_ = restart;

        Hot* shut = new Hot(Rect(sw_ / 2, sh_ - 150, 120, 74), [](Canvas& c, Rect r) {
            draw_power(c, r, Symbol::Power, "Shut Down");
        });
        shut->on_click = [] { update::confirm_by_user(); power_shutdown(); };
        add(shut);
        shut_ = shut;

        // ---- forgotten password
        forgot_ = new Hot(Rect(sw_ / 2 - 90, sh_ / 2 + 112, 180, 24), [](Canvas& c, Rect r) {
            c.draw_text_in(r, "Forgot password?", rgba(255, 255, 255, 215), 14, Font::Medium, Align::Center);
            int tw = text_width("Forgot password?", 14, Font::Medium);
            c.fill_rect(Rect(r.x + (r.w - tw) / 2, r.bottom() - 3, tw, 1), rgba(255, 255, 255, 120));
        });
        forgot_->on_click = [this] { set_mode(Mode::Confirm); };
        add(forgot_);

        Rect card = card_rect();
        int bw = 170, by = card.bottom() - 64;
        cancel_ = new Hot(Rect(sw_ / 2 - bw - 8, by, bw, 42), [](Canvas& c, Rect r) { draw_card_button(c, r, "Cancel", false); });
        cancel_->on_click = [this] {
            if (mode_ == Mode::Reset) sysconfig_recovery_cancel();
            set_mode(Mode::Normal);
        };
        add(cancel_);
        confirm_ = new Hot(Rect(sw_ / 2 + 8, by, bw, 42), [this](Canvas& c, Rect r) {
            draw_card_button(c, r, mode_ == Mode::Reset ? "Reset Password" : "Restart", true);
        });
        confirm_->on_click = [this] { if (mode_ == Mode::Reset) do_reset(); else restart_into_recovery(); };
        add(confirm_);

        verify_ = new PillField(Rect(sw_ / 2 - 150, card.y + 176, 300, 44));
        verify_->placeholder = "Verify password";
        verify_->on_submit = [this] { do_reset(); };
        add(verify_);

        // A recovery restart for one of the accounts: offer the reset first.
        const char* rec = sysconfig_recovery_login();
        Mode first = Mode::Normal;
        if (!use_guest_ && rec && *rec)
            for (int i = 0; i < count_; i++)
                if (!strcmp(user(i).login, rec)) { selected_ = i; first = Mode::Reset; }
        set_mode(first);
        set_ticking(true);
    }

    ~LoginWindow() override { delete bg_; }

    void on_paint(Canvas& c) override {
        c.blit(*bg_, 0, 0);

        DateTime dt = rtc_now();
        char hm[8];
        snprintf(hm, sizeof(hm), "%02d:%02d", dt.hour, dt.minute);
        c.draw_text_in(Rect(0, 48, sw_, 116), hm, rgba(255, 255, 255, 255), 96, Font::Bold, Align::Center);
        char date[48];
        snprintf(date, sizeof(date), "%s, %d %s", weekday_name(dt.weekday), dt.day, month_name(dt.month));
        c.draw_text_in(Rect(0, 166, sw_, 26), date, rgba(255, 255, 255, 230), 20, Font::Medium, Align::Center);

        if (mode_ != Mode::Normal) paint_card(c);
        else if (!error_.empty())
            c.draw_text_in(Rect(sw_ / 2 - 200, sh_ / 2 + 88, 400, 20), error_.c_str(), hex(0xFF8A80), 14,
                           Font::Medium, Align::Center);
        else if (!info_.empty())
            c.draw_text_in(Rect(sw_ / 2 - 260, sh_ / 2 + 88, 520, 20), info_.c_str(), hex(0xB9F6CA), 14,
                           Font::Medium, Align::Center);

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
        if (shaking_) {
            uint64_t e = now - shake_t0_;
            if (e >= 400) {
                shaking_ = false;
                pill_->bounds.x = pill_x_;
            } else {
                float amp = 12.0f * (1.0f - (float)e / 400.0f);
                pill_->bounds.x = pill_x_ + (int)(amp * sinf((float)e * 0.05f));
            }
            invalidate();
            return;
        }
        if (now - last_ >= 500) {   // clock, and the caret blink
            last_ = now;
            invalidate();
        }
    }

private:
    enum class Mode { Normal, Confirm, Reset };

    const UserAccount& user(int i) const { return use_guest_ ? guest_ : sysconfig().users[i]; }

    Rect card_rect() const { return Rect(sw_ / 2 - 230, sh_ / 2 - 130, 460, 330); }

    void set_mode(Mode m) {
        mode_ = m;
        bool normal = m == Mode::Normal;
        for (Hot* h : user_hots_) h->visible = normal;
        go_->visible = normal;
        restart_->visible = shut_->visible = normal;
        forgot_->visible = normal && fails_ > 0 && !use_guest_ && sysconfig_present();
        cancel_->visible = confirm_->visible = !normal;
        pill_->visible = m != Mode::Confirm;
        verify_->visible = m == Mode::Reset;
        Rect card = card_rect();
        if (m == Mode::Reset) {
            pill_->bounds = Rect(sw_ / 2 - 150, card.y + 122, 300, 44);
            pill_->placeholder = "New password";
        } else {
            pill_->bounds = Rect(sw_ / 2 - 177, sh_ / 2 + 36, 300, 44);
            pill_->placeholder = "Password";
        }
        pill_x_ = pill_->bounds.x;
        pill_->set_text("");
        verify_->set_text("");
        error_ = "";
        if (m != Mode::Confirm) focus_widget(pill_);
        invalidate();
    }

    void paint_card(Canvas& c) {
        Rect card = card_rect();
        GlassStyle gs = glass_style_panel(true);
        gs.radius = 26;
        c.draw_shadow(card, 26, 30, rgba(0, 0, 0, 90), 0, 10);
        glass_render(c, card, gs, c.clip());
        c.stroke_round_rect(card, 26, rgba(255, 255, 255, 60), 1.0f);
        const UserAccount& u = user(selected_);
        Rect text(card.x + 32, card.y + 28, card.w - 64, card.h);
        if (mode_ == Mode::Confirm) {
            c.draw_text_in(Rect(text.x, text.y, text.w, 30), "Forgot your password?", rgba(255, 255, 255, 255), 22, Font::Bold, Align::Center);
            String body = String("KleinOS will restart into Recovery. When it starts again, you can choose a new password for ") +
                          u.full_name + ". Your files stay as they are.";
            c.draw_text_wrapped(Rect(text.x, text.y + 50, text.w, 140), body.c_str(), rgba(255, 255, 255, 220), 15, Font::Regular, Align::Center, 4);
            c.draw_text_wrapped(Rect(text.x, text.y + 168, text.w, 60), "Anyone who can restart this computer can do this, so keep it somewhere safe.",
                                rgba(255, 255, 255, 150), 13, Font::Regular, Align::Center, 2);
        } else {
            c.draw_text_in(Rect(text.x, text.y, text.w, 30), "Reset Password", rgba(255, 255, 255, 255), 22, Font::Bold, Align::Center);
            String sub = String("Choose a new password for ") + u.full_name + ".";
            c.draw_text_in(Rect(text.x, text.y + 44, text.w, 22), sub.c_str(), rgba(255, 255, 255, 215), 15, Font::Regular, Align::Center);
        }
        if (!error_.empty())
            c.draw_text_in(Rect(card.x + 20, card.bottom() - 98, card.w - 40, 20), error_.c_str(), hex(0xFF8A80), 14, Font::Medium, Align::Center);
    }

    void restart_into_recovery() {
        if (!sysconfig_request_recovery(user(selected_).login)) {
            error_ = "Recovery couldn't be prepared on this disk.";
            invalidate();
            return;
        }
        update::confirm_by_user();
        power_reboot();
    }

    void do_reset() {
        if (mode_ != Mode::Reset) return;
        String a = pill_->get_text(), b = verify_->get_text();
        if (a.empty()) { error_ = "Enter a new password."; focus_widget(pill_); invalidate(); return; }
        if (a != b) { error_ = "The passwords don't match."; verify_->set_text(""); focus_widget(verify_); invalidate(); return; }
        if (!sysconfig_recovery_reset(a.c_str())) { error_ = "The new password couldn't be saved."; invalidate(); return; }
        fails_ = 0;
        set_mode(Mode::Normal);
        info_ = "Password changed. Log in with your new password.";
        invalidate();
    }

    void draw_user(Canvas& c, Rect r, int i, bool sel) const {
        const UserAccount& u = user(i);
        float rad = 56.0f;
        float cx = r.x + r.w / 2.0f, cy = r.y + rad;
        Color base = kAvatarColors[u.avatar_color % kAvatarCount];
        c.fill_circle(cx, cy, rad, sel ? base : with_alpha(base, 150));
        c.fill_ellipse(cx, cy - rad * 0.42f, rad * 0.78f, rad * 0.46f, rgba(255, 255, 255, sel ? 60 : 30));
        if (sel) c.stroke_circle(cx, cy, rad + 6, rgba(255, 255, 255, 235), 3.0f);
        String ini = initials_of(u.full_name);
        c.draw_text_in(Rect(r.x, r.y, r.w, 112), ini.c_str(), rgba(255, 255, 255, sel ? 255 : 210), 40, Font::Bold,
                       Align::Center);
        c.draw_text_in(Rect(r.x, r.y + 126, r.w, 22), u.full_name, rgba(255, 255, 255, sel ? 255 : 170), 16,
                       sel ? Font::Bold : Font::Medium, Align::Center);
    }

    void select(int i) {
        if (mode_ != Mode::Normal) return;
        if (i != selected_) { fails_ = 0; forgot_->visible = false; }
        selected_ = i;
        pill_->set_text("");
        error_ = "";
        info_ = "";
        focus_widget(pill_);
        invalidate();
    }

    void attempt() {
        const UserAccount& u = user(selected_);
        if (!sysconfig_check_password(u, pill_->get_text())) {
            error_ = "Incorrect password";
            info_ = "";
            fails_++;
            forgot_->visible = !use_guest_ && sysconfig_present();
            pill_->set_text("");
            shaking_ = true;
            shake_t0_ = timer_ms();
            invalidate();
            return;
        }
        session_enter_desktop(u.full_name, u.login);
        close();
    }

    int sw_ = 0, sh_ = 0;
    Canvas* bg_ = nullptr;
    bool use_guest_ = false;
    int count_ = 1;
    int selected_ = 0;
    UserAccount guest_;
    PillField* pill_ = nullptr;
    int pill_x_ = 0;
    bool shaking_ = false;
    uint64_t shake_t0_ = 0, last_ = 0;
    String error_, info_;
    Mode mode_ = Mode::Normal;
    int fails_ = 0;
    Vector<Hot*> user_hots_;
    Hot *go_ = nullptr, *restart_ = nullptr, *shut_ = nullptr, *forgot_ = nullptr, *cancel_ = nullptr, *confirm_ = nullptr;
    PillField* verify_ = nullptr;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x5E5CE6), hex(0x3634A3));
    draw_symbol(c, r.inset(r.w / 4), Symbol::User, hex(0xFFFFFF), r.w / 20.0f);
}

Window* create() { return new LoginWindow(); }

AppRegistrar reg({"login", "Login", icon, create, "System", true, false, 100, "Sign in to your account"});

}  // namespace
