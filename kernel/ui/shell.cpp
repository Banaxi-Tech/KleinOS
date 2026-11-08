// Desktop shell: menubar, Dock, Launchpad, Spotlight, Control Center,
// notifications, popup menus.
#include "ui/shell.h"
#include "ui/wm.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "gfx/glass.h"
#include "gfx/wallpaper.h"
#include "drivers/rtc.h"
#include "drivers/power.h"
#include "arch/interrupts.h"
#include "fs/vfs.h"
#include "sys/system.h"
#include "net/net.h"
#include "lib/math.h"
#include "lib/log.h"

using namespace wm;

bool eval_expr(const char* s, double* out);

namespace {

int W, H;
String g_user_name = "Guest", g_user_login = "guest";

// ---------------------------------------------------------------- dock model
constexpr int kDockIcon = 50;
constexpr int kDockPad = 10;
constexpr int kDockGap = 8;
constexpr int kDockMaxIcon = 78;
constexpr int kDockBottom = 8;

struct DockItem {
    const AppInfo* app;     // nullptr = launchpad / separator
    int kind;               // 0 app, 1 launchpad, 2 separator
    bool running;
    float scale;            // animated magnification
    float bounce;           // launch bounce phase
};
Vector<DockItem> g_dock;
bool g_dock_hover;
int g_dock_hover_index = -1;
int g_dock_mouse_x;
Rect g_dock_rect;           // glass pill
Rect g_dock_damage;         // pill + magnification overhang
uint64_t g_dock_press_ms;
int g_dock_pressed = -1;

// ---------------------------------------------------------------- menubar
struct BarMenu {
    String title;
    Rect rect;
    Vector<MenuItem> items;
    bool bold;
};
Vector<BarMenu> g_bar_menus;
Rect g_logo_rect, g_cc_rect, g_search_rect, g_clock_rect, g_net_rect;
int g_last_minute = -1;
int g_open_bar_menu = -1;   // index into g_bar_menus or -2 = logo menu

// ---------------------------------------------------------------- popup menu
struct Popup {
    bool open;
    Rect rect;
    Vector<MenuItem> items;
    int hover;
    int item_h;
    bool from_bar;
};
Popup g_popup;

// ---------------------------------------------------------------- spotlight
struct SpotResult {
    String title, subtitle;
    int kind;                // 0 app, 1 file, 2 calc, 3 web
    const AppInfo* app;
    String path;
};
bool g_spot_open;
Rect g_spot_rect;
String g_spot_query;
Vector<SpotResult> g_spot_results;
int g_spot_sel;

// ---------------------------------------------------------------- launchpad
bool g_lp_open;
float g_lp_anim;            // 0..1
String g_lp_query;
int g_lp_hover = -1;
Vector<const AppInfo*> g_lp_apps;
Vector<Rect> g_lp_rects;

// ---------------------------------------------------------------- control center
bool g_cc_open;
Rect g_cc_panel;
Slider* g_cc_slider_dummy;
int g_cc_drag = -1;         // which slider is being dragged
float g_cc_volume = 0.6f;
bool g_dnd;
bool g_bluetooth;

// ---------------------------------------------------------------- notifications
struct Note {
    String title, body;
    const AppInfo* app;
    uint64_t born;
    Rect rect;
};
Vector<Note> g_notes;

const AppInfo* active_app() {
    Window* w = active_window();
    if (!w) return nullptr;
    return app_find(w->app_id());
}

GlassStyle panel_glass() {
    GlassStyle s = glass_style_panel(theme().dark);
    s.blur *= glass_intensity();
    s.strength *= glass_intensity();
    if (reduce_transparency()) s.tint = with_alpha(s.tint, 240);
    return s;
}

Color shell_text() { return theme().dark ? hex(0xFFFFFF) : hex(0x111114); }
Color shell_text2() { return theme().dark ? rgba(255, 255, 255, 160) : rgba(0, 0, 0, 140); }

void damage_popup() { if (g_popup.open) damage(g_popup.rect.expand(30)); }

// ================================================================ dock
void rebuild_dock() {
    Vector<DockItem> old = g_dock;
    g_dock.clear();
    DockItem lp{nullptr, 1, false, 1, 0};
    g_dock.push(lp);
    // pinned apps sorted by dock_order
    Vector<const AppInfo*> pinned;
    for (int i = 0; i < app_count(); i++) if (app_get(i)->in_dock) pinned.push(app_get(i));
    for (size_t i = 1; i < pinned.size(); i++)
        for (size_t j = i; j > 0 && pinned[j]->dock_order < pinned[j - 1]->dock_order; j--) {
            const AppInfo* t = pinned[j]; pinned[j] = pinned[j - 1]; pinned[j - 1] = t;
        }
    auto running = [](const AppInfo* a) {
        for (Window* w : windows()) if (!w->closing_ && strcmp(w->app_id(), a->id) == 0) return true;
        return false;
    };
    for (const AppInfo* a : pinned) g_dock.push(DockItem{a, 0, running(a), 1, 0});
    bool sep = false;
    for (Window* w : windows()) {
        if (w->closing_ || (w->flags() & (WF_FULLSCREEN | WF_HIDDEN_IN_DOCK))) continue;
        const AppInfo* a = app_find(w->app_id());
        if (!a || a->in_dock) continue;
        bool dup = false;
        for (DockItem& d : g_dock) if (d.app == a) dup = true;
        if (dup) continue;
        if (!sep) { g_dock.push(DockItem{nullptr, 2, false, 1, 0}); sep = true; }
        g_dock.push(DockItem{a, 0, true, 1, 0});
    }
    // keep animation state
    for (DockItem& d : g_dock)
        for (DockItem& o : old)
            if (o.app == d.app && o.kind == d.kind) { d.scale = o.scale; d.bounce = o.bounce; }
}

int dock_item_width(const DockItem& d) { return d.kind == 2 ? 13 : (int)(kDockIcon * d.scale); }

void layout_dock() {
    int total = kDockPad * 2;
    for (size_t i = 0; i < g_dock.size(); i++) total += (g_dock[i].kind == 2 ? 13 : kDockIcon) + (i ? kDockGap : 0);
    int h = kDockIcon + kDockPad * 2 - 2;
    g_dock_rect = Rect((W - total) / 2, H - kDockBottom - h, total, h);
    g_dock_damage = Rect(0, g_dock_rect.y - (kDockMaxIcon - kDockIcon) - 50, W, H - (g_dock_rect.y - (kDockMaxIcon - kDockIcon) - 50));
}

// x positions of icons with magnification (centered on the pill)
void dock_positions(int* xs, int* ws) {
    int total = 0;
    for (size_t i = 0; i < g_dock.size(); i++) { ws[i] = dock_item_width(g_dock[i]); total += ws[i] + (i ? kDockGap : 0); }
    int x = W / 2 - total / 2;
    for (size_t i = 0; i < g_dock.size(); i++) { xs[i] = x; x += ws[i] + kDockGap; }
}

void draw_launchpad_icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0xE8E8EE), hex(0xB8B8C4));
    int cell = r.w / 5;
    Color cols[] = {hex(0xFF5F57), hex(0xFEBC2E), hex(0x28C840), hex(0x0A84FF), hex(0xA550F7), hex(0xFF9F0A), hex(0x30D158), hex(0xF74F9E), hex(0x64D2FF)};
    for (int i = 0; i < 9; i++) {
        int cx = r.x + cell + (i % 3) * cell * 3 / 2, cy = r.y + cell + (i / 3) * cell * 3 / 2;
        c.fill_round_rect(Rect(cx - cell / 2 + cell / 4, cy - cell / 2 + cell / 4, cell, cell), cell * 0.28f, cols[i]);
    }
}

