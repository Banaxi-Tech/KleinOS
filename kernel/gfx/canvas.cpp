#include "gfx/canvas.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "lib/math.h"

Canvas::Canvas(int w, int h) { resize(w, h); }

Canvas::Canvas(uint32_t* pixels, int w, int h, int stride)
    : px_(pixels), w_(w), h_(h), stride_(stride), owns_(false) {
    clip_ = Rect(0, 0, w, h);
}

Canvas::~Canvas() {
    if (owns_) kfree(px_);
}

void Canvas::resize(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (owns_ && w == w_ && h == h_) { ox_ = oy_ = 0; depth_ = 0; clip_ = Rect(0, 0, w, h); return; }
    if (owns_) kfree(px_);
    px_ = (uint32_t*)kmalloc((size_t)w * h * 4);
    memset(px_, 0, (size_t)w * h * 4);
    w_ = w; h_ = h; stride_ = w;
    owns_ = true;
    ox_ = oy_ = 0;
    depth_ = 0;
    clip_ = Rect(0, 0, w, h);
}

void Canvas::save() {
    if (depth_ < 16) stack_[depth_++] = State{ox_, oy_, clip_};
}
void Canvas::restore() {
    if (depth_ > 0) {
        State& s = stack_[--depth_];
        ox_ = s.ox; oy_ = s.oy; clip_ = s.clip;
    }
}
void Canvas::translate(int dx, int dy) { ox_ += dx; oy_ += dy; }
void Canvas::clip_to(Rect r) { clip_ = clip_.intersect(r.offset(ox_, oy_)); }
void Canvas::reset_clip() { clip_ = Rect(0, 0, w_, h_); }
Rect Canvas::clip() const { return clip_.offset(-ox_, -oy_); }

void Canvas::span(int y, int x0, int x1, uint32_t pc) {
    uint32_t* p = row(y);
    uint32_t a = pc >> 24;
    if (a == 255) {
        for (int x = x0; x < x1; x++) p[x] = pc;
    } else if (a) {
        uint32_t ia = 255 - a;
        for (int x = x0; x < x1; x++) p[x] = pc + px_scale(p[x], ia);
    }
}

void Canvas::blend_px(int dx, int dy, uint32_t pc, int cov) {
    if (cov <= 0) return;
    uint32_t* p = row(dy) + dx;
    if (cov >= 255) *p = px_over(*p, pc);
    else *p = px_over(*p, px_scale(pc, cov));
}

void Canvas::set_pixel(int x, int y, Color c) {
    x += ox_; y += oy_;
    if (!clip_.contains(x, y)) return;
    blend_px(x, y, premul(c), 255);
}

uint32_t Canvas::get_pixel(int x, int y) const {
    x += ox_; y += oy_;
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return 0;
    return row(y)[x];
}

void Canvas::clear(Color c) {
    uint32_t pc = premul(c);
    for (int y = clip_.y; y < clip_.bottom(); y++) {
        uint32_t* p = row(y);
        for (int x = clip_.x; x < clip_.right(); x++) p[x] = pc;
    }
}

void Canvas::fill_rect(Rect r, Color c) {
    Rect d = r.offset(ox_, oy_).intersect(clip_);
    if (d.empty()) return;
    uint32_t pc = premul(c);
    for (int y = d.y; y < d.bottom(); y++) span(y, d.x, d.right(), pc);
}

void Canvas::stroke_rect(Rect r, Color c, int w) {
    fill_rect(Rect(r.x, r.y, r.w, w), c);
    fill_rect(Rect(r.x, r.bottom() - w, r.w, w), c);
    fill_rect(Rect(r.x, r.y + w, w, r.h - 2 * w), c);
    fill_rect(Rect(r.right() - w, r.y + w, w, r.h - 2 * w), c);
}

