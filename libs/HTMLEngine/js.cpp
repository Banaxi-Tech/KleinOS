// Page JavaScript: QuickJS runtime per document, a handful of native DOM
// primitives (node handles, attributes, tree edits, innerHTML, selectors,
// fetch plumbing) and a JS prelude that builds the familiar browser API
// (window, document, Element, events, timers, storage, fetch, XHR, ...).
#include "js.h"
#include "internal.h"
#include "net/http.h"
#include "net/net.h"
#include "lib/string.h"
#include "lib/log.h"
#include "arch/interrupts.h"
#include "arch/cpu.h"
#include "ui/func.h"
#include "ECMAScript/quickjs.h"

namespace html {

struct JsEnv {
    Document* doc;
    JSRuntime* rt;
    JSContext* ctx;
    Vector<Node*> nodes;          // handle -> node
    Vector<ScriptInfo> scripts;
    Vector<JsFetch> fetches;
    Vector<JsPost> posts;
    Vector<JsWorkerReq> workers;
    Vector<String> console;
    String navigation;
    bool dirty = false;
    bool rescan = false;          // look for script elements added to the DOM
    int next_fetch = 1;
    uint64_t deadline = 0;        // interrupt long-running scripts
};

namespace {

JsEnv* env_of(JSContext* ctx) { return (JsEnv*)JS_GetContextOpaque(ctx); }
void mark_scripts_seen_fwd(Node* n) {
    if (n->type == Node::Element && n->tag == "script") n->script_seen = true;
    for (Node* c : n->children) mark_scripts_seen_fwd(c);
}

int handle_of(JsEnv* e, Node* n) {
    if (!n) return -1;
    if (n->js_id >= 0 && n->js_id < (int)e->nodes.size() && e->nodes[n->js_id] == n) return n->js_id;
    n->js_id = (int)e->nodes.size();
    e->nodes.push(n);
    return n->js_id;
}

Node* node_of(JSContext* ctx, JSValueConst v) {
    JsEnv* e = env_of(ctx);
    int32_t h;
    if (JS_ToInt32(ctx, &h, v) || h < 0 || h >= (int)e->nodes.size()) return nullptr;
    return e->nodes[h];
}

String to_string(JSContext* ctx, JSValueConst v) {
    size_t len;
    const char* s = JS_ToCStringLen(ctx, &len, v);
    if (!s) return String();
    String r(s, len);
    JS_FreeCString(ctx, s);
    return r;
}

JSValue str(JSContext* ctx, const String& s) { return JS_NewStringLen(ctx, s.c_str(), s.size()); }
JSValue hval(JSContext* ctx, Node* n) { return n ? JS_NewInt32(ctx, handle_of(env_of(ctx), n)) : JS_NULL; }

void detach(Node* n) {
    if (!n->parent) return;
    n->parent->children.remove_value(n);
    n->parent = nullptr;
}

void track_orphan(JsEnv* e, Node* n) {
    if (!e->doc->orphans.contains(n)) e->doc->orphans.push(n);
}

#define ARGN(i) (argc > i ? argv[i] : JS_UNDEFINED)

// ---- tree / node primitives
JSValue n_type(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    if (!n) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, n->type == Node::Element ? 1 : n->type == Node::Text ? 3 : 9);
}
JSValue n_tag(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    return n ? str(ctx, n->tag) : JS_NULL;
}
JSValue n_parent(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    return n ? hval(ctx, n->parent) : JS_NULL;
}
JSValue n_children(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    JSValue arr = JS_NewArray(ctx);
    if (!n) return arr;
    int32_t elements_only = 0;
    if (argc > 1) JS_ToInt32(ctx, &elements_only, argv[1]);
    uint32_t i = 0;
    for (Node* c : n->children) {
        if (elements_only && c->type != Node::Element) continue;
        JS_SetPropertyUint32(ctx, arr, i++, hval(ctx, c));
    }
    return arr;
}
JSValue n_sibling(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    int32_t dir = 1, elements = 0;
    if (argc > 1) JS_ToInt32(ctx, &dir, argv[1]);
    if (argc > 2) JS_ToInt32(ctx, &elements, argv[2]);
    if (!n || !n->parent) return JS_NULL;
    Vector<Node*>& sib = n->parent->children;
    long i = sib.index_of(n);
    for (long k = i + dir; k >= 0 && k < (long)sib.size(); k += dir)
        if (!elements || sib[k]->type == Node::Element) return hval(ctx, sib[k]);
    return JS_NULL;
}
JSValue n_get_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    if (!n) return JS_NULL;
    String name = to_string(ctx, ARGN(1)).lower();
    const char* v = n->attr(name.c_str());
    return v ? JS_NewString(ctx, v) : JS_NULL;
}
JSValue n_set_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    if (!n || n->type != Node::Element) return JS_UNDEFINED;
    String name = to_string(ctx, ARGN(1)).lower();
    String val = to_string(ctx, ARGN(2));
    bool found = false;
    for (Attr& a : n->attrs) if (a.name == name) { a.value = val; found = true; break; }
    if (!found) n->attrs.push(Attr{name, val});
    if (name == "value") n->value = val;
    env_of(ctx)->dirty = true;
    return JS_UNDEFINED;
}
JSValue n_remove_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    if (!n) return JS_UNDEFINED;
    String name = to_string(ctx, ARGN(1)).lower();
    for (size_t i = 0; i < n->attrs.size(); i++)
        if (n->attrs[i].name == name) { n->attrs.remove(i); env_of(ctx)->dirty = true; break; }
    return JS_UNDEFINED;
}
JSValue n_attr_names(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    JSValue arr = JS_NewArray(ctx);
    if (!n) return arr;
    for (uint32_t i = 0; i < n->attrs.size(); i++) JS_SetPropertyUint32(ctx, arr, i, str(ctx, n->attrs[i].name));
    return arr;
}
JSValue n_text_get(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    return n ? str(ctx, inner_text(n)) : JS_NULL;
}
JSValue n_text_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    if (!n) return JS_UNDEFINED;
    JsEnv* e = env_of(ctx);
    String t = to_string(ctx, ARGN(1));
    if (n->type == Node::Text) { n->text = t; e->dirty = true; return JS_UNDEFINED; }
    for (Node* c : n->children) { c->parent = nullptr; track_orphan(e, c); }
    n->children.clear();
    Node* tn = new Node();
    tn->type = Node::Text;
    tn->text = t;
    tn->parent = n;
    n->children.push(tn);
    e->dirty = true;
    return JS_UNDEFINED;
}
JSValue n_html_get(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    int32_t outer = 0;
    if (argc > 1) JS_ToInt32(ctx, &outer, argv[1]);
    if (!n) return JS_NULL;
    String out;
    if (outer) serialize_node(n, out);
    else for (Node* c : n->children) serialize_node(c, out);
    return str(ctx, out);
}
JSValue n_html_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    if (!n || n->type == Node::Text) return JS_UNDEFINED;
    JsEnv* e = env_of(ctx);
    String h = to_string(ctx, ARGN(1));
    for (Node* c : n->children) { c->parent = nullptr; track_orphan(e, c); }
    n->children.clear();
    Node* frag = parse_dom(h.c_str(), h.size());
    mark_scripts_seen_fwd(frag);   // scripts from innerHTML never run
    for (Node* c : frag->children) { c->parent = n; n->children.push(c); }
    frag->children.clear();
    delete frag;
    e->dirty = true;
    return JS_UNDEFINED;
}
JSValue n_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JsEnv* e = env_of(ctx);
    Node* n = new Node();
    int32_t text = 0;
    if (argc > 1) JS_ToInt32(ctx, &text, argv[1]);
    if (text == 1) { n->type = Node::Text; n->text = to_string(ctx, ARGN(0)); }
    else if (text == 2) { n->type = Node::Document; n->tag = "#document-fragment"; }
    else { n->type = Node::Element; n->tag = to_string(ctx, ARGN(0)).lower(); }
    track_orphan(e, n);
    return hval(ctx, n);
}
JSValue n_insert(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* parent = node_of(ctx, ARGN(0));
    Node* child = node_of(ctx, ARGN(1));
    Node* before = argc > 2 && !JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2]) ? node_of(ctx, argv[2]) : nullptr;
    if (!parent || !child || parent == child) return JS_UNDEFINED;
    for (Node* p = parent; p; p = p->parent) if (p == child) return JS_ThrowTypeError(ctx, "HierarchyRequestError");
    JsEnv* e = env_of(ctx);
    // document fragments insert their children
    Vector<Node*> items;
    if (child->type == Node::Document) { items = child->children; for (Node* c : items) c->parent = nullptr; child->children.clear(); }
    else { detach(child); items.push(child); }
    for (Node* c : items) {
        c->parent = parent;
        long idx = before ? parent->children.index_of(before) : -1;
        if (idx < 0) parent->children.push(c);
        else parent->children.insert(idx, c);
    }
    e->dirty = true;
    return JS_UNDEFINED;
}
JSValue n_remove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    if (!n) return JS_UNDEFINED;
    JsEnv* e = env_of(ctx);
    detach(n);
    track_orphan(e, n);
    e->dirty = true;
    return JS_UNDEFINED;
}
JSValue n_clone(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    if (!n) return JS_NULL;
    int32_t deep = 0;
    if (argc > 1) JS_ToInt32(ctx, &deep, argv[1]);
    JsEnv* e = env_of(ctx);
    Func<Node*(Node*)> copy;
    copy = [&](Node* src) -> Node* {
        Node* c = new Node();
        c->type = src->type;
        c->tag = src->tag;
        c->text = src->text;
        c->attrs = src->attrs;
        c->value = src->value;
        if (deep) for (Node* k : src->children) { Node* kc = copy(k); kc->parent = c; c->children.push(kc); }
        return c;
    };
    Node* c = copy(n);
    track_orphan(e, c);
    return hval(ctx, c);
}
JSValue n_query(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    String sel = to_string(ctx, ARGN(1));
    int32_t all = 0;
    if (argc > 2) JS_ToInt32(ctx, &all, argv[2]);
    Vector<Node*> out;
    if (n) query_selector(n, sel.c_str(), !all, out);
    if (!all) return out.empty() ? JS_NULL : hval(ctx, out[0]);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < out.size(); i++) JS_SetPropertyUint32(ctx, arr, i, hval(ctx, out[i]));
    return arr;
}
JSValue n_matches(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    String sel = to_string(ctx, ARGN(1));
    return JS_NewBool(ctx, n && matches_selector(n, sel.c_str()));
}
JSValue n_value_get(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    if (!n) return JS_NewString(ctx, "");
    if (n->value.empty() && n->attr("value")) return JS_NewString(ctx, n->attr("value"));
    return str(ctx, n->value);
}
JSValue n_value_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    if (n) { n->value = to_string(ctx, ARGN(1)); env_of(ctx)->dirty = true; }
    return JS_UNDEFINED;
}
JSValue n_checked(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = node_of(ctx, ARGN(0));
    if (!n) return JS_FALSE;
    if (argc > 1) { n->checked = JS_ToBool(ctx, argv[1]) > 0; env_of(ctx)->dirty = true; }
    return JS_NewBool(ctx, n->checked);
}
JSValue n_rect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    // bounding box of the node's fragments (document coordinates)
    Node* n = node_of(ctx, ARGN(0));
    JSValue o = JS_NewObject(ctx);
    Rect r;
    if (n) {
        Document* d = env_of(ctx)->doc;
        for (Fragment& f : d->frags) {
            for (Node* p = f.node; p; p = p->parent)
                if (p == n) { r = r.unite(f.r); break; }
        }
    }
    JS_SetPropertyStr(ctx, o, "x", JS_NewInt32(ctx, r.x));
    JS_SetPropertyStr(ctx, o, "y", JS_NewInt32(ctx, r.y));
    JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, r.w));
    JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, r.h));
    return o;
}
JSValue n_style(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    // computed style subset
    Node* n = node_of(ctx, ARGN(0));
    JSValue o = JS_NewObject(ctx);
    if (!n) return o;
    static const char* disp[] = {"inline", "block", "none", "list-item", "table", "table-row", "table-cell", "inline-block", "flex", "grid", "contents"};
    const Style& s = n->style;
    JS_SetPropertyStr(ctx, o, "display", JS_NewString(ctx, disp[s.display]));
    JS_SetPropertyStr(ctx, o, "visibility", JS_NewString(ctx, s.hidden ? "hidden" : "visible"));
    char buf[64];
    snprintf(buf, sizeof(buf), "rgb(%d, %d, %d)", color_r(s.color), color_g(s.color), color_b(s.color));
    JS_SetPropertyStr(ctx, o, "color", JS_NewString(ctx, buf));
    snprintf(buf, sizeof(buf), "%dpx", s.font_size);
    JS_SetPropertyStr(ctx, o, "fontSize", JS_NewString(ctx, buf));
    JS_SetPropertyStr(ctx, o, "position", JS_NewString(ctx, s.position == Style::Absolute ? "absolute" : s.position == Style::Fixed ? "fixed" : s.position == Style::Relative ? "relative" : "static"));
    return o;
}
JSValue n_doc(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JsEnv* e = env_of(ctx);
    String which = to_string(ctx, ARGN(0));
    if (which == "root") return hval(ctx, e->doc->root);
    Node* n = find_tag(e->doc->root, which.c_str());
    if (!n && (which == "body" || which == "head")) {
        // synthesize missing <head>/<body>
        Node* html_el = find_tag(e->doc->root, "html");
        if (!html_el) return JS_NULL;
        n = new Node();
        n->type = Node::Element;
        n->tag = which;
        n->parent = html_el;
        html_el->children.push(n);
    }
    return hval(ctx, n);
}

