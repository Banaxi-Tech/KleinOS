// Notes: glass sidebar of notes with search, a TextArea editor with autosave,
// and a two-step delete. Each note is a file in <home>/Notes/note-N.txt.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "fs/vfs.h"
#include "sys/session.h"
#include "drivers/rtc.h"
#include "arch/interrupts.h"
#include "lib/math.h"
#include "lib/string.h"

namespace {

constexpr int kSidebarW = 220;
constexpr int kTopBarH = 44;
constexpr int kAutosaveDelayMs = 600;
constexpr uint64_t kDeleteArmMs = 3000;

String notes_dir() { return String::format("/Users/%s/Notes", session_user()); }
String note_path(const char* file) { return String::format("%s/%s", notes_dir().c_str(), file); }

// Title = first non-blank line of the note.
String note_title(const String& text) {
    const char* p = text.c_str();
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    String line;
    while (*p && *p != '\n' && line.size() < 48) {
        if (*p != '\r') line += *p;
        p++;
    }
    line = line.trim();
    if (line.empty()) return String("New Note");
    return line;
}

// "Today" or a short date such as "Nov 6".
String note_date(uint64_t mtime) {
    DateTime now = rtc_now();
    DateTime d = unix_to_datetime(mtime + (int64_t)rtc_utc_offset_minutes() * 60);
    if (d.year == now.year && d.month == now.month && d.day == now.day) return String("Today");
    return String::format("%.3s %d", month_name(d.month), d.day);
}

// Case-insensitive substring test.
bool contains_ci(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    if (n == 0) return true;
    for (const char* p = hay; *p; p++)
        if (strncasecmp(p, needle, n) == 0) return true;
    return false;
}

struct Note {
    String file;      // file name inside Notes/
    String text;
    uint64_t mtime = 0;
};

class NotesWindow : public Window {
public:
    NotesWindow() : Window("Notes", 760, 500) {
        set_sidebar_width(kSidebarW);
        set_min_size(560, 340);
        set_ticking(true);

        area_ = (TextArea*)add(new TextArea(Rect()));
        area_->font_size = 15;
        area_->on_change = [this] { on_text_changed(); };

        search_ = (TextField*)add(new TextField(Rect(), "Search"));
        search_->rounded = true;
        search_->search_icon = true;
        search_->font_size = 13;
        search_->on_change = [this] { rebuild_list(); };

        add_btn_ = (Button*)add(new Button(Rect(), ""));
        add_btn_->icon = [](Canvas& c, Rect r) { draw_symbol(c, r, Symbol::Plus, theme().accent, 1.8f); };
        add_btn_->on_click = [this] { create_note(); };

        list_ = (ListView*)add(new ListView(Rect()));
        list_->transparent = true;
        list_->row_height = 34;
        list_->on_select = [this](int vi) { select_visible(vi); };

        del_btn_ = (Button*)add(new Button(Rect(), "Delete"));
        del_btn_->font_size = 13;
        del_btn_->on_click = [this] { on_delete_click(); };

        load_notes();
        rebuild_list();
        if (cur_ < 0 && !visible_.empty()) select_visible(0);
        else update_editor();
        layout();
    }

    void on_paint(Canvas& c) override {
        Window::on_paint(c);
        const Theme& t = theme();
        int W = width(), H = height();
        c.fill_rect(Rect(kSidebarW, kTopBarH - 1, W - kSidebarW, 1), t.separator);
        c.draw_text_in(Rect(kSidebarW + 14, 0, 160, kTopBarH), dirty_ ? "Editing\xE2\x80\xA6" : "Saved",
                       t.text_secondary, 12, Font::Regular);

        if (cur_ < 0) {
            c.draw_text_in(Rect(kSidebarW, H / 2 - 30, W - kSidebarW, 26), "No Note Selected", t.text_secondary, 17,
                           Font::Medium, Align::Center);
            c.draw_text_in(Rect(kSidebarW, H / 2 + 2, W - kSidebarW, 20), "Press + to write a new note", t.text_tertiary,
                           13, Font::Regular, Align::Center);
        }

        char count[32];
        snprintf(count, sizeof(count), "%d note%s", (int)notes_.size(), notes_.size() == 1 ? "" : "s");
        c.draw_text_in(Rect(12, H - 32, kSidebarW - 24, 22), count, t.text_secondary, 12, Font::Regular, Align::Center);
    }

    void on_resize() override { layout(); }

