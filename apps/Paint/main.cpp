// Paint: a small bitmap editor. Tools, 16 swatches, 10-step undo, BMP save/open.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "fs/vfs.h"
#include "sys/session.h"
#include "lib/heap.h"
#include "lib/math.h"
#include "lib/string.h"

namespace {

constexpr int kCW = 800, kCH = 560;      // artwork size
constexpr int kCX = 128, kCY = 72;       // artwork origin inside the window
constexpr int kWinW = 944, kWinH = 648;
constexpr int kUndoDepth = 10;

enum Tool : int { T_Pen, T_Brush, T_Eraser, T_Line, T_Rect, T_Ellipse, T_Fill, T_Picker, T_Count };

const Color kSwatches[16] = {
    hex(0x000000), hex(0x4A4A4F), hex(0x8E8E93), hex(0xC7C7CC),
    hex(0xFFFFFF), hex(0xFF3B30), hex(0xFF9500), hex(0xFFCC00),
    hex(0x34C759), hex(0x00C7BE), hex(0x32ADE6), hex(0x007AFF),
    hex(0x5856D6), hex(0xAF52DE), hex(0xFF2D55), hex(0xA2845E),
};

String pictures_dir() { return String("/Users/") + session_user() + "/Pictures"; }

bool is_shape(int t) { return t == T_Line || t == T_Rect || t == T_Ellipse; }

Rect tool_rect(int i) { return Rect(20 + (i % 2) * 44, 84 + (i / 2) * 44, 40, 40); }
Rect swatch_rect(int i) { return Rect(20 + (i % 4) * 24, 404 + (i / 4) * 24, 22, 22); }
Rect color_well() { return Rect(20, 350, 92, 36); }
Rect size_slider_rect() { return Rect(14, 288, 100, 26); }

void put16(uint8_t* p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
void put32(uint8_t* p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, (v >> 16) & 0xFFFF); }

// 24-bit uncompressed BMP (bottom-up rows, BGR).
bool save_bmp(const Canvas& img, const char* path) {
    int w = img.width(), h = img.height();
    uint32_t row_bytes = ((w * 3 + 3) / 4) * 4;
    uint32_t data = row_bytes * (uint32_t)h;
    uint32_t total = 54 + data;
    uint8_t* buf = (uint8_t*)kmalloc(total);
    if (!buf) return false;
    memset(buf, 0, 54);
    buf[0] = 'B';
    buf[1] = 'M';
    put32(buf + 2, total);
    put32(buf + 10, 54);
    put32(buf + 14, 40);
    put32(buf + 18, (uint32_t)w);
    put32(buf + 22, (uint32_t)h);
    put16(buf + 26, 1);
    put16(buf + 28, 24);
    put32(buf + 34, data);
    put32(buf + 38, 2835);
    put32(buf + 42, 2835);
    for (int y = 0; y < h; y++) {
        const uint32_t* src = img.row(h - 1 - y);
        uint8_t* d = buf + 54 + (size_t)y * row_bytes;
        for (int x = 0; x < w; x++) {
            uint32_t p = src[x];
            d[x * 3] = p & 0xFF;
            d[x * 3 + 1] = (p >> 8) & 0xFF;
            d[x * 3 + 2] = (p >> 16) & 0xFF;
        }
        for (uint32_t i = w * 3; i < row_bytes; i++) d[i] = 0;
    }
    bool ok = vfs::write_file(path, buf, total);
    kfree(buf);
    return ok;
}

void outline_ellipse(Canvas& c, float cx, float cy, float rx, float ry, Color col, float w) {
    const int n = 72;
    float px = cx + rx, py = cy;
    for (int i = 1; i <= n; i++) {
        float a = (float)i * 2.0f * M_PI_F / n;
        float x = cx + rx * cosf(a), y = cy + ry * sinf(a);
        c.draw_line(px, py, x, y, col, w);
        px = x;
        py = y;
    }
}

// Shape from (x0,y0) to (x1,y1) in artwork space, drawn at offset (dx,dy).
void draw_shape(Canvas& c, int dx, int dy, int tool, int x0, int y0, int x1, int y1, Color col, float w) {
    x0 += dx; y0 += dy; x1 += dx; y1 += dy;
    int l = kmin(x0, x1), r = kmax(x0, x1), t = kmin(y0, y1), b = kmax(y0, y1);
    if (tool == T_Line) {
        c.draw_line(x0, y0, x1, y1, col, w);
        c.fill_circle(x0, y0, w / 2, col);
        c.fill_circle(x1, y1, w / 2, col);
    } else if (tool == T_Rect) {
        c.draw_line(l, t, r, t, col, w);
        c.draw_line(r, t, r, b, col, w);
        c.draw_line(r, b, l, b, col, w);
        c.draw_line(l, b, l, t, col, w);
    } else if (tool == T_Ellipse) {
        outline_ellipse(c, (l + r) / 2.0f, (t + b) / 2.0f, (r - l) / 2.0f, (b - t) / 2.0f, col, w);
    }
}

// Scanline-free flood fill (explicit stack) on a premultiplied artwork.
void flood_fill(Canvas& a, int sx, int sy, Color col) {
    uint32_t target = a.get_pixel(sx, sy);
    uint32_t fillv = premul(col);
    if (target == fillv) return;
    Vector<int> stack;
    stack.push(sy * kCW + sx);
    while (!stack.empty()) {
        int idx = stack.back();
        stack.pop();
        int x = idx % kCW, y = idx / kCW;
        uint32_t* row = a.row(y);
        if (row[x] != target) continue;
        row[x] = fillv;
        if (x > 0 && row[x - 1] == target) stack.push(idx - 1);
        if (x < kCW - 1 && row[x + 1] == target) stack.push(idx + 1);
        if (y > 0 && a.row(y - 1)[x] == target) stack.push(idx - kCW);
        if (y < kCH - 1 && a.row(y + 1)[x] == target) stack.push(idx + kCW);
    }
}

void tool_icon(Canvas& c, Rect r, int tool, Color col) {
    float s = r.w / 40.0f;
    auto X = [&](float v) { return r.x + v * s; };
    auto Y = [&](float v) { return r.y + v * s; };
    switch (tool) {
    case T_Pen: {
        PointF body[4] = {{X(26), Y(10)}, {X(30), Y(14)}, {X(16), Y(28)}, {X(12), Y(24)}};
        c.fill_polygon(body, 4, col);
        PointF tip[3] = {{X(12), Y(24)}, {X(16), Y(28)}, {X(10), Y(31)}};
        c.fill_polygon(tip, 3, col);
        break;
    }
    case T_Brush:
        c.draw_line(X(12), Y(28), X(24), Y(16), col, 3 * s);
        c.fill_circle(X(27), Y(13), 5 * s, col);
        break;
    case T_Eraser: {
        PointF d[4] = {{X(10), Y(20)}, {X(22), Y(10)}, {X(32), Y(20)}, {X(20), Y(30)}};
        c.fill_polygon(d, 4, col);
        break;
    }
    case T_Line:
        c.draw_line(X(10), Y(30), X(30), Y(10), col, 2.2f * s);
        c.fill_circle(X(10), Y(30), 3 * s, col);
        c.fill_circle(X(30), Y(10), 3 * s, col);
        break;
    case T_Rect:
        c.stroke_rect(Rect((int)X(9), (int)Y(12), (int)(22 * s), (int)(18 * s)), col, 2);
        break;
    case T_Ellipse:
        outline_ellipse(c, X(20), Y(20), 12 * s, 9 * s, col, 2.2f * s);
        break;
    case T_Fill: {
        PointF bucket[4] = {{X(10), Y(14)}, {X(24), Y(8)}, {X(30), Y(22)}, {X(16), Y(28)}};
        c.fill_polygon(bucket, 4, col);
        c.fill_circle(X(31), Y(31), 3 * s, col);
        break;
    }
    case T_Picker:
        c.draw_line(X(12), Y(28), X(22), Y(18), col, 3 * s);
        c.fill_circle(X(27), Y(13), 5 * s, col);
        break;
    }
}

void collect_images(vfs::Node* dir, Vector<String>& out) {
    for (vfs::Node* ch : dir->children) {
        if (!ch) continue;
        if (ch->is_dir) { collect_images(ch, out); continue; }
        String ext = vfs::extension(ch->name.c_str());
        if (ext == "bmp" || ext == "png" || ext == "jpg" || ext == "jpeg") out.push(vfs::path_of(ch));
    }
}

class PaintWindow : public Window {
public:
    PaintWindow() : Window("Paint", kWinW, kWinH, WF_NO_ZOOM) {
        art_.resize(kCW, kCH);
        art_.clear(hex(0xFFFFFF));

        dd_ = (Dropdown*)add(new Dropdown(Rect(14, 13, 250, 30)));
        open_btn_ = (Button*)add(new Button(Rect(272, 13, 70, 30), "Open"));
        name_ = (TextField*)add(new TextField(Rect(352, 13, 168, 30), "Untitled"));
        name_->accept_char = [](uint32_t ch) { return ch != '/'; };
        name_->set_text("Untitled");
        save_ = (Button*)add(new Button(Rect(528, 13, 70, 30), "Save", Button::Primary));
        undo_btn_ = (Button*)add(new Button(Rect(760, 13, 80, 30), "Undo"));
        clear_btn_ = (Button*)add(new Button(Rect(848, 13, 80, 30), "Clear", Button::Destructive));
        slider_ = (Slider*)add(new Slider(size_slider_rect(), 0.3f));

        open_btn_->on_click = [this] { open_selected(); };
        save_->on_click = [this] { save_current(); };
        undo_btn_->on_click = [this] { undo(); };
        clear_btn_->on_click = [this] { push_undo(); art_.clear(hex(0xFFFFFF)); invalidate(); };

        set_ticking(true);
        gen_ = vfs::generation();
        rescan();
    }

