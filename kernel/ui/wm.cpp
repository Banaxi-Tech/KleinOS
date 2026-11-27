// Window manager + compositor.
//
// Every frame: drain input, run ticks, let dirty windows repaint into their
// own canvases, then recompose only the damaged screen regions into a RAM
// back buffer (wallpaper -> windows with shadows and glass chrome -> shell
// glass -> cursor) and copy those regions to the framebuffer. Glass needs the
// final backdrop under it, so damage that touches a glass surface is grown to
// cover the whole surface plus its blur margin before composing.
#include "drivers/sound.h"
#include "ui/wm.h"
#include "ui/shell.h"
#include "ui/theme.h"
#include "ui/app.h"
#include "ui/widgets.h"
#include "gfx/glass.h"
#include "gfx/wallpaper.h"
#include "drivers/framebuffer.h"
#include "drivers/input.h"
#include "arch/interrupts.h"
#include "arch/cpu.h"
#include "lib/log.h"
#include "lib/math.h"
#include "fs/vfs.h"
#include "drivers/rtc.h"

namespace wm {

namespace {

Canvas* g_screen;          // back buffer, half-finished while a frame is composed
Canvas* g_front;           // finished picture currently on screen, without the cursor
Canvas* g_wallpaper;
int g_wallpaper_index = 0;
int W, H;
Vector<Window*> g_windows;     // bottom -> top
Window* g_active;
Vector<Rect> g_damage;
Vector<Func<void()>> g_deferred;
bool g_shell_visible = true;
float g_glass_k = 1.0f;
bool g_reduce_transparency = false;
bool g_dock_mag = true;
bool g_lite = false;         // slow CPU: cheaper glass and shadows
String g_clipboard;
Func<void()> g_logout;

Point g_mouse{0, 0};
uint8_t g_buttons;
uint64_t g_frames;
uint64_t g_last_frame_ms, g_last_shell_ms;
uint64_t g_next_draw_ms;
bool g_cheap_drag;           // this drag is too slow for live glass on the moving window
float g_fps;
uint64_t g_fps_t0, g_fps_n;

// interaction state
enum class Drag { None, Move, Resize, Content, Shell, ViewBar };
Drag g_drag = Drag::None;
Window* g_drag_win;
int g_drag_dx, g_drag_dy;
int g_resize_edges;          // 1=right 2=bottom 4=left
Rect g_drag_start;
uint64_t g_last_click_ms;
Point g_last_click_pos;
Window* g_last_click_win;
Window* g_hover_traffic_win;

// cursor
Canvas* g_cursor;
Point g_cursor_drawn{-100, -100};
volatile int g_pointer_x, g_pointer_y;   // updated by the mouse interrupt
constexpr int kCursorW = 20, kCursorH = 26;

void build_cursor() {
    g_cursor = new Canvas(kCursorW, kCursorH);
    PointF arrow[] = {{1.5f, 1.5f}, {1.5f, 19.5f}, {6.0f, 15.5f}, {9.0f, 22.5f}, {12.0f, 21.2f}, {9.2f, 14.5f}, {15.0f, 14.5f}};
    PointF shadow[7];
    for (int i = 0; i < 7; i++) shadow[i] = {arrow[i].x + 0.8f, arrow[i].y + 1.2f};
    g_cursor->fill_polygon(shadow, 7, rgba(0, 0, 0, 70));
    // outline: draw black slightly bigger then white inside
    for (int i = 0; i < 7; i++) {
        PointF a = arrow[i], b = arrow[(i + 1) % 7];
        g_cursor->draw_line(a.x, a.y, b.x, b.y, rgba(0, 0, 0, 255), 2.2f);
    }
    g_cursor->fill_polygon(arrow, 7, hex(0x111111));
    PointF inner[] = {{2.8f, 4.6f}, {2.8f, 16.6f}, {6.3f, 13.5f}, {9.6f, 20.6f}, {10.6f, 20.1f}, {7.6f, 13.2f}, {12.0f, 13.2f}};
    g_cursor->fill_polygon(inner, 7, hex(0xFFFFFF));
}

Rect shadow_bounds(Window* w) { return w->frame().expand(50); }

// ----------------------------------------------------------------- shadow cache
// A window's shadow only depends on its size and on being active, but computing it
// is a distance field per pixel. Render it once into an alpha mask (shadows are black,
// so the mask is all there is) and darken with it; re-render only when the size changes.
struct ShadowMask {
    int w = 0, h = 0, blur = 0, dy = 0, alpha = 0;   // key (w/h: the window frame)
    int ox = 0, oy = 0, mw = 0, mh = 0;              // mask origin relative to the frame, and size
    Rect hole;                                       // fully covered by the window: skipped
    uint8_t* a = nullptr;
    uint64_t used = 0;
};
constexpr int kShadowSlots = 6;
ShadowMask g_shadows[kShadowSlots];
uint64_t g_shadow_tick;

const ShadowMask& shadow_mask(int w, int h, int blur, int dy, int alpha) {
    g_shadow_tick++;
    ShadowMask* lru = &g_shadows[0];
    for (ShadowMask& m : g_shadows) {
        if (m.a && m.w == w && m.h == h && m.blur == blur && m.dy == dy && m.alpha == alpha) { m.used = g_shadow_tick; return m; }
        if (m.used < lru->used) lru = &m;
    }
    ShadowMask& m = *lru;
    kfree(m.a);
    int ext = (int)ceilf(blur * 1.5f) + 2;
    m.w = w; m.h = h; m.blur = blur; m.dy = dy; m.alpha = alpha;
    m.ox = -ext; m.oy = dy - ext; m.mw = w + 2 * ext; m.mh = h + 2 * ext;
    Canvas tmp(m.mw, m.mh);
    tmp.clear(0);
    Rect frame(-m.ox, -m.oy, w, h);
    tmp.draw_shadow(frame, kWindowRadius, (float)blur, rgba(0, 0, 0, alpha), 0, dy);
    m.a = (uint8_t*)kmalloc((size_t)m.mw * m.mh);
    for (int y = 0; y < m.mh; y++) {
        const uint32_t* row = tmp.row(y);
        for (int x = 0; x < m.mw; x++) m.a[(size_t)y * m.mw + x] = row[x] >> 24;
    }
    m.hole = frame.inset((int)(kWindowRadius * 0.3f) + 1).offset(m.ox, m.oy);   // relative to the frame origin
    m.used = g_shadow_tick;
    return m;
}

void draw_window_shadow(Canvas& s, Rect f, int blur, int dy, int alpha, Rect clip) {
    const ShadowMask& m = shadow_mask(f.w, f.h, blur, dy, alpha);
    Rect box(f.x + m.ox, f.y + m.oy, m.mw, m.mh);
    Rect cl = box.intersect(clip).intersect(Rect(0, 0, s.width(), s.height()));
    if (cl.empty()) return;
    Rect hole = m.hole.offset(f.x, f.y);
    for (int y = cl.y; y < cl.bottom(); y++) {
        uint32_t* dst = s.row(y);
        const uint8_t* src = m.a + (size_t)(y - box.y) * m.mw - box.x;
        bool in_hole_rows = y >= hole.y && y < hole.bottom();
        for (int x = cl.x; x < cl.right(); x++) {
            if (in_hole_rows && x >= hole.x && x < hole.right()) { x = hole.right() - 1; continue; }
            uint32_t a = src[x];
            if (!a) continue;
            // black at alpha a over p: scale the colour channels, the screen stays opaque
            uint32_t p = dst[x], k = 255 - a;
            uint32_t rb = (((p & 0x00FF00FF) * k + 0x00800080) >> 8) & 0x00FF00FF;
            uint32_t g = ((((p >> 8) & 0xFF) * k + 0x80) >> 8) << 8;
            dst[x] = (p & 0xFF000000u) | rb | g;
        }
    }
}

GlassStyle window_glass() {
    GlassStyle s = glass_style_window(theme().dark);
    s.blur *= g_glass_k;
    s.strength *= g_glass_k;
    if (g_reduce_transparency) s.tint = with_alpha(s.tint, 245);
    return s;
}

bool window_has_glass(Window* w) { return !(w->flags() & WF_FULLSCREEN); }
bool window_is_glassy(Window* w) { return (w->flags() & WF_GLASS) || w->sidebar_width() > 0; }

// glass regions of a window (screen coords)
Rect window_glass_rect(Window* w) {
    Rect f = w->frame();
    if (w->flags() & WF_GLASS) return f;
    if (w->sidebar_width() > 0) return f;   // full height for the sidebar
    return Rect(f.x, f.y, f.w, kTitlebarHeight + 1);
}

// ----------------------------------------------------------------- damage
void add_damage(Rect r) {
    r = r.intersect(Rect(0, 0, W, H));
    if (r.empty()) return;
    g_damage.push(r);
}

GlassStyle overlay_glass(int radius) {
    GlassStyle s = glass_style_panel(theme().dark);
    s.blur *= g_glass_k;
    s.strength *= g_glass_k;
    if (g_reduce_transparency) s.tint = with_alpha(s.tint, 240);
    s.radius = radius;
    return s;
}

// ----------------------------------------------------------------- screenshots
// Super+Shift+3: whole screen, Super+Shift+4: the active window. Saved as BMP to
// ~/Pictures (Photos picks them up), with a white flash and a notification.
uint64_t g_flash_ms;              // when the capture flash started (0 = none)
constexpr uint64_t kFlashMs = 280;
uint64_t g_shot_at_ms;            // delayed capture (from a menu: let it close first)
bool g_shot_window;

bool write_bmp(const Canvas& img, Rect r, const char* path) {
    r = r.intersect(Rect(0, 0, img.width(), img.height()));
    if (r.empty()) return false;
    uint32_t row_bytes = ((r.w * 3 + 3) / 4) * 4, data = row_bytes * (uint32_t)r.h, total = 54 + data;
    uint8_t* buf = (uint8_t*)kmalloc(total);
    if (!buf) return false;
    memset(buf, 0, 54);
    auto put = [&](int off, uint32_t v, int n) { for (int i = 0; i < n; i++) buf[off + i] = (v >> (8 * i)) & 0xFF; };
    buf[0] = 'B'; buf[1] = 'M';
    put(2, total, 4); put(10, 54, 4); put(14, 40, 4); put(18, (uint32_t)r.w, 4); put(22, (uint32_t)r.h, 4);
    put(26, 1, 2); put(28, 24, 2); put(34, data, 4); put(38, 2835, 4); put(42, 2835, 4);
    for (int y = 0; y < r.h; y++) {
        const uint32_t* src = img.row(r.bottom() - 1 - y) + r.x;
        uint8_t* d = buf + 54 + (size_t)y * row_bytes;
        for (int x = 0; x < r.w; x++) { uint32_t p = src[x]; d[x * 3] = p; d[x * 3 + 1] = p >> 8; d[x * 3 + 2] = p >> 16; }
        for (uint32_t i = r.w * 3; i < row_bytes; i++) d[i] = 0;
    }
    bool ok = vfs::write_file(path, buf, total);
    kfree(buf);
    return ok;
}

void capture_now(bool window_only) {
    Rect r(0, 0, W, H);
    if (window_only) {
        if (!g_active || g_active->minimized_) { wm::notify("Screenshot", "No window is active to capture."); return; }
        r = g_active->frame();
    }
    DateTime t = rtc_now();
    const char* login = shell_user_login();
    char path[192];
    snprintf(path, sizeof(path), "/Users/%s/Pictures/Screenshot %04d-%02d-%02d at %02d.%02d.%02d.bmp",
             login && *login ? login : "guest", t.year, t.month, t.day, t.hour, t.minute, t.second);
    if (!write_bmp(*g_front, r, path)) { wm::notify("Screenshot", "Couldn't save the screenshot."); return; }
    klog("wm: screenshot %dx%d -> %s", r.w, r.h, path);
    g_flash_ms = timer_ms();
    sound_effect(SoundEffect::Screenshot);
    add_damage(window_only ? r : Rect(0, 0, W, H));
    const char* name = strrchr(path, '/') + 1;
    wm::notify(window_only ? "Window captured" : "Screenshot saved", name, "photos");
}

// ----------------------------------------------------------------- snapping
// Drag a window against the left/right screen edge to fill that half (the corners
// give quarters), or against the top edge to maximize it. Super+arrows do the same.
// A tiled window reuses the zoom state: unzoomed_ keeps the size to go back to.
enum class Snap { None, Left, Right, Max, TopLeft, TopRight, BottomLeft, BottomRight };
constexpr int kSnapGap = 6;
constexpr int kSnapCorner = 90;   // px from the top/bottom of the desktop that pick a quarter
Snap g_snap = Snap::None;         // zone under the pointer while dragging
Rect g_snap_rect;                 // its preview (screen)

Rect snap_frame(Snap z) {
    Rect d = wm::desktop_area();
    int g = kSnapGap;
    int hw = (d.w - 3 * g) / 2, hh = (d.h - 3 * g) / 2;
    int lx = d.x + g, rx = d.right() - g - hw, ty = d.y + g, by = d.bottom() - g - hh;
    switch (z) {
    case Snap::Left: return Rect(lx, ty, hw, d.h - 2 * g);
    case Snap::Right: return Rect(rx, ty, hw, d.h - 2 * g);
    case Snap::Max: return Rect(lx, ty, d.w - 2 * g, d.h - 2 * g);
    case Snap::TopLeft: return Rect(lx, ty, hw, hh);
    case Snap::TopRight: return Rect(rx, ty, hw, hh);
    case Snap::BottomLeft: return Rect(lx, by, hw, hh);
    case Snap::BottomRight: return Rect(rx, by, hw, hh);
    default: return Rect();
    }
}

bool can_tile(Window* w) { return w && (w->flags() & WF_RESIZABLE) && !(w->flags() & (WF_FULLSCREEN | WF_DIALOG)); }

// Which tile a window currently fills (None if it is free-floating).
Snap current_snap(Window* w) {
    if (!w->zoomed_) return Snap::None;
    Rect f = w->frame();
    for (int z = (int)Snap::Left; z <= (int)Snap::BottomRight; z++) {
        Rect t = snap_frame((Snap)z);
        if (abs(f.x - t.x) <= 2 && abs(f.y - t.y) <= 2 && abs(f.w - t.w) <= 2 && abs(f.h - t.h) <= 2) return (Snap)z;
    }
    return Snap::None;
}

void tile_window(Window* w, Snap z) {
    if (!can_tile(w) || z == Snap::None) return;
    if (!w->zoomed_) w->unzoomed_ = Rect(w->x_, w->y_, w->width(), w->height());
    w->zoomed_ = true;
    Rect t = snap_frame(z);
    w->resize(t.w, t.h - kTitlebarHeight);
    w->move_to(t.x, t.y);
}

void untile_window(Window* w) {
    if (!w->zoomed_) return;
    w->zoomed_ = false;
    w->resize(w->unzoomed_.w, w->unzoomed_.h);
    w->move_to(w->unzoomed_.x, w->unzoomed_.y);
}

Snap snap_zone_at(int x, int y) {
    Rect d = wm::desktop_area();
    if (x <= 1) return y < d.y + kSnapCorner ? Snap::TopLeft : y >= d.bottom() - kSnapCorner ? Snap::BottomLeft : Snap::Left;
    if (x >= W - 2) return y < d.y + kSnapCorner ? Snap::TopRight : y >= d.bottom() - kSnapCorner ? Snap::BottomRight : Snap::Right;
    if (y <= 1) return Snap::Max;
    return Snap::None;
}

int snap_damage_margin() { return glass_margin(overlay_glass(kWindowRadius)) + 40; }

void set_snap(Snap z) {
    if (z == g_snap) return;
    if (g_snap != Snap::None) add_damage(g_snap_rect.expand(snap_damage_margin()));
    g_snap = z;
    g_snap_rect = snap_frame(z);
    if (z != Snap::None) add_damage(g_snap_rect.expand(snap_damage_margin()));
}

void draw_snap_preview(Canvas& s, Rect clip) {
    Rect p = g_snap_rect;
    if (!p.expand(40).intersects(clip)) return;
    s.save();
    s.clip_to(clip);
    s.draw_shadow(p, kWindowRadius, 22, rgba(0, 0, 0, theme().dark ? 110 : 55), 0, 8);
    s.restore();
    glass_render(s, p, overlay_glass(kWindowRadius), clip);
    s.save();
    s.clip_to(clip);
    s.fill_round_rect(p, kWindowRadius, with_alpha(theme().accent, 46));
    s.stroke_round_rect(p.expand(-1), kWindowRadius - 1, with_alpha(theme().accent, 170), 1.5f);
    s.stroke_round_rect(p, kWindowRadius, theme().dark ? rgba(255, 255, 255, 50) : rgba(255, 255, 255, 140), 1.0f);
    s.restore();
}

// ----------------------------------------------------------------- window switcher
// Alt+Tab (or Super+Tab) opens a strip of the open windows, most recent first.
// Tab / Shift+Tab / arrows move, releasing the modifier switches, Escape cancels.
struct Switcher {
    bool open = false;
    bool shown = false;           // drawn only after a short delay, so a quick Alt+Tab doesn't flash
    uint8_t mod = 0;              // the modifier that has to stay held
    uint64_t opened_ms = 0;
    Vector<Window*> list;
    int sel = 0;
    Rect rect;
};
Switcher g_sw;
constexpr int kSwCell = 108, kSwIcon = 68, kSwPad = 14, kSwTitleH = 34;
constexpr uint64_t kSwShowDelay = 110;

int sw_visible() { int fit = (W - 80 - 2 * kSwPad) / kSwCell; return kmax(1, kmin((int)g_sw.list.size(), fit)); }
int sw_first() { int v = sw_visible(); return g_sw.sel >= v ? g_sw.sel - v + 1 : 0; }

void sw_layout() {
    int v = sw_visible();
    int w = v * kSwCell + 2 * kSwPad, h = kSwCell + kSwTitleH + 2 * kSwPad - 6;
    g_sw.rect = Rect((W - w) / 2, (H - h) / 2, w, h);
}

void sw_damage() {
    if (!g_sw.shown) return;
    sw_layout();
    add_damage(g_sw.rect.expand(glass_margin(overlay_glass(22)) + 50));
}

void sw_close() {
    sw_damage();
    g_sw.open = g_sw.shown = false;
    g_sw.list.clear();
}

void sw_open(uint8_t mod, bool backwards) {
    g_sw.list.clear();
    for (size_t i = g_windows.size(); i > 0; i--) {      // visible windows, top first
        Window* w = g_windows[i - 1];
        if (!w->closing_ && !w->minimized_) g_sw.list.push(w);
    }
    for (size_t i = g_windows.size(); i > 0; i--) {      // then minimized ones
        Window* w = g_windows[i - 1];
        if (!w->closing_ && w->minimized_) g_sw.list.push(w);
    }
    int n = (int)g_sw.list.size();
    if (!n) return;
    g_sw.open = true;
    g_sw.shown = false;
    g_sw.mod = mod;
    g_sw.opened_ms = timer_ms();
    bool top_is_active = g_active && g_sw.list[0] == g_active;
    if (backwards) g_sw.sel = n - 1;
    else g_sw.sel = top_is_active && n > 1 ? 1 : 0;
}

void sw_step(int dir) {
    int n = (int)g_sw.list.size();
    if (!n) return;
    sw_damage();
    g_sw.sel = (g_sw.sel + dir + n) % n;
    if (!g_sw.shown) g_sw.shown = true;   // second Tab: show right away
    sw_damage();
}

void sw_commit() {
    Window* w = g_sw.sel >= 0 && g_sw.sel < (int)g_sw.list.size() ? g_sw.list[g_sw.sel] : nullptr;
    sw_close();
    if (!w) return;
    if (w->minimized_) wm::restore(w);
    wm::focus(w);
}

int sw_hit(int x, int y) {
    if (!g_sw.shown) return -1;
    sw_layout();
    Rect r = g_sw.rect;
    int first = sw_first(), v = sw_visible();
    for (int i = 0; i < v; i++) {
        Rect cell(r.x + kSwPad + i * kSwCell, r.y + kSwPad, kSwCell, kSwCell);
        if (cell.contains(x, y)) return first + i;
    }
    return -1;
}

void draw_switcher(Canvas& s, Rect clip) {
    if (!g_sw.open || !g_sw.shown) return;
    sw_layout();
    Rect p = g_sw.rect;
    if (!p.expand(50).intersects(clip)) return;
    GlassStyle gs = overlay_glass(22);
    s.save();
    s.clip_to(clip);
    s.draw_shadow(p, 22, 30, rgba(0, 0, 0, theme().dark ? 140 : 70), 0, 10);
    s.restore();
    glass_render(s, p, gs, clip);
    s.save();
    s.clip_to(clip);
    s.stroke_round_rect(p, 22, theme().dark ? rgba(255, 255, 255, 46) : rgba(255, 255, 255, 130), 1.0f);
    int first = sw_first(), v = sw_visible();
    for (int i = 0; i < v; i++) {
        int idx = first + i;
        Window* w = g_sw.list[idx];
        Rect cell(p.x + kSwPad + i * kSwCell, p.y + kSwPad, kSwCell, kSwCell);
        if (idx == g_sw.sel) {
            s.fill_round_rect(cell.expand(-3), 16, theme().dark ? rgba(255, 255, 255, 44) : rgba(0, 0, 0, 30));
            s.stroke_round_rect(cell.expand(-3), 16, theme().dark ? rgba(255, 255, 255, 60) : rgba(0, 0, 0, 22), 1.0f);
        }
        Rect ir(cell.x + (kSwCell - kSwIcon) / 2, cell.y + (kSwCell - kSwIcon) / 2 - 2, kSwIcon, kSwIcon);
        const AppInfo* a = app_find(w->app_id());
        if (a && a->draw_icon) {
            a->draw_icon(s, ir);
        } else {
            s.fill_round_rect(ir, kSwIcon * 0.225f, theme().accent);
            char letter[2] = {w->title()[0] ? w->title()[0] : '?', 0};
            s.draw_text_in(ir, letter, theme().accent_text, 32, Font::Bold, Align::Center);
        }
        if (w->minimized_)   // dimmed, like a window that's tucked away
            s.fill_round_rect(ir, kSwIcon * 0.225f, theme().dark ? rgba(30, 30, 34, 120) : rgba(255, 255, 255, 120));
        // a dot per window of the same app would crowd it; mark the minimized ones instead
        if (w->minimized_) s.fill_round_rect(Rect(cell.x + kSwCell / 2 - 9, cell.bottom() - 12, 18, 3), 1.5f, theme().text_secondary);
    }
    if (first > 0) s.draw_text_in(Rect(p.x + 2, p.y + kSwPad, 14, kSwCell), "\xE2\x80\xB9", theme().text_secondary, 18, Font::Bold, Align::Center);
    if (first + v < (int)g_sw.list.size())
        s.draw_text_in(Rect(p.right() - 16, p.y + kSwPad, 14, kSwCell), "\xE2\x80\xBA", theme().text_secondary, 18, Font::Bold, Align::Center);
    Window* cur = g_sw.list[g_sw.sel];
    const AppInfo* ca = app_find(cur->app_id());
    const char* title = cur->title()[0] ? cur->title() : ca ? ca->name : "Window";
    String label = title;
    if (ca && strcmp(ca->name, title) != 0) label = String(ca->name) + " \xE2\x80\x94 " + title;
    s.draw_text_in(Rect(p.x + 16, p.y + kSwPad + kSwCell - 2, p.w - 32, kSwTitleH - 8), label.c_str(), theme().text, 14, Font::Medium, Align::Center);
    s.restore();
}

// Grow damage to cover glass surfaces they touch, merge overlaps.
void normalize_damage() {
    Vector<Rect> glass;
    Vector<Window*> owner;   // windows' glass first, then the shell's (no owner)
    for (Window* w : g_windows)
        if (!w->minimized_ && !w->closing_ && window_has_glass(w)) {
            glass.push(window_glass_rect(w).expand(glass_margin(window_glass())));
            owner.push(w);
        }
    shell_glass_rects(glass);
    if (g_snap != Snap::None) glass.push(g_snap_rect.expand(glass_margin(overlay_glass(kWindowRadius))));
    if (g_sw.shown) { sw_layout(); glass.push(g_sw.rect.expand(glass_margin(overlay_glass(22)))); }
    for (int iter = 0; iter < 6; iter++) {
        bool changed = false;
        for (Rect& d : g_damage)
            for (size_t i = 0; i < glass.size(); i++) {
                Rect& g = glass[i];
                // an opaque window's content doesn't show through its own titlebar
                if (i < owner.size() && !window_is_glassy(owner[i]) && d.intersect(owner[i]->content_rect()) == d) continue;
                if (d.intersects(g)) {
                    Rect u = d.unite(g);
                    if (u != d) { d = u; changed = true; }
                }
            }
        // merge overlapping damage
        for (size_t i = 0; i < g_damage.size(); i++)
            for (size_t j = i + 1; j < g_damage.size();) {
                if (g_damage[i].intersects(g_damage[j].expand(8))) {
                    g_damage[i] = g_damage[i].unite(g_damage[j]);
                    g_damage.remove(j);
                    changed = true;
                } else {
                    j++;
                }
            }
        if (!changed) break;
    }
    // too many pieces -> one bounding box
    if (g_damage.size() > 12) {
        Rect u;
        for (Rect& d : g_damage) u = u.unite(d);
        g_damage.clear();
        g_damage.push(u);
    }
    for (Rect& d : g_damage) d = d.intersect(Rect(0, 0, W, H));
}

// ----------------------------------------------------------------- drawing
void draw_traffic_lights(Canvas& c, Window* w, Rect f) {
    bool active = w == g_active;
    bool hover = g_hover_traffic_win == w;
    const Color cols[3] = {hex(0xFF5F57), hex(0xFEBC2E), hex(0x28C840)};
    for (int i = 0; i < 3; i++) {
        bool disabled = (i == 1 && (w->flags() & (WF_NO_MINIMIZE | WF_DIALOG))) ||
                        (i == 2 && (w->flags() & (WF_NO_ZOOM | WF_DIALOG)));
        float cx = f.x + 20 + i * 20, cy = f.y + kTitlebarHeight / 2.0f;
        Color col = (active || hover) && !disabled ? cols[i] : (theme().dark ? rgba(255, 255, 255, 50) : rgba(0, 0, 0, 40));
        c.fill_circle(cx, cy, 6.5f, col);
        c.stroke_circle(cx, cy, 6.5f, rgba(0, 0, 0, 30), 0.8f);
        if (hover && !disabled) {
            Rect g((int)cx - 4, (int)cy - 4, 8, 8);
            Color gc = rgba(0, 0, 0, 140);
            if (i == 0) { c.draw_line(g.x, g.y, g.right(), g.bottom(), gc, 1.3f); c.draw_line(g.right(), g.y, g.x, g.bottom(), gc, 1.3f); }
            if (i == 1) c.draw_line(g.x, cy, g.right(), cy, gc, 1.5f);
            if (i == 2) {
                PointF a[] = {{(float)g.x, (float)g.y}, {(float)g.x + 5.5f, (float)g.y}, {(float)g.x, (float)g.y + 5.5f}};
                PointF b[] = {{(float)g.right(), (float)g.bottom()}, {(float)g.right() - 5.5f, (float)g.bottom()}, {(float)g.right(), (float)g.bottom() - 5.5f}};
                c.fill_polygon(a, 3, gc);
                c.fill_polygon(b, 3, gc);
            }
        }
    }
}

// scrollbar of a fixed-layout window too tall for the screen
constexpr int kViewBarW = 12;

Rect view_bar_thumb(Window* w) {
    Rect cr = w->content_rect();
    int th = kmax(30, cr.h * cr.h / w->height());
    int range = w->height() - cr.h;
    int y = cr.y + (range > 0 ? (cr.h - th) * w->view_scroll() / range : 0);
    return Rect(cr.right() - kViewBarW + 2, y, kViewBarW - 4, th);
}

void view_bar_drag(Window* w, int mouse_y) {
    Rect cr = w->content_rect();
    int th = view_bar_thumb(w).h;
    int track = cr.h - th;
    if (track > 0) w->set_view_scroll((mouse_y - cr.y - th / 2) * (w->height() - cr.h) / track);
}

void compose_window(Canvas& s, Window* w, Rect clip) {
    Rect f = w->frame();
    if (!shadow_bounds(w).intersects(clip)) return;
    s.save();
    s.clip_to(clip);
    bool full = w->flags() & WF_FULLSCREEN;
    if (full) {
        s.blit(w->content_canvas(), f.x, f.y);
        s.restore();
        return;
    }
    bool active = w == g_active;
    {
        int blur = (active ? 26 : 16) >> (g_lite ? 1 : 0), alpha = theme().dark ? (active ? 170 : 120) : (active ? 75 : 45);
        if (g_drag == Drag::Resize && w == g_drag_win)   // a new size every frame: no point caching
            s.draw_shadow(f, kWindowRadius, blur, rgba(0, 0, 0, alpha), 0, active ? 12 : 6);
        else
            draw_window_shadow(s, f, blur, active ? 12 : 6, alpha, clip);
    }
    // glass part (titlebar / sidebar / whole window)
    Rect gr = window_glass_rect(w);
    if (gr.intersects(clip) && g_cheap_drag && w == g_drag_win && g_drag == Drag::Move) {
        // re-frosting the backdrop under a moving window every frame is what a slow CPU
        // can't keep up with: use the glass tint, solid, until the window is let go
        s.save();
        s.clip_to(gr);
        s.fill_round_rect(f, kWindowRadius, theme().dark ? rgba(46, 46, 50, 255) : rgba(238, 238, 242, 255));
        s.restore();
    } else if (gr.intersects(clip)) {
        GlassStyle gs = window_glass();
        gs.radius = kWindowRadius;
        // render glass for the full window shape but only inside the glass region
        // a sidebar window's body is opaque, so frost only the title bar and sidebar
        Rect pieces[2] = {gr, Rect()};
        if (!(w->flags() & WF_GLASS) && w->sidebar_width() > 0) {
            pieces[0] = Rect(f.x, f.y, f.w, kTitlebarHeight + 1);
            pieces[1] = Rect(f.x, f.y + kTitlebarHeight + 1, w->sidebar_width(), f.h - kTitlebarHeight - 1);
        }
        s.restore();
        for (const Rect& piece : pieces) {
            if (piece.empty()) continue;
            s.save();
            s.clip_to(clip.intersect(piece));
            glass_render(s, f, gs, s.clip());
            s.restore();
        }
        s.save();
        s.clip_to(clip);
    }
    // content
    Rect cr = w->content_rect();
    Canvas& cc = w->content_canvas();
    bool glassy = (w->flags() & WF_GLASS) || w->sidebar_width() > 0;
    // body rows above the rounded corners: plain copy/blend
    int R = kWindowRadius;
    Rect body = Rect(cr.x, cr.y, cr.w, cr.h - R).intersect(clip);
    if (!body.empty()) {
        s.save();
        s.clip_to(body);
        if (glassy) s.draw(cc, cr.x, cr.y - w->view_scroll());
        else s.blit(cc, cr.x, cr.y - w->view_scroll());
        s.restore();
    }
    // bottom R rows: coverage-masked so the corners come out round
    Rect tail = Rect(cr.x, cr.bottom() - R, cr.w, R).intersect(clip);
    for (int y = tail.y; y < tail.bottom(); y++) {
        uint32_t* dst = s.row(y);
        const uint32_t* src = cc.row(y - cr.y + w->view_scroll());
        float dy = y + 0.5f - (cr.bottom() - R);
        for (int x = tail.x; x < tail.right(); x++) {
            uint32_t p = src[x - cr.x];
            float dx = 0;
            if (x < cr.x + R) dx = (cr.x + R) - (x + 0.5f);
            else if (x >= cr.right() - R) dx = (x + 0.5f) - (cr.right() - R);
            int cov = 255;
            if (dx > 0) {
                float d = sqrtf(dx * dx + dy * dy) - R;
                if (d >= 0.5f) continue;
                if (d > -0.5f) cov = (int)((0.5f - d) * 255);
            }
            if (!glassy && cov == 255) dst[x] = p;
            else dst[x] = px_over(dst[x], cov == 255 ? p : px_scale(p, cov));
        }
    }
    if (w->overflows()) {
        Rect thumb = view_bar_thumb(w);
        s.fill_round_rect(Rect(cr.right() - kViewBarW, cr.y, kViewBarW, cr.h), 0, theme().dark ? rgba(255, 255, 255, 18) : rgba(0, 0, 0, 14));
        s.fill_round_rect(thumb, 4, theme().dark ? rgba(255, 255, 255, 120) : rgba(0, 0, 0, 110));
    }
    // titlebar contents
    if (!(w->flags() & WF_NO_TITLE)) {
        Color tc = active ? theme().text : theme().text_secondary;
        s.draw_text_in(Rect(f.x + 80, f.y, f.w - 160, kTitlebarHeight), w->title(), tc, 14, Font::Medium, Align::Center);
    }
    s.fill_rect(Rect(f.x, f.y + kTitlebarHeight - 1, f.w, 1), theme().dark ? rgba(0, 0, 0, 90) : rgba(0, 0, 0, 18));
    draw_traffic_lights(s, w, f);
    // hairline border
    s.stroke_round_rect(f, kWindowRadius, theme().dark ? rgba(255, 255, 255, 34) : rgba(0, 0, 0, 40), 1.0f);
    s.restore();
}

// Everything at or below window index `upto` (exclusive), inside q.
void compose_below(Canvas& s, Rect q, size_t upto, bool& preview_drawn) {
    if (q.empty()) return;
    s.reset_clip();
    s.save();
    s.clip_to(q);
    s.blit(*g_wallpaper, 0, 0); shell_draw_desktop(s, q);
    s.restore();
    for (size_t i = 0; i < upto; i++) {
        Window* w = g_windows[i];
        if (w->minimized_ || w->closing_) continue;
        if (g_snap != Snap::None && w == g_drag_win) { draw_snap_preview(s, q); preview_drawn = true; }
        compose_window(s, w, q);
    }
}

void compose_region(Rect r) {
    Canvas& s = *g_screen;
    // The highest window with opaque content covering a good part of r hides everything
    // under it there: draw the wallpaper and lower windows only around it.
    size_t occ = g_windows.size();
    Rect O;
    for (size_t i = g_windows.size(); i > 0; i--) {
        Window* w = g_windows[i - 1];
        if (w->minimized_ || w->closing_ || (w->flags() & WF_GLASS)) continue;
        Rect c = w->content_rect();
        if (w->sidebar_width() > 0) { c.x += w->sidebar_width(); c.w -= w->sidebar_width(); }   // the body is opaque
        if (!(w->flags() & WF_FULLSCREEN)) c.h -= kWindowRadius;   // rounded bottom corners show what's behind
        Rect o = c.intersect(r);
        if ((int64_t)o.w * o.h * 4 >= (int64_t)r.w * r.h) { occ = i - 1; O = o; break; }
    }
    bool preview_drawn = false;
    if (occ == g_windows.size()) {
        compose_below(s, r, g_windows.size(), preview_drawn);
    } else {
        compose_below(s, Rect(r.x, r.y, r.w, O.y - r.y), occ, preview_drawn);
        compose_below(s, Rect(r.x, O.bottom(), r.w, r.bottom() - O.bottom()), occ, preview_drawn);
        compose_below(s, Rect(r.x, O.y, O.x - r.x, O.h), occ, preview_drawn);
        compose_below(s, Rect(O.right(), O.y, r.right() - O.right(), O.h), occ, preview_drawn);
        s.reset_clip();
        for (size_t i = occ; i < g_windows.size(); i++) {
            Window* w = g_windows[i];
            if (w->minimized_ || w->closing_) continue;
            if (g_snap != Snap::None && w == g_drag_win && !preview_drawn) { draw_snap_preview(s, r); preview_drawn = true; }
            compose_window(s, w, r);
        }
    }
    if (g_snap != Snap::None && !preview_drawn) draw_snap_preview(s, r);
    if (g_shell_visible) shell_draw(s, r);
    shell_draw_overlays(s, r);
    draw_switcher(s, r);
    if (g_flash_ms) {
        uint64_t age = timer_ms() - g_flash_ms;
        if (age < kFlashMs) {
            float k = 1.0f - (float)age / kFlashMs;
            s.save();
            s.clip_to(r);
            s.fill_rect(r, rgba(255, 255, 255, (int)(200 * k * k)));
            s.restore();
        }
    }
}

void present(Rect r) {
    FramebufferInfo& fb = framebuffer();
    for (int y = r.y; y < r.bottom(); y++) {
        uint32_t* dst = (uint32_t*)((uint8_t*)fb.address + (size_t)y * fb.pitch) + r.x;
        uint32_t* front = g_front->row(y) + r.x;
        memcpy(front, g_screen->row(y) + r.x, r.w * 4);
        memcpy(dst, front, r.w * 4);
    }
}

// The cursor goes straight to the framebuffer from the mouse interrupt, so it keeps
// moving whatever the main loop is busy with. It only reads g_front, never the
// back buffer, because an interrupt can land in the middle of composing a frame.
void draw_cursor_to_fb() {
    IrqGuard guard;
    FramebufferInfo& fb = framebuffer();
    Point at{g_pointer_x, g_pointer_y};
    Rect old(g_cursor_drawn.x, g_cursor_drawn.y, kCursorW, kCursorH);
    old = old.intersect(Rect(0, 0, W, H));
    for (int y = old.y; y < old.bottom(); y++) {
        uint32_t* dst = (uint32_t*)((uint8_t*)fb.address + (size_t)y * fb.pitch) + old.x;
        memcpy(dst, g_front->row(y) + old.x, old.w * 4);
    }
    Rect nr(at.x, at.y, kCursorW, kCursorH);
    nr = nr.intersect(Rect(0, 0, W, H));
    for (int y = nr.y; y < nr.bottom(); y++) {
        uint32_t* dst = (uint32_t*)((uint8_t*)fb.address + (size_t)y * fb.pitch);
        const uint32_t* bb = g_front->row(y);
        const uint32_t* cur = g_cursor->row(y - at.y);
        for (int x = nr.x; x < nr.right(); x++) dst[x] = px_over(bb[x], cur[x - at.x]);
    }
    g_cursor_drawn = at;
}

void pointer_moved(int x, int y) {
    g_pointer_x = x;
    g_pointer_y = y;
    draw_cursor_to_fb();
}

// ----------------------------------------------------------------- input
Window* window_at(int x, int y) {
    for (size_t i = g_windows.size(); i > 0; i--) {
        Window* w = g_windows[i - 1];
        if (w->minimized_ || w->closing_) continue;
        if (w->frame().expand((w->flags() & WF_RESIZABLE) ? 4 : 0).contains(x, y)) return w;
    }
    return nullptr;
}

int traffic_hit(Window* w, int x, int y) {
    if (w->flags() & WF_FULLSCREEN) return -1;
    Rect f = w->frame();
    for (int i = 0; i < 3; i++) {
        int cx = f.x + 20 + i * 20, cy = f.y + kTitlebarHeight / 2;
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= 64) return i;
    }
    return -1;
}

int resize_edges(Window* w, int x, int y) {
    if (!(w->flags() & WF_RESIZABLE) || (w->flags() & WF_FULLSCREEN)) return 0;
    Rect f = w->frame();
    int e = 0;
    if (x >= f.right() - 6 && x < f.right() + 4) e |= 1;
    if (y >= f.bottom() - 6 && y < f.bottom() + 4) e |= 2;
    if (x >= f.x - 4 && x < f.x + 5) e |= 4;
    return e;
}

void handle_mouse(const InputEvent& ev) {
    Point old = g_mouse;
    g_mouse = Point{ev.x, ev.y};
    uint8_t pressed = ev.buttons & ~g_buttons;
    uint8_t released = g_buttons & ~ev.buttons;
    g_buttons = ev.buttons;

    if (g_sw.open) {
        int hit = sw_hit(ev.x, ev.y);
        if (hit >= 0 && hit != g_sw.sel && (old.x != ev.x || old.y != ev.y)) { sw_damage(); g_sw.sel = hit; sw_damage(); }
        if (pressed) { if (hit >= 0) { g_sw.sel = hit; sw_commit(); } else sw_close(); }
        return;
    }

    // shell gets first pick (menus, dock, menubar, popups)
    if (shell_mouse(ev, pressed, released)) {
        if (pressed) g_drag = Drag::Shell;
        if (released && g_drag == Drag::Shell) g_drag = Drag::None;
        return;
    }
    if (g_drag == Drag::Shell && released) { g_drag = Drag::None; return; }

    if (ev.wheel) {
        Window* w = window_at(ev.x, ev.y);
        if (w) {
            Rect cr = w->content_rect();
            if (cr.contains(ev.x, ev.y)) {
                if (w->overflows()) w->set_view_scroll(w->view_scroll() + ev.wheel * 40);
                else w->on_scroll(ev.x - cr.x, ev.y - cr.y, ev.wheel);
            }
        }
    }

    if (pressed) {
        int button = pressed & MB_LEFT ? MB_LEFT : pressed & MB_RIGHT ? MB_RIGHT : MB_MIDDLE;
        Window* w = window_at(ev.x, ev.y);
        if (!w) {
            if (g_active) focus(nullptr);
            shell_desktop_click(ev.x, ev.y, button);
            return;
        }
        focus(w);
        Rect f = w->frame();
        Rect cr = w->content_rect();
        int edges = resize_edges(w, ev.x, ev.y);
        int th = traffic_hit(w, ev.x, ev.y);
        if (button == MB_LEFT && th >= 0) {
            if (th == 0) w->close();
            else if (th == 1 && !(w->flags() & (WF_NO_MINIMIZE | WF_DIALOG))) minimize(w);
            else if (th == 2 && !(w->flags() & (WF_NO_ZOOM | WF_DIALOG))) toggle_zoom(w);
            return;
        }
        if (button == MB_LEFT && edges) {
            g_drag = Drag::Resize;
            g_drag_win = w;
            g_resize_edges = edges;
            g_drag_start = Rect(f.x, f.y, w->width(), w->height());
            g_drag_dx = ev.x;
            g_drag_dy = ev.y;
            return;
        }
        if (!(w->flags() & WF_FULLSCREEN) && ev.y < f.y + kTitlebarHeight && !cr.contains(ev.x, ev.y)) {
            uint64_t now = timer_ms();
            if (button == MB_LEFT && g_last_click_win == w && now - g_last_click_ms < 400 && !(w->flags() & (WF_NO_ZOOM | WF_DIALOG))) {
                toggle_zoom(w);
                g_last_click_ms = 0;
                return;
            }
            g_last_click_ms = now;
            g_last_click_win = w;
            if (button == MB_LEFT) {
                g_drag = Drag::Move;
                g_drag_win = w;
                g_drag_dx = ev.x - f.x;
                g_drag_dy = ev.y - f.y;
            }
            return;
        }
        if (button == MB_LEFT && w->overflows() && cr.contains(ev.x, ev.y) && ev.x >= cr.right() - kViewBarW) {
            g_drag = Drag::ViewBar;
            g_drag_win = w;
            view_bar_drag(w, ev.y);
            return;
        }
        if (cr.contains(ev.x, ev.y)) {
            uint64_t now = timer_ms();
            int lx = ev.x - cr.x, ly = ev.y - cr.y + w->view_scroll();
            bool dbl = button == MB_LEFT && g_last_click_win == w && now - g_last_click_ms < 400 &&
                       abs(ev.x - g_last_click_pos.x) < 5 && abs(ev.y - g_last_click_pos.y) < 5;
            g_last_click_ms = dbl ? 0 : now;
            g_last_click_pos = g_mouse;
            g_last_click_win = w;
            g_drag = Drag::Content;
            g_drag_win = w;
            w->on_mouse_down(lx, ly, button);
            if (dbl) {
                w->on_double_click(lx, ly);
                for (Widget* wd : w->widgets())
                    if (wd->visible && wd->enabled && wd->bounds.contains(lx, ly)) { wd->double_click(lx, ly); break; }
            }
        }
        return;
    }

    if (released) {
        int button = released & MB_LEFT ? MB_LEFT : released & MB_RIGHT ? MB_RIGHT : MB_MIDDLE;
        if (g_drag == Drag::Content && g_drag_win) {
            Rect cr = g_drag_win->content_rect();
            g_drag_win->on_mouse_up(ev.x - cr.x, ev.y - cr.y + g_drag_win->view_scroll(), button);
        }
        if (g_drag == Drag::Move && g_drag_win) {
            Window* w = g_drag_win;
            if (g_cheap_drag) { g_cheap_drag = false; damage(shadow_bounds(w)); }
            Snap z = g_snap;
            set_snap(Snap::None);
            if (z != Snap::None) {
                tile_window(w, z);
            } else {
                // keep titlebar reachable
                Rect d = desktop_area();
                if (w->y_ < d.y) w->move_to(w->x_, d.y);
            }
        }
        if (!g_buttons) { g_drag = Drag::None; g_drag_win = nullptr; }
        return;
    }

    // motion
    if (old.x == g_mouse.x && old.y == g_mouse.y) return;
    if (g_drag == Drag::Move && g_drag_win) {
        int ny = ev.y - g_drag_dy;
        if (g_shell_visible && ny < kMenubarHeight) ny = kMenubarHeight;
        if (g_drag_win->zoomed_) {
            g_drag_win->zoomed_ = false;
            g_drag_win->resize(g_drag_win->unzoomed_.w, g_drag_win->unzoomed_.h);
            g_drag_dx = g_drag_win->width() / 2;
        }
        g_drag_win->move_to(ev.x - g_drag_dx, ny);
        set_snap(can_tile(g_drag_win) ? snap_zone_at(ev.x, ev.y) : Snap::None);
        return;
    }
    if (g_drag == Drag::Resize && g_drag_win) {
        int dx = ev.x - g_drag_dx, dy = ev.y - g_drag_dy;
        int nw = g_drag_start.w, nh = g_drag_start.h, nx = g_drag_start.x;
        if (g_resize_edges & 1) nw += dx;
        if (g_resize_edges & 2) nh += dy;
        if (g_resize_edges & 4) { nw -= dx; nx += dx; }
        g_drag_win->resize(nw, nh);
        if (g_resize_edges & 4) g_drag_win->move_to(g_drag_start.x + g_drag_start.w - g_drag_win->width(), g_drag_win->y_);
        (void)nx;
        return;
    }
    if (g_drag == Drag::ViewBar && g_drag_win) {
        view_bar_drag(g_drag_win, ev.y);
        return;
    }
    if (g_drag == Drag::Content && g_drag_win) {
        Rect cr = g_drag_win->content_rect();
        g_drag_win->on_mouse_move(ev.x - cr.x, ev.y - cr.y + g_drag_win->view_scroll(), g_buttons);
        return;
    }
    // hover
    Window* w = window_at(ev.x, ev.y);
    Window* th = nullptr;
    if (w) {
        Rect f = w->frame();
        Rect tl(f.x + 8, f.y + 6, 66, kTitlebarHeight - 12);
        if (tl.contains(ev.x, ev.y) && !(w->flags() & WF_FULLSCREEN)) th = w;
        Rect cr = w->content_rect();
        if (cr.contains(ev.x, ev.y)) w->on_mouse_move(ev.x - cr.x, ev.y - cr.y + w->view_scroll(), 0);
    }
    if (th != g_hover_traffic_win) {
        if (g_hover_traffic_win) damage(Rect(g_hover_traffic_win->frame().x, g_hover_traffic_win->frame().y, 80, kTitlebarHeight));
        g_hover_traffic_win = th;
        if (th) damage(Rect(th->frame().x, th->frame().y, 80, kTitlebarHeight));
    }
}

void handle_key(const KeyEvent& e) {
    if (g_sw.open) {
        if (!(e.mods & g_sw.mod)) { sw_commit(); return; }   // modifier let go
        if (!e.pressed) return;
        switch (e.key) {
        case Key::Tab: sw_step(e.shift() ? -1 : 1); break;
        case Key::Right: case Key::Down: sw_step(1); break;
        case Key::Left: case Key::Up: sw_step(-1); break;
        case Key::Enter: sw_commit(); break;
        case Key::Escape: sw_close(); break;
        default: break;
        }
        return;
    }
    if (!e.pressed) return;
    if (e.key == Key::Tab && (e.alt() || e.super())) {
        sw_open(e.alt() ? MOD_ALT : MOD_SUPER, e.shift());
        return;
    }
    // Super/Ctrl+Shift+3 and +4 (the shifted characters depend on the layout)
    if (e.cmd() && e.shift() && e.key == Key::Char) {
        uint32_t c = e.ch;
        if (c == '3' || c == '#' || c == 0xA3 || c == 0xA7) { capture_now(false); return; }
        if (c == '4' || c == '$') { capture_now(true); return; }
    }
    if (shell_key(e)) return;
    // global shortcuts
    if (e.cmd() && e.key == Key::Char) {
        uint32_t ch = e.ch | 0x20;
        if (ch == 'w' && g_active && !(g_active->flags() & WF_FULLSCREEN) && e.super()) { g_active->close(); return; }
        if (ch == 'q' && g_active && e.super()) {
            const char* id = g_active->app_id();
            Vector<Window*> ws = g_windows;
            for (Window* w : ws) if (strcmp(w->app_id(), id) == 0) w->close();
            return;
        }
        if (ch == 'm' && g_active && e.super()) { minimize(g_active); return; }
    }
    // Super+arrows: tile the active window (halves, quarters, maximize), Super+Down undoes / minimizes
    if (e.super() && !e.ctrl() && !e.alt() && can_tile(g_active) &&
        (e.key == Key::Left || e.key == Key::Right || e.key == Key::Up || e.key == Key::Down)) {
        Window* w = g_active;
        Snap cur = current_snap(w), to = Snap::None;
        bool untile = false;
        switch (e.key) {
        case Key::Left:
            if (cur == Snap::Right || cur == Snap::TopRight || cur == Snap::BottomRight) untile = true;
            else to = cur == Snap::TopLeft || cur == Snap::BottomLeft ? cur : Snap::Left;
            break;
        case Key::Right:
            if (cur == Snap::Left || cur == Snap::TopLeft || cur == Snap::BottomLeft) untile = true;
            else to = cur == Snap::TopRight || cur == Snap::BottomRight ? cur : Snap::Right;
            break;
        case Key::Up:
            to = cur == Snap::Left ? Snap::TopLeft : cur == Snap::Right ? Snap::TopRight :
                 cur == Snap::BottomLeft ? Snap::Left : cur == Snap::BottomRight ? Snap::Right : Snap::Max;
            break;
        default:   // Down
            if (cur == Snap::Left) to = Snap::BottomLeft;
            else if (cur == Snap::Right) to = Snap::BottomRight;
            else if (cur == Snap::TopLeft) to = Snap::Left;
            else if (cur == Snap::TopRight) to = Snap::Right;
            else if (w->zoomed_) untile = true;
            else if (!(w->flags() & WF_NO_MINIMIZE)) { minimize(w); return; }
            break;
        }
        if (untile) untile_window(w);
        else if (to != cur) tile_window(w, to);
        return;
    }
    if (g_active) g_active->on_key(e);
}

}  // namespace

