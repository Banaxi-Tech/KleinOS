// CSS engine: tokenizer-ish parser (escapes, @media/@layer/@supports),
// selectors (descendant/child/sibling combinators, attribute operators,
// :is/:where/:not/:first-child/...), custom properties with var(), modern
// colors (rgb/hsl/oklch/oklab), flexbox/grid/positioning properties, and the
// cascade with specificity, !important and inheritance.
#include "html.h"
#include "internal.h"
#include "lib/string.h"
#include "lib/math.h"
#include <stdint.h>

namespace html {

// ================================================================ custom properties
struct VarMap {
    Vector<String> names, values;
    VarMap* parent = nullptr;
    const String* get(const char* name) const {
        for (const VarMap* m = this; m; m = m->parent)
            for (size_t i = 0; i < m->names.size(); i++)
                if (m->names[i] == name) return &m->values[i];
        return nullptr;
    }
    void set(const String& n, const String& v) {
        for (size_t i = 0; i < names.size(); i++) if (names[i] == n) { values[i] = v; return; }
        names.push(n);
        values.push(v);
    }
};

void free_vars(Node* n) {
    if (n->own_vars) { delete n->own_vars; n->own_vars = nullptr; }
    for (Node* c : n->children) free_vars(c);
}

// ================================================================ selectors
struct Selector;

struct AttrTest {
    String name, value;
    char op;   // 0 exists, '=' equals, '~' word, '^' prefix, '$' suffix, '*' contains, '|' dash
};

static const Color kClearBorder = 0x00000001;   // explicit transparent border (0 means "not set"; alpha is 0)

struct Pseudo {
    enum Kind : uint8_t { FirstChild, LastChild, OnlyChild, NthChild, NthLastChild, Not, Is, Empty, Root, FirstOfType, LastOfType, Has, Checked, Disabled, Enabled } kind;
    bool has_child = false;           // :has(> x)
    int a = 0, b = 0;                 // nth-child(an+b)
    Vector<Selector*> list;           // for :not / :is / :where
};

struct Compound {
    String tag;                       // "" = any
    String id;
    Vector<String> classes;
    Vector<AttrTest> attrs;
    Vector<Pseudo> pseudos;
    bool impossible = false;          // unsupported (hover, ::before ...)
};

struct Selector {
    Vector<Compound> parts;           // left .. right
    Vector<char> combinators;         // between parts: ' ', '>', '+', '~'
    int specificity = 0;
    ~Selector() {
        for (Compound& c : parts)
            for (Pseudo& p : c.pseudos)
                for (Selector* s : p.list) delete s;
    }
};

struct Decl { String prop, value; bool important; };

struct Document::Rule {
    Selector sel;
    Vector<Decl> decls;
    int order;
    String text;   // selector source (debugging)
};

void (*g_css_debug)(Node* n, const char* selector, const char* prop, const char* value) = nullptr;

void free_rules(Document* d) {
    for (Document::Rule* r : d->rules) delete r;
    d->rules.clear();
    if (d->root) free_vars(d->root);
}

static int g_viewport = 1024;
static int g_viewport_h = 640;
void set_viewport(int w, int h) { g_viewport = w; g_viewport_h = h; }
static Document* g_doc;

// ---------------------------------------------------------------- lexing helpers
static void skip_ws_comments(const char*& p, const char* end) {
    while (p < end) {
        if (isspace(*p)) { p++; continue; }
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            const char* e = p + 2;
            while (e + 1 < end && !(e[0] == '*' && e[1] == '/')) e++;
            p = e + 2 <= end ? e + 2 : end;
            continue;
        }
        break;
    }
}

static bool ident_char(char c) { return isalnum(c) || c == '-' || c == '_' || (uint8_t)c >= 0x80; }

// Read a CSS identifier with escapes (\: \/ \. \31 ...).
static String read_ident(const char*& p, const char* end) {
    String out;
    while (p < end) {
        if (*p == '\\' && p + 1 >= end) { p++; break; }   // dangling escape: consume it
        if (*p == '\\' && p + 1 < end) {
            p++;
            if (isxdigit(*p)) {
                uint32_t cp = 0;
                int n = 0;
                while (p < end && isxdigit(*p) && n < 6) { cp = cp * 16 + (isdigit(*p) ? *p - '0' : (tolower(*p) - 'a' + 10)); p++; n++; }
                if (p < end && *p == ' ') p++;
                char buf[5];
                int k = utf8_encode(cp ? cp : 0xFFFD, buf);
                out.append(buf, k);
            } else {
                out += *p++;
            }
            continue;
        }
        if (!ident_char(*p)) break;
        out += *p++;
    }
    return out;
}

static bool parse_selector(const char*& p, const char* end, Selector& sel, char stop);

static void parse_nth(const String& arg, int& a, int& b) {
    String s = arg.trim().lower();
    if (s == "odd") { a = 2; b = 1; return; }
    if (s == "even") { a = 2; b = 0; return; }
    long n = s.find('n');
    if (n < 0) { a = 0; b = atoi(s.c_str()); return; }
    String as = s.substr(0, n).trim();
    a = as.empty() || as == "+" ? 1 : as == "-" ? -1 : atoi(as.c_str());
    String bs = s.substr(n + 1).trim();
    String bb;
    for (size_t i = 0; i < bs.size(); i++) if (bs[i] != ' ') bb += bs[i];
    b = bb.empty() ? 0 : atoi(bb.c_str());
}

static bool parse_compound(const char*& p, const char* end, Compound& c, int& spec) {
    bool any = false;
    while (p < end) {
        char ch = *p;
        if (ch == '*') { p++; any = true; continue; }
        if (ident_char(ch) || ch == '\\') {
            c.tag = read_ident(p, end).lower();
            spec += 1;
            any = true;
            continue;
        }
        if (ch == '#' || ch == '.') {
            p++;
            String name = read_ident(p, end);
            if (ch == '#') { c.id = name; spec += 100; }
            else { c.classes.push(name); spec += 10; }
            any = true;
            continue;
        }
        if (ch == '[') {
            p++;
            while (p < end && *p == ' ') p++;
            AttrTest t;
            t.name = read_ident(p, end).lower();
            while (p < end && *p == ' ') p++;
            t.op = 0;
            if (p < end && *p != ']') {
                if (*p == '=') { t.op = '='; p++; }
                else if (p + 1 < end && p[1] == '=') { t.op = *p; p += 2; }
                while (p < end && *p == ' ') p++;
                if (p < end && (*p == '"' || *p == '\'')) {
                    char q = *p++;
                    const char* s = p;
                    while (p < end && *p != q) p++;
                    t.value = String(s, p - s);
                    if (p < end) p++;
                } else {
                    t.value = read_ident(p, end);
                }
                while (p < end && *p != ']') p++;   // flags like " i"
            }
            if (p < end) p++;
            c.attrs.push(t);
            spec += 10;
            any = true;
            continue;
        }
        if (ch == ':') {
            p++;
            bool element = p < end && *p == ':';
            if (element) p++;
            String name = read_ident(p, end).lower();
            String arg;
            const char* arg_s = nullptr;
            const char* arg_e = nullptr;
            if (p < end && *p == '(') {
                int depth = 0;
                arg_s = p + 1;
                while (p < end) {
                    if (*p == '\\' && p + 1 < end) { p += 2; continue; }   // escaped char
                    if (*p == '(') depth++;
                    else if (*p == ')' && --depth == 0) { arg_e = p; p++; break; }
                    p++;
                }
                if (arg_e) arg = String(arg_s, arg_e - arg_s);
            }
            any = true;
            if (element) { c.impossible = true; continue; }   // ::before etc.
            Pseudo ps;
            if (name == "first-child") ps.kind = Pseudo::FirstChild;
            else if (name == "last-child") ps.kind = Pseudo::LastChild;
            else if (name == "only-child") ps.kind = Pseudo::OnlyChild;
            else if (name == "first-of-type") ps.kind = Pseudo::FirstOfType;
            else if (name == "last-of-type") ps.kind = Pseudo::LastOfType;
            else if (name == "empty") ps.kind = Pseudo::Empty;
            else if (name == "root") ps.kind = Pseudo::Root;
            else if (name == "checked") ps.kind = Pseudo::Checked;
            else if (name == "disabled") ps.kind = Pseudo::Disabled;
            else if (name == "enabled") ps.kind = Pseudo::Enabled;
            else if (name == "nth-child" || name == "nth-last-child") {
                ps.kind = name == "nth-child" ? Pseudo::NthChild : Pseudo::NthLastChild;
                parse_nth(arg, ps.a, ps.b);
            } else if ((name == "not" || name == "is" || name == "where" || name == "matches" || name == "any" || name == "has") && arg_s) {
                ps.kind = name == "not" ? Pseudo::Not : name == "has" ? Pseudo::Has : Pseudo::Is;
                const char* q = arg_s;
                if (ps.kind == Pseudo::Has) {
                    while (q < arg_e && *q == ' ') q++;
                    if (q < arg_e && (*q == '+' || *q == '~')) { c.impossible = true; continue; }   // sibling forms unsupported
                    if (q < arg_e && *q == '>') { ps.has_child = true; q++; while (q < arg_e && *q == ' ') q++; }
                }
                int best = 0;
                while (q < arg_e) {
                    Selector* sub = new Selector();
                    const char* before = q;
                    bool ok = parse_selector(q, arg_e, *sub, ',');
                    bool impossible = false;
                    for (Compound& cc : sub->parts) if (cc.impossible) impossible = true;
                    if (ok && !impossible) { if (sub->specificity > best) best = sub->specificity; ps.list.push(sub); }
                    else delete sub;
                    if (q < arg_e && *q == ',') q++;
                    if (q == before) q++;
                }
                if (ps.list.empty() && ps.kind != Pseudo::Not) { c.impossible = true; continue; }
                if (name != "where") spec += best;
                c.pseudos.push(std::move(ps));
                continue;
            } else if (name == "link" || name == "visited" || name == "any-link" || name == "lang" || name == "dir" || name == "defined") {
                continue;   // treat as always matching
            } else {
                c.impossible = true;   // :hover, :focus ... (would need a restyle on every state change)
                continue;
            }
            spec += 10;
            c.pseudos.push(std::move(ps));
            continue;
        }
        break;
    }
    return any;
}

// Parse one complex selector until `stop` (',' or '{') or end.
static bool parse_selector(const char*& p, const char* end, Selector& sel, char stop) {
    char comb = 0;
    while (p < end && *p != stop) {
        if (isspace(*p)) { p++; if (!comb && !sel.parts.empty()) comb = ' '; continue; }
        if (*p == '>' || *p == '+' || *p == '~') { comb = *p++; continue; }
        Compound c;
        const char* before = p;
        if (!parse_compound(p, end, c, sel.specificity)) {
            if (p == before) { p++; return false; }
            return false;
        }
        if (!sel.parts.empty()) sel.combinators.push(comb ? comb : ' ');
        else if (comb && comb != ' ') {}   // leading combinator (inside :is(>a)) ignored
        sel.parts.push(std::move(c));
        comb = 0;
    }
    return !sel.parts.empty();
}

