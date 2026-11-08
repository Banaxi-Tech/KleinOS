#pragma once
// Standard controls. Positions are content-local (Window content area).
// Create with new, add to a window with win->add(...); the window owns them.
//
//   auto* b = (Button*)add(new Button(Rect(20, 20, 100, 30), "OK"));
//   b->on_click = [this] { close(); };
#include "gfx/canvas.h"
#include "drivers/input.h"
#include "lib/kstring.h"
#include "lib/vector.h"
#include "ui/func.h"

class Window;

class Widget {
public:
    explicit Widget(Rect bounds) : bounds(bounds) {}
    virtual ~Widget() {}
    virtual void paint(Canvas& c) {}
    virtual void mouse_down(int x, int y, int button) {}   // x,y relative to window content
    virtual void mouse_up(int x, int y, int button) {}
    virtual void mouse_move(int x, int y, int buttons) {}
    virtual void double_click(int x, int y) {}
    virtual void scroll(int x, int y, int delta) {}
    virtual bool key(const KeyEvent& e) { return false; }  // return true if handled
    virtual void focus_changed(bool focused) {}
    virtual void hover_changed(bool hovered) {}
    virtual bool focusable() const { return false; }
    virtual bool wants_initial_focus() const { return false; }   // text inputs
    void invalidate();
    bool focused() const;

    Rect bounds;
    bool visible = true;
    bool enabled = true;
    bool hovered = false;
    Window* window = nullptr;
    int tag = 0;               // free for app use
};

class Label : public Widget {
public:
    Label(Rect r, const char* text, int size = 14, Font font = Font::Regular)
        : Widget(r), text(text), size(size), font(font) {}
    void paint(Canvas& c) override;
    void set_text(const char* t) { text = t; invalidate(); }
    String text;
    int size;
    Font font;
    Color color = 0;           // 0 = theme text color
    Align align = Align::Left;
    bool wrap = false;         // word-wrap inside bounds
};

class Button : public Widget {
public:
    enum Style { Normal, Primary, Destructive, Borderless, Glass };
    Button(Rect r, const char* text, Style style = Normal) : Widget(r), text(text), style(style) {}
    void paint(Canvas& c) override;
    void mouse_down(int x, int y, int b) override;
    void mouse_up(int x, int y, int b) override;
    void hover_changed(bool) override { invalidate(); }
    bool key(const KeyEvent& e) override;
    bool focusable() const override { return true; }
    String text;
    Style style;
    Func<void()> on_click;
    bool pressed = false;
    int font_size = 14;
    // optional icon drawn left of the text
    Func<void(Canvas&, Rect)> icon;
};

class TextField : public Widget {
public:
    TextField(Rect r, const char* placeholder = "") : Widget(r), placeholder(placeholder) {}
    void paint(Canvas& c) override;
    void mouse_down(int x, int y, int b) override;
    void mouse_move(int x, int y, int buttons) override;
    void double_click(int x, int y) override;
    bool key(const KeyEvent& e) override;
    void focus_changed(bool) override { invalidate(); }
    bool focusable() const override { return true; }
    bool wants_initial_focus() const override { return true; }
    void set_text(const char* t);
    const char* get_text() const { return text.c_str(); }
    void select_all();
    String text;
    String placeholder;
    bool password = false;
    bool rounded = false;        // pill shaped (search fields)
    bool search_icon = false;
    int max_length = 1024;
    int font_size = 14;
    Func<void()> on_change;
    Func<void()> on_submit;      // Enter pressed
    // filter: return false to reject a typed character
    Func<bool(uint32_t)> accept_char;
    int cursor = 0;              // byte offset
    int sel_anchor = -1;         // selection anchor (-1 = none)
    int scroll_x = 0;
};