// ================================================================= public
void init() {
    FramebufferInfo& fb = framebuffer();
    W = fb.width;
    H = fb.height;
    g_screen = new Canvas(W, H);
    g_front = new Canvas(W, H);
    g_wallpaper = new Canvas(W, H);
    wallpaper_render(*g_wallpaper, g_wallpaper_index);
    build_cursor();
    g_mouse = Point{W / 2, H / 2};
    g_pointer_x = g_mouse.x;
    g_pointer_y = g_mouse.y;
    // blend speed decides how much glass this machine can afford
    uint64_t t0 = timer_ms();
    uint32_t v = 0x80404040;
    for (uint32_t i = 0; i < 3000000; i++) v = px_over(v | 0xFF000000u, 0x80000000u | (i & 0x7F7F7F));
    volatile uint32_t sink = v;
    (void)sink;
    uint64_t took = timer_ms() - t0;
    g_lite = took >= 20;
    glass_set_lite(g_lite);
    klog("wm: blend test %llu ms, %s graphics", (unsigned long long)took, g_lite ? "lite" : "full");
    shell_init();
    damage_all();
    input_set_pointer_hook(pointer_moved);
    klog("wm: %dx%d compositor ready", W, H);
}

int screen_width() { return W; }
int screen_height() { return H; }

