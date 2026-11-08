#pragma once
// Colors are 0xAARRGGBB. Canvases store PREMULTIPLIED alpha internally; the
// drawing API takes straight-alpha colors (what rgba() makes) and converts.
#include <stdint.h>

using Color = uint32_t;

constexpr Color rgb(int r, int g, int b) { return 0xFF000000u | (r << 16) | (g << 8) | b; }
constexpr Color rgba(int r, int g, int b, int a) { return ((uint32_t)a << 24) | (r << 16) | (g << 8) | b; }
constexpr Color hex(uint32_t rrggbb) { return 0xFF000000u | rrggbb; }
constexpr int color_a(Color c) { return c >> 24; }
constexpr int color_r(Color c) { return (c >> 16) & 0xFF; }
constexpr int color_g(Color c) { return (c >> 8) & 0xFF; }
constexpr int color_b(Color c) { return c & 0xFF; }
constexpr Color with_alpha(Color c, int a) { return (c & 0x00FFFFFF) | ((uint32_t)a << 24); }

// Linear mix of two straight colors, t in 0..255
constexpr Color mix(Color a, Color b, int t) {
    return rgba((color_r(a) * (255 - t) + color_r(b) * t) / 255, (color_g(a) * (255 - t) + color_g(b) * t) / 255,
                (color_b(a) * (255 - t) + color_b(b) * t) / 255, (color_a(a) * (255 - t) + color_a(b) * t) / 255);
}
constexpr Color lighten(Color c, int amt) { return mix(c, with_alpha(0xFFFFFF, color_a(c)), amt); }
constexpr Color darken(Color c, int amt) { return mix(c, with_alpha(0x000000, color_a(c)), amt); }

// straight -> premultiplied
static inline uint32_t premul(Color c) {
    uint32_t a = c >> 24;
    if (a == 255) return c;
    if (a == 0) return 0;
    uint32_t rb = (c & 0x00FF00FF) * a + 0x00800080;
    rb = ((rb + ((rb >> 8) & 0x00FF00FF)) >> 8) & 0x00FF00FF;
    uint32_t g = ((c >> 8) & 0xFF) * a + 0x80;
    g = ((g + (g >> 8)) >> 8) & 0xFF;
    return (a << 24) | rb | (g << 8);
}

// Scale all four channels of a premultiplied pixel by k (0..255).
static inline uint32_t px_scale(uint32_t p, uint32_t k) {
    uint32_t rb = (p & 0x00FF00FF) * k + 0x00800080;
    rb = ((rb + ((rb >> 8) & 0x00FF00FF)) >> 8) & 0x00FF00FF;
    uint32_t ag = ((p >> 8) & 0x00FF00FF) * k + 0x00800080;
    ag = (ag + ((ag >> 8) & 0x00FF00FF)) & 0xFF00FF00;
    return rb | ag;
}

// Porter-Duff source-over, both premultiplied.
static inline uint32_t px_over(uint32_t dst, uint32_t src) {
    uint32_t sa = src >> 24;
    if (sa == 255) return src;
    if (sa == 0) return dst;
    return src + px_scale(dst, 255 - sa);
}
