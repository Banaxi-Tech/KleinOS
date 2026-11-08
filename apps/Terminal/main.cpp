// Terminal — KleinOS shell window: glass background, scrollback, line editing,
// history, tab completion and the built-in command set.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "fs/vfs.h"
#include "sys/dl.h"
#include "sys/session.h"
#include "sys/system.h"
#include "drivers/power.h"
#include "drivers/rtc.h"
#include "drivers/ata.h"
#include "drivers/pci.h"
#include "net/net.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "boot/multiboot2.h"
#include "lib/heap.h"
#include "lib/math.h"
#include "lib/string.h"
#include "lib/kstring.h"
#include "lib/vector.h"

namespace {

#define ESC "\x1b"   // markup escape: ESC + one letter (never write "\x1bd": 'd' is a hex digit)

constexpr int kTermSize = 14;
constexpr int kSidePad = 14;
constexpr int kTopPad = 10;
constexpr int kBottomPad = 10;
constexpr int kMaxRows = 2000;
constexpr int kMaxHistory = 500;
constexpr int kPingCount = 4;

// Colour indices used by per-character style bytes.
enum : uint8_t { S_DEF, S_GREEN, S_BLUE, S_RED, S_YELLOW, S_CYAN, S_MAGENTA, S_GRAY, S_WHITE, S_ORANGE, S_PINK };

Color palette(uint8_t s) {
    switch (s) {
    case S_GREEN: return hex(0x6BE675);
    case S_BLUE: return hex(0x6AA8FF);
    case S_RED: return hex(0xFF6B6B);
    case S_YELLOW: return hex(0xFFD86B);
    case S_CYAN: return hex(0x6BE6F0);
    case S_MAGENTA: return hex(0xE58CFF);
    case S_GRAY: return hex(0x8A8F98);
    case S_WHITE: return hex(0xFFFFFF);
    case S_ORANGE: return hex(0xFFA65C);
    case S_PINK: return hex(0xFF8FC7);
    default: return hex(0xE6E8EB);
    }
}

uint8_t style_for_code(char c) {
    switch (c) {
    case 'g': return S_GREEN;
    case 'b': return S_BLUE;
    case 'r': return S_RED;
    case 'y': return S_YELLOW;
    case 'c': return S_CYAN;
    case 'm': return S_MAGENTA;
    case 'k': return S_GRAY;
    case 'w': return S_WHITE;
    case 'o': return S_ORANGE;
    case 'p': return S_PINK;
    default: return S_DEF;
    }
}

struct Row {
    String text;
    Vector<uint8_t> st;   // one style byte per character
};

struct HelpEntry {
    const char* name;
    const char* desc;
};

constexpr HelpEntry kCommands[] = {
    {"help", "list the built-in commands"},
    {"ls", "list directory contents  [-l] [-a]"},
    {"cd", "change the working directory"},
    {"pwd", "print the working directory"},
    {"cat", "print file contents"},
    {"echo", "print text  (> and >> redirect to a file)"},
    {"mkdir", "make directories"},
    {"rm", "remove files  [-r] [-f]"},
    {"touch", "create empty files"},
    {"mv", "move or rename"},
    {"cp", "copy a file"},
    {"clear", "clear the screen"},
    {"date", "show the date and time"},
    {"uptime", "time since boot"},
    {"uname", "system name  [-a]"},
    {"whoami", "current user"},
    {"hostname", "machine name"},
    {"free", "memory usage"},
    {"df", "storage devices"},
    {"lspci", "PCI devices"},
    {"lsdl", "loaded .dl modules"},
    {"apps", "installed applications"},
    {"open", "open an app id or a path"},
    {"ps", "open windows"},
    {"kill", "close window number N from ps"},
    {"history", "command history"},
    {"neofetch", "system summary"},
    {"ping", "ICMP echo to a host or IP"},
    {"nslookup", "resolve a host name"},
    {"calc", "evaluate an expression"},
    {"klein", "klein say <text>  -- a little mascot"},
    {"fortune", "a random thought"},
    {"beep", "speaker beep"},
    {"reboot", "restart the machine"},
    {"shutdown", "power off"},
    {"exit", "close this window"},
};
constexpr int kCommandCount = sizeof(kCommands) / sizeof(kCommands[0]);

const char* const kFortunes[] = {
    "The best way to predict the future is to kernel it.",
    "A bug is just an undocumented feature waiting for a changelog.",
    "Every page fault is a small lesson in humility.",
    "Ship it, then wish you had shipped it sooner.",
    "The cursor blinks because it is waiting for you.",
    "Your Klein bottle has no inside. Neither does this fortune.",
    "Measure twice, then profile once more.",
    "Simple code is the hardest code to write and the easiest to keep.",
};

// ---------------------------------------------------------------- helpers
String normalize(const char* path) {
    Vector<String> parts;
    const char* p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* s = p;
        while (*p && *p != '/') p++;
        String seg(s, (size_t)(p - s));
        if (seg == ".") continue;
        if (seg == "..") { if (!parts.empty()) parts.pop(); continue; }
        parts.push(seg);
    }
    if (parts.empty()) return String("/");
    String out;
    for (size_t i = 0; i < parts.size(); i++) { out += "/"; out += parts[i]; }
    return out;
}

String clean(const char* s) {
    String out;
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n' || c == '\t') out += (char)c;
        else if (c >= 32 && c < 127) out += (char)c;
        else out += '?';
    }
    return out;
}

String abbrev(const char* s) { return String(s, 3); }