    ~PaintWindow() {
        for (int i = 0; i < hist_n_; i++) delete hist_[i];
    }

    void on_paint(Canvas& c) override {
        Window::on_paint(c);
        const Theme& t = theme();
        c.fill_rect(Rect(0, 56, width(), 1), t.separator);

        // Tool palette
        draw_group_box(c, Rect(12, kCY, 104, kCH));
        slider_->paint(c);   // repaint over the group box
        for (int i = 0; i < T_Count; i++) {
            Rect r = tool_rect(i);
            bool sel = i == tool_;
            c.fill_round_rect(r, 10, sel ? t.accent : (t.dark ? rgba(255, 255, 255, 22) : rgba(0, 0, 0, 12)));
            tool_icon(c, r, i, sel ? hex(0xFFFFFF) : t.text);
        }

        // Size
        c.draw_text(20, 268, "Size", t.text_secondary, 12, Font::Medium);
        char sz[16];
        snprintf(sz, sizeof(sz), "%d px", size_px());
        c.draw_text_in(Rect(20, 268, 92, 16), sz, t.text_secondary, 12, Font::Regular, Align::Right);

        // Current colour and swatches
        c.draw_text(20, 332, "Color", t.text_secondary, 12, Font::Medium);
        Rect well = color_well();
        c.draw_shadow(well, 8, 3, rgba(0, 0, 0, 30), 0, 1);
        c.fill_round_rect(well, 8, fg_);
        c.stroke_round_rect(well, 8, t.separator, 1);
        for (int i = 0; i < 16; i++) {
            Rect r = swatch_rect(i);
            c.fill_round_rect(r, 5, kSwatches[i]);
            c.stroke_round_rect(r, 5, rgba(0, 0, 0, 40), 1);
            if (kSwatches[i] == fg_) c.stroke_round_rect(r.expand(2), 7, t.accent, 2);
        }

        // Artwork
        c.draw_shadow(Rect(kCX, kCY, kCW, kCH), 4, 10, rgba(0, 0, 0, 60), 0, 4);
        c.blit(art_, kCX, kCY);
        c.stroke_rect(Rect(kCX - 1, kCY - 1, kCW + 2, kCH + 2), t.separator, 1);
        if (drawing_ && is_shape(tool_))
            draw_shape(c, kCX, kCY, tool_, sx_, sy_, ex_, ey_, fg_, (float)size_px());

        // Stroke preview at the bottom of the palette
        c.fill_round_rect(Rect(20, 530, 92, 80), 10, t.dark ? rgba(255, 255, 255, 18) : rgba(0, 0, 0, 8));
        c.save();
        c.clip_to(Rect(20, 530, 92, 80));
        c.draw_line(30, 596, 102, 564, fg_, (float)size_px());
        c.restore();
    }

