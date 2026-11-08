#include "gfx/font.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "lib/math.h"
#include "lib/log.h"
#include "fs/vfs.h"

#define STBTT_ifloor(x) ((int)floor(x))
#define STBTT_iceil(x) ((int)ceil(x))
#define STBTT_sqrt(x) sqrt(x)
#define STBTT_pow(x, y) pow(x, y)
#define STBTT_fmod(x, y) fmod(x, y)
#define STBTT_cos(x) cos(x)
#define STBTT_acos(x) acos(x)
#define STBTT_fabs(x) fabs(x)
#define STBTT_malloc(x, u) ((void)(u), kmalloc(x))
#define STBTT_free(x, u) ((void)(u), kfree(x))
#define STBTT_assert(x) ((void)0)
#define STBTT_strlen(x) strlen(x)
#define STBTT_memcpy memcpy
#define STBTT_memset memset
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "third_party/stb_truetype.h"
#pragma GCC diagnostic pop

namespace {

struct FontFace {
    stbtt_fontinfo info;
    bool loaded;
    int ascent, descent, gap;
};

FontFace g_faces[(int)Font::Count];

struct CacheEntry {
    uint32_t key_hi;   // font | size | subpixel
    uint32_t cp;
    Glyph glyph;
    bool used;
};

constexpr int kCacheSize = 8192;
CacheEntry* g_cache;
int g_cache_count;

// Gamma-ish boost so light-weight text doesn't look washed out.
uint8_t g_coverage_lut[256];

FontFace& face(Font f) {
    FontFace& ff = g_faces[(int)f];
    if (!ff.loaded) return g_faces[0];
    return ff;
}

float scale_for(FontFace& f, int size) { return stbtt_ScaleForMappingEmToPixels(&f.info, (float)size); }

void cache_clear() {
    for (int i = 0; i < kCacheSize; i++) {
        if (g_cache[i].used) kfree(g_cache[i].glyph.bitmap);
        g_cache[i].used = false;
    }
    g_cache_count = 0;
}

bool load_face(Font f, const char* path) {
    const uint8_t* data;
    size_t size;
    if (!vfs::read_file(path, &data, &size)) {
        klog("font: missing %s", path);
        return false;
    }
    FontFace& ff = g_faces[(int)f];
    if (!stbtt_InitFont(&ff.info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        klog("font: bad font %s", path);
        return false;
    }
    stbtt_GetFontVMetrics(&ff.info, &ff.ascent, &ff.descent, &ff.gap);
    ff.loaded = true;
    return true;
}

}  // namespace

bool font_init() {
    g_cache = (CacheEntry*)kcalloc(kCacheSize, sizeof(CacheEntry));
    for (int i = 0; i < 256; i++) g_coverage_lut[i] = (uint8_t)(pow(i / 255.0, 0.80) * 255.0 + 0.5);
    bool ok = load_face(Font::Regular, "/System/Fonts/NotoSans-Regular.ttf");
    load_face(Font::Medium, "/System/Fonts/NotoSans-Medium.ttf");
    load_face(Font::Bold, "/System/Fonts/NotoSans-Bold.ttf");
    load_face(Font::Mono, "/System/Fonts/DejaVuSansMono.ttf");
    klog("font: initialized (%s)", ok ? "ok" : "FAILED");
    return ok;
}

const Glyph* font_glyph(Font font, int size, uint32_t cp, int subpixel) {
    if (!g_cache) return nullptr;
    if (size < 4) size = 4;
    if (size > 255) size = 255;
    uint32_t key_hi = ((uint32_t)font << 16) | ((uint32_t)size << 4) | (uint32_t)subpixel;
    uint32_t h = (cp * 2654435761u) ^ (key_hi * 40503u);
    for (int probe = 0; probe < 32; probe++) {
        CacheEntry& e = g_cache[(h + probe) & (kCacheSize - 1)];
        if (e.used && e.cp == cp && e.key_hi == key_hi) return &e.glyph;
        if (!e.used) {
            if (g_cache_count > kCacheSize * 3 / 4) {
                cache_clear();
                return font_glyph(font, size, cp, subpixel);
            }
            FontFace& ff = face(font);
            float scale = scale_for(ff, size);
            int gi = stbtt_FindGlyphIndex(&ff.info, cp);
            if (gi == 0 && font != Font::Regular && g_faces[0].loaded) {
                // fallback face
                FontFace& fb = g_faces[0];
                int gi2 = stbtt_FindGlyphIndex(&fb.info, cp);
                if (gi2) {
                    // render from fallback via recursion with Regular
                    const Glyph* g = font_glyph(Font::Regular, size, cp, subpixel);
                    if (g) {
                        e.used = true; e.cp = cp; e.key_hi = key_hi;
                        e.glyph = *g;
                        e.glyph.bitmap = (uint8_t*)kmalloc(g->w * g->h + 1);
                        memcpy(e.glyph.bitmap, g->bitmap, g->w * g->h);
                        g_cache_count++;
                        return &e.glyph;
                    }
                }
            }
            int adv, lsb;
            stbtt_GetGlyphHMetrics(&ff.info, gi, &adv, &lsb);
            int x0, y0, x1, y1;
            float shift = subpixel * 0.25f;
            stbtt_GetGlyphBitmapBoxSubpixel(&ff.info, gi, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
            int w = x1 - x0, hh = y1 - y0;
            if (w < 0) w = 0;
            if (hh < 0) hh = 0;
            uint8_t* bmp = (uint8_t*)kmalloc(w * hh + 1);
            if (w && hh) {
                stbtt_MakeGlyphBitmapSubpixel(&ff.info, bmp, w, hh, w, scale, scale, shift, 0, gi);
                for (int i = 0; i < w * hh; i++) bmp[i] = g_coverage_lut[bmp[i]];
            }
            e.used = true;
            e.cp = cp;
            e.key_hi = key_hi;
            e.glyph = Glyph{bmp, w, hh, x0, y0, adv * scale};
            g_cache_count++;
            return &e.glyph;
        }
    }
    cache_clear();
    return font_glyph(font, size, cp, subpixel);
}

float font_kern(Font font, int size, uint32_t a, uint32_t b) {
    FontFace& ff = face(font);
    if (!ff.info.kern && !ff.info.gpos) return 0;
    return stbtt_GetCodepointKernAdvance(&ff.info, a, b) * scale_for(ff, size);
}

int text_width_n(const char* s, int nbytes, int size, Font font) {
    float x = 0;
    const char* end = s + nbytes;
    uint32_t prev = 0;
    while (s < end && *s) {
        uint32_t cp = utf8_next(&s);
        if (!cp) break;
        const Glyph* g = font_glyph(font, size, cp, 0);
        if (!g) break;
        if (prev && font != Font::Mono) x += font_kern(font, size, prev, cp);
        x += g->advance;
        prev = cp;
    }
    return (int)(x + 0.5f);
}

int text_width(const char* s, int size, Font font) { return text_width_n(s, (int)strlen(s), size, font); }

int line_height(int size, Font font) {
    FontFace& ff = face(font);
    if (!ff.loaded) return size + 4;
    float sc = scale_for(ff, size);
    return (int)((ff.ascent - ff.descent + ff.gap) * sc + 0.5f);
}

int font_ascent(int size, Font font) {
    FontFace& ff = face(font);
    if (!ff.loaded) return size;
    float sc = scale_for(ff, size);
    // center the ink a bit: include half the line gap above
    return (int)((ff.ascent + ff.gap * 0.5f) * sc + 0.5f);
}

int text_hit_test(const char* s, int px, int size, Font font) {
    const char* start = s;
    float x = 0;
    uint32_t prev = 0;
    while (*s) {
        const char* before = s;
        uint32_t cp = utf8_next(&s);
        const Glyph* g = font_glyph(font, size, cp, 0);
        float adv = g ? g->advance : 0;
        if (prev && font != Font::Mono) x += font_kern(font, size, prev, cp);
        if (px < x + adv / 2) return (int)(before - start);
        x += adv;
        prev = cp;
    }
    return (int)(s - start);
}
