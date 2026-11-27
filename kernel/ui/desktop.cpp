// Desktop icons: whatever is in ~/Desktop, top-right first, column by column like
// macOS. Click selects, double-click (or Enter) opens, drag moves an icon to another
// grid cell (remembered per user in ~/Library/Settings/desktop.txt), right-click
// gives Open / Show in Files / Move to Trash. Images get a real thumbnail.
#include "drivers/sound.h"
#include "ui/desktop.h"
#include "ui/wm.h"
#include "ui/shell.h"
#include "ui/theme.h"
#include "ui/app.h"
#include "fs/vfs.h"
#include "gfx/font.h"
#include "arch/interrupts.h"
#include "lib/kstring.h"
#include "lib/vector.h"
#include "lib/math.h"
#include "lib/log.h"

namespace {

constexpr int kCellW = 96, kCellH = 104, kIcon = 60, kMarginR = 14, kMarginT = 12;
constexpr int kLabelSize = 12;
constexpr size_t kMaxThumbSource = 24u << 20;

enum class Kind : uint8_t { Folder, Image, Text, Code, Audio, App, Other };

struct Item {
    String name, path;
    Kind kind = Kind::Other;
    uint64_t mtime = 0;
    size_t size = 0;
    int col = -1, row = -1;     // grid cell
    Canvas* thumb = nullptr;    // images only
};

Vector<Item> g_items;
uint64_t g_gen = ~0ull;
String g_user;                  // whose Desktop is shown
int g_sel = -1;
// press / drag
int g_press = -1;
Point g_press_at, g_drag_pos;
int g_grab_dx, g_grab_dy;
bool g_dragging;
uint64_t g_last_click_ms;
int g_last_click_item = -1;

String desktop_dir() { return String("/Users/") + shell_user_login() + "/Desktop"; }
String layout_file() { return String("/Users/") + shell_user_login() + "/Library/Settings/desktop.txt"; }

Kind kind_of(const char* name, bool dir) {
    if (dir) return Kind::Folder;
    String e = vfs::extension(name);
    const char* x = e.c_str();
    if (!strcmp(x, "png") || !strcmp(x, "jpg") || !strcmp(x, "jpeg") || !strcmp(x, "bmp") || !strcmp(x, "gif")) return Kind::Image;
    if (!strcmp(x, "txt") || !strcmp(x, "md") || !strcmp(x, "log") || !strcmp(x, "rtf") || !strcmp(x, "csv")) return Kind::Text;
    if (!strcmp(x, "c") || !strcmp(x, "cpp") || !strcmp(x, "h") || !strcmp(x, "js") || !strcmp(x, "py") || !strcmp(x, "html") ||
        !strcmp(x, "css") || !strcmp(x, "json") || !strcmp(x, "sh"))
        return Kind::Code;
    if (!strcmp(x, "wav") || !strcmp(x, "mp3") || !strcmp(x, "ogg")) return Kind::Audio;
    if (!strcmp(x, "run")) return Kind::App;
    return Kind::Other;
}

// ---------------------------------------------------------------- grid
Rect area() {
    Rect d = wm::desktop_area();
    return Rect(d.x, d.y + kMarginT, d.w - kMarginR, d.h - kMarginT);
}
int rows() { return kmax(1, area().h / kCellH); }
int cols() { return kmax(1, area().w / kCellW); }
Rect cell_rect(int col, int row) {
    Rect a = area();
    return Rect(a.right() - (col + 1) * kCellW, a.y + row * kCellH, kCellW, kCellH);
}
Rect icon_rect(const Item& it) {
    Rect c = cell_rect(it.col, it.row);
    return Rect(c.x + (kCellW - kIcon) / 2, c.y + 4, kIcon, kIcon);
}
Rect hit_rect(const Item& it) {   // icon + label
    Rect c = cell_rect(it.col, it.row);
    return Rect(c.x + 4, c.y, kCellW - 8, kCellH - 4);
}
int cell_at(int x, int y, int* col, int* row) {
    Rect a = area();
    int c = (a.right() - 1 - x) / kCellW, r = (y - a.y) / kCellH;
    if (x >= a.right() || y < a.y) return 0;
    *col = kclamp(c, 0, cols() - 1);
    *row = kclamp(r, 0, rows() - 1);
    return 1;
}
bool cell_taken(int col, int row, int except) {
    for (int i = 0; i < (int)g_items.size(); i++)
        if (i != except && g_items[i].col == col && g_items[i].row == row) return true;
    return false;
}
void damage_item(const Item& it) { wm::damage(hit_rect(it).expand(6)); }
void damage_all_items() { for (Item& it : g_items) damage_item(it); }

// ---------------------------------------------------------------- layout file
void save_layout() {
    String out;
    for (Item& it : g_items) out += String::format("%d %d ", it.col, it.row) + it.name + "\n";
    vfs::write_text(layout_file().c_str(), out.c_str());
}

void load_layout(Vector<Item>& items) {
    String text;
    if (!vfs::read_text(layout_file().c_str(), text)) return;
    const char* p = text.c_str();
    while (*p) {
        const char* e = strchr(p, '\n');
        String line = e ? String(p, e - p) : String(p);
        p = e ? e + 1 : p + strlen(p);
        char* end;
        long c = strtol(line.c_str(), &end, 10);
        long r = strtol(end, &end, 10);
        if (*end != ' ') continue;
        const char* name = end + 1;
        for (Item& it : items)
            if (it.name == name && c >= 0 && r >= 0 && c < cols() && r < rows() && !cell_taken(c, r, -1)) {
                it.col = (int)c;
                it.row = (int)r;
            }
    }
}

// ---------------------------------------------------------------- thumbnails
Canvas* make_thumb(const char* path, size_t size) {
    if (size > kMaxThumbSource) return nullptr;
    const uint8_t* data = nullptr;
    size_t n = 0;
    if (!vfs::read_file(path, &data, &n)) return nullptr;
    Canvas* img = image_decode(data, n);
    if (!img) return nullptr;
    int w = img->width(), h = img->height();
    int tw = w >= h ? kIcon : kmax(1, kIcon * w / h), th = w >= h ? kmax(1, kIcon * h / w) : kIcon;
    Canvas* t = new Canvas(tw, th);
    t->draw_scaled(*img, Rect(0, 0, tw, th));
    delete img;
    return t;
}

// ---------------------------------------------------------------- scan
void rescan() {
    String dir = desktop_dir();
    vfs::Node* d = vfs::lookup(dir.c_str());
    Vector<Item> items;
    if (d && d->is_dir) {
        for (vfs::Node* n : d->children) {
            if (n->name.empty() || n->name[0] == '.') continue;
            Item it;
            it.name = n->name;
            it.path = dir + "/" + n->name;
            it.kind = kind_of(n->name.c_str(), n->is_dir);
            it.mtime = n->mtime;
            it.size = n->size;
            items.push(std::move(it));
        }
    }
    // keep thumbnails and positions of files we already had
    for (Item& it : items)
        for (Item& old : g_items)
            if (old.name == it.name) {
                it.col = old.col;
                it.row = old.row;
                if (old.thumb && old.mtime == it.mtime && old.size == it.size) { it.thumb = old.thumb; old.thumb = nullptr; }
            }
    String sel = g_sel >= 0 && g_sel < (int)g_items.size() ? g_items[g_sel].name : String();
    damage_all_items();
    for (Item& old : g_items) delete old.thumb;
    bool fresh_user = g_user != shell_user_login();
    g_items = std::move(items);
    g_user = shell_user_login();
    if (fresh_user) load_layout(g_items);
    // sort the unplaced ones by name and give them the first free cells, column by column
    for (size_t i = 1; i < g_items.size(); i++)
        for (size_t j = i; j > 0 && g_items[j].name.lower() < g_items[j - 1].name.lower(); j--) {
            Item t = std::move(g_items[j]); g_items[j] = std::move(g_items[j - 1]); g_items[j - 1] = std::move(t);
        }
    int R = rows(), C = cols();
    for (int i = 0; i < (int)g_items.size(); i++) {
        Item& it = g_items[i];
        if (it.col >= 0 && it.col < C && it.row >= 0 && it.row < R && !cell_taken(it.col, it.row, i)) continue;
        it.col = it.row = -1;
        for (int k = 0; k < R * C; k++)
            if (!cell_taken(k / R, k % R, i)) { it.col = k / R; it.row = k % R; break; }
        if (it.col < 0) { it.col = C - 1; it.row = R - 1; }   // more files than fit: pile on the last cell
    }
    for (Item& it : g_items)
        if (it.kind == Kind::Image && !it.thumb) it.thumb = make_thumb(it.path.c_str(), it.size);
    g_sel = -1;
    for (int i = 0; i < (int)g_items.size(); i++) if (!sel.empty() && g_items[i].name == sel) g_sel = i;
    damage_all_items();
}

// ---------------------------------------------------------------- drawing
void page(Canvas& c, Rect r, Color fill, Color edge) {
    float u = r.w / 64.0f, x = (float)r.x, y = (float)r.y;
    PointF p[5] = {{x + 12 * u, y + 4 * u}, {x + 40 * u, y + 4 * u}, {x + 52 * u, y + 16 * u}, {x + 52 * u, y + 60 * u}, {x + 12 * u, y + 60 * u}};
    c.draw_shadow(Rect((int)(x + 12 * u), (int)(y + 4 * u), (int)(40 * u), (int)(56 * u)), 3 * u, 4 * u, rgba(0, 0, 0, 70), 0, 2);
    c.fill_polygon(p, 5, fill);
    PointF f[3] = {{x + 40 * u, y + 4 * u}, {x + 40 * u, y + 16 * u}, {x + 52 * u, y + 16 * u}};
    c.fill_polygon(f, 3, edge);
}

void draw_folder(Canvas& c, Rect r) {
    float u = r.w / 64.0f, x = (float)r.x, y = (float)r.y;
    c.fill_round_rect(Rect((int)(x + 4 * u), (int)(y + 9 * u), (int)(24 * u), (int)(12 * u)), 4 * u, hex(0x1B63C9));
    Rect body((int)x, (int)(y + 14 * u), r.w, (int)(r.h - 14 * u));
    c.draw_shadow(body, 7 * u, 4 * u, rgba(0, 0, 0, 80), 0, 3);
    c.fill_gradient(body, hex(0x93D3FF), hex(0x2E7FE8), 7 * u);
    c.fill_gradient(Rect((int)(x + u), (int)(y + 15 * u), (int)(r.w - 2 * u), (int)(12 * u)), rgba(255, 255, 255, 85), rgba(255, 255, 255, 0), 6 * u);
    c.stroke_round_rect(body, 7 * u, rgba(255, 255, 255, 110), 1.0f);
}

void draw_item_icon(Canvas& c, const Item& it, Rect r) {
    float u = r.w / 64.0f, x = (float)r.x, y = (float)r.y;
    Color paper = hex(0xF6F7FA), edge = hex(0xC9CDD6);
    switch (it.kind) {
    case Kind::Folder: draw_folder(c, r); return;
    case Kind::Image:
        if (it.thumb) {
            Rect t(r.x + (r.w - it.thumb->width()) / 2, r.y + (r.h - it.thumb->height()) / 2, it.thumb->width(), it.thumb->height());
            c.draw_shadow(t, 2, 5, rgba(0, 0, 0, 90), 0, 2);
            c.fill_rect(t.expand(2), hex(0xFFFFFF));
            c.blit(*it.thumb, t.x, t.y);
            return;
        }
        page(c, r, paper, edge);
        draw_symbol(c, Rect((int)(x + 18 * u), (int)(y + 24 * u), (int)(28 * u), (int)(26 * u)), Symbol::Image, hex(0x2B7BE6), 2.2f);
        return;
    case Kind::Text:
        page(c, r, paper, edge);
        for (int i = 0; i < 4; i++) {
            float ly = y + (26 + i * 8) * u;
            c.draw_line(x + 19 * u, ly, x + (i == 3 ? 36 : 46) * u, ly, hex(0x9AA1AD), 2.0f * u + 0.5f);
        }
        return;
    case Kind::Code:
        page(c, r, paper, edge);
        c.draw_text_in(Rect((int)(x + 12 * u), (int)(y + 24 * u), (int)(40 * u), (int)(24 * u)), "</>", hex(0x2E7FE8), (int)(15 * u + 0.5f), Font::Bold, Align::Center);
        return;
    case Kind::Audio:
        page(c, r, paper, edge);
        draw_symbol(c, Rect((int)(x + 17 * u), (int)(y + 22 * u), (int)(30 * u), (int)(30 * u)), Symbol::Music, hex(0xE0457B), 2.4f);
        return;
    case Kind::App:
        icon_base(c, r.inset(4), hex(0x8E8E93), hex(0x48484A));
        draw_symbol(c, r.inset(r.w / 4), Symbol::Grid, hex(0xFFFFFF), 2.0f);
        return;
    default:
        page(c, r, paper, edge);
        for (int i = 0; i < 3; i++) c.draw_line(x + 19 * u, y + (28 + i * 8) * u, x + 46 * u, y + (28 + i * 8) * u, hex(0xB7BCC6), 2.0f * u + 0.5f);
        return;
    }
}

// Split a name over at most two lines: break at a space if the first line can end on one.
void split_label(const char* s, int maxw, String& l1, String& l2) {
    int n = (int)strlen(s);
    if (text_width(s, kLabelSize, Font::Medium) <= maxw) { l1 = s; l2 = ""; return; }
    int fit = 0, space = -1;
    for (int i = 1; i <= n; i++) {
        if (i < n && ((uint8_t)s[i] & 0xC0) == 0x80) continue;    // inside a UTF-8 sequence
        if (text_width_n(s, i, kLabelSize, Font::Medium) > maxw) break;
        fit = i;
        if (i < n && s[i] == ' ') space = i;
    }
    int cut = space > 0 ? space : fit;
    if (cut <= 0) cut = 1;
    l1 = String(s, cut);
    const char* rest = s + cut;
    while (*rest == ' ') rest++;
    l2 = rest;
}

void draw_label(Canvas& c, Rect r, const char* text, bool selected) {
    if (selected) {
        int tw = kmin(text_width(text, kLabelSize, Font::Medium), r.w) + 10;
        c.fill_round_rect(Rect(r.x + (r.w - tw) / 2, r.y, tw, r.h), 4, theme().accent);
        c.draw_text_in(r, text, theme().accent_text, kLabelSize, Font::Medium, Align::Center);
        return;
    }
    // white on a soft dark halo, readable on light skies and dark rocks alike
    static const int8_t halo[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, 1}, {1, 1}, {0, 2}};
    for (auto& o : halo) c.draw_text_in(r.offset(o[0], o[1]), text, rgba(0, 0, 0, 95), kLabelSize, Font::Medium, Align::Center);
    c.draw_text_in(r, text, rgba(255, 255, 255, 255), kLabelSize, Font::Medium, Align::Center);
}