    void on_mouse_down(int x, int y, int button) override {
        if (button != MB_LEFT) { Window::on_mouse_down(x, y, button); return; }
        int lx = x - kCX, ly = y - kCY;
        if (lx >= 0 && ly >= 0 && lx < kCW && ly < kCH) {
            focus_widget(nullptr);
            if (tool_ == T_Picker) {
                uint32_t raw = art_.get_pixel(lx, ly);
                fg_ = 0xFF000000u | (raw & 0x00FFFFFF);
                invalidate();
                return;
            }
            if (tool_ == T_Fill) {
                push_undo();
                flood_fill(art_, lx, ly, fg_);
                invalidate();
                return;
            }
            push_undo();
            drawing_ = true;
            sx_ = ex_ = lx;
            sy_ = ey_ = ly;
            if (!is_shape(tool_)) {
                stroke(art_, lx, ly, lx, ly);
                invalidate();
            }
            invalidate();
            return;
        }
        for (int i = 0; i < T_Count; i++) {
            if (tool_rect(i).contains(x, y)) { tool_ = i; invalidate(); return; }
        }
        for (int i = 0; i < 16; i++) {
            if (swatch_rect(i).contains(x, y)) { fg_ = kSwatches[i]; invalidate(); return; }
        }
        if (color_well().contains(x, y)) return;
        Window::on_mouse_down(x, y, button);
    }

