// Layout: block flow (margin collapsing), inline formatting (word wrap,
// baseline alignment, inline-block atoms), lists, tables, flexbox (direction,
// wrap, grow/shrink/basis, justify-content, align-items, gap, order), simple
// grid (template columns, fr units, spans), positioning (relative offsets,
// absolute/fixed out-of-flow), plus painting, hit testing and GET forms.
#include "html.h"
#include "internal.h"
#if __has_include("svg.cpp")
#include "svg.h"
#define HAVE_SVG 1
#endif
#include "net/http.h"
#include "lib/string.h"
#include "lib/math.h"
#include "lib/log.h"

namespace html {

Node* find_tag(Node* n, const char* tag) {
    if (!n) return nullptr;
    if (n->type == Node::Element && n->tag == tag) return n;
    for (Node* c : n->children)
        if (Node* f = find_tag(c, tag)) return f;
    return nullptr;
}

String inner_text(Node* n) {
    if (n->type == Node::Text) return n->text;
    String s;
    for (Node* c : n->children) s += inner_text(c);
    return s;
}

void resolve_url(Document* d, const char* ref, char* out, size_t size) { net::url_resolve(d->url.c_str(), ref, out, size); }

static void collect(Node* n, Document* d) {
    if (n->type != Node::Element) { for (Node* c : n->children) collect(c, d); return; }
    if (n->tag == "noscript" && d->scripting) return;   // fallback content for script-less browsers
    if (n->tag == "style") {
        for (Node* c : n->children) if (c->type == Node::Text) parse_css(d, c->text.c_str(), c->text.size());
        return;
    }
    if (n->tag == "link") {
        const char* rel = n->attr("rel");
        const char* href = n->attr("href");
        const char* media = n->attr("media");
        if (rel && href && strstr(String(rel).lower().c_str(), "stylesheet") && (!media || !strstr(media, "print"))) {
            char abs[2300];
            net::url_resolve(d->url.c_str(), href, abs, sizeof(abs));
            if (d->stylesheet_urls.size() < 8) d->stylesheet_urls.push(String(abs));
        }
        return;
    }
    if (n->tag == "title" && d->title.empty()) {
        String t = inner_text(n).trim();
        String o;
        bool sp = false;
        for (size_t i = 0; i < t.size(); i++) {
            if (isspace(t[i])) { if (!sp) o += ' '; sp = true; }
            else { o += t[i]; sp = false; }
        }
        d->title = o;
    }
    if (n->tag == "svg") return;
    for (Node* c : n->children) collect(c, d);
}

Document* parse(const char* src, size_t len, const char* url, bool scripting) {
    Document* d = new Document();
    d->url = url;
    d->scripting = scripting;
    d->root = parse_dom(src, len);
    collect(d->root, d);
    compute_styles(d);
    return d;
}

Document* parse_text(const char* text, size_t len, const char* url) {
    String h = "<html><body><pre style=\"white-space:pre-wrap\">";
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '<') h += "&lt;";
        else if (c == '&') h += "&amp;";
        else h += c;
    }
    h += "</pre></body></html>";
    return parse(h.c_str(), h.size(), url);
}

void add_stylesheet(Document* d, const char* css, size_t len) {
    parse_css(d, css, len);
    compute_styles(d);
}

Vector<Image>& images(Document* d) { return d->images; }

void restyle(Document* d) { compute_styles(d); }

// ================================================================ layout
namespace {

struct Ctx {
    Document* d;
    Vector<Fragment>* out;
    int viewport_w;
    int depth = 0;
    int cb_height = -1;   // definite content height of the containing block (for height: N%)
};

struct DepthGuard {
    Ctx& c;
    DepthGuard(Ctx& cx) : c(cx) { c.depth++; }
    ~DepthGuard() { c.depth--; }
};

Font font_of(const Style& s) {
    if (s.mono) return Font::Mono;
    if (s.weight >= 600 || s.bold) return Font::Bold;
    if (s.weight >= 500) return Font::Medium;
    return Font::Regular;
}

int lh_of(const Style& s) {
    int base = line_height(s.font_size, font_of(s));
    if (s.line_height_pct > 0) {
        int v = s.font_size * s.line_height_pct / 100;
        if (v > 0) return kmax(v, s.font_size);
    }
    return base;
}

Color fade(Color c, float opacity) {
    if (opacity >= 0.999f) return c;
    return with_alpha(c, (int)(color_a(c) * opacity));
}

bool out_of_flow(Node* n) {
    return n->type == Node::Element && (n->style.position == Style::Absolute || n->style.position == Style::Fixed);
}

bool is_block_level(Node* n, int depth = 0);

bool has_block_child(Node* n, int depth) {
    if (depth > 6) return false;
    for (Node* c : n->children) {
        if (c->type != Node::Element) continue;
        if (c->style.display == Style::None || out_of_flow(c)) continue;
        if (is_block_level(c, depth + 1)) return true;
    }
    return false;
}

bool replaced(Node* n) {
    return n->tag == "img" || n->tag == "input" || n->tag == "button" || n->tag == "select" || n->tag == "textarea" || n->tag == "svg" || n->tag == "iframe";
}

bool is_block_level(Node* n, int depth) {
    if (n->type != Node::Element) return false;
    switch (n->style.display) {
    case Style::Block: case Style::ListItem: case Style::Table: case Style::TableRow: case Style::TableCell: case Style::Flex: case Style::Grid: return true;
    case Style::None: return false;
    case Style::InlineBlock: return false;
    case Style::Contents: return has_block_child(n, depth);
    default:
        if (replaced(n)) return false;
        return has_block_child(n, depth);
    }
}

int image_index(Document* d, const String& url) {
    for (size_t i = 0; i < d->images.size(); i++) if (d->images[i].url == url) return (int)i;
    Image& im = d->images.emplace();
    im.url = url;
    return (int)d->images.size() - 1;
}

void translate_frags(Vector<Fragment>& v, size_t from, int dx, int dy) {
    for (size_t i = from; i < v.size(); i++) { v[i].r.x += dx; v[i].r.y += dy; }
}

}  // namespace
void serialize_node(Node* n, String& out);
namespace {
void serialize(Node* n, String& out) { serialize_node(n, out); }
}  // namespace

// Serialize an element subtree back to markup (inline SVG rendering, innerHTML).
void serialize_node(Node* n, String& out) {
    if (n->type == Node::Document) { for (Node* c : n->children) serialize_node(c, out); return; }
    if (n->type == Node::Text) {
        for (size_t i = 0; i < n->text.size(); i++) {
            char c = n->text[i];
            if (c == '<') out += "&lt;"; else if (c == '&') out += "&amp;"; else out += c;
        }
        return;
    }
    out += '<';
    out += n->tag;
    for (Attr& a : n->attrs) {
        out += ' ';
        out += a.name;
        out += "=\"";
        for (size_t i = 0; i < a.value.size(); i++) { char c = a.value[i]; if (c == '"') out += "&quot;"; else out += c; }
        out += '"';
    }
    out += '>';
    static const char* voids[] = {"br", "img", "hr", "input", "meta", "link", "area", "base", "col", "embed", "source", "track", "wbr"};
    for (const char* v : voids) if (n->tag == v) return;
    for (Node* c : n->children) serialize_node(c, out);
    out += "</";
    out += n->tag;
    out += '>';
}

