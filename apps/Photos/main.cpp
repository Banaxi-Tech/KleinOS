// Photos: browse images in ~/Pictures (and subfolders), view them fit-to-window
// with zoom and arrow-key navigation. First run creates an 8-piece gallery.
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

constexpr int kHeaderH = 52;
constexpr int kCellW = 200, kCellH = 180;
constexpr int kThumbW = 168, kThumbH = 112;
constexpr int kArtW = 480, kArtH = 320;
constexpr int kGalleryCount = 8;
constexpr float kMaxZoom = 8.0f;

String pictures_dir() { return String("/Users/") + session_user() + "/Pictures"; }
String gallery_dir() { return pictures_dir() + "/Gallery"; }

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

// ---------------------------------------------------------------- artwork
struct Rng {
    uint32_t s;
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float f() { return (next() & 0xFFFFFF) / 16777216.0f; }
};

struct RGBf { float r, g, b; };

RGBf rgb_of(uint32_t hexv) { return RGBf{(float)((hexv >> 16) & 0xFF), (float)((hexv >> 8) & 0xFF), (float)(hexv & 0xFF)}; }

RGBf mix3(RGBf a, RGBf b, float t) {
    return RGBf{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// Sample a multi-stop palette at t in [0,1].
RGBf palette(const uint32_t* stops, int n, float t) {
    t = kclamp(t, 0.0f, 1.0f) * (n - 1);
    int i = (int)t;
    if (i >= n - 1) return rgb_of(stops[n - 1]);
    return mix3(rgb_of(stops[i]), rgb_of(stops[i + 1]), t - i);
}

void hsv(float h, float s, float v, RGBf& out) {
    h = h - floorf(h);
    float hh = h * 6.0f;
    int i = (int)hh;
    float f = hh - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r, g, b;
    switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    out = RGBf{r * 255, g * 255, b * 255};
}

uint32_t pack(RGBf c) {
    int r = kclamp((int)c.r, 0, 255), g = kclamp((int)c.g, 0, 255), b = kclamp((int)c.b, 0, 255);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void gen_gradient(Canvas& c) {
    const uint32_t stops[4] = {0x2B1B7A, 0xFF5E8A, 0xFFB35C, 0xFFF1D6};
    for (int y = 0; y < kArtH; y++) {
        uint32_t* row = c.row(y);
        float v = y / (float)(kArtH - 1);
        for (int x = 0; x < kArtW; x++) {
            float u = x / (float)(kArtW - 1);
            float t = 0.6f * u + 0.4f * v;
            RGBf col = palette(stops, 4, t);
            float dx = u - 0.72f, dy = v - 0.28f;
            float glow = 1.0f / (1.0f + 16.0f * (dx * dx + dy * dy));
            col = mix3(col, RGBf{255, 246, 225}, glow * 0.6f);
            row[x] = pack(col);
        }
    }
}

void gen_plasma(Canvas& c) {
    const uint32_t stops[5] = {0x0B1D51, 0x2A6FDB, 0x7EE0C4, 0xFFD166, 0xEF476F};
    for (int y = 0; y < kArtH; y++) {
        uint32_t* row = c.row(y);
        for (int x = 0; x < kArtW; x++) {
            float dx = x - 240.0f, dy = y - 160.0f;
            float v = sinf(x * 0.021f) + sinf(y * 0.027f) + sinf((x + y) * 0.017f) + sinf(sqrtf(dx * dx + dy * dy) * 0.024f);
            float t = 0.5f + 0.5f * sinf(v * 0.8f);
            row[x] = pack(palette(stops, 5, t));
        }
    }
}

void gen_mandelbrot(Canvas& c) {
    const int max_iter = 120;
    for (int y = 0; y < kArtH; y++) {
        uint32_t* row = c.row(y);
        float ci = -1.0f + y * (2.0f / kArtH);
        for (int x = 0; x < kArtW; x++) {
            float cr = -2.1f + x * (2.8f / kArtW);
            float zr = 0, zi = 0;
            int n = 0;
            while (n < max_iter && zr * zr + zi * zi < 4.0f) {
                float t = zr * zr - zi * zi + cr;
                zi = 2.0f * zr * zi + ci;
                zr = t;
                n++;
            }
            if (n == max_iter) { row[x] = 0xFF08061A; continue; }
            float t = n / (float)max_iter;
            RGBf col{9 * (1 - t) * t * t * t * 255, 15 * (1 - t) * (1 - t) * t * t * 255, 8.5f * (1 - t) * (1 - t) * (1 - t) * t * 255 + 40 * t};
            row[x] = pack(col);
        }
    }
}

void gen_circles(Canvas& c) {
    const uint32_t stops[4] = {0x0F3D3E, 0x2A9D8F, 0xF6C177, 0xF28A80};
    const float cx[3] = {120, 360, 240}, cy[3] = {100, 220, 160};
    for (int y = 0; y < kArtH; y++) {
        uint32_t* row = c.row(y);
        for (int x = 0; x < kArtW; x++) {
            float v = 0;
            v += sinf(sqrtf((x - cx[0]) * (x - cx[0]) + (y - cy[0]) * (y - cy[0])) * 0.09f);
            v += sinf(sqrtf((x - cx[1]) * (x - cx[1]) + (y - cy[1]) * (y - cy[1])) * 0.11f);
            v += sinf(sqrtf((x - cx[2]) * (x - cx[2]) + (y - cy[2]) * (y - cy[2])) * 0.07f);
            row[x] = pack(palette(stops, 4, (v + 3.0f) / 6.0f));
        }
    }
}

void gen_waves(Canvas& c) {
    const int layers = 6;
    const uint32_t layer_col[layers] = {0xA7E8E0, 0x5CC8C2, 0x2A9DA5, 0x1B6E82, 0x12465E, 0x0B2A3F};
    static float tops[layers][kArtW];
    for (int k = 0; k < layers; k++)
        for (int x = 0; x < kArtW; x++)
            tops[k][x] = kArtH * (0.30f + 0.11f * k) + 18 * sinf(x * 0.021f * (1 + 0.25f * k) + k * 1.7f) +
                         6 * sinf(x * 0.057f + k * 0.9f);
    const uint32_t sky[2] = {0x3B2F80, 0xF28B82};
    for (int y = 0; y < kArtH; y++) {
        uint32_t* row = c.row(y);
        RGBf skyc = palette(sky, 2, y / (float)kArtH);
        for (int x = 0; x < kArtW; x++) {
            RGBf col = skyc;
            float dx = x - 340.0f, dy = y - 90.0f;
            float sun = sqrtf(dx * dx + dy * dy);
            if (sun < 46) col = mix3(col, RGBf{255, 233, 176}, 1.0f - sun / 46 * 0.25f);
            else if (sun < 120) col = mix3(col, RGBf{255, 200, 150}, (1 - (sun - 46) / 74) * 0.25f);
            for (int k = 0; k < layers; k++)
                if (y >= tops[k][x]) col = rgb_of(layer_col[k]);
            row[x] = pack(col);
        }
    }
}

void gen_voronoi(Canvas& c) {
    const int n = 26;
    float sx[n], sy[n];
    RGBf sc[n];
    Rng rng{12345};
    for (int i = 0; i < n; i++) {
        sx[i] = rng.f() * kArtW;
        sy[i] = rng.f() * kArtH;
        hsv(i / (float)n + 0.1f * rng.f(), 0.5f + 0.4f * rng.f(), 0.95f, sc[i]);
    }
    for (int y = 0; y < kArtH; y++) {
        uint32_t* row = c.row(y);
        for (int x = 0; x < kArtW; x++) {
            float d1 = 1e9f, d2 = 1e9f;
            int best = 0;
            for (int i = 0; i < n; i++) {
                float dx = x - sx[i], dy = y - sy[i];
                float d = dx * dx + dy * dy;
                if (d < d1) { d2 = d1; d1 = d; best = i; }
                else if (d < d2) d2 = d;
            }
            float edge = sqrtf(d2) - sqrtf(d1);
            RGBf col = sc[best];
            if (edge < 2.5f) col = mix3(col, RGBf{20, 20, 30}, 0.85f);
            row[x] = pack(col);
        }
    }
}

void gen_stripes(Canvas& c) {
    for (int y = 0; y < kArtH; y++) {
        uint32_t* row = c.row(y);
        for (int x = 0; x < kArtW; x++) {
            float s = (x * 0.8f + y * 0.6f) * 0.035f;
            float band = floorf(s);
            float f = s - band;
            RGBf col;
            hsv(band * 0.137f + y * 0.00035f, 0.72f, 0.96f, col);
            if (f < 0.07f) col = mix3(col, RGBf{15, 12, 30}, 0.6f);
            row[x] = pack(col);
        }
    }
}

void add_light(Canvas& c, int x, int y, float r, float g, float b) {
    if (x < 0 || y < 0 || x >= kArtW || y >= kArtH) return;
    uint32_t p = c.row(y)[x];
    float R = ((p >> 16) & 0xFF) + r, G = ((p >> 8) & 0xFF) + g, B = (p & 0xFF) + b;
    c.row(y)[x] = pack(RGBf{R, G, B});
}

void gen_starfield(Canvas& c) {
    const uint32_t top = 0x02030A, bottom = 0x0B1638;
    for (int y = 0; y < kArtH; y++) {
        uint32_t* row = c.row(y);
        RGBf col = mix3(rgb_of(top), rgb_of(bottom), y / (float)kArtH);
        for (int x = 0; x < kArtW; x++) row[x] = pack(col);
    }
    Rng rng{777};
    for (int i = 0; i < 420; i++) {
        int sx = (int)(rng.f() * kArtW), sy = (int)(rng.f() * kArtH);
        float f = rng.f();
        float b = 0.25f + 0.75f * f * f * f;
        float tint = rng.f();
        float r = 255 * b, g = (tint < 0.7f ? 255 : 210) * b, bl = 255 * b;
        if (tint > 0.9f) { r = 255 * b; g = 170 * b; bl = 120 * b; }
        int radius = b > 0.8f ? 3 : 1;
        for (int dy = -radius; dy <= radius; dy++)
            for (int dx = -radius; dx <= radius; dx++) {
                float d2 = (float)(dx * dx + dy * dy);
                float k = dx == 0 && dy == 0 ? 1.0f : 0.35f / (1.0f + d2 * 1.5f);
                add_light(c, sx + dx, sy + dy, r * k, g * k, bl * k);
            }
    }
}

struct Artwork {
    const char* name;
    void (*paint)(Canvas&);
};

const Artwork kArtworks[kGalleryCount] = {
    {"Gradient", gen_gradient},   {"Plasma", gen_plasma},     {"Mandelbrot", gen_mandelbrot}, {"Circles", gen_circles},
    {"Waves", gen_waves},         {"Voronoi", gen_voronoi},   {"Stripes", gen_stripes},       {"Starfield", gen_starfield},
};

// ---------------------------------------------------------------- library
Canvas* make_thumb(const Canvas& src, int tw, int th) {
    Canvas* t = new Canvas(tw, th);
    t->clear(rgba(0, 0, 0, 0));
    float s = kmin(tw / (float)src.width(), th / (float)src.height());
    int dw = kmax(1, (int)(src.width() * s)), dh = kmax(1, (int)(src.height() * s));
    t->draw_scaled(src, Rect((tw - dw) / 2, (th - dh) / 2, dw, dh));
    return t;
}

struct Entry {
    String path;
    String name;
    Canvas* thumb = nullptr;
    bool failed = false;
    ~Entry() { delete thumb; }
};

void collect(vfs::Node* dir, Vector<String>& out) {
    for (vfs::Node* ch : dir->children) {
        if (!ch) continue;
        if (ch->is_dir) { collect(ch, out); continue; }
        String ext = vfs::extension(ch->name.c_str());
        if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp") out.push(vfs::path_of(ch));
    }
}

class PhotosWindow : public Window {
public:
    PhotosWindow() : Window("Photos", 960, 640, WF_RESIZABLE) {
        set_min_size(420, 300);
        set_ticking(true);
        gen_ = vfs::generation();
        start_gallery();
        rescan();
    }

    ~PhotosWindow() {
        for (Entry* e : entries_) delete e;
        delete full_;
    }

    void on_paint(Canvas& c) override {
        if (view_ == Viewer) paint_viewer(c);
        else paint_grid(c);
    }

    void on_mouse_down(int x, int y, int button) override {
        if (button != MB_LEFT) return;
        if (view_ == Grid) {
            int i = cell_at(x, y);
            if (i >= 0) open_entry(i);
            return;
        }
        if (back_rect().contains(x, y)) { show_grid(); return; }
        if (left_rect().contains(x, y)) { step(-1); return; }
        if (right_rect().contains(x, y)) { step(1); return; }
        if (zoom_ > 1.0f) { panning_ = true; drag_x_ = x; drag_y_ = y; }
    }

    void on_mouse_move(int x, int y, int buttons) override {
        if (view_ == Grid) {
            int i = cell_at(x, y);
            if (i != hover_) { hover_ = i; invalidate(); }
            return;
        }
        if (panning_) {
            if (!(buttons & MB_LEFT)) { panning_ = false; return; }
            pan_x_ += x - drag_x_;
            pan_y_ += y - drag_y_;
            drag_x_ = x;
            drag_y_ = y;
            invalidate();
        }
    }

    void on_mouse_up(int, int, int) override { panning_ = false; }

    void on_scroll(int, int, int delta) override {
        if (view_ == Grid) {
            scroll_ += delta * 48;
            clamp_scroll();
            invalidate();
            return;
        }
        float nz = delta > 0 ? zoom_ / 1.25f : zoom_ * 1.25f;
        zoom_ = kclamp(nz, 1.0f, kMaxZoom);
        if (zoom_ <= 1.0001f) { pan_x_ = pan_y_ = 0; zoom_ = 1; }
        invalidate();
    }

    void on_key(const KeyEvent& e) override {
        if (e.key == Key::Escape && view_ == Viewer) { show_grid(); return; }
        if (view_ == Viewer && e.key == Key::Left) { step(-1); return; }
        if (view_ == Viewer && e.key == Key::Right) { step(1); return; }
        Window::on_key(e);
    }

    void on_tick(uint64_t) override {
        uint64_t g = vfs::generation();
        if (g != gen_) { gen_ = g; rescan(); }
        if (generating_) { generate_next(); return; }
        // thumbnails: one per tick
        for (Entry* e : entries_) {
            if (e->thumb || e->failed) continue;
            const uint8_t* data = nullptr;
            size_t size = 0;
            Canvas* img = nullptr;
            if (vfs::read_file(e->path.c_str(), &data, &size)) img = image_decode(data, size);
            if (img) {
                e->thumb = make_thumb(*img, kThumbW, kThumbH);
                delete img;
            } else {
                e->failed = true;
            }
            invalidate();
            break;
        }
    }

    // Called when another app or Finder asks us to show a file.
    void open_external(const char* path) {
        if (index_of(String(path)) < 0) {
            Entry* e = new Entry();
            e->path = path;
            String base = vfs::basename(path);
            long dot = base.rfind('.');
            e->name = dot > 0 ? base.substr(0, dot) : base;
            entries_.insert(0, e);
            clamp_scroll();
        }
        show_viewer(String(path));
    }

private:
    enum ViewMode { Grid, Viewer };

    int columns() const { return kmax(1, (width() - 24) / kCellW); }
    int rows() const { int n = (int)entries_.size(); int c = columns(); return (n + c - 1) / c; }
    int content_h() const { return height() - kHeaderH; }

    void clamp_scroll() {
        int max = kmax(0, rows() * kCellH + 12 - content_h());
        scroll_ = kclamp(scroll_, 0, max);
    }

    Rect cell_rect(int i) const {
        int c = columns();
        int left = (width() - c * kCellW) / 2;
        return Rect(left + (i % c) * kCellW, kHeaderH + 6 + (i / c) * kCellH - scroll_, kCellW, kCellH);
    }

    int cell_at(int x, int y) const {
        if (y < kHeaderH) return -1;
        for (int i = 0; i < (int)entries_.size(); i++) {
            Rect r = cell_rect(i);
            if (r.contains(x, y)) return i;
        }
        return -1;
    }

    Rect back_rect() const { return Rect(14, 14, 92, 30); }
    Rect left_rect() const { return Rect(14, height() / 2 - 26, 52, 52); }
    Rect right_rect() const { return Rect(width() - 66, height() / 2 - 26, 52, 52); }

    void paint_grid(Canvas& c) {
        const Theme& t = theme();
        c.clear(t.window_bg);
        c.fill_rect(Rect(0, kHeaderH - 1, width(), 1), t.separator);
        c.draw_text(20, 16, "Library", t.text, 17, Font::Bold);
        char cnt[48];
        snprintf(cnt, sizeof(cnt), "%d %s", (int)entries_.size(), entries_.size() == 1 ? "photo" : "photos");
        c.draw_text_in(Rect(width() - 240, 16, 220, 22), cnt, t.text_secondary, 13, Font::Regular, Align::Right);

        c.save();
        c.clip_to(Rect(0, kHeaderH, width(), content_h()));
        if (entries_.empty()) {
            c.draw_text_in(Rect(0, kHeaderH + content_h() / 2 - 20, width(), 40),
                           generating_ ? "Creating your gallery..." : "No photos yet. Add images to Pictures.",
                           t.text_secondary, 15, Font::Medium, Align::Center);
        }
        for (int i = 0; i < (int)entries_.size(); i++) {
            Rect cell = cell_rect(i);
            if (cell.bottom() < kHeaderH || cell.y > height()) continue;
            Rect card(cell.x + 12, cell.y + 10, kCellW - 24, kCellH - 20);
            bool hov = i == hover_;
            c.draw_shadow(card, 12, 6, rgba(0, 0, 0, t.dark ? 90 : 35), 0, 2);
            c.fill_round_rect(card, 12, t.dark ? hex(0x2C2C2E) : hex(0xFFFFFF));
            Rect thumb_box(card.x + (card.w - kThumbW) / 2, card.y + 4, kThumbW, kThumbH);
            if (entries_[i]->thumb) {
                c.draw(*entries_[i]->thumb, thumb_box.x, thumb_box.y);
            } else {
                c.fill_round_rect(thumb_box, 8, t.dark ? hex(0x3A3A3C) : hex(0xE5E5EA));
                c.draw_text_in(thumb_box, entries_[i]->failed ? "Unavailable" : "Loading", t.text_tertiary, 12,
                               Font::Regular, Align::Center);
            }
            c.draw_text_in(Rect(card.x + 8, card.y + card.h - 26, card.w - 16, 20), entries_[i]->name.c_str(), t.text,
                           12, hov ? Font::Medium : Font::Regular, Align::Center);
            if (hov) c.stroke_round_rect(card, 12, t.accent, 2);
        }
        c.restore();
    }

    void paint_viewer(Canvas& c) {
        int w = width(), h = height();
        c.clear(rgb(14, 14, 16));
        if (!full_) {
            c.draw_text_in(Rect(0, h / 2 - 12, w, 24), "This image could not be opened.", rgb(200, 200, 205), 15,
                           Font::Medium, Align::Center);
        } else {
            float fw = (float)full_->width(), fh = (float)full_->height();
            float s = kmin(w / fw, h / fh);
            float dw = fw * s * zoom_, dh = fh * s * zoom_;
            float maxx = kmax(0.0f, (dw - w) / 2), maxy = kmax(0.0f, (dh - h) / 2);
            pan_x_ = kclamp(pan_x_, -(int)maxx, (int)maxx);
            pan_y_ = kclamp(pan_y_, -(int)maxy, (int)maxy);
            int x = (int)((w - dw) / 2) + pan_x_, y = (int)((h - dh) / 2) + pan_y_;
            c.save();
            c.clip_to(Rect(0, 0, w, h));
            c.draw_scaled(*full_, Rect(x, y, (int)dw, (int)dh));
            c.restore();
        }

        // bottom caption bar
        c.fill_gradient(Rect(0, h - 52, w, 52), rgba(0, 0, 0, 0), rgba(0, 0, 0, 170));
        int idx = index_of(viewer_path_);
        char cap[160];
        snprintf(cap, sizeof(cap), "%s    %d of %d", viewer_name().c_str(), idx + 1, (int)entries_.size());
        c.draw_text_in(Rect(20, h - 34, w - 160, 22), cap, rgb(235, 235, 240), 13, Font::Medium);
        char zoom[16];
        snprintf(zoom, sizeof(zoom), "%d%%", (int)(zoom_ * 100 + 0.5f));
        c.draw_text_in(Rect(w - 140, h - 34, 120, 22), zoom, rgb(200, 200, 205), 13, Font::Regular, Align::Right);

        // back pill
        Rect b = back_rect();
        c.fill_round_rect(b, b.h / 2.0f, rgba(255, 255, 255, 40));
        c.stroke_round_rect(b, b.h / 2.0f, rgba(255, 255, 255, 60), 1);
        draw_symbol(c, Rect(b.x + 12, b.y + 8, 12, 14), Symbol::ChevronLeft, hex(0xFFFFFF), 2.0f);
        c.draw_text(b.x + 30, b.y + 6, "Back", hex(0xFFFFFF), 13, Font::Medium);

        // chevrons
        draw_chevron(c, left_rect(), Symbol::ChevronLeft, idx > 0);
        draw_chevron(c, right_rect(), Symbol::ChevronRight, idx < (int)entries_.size() - 1);
    }

    void draw_chevron(Canvas& c, Rect r, Symbol s, bool enabled) {
        c.fill_circle(r.x + r.w / 2.0f, r.y + r.h / 2.0f, r.w / 2.0f, rgba(0, 0, 0, enabled ? 120 : 50));
        c.stroke_circle(r.x + r.w / 2.0f, r.y + r.h / 2.0f, r.w / 2.0f - 0.5f, rgba(255, 255, 255, enabled ? 60 : 20), 1);
        draw_symbol(c, r.inset(r.w / 3), s, rgba(255, 255, 255, enabled ? 255 : 90), 2.2f);
    }

    int index_of(const String& path) const {
        for (int i = 0; i < (int)entries_.size(); i++)
            if (entries_[i]->path == path) return i;
        return -1;
    }

    String viewer_name() const {
        int i = index_of(viewer_path_);
        if (i >= 0) return entries_[i]->name;
        return vfs::basename(viewer_path_.c_str());
    }

    void open_entry(int i) {
        if (i < 0 || i >= (int)entries_.size()) return;
        show_viewer(entries_[i]->path);
    }

    void show_viewer(const String& path) {
        viewer_path_ = path;
        view_ = Viewer;
        zoom_ = 1;
        pan_x_ = pan_y_ = 0;
        panning_ = false;
        delete full_;
        full_ = nullptr;
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (vfs::read_file(path.c_str(), &data, &size)) full_ = image_decode(data, size);
        invalidate();
    }

    void show_grid() {
        view_ = Grid;
        panning_ = false;
        delete full_;
        full_ = nullptr;
        invalidate();
    }

    void step(int dir) {
        int i = index_of(viewer_path_);
        int n = (int)entries_.size();
        if (n == 0) return;
        int j = i < 0 ? (dir > 0 ? 0 : n - 1) : i + dir;
        if (j < 0 || j >= n) return;
        show_viewer(entries_[j]->path);
    }

    void rescan() {
        Vector<String> paths;
        vfs::Node* dir = vfs::lookup(pictures_dir().c_str());
        if (dir && dir->is_dir) collect(dir, paths);
        for (size_t i = 1; i < paths.size(); i++)
            for (size_t j = i; j > 0 && paths[j] < paths[j - 1]; j--) {
                String tmp = paths[j]; paths[j] = paths[j - 1]; paths[j - 1] = tmp;
            }
        // keep existing entries (and their thumbnails) for paths that remain
        Vector<Entry*> next;
        for (size_t i = 0; i < paths.size(); i++) {
            Entry* e = nullptr;
            for (size_t k = 0; k < entries_.size(); k++) {
                if (entries_[k] && entries_[k]->path == paths[i]) {
                    e = entries_[k];
                    entries_[k] = nullptr;
                    break;
                }
            }
            if (!e) {
                e = new Entry();
                e->path = paths[i];
                String base = vfs::basename(paths[i].c_str());
                long dot = base.rfind('.');
                e->name = dot > 0 ? base.substr(0, dot) : base;
            }
            next.push(e);
        }
        for (Entry* e : entries_) delete e;
        entries_ = next;
        clamp_scroll();
        invalidate();
    }

    void start_gallery() {
        String marker = gallery_dir() + "/.generated";
        if (vfs::exists(marker.c_str())) return;
        vfs::mkdir(gallery_dir().c_str());
        generating_ = true;
        gallery_step_ = 0;
    }

    void generate_next() {
        if (gallery_step_ >= kGalleryCount) {
            generating_ = false;
            String marker = gallery_dir() + "/.generated";
            vfs::write_text(marker.c_str(), "1");
            rescan();
            return;
        }
        const Artwork& a = kArtworks[gallery_step_++];
        Canvas* art = new Canvas(kArtW, kArtH);
        a.paint(*art);
        String path = gallery_dir() + "/" + a.name + ".bmp";
        save_bmp(*art, path.c_str());
        delete art;
        invalidate();
    }

    Vector<Entry*> entries_;
    uint64_t gen_ = 0;
    ViewMode view_ = Grid;
    int scroll_ = 0;
    int hover_ = -1;
    bool generating_ = false;
    int gallery_step_ = 0;

    // viewer state
    String viewer_path_;
    Canvas* full_ = nullptr;
    float zoom_ = 1.0f;
    int pan_x_ = 0, pan_y_ = 0;
    bool panning_ = false;
    int drag_x_ = 0, drag_y_ = 0;
};

// Opened from elsewhere (Finder, Spotlight, other apps) through app_open_file.
void open_image_path(const char* path) {
    Window* w = app_launch("photos");
    if (!w) return;
    PhotosWindow* p = static_cast<PhotosWindow*>(w);
    p->open_external(path);
}

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x5AC8FA), hex(0x0A5BD3));
    float u = r.w / 100.0f;
    // framed picture: sky, sun, two mountains
    Rect frame(r.x + 20 * u, r.y + 26 * u, 60 * u, 48 * u);
    c.fill_round_rect(frame, 6 * u, hex(0xFFFFFF));
    Rect inner(frame.x + 4 * u, frame.y + 4 * u, frame.w - 8 * u, frame.h - 8 * u);
    c.fill_gradient(inner, hex(0xFFE08A), hex(0xFF9F6E), 3 * u);
    c.fill_circle(inner.x + inner.w * 0.72f, inner.y + inner.h * 0.34f, 5 * u, hex(0xFFF6D5));
    float ix = (float)inner.x, iy = (float)inner.y, iw = (float)inner.w, ih = (float)inner.h;
    float ib = iy + ih;
    PointF back[3] = {{ix, ib}, {ix + iw * 0.42f, iy + ih * 0.28f}, {ix + iw, ib}};
    c.fill_polygon(back, 3, hex(0x3D6FD6));
    PointF front[3] = {{ix + iw * 0.22f, ib}, {ix + iw * 0.6f, iy + ih * 0.5f}, {ix + iw, ib}};
    c.fill_polygon(front, 3, hex(0x1B3F8B));
}

Window* create() { return new PhotosWindow(); }

struct OpenRegistration {
    OpenRegistration() { register_open_handler("photos", open_image_path); }
} open_reg;

AppRegistrar reg({"photos", "Photos", icon, create, "Media", true, true, 45, "Browse and view your pictures"});

}  // namespace
