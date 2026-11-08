#include "ui/widgets.h"
#include "ui/window.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "ui/app.h"
#include "gfx/glass.h"
#include "lib/math.h"
#include "arch/interrupts.h"

void Widget::invalidate() { if (window) window->invalidate(); }
bool Widget::focused() const { return window && window->focused_widget() == this; }

static void focus_ring(Canvas& c, Rect r, float radius) {
    c.stroke_round_rect(r.expand(3), radius + 3, with_alpha(theme().accent, 110), 3.0f);
}

void draw_group_box(Canvas& c, Rect r) {
    const Theme& t = theme();
    c.fill_round_rect(r, 12, t.dark ? rgba(255, 255, 255, 14) : rgba(255, 255, 255, 255));
    c.stroke_round_rect(r, 12, t.separator, 1);
}

// ---------------------------------------------------------------- Label
void Label::paint(Canvas& c) {
    Color col = color ? color : theme().text;
    if (wrap) c.draw_text_wrapped(bounds, text.c_str(), col, size, font, align);
    else c.draw_text_in(bounds, text.c_str(), col, size, font, align);
}

// ---------------------------------------------------------------- Button
void Button::paint(Canvas& c) {
    const Theme& t = theme();
    Rect r = bounds;
    float rad = r.h >= 36 ? r.h / 2.0f : kControlRadius;
    Color fg = t.text;
    switch (style) {
    case Primary:
    case Destructive: {
        Color base = style == Primary ? t.accent : t.destructive;
        if (!enabled) base = with_alpha(base, 110);
        Color top = lighten(base, pressed ? 0 : 30), bot = darken(base, pressed ? 40 : 10);
        c.draw_shadow(r, rad, 3, rgba(0, 0, 0, 40), 0, 1);
        c.fill_gradient(r, top, bot, rad);
        c.stroke_round_rect(r, rad, rgba(255, 255, 255, 50), 1);
        fg = enabled ? hex(0xFFFFFF) : rgba(255, 255, 255, 160);
        break;
    }
    case Normal: {
        Color bg = t.control_bg;
        if (pressed) bg = darken(bg, 25);
        else if (hovered) bg = t.dark ? lighten(bg, 15) : darken(bg, 6);
        c.draw_shadow(r, rad, 2, rgba(0, 0, 0, t.dark ? 60 : 25), 0, 1);
        c.fill_round_rect(r, rad, bg);
        c.stroke_round_rect(r, rad, t.control_border, 1);
        if (!enabled) fg = t.text_tertiary;
        break;
    }
    case Glass: {
        // light glass pill: translucent white with rim highlight
        c.fill_round_rect(r, r.h / 2.0f, pressed ? rgba(255, 255, 255, 90) : hovered ? rgba(255, 255, 255, 70) : rgba(255, 255, 255, 46));
        c.stroke_round_rect(r, r.h / 2.0f, rgba(255, 255, 255, 120), 1);
        c.fill_gradient(Rect(r.x + 2, r.y + 1, r.w - 4, r.h / 2), rgba(255, 255, 255, 50), rgba(255, 255, 255, 0), r.h / 2.0f);
        fg = t.dark ? hex(0xFFFFFF) : hex(0x1C1C1E);
        rad = r.h / 2.0f;
        break;
    }
    case Borderless:
        if (pressed || hovered) c.fill_round_rect(r, rad, pressed ? t.separator : t.hover);
        fg = enabled ? t.accent : t.text_tertiary;
        break;
    }
    int tw = text_width(text.c_str(), font_size, Font::Medium);
    int iw = icon ? r.h - 12 : 0;
    int total = tw + (iw ? iw + (text.empty() ? 0 : 6) : 0);
    int x = r.x + (r.w - total) / 2;
    if (icon) {
        icon(c, Rect(x, r.y + 6, iw, iw));
        x += iw + 6;
    }
    if (!text.empty()) c.draw_text_in(Rect(x, r.y, r.right() - x, r.h), text.c_str(), fg, font_size, Font::Medium);
    if (focused() && style != Borderless) focus_ring(c, r, rad);
}

void Button::mouse_down(int, int, int) { if (enabled) { pressed = true; invalidate(); } }

void Button::mouse_up(int x, int y, int) {
    bool was = pressed;
    pressed = false;
    invalidate();
    if (was && enabled && bounds.contains(x, y) && on_click) on_click();
}

bool Button::key(const KeyEvent& e) {
    if ((e.key == Key::Space || e.key == Key::Enter) && enabled && on_click) { on_click(); return true; }
    return false;
}