    void on_tick(uint64_t now) override {
        if (dirty_ && cur_ >= 0 && now - last_edit_ > kAutosaveDelayMs) {
            save_note(cur_);
            dirty_ = false;
            invalidate();
        }
        if (confirm_delete_ && now - armed_at_ > kDeleteArmMs) {
            confirm_delete_ = false;
            update_delete_button();
        }
    }

    bool on_close() override {
        flush_current();
        return true;
    }

    void on_key(const KeyEvent& e) override {
        if (e.key == Key::Char && e.cmd()) {
            uint32_t ch = e.ch | 0x20;
            if (ch == 'n') { create_note(); return; }
            if (ch == 'f') { focus_widget(search_); return; }
        }
        Window::on_key(e);
    }

    void build_menus(Vector<Menu>& menus) override {
        Menu& f = menus.emplace();
        f.title = "File";
        f.add("New Note", [this] { create_note(); }, "\xE2\x8C\x98N");
        f.add("Delete Note", [this] { on_delete_click(); });
        Menu& e = menus.emplace();
        e.title = "Find";
        e.add("Search Notes", [this] { focus_widget(search_); }, "\xE2\x8C\x98" "F");
    }

private:
    void layout() {
        int W = width(), H = height();
        area_->bounds = Rect(kSidebarW, kTopBarH, W - kSidebarW, H - kTopBarH);
        search_->bounds = Rect(12, 12, kSidebarW - 12 - 12 - 36, 28);
        add_btn_->bounds = Rect(kSidebarW - 12 - 28, 12, 28, 28);
        list_->bounds = Rect(0, 52, kSidebarW, H - 52 - 40);
        del_btn_->bounds = Rect(W - 12 - 84, 8, 84, 28);
    }

    // ---- storage
    void load_notes() {
        String dir = notes_dir();
        bool fresh = !vfs::exists(dir.c_str());
        if (fresh) {
            vfs::mkdir(dir.c_str());
            write_new_file("note-1.txt", "Groceries\n\n- Oat milk\n- Coffee beans\n- Sourdough\n- Lemons\n");
            write_new_file("note-2.txt", "Trip ideas\n\nA weekend by the lake. Pack light and bring the camera.\n"
                                         "Check the ferry times before Friday.\n");
        }
        vfs::Node* d = vfs::lookup(dir.c_str());
        if (!d) return;
        for (vfs::Node* n : d->children) {
            if (n->is_dir || !n->name.ends_with(".txt")) continue;
            if (n->name.starts_with("note-")) {
                int id = atoi(n->name.c_str() + 5);
                if (id >= next_id_) next_id_ = id + 1;
            }
            Note note;
            note.file = n->name;
            note.mtime = n->mtime;
            String text;
            if (vfs::read_text(note_path(n->name.c_str()).c_str(), text)) note.text = text;
            // newest first (insertion sort; there are only a few notes)
            notes_.push(note);
            size_t j = notes_.size() - 1;
            while (j > 0 && notes_[j - 1].mtime < notes_[j].mtime) {
                Note tmp = notes_[j - 1];
                notes_[j - 1] = notes_[j];
                notes_[j] = tmp;
                j--;
            }
        }
    }

    void write_new_file(const char* file, const char* text) {
        vfs::write_text(note_path(file).c_str(), text);
    }

    String next_file_name() {
        for (;;) {
            String name = String::format("note-%d.txt", next_id_++);
            if (!vfs::exists(note_path(name.c_str()).c_str())) return name;
        }
    }

    void save_note(int i) {
        Note& n = notes_[i];
        String path = note_path(n.file.c_str());
        vfs::write_text(path.c_str(), n.text.c_str());
        vfs::Node* node = vfs::lookup(path.c_str());
        if (node) n.mtime = node->mtime;
        int vi = visible_index(i);
        if (vi >= 0) {
            list_->items[vi].text = note_title(n.text);
            list_->items[vi].detail = note_date(n.mtime);
        }
        list_->invalidate();
    }

    void flush_current() {
        if (cur_ >= 0 && dirty_) save_note(cur_);
        dirty_ = false;
    }

    // ---- list and selection
    int visible_index(int note_index) const {
        for (size_t i = 0; i < visible_.size(); i++)
            if (visible_[i] == note_index) return (int)i;
        return -1;
    }

