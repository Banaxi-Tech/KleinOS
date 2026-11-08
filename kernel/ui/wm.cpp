// Window manager + compositor.
//
// Every frame: drain input, run ticks, let dirty windows repaint into their
// own canvases, then recompose only the damaged screen regions into a RAM
// back buffer (wallpaper -> windows with shadows and glass chrome -> shell
// glass -> cursor) and copy those regions to the framebuffer. Glass needs the
// final backdrop under it, so damage that touches a glass surface is grown to
// cover the whole surface plus its blur margin before composing.
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
#include "lib/log.h"
#include "lib/math.h"

namespace wm {

namespace {

Canvas* g_screen;          // back buffer
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
float g_fps;
uint64_t g_fps_t0, g_fps_n;

// interaction state
enum class Drag { None, Move, Resize, Content, Shell };
Drag g_drag = Drag::None;
Window* g_drag_win;
int g_drag_dx, g_drag_dy;
int g_resize_edges;          // 1=right 2=bottom 4=left
Rect g_drag_start;
uint64_t g_last_click_ms;
Point g_last_click_pos;
Window* g_last_click_win;
int g_hover_traffic = -1;    // which window's traffic lights are hovered
Window* g_hover_traffic_win;

// cursor
Canvas* g_cursor;
Point g_cursor_drawn{-100, -100};
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
    s.draw_shadow(f, kWindowRadius, (active ? 26 : 16) >> (g_lite ? 1 : 0), theme().dark ? rgba(0, 0, 0, active ? 170 : 120) : rgba(0, 0, 0, active ? 75 : 45), 0, active ? 12 : 6);
    // glass part (titlebar / sidebar / whole window)
    Rect gr = window_glass_rect(w);
    if (gr.intersects(clip)) {
        GlassStyle gs = window_glass();
        gs.radius = kWindowRadius;
        // render glass for the full window shape but only inside the glass region
        s.restore();
        s.save();
        s.clip_to(clip.intersect(gr));
        glass_render(s, f, gs, s.clip());
        s.restore();
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
        if (glassy) s.draw(cc, cr.x, cr.y);
        else s.blit(cc, cr.x, cr.y);
        s.restore();
    }
    // bottom R rows: coverage-masked so the corners come out round
    Rect tail = Rect(cr.x, cr.bottom() - R, cr.w, R).intersect(clip);
    for (int y = tail.y; y < tail.bottom(); y++) {
        uint32_t* dst = s.row(y);
        const uint32_t* src = cc.row(y - cr.y);
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

void compose_region(Rect r) {
    Canvas& s = *g_screen;
    s.reset_clip();
    s.save();
    s.clip_to(r);
    s.blit(*g_wallpaper, 0, 0);
    shell_draw_desktop(s, r);
    s.restore();
    for (Window* w : g_windows) {
        if (w->minimized_ || w->closing_) continue;
        compose_window(s, w, r);
    }
    if (g_shell_visible) shell_draw(s, r);
    shell_draw_overlays(s, r);
}

void present(Rect r) {
    FramebufferInfo& fb = framebuffer();
    for (int y = r.y; y < r.bottom(); y++) {
        uint32_t* dst = (uint32_t*)((uint8_t*)fb.address + (size_t)y * fb.pitch) + r.x;
        const uint32_t* src = g_screen->row(y) + r.x;
        memcpy(dst, src, r.w * 4);
    }
}

void draw_cursor_to_fb() {
    // cursor is drawn directly into the framebuffer over the back buffer copy
    FramebufferInfo& fb = framebuffer();
    // restore old position from back buffer
    Rect old(g_cursor_drawn.x, g_cursor_drawn.y, kCursorW, kCursorH);
    old = old.intersect(Rect(0, 0, W, H));
    if (!old.empty()) present(old);
    Rect nr(g_mouse.x, g_mouse.y, kCursorW, kCursorH);
    nr = nr.intersect(Rect(0, 0, W, H));
    for (int y = nr.y; y < nr.bottom(); y++) {
        uint32_t* dst = (uint32_t*)((uint8_t*)fb.address + (size_t)y * fb.pitch);
        const uint32_t* bb = g_screen->row(y);
        const uint32_t* cur = g_cursor->row(y - g_mouse.y);
        for (int x = nr.x; x < nr.right(); x++) dst[x] = px_over(bb[x], cur[x - g_mouse.x]);
    }
    g_cursor_drawn = g_mouse;
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
            if (cr.contains(ev.x, ev.y)) w->on_scroll(ev.x - cr.x, ev.y - cr.y, ev.wheel);
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
        if (cr.contains(ev.x, ev.y)) {
            uint64_t now = timer_ms();
            int lx = ev.x - cr.x, ly = ev.y - cr.y;
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
            g_drag_win->on_mouse_up(ev.x - cr.x, ev.y - cr.y, button);
        }
        if (g_drag == Drag::Move && g_drag_win) {
            // keep titlebar reachable
            Rect d = desktop_area();
            Window* w = g_drag_win;
            if (w->y_ < d.y) w->move_to(w->x_, d.y);
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
    if (g_drag == Drag::Content && g_drag_win) {
        Rect cr = g_drag_win->content_rect();
        g_drag_win->on_mouse_move(ev.x - cr.x, ev.y - cr.y, g_buttons);
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
        if (cr.contains(ev.x, ev.y)) w->on_mouse_move(ev.x - cr.x, ev.y - cr.y, 0);
    }
    if (th != g_hover_traffic_win) {
        if (g_hover_traffic_win) damage(Rect(g_hover_traffic_win->frame().x, g_hover_traffic_win->frame().y, 80, kTitlebarHeight));
        g_hover_traffic_win = th;
        if (th) damage(Rect(th->frame().x, th->frame().y, 80, kTitlebarHeight));
    }
}

void handle_key(const KeyEvent& e) {
    if (!e.pressed) return;
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
    if (e.alt() && e.key == Key::Tab) {
        // cycle to the bottom-most visible window
        for (Window* w : g_windows)
            if (!w->minimized_ && !w->closing_ && w != g_active && !(w->flags() & WF_FULLSCREEN)) { focus(w); break; }
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
    g_wallpaper = new Canvas(W, H);
    wallpaper_render(*g_wallpaper, g_wallpaper_index);
    build_cursor();
    g_mouse = Point{W / 2, H / 2};
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
    Rect d = desktop_area();
    if (w->zoomed_) {
        w->zoomed_ = false;
        w->resize(w->unzoomed_.w, w->unzoomed_.h);
        w->move_to(w->unzoomed_.x, w->unzoomed_.y);
    } else {
        w->unzoomed_ = Rect(w->x_, w->y_, w->width(), w->height());
        w->zoomed_ = true;
        w->move_to(d.x + 6, d.y + 6);
        w->resize(d.w - 12, d.h - 12 - kTitlebarHeight);
    }
}

void damage(Rect r) { add_damage(r); }
void damage_all() { g_damage.clear(); g_damage.push(Rect(0, 0, W, H)); }

void set_shell_visible(bool v) { g_shell_visible = v; damage_all(); }
bool shell_visible() { return g_shell_visible; }
void set_wallpaper(int i) {
    g_wallpaper_index = (i % wallpaper_count() + wallpaper_count()) % wallpaper_count();
    wallpaper_render(*g_wallpaper, g_wallpaper_index);
    damage_all();
}
int wallpaper() { return g_wallpaper_index; }
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
    // input
    InputEvent ev;
    int n = 0;
    while (input_poll(&ev) && n++ < 256) {
        if (ev.type == InputEvent::MOUSE) handle_mouse(ev);
        else handle_key(ev.key);
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
    for (Window* w : g_windows)
        if (w->needs_paint() && !w->closing_) {
            w->paint_now();
            if (window_has_glass(w)) damage(window_glass_rect(w));
        }
    // compose
    if (!g_damage.empty()) {
        normalize_damage();
        Vector<Rect> dmg = std::move(g_damage);
        g_damage = Vector<Rect>();
        for (Rect& r : dmg) compose_region(r);
        for (Rect& r : dmg) present(r);
        // keep g_cursor_drawn: draw_cursor_to_fb() restores the old spot from the back buffer
        draw_cursor_to_fb();
    } else if (g_cursor_drawn.x != g_mouse.x || g_cursor_drawn.y != g_mouse.y) {
        draw_cursor_to_fb();
    }
    asm volatile("sfence");
    g_frames++;
    g_fps_n++;
    if (now - g_fps_t0 >= 1000) {
        g_fps = g_fps_n * 1000.0f / (now - g_fps_t0);
        g_fps_t0 = now;
        g_fps_n = 0;
    }
}

}  // namespace wm