// Coverage of pixel (px,py) center for a rounded rect via signed distance.
static inline float rr_sdf(float px, float py, float cx, float cy, float hw, float hh, float rad) {
    float qx = fabsf(px - cx) - hw + rad;
    float qy = fabsf(py - cy) - hh + rad;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float outside = sqrtf(ox * ox + oy * oy);
    float inside = qx > qy ? qx : qy;
    if (inside > 0) inside = 0;
    return outside + inside - rad;
}

void Canvas::fill_round_rect(Rect r, float radius, Color c) {
    if (radius <= 0.5f) { fill_rect(r, c); return; }
    float maxr = (r.w < r.h ? r.w : r.h) / 2.0f;
    if (radius > maxr) radius = maxr;
    Rect d = r.offset(ox_, oy_);
    Rect cl = d.intersect(clip_);
    if (cl.empty()) return;
    uint32_t pc = premul(c);
    float cx = d.x + d.w / 2.0f, cy = d.y + d.h / 2.0f, hw = d.w / 2.0f, hh = d.h / 2.0f;
    int rad_i = (int)ceilf(radius);
    for (int y = cl.y; y < cl.bottom(); y++) {
        bool corner_row = y < d.y + rad_i || y >= d.bottom() - rad_i;
        if (!corner_row) { span(y, cl.x, cl.right(), pc); continue; }
        // middle part of the row is solid
        int sx0 = d.x + rad_i, sx1 = d.right() - rad_i;
        for (int x = cl.x; x < cl.right(); x++) {
            if (x >= sx0 && x < sx1) {
                int e = sx1 < cl.right() ? sx1 : cl.right();
                span(y, x, e, pc);
                x = e - 1;
                continue;
            }
            float sd = rr_sdf(x + 0.5f, y + 0.5f, cx, cy, hw, hh, radius);
            float cov = 0.5f - sd;
            if (cov <= 0) continue;
            blend_px(x, y, pc, cov >= 1 ? 255 : (int)(cov * 255));
        }
    }
}

void Canvas::stroke_round_rect(Rect r, float radius, Color c, float width) {
    float maxr = (r.w < r.h ? r.w : r.h) / 2.0f;
    if (radius > maxr) radius = maxr;
    Rect d = r.offset(ox_, oy_);
    Rect cl = d.intersect(clip_);
    if (cl.empty()) return;
    uint32_t pc = premul(c);
    float cx = d.x + d.w / 2.0f, cy = d.y + d.h / 2.0f;
    // stroke centered at width/2 inside the edge
    float hw = d.w / 2.0f - width / 2, hh = d.h / 2.0f - width / 2;
    float rad = radius - width / 2;
    if (rad < 0) rad = 0;
    int band = (int)ceilf(radius + width) + 1;
    for (int y = cl.y; y < cl.bottom(); y++) {
        bool edge_row = y < d.y + band || y >= d.bottom() - band;
        for (int x = cl.x; x < cl.right(); x++) {
            if (!edge_row && x >= d.x + band && x < d.right() - band) { x = d.right() - band - 1; continue; }
            float sd = rr_sdf(x + 0.5f, y + 0.5f, cx, cy, hw, hh, rad);
            float cov = width / 2 + 0.5f - fabsf(sd);
            if (cov <= 0) continue;
            blend_px(x, y, pc, cov >= 1 ? 255 : (int)(cov * 255));
        }
    }
}

static inline Color lerp_color(Color a, Color b, float t) {
    int ti = (int)(t * 255.0f + 0.5f);
    if (ti < 0) ti = 0;
    if (ti > 255) ti = 255;
    return mix(a, b, ti);
}