// ---------------------------------------------------------------- text helpers
static int prev_char(const String& s, int i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80) i--;
    return i;
}
static int next_char(const String& s, int i) {
    if (i >= (int)s.size()) return (int)s.size();
    i++;
    while (i < (int)s.size() && ((uint8_t)s[i] & 0xC0) == 0x80) i++;
    return i;
}
static bool is_word(char c) { return isalnum(c) || c == '_' || ((uint8_t)c & 0x80); }

// ---------------------------------------------------------------- TextField
void TextField::set_text(const char* t) {
    text = t;
    cursor = (int)text.size();
    sel_anchor = -1;
    invalidate();
}

void TextField::select_all() {
    sel_anchor = 0;
    cursor = (int)text.size();
    invalidate();
}

static String masked(const String& s) {
    String m;
    const char* p = s.c_str();
    while (*p) { utf8_next(&p); m += "\xE2\x80\xA2"; }
    return m;
}

// map byte offset in text to byte offset in display string
static int disp_offset(const String& text, bool password, int off) {
    if (!password) return off;
    int n = 0;
    const char* p = text.c_str();
    const char* end = p + off;
    while (p < end && *p) { utf8_next(&p); n++; }
    return n * 3;
}

void TextField::paint(Canvas& c) {
    const Theme& t = theme();
    Rect r = bounds;
    float rad = rounded ? r.h / 2.0f : 7.0f;
    c.fill_round_rect(r, rad, t.field_bg);
    c.stroke_round_rect(r, rad, focused() ? with_alpha(t.accent, 200) : t.control_border, focused() ? 1.5f : 1.0f);
    if (focused()) focus_ring(c, r, rad);
    int pad = rounded ? 12 : 8;
    if (search_icon) {
        draw_symbol(c, Rect(r.x + 9, r.y + (r.h - 14) / 2, 14, 14), Symbol::Search, t.text_secondary, 1.5f);
        pad = 30;
    }
    Rect inner(r.x + pad, r.y, r.w - pad - 8, r.h);
    String disp = password ? masked(text) : text;
    int lh = line_height(font_size);
    int ty = r.y + (r.h - lh) / 2;
    int cur_px = text_width_n(disp.c_str(), disp_offset(text, password, cursor), font_size);
    if (cur_px - scroll_x > inner.w - 2) scroll_x = cur_px - inner.w + 2;
    if (cur_px - scroll_x < 0) scroll_x = cur_px;
    if (scroll_x < 0) scroll_x = 0;
    c.save();
    c.clip_to(inner);
    if (text.empty() && !placeholder.empty())
        c.draw_text(inner.x, ty, placeholder.c_str(), t.text_tertiary, font_size);
    if (sel_anchor >= 0 && sel_anchor != cursor && focused()) {
        int a = sel_anchor < cursor ? sel_anchor : cursor, b = sel_anchor < cursor ? cursor : sel_anchor;
        int xa = text_width_n(disp.c_str(), disp_offset(text, password, a), font_size);
        int xb = text_width_n(disp.c_str(), disp_offset(text, password, b), font_size);
        c.fill_rect(Rect(inner.x + xa - scroll_x, ty + 1, xb - xa, lh - 2), with_alpha(t.accent, 80));
    }
    c.draw_text(inner.x - scroll_x, ty, disp.c_str(), t.text, font_size);
    if (focused() && (timer_ms() / 530) % 2 == 0)
        c.fill_rect(Rect(inner.x + cur_px - scroll_x, ty + 2, 2, lh - 4), t.accent);
    c.restore();
    if (focused()) window->set_ticking(true);   // caret blink
}

int text_field_hit(TextField* f, int x) {
    int pad = f->search_icon ? 30 : (f->rounded ? 12 : 8);
    int px = x - (f->bounds.x + pad) + f->scroll_x;
    if (f->password) {
        String d = masked(f->text);
        int off = text_hit_test(d.c_str(), px, f->font_size) / 3;
        const char* p = f->text.c_str();
        for (int i = 0; i < off && *p; i++) utf8_next(&p);
        return (int)(p - f->text.c_str());
    }
    return text_hit_test(f->text.c_str(), px, f->font_size);
}

void TextField::mouse_down(int x, int y, int) {
    cursor = text_field_hit(this, x);
    sel_anchor = cursor;
    invalidate();
}

void TextField::mouse_move(int x, int y, int buttons) {
    if (!(buttons & MB_LEFT)) return;
    cursor = text_field_hit(this, x);
    invalidate();
}

void TextField::double_click(int, int) { select_all(); }

