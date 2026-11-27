#include "ui/wifi_panel.h"
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "arch/interrupts.h"
#include "drivers/framebuffer.h"
#include "lib/math.h"
#include "lib/string.h"
#include "net/net.h"
#include "net/wifi.h"

namespace {

constexpr int kPanelW = 320;
constexpr int kMaxHeight = 390;
constexpr int kDockRoom = 96;          // keep the panel clear of the dock on short screens
constexpr int kHeaderH = 48, kRowH = 46, kMinPassword = 8;
constexpr int kMaxNetworks = 32;

class WifiPanel;
WifiPanel* g_panel;
uint64_t g_closed_at;

int panel_height() {
    int room = (int)framebuffer().height - kMenubarHeight - 6 - kTitlebarHeight - kDockRoom;
    return kclamp(room, 250, kMaxHeight);
}

class WifiPanel : public Window {
public:
    WifiPanel() : Window("Wi-Fi", kPanelW, panel_height(), WF_DIALOG | WF_HIDDEN_IN_DOCK) {
        int H = height();
        refresh_ = (Button*)add(new Button(Rect(kPanelW - 94, 9, 82, 30), "Refresh"));
        refresh_->on_click = [this] { rescan(); };
        password_ = (TextField*)add(new TextField(Rect(16, 78, kPanelW - 32, 38), "Password"));
        password_->password = true;
        password_->font_size = 16;
        password_->max_length = 63;
        password_->on_change = [this] { update_join(); };
        password_->on_submit = [this] { if (join_->enabled) join(); };
        show_ = (Checkbox*)add(new Checkbox(Rect(16, 126, 200, 26), "Show password"));
        show_->on_change = [this](bool on) { password_->password = !on; password_->invalidate(); };
        cancel_ = (Button*)add(new Button(Rect(16, H - 54, 138, 40), "Cancel"));
        cancel_->on_click = [this] { cancel(); };
        join_ = (Button*)add(new Button(Rect(166, H - 54, 138, 40), "Join", Button::Primary));
        join_->on_click = [this] { join(); };
        set_ticking(true);
        g_panel = this;
        net::wifi_set_panel_open(true);
        set_page(List);
        rescan();
    }
    ~WifiPanel() override {
        g_panel = nullptr;
        g_closed_at = timer_ms();
        net::wifi_set_panel_open(false);
    }

    void on_focus(bool focused) override {
        if (!focused && !closing_) close();
    }

    void on_key(const KeyEvent& e) override {
        if (e.key == Key::Escape) {
            if (page_ == List) close();
            else cancel();
            return;
        }
        Window::on_key(e);
    }

    void on_tick(uint64_t now) override {
        const net::WifiDriver* d = net::wifi_driver();
        if (!d) return;
        bool busy = d->scanning();
        if (was_scanning_ && !busy) load_results();
        if (busy != was_scanning_) { was_scanning_ = busy; invalidate(); }
        if (busy || page_ == Connecting) {
            if (now - last_spin_ > 40) { last_spin_ = now; invalidate(); }
        }
        if (page_ != Connecting) return;
        switch (d->status()) {
            case net::WifiStatus::Connected: {
                char msg[64];
                snprintf(msg, sizeof(msg), "Connected to %s", target_.ssid);
                wm::notify("Wi-Fi", msg);
                close();
                break;
            }
            case net::WifiStatus::WrongPassword:
                error_ = "That password didn't work. Try again.";
                if (used_saved_) { net::wifi_forget(target_.ssid); error_ = "The saved password no longer works. Enter it again."; }
                set_page(target_.secured ? Password : List);
                break;
            case net::WifiStatus::Failed:
                error_ = "Couldn't connect. Move closer and try again.";
                set_page(target_.secured ? Password : List);
                break;
            case net::WifiStatus::Unsupported:
                error_ = "This network needs WPA2. Its security isn't supported.";
                set_page(target_.secured ? Password : List);
                break;
            default: break;
        }
    }