void draw_dock(Canvas& s, Rect clip) {
    if (!g_dock_rect.intersects(clip.expand(kDockMaxIcon))) return;
    GlassStyle gs = panel_glass();
    gs.radius = 24;
    s.save();
    s.clip_to(clip);
    s.draw_shadow(g_dock_rect, 24, 18, rgba(0, 0, 0, 60), 0, 6);
    s.restore();
    glass_render(s, g_dock_rect, gs, clip);
    s.save();
    s.clip_to(clip);
    s.stroke_round_rect(g_dock_rect, 24, theme().dark ? rgba(255, 255, 255, 40) : rgba(255, 255, 255, 110), 1.0f);
    int xs[64], ws[64];
    int n = g_dock.size() < 64 ? (int)g_dock.size() : 64;
    dock_positions(xs, ws);
    int base_y = g_dock_rect.bottom() - kDockPad + 1;
    for (int i = 0; i < n; i++) {
        DockItem& d = g_dock[i];
        if (d.kind == 2) {
            s.fill_rect(Rect(xs[i] + 6, g_dock_rect.y + 12, 1, g_dock_rect.h - 24), theme().dark ? rgba(255, 255, 255, 60) : rgba(0, 0, 0, 50));
            continue;
        }
        int sz = ws[i];
        int bounce = (int)(fabsf(sinf(d.bounce * 3.14159f * 2)) * 18 * (d.bounce > 0 ? 1 : 0));
        Rect ir(xs[i], base_y - sz - bounce, sz, sz);
        if (!ir.expand(10).intersects(clip)) continue;
        if (d.kind == 1) draw_launchpad_icon(s, ir);
        else if (d.app->draw_icon) d.app->draw_icon(s, ir);
        if (g_dock_pressed == i) s.fill_round_rect(ir, sz * 0.225f, rgba(0, 0, 0, 60));
        if (d.running) s.fill_circle(xs[i] + sz / 2.0f, g_dock_rect.bottom() - 4.0f, 2.2f, shell_text());
    }
    // tooltip
    if (g_dock_hover_index >= 0 && g_dock_hover_index < n && g_dock[g_dock_hover_index].kind != 2) {
        DockItem& d = g_dock[g_dock_hover_index];
        const char* name = d.kind == 1 ? "Launchpad" : d.app->name;
        int tw = text_width(name, 13, Font::Medium) + 24;
        int cx = xs[g_dock_hover_index] + ws[g_dock_hover_index] / 2;
        Rect tr(cx - tw / 2, base_y - ws[g_dock_hover_index] - 40, tw, 26);
        s.restore();
        GlassStyle ts = panel_glass();
        ts.radius = 13;
        glass_render(s, tr, ts, clip);
        s.save();
        s.clip_to(clip);
        s.draw_text_in(tr, name, shell_text(), 13, Font::Medium, Align::Center);
    }
    s.restore();
}

void dock_update_scales(uint64_t) {
    int xs[64], ws[64];
    bool changed = false;
    int n = g_dock.size() < 64 ? (int)g_dock.size() : 64;
    // compute targets from mouse position using unmagnified layout
    int total = 0;
    for (int i = 0; i < n; i++) total += (g_dock[i].kind == 2 ? 13 : kDockIcon) + (i ? kDockGap : 0);
    int x = W / 2 - total / 2;
    for (int i = 0; i < n; i++) {
        int w = g_dock[i].kind == 2 ? 13 : kDockIcon;
        float target = 1;
        if (g_dock_hover && dock_magnification() && g_dock[i].kind != 2) {
            float d = fabsf(g_dock_mouse_x - (x + w / 2.0f)) / (kDockIcon * 2.2f);
            if (d < 1) target = 1 + (kDockMaxIcon / (float)kDockIcon - 1) * (0.5f + 0.5f * cosf(d * 3.14159f));
        }
        float ns = g_dock[i].scale + (target - g_dock[i].scale) * 0.35f;
        if (fabsf(ns - target) < 0.01f) ns = target;
        if (ns != g_dock[i].scale) { g_dock[i].scale = ns; changed = true; }
        if (g_dock[i].bounce > 0) {
            g_dock[i].bounce += 0.025f;
            if (g_dock[i].bounce >= 1.5f || g_dock[i].running) { if (g_dock[i].bounce > 0.5f) g_dock[i].bounce = 0; }
            changed = true;
        }
        x += w + kDockGap;
    }
    (void)xs; (void)ws;
    if (changed) damage(g_dock_damage);
}

int dock_hit(int mx, int my) {
    if (my < g_dock_rect.y - (kDockMaxIcon - kDockIcon) || my > H) return -1;
    int xs[64], ws[64];
    int n = g_dock.size() < 64 ? (int)g_dock.size() : 64;
    dock_positions(xs, ws);
    for (int i = 0; i < n; i++)
        if (mx >= xs[i] - kDockGap / 2 && mx < xs[i] + ws[i] + kDockGap / 2) return i;
    return -1;
}

void dock_activate(int i) {
    DockItem& d = g_dock[i];
    if (d.kind == 1) { open_launchpad(); return; }
    if (d.kind != 0) return;
    // focus / restore existing windows
    Window* found = nullptr;
    for (Window* w : windows())
        if (!w->closing_ && strcmp(w->app_id(), d.app->id) == 0) found = w;
    if (found) {
        if (found->minimized_) restore(found);
        focus(found);
        return;
    }
    d.bounce = 0.01f;
    app_launch(d.app->id);
}

// ================================================================ menubar
void open_bar_popup(int index);

void rebuild_menubar() {
    g_bar_menus.clear();
    const AppInfo* app = active_app();
    Window* w = active_window();
    BarMenu& appm = g_bar_menus.emplace();
    appm.title = app ? app->name : "Finder";
    if (!app && !w) appm.title = "Desktop";
    appm.bold = true;
    {
        String about = String("About ") + appm.title;
        MenuItem& a = appm.items.emplace();
        a.title = about;
        const char* id = app ? app->id : nullptr;
        a.action = [id] { if (id) notify(app_find(id)->name, app_find(id)->description[0] ? app_find(id)->description : "A KleinOS app", id); };
        MenuItem& sep = appm.items.emplace(); sep.title = "-";
        MenuItem& set = appm.items.emplace(); set.title = "Settings…"; set.shortcut = "⌘,";
        set.action = [] { app_launch("settings"); };
        MenuItem& sep2 = appm.items.emplace(); sep2.title = "-";
        MenuItem& hide = appm.items.emplace(); hide.title = String("Hide ") + appm.title; hide.shortcut = "⌘M";
        hide.action = [] { if (active_window()) minimize(active_window()); };
        MenuItem& q = appm.items.emplace(); q.title = String("Quit ") + appm.title; q.shortcut = "⌘Q";
        q.enabled = w != nullptr;
        q.action = [] {
            Window* aw = active_window();
            if (!aw) return;
            String id = aw->app_id();
            Vector<Window*> ws = windows();
            for (Window* x : ws) if (id == x->app_id()) x->close();
        };
    }
    if (w) {
        Vector<Menu> menus;
        w->build_menus(menus);
        bool has_file = false, has_edit = false;
        for (Menu& m : menus) { if (m.title == "File") has_file = true; if (m.title == "Edit") has_edit = true; }
        if (!has_file) {
            BarMenu& f = g_bar_menus.emplace();
            f.title = "File";
            MenuItem& c = f.items.emplace(); c.title = "Close Window"; c.shortcut = "⌘W";
            c.action = [] { if (active_window()) active_window()->close(); };
        }
        for (Menu& m : menus) {
            BarMenu& b = g_bar_menus.emplace();
            b.title = m.title;
            b.items = m.items;
        }
        if (!has_edit) {
            BarMenu& e = g_bar_menus.emplace();
            e.title = "Edit";
            const char* names[] = {"Undo", "-", "Cut", "Copy", "Paste", "Select All"};
            const char* keys[] = {"⌘Z", "", "⌘X", "⌘C", "⌘V", "⌘A"};
            const char chars[] = {'z', 0, 'x', 'c', 'v', 'a'};
            for (int i = 0; i < 6; i++) {
                MenuItem& it = e.items.emplace();
                it.title = names[i];
                it.shortcut = keys[i];
                char ch = chars[i];
                it.action = [ch] {
                    Window* aw = active_window();
                    if (!aw) return;
                    KeyEvent k{Key::Char, (uint32_t)ch, MOD_CTRL, true};
                    aw->on_key(k);
                };
            }
        }
        BarMenu& win = g_bar_menus.emplace();
        win.title = "Window";
        MenuItem& mn = win.items.emplace(); mn.title = "Minimize"; mn.shortcut = "⌘M";
        mn.action = [] { if (active_window()) minimize(active_window()); };
        MenuItem& zm = win.items.emplace(); zm.title = "Zoom";
        zm.action = [] { if (active_window()) toggle_zoom(active_window()); };
        MenuItem& sp = win.items.emplace(); sp.title = "-";
        for (Window* x : windows()) {
            if (x->closing_ || (x->flags() & WF_FULLSCREEN)) continue;
            MenuItem& it = win.items.emplace();
            it.title = x->title();
            it.checked = x == w;
            it.action = [x] { restore(x); focus(x); };
        }
    } else {
        BarMenu& f = g_bar_menus.emplace();
        f.title = "File";
        MenuItem& nf = f.items.emplace(); nf.title = "New Files Window"; nf.shortcut = "⌘N";
        nf.action = [] { app_launch("files"); };
        BarMenu& go = g_bar_menus.emplace();
        go.title = "Go";
        const char* places[] = {"Applications", "Documents", "Desktop", "Downloads", "Home"};
        for (const char* p : places) {
            MenuItem& it = go.items.emplace();
            it.title = p;
            it.action = [] { app_launch("files"); };
        }
    }
    BarMenu& help = g_bar_menus.emplace();
    help.title = "Help";
    MenuItem& hs = help.items.emplace(); hs.title = "KleinOS Help";
    hs.action = [] { if (!app_launch("help")) notify("KleinOS Help", "Press Super+Space to search, drag windows by their glass title bar."); };
    MenuItem& kb = help.items.emplace(); kb.title = "Keyboard Shortcuts";
    kb.action = [] { notify("Shortcuts", "Super+Space Spotlight · F4 Launchpad · Super+W close · Super+Q quit · Alt+Tab switch"); };

    // layout
    int x = 44;
    for (BarMenu& m : g_bar_menus) {
        int w2 = text_width(m.title.c_str(), 14, m.bold ? Font::Bold : Font::Medium) + 20;
        m.rect = Rect(x, 0, w2, kMenubarHeight);
        x += w2;
    }
    g_logo_rect = Rect(8, 0, 36, kMenubarHeight);
    damage(Rect(0, 0, W, kMenubarHeight + 30));
}

