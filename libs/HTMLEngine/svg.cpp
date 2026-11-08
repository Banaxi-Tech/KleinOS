// SVG rasterizer. See svg.h.
//
// Pipeline: a streaming tokenizer walks the document once (plus a small
// pre-pass that records gradient first-stop colors). Each element computes its
// style and CTM from the parent frame; shapes are flattened straight into device
// space as contours, then either filled or expanded into stroke quads/circles
// and filled with the same scanline rasterizer (nonzero or evenodd).
//
// Robustness: every loop advances the input cursor, element count, nesting
// depth, attribute count and flattened point count are capped, and numbers are
// scanned with a bounded parser (no exponent loops).
#include "svg.h"
#include "lib/kstring.h"
#include "lib/vector.h"
#include "lib/string.h"
#include "lib/math.h"
#include "gfx/color.h"

namespace svg {
namespace {

constexpr int kMaxElements = 20000;
constexpr int kMaxDepth = 256;
constexpr int kMaxAttrs = 64;
constexpr int kMaxDim = 8192;
constexpr long kMaxPixels = 16L * 1024 * 1024;
constexpr size_t kMaxPoints = 400000;
constexpr size_t kMaxGrads = 4096;
constexpr float kTolPx = 0.2f;   // curve/arc flattening tolerance in device pixels
constexpr float kTwoPi = 6.28318530717959f;
constexpr float kPi = 3.14159265358979f;

constexpr uint8_t kPaintNone = 0, kPaintSolid = 1, kPaintCurrent = 2;

// ---------------------------------------------------------------- basics

inline bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool is_name_start(char c) { return is_alpha(c) || c == '_' || c == ':'; }
inline bool is_name_char(char c) { return !is_ws(c) && c != '/' && c != '>' && c != '=' && c != '<'; }
inline bool fin(float v) { return v == v && v < 3.0e38f && v > -3.0e38f; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// NaN-safe clamp of a float to an int range (does not truncate toward a value outside [lo, hi]).
inline int clampi(float v, int lo, int hi) {
    if (!(v >= (float)lo)) return lo;
    if (v > (float)hi) return hi;
    return (int)v;
}
// Sanitize a device coordinate: NaN -> 0, clamp to a range far outside any canvas.
inline float sane(float v) {
    if (v != v) return 0;
    return clampf(v, -1.0e7f, 1.0e7f);
}
inline bool eq(const char* a, const char* b) { return strcmp(a, b) == 0; }

const char* skip_ws(const char* p, const char* e) {
    while (p < e && is_ws(*p)) p++;
    return p;
}
const char* skip_seps(const char* p, const char* e) {
    while (p < e && (is_ws(*p) || *p == ',')) p++;
    return p;
}
const char* rtrim(const char* p, const char* e) {
    while (e > p && is_ws(e[-1])) e--;
    return e;
}
String trim_str(const char* a, const char* b) {
    a = skip_ws(a, b);
    b = rtrim(a, b);
    return String(a, (size_t)(b - a));
}
// Case-insensitive comparison of a whole trimmed value against a keyword.
bool value_is(const char* v, size_t n, const char* kw) {
    const char* e = v + n;
    v = skip_ws(v, e);
    e = rtrim(v, e);
    size_t k = strlen(kw);
    return (size_t)(e - v) == k && strncasecmp(v, kw, k) == 0;
}

// Scan one number (leading separators skipped). Bounded: no exponent loops.
bool scan_num(const char*& p, const char* e, float& out) {
    p = skip_seps(p, e);
    const char* s = p;
    bool neg = false;
    if (p < e && (*p == '+' || *p == '-')) { neg = *p == '-'; p++; }
    double mant = 0;
    int e10 = 0;
    bool any = false;
    while (p < e && is_digit(*p)) {
        any = true;
        if (mant < 1e17) mant = mant * 10 + (*p - '0'); else e10++;
        p++;
    }
    if (p < e && *p == '.') {
        p++;
        while (p < e && is_digit(*p)) {
            any = true;
            if (mant < 1e17) { mant = mant * 10 + (*p - '0'); e10--; }
            p++;
        }
    }
    if (!any) { p = s; return false; }
    if (p < e && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        bool eneg = false;
        if (q < e && (*q == '+' || *q == '-')) { eneg = *q == '-'; q++; }
        if (q < e && is_digit(*q)) {
            int ev = 0;
            while (q < e && is_digit(*q)) { if (ev < 100000) ev = ev * 10 + (*q - '0'); q++; }
            e10 += eneg ? -ev : ev;
            p = q;
        }
    }
    double v = mant;
    if (v != 0) {
        int k = e10 < 0 ? -e10 : e10;
        if (k > 400) k = 400;
        double m = 1;
        while (k-- > 0) m *= 10;
        v = e10 < 0 ? v / m : v * m;
    }
    float f = (float)(neg ? -v : v);
    if (!fin(f)) { p = s; return false; }
    out = f;
    return true;
}

// A complete length-like attribute: number plus optional unit letters. Percent fails.
bool parse_len(const char* s, size_t n, float& out) {
    const char* p = s;
    const char* e = s + n;
    if (!scan_num(p, e, out)) return false;
    p = skip_ws(p, e);
    while (p < e && is_alpha(*p)) p++;
    p = skip_ws(p, e);
    return p == e;
}

bool parse_opacity(const char* v, size_t n, float& out) {
    const char* p = v;
    const char* e = v + n;
    float x;
    if (!scan_num(p, e, x)) return false;
    p = skip_ws(p, e);
    if (p < e && *p == '%') { x /= 100.0f; p++; }
    p = skip_ws(p, e);
    if (p != e) return false;
    out = clampf(x, 0, 1);
    return true;
}

// ---------------------------------------------------------------- XML tokenizer

struct Attr {
    String name, value;
};

struct Tok {
    enum Kind : uint8_t { Start, End, Eof };
    Kind kind = Eof;
    bool selfclose = false;
    String name;
    Vector<Attr> attrs;
};

class Tokenizer {
public:
    Tokenizer(const char* s, size_t n) : s_(s), n_(n) {}

    // Next start/end tag. Text, comments, PIs, CDATA and DOCTYPE are skipped.
    void next(Tok& t) {
        t.attrs.clear();
        t.selfclose = false;
        for (;;) {
            while (p_ < n_ && s_[p_] != '<') p_++;
            if (p_ >= n_) { t.kind = Tok::Eof; return; }
            if (at("<!--")) { p_ += 4; skip_past("-->"); continue; }
            if (at("<![CDATA[")) { p_ += 9; skip_past("]]>"); continue; }
            if (at("<?")) { p_ += 2; skip_past("?>"); continue; }
            if (at("<!")) { skip_decl(); continue; }
            if (at("</")) {
                p_ += 2;
                read_name(t.name);
                while (p_ < n_ && s_[p_] != '>') p_++;
                if (p_ < n_) p_++;
                t.kind = Tok::End;
                return;
            }
            if (p_ + 1 < n_ && is_name_start(s_[p_ + 1])) {
                p_++;
                read_name(t.name);
                parse_attrs(t);
                t.kind = Tok::Start;
                return;
            }
            p_++;   // stray '<'
        }
    }

private:
    bool at(const char* lit) const {
        size_t k = strlen(lit);
        return p_ + k <= n_ && memcmp(s_ + p_, lit, k) == 0;
    }
    void skip_past(const char* lit) {
        size_t k = strlen(lit);
        for (size_t i = p_; i + k <= n_; i++) {
            if (memcmp(s_ + i, lit, k) == 0) { p_ = i + k; return; }
        }
        p_ = n_;
    }
    void skip_ws() {
        while (p_ < n_ && is_ws(s_[p_])) p_++;
    }
    void skip_decl() {
        p_ += 2;
        int depth = 0;
        while (p_ < n_) {
            char c = s_[p_++];
            if (c == '[') depth++;
            else if (c == ']') depth--;
            else if (c == '>' && depth <= 0) break;
        }
    }
    void read_name(String& out) {
        size_t ns = p_;
        while (p_ < n_ && is_name_char(s_[p_])) p_++;
        out = String(s_ + ns, p_ - ns);
    }
    void parse_attrs(Tok& t) {
        for (;;) {
            skip_ws();
            if (p_ >= n_) return;
            char c = s_[p_];
            if (c == '>') { p_++; return; }
            if (c == '<') return;
            if (c == '/') {
                if (p_ + 1 < n_ && s_[p_ + 1] == '>') { t.selfclose = true; p_ += 2; return; }
                p_++;
                continue;
            }
            size_t ns = p_;
            while (p_ < n_ && is_name_char(s_[p_])) p_++;
            if (p_ == ns) { p_++; continue; }   // e.g. a lone '='
            String name(s_ + ns, p_ - ns);
            skip_ws();
            String value;
            if (p_ < n_ && s_[p_] == '=') {
                p_++;
                skip_ws();
                if (p_ < n_ && (s_[p_] == '"' || s_[p_] == '\'')) {
                    char q = s_[p_++];
                    size_t vs = p_;
                    while (p_ < n_ && s_[p_] != q) p_++;
                    value = String(s_ + vs, p_ - vs);
                    if (p_ < n_) p_++;
                } else {
                    size_t vs = p_;
                    while (p_ < n_ && !is_ws(s_[p_]) && s_[p_] != '>') p_++;
                    value = String(s_ + vs, p_ - vs);
                }
            }
            if (t.attrs.size() < (size_t)kMaxAttrs) t.attrs.push(Attr{std::move(name), std::move(value)});
        }
    }

    const char* s_;
    size_t n_;
    size_t p_ = 0;
};

const String* find_attr(const Vector<Attr>& a, const char* name) {
    for (size_t i = 0; i < a.size(); i++) {
        if (eq(a[i].name.c_str(), name)) return &a[i].value;
    }
    return nullptr;
}

bool attr_len(const Vector<Attr>& a, const char* name, float& out) {
    const String* v = find_attr(a, name);
    return v && parse_len(v->c_str(), v->size(), out);
}

// Iterates "name: value; ..." declarations. Returns false at the end.
bool next_decl(const char*& p, const char* e, String& name, String& value) {
    while (p < e) {
        const char* ds = p;
        while (p < e && *p != ';') p++;
        const char* de = p;
        if (p < e) p++;
        const char* colon = ds;
        while (colon < de && *colon != ':') colon++;
        if (colon >= de) continue;
        name = trim_str(ds, colon);
        value = trim_str(colon + 1, de);
        if (name.empty()) continue;
        return true;
    }
    return false;
}

// Value of a property from presentation attributes, overridden by style="".
bool prop_value(const Vector<Attr>& attrs, const char* prop, String& out) {
    bool found = false;
    for (size_t i = 0; i < attrs.size(); i++) {
        if (eq(attrs[i].name.c_str(), prop)) { out = attrs[i].value; found = true; }
    }
    for (size_t i = 0; i < attrs.size(); i++) {
        if (!eq(attrs[i].name.c_str(), "style")) continue;
        const char* p = attrs[i].value.c_str();
        const char* e = p + attrs[i].value.size();
        String nm, vl;
        while (next_decl(p, e, nm, vl)) {
            if (eq(nm.c_str(), prop)) { out = vl; found = true; }
        }
    }
    return found;
}

// ---------------------------------------------------------------- colors and paint

int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

struct NamedColor {
    const char* name;
    uint32_t rgb;
};
constexpr NamedColor kNamed[] = {
    {"black", 0x000000}, {"silver", 0xc0c0c0}, {"gray", 0x808080}, {"grey", 0x808080},
    {"white", 0xffffff}, {"maroon", 0x800000}, {"red", 0xff0000}, {"purple", 0x800080},
    {"fuchsia", 0xff00ff}, {"magenta", 0xff00ff}, {"green", 0x008000}, {"lime", 0x00ff00},
    {"olive", 0x808000}, {"yellow", 0xffff00}, {"navy", 0x000080}, {"blue", 0x0000ff},
    {"teal", 0x008080}, {"aqua", 0x00ffff}, {"cyan", 0x00ffff}, {"orange", 0xffa500},
    {"brown", 0xa52a2a}, {"pink", 0xffc0cb}, {"gold", 0xffd700}, {"darkgray", 0xa9a9a9},
    {"darkgrey", 0xa9a9a9}, {"lightgray", 0xd3d3d3}, {"lightgrey", 0xd3d3d3}, {"darkblue", 0x00008b},
    {"darkgreen", 0x006400}, {"darkred", 0x8b0000}, {"indigo", 0x4b0082}, {"violet", 0xee82ee},
    {"coral", 0xff7f50}, {"tomato", 0xff6347}, {"crimson", 0xdc143c}, {"salmon", 0xfa8072},
    {"khaki", 0xf0e68c}, {"skyblue", 0x87ceeb}, {"steelblue", 0x4682b4}, {"royalblue", 0x4169e1},
    {"slategray", 0x708090}, {"dimgray", 0x696969}, {"whitesmoke", 0xf5f5f5}, {"beige", 0xf5f5dc},
    {"ivory", 0xfffff0}, {"lavender", 0xe6e6fa}, {"plum", 0xdda0dd}, {"orchid", 0xda70d6},
    {"tan", 0xd2b48c}, {"chocolate", 0xd2691e}, {"sienna", 0xa0522d}, {"turquoise", 0x40e0d0},
    {"hotpink", 0xff69b4}, {"deeppink", 0xff1493}, {"limegreen", 0x32cd32}, {"forestgreen", 0x228b22},
};

// Parses #rgb, #rgba, #rrggbb, #rrggbbaa, rgb(), rgba(), named colors, transparent.
bool parse_color(const char* v, size_t n, Color& out) {
    const char* e = v + n;
    const char* s = skip_ws(v, e);
    e = rtrim(s, e);
    size_t len = (size_t)(e - s);
    if (len == 0) return false;
    if (s[0] == '#') {
        size_t nd = len - 1;
        if (nd != 3 && nd != 4 && nd != 6 && nd != 8) return false;
        int d[8];
        for (size_t i = 0; i < nd; i++) {
            d[i] = hexv(s[1 + i]);
            if (d[i] < 0) return false;
        }
        int c[4] = {0, 0, 0, 255};
        size_t nc = nd <= 4 ? nd : nd / 2;
        for (size_t k = 0; k < nc; k++) c[k] = nd <= 4 ? d[k] * 17 : d[2 * k] * 16 + d[2 * k + 1];
        out = rgba(c[0], c[1], c[2], c[3]);
        return true;
    }
    if (len >= 3 && strncasecmp(s, "rgb", 3) == 0) {
        const char* p = s + 3;
        if (p < e && (*p == 'a' || *p == 'A')) p++;
        p = skip_ws(p, e);
        if (p >= e || *p != '(') return false;
        p++;
        float ch[4] = {0, 0, 0, 0};
        bool pct[4] = {false, false, false, false};
        int cnt = 0;
        for (;;) {
            p = skip_seps(p, e);
            if (p < e && *p == ')') break;
            if (cnt >= 4) return false;
            float x;
            if (!scan_num(p, e, x)) return false;
            p = skip_ws(p, e);
            if (p < e && *p == '%') { pct[cnt] = true; p++; }
            ch[cnt++] = x;
        }
        if (p >= e) return false;
        p++;
        if (cnt != 3 && cnt != 4) return false;
        int c[4];
        for (int k = 0; k < 3; k++) {
            float cv = pct[k] ? ch[k] * 2.55f : ch[k];
            c[k] = (int)(clampf(cv, 0, 255) + 0.5f);
        }
        float a = cnt == 4 ? (pct[3] ? ch[3] / 100.0f : ch[3]) : 1.0f;
        c[3] = (int)(clampf(a, 0, 1) * 255.0f + 0.5f);
        out = rgba(c[0], c[1], c[2], c[3]);
        return true;
    }
    if (len < 24) {
        char buf[24];
        memcpy(buf, s, len);
        buf[len] = 0;
        if (strcasecmp(buf, "transparent") == 0) { out = 0; return true; }
        for (const NamedColor& nc : kNamed) {
            if (strcasecmp(buf, nc.name) == 0) { out = hex(nc.rgb); return true; }
        }
    }
    return false;
}

struct GradRef {
    String id;
    Color color = 0;
};

struct Doc {
    const Vector<GradRef>* grads = nullptr;
    Color current = 0xFF000000;
};

struct Paint {
    uint8_t kind = kPaintNone;
    Color c = 0;
};

bool find_grad(const Doc& d, const char* id, Color& out) {
    if (!d.grads) return false;
    for (size_t i = 0; i < d.grads->size(); i++) {
        if (eq((*d.grads)[i].id.c_str(), id)) { out = (*d.grads)[i].color; return true; }
    }
    return false;
}

// Parses url(#id) at the start of v. `after` points past the closing paren.
bool parse_url(const char* v, size_t n, String& id, const char*& after) {
    const char* p = v;
    const char* e = v + n;
    if (n < 4 || strncasecmp(p, "url(", 4) != 0) return false;
    p = skip_ws(p + 4, e);
    if (p < e && (*p == '\'' || *p == '"')) p++;
    p = skip_ws(p, e);
    if (p >= e || *p != '#') return false;
    p++;
    const char* s = p;
    while (p < e && *p != ')' && *p != '\'' && *p != '"' && !is_ws(*p)) p++;
    id = String(s, (size_t)(p - s));
    while (p < e && *p != ')') p++;
    if (p < e) p++;
    after = p;
    return true;
}

bool parse_paint(const char* v, size_t n, const Doc& doc, Paint& out, bool allow_url) {
    const char* e = v + n;
    const char* s = skip_ws(v, e);
    e = rtrim(s, e);
    size_t len = (size_t)(e - s);
    if (len == 0) return false;
    if (value_is(s, len, "none")) { out = Paint{kPaintNone, 0}; return true; }
    if (value_is(s, len, "currentColor")) { out = Paint{kPaintCurrent, 0}; return true; }
    if (allow_url && len >= 4 && strncasecmp(s, "url(", 4) == 0) {
        String id;
        const char* after = s;
        if (!parse_url(s, len, id, after)) return false;
        Color c = 0;
        if (find_grad(doc, id.c_str(), c)) { out = Paint{kPaintSolid, c}; return true; }
        const char* fb = skip_ws(after, e);
        if (fb < e && parse_paint(fb, (size_t)(e - fb), doc, out, false)) return true;
        out = Paint{kPaintNone, 0};
        return true;
    }
    Color c = 0;
    if (!parse_color(s, len, c)) return false;
    out = Paint{kPaintSolid, c};
    return true;
}

// Straight color with `op` applied to its alpha, converted to premultiplied.
uint32_t premul_alpha(Color c, float op) {
    float a = (float)color_a(c) * clampf(op, 0, 1) + 0.5f;
    int ai = (int)a;
    if (ai <= 0) return 0;
    if (ai > 255) ai = 255;
    return premul(with_alpha(c, ai));
}

// ---------------------------------------------------------------- transforms

struct Mat {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

// Returns A * B (apply B first, then A).
Mat mmul(const Mat& A, const Mat& B) {
    Mat r;
    r.a = A.a * B.a + A.c * B.b;
    r.b = A.b * B.a + A.d * B.b;
    r.c = A.a * B.c + A.c * B.d;
    r.d = A.b * B.c + A.d * B.d;
    r.e = A.a * B.e + A.c * B.f + A.e;
    r.f = A.b * B.e + A.d * B.f + A.f;
    return r;
}

// Largest axis scale factor of m (used to convert user lengths to device px).
float mscale(const Mat& m) {
    float s1 = sqrtf(m.a * m.a + m.b * m.b);
    float s2 = sqrtf(m.c * m.c + m.d * m.d);
    return s1 > s2 ? s1 : s2;
}

Mat mtranslate(float x, float y) { return Mat{1, 0, 0, 1, x, y}; }

bool name_is(const char* ns, size_t nl, const char* lit) {
    return nl == strlen(lit) && memcmp(ns, lit, nl) == 0;
}

// Builds one transform function. Returns false on wrong arity or unknown name.
bool build_op(const char* ns, size_t nl, const float* a, int na, Mat& op) {
    if (name_is(ns, nl, "matrix")) {
        if (na != 6) return false;
        op = Mat{a[0], a[1], a[2], a[3], a[4], a[5]};
        return true;
    }
    if (name_is(ns, nl, "translate")) {
        if (na == 1) op = mtranslate(a[0], 0);
        else if (na == 2) op = mtranslate(a[0], a[1]);
        else return false;
        return true;
    }
    if (name_is(ns, nl, "scale")) {
        if (na == 1) op = Mat{a[0], 0, 0, a[0], 0, 0};
        else if (na == 2) op = Mat{a[0], 0, 0, a[1], 0, 0};
        else return false;
        return true;
    }
    if (name_is(ns, nl, "rotate")) {
        if (na != 1 && na != 3) return false;
        double rad = (double)a[0] * (3.14159265358979 / 180.0);
        float cs = (float)cos(rad), sn = (float)sin(rad);
        Mat r{cs, sn, -sn, cs, 0, 0};
        if (na == 3) {
            r = mmul(mtranslate(a[1], a[2]), mmul(r, mtranslate(-a[1], -a[2])));
        }
        op = r;
        return true;
    }
    if (name_is(ns, nl, "skewX")) {
        if (na != 1) return false;
        op = Mat{1, 0, (float)tan((double)a[0] * (3.14159265358979 / 180.0)), 1, 0, 0};
        return true;
    }
    if (name_is(ns, nl, "skewY")) {
        if (na != 1) return false;
        op = Mat{1, (float)tan((double)a[0] * (3.14159265358979 / 180.0)), 0, 1, 0, 0};
        return true;
    }
    return false;
}

// Parses a transform list. Stops at the first malformed function, keeping the prefix.
Mat parse_transform(const char* s, size_t n) {
    Mat r;
    const char* p = s;
    const char* e = s + n;
    for (;;) {
        p = skip_seps(p, e);
        if (p >= e) break;
        const char* ns = p;
        while (p < e && is_alpha(*p)) p++;
        if (p == ns) break;
        size_t nl = (size_t)(p - ns);
        p = skip_ws(p, e);
        if (p >= e || *p != '(') break;
        p++;
        float a[6] = {0, 0, 0, 0, 0, 0};
        int na = 0;
        bool bad = false;
        for (;;) {
            p = skip_seps(p, e);
            if (p < e && *p == ')') { p++; break; }
            float v;
            if (na >= 6 || !scan_num(p, e, v)) { bad = true; break; }
            a[na++] = v;
        }
        if (bad) break;
        Mat op;
        if (!build_op(ns, nl, a, na, op)) break;
        r = mmul(r, op);
    }
    return r;
}

Mat transform_of(const Vector<Attr>& a) {
    const String* t = find_attr(a, "transform");
    return t ? parse_transform(t->c_str(), t->size()) : Mat();
}

struct ViewBox {
    float x = 0, y = 0, w = 0, h = 0;
};

bool get_viewbox(const Vector<Attr>& attrs, ViewBox& vb) {
    const String* v = find_attr(attrs, "viewBox");
    if (!v) return false;
    const char* p = v->c_str();
    const char* e = p + v->size();
    float a[4];
    for (int i = 0; i < 4; i++) {
        if (!scan_num(p, e, a[i])) return false;
    }
    if (!(a[2] > 0 && a[3] > 0)) return false;
    vb = ViewBox{a[0], a[1], a[2], a[3]};
    return true;
}

// Maps a viewBox into a vw x vh viewport (preserveAspectRatio; default xMidYMid meet).
Mat viewbox_matrix(const ViewBox& vb, float vw, float vh, const char* par) {
    float sx = vw / vb.w, sy = vh / vb.h;
    size_t pl = strlen(par);
    const char* p = skip_ws(par, par + pl);
    pl = (size_t)((par + pl) - p);
    if (pl >= 4 && strncmp(p, "none", 4) == 0) return Mat{sx, 0, 0, sy, -vb.x * sx, -vb.y * sy};
    bool slice = false;
    for (size_t i = 0; i + 5 <= pl; i++) {
        if (strncmp(p + i, "slice", 5) == 0) { slice = true; break; }
    }
    float s = slice ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    float fx = 0.5f, fy = 0.5f;
    if (pl >= 8 && p[0] == 'x' && p[1] == 'M') {
        if (strncmp(p + 1, "Min", 3) == 0) fx = 0;
        else if (strncmp(p + 1, "Max", 3) == 0) fx = 1;
        if (strncmp(p + 5, "Min", 3) == 0) fy = 0;
        else if (strncmp(p + 5, "Max", 3) == 0) fy = 1;
    }
    float tx = (vw - vb.w * s) * fx, ty = (vh - vb.h * s) * fy;
    return Mat{s, 0, 0, s, tx - vb.x * s, ty - vb.y * s};
}

// Width/height/viewBox of the root <svg>, with the 24x24 default.
void root_size(const Vector<Attr>& attrs, ViewBox& vb, bool& hasvb, float& W, float& H) {
    float ww = 0, hh = 0;
    bool hw = attr_len(attrs, "width", ww) && ww > 0;
    bool hhh = attr_len(attrs, "height", hh) && hh > 0;
    hasvb = get_viewbox(attrs, vb);
    if (!hasvb) {
        W = hw ? ww : 24.0f;
        H = hhh ? hh : 24.0f;
        vb = ViewBox{0, 0, W, H};
    } else if (hw && hhh) {
        W = ww;
        H = hh;
    } else if (hw) {
        W = ww;
        H = ww * vb.h / vb.w;
    } else if (hhh) {
        H = hh;
        W = hh * vb.w / vb.h;
    } else {
        W = vb.w;
        H = vb.h;
    }
}

int to_px(float v) {
    if (!(v >= 1)) return 1;
    if (v > (float)kMaxDim) return kMaxDim;
    return (int)(v + 0.5f);
}

// ---------------------------------------------------------------- geometry

// Device-space polygons. Each contour is pts[starts[i] .. starts[i+1]).
struct Contours {
    Vector<PointF> pts;
    Vector<int> starts;
    Vector<uint8_t> closed;

    void begin() {
        starts.push((int)pts.size());
        closed.push(0);
    }
    int count() const { return (int)starts.size(); }
    int first(int i) const { return starts[i]; }
    int last(int i) const { return i + 1 < count() ? starts[i + 1] : (int)pts.size(); }
    void mark_closed() {
        if (!closed.empty()) closed[closed.size() - 1] = 1;
    }
};

// Path builder: user-space commands in, device-space points out.
struct PB {
    Contours* g = nullptr;
    Mat m;
    float tol = kTolPx;   // flattening tolerance in user units
    float cx = 0, cy = 0, sx = 0, sy = 0;
    bool open = false;

    void emit(float x, float y) {
        if (g->pts.size() >= kMaxPoints) return;
        float X = m.a * x + m.c * y + m.e;
        float Y = m.b * x + m.d * y + m.f;
        g->pts.push(PointF{sane(X), sane(Y)});
    }
    void ensure() {
        if (!open) {
            g->begin();
            emit(cx, cy);
            open = true;
        }
    }
    void move(float x, float y) {
        g->begin();
        open = true;
        cx = sx = x;
        cy = sy = y;
        emit(x, y);
    }
    void line(float x, float y) {
        ensure();
        emit(x, y);
        cx = x;
        cy = y;
    }
    void cubic(float x1, float y1, float x2, float y2, float x, float y) {
        ensure();
        const float x0 = cx, y0 = cy;
        float ax = x0 - 2 * x1 + x2, ay = y0 - 2 * y1 + y2;
        float bx = x1 - 2 * x2 + x, by = y1 - 2 * y2 + y;
        float M = sqrtf(ax * ax + ay * ay);
        float M2 = sqrtf(bx * bx + by * by);
        if (M2 > M) M = M2;
        int n = clampi(ceilf(sqrtf(0.75f * M / tol)), 1, 256);
        for (int i = 1; i <= n; i++) {
            float t = (float)i / (float)n;
            float mt = 1 - t;
            float ca = mt * mt * mt, cb = 3 * mt * mt * t, cc = 3 * mt * t * t, cd = t * t * t;
            emit(ca * x0 + cb * x1 + cc * x2 + cd * x, ca * y0 + cb * y1 + cc * y2 + cd * y);
        }
        cx = x;
        cy = y;
    }
    void quad(float x1, float y1, float x, float y) {
        const float x0 = cx, y0 = cy;
        const float k = 2.0f / 3.0f;
        cubic(x0 + k * (x1 - x0), y0 + k * (y1 - y0), x + k * (x1 - x), y + k * (y1 - y), x, y);
    }
    // SVG endpoint arc (F.6), flattened in user space.
    void arc(float rx, float ry, float phi_deg, bool fa, bool fs, float x, float y) {
        ensure();
        const float x1 = cx, y1 = cy, x2 = x, y2 = y;
        if (x1 == x2 && y1 == y2) return;
        rx = fabsf(rx);
        ry = fabsf(ry);
        if (rx == 0 || ry == 0) { line(x, y); return; }
        const float phi = phi_deg * (kPi / 180.0f);
        const float cp = cosf(phi), sp = sinf(phi);
        const float dx2 = (x1 - x2) / 2, dy2 = (y1 - y2) / 2;
        const float x1p = cp * dx2 + sp * dy2;
        const float y1p = -sp * dx2 + cp * dy2;
        float lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
        if (lam > 1) {
            float s = sqrtf(lam);
            rx *= s;
            ry *= s;
        }
        const float rx2 = rx * rx, ry2 = ry * ry;
        float num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
        float den = rx2 * y1p * y1p + ry2 * x1p * x1p;
        float coef = 0;
        if (num > 0 && den > 0) coef = sqrtf(num / den);
        if (fa == fs) coef = -coef;
        const float cxp = coef * rx * y1p / ry;
        const float cyp = coef * -ry * x1p / rx;
        const float ccx = cp * cxp - sp * cyp + (x1 + x2) / 2;
        const float ccy = sp * cxp + cp * cyp + (y1 + y2) / 2;
        const float ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
        const float vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
        const float th1 = atan2f(uy, ux);
        float dth = atan2f(ux * vy - uy * vx, ux * vx + uy * vy);
        if (!fs && dth > 0) dth -= kTwoPi;
        else if (fs && dth < 0) dth += kTwoPi;
        const float rmax = rx > ry ? rx : ry;
        float step = sqrtf(8.0f * tol / rmax);
        if (!(step >= 0.005f)) step = 0.005f;
        int n = clampi(ceilf(fabsf(dth) / step), 1, 512);
        for (int i = 1; i <= n; i++) {
            if (i == n) { emit(x, y); break; }
            float th = th1 + dth * (float)i / (float)n;
            float ct = cosf(th), st = sinf(th);
            emit(ccx + rx * ct * cp - ry * st * sp, ccy + rx * ct * sp + ry * st * cp);
        }
        cx = x;
        cy = y;
    }
    void close() {
        if (!open) return;
        g->mark_closed();
        open = false;
        cx = sx;
        cy = sy;
    }
};

// Parses the d attribute. On a syntax error, keeps everything drawn so far.
void parse_path(const char* d, size_t n, PB& pb) {
    const char* p = d;
    const char* e = d + n;
    char cmd = 0;
    char prev = 0;            // previous command (uppercase) for S/T reflection
    float rcx = 0, rcy = 0;   // last control point (absolute)
    bool first = true;
    for (;;) {
        p = skip_seps(p, e);
        if (p >= e) return;
        if (is_alpha(*p)) {
            if (!strchr("MmLlHhVvCcSsQqTtAaZz", *p)) return;
            cmd = *p++;
        } else if (cmd == 0 || cmd == 'Z' || cmd == 'z') {
            return;
        }
        const bool rel = cmd >= 'a';
        const char up = rel ? (char)(cmd - 32) : cmd;
        if (first && up != 'M') return;
        first = false;
        float a[7] = {0, 0, 0, 0, 0, 0, 0};
        switch (up) {
        case 'Z':
            pb.close();
            prev = 'Z';
            break;
        case 'M':
            if (!scan_num(p, e, a[0]) || !scan_num(p, e, a[1])) return;
            if (rel) { a[0] += pb.cx; a[1] += pb.cy; }
            pb.move(a[0], a[1]);
            cmd = rel ? 'l' : 'L';   // further pairs are implicit lines
            prev = 'M';
            break;
        case 'L':
            if (!scan_num(p, e, a[0]) || !scan_num(p, e, a[1])) return;
            if (rel) { a[0] += pb.cx; a[1] += pb.cy; }
            pb.line(a[0], a[1]);
            prev = 'L';
            break;
        case 'H':
            if (!scan_num(p, e, a[0])) return;
            if (rel) a[0] += pb.cx;
            pb.line(a[0], pb.cy);
            prev = 'H';
            break;
        case 'V':
            if (!scan_num(p, e, a[1])) return;
            if (rel) a[1] += pb.cy;
            pb.line(pb.cx, a[1]);
            prev = 'V';
            break;
        case 'C':
            for (int i = 0; i < 6; i++) if (!scan_num(p, e, a[i])) return;
            if (rel) for (int i = 0; i < 6; i += 2) { a[i] += pb.cx; a[i + 1] += pb.cy; }
            pb.cubic(a[0], a[1], a[2], a[3], a[4], a[5]);
            rcx = a[2];
            rcy = a[3];
            prev = 'C';
            break;
        case 'S': {
            for (int i = 0; i < 4; i++) if (!scan_num(p, e, a[i])) return;
            if (rel) for (int i = 0; i < 4; i += 2) { a[i] += pb.cx; a[i + 1] += pb.cy; }
            float x1 = pb.cx, y1 = pb.cy;
            if (prev == 'C' || prev == 'S') { x1 = 2 * pb.cx - rcx; y1 = 2 * pb.cy - rcy; }
            pb.cubic(x1, y1, a[0], a[1], a[2], a[3]);
            rcx = a[0];
            rcy = a[1];
            prev = 'S';
            break;
        }
        case 'Q':
            for (int i = 0; i < 4; i++) if (!scan_num(p, e, a[i])) return;
            if (rel) for (int i = 0; i < 4; i += 2) { a[i] += pb.cx; a[i + 1] += pb.cy; }
            pb.quad(a[0], a[1], a[2], a[3]);
            rcx = a[0];
            rcy = a[1];
            prev = 'Q';
            break;
        case 'T': {
            for (int i = 0; i < 2; i++) if (!scan_num(p, e, a[i])) return;
            if (rel) { a[0] += pb.cx; a[1] += pb.cy; }
            float x1 = pb.cx, y1 = pb.cy;
            if (prev == 'Q' || prev == 'T') { x1 = 2 * pb.cx - rcx; y1 = 2 * pb.cy - rcy; }
            pb.quad(x1, y1, a[0], a[1]);
            rcx = x1;
            rcy = y1;
            prev = 'T';
            break;
        }
        case 'A': {
            float rx, ry, rot;
            bool fa, fs;
            if (!scan_num(p, e, rx) || !scan_num(p, e, ry) || !scan_num(p, e, rot)) return;
            p = skip_seps(p, e);
            if (p >= e || (*p != '0' && *p != '1')) return;
            fa = *p++ == '1';
            p = skip_seps(p, e);
            if (p >= e || (*p != '0' && *p != '1')) return;
            fs = *p++ == '1';
            if (!scan_num(p, e, a[0]) || !scan_num(p, e, a[1])) return;
            if (rel) { a[0] += pb.cx; a[1] += pb.cy; }
            pb.arc(rx, ry, rot, fa, fs, a[0], a[1]);
            prev = 'A';
            break;
        }
        default:
            return;
        }
    }
}

// Shape geometry in user space (already routed through pb.m).
void geom_ellipse(float cx, float cy, float rx, float ry, PB& pb) {
    if (!(rx > 0 && ry > 0)) return;
    float rmax = rx > ry ? rx : ry;
    float step = sqrtf(8.0f * pb.tol / rmax);
    int n = clampi(ceilf(kTwoPi / step), 8, 1024);
    pb.move(cx + rx, cy);
    for (int i = 1; i < n; i++) {
        float t = kTwoPi * (float)i / (float)n;
        pb.line(cx + rx * cosf(t), cy + ry * sinf(t));
    }
    pb.close();
}

void geom_rect(const Vector<Attr>& a, PB& pb) {
    float x = 0, y = 0, w = 0, h = 0, rx = 0, ry = 0;
    attr_len(a, "x", x);
    attr_len(a, "y", y);
    attr_len(a, "width", w);
    attr_len(a, "height", h);
    bool hrx = attr_len(a, "rx", rx);
    bool hry = attr_len(a, "ry", ry);
    if (!(w > 0 && h > 0)) return;
    if (!hrx && !hry) { rx = ry = 0; }
    else if (!hrx) rx = ry;
    else if (!hry) ry = rx;
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx > w * 0.5f) rx = w * 0.5f;
    if (ry > h * 0.5f) ry = h * 0.5f;
    if (rx <= 0 || ry <= 0) {
        pb.move(x, y);
        pb.line(x + w, y);
        pb.line(x + w, y + h);
        pb.line(x, y + h);
        pb.close();
        return;
    }
    pb.move(x + rx, y);
    pb.line(x + w - rx, y);
    pb.arc(rx, ry, 0, false, true, x + w, y + ry);
    pb.line(x + w, y + h - ry);
    pb.arc(rx, ry, 0, false, true, x + w - rx, y + h);
    pb.line(x + rx, y + h);
    pb.arc(rx, ry, 0, false, true, x, y + h - ry);
    pb.line(x, y + ry);
    pb.arc(rx, ry, 0, false, true, x + rx, y);
    pb.close();
}

void geom_points(const Vector<Attr>& a, bool polygon, PB& pb) {
    const String* pts = find_attr(a, "points");
    if (!pts) return;
    const char* p = pts->c_str();
    const char* e = p + pts->size();
    int cnt = 0;
    float x, y;
    while (scan_num(p, e, x)) {
        if (!scan_num(p, e, y)) break;
        if (cnt == 0) pb.move(x, y);
        else pb.line(x, y);
        cnt++;
    }
    if (polygon && cnt >= 2) pb.close();
}

void build_shape(const char* nm, const Vector<Attr>& a, PB& pb) {
    if (eq(nm, "path")) {
        const String* d = find_attr(a, "d");
        if (d) parse_path(d->c_str(), d->size(), pb);
    } else if (eq(nm, "rect")) {
        geom_rect(a, pb);
    } else if (eq(nm, "circle")) {
        float cx = 0, cy = 0, r = 0;
        attr_len(a, "cx", cx);
        attr_len(a, "cy", cy);
        attr_len(a, "r", r);
        geom_ellipse(cx, cy, r, r, pb);
    } else if (eq(nm, "ellipse")) {
        float cx = 0, cy = 0, rx = 0, ry = 0;
        attr_len(a, "cx", cx);
        attr_len(a, "cy", cy);
        attr_len(a, "rx", rx);
        attr_len(a, "ry", ry);
        geom_ellipse(cx, cy, rx, ry, pb);
    } else if (eq(nm, "line")) {
        float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        attr_len(a, "x1", x1);
        attr_len(a, "y1", y1);
        attr_len(a, "x2", x2);
        attr_len(a, "y2", y2);
        pb.move(x1, y1);
        pb.line(x2, y2);
    } else if (eq(nm, "polyline")) {
        geom_points(a, false, pb);
    } else if (eq(nm, "polygon")) {
        geom_points(a, true, pb);
    }
}

// ---------------------------------------------------------------- stroking

// Appends a closed contour, normalized to positive signed area so that all
// stroke pieces wind the same way and nonzero fill unions them.
void add_positive_poly(Contours& out, const PointF* p, int n) {
    float area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += p[i].x * p[j].y - p[j].x * p[i].y;
    }
    out.begin();
    if (area >= 0) {
        for (int i = 0; i < n; i++) out.pts.push(p[i]);
    } else {
        for (int i = n - 1; i >= 0; i--) out.pts.push(p[i]);
    }
    out.mark_closed();
}

void stroke_segment(Contours& out, PointF a, PointF b, float hw) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float l = sqrtf(dx * dx + dy * dy);
    if (!(l > 1e-6f)) return;
    float nx = -dy / l * hw, ny = dx / l * hw;
    PointF q[4] = {{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}};
    add_positive_poly(out, q, 4);
}

void stroke_disc(Contours& out, PointF c, float r) {
    if (!(r > 0)) return;
    float step = sqrtf(8.0f * kTolPx / r);
    int n = clampi(ceilf(kTwoPi / step), 8, 64);
    PointF q[64];
    for (int i = 0; i < n; i++) {
        float t = kTwoPi * (float)i / (float)n;
        q[i] = PointF{c.x + r * cosf(t), c.y + r * sinf(t)};
    }
    add_positive_poly(out, q, n);
}

// Expands each polyline into per-segment quads plus disc joins (and round caps).
void stroke_contours(const Contours& g, float hw, bool round_cap, Contours& out) {
    Vector<PointF> P;
    for (int ci = 0; ci < g.count(); ci++) {
        P.clear();
        for (int i = g.first(ci); i < g.last(ci); i++) {
            PointF q = g.pts[i];
            if (P.empty()) { P.push(q); continue; }
            PointF b = P.back();
            float dx = q.x - b.x, dy = q.y - b.y;
            if (dx * dx + dy * dy > 1e-6f) P.push(q);
        }
        const bool closed = g.closed[ci] != 0;
        if (closed && P.size() > 1) {
            PointF f = P[0], b = P.back();
            float dx = f.x - b.x, dy = f.y - b.y;
            if (dx * dx + dy * dy <= 1e-6f) P.pop();
        }
        const int n = (int)P.size();
        if (n < 2) continue;
        const int nseg = closed ? n : n - 1;
        for (int i = 0; i < nseg; i++) stroke_segment(out, P[i], P[(i + 1) % n], hw);
        for (int j = 0; j < n; j++) {
            if (!closed && (j == 0 || j == n - 1)) continue;
            PointF pr = P[(j + n - 1) % n], nx = P[(j + 1) % n];
            float d1x = P[j].x - pr.x, d1y = P[j].y - pr.y;
            float d2x = nx.x - P[j].x, d2y = nx.y - P[j].y;
            float l1 = sqrtf(d1x * d1x + d1y * d1y), l2 = sqrtf(d2x * d2x + d2y * d2y);
            if (l1 > 0 && l2 > 0) {
                float cross = (d1x * d2y - d1y * d2x) / (l1 * l2);
                float dot = (d1x * d2x + d1y * d2y) / (l1 * l2);
                if (cross < 1e-3f && cross > -1e-3f && dot > 0) continue;   // straight through
            }
            stroke_disc(out, P[j], hw);
        }
        if (!closed && round_cap) {
            stroke_disc(out, P[0], hw);
            stroke_disc(out, P[n - 1], hw);
        }
    }
}

// ---------------------------------------------------------------- rasterizer

struct Edge {
    float x0, y0, y1, dxdy;
    int dir;
};

struct Cross {
    float x;
    int dir;
};

void sort_edges(Edge* a, size_t n) {
    for (size_t gap = n / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < n; i++) {
            Edge t = a[i];
            size_t j = i;
            while (j >= gap && a[j - gap].y0 > t.y0) { a[j] = a[j - gap]; j -= gap; }
            a[j] = t;
        }
    }
}