    void on_mouse_down(int x, int y, int button) override {
        if (page_ == List && button == MB_LEFT && y >= kHeaderH) {
            int i = (y - kHeaderH + scroll_) / kRowH;
            if (i >= 0 && i < count_) choose(i);
            return;
        }
        Window::on_mouse_down(x, y, button);
    }

    void on_mouse_move(int x, int y, int buttons) override {
        int h = page_ == List && y >= kHeaderH ? (y - kHeaderH + scroll_) / kRowH : -1;
        if (h >= count_) h = -1;
        if (h != hover_) { hover_ = h; invalidate(); }
        Window::on_mouse_move(x, y, buttons);
    }

    void on_scroll(int x, int y, int delta) override {
        if (page_ != List) return;
        int s = kclamp(scroll_ + delta * kRowH, 0, kmax(0, count_ * kRowH - (height() - kHeaderH)));
        if (s != scroll_) { scroll_ = s; invalidate(); }
    }

    void on_paint(Canvas& c) override {
        c.clear(theme().window_bg);
        if (page_ == List) paint_list(c);
        else if (page_ == Password) paint_password(c);
        else paint_connecting(c);
        paint_widgets(c);
    }

private:
    enum Page { List, Password, Connecting };

    void set_page(Page p) {
        page_ = p;
        refresh_->visible = p == List && net::wifi_driver();
        password_->visible = show_->visible = p == Password;
        join_->visible = p == Password;
        cancel_->visible = p != List;
        if (p == Password) {
            password_->set_text("");
            focus_widget(password_);
        }
        update_join();
        invalidate();
    }

    void update_join() { join_->enabled = strlen(password_->get_text()) >= kMinPassword; join_->invalidate(); }

    void rescan() {
        const net::WifiDriver* d = net::wifi_driver();
        if (!d) return;
        d->scan();
        was_scanning_ = true;
        count_ = 0;
        scroll_ = 0;
        invalidate();
    }
    void load_results() {
        const net::WifiDriver* d = net::wifi_driver();
        count_ = d->results(nets_, kMaxNetworks);
        invalidate();
    }

    void choose(int i) {
        target_ = nets_[i];
        error_ = "";
        used_saved_ = net::wifi_connect_saved(target_.ssid);
        if (used_saved_) { set_page(Connecting); return; }
        if (target_.secured) { set_page(Password); return; }
        net::wifi_connect(target_.ssid, "");
        set_page(Connecting);
    }
    void join() {
        used_saved_ = false;
        net::wifi_connect(target_.ssid, password_->get_text());
        password_->set_text("");
        error_ = "";
        set_page(Connecting);
    }
    void cancel() {
        if (page_ == Connecting) net::wifi_driver()->disconnect();
        password_->set_text("");
        error_ = "";
        set_page(List);
    }

    void draw_bars(Canvas& c, int x, int cy, int level, Color on, Color off) {
        for (int b = 0; b < 4; b++) {
            int h = 4 + b * 4;
            c.fill_round_rect(Rect(x + b * 6, cy + 8 - h, 4, h), 1, b < level ? on : off);
        }
    }

    void spinner(Canvas& c, int cx, int cy, int r) {
        const Theme& t = theme();
        c.fill_arc(cx, cy, r, r - 5, 0, 6.2831853f, rgba(128, 128, 128, 60));
        float ph = (float)((timer_ms() % 900) / 900.0 * 6.2831853);
        c.fill_arc(cx, cy, r, r - 5, ph, ph + 1.6f, t.accent);
    }