// ---------------------------------------------------------------- media queries
static double media_px(const char* v) {
    while (*v == ':' || *v == ' ') v++;
    char* e;
    double px = strtod(v, &e);
    if (!strncmp(e, "em", 2) || !strncmp(e, "rem", 3)) px *= 16;
    return px;
}

static bool media_one(const String& q) {
    String m = q.trim().lower();
    bool neg = false;
    if (m.starts_with("not ")) { neg = true; m = m.substr(4).trim(); }
    if (m.starts_with("only ")) m = m.substr(5).trim();
    bool ok = true;
    if (m.starts_with("print") || m.starts_with("speech")) ok = false;
    // check each (feature) in an "and" chain
    long i = 0;
    while ((i = m.find('(', i)) >= 0) {
        long j = m.find(')', i);
        if (j < 0) break;
        String f = m.substr(i + 1, j - i - 1).trim();
        if (f.starts_with("min-width")) { if (media_px(f.c_str() + 9) > g_viewport) ok = false; }
        else if (f.starts_with("max-width")) { if (media_px(f.c_str() + 9) < g_viewport) ok = false; }
        else if (f.starts_with("width >=") || f.starts_with("width>=")) { if (media_px(f.c_str() + f.find('=') + 1) > g_viewport) ok = false; }
        else if (f.starts_with("width <") || f.starts_with("width<")) { if (media_px(f.c_str() + f.find('<') + 1 + (f.find("<=") >= 0)) <= g_viewport) ok = false; }
        else if (f.starts_with("min-height")) { if (media_px(f.c_str() + 10) > 900) ok = false; }
        else if (f.starts_with("max-height")) { if (media_px(f.c_str() + 10) < 700) ok = false; }
        else if (f.find("prefers-color-scheme") >= 0) { if (f.find("dark") >= 0) ok = false; }
        else if (f.find("prefers-reduced-motion") >= 0) { if (f.find("reduce") >= 0) ok = false; }
        else if (f.find("prefers-contrast") >= 0 || f.find("forced-colors") >= 0) ok = false;
        else if (f.starts_with("hover") || f.starts_with("pointer")) { if (f.find("none") >= 0 || f.find("coarse") >= 0) ok = false; }
        else if (f.starts_with("orientation")) { if (f.find("portrait") >= 0) ok = false; }
        i = j + 1;
    }
    return neg ? !ok : ok;
}

static bool media_matches(const String& prelude) {
    // "@media <list>"
    String q = prelude;
    long sp = q.find(' ');
    q = sp >= 0 ? q.substr(sp + 1) : String();
    if (q.trim().empty()) return true;
    long start = 0;
    for (size_t i = 0; i <= q.size(); i++) {
        if (i == q.size() || q[i] == ',') {
            if (media_one(q.substr(start, i - start))) return true;
            start = i + 1;
        }
    }
    return false;
}

// ---------------------------------------------------------------- rules
static void add_rules(Document* d, const char* sel_s, const char* sel_e, Vector<Decl>& decls);
static void parse_decls(const char* p, const char* end, Vector<Decl>& out);

// CSS nesting: emit "parent child" rules for blocks nested in [p, end).
static void parse_nested(Document* d, const String& parent, const char* p, const char* end, int depth) {
    if (depth > 8) return;
    const char* start = p;
    while (p < end) {
        if (*p == ';') { start = ++p; continue; }
        if (*p == '(') { int par = 0; while (p < end) { if (*p == '(') par++; else if (*p == ')' && --par == 0) break; p++; } if (p < end) p++; continue; }
        if (*p != '{') { p++; continue; }
        String child = String(start, p - start).trim();
        const char* body = ++p;
        int lvl = 1;
        while (p < end && lvl) { if (*p == '{') lvl++; else if (*p == '}' && --lvl == 0) break; p++; }
        const char* body_end = p;
        if (p < end) p++;
        start = p;
        if (child.starts_with("@")) {
            if (!child.lower().starts_with("@media") || media_matches(child)) parse_nested(d, parent, body, body_end, depth + 1);
            continue;
        }
        // cross parent list x child list; '&' marks where the parent goes
        String full;
        long ps = 0;
        for (size_t i = 0; i <= parent.size(); i++) {
            if (i != parent.size() && parent[i] != ',') continue;
            String one = parent.substr(ps, i - ps).trim();
            ps = i + 1;
            long cs = 0;
            for (size_t j = 0; j <= child.size(); j++) {
                if (j != child.size() && child[j] != ',') continue;
                String part = child.substr(cs, j - cs).trim();
                cs = j + 1;
                if (!full.empty()) full += ", ";
                if (part.find('&') < 0) { full += one; full += " "; full += part; continue; }
                for (size_t k = 0; k < part.size(); k++) { if (part[k] == '&') full += one; else full.append(part.c_str() + k, 1); }
            }
        }
        Vector<Decl> decls;
        parse_decls(body, body_end, decls);
        add_rules(d, full.c_str(), full.c_str() + full.size(), decls);
        parse_nested(d, full, body, body_end, depth + 1);
    }
}

static void parse_decls(const char* p, const char* end, Vector<Decl>& out) {
    while (p < end) {
        skip_ws_comments(p, end);
        const char* ps = p;
        while (p < end && *p != ':' && *p != ';' && *p != '{') p++;
        if (p >= end) break;
        if (*p == ';') { p++; continue; }
        if (*p == '{') {   // nested rule (CSS nesting) - skip block
            int lvl = 0;
            while (p < end) { if (*p == '{') lvl++; else if (*p == '}' && --lvl == 0) { p++; break; } p++; }
            continue;
        }
        String prop = String(ps, p - ps).trim();
        if (!prop.starts_with("--")) prop = prop.lower();
        p++;
        const char* vs = p;
        int paren = 0, brace = 0;
        while (p < end && (paren || brace || *p != ';')) {
            if (*p == '(') paren++;
            else if (*p == ')') paren--;
            else if (*p == '{') brace++;
            else if (*p == '}') { if (!brace) break; brace--; }
            p++;
        }
        String val = String(vs, p - vs).trim();
        if (p < end && *p == ';') p++;
        bool imp = false;
        long bang = val.find("!important");
        if (bang >= 0) { imp = true; val = val.substr(0, bang).trim(); }
        if (!prop.empty()) out.push(Decl{prop, val, imp});
    }
}

static void add_rules(Document* d, const char* sel_s, const char* sel_e, Vector<Decl>& decls) {
    if (decls.empty()) return;
    const char* p = sel_s;
    while (p < sel_e) {
        const char* e = p;
        int paren = 0;
        while (e < sel_e && (paren || *e != ',')) {
            if (*e == '\\' && e + 1 < sel_e) { e += 2; continue; }
            if (*e == '(') paren++; else if (*e == ')') paren--;
            e++;
        }
        Document::Rule* r = new Document::Rule();
        const char* q = p;
        bool ok = parse_selector(q, e, r->sel, ',');
        if (ok) for (Compound& c : r->sel.parts) if (c.impossible) ok = false;
        if (ok) {
            r->decls = decls;
            r->order = (int)d->rules.size();
            if (g_css_debug) r->text = String(p, e - p).trim();
            d->rules.push(r);
        } else {
            delete r;
        }
        p = e + 1;
    }
}

static void parse_block(Document* d, const char*& p, const char* end, int depth) {
    while (p < end) {
        skip_ws_comments(p, end);
        if (p >= end) return;
        if (*p == '}') { p++; if (depth > 0) return; continue; }
        if (*p == '@') {
            const char* s = p;
            while (p < end && *p != '{' && *p != ';') p++;
            String at = String(s, p - s).trim();
            if (p >= end) return;
            if (*p == ';') { p++; continue; }
            p++;
            String atl = at.lower();
            if (atl.starts_with("@container")) {
                // no container tracking: size queries are evaluated against the viewport
                long open = at.find('(');
                if (at.find("style(") >= 0 || open < 0 || media_matches(String("@media ") + at.substr(open))) { parse_block(d, p, end, depth + 1); continue; }
                int cl = 1;
                while (p < end && cl) { if (*p == '{') cl++; else if (*p == '}') cl--; p++; }
                continue;
            }
            if ((atl.starts_with("@media") && media_matches(at)) || atl.starts_with("@supports") || atl.starts_with("@layer") ||
                atl.starts_with("@scope") || atl.starts_with("@document")) {
                if (atl.starts_with("@supports") && atl.find("not") >= 0 && atl.find("not") < 12) {
                    // @supports not (...) - assume we support it, so skip
                } else {
                    parse_block(d, p, end, depth + 1);
                    continue;
                }
            }
            int lvl = 1;
            while (p < end && lvl) { if (*p == '{') lvl++; else if (*p == '}') lvl--; p++; }
            continue;
        }
        const char* sel_s = p;
        int paren = 0;
        while (p < end && (paren > 0 || (*p != '{' && *p != '}'))) {
            if (*p == '\\' && p + 1 < end) { p += 2; continue; }
            if (*p == '(') paren++; else if (*p == ')') paren--;
            p++;
        }
        if (p >= end) return;
        if (*p == '}') { p++; continue; }
        const char* sel_e = p++;
        const char* body = p;
        int lvl = 1;
        while (p < end && lvl) { if (*p == '{') lvl++; else if (*p == '}') { if (--lvl == 0) break; } p++; }
        Vector<Decl> decls;
        parse_decls(body, p, decls);
        add_rules(d, sel_s, sel_e, decls);
        for (const char* b = body; b < p; b++) if (*b == '{') { parse_nested(d, String(sel_s, sel_e - sel_s).trim(), body, p, 0); break; }
        if (p < end) p++;
    }
}

void parse_css(Document* d, const char* css, size_t len) {
    g_viewport = d->viewport_w;
    const char* p = css;
    if (len > 3 * 1024 * 1024) len = 3 * 1024 * 1024;
    parse_block(d, p, css + len, 0);
}