void Canvas::fill_gradient(Rect r, Color top, Color bottom, float radius) {
    Rect d = r.offset(ox_, oy_);
    Rect cl = d.intersect(clip_);
    if (cl.empty()) return;
    float maxr = (r.w < r.h ? r.w : r.h) / 2.0f;
    if (radius > maxr) radius = maxr;
    float cx = d.x + d.w / 2.0f, cy = d.y + d.h / 2.0f, hw = d.w / 2.0f, hh = d.h / 2.0f;
    int rad_i = (int)ceilf(radius);
    for (int y = cl.y; y < cl.bottom(); y++) {
        float t = d.h > 1 ? (float)(y - d.y) / (d.h - 1) : 0;
        uint32_t pc = premul(lerp_color(top, bottom, t));
        bool corner_row = radius > 0.5f && (y < d.y + rad_i || y >= d.bottom() - rad_i);
        if (!corner_row) { span(y, cl.x, cl.right(), pc); continue; }
        for (int x = cl.x; x < cl.right(); x++) {
            if (x >= d.x + rad_i && x < d.right() - rad_i) { blend_px(x, y, pc, 255); continue; }
            float sd = rr_sdf(x + 0.5f, y + 0.5f, cx, cy, hw, hh, radius);
            float cov = 0.5f - sd;
            if (cov <= 0) continue;
            blend_px(x, y, pc, cov >= 1 ? 255 : (int)(cov * 255));
        }
    }
}

void Canvas::fill_gradient_h(Rect r, Color left, Color right, float radius) {
    Rect d = r.offset(ox_, oy_);
    Rect cl = d.intersect(clip_);
    if (cl.empty()) return;
    float maxr = (r.w < r.h ? r.w : r.h) / 2.0f;
    if (radius > maxr) radius = maxr;
    float cx = d.x + d.w / 2.0f, cy = d.y + d.h / 2.0f, hw = d.w / 2.0f, hh = d.h / 2.0f;
    for (int y = cl.y; y < cl.bottom(); y++) {
        for (int x = cl.x; x < cl.right(); x++) {
            float t = d.w > 1 ? (float)(x - d.x) / (d.w - 1) : 0;
            uint32_t pc = premul(lerp_color(left, right, t));
            if (radius > 0.5f) {
                float sd = rr_sdf(x + 0.5f, y + 0.5f, cx, cy, hw, hh, radius);
                float cov = 0.5f - sd;
                if (cov <= 0) continue;
                blend_px(x, y, pc, cov >= 1 ? 255 : (int)(cov * 255));
            } else {
                blend_px(x, y, pc, 255);
            }
        }
    }
}

void Canvas::fill_circle(float cx, float cy, float r, Color c) { fill_ellipse(cx, cy, r, r, c); }

void Canvas::fill_ellipse(float cx, float cy, float rx, float ry, Color c) {
    cx += ox_; cy += oy_;
    Rect bb((int)floorf(cx - rx - 1), (int)floorf(cy - ry - 1), (int)(2 * rx + 3), (int)(2 * ry + 3));
    Rect cl = bb.intersect(clip_);
    if (cl.empty() || rx <= 0 || ry <= 0) return;
    uint32_t pc = premul(c);
    float k = ry / rx;
    for (int y = cl.y; y < cl.bottom(); y++) {
        float dy = y + 0.5f - cy;
        for (int x = cl.x; x < cl.right(); x++) {
            float dx = (x + 0.5f - cx) * k;
            float dist = sqrtf(dx * dx + dy * dy);
            float cov = ry + 0.5f - dist;
            if (cov <= 0) continue;
            blend_px(x, y, pc, cov >= 1 ? 255 : (int)(cov * 255));
        }
    }
}

void Canvas::stroke_circle(float cx, float cy, float r, Color c, float width) {
    cx += ox_; cy += oy_;
    float ext = r + width / 2 + 1;
    Rect bb((int)floorf(cx - ext), (int)floorf(cy - ext), (int)(2 * ext + 2), (int)(2 * ext + 2));
    Rect cl = bb.intersect(clip_);
    if (cl.empty()) return;
    uint32_t pc = premul(c);
    for (int y = cl.y; y < cl.bottom(); y++) {
        float dy = y + 0.5f - cy;
        for (int x = cl.x; x < cl.right(); x++) {
            float dx = x + 0.5f - cx;
            float dist = sqrtf(dx * dx + dy * dy);
            float cov = width / 2 + 0.5f - fabsf(dist - r);
            if (cov <= 0) continue;
            blend_px(x, y, pc, cov >= 1 ? 255 : (int)(cov * 255));
        }
    }
}