namespace {

// ---------------------------------------------------------------- inline items
struct Item {
    enum Kind { Word, Space, Break, Atomic } kind;
    String text;
    const Style* st;
    Node* node;
    String href;
    int w, h;
    int ascent;
    Fragment::Kind akind;
    String src;
    int image = -1;
    Vector<Fragment> sub;       // pre-laid-out inline-block contents (relative to 0,0)
};

String transform_text(const String& s, uint8_t t) {
    if (!t) return s;
    String o(s);
    bool start = true;
    for (size_t i = 0; i < o.size(); i++) {
        char c = o[i];
        if (t == 1) o[i] = (char)toupper(c);
        else if (t == 2) o[i] = (char)tolower(c);
        else if (t == 3) { if (start && isalpha(c)) o[i] = (char)toupper(c); }
        start = isspace(c) || c == '-';
    }
    return o;
}

void push_text(Ctx& cx, Vector<Item>& items, Node* t, const String& href) {
    const Style& s = t->style;
    Font f = font_of(s);
    String text = transform_text(t->text, s.text_transform);
    const char* p = text.c_str();
    int lh = lh_of(s), asc = font_ascent(s.font_size, f) + (lh - line_height(s.font_size, f)) / 2;
    if (s.pre) {
        while (*p) {
            const char* st = p;
            while (*p && *p != '\n') p++;
            if (p > st) {
                String chunk;
                for (const char* q = st; q < p; q++) { if (*q == '\t') chunk += "    "; else if (*q != '\r') chunk += *q; }
                items.push(Item{Item::Word, chunk, &s, t, href, text_width(chunk.c_str(), s.font_size, f), lh, asc, Fragment::Text, String()});
            }
            if (*p == '\n') { items.push(Item{Item::Break, String(), &s, t, href, 0, lh, 0, Fragment::Text, String()}); p++; }
        }
        return;
    }
    while (*p) {
        if (isspace(*p)) {
            while (*p && isspace(*p)) p++;
            if (items.empty() || items.back().kind != Item::Space)
                items.push(Item{Item::Space, String(" "), &s, t, href, text_width(" ", s.font_size, f), lh, asc, Fragment::Text, String()});
            continue;
        }
        const char* st = p;
        while (*p && !isspace(*p)) p++;
        String w(st, p - st);
        items.push(Item{Item::Word, w, &s, t, href, text_width(w.c_str(), s.font_size, f), lh, asc, Fragment::Text, String()});
    }
}

int layout_block(Ctx& cx, Node* n, int x, int y, int avail, int* margin_bottom, const String& href);
int max_content(Node* n, int depth = 0);
int min_content(Node* n, int depth = 0);

String link_of(Ctx& cx, Node* n, const String& href) {
    if (n->tag == "a" && n->attr("href")) {
        static char abs[2300];   // static: keeps recursive layout frames small
        net::url_resolve(cx.d->url.c_str(), n->attr("href"), abs, sizeof(abs));
        return String(abs);
    }
    return href;
}

int svg_dim(Node* n, const char* attr) {
    const char* v = n->attr(attr);
    if (!v || strchr(v, '%')) return 0;
    return atoi(v);
}

void add_replaced(Ctx& cx, Node* n, Vector<Item>& items, const String& href) {
    const Style& s = n->style;
    const String& t = n->tag;
    if (t == "svg") {
        int w = s.width > 0 ? s.width : 0, h = s.height > 0 ? s.height : 0;
        if (!w) w = svg_dim(n, "width");
        if (!h) h = svg_dim(n, "height");
        if (!w || !h) {
            const char* vb = n->attr("viewbox");
            if (!vb) vb = n->attr("viewBox");
            if (vb) {
                double a[4] = {0, 0, 0, 0};
                const char* p = vb;
                for (int i = 0; i < 4 && *p; i++) { while (*p == ' ' || *p == ',') p++; a[i] = strtod(p, (char**)&p); }
                if (a[2] > 0 && a[3] > 0) {
                    if (!w && !h) { h = (int)kmin(a[3], 24.0); w = (int)(h * a[2] / a[3]); }
                    else if (!w) w = (int)(h * a[2] / a[3]);
                    else h = (int)(w * a[3] / a[2]);
                }
            }
        }
        if (!w) w = s.font_size;
        if (!h) h = s.font_size;
        w = kclamp(w, 1, 2000);
        h = kclamp(h, 1, 2000);
        String markup;
        serialize(n, markup);
#ifdef HAVE_SVG
        Canvas* c = svg::render(markup.c_str(), markup.size(), w, h, s.color);
#else
        Canvas* c = nullptr;
#endif
        Item it{Item::Atomic, String(), &s, n, href, w, h, h, Fragment::Svg, String()};
        if (c) { cx.d->inline_svgs.push(c); it.image = (int)cx.d->inline_svgs.size() - 1; }
        items.push(std::move(it));
        return;
    }
    if (t == "img") {
        // real URL first: a data: src is usually a placeholder for a lazy loader
        const char* src = n->attr("src");
        if (!src || !*src || !strncmp(src, "data:", 5)) {
            for (const char* lazy : {"data-src", "data-lazy-src", "data-original"})
                if (n->attr(lazy) && *n->attr(lazy)) { src = n->attr(lazy); break; }
        }
        if ((!src || !*src) && n->attr("srcset")) src = n->attr("srcset");
        if ((!src || !*src) && n->parent && n->parent->tag == "picture")
            for (Node* c : n->parent->children) if (c->tag == "source" && c->attr("srcset")) { src = c->attr("srcset"); break; }
        int w = s.width > 0 ? s.width : 0, h = s.height > 0 ? s.height : 0;
        if (s.width_pct > 0) w = cx.viewport_w * s.width_pct / 100;
        int idx = -1;
        if (src && *src) {
            String ref = String(src).trim();
            bool data = ref.starts_with("data:");
            long sp = data ? -1 : ref.find(' ');
            if (sp > 0) ref = ref.substr(0, sp);
            static char abs[2300];
            if (!data) net::url_resolve(cx.d->url.c_str(), ref.c_str(), abs, sizeof(abs));
            idx = image_index(cx.d, data ? ref : String(abs));
            Image& im = cx.d->images[idx];
            if (data ? ref.starts_with("data:image/svg") : String(abs).lower().find(".svg") >= 0) im.svg = true;
            if (im.canvas) {
                int iw = im.canvas->width(), ih = im.canvas->height();
                if (!w && !h) { w = iw; h = ih; }
                else if (!w) w = ih ? iw * h / ih : iw;
                else if (!h) h = iw ? ih * w / iw : ih;
            }
            if (!w && !h && im.svg) { w = 24; h = 24; }
        }
        if (!w && !h) {
            const char* alt = n->attr("alt");
            if (alt && *alt && idx < 0) { w = kmin(text_width(alt, 13) + 12, 300); h = 22; }
            else { w = 1; h = 1; }
        }
        if (!w) w = s.aspect_ratio > 0 ? (int)(h * s.aspect_ratio) : h;
        if (!h) h = s.aspect_ratio > 0 ? (int)(w / s.aspect_ratio) : w;
        if (s.max_width > 0 && w > s.max_width) { h = h * s.max_width / w; w = s.max_width; }
        if (w > cx.viewport_w) { h = h * cx.viewport_w / w; w = cx.viewport_w; }
        if (idx >= 0) { cx.d->images[idx].want_w = w; cx.d->images[idx].want_h = h; }
        Item it{Item::Atomic, String(n->attr("alt") ? n->attr("alt") : ""), &s, n, href, w, h, h, Fragment::Image, String()};
        it.image = idx;
        if (idx >= 0) it.src = cx.d->images[idx].url;
        items.push(std::move(it));
        return;
    }
    if (t == "iframe") {
        auto dim = [](const char* a, int whole, int fallback) {
            if (!a || !*a) return fallback;
            int v = atoi(a);
            return strchr(a, '%') ? whole * v / 100 : v;
        };
        int w = s.width > 0 ? s.width : s.width_pct > 0 ? cx.viewport_w * s.width_pct / 100 : dim(n->attr("width"), cx.viewport_w, 300);
        int h = s.height > 0 ? s.height : s.height_pct > 0 ? cx.d->viewport_h * s.height_pct / 100 : dim(n->attr("height"), cx.d->viewport_h, 150);
        w = kclamp(w, 1, cx.viewport_w);
        h = kmax(h, 1);
        items.push(Item{Item::Atomic, String(), &s, n, href, w, h, h, Fragment::Frame, String()});
        return;
    }
    // form controls
    const char* type = n->attr("type");
    String ty = type ? String(type).lower() : String(t == "button" ? "submit" : "text");
    int w = 180, h = 30;
    String label;
    if (t == "button") {
        // buttons render their content as an inline-block
        Vector<Fragment> sub;
        Vector<Fragment>* saved = cx.out;
        cx.out = &sub;
        int mw = kmin(max_content(n) + 4, cx.viewport_w);
        if (s.width > 0) mw = s.width;
        int mb;
        int bh = layout_block(cx, n, 0, 0, mw, &mb, href);
        cx.out = saved;
        bool styled = color_a(s.background) || s.border_width[0] || s.border_width[1];
        if (!styled) {
            label = inner_text(n).trim();
            w = text_width(label.c_str(), 14) + 26;
            if (!label.empty() || bh < 8) {
                Item it{Item::Atomic, label, &s, n, href, kmax(w, 30), kmax(h, bh), kmax(h, bh) - 6, Fragment::Input, String()};
                if (label.empty()) { it.sub = std::move(sub); it.akind = Fragment::Box; it.w = mw; it.h = bh; it.ascent = bh; }
                items.push(std::move(it));
                return;
            }
        }
        Item it{Item::Atomic, String(), &s, n, href, mw, bh, bh, Fragment::Box, String()};
        it.sub = std::move(sub);
        items.push(std::move(it));
        return;
    }
    if (t == "select") {
        Node* opt = nullptr;
        for (Node* c : n->children) if (c->tag == "option") { if (!opt || c->attr("selected")) opt = c; }
        label = opt ? inner_text(opt).trim() : String();
        w = kmax(80, text_width(label.c_str(), 14) + 40);
    } else if (t == "textarea") {
        int cols = n->attr("cols") ? atoi(n->attr("cols")) : 40, rows = n->attr("rows") ? atoi(n->attr("rows")) : 3;
        w = kmin(cols * 8, cx.viewport_w);
        h = rows * 18 + 8;
        if (n->value.empty() && !n->children.empty()) n->value = inner_text(n);
    } else if (ty == "submit" || ty == "button" || ty == "reset") {
        label = n->attr("value") ? n->attr("value") : (ty == "reset" ? "Reset" : "Submit");
        w = text_width(label.c_str(), 14) + 26;
    } else if (ty == "checkbox" || ty == "radio") { w = 16; h = 16; if (n->attr("checked")) n->checked = true; }
    else if (ty == "image") { w = 24; h = 24; }
    else {
        if (n->attr("size")) w = kclamp(atoi(n->attr("size")) * 8, 40, 600);
        if (n->value.empty() && n->attr("value")) n->value = n->attr("value");
    }
    if (s.width > 0) w = s.width;
    if (s.width_pct > 0) w = kmax(40, cx.viewport_w * s.width_pct / 100);
    if (s.height > 0) h = s.height;
    w = kmin(w, cx.viewport_w);
    items.push(Item{Item::Atomic, label, &s, n, href, w, h, h - 8, Fragment::Input, String()});
}

void collect_inline(Ctx& cx, Node* n, Vector<Item>& items, String href) {
    if (n->type == Node::Text) { push_text(cx, items, n, href); return; }
    if (cx.depth > 160) return;
    DepthGuard guard(cx);
    if (n->type != Node::Element || n->style.display == Style::None || out_of_flow(n)) return;
    href = link_of(cx, n, href);
    const Style& s = n->style;
    if (n->tag == "br") { items.push(Item{Item::Break, String(), &s, n, href, 0, lh_of(s), 0, Fragment::Text, String()}); return; }
    if (replaced(n)) { add_replaced(cx, n, items, href); return; }
    if (s.display == Style::InlineBlock || (s.display == Style::Inline && (color_a(s.background) || s.border_width[0]) && s.padding[1] + s.padding[3] >= 6)) {
        // atomic inline-block: lay out as a shrink-to-fit block
        Vector<Fragment> sub;
        Vector<Fragment>* saved = cx.out;
        cx.out = &sub;
        int w = s.width > 0 ? s.width + s.padding[1] + s.padding[3] + s.border_width[1] + s.border_width[3]
                            : kmin(max_content(n) + 1, cx.viewport_w);
        if (s.width_pct > 0) w = cx.viewport_w * s.width_pct / 100;
        if (s.min_width > 0) w = kmax(w, s.min_width);
        Style saved_style = n->style;
        n->style.display = Style::Block;
        int mb;
        int h = layout_block(cx, n, 0, 0, w, &mb, href);
        n->style = saved_style;
        cx.out = saved;
        // baseline: first text fragment baseline or bottom
        int asc = h;
        for (Fragment& f : sub) if (f.kind == Fragment::Text) { asc = f.r.y + font_ascent(f.size, f.font); break; }
        int ml = s.margin[3] == kUnset ? 0 : s.margin[3], mr = s.margin[1] == kUnset ? 0 : s.margin[1];
        if (ml) translate_frags(sub, 0, ml, 0);
        Item it{Item::Atomic, String(), &s, n, href, w + ml + mr, h, asc, Fragment::Box, String()};
        it.sub = std::move(sub);
        items.push(std::move(it));
        return;
    }
    if (n->layout_y < 0 && (n->attr("id") || n->attr("name"))) n->layout_y = -2;
    // inline element with horizontal margins/padding: emit spacing
    int pad_l = (s.margin[3] != kUnset ? s.margin[3] : 0) + s.padding[3];
    int pad_r = (s.margin[1] != kUnset ? s.margin[1] : 0) + s.padding[1];
    if (pad_l > 0 && n->tag != "a") items.push(Item{Item::Atomic, String(), &s, n, href, pad_l, 1, 1, Fragment::Box, String()});
    for (Node* c : n->children) collect_inline(cx, c, items, href);
    if (pad_r > 0 && n->tag != "a") items.push(Item{Item::Atomic, String(), &s, n, href, pad_r, 1, 1, Fragment::Box, String()});
}

// Lay out a run of inline items in [x, x+width) starting at y. Returns height.
// An inline-block filling its parent's height spans the whole line, so its baseline can't move it.
static int item_valign(const Item& it) {
    if (it.st->valign) return it.st->valign;
    return it.st->display == Style::InlineBlock && it.st->height_pct >= 100 ? 1 : 0;
}

int layout_inline(Ctx& cx, Vector<Item>& items, int x, int y, int width, const Style& block) {
    int cy = y;
    size_t i = 0;
    while (i < items.size() && items[i].kind == Item::Space) i++;
    bool nowrap = block.nowrap || block.pre;
    Vector<Fragment>& out = *cx.out;
    while (i < items.size()) {
        size_t start = i;
        int lw = 0;
        size_t end = i;
        size_t last_break = (size_t)-1;
        while (end < items.size()) {
            Item& it = items[end];
            if (it.kind == Item::Break) break;
            if (it.kind == Item::Space) last_break = end;
            int nw = lw + it.w;
            if (nw > width && end > start && !nowrap && !it.st->nowrap) {
                if (it.kind == Item::Space) break;
                if (last_break != (size_t)-1 && last_break > start) { end = last_break; break; }
                break;
            }
            lw = nw;
            end++;
        }
        if (end == start && end < items.size() && items[end].kind != Item::Break) end++;
        size_t line_end = end;
        while (line_end > start && items[line_end - 1].kind == Item::Space) line_end--;
        int w = 0, ascent = 0, descent = 0;
        for (size_t k = start; k < line_end; k++) {
            Item& it = items[k];
            w += it.w;
            if (it.kind == Item::Atomic && it.h <= 1) continue;   // spacers
            if (it.kind == Item::Atomic && item_valign(it)) continue;   // placed against the finished line below
            int a = it.kind == Item::Atomic ? it.ascent : it.ascent;
            int dsc = it.h - a;
            ascent = kmax(ascent, a);
            descent = kmax(descent, dsc);
        }
        if (ascent + descent == 0) {
            int h = (end < items.size()) ? items[end].h : lh_of(block);
            if (start < line_end && items[start].kind != Item::Atomic) h = items[start].h;
            ascent = h * 3 / 4;
            descent = h - ascent;
        }
        for (size_t k = start; k < line_end; k++)
            if (items[k].kind == Item::Atomic && items[k].h > 1 && item_valign(items[k])) descent = kmax(descent, items[k].h - ascent);
        int lx = x;
        if (block.text_align == Style::Center) lx = x + (width - w) / 2;
        else if (block.text_align == Style::Right) lx = x + width - w;
        if (lx < x && !nowrap) lx = x;
        int baseline = cy + ascent;
        int line_h = ascent + descent;
        auto atom_y = [&](const Item& it) {
            switch (item_valign(it)) {
            case 1: return cy;
            case 2: return cy + (line_h - it.h) / 2;
            case 3: return cy + line_h - it.h;
            default: return baseline - it.ascent;
            }
        };
        int px = lx;
        for (size_t k = start; k < line_end; k++) {
            Item& it = items[k];
            for (Node* p = it.node; p; p = p->parent) if (p->layout_y == -2) p->layout_y = cy;
            if (it.kind == Item::Atomic) {
                if (!it.sub.empty()) {
                    size_t from = out.size();
                    for (Fragment& f : it.sub) out.push(f);
                    translate_frags(out, from, px, atom_y(it));
                    if (!it.href.empty()) for (size_t q = from; q < out.size(); q++) if (out[q].href.empty()) out[q].href = it.href;
                } else if (it.h > 1 || it.akind != Fragment::Box) {
                    Fragment f;
                    f.kind = it.akind;
                    f.r = Rect(px, atom_y(it), it.w, it.h);
                    f.node = it.node;
                    f.href = it.href;
                    f.src = it.src;
                    f.image = it.image;
                    f.text = it.text;
                    f.size = 14;
                    f.color = it.st->color;
                    out.push(f);
                }
                px += it.w;
                continue;
            }
            const Style& s = *it.st;
            Color col = fade(s.color, s.opacity);
            Fragment* last = out.empty() ? nullptr : &out.back();
            bool merge = last && last->kind == Fragment::Text && last->r.right() == px && last->r.y == baseline - it.ascent &&
                         last->color == col && last->size == s.font_size && last->font == font_of(s) &&
                         last->underline == s.underline && last->href == it.href && last->bg == s.background && last->strike == s.strike;
            if (merge && last->text.size() < 400) {
                last->text += it.text;
                last->r.w += it.w;
            } else {
                if (it.kind == Item::Space && (k == start)) { px += it.w; continue; }
                Fragment f;
                f.kind = Fragment::Text;
                f.r = Rect(px, baseline - it.ascent, it.w, it.h);
                f.text = it.text;
                f.color = col;
                f.bg = fade(s.background, s.opacity);
                f.size = s.font_size;
                f.font = font_of(s);
                f.underline = s.underline;
                f.strike = s.strike;
                f.href = it.href;
                f.node = it.node;
                out.push(f);
            }
            px += it.w;
        }
        cy = baseline + descent;
        i = end;
        if (i < items.size() && items[i].kind == Item::Break) i++;
        while (i < items.size() && items[i].kind == Item::Space) i++;
    }
    return cy - y;
}

// ---------------------------------------------------------------- intrinsic sizes
int box_extra_h(const Style& s) {
    return s.padding[1] + s.padding[3] + s.border_width[1] + s.border_width[3] + kmax(0, s.margin[1] == kUnset ? 0 : s.margin[1]) +
           kmax(0, s.margin[3] == kUnset ? 0 : s.margin[3]);
}

int max_content(Node* n, int depth) {
    if (depth > 24) return 0;
    if (n->type == Node::Text) {
        const Style& s = n->style;
        Font f = font_of(s);
        if (s.pre) {
            int best = 0;
            const char* p = n->text.c_str();
            while (*p) {
                const char* st = p;
                while (*p && *p != '\n') p++;
                best = kmax(best, text_width_n(st, (int)(p - st), s.font_size, f));
                if (*p) p++;
            }
            return best;
        }
        // collapse whitespace
        String t;
        bool sp = false;
        for (size_t i = 0; i < n->text.size(); i++) {
            char c = n->text[i];
            if (isspace(c)) { if (!sp) t += ' '; sp = true; } else { t += c; sp = false; }
        }
        return kmin(text_width(transform_text(t, s.text_transform).c_str(), s.font_size, f), 2000);
    }
    if (n->type != Node::Element || n->style.display == Style::None || out_of_flow(n)) return 0;
    const Style& s = n->style;
    int extra = box_extra_h(s);
    if (s.width > 0) return s.width + extra;
    if (n->tag == "img") {
        if (s.width > 0) return s.width + extra;
        int h = s.height > 0 ? s.height : 0;
        return (h ? h : 40) + extra;
    }
    if (n->tag == "svg") { int w = s.width > 0 ? s.width : svg_dim(n, "width"); return (w ? w : s.font_size) + extra; }
    if (n->tag == "input" || n->tag == "select") return (s.width > 0 ? s.width : 180) + extra;
    if (n->tag == "iframe") return (s.width > 0 ? s.width : n->attr("width") ? atoi(n->attr("width")) : 300) + extra;
    if (n->tag == "br") return 0;
    bool block = is_block_level(n);
    if (s.display == Style::Flex && s.flex_row) {
        int sum = 0, cnt = 0;
        for (Node* c : n->children) { int w = max_content(c, depth + 1); if (w) { sum += w; cnt++; } }
        return sum + (cnt > 1 ? (cnt - 1) * s.gap_col : 0) + extra;
    }
    int sum = 0, best = 0;
    for (Node* c : n->children) {
        int w = max_content(c, depth + 1);
        if (is_block_level(c)) { best = kmax(best, sum); sum = 0; best = kmax(best, w); }
        else sum += w;
    }
    (void)block;
    int r = kmax(best, sum) + extra;
    if (s.max_width > 0) r = kmin(r, s.max_width + extra);
    return r;
}

int min_content(Node* n, int depth) {
    if (depth > 24) return 0;
    if (n->type == Node::Text) {
        const Style& s = n->style;
        if (s.nowrap || s.pre) return max_content(n, depth);
        int best = 0;
        const char* p = n->text.c_str();
        while (*p) {
            while (*p && isspace(*p)) p++;
            const char* st = p;
            while (*p && !isspace(*p)) p++;
            if (p > st) best = kmax(best, text_width_n(st, (int)(p - st), s.font_size, font_of(s)));
        }
        return kmin(best, 400);
    }
    if (n->type != Node::Element || n->style.display == Style::None || out_of_flow(n)) return 0;
    const Style& s = n->style;
    if (s.width > 0) return s.width + box_extra_h(s);
    if (n->tag == "img" || n->tag == "svg" || n->tag == "input") return kmin(max_content(n, depth), 400);
    if (s.nowrap) return kmin(max_content(n, depth), 600);
    int best = 0;
    if (s.display == Style::Flex && s.flex_row && !s.flex_wrap) {
        int sum = 0;
        for (Node* c : n->children) sum += min_content(c, depth + 1);
        best = sum;
    } else {
        for (Node* c : n->children) best = kmax(best, min_content(c, depth + 1));
    }
    return best + box_extra_h(s);
}

// ---------------------------------------------------------------- containers
int layout_flex(Ctx& cx, Node* n, int x, int y, int width, int fixed_h, const String& href);
int layout_grid(Ctx& cx, Node* n, int x, int y, int width, const String& href);
int layout_table(Ctx& cx, Node* table, int x, int y, int width, const String& href);

void layout_positioned(Ctx& cx, Node* c, int cb_x, int cb_y, int cb_w, int cb_h, int static_y, const String& href) {
    const Style& s = c->style;
    int w;
    if (s.width > 0) w = s.width + s.padding[1] + s.padding[3] + s.border_width[1] + s.border_width[3];
    else if (s.width_pct > 0) w = cb_w * s.width_pct / 100;
    else if (s.offset[1] != kUnset && s.offset[3] != kUnset) w = cb_w - s.offset[1] - s.offset[3];
    else w = kmin(max_content(c) + 1, cb_w > 0 ? cb_w : cx.viewport_w);
    if (w <= 0) return;
    // skip tiny/visually-hidden helpers and offscreen elements
    if ((s.width > 0 && s.width <= 1) || (s.height > 0 && s.height <= 1) || (s.offset[3] != kUnset && s.offset[3] < -1000) || (s.offset[0] != kUnset && s.offset[0] < -1000)) return;
    if (s.position == Style::Fixed && s.offset[0] == kUnset && s.offset[2] == kUnset && s.offset[1] == kUnset && s.offset[3] == kUnset) return;
    Vector<Fragment> sub;
    Vector<Fragment>* saved = cx.out;
    cx.out = &sub;
    int mb;
    Style saved_style = c->style;
    c->style.position = Style::Static;
    {
        int ch = s.position == Style::Fixed ? cx.d->viewport_h : cb_h;
        if (s.height <= 0 && s.offset[0] != kUnset && s.offset[2] != kUnset && ch > 0)
            c->style.height = kmax(1, ch - s.offset[0] - s.offset[2] - s.padding[0] - s.padding[2]);
        else if (s.height <= 0 && s.height_pct > 0 && ch > 0)
            c->style.height = ch * s.height_pct / 100;
    }
    int saved_cb = cx.cb_height;
    cx.cb_height = c->style.height > 0 ? c->style.height : -1;
    if (c->style.display == Style::Inline || c->style.display == Style::InlineBlock) c->style.display = Style::Block;
    int h = layout_block(cx, c, 0, 0, w, &mb, href);
    c->style = saved_style;
    cx.out = saved;
    cx.cb_height = saved_cb;
    bool fixed = s.position == Style::Fixed;
    int vh = cx.d->viewport_h;
    if (fixed) { cb_x = 0; cb_y = 0; cb_w = cx.viewport_w; cb_h = vh; }
    int px = s.offset[3] != kUnset ? cb_x + s.offset[3] : s.offset[1] != kUnset ? cb_x + cb_w - s.offset[1] - w : cb_x;
    int py = s.offset[0] != kUnset ? cb_y + s.offset[0] : (s.offset[2] != kUnset && cb_h > 0) ? cb_y + cb_h - s.offset[2] - h : static_y;
    if (fixed && s.offset[0] != kUnset && s.offset[2] != kUnset && h > vh) {
        // fixed box stretched top..bottom: make it a viewport-sized scroll container
    }
    size_t from = saved->size();
    for (Fragment& f : sub) saved->push(f);
    translate_frags(*saved, from, px, py);
    if (fixed) for (size_t i = from; i < saved->size(); i++) (*saved)[i].fixed = true;
}

// Children of a block container: inline runs + blocks (+ out-of-flow).
int layout_children(Ctx& cx, Node* n, int x, int y, int width, const String& href_in, int* last_mb) {
    int cy = y;
    Vector<Item> items;
    int pending_mb = 0;
    bool first_block = true;
    auto flush = [&]() {
        bool any = false;
        for (Item& it : items) if (it.kind != Item::Space) { any = true; break; }
        if (any) {
            cy += pending_mb;
            pending_mb = 0;
            cy += layout_inline(cx, items, x, cy, width, n->style);
            first_block = false;
        }
        items.clear();
    };
    String href = link_of(cx, n, href_in);
    Vector<Node*> positioned;
    Vector<int> positioned_y;
    for (Node* c : n->children) {
        if (c->type == Node::Element && c->style.display == Style::None) continue;
        if (out_of_flow(c)) { positioned.push(c); positioned_y.push(cy); continue; }
        if (c->type == Node::Element && c->style.display == Style::Contents) {
            // children participate directly (approximation: lay out as a block container)
        }
        if (is_block_level(c)) {
            flush();
            int mt = c->style.margin[0] == kUnset ? 0 : c->style.margin[0];
            int collapse = first_block && false ? mt : kmax(mt, pending_mb);
            if (mt < 0) collapse = pending_mb + mt;
            int mb = 0;
            int hgt = layout_block(cx, c, x, cy + collapse, width, &mb, href);
            cy += collapse + hgt;
            pending_mb = mb;
            first_block = false;
        } else {
            collect_inline(cx, c, items, href);
        }
    }
    flush();
    *last_mb = pending_mb;
    int h = cy - y;
    bool is_cb = n->style.position != Style::Static || n->tag == "body" || n->tag == "html";
    for (size_t i = 0; i < positioned.size(); i++) {
        Node* c = positioned[i];
        if (c->style.position == Style::Fixed) layout_positioned(cx, c, 0, 0, cx.viewport_w, 0, positioned_y[i], href);
        else if (is_cb) layout_positioned(cx, c, x, y, width, h, positioned_y[i], href);
        else layout_positioned(cx, c, x, y, width, h, positioned_y[i], href);
    }
    return h;
}

int layout_block(Ctx& cx, Node* n, int x, int y, int avail, int* margin_bottom, const String& href) {
    DepthGuard guard(cx);
    *margin_bottom = 0;
    if (cx.depth > 160) return 0;   // pathological nesting
    Style& s = n->style;
    n->layout_y = y;
    int saved_h = s.height;
    if (s.height <= 0 && s.height_pct > 0 && cx.cb_height > 0) s.height = kmax(1, cx.cb_height * s.height_pct / 100 - s.padding[0] - s.padding[2]);
    int saved_cb = cx.cb_height;
    cx.cb_height = s.height > 0 ? s.height : -1;
    struct Restore { Style& s; int h; Ctx& c; int cb; ~Restore() { s.height = h; c.cb_height = cb; } } restore{s, saved_h, cx, saved_cb};
    int ml = s.margin[3] == kUnset ? 0 : s.margin[3], mr = s.margin[1] == kUnset ? 0 : s.margin[1];
    int box_extra = s.padding[1] + s.padding[3] + s.border_width[1] + s.border_width[3];
    int w = avail - ml - mr;
    if (s.width > 0) w = s.width + box_extra;
    else if (s.width_pct > 0) w = avail * s.width_pct / 100;
    if (s.max_width > 0) w = kmin(w, s.max_width + box_extra);
    if (s.max_width_pct > 0) w = kmin(w, avail * s.max_width_pct / 100);
    if (s.min_width > 0) w = kmax(w, s.min_width);
    if (s.display == Style::Table && s.width <= 0 && s.width_pct <= 0) w = avail - ml - mr;
    if (w > avail - ml - mr && s.width <= 0) w = avail - ml - mr;
    if (w < 1) w = 1;
    int bx = x + ml;
    if (s.margin[3] == kUnset && s.margin[1] == kUnset) bx = x + (avail - w) / 2;
    else if (s.margin[3] == kUnset) bx = x + avail - mr - w;
    *margin_bottom = s.margin[2] == kUnset ? 0 : s.margin[2];

    Vector<Fragment>& out = *cx.out;
    size_t bgi = out.size();
    bool decorate = color_a(s.background) || s.border_width[0] || s.border_width[2] || s.border_width[1] || s.border_width[3] ||
                    s.shadow || s.grad_a || s.bg_image >= 0;
    if (decorate) {
        Fragment f;
        f.kind = Fragment::Box;
        f.bg = fade(s.background, s.opacity);
        f.border = kmax(kmax(s.border_width[0], s.border_width[2]), kmax(s.border_width[1], s.border_width[3]));
        f.border_color = fade(s.border_color ? s.border_color : hex(0xCCCCCC), s.opacity);
        f.radius = s.border_radius;
        f.node = n;
        if (s.shadow) { f.shadow_x = s.shadow_x; f.shadow_y = s.shadow_y; f.shadow_blur = s.shadow_blur; f.shadow_color = s.shadow_color; }
        f.grad_a = s.grad_a; f.grad_b = s.grad_b; f.grad_horizontal = s.grad_horizontal;
        f.image = s.bg_image;
        bool clear_border = s.border_color && !color_a(s.border_color);
        if (clear_border) f.border = 0;
        // only one edge has a border -> draw as a line, not a frame
        int edges = (s.border_width[0] > 0) + (s.border_width[1] > 0) + (s.border_width[2] > 0) + (s.border_width[3] > 0);
        if (edges == 1 && !clear_border) {
            f.border = 0;
            out.push(f);
            Fragment line;
            line.kind = Fragment::HRule;
            line.color = f.border_color;
            line.size = s.border_width[0] ? 0 : s.border_width[2] ? 2 : s.border_width[1] ? 1 : 3;   // edge id
            line.radius = kmax(kmax(s.border_width[0], s.border_width[2]), kmax(s.border_width[1], s.border_width[3]));
            out.push(line);
        } else {
            out.push(f);
        }
    }
    size_t content_start = out.size();
    int cx0 = bx + s.border_width[3] + s.padding[3];
    int cy0 = y + s.border_width[0] + s.padding[0];
    int cw = w - box_extra;
    if (cw < 1) cw = 1;
    int fixed_h = s.height > 0 ? s.height : -1;
    bool leaf = replaced(n) && n->tag != "button";   // display:flex on a textarea doesn't make it a container
    int h;
    if (n->tag == "hr") {
        Fragment f;
        f.kind = Fragment::HRule;
        f.r = Rect(bx, cy0, w, 1);
        f.color = s.border_color ? s.border_color : hex(0xD0D0D8);
        f.size = -1;
        out.push(f);
        h = 1;
    } else if (s.display == Style::Table) {
        h = layout_table(cx, n, cx0, cy0, cw, href);
    } else if (!leaf && s.display == Style::Flex) {
        int limit = fixed_h > 0 ? fixed_h : (s.max_height > 0 ? s.max_height : -1);
        h = layout_flex(cx, n, cx0, cy0, cw, limit, href);
        if (fixed_h <= 0 && limit > 0 && h > limit) h = limit;
    } else if (!leaf && s.display == Style::Grid) {
        h = layout_grid(cx, n, cx0, cy0, cw, href);
    } else if (leaf) {
        Vector<Item> items;
        add_replaced(cx, n, items, href);
        h = layout_inline(cx, items, cx0, cy0, cw, s);
    } else {
        if (s.display == Style::ListItem && !s.list_none) {
            Fragment f;
            f.kind = Fragment::Bullet;
            int lh = lh_of(s);
            if (s.list_ordered) {
                f.text = String::format("%d.", n->list_index ? n->list_index : 1);
                int tw = text_width(f.text.c_str(), s.font_size, font_of(s));
                f.r = Rect(cx0 - tw - 6, cy0, tw, lh);
            } else {
                int r = kmax(2, s.font_size / 6);
                f.r = Rect(cx0 - 14, cy0 + lh / 2 - r, r * 2, r * 2);
                f.radius = r;
            }
            f.color = s.color;
            f.size = s.font_size;
            f.font = font_of(s);
            out.push(f);
        }
        int mb;
        h = layout_children(cx, n, cx0, cy0, cw, href, &mb);
        h += mb;
    }
    int total = h + s.padding[0] + s.padding[2] + s.border_width[0] + s.border_width[2];
    int natural = total;
    bool clips = s.overflow_hidden || s.overflow_scroll;
    int box_y = s.padding[0] + s.padding[2] + s.border_width[0] + s.border_width[2];
    if (s.height > 0) total = clips ? s.height + box_y : kmax(total, s.height + box_y);
    int max_h = s.max_height > 0 ? s.max_height : (s.max_height_pct > 0 && saved_cb > 0 ? saved_cb * s.max_height_pct / 100 - s.padding[0] - s.padding[2] : -1);
    if (max_h > 0 && clips && total > max_h + s.padding[0] + s.padding[2]) total = max_h + s.padding[0] + s.padding[2];
    if (s.aspect_ratio > 0 && s.height <= 0) total = kmax(total, (int)(w / s.aspect_ratio));
    if (s.min_height > 0) total = kmax(total, s.min_height);
    if (decorate) {
        out[bgi].r = Rect(bx, y, w, total);
        if (bgi + 1 < out.size() && out[bgi + 1].kind == Fragment::HRule && out[bgi + 1].r.w == 0) {
            Fragment& l = out[bgi + 1];
            int t = l.radius;
            switch (l.size) {
            case 0: l.r = Rect(bx, y, w, t); break;
            case 1: l.r = Rect(bx + w - t, y, t, total); break;
            case 2: l.r = Rect(bx, y + total - t, w, t); break;
            default: l.r = Rect(bx, y, t, total); break;
            }
            l.size = -1;
        }
    }
    if (clips && natural > total + 1) {
        // scroll container (or clipped box): register and tag the content
        Document::Scroller sc{n, Rect(bx, y, w, total), natural, -1, false};
        int idx = (int)cx.d->scrollers.size();
        cx.d->scrollers.push(sc);
        int maxs = natural - total;
        if (n->scroll_y > maxs) n->scroll_y = maxs;
        for (size_t i = content_start; i < out.size(); i++) {
            if (out[i].box_of >= 0) continue;
            if (out[i].scroller < 0) out[i].scroller = idx;
            else if (cx.d->scrollers[out[i].scroller].parent < 0 && out[i].scroller != idx) cx.d->scrollers[out[i].scroller].parent = idx;
        }
        // marker: moves with the fragments when this subtree is translated later
        Fragment mk;
        mk.kind = Fragment::Box;
        mk.r = Rect(bx, y, w, total);
        mk.box_of = idx;
        out.push(mk);
    }
    if (s.position == Style::Relative) {
        int dx = s.offset[3] != kUnset ? s.offset[3] : s.offset[1] != kUnset ? -s.offset[1] : 0;
        int dy = s.offset[0] != kUnset ? s.offset[0] : s.offset[2] != kUnset ? -s.offset[2] : 0;
        if (dx || dy) translate_frags(out, bgi, dx, dy);
    }
    if (s.hidden) {
        for (size_t i = out.size(); i > bgi; i--) out.remove(i - 1);
    }
    return total;
}

int layout_flex(Ctx& cx, Node* n, int x, int y, int width, int fixed_h, const String& href_in) {
    const Style& s = n->style;
    String href = link_of(cx, n, href_in);
    Vector<Node*> kids;
    Vector<Node*> positioned;
    for (Node* c : n->children) {
        if (c->type == Node::Element && c->style.display == Style::None) continue;
        if (c->type == Node::Text && c->text.trim().empty()) continue;
        if (out_of_flow(c)) { positioned.push(c); continue; }
        kids.push(c);
    }
    // order
    for (size_t i = 1; i < kids.size(); i++)
        for (size_t j = i; j > 0 && kids[j]->style.order < kids[j - 1]->style.order; j--) { Node* t = kids[j]; kids[j] = kids[j - 1]; kids[j - 1] = t; }
    if (s.flex_reverse) for (size_t i = 0; i < kids.size() / 2; i++) { Node* t = kids[i]; kids[i] = kids[kids.size() - 1 - i]; kids[kids.size() - 1 - i] = t; }
    Vector<Fragment>& out = *cx.out;
    if (kids.empty()) return fixed_h > 0 ? fixed_h : 0;

    auto lay_item = [&](Node* c, int w, Vector<Fragment>& dst, int* mt, int* mb) -> int {
        Vector<Fragment>* saved = cx.out;
        cx.out = &dst;
        int h;
        *mt = 0;
        *mb = 0;
        if (c->type == Node::Text || (!is_block_level(c) && c->style.display != Style::InlineBlock && !replaced(c))) {
            Vector<Item> items;
            collect_inline(cx, c, items, href);
            Style plain = s;
            plain.text_align = Style::Left;
            h = layout_inline(cx, items, 0, 0, w, plain);
        } else {
            Style saved_style = c->style;
            if (c->style.display == Style::Inline || c->style.display == Style::InlineBlock) c->style.display = Style::Block;
            *mt = c->style.margin[0] == kUnset ? 0 : c->style.margin[0];
            // flex items: auto margins don't center inside our block layout
            int mlr[2] = {c->style.margin[1], c->style.margin[3]};
            if (c->style.margin[1] == kUnset) c->style.margin[1] = 0;
            if (c->style.margin[3] == kUnset) c->style.margin[3] = 0;
            int w_save = c->style.width, wp_save = c->style.width_pct;
            // form controls size from style.width, so hand them the flexed width
            bool control = c->tag == "textarea" || c->tag == "input" || c->tag == "select";
            c->style.width = control ? kmax(1, w - c->style.padding[1] - c->style.padding[3] - c->style.border_width[1] - c->style.border_width[3]) : -1;
            c->style.width_pct = -1;
            h = layout_block(cx, c, 0, *mt, w, mb, href) ;
            c->style.width = w_save;
            c->style.width_pct = wp_save;
            c->style.margin[1] = mlr[0];
            c->style.margin[3] = mlr[1];
            c->style = saved_style;
        }
        cx.out = saved;
        return h;
    };

    if (!s.flex_row) {
        // column: stack items, honour align-items for width. If the column has a
        // height limit and overflows, shrink items that can scroll/clip (or grow
        // items with flex-grow to fill it).
        int gap = s.gap_row;
        Vector<int> forced;
        forced.resize(kids.size());
        for (size_t i = 0; i < kids.size(); i++) forced[i] = -1;
        if (fixed_h > 0) {
            Vector<int> nat;
            int total = 0;
            for (size_t i = 0; i < kids.size(); i++) {
                Node* c = kids[i];
                int w = width;
                Vector<Fragment> tmp;
                int mt, mb;
                int h = lay_item(c, w, tmp, &mt, &mb);
                nat.push(h + mt + mb);
                total += h + mt + mb + (i + 1 < kids.size() ? gap : 0);
            }
            if (total > fixed_h) {
                int deficit = total - fixed_h;
                int shrinkable = 0;
                for (size_t i = 0; i < kids.size(); i++)
                    if (kids[i]->type == Node::Element && (kids[i]->style.overflow_scroll || kids[i]->style.overflow_hidden || kids[i]->style.flex_shrink > 0 && kids[i]->style.flex_grow > 0))
                        shrinkable += nat[i];
                for (size_t i = 0; i < kids.size() && shrinkable > 0; i++) {
                    Node* c = kids[i];
                    if (!(c->type == Node::Element && (c->style.overflow_scroll || c->style.overflow_hidden || c->style.flex_shrink > 0 && c->style.flex_grow > 0))) continue;
                    int cut = (int)((long long)deficit * nat[i] / shrinkable);
                    forced[i] = kmax(20, nat[i] - cut - c->style.padding[0] - c->style.padding[2]);
                }
            } else if (total < fixed_h) {
                float grow = 0;
                for (Node* c : kids) if (c->type == Node::Element) grow += c->style.flex_grow;
                if (grow > 0)
                    for (size_t i = 0; i < kids.size(); i++)
                        if (kids[i]->type == Node::Element && kids[i]->style.flex_grow > 0)
                            forced[i] = nat[i] + (int)((fixed_h - total) * kids[i]->style.flex_grow / grow) - kids[i]->style.padding[0] - kids[i]->style.padding[2];
            }
        }
        int cy = y;
        size_t idx = 0;
        for (size_t ki = 0; ki < kids.size(); ki++) {
            Node* c = kids[ki];
            int saved_height = c->type == Node::Element ? c->style.height : 0;
            bool saved_scroll = c->type == Node::Element ? c->style.overflow_scroll : false;
            if (forced[ki] > 0) {
                c->style.height = forced[ki];
                if (!c->style.overflow_hidden) c->style.overflow_scroll = true;
            }
            const Style& cs = c->style;
            uint8_t al = cs.align_self != 255 ? cs.align_self : s.align_items;
            int w = width;
            bool shrink = al != 0 && c->type == Node::Element;
            if (cs.width > 0) w = kmin(width, cs.width + cs.padding[1] + cs.padding[3] + cs.border_width[1] + cs.border_width[3]);
            else if (cs.width_pct > 0) w = width * cs.width_pct / 100;
            else if (shrink) w = kmin(width, max_content(c) + 1);
            Vector<Fragment> sub;
            int mt, mb;
            int h = lay_item(c, w, sub, &mt, &mb);
            int ix = x;
            if (al == 2) ix = x + (width - w) / 2;
            else if (al == 3) ix = x + width - w;
            if (c->type == Node::Element && cs.margin[3] == kUnset && cs.margin[1] == kUnset) ix = x + (width - w) / 2;
            size_t from = out.size();
            for (Fragment& f : sub) out.push(f);
            translate_frags(out, from, ix, cy);
            cy += h + mt + mb + (++idx < kids.size() ? gap : 0);
            if (c->type == Node::Element) { c->style.height = saved_height; c->style.overflow_scroll = saved_scroll; }
        }
        int h = cy - y;
        if (fixed_h > 0 && s.justify == 1 && h < fixed_h) { translate_frags(out, out.size() - 0, 0, 0); }
        for (Node* c : positioned) layout_positioned(cx, c, x, y, width, kmax(h, fixed_h), y, href);
        return kmax(h, fixed_h);
    }

    // ---- row
    struct FI { Node* node; int basis, minw, w; float grow, shrink; };
    Vector<FI> fis;
    for (Node* c : kids) {
        FI fi{c, 0, 0, 0, 0, 1};
        if (c->type == Node::Element) {
            const Style& cs = c->style;
            int extra = cs.padding[1] + cs.padding[3] + cs.border_width[1] + cs.border_width[3];
            int margins = (cs.margin[1] == kUnset ? 0 : cs.margin[1]) + (cs.margin[3] == kUnset ? 0 : cs.margin[3]);
            if (cs.flex_basis_pct >= 0) fi.basis = width * cs.flex_basis_pct / 100;
            else if (cs.flex_basis >= 0) fi.basis = cs.flex_basis + extra;
            else if (cs.width > 0) fi.basis = cs.width + extra;
            else if (cs.width_pct > 0) fi.basis = width * cs.width_pct / 100;
            else fi.basis = max_content(c) + 1 - margins;
            if (cs.max_width > 0) fi.basis = kmin(fi.basis, cs.max_width + extra);
            fi.basis += margins;
            fi.minw = kmax(cs.min_width > 0 ? cs.min_width : 0, kmin(min_content(c), fi.basis));
            if (cs.width > 0 && cs.flex_shrink == 0) fi.minw = fi.basis;
            fi.grow = cs.flex_grow;
            fi.shrink = cs.flex_shrink;
        } else {
            fi.basis = max_content(c) + 1;
            fi.minw = min_content(c);
        }
        fi.basis = kmax(0, kmin(fi.basis, kmax(width, 1) * 4));
        fis.push(fi);
    }
    // break into lines
    int gap = s.gap_col;
    Vector<int> line_starts;
    line_starts.push(0);
    if (s.flex_wrap) {
        int lw = 0;
        for (size_t i = 0; i < fis.size(); i++) {
            int add = fis[i].basis + (lw > 0 ? gap : 0);
            if (lw > 0 && lw + add > width) { line_starts.push((int)i); lw = fis[i].basis; }
            else lw += add;
        }
    }
    line_starts.push((int)fis.size());
    int cy = y;
    for (size_t L = 0; L + 1 < line_starts.size(); L++) {
        int a = line_starts[L], b = line_starts[L + 1];
        int used = 0;
        float grow_sum = 0, shrink_sum = 0;
        for (int i = a; i < b; i++) {
            used += fis[i].basis;
            grow_sum += fis[i].grow;
            shrink_sum += fis[i].shrink * fis[i].basis;
        }
        used += gap * (b - a - 1);
        int free = width - used;
        for (int i = a; i < b; i++) {
            FI& f = fis[i];
            f.w = f.basis;
            if (free > 0 && grow_sum > 0) f.w += (int)(free * f.grow / grow_sum);
            else if (free < 0 && shrink_sum > 0) f.w = kmax(f.minw, f.basis + (int)(free * f.shrink * f.basis / shrink_sum));
        }
        // re-measure after shrinking clamp
        int total = 0;
        for (int i = a; i < b; i++) total += fis[i].w;
        total += gap * (b - a - 1);
        int rem = width - total;
        // lay out items
        Vector<Vector<Fragment>> subs;
        Vector<int> hs, mts;
        int line_h = 0;
        for (int i = a; i < b; i++) {
            Vector<Fragment>& sub = subs.emplace();
            Node* c = fis[i].node;
            int margins = 0, ml = 0;
            if (c->type == Node::Element) {
                ml = c->style.margin[3] == kUnset ? 0 : c->style.margin[3];
                margins = ml + (c->style.margin[1] == kUnset ? 0 : c->style.margin[1]);
            }
            int mt, mb;
            int h = lay_item(c, kmax(1, fis[i].w - margins), sub, &mt, &mb);
            if (ml) translate_frags(sub, 0, ml, 0);
            hs.push(h + mt + mb);
            mts.push(mt);
            line_h = kmax(line_h, h + mt + mb);
        }
        if (fixed_h > 0 && line_starts.size() == 2) line_h = kmax(line_h, fixed_h);
        // justify
        int px = x, spacing = gap;
        int n_items = b - a;
        // auto margins absorb free space
        int auto_count = 0;
        for (int i = a; i < b; i++) {
            Node* c = fis[i].node;
            if (c->type == Node::Element) { if (c->style.margin[3] == kUnset) auto_count++; if (c->style.margin[1] == kUnset) auto_count++; }
        }
        if (rem > 0 && auto_count == 0) {
            switch (s.justify) {
            case 1: px += rem / 2; break;
            case 2: px += rem; break;
            case 3: if (n_items > 1) spacing = gap + rem / (n_items - 1); else px += 0; break;
            case 4: spacing = gap + rem / n_items; px += rem / n_items / 2; break;
            case 5: spacing = gap + rem / (n_items + 1); px += rem / (n_items + 1); break;
            }
        }
        int auto_share = auto_count && rem > 0 ? rem / auto_count : 0;
        for (int i = a; i < b; i++) {
            Node* c = fis[i].node;
            if (c->type == Node::Element && c->style.margin[3] == kUnset) px += auto_share;
            const Style& cs = c->type == Node::Element ? c->style : s;
            uint8_t al = c->type == Node::Element && cs.align_self != 255 ? cs.align_self : s.align_items;
            int iy = cy;
            int ih = hs[i - a];
            if (al == 2) iy = cy + (line_h - ih) / 2;
            else if (al == 3) iy = cy + line_h - ih;
            Vector<Fragment>& sub = subs[i - a];
            // stretch: extend the item's own background to the line height
            if (al == 0 && !sub.empty() && sub[0].kind == Fragment::Box && sub[0].node == c && c->style.height <= 0)
                sub[0].r.h = kmax(sub[0].r.h, line_h - mts[i - a]);
            size_t from = out.size();
            for (Fragment& f : sub) out.push(f);
            translate_frags(out, from, px, iy);
            px += fis[i].w + spacing;
            if (c->type == Node::Element && c->style.margin[1] == kUnset) px += auto_share;
        }
        cy += line_h + (L + 2 < line_starts.size() ? s.gap_row : 0);
    }
    int h = cy - y;
    for (Node* c : positioned) layout_positioned(cx, c, x, y, width, h, y, href);
    return h;
}

int layout_grid(Ctx& cx, Node* n, int x, int y, int width, const String& href_in) {
    const Style& s = n->style;
    String href = link_of(cx, n, href_in);
    Vector<Node*> kids;
    for (Node* c : n->children) {
        if (c->type == Node::Element && (c->style.display == Style::None || out_of_flow(c))) continue;
        if (c->type == Node::Text && c->text.trim().empty()) continue;
        kids.push(c);
    }
    int ncols = s.grid_ncols > 0 ? s.grid_ncols : 1;
    int gap = s.gap_col, rgap = s.gap_row;
    int fixed = 0;
    float fr = 0;
    for (int i = 0; i < ncols; i++) { fixed += s.grid_ncols ? s.grid_col_px[i] : 0; fr += s.grid_ncols ? s.grid_col_fr[i] : 1; }
    int avail = width - gap * (ncols - 1) - fixed;
    int cw[16];
    for (int i = 0; i < ncols; i++) {
        if (s.grid_ncols && s.grid_col_px[i] > 0) cw[i] = s.grid_col_px[i];
        else cw[i] = fr > 0 ? (int)(kmax(avail, 0) * (s.grid_ncols ? s.grid_col_fr[i] : 1) / fr) : avail / ncols;
    }
    Vector<Fragment>& out = *cx.out;
    int col = 0, cy = y, row_h = 0;
    for (Node* c : kids) {
        int span = c->type == Node::Element ? c->style.grid_span : 1;
        if (span < 0 || span > ncols) span = ncols;
        if (col + span > ncols) { col = 0; cy += row_h + rgap; row_h = 0; }
        int cx_ = x;
        for (int i = 0; i < col; i++) cx_ += cw[i] + gap;
        int w = 0;
        for (int i = col; i < col + span; i++) w += cw[i] + (i > col ? gap : 0);
        Vector<Fragment> sub;
        Vector<Fragment>* saved = cx.out;
        cx.out = &sub;
        int h;
        if (c->type == Node::Text || (!is_block_level(c) && c->style.display != Style::InlineBlock)) {
            Vector<Item> items;
            collect_inline(cx, c, items, href);
            h = layout_inline(cx, items, 0, 0, w, s);
        } else {
            Style saved_style = c->style;
            if (c->style.display == Style::InlineBlock) c->style.display = Style::Block;
            int mb;
            h = layout_block(cx, c, 0, 0, w, &mb, href);
            c->style = saved_style;
        }
        cx.out = saved;
        size_t from = out.size();
        for (Fragment& f : sub) out.push(f);
        translate_frags(out, from, cx_, cy);
        row_h = kmax(row_h, h);
        col += span;
        if (col >= ncols) { col = 0; cy += row_h + rgap; row_h = 0; }
    }
    if (col > 0) cy += row_h; else if (cy > y) cy -= rgap;
    return cy - y;
}

int layout_table(Ctx& cx, Node* table, int x, int y, int width, const String& href) {
    Vector<Node*> rows;
    for (Node* c : table->children) {
        if (c->type != Node::Element || c->style.display == Style::None) continue;
        if (c->tag == "tr" || c->style.display == Style::TableRow) rows.push(c);
        else if (c->tag == "thead" || c->tag == "tbody" || c->tag == "tfoot")
            for (Node* r : c->children) if (r->type == Node::Element && (r->tag == "tr" || r->style.display == Style::TableRow) && r->style.display != Style::None) rows.push(r);
    }
    if (rows.empty()) {
        // display:table holding display:table-cell children directly (no rows)
        bool cells = false;
        for (Node* c : table->children) if (c->type == Node::Element && c->style.display == Style::TableCell) cells = true;
        int mb;
        if (!cells) return layout_children(cx, table, x, y, width, href, &mb);
        Vector<Fragment>& cell_out = *cx.out;
        int want = table->style.height, cell_x = x, tallest = 0;
        struct Placed { size_t from, to; int h; uint8_t valign; };
        Vector<Placed> placed;
        for (Node* c : table->children) {
            if (c->type != Node::Element || c->style.display != Style::TableCell) continue;
            int w = c->style.width > 0 ? c->style.width + c->style.padding[1] + c->style.padding[3] : kmin(max_content(c) + 1, kmax(1, x + width - cell_x));
            size_t from = cell_out.size();
            int h = layout_block(cx, c, cell_x, y, w, &mb, href);
            placed.push(Placed{from, cell_out.size(), h, c->style.valign});
            tallest = kmax(tallest, h);
            cell_x += w;
        }
        int total = kmax(want, tallest);
        for (Placed& p : placed) {
            int dy = p.valign == 2 ? (total - p.h) / 2 : p.valign == 3 ? total - p.h : 0;
            if (dy) for (size_t i = p.from; i < p.to; i++) cell_out[i].r.y += dy;
        }
        return total;
    }
    int ncols = 0;
    for (Node* r : rows) {
        int n = 0;
        for (Node* c : r->children) if (c->type == Node::Element && (c->tag == "td" || c->tag == "th")) n += c->attr("colspan") ? kclamp(atoi(c->attr("colspan")), 1, 20) : 1;
        ncols = kmax(ncols, n);
    }
    if (!ncols) return 0;
    if (ncols > 64) ncols = 64;
    int spacing = table->attr("cellspacing") ? atoi(table->attr("cellspacing")) : 2;
    int pad = table->attr("cellpadding") ? atoi(table->attr("cellpadding")) : -1;
    Vector<int> maxw, minw, fixed;
    maxw.resize(ncols); minw.resize(ncols); fixed.resize(ncols);
    for (int i = 0; i < ncols; i++) { maxw[i] = 0; minw[i] = 0; fixed[i] = 0; }
    for (Node* r : rows) {
        int col = 0;
        for (Node* c : r->children) {
            if (c->type != Node::Element || !(c->tag == "td" || c->tag == "th")) continue;
            if (pad >= 0) for (int k = 0; k < 4; k++) c->style.padding[k] = pad;
            int span = c->attr("colspan") ? kclamp(atoi(c->attr("colspan")), 1, 20) : 1;
            if (col >= ncols) break;
            int mx = max_content(c), mn = min_content(c);
            if (span == 1) {
                maxw[col] = kmax(maxw[col], mx);
                minw[col] = kmax(minw[col], mn);
                if (c->style.width > 0) fixed[col] = kmax(fixed[col], c->style.width);
                if (c->style.width_pct > 0) fixed[col] = kmax(fixed[col], width * c->style.width_pct / 100);
            }
            col += span;
        }
    }
    int avail = width - spacing * (ncols + 1) - table->style.border_width[1] - table->style.border_width[3];
    Vector<int> cw;
    cw.resize(ncols);
    int total_max = 0, total_min = 0;
    for (int i = 0; i < ncols; i++) {
        if (fixed[i]) maxw[i] = minw[i] = kmax(fixed[i], minw[i]);
        total_max += kmax(maxw[i], 1);
        total_min += minw[i];
    }
    bool stretch = table->style.width_pct > 0 || table->style.width > 0;
    int target = table->style.width > 0 ? kmin(table->style.width, width) : table->style.width_pct > 0 ? width * table->style.width_pct / 100 : width;
    target -= spacing * (ncols + 1);
    if (total_max <= avail && !stretch) {
        for (int i = 0; i < ncols; i++) cw[i] = kmax(maxw[i], 1);
    } else if (total_max <= target) {
        int extra = target - total_max;
        int flex_total = 0;
        for (int i = 0; i < ncols; i++) if (!fixed[i]) flex_total += kmax(maxw[i], 1);
        for (int i = 0; i < ncols; i++)
            cw[i] = kmax(maxw[i], 1) + (fixed[i] ? 0 : (flex_total ? extra * kmax(maxw[i], 1) / flex_total : extra / ncols));
    } else if (total_min <= avail) {
        int extra = avail - total_min, span = kmax(1, total_max - total_min);
        for (int i = 0; i < ncols; i++) cw[i] = minw[i] + extra * kmax(0, maxw[i] - minw[i]) / span;
    } else {
        for (int i = 0; i < ncols; i++) cw[i] = kmax(10, avail * kmax(minw[i], 1) / kmax(1, total_min));
    }
    int table_w = spacing * (ncols + 1);
    for (int i = 0; i < ncols; i++) table_w += cw[i];
    int tx = x;
    if (table->style.margin[1] == kUnset && table->style.margin[3] == kUnset) tx = x + (width - table_w) / 2;
    else if (table->attr("align") && !strcasecmp(table->attr("align"), "center")) tx = x + (width - table_w) / 2;
    else if (table->parent && table->parent->style.text_align == Style::Center) tx = x + (width - table_w) / 2;
    Vector<Fragment>& out = *cx.out;
    int cy = y + spacing;
    for (Node* r : rows) {
        int col = 0;
        int cxp = tx + spacing;
        int row_h = 0;
        size_t row_start = out.size();
        Vector<size_t> cell_bg;
        bool has_rowbg = color_a(r->style.background) > 0;
        if (has_rowbg) { Fragment rowbg; rowbg.kind = Fragment::Box; rowbg.bg = r->style.background; out.push(rowbg); }
        for (Node* c : r->children) {
            if (c->type != Node::Element || !(c->tag == "td" || c->tag == "th")) continue;
            if (col >= ncols) break;
            int span = c->attr("colspan") ? kclamp(atoi(c->attr("colspan")), 1, 20) : 1;
            int w = 0;
            for (int k = 0; k < span && col + k < ncols; k++) w += cw[col + k] + (k ? spacing : 0);
            const Style& s = c->style;
            size_t bgi = out.size();
            Fragment bg;
            bg.kind = Fragment::Box;
            bg.bg = s.background;
            bg.border = s.border_width[0] ? s.border_width[0] : (table->style.border_width[0] ? 1 : 0);
            bg.border_color = s.border_color ? s.border_color : table->style.border_color;
            bg.r = Rect(cxp, cy, w, 0);
            out.push(bg);
            cell_bg.push(bgi);
            int mb;
            int inner_x = cxp + s.padding[3] + s.border_width[3];
            int inner_w = w - s.padding[1] - s.padding[3] - s.border_width[1] - s.border_width[3];
            int h = layout_children(cx, c, inner_x, cy + s.padding[0] + s.border_width[0], kmax(1, inner_w), href, &mb);
            h += s.padding[0] + s.padding[2] + s.border_width[0] + s.border_width[2];
            if (s.height > 0) h = kmax(h, s.height);
            row_h = kmax(row_h, h);
            cxp += w + spacing;
            col += span;
        }
        if (r->style.height > 0) row_h = kmax(row_h, r->style.height);
        for (size_t bi : cell_bg) out[bi].r.h = row_h;
        if (has_rowbg) out[row_start].r = Rect(tx + spacing, cy, table_w - 2 * spacing, row_h);
        cy += row_h + spacing;
    }
    return cy - y;
}

}  // namespace

