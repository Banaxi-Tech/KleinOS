// HTML tokenizer + tree builder (forgiving, HTML5-ish implied end tags).
#include "html.h"
#include "internal.h"
#include "lib/string.h"

namespace html {

const char* Node::attr(const char* name) const {
    for (const Attr& a : attrs)
        if (a.name == name) return a.value.c_str();
    return nullptr;
}

bool Node::has_class(const char* cls) const {
    const char* c = attr("class");
    if (!c) return false;
    size_t n = strlen(cls);
    const char* p = c;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        const char* s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if ((size_t)(p - s) == n && strncmp(s, cls, n) == 0) return true;
    }
    return false;
}

static struct { const char* name; uint32_t cp; } kEntities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"copy", 0xA9},
    {"reg", 0xAE}, {"trade", 0x2122}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"laquo", 0xAB},
    {"raquo", 0xBB}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bull", 0x2022},
    {"middot", 0xB7}, {"times", 0xD7}, {"divide", 0xF7}, {"deg", 0xB0}, {"plusmn", 0xB1}, {"para", 0xB6},
    {"sect", 0xA7}, {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5}, {"cent", 0xA2}, {"auml", 0xE4},
    {"ouml", 0xF6}, {"uuml", 0xFC}, {"Auml", 0xC4}, {"Ouml", 0xD6}, {"Uuml", 0xDC}, {"szlig", 0xDF},
    {"eacute", 0xE9}, {"egrave", 0xE8}, {"aacute", 0xE1}, {"agrave", 0xE0}, {"ccedil", 0xE7}, {"ntilde", 0xF1},
    {"larr", 0x2190}, {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193}, {"hearts", 0x2665}, {"zwj", 0},
    {"zwnj", 0}, {"shy", 0}, {"thinsp", 0x2009}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"iexcl", 0xA1},
    {"iquest", 0xBF}, {"frac12", 0xBD}, {"frac14", 0xBC}, {"sup2", 0xB2}, {"micro", 0xB5},
};

String decode_entities(const char* s, size_t len) {
    String out;
    out.reserve(len);
    const char* end = s + len;
    while (s < end) {
        if (*s != '&') { out += *s++; continue; }
        const char* semi = s + 1;
        while (semi < end && semi - s < 12 && *semi != ';' && (isalnum(*semi) || *semi == '#')) semi++;
        uint32_t cp = 0xFFFFFFFF;
        if (semi < end && *semi == ';') {
            if (s[1] == '#') {
                cp = (s[2] == 'x' || s[2] == 'X') ? (uint32_t)strtoul(s + 3, nullptr, 16) : (uint32_t)strtoul(s + 2, nullptr, 10);
            } else {
                size_t n = semi - s - 1;
                for (auto& e : kEntities)
                    if (strlen(e.name) == n && strncmp(e.name, s + 1, n) == 0) { cp = e.cp; break; }
            }
        }
        if (cp == 0xFFFFFFFF) { out += *s++; continue; }
        if (cp) {
            char buf[5];
            int n = utf8_encode(cp == 0xA0 ? 0xA0 : cp, buf);
            out.append(buf, n);
        }
        s = semi + 1;
    }
    return out;
}

static bool is_void(const String& t) {
    static const char* v[] = {"br", "img", "hr", "input", "meta", "link", "area", "base", "col", "embed", "param", "source", "track", "wbr"};
    for (const char* x : v) if (t == x) return true;
    return false;
}

static bool closes_p(const String& t) {
    static const char* v[] = {"p", "div", "ul", "ol", "table", "h1", "h2", "h3", "h4", "h5", "h6", "pre", "blockquote",
                              "section", "article", "header", "footer", "nav", "form", "hr", "dl", "address", "aside", "main", "figure"};
    for (const char* x : v) if (t == x) return true;
    return false;
}

static Node* new_node(Node::Type t, Node* parent) {
    Node* n = new Node();
    n->type = t;
    n->parent = parent;
    if (parent) parent->children.push(n);
    return n;
}

void free_node(Node* n) {
    if (!n) return;
    for (Node* c : n->children) free_node(c);
    delete n;
}

Document::~Document() {
    free_rules(this);
    free_node(root);
    for (Node* o : orphans) if (!o->parent) free_node(o);
    for (Image& im : images) delete im.canvas;
    for (Canvas* c : inline_svgs) delete c;
}

static bool in_scope(Node* cur, const char* tag) {
    for (Node* n = cur; n && n->type == Node::Element; n = n->parent)
        if (n->tag == tag) return true;
    return false;
}