// ---- environment
JSValue g_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JsEnv* e = env_of(ctx);
    String level = to_string(ctx, ARGN(0));
    String msg = to_string(ctx, ARGN(1));
    if (e->console.size() > 200) e->console.remove(0);
    e->console.push(level + ": " + msg);
    if (level != "debug") klog("js %s: %.300s", level.c_str(), msg.c_str());
    return JS_UNDEFINED;
}
JSValue g_now(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewFloat64(ctx, (double)timer_ms()); }
JSValue g_dirty(JSContext* ctx, JSValueConst, int, JSValueConst*) { env_of(ctx)->dirty = true; return JS_UNDEFINED; }
JSValue g_navigate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JsEnv* e = env_of(ctx);
    String to = to_string(ctx, ARGN(0));
    char abs[2300];
    net::url_resolve(e->doc->url.c_str(), to.c_str(), abs, sizeof(abs));
    e->navigation = abs;
    return JS_UNDEFINED;
}
JSValue g_post(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JsEnv* e = env_of(ctx);
    e->posts.push(JsPost{node_of(ctx, ARGN(0)), to_string(ctx, ARGN(1)), to_string(ctx, ARGN(2)), to_string(ctx, ARGN(3)), String(), to_string(ctx, ARGN(4))});
    return JS_UNDEFINED;
}
JSValue g_post_port(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    env_of(ctx)->posts.push(JsPost{nullptr, to_string(ctx, ARGN(1)), String(), to_string(ctx, ARGN(2)), to_string(ctx, ARGN(0))});
    return JS_UNDEFINED;
}
JSValue g_worker(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    env_of(ctx)->workers.push(JsWorkerReq{to_string(ctx, ARGN(0)), to_string(ctx, ARGN(1)), to_string(ctx, ARGN(2))});
    return JS_UNDEFINED;
}
// importScripts is synchronous by definition, so this waits on the network here.
JSValue g_import_script(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JsEnv* e = env_of(ctx);
    String ref = to_string(ctx, ARGN(0));
    char abs[2300];
    net::url_resolve(e->doc->url.c_str(), ref.c_str(), abs, sizeof(abs));
    uint64_t saved = e->deadline;
    e->deadline = 0;
    net::HttpRequest* r = net::http_get(abs);
    net::HttpState st = net::http_state(r);
    while (st != net::HttpState::Done && st != net::HttpState::Failed) { net::poll(); cpu_relax(); st = net::http_state(r); }
    size_t len = 0;
    const uint8_t* body = st == net::HttpState::Done && net::http_status(r) < 400 ? net::http_body(r, &len) : nullptr;
    bool ok = false;
    if (body && len) {
        String code((const char*)body, len);
        e->deadline = timer_ms() + 8000;
        JSValue v = JS_Eval(ctx, code.c_str(), code.size(), abs, JS_EVAL_TYPE_GLOBAL);
        ok = !JS_IsException(v);
        if (!ok) { net::http_free(r); e->deadline = saved; return v; }
        JS_FreeValue(ctx, v);
    }
    net::http_free(r);
    e->deadline = saved;
    if (!ok) return JS_ThrowTypeError(ctx, "importScripts: could not load %s", abs);
    return JS_UNDEFINED;
}
// random so a page can't address a port it was never given
JSValue g_port_id(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return str(ctx, String::format("%016llx%016llx", (unsigned long long)cpu_random64(), (unsigned long long)cpu_random64()));
}
JSValue g_url(JSContext* ctx, JSValueConst, int, JSValueConst*) { return str(ctx, env_of(ctx)->doc->url); }
JSValue g_resolve(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JsEnv* e = env_of(ctx);
    String ref = to_string(ctx, ARGN(0));
    String base = argc > 1 && !JS_IsUndefined(argv[1]) ? to_string(ctx, argv[1]) : e->doc->url;
    char abs[2300];
    net::url_resolve(base.c_str(), ref.c_str(), abs, sizeof(abs));
    return JS_NewString(ctx, abs);
}
JSValue g_fetch(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JsEnv* e = env_of(ctx);
    JsFetch f;
    f.id = e->next_fetch++;
    char abs[2300];
    String u = to_string(ctx, ARGN(0));
    net::url_resolve(e->doc->url.c_str(), u.c_str(), abs, sizeof(abs));
    f.url = abs;
    f.method = argc > 1 ? to_string(ctx, argv[1]) : String("GET");
    f.body = argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2]) ? to_string(ctx, argv[2]) : String();
    f.started = false;
    e->fetches.push(f);
    return JS_NewInt32(ctx, f.id);
}
JSValue g_title(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JsEnv* e = env_of(ctx);
    if (argc > 0) { e->doc->title = to_string(ctx, argv[0]); e->dirty = true; }
    return str(ctx, e->doc->title);
}
JSValue g_viewport(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewInt32(ctx, env_of(ctx)->doc->viewport_w); }
JSValue g_random(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewFloat64(ctx, (double)(cpu_random64() >> 11) / 9007199254740992.0);
}

int interrupt_handler(JSContext*, void* opaque) {
    JsEnv* e = (JsEnv*)opaque;
    return e->deadline && timer_ms() > e->deadline;
}

void report_exception(JsEnv* e, const char* where) {
    JSValue ex = JS_GetException(e->ctx);
    String msg = to_string(e->ctx, ex);
    JSValue stack = JS_GetPropertyStr(e->ctx, ex, "stack");
    String st = JS_IsUndefined(stack) ? String() : to_string(e->ctx, stack);
    JS_FreeValue(e->ctx, stack);
    JS_FreeValue(e->ctx, ex);
    klog("js error (%s): %.200s %.200s", where, msg.c_str(), st.c_str());
    if (e->console.size() > 200) e->console.remove(0);
    e->console.push(String("error: ") + msg);
}

bool eval(JsEnv* e, const char* code, size_t len, const char* name, uint64_t budget_ms = 4000) {
    e->deadline = timer_ms() + budget_ms;
    JSValue r = JS_Eval(e->ctx, code, len, name, JS_EVAL_TYPE_GLOBAL);
    e->deadline = 0;
    bool ok = !JS_IsException(r);
    if (!ok) report_exception(e, name);
    JS_FreeValue(e->ctx, r);
    return ok;
}

bool call_global(JsEnv* e, const char* fn, int argc, JSValue* argv, JSValue* result = nullptr) {
    JSValue g = JS_GetGlobalObject(e->ctx);
    JSValue f = JS_GetPropertyStr(e->ctx, g, fn);
    bool ok = false;
    if (JS_IsFunction(e->ctx, f)) {
        e->deadline = timer_ms() + 2000;
        JSValue r = JS_Call(e->ctx, f, g, argc, argv);
        e->deadline = 0;
        ok = !JS_IsException(r);
        if (!ok) report_exception(e, fn);
        if (result && ok) *result = r;
        else JS_FreeValue(e->ctx, r);
    }
    JS_FreeValue(e->ctx, f);
    JS_FreeValue(e->ctx, g);
    return ok;
}