// ================================================================ values
static Color named_color(const String& n) {
    static const struct { const char* n; uint32_t v; } k[] = {
        {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000}, {"green", 0x008000}, {"blue", 0x0000FF},
        {"yellow", 0xFFFF00}, {"orange", 0xFFA500}, {"purple", 0x800080}, {"gray", 0x808080}, {"grey", 0x808080},
        {"silver", 0xC0C0C0}, {"maroon", 0x800000}, {"navy", 0x000080}, {"teal", 0x008080}, {"olive", 0x808000},
        {"lime", 0x00FF00}, {"aqua", 0x00FFFF}, {"cyan", 0x00FFFF}, {"fuchsia", 0xFF00FF}, {"magenta", 0xFF00FF},
        {"pink", 0xFFC0CB}, {"brown", 0xA52A2A}, {"gold", 0xFFD700}, {"darkgray", 0xA9A9A9}, {"darkgrey", 0xA9A9A9},
        {"lightgray", 0xD3D3D3}, {"lightgrey", 0xD3D3D3}, {"darkblue", 0x00008B}, {"darkred", 0x8B0000},
        {"darkgreen", 0x006400}, {"lightblue", 0xADD8E6}, {"lightgreen", 0x90EE90}, {"whitesmoke", 0xF5F5F5},
        {"gainsboro", 0xDCDCDC}, {"beige", 0xF5F5DC}, {"ivory", 0xFFFFF0}, {"crimson", 0xDC143C}, {"indigo", 0x4B0082},
        {"violet", 0xEE82EE}, {"tomato", 0xFF6347}, {"coral", 0xFF7F50}, {"salmon", 0xFA8072}, {"khaki", 0xF0E68C},
        {"steelblue", 0x4682B4}, {"royalblue", 0x4169E1}, {"skyblue", 0x87CEEB}, {"slategray", 0x708090},
        {"dimgray", 0x696969}, {"aliceblue", 0xF0F8FF}, {"lavender", 0xE6E6FA}, {"linen", 0xFAF0E6},
        {"darkorange", 0xFF8C00}, {"firebrick", 0xB22222}, {"seagreen", 0x2E8B57}, {"chocolate", 0xD2691E},
        {"tan", 0xD2B48C}, {"wheat", 0xF5DEB3}, {"midnightblue", 0x191970}, {"darkslategray", 0x2F4F4F},
        {"ghostwhite", 0xF8F8FF}, {"snow", 0xFFFAFA}, {"honeydew", 0xF0FFF0}, {"mintcream", 0xF5FFFA},
        {"lightyellow", 0xFFFFE0}, {"lightcyan", 0xE0FFFF}, {"dodgerblue", 0x1E90FF}, {"deepskyblue", 0x00BFFF},
        {"orangered", 0xFF4500}, {"hotpink", 0xFF69B4}, {"limegreen", 0x32CD32}, {"forestgreen", 0x228B22},
        {"rebeccapurple", 0x663399}, {"slateblue", 0x6A5ACD}, {"cornflowerblue", 0x6495ED},
    };
    for (auto& e : k) if (n == e.n) return hex(e.v);
    return 0;
}

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static double srgb_encode(double c) {
    c = clamp01(c);
    return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1 / 2.4) - 0.055;
}

static Color from_oklab(double L, double a, double b, double alpha) {
    double l_ = L + 0.3963377774 * a + 0.2158037573 * b;
    double m_ = L - 0.1055613458 * a - 0.0638541728 * b;
    double s_ = L - 0.0894841775 * a - 1.2914855480 * b;
    double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    double r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    double g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    double bl = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    return rgba((int)(srgb_encode(r) * 255 + 0.5), (int)(srgb_encode(g) * 255 + 0.5), (int)(srgb_encode(bl) * 255 + 0.5),
                (int)(clamp01(alpha) * 255 + 0.5));
}

static Color from_hsl(double h, double s, double l, double a) {
    h = fmod(fmod(h, 360) + 360, 360) / 360;
    s = clamp01(s); l = clamp01(l);
    auto hue = [](double p, double q, double t) {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1.0 / 6) return p + (q - p) * 6 * t;
        if (t < 0.5) return q;
        if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
        return p;
    };
    double r, g, b;
    if (s == 0) r = g = b = l;
    else {
        double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        r = hue(p, q, h + 1.0 / 3); g = hue(p, q, h); b = hue(p, q, h - 1.0 / 3);
    }
    return rgba((int)(r * 255 + 0.5), (int)(g * 255 + 0.5), (int)(b * 255 + 0.5), (int)(clamp01(a) * 255 + 0.5));
}

// Parse up to 4 numeric components of a color function. pct[i] set if given as %.
static int color_args(const char* p, double* v, bool* pct) {
    int n = 0;
    while (*p && *p != ')' && n < 4) {
        while (*p == ' ' || *p == ',' || *p == '/') p++;
        if (*p == ')' || !*p) break;
        if (!strncmp(p, "none", 4)) { v[n] = 0; pct[n] = false; n++; p += 4; continue; }
        char* e;
        v[n] = strtod(p, &e);
        if (e == p) break;
        pct[n] = *e == '%';
        if (*e == '%') e++;
        else if (!strncmp(e, "deg", 3)) e += 3;
        else if (!strncmp(e, "turn", 4)) { v[n] *= 360; e += 4; }
        else if (!strncmp(e, "rad", 3)) { v[n] *= 57.2957795; e += 3; }
        p = e;
        n++;
    }
    return n;
}

static int split_tokens_fwd(const String& v, String* out, int max);

bool parse_color(const String& v0, Color* out) {
    String v = v0.trim().lower();
    if (v.empty()) return false;
    if (v == "transparent") { *out = 0; return true; }
    if (v == "currentcolor") return false;
    if (v[0] == '#') {
        const char* h = v.c_str() + 1;
        size_t n = 0;
        while (isxdigit(h[n])) n++;
        uint32_t x = (uint32_t)strtoul(h, nullptr, 16);
        if (n == 3) { *out = rgb(((x >> 8) & 15) * 17, ((x >> 4) & 15) * 17, (x & 15) * 17); return true; }
        if (n == 4) { *out = rgba(((x >> 12) & 15) * 17, ((x >> 8) & 15) * 17, ((x >> 4) & 15) * 17, (x & 15) * 17); return true; }
        if (n == 6) { *out = hex(x); return true; }
        if (n == 8) { *out = rgba(x >> 24, (x >> 16) & 255, (x >> 8) & 255, x & 255); return true; }
        return false;
    }
    if (v.starts_with("color-mix(") && v.ends_with(")")) {
        // color-mix(in <space>, A [p%], B [q%]) - mixed in sRGB whatever the space
        String arg[3];
        int n = 0, depth = 0;
        size_t st = 10;
        for (size_t i = 10; i < v.size() - 1 && n < 3; i++) {
            if (v[i] == '(') depth++;
            else if (v[i] == ')') depth--;
            else if (v[i] == ',' && !depth) { arg[n++] = v.substr(st, i - st).trim(); st = i + 1; }
        }
        if (n != 2) return false;
        arg[2] = v.substr(st, v.size() - 1 - st).trim();
        Color col[2];
        double weight[2] = {-1, -1};
        for (int i = 0; i < 2; i++) {
            String t[2];
            int k = split_tokens_fwd(arg[i + 1], t, 2);
            int ci = 0;
            if (k == 2 && t[1].find('%') >= 0) { weight[i] = strtod(t[1].c_str(), nullptr); }
            else if (k == 2 && t[0].find('%') >= 0) { weight[i] = strtod(t[0].c_str(), nullptr); ci = 1; }
            if (!parse_color(t[ci], &col[i])) return false;
        }
        if (weight[0] < 0 && weight[1] < 0) weight[0] = weight[1] = 50;
        else if (weight[0] < 0) weight[0] = 100 - weight[1];
        else if (weight[1] < 0) weight[1] = 100 - weight[0];
        double sum = weight[0] + weight[1];
        if (sum <= 0) return false;
        double w0 = weight[0] / sum, w1 = weight[1] / sum;
        double a0 = ((col[0] >> 24) & 255) * w0, a1 = ((col[1] >> 24) & 255) * w1, alpha = a0 + a1;
        if (alpha <= 0) { *out = 0; return true; }
        int ch[3];
        for (int i = 0; i < 3; i++) ch[i] = (int)((((col[0] >> (16 - i * 8)) & 255) * a0 + ((col[1] >> (16 - i * 8)) & 255) * a1) / alpha + 0.5);
        *out = rgba(ch[0], ch[1], ch[2], (int)(alpha * (sum < 100 ? sum / 100 : 1) + 0.5));
        return true;
    }
    const char* paren = strchr(v.c_str(), '(');
    if (paren) {
        double a[4] = {0, 0, 0, 1};
        bool pct[4] = {false, false, false, false};
        int n = color_args(paren + 1, a, pct);
        if (n < 3) return false;
        double alpha = n >= 4 ? (pct[3] ? a[3] / 100 : a[3]) : 1;
        if (v.starts_with("rgb")) {
            for (int i = 0; i < 3; i++) if (pct[i]) a[i] *= 2.55;
            *out = rgba((int)kclamp(a[0], 0.0, 255.0), (int)kclamp(a[1], 0.0, 255.0), (int)kclamp(a[2], 0.0, 255.0), (int)(clamp01(alpha) * 255));
            return true;
        }
        if (v.starts_with("hsl")) { *out = from_hsl(a[0], a[1] / 100, a[2] / 100, alpha); return true; }
        if (v.starts_with("oklch")) {
            double L = pct[0] ? a[0] / 100 : a[0], C = pct[1] ? a[1] * 0.004 : a[1], H = a[2] * 3.14159265 / 180;
            *out = from_oklab(L, C * cos(H), C * sin(H), alpha);
            return true;
        }
        if (v.starts_with("oklab")) {
            double L = pct[0] ? a[0] / 100 : a[0];
            *out = from_oklab(L, pct[1] ? a[1] * 0.004 : a[1], pct[2] ? a[2] * 0.004 : a[2], alpha);
            return true;
        }
        if (v.starts_with("color(srgb")) { *out = rgba((int)(a[0] * 255), (int)(a[1] * 255), (int)(a[2] * 255), (int)(alpha * 255)); return true; }
        return false;
    }
    Color c = named_color(v);
    if (c) { *out = c; return true; }
    return false;
}