static void delete_selection(String& text, int& cursor, int& anchor) {
    if (anchor < 0 || anchor == cursor) { anchor = -1; return; }
    int a = anchor < cursor ? anchor : cursor, b = anchor < cursor ? cursor : anchor;
    text.erase(a, b - a);
    cursor = a;
    anchor = -1;
}

bool TextField::key(const KeyEvent& e) {
    bool changed = false;
    bool has_sel = sel_anchor >= 0 && sel_anchor != cursor;
    auto move = [&](int to) {
        if (e.shift()) { if (sel_anchor < 0) sel_anchor = cursor; }
        else sel_anchor = -1;
        cursor = to;
    };
    if (e.cmd() && e.key == Key::Char) {
        uint32_t ch = e.ch | 0x20;
        if (ch == 'a') { select_all(); return true; }
        if ((ch == 'c' || ch == 'x') && has_sel && !password) {
            int a = sel_anchor < cursor ? sel_anchor : cursor, b = sel_anchor < cursor ? cursor : sel_anchor;
            wm::clipboard_set(text.substr(a, b - a).c_str());
            if (ch == 'x') { delete_selection(text, cursor, sel_anchor); changed = true; }
            else return true;
        } else if (ch == 'v') {
            delete_selection(text, cursor, sel_anchor);
            const char* clip = wm::clipboard_get();
            String ins;
            for (const char* p = clip; *p; p++) if (*p != '\n' && *p != '\r') ins += *p;
            if ((int)(text.size() + ins.size()) <= max_length) {
                text.insert(cursor, ins.c_str(), ins.size());
                cursor += (int)ins.size();
                changed = true;
            }
        } else {
            return false;
        }
    } else {
        switch (e.key) {
        case Key::Left:
            if (has_sel && !e.shift()) { move(sel_anchor < cursor ? sel_anchor : cursor); break; }
            if (e.alt() || e.ctrl()) {
                int i = cursor;
                while (i > 0 && !is_word(text[i - 1])) i--;
                while (i > 0 && is_word(text[i - 1])) i--;
                move(i);
            } else move(prev_char(text, cursor));
            break;
        case Key::Right:
            if (has_sel && !e.shift()) { move(sel_anchor > cursor ? sel_anchor : cursor); break; }
            if (e.alt() || e.ctrl()) {
                int i = cursor, n = (int)text.size();
                while (i < n && !is_word(text[i])) i++;
                while (i < n && is_word(text[i])) i++;
                move(i);
            } else move(next_char(text, cursor));
            break;
        case Key::Home: case Key::Up: move(0); break;
        case Key::End: case Key::Down: move((int)text.size()); break;
        case Key::Backspace:
            if (has_sel) delete_selection(text, cursor, sel_anchor);
            else if (cursor > 0) { int p = prev_char(text, cursor); text.erase(p, cursor - p); cursor = p; }
            changed = true;
            break;
        case Key::Delete:
            if (has_sel) delete_selection(text, cursor, sel_anchor);
            else if (cursor < (int)text.size()) { int n = next_char(text, cursor); text.erase(cursor, n - cursor); }
            changed = true;
            break;
        case Key::Enter:
            if (on_submit) on_submit();
            return true;
        case Key::Escape:
            return false;
        case Key::Char:
        case Key::Space: {
            if (!e.ch) return false;
            if (accept_char && !accept_char(e.ch)) return true;
            delete_selection(text, cursor, sel_anchor);
            char buf[5];
            int n = utf8_encode(e.ch, buf);
            if ((int)text.size() + n > max_length) return true;
            text.insert(cursor, buf, n);
            cursor += n;
            changed = true;
            break;
        }
        default:
            return false;
        }
    }
    invalidate();
    if (changed && on_change) on_change();
    return true;
}

// ---------------------------------------------------------------- TextArea
void TextArea::set_text(const char* t) {
    text = t;
    cursor = 0;
    sel_anchor = -1;
    scroll_y = 0;
    layout_width_ = -1;
    invalidate();
}

static int gutter_width(TextArea* a) { return a->line_numbers ? text_width("0000", a->font_size - 1, Font::Mono) + 14 : 0; }

