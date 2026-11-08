#pragma once
// Window: base class for every app window.
//
// Subclass it, override the on_* handlers, and paint in on_paint(). The canvas
// passed to on_paint covers the content area (below the glass titlebar), with
// (0,0) at its top-left. Call invalidate() whenever the content must be
// repainted. Coordinates in mouse handlers are content-local too.
//
// Widgets (ui/widgets.h) can be added with add(); the default handlers paint
// them and route events to them, so call Window::on_paint(c) etc. from your
// overrides if you mix custom drawing with widgets.
#include "gfx/canvas.h"
#include "drivers/input.h"
#include "lib/vector.h"
#include "lib/kstring.h"
#include "ui/func.h"

class Widget;

enum WindowFlags : uint32_t {
    WF_RESIZABLE = 1 << 0,     // user can resize (bottom/right edges)
    WF_GLASS = 1 << 1,         // whole content area is glass: paint with transparency (alpha < 255) to let it show
    WF_NO_TITLE = 1 << 2,      // no title text (traffic lights still shown)
    WF_FULLSCREEN = 1 << 3,    // covers the whole screen, no chrome (installer, login, OOBE)
    WF_NO_MINIMIZE = 1 << 4,
    WF_NO_ZOOM = 1 << 5,
    WF_DIALOG = 1 << 6,        // small modal-ish panel, centered, no minimize/zoom
    WF_HIDDEN_IN_DOCK = 1 << 7,
};

struct MenuItem {
    String title;              // "-" = separator
    String shortcut;           // display only, e.g. "⌘N"
    Func<void()> action;
    bool enabled = true;
    bool checked = false;
};

struct Menu {
    String title;              // "File"
    Vector<MenuItem> items;
    void add(const char* t, Func<void()> action, const char* shortcut = "") {
        MenuItem& m = items.emplace();
        m.title = t; m.action = action; m.shortcut = shortcut;
    }
    void separator() { MenuItem& m = items.emplace(); m.title = "-"; }
};

class Window {
public:
    Window(const char* title, int content_w, int content_h, uint32_t flags = WF_RESIZABLE);
    virtual ~Window();

    // ---- override these
    virtual void on_paint(Canvas& c);                    // default: background + widgets
    virtual void on_mouse_down(int x, int y, int button);
    virtual void on_mouse_up(int x, int y, int button);
    virtual void on_mouse_move(int x, int y, int buttons);
    virtual void on_double_click(int x, int y) {}
    virtual void on_scroll(int x, int y, int delta);     // delta > 0 = scroll down (lines)
    virtual void on_key(const KeyEvent& e);              // only key presses (pressed == true)
    virtual void on_tick(uint64_t now_ms) {}             // ~60 Hz when set_ticking(true)
    virtual void on_resize() {}                          // content size changed
    virtual bool on_close() { return true; }             // return false to keep the window open
    virtual void on_focus(bool focused) {}
    // Menus shown in the menubar while this window is active (after the app menu).
    virtual void build_menus(Vector<Menu>& menus) {}

    // ---- API
    void invalidate();                                   // repaint content next frame
    void close();                                        // closes after this event
    void set_title(const char* t);
    const char* title() const { return title_.c_str(); }
    int width() const { return cw_; }                    // content size
    int height() const { return ch_; }
    void resize(int content_w, int content_h);
    void move_to(int x, int y);                          // frame position (screen)
    void center();
    Rect frame() const;                                  // screen rect incl. titlebar
    Rect content_rect() const;                           // screen rect of content
    void set_ticking(bool on) { ticking_ = on; }
    bool ticking() const { return ticking_; }
    void set_min_size(int w, int h) { min_w_ = w; min_h_ = h; }
    // Width of a glass sidebar at the content's left edge (0 = none). The
    // sidebar region shows the frosted desktop behind the window; paint it
    // with transparent/translucent colors.
    void set_sidebar_width(int w) { sidebar_w_ = w; invalidate(); }
    int sidebar_width() const { return sidebar_w_; }
    uint32_t flags() const { return flags_; }
    bool focused() const;
    bool minimized() const { return minimized_; }
    void set_app_id(const char* id) { app_id_ = id; }
    const char* app_id() const { return app_id_.c_str(); }
    void bring_to_front();

    // ---- widgets
    Widget* add(Widget* w);                              // window takes ownership
    void remove(Widget* w);                              // deletes it
    void focus_widget(Widget* w);
    void paint_widgets(Canvas& c);                        // draw widgets only (for custom on_paint)
    Widget* focused_widget() const { return focus_; }
    const Vector<Widget*>& widgets() const { return widgets_; }

    // ---- for the window manager
    Canvas& content_canvas() { return content_; }
    bool needs_paint() const { return dirty_; }
    void paint_now();
    int x_ = 0, y_ = 0;              // frame top-left on screen
    bool minimized_ = false;
    bool closing_ = false;
    bool zoomed_ = false;
    Rect unzoomed_;
    int anim_ = 0;                   // open animation progress 0..255
    uint64_t opened_at_ = 0;

private:
    String title_;
    String app_id_;
    uint32_t flags_;
    int cw_, ch_;
    int min_w_ = 200, min_h_ = 120;
    int sidebar_w_ = 0;
    bool dirty_ = true;
    bool ticking_ = false;
    Canvas content_;
    Vector<Widget*> widgets_;
    Widget* focus_ = nullptr;
    Widget* captured_ = nullptr;
    Widget* hovered_ = nullptr;
};