void sort_cross(Cross* a, size_t n) {
    for (size_t gap = n / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < n; i++) {
            Cross t = a[i];
            size_t j = i;
            while (j >= gap && a[j - gap].x > t.x) { a[j] = a[j - gap]; j -= gap; }
            a[j] = t;
        }
    }
}

// Accumulates horizontal coverage for one pixel row: 4 sub-scanlines per row
// (each contributes 1/4), with exact fractional x at span ends.
struct RowAcc {
    float* part = nullptr;   // per-pixel partial coverage
    float* delta = nullptr;  // run-length deltas for fully covered pixels
    float x0 = 0;            // device x of local column 0
    int bw = 0;

    void add(float xa, float xb) {
        float la = xa - x0, lb = xb - x0;
        if (la < 0) la = 0;
        if (lb > (float)bw) lb = (float)bw;
        if (!(lb > la)) return;
        int ia = (int)la, ib = (int)lb;
        const float w = 0.25f;
        if (ia == ib) {
            part[ia] += (lb - la) * w;
        } else {
            part[ia] += ((float)(ia + 1) - la) * w;
            delta[ia + 1] += w;
            delta[ib] -= w;
            part[ib] += (lb - (float)ib) * w;
        }
    }
};

// Scan-converts the union of all contours into canvas `cv` with premultiplied color pc.
void fill_contours(Canvas& cv, const Contours& g, bool evenodd, uint32_t pc) {
    if ((pc >> 24) == 0 || g.count() == 0) return;
    const int W = cv.width(), H = cv.height();
    Vector<Edge> edges;
    float minx = 3.0e38f, maxx = -3.0e38f, miny = 3.0e38f, maxy = -3.0e38f;
    for (int ci = 0; ci < g.count(); ci++) {
        const int b = g.first(ci);
        const int n = g.last(ci) - b;
        if (n < 2) continue;
        for (int i = 0; i < n; i++) {
            PointF a = g.pts[b + i];
            PointF c = g.pts[b + (i + 1) % n];
            if (a.x < minx) minx = a.x;
            if (a.x > maxx) maxx = a.x;
            if (a.y < miny) miny = a.y;
            if (a.y > maxy) maxy = a.y;
            if (a.y == c.y) continue;
            Edge ed;
            if (a.y < c.y) { ed.x0 = a.x; ed.y0 = a.y; ed.y1 = c.y; ed.dir = 1; }
            else { ed.x0 = c.x; ed.y0 = c.y; ed.y1 = a.y; ed.dir = -1; }
            ed.dxdy = (c.x - a.x) / (c.y - a.y);
            edges.push(ed);
        }
    }
    if (edges.empty()) return;

    float ya = miny < 0 ? 0 : miny, yb = maxy > (float)H ? (float)H : maxy;
    float xa = minx < 0 ? 0 : minx, xb = maxx + 1 > (float)W ? (float)W : maxx + 1;
    if (!(ya < yb) || !(xa < xb)) return;
    const int r0 = (int)ya;
    int r1 = (int)ceilf(yb);
    if (r1 > H) r1 = H;
    const int bx0 = (int)xa;
    int bx1 = (int)ceilf(xb);
    if (bx1 > W) bx1 = W;
    const int bw = bx1 - bx0;
    if (bw <= 0 || r1 <= r0) return;

    sort_edges(edges.data(), edges.size());
    Vector<float> part, delta;
    part.resize((size_t)bw + 2);
    delta.resize((size_t)bw + 2);
    RowAcc acc;
    acc.part = part.data();
    acc.delta = delta.data();
    acc.x0 = (float)bx0;
    acc.bw = bw;

    Vector<int> active;
    Vector<Cross> xs;
    size_t next = 0;
    for (int row = r0; row < r1; row++) {
        for (int s = 0; s < 4; s++) {
            const float sy = (float)row + ((float)s + 0.5f) * 0.25f;
            while (next < edges.size() && edges[next].y0 <= sy) {
                if (edges[next].y1 > sy) active.push((int)next);
                next++;
            }
            size_t k = 0;
            for (size_t i = 0; i < active.size(); i++) {
                int id = active[i];
                if (edges[id].y1 > sy) active[k++] = id;
            }
            while (active.size() > k) active.pop();

            xs.clear();
            for (size_t i = 0; i < active.size(); i++) {
                const Edge& ed = edges[active[i]];
                if (ed.y0 > sy) continue;
                Cross c;
                c.x = ed.x0 + (sy - ed.y0) * ed.dxdy;
                c.dir = ed.dir;
                xs.push(c);
            }
            sort_cross(xs.data(), xs.size());

            int wind = 0;
            bool in = false;
            float start = 0;
            for (size_t i = 0; i < xs.size(); i++) {
                wind += xs[i].dir;
                bool now = evenodd ? ((wind & 1) != 0) : (wind != 0);
                if (now && !in) start = xs[i].x;
                else if (!now && in) acc.add(start, xs[i].x);
                in = now;
            }
            if (in) acc.add(start, (float)bx1);
        }

        // Flush the row: prefix-sum the deltas, combine with partials, blend.
        uint32_t* rowp = cv.row(row) + bx0;
        float run = 0;
        for (int u = 0; u < bw; u++) {
            run += delta[u];
            float cov = run + part[u];
            if (cov > 0) {
                if (cov > 1) cov = 1;
                int a = (int)(cov * 255.0f + 0.5f);
                if (a > 0) rowp[u] = px_over(rowp[u], px_scale(pc, (uint32_t)a));
            }
        }
        for (int u = 0; u <= bw + 1; u++) { part[u] = 0; delta[u] = 0; }
    }
}

