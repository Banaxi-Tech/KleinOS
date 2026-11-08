// Reminders: glass sidebar of smart lists, a checklist, and items saved in the home folder.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "drivers/rtc.h"
#include "fs/vfs.h"
#include "sys/session.h"
#include "lib/kstring.h"
#include "lib/math.h"

namespace {

constexpr int kW = 600, kH = 470, kSide = 200;
constexpr int kRowH = 44, kListTop = 84;
constexpr int kViewBottom = kH - 60;            // rows end above the input field
constexpr int kFieldY = kH - 50;

enum Filter : int { Today, Scheduled, All, Completed, kFilterCount };
const char* const kFilterNames[kFilterCount] = {"Today", "Scheduled", "All", "Completed"};

// Dates are "YYYY-MM-DD" strings, so plain string comparison orders them.
struct Item {
    bool done = false;
    String due;      // empty = no date
    String text;
};

// Local date as "YYYY-MM-DD", offset by whole days.
String local_date(int add_days) {
    uint64_t t = rtc_unix_time() + (int64_t)rtc_utc_offset_minutes() * 60 + (uint64_t)(add_days * 86400);
    DateTime d = unix_to_datetime(t);
    char b[12];
    snprintf(b, sizeof b, "%04d-%02d-%02d", d.year, d.month, d.day);
    return String(b);
}

bool in_filter(const Item& it, int f, const String& today, const String& tomorrow) {
    (void)tomorrow;
    switch (f) {
    case Today: return !it.done && (it.due.empty() || strcmp(it.due.c_str(), today.c_str()) <= 0);
    case Scheduled: return !it.done && !it.due.empty() && strcmp(it.due.c_str(), today.c_str()) > 0;
    case Completed: return it.done;
    default: return true;   // All
    }
}

// "14 Nov", or "Today" for today's date.
String due_label(const String& due, const String& today) {
    if (due == today) return String("Today");
    if (due.size() != 10) return due;
    int m = (due[5] - '0') * 10 + (due[6] - '0');
    int d = (due[8] - '0') * 10 + (due[9] - '0');
    if (m < 1 || m > 12) return due;
    char mon[4];
    memcpy(mon, month_name(m), 3);
    mon[3] = 0;
    return String::format("%d %s", d, mon);
}

class RemindersWindow : public Window {
public:
    RemindersWindow() : Window("Reminders", kW, kH, WF_NO_ZOOM) {
        set_sidebar_width(kSide);

        list_ = (ListView*)add(new ListView(Rect(8, 40, kSide - 16, kFilterCount * 34 + 6)));
        list_->transparent = true;
        list_->row_height = 34;
        list_->on_select = [this](int i) { set_filter(i); };

        field_ = (TextField*)add(new TextField(Rect(kSide + 16, kFieldY, kW - kSide - 32, 30), "Add a reminder"));
        field_->rounded = true;
        field_->on_submit = [this] { add_item(); };

        load();
        rebuild_counts();
        rebuild_view();
    }