Rect desktop_area() {
    if (!g_shell_visible) return Rect(0, 0, W, H);
    return Rect(0, kMenubarHeight, W, H - kMenubarHeight - shell_dock_height());
}

void add_window(Window* w) {
    g_windows.push(w);
    damage(shadow_bounds(w));
}

void remove_window(Window* w) {
    damage(shadow_bounds(w));
    g_windows.remove_value(w);
    if (g_drag_win == w) { g_drag_win = nullptr; g_drag = Drag::None; }
    if (g_last_click_win == w) g_last_click_win = nullptr;
    if (g_hover_traffic_win == w) g_hover_traffic_win = nullptr;
    if (g_drag_win == nullptr) set_snap(Snap::None);
    if (g_sw.open) {
        for (size_t i = 0; i < g_sw.list.size(); i++)
            if (g_sw.list[i] == w) { g_sw.list.remove(i); if (g_sw.sel > (int)i) g_sw.sel--; break; }
        if (g_sw.list.empty()) sw_close();
        else { if (g_sw.sel >= (int)g_sw.list.size()) g_sw.sel = 0; damage_all(); }
    }
    if (g_active == w) {
        g_active = nullptr;
        for (size_t i = g_windows.size(); i > 0; i--)
            if (!g_windows[i - 1]->minimized_ && !g_windows[i - 1]->closing_) { focus(g_windows[i - 1]); break; }
    }
    shell_windows_changed();
}