void Canvas::fill_arc(float cx, float cy, float ro, float ri, float a0, float a1, Color c) {
    cx += ox_; cy += oy_;
    Rect bb((int)floorf(cx - ro - 1), (int)floorf(cy - ro - 1), (int)(2 * ro + 3), (int)(2 * ro + 3));
    Rect cl = bb.intersect(clip_);
    if (cl.empty()) return;
    uint32_t pc = premul(c);
    const float two_pi = 6.2831853f;
    while (a1 < a0) a1 += two_pi;
    float span_a = a1 - a0;
    for (int y = cl.y; y < cl.bottom(); y++) {
        float dy = y + 0.5f - cy;
        for (int x = cl.x; x < cl.right(); x++) {
            float dx = x + 0.5f - cx;
            float dist = sqrtf(dx * dx + dy * dy);
            float cov = ro + 0.5f - dist;
            if (ri > 0) { float ci = dist - ri + 0.5f; if (ci < cov) cov = ci; }
            if (cov <= 0) continue;
            if (span_a < two_pi - 0.001f) {
                float a = atan2f(dy, dx) - a0;
                while (a < 0) a += two_pi;
                while (a >= two_pi) a -= two_pi;
                if (a > span_a) continue;
            }
            blend_px(x, y, pc, cov >= 1 ? 255 : (int)(cov * 255));
        }
    }
}

void Canvas::draw_line(float x0, float y0, float x1, float y1, Color c, float width) {
    x0 += ox_; y0 += oy_; x1 += ox_; y1 += oy_;
    float hw = width / 2;
    Rect bb((int)floorf(kmin(x0, x1) - hw - 1), (int)floorf(kmin(y0, y1) - hw - 1),
            (int)(fabsf(x1 - x0) + width + 3), (int)(fabsf(y1 - y0) + width + 3));
    Rect cl = bb.intersect(clip_);
    if (cl.empty()) return;
    uint32_t pc = premul(c);
    float vx = x1 - x0, vy = y1 - y0;
    float len2 = vx * vx + vy * vy;
    for (int y = cl.y; y < cl.bottom(); y++) {
        for (int x = cl.x; x < cl.right(); x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = len2 > 0 ? (px * vx + py * vy) / len2 : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            float dx = px - vx * t, dy = py - vy * t;
            float d = sqrtf(dx * dx + dy * dy);
            float cov = hw + 0.5f - d;
            if (cov <= 0) continue;
            blend_px(x, y, pc, cov >= 1 ? 255 : (int)(cov * 255));
        }
    }
}

