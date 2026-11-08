#pragma once
// Canvas: a 32-bit premultiplied ARGB pixel buffer with anti-aliased drawing.
//
// All coordinates are local to the current translation (translate()) and are
// clipped to the current clip rect. Colors passed in are straight alpha
// (use rgb()/rgba()/hex()); alpha < 255 blends over existing pixels.
#include <stdint.h>
#include <stddef.h>
#include "gfx/color.h"
#include "gfx/rect.h"
#include "gfx/font.h"

enum class Align : uint8_t { Left, Center, Right };

struct PointF {
    float x, y;
};

class Canvas {
public:
    Canvas() = default;
    Canvas(int w, int h);                                // owns a new buffer (transparent)
    Canvas(uint32_t* pixels, int w, int h, int stride);  // wraps external memory
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    void resize(int w, int h);   // reallocates (contents undefined), resets state
    int width() const { return w_; }
    int height() const { return h_; }
    int stride() const { return stride_; }   // in pixels
    uint32_t* pixels() { return px_; }
    const uint32_t* pixels() const { return px_; }
    uint32_t* row(int y) { return px_ + (long)y * stride_; }
    const uint32_t* row(int y) const { return px_ + (long)y * stride_; }
    Rect bounds() const { return Rect(0, 0, w_, h_); }

    // ---- state: translation + clip (save/restore nest up to 16 levels)
    void save();
    void restore();
    void translate(int dx, int dy);
    void clip_to(Rect r);            // intersect clip with r (local coords)
    void reset_clip();
    Rect clip() const;               // current clip in local coords
    int origin_x() const { return ox_; }
    int origin_y() const { return oy_; }

    // ---- fills
    void clear(Color c);                                       // replaces pixels inside clip (no blending)
    void fill_rect(Rect r, Color c);
    void fill_round_rect(Rect r, float radius, Color c);
    void stroke_rect(Rect r, Color c, int width = 1);
    void stroke_round_rect(Rect r, float radius, Color c, float width = 1.0f);
    // Vertical gradient (top -> bottom), optional rounded corners.
    void fill_gradient(Rect r, Color top, Color bottom, float radius = 0);
    // Horizontal gradient (left -> right).
    void fill_gradient_h(Rect r, Color left, Color right, float radius = 0);
    void fill_circle(float cx, float cy, float r, Color c);
    void stroke_circle(float cx, float cy, float r, Color c, float width = 1.0f);
    void fill_ellipse(float cx, float cy, float rx, float ry, Color c);
    // Pie slice / arc ring, angles in radians (0 = right, clockwise since y is down).
    void fill_arc(float cx, float cy, float r_outer, float r_inner, float a0, float a1, Color c);
    void draw_line(float x0, float y0, float x1, float y1, Color c, float width = 1.0f);
    void fill_polygon(const PointF* pts, int n, Color c);      // anti-aliased, non-zero winding
    // Soft drop shadow for a rounded rect (drawn outside and under r).
    void draw_shadow(Rect r, float radius, float blur, Color c, int dx = 0, int dy = 0);
    // Box blur a region in place (radius in px, passes = 1..3).
    void blur(Rect r, int radius, int passes = 3);

    // ---- images
    void blit(const Canvas& src, int x, int y);                         // copy (no blend)
    void blit(const Canvas& src, Rect src_rect, int x, int y);
    void draw(const Canvas& src, int x, int y, int opacity = 255);      // alpha blend
    void draw(const Canvas& src, Rect src_rect, int x, int y, int opacity = 255);
    void draw_scaled(const Canvas& src, Rect dst, int opacity = 255);   // bilinear
    // Alpha mask (coverage bytes) tinted with color.
    void draw_mask(const uint8_t* mask, int mw, int mh, int mstride, int x, int y, Color c);

    // ---- text (y = top of the line box; returns advance width in px)
    int draw_text(int x, int y, const char* utf8, Color c, int size = 14, Font font = Font::Regular);
    // Draw at most nbytes of the string.
    int draw_text_n(int x, int y, const char* utf8, int nbytes, Color c, int size = 14, Font font = Font::Regular);
    // Single line inside r: horizontal alignment, vertically centered, ellipsized with "…" if too wide.
    void draw_text_in(Rect r, const char* utf8, Color c, int size = 14, Font font = Font::Regular,
                      Align align = Align::Left);
    // Word-wrapped paragraph inside r.width starting at r.y. Returns total height used.
    int draw_text_wrapped(Rect r, const char* utf8, Color c, int size = 14, Font font = Font::Regular,
                          Align align = Align::Left, int line_spacing = 0);
    static int measure_wrapped(int width, const char* utf8, int size, Font font = Font::Regular, int line_spacing = 0);

    // Raw pixel access in local coords (no clipping checks beyond bounds).
    void set_pixel(int x, int y, Color c);           // blends
    uint32_t get_pixel(int x, int y) const;          // premultiplied raw value

private:
    // fill a span [x0,x1) on row y (device coords, already clipped) with premul color
    void span(int y, int x0, int x1, uint32_t pc);
    void blend_px(int dx, int dy, uint32_t pc, int coverage);   // device coords, clipped by caller
    Rect device_clip() const { return clip_; }

    uint32_t* px_ = nullptr;
    int w_ = 0, h_ = 0, stride_ = 0;
    bool owns_ = false;
    int ox_ = 0, oy_ = 0;
    Rect clip_;
    struct State { int ox, oy; Rect clip; };
    State stack_[16];
    int depth_ = 0;
};

// Decode PNG/JPEG/GIF/BMP into a new Canvas (nullptr on failure).
Canvas* image_decode(const uint8_t* data, size_t size);