Window* active_window() { return g_active; }
const Vector<Window*>& windows() { return g_windows; }

void focus(Window* w) {
    if (w && (w->closing_)) return;
    Window* old = g_active;
    if (w) {
        g_windows.remove_value(w);
        g_windows.push(w);
        damage(shadow_bounds(w));
    }
    if (old != w) {
        g_active = w;
        if (old) { old->on_focus(false); damage(shadow_bounds(old)); }
        if (w) w->on_focus(true);
        shell_windows_changed();
    }
}

void minimize(Window* w) {
    w->minimized_ = true;
    damage(shadow_bounds(w));
    if (g_active == w) {
        g_active = nullptr;
        for (size_t i = g_windows.size(); i > 0; i--)
            if (!g_windows[i - 1]->minimized_ && !g_windows[i - 1]->closing_) { focus(g_windows[i - 1]); break; }
    }
    shell_windows_changed();
}

void restore(Window* w) {
    if (!w->minimized_) return;
    w->minimized_ = false;
    w->opened_at_ = timer_ms();
    damage(shadow_bounds(w));
    shell_windows_changed();
}

void toggle_zoom(Window* w) {
    if (!(w->flags() & WF_RESIZABLE)) return;
    Snap cur = current_snap(w);
    if (w->zoomed_ && (cur == Snap::Max || cur == Snap::None)) untile_window(w);
    else tile_window(w, Snap::Max);    // a half/quarter tile grows to full, keeping its old free size
}