// ---------------------------------------------------------------- render driver

struct Own {
    float op = 1;             // the element's own opacity
    bool display_none = false;
};

struct Style {
    Paint fill{kPaintSolid, 0xFF000000u};
    Paint stroke;
    float fill_op = 1, stroke_op = 1, sw = 1;
    float op = 1;             // product of opacity over ancestors (group approximation)
    Color color = 0xFF000000u;
    bool evenodd = false, round_cap = false, visible = true;
};

void apply_prop(const char* name, const char* v, size_t n, const Doc& doc, Style& st, Own& own) {
    float f = 0;
    if (eq(name, "fill")) {
        Paint p;
        if (parse_paint(v, n, doc, p, true)) st.fill = p;
    } else if (eq(name, "stroke")) {
        Paint p;
        if (parse_paint(v, n, doc, p, true)) st.stroke = p;
    } else if (eq(name, "fill-opacity")) {
        if (parse_opacity(v, n, f)) st.fill_op = f;
    } else if (eq(name, "stroke-opacity")) {
        if (parse_opacity(v, n, f)) st.stroke_op = f;
    } else if (eq(name, "opacity")) {
        if (parse_opacity(v, n, f)) own.op = f;
    } else if (eq(name, "stroke-width")) {
        if (parse_len(v, n, f) && f >= 0) st.sw = f;
    } else if (eq(name, "fill-rule")) {
        if (value_is(v, n, "evenodd")) st.evenodd = true;
        else if (value_is(v, n, "nonzero")) st.evenodd = false;
    } else if (eq(name, "stroke-linecap")) {
        if (value_is(v, n, "round")) st.round_cap = true;
        else if (value_is(v, n, "butt") || value_is(v, n, "square")) st.round_cap = false;
    } else if (eq(name, "color")) {
        Color c = 0;
        if (parse_color(v, n, c)) st.color = c;
    } else if (eq(name, "display")) {
        own.display_none = value_is(v, n, "none");
    } else if (eq(name, "visibility")) {
        if (value_is(v, n, "hidden") || value_is(v, n, "collapse")) st.visible = false;
        else if (value_is(v, n, "visible")) st.visible = true;
    }
}