// Multi-line text editor with scrolling, selection, clipboard.
class TextArea : public Widget {
public:
    explicit TextArea(Rect r) : Widget(r) {}
    void paint(Canvas& c) override;
    void mouse_down(int x, int y, int b) override;
    void mouse_move(int x, int y, int buttons) override;
    void double_click(int x, int y) override;
    void scroll(int x, int y, int delta) override;
    bool key(const KeyEvent& e) override;
    void focus_changed(bool) override { invalidate(); }
    bool focusable() const override { return true; }
    bool wants_initial_focus() const override { return !readonly; }
    void set_text(const char* t);
    const String& get_text() const { return text; }
    String text;
    int font_size = 14;
    Font font = Font::Regular;
    bool readonly = false;
    bool word_wrap = true;
    bool line_numbers = false;
    Color background = 0;        // 0 = theme field bg; with_alpha(...,0) = transparent
    Func<void()> on_change;
    // optional per-line syntax coloring: fills colors[] (one per byte of the line)
    Func<void(const char* line, int len, Color* colors)> highlighter;
    int cursor = 0;
    int sel_anchor = -1;
    int scroll_y = 0;            // px
    void ensure_cursor_visible();
private:
    struct Line { int start, len, y; };
    Vector<Line> lines_;
    int layout_width_ = -1;
    void layout();
    int offset_at(int x, int y);
    Point pos_of(int offset);
    friend class TextAreaHelper;
};

class Checkbox : public Widget {
public:
    Checkbox(Rect r, const char* text, bool checked = false) : Widget(r), text(text), checked(checked) {}
    void paint(Canvas& c) override;
    void mouse_up(int x, int y, int b) override;
    bool key(const KeyEvent& e) override;
    bool focusable() const override { return true; }
    String text;
    bool checked;
    Func<void(bool)> on_change;
};

// iOS-style switch with a liquid glass thumb.
class Toggle : public Widget {
public:
    Toggle(Rect r, bool on = false) : Widget(r), on(on) {}
    void paint(Canvas& c) override;
    void mouse_up(int x, int y, int b) override;
    bool on;
    Func<void(bool)> on_change;
};

class Slider : public Widget {
public:
    Slider(Rect r, float value = 0.5f) : Widget(r), value(value) {}
    void paint(Canvas& c) override;
    void mouse_down(int x, int y, int b) override;
    void mouse_move(int x, int y, int buttons) override;
    void mouse_up(int x, int y, int b) override;
    float value;                 // 0..1
    Func<void(float)> on_change;
    bool dragging = false;
};

class ProgressBar : public Widget {
public:
    ProgressBar(Rect r, float value = 0) : Widget(r), value(value) {}
    void paint(Canvas& c) override;
    float value;                 // 0..1, < 0 = indeterminate
};

class SegmentedControl : public Widget {
public:
    SegmentedControl(Rect r, std::initializer_list<const char*> items) : Widget(r) {
        for (const char* s : items) segments.push(String(s));
    }
    void paint(Canvas& c) override;
    void mouse_down(int x, int y, int b) override;
    Vector<String> segments;
    int selected = 0;
    Func<void(int)> on_change;
};

// Scrollable list of rows with optional icons / detail text.
class ListView : public Widget {
public:
    struct Item {
        String text;
        String detail;           // right-aligned secondary text
        Func<void(Canvas&, Rect)> icon;
        void* data = nullptr;
        int tag = 0;
    };
    explicit ListView(Rect r) : Widget(r) {}
    void paint(Canvas& c) override;
    void mouse_down(int x, int y, int b) override;
    void double_click(int x, int y) override;
    void scroll(int x, int y, int delta) override;
    bool key(const KeyEvent& e) override;
    bool focusable() const override { return true; }
    void clear() { items.clear(); selected = -1; scroll_y = 0; invalidate(); }
    Item& add(const char* text, const char* detail = "") {
        Item& it = items.emplace(); it.text = text; it.detail = detail; invalidate(); return it;
    }
    void ensure_visible(int index);
    Vector<Item> items;
    int selected = -1;
    int row_height = 30;
    int scroll_y = 0;
    bool transparent = false;    // no background (sidebars)
    Func<void(int)> on_select;   // selection changed
    Func<void(int)> on_activate; // double-click / Enter
};

// Pop-up button: shows the selected option, opens a menu to choose another.
class Dropdown : public Widget {
public:
    Dropdown(Rect r, std::initializer_list<const char*> opts) : Widget(r) {
        for (const char* s : opts) options.push(String(s));
    }
    Dropdown(Rect r) : Widget(r) {}
    void paint(Canvas& c) override;
    void mouse_down(int x, int y, int b) override;
    bool focusable() const override { return true; }
    Vector<String> options;
    int selected = 0;
    Func<void(int)> on_change;
};

// Paint a standard rounded "card"/group box background (Settings style).
void draw_group_box(Canvas& c, Rect r);
