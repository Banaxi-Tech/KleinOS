// Calendar: month grid, the selected day's events, and events saved in the home folder.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "drivers/rtc.h"
#include "fs/vfs.h"
#include "sys/session.h"
#include "lib/kstring.h"

namespace {

constexpr int kW = 640, kH = 460;
constexpr int kGridX = 20, kGridY = 84, kCellW = 54, kCellH = 56;
constexpr int kPanelX = 420;
const char* const kWeekdays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

struct Date {
    int y, m, d;
};

bool same(const Date& a, const Date& b) { return a.y == b.y && a.m == b.m && a.d == b.d; }

struct Event {
    Date date;
    String text;
};

// Date shown in grid cell i (0..41) of the given month; leading and trailing cells come from the neighbours.
Date cell_date(int y, int m, int i) {
    int first = day_of_week(y, m, 1);
    int days = days_in_month(y, m);
    int d = i - first + 1;
    if (d < 1) {
        int pm = m == 1 ? 12 : m - 1;
        int py = m == 1 ? y - 1 : y;
        return Date{py, pm, days_in_month(py, pm) + d};
    }
    if (d > days) {
        int nm = m == 12 ? 1 : m + 1;
        int ny = m == 12 ? y + 1 : y;
        return Date{ny, nm, d - days};
    }
    return Date{y, m, d};
}

int parse_num(const char* s, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) v = v * 10 + (s[i] - '0');
    return v;
}

// Storage: one event per line, "YYYY-MM-DD|text".
void events_path(char* out, size_t n) {
    snprintf(out, n, "/Users/%s/Library/Calendar/events.txt", session_user());
}

class CalendarWindow : public Window {
public:
    CalendarWindow() : Window("Calendar", kW, kH, WF_NO_ZOOM) {
        DateTime now = rtc_now();
        today_ = Date{now.year, now.month, now.day};
        view_y_ = today_.y;
        view_m_ = today_.m;
        sel_ = today_;

        Button* prev = (Button*)add(new Button(Rect(252, 18, 32, 28), "", Button::Normal));
        prev->icon = [](Canvas& c, Rect r) { draw_symbol(c, r, Symbol::ChevronLeft, theme().text, 1.6f); };
        prev->on_click = [this] { shift_month(-1); };
        Button* next = (Button*)add(new Button(Rect(368, 18, 32, 28), "", Button::Normal));
        next->icon = [](Canvas& c, Rect r) { draw_symbol(c, r, Symbol::ChevronRight, theme().text, 1.6f); };
        next->on_click = [this] { shift_month(1); };
        Button* today = (Button*)add(new Button(Rect(290, 18, 70, 28), "Today", Button::Normal));
        today->on_click = [this] { go_today(); };

        list_ = (ListView*)add(new ListView(Rect(430, 100, 200, 296)));
        list_->transparent = true;
        list_->row_height = 30;

        field_ = (TextField*)add(new TextField(Rect(430, 412, 200, 30), "Add event"));
        field_->on_submit = [this] { add_event(); };

        load_events();
        refresh_list();
    }

    void on_paint(Canvas& c) override {
        const Theme& t = theme();
        c.clear(t.window_bg);
        c.fill_rect(Rect(kPanelX, 0, width() - kPanelX, height()), t.dark ? rgba(255, 255, 255, 12) : rgba(0, 0, 0, 6));
        c.fill_rect(Rect(kPanelX, 0, 1, height()), t.separator);

        // month title
        char title[48];
        snprintf(title, sizeof title, "%s %d", month_name(view_m_), view_y_);
        c.draw_text_in(Rect(20, 18, 220, 28), title, t.text, 20, Font::Bold, Align::Left);

        for (int i = 0; i < 7; i++)
            c.draw_text_in(Rect(kGridX + i * kCellW, 62, kCellW, 18), kWeekdays[i], t.text_secondary, 12, Font::Medium,
                           Align::Center);

        for (int i = 0; i < 42; i++) {
            int row = i / 7, col = i % 7;
            float cx = kGridX + col * kCellW + kCellW / 2.0f;
            float cy = kGridY + row * kCellH + kCellH / 2.0f;
            Date d = cell_date(view_y_, view_m_, i);
            bool inside = d.m == view_m_ && d.y == view_y_;
            bool is_today = same(d, today_);
            bool is_sel = same(d, sel_);
            Color text = inside ? t.text : t.text_tertiary;
            if (is_today) {
                c.fill_circle(cx, cy, 19, t.accent);
                text = hex(0xFFFFFF);
            } else if (is_sel) {
                c.stroke_circle(cx, cy, 18, t.accent, 2.0f);
            }
            char num[4];
            snprintf(num, sizeof num, "%d", d.d);
            c.draw_text_in(Rect((int)cx - 19, (int)cy - 10, 38, 20), num, text, 15,
                           is_today ? Font::Medium : Font::Regular, Align::Center);
            if (has_event(d))
                c.fill_circle(cx, cy + 23, 2.5f, is_today ? hex(0xFFFFFF) : t.accent);
        }

        // selected day
        int wd = day_of_week(sel_.y, sel_.m, sel_.d);
        c.draw_text_in(Rect(440, 22, 190, 18), weekday_name(wd), t.text_secondary, 13, Font::Medium, Align::Left);
        char date[48];
        snprintf(date, sizeof date, "%d %s", sel_.d, month_name(sel_.m));
        c.draw_text_in(Rect(440, 40, 190, 30), date, t.text, 22, Font::Bold, Align::Left);
        if (!list_->items.size())
            c.draw_text_in(Rect(440, 104, 190, 20), "No events", t.text_tertiary, 13, Font::Regular, Align::Left);

        for (Widget* w : widgets())
            if (w->visible) {
                c.save();
                w->paint(c);
                c.restore();
            }
    }