Node* parse_dom(const char* src, size_t len) {
    Node* doc = new Node();
    doc->type = Node::Document;
    doc->tag = "#document";
    Node* cur = doc;
    const char* p = src;
    const char* end = src + len;
    auto close_to = [&](const char* tag) {
        for (Node* n = cur; n && n != doc; n = n->parent)
            if (n->tag == tag) { cur = n->parent; return; }
    };
    while (p < end) {
        if (*p == '<') {
            if (p + 4 <= end && strncmp(p, "<!--", 4) == 0) {
                const char* e = strstr(p + 4, "-->");
                p = e ? e + 3 : end;
                continue;
            }
            if (p + 1 < end && (p[1] == '!' || p[1] == '?')) {
                const char* e = (const char*)memchr(p, '>', end - p);
                p = e ? e + 1 : end;
                continue;
            }
            bool closing = p + 1 < end && p[1] == '/';
            const char* q = p + (closing ? 2 : 1);
            if (q >= end || !isalpha(*q)) { new_node(Node::Text, cur)->text = "<"; p++; continue; }
            const char* ts = q;
            while (q < end && (isalnum(*q) || *q == '-' || *q == ':')) q++;
            String tag = String(ts, q - ts).lower();
            if (closing) {
                const char* e = (const char*)memchr(q, '>', end - q);
                p = e ? e + 1 : end;
                if (in_scope(cur, tag.c_str())) close_to(tag.c_str());
                continue;
            }
            // attributes
            Vector<Attr> attrs;
            bool self_close = false;
            while (q < end && *q != '>') {
                while (q < end && isspace(*q)) q++;
                if (q < end && *q == '/') { self_close = true; q++; continue; }
                if (q >= end || *q == '>') break;
                const char* ns = q;
                while (q < end && !isspace(*q) && *q != '=' && *q != '>' && *q != '/') q++;
                Attr a;
                a.name = String(ns, q - ns).lower();
                while (q < end && isspace(*q)) q++;
                if (q < end && *q == '=') {
                    q++;
                    while (q < end && isspace(*q)) q++;
                    if (q < end && (*q == '"' || *q == '\'')) {
                        char quote = *q++;
                        const char* vs = q;
                        while (q < end && *q != quote) q++;
                        a.value = decode_entities(vs, q - vs);
                        if (q < end) q++;
                    } else {
                        const char* vs = q;
                        while (q < end && !isspace(*q) && *q != '>') q++;
                        a.value = decode_entities(vs, q - vs);
                    }
                }
                if (!a.name.empty()) attrs.push(a);
                if (q == ns) q++;
            }
            p = q < end ? q + 1 : end;
            // implied end tags
            if (closes_p(tag) && in_scope(cur, "p")) close_to("p");
            if (tag == "li" && cur->tag == "li") close_to("li");
            if ((tag == "dt" || tag == "dd") && (cur->tag == "dt" || cur->tag == "dd")) cur = cur->parent;
            if ((tag == "td" || tag == "th") && (cur->tag == "td" || cur->tag == "th")) cur = cur->parent;
            if (tag == "tr") { if (cur->tag == "td" || cur->tag == "th") cur = cur->parent; if (cur->tag == "tr") cur = cur->parent; }
            if (tag == "option" && cur->tag == "option") cur = cur->parent;
            Node* el = new_node(Node::Element, cur);
            el->tag = tag;
            el->attrs = std::move(attrs);
            // raw text elements
            if (tag == "script" || tag == "style" || tag == "textarea" || tag == "title" || tag == "xmp") {
                String close = String("</") + tag;
                const char* e = p;
                while (e < end) {
                    if (*e == '<' && e + close.size() <= end && strncasecmp(e, close.c_str(), close.size()) == 0) break;
                    e++;
                }
                Node* t = new_node(Node::Text, el);
                t->text = (tag == "script" || tag == "style") ? String(p, e - p) : decode_entities(p, e - p);
                const char* gt = e < end ? (const char*)memchr(e, '>', end - e) : nullptr;
                p = gt ? gt + 1 : end;
                continue;
            }
            if (!is_void(tag) && !self_close) cur = el;
            continue;
        }
        const char* ts = p;
        while (p < end && *p != '<') p++;
        Node* t = new_node(Node::Text, cur);
        t->text = decode_entities(ts, p - ts);
    }
    return doc;
}

}  // namespace html