void layout(Document* d, int width) {
    d->frags.clear();
    d->scrollers.clear();
    for (Canvas* c : d->inline_svgs) delete c;
    d->inline_svgs.clear();
    d->width = width;
    if (d->viewport_w != width) d->viewport_w = width;
    Ctx cx{d, &d->frags, width};
    int mb = 0;
    Node* html_el = find_tag(d->root, "html");
    Node* start = html_el ? html_el : d->root;
    if (start->style.display == Style::None || start->style.display == Style::Inline) start->style.display = Style::Block;
    int h = layout_block(cx, start, 0, 0, width, &mb, String());
    d->height = h + mb + 16;
    for (Fragment& f : d->frags)
        if (f.box_of >= 0 && f.box_of < (int)d->scrollers.size()) {
            d->scrollers[f.box_of].box = f.r;
            d->scrollers[f.box_of].fixed = f.fixed;
        }
    for (Fragment& f : d->frags) if (f.r.bottom() > d->height && f.r.bottom() < h + 4000) d->height = f.r.bottom() + 8;
}

// ================================================================ paint
// Offset (viewport y = doc y + dy) and clip (viewport coords) for a fragment.
static int frag_offset(Document* d, const Fragment& f, int page_scroll, Rect* clip) {
    int dy = f.fixed ? 0 : -page_scroll;
    *clip = Rect(-100000, -100000, 200000, 200000);
    for (int s = f.scroller; s >= 0 && s < (int)d->scrollers.size(); s = d->scrollers[s].parent) {
        const Document::Scroller& sc = d->scrollers[s];
        int box_dy = sc.fixed ? 0 : -page_scroll;
        // the box itself moves with outer scrollers too (approximate: only page scroll)
        *clip = clip->intersect(sc.box.offset(0, box_dy));
        dy -= sc.node->scroll_y;
        if (sc.parent == s) break;
    }
    return dy;
}