// calc()/min()/max()/clamp() evaluator. Lengths resolve to px; percentages
// against the viewport width (approximation). Returns false on parse error.
struct CalcParser {
    const char* p;
    int fs;
    bool ok = true;
    bool has_pct = false;
    double pct = 0;
    void ws() { while (*p == ' ' || *p == '\t' || *p == '\n') p++; }
    double fn_args(int kind) {   // 0 min, 1 max, 2 clamp
        double vals[3];
        int n = 0;
        while (*p && *p != ')' && n < 3) {
            vals[n++] = sum();
            ws();
            if (*p == ',') p++;
        }
        if (*p == ')') p++;
        if (!n) { ok = false; return 0; }
        if (kind == 2 && n == 3) return vals[1] < vals[0] ? vals[0] : vals[1] > vals[2] ? vals[2] : vals[1];
        double r = vals[0];
        for (int i = 1; i < n; i++) r = kind == 0 ? (vals[i] < r ? vals[i] : r) : (vals[i] > r ? vals[i] : r);
        return r;
    }
    double atom() {
        ws();
        if (*p == '(') { p++; double v = sum(); ws(); if (*p == ')') p++; return v; }
        if (!strncmp(p, "calc(", 5)) { p += 5; double v = sum(); ws(); if (*p == ')') p++; return v; }
        if (!strncmp(p, "min(", 4)) { p += 4; return fn_args(0); }
        if (!strncmp(p, "max(", 4)) { p += 4; return fn_args(1); }
        if (!strncmp(p, "clamp(", 6)) { p += 6; return fn_args(2); }
        if (*p == '-' && (p[1] == '(' || isalpha(p[1]))) { p++; return -atom(); }
        if (!isdigit(*p) && *p != '.' && *p != '-' && *p != '+') { ok = false; return 0; }
        char* e;
        double v = strtod(p, &e);
        p = e;
        if (!strncmp(p, "px", 2)) p += 2;
        else if (!strncmp(p, "rem", 3)) { v *= 16; p += 3; }
        else if (!strncmp(p, "em", 2)) { v *= fs; p += 2; }
        else if (!strncmp(p, "vw", 2) || !strncmp(p, "dvw", 3) || !strncmp(p, "svw", 3)) { v = v * g_viewport / 100; while (isalpha(*p)) p++; }
        else if (!strncmp(p, "vh", 2) || !strncmp(p, "dvh", 3) || !strncmp(p, "svh", 3) || !strncmp(p, "lvh", 3)) { v = v * g_viewport_h / 100; while (isalpha(*p)) p++; }
        else if (!strncmp(p, "pt", 2)) { v = v * 4 / 3; p += 2; }
        else if (!strncmp(p, "ch", 2) || !strncmp(p, "ex", 2)) { v = v * fs / 2; p += 2; }
        else if (*p == '%') { has_pct = true; v = v * g_viewport / 100; p++; }
        return v;
    }
    double product() {
        double v = atom();
        for (;;) {
            ws();
            if (*p == '*') { p++; v *= atom(); }
            else if (*p == '/') { p++; double d = atom(); if (d != 0) v /= d; }
            else return v;
        }
    }
    double sum() {
        double v = product();
        for (;;) {
            ws();
            if ((*p == '+' || *p == '-') && (p[1] == ' ' || p[1] == '(')) { char op = *p++; double r = product(); v = op == '+' ? v + r : v - r; }
            else return v;
        }
    }
};

// Length in px; pct_out gets percentage if given in %. Returns false if not a length.
static bool parse_len(const char* s, int font_size, int* px, int* pct_out = nullptr) {
    while (*s == ' ') s++;
    if (!strncmp(s, "auto", 4)) { *px = kUnset; return true; }
    if (!strncmp(s, "calc(", 5) || !strncmp(s, "min(", 4) || !strncmp(s, "max(", 4) || !strncmp(s, "clamp(", 6)) {
        CalcParser cp{s, font_size};
        double v = cp.atom();
        if (!cp.ok) return false;
        *px = (int)(v >= 0 ? v + 0.5 : v - 0.5);
        return true;
    }
    if (!isdigit(*s) && *s != '.' && *s != '-' && *s != '+') return false;
    char* e;
    double v = strtod(s, &e);
    if (!strncmp(e, "px", 2) || *e == 0 || *e == ' ' || *e == ')' || *e == ',') *px = (int)(v >= 0 ? v + 0.5 : v - 0.5);
    else if (!strncmp(e, "rem", 3)) *px = (int)(v * 16 + 0.5);
    else if (!strncmp(e, "em", 2)) *px = (int)(v * font_size + 0.5);
    else if (!strncmp(e, "pt", 2)) *px = (int)(v * 4 / 3);
    else if (!strncmp(e, "vw", 2) || !strncmp(e, "dvw", 3) || !strncmp(e, "svw", 3)) *px = (int)(v * g_viewport / 100);
    else if (!strncmp(e, "vh", 2) || !strncmp(e, "dvh", 3) || !strncmp(e, "svh", 3) || !strncmp(e, "lvh", 3)) *px = (int)(v * g_viewport_h / 100);
    else if (!strncmp(e, "vmin", 4)) *px = (int)(v * kmin(g_viewport, g_viewport_h) / 100);
    else if (!strncmp(e, "vmax", 4)) *px = (int)(v * kmax(g_viewport, g_viewport_h) / 100);
    else if (!strncmp(e, "cqw", 3) || !strncmp(e, "cqi", 3)) *px = (int)(v * g_viewport / 100);
    else if (!strncmp(e, "cqh", 3) || !strncmp(e, "cqb", 3)) *px = (int)(v * g_viewport_h / 100);
    else if (!strncmp(e, "lh", 2)) *px = (int)(v * font_size * 1.2 + 0.5);
    else if (!strncmp(e, "rlh", 3)) *px = (int)(v * 16 * 1.2 + 0.5);
    else if (!strncmp(e, "ch", 2)) *px = (int)(v * font_size / 2);
    else if (!strncmp(e, "ex", 2)) *px = (int)(v * font_size / 2);
    else if (*e == '%') { if (pct_out) { *pct_out = (int)v; *px = -1; return true; } *px = (int)(v * font_size / 100); }
    else return false;
    return true;
}

static int split_tokens(const String& v, String* out, int max) {
    int n = 0;
    const char* p = v.c_str();
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        const char* st = p;
        int paren = 0;
        while (*p && (paren || (*p != ' ' && *p != '\t' && *p != '\n'))) { if (*p == '(') paren++; else if (*p == ')') paren--; p++; }
        out[n++] = String(st, p - st);
    }
    return n;
}

static int split_tokens_fwd(const String& v, String* out, int max) { return split_tokens(v, out, max); }

static void parse_box4(const String& v, int fs, int out[4]) {
    String t[4];
    int n = split_tokens(v, t, 4);
    int vals[4];
    for (int i = 0; i < n; i++) { int px = 0; if (!parse_len(t[i].c_str(), fs, &px)) px = 0; vals[i] = px; }
    if (n == 1) { out[0] = out[1] = out[2] = out[3] = vals[0]; }
    else if (n == 2) { out[0] = out[2] = vals[0]; out[1] = out[3] = vals[1]; }
    else if (n == 3) { out[0] = vals[0]; out[1] = out[3] = vals[1]; out[2] = vals[2]; }
    else if (n == 4) { for (int i = 0; i < 4; i++) out[i] = vals[i]; }
}

// Substitute var(--x, fallback) recursively.
static String resolve_vars(const String& v, const VarMap* vars, int depth = 0) {
    if (depth > 8 || v.find("var(") < 0) return v;
    String out;
    const char* p = v.c_str();
    while (*p) {
        const char* f = strstr(p, "var(");
        if (!f) { out += p; break; }
        out.append(p, f - p);
        const char* q = f + 4;
        int paren = 1;
        const char* arg = q;
        while (*q && paren) { if (*q == '(') paren++; else if (*q == ')') { if (--paren == 0) break; } q++; }
        String inner(arg, q - arg);
        long comma = inner.find(',');
        String name = (comma >= 0 ? inner.substr(0, comma) : inner).trim();
        const String* val = vars ? vars->get(name.c_str()) : nullptr;
        if (val && !val->trim().empty()) out += resolve_vars(*val, vars, depth + 1);
        else if (comma >= 0) out += resolve_vars(inner.substr(comma + 1).trim(), vars, depth + 1);
        p = *q ? q + 1 : q;
    }
    return out;
}

static void parse_gradient(Style& s, const String& v) {
    const char* g = strstr(v.c_str(), "linear-gradient(");
    if (!g) {
        g = strstr(v.c_str(), "radial-gradient(");
        if (!g) return;
    }
    const char* p = strchr(g, '(') + 1;
    // split top-level args
    Vector<String> args;
    int paren = 0;
    const char* st = p;
    for (; *p; p++) {
        if (*p == '(') paren++;
        else if (*p == ')') { if (paren == 0) break; paren--; }
        else if (*p == ',' && paren == 0) { args.push(String(st, p - st).trim()); st = p + 1; }
    }
    args.push(String(st, p - st).trim());
    Vector<Color> stops;
    s.grad_horizontal = false;
    for (String& a : args) {
        String al = a.lower();
        if (al.starts_with("to ")) { s.grad_horizontal = al.find("right") >= 0 || al.find("left") >= 0; continue; }
        if (al.find("deg") >= 0 && !al.starts_with("rgb") && !al.starts_with("hsl") && !al.starts_with("oklch")) {
            int deg = atoi(al.c_str());
            s.grad_horizontal = (deg % 180) > 45 && (deg % 180) < 135;
            continue;
        }
        String tok[2];
        split_tokens(a, tok, 2);
        Color c;
        if (parse_color(tok[0], &c)) stops.push(c);
    }
    if (stops.size() >= 2) { s.grad_a = stops[0]; s.grad_b = stops.back(); }
    else if (stops.size() == 1) s.background = stops[0];
}

static void parse_grid_cols(Style& s, const String& v) {
    s.grid_ncols = 0;
    String val = v.trim();
    // expand repeat(N, X)
    String expanded;
    const char* p = val.c_str();
    while (*p) {
        const char* r = strstr(p, "repeat(");
        if (!r) { expanded += p; break; }
        expanded.append(p, r - p);
        const char* q = r + 7;
        int n = atoi(q);
        bool auto_fill = !strncmp(q, "auto-fill", 9) || !strncmp(q, "auto-fit", 8);
        const char* comma = strchr(q, ',');
        if (!comma) break;
        int paren = 1;
        const char* e = comma + 1;
        while (*e && paren) { if (*e == '(') paren++; else if (*e == ')') paren--; if (paren) e++; }
        String inner = String(comma + 1, e - comma - 1).trim();
        if (auto_fill) {
            int px = 200;
            const char* mm = strstr(inner.c_str(), "minmax(");
            parse_len(mm ? mm + 7 : inner.c_str(), s.font_size, &px);
            if (px <= 0) px = 200;
            n = kclamp(g_viewport / kmax(px, 1), 1, 8);
            inner = "1fr";
        }
        for (int i = 0; i < kclamp(n, 1, 16); i++) { expanded += inner; expanded += " "; }
        p = *e ? e + 1 : e;
    }
    String toks[16];
    int n = split_tokens(expanded, toks, 16);
    for (int i = 0; i < n; i++) {
        String t = toks[i].lower();
        if (t.starts_with("[")) continue;   // line names
        int idx = s.grid_ncols;
        if (idx >= 16) break;
        s.grid_col_px[idx] = 0;
        s.grid_col_fr[idx] = 0;
        if (t.starts_with("minmax(")) {
            const char* c = strchr(t.c_str(), ',');
            t = c ? String(c + 1).trim() : String("1fr");
            if (t.ends_with(")")) t = t.substr(0, t.size() - 1);
        }
        if (t.ends_with("fr")) s.grid_col_fr[idx] = (float)strtod(t.c_str(), nullptr);
        else if (t == "auto" || t == "min-content" || t == "max-content" || t.starts_with("fit-content")) s.grid_col_fr[idx] = 1;
        else { int px; if (parse_len(t.c_str(), s.font_size, &px) && px > 0) s.grid_col_px[idx] = px; else s.grid_col_fr[idx] = 1; }
        s.grid_ncols++;
    }
}

// Background-image url() collection (resolved by layout through Document)

static int image_index(Document* d, const String& url) {
    for (size_t i = 0; i < d->images.size(); i++) if (d->images[i].url == url) return (int)i;
    Image& im = d->images.emplace();
    im.url = url;
    return (int)d->images.size() - 1;
}

void resolve_url(Document* d, const char* ref, char* out, size_t size);