    void on_paint(Canvas& c) override {
        Window::on_paint(c);
        const Theme& t = theme();
        String today = local_date(0);

        c.draw_text_in(Rect(20, 14, kSide - 40, 20), "Lists", t.text_secondary, 12, Font::Medium, Align::Left);

        // header
        c.draw_text_in(Rect(kSide + 20, 18, kW - kSide - 40, 30), kFilterNames[filter_], t.text, 24, Font::Bold,
                       Align::Left);
        char sub[32];
        int n = (int)view_.size();
        snprintf(sub, sizeof sub, "%d %s", n, n == 1 ? "item" : "items");
        c.draw_text_in(Rect(kSide + 20, 50, kW - kSide - 40, 18), sub, t.text_secondary, 13, Font::Regular, Align::Left);

        int view_h = kViewBottom - kListTop;
        if (view_.size() == 0) {
            c.draw_text_in(Rect(kSide, kListTop, kW - kSide, view_h), "No reminders", t.text_tertiary, 15,
                           Font::Regular, Align::Center);
        }

        c.save();
        c.clip_to(Rect(kSide, kListTop, kW - kSide, view_h));
        for (int i = 0; i < n; i++) {
            int y = kListTop + i * kRowH - scroll_;
            if (y + kRowH < kListTop || y > kViewBottom) continue;
            const Item& it = items_[view_[i]];
            Rect row(kSide + 8, y, kW - kSide - 16, kRowH);
            if (i == selected_)
                c.fill_round_rect(row, 8, with_alpha(t.accent, t.dark ? 60 : 40));

            // round checkbox
            float cx = row.x + 22.0f, cy = y + kRowH / 2.0f, cr = 9.0f;
            if (it.done) {
                c.fill_circle(cx, cy, cr, t.accent);
                draw_symbol(c, Rect((int)(cx - 6), (int)(cy - 6), 12, 12), Symbol::Check, hex(0xFFFFFF), 1.8f);
            } else {
                c.stroke_circle(cx, cy, cr, t.text_tertiary, 1.6f);
            }

            // due date on the right
            int tx = row.x + 42;
            int right = row.right() - 10;
            bool has_due = !it.due.empty() && !it.done;
            if (has_due) {
                bool overdue = strcmp(it.due.c_str(), today.c_str()) < 0;
                String label = due_label(it.due, today);
                int lw = text_width(label.c_str(), 12, Font::Medium);
                c.draw_text_in(Rect(right - lw - 2, y, lw + 4, kRowH), label.c_str(),
                               overdue ? t.destructive : t.text_secondary, 12, Font::Medium, Align::Right);
                right -= lw + 14;
            }

            // title, struck through when done
            int avail = right - tx;
            Color fg = it.done ? t.text_tertiary : t.text;
            c.draw_text_in(Rect(tx, y, avail, kRowH), it.text.c_str(), fg, 15, Font::Regular, Align::Left);
            if (it.done) {
                int tw = text_width(it.text.c_str(), 15);
                if (tw > avail) tw = avail;
                c.draw_line((float)tx, cy, (float)(tx + tw), cy, t.text_tertiary, 1.2f);
            }
            if (i < n - 1 && i != selected_ && i + 1 != selected_)
                c.fill_rect(Rect(tx, y + kRowH - 1, row.right() - tx - 8, 1), t.separator);
        }
        c.restore();
    }

    void on_mouse_down(int x, int y, int button) override {
        if (x >= kSide && y >= kListTop && y < kViewBottom) {
            int idx = (y - kListTop + scroll_) / kRowH;
            focus_widget(nullptr);
            if (idx >= 0 && idx < (int)view_.size()) {
                selected_ = idx;
                toggle(view_[idx]);
            } else {
                selected_ = -1;
                invalidate();
            }
            return;
        }
        Window::on_mouse_down(x, y, button);
    }

    void on_scroll(int x, int y, int delta) override {
        if (x < kSide) { Window::on_scroll(x, y, delta); return; }
        scroll_ += delta * 22;
        clamp_scroll();
        invalidate();
    }

    void on_key(const KeyEvent& e) override {
        bool typing = focused_widget() == field_;
        if (!typing && e.key == Key::Delete && selected_ >= 0 && selected_ < (int)view_.size()) {
            remove_selected();
            return;
        }
        if (!typing && e.key == Key::Escape) {
            selected_ = -1;
            invalidate();
            return;
        }
        if (!typing && (e.key == Key::Up || e.key == Key::Down) && view_.size()) {
            int n = (int)view_.size();
            int s = selected_ < 0 ? (e.key == Key::Down ? 0 : n - 1) : selected_ + (e.key == Key::Down ? 1 : -1);
            selected_ = kclamp(s, 0, n - 1);
            ensure_visible(selected_);
            invalidate();
            return;
        }
        Window::on_key(e);
    }

private:
    void set_filter(int f) {
        if (f < 0 || f >= kFilterCount) return;
        filter_ = f;
        selected_ = -1;
        scroll_ = 0;
        list_->selected = f;
        rebuild_view();
    }

    // Recomputes which items the current list shows. Toggling done doesn't call this,
    // so a checked item stays visible (struck through) until you switch lists.
    void rebuild_view() {
        view_.clear();
        String today = local_date(0), tomorrow = local_date(1);
        for (int i = 0; i < (int)items_.size(); i++)
            if (in_filter(items_[i], filter_, today, tomorrow)) view_.push(i);
        if (selected_ >= (int)view_.size()) selected_ = -1;
        clamp_scroll();
        invalidate();
    }

    void rebuild_counts() {
        list_->clear();
        String today = local_date(0), tomorrow = local_date(1);
        for (int f = 0; f < kFilterCount; f++) {
            int n = 0;
            for (int i = 0; i < (int)items_.size(); i++)
                if (in_filter(items_[i], f, today, tomorrow)) n++;
            char cnt[16];
            snprintf(cnt, sizeof cnt, "%d", n);
            list_->add(kFilterNames[f], cnt);
        }
        list_->selected = filter_;
        invalidate();
    }

    void refresh_all() {
        save();
        rebuild_counts();
        rebuild_view();
    }

