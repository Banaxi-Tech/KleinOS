// Contacts: sorted list with colored initial avatars, a detail card, and an
// edit mode built from TextFields. Stored in <home>/Library/Contacts/contacts.txt
// as one record per line: name|phone|email|address|notes
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "fs/vfs.h"
#include "sys/session.h"
#include "arch/interrupts.h"
#include "lib/math.h"
#include "lib/string.h"

namespace {

constexpr int kSidebarW = 240;
constexpr int kRowH = 44;
constexpr int kDeleteArmMs = 3000;

const Color kAvatarColors[] = {
    hex(0xFF6482), hex(0xFF9F0A), hex(0x30B0C7), hex(0x0A84FF), hex(0xBF5AF2), hex(0x32B350), hex(0xAC8E68),
};

String contacts_file() { return String::format("/Users/%s/Library/Contacts/contacts.txt", session_user()); }

Color avatar_color(const char* name) {
    unsigned h = 0;
    for (const char* p = name; *p; p++) h = h * 31 + (unsigned char)*p;
    return kAvatarColors[h % (sizeof(kAvatarColors) / sizeof(kAvatarColors[0]))];
}

// "Maya Ferreira" -> "MF"
String initials_of(const char* name) {
    char out[3] = {0, 0, 0};
    int n = 0;
    const char* p = name;
    while (*p == ' ') p++;
    if (*p) out[n++] = (char)toupper((unsigned char)*p);
    const char* last = nullptr;
    for (const char* q = p; *q; q++)
        if (*q == ' ' && q[1] && q[1] != ' ') last = q + 1;
    if (last && n < 2) out[n++] = (char)toupper((unsigned char)*last);
    return String(out);
}

void draw_avatar(Canvas& c, Rect r, Color base, const char* ini) {
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, rad = r.w / 2.0f;
    c.fill_circle(cx, cy, rad, base);
    c.fill_ellipse(cx, cy - rad * 0.45f, rad * 0.75f, rad * 0.45f, rgba(255, 255, 255, 46));
    c.stroke_circle(cx, cy, rad - 0.5f, rgba(0, 0, 0, 30), 1.0f);
    c.draw_text_in(r, ini, hex(0xFFFFFF), (int)(r.w * 0.40f), Font::Bold, Align::Center);
}

// Strip field separators and line breaks so a record stays on one line.
String clean_field(const String& s) {
    String r;
    for (size_t i = 0; i < s.size(); i++) {
        char ch = s[i];
        r += (ch == '|' || ch == '\n' || ch == '\r') ? ' ' : ch;
    }
    return r;
}

struct Contact {
    uint32_t id = 0;
    String name, phone, email, address, notes;
};

void parse_record(const char* s, Contact& c) {
    String* fields[5] = {&c.name, &c.phone, &c.email, &c.address, &c.notes};
    int f = 0;
    String cur;
    for (const char* p = s;; p++) {
        if (*p == '|' || *p == '\0') {
            if (f < 5) *fields[f] = cur;
            f++;
            cur.clear();
            if (*p == '\0' || f >= 5) break;
        } else {
            cur += *p;
        }
    }
}

class ContactsWindow : public Window {
public:
    ContactsWindow() : Window("Contacts", 720, 480) {
        set_sidebar_width(kSidebarW);
        set_min_size(620, 380);
        set_ticking(true);

        list_ = (ListView*)add(new ListView(Rect()));
        list_->transparent = true;
        list_->row_height = kRowH;
        list_->on_select = [this](int i) { select_index(i); };

        add_btn_ = (Button*)add(new Button(Rect(), ""));
        add_btn_->icon = [](Canvas& c, Rect r) { draw_symbol(c, r, Symbol::Plus, theme().accent, 1.8f); };
        add_btn_->on_click = [this] { add_contact(); };

        edit_btn_ = (Button*)add(new Button(Rect(), "Edit"));
        edit_btn_->font_size = 13;
        edit_btn_->on_click = [this] { if (editing_) end_edit(); else begin_edit(); };

        del_btn_ = (Button*)add(new Button(Rect(), "Delete"));
        del_btn_->font_size = 13;
        del_btn_->on_click = [this] { on_delete_click(); };

        f_name_ = field("Full name");
        f_phone_ = field("Phone");
        f_email_ = field("Email");
        f_addr_ = field("Address");
        f_notes_ = field("Notes");
        f_name_->on_submit = [this] { end_edit(); };

        load();
        rebuild_list();
        if (!contacts_.empty()) select_index(0);
        focus_widget(list_);
        layout();
        update_buttons();
    }