void paint(Document* d, Canvas& c, int scroll_y) {
    c.clear(d->background);
    Rect view(0, 0, c.width(), c.height());
    for (Fragment& f : d->frags) {
        if (f.box_of >= 0) continue;
        Rect clip;
        int dy = frag_offset(d, f, scroll_y, &clip);
        Rect vis = f.r.expand(f.shadow_blur + 8).offset(0, dy);
        if (!vis.intersects(view) || !vis.intersects(clip)) continue;
        c.save();
        c.translate(0, dy);
        c.clip_to(clip.offset(0, -dy));
        switch (f.kind) {
        case Fragment::Box: {
            int rad = kmin(f.radius, kmin(f.r.w, f.r.h) / 2);
            if (f.shadow_blur || f.shadow_x || f.shadow_y)
                c.draw_shadow(f.r, (float)rad, (float)kmax(1, f.shadow_blur / 2), f.shadow_color, f.shadow_x, f.shadow_y);
            if (f.grad_a && f.grad_b) {
                if (f.grad_horizontal) c.fill_gradient_h(f.r, f.grad_a, f.grad_b, (float)rad);
                else c.fill_gradient(f.r, f.grad_a, f.grad_b, (float)rad);
            } else if (color_a(f.bg)) c.fill_round_rect(f.r, (float)rad, f.bg);
            if (f.image >= 0 && f.image < (int)d->images.size() && d->images[f.image].canvas) {
                Canvas* im = d->images[f.image].canvas;
                c.save();
                c.clip_to(f.r);
                // cover
                float sc = kmax((float)f.r.w / im->width(), (float)f.r.h / im->height());
                int dw = (int)(im->width() * sc), dh = (int)(im->height() * sc);
                c.draw_scaled(*im, Rect(f.r.x + (f.r.w - dw) / 2, f.r.y + (f.r.h - dh) / 2, dw, dh));
                c.restore();
            }
            if (f.border) c.stroke_round_rect(f.r, (float)rad, f.border_color, (float)f.border);
            break;
        }
        case Fragment::Text:
            if (color_a(f.bg)) c.fill_rect(f.r, f.bg);
            c.draw_text(f.r.x, f.r.y + (f.r.h - line_height(f.size, f.font)) / 2, f.text.c_str(), f.color, f.size, f.font);
            if (f.underline) c.fill_rect(Rect(f.r.x, f.r.y + (f.r.h - line_height(f.size, f.font)) / 2 + font_ascent(f.size, f.font) + 2, f.r.w, kmax(1, f.size / 14)), f.color);
            if (f.strike) c.fill_rect(Rect(f.r.x, f.r.y + f.r.h / 2, f.r.w, 1), f.color);
            break;
        case Fragment::Bullet:
            if (f.text.empty()) c.fill_circle(f.r.x + f.r.w / 2.0f, f.r.y + f.r.h / 2.0f, f.r.w / 2.0f, f.color);
            else c.draw_text(f.r.x, f.r.y, f.text.c_str(), f.color, f.size, f.font);
            break;
        case Fragment::HRule:
            c.fill_rect(f.r, f.color);
            break;
        case Fragment::Frame:
            if (f.node && f.node->content_doc && f.r.w > 0 && f.r.h > 0) {
                Canvas frame(f.r.w, f.r.h);
                paint(f.node->content_doc, frame, f.node->content_scroll);
                c.draw(frame, f.r.x, f.r.y);
            }
            break;
        case Fragment::Svg:
            if (f.image >= 0 && f.image < (int)d->inline_svgs.size()) c.draw(*d->inline_svgs[f.image], f.r.x, f.r.y);
            break;
        case Fragment::Image: {
            Image* im = f.image >= 0 && f.image < (int)d->images.size() ? &d->images[f.image] : nullptr;
            if (im && im->canvas) {
                if (im->canvas->width() == f.r.w && im->canvas->height() == f.r.h) c.draw(*im->canvas, f.r.x, f.r.y);
                else c.draw_scaled(*im->canvas, f.r);
            } else if (f.r.w > 2 && f.r.h > 2 && !(im && im->failed && f.text.empty())) {
                c.fill_rect(f.r, hex(0xF2F2F5));
                if (!f.text.empty() && f.r.w > 30) c.draw_text_in(f.r.inset(4, 0), f.text.c_str(), hex(0x6E6E78), 12);
            }
            break;
        }
        case Fragment::Input: {
            Node* n = f.node;
            const Style& st = n->style;
            const char* type = n->attr("type");
            String ty = type ? String(type).lower() : String(n->tag == "button" ? "submit" : n->tag == "select" ? "select" : n->tag == "textarea" ? "textarea" : "text");
            bool focused = d->focused_input == n;
            int rad = st.border_radius ? kmin(st.border_radius, f.r.h / 2) : 6;
            Color bg = st.bg_set ? st.background : hex(0xFFFFFF);
            bool has_border = st.border_width[0] || st.border_width[1] || st.border_width[2] || st.border_width[3];
            Color border = has_border ? (st.border_color ? st.border_color : hex(0xB8B8C0)) : (st.border_set || color_a(st.background) ? 0 : hex(0xB8B8C0));
            if (ty == "submit" || ty == "button" || ty == "reset") {
                if (color_a(st.background)) c.fill_round_rect(f.r, (float)rad, st.background);
                else c.fill_gradient(f.r, hex(0xFDFDFD), hex(0xE9E9ED), (float)rad);
                if (border) c.stroke_round_rect(f.r, (float)rad, border, 1);
                c.draw_text_in(f.r, f.text.c_str(), color_a(st.background) ? st.color : hex(0x1C1C1E), kmin(st.font_size, 16), Font::Medium, Align::Center);
            } else if (ty == "checkbox" || ty == "radio") {
                float r2 = ty == "radio" ? 8 : 3;
                c.fill_round_rect(f.r, r2, n->checked ? hex(0x0A84FF) : hex(0xFFFFFF));
                c.stroke_round_rect(f.r, r2, hex(0x8E8E93), 1);
                if (n->checked) c.fill_circle(f.r.x + 8, f.r.y + 8, 3, hex(0xFFFFFF));
            } else if (ty == "select") {
                c.fill_round_rect(f.r, (float)rad, bg);
                c.stroke_round_rect(f.r, (float)rad, hex(0xB8B8C0), 1);
                c.draw_text_in(Rect(f.r.x + 8, f.r.y, f.r.w - 30, f.r.h), f.text.c_str(), hex(0x1C1C1E), 14);
                c.draw_text(f.r.right() - 18, f.r.y + (f.r.h - 20) / 2, "\xE2\x96\xBE", hex(0x555555), 14);
            } else {
                c.fill_round_rect(f.r, (float)rad, bg);
                if (color_a(border) || (focused && !st.border_set)) c.stroke_round_rect(f.r, (float)rad, focused ? hex(0x0A84FF) : border, focused ? 2.0f : 1.0f);
                String shown = ty == "password" ? String() : n->value;
                if (ty == "password") for (size_t k = 0; k < n->value.size(); k++) shown += "\xE2\x80\xA2";
                c.save();
                c.clip_to(f.r.inset(6, 2));
                int fsz = kclamp(st.font_size, 11, 18);
                int pad = kmax(8, st.padding[3]);
                if (shown.empty() && n->attr("placeholder") && !focused)
                    c.draw_text_in(Rect(f.r.x + pad, f.r.y, f.r.w - pad - 6, ty == "textarea" ? 24 : f.r.h), n->attr("placeholder"), hex(0x8A8A94), fsz);
                int tw = text_width(shown.c_str(), fsz);
                int off = tw > f.r.w - pad - 10 ? tw - (f.r.w - pad - 10) : 0;
                int ty_ = f.r.y + (ty == "textarea" ? 4 : (f.r.h - line_height(fsz)) / 2);
                c.draw_text(f.r.x + pad - off, ty_, shown.c_str(), hex(0x1C1C1E), fsz);
                if (focused) c.fill_rect(Rect(f.r.x + pad + 1 + tw - off, ty_ + 2, 2, line_height(fsz) - 4), hex(0x0A84FF));
                c.restore();
            }
            break;
        }
        }
        c.restore();
    }
    // scrollbars for scroll containers
    for (Document::Scroller& sc : d->scrollers) {
        if (!sc.node->style.overflow_scroll || sc.content_h <= sc.box.h) continue;
        Rect b = sc.box.offset(0, sc.fixed ? 0 : -scroll_y);
        int bar_h = kmax(24, b.h * b.h / sc.content_h);
        int bar_y = b.y + (b.h - bar_h) * sc.node->scroll_y / kmax(1, sc.content_h - b.h);
        c.fill_round_rect(Rect(b.right() - 7, bar_y + 2, 5, bar_h - 4), 2.5f, rgba(0, 0, 0, 80));
    }
}

