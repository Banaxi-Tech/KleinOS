// Calculator: standard and scientific modes, frosted glass keys.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "lib/math.h"
#include "lib/kstring.h"

namespace {

constexpr int kStdW = 260, kSciW = 368, kH = 400;
constexpr int kMargin = 14;
constexpr int kKey = 50, kGap = 10, kStep = kKey + kGap;   // standard keys
constexpr int kSciKey = 40, kSciStep = 50;                 // scientific keys
constexpr int kGridTop = 96;
constexpr int kStdGridX = kMargin, kSciGridX = 124;

enum class Tone : uint8_t { Digit, Func, Op };

// ------------------------------------------------------------ number output
double pow10i(int n) {
    double r = 1;
    for (int i = 0; i < n; i++) r *= 10;
    return r;
}

// Up to 10 significant digits, trailing zeros removed. Scientific form for very large/small values.
String format_number(double v) {
    if (v != v || v > 1e300 || v < -1e300) return String("Error");
    if (v == 0) return String("0");
    bool neg = v < 0;
    double a = neg ? -v : v;
    int e = 0;
    double m = a;
    while (m >= 10) { m /= 10; e++; }
    while (m < 1) { m *= 10; e--; }
    String out;
    if (neg) out += '-';
    if (e >= 10 || e <= -7) {
        unsigned long long d = (unsigned long long)(m * 1e9 + 0.5);
        if (d >= 10000000000ULL) { d /= 10; e++; }
        char ds[10];
        for (int i = 9; i >= 0; i--) { ds[i] = (char)('0' + d % 10); d /= 10; }
        int last = 0;
        for (int i = 1; i < 10; i++) if (ds[i] != '0') last = i;
        out += ds[0];
        if (last) { out += '.'; out.append(ds + 1, last); }
        out += e < 0 ? "e-" : "e+";
        out += String::format("%d", e < 0 ? -e : e);
        return out;
    }
    int dec = 9 - e;                                  // 10 significant digits in total
    unsigned long long ps = (unsigned long long)pow10i(dec);
    unsigned long long scaled = (unsigned long long)(a * (double)ps + 0.5);
    unsigned long long ip = scaled / ps, fp = scaled % ps;
    out += String::format("%llu", ip);
    if (fp && dec > 0) {
        char fb[24];
        for (int i = dec - 1; i >= 0; i--) { fb[i] = (char)('0' + fp % 10); fp /= 10; }
        int n = dec;
        while (n > 0 && fb[n - 1] == '0') n--;
        out += '.';
        out.append(fb, n);
    }
    return out;
}

// ------------------------------------------------------------ expression evaluation
// Grammar: expr = term {(+|-) term}; term = unary {(*|/) unary};
// unary = (-|+) unary | power; power = primary [^ unary]; primary = number | (expr) | fn(expr)
struct Parser {
    explicit Parser(const char* text) : s(text) {}
    const char* s;
    size_t p = 0;
    bool ok = true;

    char peek() const { return s[p]; }