void draw_item(Canvas& s, const Item& it, Rect ir, bool selected, int alpha_dim) {
    if (selected) s.fill_round_rect(ir.expand(5), 8, rgba(0, 0, 0, 70));
    draw_item_icon(s, it, ir);
    if (alpha_dim) s.fill_round_rect(ir.expand(2), 8, rgba(255, 255, 255, alpha_dim));
    Rect c(ir.x - (kCellW - kIcon) / 2, ir.y - 4, kCellW, kCellH);
    String l1, l2;
    split_label(it.name.c_str(), kCellW - 12, l1, l2);
    int ly = ir.bottom() + 6;
    draw_label(s, Rect(c.x + 4, ly, c.w - 8, 16), l1.c_str(), selected);
    if (!l2.empty()) draw_label(s, Rect(c.x + 4, ly + 16, c.w - 8, 16), l2.c_str(), selected);
}

int item_at(int x, int y) {
    for (int i = (int)g_items.size() - 1; i >= 0; i--)
        if (hit_rect(g_items[i]).contains(x, y)) return i;
    return -1;
}

bool over_window(int x, int y) {
    for (Window* w : wm::windows())
        if (!w->minimized() && !w->closing_ && w->frame().contains(x, y)) return true;
    return false;
}

void select_item(int i) {
    if (i == g_sel) return;
    if (g_sel >= 0 && g_sel < (int)g_items.size()) damage_item(g_items[g_sel]);
    g_sel = i;
    if (g_sel >= 0) damage_item(g_items[g_sel]);
}