const Fragment* hit(Document* d, int x, int vy, int page_scroll) {
    const Fragment* best = nullptr;
    for (size_t i = d->frags.size(); i > 0; i--) {
        const Fragment& f = d->frags[i - 1];
        if (f.box_of >= 0) continue;
        Rect clip;
        int dy = frag_offset(d, f, page_scroll, &clip);
        if (!clip.contains(x, vy)) continue;
        if (!f.r.offset(0, dy).expand(1).contains(x, vy)) continue;
        if (f.kind == Fragment::Input || f.kind == Fragment::Frame || !f.href.empty()) return &f;
        if (!best && f.kind != Fragment::Box) best = &f;
    }
    return best;
}

Node* hit_node(Document* d, int x, int vy, int page_scroll) {
    for (size_t i = d->frags.size(); i > 0; i--) {
        const Fragment& f = d->frags[i - 1];
        if (f.box_of >= 0 || !f.node) continue;
        Rect clip;
        int dy = frag_offset(d, f, page_scroll, &clip);
        if (clip.contains(x, vy) && f.r.offset(0, dy).expand(1).contains(x, vy)) return f.node;
    }
    return nullptr;
}

Rect fragment_view_rect(Document* d, const Fragment* f, int page_scroll) {
    Rect clip;
    int dy = frag_offset(d, *f, page_scroll, &clip);
    return f->r.offset(0, dy);
}

