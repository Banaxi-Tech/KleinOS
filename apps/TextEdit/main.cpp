// TextEdit: plain-text editor. Full-window TextArea, a toolbar, and an in-window
// path bar for Open / Save (with recent files from ~/Documents).
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "fs/vfs.h"
#include "sys/session.h"
#include "lib/math.h"

namespace {

constexpr int kToolbarH = 46;
constexpr int kPathH = 44;
constexpr int kStatusH = 26;
constexpr int kMinFont = 10;
constexpr int kMaxFont = 36;
constexpr int kDefaultFont = 15;
constexpr int kRecentMax = 20;

const char* kWelcome =
    "Welcome to TextEdit\n"
    "\n"
    "A plain-text editor for KleinOS.\n"
    "\n"
    "  Cmd+N   new window\n"
    "  Cmd+O   open a file\n"
    "  Cmd+S   save\n"
    "  Cmd+=   bigger text, Cmd+-   smaller\n";

String docs_dir() { return String::format("/Users/%s/Documents", session_user()); }

class TextEditWindow : public Window {
public:
    TextEditWindow() : Window("TextEdit", 640, 460) {
        set_min_size(520, 320);

        area_ = (TextArea*)add(new TextArea(Rect(0, 0, 10, 10)));
        area_->font_size = font_size_;
        area_->font = Font::Regular;
        area_->on_change = [this] { status_msg_.clear(); set_dirty(true); };

        btn_new_ = (Button*)add(new Button(Rect(), "New"));
        btn_new_->icon = [](Canvas& c, Rect r) { draw_symbol(c, r, Symbol::File, theme().accent, 1.6f); };
        btn_new_->on_click = [] { app_launch("textedit"); };

        btn_open_ = (Button*)add(new Button(Rect(), "Open"));
        btn_open_->icon = [](Canvas& c, Rect r) { draw_symbol(c, r, Symbol::Folder, theme().accent, 1.6f); };
        btn_open_->on_click = [this] { open_path_bar(false); };

        btn_save_ = (Button*)add(new Button(Rect(), "Save"));
        btn_save_->icon = [](Canvas& c, Rect r) { draw_symbol(c, r, Symbol::Download, theme().accent, 1.6f); };
        btn_save_->on_click = [this] { save(); };

        btn_smaller_ = (Button*)add(new Button(Rect(), ""));
        btn_smaller_->icon = [](Canvas& c, Rect r) { draw_symbol(c, r, Symbol::Minus, theme().text, 1.8f); };
        btn_smaller_->on_click = [this] { set_font_size(font_size_ - 1); };

        btn_bigger_ = (Button*)add(new Button(Rect(), ""));
        btn_bigger_->icon = [](Canvas& c, Rect r) { draw_symbol(c, r, Symbol::Plus, theme().text, 1.8f); };
        btn_bigger_->on_click = [this] { set_font_size(font_size_ + 1); };

        mono_toggle_ = (Toggle*)add(new Toggle(Rect(), false));
        mono_toggle_->on_change = [this](bool on) { set_mono(on); };

        path_field_ = (TextField*)add(new TextField(Rect(), "Path or file name"));
        path_field_->font_size = 13;
        path_field_->visible = false;
        path_field_->on_submit = [this] { confirm_path(); };

        recent_ = (Dropdown*)add(new Dropdown(Rect()));
        recent_->visible = false;
        recent_->on_change = [this](int i) {
            if (i <= 0 || i - 1 >= (int)recent_paths_.size()) return;
            path_field_->set_text(recent_paths_[i - 1].c_str());
            invalidate();
        };

        cancel_ = (Button*)add(new Button(Rect(), "Cancel"));
        cancel_->visible = false;
        cancel_->on_click = [this] { close_path_bar(); };

        confirm_ = (Button*)add(new Button(Rect(), "Open", Button::Primary));
        confirm_->visible = false;
        confirm_->on_click = [this] { confirm_path(); };

        focus_widget(area_);
        layout();
        load_welcome();
    }