void TextArea::layout() {
    lines_.clear();
    int lh = line_height(font_size, font);
    int width = bounds.w - 16 - gutter_width(this);
    layout_width_ = bounds.w;
    int y = 0;
    const char* s = text.c_str();
    int n = (int)text.size();
    int i = 0;
    while (true) {
        int start = i;
        int last_space = -1;
        float w = 0;
        while (i < n && s[i] != '\n') {
            if (word_wrap) {
                const char* p = s + i;
                uint32_t cp = utf8_next(&p);
                const Glyph* g = font_glyph(font, font_size, cp == '\t' ? ' ' : cp, 0);
                float adv = g ? g->advance * (cp == '\t' ? 4 : 1) : 0;
                if (w + adv > width && i > start) {
                    if (last_space > start) i = last_space + 1;
                    break;
                }
                w += adv;
                if (s[i] == ' ') last_space = i;
                i = (int)(p - s);
            } else {
                i++;
            }
        }
        lines_.push(Line{start, i - start, y});
        y += lh;
        if (i >= n) break;
        if (s[i] == '\n') {
            i++;
            if (i >= n) { lines_.push(Line{i, 0, y}); break; }
        }
    }
    if (lines_.empty()) lines_.push(Line{0, 0, 0});
}

Point TextArea::pos_of(int off) {
    int lh = line_height(font_size, font);
    for (size_t li = 0; li < lines_.size(); li++) {
        Line& l = lines_[li];
        bool last = li + 1 == lines_.size();
        if (off >= l.start && (off <= l.start + l.len && (last || off < lines_[li + 1].start || off == l.start + l.len))) {
            if (off <= l.start + l.len) {
                int x = text_width_n(text.c_str() + l.start, off - l.start, font_size, font);
                return Point{x, l.y};
            }
        }
    }
    Line& l = lines_.back();
    (void)lh;
    return Point{text_width_n(text.c_str() + l.start, l.len, font_size, font), l.y};
}

int TextArea::offset_at(int x, int y) {
    int lh = line_height(font_size, font);
    int ly = y - (bounds.y + 6) + scroll_y;
    int li = ly / lh;
    if (li < 0) li = 0;
    if (li >= (int)lines_.size()) li = (int)lines_.size() - 1;
    Line& l = lines_[li];
    String line = text.substr(l.start, l.len);
    int px = x - (bounds.x + 8 + gutter_width(this));
    return l.start + text_hit_test(line.c_str(), px, font_size, font);
}

void TextArea::ensure_cursor_visible() {
    if (layout_width_ != bounds.w) layout();
    Point p = pos_of(cursor);
    int lh = line_height(font_size, font);
    int view = bounds.h - 12;
    if (p.y < scroll_y) scroll_y = p.y;
    if (p.y + lh > scroll_y + view) scroll_y = p.y + lh - view;
    if (scroll_y < 0) scroll_y = 0;
}

void TextArea::paint(Canvas& c) {
    const Theme& t = theme();
    layout();
    Rect r = bounds;
    Color bg = background ? background : t.field_bg;
    if (color_a(bg)) c.fill_rect(r, bg);
    int lh = line_height(font_size, font);
    int gw = gutter_width(this);
    c.save();
    c.clip_to(r);
    int sa = -1, sb = -1;
    if (sel_anchor >= 0 && sel_anchor != cursor) { sa = kmin(sel_anchor, cursor); sb = kmax(sel_anchor, cursor); }
    Color colors[1024];
    int logical_line = 1;
    for (size_t li = 0; li < lines_.size(); li++) {
        Line& l = lines_[li];
        bool new_logical = li == 0 || text[l.start - 1] == '\n';
        if (li > 0 && new_logical) logical_line++;
        int y = r.y + 6 + l.y - scroll_y;
        if (y + lh < r.y || y > r.bottom()) continue;
        int x0 = r.x + 8 + gw;
        if (gw && new_logical) {
            char num[12];
            snprintf(num, sizeof(num), "%d", logical_line);
            int nw = text_width(num, font_size - 1, Font::Mono);
            c.draw_text(r.x + gw - 6 - nw, y + 1, num, t.text_tertiary, font_size - 1, Font::Mono);
        }
        // selection
        if (sa >= 0 && sb > l.start && sa <= l.start + l.len) {
            int a = kmax(sa, l.start), b = kmin(sb, l.start + l.len);
            int xa = text_width_n(text.c_str() + l.start, a - l.start, font_size, font);
            int xb = text_width_n(text.c_str() + l.start, b - l.start, font_size, font);
            if (sb > l.start + l.len) xb += 6;
            c.fill_rect(Rect(x0 + xa, y, xb - xa, lh), with_alpha(t.accent, 70));
        }
        if (highlighter && l.len > 0 && l.len < 1024) {
            for (int k = 0; k < l.len; k++) colors[k] = t.text;
            highlighter(text.c_str() + l.start, l.len, colors);
            // draw runs of equal color
            int k = 0;
            int x = x0;
            while (k < l.len) {
                int e = k;
                while (e < l.len && colors[e] == colors[k]) e++;
                x += c.draw_text_n(x, y, text.c_str() + l.start + k, e - k, colors[k], font_size, font);
                k = e;
            }
        } else {
            c.draw_text_n(x0, y, text.c_str() + l.start, l.len, t.text, font_size, font);
        }
    }
    if (focused() && !readonly && (timer_ms() / 530) % 2 == 0) {
        Point p = pos_of(cursor);
        c.fill_rect(Rect(r.x + 8 + gw + p.x, r.y + 6 + p.y - scroll_y + 1, 2, lh - 2), t.accent);
    }
    // scrollbar
    int content_h = lines_.back().y + lh + 12;
    if (content_h > r.h) {
        int bar_h = kmax(30, r.h * r.h / content_h);
        int bar_y = r.y + (r.h - bar_h) * scroll_y / kmax(1, content_h - r.h);
        c.fill_round_rect(Rect(r.right() - 7, bar_y + 2, 5, bar_h - 4), 2.5f, t.dark ? rgba(255, 255, 255, 60) : rgba(0, 0, 0, 60));
    }
    c.restore();
    if (focused()) window->set_ticking(true);
}

