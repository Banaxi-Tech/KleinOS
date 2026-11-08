#include "gfx/glass.h"
#include "lib/heap.h"
#include "lib/math.h"
#include "lib/string.h"

namespace {

// Per-pixel lens data for one shape.
struct LensPx {
    int16_t dx, dy;    // sample offset in 1/16 px (already scaled by reach)
    uint8_t light;     // light caught, 0..255
    uint8_t mask;      // shape coverage, 0..255
};

struct LensMap {
    int w, h;
    float radius, depth, dome, strength, angle;
    bool relative;
    LensPx* px;
    uint64_t last_use;
};

constexpr int kMaxMaps = 24;
LensMap g_maps[kMaxMaps];
uint64_t g_use_counter;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float erf_approx(float x) { return tanhf(1.7724538509f * x); }

// Signed distance to a rounded rectangle centred on the origin; negative inside.
inline float sdf(float x, float y, float hw, float hh, float r) {
    float qx = fabsf(x) - hw + r, qy = fabsf(y) - hh + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    if (in > 0) in = 0;
    return sqrtf(ox * ox + oy * oy) + in - r;
}

struct Arc { float radius, scale; };

// The circular arc that sags by `sag` across `half`, scaled so its slope averages 1/2.
Arc make_arc(float sag, float half) {
    float radius = (half * half + sag * sag) / (2 * sag);
    float sum = 0;
    for (int i = 0; i <= 100; i++) {
        float x = (i / 100.0f) * half;
        float s = x / sqrtf(radius * radius - x * x + 1e-6f);
        sum += (i == 0 || i == 100) ? s / 2 : s;
    }
    float mean = sum / 100;
    return Arc{radius, mean > 0 ? 0.5f / mean : 1};
}

inline float arc_slope(float x, const Arc& a) {
    float in = x < 0.999f * a.radius ? x : 0.999f * a.radius;
    return in / sqrtf(a.radius * a.radius - in * in) * a.scale;
}

void build_map(LensMap& m) {
    int w = m.w, h = m.h;
    float hw = w / 2.0f, hh = h / 2.0f;
    float radius = clampf(m.radius, 0, kmin(hw, hh));
    float share = m.relative ? kmin((float)w, (float)h) : 1.0f;
    float depth = m.depth * share, dome = m.dome * share;
    float reach = m.strength * share;
    float inner_w = kmax(0.0f, hw - depth), inner_h = kmax(0.0f, hh - depth);
    float inner_r = clampf(radius, 0, kmin(inner_w, inner_h));
    float ramp = depth > 0 ? 1.0f / (depth * 1.41421356f) : 1e6f;
    float sag = kmax(0.01f, kmin(dome, kmin(hw, hh) - 1));
    Arc across = make_arc(sag, hw), down = make_arc(sag, hh);
    float ca = cosf(m.angle * (float)M_PI / 180), sa = sinf(m.angle * (float)M_PI / 180);
    const float glow_k = 0.4f, spread = 0.5f, glow_pow = 1.5f, edge_k = 0.5f, edge_w = 1.6f;
    float glow_start = (1 - spread) * 1.41421356f, glow_range = spread * 1.41421356f;

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            LensPx& p = m.px[row * w + col];
            float x = col + 0.5f - hw, y = row + 0.5f - hh;
            float edge = sdf(x, y, hw, hh, radius);
            float inside = clampf(-edge, 0, 1);
            p.mask = (uint8_t)(inside * 255 + 0.5f);
            if (edge >= 0) { p.dx = p.dy = 0; p.light = 0; continue; }
            float ax = fabsf(x), ay = fabsf(y);
            float dx = (x < 0 ? -1 : 1) * arc_slope(ax, across);
            float dy = (y < 0 ? -1 : 1) * arc_slope(ay, down);
            float falloff = 0.5f * (1 + erf_approx(sdf(x, y, inner_w, inner_h, inner_r) * ramp));
            // pull inward: sample from further in
            float ox = -dx * falloff * reach, oy = -dy * falloff * reach;
            p.dx = (int16_t)clampf(ox * 16, -32000, 32000);
            p.dy = (int16_t)clampf(oy * 16, -32000, 32000);
            float facing = fabsf(clampf(x / hw, -1, 1) * ca + clampf(y / hh, -1, 1) * sa);
            float g = glow_range > 0.001f ? glow_k * powf(clampf((facing - glow_start) / glow_range, 0, 1), glow_pow) * falloff : 0;
            float rim = edge_k * kmax(0.0f, 1 + edge / edge_w) * powf(facing, 1.5f);
            float amount = kmin(1.0f, g + rim);
            p.light = (uint8_t)(amount * 255);
        }
    }
}

