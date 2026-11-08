// Clock: world clocks, stopwatch with laps, and a countdown timer.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "drivers/rtc.h"
#include "drivers/power.h"
#include "arch/interrupts.h"
#include "lib/math.h"
#include "lib/kstring.h"

namespace {

constexpr int kW = 460, kH = 460;
constexpr uint64_t kMinuteMs = 60000;

// Fixed offsets from UTC (no daylight saving).
struct City { const char* name; int utc_offset_hours; };
const City kCities[4] = {{"London", 0}, {"Tokyo", 9}, {"New York", -5}, {"Sydney", 11}};

struct Hms { int h, m, s; };

Hms hms_of(int64_t t) {
    int64_t d = ((t % 86400) + 86400) % 86400;
    return Hms{(int)(d / 3600), (int)(d / 60 % 60), (int)(d % 60)};
}

void format_hms(const Hms& t, char* out, size_t n) {
    snprintf(out, n, "%02d:%02d:%02d", t.h, t.m, t.s);
}

// Stopwatch: MM:SS.cc
void format_stopwatch(uint64_t ms, char* out, size_t n) {
    unsigned long long mins = ms / kMinuteMs;
    unsigned long long secs = ms / 1000 % 60;
    unsigned long long cs = ms / 10 % 100;
    snprintf(out, n, "%02llu:%02llu.%02llu", mins, secs, cs);
}

// Timer: rounds up, so the display only reads 00:00 once time is really up.
void format_countdown(uint64_t ms, char* out, size_t n) {
    unsigned long long secs = (ms + 999) / 1000;
    snprintf(out, n, "%02llu:%02llu", secs / 60, secs % 60);
}

// Analog face. sec includes the fractional part, so the second hand glides.
void draw_face(Canvas& c, float cx, float cy, float R, int h, int m, float sec, bool detail) {
    const Theme& t = theme();
    Color red = hex(0xFF453A);
    c.draw_shadow(Rect((int)(cx - R), (int)(cy - R), (int)(2 * R), (int)(2 * R)), R, R * 0.12f,
                  rgba(0, 0, 0, t.dark ? 120 : 45), 0, (int)(R * 0.06f) + 1);
    c.fill_circle(cx, cy, R, t.dark ? hex(0x2C2C31) : hex(0xFFFFFF));
    c.stroke_circle(cx, cy, R, t.separator, 1.0f);

    for (int i = 0; i < 60; i++) {
        bool major = i % 5 == 0;
        if (!major && !detail) continue;
        float a = (i * 6.0f - 90.0f) * (float)M_PI / 180.0f;
        float r0 = R * (major ? 0.80f : 0.90f), r1 = R * 0.94f;
        float w = major ? kmax(1.2f, R * 0.04f) : 1.0f;
        c.draw_line(cx + cosf(a) * r0, cy + sinf(a) * r0, cx + cosf(a) * r1, cy + sinf(a) * r1,
                    major ? t.text : t.text_tertiary, w);
    }

    auto hand = [&](float deg, float len, float width, Color col) {
        float a = (deg - 90.0f) * (float)M_PI / 180.0f;
        c.draw_line(cx, cy, cx + cosf(a) * len, cy + sinf(a) * len, col, width);
    };
    float mdeg = (m + sec / 60.0f) * 6.0f;
    float hdeg = ((h % 12) + (m + sec / 60.0f) / 60.0f) * 30.0f;
    hand(hdeg, R * 0.50f, kmax(2.0f, R * 0.075f), t.text);
    hand(mdeg, R * 0.76f, kmax(1.5f, R * 0.05f), t.text);
    float sdeg = sec * 6.0f;
    hand(sdeg + 180.0f, R * 0.20f, kmax(1.0f, R * 0.02f), red);
    hand(sdeg, R * 0.86f, kmax(1.0f, R * 0.02f), red);
    c.fill_circle(cx, cy, kmax(2.0f, R * 0.05f), red);
}

enum class Tab : uint8_t { World, Stopwatch, Timer };

class ClockWindow : public Window {
public:
    ClockWindow() : Window("Clock", kW, kH, WF_NO_ZOOM) {
        seg_ = (SegmentedControl*)add(new SegmentedControl(Rect(20, 14, 420, 30), {"World Clock", "Stopwatch", "Timer"}));
        seg_->on_change = [this](int i) { set_tab((Tab)i); };

        // stopwatch
        sw_left_ = button(Rect(110, 150, 120, 34), "Reset", Button::Normal, [this] { sw_left(); }, Tab::Stopwatch);
        sw_right_ = button(Rect(230, 150, 120, 34), "Start", Button::Primary, [this] { sw_toggle(); }, Tab::Stopwatch);
        laps_ = (ListView*)add(new ListView(Rect(20, 200, 420, 240)));
        laps_->row_height = 28;
        laps_->visible = false;
        sw_widgets_.push(laps_);

        // timer
        tm_minus_ = button(Rect(35, 318, 40, 34), "−", Button::Normal, [this] { tm_adjust(-1); }, Tab::Timer);
        tm_minus_->font_size = 18;
        button(Rect(85, 318, 90, 34), "1 min", Button::Normal, [this] { tm_preset(1); }, Tab::Timer);
        button(Rect(185, 318, 90, 34), "5 min", Button::Normal, [this] { tm_preset(5); }, Tab::Timer);
        button(Rect(285, 318, 90, 34), "10 min", Button::Normal, [this] { tm_preset(10); }, Tab::Timer);
        tm_plus_ = button(Rect(385, 318, 40, 34), "+", Button::Normal, [this] { tm_adjust(1); }, Tab::Timer);
        tm_plus_->font_size = 18;
        tm_reset_ = button(Rect(102, 372, 120, 34), "Reset", Button::Normal, [this] { tm_reset(); }, Tab::Timer);
        tm_start_ = button(Rect(238, 372, 120, 34), "Start", Button::Primary, [this] { tm_toggle(); }, Tab::Timer);

        tm_total_ = tm_remaining_ = 5 * kMinuteMs;
        refresh_buttons();
        update_tabs();
        set_ticking(true);
    }