void open_item(int i) {
    if (i < 0 || i >= (int)g_items.size()) return;
    app_open_file(g_items[i].path.c_str());
}

void trash_item(int i) {
    if (i < 0 || i >= (int)g_items.size()) return;
    String trash = String("/Users/") + shell_user_login() + "/.Trash";
    if (!vfs::is_dir(trash.c_str())) vfs::mkdir(trash.c_str());
    String name = g_items[i].name, dest = trash + "/" + name;
    for (int n = 2; vfs::exists(dest.c_str()) && n < 1000; n++) dest = trash + "/" + name + String::format(" %d", n);
    if (vfs::rename(g_items[i].path.c_str(), dest.c_str())) sound_effect(SoundEffect::Trash), wm::notify("Moved to Trash", name.c_str(), "files");
    else wm::notify("Desktop", "Couldn't move the item to the Trash.", "files");
}

void item_menu(int i, int x, int y) {
    String path = g_items[i].path;
    Vector<MenuItem> items;
    MenuItem& o = items.emplace(); o.title = "Open"; o.action = [path] { app_open_file(path.c_str()); };
    MenuItem& f = items.emplace(); f.title = "Show in Files"; f.action = [] { app_open_file(desktop_dir().c_str()); };
    MenuItem& sp = items.emplace(); sp.title = "-";
    MenuItem& t = items.emplace(); t.title = "Move to Trash";
    t.action = [path] {
        for (int k = 0; k < (int)g_items.size(); k++) if (g_items[k].path == path) { trash_item(k); break; }
    };
    wm::popup_menu(x, y, items);
}

}  // namespace