    double expr() {
        double v = term();
        for (;;) {
            if (peek() == '+') { p++; v += term(); }
            else if (peek() == '-') { p++; v -= term(); }
            else break;
        }
        return v;
    }
    double term() {
        double v = unary();
        for (;;) {
            if (peek() == '*') { p++; v *= unary(); }
            else if (peek() == '/') {
                p++;
                double d = unary();
                if (d == 0) ok = false;
                else v /= d;
            } else break;
        }
        return v;
    }
    double unary() {
        if (peek() == '-') { p++; return -unary(); }
        if (peek() == '+') { p++; return unary(); }
        return power();
    }
    double power() {
        double b = primary();
        if (peek() == '^') { p++; return pow(b, unary()); }
        return b;
    }
    double number() {
        double v = 0;
        while (isdigit(peek())) { v = v * 10 + (peek() - '0'); p++; }
        if (peek() == '.') {
            p++;
            double f = 0.1;
            while (isdigit(peek())) { v += (peek() - '0') * f; f *= 0.1; p++; }
        }
        if (peek() == 'e' && (isdigit(s[p + 1]) || ((s[p + 1] == '+' || s[p + 1] == '-') && isdigit(s[p + 2])))) {
            p++;
            bool eneg = false;
            if (peek() == '+' || peek() == '-') { eneg = peek() == '-'; p++; }
            int ex = 0;
            while (isdigit(peek())) { ex = ex * 10 + (peek() - '0'); p++; }
            v *= pow(10.0, eneg ? -ex : ex);
        }
        return v;
    }
    double primary() {
        char c = peek();
        if (c == '(') {
            p++;
            double v = expr();
            if (peek() == ')') p++; else ok = false;
            return v;
        }
        if (isdigit(c) || c == '.') return number();
        if (isalpha(c)) {
            char name[8];
            int n = 0;
            while (isalpha(peek()) && n < 7) name[n++] = s[p++];
            name[n] = 0;
            if (peek() != '(') { ok = false; return 0; }
            p++;
            double a = expr();
            if (peek() == ')') p++; else ok = false;
            double r = 0;
            if (!apply_fn(name, a, r)) ok = false;
            return r;
        }
        ok = false;
        return 0;
    }

    // Degrees for the trig functions.
    static bool apply_fn(const char* name, double a, double& out) {
        if (!strcmp(name, "sin")) { out = sin(a * M_PI / 180.0); }
        else if (!strcmp(name, "cos")) { out = cos(a * M_PI / 180.0); }
        else if (!strcmp(name, "tan")) { out = tan(a * M_PI / 180.0); }
        else if (!strcmp(name, "ln")) { if (a <= 0) return false; out = log(a); }
        else if (!strcmp(name, "log")) { if (a <= 0) return false; out = log10(a); }
        else if (!strcmp(name, "sqrt")) { if (a < 0) return false; out = sqrt(a); }
        else return false;
        if (fabs(out) < 1e-12) out = 0;                 // sin(180) etc. are not exactly 0 in floating point
        return true;
    }
};

bool evaluate(const String& text, double& out) {
    Parser ps(text.c_str());
    double v = ps.expr();
    if (ps.peek() != 0) ps.ok = false;
    if (!ps.ok || v != v || v > 1e300 || v < -1e300) return false;
    out = v;
    return true;
}

bool apply_named(const char* name, double a, double& out) { return Parser::apply_fn(name, a, out); }

// Expression text as shown to the user.
String pretty(const String& s) {
    String out;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '*') out += "×";
        else if (c == '/') out += "÷";
        else if (c == '-') out += "−";
        else out += c;
    }
    return out;
}

// Keep the end of a long string visible, with a leading ellipsis.
String tail_fit(const String& s, int size, int maxw) {
    if (text_width(s.c_str(), size) <= maxw) return s;
    size_t n = s.size();
    for (size_t i = 1; i < n; i++) {
        if (((uint8_t)s[i] & 0xC0) == 0x80) continue;   // stay on a UTF-8 boundary
        String t = String("…") + s.substr(i);
        if (text_width(t.c_str(), size) <= maxw) return t;
    }
    return String("…");
}

// ------------------------------------------------------------ keys
struct KeySpec {
    char code;            // what press() receives
    const char* label;
    int col, row, span;
};

// Standard keypad (4 columns, 5 rows).
const KeySpec kStd[] = {
    {'A', "AC", 0, 0, 1}, {'n', "±", 1, 0, 1}, {'%', "%", 2, 0, 1}, {'/', "÷", 3, 0, 1},
    {'7', "7", 0, 1, 1},  {'8', "8", 1, 1, 1}, {'9', "9", 2, 1, 1}, {'*', "×", 3, 1, 1},
    {'4', "4", 0, 2, 1},  {'5', "5", 1, 2, 1}, {'6', "6", 2, 2, 1}, {'-', "−", 3, 2, 1},
    {'1', "1", 0, 3, 1},  {'2', "2", 1, 3, 1}, {'3', "3", 2, 3, 1}, {'+', "+", 3, 3, 1},
    {'0', "0", 0, 4, 2},  {'.', ".", 2, 4, 1}, {'=', "=", 3, 4, 1},
};