void TextArea::mouse_down(int x, int y, int) {
    if (layout_width_ != bounds.w) layout();
    cursor = offset_at(x, y);
    sel_anchor = cursor;
    invalidate();
}

void TextArea::mouse_move(int x, int y, int buttons) {
    if (!(buttons & MB_LEFT)) return;
    cursor = offset_at(x, y);
    ensure_cursor_visible();
    invalidate();
}

void TextArea::double_click(int x, int y) {
    int i = offset_at(x, y), a = i, b = i, n = (int)text.size();
    while (a > 0 && is_word(text[a - 1])) a--;
    while (b < n && is_word(text[b])) b++;
    sel_anchor = a;
    cursor = b;
    invalidate();
}

void TextArea::scroll(int, int, int delta) {
    if (layout_width_ != bounds.w) layout();
    int lh = line_height(font_size, font);
    int content_h = lines_.back().y + lh + 12;
    scroll_y += delta * lh * 3;
    if (scroll_y > content_h - bounds.h) scroll_y = content_h - bounds.h;
    if (scroll_y < 0) scroll_y = 0;
    invalidate();
}

bool TextArea::key(const KeyEvent& e) {
    if (layout_width_ != bounds.w) layout();
    bool has_sel = sel_anchor >= 0 && sel_anchor != cursor;
    bool changed = false;
    auto move = [&](int to) {
        if (e.shift()) { if (sel_anchor < 0) sel_anchor = cursor; }
        else sel_anchor = -1;
        cursor = kclamp(to, 0, (int)text.size());
    };
    auto line_index = [&](int off) {
        for (size_t li = lines_.size(); li > 0; li--)
            if (off >= lines_[li - 1].start) return (int)li - 1;
        return 0;
    };
    int lh = line_height(font_size, font);
    if (e.cmd() && e.key == Key::Char) {
        uint32_t ch = e.ch | 0x20;
        if (ch == 'a') { sel_anchor = 0; cursor = (int)text.size(); invalidate(); return true; }
        if ((ch == 'c' || ch == 'x') && has_sel) {
            int a = kmin(sel_anchor, cursor), b = kmax(sel_anchor, cursor);
            wm::clipboard_set(text.substr(a, b - a).c_str());
            if (ch == 'c' || readonly) return true;
            delete_selection(text, cursor, sel_anchor);
            changed = true;
        } else if (ch == 'v' && !readonly) {
            delete_selection(text, cursor, sel_anchor);
            const char* clip = wm::clipboard_get();
            text.insert(cursor, clip, strlen(clip));
            cursor += (int)strlen(clip);
            changed = true;
        } else {
            return false;
        }
    } else {
        switch (e.key) {
        case Key::Left: move(has_sel && !e.shift() ? kmin(sel_anchor, cursor) : prev_char(text, cursor)); break;
        case Key::Right: move(has_sel && !e.shift() ? kmax(sel_anchor, cursor) : next_char(text, cursor)); break;
        case Key::Up:
        case Key::Down: {
            int li = line_index(cursor);
            Point p = pos_of(cursor);
            int target = li + (e.key == Key::Up ? -1 : 1);
            if (target < 0) { move(0); break; }
            if (target >= (int)lines_.size()) { move((int)text.size()); break; }
            Line& l = lines_[target];
            String line = text.substr(l.start, l.len);
            move(l.start + text_hit_test(line.c_str(), p.x, font_size, font));
            break;
        }
        case Key::PageUp:
        case Key::PageDown: {
            Point p = pos_of(cursor);
            int dy = (bounds.h - lh) * (e.key == Key::PageUp ? -1 : 1);
            move(offset_at(bounds.x + 8 + gutter_width(this) + p.x, bounds.y + 6 + p.y - scroll_y + dy));
            scroll_y += dy;
            break;
        }
        case Key::Home: { Line& l = lines_[line_index(cursor)]; move(e.cmd() ? 0 : l.start); break; }
        case Key::End: { Line& l = lines_[line_index(cursor)]; move(e.cmd() ? (int)text.size() : l.start + l.len); break; }
        case Key::Backspace:
            if (readonly) return true;
            if (has_sel) delete_selection(text, cursor, sel_anchor);
            else if (cursor > 0) { int p = prev_char(text, cursor); text.erase(p, cursor - p); cursor = p; }
            changed = true;
            break;
        case Key::Delete:
            if (readonly) return true;
            if (has_sel) delete_selection(text, cursor, sel_anchor);
            else if (cursor < (int)text.size()) { int n = next_char(text, cursor); text.erase(cursor, n - cursor); }
            changed = true;
            break;
        case Key::Enter:
        case Key::Tab:
        case Key::Char:
        case Key::Space: {
            if (readonly || !e.ch) return e.key == Key::Tab ? false : true;
            delete_selection(text, cursor, sel_anchor);
            char buf[8];
            int n;
            if (e.key == Key::Tab) { memcpy(buf, "    ", 4); n = 4; }
            else n = utf8_encode(e.ch, buf);
            text.insert(cursor, buf, n);
            cursor += n;
            changed = true;
            break;
        }
        default:
            return false;
        }
    }
    if (changed) layout();
    ensure_cursor_visible();
    invalidate();
    if (changed && on_change) on_change();
    return true;
}