    void on_paint(Canvas& c) override {
        const Theme& t = theme();
        int W = width(), H = height();
        c.clear(t.window_bg);
        c.fill_rect(Rect(0, kToolbarH - 1, W, 1), t.separator);
        if (pathbar_open_) {
            c.fill_rect(Rect(0, kToolbarH, W, kPathH), t.dark ? rgba(255, 255, 255, 12) : rgba(0, 0, 0, 7));
            c.fill_rect(Rect(0, kToolbarH + kPathH - 1, W, 1), t.separator);
        }
        // widgets (same order as Window::on_paint)
        for (Widget* w : widgets())
            if (w->visible) {
                c.save();
                w->paint(c);
                c.restore();
            }

        // toolbar labels
        char size_buf[16];
        snprintf(size_buf, sizeof(size_buf), "%d pt", font_size_);
        c.draw_text_in(size_rect(), size_buf, t.text, 13, Font::Medium, Align::Center);
        int mx = mono_toggle_->bounds.right() + 8;
        c.draw_text_in(Rect(mx, 0, 80, kToolbarH), "Monospace", t.text_secondary, 13, Font::Regular);

        if (pathbar_open_) {
            c.draw_text_in(Rect(12, kToolbarH, 64, kPathH), save_mode_ ? "Save as" : "Open", t.text_secondary, 13, Font::Medium);
        }

        // status bar
        int sy = H - kStatusH;
        c.fill_rect(Rect(0, sy, W, 1), t.separator);
        c.fill_rect(Rect(0, sy + 1, W, kStatusH - 1), t.dark ? rgba(255, 255, 255, 6) : rgba(0, 0, 0, 3));
        if (!status_msg_.empty()) {
            c.draw_text_in(Rect(12, sy, W - 24, kStatusH), status_msg_.c_str(), t.text_secondary, 12);
        } else {
            int words = 0;
            long chars = 0;
            bool in_word = false;
            const String& s = area_->get_text();
            for (size_t i = 0; i < s.size(); i++) {
                unsigned char ch = (unsigned char)s[i];
                if ((ch & 0xC0) != 0x80) chars++;
                bool space = ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r';
                if (!space && !in_word) { words++; in_word = true; }
                else if (space) in_word = false;
            }
            char info[96];
            snprintf(info, sizeof(info), "%d words  \xC2\xB7  %ld characters", words, chars);
            c.draw_text_in(Rect(12, sy, W - 24, kStatusH), info, t.text_secondary, 12);
        }
        const char* kind = mono_ ? "Monospace  \xC2\xB7  UTF-8" : "Plain Text  \xC2\xB7  UTF-8";
        c.draw_text_in(Rect(12, sy, W - 24, kStatusH), kind, t.text_secondary, 12, Font::Regular, Align::Right);
    }

    void on_resize() override { layout(); }

    void on_key(const KeyEvent& e) override {
        if (e.key == Key::Escape && pathbar_open_) { close_path_bar(); return; }
        if (e.key == Key::Char && e.cmd()) {
            uint32_t ch = e.ch | 0x20;
            if (ch == 'n' && !e.shift()) { app_launch("textedit"); return; }
            if (ch == 'o' && !e.shift()) { open_path_bar(false); return; }
            if (ch == 's') {
                if (e.shift()) open_path_bar(true);
                else save();
                return;
            }
            if (e.ch == '=' || e.ch == '+') { set_font_size(font_size_ + 1); return; }
            if (e.ch == '-') { set_font_size(font_size_ - 1); return; }
        }
        Window::on_key(e);
    }

    void build_menus(Vector<Menu>& menus) override {
        Menu& f = menus.emplace();
        f.title = "File";
        f.add("New", [] { app_launch("textedit"); }, "\xE2\x8C\x98N");
        f.add("Open\xE2\x80\xA6", [this] { open_path_bar(false); }, "\xE2\x8C\x98O");
        f.separator();
        f.add("Save", [this] { save(); }, "\xE2\x8C\x98S");
        f.add("Save As\xE2\x80\xA6", [this] { open_path_bar(true); }, "\xE2\x87\xA7\xE2\x8C\x98S");

        Menu& fm = menus.emplace();
        fm.title = "Format";
        fm.add("Bigger", [this] { set_font_size(font_size_ + 1); }, "\xE2\x8C\x98=");
        fm.add("Smaller", [this] { set_font_size(font_size_ - 1); }, "\xE2\x8C\x98-");
        fm.separator();
        fm.add("Monospace", [this] { set_mono(!mono_); });
        fm.items[fm.items.size() - 1].checked = mono_;
    }

    // Replace the document with a file's contents.
    void load(const char* p) {
        String text;
        if (!vfs::read_text(p, text)) {
            status_msg_ = "Couldn't open that file";
            invalidate();
            return;
        }
        path_ = p;
        area_->set_text(text.c_str());
        area_->cursor = 0;
        status_msg_.clear();
        set_dirty(false);
        close_path_bar();
    }

