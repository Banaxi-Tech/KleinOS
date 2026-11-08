// Console: live view of the kernel log. A read-only, dark, monospace TextArea
// that follows new lines until the user scrolls up. Clear only moves the
// display start (the kernel log itself is untouched).
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "lib/log.h"
#include "lib/heap.h"
#include "lib/math.h"
#include "lib/string.h"

namespace {

constexpr size_t kLogBytes = 256 * 1024;
constexpr size_t kAnchorBytes = 64;
constexpr int kToolbarH = 44;
constexpr int kStatusH = 26;
constexpr uint64_t kRefreshMs = 200;

const Color kLogBg = hex(0x16181D);
const Color kLogText = hex(0xD4D8E0);
const Color kLogWarn = hex(0xFFD60A);
const Color kLogError = hex(0xFF6B68);

// Case-insensitive search for w inside s[0..n).
bool has_ci(const char* s, size_t n, const char* w) {
    size_t wn = strlen(w);
    if (wn == 0) return true;
    if (wn > n) return false;
    for (size_t i = 0; i + wn <= n; i++) {
        size_t k = 0;
        while (k < wn && tolower((unsigned char)s[i + k]) == tolower((unsigned char)w[k])) k++;
        if (k == wn) return true;
    }
    return false;
}

Color line_color(const char* s, size_t n) {
    if (has_ci(s, n, "panic") || has_ci(s, n, "error") || has_ci(s, n, "fail")) return kLogError;
    if (has_ci(s, n, "warn")) return kLogWarn;
    return kLogText;
}

// TextArea that remembers whether the user scrolled away from the bottom.
class LogArea : public TextArea {
public:
    explicit LogArea(Rect r) : TextArea(r) {}
    void scroll(int x, int y, int delta) override {
        int before = scroll_y;
        TextArea::scroll(x, y, delta);
        if (delta < 0) follow = false;
        else if (delta > 0 && scroll_y == before) follow = true;   // already at the bottom
    }
    bool follow = true;
};

class ConsoleWindow : public Window {
public:
    ConsoleWindow() : Window("Console", 680, 440) {
        set_min_size(420, 260);
        set_ticking(true);
        snap_ = (char*)kmalloc(kLogBytes + 1);
        snap_[0] = 0;

        area_ = (LogArea*)add(new LogArea(Rect()));
        area_->readonly = true;
        area_->font = Font::Mono;
        area_->font_size = 12;
        area_->background = kLogBg;
        area_->highlighter = [](const char* line, int len, Color* colors) {
            Color col = line_color(line, (size_t)len);
            for (int k = 0; k < len; k++) colors[k] = col;
        };

        filter_ = (TextField*)add(new TextField(Rect(), "Filter"));
        filter_->rounded = true;
        filter_->search_icon = true;
        filter_->font_size = 13;
        filter_->on_change = [this] { rebuild_view(); };

        clear_btn_ = (Button*)add(new Button(Rect(), "Clear"));
        clear_btn_->on_click = [this] { clear_view(); };

        focus_widget(filter_);
        layout();
        refresh(false);
    }

    ~ConsoleWindow() override { kfree(snap_); }

    void on_paint(Canvas& c) override {
        const Theme& t = theme();
        int W = width(), H = height();
        c.clear(t.window_bg);
        c.fill_rect(Rect(0, kToolbarH - 1, W, 1), t.separator);
        for (Widget* w : widgets())
            if (w->visible) {
                c.save();
                w->paint(c);
                c.restore();
            }
        // status bar
        int sy = H - kStatusH;
        c.fill_rect(Rect(0, sy, W, 1), t.separator);
        bool live = area_->follow;
        Color dot = live ? hex(0x32D74B) : hex(0xFF9F0A);
        c.fill_circle(18, sy + kStatusH / 2.0f, 3.5f, dot);
        c.draw_text_in(Rect(28, sy, 120, kStatusH), live ? "Live" : "Paused", t.text_secondary, 12, Font::Medium);
        char info[96];
        snprintf(info, sizeof(info), "%d line%s", shown_, shown_ == 1 ? "" : "s");
        c.draw_text_in(Rect(12, sy, W - 24, kStatusH), info, t.text_secondary, 12, Font::Regular, Align::Right);
    }

    void on_resize() override { layout(); }

