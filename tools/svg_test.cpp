// Host test for libs/HTMLEngine/svg.cpp. Renders sample SVGs to PPM (composited
// over white) and runs malformed/large inputs through the parser to check for
// crashes and hangs. Build (ASan) and run, then convert with `magick x.ppm x.png`:
//   g++ -std=gnu++20 -O1 -g -fsanitize=address,undefined -Ikernel/libc -Ikernel -Ilibs \
//       tools/svg_test.cpp libs/HTMLEngine/svg.cpp kernel/gfx/canvas.cpp -o svg_test -lm
//   ./svg_test <outdir>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "HTMLEngine/svg.h"

// ---- kernel heap shims (host malloc underneath)
void heap_init(uintptr_t, size_t) {}
void* kmalloc(size_t n) { return ::malloc(n); }
void* kcalloc(size_t c, size_t s) { return ::calloc(c, s); }
void* krealloc(void* p, size_t n) { return ::realloc(p, n); }
void kfree(void* p) { ::free(p); }
size_t heap_total() { return 0; }
size_t heap_used() { return 0; }

// ---- font shims (text drawing is not exercised here)
bool font_init() { return false; }
int text_width(const char*, int, Font) { return 0; }
int text_width_n(const char*, int, int, Font) { return 0; }
int line_height(int size, Font) { return size; }
int font_ascent(int size, Font) { return size; }
int text_hit_test(const char*, int, int, Font) { return 0; }
const Glyph* font_glyph(Font, int, uint32_t, int) { return nullptr; }
float font_kern(Font, int, uint32_t, uint32_t) { return 0; }

// ---- UTF-8 helpers used by canvas.cpp
uint32_t utf8_next(const char** sp) {
    const unsigned char* s = (const unsigned char*)*sp;
    if (!*s) return 0;
    uint32_t cp = *s++;
    int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    if (extra) cp &= (0x3F >> extra);
    for (int i = 0; i < extra && (*s & 0xC0) == 0x80; i++) cp = (cp << 6) | (*s++ & 0x3F);
    *sp = (const char*)s;
    return cp;
}
int utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}
size_t strlcpy(char* dst, const char* src, size_t size) {
    size_t n = strlen(src);
    if (size) { size_t k = n < size - 1 ? n : size - 1; memcpy(dst, src, k); dst[k] = 0; }
    return n;
}

static bool write_ppm(const Canvas& cv, const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    int w = cv.width(), h = cv.height();
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<unsigned char> buf((size_t)w * 3);
    for (int y = 0; y < h; y++) {
        const uint32_t* row = cv.row(y);
        for (int x = 0; x < w; x++) {
            uint32_t p = row[x];
            int a = (int)(p >> 24);
            int ch[3] = {(int)((p >> 16) & 255), (int)((p >> 8) & 255), (int)(p & 255)};
            for (int k = 0; k < 3; k++) {
                int v = ch[k] + (255 - a);   // premultiplied over white
                buf[x * 3 + k] = (unsigned char)(v > 255 ? 255 : v);
            }
        }
        fwrite(buf.data(), 1, buf.size(), f);
    }
    fclose(f);
    return true;
}

struct Case {
    const char* name;
    std::string svg;
    int size;
};

// Google "G" built from classic filled paths (48x48 viewBox).
static const char* kGoogleG = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
<path fill="#EA4335" d="M24 9.5c3.54 0 6.71 1.22 9.21 3.6l6.85-6.85C35.9 2.38 30.47 0 24 0 14.62 0 6.51 5.38 2.56 13.22l7.98 6.19C12.43 13.72 17.74 9.5 24 9.5z"/>
<path fill="#4285F4" d="M46.98 24.55c0-1.57-.15-3.09-.38-4.55H24v9.02h12.94c-.58 2.96-2.26 5.48-4.78 7.18l7.73 6c4.51-4.18 7.09-10.36 7.09-17.65z"/>
<path fill="#FBBC05" d="M10.53 28.59c-.48-1.45-.76-2.99-.76-4.59s.27-3.14.76-4.59l-7.98-6.19C.92 16.46 0 20.12 0 24c0 3.88.92 7.54 2.56 10.78l7.97-6.19z"/>
<path fill="#34A853" d="M24 48c6.48 0 11.93-2.13 15.89-5.81l-7.73-6c-2.15 1.45-4.92 2.3-8.16 2.3-6.26 0-11.57-4.22-13.47-9.91l-7.98 6.19C6.51 42.62 14.62 48 24 48z"/>
</svg>)SVG";