LensMap& get_map(int w, int h, const GlassStyle& s) {
    g_use_counter++;
    for (LensMap& m : g_maps)
        if (m.px && m.w == w && m.h == h && m.radius == s.radius && m.depth == s.depth && m.dome == s.dome &&
            m.strength == s.strength && m.relative == s.relative && m.angle == s.light_angle) {
            m.last_use = g_use_counter;
            return m;
        }
    // evict LRU
    LensMap* victim = &g_maps[0];
    for (LensMap& m : g_maps) {
        if (!m.px) { victim = &m; break; }
        if (m.last_use < victim->last_use) victim = &m;
    }
    if (victim->px) kfree(victim->px);
    victim->w = w; victim->h = h;
    victim->radius = s.radius; victim->depth = s.depth; victim->dome = s.dome;
    victim->strength = s.strength; victim->relative = s.relative; victim->angle = s.light_angle;
    victim->px = (LensPx*)kmalloc((size_t)w * h * sizeof(LensPx));
    victim->last_use = g_use_counter;
    build_map(*victim);
    return *victim;
}

// Scratch backdrop buffer (reused)
bool g_lite;
uint32_t* g_back;
size_t g_back_cap;

inline uint32_t sample(const uint32_t* buf, int bw, int bh, int fx16, int fy16) {
    // bilinear in 1/16 px fixed point
    int x0 = fx16 >> 4, y0 = fy16 >> 4;
    int tx = fx16 & 15, ty = fy16 & 15;
    if (x0 < 0) { x0 = 0; tx = 0; }
    if (y0 < 0) { y0 = 0; ty = 0; }
    if (x0 >= bw - 1) { x0 = bw - 1; tx = 0; }
    if (y0 >= bh - 1) { y0 = bh - 1; ty = 0; }
    int x1 = x0 + (x0 < bw - 1), y1 = y0 + (y0 < bh - 1);
    uint32_t a = buf[y0 * bw + x0], b = buf[y0 * bw + x1], c = buf[y1 * bw + x0], d = buf[y1 * bw + x1];
    if (!tx && !ty) return a;
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF, cc = (c >> sh) & 0xFF, cd = (d >> sh) & 0xFF;
        int top = ca * (16 - tx) + cb * tx, bot = cc * (16 - tx) + cd * tx;
        out |= (uint32_t)((top * (16 - ty) + bot * ty) >> 8) << sh;
    }
    return out;
}

}  // namespace

GlassStyle glass_style_panel(bool dark) {
    GlassStyle s;
    s.radius = 22;
    s.blur = 10;
    s.depth = 0.28f;
    s.dome = 0.5f;
    s.strength = 0.40f;
    s.dispersion = 0.5f;
    s.saturation = 1.6f;
    s.tint = dark ? rgba(30, 30, 36, 110) : rgba(255, 255, 255, 70);
    s.shine = dark ? 0.35f : 0.6f;
    return s;
}

GlassStyle glass_style_bar(bool dark) {
    GlassStyle s;
    s.radius = 0;
    s.blur = 12;
    s.relative = false;
    s.depth = 6;
    s.dome = 4;
    s.strength = 5;
    s.dispersion = 0.3f;
    s.saturation = 1.7f;
    s.tint = dark ? rgba(20, 20, 26, 100) : rgba(255, 255, 255, 60);
    s.shine = 0.25f;
    return s;
}

GlassStyle glass_style_window(bool dark) {
    GlassStyle s;
    s.radius = 14;
    s.blur = 14;
    s.relative = false;
    s.depth = 14;
    s.dome = 10;
    s.strength = 12;
    s.dispersion = 0.4f;
    s.saturation = 1.8f;
    s.tint = dark ? rgba(38, 38, 44, 170) : rgba(246, 246, 250, 150);
    s.shine = dark ? 0.3f : 0.5f;
    return s;
}

GlassStyle glass_style_control(bool dark) {
    GlassStyle s;
    s.radius = 999;
    s.blur = 2;
    s.depth = 0.35f;
    s.dome = 0.5f;
    s.strength = 0.5f;
    s.dispersion = 0.6f;
    s.saturation = 1.2f;
    s.tint = dark ? rgba(255, 255, 255, 18) : rgba(255, 255, 255, 40);
    s.shine = 0.8f;
    return s;
}

int glass_margin(const GlassStyle& s) { return (int)(s.blur * 2) + 4; }
void glass_set_lite(bool on) { g_lite = on; }