// Scientific panel (2 columns, 6 rows) to the left of the keypad.
const KeySpec kSci[] = {
    {'s', "sin", 0, 0, 1}, {'c', "cos", 1, 0, 1},
    {'t', "tan", 0, 1, 1}, {'l', "ln", 1, 1, 1},
    {'g', "log", 0, 2, 1}, {'r', "sqrt", 1, 2, 1},
    {'q', "x²", 0, 3, 1},  {'^', "x^y", 1, 3, 1},
    {'p', "π", 0, 4, 1},   {'e', "e", 1, 4, 1},
    {'(', "(", 0, 5, 1},   {')', ")", 1, 5, 1},
};

Tone tone_of(char c) {
    if ((c >= '0' && c <= '9') || c == '.') return Tone::Digit;
    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '=') return Tone::Op;
    return Tone::Func;
}

// A round key with its own glass look: digits, function keys and orange operators.
class CalcKey : public Widget {
public:
    CalcKey(const KeySpec& s, bool sci_panel)
        : Widget(Rect()), code(s.code), label(s.label), tone(tone_of(s.code)), col(s.col), row(s.row),
          span(s.span), sci(sci_panel) {
        font_size = sci ? 13 : (tone == Tone::Func ? 18 : 22);
    }

    void paint(Canvas& c) override {
        Rect r = bounds;
        if (r.empty()) return;
        const Theme& t = theme();
        float rad = r.h / 2.0f;
        Color fg = t.text;
        if (tone == Tone::Op) {
            Color top = pressed ? hex(0xFF9A2E) : hex(0xFFAE45);
            Color bot = pressed ? hex(0xE8720A) : hex(0xFF8A1F);
            c.draw_shadow(r, rad, 6, rgba(0, 0, 0, t.dark ? 110 : 60), 0, 3);
            c.fill_gradient(r, top, bot, rad);
            fg = hex(0xFFFFFF);
        } else if (tone == Tone::Func) {
            c.fill_round_rect(r, rad, t.dark ? rgba(255, 255, 255, pressed ? 110 : 62) : rgba(120, 120, 130, pressed ? 110 : 64));
        } else {
            c.fill_round_rect(r, rad, t.dark ? rgba(255, 255, 255, pressed ? 84 : 40) : rgba(255, 255, 255, pressed ? 230 : 170));
        }
        // glossy top half and rim
        c.fill_gradient(Rect(r.x + 2, r.y + 1, r.w - 4, r.h / 2), rgba(255, 255, 255, t.dark ? 46 : 120),
                        rgba(255, 255, 255, 0), rad - 1);
        c.stroke_round_rect(r, rad, t.dark ? rgba(255, 255, 255, 70) : rgba(0, 0, 0, 24), 1);

        if (span > 1) {
            c.draw_text_in(Rect(r.x + 22, r.y, r.w - 22, r.h), label.c_str(), fg, font_size, Font::Medium, Align::Left);
        } else {
            c.draw_text_in(r, label.c_str(), fg, font_size, Font::Medium, Align::Center);
        }
    }

    void mouse_down(int, int, int) override { pressed = true; invalidate(); }
    void mouse_up(int x, int y, int) override {
        bool was = pressed;
        pressed = false;
        invalidate();
        if (was && bounds.contains(x, y) && on_press) on_press(code);
    }
    void hover_changed(bool) override { invalidate(); }

    char code;
    String label;
    Tone tone;
    int col, row, span;
    bool sci;
    int font_size;
    bool pressed = false;
    Func<void(char)> on_press;
};

// ------------------------------------------------------------ window
class CalcWindow : public Window {
public:
    CalcWindow() : Window("Calculator", kStdW, kH, WF_GLASS | WF_NO_ZOOM) {
        toggle_ = (Button*)add(new Button(Rect(kMargin, 12, 104, 24), "Scientific", Button::Glass));
        toggle_->font_size = 12;
        toggle_->on_click = [this] { set_scientific(!sci_); };

        for (const KeySpec& s : kStd) make_key(s, false);
        for (const KeySpec& s : kSci) make_key(s, true);
        layout();
    }

