// Files — Finder-style file manager: glass sidebar, icon and list views,
// inline rename, context menu, trash and keyboard navigation.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "fs/vfs.h"
#include "sys/session.h"
#include "drivers/input.h"
#include "drivers/rtc.h"
#include "lib/string.h"
#include "lib/kstring.h"
#include "lib/vector.h"
#include "lib/math.h"

namespace {

constexpr int kSidebarW = 190;
constexpr int kToolbarH = 50;
constexpr int kFooterH = 26;
constexpr int kCellW = 116;
constexpr int kCellH = 112;
constexpr int kIconSize = 60;
constexpr int kRowH = 26;
constexpr int kHeaderH = 24;
constexpr int kSideHeaderH = 26;
constexpr int kSideItemH = 26;

enum class Kind : uint8_t { Folder, Module, Image, Text, Code, Audio, Document };
enum class ViewMode : uint8_t { Icons, List };

const char* kind_name(Kind k) {
    switch (k) {
    case Kind::Folder: return "Folder";
    case Kind::Module: return "KleinOS Module";
    case Kind::Image: return "Image";
    case Kind::Text: return "Text";
    case Kind::Code: return "Source";
    case Kind::Audio: return "Audio";
    default: return "Document";
    }
}

Kind kind_for(const String& name, bool is_dir) {
    if (is_dir) return Kind::Folder;
    String ext = vfs::extension(name.c_str());
    if (ext == "dl") return Kind::Module;
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" || ext == "gif") return Kind::Image;
    if (ext == "txt" || ext == "md" || ext == "log" || ext == "rtf" || ext == "readme") return Kind::Text;
    if (ext == "cpp" || ext == "h" || ext == "c" || ext == "hpp" || ext == "html" || ext == "htm" || ext == "js" ||
        ext == "css" || ext == "json" || ext == "py" || ext == "sh" || ext == "cfg" || ext == "s") return Kind::Code;
    if (ext == "mp3" || ext == "wav" || ext == "ogg" || ext == "flac") return Kind::Audio;
    return Kind::Document;
}

void fmt_size(char* buf, size_t n, uint64_t bytes) {
    if (bytes < 1024) snprintf(buf, n, "%llu bytes", (unsigned long long)bytes);
    else if (bytes < 1024ull * 1024) snprintf(buf, n, "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024) snprintf(buf, n, "%.1f MB", bytes / 1048576.0);
    else snprintf(buf, n, "%.2f GB", bytes / 1073741824.0);
}

// Rounds float geometry to a Rect.
Rect R(float x, float y, float w, float h) {
    return Rect((int)(x + 0.5f), (int)(y + 0.5f), (int)(w + 0.5f), (int)(h + 0.5f));
}

// Case-insensitive ordering of two names.
int name_cmp(const String& a, const String& b) {
    const char* p = a.c_str();
    const char* q = b.c_str();
    while (*p && *q) {
        int x = tolower((unsigned char)*p), y = tolower((unsigned char)*q);
        if (x != y) return x - y;
        p++;
        q++;
    }
    return (int)tolower((unsigned char)*p) - (int)tolower((unsigned char)*q);
}

// "name.ext" -> stem "name", ext ".ext" (ext empty when there is no dot).
void split_ext(const String& name, String& stem, String& ext) {
    long dot = name.rfind('.');
    if (dot > 0) { stem = name.substr(0, dot); ext = name.substr(dot); }
    else { stem = name; ext = ""; }
}

// A free path in dir for a name; tries "stem suffix.ext", then "stem suffix 2.ext", ...
String unique_path(const String& dir, const String& name, const char* suffix) {
    String stem, ext;
    split_ext(name, stem, ext);
    String cand = dir + "/" + name;
    if (!vfs::exists(cand.c_str())) return cand;
    for (int n = 1; n < 1000; n++) {
        String nm = stem;
        if (suffix && *suffix) { nm += " "; nm += suffix; }
        if (n > 1) nm += String::format(" %d", n);
        nm += ext;
        cand = dir + "/" + nm;
        if (!vfs::exists(cand.c_str())) return cand;
    }
    return cand;
}

// ---------------------------------------------------------------- icons
void page(Canvas& c, Rect r, Color fill, Color edge) {
    float u = r.w / 64.0f, x = (float)r.x, y = (float)r.y;
    PointF p[5] = {
        {x + 12 * u, y + 4 * u}, {x + 40 * u, y + 4 * u}, {x + 52 * u, y + 16 * u},
        {x + 52 * u, y + 60 * u}, {x + 12 * u, y + 60 * u},
    };
    c.draw_shadow(R(x + 12 * u, y + 4 * u, 40 * u, 56 * u), 3 * u, 3 * u, rgba(0, 0, 0, 45), 0, (int)(1.5f * u) + 1);
    c.fill_polygon(p, 5, fill);
    PointF f[3] = {{x + 40 * u, y + 4 * u}, {x + 40 * u, y + 16 * u}, {x + 52 * u, y + 16 * u}};
    c.fill_polygon(f, 3, edge);
    c.draw_line(x + 40 * u, y + 4 * u, x + 52 * u, y + 16 * u, edge, 1.0f);
}

void draw_folder(Canvas& c, Rect r) {
    float u = r.w / 64.0f, x = (float)r.x, y = (float)r.y;
    c.fill_round_rect(R(x + 4 * u, y + 9 * u, 24 * u, 12 * u), 4 * u, hex(0x1B63C9));
    Rect body = R(x, y + 14 * u, r.w, r.h - 14 * u);
    c.draw_shadow(body, 7 * u, 3 * u, rgba(0, 0, 0, 55), 0, (int)(2 * u) + 1);
    c.fill_gradient(body, hex(0x93D3FF), hex(0x2E7FE8), 7 * u);
    c.fill_gradient(R(x + 1 * u, y + 15 * u, r.w - 2 * u, 12 * u), rgba(255, 255, 255, 85), rgba(255, 255, 255, 0), 6 * u);
    c.stroke_round_rect(body, 7 * u, rgba(255, 255, 255, 110), 1.0f);
}

void draw_module(Canvas& c, Rect r) {
    // puzzle piece: a body with a knob on top and on the right, and a socket on the left
    float u = r.w / 64.0f, x = (float)r.x, y = (float)r.y;
    Color lo = hex(0x5B2FD6), hi = hex(0xB48CFF);
    c.fill_circle(x + 30 * u, y + 17 * u, 8 * u, lo);
    c.fill_circle(x + 52 * u, y + 36 * u, 8 * u, lo);
    Rect body = R(x + 12 * u, y + 16 * u, 40 * u, 42 * u);
    c.draw_shadow(body, 7 * u, 4 * u, rgba(0, 0, 0, 55), 0, (int)(2 * u) + 1);
    c.fill_gradient(body, hi, lo, 7 * u);
    c.fill_gradient(R(x + 13 * u, y + 17 * u, 38 * u, 16 * u), rgba(255, 255, 255, 80), rgba(255, 255, 255, 0), 6 * u);
    c.fill_circle(x + 12 * u, y + 37 * u, 6 * u, hex(0x2A1A4F));   // socket
    c.fill_circle(x + 12 * u, y + 37 * u, 4 * u, hi);
    c.stroke_round_rect(body, 7 * u, rgba(255, 255, 255, 120), 1.0f);
}

void draw_entry_icon(Canvas& c, Rect r, Kind k) {
    float u = r.w / 64.0f, x = (float)r.x, y = (float)r.y;
    switch (k) {
    case Kind::Folder: draw_folder(c, r); return;
    case Kind::Module: draw_module(c, r); return;
    case Kind::Image: {
        page(c, r, hex(0xF6F7FA), hex(0xC9CDD6));
        Rect frame = R(x + 17 * u, y + 24 * u, 30 * u, 24 * u);
        c.fill_round_rect(frame, 3 * u, hex(0x2B7BE6));
        c.fill_gradient(frame, hex(0x9BDDFF), hex(0x3C8DF0), 3 * u);
        c.fill_circle(x + 39 * u, y + 31 * u, 3.2f * u, hex(0xFFD35C));
        PointF hills[5] = {{x + 17 * u, y + 48 * u}, {x + 27 * u, y + 38 * u}, {x + 34 * u, y + 44 * u},
                           {x + 41 * u, y + 37 * u}, {x + 47 * u, y + 48 * u}};
        c.fill_polygon(hills, 5, hex(0x34A853));
        return;
    }
    case Kind::Text:
        page(c, r, hex(0xF6F7FA), hex(0xC9CDD6));
        for (int i = 0; i < 4; i++) {
            float ly = y + (26 + i * 8) * u;
            float lx1 = x + (i == 3 ? 36 : 46) * u;
            c.draw_line(x + 19 * u, ly, lx1, ly, hex(0x9AA1AD), 2.0f * u + 0.5f);
        }
        return;
    case Kind::Code:
        page(c, r, hex(0xF6F7FA), hex(0xC9CDD6));
        c.draw_text_in(R(x + 12 * u, y + 24 * u, 40 * u, 24 * u), "</>", hex(0x2E7FE8), (int)(15 * u + 0.5f), Font::Bold, Align::Center);
        return;
    case Kind::Audio:
        page(c, r, hex(0xF6F7FA), hex(0xC9CDD6));
        draw_symbol(c, R(x + 17 * u, y + 22 * u, 30 * u, 30 * u), Symbol::Music, hex(0xE0457B), 2.4f);
        return;
    default:
        page(c, r, hex(0xF6F7FA), hex(0xC9CDD6));
        for (int i = 0; i < 3; i++) {
            float ly = y + (28 + i * 8) * u;
            c.draw_line(x + 19 * u, ly, x + 46 * u, ly, hex(0xB7BCC6), 2.0f * u + 0.5f);
        }
        return;
    }
}

// ---------------------------------------------------------------- model
struct Entry {
    String name;
    String path;
    bool is_dir = false;
    bool readonly = false;
    uint64_t size = 0;
    uint64_t mtime = 0;
    Kind kind = Kind::Document;
};

struct SideItem {
    String label;
    String path;
    bool header = false;
    Symbol sym = Symbol::Folder;
    Color color = hex(0x2E7FE8);
};

class FilesWindow : public Window {
public:
    FilesWindow() : Window("Files", 860, 540, WF_RESIZABLE) {
        set_sidebar_width(kSidebarW);
        set_min_size(700, 380);
        build_sidebar();

        back_ = (Button*)add(new Button(Rect(0, 0, 34, 30), "", Button::Glass));
        back_->icon = [](Canvas& c, Rect r) { draw_symbol(c, r.inset(r.w / 4), Symbol::ChevronLeft, theme().text, 1.8f); };
        back_->on_click = [this] { go_back(); };
        fwd_ = (Button*)add(new Button(Rect(0, 0, 34, 30), "", Button::Glass));
        fwd_->icon = [](Canvas& c, Rect r) { draw_symbol(c, r.inset(r.w / 4), Symbol::ChevronRight, theme().text, 1.8f); };
        fwd_->on_click = [this] { go_forward(); };

        view_ = (SegmentedControl*)add(new SegmentedControl(Rect(0, 0, 118, 30), {"Icons", "List"}));
        view_->on_change = [this](int i) { mode_ = i == 0 ? ViewMode::Icons : ViewMode::List; scroll_y_ = 0; invalidate(); };

        search_ = (TextField*)add(new TextField(Rect(0, 0, 160, 30), "Search"));
        search_->rounded = true;
        search_->search_icon = true;
        search_->font_size = 13;
        search_->on_change = [this] { reload(); };

        new_btn_ = (Button*)add(new Button(Rect(0, 0, 104, 30), "New Folder"));
        new_btn_->on_click = [this] { new_folder(); };

        rename_ = (TextField*)add(new TextField(Rect(0, 0, 100, 24), ""));
        rename_->font_size = 13;
        rename_->visible = false;
        rename_->on_submit = [this] { commit_rename(); };

        layout();
        navigate(home_path(), false);
        focus_widget(nullptr);   // Window::add gave the search field initial focus
        set_ticking(true);
    }

    void on_resize() override { layout(); }

    void on_tick(uint64_t) override {
        if (vfs::generation() != gen_) reload();
    }

    // Opens a folder (used by app_open_file and other apps).
    void navigate_to(const String& path) { navigate(path, true); }

    void on_paint(Canvas& c) override {
        const Theme& t = theme();
        int w = width(), h = height();
        // background: opaque content, glass sidebar on the left
        c.clear(t.window_bg);
        c.save();
        c.clip_to(Rect(0, 0, kSidebarW, h));
        c.clear(rgba(0, 0, 0, 0));
        c.restore();
        c.fill_rect(Rect(kSidebarW - 1, 0, 1, h), t.separator);
        c.fill_rect(Rect(kSidebarW, kToolbarH - 1, w - kSidebarW, 1), t.separator);
        c.fill_rect(Rect(kSidebarW, h - kFooterH, w - kSidebarW, 1), t.separator);

        paint_sidebar(c);
        paint_title(c);
        if (mode_ == ViewMode::Icons) paint_icons(c);
        else paint_list(c);
        paint_footer(c);

        // widgets last, so the rename field sits above the items
        for (Widget* wd : widgets()) {
            if (!wd->visible) continue;
            c.save();
            wd->paint(c);
            c.restore();
        }
    }

    void on_mouse_down(int x, int y, int button) override {
        if (renaming_ && !rename_->bounds.contains(x, y)) commit_rename();
        Widget* under = widget_at(x, y);
        if (under) { Window::on_mouse_down(x, y, button); return; }

        if (x < kSidebarW) {
            if (button == MB_LEFT) sidebar_click(y);
            return;
        }
        if (y < kToolbarH || y >= height() - kFooterH) { Window::on_mouse_down(x, y, button); return; }

        if (focused_widget()) focus_widget(nullptr);
        int idx = index_at(x, y);
        if (idx >= 0) {
            if (button == MB_LEFT || (button == MB_RIGHT && idx != sel_)) select(idx);
        } else if (button == MB_LEFT) {
            select(-1);
        }
        if (button == MB_RIGHT) {
            Rect cr = content_rect();
            context_menu(cr.x + x, cr.y + y, idx);
        }
    }

    void on_double_click(int x, int y) override {
        if (x < kSidebarW || y < kToolbarH || y >= height() - kFooterH) return;
        int idx = index_at(x, y);
        if (idx >= 0) open_index(idx);
    }

    void on_scroll(int x, int y, int delta) override {
        if (x < kSidebarW || y < kToolbarH || y >= height() - kFooterH) { Window::on_scroll(x, y, delta); return; }
        scroll_y_ += delta * (mode_ == ViewMode::Icons ? kCellH / 2 : kRowH * 2);
        clamp_scroll();
        invalidate();
    }

    void on_key(const KeyEvent& e) override {
        if (!e.pressed) return;
        if (renaming_) {
            if (e.key == Key::Escape) { cancel_rename(); return; }
            Window::on_key(e);
            return;
        }
        Widget* f = focused_widget();
        if (f && f == search_) {
            if (e.key == Key::Escape) {
                if (!search_->text.empty()) { search_->set_text(""); reload(); }
                else focus_widget(nullptr);
                return;
            }
            if (e.key == Key::Enter || e.key == Key::Down) {
                focus_widget(nullptr);
                if (sel_ < 0 && !entries_.empty()) select(0);
                return;
            }
            Window::on_key(e);
            return;
        }
        if (f) focus_widget(nullptr);

        int cols = icon_cols();
        int n = (int)entries_.size();
        int step_row = mode_ == ViewMode::Icons ? cols : 1;
        switch (e.key) {
        case Key::Left: if (mode_ == ViewMode::Icons) move_sel(-1); return;
        case Key::Right: if (mode_ == ViewMode::Icons) move_sel(1); return;
        case Key::Up: move_sel(-step_row); return;
        case Key::Down: move_sel(step_row); return;
        case Key::Home: if (n) select(0); return;
        case Key::End: if (n) select(n - 1); return;
        case Key::Enter: if (sel_ >= 0) open_index(sel_); return;
        case Key::Backspace:
            if (e.cmd()) { if (sel_ >= 0) trash_index(sel_); }
            else go_up();
            return;
        case Key::Delete:
            if (e.cmd() && sel_ >= 0) trash_index(sel_);
            return;
        case Key::Escape: select(-1); return;
        case Key::Char:
            if (e.cmd()) {
                uint32_t ch = e.ch | 0x20;
                if (ch == 'n') new_folder();
                else if (ch == 'o' && sel_ >= 0) open_index(sel_);
                else if (ch == 'r' && sel_ >= 0) begin_rename(sel_);
                else if (ch == 'i' && sel_ >= 0) show_info(sel_);
                else if (ch == 'l') navigate(cwd_, true);
                return;
            }
            // typing starts a search
            focus_widget(search_);
            search_->key(e);
            return;
        default:
            Window::on_key(e);
            return;
        }
    }

    void build_menus(Vector<Menu>& menus) override {
        Menu& file = menus.emplace();
        file.title = "File";
        file.add("New Folder", [this] { new_folder(); }, "⇧⌘N");
        file.add("Get Info", [this] { if (sel_ >= 0) show_info(sel_); }, "⌘I");
        file.separator();
        file.add("Move to Trash", [this] { if (sel_ >= 0) trash_index(sel_); }, "⌘⌫");
        Menu& view = menus.emplace();
        view.title = "View";
        view.add("as Icons", [this] { set_mode(ViewMode::Icons); });
        view.add("as List", [this] { set_mode(ViewMode::List); });
        view.separator();
        view.add("Enclosing Folder", [this] { go_up(); }, "⌘↑");
    }

private:
    // ---------------------------------------------------------- layout
    void layout() {
        int w = width();
        back_->bounds = Rect(kSidebarW + 12, 10, 34, 30);
        fwd_->bounds = Rect(kSidebarW + 52, 10, 34, 30);
        search_->bounds = Rect(w - 12 - 160, 10, 160, 30);
        new_btn_->bounds = Rect(search_->bounds.x - 10 - 104, 10, 104, 30);
        view_->bounds = Rect(new_btn_->bounds.x - 12 - 118, 10, 118, 30);
        clamp_scroll();
        invalidate();
    }

    Rect main_rect() const {
        return Rect(kSidebarW, kToolbarH, width() - kSidebarW, height() - kToolbarH - kFooterH);
    }
    int list_top() const { return main_rect().y + (mode_ == ViewMode::List ? kHeaderH : 0); }
    int icon_cols() const {
        int c = main_rect().w / kCellW;
        return c < 1 ? 1 : c;
    }
    int icon_origin_x() const { return kSidebarW + (main_rect().w - icon_cols() * kCellW) / 2; }

    int content_height() const {
        int n = (int)entries_.size();
        if (mode_ == ViewMode::Icons) {
            int cols = icon_cols();
            return ((n + cols - 1) / cols) * kCellH + 12;
        }
        return n * kRowH;
    }
    void clamp_scroll() {
        int view = main_rect().bottom() - list_top();
        int max = content_height() - view;
        if (max < 0) max = 0;
        if (scroll_y_ > max) scroll_y_ = max;
        if (scroll_y_ < 0) scroll_y_ = 0;
    }

    // content-local rectangle of item i (may be off-screen)
    Rect item_rect(int i) const {
        int top = list_top();
        if (mode_ == ViewMode::Icons) {
            int cols = icon_cols();
            int row = i / cols, col = i % cols;
            return Rect(icon_origin_x() + col * kCellW, top + 6 + row * kCellH - scroll_y_, kCellW, kCellH);
        }
        return Rect(kSidebarW, top + i * kRowH - scroll_y_, main_rect().w, kRowH);
    }

    int index_at(int x, int y) const {
        int n = (int)entries_.size();
        int top = list_top();
        if (y < top || x < kSidebarW) return -1;
        if (mode_ == ViewMode::Icons) {
            int ox = icon_origin_x();
            if (x < ox) return -1;
            int col = (x - ox) / kCellW, row = (y - top - 6 + scroll_y_);
            if (row < 0) return -1;
            row /= kCellH;
            int idx = row * icon_cols() + col;
            if (col >= icon_cols() || idx >= n) return -1;
            Rect cell = item_rect(idx);
            return cell.contains(x, y) ? idx : -1;
        }
        int row = (y - top + scroll_y_) / kRowH;
        return row >= 0 && row < n ? row : -1;
    }

    // ---------------------------------------------------------- sidebar
    void build_sidebar() {
        String home = home_path();
        struct Def { const char* label; const char* sub; Symbol sym; Color col; bool header; };
        static const Def defs[] = {
            {"Favorites", nullptr, Symbol::Folder, 0, true},
            {"Home", "", Symbol::Home, hex(0x5E8BFF), false},
            {"Desktop", "Desktop", Symbol::Display, hex(0x3D9BFF), false},
            {"Documents", "Documents", Symbol::File, hex(0x4C7DFF), false},
            {"Downloads", "Downloads", Symbol::Download, hex(0x30B0C7), false},
            {"Pictures", "Pictures", Symbol::Image, hex(0x34A853), false},
            {"Music", "Music", Symbol::Music, hex(0xE0457B), false},
            {"Applications", "/System/Apps", Symbol::Grid, hex(0xA55EEA), false},
            {"Locations", nullptr, Symbol::Folder, 0, true},
            {"KleinOS", "/", Symbol::Cpu, hex(0x8E9AAF), false},
        };
        for (const Def& d : defs) {
            SideItem& it = side_.emplace();
            it.label = d.label;
            it.header = d.header;
            it.sym = d.sym;
            it.color = d.col;
            if (d.header) continue;
            if (d.sub == nullptr) continue;
            if (!strcmp(d.label, "Home")) it.path = home;
            else if (d.sub[0] == '/') it.path = d.sub;
            else it.path = home + "/" + d.sub;
        }
    }

    int side_y(size_t idx) const {
        int y = 12;
        for (size_t i = 0; i < idx; i++) y += side_[i].header ? kSideHeaderH : kSideItemH;
        return y;
    }

    void paint_sidebar(Canvas& c) {
        const Theme& t = theme();
        int y = 12;
        for (size_t i = 0; i < side_.size(); i++) {
            const SideItem& it = side_[i];
            if (it.header) {
                c.draw_text_in(Rect(16, y + 4, kSidebarW - 24, 18), it.label.c_str(), t.text_secondary, 11, Font::Medium);
                y += kSideHeaderH;
                continue;
            }
            bool current = !it.path.empty() && it.path == cwd_;
            Rect row(8, y + 1, kSidebarW - 16, kSideItemH - 2);
            if (current) c.fill_round_rect(row, 7, with_alpha(t.accent, t.dark ? 90 : 60));
            draw_symbol(c, Rect(16, y + 6, 16, 16), it.sym, it.color, 1.7f);
            c.draw_text_in(Rect(40, y, kSidebarW - 48, kSideItemH), it.label.c_str(), t.text, 13,
                           current ? Font::Medium : Font::Regular);
            y += kSideItemH;
        }
    }

    void sidebar_click(int y) {
        int cy = 12;
        for (size_t i = 0; i < side_.size(); i++) {
            int hgt = side_[i].header ? kSideHeaderH : kSideItemH;
            if (y >= cy && y < cy + hgt) {
                if (!side_[i].header && !side_[i].path.empty()) navigate(side_[i].path, true);
                return;
            }
            cy += hgt;
        }
    }

    // ---------------------------------------------------------- painting
    void paint_title(Canvas& c) {
        const Theme& t = theme();
        int x0 = kSidebarW + 96;
        int x1 = view_->bounds.x - 12;
        if (x1 <= x0) return;
        String title = cwd_ == "/" ? String("KleinOS") : vfs::basename(cwd_.c_str());
        c.draw_text_in(Rect(x0, 10, x1 - x0, 30), title.c_str(), t.text, 15, Font::Bold);
    }

    void paint_icons(Canvas& c) {
        const Theme& t = theme();
        Rect m = main_rect();
        c.save();
        c.clip_to(m);
        int n = (int)entries_.size();
        for (int i = 0; i < n; i++) {
            Rect cell = item_rect(i);
            if (cell.bottom() < m.y || cell.y > m.bottom()) continue;
            bool sel = i == sel_;
            int label_w = text_width(entries_[i].name.c_str(), 13, Font::Medium) + 12;
            if (label_w > kCellW - 6) label_w = kCellW - 6;
            Rect icon(cell.x + (kCellW - kIconSize) / 2, cell.y + 6, kIconSize, kIconSize);
            if (sel) {
                c.fill_round_rect(R(cell.x + 8, cell.y + 2, kCellW - 16, kCellH - 6), 10,
                                  with_alpha(t.accent, t.dark ? 70 : 45));
            }
            draw_entry_icon(c, icon, entries_[i].kind);
            if (i == renaming_idx_) continue;
            Rect label(cell.x + (kCellW - label_w) / 2, cell.y + kIconSize + 10, label_w, 20);
            if (sel) c.fill_round_rect(label, 9, t.accent);
            c.draw_text_in(label.inset(4, 0), entries_[i].name.c_str(), sel ? t.accent_text : t.text, 13,
                           Font::Medium, Align::Center);
        }
        c.restore();
    }

    struct Columns { int name_x, name_w, size_x, kind_x, date_x; };
    Columns columns() const {
        Rect m = main_rect();
        Columns col;
        col.name_x = kSidebarW + 40;
        col.date_x = m.right() - 150;
        col.kind_x = col.date_x - 128;
        col.size_x = col.kind_x - 96;
        col.name_w = col.size_x - col.name_x - 12;
        return col;
    }

    void paint_list(Canvas& c) {
        const Theme& t = theme();
        Rect m = main_rect();
        Columns col = columns();
        // header
        c.fill_rect(Rect(m.x, m.y, m.w, kHeaderH), t.dark ? rgba(255, 255, 255, 10) : rgba(0, 0, 0, 8));
        c.fill_rect(Rect(m.x, m.y + kHeaderH - 1, m.w, 1), t.separator);
        c.draw_text_in(Rect(col.name_x, m.y, col.name_w, kHeaderH), "Name", t.text_secondary, 12, Font::Medium);
        c.draw_text_in(Rect(col.size_x, m.y, 92, kHeaderH), "Size", t.text_secondary, 12, Font::Medium, Align::Right);
        c.draw_text_in(Rect(col.kind_x, m.y, 120, kHeaderH), "Kind", t.text_secondary, 12, Font::Medium);
        c.draw_text_in(Rect(col.date_x, m.y, 150, kHeaderH), "Date Modified", t.text_secondary, 12, Font::Medium);

        c.save();
        c.clip_to(Rect(m.x, list_top(), m.w, m.bottom() - list_top()));
        int n = (int)entries_.size();
        for (int i = 0; i < n; i++) {
            Rect row = item_rect(i);
            if (row.bottom() < list_top() || row.y > m.bottom()) continue;
            bool sel = i == sel_;
            Rect band(m.x + 8, row.y + 1, m.w - 16, kRowH - 2);
            if (sel) c.fill_round_rect(band, 7, t.accent);
            Color fg = sel ? t.accent_text : t.text;
            Color sub = sel ? t.accent_text : t.text_secondary;
            draw_entry_icon(c, Rect(col.name_x - 26, row.y + 3, 20, 20), entries_[i].kind);
            if (i != renaming_idx_)
                c.draw_text_in(Rect(col.name_x, row.y, col.name_w, kRowH), entries_[i].name.c_str(), fg, 13, Font::Regular);
            char size[32] = "--";
            if (!entries_[i].is_dir) fmt_size(size, sizeof(size), entries_[i].size);
            c.draw_text_in(Rect(col.size_x, row.y, 92, kRowH), size, sub, 12, Font::Regular, Align::Right);
            c.draw_text_in(Rect(col.kind_x, row.y, 120, kRowH), kind_name(entries_[i].kind), sub, 12, Font::Regular);
            c.draw_text_in(Rect(col.date_x, row.y, 150, kRowH), date_text(entries_[i].mtime).c_str(), sub, 12, Font::Regular);
        }
        int content = n * kRowH;
        int view = m.bottom() - list_top();
        if (content > view) draw_scrollbar(c, m, content, view);
        c.restore();
    }

    void draw_scrollbar(Canvas& c, Rect m, int content, int view) {
        const Theme& t = theme();
        int bar_h = kmax(30, view * view / content);
        int max = content - view;
        int bar_y = list_top() + (view - bar_h) * scroll_y_ / (max > 0 ? max : 1);
        c.fill_round_rect(Rect(m.right() - 7, bar_y + 2, 5, bar_h - 4), 2.5f, t.dark ? rgba(255, 255, 255, 60) : rgba(0, 0, 0, 60));
    }

    void paint_footer(Canvas& c) {
        const Theme& t = theme();
        Rect m = main_rect();
        Rect bar(m.x, m.bottom(), m.w, kFooterH);
        char text[160];
        int n = (int)entries_.size();
        if (sel_ >= 0 && sel_ < n) {
            const Entry& e = entries_[sel_];
            char size[32] = "";
            if (!e.is_dir) fmt_size(size, sizeof(size), e.size);
            snprintf(text, sizeof(text), "%d items  \xc2\xb7  %s  %s", n, e.name.c_str(), size);
        } else {
            snprintf(text, sizeof(text), "%d item%s", n, n == 1 ? "" : "s");
        }
        c.draw_text_in(Rect(bar.x + 14, bar.y, bar.w - 28, bar.h), text, t.text_secondary, 12, Font::Regular, Align::Center);
    }

    // ---------------------------------------------------------- data
    static String home_path() { return String("/Users/") + session_user(); }

    static String date_text(uint64_t t) {
        if (t == 0) return String("--");
        DateTime dt = unix_to_datetime(t);
        return String::format("%04d-%02d-%02d %02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute);
    }

    void reload() {
        String keep = sel_ >= 0 && sel_ < (int)entries_.size() ? entries_[sel_].name : String();
        String query = search_->text.lower();
        entries_.clear();
        gen_ = vfs::generation();
        vfs::Node* d = vfs::lookup(cwd_.c_str());
        if (d && d->is_dir) {
            for (size_t i = 0; i < d->children.size(); i++) {
                vfs::Node* ch = d->children[i];
                if (ch->name.empty() || ch->name[0] == '.') continue;
                if (!query.empty() && ch->name.lower().find(query.c_str()) < 0) continue;
                Entry e;
                e.name = ch->name;
                e.path = cwd_ == "/" ? String("/") + ch->name : cwd_ + "/" + ch->name;
                e.is_dir = ch->is_dir;
                e.readonly = ch->readonly;
                e.size = ch->size;
                e.mtime = ch->mtime;
                e.kind = kind_for(ch->name, ch->is_dir);
                entries_.push(e);
            }
        }
        // folders first, then by name
        for (size_t i = 1; i < entries_.size(); i++) {
            Entry key = entries_[i];
            size_t j = i;
            while (j > 0) {
                const Entry& p = entries_[j - 1];
                bool before = (key.is_dir && !p.is_dir) || (key.is_dir == p.is_dir && name_cmp(key.name, p.name) < 0);
                if (!before) break;
                entries_[j] = entries_[j - 1];
                j--;
            }
            entries_[j] = key;
        }
        sel_ = -1;
        if (!keep.empty())
            for (size_t i = 0; i < entries_.size(); i++) if (entries_[i].name == keep) { sel_ = (int)i; break; }
        clamp_scroll();
        invalidate();
    }

    void navigate(const String& path, bool push) {
        if (!vfs::is_dir(path.c_str())) return;
        if (push && !cwd_.empty() && cwd_ != path) {
            back_stack_.push(cwd_);
            fwd_stack_.clear();
        }
        cwd_ = path;
        scroll_y_ = 0;
        set_title(cwd_ == "/" ? "KleinOS" : vfs::basename(cwd_.c_str()).c_str());
        update_nav();
        reload();
        select(-1);
    }

    void update_nav() {
        back_->enabled = !back_stack_.empty();
        fwd_->enabled = !fwd_stack_.empty();
    }

    void go_back() {
        if (back_stack_.empty()) return;
        String target = back_stack_.back();
        back_stack_.pop();
        fwd_stack_.push(cwd_);
        navigate_no_history(target);
    }
    void go_forward() {
        if (fwd_stack_.empty()) return;
        String target = fwd_stack_.back();
        fwd_stack_.pop();
        back_stack_.push(cwd_);
        navigate_no_history(target);
    }
    void navigate_no_history(const String& path) {
        if (!vfs::is_dir(path.c_str())) return;
        cwd_ = path;
        scroll_y_ = 0;
        set_title(cwd_ == "/" ? "KleinOS" : vfs::basename(cwd_.c_str()).c_str());
        update_nav();
        reload();
        select(-1);
    }

    void go_up() {
        if (cwd_ == "/") return;
        navigate(vfs::dirname(cwd_.c_str()), true);
    }

    void set_mode(ViewMode m) {
        mode_ = m;
        view_->selected = m == ViewMode::Icons ? 0 : 1;
        scroll_y_ = 0;
        clamp_scroll();
        invalidate();
    }

    void select(int i) {
        if (i >= (int)entries_.size()) i = -1;
        sel_ = i;
        if (i >= 0) ensure_visible(i);
        invalidate();
    }

    void move_sel(int d) {
        int n = (int)entries_.size();
        if (!n) return;
        int s = sel_ < 0 ? 0 : sel_ + d;
        if (sel_ < 0 && d < 0) s = 0;
        if (s < 0) s = 0;
        if (s >= n) s = n - 1;
        select(s);
    }

    void ensure_visible(int i) {
        Rect r = item_rect(i);
        int top = list_top();
        int view = main_rect().bottom() - top;
        if (r.y - top < 0) scroll_y_ += r.y - top;
        else if (r.bottom() - top > view) scroll_y_ += r.bottom() - top - view;
        clamp_scroll();
    }

    void open_index(int i) {
        if (i < 0 || i >= (int)entries_.size()) return;
        String path = entries_[i].path;
        if (entries_[i].is_dir) navigate(path, true);
        else app_open_file(path.c_str());
    }

    void new_folder() {
        commit_rename();
        String p = unique_path(cwd_ == "/" ? String("") : cwd_, String("untitled folder"), nullptr);
        if (!vfs::mkdir(p.c_str())) { wm::notify("Files", "Couldn't create the folder", "files"); return; }
        reload();
        for (size_t i = 0; i < entries_.size(); i++)
            if (entries_[i].path == p) { select((int)i); begin_rename((int)i); return; }
    }

    void show_info(int i) {
        if (i < 0 || i >= (int)entries_.size()) return;
        const Entry& e = entries_[i];
        char size[48] = "--";
        if (!e.is_dir) fmt_size(size, sizeof(size), e.size);
        String body = String::format("%s, %s\n%s\nModified %s", kind_name(e.kind), size, e.path.c_str(),
                                     date_text(e.mtime).c_str());
        wm::notify(e.name.c_str(), body.c_str(), "files");
    }

    void trash_index(int i) {
        if (i < 0 || i >= (int)entries_.size()) return;
        String path = entries_[i].path;
        String name = entries_[i].name;
        if (entries_[i].readonly) { wm::notify("Files", "This item is read-only", "files"); return; }
        String trash = home_path() + "/.Trash";
        if (!vfs::is_dir(trash.c_str())) vfs::mkdir(trash.c_str());
        String dest = unique_path(trash, name, nullptr);
        if (vfs::rename(path.c_str(), dest.c_str())) wm::notify("Moved to Trash", name.c_str(), "files");
        else wm::notify("Files", "Couldn't move the item to the Trash", "files");
        reload();
    }

    void delete_now(int i) {
        if (i < 0 || i >= (int)entries_.size()) return;
        String path = entries_[i].path;
        String name = entries_[i].name;
        if (entries_[i].readonly) { wm::notify("Files", "This item is read-only", "files"); return; }
        if (vfs::remove(path.c_str())) wm::notify("Deleted", name.c_str(), "files");
        else wm::notify("Files", "Couldn't delete the item", "files");
        reload();
    }

    void duplicate_index(int i) {
        if (i < 0 || i >= (int)entries_.size()) return;
        String path = entries_[i].path;
        String stem, ext;
        split_ext(entries_[i].name, stem, ext);
        String dest = unique_path(cwd_ == "/" ? String("") : cwd_, stem + ext, "copy");
        if (!vfs::copy(path.c_str(), dest.c_str())) wm::notify("Files", "Couldn't duplicate the item", "files");
        reload();
    }

    // ---------------------------------------------------------- rename
    void begin_rename(int i) {
        if (i < 0 || i >= (int)entries_.size()) return;
        commit_rename();
        ensure_visible(i);
        renaming_ = true;
        renaming_idx_ = i;
        rename_path_ = entries_[i].path;
        Rect r = item_rect(i);
        if (mode_ == ViewMode::Icons) {
            rename_->bounds = Rect(r.x + 2, r.y + kIconSize + 9, kCellW - 4, 22);
        } else {
            Columns col = columns();
            rename_->bounds = Rect(col.name_x - 4, r.y + 1, col.name_w + 8, kRowH - 2);
        }
        rename_->set_text(entries_[i].name.c_str());
        rename_->select_all();
        rename_->visible = true;
        focus_widget(rename_);
        invalidate();
    }

    void cancel_rename() {
        renaming_ = false;
        renaming_idx_ = -1;
        rename_->visible = false;
        focus_widget(nullptr);
        invalidate();
    }

    void commit_rename() {
        if (!renaming_) return;
        String text = rename_->text.trim();
        String old = rename_path_;
        cancel_rename();
        if (text.empty()) return;
        String old_name = vfs::basename(old.c_str());
        if (text == old_name) return;
        if (text.find('/') >= 0) { wm::notify("Files", "Names can't contain \"/\"", "files"); return; }
        String dest = vfs::dirname(old.c_str()) == "/" ? String("/") + text : vfs::dirname(old.c_str()) + "/" + text;
        if (vfs::exists(dest.c_str())) { wm::notify("Files", "An item with that name already exists", "files"); return; }
        if (!vfs::rename(old.c_str(), dest.c_str())) { wm::notify("Files", "Couldn't rename the item", "files"); return; }
        reload();
        for (size_t i = 0; i < entries_.size(); i++)
            if (entries_[i].path == dest) { select((int)i); break; }
    }

    // ---------------------------------------------------------- menus
    void context_menu(int sx, int sy, int idx) {
        Vector<MenuItem> items;
        if (idx >= 0) {
            String path = entries_[idx].path;
            FilesWindow* self = this;
            auto add = [&](const char* title, Func<void()> fn, bool sep_after = false) {
                MenuItem& m = items.emplace();
                m.title = title;
                m.action = fn;
                if (sep_after) { MenuItem& s = items.emplace(); s.title = "-"; }
            };
            add("Open", [self, idx] { self->open_index(idx); });
            add("Rename", [self, idx] { self->begin_rename(idx); });
            add("Duplicate", [self, idx] { self->duplicate_index(idx); }, true);
            add("Move to Trash", [self, idx] { self->trash_index(idx); });
            add("Delete Immediately", [self, idx] { self->delete_now(idx); }, true);
            add("Get Info", [self, idx] { self->show_info(idx); }, true);
            add("New Folder", [self] { self->new_folder(); });
        } else {
            FilesWindow* self = this;
            MenuItem& m = items.emplace();
            m.title = "New Folder";
            m.action = [self] { self->new_folder(); };
        }
        wm::popup_menu(sx, sy, items);
    }

    Widget* widget_at(int x, int y) {
        for (size_t i = widgets().size(); i > 0; i--) {
            Widget* w = widgets()[i - 1];
            if (w->visible && w->bounds.contains(x, y)) return w;
        }
        return nullptr;
    }

    // ---------------------------------------------------------- state
    String cwd_;
    Vector<Entry> entries_;
    Vector<SideItem> side_;
    Vector<String> back_stack_, fwd_stack_;
    ViewMode mode_ = ViewMode::Icons;
    int sel_ = -1;
    int scroll_y_ = 0;
    uint64_t gen_ = 0;
    bool renaming_ = false;
    int renaming_idx_ = -1;
    String rename_path_;

    Button* back_ = nullptr;
    Button* fwd_ = nullptr;
    Button* new_btn_ = nullptr;
    SegmentedControl* view_ = nullptr;
    TextField* search_ = nullptr;
    TextField* rename_ = nullptr;
};

void open_path_handler(const char* path) {
    FilesWindow* target = nullptr;
    Window* active = wm::active_window();
    if (active && strcmp(active->app_id(), "files") == 0) target = static_cast<FilesWindow*>(active);
    if (!target) {
        const Vector<Window*>& ws = wm::windows();
        for (size_t i = ws.size(); i > 0 && !target; i--)
            if (strcmp(ws[i - 1]->app_id(), "files") == 0) target = static_cast<FilesWindow*>(ws[i - 1]);
    }
    if (!target) return;
    String p = path;
    if (!vfs::is_dir(path)) p = vfs::dirname(path);
    target->navigate_to(p);
}

struct OpenHandlerRegistration {
    OpenHandlerRegistration() { register_open_handler("files", open_path_handler); }
} open_handler_registration;

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x3FD0C4), hex(0x0B7F86));
    float u = r.w / 96.0f, x = (float)r.x, y = (float)r.y;
    // back card and front folder
    c.fill_round_rect(R(x + 22 * u, y + 30 * u, 52 * u, 40 * u), 6 * u, rgba(255, 255, 255, 90));
    c.fill_round_rect(R(x + 16 * u, y + 40 * u, 64 * u, 40 * u), 8 * u, hex(0xFFFFFF));
    c.fill_round_rect(R(x + 16 * u, y + 34 * u, 28 * u, 10 * u), 4 * u, hex(0xE4F6F4));
    c.fill_gradient(R(x + 16 * u, y + 40 * u, 64 * u, 40 * u), hex(0xFFFFFF), hex(0xD5EFEC), 8 * u);
    c.draw_line(x + 26 * u, y + 52 * u, x + 70 * u, y + 52 * u, hex(0x2BA49C), 3 * u);
    c.draw_line(x + 26 * u, y + 62 * u, x + 58 * u, y + 62 * u, hex(0x2BA49C), 3 * u);
}

Window* create() { return new FilesWindow(); }

AppRegistrar reg({"files", "Files", icon, create, "System", false, true, 10, "Browse and organize your files"});

}  // namespace