bool scroll_at(Document* d, int x, int vy, int page_scroll, int delta) {
    // innermost (last registered) scroller under the point that can move
    for (size_t i = d->scrollers.size(); i > 0; i--) {
        Document::Scroller& sc = d->scrollers[i - 1];
        Rect b = sc.box.offset(0, sc.fixed ? 0 : -page_scroll);
        if (!b.contains(x, vy)) continue;
        int maxs = sc.content_h - sc.box.h;
        int ns = kclamp(sc.node->scroll_y + delta, 0, kmax(0, maxs));
        if (ns != sc.node->scroll_y) { sc.node->scroll_y = ns; return true; }
    }
    // a fixed full-screen overlay (cookie dialogs) blocks page scrolling underneath
    return false;
}

int anchor_y(Document* d, const char* anchor) {
    Vector<Node*> st;
    st.push(d->root);
    while (!st.empty()) {
        Node* n = st.back();
        st.pop();
        if (n->type == Node::Element) {
            const char* id = n->attr("id");
            const char* nm = n->attr("name");
            if ((id && !strcmp(id, anchor)) || (nm && !strcmp(nm, anchor))) return n->layout_y >= 0 ? n->layout_y : 0;
        }
        for (Node* c : n->children) st.push(c);
    }
    return -1;
}