void logo_menu_items(Vector<MenuItem>& items) {
    auto add = [&](const char* t, Func<void()> f, const char* sc = "") {
        MenuItem& m = items.emplace(); m.title = t; m.action = f; m.shortcut = sc;
    };
    auto sep = [&] { MenuItem& m = items.emplace(); m.title = "-"; };
    add("About KleinOS", [] { if (!app_launch("about")) notify("KleinOS", system_version_string()); });
    sep();
    add("System Settings…", [] { app_launch("settings"); });
    add("App Library", [] { open_launchpad(); }, "F4");
    sep();
    add("Force Quit…", [] { app_launch("activity"); });
    sep();
    add("Lock Screen", [] { request_logout(); });
    String lo = String("Log Out ") + g_user_name + "…";
    add(lo.c_str(), [] { request_logout(); });
    sep();
    add("Restart…", [] { power_reboot(); });
    add("Shut Down…", [] { power_shutdown(); });
}

void draw_logo(Canvas& s, Rect r, Color c) {
    // KleinOS mark: a rounded "K" made of glass-like strokes inside a soft circle
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
    s.draw_line(cx - 4.5f, cy - 7, cx - 4.5f, cy + 7, c, 2.6f);
    s.draw_line(cx - 3.5f, cy + 1, cx + 5, cy - 7, c, 2.6f);
    s.draw_line(cx - 1.5f, cy - 1, cx + 5.5f, cy + 7, c, 2.6f);
}

void draw_menubar(Canvas& s, Rect clip) {
    Rect bar(0, 0, W, kMenubarHeight);
    if (!bar.intersects(clip)) return;
    GlassStyle gs = glass_style_bar(theme().dark);
    gs.blur *= glass_intensity();
    if (reduce_transparency()) gs.tint = with_alpha(gs.tint, 235);
    glass_render(s, bar, gs, clip);
    s.save();
    s.clip_to(clip);
    s.fill_rect(Rect(0, kMenubarHeight - 1, W, 1), theme().dark ? rgba(0, 0, 0, 80) : rgba(255, 255, 255, 80));
    Color tc = shell_text();
    if (g_open_bar_menu == -2) s.fill_round_rect(g_logo_rect.inset(2, 4), 6, rgba(255, 255, 255, theme().dark ? 40 : 90));
    draw_logo(s, g_logo_rect, tc);
    for (int i = 0; i < (int)g_bar_menus.size(); i++) {
        BarMenu& m = g_bar_menus[i];
        if (g_open_bar_menu == i) s.fill_round_rect(m.rect.inset(2, 4), 6, rgba(255, 255, 255, theme().dark ? 40 : 90));
        s.draw_text_in(m.rect, m.title.c_str(), tc, 14, m.bold ? Font::Bold : Font::Medium, Align::Center);
    }
    // right side: clock, search, control center, network
    DateTime dt = rtc_now();
    char clock[64];
    snprintf(clock, sizeof(clock), "%.3s %d %.3s  %02d:%02d", weekday_name(dt.weekday), dt.day, month_name(dt.month), dt.hour, dt.minute);
    int cw = text_width(clock, 14, Font::Medium) + 16;
    g_clock_rect = Rect(W - cw - 8, 0, cw, kMenubarHeight);
    s.draw_text_in(g_clock_rect, clock, tc, 14, Font::Medium, Align::Center);
    g_cc_rect = Rect(g_clock_rect.x - 34, 0, 30, kMenubarHeight);
    g_search_rect = Rect(g_cc_rect.x - 32, 0, 30, kMenubarHeight);
    g_net_rect = Rect(g_search_rect.x - 32, 0, 30, kMenubarHeight);
    if (g_cc_open) s.fill_round_rect(g_cc_rect.inset(2, 4), 6, rgba(255, 255, 255, theme().dark ? 40 : 90));
    if (g_spot_open) s.fill_round_rect(g_search_rect.inset(2, 4), 6, rgba(255, 255, 255, theme().dark ? 40 : 90));
    // control center glyph: two toggles
    {
        Rect r = g_cc_rect;
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        s.stroke_round_rect(Rect(cx - 8, cy - 7, 16, 6), 3, tc, 1.4f);
        s.fill_circle(cx - 5, cy - 4, 2, tc);
        s.stroke_round_rect(Rect(cx - 8, cy + 1, 16, 6), 3, tc, 1.4f);
        s.fill_circle(cx + 5, cy + 4, 2, tc);
    }
    draw_symbol(s, Rect(g_search_rect.x + 7, 7, 16, 16), Symbol::Search, tc, 1.7f);
    draw_symbol(s, Rect(g_net_rect.x + 6, 6, 18, 18), net::is_up() ? Symbol::Wifi : Symbol::WifiOff, tc, 1.6f);
    s.restore();
}

// ================================================================ popup menus
void layout_popup(int x, int y) {
    int w = 200;
    int h = 8;
    g_popup.item_h = 26;
    for (MenuItem& m : g_popup.items) {
        int tw = text_width(m.title.c_str(), 14) + 60 + (m.shortcut.empty() ? 0 : text_width(m.shortcut.c_str(), 13) + 20);
        if (tw > w) w = tw;
        h += m.title == "-" ? 11 : g_popup.item_h;
    }
    h += 0;
    if (x + w > W - 6) x = W - 6 - w;
    if (y + h > H - 6) y = kmax(kMenubarHeight, H - 6 - h);
    g_popup.rect = Rect(x, y, w, h);
}

void close_popup() {
    if (!g_popup.open) return;
    damage_popup();
    g_popup.open = false;
    g_popup.items.clear();
    if (g_open_bar_menu != -1) { g_open_bar_menu = -1; damage(Rect(0, 0, W, kMenubarHeight + 20)); }
}