static void apply_decl(Style& s, const Style& parent, const String& prop, const String& raw) {
    String v = raw.find("var(") >= 0 ? resolve_vars(raw, s.vars) : raw;
    const char* val = v.c_str();
    String lv = v.lower().trim();
    if (prop[0] == 'b') {
        if (prop.starts_with("background")) s.bg_set = true;
        else if (prop.starts_with("border") && prop.find("radius") < 0) s.border_set = true;
    }
    if (prop[0] == 'i' || prop[0] == 'b' || prop[0] == 'm') {
        static const struct { const char* from; const char* to; } kLogical[] = {
            {"inline-size", "width"}, {"block-size", "height"}, {"min-inline-size", "min-width"}, {"max-inline-size", "max-width"},
            {"min-block-size", "min-height"}, {"max-block-size", "max-height"}, {"inset-block-start", "top"}, {"inset-block-end", "bottom"},
            {"inset-inline-start", "left"}, {"inset-inline-end", "right"}, {"border-block-start", "border-top"}, {"border-block-end", "border-bottom"},
            {"border-inline-start", "border-left"}, {"border-inline-end", "border-right"},
        };
        for (auto& l : kLogical) if (prop == l.from) { apply_decl(s, parent, String(l.to), raw); return; }
        if (prop == "inset" || prop == "inset-block" || prop == "inset-inline") {
            int b[4] = {0, 0, 0, 0};
            if (prop == "inset") { parse_box4(v, s.font_size, b); for (int i = 0; i < 4; i++) s.offset[i] = b[i]; return; }
            String t[2];
            int n = split_tokens(v, t, 2);
            int a = 0, e = 0;
            if (n < 1 || !parse_len(t[0].c_str(), s.font_size, &a)) return;
            e = a;
            if (n == 2) parse_len(t[1].c_str(), s.font_size, &e);
            if (prop == "inset-block") { s.offset[0] = a; s.offset[2] = e; } else { s.offset[3] = a; s.offset[1] = e; }
            return;
        }
    }
    if (lv == "inherit") {
        if (prop == "color") s.color = parent.color;
        else if (prop == "font-size") s.font_size = parent.font_size;
        else if (prop == "font-weight") { s.weight = parent.weight; s.bold = parent.bold; }
        else if (prop == "background-color" || prop == "background") s.background = parent.background;
        else if (prop == "text-decoration" || prop == "text-decoration-line") { s.underline = parent.underline; s.strike = parent.strike; }
        else if (prop == "font-family") s.mono = parent.mono;
        else if (prop == "line-height") s.line_height_pct = parent.line_height_pct;
        else if (prop == "text-align") s.text_align = parent.text_align;
        else if (prop == "font-style") s.italic = parent.italic;
        else if (prop == "border-radius") s.border_radius = parent.border_radius;
        return;
    }
    if (lv == "initial" || lv == "unset" || lv == "revert" || lv == "revert-layer") {
        if (prop == "display") s.display = Style::Inline;
        return;
    }
    const char* p0 = prop.c_str();
    switch (p0[0]) {
    case 'a':
        if (prop == "align-items") {
            s.align_items = lv.find("center") >= 0 ? 2 : (lv.ends_with("start") || lv == "flex-start" || lv == "self-start") ? 1 : lv.ends_with("end") ? 3 : lv == "baseline" ? 4 : 0;
        } else if (prop == "align-self") {
            s.align_self = lv == "auto" ? 255 : lv.find("center") >= 0 ? 2 : lv.ends_with("start") ? 1 : lv.ends_with("end") ? 3 : 0;
        } else if (prop == "aspect-ratio") {
            double a = strtod(val, nullptr), b = 1;
            const char* sl = strchr(val, '/');
            if (sl) b = strtod(sl + 1, nullptr);
            if (a > 0 && b > 0) s.aspect_ratio = (float)(a / b);
        }
        return;
    case 'b':
        if (prop == "background-color") { Color c; if (parse_color(v, &c)) s.background = c; }
        else if (prop == "background" || prop == "background-image") {
            if (prop == "background") {
                if (lv == "none" || lv == "transparent") { s.background = 0; s.grad_a = s.grad_b = 0; return; }
                String toks[8];
                int n = split_tokens(v, toks, 8);
                for (int i = 0; i < n; i++) { Color c; if (parse_color(toks[i], &c)) { s.background = c; break; } }
            }
            if (lv.find("gradient(") >= 0) parse_gradient(s, v);
            const char* u = strstr(val, "url(");
            if (u && g_doc) {
                u += 4;
                while (*u == ' ' || *u == '"' || *u == '\'') u++;
                const char* e = u;
                while (*e && *e != ')' && *e != '"' && *e != '\'') e++;
                String ref(u, e - u);
                if (ref.starts_with("data:")) s.bg_image = image_index(g_doc, ref);
                else {
                    char abs[2300];
                    resolve_url(g_doc, ref.c_str(), abs, sizeof(abs));
                    s.bg_image = image_index(g_doc, String(abs));
                }
            }
        }
        else if (prop == "border" || prop == "border-top" || prop == "border-bottom" || prop == "border-left" || prop == "border-right") {
            int w = 0;
            Color c = color_a(s.border_color) ? s.border_color : with_alpha(s.color, 255);
            bool none = lv.find("none") >= 0 || lv == "0";
            String toks[4];
            int n = split_tokens(v, toks, 4);
            bool style_given = false, width_given = false;
            for (int i = 0; i < n; i++) {
                String& tok = toks[i];
                int px;
                Color cc;
                if (tok == "thin") { w = 1; width_given = true; }
                else if (tok == "medium") { w = 3; width_given = true; }
                else if (tok == "thick") { w = 5; width_given = true; }
                else if (parse_len(tok.c_str(), s.font_size, &px) && px != kUnset) { w = px; width_given = true; }
                else if (tok == "solid" || tok == "dashed" || tok == "dotted" || tok == "double" || tok == "groove" || tok == "inset" || tok == "outset" || tok == "ridge") style_given = true;
                else if (parse_color(tok, &cc)) c = cc;
            }
            if (style_given && !width_given) w = 3;
            if (none) w = 0;
            w = kclamp(w, 0, 10);
            int idx = prop == "border-top" ? 0 : prop == "border-right" ? 1 : prop == "border-bottom" ? 2 : prop == "border-left" ? 3 : -1;
            // one colour per box: a clear edge next to a visible border is dropped so it doesn't take that colour
            if (!color_a(c) && idx >= 0 && color_a(s.border_color)) w = 0;
            if (idx < 0) for (int i = 0; i < 4; i++) s.border_width[i] = w;
            else s.border_width[idx] = w;
            if (w) s.border_color = c ? c : kClearBorder;
        }
        else if (prop == "border-width") { int b[4] = {0, 0, 0, 0}; parse_box4(v, s.font_size, b); for (int i = 0; i < 4; i++) s.border_width[i] = kclamp(b[i], 0, 10); }
        else if (prop == "border-top-width" || prop == "border-bottom-width" || prop == "border-left-width" || prop == "border-right-width") {
            int px; if (parse_len(val, s.font_size, &px) && px >= 0) {
                int idx = prop[7] == 't' ? 0 : prop[7] == 'r' ? 1 : prop[7] == 'b' ? 2 : 3;
                s.border_width[idx] = kclamp(px, 0, 10);
            }
        }
        else if (prop == "border-color" || prop == "border-top-color" || prop == "border-bottom-color" || prop == "border-left-color" || prop == "border-right-color") { Color c; if (parse_color(String(v.c_str()).trim().substr(0, v.find(' ') > 0 ? v.find(' ') : v.size()), &c)) s.border_color = c ? c : kClearBorder; }
        else if (prop == "border-style") { if (lv == "none" || lv == "hidden") for (int i = 0; i < 4; i++) s.border_width[i] = 0; else for (int i = 0; i < 4; i++) if (!s.border_width[i] && s.border_color) s.border_width[i] = 3; }
        else if (prop == "border-radius") { int px, pct = -1; if (parse_len(val, s.font_size, &px, &pct)) s.border_radius = pct >= 0 ? (pct >= 50 ? 9999 : pct) : kclamp(px, 0, 9999); }
        else if (prop == "border-top-left-radius" && !s.border_radius) { int px; if (parse_len(val, s.font_size, &px) && px > 0) s.border_radius = px; }
        else if (prop == "box-shadow") {
            if (lv == "none") { s.shadow = false; return; }
            String toks[8];
            int n = split_tokens(v.find(',') > 0 && v.find('(') > v.find(',') ? v.substr(0, v.find(',')) : v, toks, 8);
            int nums[4], nn = 0;
            Color c = rgba(0, 0, 0, 60);
            bool inset = false;
            for (int i = 0; i < n; i++) {
                int px;
                Color cc;
                if (toks[i] == "inset") inset = true;
                else if (nn < 4 && parse_len(toks[i].c_str(), s.font_size, &px) && px != kUnset) nums[nn++] = px;
                else if (parse_color(toks[i], &cc)) c = cc;
            }
            if (!inset && nn >= 2 && color_a(c) > 0) {
                s.shadow = true;
                s.shadow_x = nums[0];
                s.shadow_y = nums[1];
                s.shadow_blur = nn >= 3 ? nums[2] : 0;
                s.shadow_color = c;
            }
        }
        else if (prop == "bottom") { int px; if (parse_len(val, s.font_size, &px)) s.offset[2] = px; }
        return;
    case 'c':
        if (prop == "color") { Color c; if (parse_color(v, &c)) s.color = c; }
        else if (prop == "column-gap") { int px; if (parse_len(val, s.font_size, &px) && px > 0) s.gap_col = px; }
        return;
    case 'd':
        if (prop == "display") {
            if (lv == "none") s.display = Style::None;
            else if (lv == "block" || lv == "flow-root" || lv == "table-caption" || lv == "block flow") s.display = Style::Block;
            else if (lv == "inline" || lv == "inline flow") s.display = Style::Inline;
            else if (lv == "inline-block" || lv == "inline-table") s.display = Style::InlineBlock;
            else if (lv == "inline-flex") { s.display = Style::InlineBlock; s.flex_row = true; s.display = Style::Flex; s.width = s.width; }
            else if (lv == "inline-grid") s.display = Style::Grid;
            else if (lv == "list-item") s.display = Style::ListItem;
            else if (lv == "table") s.display = Style::Table;
            else if (lv == "table-row") s.display = Style::TableRow;
            else if (lv == "table-cell") s.display = Style::TableCell;
            else if (lv == "table-row-group" || lv == "table-header-group" || lv == "table-footer-group") s.display = Style::Block;
            else if (lv == "flex" || lv == "block flex") s.display = Style::Flex;
            else if (lv == "grid") s.display = Style::Grid;
            else if (lv == "contents") s.display = Style::Contents;
        }
        return;
    case 'f':
        if (prop == "font-size") {
            int px, pct = -1;
            if (lv == "small") s.font_size = 13;
            else if (lv == "x-small") s.font_size = 10;
            else if (lv == "xx-small") s.font_size = 9;
            else if (lv == "medium") s.font_size = 16;
            else if (lv == "large") s.font_size = 18;
            else if (lv == "x-large") s.font_size = 24;
            else if (lv == "xx-large") s.font_size = 32;
            else if (lv == "smaller") s.font_size = parent.font_size * 5 / 6;
            else if (lv == "larger") s.font_size = parent.font_size * 6 / 5;
            else if (parse_len(val, parent.font_size, &px, &pct)) s.font_size = pct >= 0 ? parent.font_size * pct / 100 : px;
            s.font_size = kclamp(s.font_size, 6, 120);
        }
        else if (prop == "font-weight") {
            int w = lv == "bold" ? 700 : lv == "bolder" ? kmin(900, parent.weight + 300) : lv == "lighter" ? 300 : lv == "normal" ? 400 : atoi(val);
            if (w > 0) { s.weight = w; s.bold = w >= 600; }
        }
        else if (prop == "font-style") s.italic = lv == "italic" || lv.starts_with("oblique");
        else if (prop == "font-family") s.mono = lv.find("mono") >= 0 || lv.find("courier") >= 0 || lv.find("consolas") >= 0 || lv.find("menlo") >= 0;
        else if (prop == "font") {
            if (lv.find("bold") >= 0) { s.bold = true; s.weight = 700; }
            String toks[8];
            int n = split_tokens(v, toks, 8);
            for (int i = 0; i < n; i++) {
                String t = toks[i];
                long sl = t.find('/');
                if (sl > 0) t = t.substr(0, sl);
                int px;
                if (isdigit(t[0]) && !(t.size() == 3 && isdigit(t[1]) && isdigit(t[2])) && parse_len(t.c_str(), parent.font_size, &px)) { s.font_size = kclamp(px, 6, 120); break; }
            }
            if (lv.find("monospace") >= 0) s.mono = true;
        }
        else if (prop == "flex-direction") { s.flex_row = !lv.starts_with("column"); s.flex_reverse = lv.ends_with("reverse"); }
        else if (prop == "flex-wrap") s.flex_wrap = lv == "wrap" || lv == "wrap-reverse";
        else if (prop == "flex-flow") { s.flex_row = lv.find("column") < 0; s.flex_wrap = lv.find("wrap") >= 0 && lv.find("nowrap") < 0; }
        else if (prop == "flex-grow") s.flex_grow = (float)strtod(val, nullptr);
        else if (prop == "flex-shrink") s.flex_shrink = (float)strtod(val, nullptr);
        else if (prop == "flex-basis") { int px, pct = -1; if (parse_len(val, s.font_size, &px, &pct)) { if (pct >= 0) s.flex_basis_pct = pct; else if (px != kUnset) s.flex_basis = px; } }
        else if (prop == "flex") {
            if (lv == "none") { s.flex_grow = 0; s.flex_shrink = 0; }
            else if (lv == "auto") { s.flex_grow = 1; s.flex_shrink = 1; }
            else {
                String toks[3];
                int n = split_tokens(v, toks, 3);
                if (n == 1 && (toks[0].find('%') >= 0 || strpbrk(toks[0].c_str(), "pxem"))) {   // flex: <basis>
                    int px, pct = -1;
                    s.flex_grow = 1;
                    if (parse_len(toks[0].c_str(), s.font_size, &px, &pct)) { if (pct >= 0) s.flex_basis_pct = pct; else if (px != kUnset) s.flex_basis = px; }
                    return;
                }
                if (n >= 1) s.flex_grow = (float)strtod(toks[0].c_str(), nullptr);
                if (n >= 2) {
                    int px;
                    if (toks[1].find('%') < 0 && !strpbrk(toks[1].c_str(), "pxem")) s.flex_shrink = (float)strtod(toks[1].c_str(), nullptr);
                    else if (parse_len(toks[1].c_str(), s.font_size, &px)) s.flex_basis = px;
                }
                if (n >= 3) { int px, pct = -1; if (parse_len(toks[2].c_str(), s.font_size, &px, &pct)) { if (pct >= 0) s.flex_basis_pct = pct; else if (px != kUnset) s.flex_basis = px; } }
                else if (n == 1) s.flex_basis = 0;
            }
        }
        return;
    case 'g':
        if (prop == "gap" || prop == "grid-gap") {
            String toks[2];
            int n = split_tokens(v, toks, 2);
            int a = 0, b = 0;
            parse_len(toks[0].c_str(), s.font_size, &a);
            if (n > 1) parse_len(toks[1].c_str(), s.font_size, &b); else b = a;
            s.gap_row = kmax(0, a);
            s.gap_col = kmax(0, b);
        }
        else if (prop == "grid-template-columns") parse_grid_cols(s, v);
        else if (prop == "grid-column" || prop == "grid-column-end") {
            if (lv.find("span") >= 0) s.grid_span = kclamp(atoi(lv.c_str() + lv.find("span") + 4), 1, 16);
            else if (lv.find("-1") >= 0) s.grid_span = -1;
        }
        else if (prop == "grid-row-gap" || prop == "row-gap") { int px; if (parse_len(val, s.font_size, &px)) s.gap_row = kmax(0, px); }
        return;
    case 'h':
        if (prop == "height") {
            if (lv.starts_with("calc(") && lv.find('%') >= 0) return;   // relative to unknown containing block height
            int px, pct = -1;
            if (parse_len(val, s.font_size, &px, &pct)) { if (pct >= 0) s.height_pct = pct; else if (px > 0) s.height = px; }
        }
        return;
    case 'j':
        if (prop == "justify-content") {
            s.justify = lv.find("between") >= 0 ? 3 : lv.find("around") >= 0 ? 4 : lv.find("evenly") >= 0 ? 5 :
                        lv.find("center") >= 0 ? 1 : (lv.ends_with("end") || lv == "right") ? 2 : 0;
        }
        return;
    case 'l':
        if (prop == "line-height") {
            int px, pct = -1;
            if (lv == "normal") s.line_height_pct = 0;
            else if (isdigit(val[0]) && !strpbrk(val, "pxe%r")) s.line_height_pct = (int)(strtod(val, nullptr) * 100);
            else if (parse_len(val, s.font_size, &px, &pct)) s.line_height_pct = pct >= 0 ? pct : px * 100 / kmax(1, s.font_size);
        }
        else if (prop == "list-style" || prop == "list-style-type") {
            if (lv.find("none") >= 0) s.list_none = true;
            else if (lv.find("decimal") >= 0) { s.list_ordered = true; s.list_none = false; }
            else if (lv.find("disc") >= 0 || lv.find("circle") >= 0 || lv.find("square") >= 0) { s.list_ordered = false; s.list_none = false; }
        }
        else if (prop == "left") { int px; if (parse_len(val, s.font_size, &px)) s.offset[3] = px; }
        return;
    case 'm':
        if (prop == "margin") parse_box4(v, s.font_size, s.margin);
        else if (prop == "margin-top" || prop == "margin-block-start") parse_len(val, s.font_size, &s.margin[0]);
        else if (prop == "margin-right" || prop == "margin-inline-end") parse_len(val, s.font_size, &s.margin[1]);
        else if (prop == "margin-bottom" || prop == "margin-block-end") parse_len(val, s.font_size, &s.margin[2]);
        else if (prop == "margin-left" || prop == "margin-inline-start") parse_len(val, s.font_size, &s.margin[3]);
        else if (prop == "margin-inline" || prop == "margin-block") {
            String toks[2]; int n = split_tokens(v, toks, 2); int a = 0, b;
            parse_len(toks[0].c_str(), s.font_size, &a); b = a;
            if (n > 1) parse_len(toks[1].c_str(), s.font_size, &b);
            if (prop == "margin-inline") { s.margin[3] = a; s.margin[1] = b; } else { s.margin[0] = a; s.margin[2] = b; }
        }
        else if (prop == "max-width") { int px, pct = -1; if (parse_len(val, s.font_size, &px, &pct)) { if (pct >= 0) s.max_width_pct = pct; else if (px > 0) s.max_width = px; } if (lv == "none") { s.max_width = -1; s.max_width_pct = -1; } }
        else if (prop == "min-width") { int px; if (parse_len(val, s.font_size, &px) && px > 0) s.min_width = px; }
        else if (prop == "min-height") { int px; if (parse_len(val, s.font_size, &px) && px > 0) s.min_height = px; }
        else if (prop == "max-height") {
            s.max_height = -1;
            s.max_height_pct = -1;
            if (lv.find('%') >= 0) {   // percentages resolve against the containing block (layout)
                if (isdigit(val[0])) s.max_height_pct = atoi(val);
            } else {
                int px;
                if (parse_len(val, s.font_size, &px) && px > 0) s.max_height = px;
            }
        }
        return;
    case 'o':
        if (prop == "opacity") { double o = strtod(val, nullptr); if (strchr(val, '%')) o /= 100; s.opacity = parent.opacity * (float)clamp01(o); }
        else if (prop == "order") s.order = atoi(val);
        else if (prop == "overflow" || prop == "overflow-y") {
            s.overflow_hidden = lv == "hidden" || lv == "clip";
            s.overflow_scroll = lv == "auto" || lv == "scroll" || lv == "overlay";
        }
        return;
    case 'p':
        if (prop == "padding") parse_box4(v, s.font_size, s.padding);
        else if (prop == "padding-top" || prop == "padding-block-start") parse_len(val, s.font_size, &s.padding[0]);
        else if (prop == "padding-right" || prop == "padding-inline-end") parse_len(val, s.font_size, &s.padding[1]);
        else if (prop == "padding-bottom" || prop == "padding-block-end") parse_len(val, s.font_size, &s.padding[2]);
        else if (prop == "padding-left" || prop == "padding-inline-start") parse_len(val, s.font_size, &s.padding[3]);
        else if (prop == "padding-inline" || prop == "padding-block") {
            String toks[2]; int n = split_tokens(v, toks, 2); int a = 0, b;
            parse_len(toks[0].c_str(), s.font_size, &a); b = a;
            if (n > 1) parse_len(toks[1].c_str(), s.font_size, &b);
            if (prop == "padding-inline") { s.padding[3] = a; s.padding[1] = b; } else { s.padding[0] = a; s.padding[2] = b; }
        }
        else if (prop == "position") {
            s.position = lv == "relative" ? Style::Relative : lv == "absolute" ? Style::Absolute : lv == "fixed" ? Style::Fixed : lv == "sticky" ? Style::Sticky : Style::Static;
        }
        else if (prop == "place-items") s.align_items = lv.find("center") >= 0 ? 2 : 0;
        else if (prop == "place-content" || prop == "place-self") { if (lv.find("center") >= 0) { s.justify = 1; s.align_items = 2; } }
        return;
    case 'r':
        if (prop == "right") { int px; if (parse_len(val, s.font_size, &px)) s.offset[1] = px; }
        else if (prop == "row-gap") { int px; if (parse_len(val, s.font_size, &px)) s.gap_row = kmax(0, px); }
        return;
    case 't':
        if (prop == "text-align") {
            if (lv.find("center") >= 0) s.text_align = Style::Center;
            else if (lv == "right" || lv == "end" || lv == "-webkit-right") s.text_align = Style::Right;
            else if (lv == "justify") s.text_align = Style::Justify;
            else s.text_align = Style::Left;
        }
        else if (prop == "text-decoration" || prop == "text-decoration-line") {
            s.underline = lv.find("underline") >= 0;
            s.strike = lv.find("line-through") >= 0;
        }
        else if (prop == "text-transform") s.text_transform = lv == "uppercase" ? 1 : lv == "lowercase" ? 2 : lv == "capitalize" ? 3 : 0;
        else if (prop == "top") { int px; if (parse_len(val, s.font_size, &px)) s.offset[0] = px; }
        return;
    case 'v':
        if (prop == "visibility") s.hidden = lv == "hidden" || lv == "collapse";
        else if (prop == "vertical-align") s.valign = lv == "top" || lv == "text-top" ? 1 : lv == "middle" ? 2 : lv == "bottom" || lv == "text-bottom" ? 3 : 0;
        return;
    case 'w':
        if (prop == "width") {
            int px, pct = -1;
            if (lv == "100%" ) { s.width_pct = 100; s.width = -1; }
            else if (parse_len(val, s.font_size, &px, &pct)) { if (pct >= 0) { s.width_pct = pct; s.width = -1; } else if (px != kUnset && px >= 0) { s.width = px; s.width_pct = -1; } }
            else if (lv == "fit-content" || lv == "max-content" || lv == "min-content") { s.width = -1; s.width_pct = -1; }
        }
        else if (prop == "white-space" || prop == "white-space-collapse" || prop == "text-wrap") {
            if (prop == "white-space") { s.pre = lv.starts_with("pre") || lv == "break-spaces"; s.nowrap = lv == "nowrap"; }
            else if (prop == "text-wrap") s.nowrap = lv == "nowrap";
        }
        return;
    }
}

