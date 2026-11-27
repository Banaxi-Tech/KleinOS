// KleinOS Recovery: GRUB starts this instead of the login window after 3 failed
// starts in a row (see tools/grub-hdd.cfg), or when Restart was chosen with Shift held. Start normally again, go back to the
// version before the last update, reset a password, or turn the computer off.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "drivers/power.h"
#include "sys/sysconfig.h"
#include "sys/system.h"
#include "sys/update.h"
#include "lib/string.h"
#include "lib/math.h"

namespace {

struct Option {
    Symbol sym;
    String title, detail;
    bool enabled = true;
    Func<void()> run;
};

class RecoveryWindow : public Window {
public:
    RecoveryWindow() : Window("", 640, 480, WF_FULLSCREEN | WF_NO_ZOOM | WF_NO_MINIMIZE) {
        wm::set_shell_visible(false);
        prev_ok_ = update::previous_version_available();
        asked_ = update::recovery_requested();
        show_main();
        set_ticking(false);
    }

    void on_paint(Canvas& c) override {
        int W = width(), H = height();
        c.fill_gradient(Rect(0, 0, W, H), hex(0x14213D), hex(0x0B1020));
        int x = W / 2 - kPanelW / 2, y = top();
        c.draw_text(x, y - 104, "KleinOS Recovery", rgba(255, 255, 255, 255), 30, Font::Bold);
        c.draw_text(x, y - 58, subtitle_.c_str(), rgba(255, 255, 255, 190), 15);
        for (int i = 0; i < (int)opts_.size(); i++) {
            Rect r = row_rect(i);
            const Option& o = opts_[i];
            bool hi = i == sel_ && o.enabled;
            c.fill_round_rect(r, 14, hi ? rgba(255, 255, 255, 40) : rgba(255, 255, 255, 16));
            if (hi) c.stroke_round_rect(r, 14, rgba(255, 255, 255, 90), 1.0f);
            int a = o.enabled ? 255 : 90;
            Rect ic(r.x + 16, r.y + (r.h - 40) / 2, 40, 40);
            c.fill_round_rect(ic, 10, o.enabled ? with_alpha(theme().accent, 230) : rgba(255, 255, 255, 30));
            draw_symbol(c, ic.inset(9), o.sym, rgba(255, 255, 255, a), 2.0f);
            c.draw_text(r.x + 72, r.y + 14, o.title.c_str(), rgba(255, 255, 255, a), 17, Font::Medium);
            c.draw_text(r.x + 72, r.y + 40, o.detail.c_str(), rgba(255, 255, 255, o.enabled ? 160 : 70), 13);
        }
        if (!message_.empty())
            c.draw_text_in(Rect(x, row_rect((int)opts_.size()).y + 4, kPanelW, 20), message_.c_str(), hex(0xFFB4A9), 14, Font::Medium);
        char foot[96];
        snprintf(foot, sizeof(foot), "%s  \xC2\xB7  Use the arrow keys and Enter, or click", system_version_string());
        c.draw_text_in(Rect(0, H - 40, W, 20), foot, rgba(255, 255, 255, 90), 12, Font::Regular, Align::Center);
    }

    void on_mouse_move(int x, int y, int) override {
        int h = hit(x, y);
        if (h >= 0 && h != sel_) { sel_ = h; invalidate(); }
    }
    void on_mouse_down(int x, int y, int button) override {
        int h = hit(x, y);
        if (h >= 0) { sel_ = h; activate(); }
    }
    void on_key(const KeyEvent& e) override {
        if (!e.pressed) return;
        int n = (int)opts_.size();
        if (e.key == Key::Down || e.key == Key::Up) {
            for (int k = 0; k < n; k++) {
                sel_ = (sel_ + (e.key == Key::Down ? 1 : n - 1)) % n;
                if (opts_[sel_].enabled) break;
            }
            invalidate();
        } else if (e.key == Key::Enter) {
            activate();
        } else if (e.key == Key::Escape && accounts_) {
            show_main();
        }
    }

private:
    static constexpr int kPanelW = 600, kRowH = 70, kGap = 10;

