#include "FileDialog/file_dialog.h"
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "drivers/framebuffer.h"
#include "drivers/rtc.h"
#include "fs/vfs.h"
#include "lib/math.h"
#include "lib/string.h"
#include "sys/session.h"

namespace file_dialog {
namespace {

constexpr int kBarY = 10, kBarH = 30, kToolY = 48, kListTop = 84, kFootH = 92;
constexpr int kSideW = 172, kHeadH = 26, kRowH = 24, kScrollW = 10;
constexpr int kDateW = 140, kTypeW = 110, kSizeW = 76;
enum Column { ColName, ColDate, ColType, ColSize };

struct Entry {
    String name;
    bool dir;
    size_t size;
    uint64_t mtime;
};
struct Place {
    String label, path;
    Symbol symbol;
};
struct Crumb {
    Rect rect;
    String path;
};

String extension_upper(const String& name) {
    String e = vfs::extension(name.c_str());
    char buf[32];
    size_t n = kmin<size_t>(e.size(), sizeof(buf) - 1);
    for (size_t i = 0; i < n; i++) {
        char ch = e.c_str()[i];
        buf[i] = ch >= 'a' && ch <= 'z' ? (char)(ch - 32) : ch;
    }
    buf[n] = 0;
    return String(buf);
}

bool extension_in(const char* list, const String& ext) {
    if (!*list) return true;
    size_t n = ext.size();
    if (!n) return false;
    for (const char* p = list; *p;) {
        const char* e = strchr(p, ';');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !memcmp(p, ext.c_str(), n)) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

// Default size, shrunk to stay clear of the menu bar and dock on small screens.
int fit_width() { return kmin(780, (int)framebuffer().width - 40); }
int fit_height() { return kclamp((int)framebuffer().height - 170, 380, 500); }

class DialogWindow;
// heap allocated: a .dl has no __dso_handle for a global with a destructor
Vector<DialogWindow*>& dialogs() {
    static Vector<DialogWindow*>* list = new Vector<DialogWindow*>();
    return *list;
}

class DialogWindow : public Window {
public:
    DialogWindow(bool saving, Window* owner, const Options& o, Done done)
        : Window(o.title ? o.title : (saving ? "Save As" : "Open"), fit_width(), fit_height(), WF_RESIZABLE | WF_NO_MINIMIZE),
          saving_(saving), owner_(owner), done_(done) {
        set_min_size(600, 380);
        if (o.default_extension) default_ext_ = o.default_extension;
        for (int i = 0; i < o.filter_count; i++) {
            filter_labels_.push(String(o.filters[i].label));
            filter_exts_.push(String(o.filters[i].extensions));
        }
        if (filter_labels_.empty()) {
            filter_labels_.push(String("All files"));
            filter_exts_.push(String(""));
        }

        search_ = (TextField*)add(new TextField(Rect(0, kBarY, 180, kBarH), "Search"));
        search_->rounded = true;
        search_->search_icon = true;
        search_->on_change = [this] { reload(); };
        new_folder_ = (Button*)add(new Button(Rect(12, kToolY, 110, 28), "New Folder"));
        new_folder_->on_click = [this] { make_folder(); };
        name_ = (TextField*)add(new TextField(Rect(0, 0, 100, kControlHeight), ""));
        name_->set_text(o.file_name);
        name_->on_submit = [this] { accept(); };
        filter_ = (Dropdown*)add(new Dropdown(Rect(0, 0, 200, kControlHeight)));
        for (size_t i = 0; i < filter_labels_.size(); i++) {
            String label = filter_labels_[i];
            if (filter_exts_[i].size()) label = label + " (" + patterns(filter_exts_[i]) + ")";
            filter_->options.push(label);
        }
        filter_->on_change = [this](int) { reload(); };
        ok_ = (Button*)add(new Button(Rect(0, 0, 90, kControlHeight), saving ? "Save" : "Open", Button::Primary));
        ok_->on_click = [this] { accept(); };
        cancel_ = (Button*)add(new Button(Rect(0, 0, 90, kControlHeight), "Cancel"));
        cancel_->on_click = [this] { dismiss(); };

        build_places();
        String start = o.directory ? String(o.directory) : home() + "/Documents";
        if (!vfs::is_dir(start.c_str())) start = home();
        if (!vfs::is_dir(start.c_str())) start = "/";
        path_ = start;
        gen_ = vfs::generation();
        layout();
        reload();
        focus_widget(name_);
        center();
        set_ticking(true);
        dialogs().push(this);
    }

    ~DialogWindow() override {
        dialogs().remove_value(this);
    }

    Window* owner() const { return owner_; }
    void cancel_silently() { done_ = Done(); close(); }

    void on_resize() override { layout(); reload(); }

    void on_tick(uint64_t) override {
        if (vfs::generation() != gen_) {
            gen_ = vfs::generation();
            reload();
        }
    }

    void on_paint(Canvas& c) override {
        const Theme& t = theme();
        c.clear(t.window_bg);
        paint_nav(c);
        paint_side(c);
        paint_list(c);
        c.fill_rect(Rect(0, height() - kFootH, width(), 1), t.separator);
        c.draw_text_in(Rect(kSideW - 70, name_->bounds.y, 80, kControlHeight), "File name:", t.text, 13, Font::Regular, Align::Right);
        if (status_.size())
            c.draw_text_in(Rect(14, ok_->bounds.y, ok_->bounds.x - 28, kControlHeight), status_.c_str(), status_error_ ? t.destructive : t.text_secondary, 13);
        paint_widgets(c);
    }

    void on_mouse_down(int x, int y, int button) override {
        if (button == MB_LEFT && press(x, y)) { invalidate(); return; }
        Window::on_mouse_down(x, y, button);
    }

    void on_mouse_move(int x, int y, int buttons) override {
        if (dragging_thumb_ && (buttons & MB_LEFT)) {
            int track = list_rows_rect().h - thumb_h();
            int range = max_scroll();
            if (track > 0) set_scroll(drag_scroll_ + (y - drag_y_) * range / track);
            return;
        }
        dragging_thumb_ = false;
        int h = list_rows_rect().contains(x, y) ? row_at(y) : -1;
        if (h != hover_) { hover_ = h; invalidate(); }
        Window::on_mouse_move(x, y, buttons);
    }

    void on_mouse_up(int x, int y, int button) override {
        dragging_thumb_ = false;
        Window::on_mouse_up(x, y, button);
    }

    void on_double_click(int x, int y) override {
        if (!list_rows_rect().contains(x, y)) return;
        int row = row_at(y);
        if (row >= 0) activate(row);
    }

    void on_scroll(int x, int y, int delta) override {
        if (list_rows_rect().contains(x, y)) set_scroll(scroll_ + delta * kRowH);
        else Window::on_scroll(x, y, delta);
    }

    void on_key(const KeyEvent& e) override {
        if (e.key == Key::Escape) { dismiss(); return; }
        if (e.key == Key::Up || e.key == Key::Down) {
            int n = (int)order_.size();
            if (n) {
                int s = selected_ < 0 ? 0 : kclamp(selected_ + (e.key == Key::Down ? 1 : -1), 0, n - 1);
                select_row(s);
                reveal(s);
            }
            return;
        }
        Window::on_key(e);
    }

private:
    String home() const { return String("/Users/") + session_user(); }

    static String patterns(const String& exts) {
        String out;
        const char* p = exts.c_str();
        while (*p) {
            const char* e = strchr(p, ';');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (out.size()) out = out + ";";
            out = out + "*." + String(p).substr(0, len);
            if (!e) break;
            p = e + 1;
        }
        return out;
    }

    void build_places() {
        struct Known { const char* label; const char* sub; Symbol symbol; };
        const Known known[] = {{"Home", "", Symbol::Home}, {"Desktop", "/Desktop", Symbol::Display}, {"Documents", "/Documents", Symbol::File},
                               {"Downloads", "/Downloads", Symbol::Download}, {"Pictures", "/Pictures", Symbol::Image}, {"Music", "/Music", Symbol::Music}};
        for (const Known& k : known) {
            String p = home() + k.sub;
            if (vfs::is_dir(p.c_str())) places_.push(Place{String(k.label), p, k.symbol});
        }
        places_.push(Place{String("Computer"), String("/"), Symbol::Cpu});
    }

    void layout() {
        int W = width(), H = height();
        search_->bounds = Rect(W - 192, kBarY, 180, kBarH);
        int fy = H - kFootH + 12;
        filter_->bounds = Rect(W - 262, fy, 250, kControlHeight);
        name_->bounds = Rect(kSideW + 16, fy, W - 262 - 10 - (kSideW + 16), kControlHeight);
        int by = fy + kControlHeight + 8;
        cancel_->bounds = Rect(W - 102, by, 90, kControlHeight);
        ok_->bounds = Rect(W - 200, by, 90, kControlHeight);
        invalidate();
    }

    Rect list_rect() const { return Rect(kSideW, kListTop, width() - kSideW, height() - kFootH - kListTop); }
    Rect list_rows_rect() const {
        Rect l = list_rect();
        return Rect(l.x, l.y + kHeadH, l.w - kScrollW, l.h - kHeadH);
    }
    Rect nav_button(int i) const { return Rect(12 + i * 34, kBarY, 30, kBarH); }
    Rect path_rect() const { return Rect(118, kBarY, width() - 192 - 10 - 118 - 12 + 0, kBarH); }
    int content_h() const { return (int)order_.size() * kRowH; }
    int max_scroll() const { return kmax(0, content_h() - list_rows_rect().h); }
    int thumb_h() const {
        int view = list_rows_rect().h, total = kmax(1, content_h());
        return kclamp(view * view / total, 24, view);
    }
    Rect thumb_rect() const {
        Rect r = list_rows_rect();
        int range = max_scroll();
        int y = r.y + (range ? scroll_ * (r.h - thumb_h()) / range : 0);
        return Rect(r.right() + 1, y, kScrollW - 2, thumb_h());
    }
    int row_at(int y) const {
        int row = (y - list_rows_rect().y + scroll_) / kRowH;
        return row >= 0 && row < (int)order_.size() ? row : -1;
    }
    int col_x(int col) const {
        Rect r = list_rows_rect();
        int size_x = r.right() - kSizeW, type_x = size_x - kTypeW, date_x = type_x - kDateW;
        return col == ColSize ? size_x : col == ColType ? type_x : col == ColDate ? date_x : r.x + 10;
    }

    void set_scroll(int s) {
        s = kclamp(s, 0, max_scroll());
        if (s != scroll_) { scroll_ = s; invalidate(); }
    }
    void reveal(int row) {
        Rect r = list_rows_rect();
        if (row * kRowH < scroll_) set_scroll(row * kRowH);
        else if ((row + 1) * kRowH > scroll_ + r.h) set_scroll((row + 1) * kRowH - r.h);
    }
    void set_status(const char* s, bool error) {
        status_ = s;
        status_error_ = error;
        confirm_path_ = "";
        invalidate();
    }

    bool sort_before(const Entry& a, const Entry& b) const {
        if (a.dir != b.dir) return a.dir;
        int c = 0;
        if (sort_ == ColDate) c = a.mtime < b.mtime ? -1 : a.mtime > b.mtime;
        else if (sort_ == ColSize) c = a.size < b.size ? -1 : a.size > b.size;
        else if (sort_ == ColType) c = strcmp(extension_upper(a.name).c_str(), extension_upper(b.name).c_str());
        if (!c) c = strcmp(a.name.lower().c_str(), b.name.lower().c_str());
        return descending_ ? c > 0 : c < 0;
    }

    void reload() {
        String keep = selected_ >= 0 && selected_ < (int)order_.size() ? entries_[order_[selected_]].name : String("");
        entries_.clear();
        order_.clear();
        vfs::Node* dir = vfs::lookup(path_.c_str());
        if (!dir || !dir->is_dir) {
            path_ = "/";
            dir = vfs::root();
        }
        int fi = kclamp(filter_->selected, 0, (int)filter_exts_.size() - 1);
        const char* exts = filter_exts_[fi].c_str();
        String query = String(search_->get_text()).lower();
        for (vfs::Node* n : dir->children) {
            if (!n->is_dir && !extension_in(exts, vfs::extension(n->name.c_str()))) continue;
            if (query.size() && !strstr(n->name.lower().c_str(), query.c_str())) continue;
            entries_.push(Entry{n->name, n->is_dir, n->size, n->mtime});
        }
        for (int i = 0; i < (int)entries_.size(); i++) {
            int at = (int)order_.size();
            order_.push(i);
            while (at > 0 && sort_before(entries_[order_[at]], entries_[order_[at - 1]])) {
                int t = order_[at]; order_[at] = order_[at - 1]; order_[at - 1] = t;
                at--;
            }
        }
        selected_ = -1;
        for (int i = 0; i < (int)order_.size(); i++)
            if (keep.size() && entries_[order_[i]].name == keep) selected_ = i;
        hover_ = -1;
        scroll_ = kclamp(scroll_, 0, max_scroll());
        invalidate();
    }

    void go(const String& path, bool record = true) {
        if (!vfs::is_dir(path.c_str()) || path == path_) return;
        if (record) {
            back_.push(path_);
            forward_.clear();
        }
        path_ = path;
        selected_ = -1;
        scroll_ = 0;
        search_->set_text("");
        set_status("", false);
        reload();
    }
    void go_back() {
        if (back_.empty()) return;
        forward_.push(path_);
        String p = back_[back_.size() - 1];
        back_.remove(back_.size() - 1);
        go(p, false);
    }
    void go_forward() {
        if (forward_.empty()) return;
        back_.push(path_);
        String p = forward_[forward_.size() - 1];
        forward_.remove(forward_.size() - 1);
        go(p, false);
    }
    void go_up() {
        if (path_ != "/") go(vfs::dirname(path_.c_str()));
    }

    String child_path(const String& name) const { return path_ == "/" ? String("/") + name : path_ + "/" + name; }
    bool protected_path(const String& p) const { return p == "/System" || p.substr(0, 8) == "/System/" || p == "/"; }

    void select_row(int row) {
        selected_ = row;
        if (row >= 0 && !entries_[order_[row]].dir) name_->set_text(entries_[order_[row]].name.c_str());
        confirm_path_ = "";
        status_ = "";
        invalidate();
    }
    void activate(int row) {
        const Entry& e = entries_[order_[row]];
        if (e.dir) {
            String name = e.name;
            go(child_path(name));
        } else {
            name_->set_text(e.name.c_str());
            accept();
        }
    }

    void make_folder() {
        if (protected_path(path_)) { set_status("You can't create a folder here.", true); return; }
        String name = "New Folder";
        for (int i = 2; vfs::exists(child_path(name).c_str()) && i < 1000; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "New Folder %d", i);
            name = buf;
        }
        vfs::mkdir(child_path(name).c_str());
        reload();
        for (int i = 0; i < (int)order_.size(); i++)
            if (entries_[order_[i]].name == name) { selected_ = i; reveal(i); }
        set_status("Folder created.", false);
    }

    void finish(const String& path) {
        Done done = done_;
        done_ = Done();
        close();
        if (done) done(path.c_str());
    }
    void dismiss() { done_ = Done(); close(); }

    void accept() {
        String typed = String(name_->get_text()).trim();
        if (typed.empty()) {
            if (selected_ >= 0 && entries_[order_[selected_]].dir) activate(selected_);
            return;
        }
        String path = typed.c_str()[0] == '/' ? typed : child_path(typed);
        if (vfs::is_dir(path.c_str())) {
            name_->set_text("");
            go(path);
            return;
        }
        if (!saving_) {
            if (!vfs::exists(path.c_str())) { set_status("That file was not found. Check the name and try again.", true); return; }
            finish(path);
            return;
        }
        if (default_ext_.size() && vfs::basename(path.c_str()).rfind('.') < 0) path = path + "." + default_ext_;
        String parent = vfs::dirname(path.c_str());
        if (!vfs::is_dir(parent.c_str())) { set_status("That folder doesn't exist.", true); return; }
        if (protected_path(parent)) { set_status("You can't save here. Choose another folder.", true); return; }
        if (vfs::exists(path.c_str()) && confirm_path_ != path) {
            String msg = vfs::basename(path.c_str()) + " already exists. Press Save again to replace it.";
            status_ = msg;
            status_error_ = true;
            confirm_path_ = path;
            invalidate();
            return;
        }
        finish(path);
    }

    bool press(int x, int y) {
        if (nav_button(0).contains(x, y)) { go_back(); return true; }
        if (nav_button(1).contains(x, y)) { go_forward(); return true; }
        if (nav_button(2).contains(x, y)) { go_up(); return true; }
        for (const Crumb& c : crumbs_)
            if (c.rect.contains(x, y)) { go(c.path); return true; }
        Rect l = list_rect();
        if (Rect(0, kListTop, kSideW, l.h).contains(x, y)) {
            int i = (y - kListTop - 8) / 28;
            if (y >= kListTop + 8 && i >= 0 && i < (int)places_.size()) go(places_[i].path);
            return true;
        }
        if (Rect(l.x, l.y, l.w, kHeadH).contains(x, y)) {
            int col = x >= col_x(ColSize) ? ColSize : x >= col_x(ColType) ? ColType : x >= col_x(ColDate) ? ColDate : ColName;
            if (col == sort_) descending_ = !descending_;
            else { sort_ = col; descending_ = false; }
            reload();
            return true;
        }
        Rect rows = list_rows_rect();
        if (Rect(rows.right(), rows.y, kScrollW, rows.h).contains(x, y)) {
            Rect th = thumb_rect();
            if (th.contains(x, y)) {
                dragging_thumb_ = true;
                drag_y_ = y;
                drag_scroll_ = scroll_;
            } else {
                set_scroll(scroll_ + (y < th.y ? -rows.h : rows.h));
            }
            return true;
        }
        if (rows.contains(x, y)) {
            select_row(row_at(y));
            return true;
        }
        return false;
    }

    void paint_nav(Canvas& c) {
        const Theme& t = theme();
        const Symbol syms[3] = {Symbol::ChevronLeft, Symbol::ChevronRight, Symbol::ChevronUp};
        const bool on[3] = {!back_.empty(), !forward_.empty(), path_ != "/"};
        for (int i = 0; i < 3; i++) {
            Rect r = nav_button(i);
            c.fill_round_rect(r, kControlRadius, t.control_bg);
            draw_symbol(c, r.inset(8), syms[i], on[i] ? t.text : t.text_tertiary, 2.0f);
        }
        Rect pr = path_rect();
        c.fill_round_rect(pr, kControlRadius, t.field_bg);
        c.stroke_round_rect(pr, kControlRadius, t.control_border);
        crumbs_.clear();
        int x = pr.x + 10;
        draw_symbol(c, Rect(x, pr.y + 7, 16, 16), Symbol::Folder, t.accent);
        x += 22;
        add_crumb(c, x, pr, String("Computer"), String("/"));
        const char* s = path_.c_str() + 1;
        String built;
        while (*s) {
            const char* e = strchr(s, '/');
            size_t n = e ? (size_t)(e - s) : strlen(s);
            String seg = String(s).substr(0, n);
            built = built + "/" + seg;
            c.draw_text(x, pr.y + 7, ">", t.text_tertiary, 13);
            x += 14;
            add_crumb(c, x, pr, seg, built);
            if (!e) break;
            s = e + 1;
        }
        c.fill_rect(Rect(0, kListTop - 1, width(), 1), t.separator);
    }

    void add_crumb(Canvas& c, int& x, Rect pr, const String& label, const String& target) {
        const Theme& t = theme();
        int w = text_width(label.c_str(), 13, Font::Regular) + 8;
        if (x + w > pr.right() - 6) return;
        Rect r(x - 4, pr.y + 3, w, pr.h - 6);
        c.draw_text(x, pr.y + 7, label.c_str(), t.text, 13);
        crumbs_.push(Crumb{r, target});
        x += w;
    }

    void paint_side(Canvas& c) {
        const Theme& t = theme();
        Rect side(0, kListTop, kSideW, list_rect().h);
        c.fill_rect(side, t.control_bg);
        c.fill_rect(Rect(kSideW - 1, side.y, 1, side.h), t.separator);
        int y = side.y + 8;
        for (const Place& p : places_) {
            Rect row(6, y, kSideW - 13, 26);
            if (p.path == path_) c.fill_round_rect(row, 6, t.selection);
            draw_symbol(c, Rect(14, y + 5, 16, 16), p.symbol, t.accent);
            c.draw_text_in(Rect(38, y, kSideW - 50, 26), p.label.c_str(), t.text, 13);
            y += 28;
        }
    }

    void paint_list(Canvas& c) {
        const Theme& t = theme();
        Rect l = list_rect(), rows = list_rows_rect();
        const char* heads[4] = {"Name", "Date modified", "Type", "Size"};
        for (int i = 0; i < 4; i++) {
            int x = col_x(i);
            c.draw_text_in(Rect(x, l.y, 110, kHeadH), heads[i], sort_ == i ? t.text : t.text_secondary, 12, Font::Medium);
            if (sort_ == i)
                draw_symbol(c, Rect(x + text_width(heads[i], 12, Font::Medium) + 4, l.y + 9, 9, 9),
                            descending_ ? Symbol::ChevronDown : Symbol::ChevronUp, t.text_secondary, 1.6f);
        }
        c.fill_rect(Rect(l.x, l.y + kHeadH - 1, l.w, 1), t.separator);
        c.save();
        c.clip_to(rows);
        int first = scroll_ / kRowH;
        int last = kmin((int)order_.size(), (scroll_ + rows.h) / kRowH + 1);
        for (int i = first; i < last; i++) {
            const Entry& e = entries_[order_[i]];
            int y = rows.y + i * kRowH - scroll_;
            Rect row(rows.x + 4, y, rows.w - 8, kRowH);
            if (i == selected_) c.fill_round_rect(row, 5, t.selection);
            else if (i == hover_) c.fill_round_rect(row, 5, t.hover);
            draw_symbol(c, Rect(col_x(ColName), y + 4, 16, 16), e.dir ? Symbol::Folder : Symbol::File, e.dir ? t.accent : t.text_secondary);
            c.draw_text_in(Rect(col_x(ColName) + 24, y, col_x(ColDate) - col_x(ColName) - 32, kRowH), e.name.c_str(), t.text, 13);
            char buf[40];
            if (e.mtime) {
                DateTime d = unix_to_datetime(e.mtime);
                snprintf(buf, sizeof(buf), "%04d-%02d-%02d  %02d:%02d", d.year, d.month, d.day, d.hour, d.minute);
                c.draw_text_in(Rect(col_x(ColDate), y, kDateW - 8, kRowH), buf, t.text_secondary, 12);
            }
            String type = e.dir ? String("File folder") : extension_upper(e.name) + (vfs::extension(e.name.c_str()).size() ? " File" : "File");
            c.draw_text_in(Rect(col_x(ColType), y, kTypeW - 8, kRowH), type.c_str(), t.text_secondary, 12);
            if (!e.dir) {
                if (e.size < 1024) snprintf(buf, sizeof(buf), "%u B", (unsigned)e.size);
                else if (e.size < 1024 * 1024) snprintf(buf, sizeof(buf), "%u KB", (unsigned)((e.size + 512) / 1024));
                else snprintf(buf, sizeof(buf), "%u MB", (unsigned)((e.size + (1 << 19)) >> 20));
                c.draw_text_in(Rect(col_x(ColSize), y, kSizeW - 8, kRowH), buf, t.text_secondary, 12);
            }
        }
        if (order_.empty())
            c.draw_text_in(Rect(rows.x, rows.y + 40, rows.w, 20), search_->text.size() ? "No matching items" : "This folder is empty", t.text_tertiary, 13, Font::Regular, Align::Center);
        c.restore();
        if (max_scroll() > 0) c.fill_round_rect(thumb_rect(), 4, t.text_tertiary);
    }

    bool saving_;
    Window* owner_;
    Done done_;
    String default_ext_;
    Vector<String> filter_labels_, filter_exts_;
    String path_, status_, confirm_path_;
    bool status_error_ = false;
    Vector<Entry> entries_;
    Vector<int> order_;
    Vector<Place> places_;
    Vector<Crumb> crumbs_;
    Vector<String> back_, forward_;
    int selected_ = -1, hover_ = -1, scroll_ = 0;
    int sort_ = ColName;
    bool descending_ = false;
    bool dragging_thumb_ = false;
    int drag_y_ = 0, drag_scroll_ = 0;
    uint64_t gen_ = 0;
    TextField* search_ = nullptr;
    TextField* name_ = nullptr;
    Button* new_folder_ = nullptr;
    Dropdown* filter_ = nullptr;
    Button* ok_ = nullptr;
    Button* cancel_ = nullptr;
};

void show(bool saving, Window* owner, const Options& o, Done done) {
    for (DialogWindow* d : dialogs())
        if (d->owner() == owner) { d->bring_to_front(); return; }
    new DialogWindow(saving, owner, o, done);
}

}  // namespace

void open(Window* owner, const Options& o, Done done) { show(false, owner, o, done); }
void save(Window* owner, const Options& o, Done done) { show(true, owner, o, done); }
void close_for(Window* owner) {
    for (DialogWindow* d : dialogs())
        if (d->owner() == owner) d->cancel_silently();
}

}  // namespace file_dialog