// Presentation attributes first, then style="" declarations (which win).
void apply_props(const Vector<Attr>& attrs, const Doc& doc, Style& st, Own& own) {
    for (size_t i = 0; i < attrs.size(); i++) {
        if (eq(attrs[i].name.c_str(), "style")) continue;
        apply_prop(attrs[i].name.c_str(), attrs[i].value.c_str(), attrs[i].value.size(), doc, st, own);
    }
    for (size_t i = 0; i < attrs.size(); i++) {
        if (!eq(attrs[i].name.c_str(), "style")) continue;
        const char* p = attrs[i].value.c_str();
        const char* e = p + attrs[i].value.size();
        String nm, vl;
        while (next_decl(p, e, nm, vl)) apply_prop(nm.c_str(), vl.c_str(), vl.size(), doc, st, own);
    }
}

Color paint_color(const Paint& p, const Style& st) {
    return p.kind == kPaintCurrent ? st.color : p.c;
}

struct Frame {
    int depth = 0;
    Style st;
    Mat m;
};

bool is_shape_name(const char* n) {
    return eq(n, "path") || eq(n, "rect") || eq(n, "circle") || eq(n, "ellipse") || eq(n, "line") ||
           eq(n, "polyline") || eq(n, "polygon");
}

bool is_group_name(const char* n) { return eq(n, "svg") || eq(n, "g") || eq(n, "a"); }