// ================================================================ UA defaults
static void ua_defaults(Node* n, Style& s, const Style& parent) {
    const String& t = n->tag;
    int fs = parent.font_size;
    auto block = [&](int mt, int mb) { s.display = Style::Block; s.margin[0] = mt; s.margin[2] = mb; };
    if (t == "html" || t == "body" || t == "div" || t == "section" || t == "article" || t == "header" || t == "footer" ||
        t == "nav" || t == "main" || t == "aside" || t == "form" || t == "fieldset" || t == "figure" || t == "address" ||
        t == "details" || t == "summary" || t == "center" || t == "hgroup" || t == "noscript" || t == "figcaption" || t == "dl" ||
        t == "legend" || t == "caption" || t == "search" || t == "picture" && false) {
        s.display = Style::Block;
        if (t == "body") { s.margin[0] = s.margin[1] = s.margin[2] = s.margin[3] = 8; }
        if (t == "center") s.text_align = Style::Center;
        if (t == "figure") { s.margin[0] = s.margin[2] = 16; s.margin[1] = s.margin[3] = 40; }
        if (t == "fieldset") { for (int k = 0; k < 4; k++) { s.border_width[k] = 1; s.padding[k] = 10; } s.border_color = hex(0xC0C0C0); s.margin[0] = s.margin[2] = 8; }
        if (t == "dl") { s.margin[0] = s.margin[2] = fs; }
        if (t == "caption") s.text_align = Style::Center;
    } else if (t == "p") block(fs, fs);
    else if (t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6') {
        static const int sizes[] = {32, 24, 19, 16, 13, 11};
        static const int margins[] = {21, 20, 19, 21, 22, 25};
        int lvl = t[1] - '1';
        s.font_size = sizes[lvl];
        s.bold = true;
        s.weight = 700;
        block(margins[lvl], margins[lvl]);
    }
    else if (t == "ul" || t == "ol" || t == "menu") { block(fs, fs); s.padding[3] = 40; s.list_ordered = t == "ol"; s.list_none = false; if (parent.display == Style::ListItem) s.margin[0] = s.margin[2] = 0; }
    else if (t == "li") s.display = Style::ListItem;
    else if (t == "dt") s.display = Style::Block;
    else if (t == "dd") { s.display = Style::Block; s.margin[3] = 40; }
    else if (t == "blockquote") { block(fs, fs); s.margin[1] = s.margin[3] = 40; }
    else if (t == "pre" || t == "listing" || t == "xmp" || t == "plaintext") { block(fs, fs); s.pre = true; s.mono = true; s.font_size = fs * 13 / 16; }
    else if (t == "hr") { block(8, 8); }
    else if (t == "table") { s.display = Style::Table; s.text_align = Style::Left; }   // quirks: tables don't inherit text-align
    else if (t == "tr") s.display = Style::TableRow;
    else if (t == "td" || t == "th") { s.display = Style::TableCell; for (int k = 0; k < 4; k++) s.padding[k] = 2; if (t == "th") { s.bold = true; s.weight = 700; s.text_align = Style::Center; } }
    else if (t == "thead" || t == "tbody" || t == "tfoot") s.display = Style::Block;
    else if (t == "a") { s.color = hex(0x0B57D0); s.underline = true; }
    else if (t == "b" || t == "strong") { s.bold = true; s.weight = 700; }
    else if (t == "i" || t == "em" || t == "cite" || t == "var" || t == "dfn") s.italic = true;
    else if (t == "u" || t == "ins") s.underline = true;
    else if (t == "s" || t == "strike" || t == "del") s.strike = true;
    else if (t == "code" || t == "kbd" || t == "samp" || t == "tt") { s.mono = true; s.font_size = fs * 14 / 16; }
    else if (t == "small") s.font_size = fs * 5 / 6;
    else if (t == "big") s.font_size = fs * 6 / 5;
    else if (t == "sub" || t == "sup") s.font_size = fs * 3 / 4;
    else if (t == "mark") { s.background = hex(0xFFF59D); }
    else if (t == "head" || t == "script" || t == "style" || t == "title" || t == "meta" || t == "link" || t == "template" ||
             t == "object" || t == "embed" || t == "canvas" || t == "audio" || t == "video" ||
             t == "map" || t == "area" || t == "datalist" || t == "dialog" || t == "base" || t == "param" || t == "source" || t == "track" || t == "slot") s.display = Style::None;
    else if (t == "img" || t == "input" || t == "button" || t == "select" || t == "textarea" || t == "svg" || t == "iframe") s.display = Style::InlineBlock;
    else if (t == "font") {
        const char* sz = n->attr("size");
        if (sz) {
            int v = atoi(sz);
            if (sz[0] == '+' || sz[0] == '-') v = 3 + v;
            static const int map[] = {10, 13, 16, 18, 24, 32, 48};
            s.font_size = map[kclamp(v, 1, 7) - 1];
        }
    }
    const char* a;
    if ((a = n->attr("bgcolor"))) { Color c; if (parse_color(String(a), &c)) s.background = c; }
    if ((a = n->attr("color")) && (t == "font")) { Color c; if (parse_color(String(a), &c)) s.color = c; }
    if ((a = n->attr("align"))) {
        String v = String(a).lower();
        if (v == "center") s.text_align = Style::Center;
        else if (v == "right") s.text_align = Style::Right;
    }
    if (t == "table" || t == "td" || t == "th" || t == "img" || t == "svg") {
        if ((a = n->attr("width"))) { if (strchr(a, '%')) s.width_pct = atoi(a); else if (atoi(a) > 0) s.width = atoi(a); }
        if ((a = n->attr("height")) && !strchr(a, '%') && atoi(a) > 0) s.height = atoi(a);
    }
    if (t == "table" && (a = n->attr("border")) && atoi(a) > 0) {
        for (int i = 0; i < 4; i++) s.border_width[i] = 1;
        s.border_color = hex(0x999999);
    }
    if ((t == "td" || t == "th") && n->attr("nowrap")) s.nowrap = true;
    if (n->attr("hidden")) s.display = Style::None;
    if (t == "noscript" && g_doc && g_doc->scripting) s.display = Style::None;
    if (t == "input") {
        const char* type = n->attr("type");
        if (type && !strcasecmp(type, "hidden")) s.display = Style::None;
    }
}