    bool save() {
        if (path_.empty()) {
            open_path_bar(true);
            return false;
        }
        if (!vfs::write_text(path_.c_str(), area_->get_text().c_str())) {
            status_msg_ = "Couldn't save the file";
            invalidate();
            return false;
        }
        status_msg_ = "Saved";
        set_dirty(false);
        return true;
    }

    bool dirty() const { return dirty_; }

private:
    Rect size_rect() const { return Rect(btn_smaller_->bounds.right(), 0, 50, kToolbarH); }

    void layout() {
        int W = width(), H = height();
        int top = kToolbarH + (pathbar_open_ ? kPathH : 0);
        area_->bounds = Rect(0, top, W, H - top - kStatusH);

        int y = (kToolbarH - 28) / 2;
        int x = 12;
        btn_new_->bounds = Rect(x, y, 72, 28);   x += 72 + 6;
        btn_open_->bounds = Rect(x, y, 78, 28);  x += 78 + 6;
        btn_save_->bounds = Rect(x, y, 78, 28);  x += 78 + 22;
        btn_smaller_->bounds = Rect(x, y, 28, 28);
        x += 28 + 50;                             // size label sits here
        btn_bigger_->bounds = Rect(x, y, 28, 28);
        x += 28 + 22;
        mono_toggle_->bounds = Rect(x, y, 50, 28);

        if (pathbar_open_) {
            int py = kToolbarH + (kPathH - 28) / 2;
            cancel_->bounds = Rect(W - 12 - 76, py, 76, 28);
            confirm_->bounds = Rect(cancel_->bounds.x - 6 - 76, py, 76, 28);
            recent_->bounds = Rect(confirm_->bounds.x - 8 - 170, py, 170, 28);
            path_field_->bounds = Rect(84, py, recent_->bounds.x - 8 - 84, 28);
        }
    }

    void set_dirty(bool d) {
        dirty_ = d;
        update_title();
        invalidate();
    }

    void update_title() {
        String name = path_.empty() ? String("Untitled") : vfs::basename(path_.c_str());
        if (dirty_) name += " \xE2\x80\x94 Edited";
        set_title(name.c_str());
    }

    void set_font_size(int s) {
        font_size_ = kclamp(s, kMinFont, kMaxFont);
        area_->font_size = font_size_;
        invalidate();
    }

    void set_mono(bool m) {
        mono_ = m;
        area_->font = m ? Font::Mono : Font::Regular;
        mono_toggle_->on = m;
        invalidate();
    }

    void load_welcome() {
        String welcome = String::format("%s/Welcome.txt", docs_dir().c_str());
        if (!vfs::exists(welcome.c_str())) vfs::write_text(welcome.c_str(), kWelcome);
        load(welcome.c_str());
        set_dirty(false);
    }

    void refresh_recent() {
        recent_paths_.clear();
        recent_->options.clear();
        recent_->options.push(String("Recent Files"));
        recent_->selected = 0;
        String dir = docs_dir();
        vfs::Node* d = vfs::lookup(dir.c_str());
        if (!d) return;
        Vector<vfs::Node*> files;
        for (vfs::Node* n : d->children)
            if (!n->is_dir) files.push(n);
        // newest first (insertion sort, lists are small)
        for (size_t i = 1; i < files.size(); i++) {
            vfs::Node* x = files[i];
            size_t j = i;
            while (j > 0 && files[j - 1]->mtime < x->mtime) { files[j] = files[j - 1]; j--; }
            files[j] = x;
        }
        for (size_t i = 0; i < files.size() && (int)i < kRecentMax; i++) {
            recent_paths_.push(vfs::join(dir.c_str(), files[i]->name.c_str()));
            recent_->options.push(files[i]->name);
        }
    }

    void open_path_bar(bool save_mode) {
        save_mode_ = save_mode;
        pathbar_open_ = true;
        String def;
        if (!path_.empty()) def = path_;
        else def = String::format(save_mode ? "%s/Untitled.txt" : "%s/", docs_dir().c_str());
        path_field_->set_text(def.c_str());
        confirm_->text = save_mode ? "Save" : "Open";
        refresh_recent();
        path_field_->visible = true;
        recent_->visible = true;
        confirm_->visible = true;
        cancel_->visible = true;
        layout();
        focus_widget(path_field_);
        invalidate();
    }