// Nested <svg>: translate(x, y) then its viewBox (if any) into width x height.
Mat nested_viewport(const Vector<Attr>& a) {
    float x = 0, y = 0;
    attr_len(a, "x", x);
    attr_len(a, "y", y);
    Mat t = mtranslate(x, y);
    ViewBox vb;
    if (get_viewbox(a, vb)) {
        float w = vb.w, h = vb.h;
        float ww = 0, hh = 0;
        if (attr_len(a, "width", ww) && ww > 0) w = ww;
        if (attr_len(a, "height", hh) && hh > 0) h = hh;
        const String* par = find_attr(a, "preserveAspectRatio");
        t = mmul(t, viewbox_matrix(vb, w, h, par ? par->c_str() : ""));
    }
    return t;
}

struct Renderer {
    Canvas* cv = nullptr;
    Doc doc;
    Frame base;
    Vector<Frame> frames;
    int depth = 0;       // open elements
    int skip_at = -1;    // >= 0: inside a non-rendered subtree rooted at this depth

    void start(const Tok& t);
    void end();
    void draw_shape(const char* nm, const Vector<Attr>& attrs, const Style& st, const Mat& m);
};

void Renderer::start(const Tok& t) {
    const int idx = depth;
    const bool sc = t.selfclose;
    if (skip_at >= 0 || idx >= kMaxDepth) {
        if (!sc) {
            if (skip_at < 0) skip_at = idx;
            depth++;
        }
        return;
    }
    const char* nm = t.name.c_str();
    const bool group = is_group_name(nm);
    const bool shape = is_shape_name(nm);
    if (!group && !shape) {
        if (!sc) { skip_at = idx; depth++; }
        return;
    }
    const Frame parent = frames.empty() ? base : frames.back();
    Style st = parent.st;
    Own own;
    apply_props(t.attrs, doc, st, own);
    st.op = parent.st.op * own.op;
    if (own.display_none) {
        if (!sc) { skip_at = idx; depth++; }
        return;
    }
    if (group) {
        if (sc) return;   // nothing inside to draw
        Mat m = parent.m;
        if (eq(nm, "svg")) {
            if (idx > 0) m = mmul(m, nested_viewport(t.attrs));
        } else {
            m = mmul(m, transform_of(t.attrs));
        }
        Frame f;
        f.depth = idx;
        f.st = st;
        f.m = m;
        frames.push(f);
        depth++;
        return;
    }
    Mat m = mmul(parent.m, transform_of(t.attrs));
    draw_shape(nm, t.attrs, st, m);
    if (!sc) { skip_at = idx; depth++; }
}