void Canvas::fill_polygon(const PointF* pts, int n, Color c) {
    if (n < 3) return;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) {
        minx = kmin(minx, pts[i].x + ox_); maxx = kmax(maxx, pts[i].x + ox_);
        miny = kmin(miny, pts[i].y + oy_); maxy = kmax(maxy, pts[i].y + oy_);
    }
    Rect bb((int)floorf(minx), (int)floorf(miny), (int)ceilf(maxx) - (int)floorf(minx) + 1,
            (int)ceilf(maxy) - (int)floorf(miny) + 1);
    Rect cl = bb.intersect(clip_);
    if (cl.empty()) return;
    uint32_t pc = premul(c);
    constexpr int SS = 4;   // 4x4 supersampling
    uint16_t* cov = (uint16_t*)kcalloc(cl.w + 1, sizeof(uint16_t));
    float xs[64];
    for (int y = cl.y; y < cl.bottom(); y++) {
        memset(cov, 0, (cl.w + 1) * sizeof(uint16_t));
        for (int s = 0; s < SS; s++) {
            float sy = y + (s + 0.5f) / SS;
            // collect crossings with winding
            int nx = 0;
            int wind[64];
            for (int i = 0; i < n && nx < 64; i++) {
                PointF a = pts[i], b = pts[(i + 1) % n];
                float ay = a.y + oy_, by = b.y + oy_;
                if ((ay <= sy && by > sy) || (by <= sy && ay > sy)) {
                    float t = (sy - ay) / (by - ay);
                    xs[nx] = a.x + ox_ + t * (b.x - a.x);
                    wind[nx] = ay < by ? 1 : -1;
                    nx++;
                }
            }
            // sort crossings
            for (int i = 1; i < nx; i++) {
                float v = xs[i]; int w = wind[i]; int j = i - 1;
                while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; wind[j + 1] = wind[j]; j--; }
                xs[j + 1] = v; wind[j + 1] = w;
            }
            int wsum = 0;
            for (int i = 0; i + 1 < nx + 1 && i < nx; i++) {
                wsum += wind[i];
                if (wsum == 0 || i + 1 >= nx) continue;
                float xa = xs[i], xb = xs[i + 1];
                for (int sx = 0; sx < SS; sx++) {
                    float off = (sx + 0.5f) / SS;
                    int x0 = (int)ceilf(xa - off), x1 = (int)ceilf(xb - off);
                    if (x0 < cl.x) x0 = cl.x;
                    if (x1 > cl.right()) x1 = cl.right();
                    for (int x = x0; x < x1; x++) cov[x - cl.x]++;
                }
            }
        }
        for (int x = 0; x < cl.w; x++)
            if (cov[x]) blend_px(cl.x + x, y, pc, cov[x] * 255 / (SS * SS));
    }
    kfree(cov);
}

// erf-like falloff table for shadows: index = (distance / blur) * 64 + 128
static float shadow_falloff(float t) {
    // t in blur units; 0 at edge. Smooth gaussian-ish via tanh.
    return 0.5f * (1.0f - tanhf(t * 1.6f));
}

void Canvas::draw_shadow(Rect r, float radius, float blur, Color c, int sdx, int sdy) {
    Rect sr = r.offset(sdx + ox_, sdy + oy_);
    int ext = (int)ceilf(blur * 1.5f) + 2;
    Rect bb = sr.expand(ext);
    Rect cl = bb.intersect(clip_);
    if (cl.empty()) return;
    // shadow never draws inside the casting rect itself (it's covered)
    Rect hole = r.offset(ox_, oy_).inset((int)(radius * 0.3f) + 1);
    float cx = sr.x + sr.w / 2.0f, cy = sr.y + sr.h / 2.0f, hw = sr.w / 2.0f, hh = sr.h / 2.0f;
    float maxr = kmin(hw, hh);
    if (radius > maxr) radius = maxr;
    uint32_t pc = premul(c);
    float lut[256];
    for (int i = 0; i < 256; i++) lut[i] = shadow_falloff((i - 128) / 32.0f);
    float inv = blur > 0 ? 32.0f / blur : 1000.0f;
    for (int y = cl.y; y < cl.bottom(); y++) {
        for (int x = cl.x; x < cl.right(); x++) {
            if (hole.contains(x, y)) { x = hole.right() - 1; continue; }
            float sd = rr_sdf(x + 0.5f, y + 0.5f, cx, cy, hw, hh, radius);
            int idx = (int)(sd * inv) + 128;
            if (idx >= 255) continue;
            if (idx < 0) idx = 0;
            int a = (int)(lut[idx] * 255.0f);
            if (a > 0) blend_px(x, y, pc, a);
        }
    }
}