void run_jobs(JsEnv* e) {
    JSContext* c;
    e->deadline = timer_ms() + 2000;
    for (int i = 0; i < 10000; i++) {
        int r = JS_ExecutePendingJob(e->rt, &c);
        if (r <= 0) { if (r < 0) report_exception(e, "promise job"); break; }
    }
    e->deadline = 0;
}

extern const char kPrelude[];

}  // namespace

static bool add_script(JsEnv* e, Node* s, bool dynamic) {
    const char* src = s->attr("src");
    if (dynamic && (!src || !*src) && s->children.empty()) return false;   // still being built: look again later
    s->script_seen = true;
    const char* type = s->attr("type");
    String ty = type ? String(type).lower() : String();
    bool module = ty == "module";
    if (!ty.empty() && !module && ty != "text/javascript" && ty != "application/javascript" && ty != "application/ecmascript" && ty != "text/ecmascript") return false;   // JSON, templates, ...
    if (s->attr("nomodule")) return false;
    ScriptInfo si;
    si.node = s;
    si.module = module;
    si.dynamic = dynamic;
    if (src && *src) {
        char abs[2300];
        net::url_resolve(e->doc->url.c_str(), src, abs, sizeof(abs));
        si.src = abs;
    } else {
        si.fetched = true;
        String code;
        for (Node* c : s->children) if (c->type == Node::Text) code += c->text;
        s->value = code;
    }
    e->scripts.push(si);
    return true;
}

static void scan_new_scripts(JsEnv* e, Node* n) {
    if (n->type == Node::Element && n->tag == "script" && !n->script_seen) add_script(e, n, true);
    for (Node* c : n->children) scan_new_scripts(e, c);
}

JsEnv* js_create(Document* d) {
    JsEnv* e = new JsEnv();
    e->doc = d;
    e->rt = JS_NewRuntime();
    JS_SetMemoryLimit(e->rt, 256 * 1024 * 1024);
    JS_SetMaxStackSize(e->rt, 2 * 1024 * 1024);
    JS_SetInterruptHandler(e->rt, interrupt_handler, e);
    e->ctx = JS_NewContext(e->rt);
    JS_SetContextOpaque(e->ctx, e);
    JSValue g = JS_GetGlobalObject(e->ctx);
    struct { const char* name; JSCFunction* fn; int n; } natives[] = {
        {"__n_type", n_type, 1}, {"__n_tag", n_tag, 1}, {"__n_parent", n_parent, 1}, {"__n_children", n_children, 2},
        {"__n_sibling", n_sibling, 3}, {"__n_get_attr", n_get_attr, 2}, {"__n_set_attr", n_set_attr, 3},
        {"__n_remove_attr", n_remove_attr, 2}, {"__n_attr_names", n_attr_names, 1}, {"__n_text_get", n_text_get, 1},
        {"__n_text_set", n_text_set, 2}, {"__n_html_get", n_html_get, 2}, {"__n_html_set", n_html_set, 2},
        {"__n_create", n_create, 2}, {"__n_insert", n_insert, 3}, {"__n_remove", n_remove, 1}, {"__n_clone", n_clone, 2},
        {"__n_query", n_query, 3}, {"__n_matches", n_matches, 2}, {"__n_value_get", n_value_get, 1},
        {"__n_value_set", n_value_set, 2}, {"__n_checked", n_checked, 2}, {"__n_rect", n_rect, 1}, {"__n_style", n_style, 1},
        {"__n_doc", n_doc, 1}, {"__log", g_log, 2}, {"__now", g_now, 0}, {"__dirty", g_dirty, 0}, {"__navigate", g_navigate, 1},
        {"__post", g_post, 5}, {"__worker", g_worker, 3}, {"__import_script", g_import_script, 1}, {"__post_port", g_post_port, 3}, {"__port_id", g_port_id, 0}, {"__url", g_url, 0}, {"__resolve", g_resolve, 2}, {"__fetch", g_fetch, 3}, {"__title", g_title, 1},
        {"__viewport", g_viewport, 0}, {"__random", g_random, 0},
    };
    for (auto& n : natives) JS_SetPropertyStr(e->ctx, g, n.name, JS_NewCFunction(e->ctx, n.fn, n.name, n.n));
    JS_FreeValue(e->ctx, g);
    eval(e, kPrelude, strlen(kPrelude), "<prelude>", 10000);
    // collect scripts in document order
    Vector<Node*> stack;
    stack.push(d->root);
    Vector<Node*> order;
    // iterative pre-order traversal
    Func<void(Node*)> walk;
    walk = [&](Node* n) {
        if (n->type == Node::Element && n->tag == "script") order.push(n);
        for (Node* c : n->children) walk(c);
    };
    walk(d->root);
    for (Node* s : order) add_script(e, s, false);
    return e;
}

void js_destroy(JsEnv* e) {
    if (!e) return;
    for (Node* n : e->nodes) if (n) n->js_id = -1;
    JS_FreeContext(e->ctx);
    JS_FreeRuntime(e->rt);
    delete e;
}

Vector<ScriptInfo>& js_scripts(JsEnv* e) { return e->scripts; }

void js_run_scripts(JsEnv* e) {
    uint64_t t0 = timer_ms();
    int ran = 0;
    for (ScriptInfo& s : e->scripts) {
        if (s.failed || s.node->value.empty()) continue;
        JSValue h = JS_NewInt32(e->ctx, handle_of(e, s.node));
        call_global(e, "__set_current_script", 1, &h);
        const char* name = s.src.empty() ? "inline" : s.src.c_str();
        if (s.module) {
            // treat module bodies as classic scripts when they don't import anything
            if (s.node->value.find("import ") >= 0 || s.node->value.find("export ") >= 0) {
                klog("js: skipping ES module %s (imports not supported)", name);
                continue;
            }
        }
        eval(e, s.node->value.c_str(), s.node->value.size(), name);
        run_jobs(e);
        if (!s.src.empty()) js_fire_event(e, s.node, "load");
        ran++;
    }
    e->rescan = true;
    call_global(e, "__fire_loaded", 0, nullptr);
    run_jobs(e);
    klog("js: ran %d scripts in %llu ms", ran, (unsigned long long)(timer_ms() - t0));
}

void js_fire_event(JsEnv* e, Node* target, const char* type) {
    if (!target) return;
    JSValue args[2] = {JS_NewInt32(e->ctx, handle_of(e, target)), JS_NewString(e->ctx, type)};
    call_global(e, "__fire_node", 2, args);
    JS_FreeValue(e->ctx, args[1]);
    run_jobs(e);
}

static void run_dynamic_scripts(JsEnv* e) {
    for (size_t i = 0; i < e->scripts.size(); i++) {
        ScriptInfo& s = e->scripts[i];
        if (!s.dynamic || s.ran || (!s.fetched && !s.failed)) continue;
        s.ran = true;
        e->rescan = true;
        bool imports = s.module && (s.node->value.find("import ") >= 0 || s.node->value.find("export ") >= 0);
        if (!s.failed && !s.node->value.empty() && !imports) {
            JSValue h = JS_NewInt32(e->ctx, handle_of(e, s.node));
            call_global(e, "__set_current_script", 1, &h);
            eval(e, s.node->value.c_str(), s.node->value.size(), s.src.empty() ? "inline" : s.src.c_str());
            run_jobs(e);
            call_global(e, "__clear_current_script", 0, nullptr);
        }
        if (!s.src.empty()) js_fire_event(e, s.node, s.failed || imports ? "error" : "load");
    }
}

bool js_tick(JsEnv* e, uint64_t now) {
    JSValue t = JS_NewFloat64(e->ctx, (double)now);
    call_global(e, "__tick", 1, &t);
    run_jobs(e);
    if (e->dirty || e->rescan) { e->rescan = false; scan_new_scripts(e, e->doc->root); }
    run_dynamic_scripts(e);
    bool d = e->dirty;
    e->dirty = false;
    return d;
}

bool js_dispatch(JsEnv* e, Node* target, const char* type) {
    if (!target) return false;
    JSValue args[2] = {JS_NewInt32(e->ctx, handle_of(e, target)), JS_NewString(e->ctx, type)};
    JSValue r = JS_UNDEFINED;
    bool prevented = false;
    if (call_global(e, "__dispatch", 2, args, &r)) {
        prevented = JS_ToBool(e->ctx, r) > 0;
        JS_FreeValue(e->ctx, r);
    }
    JS_FreeValue(e->ctx, args[1]);
    run_jobs(e);
    return prevented;
}

void js_input_changed(JsEnv* e, Node* n) { js_dispatch(e, n, "input"); }

String js_take_navigation(JsEnv* e) {
    String n = e->navigation;
    e->navigation = String();
    return n;
}

Vector<JsFetch>& js_fetches(JsEnv* e) { return e->fetches; }
Vector<JsPost>& js_posts(JsEnv* e) { return e->posts; }
Vector<JsWorkerReq>& js_worker_requests(JsEnv* e) { return e->workers; }

void js_start_worker(JsEnv* e, const String& port, const String& peer, const char* code, size_t len, const char* url) {
    JSValue args[2] = {JS_NewStringLen(e->ctx, port.c_str(), port.size()), JS_NewStringLen(e->ctx, peer.c_str(), peer.size())};
    call_global(e, "__set_worker", 2, args);
    for (JSValue& v : args) JS_FreeValue(e->ctx, v);
    String source(code, len);   // the parser needs a NUL-terminated buffer; a raw HTTP body has none
    eval(e, source.c_str(), source.size(), url, 8000);
    run_jobs(e);
}

bool js_deliver_port(JsEnv* e, const String& port, const String& data, const String& ports) {
    JSValue args[3] = {JS_NewStringLen(e->ctx, port.c_str(), port.size()), JS_NewStringLen(e->ctx, data.c_str(), data.size()),
                       JS_NewStringLen(e->ctx, ports.c_str(), ports.size())};
    JSValue r = JS_UNDEFINED;
    bool held = false;
    if (call_global(e, "__on_port_message", 3, args, &r)) { held = JS_ToBool(e->ctx, r) > 0; JS_FreeValue(e->ctx, r); }
    for (JSValue& v : args) JS_FreeValue(e->ctx, v);
    run_jobs(e);
    return held;
}