// Google-style "G" from arcs (relative and absolute), stroked with round joins.
static const char* kArcG = R"SVG(<svg viewBox="0 0 48 48">
<path d="M40.7 15.5a18 18 0 1 0 1.3 8.5H24" fill="none" stroke="#4285F4" stroke-width="6" stroke-linecap="butt"/>
<path d="M40.7 15.5 A18 18 0 1 0 42 24" fill="none" stroke="#EA4335" stroke-opacity="0.35" stroke-width="2"/>
</svg>)SVG";

// Heart with cubic curves (Material "favorite" icon, 24x24).
static const char* kHeart = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="240" height="240" viewBox="0 0 24 24">
<path fill="#E0245E" d="M12 21.35l-1.45-1.32C5.4 15.36 2 12.28 2 8.5 2 5.42 4.42 3 7.5 3c1.74 0 3.41.81 4.5 2.09C13.09 3.81 14.76 3 16.5 3 19.58 3 22 5.42 22 8.5c0 3.78-3.4 6.86-8.55 11.54L12 21.35z"/>
</svg>)SVG";

// Curve commands S, Q, T and relative/absolute mixes.
static const char* kCurves = R"SVG(<svg viewBox="0 0 240 120">
<path d="M10 80 Q 52.5 10, 95 80 T 180 80 S 230 150 250 80" fill="none" stroke="#1d4ed8" stroke-width="6"/>
<path d="m20 100 q 30 -40 60 0 t 60 0 s 20 -20 40 0" fill="none" stroke="#f59e0b" stroke-width="4" stroke-linecap="round"/>
</svg>)SVG";

// Hugging Face-like smiley: circles plus stroked paths, style attribute and a group transform.
static const char* kSmiley = R"SVG(<svg viewBox="0 0 72 72">
<g transform="translate(36 36) scale(1.0) translate(-36 -36)">
<circle cx="36" cy="36" r="34" style="fill:#FFD21E;stroke:#C98F00;stroke-width:1.5"/>
<ellipse cx="25" cy="30" rx="4" ry="5" fill="#000"/>
<ellipse cx="47" cy="30" rx="4" ry="5" fill="#000"/>
<circle cx="15" cy="42" r="5" fill="#FF9D0B" opacity=".6"/>
<circle cx="57" cy="42" r="5" fill="#FF9D0B" opacity=".6"/>
<path d="M22 44 Q36 60 50 44" stroke="#000" stroke-width="2.5" fill="none" stroke-linecap="round"/>
</g>
</svg>)SVG";

// Donuts: evenodd (same direction) and nonzero (reversed inner) via arc subpaths.
static const char* kDonutEvenodd = R"SVG(<svg viewBox="0 0 100 100">
<path fill-rule="evenodd" fill="#2a7" d="M50 10 A40 40 0 1 1 49.99 10 Z M50 70 A20 20 0 1 1 50.01 70 Z"/>
</svg>)SVG";
static const char* kDonutNonzero = R"SVG(<svg viewBox="0 0 100 100">
<path fill="#7a2" d="M50 10 A40 40 0 1 1 49.99 10 Z M50 30 A20 20 0 1 0 50.01 30 Z"/>
</svg>)SVG";