    void on_paint(Canvas& c) override {
        Window::on_paint(c);
        switch (tab_) {
        case Tab::World: draw_world(c); break;
        case Tab::Stopwatch: draw_stopwatch(c); break;
        case Tab::Timer: draw_timer(c); break;
        }
    }

    void on_tick(uint64_t) override {
        uint64_t now = timer_ms();
        uint64_t sec = rtc_unix_time();
        if (sec != last_sec_) { last_sec_ = sec; sec_started_ = now; }
        uint64_t since = now - sec_started_;
        frac_ = since >= 1000 ? 1.0f : since / 1000.0f;

        if (tm_running_ && now >= tm_end_) finish_timer(now);
        if (beeps_left_ > 0 && now >= next_beep_) {
            speaker_beep(880, 160);
            beeps_left_--;
            next_beep_ = now + 300;
        }

        bool animating = tab_ == Tab::World || (tab_ == Tab::Stopwatch && sw_running_) ||
                         (tab_ == Tab::Timer && (tm_running_ || beeps_left_ > 0));
        if (animating) invalidate();
    }

private:
    Button* button(Rect r, const char* text, Button::Style style, Func<void()> fn, Tab tab) {
        Button* b = (Button*)add(new Button(r, text, style));
        b->on_click = fn;
        (tab == Tab::Stopwatch ? sw_widgets_ : tm_widgets_).push(b);
        return b;
    }

    void set_tab(Tab t) {
        tab_ = t;
        update_tabs();
        invalidate();
    }

    void update_tabs() {
        for (Widget* w : sw_widgets_) w->visible = tab_ == Tab::Stopwatch;
        for (Widget* w : tm_widgets_) w->visible = tab_ == Tab::Timer;
        invalidate();
    }