void Renderer::end() {
    if (depth == 0) return;
    depth--;
    if (skip_at >= 0) {
        if (depth == skip_at) skip_at = -1;
        return;
    }
    if (!frames.empty() && frames.back().depth == depth) frames.pop();
}

void Renderer::draw_shape(const char* nm, const Vector<Attr>& attrs, const Style& st, const Mat& m) {
    if (!st.visible) return;
    const float s = mscale(m);
    if (!(s > 0) || !fin(s)) return;
    Contours geo;
    PB pb;
    pb.g = &geo;
    pb.m = m;
    pb.tol = kTolPx / s;
    build_shape(nm, attrs, pb);
    if (geo.count() == 0) return;
    if (st.fill.kind != kPaintNone) {
        uint32_t pc = premul_alpha(paint_color(st.fill, st), st.fill_op * st.op);
        fill_contours(*cv, geo, st.evenodd, pc);
    }
    if (st.stroke.kind != kPaintNone && st.sw > 0) {
        const float hw = st.sw * s * 0.5f;
        if (hw > 0 && fin(hw)) {
            Contours sg;
            stroke_contours(geo, hw, st.round_cap, sg);
            uint32_t pc = premul_alpha(paint_color(st.stroke, st), st.stroke_op * st.op);
            fill_contours(*cv, sg, false, pc);
        }
    }
}