    void on_tick(uint64_t now) override {
        size_t gen = klog_generation();
        if (gen != last_gen_ && now - last_refresh_ >= kRefreshMs) {
            last_gen_ = gen;
            last_refresh_ = now;
            refresh(false);
        }
    }

    void on_key(const KeyEvent& e) override {
        if (e.key == Key::Char && e.cmd()) {
            uint32_t ch = e.ch | 0x20;
            if (ch == 'k') { clear_view(); return; }
            if (ch == 'f') { focus_widget(filter_); return; }
        }
        Window::on_key(e);
    }

    void build_menus(Vector<Menu>& menus) override {
        Menu& v = menus.emplace();
        v.title = "View";
        v.add("Clear Console", [this] { clear_view(); }, "\xE2\x8C\x98K");
        v.add("Find", [this] { focus_widget(filter_); }, "\xE2\x8C\x98" "F");
    }

private:
    void layout() {
        int W = width(), H = height();
        clear_btn_->bounds = Rect(W - 12 - 72, 8, 72, 28);
        filter_->bounds = Rect(12, 8, W - 12 - 72 - 8 - 12, 28);
        area_->bounds = Rect(0, kToolbarH, W, H - kToolbarH - kStatusH);
    }

    // Read the kernel log into snap_. take_anchor: remember the end of the
    // current log as the "cleared" point.
    void refresh(bool take_anchor) {
        size_t n = klog_snapshot(snap_, kLogBytes);
        snap_[n] = 0;
        last_gen_ = klog_generation();
        if (take_anchor) {
            anchor_len_ = n < kAnchorBytes ? n : kAnchorBytes;
            if (anchor_len_) memcpy(anchor_, snap_ + n - anchor_len_, anchor_len_);
        }
        rebuild_view();
    }

    // Everything after the clear point, filtered, goes into the text area.
    void rebuild_view() {
        size_t n = strlen(snap_);
        const char* src = snap_;
        if (anchor_len_ > 0 && n >= anchor_len_) {
            // find the last occurrence of the clear anchor
            for (size_t i = n - anchor_len_ + 1; i-- > 0;) {
                if (snap_[i] == anchor_[0] && memcmp(snap_ + i, anchor_, anchor_len_) == 0) {
                    src = snap_ + i + anchor_len_;
                    break;
                }
            }
        }
        const char* needle = filter_->get_text();
        String out;
        int shown = 0;
        const char* p = src;
        while (*p) {
            const char* e = p;
            while (*e && *e != '\n') e++;
            size_t len = (size_t)(e - p);
            if (!needle[0] || has_ci(p, len, needle)) {
                out.append(p, len);
                out += '\n';
                shown++;
            }
            p = *e ? e + 1 : e;
        }
        shown_ = shown;

        int saved = area_->scroll_y;
        area_->set_text(out.c_str());
        if (area_->follow) {
            area_->cursor = (int)out.size();
            area_->ensure_cursor_visible();
        } else {
            area_->scroll_y = saved;
            area_->TextArea::scroll(0, 0, 0);   // clamp to the new content height
        }
        invalidate();
    }

    void clear_view() {
        refresh(true);
        area_->follow = true;
        rebuild_view();
    }

    LogArea* area_ = nullptr;
    TextField* filter_ = nullptr;
    Button* clear_btn_ = nullptr;
    char* snap_ = nullptr;             // kLogBytes + 1 bytes
    char anchor_[kAnchorBytes] = {0};
    size_t anchor_len_ = 0;
    size_t last_gen_ = (size_t)-1;
    uint64_t last_refresh_ = 0;
    int shown_ = 0;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x4A5263), hex(0x14171D));
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, u = r.w / 40.0f;
    Color green = hex(0x32D74B);
    c.draw_line(cx - 9 * u, cy - 8 * u, cx - 1 * u, cy, green, 3.0f * u);
    c.draw_line(cx - 1 * u, cy, cx - 9 * u, cy + 8 * u, green, 3.0f * u);
    c.draw_line(cx + 3 * u, cy + 9 * u, cx + 11 * u, cy + 9 * u, hex(0xE5E5EA), 3.0f * u);
}

Window* create() { return new ConsoleWindow(); }

AppRegistrar reg({"console", "Console", icon, create, "System", true, false, 100, "View the live kernel log"});

}  // namespace