    // ---- world clock
    void draw_world(Canvas& c) {
        const Theme& t = theme();
        uint64_t utc = rtc_unix_time();
        int64_t local = (int64_t)utc + (int64_t)rtc_utc_offset_minutes() * 60;
        Hms lh = hms_of(local);
        draw_face(c, kW / 2.0f, 150, 88, lh.h, lh.m, lh.s + frac_, true);
        c.draw_text_in(Rect(0, 248, kW, 20), "Local", t.text_secondary, 13, Font::Medium, Align::Center);
        char buf[16];
        format_hms(lh, buf, sizeof buf);
        c.draw_text_in(Rect(0, 268, kW, 22), buf, t.text, 16, Font::Mono, Align::Center);

        for (int i = 0; i < 4; i++) {
            float cx = 58.0f + i * 115.0f;
            Hms h = hms_of((int64_t)utc + kCities[i].utc_offset_hours * 3600);
            draw_face(c, cx, 366, 42, h.h, h.m, h.s + frac_, false);
            c.draw_text_in(Rect((int)cx - 56, 416, 112, 18), kCities[i].name, t.text, 13, Font::Medium, Align::Center);
            format_hms(h, buf, sizeof buf);
            c.draw_text_in(Rect((int)cx - 56, 434, 112, 18), buf, t.text_secondary, 12, Font::Mono, Align::Center);
        }
    }

    // ---- stopwatch
    uint64_t sw_elapsed(uint64_t now) const { return sw_accum_ + (sw_running_ ? now - sw_start_ : 0); }

    void draw_stopwatch(Canvas& c) {
        char buf[24];
        format_stopwatch(sw_elapsed(timer_ms()), buf, sizeof buf);
        c.draw_text_in(Rect(0, 74, kW, 60), buf, theme().text, 56, Font::Mono, Align::Center);
    }

    void sw_toggle() {
        uint64_t now = timer_ms();
        if (sw_running_) {
            sw_accum_ += now - sw_start_;
            sw_running_ = false;
        } else {
            sw_start_ = now;
            sw_running_ = true;
        }
        refresh_buttons();
        invalidate();
    }

    void sw_left() {
        if (sw_running_) { sw_lap(); return; }
        sw_running_ = false;
        sw_accum_ = 0;
        laps_totals_.clear();
        rebuild_laps();
        refresh_buttons();
        invalidate();
    }

    void sw_lap() {
        laps_totals_.push(sw_elapsed(timer_ms()));
        rebuild_laps();
        invalidate();
    }

    // Newest lap first; each split is the time since the previous lap.
    void rebuild_laps() {
        laps_->clear();
        uint64_t prev = 0;
        Vector<String> labels, splits;
        for (size_t i = 0; i < laps_totals_.size(); i++) {
            char split[24], label[24];
            format_stopwatch(laps_totals_[i] - prev, split, sizeof split);
            snprintf(label, sizeof label, "Lap %d", (int)i + 1);
            labels.push(String(label));
            splits.push(String(split));
            prev = laps_totals_[i];
        }
        for (int i = (int)labels.size() - 1; i >= 0; i--) laps_->add(labels[i].c_str(), splits[i].c_str());
    }

    // ---- timer
    uint64_t tm_now_remaining(uint64_t now) const {
        if (!tm_running_) return tm_remaining_;
        return tm_end_ > now ? tm_end_ - now : 0;
    }

    void draw_timer(Canvas& c) {
        const Theme& t = theme();
        uint64_t now = timer_ms();
        uint64_t rem = tm_now_remaining(now);
        float cx = kW / 2.0f, cy = 180, R = 110;
        Color track = t.dark ? rgba(255, 255, 255, 28) : rgba(0, 0, 0, 22);
        c.stroke_circle(cx, cy, R - 7, track, 14);
        float frac = tm_total_ ? (float)rem / (float)tm_total_ : 0;
        if (frac > 0) {
            float a0 = -(float)M_PI / 2;
            float a1 = a0 + 2 * (float)M_PI * frac;
            c.fill_arc(cx, cy, R, R - 14, a0, a1, t.accent);
        }
        char buf[16];
        format_countdown(rem, buf, sizeof buf);
        bool done = rem == 0 && !tm_running_ && tm_total_ > 0 && tm_remaining_ == 0;
        c.draw_text_in(Rect(0, (int)cy - 30, kW, 60), buf, done ? t.destructive : t.text, 50, Font::Mono, Align::Center);
    }

    void tm_preset(int minutes) {
        silence();
        tm_running_ = false;
        tm_total_ = tm_remaining_ = (uint64_t)minutes * kMinuteMs;
        refresh_buttons();
        invalidate();
    }