    int top() const { return height() / 2 - ((int)opts_.size() * (kRowH + kGap)) / 2 + 40; }
    Rect row_rect(int i) const { return Rect(width() / 2 - kPanelW / 2, top() + i * (kRowH + kGap), kPanelW, kRowH); }
    int hit(int x, int y) const {
        for (int i = 0; i < (int)opts_.size(); i++) if (row_rect(i).contains(x, y) && opts_[i].enabled) return i;
        return -1;
    }

    void activate() {
        if (sel_ < 0 || sel_ >= (int)opts_.size() || !opts_[sel_].enabled) return;
        Func<void()> run = opts_[sel_].run;
        if (run) run();
    }

    void add(Symbol s, const char* title, const char* detail, bool enabled, Func<void()> run) {
        Option& o = opts_.emplace();
        o.sym = s; o.title = title; o.detail = detail; o.enabled = enabled; o.run = run;
    }

    void fail(const char* msg) { message_ = msg; invalidate(); }

    void show_main() {
        accounts_ = false;
        opts_.clear();
        message_ = "";
        subtitle_ = asked_ ? "You restarted into Recovery. Choose what to do."
                           : "KleinOS didn't start correctly 3 times in a row. Choose what to do.";
        add(Symbol::Restart, "Start KleinOS", asked_ ? "Start normally." : "Try a normal start again.", true, [this] {
            if (!update::recovery_finish(false)) { fail("The start-up settings on the disk couldn't be changed."); return; }
            power_reboot();
        });
        add(Symbol::Clock, "Go back to the previous version",
            prev_ok_ ? "Start the version that was installed before the last update." : "There is no earlier version on this computer.",
            prev_ok_, [this] {
                if (!update::recovery_finish(true)) { fail("The previous version couldn't be selected."); return; }
                power_reboot();
            });
        bool have_users = sysconfig_present() && sysconfig().user_count > 0;
        add(Symbol::Lock, "Reset a password", have_users ? "Choose a new password for an account when KleinOS starts." : "There are no accounts on this computer.",
            have_users, [this] { show_accounts(); });
        add(Symbol::Power, "Shut down", "Turn the computer off. Recovery opens again next time.", true, [] { power_shutdown(); });
        sel_ = 0;
        invalidate();
    }

    void show_accounts() {
        accounts_ = true;
        opts_.clear();
        message_ = "";
        subtitle_ = "Whose password should be reset? KleinOS restarts and asks for a new one.";
        int n = (int)kmin(sysconfig().user_count, 4u);
        for (int i = 0; i < n; i++) {
            const UserAccount& u = sysconfig().users[i];
            String login = u.login;
            String detail = String("Account \"") + u.login + "\"";
            add(Symbol::User, u.full_name, detail.c_str(), true, [this, login] {
                if (!sysconfig_request_recovery(login.c_str()) || !update::recovery_finish(false)) {
                    fail("The reset couldn't be prepared on the disk.");
                    return;
                }
                power_reboot();
            });
        }
        add(Symbol::ChevronLeft, "Back", "Return to the recovery options.", true, [this] { show_main(); });
        sel_ = 0;
        invalidate();
    }

    Vector<Option> opts_;
    int sel_ = 0;
    bool prev_ok_ = false;
    bool accounts_ = false;
    bool asked_ = false;
    String subtitle_, message_;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x3A4A6B), hex(0x14213D));
    draw_symbol(c, r.inset(r.w / 4), Symbol::Restart, hex(0xFFFFFF), r.w / 20.0f);
}

Window* create() { return new RecoveryWindow(); }

AppRegistrar reg({"recovery", "Recovery", icon, create, "System", true, false, 100, "Repair a computer that doesn't start"});

}  // namespace