    void on_mouse_down(int x, int y, int button) override {
        int col = (x - kGridX) / kCellW, row = (y - kGridY) / kCellH;
        if (x >= kGridX && y >= kGridY && col >= 0 && col < 7 && row >= 0 && row < 6 && x < kGridX + 7 * kCellW) {
            Date d = cell_date(view_y_, view_m_, row * 7 + col);
            sel_ = d;
            view_y_ = d.y;
            view_m_ = d.m;
            focus_widget(nullptr);
            refresh_list();
            return;
        }
        Window::on_mouse_down(x, y, button);
    }

private:
    void shift_month(int delta) {
        view_m_ += delta;
        while (view_m_ < 1) { view_m_ += 12; view_y_--; }
        while (view_m_ > 12) { view_m_ -= 12; view_y_++; }
        invalidate();
    }

    void go_today() {
        view_y_ = today_.y;
        view_m_ = today_.m;
        sel_ = today_;
        refresh_list();
    }

    bool has_event(const Date& d) const {
        for (size_t i = 0; i < events_.size(); i++)
            if (same(events_[i].date, d)) return true;
        return false;
    }

    void refresh_list() {
        list_->clear();
        for (size_t i = 0; i < events_.size(); i++)
            if (same(events_[i].date, sel_)) list_->add(events_[i].text.c_str());
        invalidate();
    }

    void add_event() {
        String text = String(field_->get_text()).trim();
        if (text.empty()) return;
        Event e;
        e.date = sel_;
        e.text = text;
        events_.push(e);
        field_->set_text("");
        save_events();
        refresh_list();
    }

    void load_events() {
        char path[160];
        events_path(path, sizeof path);
        String text;
        if (!vfs::read_text(path, text)) return;
        const char* p = text.c_str();
        while (*p) {
            const char* nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len > 11 && p[4] == '-' && p[7] == '-' && p[10] == '|') {
                Event e;
                e.date = Date{parse_num(p, 4), parse_num(p + 5, 2), parse_num(p + 8, 2)};
                e.text = String(p + 11, len - 11);
                if (!e.text.empty()) events_.push(e);
            }
            if (!nl) break;
            p = nl + 1;
        }
    }

    void save_events() {
        String out;
        for (size_t i = 0; i < events_.size(); i++) {
            char head[16];
            const Event& e = events_[i];
            snprintf(head, sizeof head, "%04d-%02d-%02d|", e.date.y, e.date.m, e.date.d);
            out += head;
            out += e.text;
            out += '\n';
        }
        char path[160];
        events_path(path, sizeof path);
        vfs::write_text(path, out.c_str());
    }

    Date today_{}, sel_{};
    int view_y_ = 0, view_m_ = 1;
    ListView* list_ = nullptr;
    TextField* field_ = nullptr;
    Vector<Event> events_;
};

// ---- icon: a tear-off calendar card with a red header, showing today
void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x8E9AAF), hex(0x4A5568));
    DateTime now = rtc_now();
    Rect card = r.inset(r.w / 8);
    float rad = card.w * 0.12f;
    float band_h = card.h * 0.36f;

    // rings binding the pages
    for (int i = 0; i < 2; i++) {
        int rx = card.x + card.w * (i == 0 ? 0.28f : 0.70f) - card.w * 0.03f;
        c.fill_round_rect(Rect(rx, card.y - card.h * 0.07f, (int)(card.w * 0.06f), (int)(card.h * 0.16f)), 2, hex(0x2B2F38));
    }
    c.draw_shadow(card, rad, card.w * 0.08f, rgba(0, 0, 0, 80), 0, card.w / 24);
    c.fill_round_rect(card, rad, hex(0xFFFFFF));
    c.save();
    c.clip_to(Rect(card.x, card.y, card.w, (int)band_h));
    c.fill_gradient(card, hex(0xFF5A4E), hex(0xD42A22), rad);
    c.restore();

    const char* wn = weekday_name(now.weekday);
    char wd[4] = {(char)toupper(wn[0]), (char)toupper(wn[1]), (char)toupper(wn[2]), 0};
    c.draw_text_in(Rect(card.x, card.y, card.w, (int)band_h), wd, hex(0xFFFFFF), (int)(card.w * 0.2f), Font::Bold, Align::Center);

    char day[4];
    snprintf(day, sizeof day, "%d", now.day);
    c.draw_text_in(Rect(card.x, card.y + (int)band_h, card.w, card.h - (int)band_h), day, hex(0x1C1C1E),
                   (int)(card.h * 0.5f), Font::Bold, Align::Center);
}

Window* create() { return new CalendarWindow(); }

AppRegistrar reg({"calendar", "Calendar", icon, create, "Productivity", true, true, 50, "Month view with events"});

}  // namespace