    void toggle(int idx) {
        items_[idx].done = !items_[idx].done;
        save();
        rebuild_counts();
        invalidate();
    }

    void remove_selected() {
        int idx = view_[selected_];
        items_.remove(idx);
        selected_ = -1;
        refresh_all();
    }

    void add_item() {
        String text = String(field_->get_text()).trim();
        if (text.empty()) return;
        String due;
        if (filter_ == Today) due = local_date(0);
        else if (filter_ == Scheduled) due = local_date(1);
        else if (filter_ == Completed) set_filter(All);   // a new open item would vanish from here
        Item& it = items_.emplace();
        it.text = text;
        it.due = due;
        field_->set_text("");
        refresh_all();
    }

    void ensure_visible(int i) {
        int top = i * kRowH;
        int view_h = kViewBottom - kListTop;
        if (top < scroll_) scroll_ = top;
        if (top + kRowH > scroll_ + view_h) scroll_ = top + kRowH - view_h;
        clamp_scroll();
    }

    void clamp_scroll() {
        int max = (int)view_.size() * kRowH - (kViewBottom - kListTop);
        if (max < 0) max = 0;
        scroll_ = kclamp(scroll_, 0, max);
    }

    void path(char* out, size_t n) { snprintf(out, n, "/Users/%s/Library/Reminders/items.txt", session_user()); }

    // One item per line: "<done 0|1>|<due or ->|<text>"
    void load() {
        char p[160];
        path(p, sizeof p);
        String text;
        if (!vfs::read_text(p, text)) return;
        size_t pos = 0;
        while (pos < text.size()) {
            long nl = text.find('\n', pos);
            size_t end = nl < 0 ? text.size() : (size_t)nl;
            String line = text.substr(pos, end - pos);
            pos = end + 1;
            if (line.size() < 4 || (line[0] != '0' && line[0] != '1') || line[1] != '|') continue;
            long bar = line.find('|', 2);
            if (bar < 0) continue;
            String due = line.substr(2, (size_t)bar - 2);
            String body = line.substr((size_t)bar + 1);
            if (body.empty()) continue;
            Item& it = items_.emplace();
            it.done = line[0] == '1';
            it.due = due == "-" ? String() : due;
            it.text = body;
        }
    }

    void save() {
        String out;
        for (int i = 0; i < (int)items_.size(); i++) {
            const Item& it = items_[i];
            out += it.done ? '1' : '0';
            out += '|';
            out += it.due.empty() ? String("-") : it.due;
            out += '|';
            out += it.text;
            out += '\n';
        }
        char p[160];
        path(p, sizeof p);
        vfs::write_text(p, out.c_str());
    }

    ListView* list_ = nullptr;
    TextField* field_ = nullptr;
    Vector<Item> items_;
    Vector<int> view_;          // indices into items_ shown by the current list
    int filter_ = All;
    int selected_ = -1;         // index into view_
    int scroll_ = 0;
};

// ---- icon: teal squircle with a white checklist card
void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x4FE0CF), hex(0x0E7C72));
    Rect card = r.inset(r.w / 5);
    float rad = card.w * 0.14f;
    c.draw_shadow(card, rad, card.w * 0.06f, rgba(0, 0, 0, 60), 0, card.w / 30);
    c.fill_round_rect(card, rad, hex(0xFFFFFF));

    float u = card.w / 40.0f;
    for (int i = 0; i < 3; i++) {
        float cy = card.y + card.h * (0.25f + i * 0.25f);
        float cx = card.x + card.w * 0.24f;
        float cr = card.w * 0.095f;
        if (i == 0) {
            c.fill_circle(cx, cy, cr, hex(0x0E9C8F));
            draw_symbol(c, Rect((int)(cx - cr * 0.7f), (int)(cy - cr * 0.7f), (int)(cr * 1.4f), (int)(cr * 1.4f)),
                        Symbol::Check, hex(0xFFFFFF), u * 1.3f);
            c.draw_line(card.x + card.w * 0.42f, cy, card.x + card.w * 0.80f, cy, hex(0xB8C2CC), u * 1.5f);
        } else {
            c.stroke_circle(cx, cy, cr, hex(0x9AA6B2), u * 1.3f);
            float x1 = card.x + card.w * (i == 1 ? 0.74f : 0.62f);
            c.draw_line(card.x + card.w * 0.42f, cy, x1, cy, hex(0x3A4450), u * 1.5f);
        }
    }
}

Window* create() { return new RemindersWindow(); }

AppRegistrar reg({"reminders", "Reminders", icon, create, "Productivity", true, false, 55, "Lists of things to do"});

}  // namespace