    void rebuild_list() {
        list_->clear();
        visible_.clear();
        String q = search_->get_text();
        for (size_t i = 0; i < notes_.size(); i++) {
            const Note& n = notes_[i];
            if (!q.empty() && !contains_ci(n.text.c_str(), q.c_str())) continue;
            visible_.push((int)i);
            String title = note_title(n.text);
            String date = note_date(n.mtime);
            list_->add(title.c_str(), date.c_str());
        }
        int vi = visible_index(cur_);
        list_->selected = vi;
        if (vi >= 0) {
            list_->ensure_visible(vi);
        } else if (cur_ >= 0) {
            // the open note was filtered out
            flush_current();
            cur_ = -1;
            update_editor();
        }
        invalidate();
    }

    void select_visible(int vi) {
        if (vi < 0 || vi >= (int)visible_.size()) return;
        int ni = visible_[vi];
        if (ni == cur_) return;
        flush_current();
        cur_ = ni;
        list_->selected = vi;
        confirm_delete_ = false;
        update_editor();
    }

    void update_editor() {
        if (cur_ >= 0) {
            area_->readonly = false;
            area_->set_text(notes_[cur_].text.c_str());
        } else {
            area_->readonly = true;
            area_->set_text("");
        }
        del_btn_->enabled = cur_ >= 0;
        update_delete_button();
        invalidate();
    }

    void update_delete_button() {
        if (confirm_delete_) {
            del_btn_->text = "Delete?";
            del_btn_->style = Button::Destructive;
        } else {
            del_btn_->text = "Delete";
            del_btn_->style = Button::Normal;
        }
        del_btn_->invalidate();
    }

    // ---- actions
    void on_text_changed() {
        if (cur_ < 0) return;
        notes_[cur_].text = area_->get_text();
        dirty_ = true;
        last_edit_ = timer_ms();
        invalidate();
    }

    void create_note() {
        flush_current();
        String name = next_file_name();
        write_new_file(name.c_str(), "");
        Note n;
        n.file = name;
        vfs::Node* node = vfs::lookup(note_path(name.c_str()).c_str());
        n.mtime = node ? node->mtime : 0;
        notes_.insert(0, n);
        search_->set_text("");
        cur_ = 0;
        rebuild_list();
        update_editor();
        focus_widget(area_);
    }

    void on_delete_click() {
        if (cur_ < 0) return;
        if (!confirm_delete_) {
            confirm_delete_ = true;
            armed_at_ = timer_ms();
            update_delete_button();
            return;
        }
        confirm_delete_ = false;
        int idx = cur_;
        vfs::remove(note_path(notes_[idx].file.c_str()).c_str());
        dirty_ = false;
        cur_ = -1;
        notes_.remove(idx);
        rebuild_list();
        if (!visible_.empty()) {
            int vi = idx < (int)visible_.size() ? idx : (int)visible_.size() - 1;
            select_visible(vi);
        } else {
            update_editor();
        }
    }

    TextArea* area_ = nullptr;
    TextField* search_ = nullptr;
    Button* add_btn_ = nullptr;
    Button* del_btn_ = nullptr;
    ListView* list_ = nullptr;
    Vector<Note> notes_;
    Vector<int> visible_;        // list row -> index into notes_
    int cur_ = -1;               // index into notes_, -1 = none
    int next_id_ = 1;
    bool dirty_ = false;
    uint64_t last_edit_ = 0;
    bool confirm_delete_ = false;
    uint64_t armed_at_ = 0;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x6FD6C4), hex(0x168C7C));
    int cw = r.w * 9 / 20, ch = r.h * 12 / 20;
    // back card
    Rect back(r.x + r.w / 2 - cw / 2 + r.w / 10, r.y + r.h / 5, cw, ch);
    c.fill_round_rect(back, r.w / 30.0f, rgba(255, 255, 255, 110));
    // front card with ruled lines
    Rect front(r.x + r.w / 2 - cw / 2 - r.w / 14, r.y + r.h / 4 + r.h / 14, cw, ch);
    c.draw_shadow(front, r.w / 30.0f, r.w / 22.0f, rgba(0, 40, 40, 80), 0, r.w / 40);
    c.fill_round_rect(front, r.w / 30.0f, hex(0xFFFFFF));
    for (int i = 0; i < 3; i++) {
        int ly = front.y + ch * (i + 1) / 5;
        int lw = i == 2 ? cw * 5 / 10 : cw * 7 / 10;
        c.fill_rect(Rect(front.x + cw / 8, ly, lw, r.w / 34 + 1), hex(0x168C7C));
    }
}

Window* create() { return new NotesWindow(); }

AppRegistrar reg({"notes", "Notes", icon, create, "Productivity", true, true, 35, "Quick notes, saved as you type"});

}  // namespace