// ---------------------------------------------------------------- Checkbox
void Checkbox::paint(Canvas& c) {
    const Theme& t = theme();
    Rect box(bounds.x, bounds.y + (bounds.h - 18) / 2, 18, 18);
    if (checked) {
        c.fill_round_rect(box, 5, t.accent);
        draw_symbol(c, box.inset(2), Symbol::Check, hex(0xFFFFFF), 2.0f);
    } else {
        c.fill_round_rect(box, 5, t.field_bg);
        c.stroke_round_rect(box, 5, t.control_border, 1.2f);
    }
    if (focused()) focus_ring(c, box, 5);
    c.draw_text_in(Rect(bounds.x + 26, bounds.y, bounds.w - 26, bounds.h), text.c_str(), enabled ? t.text : t.text_tertiary, 14);
}

void Checkbox::mouse_up(int x, int y, int) {
    if (!enabled || !bounds.contains(x, y)) return;
    checked = !checked;
    invalidate();
    if (on_change) on_change(checked);
}

bool Checkbox::key(const KeyEvent& e) {
    if (e.key == Key::Space) { mouse_up(bounds.x, bounds.y, 1); return true; }
    return false;
}

// ---------------------------------------------------------------- Toggle
void Toggle::paint(Canvas& c) {
    const Theme& t = theme();
    Rect r(bounds.x, bounds.y + (bounds.h - 28) / 2, 50, 28);
    c.fill_round_rect(r, 14, on ? t.success : (t.dark ? rgba(255, 255, 255, 40) : rgba(0, 0, 0, 30)));
    int tx = on ? r.right() - 26 : r.x + 2;
    Rect thumb(tx, r.y + 2, 24, 24);
    c.draw_shadow(thumb, 12, 3, rgba(0, 0, 0, 60), 0, 1);
    // liquid-glass thumb: white body, refracted rim, top specular
    c.fill_circle(thumb.x + 12, thumb.y + 12, 12, hex(0xFFFFFF));
    c.stroke_circle(thumb.x + 12, thumb.y + 12, 11, rgba(0, 0, 0, 18), 1.0f);
    c.fill_ellipse(thumb.x + 12, thumb.y + 7, 7, 3.5f, rgba(255, 255, 255, 200));
}

void Toggle::mouse_up(int x, int y, int) {
    if (!enabled || !bounds.contains(x, y)) return;
    on = !on;
    invalidate();
    if (on_change) on_change(on);
}