    void on_mouse_move(int x, int y, int buttons) override {
        if (!drawing_) { Window::on_mouse_move(x, y, buttons); return; }
        if (!(buttons & MB_LEFT)) { finish_drawing(); return; }
        int lx = x - kCX, ly = y - kCY;
        if (is_shape(tool_)) {
            ex_ = lx;
            ey_ = ly;
        } else {
            stroke(art_, sx_, sy_, lx, ly);
            sx_ = lx;
            sy_ = ly;
        }
        invalidate();
    }

    void on_mouse_up(int x, int y, int button) override {
        if (!drawing_) { Window::on_mouse_up(x, y, button); return; }
        finish_drawing();
    }

    void on_scroll(int x, int y, int delta) override { Window::on_scroll(x, y, delta); }

    void on_key(const KeyEvent& e) override {
        if (e.key == Key::Char && e.cmd()) {
            if ((e.ch | 0x20) == 'z') { undo(); return; }
        }
        if (!focused_widget() && e.key == Key::Char && !e.cmd()) {
            switch (e.ch | 0x20) {
            case 'p': tool_ = T_Pen; invalidate(); return;
            case 'b': tool_ = T_Brush; invalidate(); return;
            case 'e': tool_ = T_Eraser; invalidate(); return;
            case 'l': tool_ = T_Line; invalidate(); return;
            case 'r': tool_ = T_Rect; invalidate(); return;
            case 'o': tool_ = T_Ellipse; invalidate(); return;
            case 'f': tool_ = T_Fill; invalidate(); return;
            case 'i': tool_ = T_Picker; invalidate(); return;
            }
        }
        Window::on_key(e);
    }

    void on_tick(uint64_t) override {
        uint64_t g = vfs::generation();
        if (g != gen_) { gen_ = g; rescan(); }
    }

private:
    int size_px() const { return 1 + (int)(slider_->value * 23.0f + 0.5f); }

    // Stroke segment in artwork space.
    void stroke(Canvas& a, int x0, int y0, int x1, int y1) {
        if (tool_ == T_Brush) {
            float wide = size_px() * 2.2f, narrow = kmax(1.0f, size_px() * 0.9f);
            Color soft = with_alpha(fg_, 90);
            if (x0 != x1 || y0 != y1) a.draw_line(x0, y0, x1, y1, soft, wide);
            a.fill_circle(x1, y1, wide / 2, soft);
            if (x0 != x1 || y0 != y1) a.draw_line(x0, y0, x1, y1, fg_, narrow);
            a.fill_circle(x1, y1, narrow / 2, fg_);
            return;
        }
        Color col = tool_ == T_Eraser ? hex(0xFFFFFF) : fg_;
        float w = tool_ == T_Eraser ? size_px() * 1.6f : (float)size_px();
        if (x0 != x1 || y0 != y1) a.draw_line(x0, y0, x1, y1, col, w);
        a.fill_circle(x1, y1, w / 2, col);
    }

    void finish_drawing() {
        drawing_ = false;
        if (is_shape(tool_)) draw_shape(art_, 0, 0, tool_, sx_, sy_, ex_, ey_, fg_, (float)size_px());
        invalidate();
    }

    void push_undo() {
        if (hist_n_ == kUndoDepth) {
            delete hist_[0];
            for (int i = 1; i < hist_n_; i++) hist_[i - 1] = hist_[i];
            hist_n_--;
        }
        Canvas* s = new Canvas(kCW, kCH);
        s->blit(art_, 0, 0);
        hist_[hist_n_++] = s;
    }

    void undo() {
        if (!hist_n_) return;
        Canvas* s = hist_[--hist_n_];
        hist_[hist_n_] = nullptr;
        art_.blit(*s, 0, 0);
        delete s;
        invalidate();
    }

