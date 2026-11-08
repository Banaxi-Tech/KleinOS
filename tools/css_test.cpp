// Host harness for HTMLEngine parsing + styling (no layout/paint).
// g++ -std=gnu++20 -O2 -Ikernel/libc -Ikernel -Ilibs -o /tmp/claude-1000/css_test tools/css_test.cpp libs/HTMLEngine/dom.cpp libs/HTMLEngine/css.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
static void on_alarm(int) { void* b[32]; int n = backtrace(b, 32); backtrace_symbols_fd(b, n, 2); _exit(3); }
#include "HTMLEngine/html.h"
#include "HTMLEngine/internal.h"

void* kmalloc(size_t n) { return malloc(n); }
void kfree(void* p) { free(p); }
void* kcalloc(size_t a, size_t b) { return calloc(a, b); }
void* krealloc(void* p, size_t n) { return realloc(p, n); }
int utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000) { out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F); out[2] = 0x80 | (cp & 0x3F); return 3; }
    out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 0x3F); out[2] = 0x80 | ((cp >> 6) & 0x3F); out[3] = 0x80 | (cp & 0x3F); return 4;
}




namespace html {
Node* find_tag(Node* n, const char* tag) {
    if (!n) return nullptr;
    if (n->type == Node::Element && n->tag == tag) return n;
    for (Node* c : n->children) if (Node* f = find_tag(c, tag)) return f;
    return nullptr;
}
String inner_text(Node* n) { if (n->type == Node::Text) return n->text; String s; for (Node* c : n->children) s += inner_text(c); return s; }
void resolve_url(Document*, const char* ref, char* out, size_t size) { snprintf(out, size, "%s", ref); }
static void collect_styles(Node* n, Document* d) {
    if (n->type == Node::Element && n->tag == "style") {
        for (Node* c : n->children) if (c->type == Node::Text) parse_css(d, c->text.c_str(), c->text.size());
        return;
    }
    for (Node* c : n->children) collect_styles(c, d);
}
Document* parse(const char* src, size_t len, const char* url, bool scripting) {
    Document* d = new Document();
    d->url = url; d->scripting = scripting;
    d->root = parse_dom(src, len);
    collect_styles(d->root, d);
    compute_styles(d);
    return d;
}
void add_stylesheet(Document* d, const char* css, size_t len) { parse_css(d, css, len); compute_styles(d); }
}
Canvas::~Canvas() {}

static char* slurp(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = (char*)malloc(*len + 1);
    fread(b, 1, *len, f);
    b[*len] = 0;
    fclose(f);
    return b;
}

static const char* dname(int d) {
    static const char* n[] = {"inline", "block", "none", "list-item", "table", "row", "cell", "inline-block", "flex", "grid", "contents"};
    return n[d];
}

static void dump(html::Node* n, int depth, int maxdepth) {
    if (depth > maxdepth) return;
    if (n->type == html::Node::Element) {
        const html::Style& s = n->style;
        const char* cls = n->attr("class");
        printf("%*s<%s class='%.60s'> display=%s bg=%08x color=%08x fs=%d pad=%d,%d gap=%d just=%d align=%d\n", depth * 2, "", n->tag.c_str(),
               cls ? cls : "", dname(s.display), s.background, s.color, s.font_size, s.padding[0], s.padding[3], s.gap_col, s.justify, s.align_items);
        if (s.display == html::Style::None) return;
    }
    for (html::Node* c : n->children) dump(c, depth + 1, maxdepth);
}

namespace html { extern void (*g_css_debug)(Node*, const char*, const char*, const char*); }
static const char* g_watch;
static void dbg(html::Node* n, const char* sel, const char* prop, const char* val) {
    const char* c = n->attr("class");
    if (g_watch && c && strstr(c, g_watch)) printf("  [%s] %s: %s  <- %s\n", c, prop, val, sel);
}
int main(int argc, char** argv) {
    g_watch = getenv("WATCH");
    if (g_watch) html::g_css_debug = dbg;
    signal(SIGALRM, on_alarm);
    alarm(argc > 4 ? atoi(argv[4]) : 5);
    setvbuf(stdout, nullptr, _IONBF, 0);
    size_t hl, cl = 0;
    char* h = slurp(argv[1], &hl);
    char* css = argc > 2 ? slurp(argv[2], &cl) : nullptr;
    clock_t t0 = clock();
    html::Document* d = html::parse(h, hl, "https://example/", true);
    clock_t t1 = clock();
    printf("parse+style: %.1f ms, rules=%zu\n", (t1 - t0) * 1000.0 / CLOCKS_PER_SEC, d->rules.size());
    if (css) {
        t0 = clock();
        html::add_stylesheet(d, css, cl);
        t1 = clock();
        printf("stylesheet: %.1f ms, rules=%zu\n", (t1 - t0) * 1000.0 / CLOCKS_PER_SEC, d->rules.size());
    }
    html::Node* body = html::find_tag(d->root, "body");
    dump(body ? body : d->root, 0, argc > 3 ? atoi(argv[3]) : 6);
    return 0;
}
