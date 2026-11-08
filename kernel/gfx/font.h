#pragma once
// Anti-aliased TrueType text (stb_truetype) with a glyph cache.
// Sizes are in pixels (em size, like CSS font-size).
#include <stdint.h>

enum class Font : uint8_t { Regular = 0, Medium, Bold, Mono, Count };

bool font_init();   // loads /System/Fonts/*.ttf from the VFS

// Width in pixels of a UTF-8 string (single line).
int text_width(const char* utf8, int size, Font font = Font::Regular);
// Width of the first n bytes of the string.
int text_width_n(const char* utf8, int nbytes, int size, Font font = Font::Regular);
// Recommended line height (ascent + descent + gap).
int line_height(int size, Font font = Font::Regular);
// Distance from the top of the line box to the baseline.
int font_ascent(int size, Font font = Font::Regular);
// Byte offset in s whose glyph boundary is closest to pixel x (for caret placement).
int text_hit_test(const char* utf8, int x, int size, Font font = Font::Regular);

struct Glyph {
    uint8_t* bitmap;   // alpha coverage, w*h
    int w, h;
    int xoff, yoff;    // offset from pen position (baseline)
    float advance;
};
// Cached glyph; subpixel = 0..3 (quarter pixel x shift).
const Glyph* font_glyph(Font font, int size, uint32_t codepoint, int subpixel);
float font_kern(Font font, int size, uint32_t a, uint32_t b);