void js_deliver_message(JsEnv* e, const String& data, const String& origin, Node* source, const String& ports) {
    JSValue args[4] = {JS_NewStringLen(e->ctx, data.c_str(), data.size()), JS_NewStringLen(e->ctx, origin.c_str(), origin.size()),
                       source ? JS_NewInt32(e->ctx, handle_of(e, source)) : JS_NULL, JS_NewStringLen(e->ctx, ports.c_str(), ports.size())};
    call_global(e, "__on_message", 4, args);
    for (JSValue& v : args) JS_FreeValue(e->ctx, v);
    run_jobs(e);
}

void js_set_framed(JsEnv* e, const char* name) {
    JSValue arg = JS_NewString(e->ctx, name ? name : "");
    call_global(e, "__set_framed", 1, &arg);
    JS_FreeValue(e->ctx, arg);
}

void js_fetch_done(JsEnv* e, int id, int status, const uint8_t* body, size_t len, const char* ctype) {
    for (size_t i = 0; i < e->fetches.size(); i++)
        if (e->fetches[i].id == id) { e->fetches.remove(i); break; }
    JSValue args[4] = {JS_NewInt32(e->ctx, id), JS_NewInt32(e->ctx, status), JS_NewStringLen(e->ctx, (const char*)body, len),
                       JS_NewString(e->ctx, ctype ? ctype : "")};
    call_global(e, "__fetch_done", 4, args);
    for (JSValue& v : args) JS_FreeValue(e->ctx, v);
    run_jobs(e);
}

const Vector<String>& js_console(JsEnv* e) { return e->console; }