    void on_paint(Canvas& c) override {
        Window::on_paint(c);
        const Theme& t = theme();
        int W = width(), H = height();
        int dx = kSidebarW;

        c.draw_text_in(Rect(14, 12, kSidebarW - 60, 28), "Contacts", t.text, 15, Font::Bold);
        char count[32];
        snprintf(count, sizeof(count), "%d contact%s", (int)contacts_.size(), contacts_.size() == 1 ? "" : "s");
        c.draw_text_in(Rect(12, H - 32, kSidebarW - 24, 22), count, t.text_secondary, 12, Font::Regular, Align::Center);

        if (sel_ < 0 || sel_ >= (int)contacts_.size()) {
            c.draw_text_in(Rect(dx, H / 2 - 30, W - dx, 26), "No Contact Selected", t.text_secondary, 17, Font::Medium,
                           Align::Center);
            c.draw_text_in(Rect(dx, H / 2 + 2, W - dx, 20), "Press + to add someone", t.text_tertiary, 13, Font::Regular,
                           Align::Center);
            return;
        }
        const Contact& ct = contacts_[sel_];
        String ini = initials_of(ct.name.c_str());
        draw_avatar(c, Rect(dx + 32, 64, 96, 96), avatar_color(ct.name.c_str()), ini.c_str());
        if (!editing_) {
            c.draw_text_in(Rect(dx + 148, 80, W - dx - 148 - 24, 30), ct.name.c_str(), t.text, 22, Font::Bold);
            c.draw_text_in(Rect(dx + 148, 114, W - dx - 148 - 24, 20),
                           ct.email.empty() ? "No email" : ct.email.c_str(), t.text_secondary, 13);
        }

        int cx = dx + 24, cw = W - dx - 48, cy = card_y();
        draw_group_box(c, Rect(cx, cy, cw, 4 * kRowH + 8));
        const char* labels[] = {"Phone", "Email", "Address", "Notes"};
        const String* values[] = {&ct.phone, &ct.email, &ct.address, &ct.notes};
        for (int i = 0; i < 4; i++) {
            int ry = cy + 4 + i * kRowH;
            c.draw_text_in(Rect(cx + 16, ry, 90, kRowH), labels[i], t.text_secondary, 13, Font::Medium);
            if (editing_) {
                // fields are real TextFields; only the labels are ours
            } else if (i == 3) {
                if (values[i]->empty())
                    c.draw_text_in(Rect(cx + 120, ry, cw - 136, kRowH), "\xE2\x80\x94", t.text_tertiary, 14);
                else
                    c.draw_text_wrapped(Rect(cx + 120, ry + 6, cw - 136, kRowH - 8), values[i]->c_str(), t.text, 13);
            } else {
                if (values[i]->empty())
                    c.draw_text_in(Rect(cx + 120, ry, cw - 136, kRowH), "\xE2\x80\x94", t.text_tertiary, 14);
                else
                    c.draw_text_in(Rect(cx + 120, ry, cw - 136, kRowH), values[i]->c_str(), t.text, 14);
            }
            if (i < 3) c.fill_rect(Rect(cx + 16, ry + kRowH - 1, cw - 32, 1), t.separator);
        }
    }

    void on_resize() override { layout(); }

    void on_tick(uint64_t now) override {
        if (confirm_delete_ && now - armed_at_ > kDeleteArmMs) {
            confirm_delete_ = false;
            update_buttons();
        }
    }

    void on_key(const KeyEvent& e) override {
        if (e.key == Key::Escape && editing_) { cancel_edit(); return; }
        if (e.key == Key::Char && e.cmd() && (e.ch | 0x20) == 'n') { add_contact(); return; }
        Window::on_key(e);
    }

    void build_menus(Vector<Menu>& menus) override {
        Menu& f = menus.emplace();
        f.title = "File";
        f.add("New Contact", [this] { add_contact(); }, "\xE2\x8C\x98N");
        f.add("Edit Contact", [this] { if (editing_) end_edit(); else begin_edit(); });
        f.add("Delete Contact", [this] { on_delete_click(); });
    }

private:
    TextField* field(const char* placeholder) {
        TextField* f = (TextField*)add(new TextField(Rect(), placeholder));
        f->font_size = 13;
        f->visible = false;
        return f;
    }

    int card_y() const { return 184; }