void draw_popup(Canvas& s, Rect clip) {
    if (!g_popup.open || !g_popup.rect.intersects(clip)) return;
    Rect r = g_popup.rect;
    s.save();
    s.clip_to(clip);
    s.draw_shadow(r, 12, 20, rgba(0, 0, 0, 80), 0, 8);
    s.restore();
    GlassStyle gs = panel_glass();
    gs.radius = 12;
    gs.tint = theme().dark ? rgba(40, 40, 46, 170) : rgba(250, 250, 252, 150);
    glass_render(s, r, gs, clip);
    s.save();
    s.clip_to(clip);
    s.stroke_round_rect(r, 12, theme().dark ? rgba(255, 255, 255, 40) : rgba(0, 0, 0, 30), 1);
    int y = r.y + 4;
    for (int i = 0; i < (int)g_popup.items.size(); i++) {
        MenuItem& m = g_popup.items[i];
        if (m.title == "-") {
            s.fill_rect(Rect(r.x + 12, y + 5, r.w - 24, 1), theme().separator);
            y += 11;
            continue;
        }
        Rect ir(r.x + 5, y, r.w - 10, g_popup.item_h);
        bool hov = i == g_popup.hover && m.enabled;
        if (hov) s.fill_round_rect(ir, 6, theme().accent);
        Color fg = !m.enabled ? theme().text_tertiary : hov ? hex(0xFFFFFF) : theme().text;
        if (m.checked) draw_symbol(s, Rect(ir.x + 6, ir.y + 6, 14, 14), Symbol::Check, fg, 1.8f);
        s.draw_text_in(Rect(ir.x + 26, ir.y, ir.w - 30, ir.h), m.title.c_str(), fg, 14);
        if (!m.shortcut.empty())
            s.draw_text_in(Rect(ir.x, ir.y, ir.w - 10, ir.h), m.shortcut.c_str(), hov ? fg : theme().text_secondary, 13, Font::Regular, Align::Right);
        y += g_popup.item_h;
    }
    s.restore();
}

int popup_hit(int mx, int my) {
    if (!g_popup.rect.contains(mx, my)) return -1;
    int y = g_popup.rect.y + 4;
    for (int i = 0; i < (int)g_popup.items.size(); i++) {
        int h = g_popup.items[i].title == "-" ? 11 : g_popup.item_h;
        if (my >= y && my < y + h) return g_popup.items[i].title == "-" ? -1 : i;
        y += h;
    }
    return -1;
}

void open_bar_popup(int index) {
    close_popup();
    g_popup.open = true;
    g_popup.hover = -1;
    g_popup.from_bar = true;
    int x;
    if (index == -2) { logo_menu_items(g_popup.items); x = g_logo_rect.x; }
    else { g_popup.items = g_bar_menus[index].items; x = g_bar_menus[index].rect.x; }
    g_open_bar_menu = index;
    layout_popup(x, kMenubarHeight + 2);
    damage_popup();
    damage(Rect(0, 0, W, kMenubarHeight + 20));
}

// ================================================================ spotlight

void spot_search() {
    g_spot_results.clear();
    g_spot_sel = 0;
    String q = g_spot_query.trim().lower();
    if (q.empty()) return;
    double v;
    if (eval_expr(g_spot_query.c_str(), &v)) {
        SpotResult& r = g_spot_results.emplace();
        char buf[64];
        if (v == (double)(long long)v && fabs(v) < 1e15) snprintf(buf, sizeof(buf), "= %lld", (long long)v);
        else snprintf(buf, sizeof(buf), "= %.6g", v);
        r.title = buf;
        r.subtitle = "Calculator";
        r.kind = 2;
    }
    for (int i = 0; i < app_count(); i++) {
        const AppInfo* a = app_get(i);
        String name = String(a->name).lower();
        String id = String(a->id);
        if (name.find(q.c_str()) >= 0 || id.find(q.c_str()) >= 0 || String(a->category).lower().find(q.c_str()) == 0) {
            SpotResult& r = g_spot_results.emplace();
            r.title = a->name;
            r.subtitle = a->category;
            r.kind = 0;
            r.app = a;
        }
        if (g_spot_results.size() > 8) break;
    }
    // files in /Users
    Vector<vfs::Node*> stack;
    if (vfs::Node* u = vfs::lookup("/Users")) stack.push(u);
    int files = 0;
    while (!stack.empty() && files < 4) {
        vfs::Node* n = stack.back();
        stack.pop();
        for (vfs::Node* c : n->children) {
            if (c->is_dir) stack.push(c);
            else if (c->name.lower().find(q.c_str()) >= 0) {
                SpotResult& r = g_spot_results.emplace();
                r.title = c->name;
                r.path = vfs::path_of(c);
                r.subtitle = r.path;
                r.kind = 1;
                if (++files >= 4) break;
            }
        }
    }
    SpotResult& web = g_spot_results.emplace();
    web.title = String("Search the web for \"") + g_spot_query + "\"";
    web.subtitle = "Browser";
    web.kind = 3;
}

void spot_layout() {
    int w = 660;
    int h = 56 + (g_spot_results.empty() ? 0 : (int)g_spot_results.size() * 44 + 12);
    g_spot_rect = Rect((W - w) / 2, H / 5, w, h);
}

void spot_close() {
    if (!g_spot_open) return;
    damage(g_spot_rect.expand(40));
    g_spot_open = false;
    damage(Rect(0, 0, W, kMenubarHeight + 20));
}