void glass_render(Canvas& dst, Rect r, const GlassStyle& s, Rect clip) {
    Rect area = r.intersect(clip).intersect(dst.bounds());
    if (area.empty() || r.w < 2 || r.h < 2) return;
    LensMap& map = get_map(r.w, r.h, s);

    // Backdrop region: the glass rect plus a blur margin, downsampled 2x when frosted.
    int margin = glass_margin(s);
    int sh = s.blur >= 3 ? (g_lite ? 2 : 1) : 0;
    int scale = 1 << sh;
    // only what the lens can reach from the pixels being drawn, not the whole shape
    float reach = s.strength * (s.relative ? kmin(r.w, r.h) : 1) * (1 + s.dispersion * 0.1f);
    Rect br = area.expand(margin + (int)reach + 2).intersect(r.expand(margin)).intersect(dst.bounds());
    // stay on the absolute sampling grid so separately composed regions line up
    int ax = br.x & (scale - 1), ay = br.y & (scale - 1);
    br.x -= ax; br.w += ax;
    br.y -= ay; br.h += ay;
    int samples = g_lite ? 1 : scale;
    int bw = (br.w + scale - 1) / scale, bh = (br.h + scale - 1) / scale;
    size_t need = (size_t)bw * bh;
    if (need > g_back_cap) {
        kfree(g_back);
        g_back_cap = need + need / 4;
        g_back = (uint32_t*)kmalloc(g_back_cap * 4);
    }
    float sat = s.saturation;
    int sat_k = (int)(sat * 256);
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            int sx = br.x + x * scale, sy = br.y + y * scale;
            uint32_t r_ = 0, g_ = 0, b_ = 0, n = 0;
            for (int yy = 0; yy < samples; yy++) {
                if (sy + yy >= br.bottom()) break;
                const uint32_t* row = dst.row(sy + yy);
                for (int xx = 0; xx < samples; xx++) {
                    if (sx + xx >= br.right()) break;
                    uint32_t p = row[sx + xx];
                    r_ += (p >> 16) & 0xFF; g_ += (p >> 8) & 0xFF; b_ += p & 0xFF; n++;
                }
            }
            int R = r_, G = g_, B = b_;
            if (n == 4) { R >>= 2; G >>= 2; B >>= 2; }
            else if (n > 1) { R /= n; G /= n; B /= n; }
            // saturation boost around luma
            int L = (R * 77 + G * 150 + B * 29) >> 8;
            R = L + (((R - L) * sat_k) >> 8);
            G = L + (((G - L) * sat_k) >> 8);
            B = L + (((B - L) * sat_k) >> 8);
            R = R < 0 ? 0 : R > 255 ? 255 : R;
            G = G < 0 ? 0 : G > 255 ? 255 : G;
            B = B < 0 ? 0 : B > 255 ? 255 : B;
            g_back[y * bw + x] = 0xFF000000u | (R << 16) | (G << 8) | B;
        }
    }
    if (s.blur >= 1) {
        Canvas tmp(g_back, bw, bh, bw);
        int rad = (int)(s.blur / scale + 0.5f);
        if (rad < 1) rad = 1;
        tmp.blur(tmp.bounds(), rad, g_lite ? 2 : 3);
    }

    uint32_t tint = premul(s.tint);
    uint32_t tint_ia = 255 - (tint >> 24);
    int shine = (int)(s.shine * 256);
    float disp = g_lite ? 0 : s.dispersion * 0.1f;
    int disp_r = (int)((1 + disp) * 256), disp_b = (int)((1 - disp) * 256);
    // backdrop coordinate of glass pixel (x,y): ((x - br.x)/scale)
    for (int y = area.y; y < area.bottom(); y++) {
        uint32_t* drow = dst.row(y);
        const LensPx* mrow = map.px + (y - r.y) * map.w;
        int by16 = (((y - br.y) * 16 + 8) >> sh) - 8;
        for (int x = area.x; x < area.right(); x++) {
            const LensPx& lp = mrow[x - r.x];
            if (!lp.mask) continue;
            int bx16 = (((x - br.x) * 16 + 8) >> sh) - 8;
            uint32_t c;
            if (lp.dx == 0 && lp.dy == 0) {
                c = sample(g_back, bw, bh, bx16, by16);
            } else {
                int ox = lp.dx >> sh, oy = lp.dy >> sh;
                uint32_t g = sample(g_back, bw, bh, bx16 + ox, by16 + oy);
                if (disp > 0 && (ox > 8 || ox < -8 || oy > 8 || oy < -8)) {
                    uint32_t rr = sample(g_back, bw, bh, bx16 + ((ox * disp_r) >> 8), by16 + ((oy * disp_r) >> 8));
                    uint32_t bb = sample(g_back, bw, bh, bx16 + ((ox * disp_b) >> 8), by16 + ((oy * disp_b) >> 8));
                    c = 0xFF000000u | (rr & 0x00FF0000) | (g & 0x0000FF00) | (bb & 0x000000FF);
                } else {
                    c = g;
                }
            }
            // frost tint
            c = tint + px_scale(c, tint_ia);
            c |= 0xFF000000u;
            // rim light: lift towards white
            if (lp.light) {
                int k = (lp.light * shine) >> 8;
                if (k > 255) k = 255;
                uint32_t R = (c >> 16) & 0xFF, G = (c >> 8) & 0xFF, B = c & 0xFF;
                R += ((255 - R) * k) >> 8;
                G += ((255 - G) * k) >> 8;
                B += ((255 - B) * k) >> 8;
                c = 0xFF000000u | (R << 16) | (G << 8) | B;
            }
            if (lp.mask == 255) drow[x] = c;
            else drow[x] = px_over(drow[x], px_scale(c, lp.mask));
        }
    }
}