void Canvas::blur(Rect r, int radius, int passes) {
    Rect d = r.offset(ox_, oy_).intersect(Rect(0, 0, w_, h_));
    if (d.empty() || radius < 1) return;
    int maxdim = d.w > d.h ? d.w : d.h;
    uint32_t* tmp = (uint32_t*)kmalloc(maxdim * 4 * 4);
    uint32_t* line = tmp;
    for (int pass = 0; pass < passes; pass++) {
        // horizontal
        for (int y = d.y; y < d.bottom(); y++) {
            uint32_t* p = row(y) + d.x;
            memcpy(line, p, d.w * 4);
            uint32_t sa = 0, sr = 0, sg = 0, sb = 0;
            int n = 2 * radius + 1;
            for (int i = -radius; i <= radius; i++) {
                uint32_t v = line[kclamp(i, 0, d.w - 1)];
                sa += v >> 24; sr += (v >> 16) & 0xFF; sg += (v >> 8) & 0xFF; sb += v & 0xFF;
            }
            for (int x = 0; x < d.w; x++) {
                p[x] = ((sa / n) << 24) | ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
                uint32_t out = line[kclamp(x - radius, 0, d.w - 1)];
                uint32_t in = line[kclamp(x + radius + 1, 0, d.w - 1)];
                sa += (in >> 24) - (out >> 24);
                sr += ((in >> 16) & 0xFF) - ((out >> 16) & 0xFF);
                sg += ((in >> 8) & 0xFF) - ((out >> 8) & 0xFF);
                sb += (in & 0xFF) - (out & 0xFF);
            }
        }
        // vertical
        for (int x = d.x; x < d.right(); x++) {
            for (int y = 0; y < d.h; y++) line[y] = row(d.y + y)[x];
            uint32_t sa = 0, sr = 0, sg = 0, sb = 0;
            int n = 2 * radius + 1;
            for (int i = -radius; i <= radius; i++) {
                uint32_t v = line[kclamp(i, 0, d.h - 1)];
                sa += v >> 24; sr += (v >> 16) & 0xFF; sg += (v >> 8) & 0xFF; sb += v & 0xFF;
            }
            for (int y = 0; y < d.h; y++) {
                row(d.y + y)[x] = ((sa / n) << 24) | ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
                uint32_t out = line[kclamp(y - radius, 0, d.h - 1)];
                uint32_t in = line[kclamp(y + radius + 1, 0, d.h - 1)];
                sa += (in >> 24) - (out >> 24);
                sr += ((in >> 16) & 0xFF) - ((out >> 16) & 0xFF);
                sg += ((in >> 8) & 0xFF) - ((out >> 8) & 0xFF);
                sb += (in & 0xFF) - (out & 0xFF);
            }
        }
    }
    kfree(tmp);
}

void Canvas::blit(const Canvas& src, int x, int y) { blit(src, src.bounds(), x, y); }

void Canvas::blit(const Canvas& src, Rect sr, int x, int y) {
    sr = sr.intersect(src.bounds());
    Rect d = Rect(x + ox_, y + oy_, sr.w, sr.h).intersect(clip_);
    if (d.empty()) return;
    int sx = sr.x + (d.x - (x + ox_)), sy = sr.y + (d.y - (y + oy_));
    for (int j = 0; j < d.h; j++) memcpy(row(d.y + j) + d.x, src.row(sy + j) + sx, d.w * 4);
}

void Canvas::draw(const Canvas& src, int x, int y, int opacity) { draw(src, src.bounds(), x, y, opacity); }

void Canvas::draw(const Canvas& src, Rect sr, int x, int y, int opacity) {
    sr = sr.intersect(src.bounds());
    Rect d = Rect(x + ox_, y + oy_, sr.w, sr.h).intersect(clip_);
    if (d.empty() || opacity <= 0) return;
    int sx = sr.x + (d.x - (x + ox_)), sy = sr.y + (d.y - (y + oy_));
    for (int j = 0; j < d.h; j++) {
        uint32_t* dp = row(d.y + j) + d.x;
        const uint32_t* sp = src.row(sy + j) + sx;
        if (opacity >= 255)
            for (int i = 0; i < d.w; i++) dp[i] = px_over(dp[i], sp[i]);
        else
            for (int i = 0; i < d.w; i++) dp[i] = px_over(dp[i], px_scale(sp[i], opacity));
    }
}