// ================================================================ forms
static void url_encode(String& out, const String& v) {
    static const char* hexd = "0123456789ABCDEF";
    for (size_t i = 0; i < v.size(); i++) {
        uint8_t c = (uint8_t)v[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else if (c == ' ') out += '+';
        else { out += '%'; out += hexd[c >> 4]; out += hexd[c & 15]; }
    }
}

static void gather_fields(Node* n, String& q, Node* submitter) {
    if (n->type != Node::Element) return;
    const char* name = n->attr("name");
    if (name && (n->tag == "input" || n->tag == "select" || n->tag == "textarea" || n->tag == "button")) {
        const char* type = n->attr("type");
        String ty = type ? String(type).lower() : String(n->tag == "button" ? "submit" : "text");
        bool include = true;
        String val = n->value;
        if (ty == "submit" || ty == "button" || ty == "image" || ty == "reset") { include = n == submitter; val = n->attr("value") ? n->attr("value") : ""; }
        else if (ty == "checkbox" || ty == "radio") { include = n->checked; val = n->attr("value") ? n->attr("value") : "on"; }
        else if (ty == "hidden") val = n->attr("value") ? n->attr("value") : "";
        else if (n->tag == "select") {
            Node* opt = nullptr;
            for (Node* c : n->children) if (c->tag == "option") { if (!opt || c->attr("selected")) opt = c; }
            val = opt ? (opt->attr("value") ? String(opt->attr("value")) : inner_text(opt).trim()) : String();
        }
        if (include) {
            if (!q.empty()) q += '&';
            url_encode(q, String(name));
            q += '=';
            url_encode(q, val);
        }
    }
    for (Node* c : n->children) gather_fields(c, q, submitter);
}

static String submit_form(Document* d, Node* from) {
    Node* form = from;
    while (form && form->tag != "form") form = form->parent;
    if (!form) return String();
    String q;
    gather_fields(form, q, from);
    const char* action = form->attr("action");
    char abs[2300];
    net::url_resolve(d->url.c_str(), action && *action ? action : d->url.c_str(), abs, sizeof(abs));
    String url = abs;
    long qm = url.find('?');
    if (qm >= 0) url = url.substr(0, qm);
    return url + "?" + q;
}

bool click_input(Document* d, const Fragment* f) {
    Node* n = f->node;
    if (!n) return false;
    const char* type = n->attr("type");
    String ty = type ? String(type).lower() : String(n->tag == "button" ? "submit" : "text");
    if (ty == "checkbox") { n->checked = !n->checked; return true; }
    if (ty == "radio") { n->checked = true; return true; }
    d->focused_input = n;
    return true;
}

String key_input(Document* d, uint32_t ch, bool enter, bool backspace) {
    Node* n = d->focused_input;
    if (!n) return String();
    const char* type = n->attr("type");
    String ty = type ? String(type).lower() : String(n->tag == "button" ? "submit" : "text");
    if (enter || ty == "submit" || ty == "image") return submit_form(d, n);
    if (backspace) {
        if (!n->value.empty()) {
            size_t k = n->value.size() - 1;
            while (k > 0 && ((uint8_t)n->value[k] & 0xC0) == 0x80) k--;
            n->value.erase(k, n->value.size() - k);
        }
    } else if (ch) {
        char buf[5];
        int len = utf8_encode(ch, buf);
        n->value.append(buf, len);
    }
    return String();
}

}  // namespace html