// ---------------------------------------------------------------- document passes

bool first_svg(Tokenizer& tz, Tok& t) {
    for (;;) {
        tz.next(t);
        if (t.kind == Tok::Eof) return false;
        if (t.kind == Tok::Start) return eq(t.name.c_str(), "svg");
    }
}

// Pre-pass: first-stop color of every gradient with an id.
void collect_gradients(const char* src, size_t len, Color current, Vector<GradRef>& out) {
    Tokenizer tz(src, len);
    Tok t;
    bool pending = false;
    String pid;
    int count = 0;
    for (;;) {
        tz.next(t);
        if (t.kind == Tok::Eof) return;
        const char* nm = t.name.c_str();
        if (t.kind == Tok::End) {
            if (eq(nm, "linearGradient") || eq(nm, "radialGradient")) pending = false;
            continue;
        }
        if (++count > kMaxElements) return;
        if (eq(nm, "linearGradient") || eq(nm, "radialGradient")) {
            pending = false;
            const String* id = find_attr(t.attrs, "id");
            if (id && id->size() > 0 && !t.selfclose && out.size() < kMaxGrads) {
                pid = *id;
                pending = true;
            }
        } else if (pending && eq(nm, "stop")) {
            Color c = 0xFF000000u;
            String v;
            if (prop_value(t.attrs, "stop-color", v)) {
                if (value_is(v.c_str(), v.size(), "currentColor")) c = current;
                else parse_color(v.c_str(), v.size(), c);
            }
            float op = 1;
            if (prop_value(t.attrs, "stop-opacity", v) && parse_opacity(v.c_str(), v.size(), op)) {
                c = with_alpha(c, (int)((float)color_a(c) * op + 0.5f));
            }
            GradRef g;
            g.id = pid;
            g.color = c;
            out.push(g);
            pending = false;
        }
    }
}

}  // namespace