// ================================================================ matching
static bool match(const Selector& s, Node* n);

static Node* prev_element(Node* n) {
    if (!n->parent) return nullptr;
    Node* prev = nullptr;
    for (Node* c : n->parent->children) {
        if (c == n) return prev;
        if (c->type == Node::Element) prev = c;
    }
    return nullptr;
}

static bool has_match(const Pseudo& p, Node* n, bool child_only) {
    for (Node* c : n->children) {
        if (c->type != Node::Element) continue;
        for (Selector* sel : p.list) if (match(*sel, c)) return true;
        if (!child_only && has_match(p, c, false)) return true;
    }
    return false;
}

static bool match_pseudo(const Pseudo& p, Node* n) {
    Node* par = n->parent;
    switch (p.kind) {
    case Pseudo::Root: return par && par->type == Node::Document;
    case Pseudo::Has: return has_match(p, n, p.has_child);
    case Pseudo::Checked: return n->checked || n->attr("checked") || n->attr("selected");
    case Pseudo::Disabled: return n->attr("disabled") != nullptr;
    case Pseudo::Enabled: return !n->attr("disabled") && (n->tag == "input" || n->tag == "button" || n->tag == "select" || n->tag == "textarea");
    case Pseudo::Empty:
        for (Node* c : n->children) if (c->type == Node::Element || (c->type == Node::Text && !c->text.empty())) return false;
        return true;
    case Pseudo::Not:
        for (Selector* s : p.list) if (match(*s, n)) return false;
        return true;
    case Pseudo::Is:
        for (Selector* s : p.list) if (match(*s, n)) return true;
        return false;
    default: break;
    }
    if (!par) return false;
    int idx = 0, count = 0, my = -1, type_idx = 0, type_count = 0, my_type = -1;
    for (Node* c : par->children) {
        if (c->type != Node::Element) continue;
        if (c == n) { my = idx; my_type = type_idx; }
        idx++;
        count++;
        if (c->tag == n->tag) { type_idx++; type_count++; }
    }
    switch (p.kind) {
    case Pseudo::FirstChild: return my == 0;
    case Pseudo::LastChild: return my == count - 1;
    case Pseudo::OnlyChild: return count == 1;
    case Pseudo::FirstOfType: return my_type == 0;
    case Pseudo::LastOfType: return my_type == type_count - 1;
    case Pseudo::NthChild:
    case Pseudo::NthLastChild: {
        int pos = p.kind == Pseudo::NthChild ? my + 1 : count - my;
        if (p.a == 0) return pos == p.b;
        int k = pos - p.b;
        return k % p.a == 0 && k / p.a >= 0;
    }
    default: return false;
    }
}

static bool word_in(const char* list, const String& w) {
    size_t n = w.size();
    const char* p = list;
    while (*p) {
        while (*p == ' ') p++;
        const char* s = p;
        while (*p && *p != ' ') p++;
        if ((size_t)(p - s) == n && !strncmp(s, w.c_str(), n)) return true;
    }
    return false;
}