    void tm_adjust(int dir) {
        int64_t total = (int64_t)tm_total_ + dir * (int64_t)kMinuteMs;
        if (total < (int64_t)kMinuteMs) total = kMinuteMs;
        if (total > 99 * (int64_t)kMinuteMs) total = 99 * kMinuteMs;
        int64_t delta = total - (int64_t)tm_total_;
        tm_total_ = (uint64_t)total;
        if (tm_running_) tm_end_ = (uint64_t)((int64_t)tm_end_ + delta);
        else tm_remaining_ = tm_total_;
        invalidate();
    }

    void tm_toggle() {
        uint64_t now = timer_ms();
        if (tm_running_) {
            tm_remaining_ = tm_now_remaining(now);
            tm_running_ = false;
        } else {
            if (tm_remaining_ == 0) tm_remaining_ = tm_total_;
            tm_end_ = now + tm_remaining_;
            tm_running_ = true;
            silence();
        }
        refresh_buttons();
        invalidate();
    }

    void tm_reset() {
        silence();
        tm_running_ = false;
        tm_remaining_ = tm_total_;
        refresh_buttons();
        invalidate();
    }

    void finish_timer(uint64_t now) {
        tm_running_ = false;
        tm_remaining_ = 0;
        beeps_left_ = 6;
        next_beep_ = now;
        wm::notify("Timer", "Time is up", "clock");
        refresh_buttons();
    }

    void silence() { beeps_left_ = 0; }

    void refresh_buttons() {
        sw_left_->text = sw_running_ ? "Lap" : "Reset";
        sw_right_->text = sw_running_ ? "Stop" : "Start";
        sw_right_->style = sw_running_ ? Button::Destructive : Button::Primary;
        tm_start_->text = tm_running_ ? "Pause" : "Start";
        tm_start_->style = tm_running_ ? Button::Destructive : Button::Primary;
    }

    SegmentedControl* seg_ = nullptr;
    Tab tab_ = Tab::World;
    Vector<Widget*> sw_widgets_, tm_widgets_;

    uint64_t last_sec_ = 0, sec_started_ = 0;
    float frac_ = 0;

    Button* sw_left_ = nullptr;
    Button* sw_right_ = nullptr;
    ListView* laps_ = nullptr;
    bool sw_running_ = false;
    uint64_t sw_start_ = 0, sw_accum_ = 0;
    Vector<uint64_t> laps_totals_;

    Button* tm_minus_ = nullptr;
    Button* tm_plus_ = nullptr;
    Button* tm_reset_ = nullptr;
    Button* tm_start_ = nullptr;
    bool tm_running_ = false;
    uint64_t tm_total_ = 0, tm_remaining_ = 0, tm_end_ = 0;
    int beeps_left_ = 0;
    uint64_t next_beep_ = 0;
};

// ---- icon: navy squircle with a white dial
void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x2F4A7A), hex(0x0D1830));
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, R = r.w * 0.34f;
    c.draw_shadow(Rect((int)(cx - R), (int)(cy - R), (int)(2 * R), (int)(2 * R)), R, R * 0.1f, rgba(0, 0, 0, 90), 0, 2);
    c.fill_circle(cx, cy, R, hex(0xF4F6FA));
    for (int i = 0; i < 12; i++) {
        float a = (i * 30.0f - 90.0f) * (float)M_PI / 180.0f;
        float r0 = R * 0.74f, r1 = R * 0.9f;
        c.draw_line(cx + cosf(a) * r0, cy + sinf(a) * r0, cx + cosf(a) * r1, cy + sinf(a) * r1,
                    hex(0x1B2740), i % 3 == 0 ? R * 0.1f : R * 0.05f);
    }
    // hands at 10:10
    auto hand = [&](float deg, float len, float w, Color col) {
        float a = (deg - 90.0f) * (float)M_PI / 180.0f;
        c.draw_line(cx, cy, cx + cosf(a) * len, cy + sinf(a) * len, col, w);
    };
    hand(300.0f, R * 0.5f, R * 0.12f, hex(0x1B2740));
    hand(60.0f, R * 0.72f, R * 0.08f, hex(0x1B2740));
    hand(150.0f, R * 0.8f, R * 0.035f, hex(0xFF6B4A));
    c.fill_circle(cx, cy, R * 0.08f, hex(0xFF6B4A));
}

Window* create() { return new ClockWindow(); }

AppRegistrar reg({"clock", "Clock", icon, create, "Utilities", true, false, 60, "World clocks, stopwatch and timer"});

}  // namespace