void spot_activate(int i) {
    if (i < 0 || i >= (int)g_spot_results.size()) return;
    SpotResult r = g_spot_results[i];
    String q = g_spot_query;
    spot_close();
    switch (r.kind) {
    case 0: app_launch(r.app->id); break;
    case 1: {
        String ext = vfs::extension(r.path.c_str());
        const char* app = (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" || ext == "gif") ? "photos"
                          : (ext == "html" || ext == "htm") ? "browser" : "textedit";
        (void)app;
        app_open_file(r.path.c_str());
        break;
    }
    case 2: wm::clipboard_set(r.title.c_str() + 2); notify("Calculator", "Result copied to clipboard"); break;
    case 3: {
        String url = String("http://frogfind.com/?q=") + q;
        for (size_t k = 0; k < url.size(); k++) if (url[k] == ' ') url[k] = '+';
        app_launch("browser");
        open_file_hook("browser", url.c_str());
        break;
    }
    }
}

void draw_spotlight(Canvas& s, Rect clip) {
    if (!g_spot_open) return;
    spot_layout();
    Rect r = g_spot_rect;
    if (!r.expand(30).intersects(clip)) return;
    s.save();
    s.clip_to(clip);
    s.draw_shadow(r, 22, 30, rgba(0, 0, 0, 90), 0, 12);
    s.restore();
    GlassStyle gs = panel_glass();
    gs.radius = 22;
    gs.tint = theme().dark ? rgba(36, 36, 42, 150) : rgba(255, 255, 255, 120);
    glass_render(s, r, gs, clip);
    s.save();
    s.clip_to(clip);
    s.stroke_round_rect(r, 22, theme().dark ? rgba(255, 255, 255, 50) : rgba(255, 255, 255, 140), 1);
    draw_symbol(s, Rect(r.x + 18, r.y + 16, 24, 24), Symbol::Search, shell_text2(), 2.0f);
    if (g_spot_query.empty()) s.draw_text(r.x + 54, r.y + 12, "Spotlight Search", shell_text2(), 22);
    else {
        int adv = s.draw_text(r.x + 54, r.y + 12, g_spot_query.c_str(), shell_text(), 22);
        if ((timer_ms() / 530) % 2 == 0) s.fill_rect(Rect(r.x + 55 + adv, r.y + 16, 2, 26), theme().accent);
    }
    if (g_spot_query.empty() && (timer_ms() / 530) % 2 == 0) s.fill_rect(Rect(r.x + 53, r.y + 16, 2, 26), theme().accent);
    if (!g_spot_results.empty()) {
        s.fill_rect(Rect(r.x + 14, r.y + 56, r.w - 28, 1), theme().separator);
        int y = r.y + 62;
        for (int i = 0; i < (int)g_spot_results.size(); i++) {
            SpotResult& sr = g_spot_results[i];
            Rect row(r.x + 8, y, r.w - 16, 42);
            bool sel = i == g_spot_sel;
            if (sel) s.fill_round_rect(row, 10, theme().accent);
            Rect ic(row.x + 8, row.y + 5, 32, 32);
            if (sr.kind == 0 && sr.app->draw_icon) sr.app->draw_icon(s, ic);
            else if (sr.kind == 1) draw_symbol(s, ic.inset(4), Symbol::File, sel ? hex(0xFFFFFF) : shell_text(), 1.6f);
            else if (sr.kind == 2) icon_text(s, ic, "=", sel ? hex(0xFFFFFF) : shell_text(), 0.7f);
            else draw_symbol(s, ic.inset(4), Symbol::Globe, sel ? hex(0xFFFFFF) : shell_text(), 1.6f);
            Color fg = sel ? hex(0xFFFFFF) : shell_text();
            s.draw_text_in(Rect(row.x + 52, row.y + 2, row.w - 200, 22), sr.title.c_str(), fg, 15, Font::Medium);
            s.draw_text_in(Rect(row.x + 52, row.y + 21, row.w - 200, 18), sr.subtitle.c_str(), sel ? rgba(255, 255, 255, 200) : shell_text2(), 12);
            y += 44;
        }
    }
    s.restore();
}

// ================================================================ launchpad
void lp_filter() {
    g_lp_apps.clear();
    String q = g_lp_query.lower();
    for (int i = 0; i < app_count(); i++) {
        const AppInfo* a = app_get(i);
        if (q.empty() || String(a->name).lower().find(q.c_str()) >= 0) g_lp_apps.push(a);
    }
    // alphabetical
    for (size_t i = 1; i < g_lp_apps.size(); i++)
        for (size_t j = i; j > 0 && strcasecmp(g_lp_apps[j]->name, g_lp_apps[j - 1]->name) < 0; j--) {
            const AppInfo* t = g_lp_apps[j]; g_lp_apps[j] = g_lp_apps[j - 1]; g_lp_apps[j - 1] = t;
        }
    int cols = 7, icon = 84, cellw = (W - 240) / cols, cellh = 128;
    g_lp_rects.clear();
    for (int i = 0; i < (int)g_lp_apps.size(); i++) {
        int cx = 120 + (i % cols) * cellw + cellw / 2, y = 130 + (i / cols) * cellh;
        g_lp_rects.push(Rect(cx - icon / 2, y, icon, icon));
    }
}

void lp_close() {
    if (!g_lp_open) return;
    g_lp_open = false;
    damage_all();
}

void draw_launchpad(Canvas& s, Rect clip) {
    if (!g_lp_open) return;
    GlassStyle gs = glass_style_panel(theme().dark);
    gs.radius = 0;
    gs.blur = 24;
    gs.relative = false;
    gs.depth = 0; gs.strength = 0; gs.dome = 1;
    gs.tint = rgba(0, 0, 0, 70);
    gs.shine = 0;
    glass_render(s, Rect(0, 0, W, H), gs, clip);
    s.save();
    s.clip_to(clip);
    // search field
    Rect sf((W - 260) / 2, 60, 260, 32);
    s.fill_round_rect(sf, 16, rgba(255, 255, 255, 40));
    s.stroke_round_rect(sf, 16, rgba(255, 255, 255, 70), 1);
    draw_symbol(s, Rect(sf.x + 10, sf.y + 8, 16, 16), Symbol::Search, rgba(255, 255, 255, 200), 1.6f);
    s.draw_text_in(Rect(sf.x + 34, sf.y, sf.w - 40, sf.h), g_lp_query.empty() ? "Search" : g_lp_query.c_str(),
                   g_lp_query.empty() ? rgba(255, 255, 255, 150) : hex(0xFFFFFF), 14);
    for (int i = 0; i < (int)g_lp_apps.size(); i++) {
        Rect ir = g_lp_rects[i];
        if (!ir.expand(30).intersects(clip)) continue;
        Rect draw_r = i == g_lp_hover ? ir.expand(3) : ir;
        g_lp_apps[i]->draw_icon(s, draw_r);
        s.draw_text_in(Rect(ir.x - 30, ir.bottom() + 6, ir.w + 60, 20), g_lp_apps[i]->name, hex(0xFFFFFF), 13, Font::Medium, Align::Center);
    }
    s.restore();
}

// ================================================================ control center
struct CcTile { Rect r; int id; };
Vector<CcTile> g_cc_tiles;

void cc_layout() {
    int w = 330, h = 330;
    g_cc_panel = Rect(W - w - 10, kMenubarHeight + 6, w, h);
    g_cc_tiles.clear();
    int x = g_cc_panel.x + 14, y = g_cc_panel.y + 14;
    int big = (w - 28 - 12) / 2;
    g_cc_tiles.push(CcTile{Rect(x, y, big, 120), 0});               // connectivity block
    g_cc_tiles.push(CcTile{Rect(x + big + 12, y, big, 54), 1});     // dark mode
    g_cc_tiles.push(CcTile{Rect(x + big + 12, y + 66, big, 54), 2}); // focus
    g_cc_tiles.push(CcTile{Rect(x, y + 132, w - 28, 54), 3});       // glass slider
    g_cc_tiles.push(CcTile{Rect(x, y + 198, w - 28, 54), 4});       // volume slider
    g_cc_tiles.push(CcTile{Rect(x, y + 264, (w - 28 - 24) / 3, 38), 5});   // wallpaper
    g_cc_tiles.push(CcTile{Rect(x + (w - 28 - 24) / 3 + 12, y + 264, (w - 28 - 24) / 3, 38), 6});  // lock
    g_cc_tiles.push(CcTile{Rect(x + 2 * ((w - 28 - 24) / 3 + 12), y + 264, (w - 28 - 24) / 3, 38), 7});  // power
}

void cc_close() {
    if (!g_cc_open) return;
    damage(g_cc_panel.expand(40));
    g_cc_open = false;
    damage(Rect(0, 0, W, kMenubarHeight + 20));
}

void draw_tile_bg(Canvas& s, Rect r, bool active) {
    s.fill_round_rect(r, 16, active ? with_alpha(theme().accent, 230) : (theme().dark ? rgba(255, 255, 255, 30) : rgba(255, 255, 255, 120)));
    s.stroke_round_rect(r, 16, rgba(255, 255, 255, theme().dark ? 30 : 140), 1);
}

void draw_round_btn(Canvas& s, int cx, int cy, bool on, Symbol sym) {
    s.fill_circle(cx, cy, 15, on ? theme().accent : (theme().dark ? rgba(255, 255, 255, 40) : rgba(0, 0, 0, 25)));
    draw_symbol(s, Rect(cx - 9, cy - 9, 18, 18), sym, on ? hex(0xFFFFFF) : shell_text(), 1.6f);
}

void draw_cc(Canvas& s, Rect clip) {
    if (!g_cc_open) return;
    cc_layout();
    Rect r = g_cc_panel;
    if (!r.expand(30).intersects(clip)) return;
    s.save();
    s.clip_to(clip);
    s.draw_shadow(r, 26, 24, rgba(0, 0, 0, 80), 0, 10);
    s.restore();
    GlassStyle gs = panel_glass();
    gs.radius = 26;
    glass_render(s, r, gs, clip);
    s.save();
    s.clip_to(clip);
    s.stroke_round_rect(r, 26, theme().dark ? rgba(255, 255, 255, 40) : rgba(255, 255, 255, 140), 1);
    Color tc = shell_text(), t2 = shell_text2();
    for (CcTile& t : g_cc_tiles) {
        switch (t.id) {
        case 0: {
            draw_tile_bg(s, t.r, false);
            bool up = net::is_up();
            draw_round_btn(s, t.r.x + 26, t.r.y + 30, up, Symbol::Wifi);
            s.draw_text(t.r.x + 48, t.r.y + 12, "Network", tc, 13, Font::Medium);
            s.draw_text(t.r.x + 48, t.r.y + 30, up ? "Connected" : "Offline", t2, 12);
            draw_round_btn(s, t.r.x + 26, t.r.y + 82, g_bluetooth, Symbol::Bluetooth);
            s.draw_text(t.r.x + 48, t.r.y + 64, "Bluetooth", tc, 13, Font::Medium);
            s.draw_text(t.r.x + 48, t.r.y + 82, g_bluetooth ? "On" : "Off", t2, 12);
            break;
        }
        case 1:
            draw_tile_bg(s, t.r, theme().dark);
            draw_symbol(s, Rect(t.r.x + 14, t.r.y + 17, 20, 20), theme().dark ? Symbol::Moon : Symbol::Sun, theme().dark ? hex(0xFFFFFF) : tc, 1.7f);
            if (theme().dark) s.fill_circle(t.r.x + 24, t.r.y + 27, 7, hex(0xFFFFFF));
            s.draw_text_in(Rect(t.r.x + 42, t.r.y, t.r.w - 46, t.r.h), "Dark Mode", theme().dark ? hex(0xFFFFFF) : tc, 13, Font::Medium);
            break;
        case 2:
            draw_tile_bg(s, t.r, g_dnd);
            draw_symbol(s, Rect(t.r.x + 14, t.r.y + 17, 20, 20), Symbol::Bell, g_dnd ? hex(0xFFFFFF) : tc, 1.7f);
            s.draw_text_in(Rect(t.r.x + 42, t.r.y, t.r.w - 46, t.r.h), "Focus", g_dnd ? hex(0xFFFFFF) : tc, 13, Font::Medium);
            break;
        case 3:
        case 4: {
            draw_tile_bg(s, t.r, false);
            const char* label = t.id == 3 ? "Liquid Glass" : "Sound";
            float v = t.id == 3 ? glass_intensity() / 1.5f : g_cc_volume;
            s.draw_text(t.r.x + 14, t.r.y + 6, label, tc, 12, Font::Medium);
            Rect track(t.r.x + 14, t.r.y + 28, t.r.w - 28, 16);
            s.fill_round_rect(track, 8, theme().dark ? rgba(255, 255, 255, 40) : rgba(0, 0, 0, 25));
            int fw = (int)(track.w * v);
            if (fw > 8) s.fill_round_rect(Rect(track.x, track.y, fw, track.h), 8, hex(0xFFFFFF));
            draw_symbol(s, Rect(track.x + 3, track.y + 1, 14, 14), t.id == 3 ? Symbol::Sun : Symbol::Volume, rgba(0, 0, 0, 160), 1.4f);
            break;
        }
        case 5: case 6: case 7: {
            draw_tile_bg(s, t.r, false);
            Symbol sy = t.id == 5 ? Symbol::Image : t.id == 6 ? Symbol::Lock : Symbol::Power;
            draw_symbol(s, Rect(t.r.x + t.r.w / 2 - 9, t.r.y + 10, 18, 18), sy, tc, 1.6f);
            break;
        }
        }
    }
    s.restore();
}

void cc_slider_set(int id, int mx) {
    for (CcTile& t : g_cc_tiles)
        if (t.id == id) {
            float v = kclamp((float)(mx - (t.r.x + 14)) / (t.r.w - 28), 0.0f, 1.0f);
            if (id == 3) set_glass_intensity(v * 1.5f);
            else g_cc_volume = v;
            damage(g_cc_panel.expand(30));
        }
}

bool cc_click(int mx, int my) {
    for (CcTile& t : g_cc_tiles) {
        if (!t.r.contains(mx, my)) continue;
        switch (t.id) {
        case 0:
            if (my > t.r.y + 56) { g_bluetooth = !g_bluetooth; }
            else app_launch("settings");
            break;
        case 1: theme_set_dark(!theme().dark); break;
        case 2: g_dnd = !g_dnd; break;
        case 3: case 4: g_cc_drag = t.id; cc_slider_set(t.id, mx); break;
        case 5: set_wallpaper(wallpaper() + 1); break;
        case 6: cc_close(); request_logout(); return true;
        case 7: {
            Vector<MenuItem> items;
            MenuItem& a = items.emplace(); a.title = "Restart"; a.action = [] { power_reboot(); };
            MenuItem& b = items.emplace(); b.title = "Shut Down"; b.action = [] { power_shutdown(); };
            MenuItem& c = items.emplace(); c.title = "Log Out"; c.action = [] { request_logout(); };
            popup_menu(t.r.x, t.r.bottom() + 6, items);
            return true;
        }
        }
        damage(g_cc_panel.expand(30));
        return true;
    }
    return g_cc_panel.contains(mx, my);
}

// ================================================================ notifications
void layout_notes() {
    int y = kMenubarHeight + 10;
    for (Note& n : g_notes) {
        int h = 76;
        n.rect = Rect(W - 370, y, 356, h);
        y += h + 10;
    }
}

void draw_notes(Canvas& s, Rect clip) {
    uint64_t now = timer_ms();
    for (Note& n : g_notes) {
        // slide in from the right during the first 250ms
        int age = (int)(now - n.born);
        int slide = age < 250 ? (int)((1 - age / 250.0f) * (1 - age / 250.0f) * 380) : 0;
        Rect r = n.rect.offset(slide, 0);
        if (!r.expand(30).intersects(clip)) continue;
        s.save();
        s.clip_to(clip);
        s.draw_shadow(r, 20, 18, rgba(0, 0, 0, 70), 0, 6);
        s.restore();
        GlassStyle gs = panel_glass();
        gs.radius = 20;
        glass_render(s, r, gs, clip);
        s.save();
        s.clip_to(clip);
        s.stroke_round_rect(r, 20, theme().dark ? rgba(255, 255, 255, 40) : rgba(255, 255, 255, 140), 1);
        Rect ic(r.x + 14, r.y + 18, 40, 40);
        if (n.app && n.app->draw_icon) n.app->draw_icon(s, ic);
        else { icon_base(s, ic, hex(0x5E5CE6), hex(0x3634A3)); draw_logo(s, ic, hex(0xFFFFFF)); }
        s.draw_text_in(Rect(r.x + 66, r.y + 12, r.w - 80, 22), n.title.c_str(), shell_text(), 14, Font::Bold);
        s.draw_text_wrapped(Rect(r.x + 66, r.y + 34, r.w - 80, 40), n.body.c_str(), shell_text(), 13);
        s.restore();
    }
}

}  // namespace