    void rescan() {
        String prev = files_.empty() ? String() : files_[dd_->selected < (int)files_.size() ? dd_->selected : 0];
        files_.clear();
        String dir = pictures_dir();
        vfs::Node* node = vfs::lookup(dir.c_str());
        if (node && node->is_dir) collect_images(node, files_);
        // alphabetical
        for (size_t i = 1; i < files_.size(); i++)
            for (size_t j = i; j > 0 && files_[j] < files_[j - 1]; j--) {
                String tmp = files_[j]; files_[j] = files_[j - 1]; files_[j - 1] = tmp;
            }
        dd_->options.clear();
        dd_->selected = 0;
        String base = dir + "/";
        for (size_t i = 0; i < files_.size(); i++) {
            String rel = files_[i].substr(base.size());
            dd_->options.push(rel);
            if (files_[i] == prev) dd_->selected = (int)i;
        }
        if (files_.empty()) dd_->options.push(String("No images in Pictures"));
        open_btn_->enabled = !files_.empty();
        dd_->invalidate();
        invalidate();
    }

    void open_selected() {
        if (files_.empty()) return;
        int i = dd_->selected;
        if (i < 0 || i >= (int)files_.size()) return;
        load_file(files_[i].c_str());
    }

    void load_file(const char* path) {
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!vfs::read_file(path, &data, &size)) return;
        Canvas* img = image_decode(data, size);
        if (!img) { wm::notify("Paint", "That image could not be opened.", "paint"); return; }
        push_undo();
        art_.clear(hex(0xFFFFFF));
        int iw = img->width(), ih = img->height();
        if (iw <= kCW && ih <= kCH) {
            art_.draw(*img, (kCW - iw) / 2, (kCH - ih) / 2);
        } else {
            float s = kmin(kCW / (float)iw, kCH / (float)ih);
            int dw = (int)(iw * s), dh = (int)(ih * s);
            art_.draw_scaled(*img, Rect((kCW - dw) / 2, (kCH - dh) / 2, dw, dh));
        }
        delete img;
        String base = vfs::basename(path);
        long dot = base.rfind('.');
        if (dot > 0) base = base.substr(0, dot);
        name_->set_text(base.c_str());
        invalidate();
    }

    void save_current() {
        String name = String(name_->get_text()).trim();
        if (name.empty()) name = "Untitled";
        String dir = pictures_dir();
        vfs::mkdir(dir.c_str());
        String path = dir + "/" + name + ".bmp";
        if (save_bmp(art_, path.c_str())) {
            String msg = String("Saved ") + name + ".bmp to Pictures.";
            wm::notify("Paint", msg.c_str(), "paint");
        } else {
            wm::notify("Paint", "Could not save the image.", "paint");
        }
    }

    Canvas art_;
    Canvas* hist_[kUndoDepth] = {};
    int hist_n_ = 0;
    int tool_ = T_Pen;
    Color fg_ = hex(0x000000);
    bool drawing_ = false;
    int sx_ = 0, sy_ = 0, ex_ = 0, ey_ = 0;
    Vector<String> files_;
    uint64_t gen_ = 0;

    Dropdown* dd_ = nullptr;
    Button* open_btn_ = nullptr;
    TextField* name_ = nullptr;
    Button* save_ = nullptr;
    Button* undo_btn_ = nullptr;
    Button* clear_btn_ = nullptr;
    Slider* slider_ = nullptr;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0xFFFFFF), hex(0xD8DBE2));
    float u = r.w / 100.0f;
    auto X = [&](float v) { return r.x + v * u; };
    auto Y = [&](float v) { return r.y + v * u; };
    // rainbow brush stroke across the sheet
    Color cols[5] = {hex(0xFF3B30), hex(0xFF9500), hex(0xFFCC00), hex(0x34C759), hex(0x007AFF)};
    for (int i = 0; i < 5; i++) {
        float t0 = i / 5.0f, t1 = (i + 1) / 5.0f;
        float x0 = 20 + 60 * t0, x1 = 20 + 60 * t1;
        float y0 = 66 - 26 * sinf(t0 * M_PI_F), y1 = 66 - 26 * sinf(t1 * M_PI_F);
        c.draw_line(X(x0), Y(y0), X(x1), Y(y1), cols[i], 9 * u);
        c.fill_circle(X(x1), Y(y1), 4.5f * u, cols[i]);
    }
    // brush handle
    c.draw_line(X(70), Y(34), X(84), Y(18), hex(0x3A3A3C), 6 * u);
    c.fill_circle(X(84), Y(18), 3 * u, hex(0x3A3A3C));
}

Window* create() { return new PaintWindow(); }

AppRegistrar reg({"paint", "Paint", icon, create, "Creativity", true, true, 65, "Draw, sketch and save images"});

}  // namespace