void damage(Rect r) { add_damage(r); }
void take_screenshot(bool window_only, int delay_ms) {
    if (delay_ms <= 0) { capture_now(window_only); return; }
    g_shot_window = window_only;
    g_shot_at_ms = timer_ms() + delay_ms;
}
void damage_all() { g_damage.clear(); g_damage.push(Rect(0, 0, W, H)); }

void set_shell_visible(bool v) { g_shell_visible = v; damage_all(); }
bool shell_visible() { return g_shell_visible; }
void set_wallpaper(int i) {
    g_wallpaper_index = (i % wallpaper_count() + wallpaper_count()) % wallpaper_count();
    wallpaper_render(*g_wallpaper, g_wallpaper_index);
    damage_all();
}
int wallpaper() { return g_wallpaper_index; }
const Canvas& wallpaper_canvas() { return *g_wallpaper; }
void set_glass_intensity(float k) { g_glass_k = kclamp(k, 0.0f, 1.5f); damage_all(); }
float glass_intensity() { return g_glass_k; }
void set_dock_magnification(bool on) { g_dock_mag = on; damage_all(); }
bool dock_magnification() { return g_dock_mag && !g_lite; }
void set_reduce_transparency(bool on) { g_reduce_transparency = on; damage_all(); }
bool reduce_transparency() { return g_reduce_transparency; }