// ---------------------------------------------------------------- Slider
void Slider::paint(Canvas& c) {
    const Theme& t = theme();
    int cy = bounds.y + bounds.h / 2;
    Rect track(bounds.x + 10, cy - 3, bounds.w - 20, 6);
    c.fill_round_rect(track, 3, t.dark ? rgba(255, 255, 255, 40) : rgba(0, 0, 0, 30));
    int fx = track.x + (int)(track.w * kclamp(value, 0.0f, 1.0f));
    c.fill_round_rect(Rect(track.x, track.y, fx - track.x, track.h), 3, t.accent);
    float r = dragging ? 13 : 11;
    c.draw_shadow(Rect(fx - (int)r, cy - (int)r, (int)(2 * r), (int)(2 * r)), r, 3, rgba(0, 0, 0, 60), 0, 1);
    c.fill_circle(fx, cy, r, hex(0xFFFFFF));
    c.stroke_circle(fx, cy, r - 0.5f, rgba(0, 0, 0, 20), 1);
    c.fill_ellipse(fx, cy - r * 0.45f, r * 0.6f, r * 0.3f, rgba(255, 255, 255, 220));
}

static void slider_set(Slider* s, int x) {
    float v = (float)(x - (s->bounds.x + 10)) / (s->bounds.w - 20);
    v = kclamp(v, 0.0f, 1.0f);
    if (v != s->value) {
        s->value = v;
        s->invalidate();
        if (s->on_change) s->on_change(v);
    }
}

void Slider::mouse_down(int x, int, int) { dragging = true; slider_set(this, x); }
void Slider::mouse_move(int x, int, int buttons) { if (dragging && (buttons & MB_LEFT)) slider_set(this, x); }
void Slider::mouse_up(int, int, int) { dragging = false; invalidate(); }

// ---------------------------------------------------------------- ProgressBar
void ProgressBar::paint(Canvas& c) {
    const Theme& t = theme();
    Rect r(bounds.x, bounds.y + (bounds.h - 6) / 2, bounds.w, 6);
    c.fill_round_rect(r, 3, t.dark ? rgba(255, 255, 255, 30) : rgba(0, 0, 0, 22));
    if (value < 0) {
        int w = r.w / 4;
        int pos = (int)((timer_ms() / 4) % (r.w + w)) - w;
        c.save();
        c.clip_to(r);
        c.fill_round_rect(Rect(r.x + pos, r.y, w, r.h), 3, t.accent);
        c.restore();
        window->set_ticking(true);
    } else {
        int w = (int)(r.w * kclamp(value, 0.0f, 1.0f));
        if (w > 0) c.fill_round_rect(Rect(r.x, r.y, kmax(w, 6), r.h), 3, t.accent);
    }
}

// ---------------------------------------------------------------- SegmentedControl
void SegmentedControl::paint(Canvas& c) {
    const Theme& t = theme();
    Rect r = bounds;
    c.fill_round_rect(r, r.h / 2.0f, t.dark ? rgba(255, 255, 255, 26) : rgba(0, 0, 0, 16));
    int n = (int)segments.size();
    if (!n) return;
    int sw = r.w / n;
    Rect sel(r.x + selected * sw + 2, r.y + 2, sw - 4, r.h - 4);
    c.draw_shadow(sel, sel.h / 2.0f, 3, rgba(0, 0, 0, 40), 0, 1);
    c.fill_round_rect(sel, sel.h / 2.0f, t.dark ? hex(0x636366) : hex(0xFFFFFF));
    for (int i = 0; i < n; i++)
        c.draw_text_in(Rect(r.x + i * sw, r.y, sw, r.h), segments[i].c_str(), t.text, 13,
                       i == selected ? Font::Medium : Font::Regular, Align::Center);
}

void SegmentedControl::mouse_down(int x, int, int) {
    int n = (int)segments.size();
    if (!n) return;
    int i = (x - bounds.x) * n / bounds.w;
    i = kclamp(i, 0, n - 1);
    if (i != selected) {
        selected = i;
        invalidate();
        if (on_change) on_change(i);
    }
}