    void close_path_bar() {
        if (!pathbar_open_) return;
        pathbar_open_ = false;
        path_field_->visible = false;
        recent_->visible = false;
        confirm_->visible = false;
        cancel_->visible = false;
        layout();
        focus_widget(area_);
        invalidate();
    }

    void confirm_path() {
        String p = path_field_->get_text();
        p = p.trim();
        if (p.empty()) { status_msg_ = "Enter a file path"; invalidate(); return; }
        if (p[0] != '/') p = String::format("%s/%s", docs_dir().c_str(), p.c_str());

        if (save_mode_) {
            String old = path_;
            path_ = p;
            if (!save()) { path_ = old; return; }
            update_title();
            close_path_bar();
            return;
        }
        if (!vfs::exists(p.c_str()) || vfs::is_dir(p.c_str())) {
            status_msg_ = "No file at that path";
            invalidate();
            return;
        }
        if (dirty_) {
            // don't throw away unsaved edits: open the file in a fresh window
            Window* w = app_launch("textedit");
            if (w && strcmp(w->app_id(), "textedit") == 0) ((TextEditWindow*)w)->load(p.c_str());
            close_path_bar();
            return;
        }
        load(p.c_str());
    }

    TextArea* area_ = nullptr;
    Button* btn_new_ = nullptr;
    Button* btn_open_ = nullptr;
    Button* btn_save_ = nullptr;
    Button* btn_smaller_ = nullptr;
    Button* btn_bigger_ = nullptr;
    Toggle* mono_toggle_ = nullptr;
    TextField* path_field_ = nullptr;
    Dropdown* recent_ = nullptr;
    Button* cancel_ = nullptr;
    Button* confirm_ = nullptr;
    String path_;                  // empty = untitled
    String status_msg_;
    Vector<String> recent_paths_;
    int font_size_ = kDefaultFont;
    bool mono_ = false;
    bool dirty_ = false;
    bool pathbar_open_ = false;
    bool save_mode_ = false;
};

// Most recent TextEdit window (the active one if it is TextEdit).
TextEditWindow* find_recent() {
    Window* act = wm::active_window();
    if (act && !act->closing_ && strcmp(act->app_id(), "textedit") == 0) return (TextEditWindow*)act;
    const Vector<Window*>& ws = wm::windows();
    for (size_t i = ws.size(); i > 0; i--) {
        Window* w = ws[i - 1];
        if (!w->closing_ && strcmp(w->app_id(), "textedit") == 0) return (TextEditWindow*)w;
    }
    return nullptr;
}

void open_file_handler(const char* path) {
    TextEditWindow* w = find_recent();
    if (!w || w->dirty()) {
        Window* nw = app_launch("textedit");
        if (!nw || strcmp(nw->app_id(), "textedit") != 0) return;
        w = (TextEditWindow*)nw;
    }
    w->load(path);
    w->bring_to_front();
}

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x5AB0FF), hex(0x1F5FD6));
    // page with folded corner
    int pw = r.w * 11 / 20, ph = r.h * 13 / 20;
    int px = r.x + (r.w - pw) / 2, py = r.y + (r.h - ph) / 2;
    Rect page(px, py, pw, ph);
    c.draw_shadow(page, 3, r.w / 20.0f, rgba(0, 0, 40, 70), 0, r.w / 40);
    c.fill_round_rect(page, r.w / 28.0f, hex(0xFFFFFF));
    float fold = r.w / 7.0f;
    PointF tri[3] = {{(float)(px + pw - fold), (float)py}, {(float)(px + pw), (float)(py + fold)}, {(float)(px + pw - fold), (float)(py + fold)}};
    c.fill_polygon(tri, 3, hex(0xC9D9F5));
    // text lines
    float lw = r.w / 34.0f;
    for (int i = 0; i < 4; i++) {
        int ly = py + ph * (i + 2) / 8 + (i > 0 ? 0 : 0);
        int len = (i == 3) ? pw * 5 / 10 : pw * 7 / 10;
        c.fill_rect(Rect(px + pw / 8, ly, len, (int)(lw + 0.5f) + 1), hex(0x1F5FD6));
    }
}

Window* create() { return new TextEditWindow(); }

struct OpenHandlerRegistrar {
    OpenHandlerRegistrar() { register_open_handler("textedit", open_file_handler); }
};
OpenHandlerRegistrar open_handler_reg;

AppRegistrar reg({"textedit", "TextEdit", icon, create, "Productivity", false, true, 30, "Write and edit plain text documents"});

}  // namespace