void clipboard_set(const char* t) { g_clipboard = t; }
const char* clipboard_get() { return g_clipboard.c_str(); }
Point mouse_pos() { return g_mouse; }
uint64_t frame_count() { return g_frames; }
float fps() { return g_fps; }
void defer(Func<void()> fn) { g_deferred.push(std::move(fn)); }
void set_logout_handler(Func<void()> fn) { g_logout = fn; }
void request_logout() { if (g_logout) defer(g_logout); }

void frame() {
    uint64_t now = timer_ms();
    // input; plain pointer motion is coalesced so a slow frame can't queue up hover work
    InputEvent ev, motion;
    bool have_motion = false;
    int n = 0;
    while (input_poll(&ev) && n++ < 256) {
        if (ev.type == InputEvent::MOUSE && !ev.wheel && !ev.buttons && !g_buttons) {
            motion = ev;
            have_motion = true;
            continue;
        }
        if (have_motion) { handle_mouse(motion); have_motion = false; }
        if (ev.type == InputEvent::MOUSE) handle_mouse(ev);
        else handle_key(ev.key);
    }
    if (have_motion) handle_mouse(motion);
    // Input handlers stamp things with timer_ms() (notifications, the switcher, screenshots);
    // an older `now` would make their age negative, i.e. huge as unsigned, and expire them at once.
    now = timer_ms();
    {
        uint64_t t = now;
        if (g_sw.open && !g_sw.shown && t - g_sw.opened_ms >= kSwShowDelay) { g_sw.shown = true; sw_damage(); }
        if (g_shot_at_ms && t >= g_shot_at_ms) { g_shot_at_ms = 0; capture_now(g_shot_window); }
        if (g_flash_ms) {
            add_damage(Rect(0, 0, W, H));
            if (t - g_flash_ms >= kFlashMs + 20) g_flash_ms = 0;
        }
    }
    // deferred work (window deletion etc.)
    if (!g_deferred.empty()) {
        Vector<Func<void()>> work = std::move(g_deferred);
        g_deferred = Vector<Func<void()>>();
        for (auto& f : work) f();
    }
    // no frame cap: the loop runs once per timer tick (up to 1000 fps)
    uint64_t prev = g_last_frame_ms;
    g_last_frame_ms = now;
    // ticks
    Vector<Window*> ws = g_windows;
    for (Window* w : ws)
        if (w->ticking() && !w->closing_) {
            w->on_tick(now);
            Widget* f = w->focused_widget();
            if (f && f->wants_initial_focus() && w == g_active && (now / 530) != (prev / 530)) w->invalidate();
        }
    // shell animations advance per step, so they keep a fixed 60 Hz clock
    if (now - g_last_shell_ms >= 16) {
        shell_tick(now);
        g_last_shell_ms = now;
    }
    // paint dirty windows
    uint64_t work_t0 = timer_ms();
    // when drawing is slow, leave as much time free as the last frame took so the
    // cursor and input keep moving between repaints
    bool draw_now = work_t0 >= g_next_draw_ms;
    if (draw_now) for (Window* w : g_windows)
        if (w->needs_paint() && !w->closing_) {
            w->paint_now();     // damages its content; glass is grown in normalize_damage if it shows through
        }
    uint64_t paint_ms = timer_ms() - work_t0;
    // compose
    if (draw_now && !g_damage.empty()) {
        normalize_damage();
        Vector<Rect> dmg = std::move(g_damage);
        g_damage = Vector<Rect>();
        for (Rect& r : dmg) compose_region(r);
        for (Rect& r : dmg) present(r);
        uint64_t work_ms = timer_ms() - work_t0;
        if (g_drag == Drag::Move && !g_cheap_drag && (g_lite || work_ms > 20)) g_cheap_drag = true;
        g_next_draw_ms = work_ms >= 8 ? timer_ms() + kmin(work_ms, (uint64_t)100) : 0;
        if (work_ms >= 30) {
            static uint64_t last_slow_log;
            if (!last_slow_log || timer_ms() - last_slow_log >= 1000) {
                last_slow_log = timer_ms();
                klog("wm: slow frame %u ms (paint %u, compose %u)", (unsigned)work_ms, (unsigned)paint_ms, (unsigned)(work_ms - paint_ms));
            }
        }
        // keep g_cursor_drawn: draw_cursor_to_fb() restores the old spot from the back buffer
        draw_cursor_to_fb();
    } else if (g_cursor_drawn.x != g_pointer_x || g_cursor_drawn.y != g_pointer_y) {
        draw_cursor_to_fb();
    }
    cpu_store_fence();
    g_frames++;
    g_fps_n++;
    if (now - g_fps_t0 >= 1000) {
        g_fps = g_fps_n * 1000.0f / (now - g_fps_t0);
        g_fps_t0 = now;
        g_fps_n = 0;
    }
}

}  // namespace wm