void fmt_size(char* buf, size_t n, uint64_t bytes) {
    if (bytes < 1024) snprintf(buf, n, "%llu B", (unsigned long long)bytes);
    else if (bytes < 1024ull * 1024) snprintf(buf, n, "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024) snprintf(buf, n, "%.1f MB", bytes / 1048576.0);
    else snprintf(buf, n, "%.2f GB", bytes / 1073741824.0);
}

bool parse_ip(const char* s, uint32_t& ip) {
    uint32_t v = 0;
    int parts = 0;
    const char* p = s;
    while (parts < 4) {
        if (!isdigit((unsigned char)*p)) return false;
        int n = 0;
        while (isdigit((unsigned char)*p)) { n = n * 10 + (*p - '0'); p++; if (n > 255) return false; }
        v = (v << 8) | (uint32_t)n;
        parts++;
        if (parts < 4) { if (*p != '.') return false; p++; }
    }
    if (*p) return false;
    ip = v;
    return true;
}

// Tiny recursive-descent calculator: + - * / % ^, parentheses, pi, e, sqrt(), abs(), floor().
struct Calc {
    const char* p = "";
    bool bad = false;
    void ws() { while (*p == ' ') p++; }
    double expr() {
        double v = term();
        for (;;) {
            ws();
            if (*p == '+') { p++; v += term(); }
            else if (*p == '-') { p++; v -= term(); }
            else break;
        }
        return v;
    }
    double term() {
        double v = unary();
        for (;;) {
            ws();
            if (*p == '*') { p++; v *= unary(); }
            else if (*p == '/') { p++; double d = unary(); if (d == 0) { bad = true; return 0; } v /= d; }
            else if (*p == '%') { p++; double d = unary(); if (d == 0) { bad = true; return 0; } v = fmod(v, d); }
            else break;
        }
        return v;
    }
    double unary() {
        ws();
        if (*p == '-') { p++; return -unary(); }
        if (*p == '+') { p++; return unary(); }
        return power();
    }
    double power() {
        double b = factor();
        ws();
        if (*p == '^') { p++; return pow(b, unary()); }
        return b;
    }
    double factor() {
        ws();
        if (*p == '(') {
            p++;
            double v = expr();
            ws();
            if (*p == ')') p++; else bad = true;
            return v;
        }
        if (isdigit((unsigned char)*p) || *p == '.') {
            char* end = nullptr;
            double v = strtod(p, &end);
            if (!end || end == p) { bad = true; return 0; }
            p = end;
            return v;
        }
        if (isalpha((unsigned char)*p)) {
            char name[16];
            int n = 0;
            while (isalpha((unsigned char)*p) && n < 15) name[n++] = *p++;
            name[n] = 0;
            if (!strcmp(name, "pi")) return 3.14159265358979323846;
            if (!strcmp(name, "e")) return 2.71828182845904523536;
            ws();
            if (*p != '(') { bad = true; return 0; }
            p++;
            double v = expr();
            ws();
            if (*p == ')') p++; else bad = true;
            if (!strcmp(name, "sqrt")) { if (v < 0) bad = true; return sqrt(v < 0 ? 0 : v); }
            if (!strcmp(name, "abs")) return fabs(v);
            if (!strcmp(name, "floor")) return floor(v);
            bad = true;
            return 0;
        }
        bad = true;
        return 0;
    }
};

// ---------------------------------------------------------------- Terminal
class TerminalWindow : public Window {
public:
    TerminalWindow() : Window("Terminal", 720, 460, WF_GLASS | WF_RESIZABLE) {
        home_ = String("/Users/") + session_user();
        if (!vfs::is_dir(home_.c_str())) home_ = "/";
        cwd_ = home_;
        set_title_for_cwd();
        set_ticking(true);
        line(String::format("KleinOS %s.%d %s", KLEIN_VERSION, system_build_number(), KLEIN_CODENAME).c_str(), S_GRAY);
        line("Type 'help' for a list of commands. Tab completes, Up/Down recalls history.", S_GRAY);
        line("", S_DEF);
    }

    ~TerminalWindow() override {}

    void on_paint(Canvas& c) override {
        c.clear(rgba(0, 0, 0, 0));
        int w = width(), h = height();
        c.fill_rect(Rect(0, 0, w, h), rgba(20, 20, 24, 225));
        int cw = cell_w(), rh = row_h(), cols = cols_for(w), vis = visible_rows(h);

        Row line = input_row();
        int total_in = (int)line.text.size();
        int in_rows = total_in / cols + 1;
        int total = (int)rows_.size() + in_rows;
        int max_off = total > vis ? total - vis : 0;
        if (view_offset_ > max_off) view_offset_ = max_off;
        if (view_offset_ < 0) view_offset_ = 0;
        int start = total > vis ? total - vis - view_offset_ : 0;

        for (int i = start; i < total && i < start + vis; i++) {
            int y = kTopPad + (i - start) * rh;
            if (i < (int)rows_.size()) {
                draw_run_row(c, rows_[i], 0, (int)rows_[i].text.size(), kSidePad, y, cw);
            } else {
                int from = (i - (int)rows_.size()) * cols;
                int to = from + cols;
                if (to > total_in) to = total_in;
                draw_run_row(c, line, from, to, kSidePad, y, cw);
            }
        }

        // blinking block cursor on the input line (only when showing the bottom)
        if (view_offset_ == 0) {
            int idx = (int)prompt_len_ + cursor_;
            int vrow = (int)rows_.size() + idx / cols;
            if (vrow >= start && vrow < start + vis) {
                int x = kSidePad + (idx % cols) * cw;
                int y = kTopPad + (vrow - start) * rh;
                if (cursor_on_) {
                    c.fill_round_rect(Rect(x, y + 1, cw, rh - 3), 2, rgba(230, 232, 236, 205));
                    char ch = idx < total_in ? line.text[idx] : ' ';
                    char buf[2] = {ch, 0};
                    c.draw_text(x, y, buf, rgba(20, 20, 24, 255), kTermSize, Font::Mono);
                } else {
                    c.stroke_rect(Rect(x, y + 1, cw, rh - 3), rgba(230, 232, 236, 120), 1);
                }
            }
        }
    }

    void on_tick(uint64_t now) override {
        bool on = (now / 530) % 2 == 0;
        if (on != cursor_on_) { cursor_on_ = on; invalidate(); }
        tick_job(now);
    }

    void on_scroll(int, int, int delta) override {
        view_offset_ -= delta * 3;
        if (view_offset_ < 0) view_offset_ = 0;
        invalidate();
    }

    void on_resize() override { invalidate(); }

    void on_key(const KeyEvent& e) override {
        if (!e.pressed) return;
        if (e.key == Key::Char) {
            uint32_t ch = e.ch;
            if (e.ctrl() || (ch >= 1 && ch <= 26)) {
                uint32_t k = (ch >= 1 && ch <= 26) ? ch + 96 : (ch | 0x20);
                ctrl_key(k);
                return;
            }
            if (e.super() || ch < 32 || ch > 126) return;
            insert_char((char)ch);
            return;
        }
        view_offset_ = 0;
        switch (e.key) {
        case Key::Space: insert_char(' '); break;
        case Key::Enter: submit(); break;
        case Key::Backspace:
            if (cursor_ > 0) { input_.erase(cursor_ - 1, 1); cursor_--; }
            break;
        case Key::Delete:
            if (cursor_ < (int)input_.size()) input_.erase(cursor_, 1);
            break;
        case Key::Left: if (cursor_ > 0) cursor_--; break;
        case Key::Right: if (cursor_ < (int)input_.size()) cursor_++; break;
        case Key::Home: cursor_ = 0; break;
        case Key::End: cursor_ = (int)input_.size(); break;
        case Key::Up: history_prev(); break;
        case Key::Down: history_next(); break;
        case Key::Tab: complete(); break;
        case Key::PageUp: view_offset_ += visible_rows(height()) - 1; break;
        case Key::PageDown: view_offset_ -= visible_rows(height()) - 1; if (view_offset_ < 0) view_offset_ = 0; break;
        default: break;
        }
        cursor_on_ = true;
        invalidate();
    }

    void build_menus(Vector<Menu>& menus) override {
        Menu& m = menus.emplace();
        m.title = "Shell";
        m.add("Clear Screen", [this] { clear_screen(); }, "⌘K");
        m.separator();
        m.add("Close Window", [this] { close(); }, "⌘W");
    }

private:
    // ---- geometry
    static int cell_w() { return (text_width("MMMMMMMMMM", kTermSize, Font::Mono) + 5) / 10; }
    static int row_h() { return line_height(kTermSize, Font::Mono) + 2; }
    static int cols_for(int w) { int c = (w - 2 * kSidePad) / cell_w(); return c < 10 ? 10 : c; }
    static int visible_rows(int h) { int v = (h - kTopPad - kBottomPad) / row_h(); return v < 1 ? 1 : v; }

    // ---- scrollback
    void push_row() {
        rows_.push(Row());
        if ((int)rows_.size() > kMaxRows + 200) {
            for (int i = 0; i < 200; i++) rows_.remove(0);
        }
    }

    static void markup_to_row(const char* s, Row& r) {
        uint8_t style = S_DEF;
        for (const char* p = s; *p; p++) {
            if (*p == 0x1b) {
                if (p[1]) { p++; style = style_for_code(*p); }
                continue;
            }
            r.text += *p;
            r.st.push(style);
        }
    }

    // Writes markup text to the screen buffer (or the redirection capture), wrapping at the column count.
    void put(const char* s) {
        invalidate();
        if (capture_) {
            for (const char* p = s; *p; p++) {
                if (*p == 0x1b) { if (p[1]) p++; continue; }
                *capture_ += *p;
            }
            return;
        }
        int cols = cols_for(width());
        uint8_t style = S_DEF;
        for (const char* p = s; *p; p++) {
            unsigned char ch = (unsigned char)*p;
            if (ch == 0x1b) {
                if (p[1]) { p++; style = style_for_code(*p); }
                continue;
            }
            if (ch == '\n') {
                if (!tail_open_) push_row();
                tail_open_ = false;
                continue;
            }
            if (ch == '\t') { put_cell(' ', style, cols); put_cell(' ', style, cols); continue; }
            if (ch < 32 || ch > 126) ch = '?';
            put_cell((char)ch, style, cols);
        }
    }

    void put_cell(char ch, uint8_t style, int cols) {
        if (rows_.empty() || !tail_open_ || (int)rows_.back().text.size() >= cols) {
            push_row();
            tail_open_ = true;
        }
        rows_.back().text += ch;
        rows_.back().st.push(style);
    }

    void line(const char* text, uint8_t style) {
        String s = ESC;
        s += (char)(style == S_GREEN ? 'g' : style == S_BLUE ? 'b' : style == S_RED ? 'r' : style == S_YELLOW ? 'y' :
                    style == S_CYAN ? 'c' : style == S_MAGENTA ? 'm' : style == S_GRAY ? 'k' : style == S_WHITE ? 'w' :
                    style == S_ORANGE ? 'o' : style == S_PINK ? 'p' : 'd');
        s += text;
        s += ESC "d\n";
        put(s.c_str());
    }
    void say(const String& s) { put(s.c_str()); put("\n"); }
    void err(const char* msg) { String s = ESC "r"; s += msg; s += ESC "d"; say(s); }

    void clear_screen() {
        rows_.clear();
        tail_open_ = false;
        view_offset_ = 0;
        invalidate();
    }

    // ---- prompt and input line
    String shown_cwd() const {
        if (cwd_ == home_) return String("~");
        size_t hl = home_.size();
        if (hl > 1 && cwd_.size() > hl && strncmp(cwd_.c_str(), home_.c_str(), hl) == 0 && cwd_[hl] == '/')
            return String("~") + cwd_.substr(hl);
        return cwd_;
    }
    String prompt_markup() const {
        String p = ESC "g";
        p += session_user();
        p += ESC "d@klein " ESC "b";
        p += shown_cwd();
        p += ESC "d % ";
        return p;
    }
    Row input_row() {
        Row r;
        markup_to_row(prompt_markup().c_str(), r);
        prompt_len_ = r.text.size();
        for (size_t i = 0; i < input_.size(); i++) { r.text += input_[i]; r.st.push(S_DEF); }
        return r;
    }

    void draw_run_row(Canvas& c, const Row& r, int from, int to, int x0, int y, int cw) {
        int n_text = (int)r.text.size();
        if (to > n_text) to = n_text;
        int i = from;
        char buf[256];
        while (i < to) {
            uint8_t s = r.st[i];
            int j = i, n = 0;
            while (j < to && r.st[j] == s && n < 200) { buf[n++] = r.text[j]; j++; }
            buf[n] = 0;
            c.draw_text(x0 + (i - from) * cw, y, buf, palette(s), kTermSize, Font::Mono);
            i = j;
        }
    }

    void set_title_for_cwd() {
        String t = session_user();
        t += " \xe2\x80\x94 ";
        t += shown_cwd();
        set_title(t.c_str());
    }

    // ---- editing
    void insert_char(char ch) {
        input_.insert(cursor_, &ch, 1);
        cursor_++;
        view_offset_ = 0;
        invalidate();
    }

    void ctrl_key(uint32_t k) {
        switch (k) {
        case 'c': {
            if (job_ != Job::None) { cancel_job(); }
            String s = prompt_markup();
            s += input_;
            s += "^C";
            say(s);
            input_.clear();
            cursor_ = 0;
            hist_pos_ = -1;
            break;
        }
        case 'l': clear_screen(); break;
        case 'u': input_.erase(0, cursor_); cursor_ = 0; break;
        case 'k': input_.erase(cursor_, input_.size() - cursor_); break;
        case 'a': cursor_ = 0; break;
        case 'e': cursor_ = (int)input_.size(); break;
        default: break;
        }
        cursor_on_ = true;
        invalidate();
    }

    void history_prev() {
        if (history_.empty()) return;
        if (hist_pos_ < 0) { saved_ = input_; hist_pos_ = (int)history_.size() - 1; }
        else if (hist_pos_ > 0) hist_pos_--;
        input_ = history_[hist_pos_];
        cursor_ = (int)input_.size();
    }
    void history_next() {
        if (hist_pos_ < 0) return;
        if (hist_pos_ < (int)history_.size() - 1) {
            hist_pos_++;
            input_ = history_[hist_pos_];
        } else {
            hist_pos_ = -1;
            input_ = saved_;
        }
        cursor_ = (int)input_.size();
    }

    // Candidates are full words that start with the typed prefix (the text
    // from the last space up to the cursor).
    void complete() {
        int start = cursor_;
        while (start > 0 && input_[start - 1] != ' ') start--;
        String prefix = input_.substr(start, cursor_ - start);
        bool first = input_.substr(0, start).trim().empty();
        bool is_cmd = first && prefix.find('/') < 0;

        Vector<String> cands;
        if (is_cmd) {
            for (int i = 0; i < kCommandCount; i++)
                if (strncmp(kCommands[i].name, prefix.c_str(), prefix.size()) == 0) cands.push(String(kCommands[i].name));
        } else {
            long slash = prefix.rfind('/');
            String dir_part = slash >= 0 ? prefix.substr(0, slash + 1) : String();
            String base = slash >= 0 ? prefix.substr(slash + 1) : prefix;
            String dir_abs = slash >= 0 ? resolve(dir_part.c_str()) : cwd_;
            vfs::Node* d = vfs::lookup(dir_abs.c_str());
            if (d && d->is_dir) {
                for (size_t i = 0; i < d->children.size(); i++) {
                    vfs::Node* ch = d->children[i];
                    if (ch->name.empty()) continue;
                    if (ch->name[0] == '.' && (base.empty() || base[0] != '.')) continue;
                    if (strncmp(ch->name.c_str(), base.c_str(), base.size()) != 0) continue;
                    String full = dir_part;
                    full += ch->name;
                    if (ch->is_dir) full += "/";
                    cands.push(full);
                }
            }
        }
        if (cands.empty()) return;

        if (cands.size() == 1) {
            String add = cands[0].substr(prefix.size());
            if (!cands[0].ends_with("/")) add += " ";
            insert_text(add.c_str());
            return;
        }
        // longest common prefix of all candidates
        String common = cands[0];
        for (size_t i = 1; i < cands.size(); i++) {
            size_t k = 0;
            while (k < common.size() && k < cands[i].size() && common[k] == cands[i][k]) k++;
            common = common.substr(0, k);
        }
        if (common.size() > prefix.size()) {
            insert_text(common.substr(prefix.size()).c_str());
            return;
        }
        // list the candidates under the current line
        String echo = prompt_markup();
        echo += input_;
        say(echo);
        String list;
        for (size_t i = 0; i < cands.size(); i++) {
            list += cands[i];
            list += "   ";
        }
        say(list);
    }

    void insert_text(const char* s) {
        size_t n = strlen(s);
        input_.insert(cursor_, s, n);
        cursor_ += (int)n;
        invalidate();
    }

    // ---- paths
    String resolve(const char* p) const {
        if (!p || !*p) return cwd_;
        if (p[0] == '~') return normalize((home_ + String(p + 1)).c_str());
        if (p[0] == '/') return normalize(p);
        String full = cwd_;
        full += "/";
        full += p;
        return normalize(full.c_str());
    }

    // ---- submit / dispatch
    void submit() {
        String text = input_;
        tail_open_ = false;
        String echo = prompt_markup();
        echo += text;
        say(echo);
        input_.clear();
        cursor_ = 0;
        hist_pos_ = -1;
        if (!text.trim().empty()) {
            if (history_.empty() || !(history_.back() == text)) history_.push(text);
            if (history_.size() > kMaxHistory) history_.remove(0);
        }
        run_line(text.c_str());
        view_offset_ = 0;
    }

    // Splits a command line into words, with > and >> operators.
    static void tokenize(const char* s, Vector<String>& toks) {
        String cur;
        bool in_tok = false;
        char q = 0;
        for (const char* p = s; *p; p++) {
            char c = *p;
            if (q) {
                if (c == q) q = 0; else cur += c;
                continue;
            }
            if (c == '"' || c == '\'') { q = c; in_tok = true; continue; }
            if (c == ' ' || c == '\t') {
                if (in_tok) { toks.push(cur); cur.clear(); in_tok = false; }
                continue;
            }
            if (c == '>') {
                if (in_tok) { toks.push(cur); cur.clear(); in_tok = false; }
                if (p[1] == '>') { toks.push(String(">>")); p++; }
                else toks.push(String(">"));
                continue;
            }
            cur += c;
            in_tok = true;
        }
        if (in_tok) toks.push(cur);
    }

    void run_line(const char* s) {
        Vector<String> toks;
        tokenize(s, toks);
        if (toks.empty()) return;

        Vector<String> argv;
        String redir;
        bool append = false, has_redir = false;
        for (size_t i = 0; i < toks.size(); i++) {
            if (toks[i] == ">" || toks[i] == ">>") {
                if (i + 1 >= toks.size()) { err("syntax error near unexpected token `newline'"); return; }
                append = toks[i] == ">>";
                has_redir = true;
                redir = toks[i + 1];
                i++;
            } else argv.push(toks[i]);
        }
        if (argv.empty()) return;

        if (!has_redir) { dispatch(argv); return; }

        String buf;
        capture_ = &buf;
        dispatch(argv);
        capture_ = nullptr;
        String path = resolve(redir.c_str());
        bool ok = append ? vfs::append_file(path.c_str(), buf.c_str(), buf.size())
                         : vfs::write_file(path.c_str(), buf.c_str(), buf.size());
        if (!ok) err("could not write file");
    }

    void dispatch(const Vector<String>& a) {
        const char* c = a[0].c_str();
        if (!strcmp(c, "help")) cmd_help();
        else if (!strcmp(c, "ls")) cmd_ls(a);
        else if (!strcmp(c, "cd")) cmd_cd(a);
        else if (!strcmp(c, "pwd")) say(String(cwd_));
        else if (!strcmp(c, "cat")) cmd_cat(a);
        else if (!strcmp(c, "echo")) cmd_echo(a);
        else if (!strcmp(c, "mkdir")) cmd_mkdir(a);
        else if (!strcmp(c, "rm")) cmd_rm(a);
        else if (!strcmp(c, "touch")) cmd_touch(a);
        else if (!strcmp(c, "mv")) cmd_mv(a, false);
        else if (!strcmp(c, "cp")) cmd_mv(a, true);
        else if (!strcmp(c, "clear")) clear_screen();
        else if (!strcmp(c, "date")) cmd_date();
        else if (!strcmp(c, "uptime")) cmd_uptime();
        else if (!strcmp(c, "uname")) cmd_uname(a);
        else if (!strcmp(c, "whoami")) say(String(session_user()));
        else if (!strcmp(c, "hostname")) say(String("klein"));
        else if (!strcmp(c, "free")) cmd_free();
        else if (!strcmp(c, "df")) cmd_df();
        else if (!strcmp(c, "lspci")) cmd_lspci();
        else if (!strcmp(c, "lsdl")) cmd_lsdl();
        else if (!strcmp(c, "apps")) cmd_apps();
        else if (!strcmp(c, "open")) cmd_open(a);
        else if (!strcmp(c, "ps")) cmd_ps();
        else if (!strcmp(c, "kill")) cmd_kill(a);
        else if (!strcmp(c, "history")) cmd_history();
        else if (!strcmp(c, "neofetch")) cmd_neofetch();
        else if (!strcmp(c, "ping")) cmd_ping(a);
        else if (!strcmp(c, "nslookup")) cmd_nslookup(a);
        else if (!strcmp(c, "calc")) cmd_calc(a);
        else if (!strcmp(c, "klein")) cmd_klein(a);
        else if (!strcmp(c, "fortune")) say(String(kFortunes[timer_ms() % (sizeof(kFortunes) / sizeof(kFortunes[0]))]));
        else if (!strcmp(c, "beep")) speaker_beep(880, 120);
        else if (!strcmp(c, "reboot")) power_reboot();
        else if (!strcmp(c, "shutdown")) power_shutdown();
        else if (!strcmp(c, "exit")) close();
        else {
            String m = String(c) + ": command not found";
            err(m.c_str());
        }
    }

    // ---- built-ins: files
    void cmd_help() {
        line("Built-in commands:", S_WHITE);
        for (int i = 0; i < kCommandCount; i++) {
            String s = ESC "b";
            s += String::format("  %-10s", kCommands[i].name);
            s += ESC "d";
            s += kCommands[i].desc;
            say(s);
        }
        line("Use > and >> to redirect output to a file. Tab completes names and paths.", S_GRAY);
    }

    static uint8_t entry_style(const vfs::Node* n) {
        if (n->is_dir) return S_BLUE;
        if (n->name.ends_with(".dl")) return S_GREEN;
        return S_DEF;
    }

    void cmd_ls(const Vector<String>& a) {
        bool longfmt = false, all = false;
        Vector<String> paths;
        for (size_t i = 1; i < a.size(); i++) {
            const char* s = a[i].c_str();
            if (s[0] == '-' && s[1]) {
                for (const char* f = s + 1; *f; f++) {
                    if (*f == 'l') longfmt = true;
                    else if (*f == 'a') all = true;
                    else { err(String::format("ls: invalid option -- '%c'", *f).c_str()); return; }
                }
            } else paths.push(a[i]);
        }
        if (paths.empty()) paths.push(String("."));
        for (size_t pi = 0; pi < paths.size(); pi++) {
            String abs = resolve(paths[pi].c_str());
            vfs::Node* node = vfs::lookup(abs.c_str());
            if (!node) { err(String::format("ls: %s: No such file or directory", paths[pi].c_str()).c_str()); continue; }
            if (paths.size() > 1 && node->is_dir) line(String::format("%s:", paths[pi].c_str()).c_str(), S_WHITE);
            if (!node->is_dir) { list_entries(&node, 1, longfmt); continue; }

            // collect and sort children by name
            Vector<vfs::Node*> items;
            for (size_t i = 0; i < node->children.size(); i++) {
                vfs::Node* ch = node->children[i];
                if (!all && ch->name[0] == '.') continue;
                items.push(ch);
            }
            for (size_t i = 1; i < items.size(); i++) {
                vfs::Node* key = items[i];
                size_t j = i;
                while (j > 0 && strcmp(items[j - 1]->name.c_str(), key->name.c_str()) > 0) { items[j] = items[j - 1]; j--; }
                items[j] = key;
            }
            list_entries(items.empty() ? nullptr : &items[0], items.size(), longfmt);
        }
    }

    void list_entries(vfs::Node* const* nodes, size_t n, bool longfmt) {
        if (longfmt) {
            for (size_t i = 0; i < n; i++) {
                vfs::Node* nd = nodes[i];
                char size[32];
                if (nd->is_dir) snprintf(size, sizeof(size), "%s", "-");
                else fmt_size(size, sizeof(size), nd->size);
                DateTime dt = unix_to_datetime(nd->mtime);
                const char* perms = nd->is_dir ? "drwxr-xr-x" : nd->readonly ? "-r--r--r--" : "-rw-r--r--";
                String s = String::format("%s %8s  %.3s %02d %02d:%02d  ", perms, size, month_name(dt.month), dt.day, dt.hour, dt.minute);
                s += ESC;
                s += (char)(entry_style(nd) == S_BLUE ? 'b' : entry_style(nd) == S_GREEN ? 'g' : 'd');
                s += nd->name;
                if (nd->is_dir) s += "/";
                s += ESC "d";
                say(s);
            }
            return;
        }
        // column layout
        int widest = 0;
        for (size_t i = 0; i < n; i++) {
            int l = (int)nodes[i]->name.size() + (nodes[i]->is_dir ? 1 : 0);
            if (l > widest) widest = l;
        }
        if (!n) return;
        int colw = widest + 2;
        int cols = cols_for(width());
        int per = cols / colw;
        if (per < 1) per = 1;
        for (size_t i = 0; i < n; i += per) {
            String s;
            for (size_t j = i; j < n && j < i + per; j++) {
                vfs::Node* nd = nodes[j];
                String name = nd->name;
                if (nd->is_dir) name += "/";
                s += ESC;
                s += (char)(entry_style(nd) == S_BLUE ? 'b' : entry_style(nd) == S_GREEN ? 'g' : 'd');
                s += name;
                s += ESC "d";
                int pad = colw - (int)name.size();
                for (int k = 0; k < pad; k++) s += ' ';
            }
            say(s);
        }
    }

    void cmd_cd(const Vector<String>& a) {
        String target = a.size() > 1 ? resolve(a[1].c_str()) : home_;
        vfs::Node* n = vfs::lookup(target.c_str());
        if (!n) { err(String::format("cd: no such file or directory: %s", a.size() > 1 ? a[1].c_str() : "~").c_str()); return; }
        if (!n->is_dir) { err(String::format("cd: not a directory: %s", a[1].c_str()).c_str()); return; }
        cwd_ = target;
        set_title_for_cwd();
    }

    void cmd_cat(const Vector<String>& a) {
        if (a.size() < 2) { err("cat: missing file operand"); return; }
        for (size_t i = 1; i < a.size(); i++) {
            String abs = resolve(a[i].c_str());
            vfs::Node* n = vfs::lookup(abs.c_str());
            if (n && n->is_dir) { err(String::format("cat: %s: Is a directory", a[i].c_str()).c_str()); continue; }
            String text;
            if (!vfs::read_text(abs.c_str(), text)) { err(String::format("cat: %s: No such file or directory", a[i].c_str()).c_str()); continue; }
            String c = clean(text.c_str());
            put(c.c_str());
            if (c.empty() || c[c.size() - 1] != '\n') put("\n");
        }
    }

    void cmd_echo(const Vector<String>& a) {
        String s;
        for (size_t i = 1; i < a.size(); i++) {
            if (i > 1) s += " ";
            s += clean(a[i].c_str());
        }
        say(s);
    }

    void cmd_mkdir(const Vector<String>& a) {
        if (a.size() < 2) { err("mkdir: missing operand"); return; }
        for (size_t i = 1; i < a.size(); i++) {
            if (a[i] == "-p") continue;
            String abs = resolve(a[i].c_str());
            if (vfs::exists(abs.c_str())) { err(String::format("mkdir: %s: File exists", a[i].c_str()).c_str()); continue; }
            vfs::mkdir(abs.c_str());
        }
    }

    void cmd_rm(const Vector<String>& a) {
        bool recursive = false, force = false;
        Vector<String> paths;
        for (size_t i = 1; i < a.size(); i++) {
            if (a[i] == "-r" || a[i] == "-R") recursive = true;
            else if (a[i] == "-f") force = true;
            else if (a[i] == "-rf" || a[i] == "-fr") { recursive = true; force = true; }
            else paths.push(a[i]);
        }
        if (paths.empty()) { err("rm: missing operand"); return; }
        for (size_t i = 0; i < paths.size(); i++) {
            String abs = resolve(paths[i].c_str());
            if (abs == "/" || abs == "/System" || abs == "/Users") { err(String::format("rm: refusing to remove %s", abs.c_str()).c_str()); continue; }
            vfs::Node* n = vfs::lookup(abs.c_str());
            if (!n) { if (!force) err(String::format("rm: %s: No such file or directory", paths[i].c_str()).c_str()); continue; }
            if (n->is_dir && !recursive) { err(String::format("rm: %s: is a directory", paths[i].c_str()).c_str()); continue; }
            if (n->readonly) { err(String::format("rm: %s: Read-only file system", paths[i].c_str()).c_str()); continue; }
            if (!vfs::remove(abs.c_str())) { err(String::format("rm: cannot remove %s", paths[i].c_str()).c_str()); continue; }
            if (cwd_ == abs || (cwd_.size() > abs.size() && strncmp(cwd_.c_str(), abs.c_str(), abs.size()) == 0 && cwd_[abs.size()] == '/')) {
                cwd_ = home_;
                set_title_for_cwd();
            }
        }
    }

    void cmd_touch(const Vector<String>& a) {
        if (a.size() < 2) { err("touch: missing file operand"); return; }
        for (size_t i = 1; i < a.size(); i++) {
            String abs = resolve(a[i].c_str());
            if (!vfs::exists(abs.c_str()) && !vfs::write_text(abs.c_str(), "")) err(String::format("touch: cannot touch %s", a[i].c_str()).c_str());
        }
    }

    void cmd_mv(const Vector<String>& a, bool copy) {
        if (a.size() != 3) { err(copy ? "usage: cp <source> <dest>" : "usage: mv <source> <dest>"); return; }
        String src = resolve(a[1].c_str());
        String dst = resolve(a[2].c_str());
        if (!vfs::exists(src.c_str())) { err(String::format("%s: %s: No such file or directory", copy ? "cp" : "mv", a[1].c_str()).c_str()); return; }
        if (vfs::is_dir(dst.c_str())) dst = vfs::join(dst.c_str(), vfs::basename(src.c_str()).c_str());
        bool ok = copy ? vfs::copy(src.c_str(), dst.c_str()) : vfs::rename(src.c_str(), dst.c_str());
        if (!ok) err(String::format("%s: cannot write %s", copy ? "cp" : "mv", a[2].c_str()).c_str());
    }

    // ---- built-ins: system
    void cmd_date() {
        DateTime dt = rtc_now();
        say(String::format("%s %s %2d %02d:%02d:%02d %d", abbrev(weekday_name(dt.weekday)).c_str(),
                           abbrev(month_name(dt.month)).c_str(), dt.day, dt.hour, dt.minute, dt.second, dt.year));
    }

    void cmd_uptime() {
        uint64_t s = timer_ms() / 1000;
        say(String::format("up %lluh %02llum %02llus", (unsigned long long)(s / 3600),
                           (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60)));
    }

    void cmd_uname(const Vector<String>& a) {
        bool all = a.size() > 1 && a[1] == "-a";
        if (!all) { say(String("KleinOS")); return; }
        say(String::format("KleinOS klein %s.%d %s %s KleinOS", KLEIN_VERSION, system_build_number(), KLEIN_CODENAME, sizeof(void*) == 4 ? "i686" : "x86_64"));
    }

    void cmd_free() {
        uint64_t mem = boot_info().mem_total >> 20;
        uint64_t heap_t = heap_total() >> 20, heap_u = heap_used() >> 20;
        line("              total        used        free", S_WHITE);
        say(String::format("Memory   %8llu MB %8llu MB %8llu MB", (unsigned long long)mem, (unsigned long long)heap_u,
                           (unsigned long long)(mem > heap_u ? mem - heap_u : 0)));
        say(String::format("Heap     %8llu MB %8llu MB %8llu MB", (unsigned long long)heap_t, (unsigned long long)heap_u,
                           (unsigned long long)(heap_t > heap_u ? heap_t - heap_u : 0)));
    }

    void cmd_df() {
        line("Device       Size       Sectors  Model", S_WHITE);
        for (int i = 0; i < ata_drive_count(); i++) {
            AtaDrive* d = ata_drive(i);
            if (!d || !d->present) continue;
            char size[32];
            fmt_size(size, sizeof(size), d->sectors * 512);
            say(String::format("ata%d        %-10s %-8llu %s", i, size, (unsigned long long)d->sectors, d->model));
        }
        line("vfs          in memory  -        /", S_GRAY);
    }

    void cmd_lspci() {
        int n = pci_count();
        for (int i = 0; i < n; i++) {
            const PciDevice* d = pci_get(i);
            if (!d) continue;
            String s = ESC "k";
            s += String::format("%02x:%02x.%x  ", d->bus, d->slot, d->func);
            s += ESC "d";
            s += String::format("%04x:%04x  ", d->vendor, d->device);
            s += ESC "b";
            s += pci_class_name(d->class_code, d->subclass);
            s += ESC "d";
            say(s);
        }
        say(String::format("%d devices", n));
    }

    void cmd_lsdl() {
        const Vector<DlModule*>& mods = dl_modules();
        line("name                 size      relocs  imports", S_WHITE);
        for (size_t i = 0; i < mods.size(); i++) {
            DlModule* m = mods[i];
            char size[32];
            fmt_size(size, sizeof(size), m->file_size);
            s_line(String::format("%-20s %-9s %6d  %7d", m->name.c_str(), size, m->relocations, m->imports), S_GREEN);
        }
        say(String::format("%zu modules, %zu kernel symbols", mods.size(), kernel_symbol_count()));
    }

    void s_line(const String& text, uint8_t style) {
        String s = ESC;
        s += (char)(style == S_GREEN ? 'g' : 'd');
        s += text;
        s += ESC "d";
        say(s);
    }

    void cmd_apps() {
        int n = app_count();
        for (int i = 0; i < n; i++) {
            const AppInfo* a = app_get(i);
            if (!a) continue;
            String s = ESC "b";
            s += String::format("%-12s", a->id);
            s += ESC "d";
            s += String::format("%-22s %-13s ", a->name, a->category);
            s += ESC "k";
            s += app_module(a->id);
            s += ESC "d";
            say(s);
        }
    }

    void cmd_open(const Vector<String>& a) {
        if (a.size() < 2) { err("open: missing operand"); return; }
        if (app_find(a[1].c_str())) { app_launch(a[1].c_str()); return; }
        String abs = resolve(a[1].c_str());
        if (vfs::exists(abs.c_str())) { app_open_file(abs.c_str()); return; }
        err(String::format("open: no such app or file: %s", a[1].c_str()).c_str());
    }

    void cmd_ps() {
        const Vector<Window*>& ws = wm::windows();
        line("  #  app             title", S_WHITE);
        for (size_t i = 0; i < ws.size(); i++) {
            Window* w = ws[i];
            String s = String::format("%3zu  ", i + 1);
            s += ESC "b";
            s += String::format("%-15s", w->app_id()[0] ? w->app_id() : "-");
            s += ESC "d ";
            s += w->title();
            if (w->minimized()) s += ESC "k  (minimized)" ESC "d";
            say(s);
        }
    }

    void cmd_kill(const Vector<String>& a) {
        if (a.size() < 2) { err("usage: kill <window number from ps>"); return; }
        int n = atoi(a[1].c_str());
        const Vector<Window*>& ws = wm::windows();
        if (n < 1 || n > (int)ws.size()) { err(String::format("kill: no window %s", a[1].c_str()).c_str()); return; }
        Window* w = ws[n - 1];
        say(String::format("killed #%d %s", n, w->title()));
        w->close();
    }

    void cmd_history() {
        for (size_t i = 0; i < history_.size(); i++) say(String::format("%5zu  %s", i + 1, history_[i].c_str()));
    }

    void cmd_neofetch() {
        static const char* const logo[] = {
            "##   ##", "##  ##", "## ##", "####", "## ##", "##  ##", "##   ##",
        };
        static const uint8_t logo_style[] = {S_CYAN, S_CYAN, S_BLUE, S_BLUE, S_MAGENTA, S_MAGENTA, S_PINK};
        uint64_t s = timer_ms() / 1000;
        uint64_t mem = boot_info().mem_total >> 20;
        uint64_t used = heap_used() >> 20;
        char ip[16] = "offline";
        if (net::is_up()) snprintf(ip, sizeof(ip), "%s", net::ip_string());
        String cpu = cpu_brand();
        if (cpu.size() > 34) cpu = cpu.substr(0, 34);
        String info[] = {
            String::format("%s%s@klein%s", ESC "g", session_user(), ESC "d"),
            String("------------------"),
            String::format("OS: KleinOS %s.%d \"%s\"", KLEIN_VERSION, system_build_number(), KLEIN_CODENAME),
            String::format("Build: %d%s", system_build_number(), system_is_dev_build() ? " (dev)" : ""),
            String::format("Uptime: %lluh %llum %llus", (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60)),
            String::format("Shell: Terminal (%s)", shown_cwd().c_str()),
            String::format("CPU: %s", cpu.c_str()),
            String::format("Memory: %llu / %llu MB", (unsigned long long)used, (unsigned long long)mem),
            String::format("Resolution: %d x %d", wm::screen_width(), wm::screen_height()),
            String::format("Apps: %d   Modules: %zu", app_count(), dl_modules().size()),
            String::format("Network: %s", ip),
        };
        const int nlogo = sizeof(logo) / sizeof(logo[0]);
        const int ninfo = sizeof(info) / sizeof(info[0]);
        int rows = nlogo > ninfo ? nlogo : ninfo;
        for (int r = 0; r < rows; r++) {
            String s;
            if (r < nlogo) {
                s += ESC;
                s += (char)(logo_style[r] == S_CYAN ? 'c' : logo_style[r] == S_BLUE ? 'b' : logo_style[r] == S_MAGENTA ? 'm' : 'p');
                s += logo[r];
                s += ESC "d";
            }
            int pad = 14 - (r < nlogo ? (int)strlen(logo[r]) : 0);
            for (int k = 0; k < pad; k++) s += ' ';
            if (r < ninfo) {
                if (r == 0) s += info[0];
                else if (r == 1) { s += ESC "k"; s += info[1]; s += ESC "d"; }
                else { String lbl = info[r]; long colon = lbl.find(':'); if (colon > 0) { s += ESC "b"; s += lbl.substr(0, colon + 1); s += ESC "d"; s += lbl.substr(colon + 1); } else s += lbl; }
            }
            say(s);
        }
        // colour swatches
        String sw = "              ";
        for (uint8_t k = S_GREEN; k <= S_PINK; k++) {
            sw += ESC;
            sw += (char)(k == S_GREEN ? 'g' : k == S_BLUE ? 'b' : k == S_RED ? 'r' : k == S_YELLOW ? 'y' : k == S_CYAN ? 'c' : k == S_MAGENTA ? 'm' : k == S_GRAY ? 'k' : k == S_WHITE ? 'w' : k == S_ORANGE ? 'o' : 'p');
            sw += "### ";
        }
        sw += ESC "d";
        say(sw);
    }

    // ---- built-ins: network (asynchronous, polled from on_tick)
    enum class Job { None, Dns, Ping, Lookup };

    void cmd_ping(const Vector<String>& a) {
        if (a.size() < 2) { err("usage: ping <host|ip>"); return; }
        if (job_ != Job::None) { err("ping: another request is running (Ctrl+C cancels)"); return; }
        if (!net::is_up()) { line("ping: network unavailable", S_RED); return; }
        uint32_t ip = 0;
        if (parse_ip(a[1].c_str(), ip)) { start_ping(ip, a[1].c_str()); return; }
        job_host_ = a[1];
        job_dns_ = net::dns_resolve(a[1].c_str());
        if (job_dns_ < 0) { line("ping: cannot resolve host", S_RED); return; }
        job_ = Job::Dns;
        job_t0_ = timer_ms();
    }

    void cmd_nslookup(const Vector<String>& a) {
        if (a.size() < 2) { err("usage: nslookup <host>"); return; }
        if (job_ != Job::None) { err("nslookup: another request is running (Ctrl+C cancels)"); return; }
        if (!net::is_up()) { line("nslookup: network unavailable", S_RED); return; }
        uint32_t ip = 0;
        if (parse_ip(a[1].c_str(), ip)) { say(String::format("%s is an address", a[1].c_str())); return; }
        job_host_ = a[1];
        job_dns_ = net::dns_resolve(a[1].c_str());
        if (job_dns_ < 0) { line("nslookup: can't resolve host", S_RED); return; }
        job_ = Job::Lookup;
        job_t0_ = timer_ms();
    }

    void start_ping(uint32_t ip, const char* host) {
        char ips[16];
        net::ip_to_string(ip, ips);
        job_ip_ = ip;
        job_host_ = host;
        job_ = Job::Ping;
        job_sent_ = 0;
        job_recv_ = 0;
        job_seq_ = -1;
        job_next_ = 0;
        say(String::format("PING %s (%s): 56 data bytes", host, ips));
    }

    void cancel_job() {
        job_ = Job::None;
        job_seq_ = -1;
    }

    void tick_job(uint64_t now) {
        if (job_ == Job::None) return;
        if (job_ == Job::Dns || job_ == Job::Lookup) {
            uint32_t ip = 0;
            int r = net::dns_result(job_dns_, &ip);
            if (r == 1) {
                if (job_ == Job::Lookup) {
                    char ips[16];
                    net::ip_to_string(ip, ips);
                    char srv[16];
                    net::ip_to_string(net::status().dns, srv);
                    say(String::format("Server:  %s", srv));
                    say(String::format("Name:    %s", job_host_.c_str()));
                    say(String::format("Address: %s", ips));
                    job_ = Job::None;
                } else {
                    start_ping(ip, job_host_.c_str());
                }
            } else if (r < 0 || now - job_t0_ > 6000) {
                line(job_ == Job::Lookup ? "nslookup: lookup timed out" : "ping: cannot resolve host", S_RED);
                job_ = Job::None;
            }
            return;
        }
        // ping: four echoes, one per second; each waits up to two seconds
        if (job_seq_ < 0) {
            if (job_sent_ >= kPingCount) {
                int loss = job_sent_ ? (job_sent_ - job_recv_) * 100 / job_sent_ : 0;
                char ips[16];
                net::ip_to_string(job_ip_, ips);
                say(String::format("--- %s ping statistics ---", job_host_.c_str()));
                say(String::format("%d packets transmitted, %d received, %d%% packet loss", job_sent_, job_recv_, loss));
                job_ = Job::None;
                return;
            }
            if (now < job_next_) return;
            job_seq_ = net::ping_send(job_ip_);
            job_t0_ = now;
            job_sent_++;
            if (job_seq_ < 0) { line("ping: sendto failed", S_RED); job_ = Job::None; }
            return;
        }
        int rtt = net::ping_result(job_seq_);
        char ips[16];
        net::ip_to_string(job_ip_, ips);
        if (rtt >= 0) {
            job_recv_++;
            say(String::format("64 bytes from %s: icmp_seq=%d ttl=64 time=%d ms", ips, job_sent_ - 1, rtt));
            job_seq_ = -1;
            job_next_ = now + 800;
        } else if (now - job_t0_ > 2000) {
            say(String::format("Request timeout for icmp_seq %d", job_sent_ - 1));
            job_seq_ = -1;
            job_next_ = now + 800;
        }
    }

    // ---- built-ins: misc
    void cmd_calc(const Vector<String>& a) {
        if (a.size() < 2) { err("usage: calc <expression>"); return; }
        String expr;
        for (size_t i = 1; i < a.size(); i++) { if (i > 1) expr += " "; expr += a[i]; }
        Calc c;
        c.p = expr.c_str();
        double v = c.expr();
        c.ws();
        if (c.bad || *c.p) { err("calc: syntax error"); return; }
        if (fabs(v - floor(v)) < 1e-9 && fabs(v) < 1e15) say(String::format("= %lld", (long long)(v < 0 ? v - 0.5 : v + 0.5)));
        else {
            String s = String::format("= %.10f", v);
            while (s.size() > 3 && s[s.size() - 1] == '0') s.erase(s.size() - 1, 1);
            say(s);
        }
    }

    void cmd_klein(const Vector<String>& a) {
        if (a.size() >= 2 && a[1] == "say") {
            String text;
            for (size_t i = 2; i < a.size(); i++) { if (i > 2) text += " "; text += clean(a[i].c_str()); }
            if (text.empty()) text = "...";
            size_t n = text.size();
            String top = " "; for (size_t i = 0; i < n + 2; i++) top += "_";
            String mid = String("< ") + text + " >";
            String bot = " "; for (size_t i = 0; i < n + 2; i++) bot += "-";
            say(top);
            say(mid);
            say(bot);
            line("     \\", S_CYAN);
            line("      \\   .-~~~-.", S_CYAN);
            line("       \\ ( o   o )", S_CYAN);
            line("          \\  ^  /", S_CYAN);
            line("           `~~~~~'", S_CYAN);
            return;
        }
        if (a.size() >= 2 && a[1] == "version") { say(String::format("klein %s.%d (%s)", KLEIN_VERSION, system_build_number(), KLEIN_CODENAME)); return; }
        line("usage: klein say <text> | klein version", S_GRAY);
    }

    // ---- state
    Vector<Row> rows_;
    bool tail_open_ = false;
    String cwd_, home_;
    String input_;
    int cursor_ = 0;
    size_t prompt_len_ = 0;
    Vector<String> history_;
    int hist_pos_ = -1;
    String saved_;
    int view_offset_ = 0;
    bool cursor_on_ = true;
    String* capture_ = nullptr;

    Job job_ = Job::None;
    String job_host_;
    int job_dns_ = -1;
    uint32_t job_ip_ = 0;
    uint64_t job_t0_ = 0, job_next_ = 0;
    int job_sent_ = 0, job_recv_ = 0, job_seq_ = -1;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x3A4150), hex(0x12151B));
    // prompt chevron and cursor block
    float u = r.w / 96.0f;
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
    c.draw_line(cx - 26 * u, cy - 12 * u, cx - 12 * u, cy, hex(0x6BE675), 7 * u);
    c.draw_line(cx - 12 * u, cy, cx - 26 * u, cy + 12 * u, hex(0x6BE675), 7 * u);
    c.fill_round_rect(Rect((int)(cx - 2 * u), (int)(cy + 8 * u), (int)(26 * u), (int)(8 * u)), 3 * u, hex(0xE6E8EB));
}

Window* create() { return new TerminalWindow(); }

AppRegistrar reg({"terminal", "Terminal", icon, create, "System", false, true, 20, "Command-line shell"});

}  // namespace