static inline uint32_t bilinear(const Canvas& s, float fx, float fy) {
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float tx = fx - x0, ty = fy - y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = kclamp(x0, 0, s.width() - 1); x1 = kclamp(x1, 0, s.width() - 1);
    y0 = kclamp(y0, 0, s.height() - 1); y1 = kclamp(y1, 0, s.height() - 1);
    uint32_t a = s.row(y0)[x0], b = s.row(y0)[x1], c = s.row(y1)[x0], d = s.row(y1)[x1];
    int wx = (int)(tx * 256), wy = (int)(ty * 256);
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF, cc = (c >> sh) & 0xFF, cd = (d >> sh) & 0xFF;
        int top = ca * (256 - wx) + cb * wx, bot = cc * (256 - wx) + cd * wx;
        int v = (top * (256 - wy) + bot * wy) >> 16;
        out |= (uint32_t)v << sh;
    }
    return out;
}

void Canvas::draw_scaled(const Canvas& src, Rect dst, int opacity) {
    Rect d = dst.offset(ox_, oy_);
    Rect cl = d.intersect(clip_);
    if (cl.empty() || dst.w <= 0 || dst.h <= 0) return;
    float sx = (float)src.width() / dst.w, sy = (float)src.height() / dst.h;
    bool down = sx > 2.0f || sy > 2.0f;
    for (int y = cl.y; y < cl.bottom(); y++) {
        uint32_t* dp = row(y);
        float fy = (y - d.y + 0.5f) * sy - 0.5f;
        for (int x = cl.x; x < cl.right(); x++) {
            float fx = (x - d.x + 0.5f) * sx - 0.5f;
            uint32_t p;
            if (down) {
                // box average for strong downscale
                int x0 = (int)((x - d.x) * sx), x1 = (int)((x - d.x + 1) * sx);
                int y0 = (int)((y - d.y) * sy), y1 = (int)((y - d.y + 1) * sy);
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                uint32_t acc[4] = {0, 0, 0, 0};
                int cnt = 0;
                for (int yy = y0; yy < y1 && yy < src.height(); yy += 1)
                    for (int xx = x0; xx < x1 && xx < src.width(); xx += 1) {
                        uint32_t v = src.row(yy)[xx];
                        acc[0] += v & 0xFF; acc[1] += (v >> 8) & 0xFF; acc[2] += (v >> 16) & 0xFF; acc[3] += v >> 24;
                        cnt++;
                    }
                if (!cnt) continue;
                p = (acc[0] / cnt) | ((acc[1] / cnt) << 8) | ((acc[2] / cnt) << 16) | ((acc[3] / cnt) << 24);
            } else {
                p = bilinear(src, fx, fy);
            }
            if (opacity < 255) p = px_scale(p, opacity);
            dp[x] = px_over(dp[x], p);
        }
    }
}

void Canvas::draw_mask(const uint8_t* mask, int mw, int mh, int mstride, int x, int y, Color c) {
    Rect d = Rect(x + ox_, y + oy_, mw, mh).intersect(clip_);
    if (d.empty()) return;
    uint32_t pc = premul(c);
    int mx0 = d.x - (x + ox_), my0 = d.y - (y + oy_);
    for (int j = 0; j < d.h; j++) {
        const uint8_t* m = mask + (my0 + j) * mstride + mx0;
        uint32_t* p = row(d.y + j) + d.x;
        for (int i = 0; i < d.w; i++) {
            uint32_t cov = m[i];
            if (!cov) continue;
            p[i] = px_over(p[i], cov == 255 ? pc : px_scale(pc, cov));
        }
    }
}

int Canvas::draw_text_n(int x, int y, const char* s, int nbytes, Color c, int size, Font font) {
    int baseline = y + font_ascent(size, font);
    float pen = (float)x;
    const char* end = s + nbytes;
    uint32_t prev = 0;
    while (s < end && *s) {
        uint32_t cp = utf8_next(&s);
        if (!cp) break;
        if (prev && font != Font::Mono) pen += font_kern(font, size, prev, cp);
        float fx = pen + ox_;
        int ix = (int)floorf(fx);
        int sub = (int)((fx - ix) * 4.0f) & 3;
        const Glyph* g = font_glyph(font, size, cp, sub);
        if (!g) break;
        if (g->w && cp != ' ')
            draw_mask(g->bitmap, g->w, g->h, g->w, ix - ox_ + g->xoff, baseline + g->yoff, c);
        pen += g->advance;
        prev = cp;
    }
    return (int)(pen - x + 0.5f);
}