    void on_paint(Canvas& c) override {
        const Theme& t = theme();
        c.clear(rgba(0, 0, 0, 0));
        c.fill_rect(Rect(0, 0, width(), height()), t.dark ? rgba(18, 18, 22, 110) : rgba(255, 255, 255, 96));

        // display: expression line on top, result below
        String top = expr_.empty() ? history_ : expr_;
        int topw = width() - 124 - kMargin;
        if (!top.empty()) {
            String shown = tail_fit(pretty(top), 13, topw);
            c.draw_text_in(Rect(124, 14, topw, 20), shown.c_str(), t.text_secondary, 13, Font::Regular, Align::Right);
        }
        String big = entry_.empty() ? String("0") : pretty(entry_);
        int avail = width() - 2 * kMargin;
        int fit = 56;
        while (fit > 22 && text_width(big.c_str(), fit, Font::Regular) > avail) fit -= 2;
        c.draw_text_in(Rect(kMargin, 40, avail, 56), big.c_str(), error_ ? t.destructive : t.text, fit,
                       Font::Regular, Align::Right);

        for (Widget* w : widgets())
            if (w->visible) {
                c.save();
                w->paint(c);
                c.restore();
            }
    }

    void on_key(const KeyEvent& e) override {
        if (e.key == Key::Enter) { press('='); }
        else if (e.key == Key::Backspace) { press('B'); }
        else if (e.key == Key::Escape) { press('A'); }
        else if (e.key == Key::Char) {
            uint32_t ch = e.ch;
            if (e.cmd() && (ch == 't' || ch == 'T')) { set_scientific(!sci_); }
            else if (ch >= '0' && ch <= '9') press((char)ch);
            else if (ch == '.' || ch == ',') press('.');
            else if (ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '^' || ch == '(' || ch == ')' ||
                     ch == '%' || ch == '=') press((char)ch);
            else { Window::on_key(e); return; }
        } else {
            Window::on_key(e);
            return;
        }
        invalidate();
    }

    void build_menus(Vector<Menu>& menus) override {
        Menu& m = menus.emplace();
        m.title = "View";
        m.add("Scientific", [this] { set_scientific(!sci_); }, "⌘T");
        m.items[m.items.size() - 1].checked = sci_;
    }

private:
    void make_key(const KeySpec& s, bool sci_panel) {
        CalcKey* k = new CalcKey(s, sci_panel);
        k->on_press = [this](char c) { press(c); };
        add(k);
        keys_.push(k);
    }

    void set_scientific(bool on) {
        sci_ = on;
        toggle_->text = on ? "Standard" : "Scientific";
        resize(on ? kSciW : kStdW, kH);
        layout();
        invalidate();
    }

    void layout() {
        int gx = sci_ ? kSciGridX : kStdGridX;
        for (CalcKey* k : keys_) {
            bool shown = !k->sci || sci_;
            k->visible = shown;
            if (!shown) { k->bounds = Rect(); continue; }
            if (k->sci) {
                k->bounds = Rect(kMargin + k->col * kSciStep, kGridTop + k->row * kSciStep, kSciKey, kSciKey);
            } else {
                int w = k->span * kKey + (k->span - 1) * kGap;
                k->bounds = Rect(gx + k->col * kStep, kGridTop + k->row * kStep, w, kKey);
            }
        }
    }

    // ---- editing model
    // expr_: committed tokens (ASCII operators: + - * / ^ and fn(...)), entry_: number being typed.
    // After '=', entry_ holds the result and fresh_ is set: the next digit starts over, an operator continues.
    static bool is_op(char c) { return c == '+' || c == '-' || c == '*' || c == '/' || c == '^'; }
    static bool is_value_end(char c) { return isdigit(c) || c == ')'; }

    int count(char ch) const {
        int n = 0;
        for (size_t i = 0; i < expr_.size(); i++) if (expr_[i] == ch) n++;
        return n;
    }

    void reset_for_new_input() {
        if (!fresh_) return;
        expr_.clear();
        entry_.clear();
        history_.clear();
        fresh_ = false;
        error_ = false;
    }

