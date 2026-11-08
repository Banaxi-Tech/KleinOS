// Procedural wallpapers: soft colour fields made of large blurred blobs and
// gentle waves, computed at quarter resolution and upscaled with dithering.
#include "gfx/wallpaper.h"
#include "lib/math.h"
#include "lib/heap.h"
#include "fs/vfs.h"

namespace {

struct Blob { float x, y, r; Color c; };
struct Preset {
    const char* name;
    Color base_top, base_bottom;
    Blob blobs[6];
    int nblobs;
    float wave;        // strength of the light ribbon
    Color wave_color;
};

const Preset kPresets[] = {
    {"Aurora", hex(0x0B1033), hex(0x1C0B3A),
     {{0.15f, 0.25f, 0.55f, hex(0x2D5BFF)}, {0.85f, 0.2f, 0.5f, hex(0xB13CFF)}, {0.6f, 0.85f, 0.6f, hex(0x00C2D1)},
      {0.3f, 0.75f, 0.35f, hex(0xFF4FA3)}},
     4, 0.55f, hex(0xBFE9FF)},
    {"Sunrise", hex(0xFF9A5A), hex(0x6A2C91),
     {{0.2f, 0.2f, 0.6f, hex(0xFFD36E)}, {0.85f, 0.35f, 0.55f, hex(0xFF5E7E)}, {0.5f, 0.95f, 0.7f, hex(0x5B2A9B)},
      {0.1f, 0.8f, 0.4f, hex(0xFF7A3D)}},
     4, 0.35f, hex(0xFFF1D6)},
    {"Lagoon", hex(0x02364A), hex(0x032B3D),
     {{0.25f, 0.3f, 0.6f, hex(0x00B4A6)}, {0.8f, 0.25f, 0.5f, hex(0x2F80ED)}, {0.55f, 0.9f, 0.55f, hex(0x56F0C8)},
      {0.95f, 0.85f, 0.35f, hex(0x0057B8)}},
     4, 0.45f, hex(0xD9FFF6)},
    {"Bloom", hex(0xF6E7FF), hex(0xFFE3EC),
     {{0.2f, 0.3f, 0.45f, hex(0xFF8FB8)}, {0.75f, 0.25f, 0.5f, hex(0x8FB8FF)}, {0.5f, 0.8f, 0.55f, hex(0xFFD08F)},
      {0.9f, 0.9f, 0.4f, hex(0xB58FFF)}, {0.05f, 0.95f, 0.35f, hex(0x8FFFD8)}},
     5, 0.3f, hex(0xFFFFFF)},
    {"Graphite", hex(0x1A1B1F), hex(0x0C0C0E),
     {{0.2f, 0.2f, 0.6f, hex(0x3A3F4B)}, {0.85f, 0.8f, 0.6f, hex(0x2A2F3A)}, {0.7f, 0.25f, 0.35f, hex(0x4E5566)}},
     3, 0.25f, hex(0x9AA3B5)},
    {"Dunes", hex(0xF3C38B), hex(0x9C5B3B),
     {{0.25f, 0.35f, 0.6f, hex(0xFFE0B0)}, {0.8f, 0.6f, 0.55f, hex(0xD9824B)}, {0.4f, 0.95f, 0.5f, hex(0x7A3E2A)}},
     3, 0.4f, hex(0xFFF4E0)},
};

inline float fclamp(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

}  // namespace

// Wallpaper 0 is a photo from /System/Library/Wallpapers; the rest are procedural.
static const char* kPhoto = "/System/Library/Wallpapers/Durdle-Door.jpg";
static const int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

int wallpaper_count() { return kPresetCount + 1; }
const char* wallpaper_name(int i) {
    i = (i % wallpaper_count() + wallpaper_count()) % wallpaper_count();
    return i == 0 ? "Durdle Door" : kPresets[i - 1].name;
}

static bool render_photo(Canvas& c) {
    const uint8_t* data;
    size_t size;
    if (!vfs::read_file(kPhoto, &data, &size)) return false;
    Canvas* img = image_decode(data, size);
    if (!img) return false;
    // cover the screen, centered
    float sc = kmax((float)c.width() / img->width(), (float)c.height() / img->height());
    int w = (int)(img->width() * sc + 0.5f), h = (int)(img->height() * sc + 0.5f);
    c.clear(hex(0x000000));
    if (w == img->width() && h == img->height()) c.blit(*img, (c.width() - w) / 2, (c.height() - h) / 2);
    else c.draw_scaled(*img, Rect((c.width() - w) / 2, (c.height() - h) / 2, w, h));
    delete img;
    return true;
}

void wallpaper_render(Canvas& c, int index) {
    index = (index % wallpaper_count() + wallpaper_count()) % wallpaper_count();
    if (index == 0 && render_photo(c)) return;
    if (index == 0) index = 1;
    const Preset& p = kPresets[index - 1];
    int W = c.width(), H = c.height();
    int qw = W / 4 + 2, qh = H / 4 + 2;
    float* buf = (float*)kmalloc((size_t)qw * qh * 3 * sizeof(float));
    float aspect = (float)W / H;
    for (int y = 0; y < qh; y++) {
        float v = (float)y / (qh - 1);
        for (int x = 0; x < qw; x++) {
            float u = (float)x / (qw - 1);
            float r = color_r(p.base_top) + (color_r(p.base_bottom) - color_r(p.base_top)) * v;
            float g = color_g(p.base_top) + (color_g(p.base_bottom) - color_g(p.base_top)) * v;
            float b = color_b(p.base_top) + (color_b(p.base_bottom) - color_b(p.base_top)) * v;
            for (int i = 0; i < p.nblobs; i++) {
                const Blob& bl = p.blobs[i];
                float dx = (u - bl.x) * aspect, dy = v - bl.y;
                float d2 = (dx * dx + dy * dy) / (bl.r * bl.r);
                float w = 1.0f / (1.0f + d2 * 2.2f);
                w = w * w;
                r += (color_r(bl.c) - r) * w;
                g += (color_g(bl.c) - g) * w;
                b += (color_b(bl.c) - b) * w;
            }
            // flowing light ribbon
            float wy = 0.55f + 0.12f * sinf(u * 5.0f + 0.6f) + 0.05f * sinf(u * 11.0f);
            float dd = (v - wy) / 0.09f;
            float wv = p.wave * expf(-dd * dd);
            r += (color_r(p.wave_color) - r) * wv * 0.6f;
            g += (color_g(p.wave_color) - g) * wv * 0.6f;
            b += (color_b(p.wave_color) - b) * wv * 0.6f;
            float* o = buf + (y * qw + x) * 3;
            o[0] = r; o[1] = g; o[2] = b;
        }
    }
    // upscale with bilinear + ordered dither
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int y = 0; y < H; y++) {
        uint32_t* row = c.row(y);
        float fy = y / 4.0f;
        int y0 = (int)fy;
        float ty = fy - y0;
        if (y0 >= qh - 1) { y0 = qh - 2; ty = 1; }
        for (int x = 0; x < W; x++) {
            float fx = x / 4.0f;
            int x0 = (int)fx;
            float tx = fx - x0;
            if (x0 >= qw - 1) { x0 = qw - 2; tx = 1; }
            const float* a = buf + (y0 * qw + x0) * 3;
            const float* b = a + 3;
            const float* cc = a + qw * 3;
            const float* d = cc + 3;
            float dither = (bayer[y & 3][x & 3] - 7.5f) / 16.0f;
            int out[3];
            for (int k = 0; k < 3; k++) {
                float top = a[k] + (b[k] - a[k]) * tx, bot = cc[k] + (d[k] - cc[k]) * tx;
                float v = top + (bot - top) * ty + dither;
                out[k] = (int)(fclamp(v / 255.0f) * 255.0f + 0.5f);
            }
            row[x] = 0xFF000000u | (out[0] << 16) | (out[1] << 8) | out[2];
        }
    }
    kfree(buf);
}