int Canvas::draw_text(int x, int y, const char* s, Color c, int size, Font font) {
    return draw_text_n(x, y, s, (int)strlen(s), c, size, font);
}

void Canvas::draw_text_in(Rect r, const char* s, Color c, int size, Font font, Align align) {
    int lh = line_height(size, font);
    int ty = r.y + (r.h - lh) / 2;
    int len = (int)strlen(s);
    int w = text_width(s, size, font);
    if (w <= r.w) {
        int tx = align == Align::Left ? r.x : align == Align::Center ? r.x + (r.w - w) / 2 : r.right() - w;
        draw_text(tx, ty, s, c, size, font);
        return;
    }
    // ellipsize
    const char* ell = "\xE2\x80\xA6";
    int ew = text_width(ell, size, font);
    int n = len;
    while (n > 0 && text_width_n(s, n, size, font) + ew > r.w) {
        n--;
        while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;
    }
    save();
    clip_to(r);
    int adv = draw_text_n(r.x, ty, s, n, c, size, font);
    draw_text(r.x + adv, ty, ell, c, size, font);
    restore();
}

// Shared line-breaking for wrapped text. Calls fn(start, nbytes, line_width) per line.
template <typename F>
static int wrap_lines(int width, const char* s, int size, Font font, F fn) {
    int lines = 0;
    while (*s) {
        // hard break
        const char* line_start = s;
        const char* last_space = nullptr;
        const char* p = s;
        while (*p && *p != '\n') {
            const char* q = p;
            utf8_next(&q);
            int w = text_width_n(line_start, (int)(q - line_start), size, font);
            if (w > width && p > line_start) break;
            if (*p == ' ') last_space = p;
            p = q;
        }
        const char* brk = p;
        if (*p && *p != '\n' && last_space) brk = last_space;
        int n = (int)(brk - line_start);
        fn(line_start, n, text_width_n(line_start, n, size, font));
        lines++;
        s = brk;
        if (*s == ' ' || *s == '\n') s++;
    }
    return lines;
}

int Canvas::draw_text_wrapped(Rect r, const char* s, Color c, int size, Font font, Align align, int spacing) {
    int lh = line_height(size, font) + spacing;
    int y = r.y;
    wrap_lines(r.w, s, size, font, [&](const char* ls, int n, int w) {
        int x = align == Align::Left ? r.x : align == Align::Center ? r.x + (r.w - w) / 2 : r.right() - w;
        draw_text_n(x, y, ls, n, c, size, font);
        y += lh;
    });
    return y - r.y;
}

int Canvas::measure_wrapped(int width, const char* s, int size, Font font, int spacing) {
    int lines = wrap_lines(width, s, size, font, [](const char*, int, int) {});
    return lines * (line_height(size, font) + spacing);
}

// ---------------------------------------------------------------- images
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)
#define STBI_MALLOC(sz) kmalloc(sz)
#define STBI_REALLOC(p, sz) krealloc(p, sz)
#define STBI_FREE(p) kfree(p)
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wtype-limits"
#include "third_party/stb_image.h"
#pragma GCC diagnostic pop

Canvas* image_decode(const uint8_t* data, size_t size) {
    int w, h, n;
    uint8_t* px = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
    if (!px) return nullptr;
    Canvas* c = new Canvas(w, h);
    for (int y = 0; y < h; y++) {
        uint32_t* row = c->row(y);
        const uint8_t* s = px + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            Color col = rgba(s[x * 4], s[x * 4 + 1], s[x * 4 + 2], s[x * 4 + 3]);
            row[x] = premul(col);
        }
    }
    stbi_image_free(px);
    return c;
}