    void clear_all() {
        expr_.clear();
        entry_.clear();
        history_.clear();
        fresh_ = false;
        error_ = false;
    }

    // Adds a number, '(' or function name, with an implicit '*' after a value.
    void append_token(const char* tok) {
        if (!expr_.empty() && is_value_end(expr_[expr_.size() - 1]) &&
            (isalpha(tok[0]) || tok[0] == '(' || isdigit(tok[0]) || tok[0] == '.'))
            expr_ += '*';
        expr_ += tok;
    }

    void flush_entry() {
        if (entry_.empty()) return;
        append_token(entry_.c_str());
        entry_.clear();
    }

    void digit(char d) {
        reset_for_new_input();
        if (error_) clear_all();
        if (entry_ == "0") entry_.clear();
        else if (entry_ == "-0") entry_ = "-";
        if (entry_.size() >= 14) return;
        entry_ += d;
    }

    void dot() {
        reset_for_new_input();
        if (error_) clear_all();
        if (entry_.find('.') >= 0) return;
        if (entry_.empty()) entry_ = "0.";
        else entry_ += '.';
    }

    void op(char c) {
        if (error_ && fresh_) { clear_all(); return; }
        if (fresh_) {                      // continue from the result
            expr_.clear();
            history_.clear();
            fresh_ = false;
        }
        if (!entry_.empty()) { flush_entry(); expr_ += c; return; }
        if (expr_.empty()) { if (c == '-') expr_ += c; return; }
        char last = expr_[expr_.size() - 1];
        if (last == '(') { if (c == '-') expr_ += c; return; }
        if (is_op(last)) expr_.erase(expr_.size() - 1, 1);
        expr_ += c;
    }

    void open_paren() {
        reset_for_new_input();
        if (error_) clear_all();
        flush_entry();
        append_token("(");
    }

    void close_paren() {
        reset_for_new_input();
        if (error_) return;
        flush_entry();
        if (expr_.empty()) return;
        char last = expr_[expr_.size() - 1];
        if (is_op(last) || last == '(') return;
        if (count('(') > count(')')) expr_ += ')';
    }

    void equals() {
        if (fresh_ || error_) return;
        flush_entry();
        while (!expr_.empty()) {
            char last = expr_[expr_.size() - 1];
            if (!is_op(last) && last != '(') break;
            expr_.erase(expr_.size() - 1, 1);
        }
        if (expr_.empty()) return;
        while (count('(') > count(')')) expr_ += ')';
        double v = 0;
        history_ = expr_;
        if (evaluate(expr_, v)) {
            entry_ = format_number(v);
            error_ = false;
        } else {
            entry_ = "Error";
            error_ = true;
        }
        expr_.clear();
        fresh_ = true;
    }

    void negate() {
        if (error_ || entry_.empty()) return;
        if (entry_[0] == '-') entry_.erase(0, 1);
        else entry_.insert(0, "-", 1);
    }

    void percent() {
        if (error_ || entry_.empty()) return;
        double v;
        if (!evaluate(entry_, v)) return;
        entry_ = format_number(v / 100);
        fresh_ = false;
    }

    // Square, square root, trig, logs. Applies to the number on screen, or opens the function.
    void function(char code) {
        const char* name = nullptr;
        switch (code) {
        case 's': name = "sin"; break;
        case 'c': name = "cos"; break;
        case 't': name = "tan"; break;
        case 'l': name = "ln"; break;
        case 'g': name = "log"; break;
        case 'r': name = "sqrt"; break;
        default: return;
        }
        if (error_) clear_all();
        if (entry_.empty()) {
            char tok[16];
            snprintf(tok, sizeof(tok), "%s(", name);
            reset_for_new_input();
            append_token(tok);
            return;
        }
        double v, r = 0;
        if (!evaluate(entry_, v) || !apply_named(name, v, r)) { show_error(); return; }
        apply_direct(r);
    }

    void square() {
        if (error_ || entry_.empty()) return;
        double v;
        if (!evaluate(entry_, v)) return;
        apply_direct(v * v);
    }