// ================================================================ shell API
void shell_set_user(const char* full, const char* login) { g_user_name = full; g_user_login = login; }
const char* shell_user_name() { return g_user_name.c_str(); }
const char* shell_user_login() { return g_user_login.c_str(); }

void shell_init() {
    W = screen_width();
    H = screen_height();
    rebuild_dock();
    layout_dock();
    rebuild_menubar();
}

int shell_dock_height() { return H - g_dock_rect.y + 4; }

void shell_windows_changed() {
    rebuild_dock();
    layout_dock();
    rebuild_menubar();
    damage(g_dock_damage);
}

void shell_tick(uint64_t now) {
    if (shell_visible()) {
        DateTime dt = rtc_now();
        if (dt.minute != g_last_minute) {
            g_last_minute = dt.minute;
            damage(Rect(0, 0, W, kMenubarHeight));
        }
        dock_update_scales(now);
        static int net_state = -1;
        if (net::is_up() != net_state) { net_state = net::is_up(); damage(Rect(0, 0, W, kMenubarHeight)); }
    }
    if (g_spot_open && (now / 530) != ((now - 16) / 530)) damage(Rect(g_spot_rect.x, g_spot_rect.y, g_spot_rect.w, 56));
    // notifications: animate + expire
    bool changed = false;
    for (size_t i = 0; i < g_notes.size();) {
        uint64_t age = now - g_notes[i].born;
        if (age < 300) { damage(g_notes[i].rect.expand(20).unite(g_notes[i].rect.offset(380, 0))); }
        if (age > 6000) { damage(g_notes[i].rect.expand(30)); g_notes.remove(i); changed = true; }
        else i++;
    }
    if (changed) { layout_notes(); for (Note& n : g_notes) damage(n.rect.expand(30)); damage(Rect(W - 400, kMenubarHeight, 400, 400)); }
}