    void layout() {
        int W = width(), H = height();
        int dx = kSidebarW;
        list_->bounds = Rect(0, 52, dx, H - 52 - 40);
        add_btn_->bounds = Rect(dx - 12 - 28, 12, 28, 28);
        del_btn_->bounds = Rect(W - 12 - 80, 12, 80, 28);
        edit_btn_->bounds = Rect(del_btn_->bounds.x - 8 - 72, 12, 72, 28);

        f_name_->bounds = Rect(dx + 148, 80, W - dx - 148 - 24, 28);
        int cx = dx + 24, cw = W - dx - 48, cy = card_y();
        int fx = cx + 120, fw = cw - 136;
        f_phone_->bounds = Rect(fx, cy + 12, fw, 28);
        f_email_->bounds = Rect(fx, cy + 12 + kRowH, fw, 28);
        f_addr_->bounds = Rect(fx, cy + 12 + 2 * kRowH, fw, 28);
        f_notes_->bounds = Rect(fx, cy + 12 + 3 * kRowH, fw, 28);
    }

    // ---- storage
    void load() {
        String path = contacts_file();
        String text;
        if (!vfs::read_text(path.c_str(), text)) {
            seed();
            save_all();
            return;
        }
        const char* p = text.c_str();
        while (*p) {
            const char* e = p;
            while (*e && *e != '\n') e++;
            String line(p, (size_t)(e - p));
            p = *e ? e + 1 : e;
            if (line.trim().empty()) continue;
            Contact c;
            c.id = next_id_++;
            parse_record(line.c_str(), c);
            contacts_.push(c);
        }
        sort_contacts();
    }

    void seed() {
        struct Row { const char* name; const char* phone; const char* email; const char* address; const char* notes; };
        static const Row rows[] = {
            {"Maya Ferreira", "+1 555 0142", "maya.ferreira@example.com", "18 Harbor Road, Portsmouth",
             "Met at the robotics meetup. Loves rock climbing."},
            {"Tomas Lind", "+1 555 0177", "tomas@example.org", "4 Birch Lane, Riverton",
             "Co-owns the sailboat. Prefers email."},
            {"Priya Raman", "+1 555 0108", "priya.raman@example.net", "92 Quarry Street, Oakfield",
             "Designer. Ask about the logo project."},
            {"Owen Blackwood", "+1 555 0133", "owen.b@example.com", "7 Mill Court, Lakeview",
             "Dentist appointment every March."},
            {"Lena Okafor", "+1 555 0191", "lena@example.org", "310 Cedar Avenue, Brookfield",
             "Book club organizer. Vegetarian."},
        };
        for (const Row& r : rows) {
            Contact c;
            c.id = next_id_++;
            c.name = r.name;
            c.phone = r.phone;
            c.email = r.email;
            c.address = r.address;
            c.notes = r.notes;
            contacts_.push(c);
        }
        sort_contacts();
    }

    void save_all() {
        String out;
        for (const Contact& c : contacts_) {
            out += clean_field(c.name);
            out += '|';
            out += clean_field(c.phone);
            out += '|';
            out += clean_field(c.email);
            out += '|';
            out += clean_field(c.address);
            out += '|';
            out += clean_field(c.notes);
            out += '\n';
        }
        vfs::write_text(contacts_file().c_str(), out.c_str());
    }

    void sort_contacts() {
        for (size_t i = 1; i < contacts_.size(); i++) {
            size_t j = i;
            while (j > 0 && strcasecmp(contacts_[j - 1].name.c_str(), contacts_[j].name.c_str()) > 0) {
                Contact tmp = contacts_[j - 1];
                contacts_[j - 1] = contacts_[j];
                contacts_[j] = tmp;
                j--;
            }
        }
    }

    int index_of_id(uint32_t id) const {
        for (size_t i = 0; i < contacts_.size(); i++)
            if (contacts_[i].id == id) return (int)i;
        return -1;
    }

    // ---- list
    void rebuild_list() {
        list_->clear();
        for (const Contact& c : contacts_) {
            ListView::Item& it = list_->add(c.name.c_str(), "");
            Color col = avatar_color(c.name.c_str());
            String ini = initials_of(c.name.c_str());
            it.icon = [col, ini](Canvas& cv, Rect r) { draw_avatar(cv, r, col, ini.c_str()); };
        }
        list_->selected = sel_;
        if (sel_ >= 0) list_->ensure_visible(sel_);
        invalidate();
    }

    void select_index(int i) {
        if (i < 0 || i >= (int)contacts_.size()) return;
        if (editing_) {
            // committing an edit may re-sort the list, so track the clicked contact by id
            uint32_t id = contacts_[i].id;
            end_edit();
            i = index_of_id(id);
            if (i < 0) return;
        }
        sel_ = i;
        list_->selected = i;
        confirm_delete_ = false;
        update_buttons();
        invalidate();
    }