bool intrinsic_size(const char* src, size_t len, int* w, int* h) {
    if (!src || !w || !h) return false;
    Tokenizer tz(src, len);
    Tok t;
    if (!first_svg(tz, t)) return false;
    ViewBox vb;
    bool hasvb = false;
    float iw = 0, ih = 0;
    root_size(t.attrs, vb, hasvb, iw, ih);
    *w = to_px(iw);
    *h = to_px(ih);
    return true;
}

Canvas* render(const char* src, size_t len, int w, int h, Color current_color) {
    if (!src || w <= 0 || h <= 0 || w > kMaxDim || h > kMaxDim || (long)w * h > kMaxPixels) return nullptr;
    Tokenizer tz(src, len);
    Tok root;
    if (!first_svg(tz, root)) return nullptr;
    ViewBox vb;
    bool hasvb = false;
    float iw = 0, ih = 0;
    root_size(root.attrs, vb, hasvb, iw, ih);
    const String* par = find_attr(root.attrs, "preserveAspectRatio");
    Mat rootm = viewbox_matrix(vb, (float)w, (float)h, par ? par->c_str() : "");

    Canvas* cv = new Canvas(w, h);
    if (!cv) return nullptr;

    Vector<GradRef> grads;
    collect_gradients(src, len, current_color, grads);

    Renderer r;
    r.cv = cv;
    r.doc.grads = &grads;
    r.doc.current = current_color;
    r.base.st.color = current_color;
    r.base.m = rootm;

    Tokenizer main_tz(src, len);
    Tok t;
    int count = 0;
    for (;;) {
        main_tz.next(t);
        if (t.kind == Tok::Eof) break;
        if (t.kind == Tok::End) { r.end(); continue; }
        if (++count > kMaxElements) break;
        r.start(t);
    }
    return cv;
}

}  // namespace svg