    // Result of a direct function on the entry: it becomes the new entry.
    void apply_direct(double r) {
        if (r != r || r > 1e300 || r < -1e300) { show_error(); return; }
        expr_.clear();
        history_.clear();
        fresh_ = false;
        entry_ = format_number(r);
    }

    void show_error() {
        expr_.clear();
        history_.clear();
        entry_ = "Error";
        error_ = true;
        fresh_ = true;
    }

    void constant(const char* value) {
        reset_for_new_input();
        if (error_) clear_all();
        if (!entry_.empty()) return;
        entry_ = value;
        fresh_ = false;
    }

    // Removes the number at the end of expr_ and returns it.
    String take_trailing_number() {
        size_t n = expr_.size(), s = n;
        while (s > 0 && (isdigit(expr_[s - 1]) || expr_[s - 1] == '.')) s--;
        String num = expr_.substr(s, n - s);
        expr_.erase(s, n - s);
        return num;
    }

    void backspace() {
        if (fresh_ || error_) {
            if (error_) { clear_all(); return; }
            fresh_ = false;
            expr_.clear();
            history_.clear();
        }
        if (!entry_.empty()) {
            entry_.erase(entry_.size() - 1, 1);
            if (entry_ == "-") entry_.clear();
            return;
        }
        if (expr_.empty()) return;
        size_t n = expr_.size();
        char last = expr_[n - 1];
        if (isdigit(last) || last == '.') {
            // the number's last digit goes; the rest stays editable in the entry
            String num = take_trailing_number();
            num.erase(num.size() - 1, 1);
            entry_ = num;
        } else if (last == '(') {
            expr_.erase(n - 1, 1);
            while (!expr_.empty() && isalpha(expr_[expr_.size() - 1])) expr_.erase(expr_.size() - 1, 1);
        } else {
            expr_.erase(n - 1, 1);
            if (!expr_.empty() && (isdigit(expr_[expr_.size() - 1]) || expr_[expr_.size() - 1] == '.'))
                entry_ = take_trailing_number();
        }
    }

    void press(char k) {
        if (k >= '0' && k <= '9') { digit(k); return; }
        switch (k) {
        case '.': dot(); break;
        case '+': case '-': case '*': case '/': case '^': op(k); break;
        case '(': open_paren(); break;
        case ')': close_paren(); break;
        case '=': equals(); break;
        case 'A': clear_all(); break;
        case 'n': negate(); break;
        case '%': percent(); break;
        case 'B': backspace(); break;
        case 'q': square(); break;
        case 'p': constant("3.141592654"); break;
        case 'e': constant("2.718281828"); break;
        default: function(k); break;
        }
        invalidate();
    }

    Vector<CalcKey*> keys_;
    Button* toggle_ = nullptr;
    bool sci_ = false;
    String expr_;
    String entry_;
    String history_;
    bool fresh_ = false;
    bool error_ = false;
};

// ------------------------------------------------------------ icon
void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x5B6170), hex(0x23262E));
    float u = r.w / 64.0f;
    auto X = [&](float v) { return r.x + v * u; };
    auto Y = [&](float v) { return r.y + v * u; };
    // display
    c.fill_round_rect(Rect((int)X(11), (int)Y(10), (int)(42 * u), (int)(14 * u)), 4 * u, hex(0xBFF5D9));
    c.fill_round_rect(Rect((int)X(11), (int)Y(11), (int)(42 * u), (int)(5 * u)), 2 * u, rgba(255, 255, 255, 110));
    // keys: 3x3, operators in orange down the right column
    const float xs[3] = {19, 32, 45}, ys[3] = {36, 46, 56};
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++) {
            Color k = col == 2 ? hex(0xFF9A2E) : rgba(255, 255, 255, 220);
            c.fill_circle(X(xs[col]), Y(ys[row]), 4.2f * u, k);
        }
}

Window* create() { return new CalcWindow(); }

AppRegistrar reg({"calculator", "Calculator", icon, create, "Utilities", true, true, 40, "Basic and scientific calculator"});

}  // namespace