    void update_buttons() {
        edit_btn_->text = editing_ ? "Done" : "Edit";
        edit_btn_->enabled = sel_ >= 0;
        del_btn_->enabled = sel_ >= 0;
        if (confirm_delete_) {
            del_btn_->text = "Delete?";
            del_btn_->style = Button::Destructive;
        } else {
            del_btn_->text = "Delete";
            del_btn_->style = Button::Normal;
        }
        edit_btn_->invalidate();
        del_btn_->invalidate();
    }

    void set_fields_visible(bool v) {
        f_name_->visible = v;
        f_phone_->visible = v;
        f_email_->visible = v;
        f_addr_->visible = v;
        f_notes_->visible = v;
    }

    // ---- actions
    void add_contact() {
        if (editing_) end_edit();
        Contact c;
        c.id = next_id_++;
        c.name = "New Contact";
        contacts_.push(c);
        sort_contacts();
        sel_ = index_of_id(c.id);
        save_all();
        rebuild_list();
        begin_edit();
    }

    void begin_edit() {
        if (sel_ < 0) return;
        const Contact& c = contacts_[sel_];
        f_name_->set_text(c.name.c_str());
        f_phone_->set_text(c.phone.c_str());
        f_email_->set_text(c.email.c_str());
        f_addr_->set_text(c.address.c_str());
        f_notes_->set_text(c.notes.c_str());
        editing_ = true;
        confirm_delete_ = false;
        set_fields_visible(true);
        update_buttons();
        focus_widget(f_name_);
        invalidate();
    }

    void end_edit() {
        if (!editing_) return;
        if (sel_ >= 0) {
            uint32_t id = contacts_[sel_].id;
            Contact& c = contacts_[sel_];
            String name = String(f_name_->get_text()).trim();
            c.name = name.empty() ? String("Unnamed") : name;
            c.phone = String(f_phone_->get_text()).trim();
            c.email = String(f_email_->get_text()).trim();
            c.address = String(f_addr_->get_text()).trim();
            c.notes = String(f_notes_->get_text()).trim();
            sort_contacts();
            sel_ = index_of_id(id);
            save_all();
        }
        finish_edit_ui();
    }

    void cancel_edit() {
        if (!editing_) return;
        finish_edit_ui();
    }

    void finish_edit_ui() {
        editing_ = false;
        set_fields_visible(false);
        rebuild_list();
        update_buttons();
        focus_widget(list_);
        invalidate();
    }

    void on_delete_click() {
        if (sel_ < 0) return;
        if (!confirm_delete_) {
            confirm_delete_ = true;
            armed_at_ = timer_ms();
            update_buttons();
            return;
        }
        confirm_delete_ = false;
        int idx = sel_;
        editing_ = false;
        set_fields_visible(false);
        contacts_.remove(idx);
        sel_ = -1;
        save_all();
        if (!contacts_.empty()) sel_ = idx < (int)contacts_.size() ? idx : (int)contacts_.size() - 1;
        rebuild_list();
        update_buttons();
        if (sel_ >= 0) list_->selected = sel_;
        invalidate();
    }

    ListView* list_ = nullptr;
    Button* add_btn_ = nullptr;
    Button* edit_btn_ = nullptr;
    Button* del_btn_ = nullptr;
    TextField* f_name_ = nullptr;
    TextField* f_phone_ = nullptr;
    TextField* f_email_ = nullptr;
    TextField* f_addr_ = nullptr;
    TextField* f_notes_ = nullptr;
    Vector<Contact> contacts_;
    uint32_t next_id_ = 1;
    int sel_ = -1;
    bool editing_ = false;
    bool confirm_delete_ = false;
    uint64_t armed_at_ = 0;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0xFFA27A), hex(0xE2532F));
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
    float u = r.w / 40.0f;
    // head
    c.fill_circle(cx, cy - 6 * u, 7.5f * u, hex(0xFFFFFF));
    // shoulders
    c.fill_ellipse(cx, cy + 15 * u, 13 * u, 9.5f * u, hex(0xFFFFFF));
    // soft inner rim so the figure reads on the gradient
    c.stroke_circle(cx, cy - 6 * u, 7.5f * u, rgba(0, 0, 0, 25), 1.0f);
}

Window* create() { return new ContactsWindow(); }

AppRegistrar reg({"contacts", "Contacts", icon, create, "Productivity", true, false, 70, "Keep track of people you know"});

}  // namespace