void desktop_tick() {
    if (!wm::shell_visible()) return;
    if (vfs::generation() != g_gen || g_user != shell_user_login()) {
        g_gen = vfs::generation();
        rescan();
    }
}

void desktop_draw(Canvas& s, Rect clip) {
    if (!wm::shell_visible() || g_items.empty()) return;
    s.save();
    s.clip_to(clip);
    for (int i = 0; i < (int)g_items.size(); i++) {
        const Item& it = g_items[i];
        if (g_dragging && i == g_press) continue;
        if (!hit_rect(it).expand(8).intersects(clip)) continue;
        draw_item(s, it, icon_rect(it), i == g_sel, 0);
    }
    if (g_dragging && g_press >= 0 && g_press < (int)g_items.size()) {
        Rect ir(g_drag_pos.x - g_grab_dx, g_drag_pos.y - g_grab_dy, kIcon, kIcon);
        int col, row;
        if (cell_at(g_drag_pos.x, g_drag_pos.y, &col, &row) && !cell_taken(col, row, g_press))
            s.fill_round_rect(cell_rect(col, row).inset(6), 10, rgba(255, 255, 255, 40));
        draw_item(s, g_items[g_press], ir, true, 60);
    }
    s.restore();
}

bool desktop_dragging() { return g_press >= 0; }