// Shape zoo: rect with rx, polygon, polyline stroke, line, currentColor, rgb(), #rgb, named colors, rotation.
static const char* kShapes = R"SVG(<svg width="200" height="120" viewBox="0 0 200 120">
<rect x="5" y="5" width="50" height="40" rx="10" fill="#f0f" stroke="black" stroke-width="3"/>
<polygon points="80,5 100,45 60,45" fill="rgb(0, 128, 255)" fill-opacity="0.8"/>
<polyline points="110,5 130,45 150,5 170,45" fill="none" stroke="#0a0" stroke-width="5"/>
<line x1="5" y1="110" x2="195" y2="110" stroke="currentColor" stroke-width="2"/>
<rect x="150" y="55" width="40" height="40" fill="currentColor" transform="rotate(30 170 75)"/>
<g fill="none" stroke="steelblue" stroke-width="4" style="stroke-opacity:0.5"><circle cx="30" cy="80" r="20"/></g>
<rect x="70" y="60" width="60" height="30" fill="#f80" style="fill:#f4c20d;opacity:0.9"/>
</svg>)SVG";

// Malformed / adversarial inputs: must not crash or hang.
static const char* kMalformed[] = {
    "",
    "<",
    "<svg",
    "<svg width='",
    "<svg><path d='M'/></svg>",
    "<svg><path d='L 1 2 3 4 Z 5 6'/></svg>",
    "<svg><path d='M 1e999999999999 2 L 3 4'/></svg>",
    "<svg><path d='M 0 0 A 0 0 0 0 0 1e30 1e30 A 1 1 0 1 1 -1e30 5'/></svg>",
    "<svg><path d='M 0 0 C 1e30 1e30 -1e30 1e30 5 5 Q nan 3'/></svg>",
    "<svg><path d='M10 10 H 1e38 V 1e38 Z' stroke='red' stroke-width='1e30'/></svg>",
    "<svg><rect width='-5' height='100'/><circle r='0'/><ellipse rx='3' ry='-1'/></svg>",
    "<svg viewBox='0 0 0 0'><rect width='10' height='10'/></svg>",
    "<svg viewBox='1 2 3'><rect width='10' height='10'/></svg>",
    "<svg><g transform='scale(0)'><rect width='10' height='10'/></g></svg>",
    "<svg><g transform='matrix(1 2 3'><rect width='10' height='10'/></g></svg>",
    "<svg><g transform='rotate(1e300)scale(1e-300)'><rect width='10' height='10'/></g></svg>",
    "<svg><path fill='url(#nope) red' d='M0 0h10v10z'/></svg>",
    "<svg><path fill='url(#' d='M0 0h10v10z'/></svg>",
    "<svg><path style='fill' d='M0 0h10v10z' fill='rgb(1,2'/></svg>",
    "<svg><path style=';;:::;;' fill='#12' stroke='#1234567890' d='M0 0h10v10z'/></svg>",
    "<svg><!-- unterminated comment <rect/></svg>",
    "<svg><![CDATA[ <rect/> ]]><text>hi</text><style>rect{fill:red}</style></svg>",
    "<?xml version='1.0'?><!DOCTYPE svg [ <!ENTITY a 'b'> ]><svg/>",
    "<svg><unknown><rect/></unknown><rect width='5' height='5'/></svg>",
    "<svg><path d='M 0 0 L 10 10' stroke-width='nan' stroke='red'/></svg>",
    "<svg width='1e9' height='1e9'><rect width='1' height='1'/></svg>",
    "<html><body><svg/></body></html>",
    "<svg><defs><linearGradient id='g'><stop offset='0' stop-color='#ff0000'/></linearGradient></defs>"
    "<path fill='url(#g)' d='M0 0h24v24z'/></svg>",
};

// Deterministic pseudo-random SVG-ish text for fuzzing.
struct Lcg {
    unsigned long long s;
    unsigned next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return (unsigned)(s >> 33);
    }
};