    void paint_list(Canvas& c) {
        const Theme& t = theme();
        const net::WifiDriver* d = net::wifi_driver();
        c.draw_text_in(Rect(16, 0, 180, kHeaderH), "Choose a network", t.text, 15, Font::Bold);
        c.fill_rect(Rect(0, kHeaderH - 1, kPanelW, 1), t.separator);
        if (!d) {
            draw_symbol(c, Rect(kPanelW / 2 - 20, 92, 40, 40), Symbol::WifiOff, t.text_secondary, 2.0f);
            c.draw_text_in(Rect(16, 144, kPanelW - 32, 24), "Wi-Fi isn't working yet", t.text, 16, Font::Bold, Align::Center);
            c.draw_text_wrapped(Rect(24, 174, kPanelW - 48, 60), "This computer doesn't have a working Wi-Fi driver. A network cable will work once its driver is ready.",
                                t.text_secondary, 13, Font::Regular, Align::Center);
            return;
        }
        if (d->scanning() && !count_) {
            spinner(c, kPanelW / 2, 130, 20);
            c.draw_text_in(Rect(0, 162, kPanelW, 24), "Looking for networks...", t.text_secondary, 14, Font::Regular, Align::Center);
            return;
        }
        if (!count_) {
            c.draw_text_in(Rect(0, 130, kPanelW, 24), "No networks found", t.text_secondary, 14, Font::Regular, Align::Center);
            return;
        }
        const char* active = d->connected_ssid();
        c.save();
        c.clip_to(Rect(0, kHeaderH, kPanelW, height() - kHeaderH));
        for (int i = 0; i < count_; i++) {
            int y = kHeaderH + i * kRowH - scroll_;
            if (y + kRowH <= kHeaderH || y >= height()) continue;
            Rect row(6, y + 2, kPanelW - 12, kRowH - 4);
            if (i == hover_) c.fill_round_rect(row, 8, t.hover);
            draw_bars(c, 18, y + kRowH / 2, nets_[i].signal, t.text, rgba(128, 128, 128, 70));
            c.draw_text_in(Rect(52, y, kPanelW - 110, kRowH), nets_[i].ssid, t.text, 15);
            if (!strcmp(active, nets_[i].ssid)) draw_symbol(c, Rect(kPanelW - 38, y + 15, 16, 16), Symbol::Check, t.accent, 2.2f);
            else if (nets_[i].secured) draw_symbol(c, Rect(kPanelW - 38, y + 15, 16, 16), Symbol::Lock, t.text_secondary, 1.6f);
        }
        c.restore();
    }

    void paint_password(Canvas& c) {
        const Theme& t = theme();
        c.draw_text_in(Rect(16, 12, kPanelW - 32, 28), target_.ssid, t.text, 18, Font::Bold);
        c.draw_text_in(Rect(16, 44, kPanelW - 32, 22), "Type the Wi-Fi password", t.text_secondary, 14);
        const char* hint = error_.size() ? error_.c_str() : "At least 8 characters";
        c.draw_text_in(Rect(16, 158, kPanelW - 32, 22), hint, error_.size() ? t.destructive : t.text_tertiary, 13);
    }

    void paint_connecting(Canvas& c) {
        const Theme& t = theme();
        spinner(c, kPanelW / 2, 96, 24);
        char msg[64];
        snprintf(msg, sizeof(msg), "Connecting to %s", target_.ssid);
        c.draw_text_in(Rect(16, 136, kPanelW - 32, 24), msg, t.text, 15, Font::Medium, Align::Center);
    }

    Page page_ = List;
    net::WifiNetwork nets_[kMaxNetworks];
    net::WifiNetwork target_{};
    int count_ = 0, hover_ = -1, scroll_ = 0;
    bool was_scanning_ = false;
    uint64_t last_spin_ = 0;
    bool used_saved_ = false;
    String error_;
    Button* refresh_ = nullptr;
    TextField* password_ = nullptr;
    Checkbox* show_ = nullptr;
    Button* cancel_ = nullptr;
    Button* join_ = nullptr;
};

}  // namespace

void wifi_panel_toggle(int right_x) {
    if (g_panel) { g_panel->close(); return; }
    // the click that took focus away already closed it: that click meant "close"
    if (timer_ms() - g_closed_at < 200) return;
    WifiPanel* p = new WifiPanel();
    int screen = (int)framebuffer().width;
    p->move_to(kclamp(right_x - kPanelW, 6, screen - kPanelW - 6), kMenubarHeight + 6);
}