bool desktop_mouse(const InputEvent& ev, uint8_t pressed, uint8_t released) {
    int x = ev.x, y = ev.y;
    if (g_press >= 0) {   // a press on an icon owns the mouse until release
        if (g_press >= (int)g_items.size()) { g_press = -1; g_dragging = false; return true; }
        Rect old_r(g_drag_pos.x - g_grab_dx, g_drag_pos.y - g_grab_dy, kIcon, kCellH);
        if (!g_dragging && (abs(x - g_press_at.x) > 4 || abs(y - g_press_at.y) > 4)) g_dragging = true;
        if (g_dragging) {
            wm::damage(old_r.expand(kCellW));
            g_drag_pos = Point{x, y};
            wm::damage(Rect(x - g_grab_dx, y - g_grab_dy, kIcon, kCellH).expand(kCellW));
        }
        if (released & MB_LEFT) {
            if (g_dragging) {
                int col, row;
                Item& it = g_items[g_press];
                damage_item(it);
                if (cell_at(x, y, &col, &row) && !cell_taken(col, row, g_press)) { it.col = col; it.row = row; save_layout(); }
                damage_item(it);
                wm::damage(Rect(x - g_grab_dx, y - g_grab_dy, kIcon, kCellH).expand(kCellW));
            }
            g_press = -1;
            g_dragging = false;
        }
        return true;
    }
    if (!pressed || over_window(x, y)) return false;
    int hit = item_at(x, y);
    if (hit < 0) {
        select_item(-1);
        return false;   // the bare desktop: wm unfocuses windows, right-click gets the desktop menu
    }
    wm::focus(nullptr);
    select_item(hit);
    if (pressed & MB_RIGHT) { item_menu(hit, x, y); return true; }
    if (!(pressed & MB_LEFT)) return true;
    uint64_t now = timer_ms();
    if (g_last_click_item == hit && now - g_last_click_ms < 450) {
        g_last_click_ms = 0;
        open_item(hit);
        return true;
    }
    g_last_click_ms = now;
    g_last_click_item = hit;
    g_press = hit;
    g_press_at = Point{x, y};
    g_drag_pos = g_press_at;
    Rect ir = icon_rect(g_items[hit]);
    g_grab_dx = x - ir.x;
    g_grab_dy = y - ir.y;
    g_dragging = false;
    return true;
}

bool desktop_key(const KeyEvent& e) {
    if (!wm::shell_visible() || g_items.empty() || !e.pressed || e.cmd() || e.alt()) return false;
    int n = (int)g_items.size();
    switch (e.key) {
    case Key::Enter: if (g_sel >= 0) { open_item(g_sel); return true; } return false;
    case Key::Delete: case Key::Backspace: if (g_sel >= 0) { trash_item(g_sel); return true; } return false;
    case Key::Down: case Key::Up: case Key::Left: case Key::Right: {
        if (g_sel < 0) { select_item(0); return true; }
        const Item& cur = g_items[g_sel];
        int dc = e.key == Key::Left ? 1 : e.key == Key::Right ? -1 : 0;   // columns count from the right
        int dr = e.key == Key::Down ? 1 : e.key == Key::Up ? -1 : 0;
        for (int i = 0; i < n; i++)
            if (g_items[i].col == cur.col + dc && g_items[i].row == cur.row + dr) { select_item(i); break; }
        return true;
    }
    case Key::Escape: select_item(-1); return g_sel >= 0;
    default: return false;
    }
}