void shell_glass_rects(Vector<Rect>& out) {
    if (shell_visible()) {
        out.push(Rect(0, 0, W, kMenubarHeight).expand(glass_margin(glass_style_bar(false))));
        out.push(g_dock_rect.expand(glass_margin(panel_glass())));
        out.push(Rect(g_dock_rect.x - 200, g_dock_rect.y - 80, g_dock_rect.w + 400, 80));   // tooltips
    }
    int m = glass_margin(panel_glass());
    if (g_popup.open) out.push(g_popup.rect.expand(m));
    if (g_spot_open) { spot_layout(); out.push(g_spot_rect.expand(m)); }
    if (g_cc_open) { cc_layout(); out.push(g_cc_panel.expand(m)); }
    if (g_lp_open) out.push(Rect(0, 0, W, H));
    for (Note& n : g_notes) out.push(n.rect.expand(m).unite(n.rect.offset(380, 0).expand(m)));
}

void shell_draw_desktop(Canvas& s, Rect clip) {}

void shell_draw(Canvas& s, Rect clip) {
    if (g_lp_open) return;
    draw_dock(s, clip);
    draw_menubar(s, clip);
}

void shell_draw_overlays(Canvas& s, Rect clip) {
    draw_launchpad(s, clip);
    draw_cc(s, clip);
    draw_spotlight(s, clip);
    draw_notes(s, clip);
    draw_popup(s, clip);
}

bool shell_mouse(const InputEvent& ev, uint8_t pressed, uint8_t released) {
    int mx = ev.x, my = ev.y;
    // ---- popup menu
    if (g_popup.open) {
        int h = popup_hit(mx, my);
        if (h != g_popup.hover) { g_popup.hover = h; damage_popup(); }
        if (pressed) {
            if (g_popup.rect.contains(mx, my)) return true;
            // clicking another bar menu switches menus
            if (g_popup.from_bar && my < kMenubarHeight) {
                for (int i = 0; i < (int)g_bar_menus.size(); i++)
                    if (g_bar_menus[i].rect.contains(mx, my)) { if (i == g_open_bar_menu) close_popup(); else open_bar_popup(i); return true; }
                if (g_logo_rect.contains(mx, my)) { if (g_open_bar_menu == -2) close_popup(); else open_bar_popup(-2); return true; }
            }
            close_popup();
            return true;
        }
        if (released) {
            if (h >= 0 && g_popup.items[h].enabled) {
                Func<void()> act = g_popup.items[h].action;
                close_popup();
                if (act) act();
            }
            return g_popup.rect.contains(mx, my) || !g_popup.open;
        }
        // hovering across menubar titles while a bar menu is open
        if (g_popup.from_bar && my < kMenubarHeight) {
            for (int i = 0; i < (int)g_bar_menus.size(); i++)
                if (g_bar_menus[i].rect.contains(mx, my) && i != g_open_bar_menu) { open_bar_popup(i); break; }
        }
        return g_popup.rect.contains(mx, my);
    }
    // ---- launchpad
    if (g_lp_open) {
        int hov = -1;
        for (int i = 0; i < (int)g_lp_rects.size(); i++) if (g_lp_rects[i].contains(mx, my)) hov = i;
        if (hov != g_lp_hover) {
            if (g_lp_hover >= 0 && g_lp_hover < (int)g_lp_rects.size()) damage(g_lp_rects[g_lp_hover].expand(40));
            if (hov >= 0) damage(g_lp_rects[hov].expand(40));
            g_lp_hover = hov;
        }
        if (released & MB_LEFT) {
            if (hov >= 0) { const char* id = g_lp_apps[hov]->id; lp_close(); app_launch(id); }
            else lp_close();
        }
        return true;
    }
    // ---- control center
    if (g_cc_open) {
        if (g_cc_drag >= 0) {
            if (ev.buttons & MB_LEFT) cc_slider_set(g_cc_drag, mx);
            else g_cc_drag = -1;
            return true;
        }
        if (pressed) {
            if (g_cc_rect.contains(mx, my)) { cc_close(); return true; }
            if (cc_click(mx, my)) return true;
            cc_close();
            return false;
        }
        if (g_cc_panel.contains(mx, my)) return true;
    }
    // ---- spotlight
    if (g_spot_open) {
        if (pressed) {
            if (g_spot_rect.contains(mx, my)) {
                int i = (my - (g_spot_rect.y + 62)) / 44;
                if (my > g_spot_rect.y + 56 && i >= 0 && i < (int)g_spot_results.size()) spot_activate(i);
                return true;
            }
            spot_close();
            if (g_search_rect.contains(mx, my)) return true;
        }
    }
    if (!shell_visible()) return false;
    // ---- menubar
    if (my < kMenubarHeight) {
        if (pressed & MB_LEFT) {
            if (g_logo_rect.contains(mx, my)) { open_bar_popup(-2); return true; }
            for (int i = 0; i < (int)g_bar_menus.size(); i++)
                if (g_bar_menus[i].rect.contains(mx, my)) { open_bar_popup(i); return true; }
            if (g_cc_rect.contains(mx, my)) { g_cc_open = true; cc_layout(); damage(g_cc_panel.expand(40)); damage(Rect(0, 0, W, kMenubarHeight + 20)); return true; }
            if (g_search_rect.contains(mx, my)) { open_spotlight(); return true; }
            if (g_clock_rect.contains(mx, my)) { app_launch("calendar"); return true; }
            if (g_net_rect.contains(mx, my)) {
                Vector<MenuItem> items;
                MenuItem& a = items.emplace();
                a.title = net::is_up() ? String("Ethernet: ") + net::ip_string() : String("Network: not connected");
                a.enabled = false;
                MenuItem& sp = items.emplace(); sp.title = "-";
                MenuItem& b = items.emplace(); b.title = "Network Settings…"; b.action = [] { app_launch("settings"); };
                popup_menu(g_net_rect.x, kMenubarHeight + 2, items);
                return true;
            }
        }
        return true;
    }
    // ---- dock
    bool in_dock = my >= g_dock_rect.y - 4 && mx >= g_dock_rect.x - 40 && mx < g_dock_rect.right() + 40;
    int hit = in_dock ? dock_hit(mx, my) : -1;
    if (in_dock != g_dock_hover || mx != g_dock_mouse_x || hit != g_dock_hover_index) {
        g_dock_hover = in_dock;
        g_dock_mouse_x = mx;
        g_dock_hover_index = hit;
        damage(g_dock_damage);
    }
    if (in_dock) {
        if (pressed & MB_LEFT) { g_dock_pressed = hit; damage(g_dock_damage); }
        if (pressed & MB_RIGHT && hit >= 0 && g_dock[hit].kind == 0) {
            const AppInfo* a = g_dock[hit].app;
            Vector<MenuItem> items;
            MenuItem& o = items.emplace(); o.title = "Open"; o.action = [a] { app_launch(a->id); };
            MenuItem& nw = items.emplace(); nw.title = "New Window"; nw.enabled = !a->single_instance;
            nw.action = [a] { Window* w = a->create(); if (w) { w->set_app_id(a->id); focus(w); } };
            MenuItem& sp = items.emplace(); sp.title = "-";
            MenuItem& q = items.emplace(); q.title = "Quit"; q.enabled = g_dock[hit].running;
            q.action = [a] { Vector<Window*> ws = windows(); for (Window* w : ws) if (strcmp(w->app_id(), a->id) == 0) w->close(); };
            popup_menu(mx - 40, g_dock_rect.y - 10 - 26 * 4, items);
        }
        if (released & MB_LEFT) {
            if (hit >= 0 && hit == g_dock_pressed) dock_activate(hit);
            g_dock_pressed = -1;
            damage(g_dock_damage);
        }
        return true;
    }
    if (g_dock_pressed >= 0 && released) { g_dock_pressed = -1; damage(g_dock_damage); }
    return false;
}