static bool match_compound(const Compound& c, Node* n) {
    if (n->type != Node::Element) return false;
    if (!c.tag.empty() && c.tag != n->tag) return false;
    if (!c.id.empty()) { const char* id = n->attr("id"); if (!id || c.id != id) return false; }
    for (const String& cls : c.classes) if (!n->has_class(cls.c_str())) return false;
    for (const AttrTest& t : c.attrs) {
        const char* v = n->attr(t.name.c_str());
        if (!v) return false;
        size_t vl = strlen(v), tl = t.value.size();
        switch (t.op) {
        case 0: break;
        case '=': if (strcmp(v, t.value.c_str())) return false; break;
        case '~': if (!word_in(v, t.value)) return false; break;
        case '^': if (strncmp(v, t.value.c_str(), tl)) return false; break;
        case '$': if (vl < tl || strcmp(v + vl - tl, t.value.c_str())) return false; break;
        case '*': if (!strstr(v, t.value.c_str())) return false; break;
        case '|': if (strncmp(v, t.value.c_str(), tl) || (v[tl] && v[tl] != '-')) return false; break;
        }
    }
    for (const Pseudo& p : c.pseudos) if (!match_pseudo(p, n)) return false;
    return true;
}

static bool match_from(const Selector& s, int i, Node* n) {
    if (!match_compound(s.parts[i], n)) return false;
    if (i == 0) return true;
    char comb = s.combinators[i - 1];
    switch (comb) {
    case '>': return n->parent && match_from(s, i - 1, n->parent);
    case '+': { Node* p = prev_element(n); return p && match_from(s, i - 1, p); }
    case '~': {
        for (Node* p = prev_element(n); p; p = prev_element(p)) if (match_from(s, i - 1, p)) return true;
        return false;
    }
    default:
        for (Node* p = n->parent; p; p = p->parent) if (match_from(s, i - 1, p)) return true;
        return false;
    }
}

static bool match(const Selector& s, Node* n) { return match_from(s, (int)s.parts.size() - 1, n); }

// Rule index by rightmost key
struct Bucket { String key; Vector<int> rules; };
struct RuleIndex {
    static constexpr int kBuckets = 1024;
    Vector<Bucket> ids[kBuckets / 4], classes[kBuckets], tags[64];
    Vector<int> universal;
    static uint32_t h(const char* s) { uint32_t x = 2166136261u; while (*s) { x ^= (uint8_t)*s++; x *= 16777619u; } return x; }
    static Vector<int>* find(Vector<Bucket>* tab, int n, const char* k) {
        for (Bucket& b : tab[h(k) % n]) if (b.key == k) return &b.rules;
        return nullptr;
    }
    static void add(Vector<Bucket>* tab, int n, const String& k, int r) {
        Vector<Bucket>& v = tab[h(k.c_str()) % n];
        for (Bucket& b : v) if (b.key == k) { b.rules.push(r); return; }
        Bucket& b = v.emplace();
        b.key = k;
        b.rules.push(r);
    }
};

static void apply_inline_style(Style& s, const Style& parent, const char* inl, bool important_pass, Node* n) {
    Vector<Decl> decls;
    parse_decls(inl, inl + strlen(inl), decls);
    for (Decl& d : decls) {
        if (d.important != important_pass) continue;
        if (d.prop.starts_with("--")) continue;
        apply_decl(s, parent, d.prop, d.value);
    }
}

static int g_style_depth;
static void style_node(Document* d, RuleIndex& idx, Node* n, const Style& parent, Vector<int>& cand) {
    if (g_style_depth > 300) return;
    Style s;
    s.color = parent.color;
    s.font_size = parent.font_size;
    s.weight = parent.weight;
    s.bold = parent.bold;
    s.italic = parent.italic;
    s.mono = parent.mono;
    s.underline = parent.underline;
    s.strike = parent.strike;
    s.text_align = parent.text_align;
    s.text_transform = parent.text_transform;
    s.pre = parent.pre;
    s.nowrap = parent.nowrap;
    s.hidden = parent.hidden;
    s.line_height_pct = parent.line_height_pct;
    s.list_ordered = parent.list_ordered;
    s.list_none = parent.list_none;
    s.vars = parent.vars;
    s.opacity = parent.opacity;
    if (n->type == Node::Text) { n->style = s; return; }
    if (n->type == Node::Element) {
        ua_defaults(n, s, parent);
        cand.clear();
        const char* id = n->attr("id");
        if (id) if (Vector<int>* v = RuleIndex::find(idx.ids, RuleIndex::kBuckets / 4, id)) for (int r : *v) cand.push(r);
        const char* cls = n->attr("class");
        if (cls) {
            const char* p = cls;
            while (*p) {
                while (*p == ' ' || *p == '\t' || *p == '\n') p++;
                const char* st = p;
                while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
                if (p > st) {
                    String c(st, p - st);
                    if (Vector<int>* v = RuleIndex::find(idx.classes, RuleIndex::kBuckets, c.c_str())) for (int r : *v) cand.push(r);
                }
            }
        }
        if (Vector<int>* v = RuleIndex::find(idx.tags, 64, n->tag.c_str())) for (int r : *v) cand.push(r);
        for (int r : idx.universal) cand.push(r);
        // dedupe + filter
        Vector<int> matched;
        for (size_t i = 0; i < cand.size(); i++) {
            int r = cand[i];
            bool dup = false;
            for (size_t j = 0; j < matched.size(); j++) if (matched[j] == r) { dup = true; break; }
            if (!dup && match(d->rules[r]->sel, n)) matched.push(r);
        }
        // sort by (specificity, order) - insertion sort
        for (size_t i = 1; i < matched.size(); i++)
            for (size_t j = i; j > 0; j--) {
                Document::Rule* a = d->rules[matched[j - 1]];
                Document::Rule* b = d->rules[matched[j]];
                if (a->sel.specificity > b->sel.specificity || (a->sel.specificity == b->sel.specificity && a->order > b->order)) {
                    int t = matched[j]; matched[j] = matched[j - 1]; matched[j - 1] = t;
                } else break;
            }
        // custom properties first
        const char* inl = n->attr("style");
        bool has_vars = false;
        for (int r : matched) for (Decl& dc : d->rules[r]->decls) if (dc.prop.starts_with("--")) { has_vars = true; break; }
        if (inl && strstr(inl, "--")) has_vars = true;
        if (has_vars) {
            delete n->own_vars;
            n->own_vars = new VarMap();
            n->own_vars->parent = parent.vars;
            s.vars = n->own_vars;
            for (int r : matched)
                for (Decl& dc : d->rules[r]->decls)
                    if (dc.prop.starts_with("--")) n->own_vars->set(dc.prop, dc.value);
            if (inl) {
                Vector<Decl> decls;
                parse_decls(inl, inl + strlen(inl), decls);
                for (Decl& dc : decls) if (dc.prop.starts_with("--")) n->own_vars->set(dc.prop, dc.value);
            }
        }
        for (int pass = 0; pass < 2; pass++) {
            for (int r : matched)
                for (Decl& dc : d->rules[r]->decls)
                    if (dc.important == (pass == 1) && !dc.prop.starts_with("--")) {
                        apply_decl(s, parent, dc.prop, dc.value);
                        if (g_css_debug && dc.prop == "display") g_css_debug(n, d->rules[r]->text.c_str(), dc.prop.c_str(), dc.value.c_str());
                    }
            if (inl) apply_inline_style(s, parent, inl, pass == 1, n);
        }
        if (n->tag == "svg" && s.display != Style::None) s.display = Style::InlineBlock;
    }
    n->style = s;
    int li = 0;
    g_style_depth++;
    for (Node* c : n->children) {
        style_node(d, idx, c, s, cand);
        if (c->type == Node::Element && c->tag == "li") c->list_index = ++li;
    }
    g_style_depth--;
}

void compute_styles(Document* d) {
    g_doc = d;
    g_viewport = d->viewport_w;
    RuleIndex* idx = new RuleIndex();
    for (int i = 0; i < (int)d->rules.size(); i++) {
        Compound& c = d->rules[i]->sel.parts.back();
        if (!c.id.empty()) RuleIndex::add(idx->ids, RuleIndex::kBuckets / 4, c.id, i);
        else if (!c.classes.empty()) RuleIndex::add(idx->classes, RuleIndex::kBuckets, c.classes[0], i);
        else if (!c.tag.empty()) RuleIndex::add(idx->tags, 64, c.tag, i);
        else idx->universal.push(i);
    }
    Style root;
    root.display = Style::Block;
    root.color = hex(0x111111);
    Vector<int> cand;
    style_node(d, *idx, d->root, root, cand);
    delete idx;
    d->background = hex(0xFFFFFF);
    Node* html = find_tag(d->root, "html");
    Node* body = find_tag(d->root, "body");
    if (html && color_a(html->style.background)) d->background = html->style.background | 0xFF000000;
    else if (body && color_a(body->style.background)) d->background = body->style.background | 0xFF000000;
    g_doc = nullptr;
}

}  // namespace html

// ================================================================ querySelector support
namespace html {

static void query_walk(Node* n, Vector<Selector*>& sels, bool first_only, Vector<Node*>& out) {
    for (Node* c : n->children) {
        if (c->type != Node::Element) continue;
        for (Selector* s : sels)
            if (match(*s, c)) { out.push(c); break; }
        if (first_only && !out.empty()) return;
        query_walk(c, sels, first_only, out);
        if (first_only && !out.empty()) return;
    }
}

bool query_selector(Node* root, const char* selector, bool first_only, Vector<Node*>& out) {
    Vector<Selector*> sels;
    const char* p = selector;
    const char* end = selector + strlen(selector);
    while (p < end) {
        while (p < end && (*p == ' ' || *p == ',')) p++;
        if (p >= end) break;
        Selector* s = new Selector();
        const char* before = p;
        bool ok = parse_selector(p, end, *s, ',');
        bool impossible = false;
        for (Compound& c : s->parts) if (c.impossible) impossible = true;
        if (ok && !impossible) sels.push(s);
        else delete s;
        if (p == before) p++;
    }
    if (!sels.empty()) query_walk(root, sels, first_only, out);
    for (Selector* s : sels) delete s;
    return true;
}

bool matches_selector(Node* n, const char* selector) {
    Vector<Node*> out;
    if (!n->parent) return false;
    // test only n: walk its parent but filter
    Vector<Selector*> sels;
    const char* p = selector;
    const char* end = selector + strlen(selector);
    bool hit = false;
    while (p < end && !hit) {
        while (p < end && (*p == ' ' || *p == ',')) p++;
        if (p >= end) break;
        Selector s;
        const char* before = p;
        if (parse_selector(p, end, s, ',')) {
            bool impossible = false;
            for (Compound& c : s.parts) if (c.impossible) impossible = true;
            if (!impossible && match(s, n)) hit = true;
        }
        if (p == before) p++;
    }
    return hit;
}

}  // namespace html