namespace {
const char kPrelude[] = R"JS(
'use strict';
(function () {
const G = globalThis;
G.window = G; G.self = G; G.top = G; G.parent = G; G.frames = G;
const wrappers = new Map();
const listeners = new Map();   // handle -> {type: [fn...]}

class EventTarget {
  addEventListener(type, fn, opts) {
    if (!fn) return;
    const key = this.__h === undefined ? this : this.__h;
    let m = listeners.get(key); if (!m) { m = {}; listeners.set(key, m); }
    (m[type] = m[type] || []).push({fn, once: !!(opts && opts.once)});
  }
  removeEventListener(type, fn) {
    const key = this.__h === undefined ? this : this.__h;
    const m = listeners.get(key); if (!m || !m[type]) return;
    m[type] = m[type].filter(l => l.fn !== fn);
  }
  dispatchEvent(ev) { fireOn(this, ev); return !ev.defaultPrevented; }
}
class Event {
  constructor(type, init) { this.type = type; this.bubbles = !!(init && init.bubbles); this.cancelable = !!(init && init.cancelable);
    this.defaultPrevented = false; this.target = null; this.currentTarget = null; this.timeStamp = __now(); this.detail = init && init.detail; this._stop = false; }
  preventDefault() { this.defaultPrevented = true; }
  stopPropagation() { this._stop = true; }
  stopImmediatePropagation() { this._stop = true; }
}
class CustomEvent extends Event { constructor(t, i) { super(t, i); this.detail = i && i.detail; } }
class MouseEvent extends Event { constructor(t, i) { super(t, i); Object.assign(this, {clientX: 0, clientY: 0, button: 0}, i || {}); } }
class KeyboardEvent extends Event { constructor(t, i) { super(t, i); Object.assign(this, {key: '', code: ''}, i || {}); } }
class InputEvent extends Event {}
class FocusEvent extends Event {}
class PointerEvent extends MouseEvent {}
function fireOn(obj, ev) {
  ev.currentTarget = obj; if (!ev.target) ev.target = obj;
  const key = obj.__h === undefined ? obj : obj.__h;
  const m = listeners.get(key);
  if (m && m[ev.type]) for (const l of m[ev.type].slice()) {
    try { l.fn.call(obj, ev); } catch (e) { console.error(String(e) + (e && e.stack ? '\n' + e.stack : '')); }
    if (l.once) obj.removeEventListener(ev.type, l.fn);
    if (ev._stop) break;
  }
  const attr = obj['on' + ev.type];
  if (typeof attr === 'function') { try { if (attr.call(obj, ev) === false) ev.preventDefault(); } catch (e) { console.error(String(e)); } }
}

const camel2kebab = s => s.replace(/[A-Z]/g, c => '-' + c.toLowerCase());
function parseStyle(s) { const o = {}; (s || '').split(';').forEach(d => { const i = d.indexOf(':'); if (i > 0) o[d.slice(0, i).trim()] = d.slice(i + 1).trim(); }); return o; }
function styleProxy(el) {
  const target = {};
  return new Proxy(target, {
    get(t, k) {
      if (k === 'setProperty') return (p, v) => { const o = parseStyle(__n_get_attr(el.__h, 'style')); if (v === null || v === '') delete o[p]; else o[p] = String(v); write(o); };
      if (k === 'removeProperty') return p => { const o = parseStyle(__n_get_attr(el.__h, 'style')); delete o[p]; write(o); };
      if (k === 'getPropertyValue') return p => parseStyle(__n_get_attr(el.__h, 'style'))[p] || '';
      if (k === 'cssText') return __n_get_attr(el.__h, 'style') || '';
      if (typeof k !== 'string') return undefined;
      return parseStyle(__n_get_attr(el.__h, 'style'))[camel2kebab(k)] || '';
    },
    set(t, k, v) {
      if (k === 'cssText') { __n_set_attr(el.__h, 'style', String(v)); return true; }
      const o = parseStyle(__n_get_attr(el.__h, 'style'));
      const p = k.startsWith('--') ? k : camel2kebab(k);
      if (v === null || v === undefined || v === '') delete o[p]; else o[p] = String(v);
      write(o); return true;
    }
  });
  function write(o) { __n_set_attr(el.__h, 'style', Object.entries(o).map(([k, v]) => k + ': ' + v).join('; ')); }
}
class DOMTokenList {
  constructor(el) { this.el = el; }
  _get() { return (__n_get_attr(this.el.__h, 'class') || '').split(/\s+/).filter(Boolean); }
  _set(a) { __n_set_attr(this.el.__h, 'class', a.join(' ')); }
  add(...c) { const a = this._get(); for (const x of c) if (!a.includes(x)) a.push(x); this._set(a); }
  remove(...c) { this._set(this._get().filter(x => !c.includes(x))); }
  toggle(c, force) { const has = this.contains(c); const want = force === undefined ? !has : !!force; if (want && !has) this.add(c); else if (!want && has) this.remove(c); return want; }
  contains(c) { return this._get().includes(c); }
  replace(a, b) { this._set(this._get().map(x => x === a ? b : x)); }
  item(i) { return this._get()[i] || null; }
  get length() { return this._get().length; }
  get value() { return this._get().join(' '); }
  forEach(f) { this._get().forEach(f); }
  [Symbol.iterator]() { return this._get()[Symbol.iterator](); }
  toString() { return this.value; }
}
function wrap(h) {
  if (h === null || h === undefined || h < 0) return null;
  let w = wrappers.get(h); if (w) return w;
  const t = __n_type(h);
  if (t === 3) w = Object.create(Text.prototype);
  else if (t === 9) w = Object.create(DocumentFragment.prototype);
  else {
    const tag = __n_tag(h);
    const C = tagClasses[tag] || HTMLElement;
    w = Object.create(C.prototype);
  }
  Object.defineProperty(w, '__h', {value: h});
  wrappers.set(h, w);
  return w;
}
const toList = arr => { const a = arr.map(wrap); a.item = i => a[i] || null; return a; };

class Node extends EventTarget {
  get nodeType() { return __n_type(this.__h); }
  get nodeName() { const t = __n_type(this.__h); return t === 3 ? '#text' : t === 9 ? '#document-fragment' : __n_tag(this.__h).toUpperCase(); }
  get parentNode() { const p = __n_parent(this.__h); return p === null ? null : (p === __n_doc('root') ? document : wrap(p)); }
  get parentElement() { const p = __n_parent(this.__h); return p === null || __n_type(p) !== 1 ? null : wrap(p); }
  get childNodes() { return toList(__n_children(this.__h, 0)); }
  get firstChild() { const c = __n_children(this.__h, 0); return c.length ? wrap(c[0]) : null; }
  get lastChild() { const c = __n_children(this.__h, 0); return c.length ? wrap(c[c.length - 1]) : null; }
  get nextSibling() { return wrap(__n_sibling(this.__h, 1, 0)); }
  get previousSibling() { return wrap(__n_sibling(this.__h, -1, 0)); }
  get textContent() { return __n_text_get(this.__h); }
  set textContent(v) { __n_text_set(this.__h, v === null ? '' : String(v)); }
  get nodeValue() { return __n_type(this.__h) === 3 ? __n_text_get(this.__h) : null; }
  set nodeValue(v) { if (__n_type(this.__h) === 3) __n_text_set(this.__h, String(v)); }
  get isConnected() { let p = this.__h; while (p !== null) { if (p === __n_doc('root')) return true; p = __n_parent(p); } return false; }
  get ownerDocument() { return document; }
  appendChild(c) { __n_insert(this.__h, toNode(c).__h, null); return c; }
  insertBefore(c, ref) { __n_insert(this.__h, toNode(c).__h, ref ? ref.__h : null); return c; }
  removeChild(c) { __n_remove(c.__h); return c; }
  replaceChild(n, o) { __n_insert(this.__h, n.__h, o.__h); __n_remove(o.__h); return o; }
  contains(o) { if (!o) return false; let p = o.__h; while (p !== null) { if (p === this.__h) return true; p = __n_parent(p); } return false; }
  cloneNode(deep) { return wrap(__n_clone(this.__h, deep ? 1 : 0)); }
  hasChildNodes() { return __n_children(this.__h, 0).length > 0; }
  getRootNode() { return document; }
  isEqualNode(o) { return o === this; }
  compareDocumentPosition() { return 0; }
  normalize() {}
}
Node.ELEMENT_NODE = 1; Node.TEXT_NODE = 3; Node.COMMENT_NODE = 8; Node.DOCUMENT_NODE = 9; Node.DOCUMENT_FRAGMENT_NODE = 11;
function toNode(x) { return typeof x === 'string' ? document.createTextNode(x) : x; }
class CharacterData extends Node { get data() { return this.textContent; } set data(v) { this.textContent = v; } get length() { return this.textContent.length; } }
class Text extends CharacterData { get wholeText() { return this.textContent; } }
class Comment extends CharacterData {}
class ParentNodeMixin extends Node {
  get children() { return toList(__n_children(this.__h, 1)); }
  get childElementCount() { return __n_children(this.__h, 1).length; }
  get firstElementChild() { const c = __n_children(this.__h, 1); return c.length ? wrap(c[0]) : null; }
  get lastElementChild() { const c = __n_children(this.__h, 1); return c.length ? wrap(c[c.length - 1]) : null; }
  querySelector(s) { return wrap(__n_query(this.__h, String(s), 0)); }
  querySelectorAll(s) { return toList(__n_query(this.__h, String(s), 1)); }
  getElementsByTagName(t) { return toList(__n_query(this.__h, t === '*' ? '*' : String(t), 1)); }
  getElementsByClassName(c) { return toList(__n_query(this.__h, String(c).trim().split(/\s+/).map(x => '.' + x).join(''), 1)); }
  append(...n) { for (const x of n) this.appendChild(toNode(x)); }
  prepend(...n) { const f = this.firstChild; for (const x of n) this.insertBefore(toNode(x), f); }
  replaceChildren(...n) { this.textContent = ''; this.append(...n); }
}
class DocumentFragment extends ParentNodeMixin { getElementById(id) { return this.querySelector('#' + CSS.escape(id)); } }
class Element extends ParentNodeMixin {
  get tagName() { return __n_tag(this.__h).toUpperCase(); }
  get localName() { return __n_tag(this.__h); }
  get id() { return __n_get_attr(this.__h, 'id') || ''; }
  set id(v) { __n_set_attr(this.__h, 'id', String(v)); }
  get className() { return __n_get_attr(this.__h, 'class') || ''; }
  set className(v) { __n_set_attr(this.__h, 'class', String(v)); }
  get classList() { return this.__cl || (Object.defineProperty(this, '__cl', {value: new DOMTokenList(this)}), this.__cl); }
  get style() { return this.__st || (Object.defineProperty(this, '__st', {value: styleProxy(this)}), this.__st); }
  get dataset() {
    const el = this;
    return new Proxy({}, {
      get(t, k) { return typeof k === 'string' ? el.getAttribute('data-' + camel2kebab(k)) ?? undefined : undefined; },
      set(t, k, v) { el.setAttribute('data-' + camel2kebab(k), v); return true; },
      has(t, k) { return el.hasAttribute('data-' + camel2kebab(k)); }
    });
  }
  getAttribute(n) { return __n_get_attr(this.__h, String(n)); }
  setAttribute(n, v) { __n_set_attr(this.__h, String(n), String(v)); }
  removeAttribute(n) { __n_remove_attr(this.__h, String(n)); }
  hasAttribute(n) { return __n_get_attr(this.__h, String(n)) !== null; }
  toggleAttribute(n, f) { const h = this.hasAttribute(n); const w = f === undefined ? !h : f; if (w) this.setAttribute(n, ''); else this.removeAttribute(n); return w; }
  getAttributeNames() { return __n_attr_names(this.__h); }
  get attributes() { return this.getAttributeNames().map(n => ({name: n, value: this.getAttribute(n)})); }
  get innerHTML() { return __n_html_get(this.__h, 0); }
  set innerHTML(v) { __n_html_set(this.__h, String(v)); }
  get outerHTML() { return __n_html_get(this.__h, 1); }
  set outerHTML(v) { const p = this.parentNode; if (!p) return; const t = document.createElement('div'); t.innerHTML = v; for (const c of [...t.childNodes]) p.insertBefore(c, this); this.remove(); }
  get innerText() { return this.textContent; }
  set innerText(v) { this.textContent = v; }
  get nextElementSibling() { return wrap(__n_sibling(this.__h, 1, 1)); }
  get previousElementSibling() { return wrap(__n_sibling(this.__h, -1, 1)); }
  remove() { __n_remove(this.__h); }
  before(...n) { const p = this.parentNode; if (p) for (const x of n) p.insertBefore(toNode(x), this); }
  after(...n) { const p = this.parentNode; if (!p) return; const nx = this.nextSibling; for (const x of n) p.insertBefore(toNode(x), nx); }
  replaceWith(...n) { this.before(...n); this.remove(); }
  insertAdjacentHTML(pos, html) { const t = document.createElement('div'); t.innerHTML = html; const kids = [...t.childNodes]; this.__insertAdj(pos, kids); }
  insertAdjacentElement(pos, el) { this.__insertAdj(pos, [el]); return el; }
  insertAdjacentText(pos, s) { this.__insertAdj(pos, [document.createTextNode(s)]); }
  __insertAdj(pos, kids) {
    pos = pos.toLowerCase();
    if (pos === 'beforebegin') this.before(...kids);
    else if (pos === 'afterend') this.after(...kids);
    else if (pos === 'afterbegin') this.prepend(...kids);
    else this.append(...kids);
  }
  matches(s) { return __n_matches(this.__h, String(s)); }
  closest(s) { let e = this; while (e && e.nodeType === 1) { if (e.matches(s)) return e; e = e.parentElement; } return null; }
  getBoundingClientRect() { const r = __n_rect(this.__h); return {x: r.x, y: r.y, left: r.x, top: r.y, width: r.width, height: r.height, right: r.x + r.width, bottom: r.y + r.height}; }
  getClientRects() { return [this.getBoundingClientRect()]; }
  get offsetWidth() { return __n_rect(this.__h).width; }
  get offsetHeight() { return __n_rect(this.__h).height; }
  get offsetTop() { return __n_rect(this.__h).y; }
  get offsetLeft() { return __n_rect(this.__h).x; }
  get clientWidth() { return this.offsetWidth; }
  get clientHeight() { return this.offsetHeight; }
  get scrollWidth() { return this.offsetWidth; }
  get scrollHeight() { return this.offsetHeight; }
  get offsetParent() { return this.parentElement; }
  scrollTop = 0; scrollLeft = 0;
  scrollIntoView() {} scrollTo() {} scroll() {} focus() { document.activeElement = this; } blur() {}
  click() { __dispatch(this.__h, 'click'); }
  animate() { return {finished: Promise.resolve(), cancel() {}, play() {}, pause() {}, onfinish: null}; }
  attachShadow() { return this; }
  getAnimations() { return []; }
  requestFullscreen() { return Promise.resolve(); }
  setPointerCapture() {} releasePointerCapture() {} hasPointerCapture() { return false; }
}
class HTMLElement extends Element {
  get hidden() { return this.hasAttribute('hidden'); } set hidden(v) { this.toggleAttribute('hidden', !!v); }
  get title() { return this.getAttribute('title') || ''; } set title(v) { this.setAttribute('title', v); }
  get lang() { return this.getAttribute('lang') || ''; }
  get dir() { return this.getAttribute('dir') || ''; }
  get tabIndex() { return +(this.getAttribute('tabindex') || 0); } set tabIndex(v) { this.setAttribute('tabindex', v); }
}
const attrProp = (C, props) => { for (const p of props) Object.defineProperty(C.prototype, p, {get() { return this.getAttribute(p.toLowerCase()) || ''; }, set(v) { this.setAttribute(p.toLowerCase(), v); }, configurable: true}); };
const urlProp = (C, props) => { for (const p of props) Object.defineProperty(C.prototype, p, {get() { const v = this.getAttribute(p); return v === null ? '' : __resolve(v); }, set(v) { this.setAttribute(p, v); }, configurable: true}); };
class HTMLAnchorElement extends HTMLElement {
  get pathname() { return new URL(this.href).pathname; } get hostname() { return new URL(this.href).hostname; }
  get host() { return new URL(this.href).host; } get search() { return new URL(this.href).search; } get hash() { return new URL(this.href).hash; }
  get origin() { return new URL(this.href).origin; } get protocol() { return new URL(this.href).protocol; }
}
urlProp(HTMLAnchorElement, ['href']); attrProp(HTMLAnchorElement, ['target', 'rel', 'download']);
class HTMLImageElement extends HTMLElement { get complete() { return true; } get naturalWidth() { return this.offsetWidth; } get naturalHeight() { return this.offsetHeight; } decode() { return Promise.resolve(); } }
urlProp(HTMLImageElement, ['src']); attrProp(HTMLImageElement, ['alt', 'srcset', 'sizes', 'loading', 'width', 'height']);
class HTMLScriptElement extends HTMLElement { get text() { return this.textContent; } set text(v) { this.textContent = v; } }
urlProp(HTMLScriptElement, ['src']); attrProp(HTMLScriptElement, ['type', 'async', 'defer']);
class HTMLLinkElement extends HTMLElement {}
urlProp(HTMLLinkElement, ['href']); attrProp(HTMLLinkElement, ['rel', 'as', 'media']);
class HTMLInputElement extends HTMLElement {
  get value() { return __n_value_get(this.__h); } set value(v) { __n_value_set(this.__h, String(v)); }
  get checked() { return __n_checked(this.__h); } set checked(v) { __n_checked(this.__h, !!v); }
  get type() { return (this.getAttribute('type') || 'text').toLowerCase(); } set type(v) { this.setAttribute('type', v); }
  get disabled() { return this.hasAttribute('disabled'); } set disabled(v) { this.toggleAttribute('disabled', !!v); }
  get form() { return this.closest('form'); }
  select() {} setSelectionRange() {} checkValidity() { return true; } reportValidity() { return true; } setCustomValidity() {}
  get validity() { return {valid: true}; }
}
attrProp(HTMLInputElement, ['name', 'placeholder', 'autocomplete', 'min', 'max', 'step', 'pattern']);
class HTMLTextAreaElement extends HTMLInputElement {}
class HTMLSelectElement extends HTMLInputElement {
  get options() { return this.querySelectorAll('option'); }
  get selectedIndex() { const o = [...this.options]; const i = o.findIndex(x => x.hasAttribute('selected')); return i < 0 ? 0 : i; }
  set selectedIndex(i) { [...this.options].forEach((o, k) => o.toggleAttribute('selected', k === i)); }
  get value() { const o = this.options[this.selectedIndex]; return o ? (o.getAttribute('value') ?? o.textContent) : ''; }
}
class HTMLOptionElement extends HTMLElement { get value() { return this.getAttribute('value') ?? this.textContent; } get selected() { return this.hasAttribute('selected'); } set selected(v) { this.toggleAttribute('selected', !!v); } get text() { return this.textContent; } }
class HTMLButtonElement extends HTMLInputElement { get type() { return (this.getAttribute('type') || 'submit').toLowerCase(); } }
class HTMLFormElement extends HTMLElement {
  get elements() { return this.querySelectorAll('input,select,textarea,button'); }
  submit() { const q = new URLSearchParams(); for (const e of this.elements) if (e.name && e.type !== 'submit') q.append(e.name, e.value); location.assign((this.getAttribute('action') || location.pathname) + '?' + q); }
  requestSubmit() { if (this.dispatchEvent(new Event('submit', {cancelable: true}))) this.submit(); }
  reset() {}
}
attrProp(HTMLFormElement, ['method', 'name']); urlProp(HTMLFormElement, ['action']);
class HTMLTemplateElement extends HTMLElement { get content() { const f = document.createDocumentFragment(); f.innerHTML = this.innerHTML; for (const c of [...this.childNodes]) f.appendChild(c.cloneNode(true)); return f; } }
class HTMLCanvasElement extends HTMLElement { getContext() { return null; } toDataURL() { return ''; } }
class HTMLIFrameElement extends HTMLElement { get contentWindow() { return G.__frameProxy(this.__h); } get contentDocument() { return null; } }
urlProp(HTMLIFrameElement, ['src']); attrProp(HTMLIFrameElement, ['srcdoc', 'name', 'sandbox', 'allow', 'width', 'height']);
class HTMLMediaElement extends HTMLElement { play() { return Promise.resolve(); } pause() {} load() {} }
class HTMLVideoElement extends HTMLMediaElement {}
class HTMLAudioElement extends HTMLMediaElement {}
class HTMLDivElement extends HTMLElement {}
class HTMLSpanElement extends HTMLElement {}
class HTMLBodyElement extends HTMLElement {}
class HTMLHeadElement extends HTMLElement {}
class HTMLHtmlElement extends HTMLElement {}
class HTMLStyleElement extends HTMLElement {}
class HTMLMetaElement extends HTMLElement { get content() { return this.getAttribute('content') || ''; } get name() { return this.getAttribute('name') || ''; } }
class SVGElement extends Element {}
const tagClasses = {a: HTMLAnchorElement, img: HTMLImageElement, script: HTMLScriptElement, link: HTMLLinkElement, input: HTMLInputElement,
  textarea: HTMLTextAreaElement, select: HTMLSelectElement, option: HTMLOptionElement, button: HTMLButtonElement, form: HTMLFormElement,
  template: HTMLTemplateElement, canvas: HTMLCanvasElement, iframe: HTMLIFrameElement, video: HTMLVideoElement, audio: HTMLAudioElement,
  div: HTMLDivElement, span: HTMLSpanElement, body: HTMLBodyElement, head: HTMLHeadElement, html: HTMLHtmlElement, style: HTMLStyleElement,
  meta: HTMLMetaElement, svg: SVGElement, path: SVGElement, g: SVGElement, circle: SVGElement, rect: SVGElement};
Object.assign(G, {EventTarget, Event, CustomEvent, MouseEvent, KeyboardEvent, InputEvent, FocusEvent, PointerEvent, Node, Text, Comment, CharacterData,
  DocumentFragment, Element, HTMLElement, HTMLAnchorElement, HTMLImageElement, HTMLScriptElement, HTMLLinkElement, HTMLInputElement,
  HTMLTextAreaElement, HTMLSelectElement, HTMLOptionElement, HTMLButtonElement, HTMLFormElement, HTMLTemplateElement, HTMLCanvasElement,
  HTMLIFrameElement, HTMLVideoElement, HTMLAudioElement, HTMLMediaElement, HTMLDivElement, HTMLSpanElement, HTMLBodyElement,
  HTMLHeadElement, HTMLHtmlElement, HTMLStyleElement, HTMLMetaElement, SVGElement, DOMTokenList});

// ---- document
class Document extends ParentNodeMixin {
  get __h() { return __n_doc('root'); }
  get documentElement() { return wrap(__n_doc('html')); }
  get body() { return wrap(__n_doc('body')); }
  get head() { return wrap(__n_doc('head')); }
  get title() { return __title(); } set title(v) { __title(String(v)); }
  get readyState() { return G.__ready ? 'complete' : 'interactive'; }
  get location() { return location; } set location(v) { location.href = v; }
  get URL() { return location.href; } get documentURI() { return location.href; }
  get referrer() { return ''; } get domain() { return location.hostname; }
  get defaultView() { return G; }
  get visibilityState() { return 'visible'; } get hidden() { return false; }
  get characterSet() { return 'UTF-8'; } get compatMode() { return 'CSS1Compat'; }
  get currentScript() { return G.__cur_script; }
  get fonts() { return {ready: Promise.resolve(), add() {}, load() { return Promise.resolve([]); }, check() { return true; }}; }
  get scrollingElement() { return this.documentElement; }
  get forms() { return this.querySelectorAll('form'); } get images() { return this.querySelectorAll('img'); } get links() { return this.querySelectorAll('a[href]'); }
  get scripts() { return this.querySelectorAll('script'); }
  createElement(t) { return wrap(__n_create(String(t), 0)); }
  createElementNS(ns, t) { return this.createElement(t); }
  createTextNode(t) { return wrap(__n_create(String(t), 1)); }
  createComment(t) { return wrap(__n_create('', 1)); }
  createDocumentFragment() { return wrap(__n_create('', 2)); }
  createEvent() { return new Event(''); }
  createRange() { return {setStart() {}, setEnd() {}, selectNodeContents() {}, collapse() {}, getBoundingClientRect() { return {x: 0, y: 0, width: 0, height: 0}; }, createContextualFragment(h) { const f = document.createDocumentFragment(); const d = document.createElement('div'); d.innerHTML = h; for (const c of [...d.childNodes]) f.appendChild(c); return f; }}; }
  createTreeWalker(root) { return {currentNode: root, nextNode() { return null; }}; }
  getElementById(id) { return this.querySelector('#' + CSS.escape(String(id))); }
  getElementsByName(n) { return this.querySelectorAll('[name="' + n + '"]'); }
  hasFocus() { return true; }
  get cookie() { return G.__cookies || ''; } set cookie(v) { const kv = String(v).split(';')[0]; const k = kv.split('=')[0]; const o = (G.__cookies || '').split('; ').filter(x => x && !x.startsWith(k + '=')); o.push(kv); G.__cookies = o.join('; '); }
  write(...s) { const b = this.body; if (b) b.insertAdjacentHTML('beforeend', s.join('')); }
  writeln(...s) { this.write(...s, '\n'); }
  open() {} close() {}
  execCommand() { return false; }
  elementFromPoint() { return this.body; }
  getSelection() { return G.getSelection(); }
  exitFullscreen() { return Promise.resolve(); }
  activeElement = null;
}
const document = Object.create(Document.prototype);
G.document = document; G.Document = Document; G.HTMLDocument = Document;

// ---- location / history / navigator
function parseURL(u) {
  const m = /^([a-z][a-z0-9+.-]*:)?(\/\/([^\/?#:]*)(?::(\d+))?)?([^?#]*)(\?[^#]*)?(#.*)?$/i.exec(u) || [];
  return {protocol: m[1] || '', hostname: m[3] || '', port: m[4] || '', pathname: m[5] || '/', search: m[6] || '', hash: m[7] || ''};
}
class URLSearchParams {
  constructor(init) { this._p = []; if (typeof init === 'string') { init.replace(/^\?/, '').split('&').filter(Boolean).forEach(kv => { const i = kv.indexOf('='); const d = s => decodeURIComponent(s.replace(/\+/g, ' ')); this._p.push(i < 0 ? [d(kv), ''] : [d(kv.slice(0, i)), d(kv.slice(i + 1))]); }); } else if (init && typeof init === 'object') { for (const [k, v] of (Array.isArray(init) ? init : Object.entries(init))) this._p.push([String(k), String(v)]); } }
  get(k) { const e = this._p.find(p => p[0] === k); return e ? e[1] : null; }
  getAll(k) { return this._p.filter(p => p[0] === k).map(p => p[1]); }
  set(k, v) { this.delete(k); this._p.push([k, String(v)]); }
  append(k, v) { this._p.push([k, String(v)]); }
  delete(k) { this._p = this._p.filter(p => p[0] !== k); }
  has(k) { return this._p.some(p => p[0] === k); }
  forEach(f) { this._p.forEach(([k, v]) => f(v, k, this)); }
  keys() { return this._p.map(p => p[0])[Symbol.iterator](); } values() { return this._p.map(p => p[1])[Symbol.iterator](); }
  entries() { return this._p.map(p => [p[0], p[1]])[Symbol.iterator](); } [Symbol.iterator]() { return this.entries(); }
  get size() { return this._p.length; }
  toString() { return this._p.map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v)).join('&'); }
}
class URL {
  constructor(u, base) { this.href = __resolve(String(u), base === undefined ? undefined : String(base)); }
  get href() { return this._h; } set href(v) { this._h = v; const p = parseURL(v); Object.assign(this, {_p: p}); this.searchParams = new URLSearchParams(p.search); }
  get protocol() { return this._p.protocol; } get hostname() { return this._p.hostname; } get port() { return this._p.port; }
  get host() { return this._p.hostname + (this._p.port ? ':' + this._p.port : ''); } get pathname() { return this._p.pathname; }
  get search() { return this._p.search; } get hash() { return this._p.hash; } get origin() { return this._p.protocol + '//' + this.host; }
  toString() { return this.href; } toJSON() { return this.href; }
  static createObjectURL() { return 'blob:'; } static revokeObjectURL() {} static canParse() { return true; }
}
G.URL = URL; G.URLSearchParams = URLSearchParams;
const location = {
  get href() { return __url(); }, set href(v) { __navigate(String(v)); },
  get protocol() { return parseURL(__url()).protocol; }, get hostname() { return parseURL(__url()).hostname; },
  get host() { const p = parseURL(__url()); return p.hostname + (p.port ? ':' + p.port : ''); }, get port() { return parseURL(__url()).port; },
  get pathname() { return parseURL(__url()).pathname; }, get search() { return parseURL(__url()).search; },
  get hash() { return parseURL(__url()).hash; }, set hash(v) {}, get origin() { const p = parseURL(__url()); return p.protocol + '//' + p.hostname + (p.port ? ':' + p.port : ''); },
  assign(u) { __navigate(String(u)); }, replace(u) { __navigate(String(u)); }, reload() { __navigate(__url()); }, toString() { return __url(); }
};
G.location = location;
G.history = {length: 1, state: null, pushState(s) { this.state = s; }, replaceState(s) { this.state = s; }, back() {}, forward() {}, go() {}, scrollRestoration: 'auto'};
G.navigator = {userAgent: 'Mozilla/5.0 (X11; KleinOS x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36 Voyager/1.0',
  language: 'en-US', languages: ['en-US', 'en'], platform: 'Linux x86_64', vendor: 'KleinOS', cookieEnabled: true, onLine: true,
  hardwareConcurrency: 1, maxTouchPoints: 0, webdriver: false, doNotTrack: '1', userAgentData: {brands: [], mobile: false, platform: 'KleinOS'},
  clipboard: {writeText() { return Promise.resolve(); }, readText() { return Promise.resolve(''); }},
  sendBeacon() { return true; }, serviceWorker: undefined, mediaDevices: undefined, permissions: {query() { return Promise.resolve({state: 'denied'}); }},
  connection: {effectiveType: '4g', saveData: false}, storage: {estimate() { return Promise.resolve({}); }}};
G.screen = {width: 1280, height: 800, availWidth: 1280, availHeight: 760, colorDepth: 24, pixelDepth: 24, orientation: {type: 'landscape-primary'}};
Object.defineProperty(G, 'innerWidth', {get: () => __viewport()});
G.innerHeight = 640; G.outerWidth = 1280; G.outerHeight = 800; G.devicePixelRatio = 1;
G.scrollX = 0; G.scrollY = 0; G.pageXOffset = 0; G.pageYOffset = 0;
G.scrollTo = G.scroll = G.scrollBy = () => {};
G.isSecureContext = true; G.origin = location.origin; G.name = '';
G.alert = m => console.log('alert: ' + m); G.confirm = () => true; G.prompt = () => null;
G.focus = G.blur = G.print = G.stop = () => {};
G.open = () => null;
const makeFrameWin = (h, sibling) => ({closed: false, postMessage(d, o, t) {
  __post(h, JSON.stringify(d === undefined ? null : d), typeof o === 'string' ? o : (o && o.targetOrigin) || '/', wirePorts(transferOf(t || o)), sibling || ''); }});
const parentWin = makeFrameWin(-1);
const siblings = new Map();
// parent.frames[name]: the frames next to this one, addressed by name
parentWin.frames = new Proxy({}, {get(_, k) {
  if (typeof k !== 'string') return undefined;
  let w = siblings.get(k);
  if (!w) { w = makeFrameWin(-1, k); siblings.set(k, w); }
  return w; }});
G.frames = new Proxy(G, {get(t, k) {
  if (typeof k !== 'string') return t[k];
  const list = [...document.querySelectorAll('iframe')];
  if (k === 'length') return list.length;
  const el = /^\d+$/.test(k) ? list[+k] : list.find(f => f.name === k);
  return el ? el.contentWindow : t[k]; }});
const frameWins = new Map();
G.__frameProxy = h => { let w = frameWins.get(h); if (!w) { w = makeFrameWin(h); frameWins.set(h, w); } return w; };
G.__set_framed = name => { G.parent = parentWin; G.top = parentWin; G.name = name; };
const fireMessage = (data, origin, source, list) => { const ev = new Event('message'); ev.data = data; ev.origin = origin; ev.source = source; ev.ports = list; fireOn(G, ev); };
G.__on_message = (json, origin, h, w) => fireMessage(JSON.parse(json), origin, h === null ? parentWin : G.__frameProxy(h), adoptPorts(w));
G.postMessage = (d, o, t) => { const copy = JSON.stringify(d === undefined ? null : d), list = transferOf(t || o); setTimeout(() => fireMessage(JSON.parse(copy), location.origin, G, list), 0); };
G.getSelection = () => ({toString: () => '', rangeCount: 0, removeAllRanges() {}, addRange() {}});
G.getComputedStyle = el => { const s = __n_style(el.__h); s.getPropertyValue = p => s[p.replace(/-([a-z])/g, (m, c) => c.toUpperCase())] || ''; return s; };
G.matchMedia = q => { const m = /min-width:\s*(\d+)/.exec(q); const M = /max-width:\s*(\d+)/.exec(q);
  const matches = (!m || __viewport() >= +m[1]) && (!M || __viewport() <= +M[1]) && !/prefers-color-scheme:\s*dark|prefers-reduced-motion:\s*reduce/.test(q);
  return {matches, media: q, onchange: null, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {}}; };
const storage = () => { const d = new Map(); return {getItem: k => d.has(String(k)) ? d.get(String(k)) : null, setItem: (k, v) => d.set(String(k), String(v)), removeItem: k => d.delete(String(k)), clear: () => d.clear(), key: i => [...d.keys()][i] ?? null, get length() { return d.size; }}; };
G.localStorage = storage(); G.sessionStorage = storage();
G.console = {};
for (const lv of ['log', 'info', 'warn', 'error', 'debug', 'trace']) G.console[lv] = (...a) => __log(lv, a.map(x => { try { return typeof x === 'string' ? x : JSON.stringify(x); } catch (e) { return String(x); } }).join(' '));
G.console.group = G.console.groupEnd = G.console.groupCollapsed = G.console.time = G.console.timeEnd = G.console.count = G.console.table = G.console.assert = () => {};
G.performance = {now: () => __now(), timeOrigin: Date.now(), mark() {}, measure() {}, getEntriesByType: () => [], getEntriesByName: () => [], getEntries: () => [], clearMarks() {}, clearMeasures() {}};
G.crypto = {getRandomValues(a) { for (let i = 0; i < a.length; i++) a[i] = Math.floor(__random() * 4294967296); return a; },
  randomUUID() { const h = '0123456789abcdef'; let s = ''; for (let i = 0; i < 36; i++) s += (i === 8 || i === 13 || i === 18 || i === 23) ? '-' : i === 14 ? '4' : h[Math.floor(__random() * 16)]; return s; }, subtle: {}};
const b64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
G.btoa = s => { s = String(s); let o = ''; for (let i = 0; i < s.length; i += 3) { const n = (s.charCodeAt(i) << 16) | ((s.charCodeAt(i + 1) || 0) << 8) | (s.charCodeAt(i + 2) || 0);
  o += b64[n >> 18 & 63] + b64[n >> 12 & 63] + (i + 1 < s.length ? b64[n >> 6 & 63] : '=') + (i + 2 < s.length ? b64[n & 63] : '='); } return o; };
G.atob = s => { s = String(s).replace(/[^A-Za-z0-9+/]/g, ''); let o = ''; for (let i = 0; i < s.length; i += 4) { const n = (b64.indexOf(s[i]) << 18) | (b64.indexOf(s[i + 1]) << 12) | ((b64.indexOf(s[i + 2]) & 63) << 6) | (b64.indexOf(s[i + 3]) & 63);
  o += String.fromCharCode(n >> 16 & 255); if (s[i + 2]) o += String.fromCharCode(n >> 8 & 255); if (s[i + 3]) o += String.fromCharCode(n & 255); } return o; };
G.TextEncoder = class { encode(s) { const u = unescape(encodeURIComponent(s)); const a = new Uint8Array(u.length); for (let i = 0; i < u.length; i++) a[i] = u.charCodeAt(i); return a; } get encoding() { return 'utf-8'; } };
G.TextDecoder = class { decode(a) { if (!a) return ''; let s = ''; const b = a instanceof Uint8Array ? a : new Uint8Array(a.buffer || a); for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]); try { return decodeURIComponent(escape(s)); } catch (e) { return s; } } };
G.CSS = {supports: () => false, escape: s => String(s).replace(/([^a-zA-Z0-9_-])/g, '\\$1')};
G.structuredClone = v => JSON.parse(JSON.stringify(v));
class Observer { constructor(cb) { this.cb = cb; } observe() {} unobserve() {} disconnect() {} takeRecords() { return []; } }
G.MutationObserver = Observer; G.ResizeObserver = Observer; G.PerformanceObserver = Observer;
G.IntersectionObserver = class extends Observer { observe(el) { const cb = this.cb, self = this; setTimeout(() => cb([{isIntersecting: true, intersectionRatio: 1, target: el, boundingClientRect: el.getBoundingClientRect()}], self), 0); } };
G.customElements = {define() {}, get() { return undefined; }, whenDefined() { return Promise.resolve(); }};
G.Image = class { constructor() { return document.createElement('img'); } };
G.Blob = class { constructor(parts) { this._s = (parts || []).join(''); this.size = this._s.length; } text() { return Promise.resolve(this._s); } };
G.FormData = class { constructor(form) { this._p = new URLSearchParams(); if (form) for (const e of form.elements) if (e.name) this._p.append(e.name, e.value); } append(k, v) { this._p.append(k, v); } get(k) { return this._p.get(k); } set(k, v) { this._p.set(k, v); } has(k) { return this._p.has(k); } entries() { return this._p.entries(); } toString() { return this._p.toString(); } };
G.AbortController = class { constructor() { this.signal = {aborted: false, addEventListener() {}, removeEventListener() {}}; } abort() { this.signal.aborted = true; } };
G.AbortSignal = {timeout() { return {aborted: false, addEventListener() {}}; }, any() { return {aborted: false, addEventListener() {}}; }};
G.BroadcastChannel = class { postMessage() {} close() {} addEventListener() {} };
const adoptPorts = w => JSON.parse(w).map(([id, peer, q]) => { const p = new MessagePort(id, peer); p._q = q || []; return p; });
const wirePorts = list => JSON.stringify(list.filter(p => p instanceof MessagePort).map(p => { ports.delete(p._id); return [p._id, p._peer, p._q]; }));
const transferOf = t => Array.isArray(t) ? t : (t && t.transfer) || [];
const ports = new Map();
class MessagePort extends EventTarget {
  constructor(id, peer) { super(); this._id = id; this._peer = peer; this._q = []; this._on = false; this._om = null; ports.set(id, this); }
  get onmessage() { return this._om; } set onmessage(f) { this._om = f; this.start(); }
  start() { if (this._on) return; this._on = true; for (const m of this._q.splice(0)) setTimeout(() => this._fire(m[0], m[1]), 0); }
  close() { ports.delete(this._id); }
  _recv(j, w) { if (this._on) this._fire(j, w); else this._q.push([j, w]); }
  _fire(j, w) { const ev = new Event('message'); ev.data = JSON.parse(j); ev.ports = adoptPorts(w); ev.origin = ''; ev.source = null; fireOn(this, ev); }
  postMessage(d, t) {
    const j = JSON.stringify(d === undefined ? null : d), w = wirePorts(transferOf(t)), peer = ports.get(this._peer);
    if (peer) setTimeout(() => peer._recv(j, w), 0); else __post_port(this._peer, j, w);
  }
}
G.MessagePort = MessagePort;
G.MessageChannel = class { constructor() { const a = __port_id(), b = __port_id(); this.port1 = new MessagePort(a, b); this.port2 = new MessagePort(b, a); } };
G.__on_port_message = (id, j, w) => { const p = ports.get(id); if (!p) return false; p._recv(j, w); return true; };
G.Worker = class extends EventTarget {
  constructor(url) {
    super();
    const a = __port_id(), b = __port_id();
    this._p = new MessagePort(a, b);
    this._p._fire = (j, w) => { const ev = new Event('message'); ev.data = JSON.parse(j); ev.ports = adoptPorts(w); ev.origin = ''; ev.source = null; fireOn(this, ev); };
    this._p.start();
    this._id = b;
    __worker(__resolve(String(url)), b, a);
  }
  postMessage(d, t) { this._p.postMessage(d, t); }
  terminate() { __worker('', this._id, ''); this._p.close(); }
};
G.__set_worker = (id, peer) => {
  const p = new MessagePort(id, peer);
  p._fire = (j, w) => { const ev = new Event('message'); ev.data = JSON.parse(j); ev.ports = adoptPorts(w); ev.origin = ''; ev.source = null; fireOn(G, ev); };
  p.start();
  G.postMessage = (d, t) => p.postMessage(d, t);
  G.importScripts = (...urls) => { for (const u of urls) __import_script(String(u)); };
  G.close = () => {};
  delete G.window; delete G.document; delete G.parent; delete G.top; delete G.frames;
};
G.WebSocket = class { constructor() { setTimeout(() => this.onerror && this.onerror(new Event('error')), 0); } send() {} close() {} addEventListener() {} };
G.EventSource = G.WebSocket;
G.indexedDB = undefined;
G.requestIdleCallback = (f) => setTimeout(() => f({didTimeout: false, timeRemaining: () => 10}), 1);
G.cancelIdleCallback = id => clearTimeout(id);

// ---- timers
const timers = new Map();
let timerSeq = 1;
G.setTimeout = (fn, ms, ...args) => { const id = timerSeq++; timers.set(id, {fn, args, at: __now() + (+ms || 0), every: 0}); return id; };
G.setInterval = (fn, ms, ...args) => { const id = timerSeq++; const every = Math.max(10, +ms || 0); timers.set(id, {fn, args, at: __now() + every, every}); return id; };
G.clearTimeout = G.clearInterval = id => { timers.delete(id); };
let rafs = []; let rafSeq = 1;
G.requestAnimationFrame = fn => { const id = rafSeq++; rafs.push({id, fn}); return id; };
G.cancelAnimationFrame = id => { rafs = rafs.filter(r => r.id !== id); };
G.queueMicrotask = fn => Promise.resolve().then(fn);
G.__tick = now => {
  const due = [];
  for (const [id, t] of timers) if (t.at <= now) due.push([id, t]);
  due.sort((a, b) => a[1].at - b[1].at);
  for (const [id, t] of due) {
    if (t.every) t.at = now + t.every; else timers.delete(id);
    try { typeof t.fn === 'function' ? t.fn(...t.args) : (0, eval)(String(t.fn)); } catch (e) { console.error('timer: ' + e + (e && e.stack ? '\n' + e.stack : '')); }
  }
  if (rafs.length) { const r = rafs; rafs = []; for (const x of r) { try { x.fn(now); } catch (e) { console.error('raf: ' + e); } } }
};

// ---- fetch / XHR
const pending = new Map();
class Headers {
  constructor(init) { this._h = {}; if (init) for (const [k, v] of (init instanceof Headers ? Object.entries(init._h) : Array.isArray(init) ? init : Object.entries(init))) this._h[k.toLowerCase()] = String(v); }
  get(k) { return this._h[String(k).toLowerCase()] ?? null; } set(k, v) { this._h[String(k).toLowerCase()] = String(v); } has(k) { return String(k).toLowerCase() in this._h; }
  append(k, v) { this.set(k, v); } delete(k) { delete this._h[String(k).toLowerCase()]; } forEach(f) { Object.entries(this._h).forEach(([k, v]) => f(v, k)); }
  entries() { return Object.entries(this._h)[Symbol.iterator](); } [Symbol.iterator]() { return this.entries(); }
}
class Response {
  constructor(body, init) { this._b = body == null ? '' : String(body); this.status = (init && init.status) || 200; this.ok = this.status >= 200 && this.status < 300; this.statusText = ''; this.headers = new Headers(init && init.headers); this.url = (init && init.url) || ''; this.redirected = false; this.type = 'basic'; this.bodyUsed = false; }
  text() { return Promise.resolve(this._b); }
  json() { return new Promise((res, rej) => { try { res(JSON.parse(this._b)); } catch (e) { rej(e); } }); }
  arrayBuffer() { return new TextEncoder().encode(this._b).buffer && Promise.resolve(new TextEncoder().encode(this._b).buffer); }
  blob() { return Promise.resolve(new Blob([this._b])); }
  clone() { return new Response(this._b, {status: this.status, headers: this.headers._h, url: this.url}); }
}
class Request { constructor(u, o) { this.url = typeof u === 'string' ? u : u.url; this.method = (o && o.method) || 'GET'; this.headers = new Headers(o && o.headers); this.body = o && o.body; } }
G.Headers = Headers; G.Response = Response; G.Request = Request;
G.fetch = (input, opts) => new Promise((resolve, reject) => {
  const url = typeof input === 'string' ? input : input instanceof URL ? input.href : input.url;
  const method = ((opts && opts.method) || (input && input.method) || 'GET').toUpperCase();
  const body = opts && opts.body !== undefined ? String(opts.body) : null;
  const id = __fetch(String(url), method, body);
  pending.set(id, {resolve, reject, url});
});
G.__fetch_done = (id, status, text, ctype) => {
  const p = pending.get(id); if (!p) return; pending.delete(id);
  if (status <= 0) { if (p.reject) p.reject(new TypeError('Failed to fetch')); if (p.xhr) p.xhr(0, ''); return; }
  if (p.xhr) { p.xhr(status, text, ctype); return; }
  p.resolve(new Response(text, {status, headers: {'content-type': ctype}, url: p.url}));
};
G.XMLHttpRequest = class extends EventTarget {
  constructor() { super(); this.readyState = 0; this.status = 0; this.responseText = ''; this.response = ''; this.responseType = ''; this._h = {}; this.withCredentials = false; this.timeout = 0; this.upload = {addEventListener() {}}; }
  open(m, u) { this._m = m; this._u = u; this.readyState = 1; }
  setRequestHeader(k, v) { this._h[k] = v; }
  getResponseHeader(k) { return k.toLowerCase() === 'content-type' ? this._ct || null : null; }
  getAllResponseHeaders() { return this._ct ? 'content-type: ' + this._ct + '\r\n' : ''; }
  overrideMimeType() {}
  abort() {}
  send(body) {
    const id = __fetch(String(this._u), this._m || 'GET', body == null ? null : String(body));
    pending.set(id, {xhr: (status, text, ct) => {
      this.status = status; this.responseText = text; this._ct = ct; this.readyState = 4;
      this.response = this.responseType === 'json' ? (() => { try { return JSON.parse(text); } catch (e) { return null; } })() : text;
      const fire = t => { const ev = new Event(t); fireOn(this, ev); };
      fire('readystatechange'); fire(status ? 'load' : 'error'); fire('loadend');
    }});
  }
};
G.XMLHttpRequest.DONE = 4;

// ---- event dispatch from the browser
G.__dispatch = (h, type) => {
  const target = wrap(h);
  const ev = type === 'click' ? new MouseEvent('click', {bubbles: true, cancelable: true}) : new Event(type, {bubbles: true, cancelable: true});
  ev.target = target;
  let n = target;
  while (n && !ev._stop) { fireOn(n, ev); n = n.parentNode === document ? null : n.parentNode; }
  if (!ev._stop) { fireOn(document, ev); fireOn(G, ev); }
  return ev.defaultPrevented;
};
G.__set_current_script = h => { G.__cur_script = wrap(h); };
G.__clear_current_script = () => { G.__cur_script = null; };
G.__fire_node = (h, type) => { const el = wrap(h); const ev = new Event(type); ev.target = el; fireOn(el, ev); };
G.__fire_loaded = () => {
  G.__cur_script = null;
  fireOn(document, new Event('readystatechange'));
  fireOn(document, new Event('DOMContentLoaded', {bubbles: true}));
  G.__ready = true;
  fireOn(document, new Event('readystatechange'));
  fireOn(G, new Event('load'));
  fireOn(G, new Event('pageshow'));
};
G.addEventListener = EventTarget.prototype.addEventListener;
G.removeEventListener = EventTarget.prototype.removeEventListener;
G.dispatchEvent = function (ev) { fireOn(G, ev); return !ev.defaultPrevented; };
})();
)JS";
}  // namespace

}  // namespace html