int main(int argc, char** argv) {
    std::string out = argc > 1 ? argv[1] : ".";
    std::vector<Case> cases = {
        {"google_g", kGoogleG, 256},
        {"arc_g", kArcG, 256},
        {"heart", kHeart, 256},
        {"curves", kCurves, 480},
        {"smiley", kSmiley, 256},
        {"donut_evenodd", kDonutEvenodd, 256},
        {"donut_nonzero", kDonutNonzero, 256},
        {"shapes", kShapes, 400},
    };
    int failures = 0;
    for (const Case& c : cases) {
        int iw = -1, ih = -1;
        bool ok = svg::intrinsic_size(c.svg.data(), c.svg.size(), &iw, &ih);
        Canvas* cv = svg::render(c.svg.data(), c.svg.size(), c.size, c.size, 0xFFD02020);
        printf("%-14s intrinsic=%dx%d svg=%d render=%s\n", c.name, iw, ih, (int)ok, cv ? "ok" : "NULL");
        if (!cv) { failures++; continue; }
        if (!write_ppm(*cv, out + "/" + c.name + ".ppm")) failures++;
        delete cv;
    }

    // Malformed inputs: every one must return (either a canvas or nullptr).
    int n_malformed = (int)(sizeof(kMalformed) / sizeof(kMalformed[0]));
    for (int i = 0; i < n_malformed; i++) {
        const char* s = kMalformed[i];
        size_t len = strlen(s);
        int w = 0, h = 0;
        svg::intrinsic_size(s, len, &w, &h);
        Canvas* cv = svg::render(s, len, 64, 64);
        delete cv;
    }
    printf("malformed: %d inputs ok\n", n_malformed);

    // Truncations of a real document at every length.
    {
        std::string full = kShapes;
        for (size_t n = 0; n <= full.size(); n++) {
            Canvas* cv = svg::render(full.data(), n, 48, 48);
            delete cv;
        }
        printf("truncations: %zu ok\n", full.size() + 1);
    }

    // Random mutations built from SVG-like tokens.
    {
        static const char* toks[] = {"<", ">", "/", "=", "\"", "'", "path", "d=", "M", "L", "C", "A", "Z", "-", ".", "9", "e", "1e308",
                                     "(", ")", ",", " ", "\n", "fill", "#", "rgb(", "url(#", "transform", "scale", "matrix", "%",
                                     "<g>", "</g>", "<svg>", "</svg>", "<rect/>", "style", ";", ":", "&amp;", "<!--", "-->"};
        const int ntok = (int)(sizeof(toks) / sizeof(toks[0]));
        Lcg rng{12345};
        int iters = 3000;
        for (int it = 0; it < iters; it++) {
            std::string s = "<svg viewBox='0 0 24 24'>";
            int len = 5 + (int)(rng.next() % 60);
            for (int k = 0; k < len; k++) s += toks[rng.next() % ntok];
            s += "</svg>";
            int w = 0, h = 0;
            svg::intrinsic_size(s.data(), s.size(), &w, &h);
            Canvas* cv = svg::render(s.data(), s.size(), 40, 40);
            delete cv;
        }
        printf("fuzz: %d random documents ok\n", iters);
    }

    // Element cap and deep nesting.
    {
        std::string many = "<svg viewBox='0 0 10 10'>";
        for (int i = 0; i < 30000; i++) many += "<rect x='1' y='1' width='2' height='2' fill='#123'/>";
        many += "</svg>";
        Canvas* cv = svg::render(many.data(), many.size(), 64, 64);
        printf("element cap (30000 rects): %s\n", cv ? "ok" : "NULL");
        delete cv;

        std::string deep = "<svg viewBox='0 0 10 10'>";
        for (int i = 0; i < 2000; i++) deep += "<g fill='red'>";
        deep += "<rect width='10' height='10'/>";
        for (int i = 0; i < 2000; i++) deep += "</g>";
        deep += "</svg>";
        cv = svg::render(deep.data(), deep.size(), 64, 64);
        printf("deep nesting (2000 groups): %s\n", cv ? "ok" : "NULL");
        delete cv;
    }

    printf(failures ? "FAILED: %d\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