bool shell_key(const KeyEvent& e) {
    // Spotlight: Super+Space, Ctrl+Space, Alt+Space
    if (e.key == Key::Space && (e.super() || e.ctrl() || e.alt()) && shell_visible()) {
        if (g_spot_open) spot_close(); else open_spotlight();
        return true;
    }
    if (e.key == Key::F4 && shell_visible()) {
        if (g_lp_open) lp_close(); else open_launchpad();
        return true;
    }
    if (g_popup.open) {
        int n = (int)g_popup.items.size();
        if (e.key == Key::Escape) { close_popup(); return true; }
        if (e.key == Key::Down || e.key == Key::Up) {
            int h = g_popup.hover;
            for (int k = 0; k < n; k++) {
                h = e.key == Key::Down ? (h + 1) % n : (h - 1 + n) % n;
                if (g_popup.items[h].title != "-" && g_popup.items[h].enabled) break;
            }
            g_popup.hover = h;
            damage_popup();
            return true;
        }
        if (e.key == Key::Enter && g_popup.hover >= 0) {
            Func<void()> act = g_popup.items[g_popup.hover].action;
            close_popup();
            if (act) act();
            return true;
        }
        return true;
    }
    if (g_spot_open) {
        bool changed = false;
        switch (e.key) {
        case Key::Escape: spot_close(); return true;
        case Key::Enter: spot_activate(g_spot_sel); return true;
        case Key::Down: if (g_spot_sel + 1 < (int)g_spot_results.size()) g_spot_sel++; break;
        case Key::Up: if (g_spot_sel > 0) g_spot_sel--; break;
        case Key::Backspace:
            if (!g_spot_query.empty()) {
                size_t n = g_spot_query.size() - 1;
                while (n > 0 && ((uint8_t)g_spot_query[n] & 0xC0) == 0x80) n--;
                g_spot_query.erase(n, g_spot_query.size() - n);
            }
            changed = true;
            break;
        case Key::Char:
        case Key::Space:
            if (e.ch && !e.ctrl() && !e.super()) {
                char buf[5];
                int n = utf8_encode(e.ch, buf);
                g_spot_query.append(buf, n);
                changed = true;
            }
            break;
        default: return true;
        }
        Rect old = g_spot_rect;
        if (changed) spot_search();
        spot_layout();
        damage(old.expand(40));
        damage(g_spot_rect.expand(40));
        return true;
    }
    if (g_lp_open) {
        if (e.key == Key::Escape) { lp_close(); return true; }
        if (e.key == Key::Enter && !g_lp_apps.empty()) { const char* id = g_lp_apps[0]->id; lp_close(); app_launch(id); return true; }
        if (e.key == Key::Backspace && !g_lp_query.empty()) { g_lp_query.erase(g_lp_query.size() - 1, 1); lp_filter(); damage_all(); return true; }
        if ((e.key == Key::Char || e.key == Key::Space) && e.ch < 128 && e.ch >= 32) { g_lp_query += (char)e.ch; lp_filter(); damage_all(); return true; }
        return true;
    }
    if (g_cc_open && e.key == Key::Escape) { cc_close(); return true; }
    return false;
}

void shell_desktop_click(int x, int y, int button) {
    if (button != MB_RIGHT) return;
    Vector<MenuItem> items;
    MenuItem& nf = items.emplace(); nf.title = "New Folder";
    nf.action = [] {
        String home = String("/Users/") + g_user_login + "/Desktop";
        for (int i = 1; i < 100; i++) {
            String p = home + (i == 1 ? String("/untitled folder") : String::format("/untitled folder %d", i));
            if (!vfs::exists(p.c_str())) { vfs::mkdir(p.c_str()); notify("Finder", "Created a new folder on the Desktop"); break; }
        }
    };
    MenuItem& sp = items.emplace(); sp.title = "-";
    MenuItem& wp = items.emplace(); wp.title = "Next Wallpaper"; wp.action = [] { set_wallpaper(wallpaper() + 1); };
    MenuItem& cw = items.emplace(); cw.title = "Change Wallpaper…"; cw.action = [] { app_launch("settings"); };
    MenuItem& sp2 = items.emplace(); sp2.title = "-";
    MenuItem& te = items.emplace(); te.title = "Open Terminal"; te.action = [] { app_launch("terminal"); };
    MenuItem& ab = items.emplace(); ab.title = "About KleinOS"; ab.action = [] { if (!app_launch("about")) notify("KleinOS", system_version_string()); };
    popup_menu(x, y, items);
}

// ================================================================ wm:: shell services
namespace wm {

void popup_menu(int x, int y, const Vector<MenuItem>& items) {
    close_popup();
    g_popup.open = true;
    g_popup.items = items;
    g_popup.hover = -1;
    g_popup.from_bar = false;
    layout_popup(x, y);
    damage_popup();
}

void notify(const char* title, const char* body, const char* app_id) {
    if (g_dnd) return;
    Note& n = g_notes.emplace();
    n.title = title;
    n.body = body;
    n.app = app_id ? app_find(app_id) : nullptr;
    n.born = timer_ms();
    if (g_notes.size() > 4) { damage(g_notes[0].rect.expand(30)); g_notes.remove(0); }
    layout_notes();
    for (Note& x : g_notes) damage(x.rect.expand(30).unite(x.rect.offset(380, 0)));
}

void open_spotlight() {
    close_popup();
    cc_close();
    g_spot_open = true;
    g_spot_query.clear();
    g_spot_results.clear();
    spot_layout();
    damage(g_spot_rect.expand(40));
    damage(Rect(0, 0, W, kMenubarHeight + 20));
}

void open_launchpad() {
    close_popup();
    spot_close();
    cc_close();
    g_lp_open = true;
    g_lp_query.clear();
    g_lp_hover = -1;
    lp_filter();
    damage_all();
}

}  // namespace wm

// ---------------------------------------------------------------- tiny expression evaluator (Spotlight math)
namespace {
struct Parser {
    const char* p;
    bool ok = true;
    void ws() { while (*p == ' ') p++; }
    double number() {
        ws();
        if (*p == '(') { p++; double v = expr(); ws(); if (*p == ')') p++; else ok = false; return v; }
        if (*p == '-') { p++; return -number(); }
        if (!isdigit(*p) && *p != '.') { ok = false; return 0; }
        char* end;
        double v = strtod(p, &end);
        p = end;
        return v;
    }
    double power() {
        double b = number();
        ws();
        if (*p == '^') { p++; return pow(b, power()); }
        return b;
    }
    double term() {
        double v = power();
        for (;;) {
            ws();
            if (*p == '*' || *p == 'x') { p++; v *= power(); }
            else if (*p == '/') { p++; double d = power(); if (d == 0) ok = false; else v /= d; }
            else if (*p == '%') { p++; v = fmod(v, power()); }
            else return v;
        }
    }
    double expr() {
        double v = term();
        for (;;) {
            ws();
            if (*p == '+') { p++; v += term(); }
            else if (*p == '-') { p++; v -= term(); }
            else return v;
        }
    }
};
}  // namespace

bool eval_expr(const char* s, double* out) {
    bool has_op = false, has_digit = false;
    for (const char* q = s; *q; q++) {
        if (strchr("+-*/^%x()", *q) && q != s) has_op = true;
        if (isdigit(*q)) has_digit = true;
        if (!strchr("0123456789.+-*/^%x() ", *q)) return false;
    }
    if (!has_op || !has_digit) return false;
    Parser ps{s};
    double v = ps.expr();
    ps.ws();
    if (!ps.ok || *ps.p) return false;
    *out = v;
    return true;
}

// Apps that can open files register a handler; Spotlight/Files call this.
static Func<void(const char*)> g_open_handlers[16];
static const char* g_open_ids[16];

void register_open_handler(const char* app_id, Func<void(const char*)> fn) {
    for (int i = 0; i < 16; i++)
        if (!g_open_ids[i] || strcmp(g_open_ids[i], app_id) == 0) { g_open_ids[i] = app_id; g_open_handlers[i] = fn; return; }
}

void open_file_hook(const char* app, const char* path) {
    for (int i = 0; i < 16; i++)
        if (g_open_ids[i] && strcmp(g_open_ids[i], app) == 0) { g_open_handlers[i](path); return; }
}

void app_open_file(const char* path) {
    String ext = vfs::extension(path);
    const char* app = "textedit";
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" || ext == "gif") app = "photos";
    else if (ext == "html" || ext == "htm") app = "browser";
    else if (ext == "dl") app = "activity";
    if (vfs::is_dir(path)) app = "files";
    if (!app_find(app)) { notify("Can't open file", path); return; }
    app_launch(app);
    open_file_hook(app, path);
}