// ---------------------------------------------------------------- ListView
void ListView::paint(Canvas& c) {
    const Theme& t = theme();
    Rect r = bounds;
    if (!transparent) c.fill_rect(r, t.field_bg);
    c.save();
    c.clip_to(r);
    for (int i = 0; i < (int)items.size(); i++) {
        int y = r.y + i * row_height - scroll_y;
        if (y + row_height < r.y || y > r.bottom()) continue;
        Rect row(r.x + 6, y + 1, r.w - 12, row_height - 2);
        bool sel = i == selected;
        if (sel) c.fill_round_rect(row, 7, focused() || transparent ? t.selection : (t.dark ? rgba(255, 255, 255, 40) : rgba(0, 0, 0, 30)));
        Color fg = sel && (focused() || transparent) ? t.accent_text : t.text;
        int tx = row.x + 10;
        if (items[i].icon) {
            int is = row_height - 10;
            items[i].icon(c, Rect(tx - 2, y + 5, is, is));
            tx += is + 6;
        }
        int dw = items[i].detail.empty() ? 0 : text_width(items[i].detail.c_str(), 13) + 12;
        c.draw_text_in(Rect(tx, y, row.right() - tx - dw - 6, row_height), items[i].text.c_str(), fg, 14);
        if (dw)
            c.draw_text_in(Rect(row.right() - dw, y, dw - 10, row_height), items[i].detail.c_str(),
                           sel ? fg : t.text_secondary, 13, Font::Regular, Align::Right);
    }
    int content = (int)items.size() * row_height;
    if (content > r.h) {
        int bar_h = kmax(30, r.h * r.h / content);
        int bar_y = r.y + (r.h - bar_h) * scroll_y / kmax(1, content - r.h);
        c.fill_round_rect(Rect(r.right() - 7, bar_y + 2, 5, bar_h - 4), 2.5f, t.dark ? rgba(255, 255, 255, 60) : rgba(0, 0, 0, 60));
    }
    c.restore();
}

void ListView::ensure_visible(int i) {
    if (i < 0) return;
    int y = i * row_height;
    if (y < scroll_y) scroll_y = y;
    if (y + row_height > scroll_y + bounds.h) scroll_y = y + row_height - bounds.h;
    invalidate();
}

void ListView::mouse_down(int x, int y, int) {
    int i = (y - bounds.y + scroll_y) / row_height;
    if (i < 0 || i >= (int)items.size()) return;
    if (i != selected) {
        selected = i;
        invalidate();
        if (on_select) on_select(i);
    }
}

void ListView::double_click(int x, int y) {
    int i = (y - bounds.y + scroll_y) / row_height;
    if (i >= 0 && i < (int)items.size() && on_activate) on_activate(i);
}

void ListView::scroll(int, int, int delta) {
    int content = (int)items.size() * row_height;
    scroll_y = kclamp(scroll_y + delta * row_height, 0, kmax(0, content - bounds.h));
    invalidate();
}

bool ListView::key(const KeyEvent& e) {
    int n = (int)items.size();
    if (!n) return false;
    int s = selected;
    if (e.key == Key::Up) s = s <= 0 ? 0 : s - 1;
    else if (e.key == Key::Down) s = s < 0 ? 0 : kmin(n - 1, s + 1);
    else if (e.key == Key::Home) s = 0;
    else if (e.key == Key::End) s = n - 1;
    else if (e.key == Key::Enter) { if (s >= 0 && on_activate) on_activate(s); return true; }
    else return false;
    if (s != selected) {
        selected = s;
        ensure_visible(s);
        if (on_select) on_select(s);
    }
    return true;
}

// ---------------------------------------------------------------- Dropdown
void Dropdown::paint(Canvas& c) {
    const Theme& t = theme();
    Rect r = bounds;
    c.draw_shadow(r, kControlRadius, 2, rgba(0, 0, 0, 25), 0, 1);
    c.fill_round_rect(r, kControlRadius, t.control_bg);
    c.stroke_round_rect(r, kControlRadius, t.control_border, 1);
    const char* label = selected >= 0 && selected < (int)options.size() ? options[selected].c_str() : "";
    c.draw_text_in(Rect(r.x + 10, r.y, r.w - 40, r.h), label, t.text, 14);
    Rect chip(r.right() - 24, r.y + 4, 20, r.h - 8);
    c.fill_round_rect(chip, 5, t.accent);
    draw_symbol(c, Rect(chip.x + 3, chip.y + chip.h / 2 - 7, 14, 7), Symbol::ChevronUp, hex(0xFFFFFF), 1.6f);
    draw_symbol(c, Rect(chip.x + 3, chip.y + chip.h / 2, 14, 7), Symbol::ChevronDown, hex(0xFFFFFF), 1.6f);
    if (focused()) focus_ring(c, r, kControlRadius);
}

void Dropdown::mouse_down(int, int, int) {
    if (!enabled || options.empty()) return;
    Vector<MenuItem> items;
    for (int i = 0; i < (int)options.size(); i++) {
        MenuItem& m = items.emplace();
        m.title = options[i];
        m.checked = i == selected;
        Dropdown* self = this;
        m.action = [self, i] {
            self->selected = i;
            self->invalidate();
            if (self->on_change) self->on_change(i);
        };
    }
    Rect cr = window->content_rect();
    wm::popup_menu(cr.x + bounds.x, cr.y + bounds.bottom() + 4, items);
}
