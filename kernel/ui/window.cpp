#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/wm.h"
#include "ui/theme.h"
#include "arch/interrupts.h"

Window::Window(const char* title, int cw, int ch, uint32_t flags) : title_(title), flags_(flags), cw_(cw), ch_(ch) {
    if (flags & WF_FULLSCREEN) {
        cw_ = wm::screen_width();
        ch_ = wm::screen_height();
    }
    content_.resize(cw_, ch_);
    center();
    opened_at_ = timer_ms();
    wm::add_window(this);
}

Window::~Window() {
    for (Widget* w : widgets_) delete w;
    wm::remove_window(this);
}

Rect Window::frame() const {
    if (flags_ & WF_FULLSCREEN) return Rect(x_, y_, cw_, ch_);
    return Rect(x_, y_, cw_, ch_ + kTitlebarHeight);
}

Rect Window::content_rect() const {
    if (flags_ & WF_FULLSCREEN) return Rect(x_, y_, cw_, ch_);
    return Rect(x_, y_ + kTitlebarHeight, cw_, ch_);
}

void Window::invalidate() {
    dirty_ = true;
}

void Window::paint_now() {
    dirty_ = false;
    content_.reset_clip();
    on_paint(content_);
    wm::damage(content_rect());
}

void Window::close() {
    if (closing_) return;
    if (!on_close()) return;
    closing_ = true;
    wm::damage(frame().expand(60));
    Window* self = this;
    wm::defer([self] { delete self; });
}

void Window::set_title(const char* t) {
    title_ = t;
    wm::damage(frame());
}

void Window::resize(int w, int h) {
    if (w < min_w_) w = min_w_;
    if (h < min_h_) h = min_h_;
    if (w == cw_ && h == ch_) return;
    wm::damage(frame().expand(60));
    cw_ = w;
    ch_ = h;
    content_.resize(w, h);
    on_resize();
    dirty_ = true;
    wm::damage(frame().expand(60));
}

void Window::move_to(int x, int y) {
    wm::damage(frame().expand(60));
    x_ = x;
    y_ = y;
    wm::damage(frame().expand(60));
}

void Window::center() {
    Rect d = wm::desktop_area();
    if (flags_ & WF_FULLSCREEN) { x_ = 0; y_ = 0; return; }
    Rect f = frame();
    // cascade new windows a little
    static int cascade = 0;
    x_ = d.x + (d.w - f.w) / 2 + cascade;
    y_ = d.y + (d.h - f.h) / 2 + cascade / 2;
    cascade = (cascade + 26) % 130;
    if (y_ < d.y) y_ = d.y;
    if (x_ < 0) x_ = 0;
}

bool Window::focused() const { return wm::active_window() == this; }
void Window::bring_to_front() { wm::focus(this); }

Widget* Window::add(Widget* w) {
    w->window = this;
    widgets_.push(w);
    if (!focus_ && w->focusable() && w->visible) {
        // don't auto-focus buttons; text inputs get initial focus
        if (w->wants_initial_focus()) focus_ = w;
    }
    invalidate();
    return w;
}

void Window::remove(Widget* w) {
    if (focus_ == w) focus_ = nullptr;
    if (captured_ == w) captured_ = nullptr;
    if (hovered_ == w) hovered_ = nullptr;
    widgets_.remove_value(w);
    delete w;
    invalidate();
}

void Window::focus_widget(Widget* w) {
    if (focus_ == w) return;
    Widget* old = focus_;
    focus_ = w;
    if (old) old->focus_changed(false);
    if (w) w->focus_changed(true);
    invalidate();
}

void Window::on_paint(Canvas& c) {
    if (flags_ & WF_GLASS) c.clear(rgba(0, 0, 0, 0));
    else {
        c.clear(theme().window_bg);
        if (sidebar_w_ > 0) {
            c.save();
            c.clip_to(Rect(0, 0, sidebar_w_, ch_));
            c.clear(rgba(0, 0, 0, 0));
            c.restore();
            c.fill_rect(Rect(sidebar_w_ - 1, 0, 1, ch_), theme().separator);
        }
    }
    for (Widget* w : widgets_)
        if (w->visible) {
            c.save();
            w->paint(c);
            c.restore();
        }
}

void Window::paint_widgets(Canvas& c) {
    for (Widget* w : widgets_)
        if (w->visible) { c.save(); w->paint(c); c.restore(); }
}

static Widget* hit(const Vector<Widget*>& ws, int x, int y) {
    for (size_t i = ws.size(); i > 0; i--) {
        Widget* w = ws[i - 1];
        if (w->visible && w->enabled && w->bounds.contains(x, y)) return w;
    }
    return nullptr;
}

void Window::on_mouse_down(int x, int y, int button) {
    Widget* w = hit(widgets_, x, y);
    captured_ = w;
    if (w) {
        if (w->focusable()) focus_widget(w);
        w->mouse_down(x, y, button);
    } else if (focus_) {
        focus_widget(nullptr);
    }
}

void Window::on_mouse_up(int x, int y, int button) {
    Widget* w = captured_ ? captured_ : hit(widgets_, x, y);
    captured_ = nullptr;
    if (w) w->mouse_up(x, y, button);
}

void Window::on_mouse_move(int x, int y, int buttons) {
    if (captured_ && buttons) { captured_->mouse_move(x, y, buttons); return; }
    Widget* w = hit(widgets_, x, y);
    if (w != hovered_) {
        if (hovered_) { hovered_->hovered = false; hovered_->hover_changed(false); }
        hovered_ = w;
        if (w) { w->hovered = true; w->hover_changed(true); }
    }
    if (w) w->mouse_move(x, y, buttons);
}

void Window::on_scroll(int x, int y, int delta) {
    Widget* w = hit(widgets_, x, y);
    if (w) w->scroll(x, y, delta);
}

void Window::on_key(const KeyEvent& e) {
    if (focus_ && focus_->visible && focus_->key(e)) return;
    if (e.key == Key::Tab) {
        // cycle focus
        Vector<Widget*> f;
        for (Widget* w : widgets_) if (w->visible && w->enabled && w->focusable()) f.push(w);
        if (f.empty()) return;
        long i = focus_ ? f.index_of(focus_) : -1;
        i = e.shift() ? (i <= 0 ? (long)f.size() - 1 : i - 1) : (i + 1) % (long)f.size();
        focus_widget(f[i]);
    }
}
