// Voyager — the KleinOS web browser (uses HTMLEngine.dl + the kernel HTTP/TLS stack).
#include "lib/log.h"
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "net/http.h"
#include "net/net.h"
#include "arch/interrupts.h"
#include "lib/math.h"
#include "HTMLEngine/html.h"
#include "HTMLEngine/js.h"
#if __has_include("HTMLEngine/svg.cpp")
#include "HTMLEngine/svg.h"
#define HAVE_SVG 1
#endif
#include "boot/multiboot2.h"
#include "fs/vfs.h"

namespace {

constexpr int kToolbar = 50;

const char* kStartPage = R"(<html><head><title>Start Page</title><style>
body { background: #f4f5fa; font-family: sans-serif; margin: 0; }
.wrap { max-width: 760px; margin: 0 auto; padding: 40px 20px; }
h1 { font-size: 34px; color: #1c1c1e; margin: 10px 0 4px 0; }
.sub { color: #6e6e78; font-size: 15px; margin-bottom: 28px; }
.grid { display: flex; }
.tile { background: #ffffff; border: 1px solid #e3e4ea; border-radius: 14px; padding: 14px 16px; margin: 6px; width: 200px; }
.tile a { color: #1c1c1e; text-decoration: none; font-weight: bold; font-size: 15px; }
.tile .d { color: #7a7a85; font-size: 13px; }
h2 { font-size: 17px; color: #3a3a40; margin-top: 30px; }
.note { color: #8a8a92; font-size: 13px; margin-top: 34px; }
</style></head><body><div class="wrap">
<h1>Voyager</h1><div class="sub">The KleinOS web browser. Type an address or a search above.</div>
<h2>Favorites</h2>
<div class="grid">
<div class="tile"><a href="https://en.wikipedia.org/wiki/Special:Random">Wikipedia</a><div class="d">Random article</div></div>
<div class="tile"><a href="https://news.ycombinator.com/">Hacker News</a><div class="d">Tech news</div></div>
<div class="tile"><a href="https://lite.duckduckgo.com/lite/">DuckDuckGo Lite</a><div class="d">Web search</div></div>
</div><div class="grid">
<div class="tile"><a href="http://frogfind.com/">FrogFind</a><div class="d">Search for simple browsers</div></div>
<div class="tile"><a href="https://text.npr.org/">NPR Text</a><div class="d">News, text only</div></div>
<div class="tile"><a href="https://example.com/">example.com</a><div class="d">Test page</div></div>
</div><div class="grid">
<div class="tile"><a href="http://info.cern.ch/hypertext/WWW/TheProject.html">The first website</a><div class="d">CERN, 1991</div></div>
<div class="tile"><a href="https://wttr.in/?format=4">Weather</a><div class="d">wttr.in</div></div>
<div class="tile"><a href="https://lite.cnn.com/">CNN Lite</a><div class="d">Headlines</div></div>
</div>
<div class="note">Voyager renders HTML and CSS with HTMLEngine.dl. JavaScript runs on QuickJS.
HTTPS uses TLS 1.3 (X25519, AES-128-GCM); certificates are not verified.</div>
</div></body></html>)";

String error_page(const char* url, const char* err) {
    return String::format(
        "<html><head><title>Can't open page</title></head><body style=\"background:#f4f5fa\">"
        "<div style=\"max-width:600px;margin:80px auto;text-align:center\">"
        "<h1 style=\"color:#1c1c1e\">Voyager can't open this page</h1>"
        "<p style=\"color:#555\">%s</p><p style=\"color:#888;font-size:13px\">%s</p>"
        "</div></body></html>", err, url);
}

const void* memmem_s(const uint8_t* hay, size_t n, const char* needle) {
    size_t m = strlen(needle);
    size_t lim = n < 512 ? n : 512;
    for (size_t i = 0; i + m <= lim; i++) if (!memcmp(hay + i, needle, m)) return hay + i;
    return nullptr;
}

struct Pending {
    net::HttpRequest* req;
    String url;
    int kind;   // 0 page, 1 image, 2 stylesheet, 3 script (tag = index), 4 js fetch (tag = id)
    int tag = 0;
};

struct Frame {
    Frame* parent = nullptr;      // nullptr = child of the main page
    html::Node* node = nullptr;   // its <iframe>, in the parent's document
    html::Document* doc = nullptr;
    html::JsEnv* js = nullptr;
    bool js_started = false, load_fired = true;
    Vector<Pending> pending;
    String url, src_attr;
    int w = 300, h = 150, scroll = 0, depth = 1;
    bool worker = false;          // a Web Worker: no element on screen, node is a placeholder
    String worker_port, worker_peer;
};

const char kFrameBlocked[] = "<html><body style=\"font:13px sans-serif;color:#777;text-align:center;padding:16px\">This content can't be shown in a frame.</body></html>";
const char kFrameFailed[] = "<html><body style=\"font:13px sans-serif;color:#777;text-align:center;padding:16px\">This content could not be loaded.</body></html>";

class BrowserWindow : public Window {
public:
    BrowserWindow() : Window("Voyager", 1040, 680) {
        set_min_size(480, 300);
        address_ = (TextField*)add(new TextField(Rect(0, 0, 10, 32), "Search or enter website name"));
        address_->rounded = true;
        address_->search_icon = true;
        address_->on_submit = [this] { go(address_->text.c_str()); focus_widget(nullptr); };
        layout_toolbar();
        set_ticking(true);
        char boot_url[256];
        static bool used_boot_url;
        if (!used_boot_url && boot_arg("url", boot_url, sizeof(boot_url))) { used_boot_url = true; navigate(boot_url, true); }
        else navigate("klein://start", true);
    }
    ~BrowserWindow() override {
        destroy_all_frames();
        for (Pending& p : pending_) net::http_free(p.req);
        html::js_destroy(js_);
        delete doc_;
    }

    // ------------------------------------------------------------ navigation
    void go(const char* input) {
        String s = String(input).trim();
        if (s.empty()) return;
        bool looks_url = s.find("://") >= 0 || (s.find('.') > 0 && s.find(' ') < 0) || s.starts_with("localhost") || s.starts_with("klein:");
        if (!looks_url) {
            String q;
            for (size_t i = 0; i < s.size(); i++) {
                char c = s[i];
                if (c == ' ') q += '+';
                else if (isalnum(c) || c == '-' || c == '_' || c == '.') q += c;
                else q += String::format("%%%02X", (uint8_t)c);
            }
            s = String("https://lite.duckduckgo.com/lite/?q=") + q;
        } else if (s.starts_with("/")) {
            s = String("file://") + s;
        } else if (s.find("://") < 0 && !s.starts_with("klein:")) {
            s = String("https://") + s;
        }
        navigate(s.c_str(), true);
    }

    void navigate(const char* url, bool push_history) {
        cancel_all();
        if (push_history) {
            while (hist_pos_ + 1 < (int)history_.size()) history_.pop();
            history_.push(String(url));
            hist_pos_ = (int)history_.size() - 1;
        }
        current_url_ = url;
        address_->set_text(url);
        scroll_ = 0;
        hover_.clear();
        if (!strncmp(url, "klein:", 6)) {
            show_html(kStartPage, strlen(kStartPage), "klein://start");
            address_->set_text("");
            return;
        }
        if (!strncmp(url, "file://", 7)) {
            String text;
            if (vfs::read_text(url + 7, text)) {
                String ext = vfs::extension(url + 7);
                show_html(text.c_str(), text.size(), url, !(ext == "html" || ext == "htm"));
            } else {
                String e = error_page(url, "The file could not be found.");
                show_html(e.c_str(), e.size(), url);
            }
            return;
        }
        loading_ = true;
        progress_ = 0.05f;
        Pending p{net::http_get(url), String(url), 0};
        pending_.push(p);
        invalidate();
    }

    void cancel_all() {
        for (Pending& p : pending_) net::http_free(p.req);
        pending_.clear();
        loading_ = false;
    }

    void show_html(const char* src, size_t len, const char* url, bool plain = false) {
        // drop requests belonging to the previous document
        for (size_t i = 0; i < pending_.size();) {
            if (pending_[i].kind != 0) { net::http_free(pending_[i].req); pending_.remove(i); } else i++;
        }
        destroy_all_frames();
        html::js_destroy(js_);
        js_ = nullptr;
        js_started_ = false;
        delete doc_;
        html::set_viewport(kmax(200, width() - 12), view_h());
        doc_ = plain ? html::parse_text(src, len, url) : html::parse(src, len, url, js_enabled_ && strncmp(url, "klein:", 6) != 0);
        if (!plain && strncmp(url, "klein:", 6) != 0 && js_enabled_) {
            js_ = html::js_create(doc_);
            Vector<html::ScriptInfo>& sc = html::js_scripts(js_);
            for (int i = 0; i < (int)sc.size(); i++)
                if (!sc[i].src.empty()) pending_.push(Pending{net::http_get(sc[i].src.c_str()), sc[i].src, 3, i});
        }
        set_title(doc_->title.empty() ? url : doc_->title.c_str());
        relayout();
        // anchor
        const char* hash = strchr(url, '#');
        if (hash) { int y = html::anchor_y(doc_, hash + 1); if (y >= 0) scroll_ = y; }
        for (String& css : doc_->stylesheet_urls) pending_.push(Pending{net::http_get(css.c_str()), css, 2});
        invalidate();
    }

    void relayout() {
        if (!doc_) return;
        doc_->viewport_w = kmax(200, width() - 12);
        doc_->viewport_h = view_h();
        html::layout(doc_, kmax(200, width() - 12));
        int maxs = kmax(0, doc_->height - view_h());
        scroll_ = kclamp(scroll_, 0, maxs);
        need_images_ = true;
        sync_frames(nullptr);
    }

    // ------------------------------------------------------------ iframes
    // Each frame owns its document and JavaScript context, so pages only reach
    // each other through postMessage. Frames load http(s) only: never file: or klein:.
    static constexpr int kMaxWorkers = 4, kMaxFrames = 8, kMaxFrameDepth = 3, kPostPatience = 900;   // ticks a message waits for its frame

    int visible_frames() { int n = 0; for (Frame* fr : frames_) if (!fr->worker) n++; return n; }
    html::Document* owner_doc(Frame* fr) { return fr->parent ? fr->parent->doc : doc_; }
    html::JsEnv* owner_js(Frame* fr) { return fr->parent ? fr->parent->js : js_; }
    bool owner_started(Frame* fr) { return fr->parent ? fr->parent->js_started : js_started_; }
    void main_viewport() { html::set_viewport(kmax(200, width() - 12), view_h()); }

    Frame* frame_for(Frame* owner, html::Node* n) {
        for (Frame* fr : frames_) if (fr->parent == owner && fr->node == n) return fr;
        return nullptr;
    }

    static String url_origin(const String& url) {
        long s = url.find("://");
        if (s < 0) return url;
        long e = s + 3;
        while (e < (long)url.size() && url[e] != '/' && url[e] != '?' && url[e] != '#') e++;
        String origin = url.substr(0, e).lower();
        // the default port is the same origin: https://a.com:443 == https://a.com
        if (origin.starts_with("https:") && origin.ends_with(":443")) origin = origin.substr(0, origin.size() - 4);
        else if (origin.starts_with("http:") && origin.ends_with(":80")) origin = origin.substr(0, origin.size() - 3);
        return origin;
    }

    String frame_target(html::Document* owner, html::Node* n) {
        if (n->attr("srcdoc")) return String("about:srcdoc");
        const char* s = n->attr("src");
        if (!s || !*s) return String("about:blank");
        if (!strncmp(s, "about:", 6)) return String(s);
        char abs[2300];
        net::url_resolve(owner->url.c_str(), s, abs, sizeof(abs));
        return String(abs);
    }

    void reset_frame(Frame* fr) {
        for (bool again = true; again;) {
            again = false;
            for (Frame* c : frames_) if (c->parent == fr) { destroy_frame(c); again = true; break; }
        }
        for (Pending& p : fr->pending) net::http_free(p.req);
        fr->pending.clear();
        html::js_destroy(fr->js);
        fr->js = nullptr;
        fr->js_started = false;
        fr->node->content_doc = nullptr;
        fr->node->content_scroll = 0;
        fr->scroll = 0;
        delete fr->doc;
        fr->doc = nullptr;
    }

    void destroy_frame(Frame* fr) {
        reset_frame(fr);
        if (focus_frame_ == fr) focus_frame_ = nullptr;
        frames_.remove_value(fr);
        if (fr->worker) delete fr->node;
        delete fr;
    }

    void destroy_all_frames() {
        while (!frames_.empty()) destroy_frame(frames_[0]);
    }

    void load_frame(Frame* fr, const String& url) {
        reset_frame(fr);
        fr->url = url;
        fr->load_fired = true;
        html::Document* owner = owner_doc(fr);
        if (url == "about:srcdoc") {
            const char* s = fr->node->attr("srcdoc");
            show_frame(fr, s, strlen(s), owner->url.c_str());
        } else if (url.starts_with("about:")) {
            show_frame(fr, "<html></html>", 13, owner->url.c_str());
        } else if (url.starts_with("http://") || url.starts_with("https://")) {
            fr->pending.push(Pending{net::http_get(url.c_str()), url, 0});
        } else {
            klog("browser: blocked frame url %.200s", url.c_str());
            show_frame(fr, kFrameBlocked, strlen(kFrameBlocked), owner->url.c_str());
        }
    }

    void show_frame(Frame* fr, const char* src, size_t len, const char* url) {
        reset_frame(fr);
        const char* sb = fr->node->attr("sandbox");
        bool scripting = js_enabled_ && (!sb || strstr(sb, "allow-scripts"));
        html::set_viewport(fr->w, fr->h);
        klog("frames: show %.120s (depth %d, %dx%d, name '%.30s')", url, fr->depth, fr->w, fr->h, fr->node->attr("name") ? fr->node->attr("name") : "");
        fr->doc = html::parse(src, len, url, scripting);
        fr->node->content_doc = fr->doc;
        if (scripting) {
            fr->js = html::js_create(fr->doc);
            html::js_set_framed(fr->js, fr->node->attr("name"));
            Vector<html::ScriptInfo>& sc = html::js_scripts(fr->js);
            for (int i = 0; i < (int)sc.size(); i++)
                if (!sc[i].src.empty()) fr->pending.push(Pending{net::http_get(sc[i].src.c_str()), sc[i].src, 3, i});
        }
        for (String& css : fr->doc->stylesheet_urls) fr->pending.push(Pending{net::http_get(css.c_str()), css, 2});
        fr->load_fired = false;
        layout_frame(fr);
    }

    void layout_frame(Frame* fr) {
        if (!fr->doc) return;
        html::set_viewport(fr->w, fr->h);
        fr->doc->viewport_w = fr->w;
        fr->doc->viewport_h = fr->h;
        html::layout(fr->doc, kmax(1, fr->w));
        fr->scroll = kclamp(fr->scroll, 0, kmax(0, fr->doc->height - fr->h));
        fr->node->content_scroll = fr->scroll;
        sync_frames(fr);
    }

    static void collect_iframes(html::Node* n, Vector<html::Node*>& out) {
        if (n->type == html::Node::Element && n->tag == "iframe") out.push(n);
        for (html::Node* c : n->children) collect_iframes(c, out);
    }

    // Match live frames to the <iframe> elements now in a document.
    void sync_frames(Frame* owner) {
        html::Document* d = owner ? owner->doc : doc_;
        if (!d || !d->root) return;
        Vector<html::Node*> found;
        collect_iframes(d->root, found);
        for (bool again = true; again;) {
            again = false;
            for (Frame* fr : frames_)
                if (fr->parent == owner && !fr->worker && !found.contains(fr->node)) { destroy_frame(fr); again = true; break; }
        }
        int depth = owner ? owner->depth + 1 : 1;
        for (html::Node* n : found) {
            String target = frame_target(d, n);
            Frame* fr = frame_for(owner, n);
            if (!fr) {
                if (depth > kMaxFrameDepth || visible_frames() >= kMaxFrames) continue;
                fr = new Frame();
                fr->parent = owner;
                fr->node = n;
                fr->depth = depth;
                fr->src_attr = target;
                frames_.push(fr);
                load_frame(fr, target);
            } else if (fr->src_attr != target) {
                fr->src_attr = target;
                load_frame(fr, target);
            }
        }
        for (html::Fragment& f : d->frags) {
            if (f.kind != html::Fragment::Frame) continue;
            Frame* fr = frame_for(owner, f.node);
            if (fr && (fr->w != f.r.w || fr->h != f.r.h)) {
                fr->w = f.r.w;
                fr->h = f.r.h;
                layout_frame(fr);
            }
        }
        main_viewport();
    }

    // X-Frame-Options / CSP frame-ancestors: the embedded site decides who may frame it.
    bool frame_allowed(Frame* fr, net::HttpRequest* req) {
        String parent = url_origin(owner_doc(fr)->url);
        String self = url_origin(String(net::http_final_url(req)));
        if (const char* xfo = net::http_header(req, "x-frame-options")) {
            String v = String(xfo).trim().lower();
            if (v == "deny" || (v == "sameorigin" && parent != self)) return false;
        }
        const char* csp = net::http_header(req, "content-security-policy");
        if (!csp) return true;
        String policy = String(csp).lower();
        long at = policy.find("frame-ancestors");
        if (at < 0) return true;
        String rest = policy.substr(at + 15);
        long semi = rest.find(';');
        if (semi >= 0) rest = rest.substr(0, semi);
        rest = rest.trim();
        if (rest.find("'none'") >= 0) return false;
        if (rest.find("*") == 0 || rest.find(" * ") >= 0) return true;
        if (rest.find("'self'") >= 0 && parent == self) return true;
        long host = parent.find("://");
        String name = host >= 0 ? parent.substr(host + 3) : parent;
        return rest.find(name.c_str()) >= 0 || rest.find(parent.substr(0, host + 1).c_str()) >= 0;
    }

    void request_frame_images(Frame* fr) {
        int inflight = 0;
        bool decoded = false;
        for (Pending& p : fr->pending) if (p.kind == 1) inflight++;
        for (html::Image& im : html::images(fr->doc)) {
            if (inflight >= 4) break;
            if (im.requested || im.url.empty()) continue;
            im.requested = true;
            if (im.url.starts_with("data:")) {
                im.canvas = decode_data_url(im);
                im.failed = !im.canvas;
                decoded |= im.canvas != nullptr;
                continue;
            }
            fr->pending.push(Pending{net::http_get(im.url.c_str(), "Accept: image/png,image/jpeg,image/gif,image/svg+xml,*/*;q=0.1\r\n"), im.url, 1});
            inflight++;
        }
        if (decoded) layout_frame(fr);
    }

    bool pump_frame(Frame* fr) {
        bool changed = false, relayout_needed = false;
        html::set_viewport(fr->w, fr->h);
        for (size_t i = 0; i < fr->pending.size();) {
            Pending p = fr->pending[i];
            net::HttpState st = net::http_state(p.req);
            if (st != net::HttpState::Done && st != net::HttpState::Failed) { i++; continue; }
            fr->pending.remove(i);
            size_t len = 0;
            const uint8_t* body = st == net::HttpState::Done ? net::http_body(p.req, &len) : nullptr;
            if (p.kind == 0) {
                String final_url = net::http_final_url(p.req);
                if (fr->worker) {
                    if (st == net::HttpState::Done && net::http_status(p.req) < 400) show_worker(fr, (const char*)body, len, final_url.c_str());
                    else klog("browser: worker script failed: %.160s", final_url.c_str());
                }
                else if (st != net::HttpState::Done) show_frame(fr, kFrameFailed, strlen(kFrameFailed), final_url.c_str());
                else if (!frame_allowed(fr, p.req)) show_frame(fr, kFrameBlocked, strlen(kFrameBlocked), owner_doc(fr)->url.c_str());
                else show_frame(fr, (const char*)body, len, final_url.c_str());
                fr->url = final_url;
                net::http_free(p.req);
                i = 0;
                changed = true;
                continue;
            }
            if (p.kind == 1 && fr->doc) {
                html::Image* target = nullptr;
                for (html::Image& im : html::images(fr->doc)) if (im.url == p.url) target = &im;
                if (target && st != net::HttpState::Done && target->retries < 2) { target->retries++; target->requested = false; }
                else if (target) {
                    const char* ct = net::http_header(p.req, "content-type");
                    bool is_svg = target->svg || (ct && strstr(ct, "svg")) || (len > 5 && memmem_s(body, len, "<svg") != nullptr);
                    target->canvas = body ? decode_image(*target, body, len, is_svg) : nullptr;
                    target->failed = !target->canvas;
                    relayout_needed |= target->canvas != nullptr;
                }
            } else if (p.kind == 2 && fr->doc && st == net::HttpState::Done) {
                html::add_stylesheet(fr->doc, (const char*)body, len);
                relayout_needed = true;
            } else if (p.kind == 3 && fr->js) {
                Vector<html::ScriptInfo>& sc = html::js_scripts(fr->js);
                if (p.tag < (int)sc.size()) {
                    html::ScriptInfo& si = sc[p.tag];
                    si.fetched = true;
                    if (body && len && len < 4 * 1024 * 1024 && net::http_status(p.req) < 400) si.node->value = String((const char*)body, len);
                    else si.failed = true;
                }
            } else if (p.kind == 4 && fr->js) {
                html::js_fetch_done(fr->js, p.tag, st == net::HttpState::Done ? net::http_status(p.req) : 0, body, len, net::http_header(p.req, "content-type"));
            }
            net::http_free(p.req);
            changed = true;
        }
        if (fr->js) {
            bool ready = true;
            if (!fr->js_started) for (html::ScriptInfo& si : html::js_scripts(fr->js)) if (!si.fetched && !si.failed) ready = false;
            if (ready) {
                if (!fr->js_started) { fr->js_started = true; html::js_run_scripts(fr->js); html::restyle(fr->doc); relayout_needed = true; }
                if (html::js_tick(fr->js, timer_ms())) { html::restyle(fr->doc); relayout_needed = true; }
                fetch_new_scripts(fr->js, fr->pending);
                start_workers(fr->js, fr);
                for (html::JsFetch& f : html::js_fetches(fr->js)) {
                    if (f.started) continue;
                    f.started = true;
                    net::HttpRequest* r = f.method == "POST" ? net::http_post(f.url.c_str(), "application/x-www-form-urlencoded", f.body.c_str(), f.body.size())
                                                             : net::http_get(f.url.c_str());
                    fr->pending.push(Pending{r, f.url, 4, f.id});
                }
                String nav = html::js_take_navigation(fr->js);
                if (!nav.empty()) { load_frame(fr, nav); return true; }
            }
        }
        if (relayout_needed) { layout_frame(fr); changed = true; }
        if (fr->doc) request_frame_images(fr);
        bool scripts_pending = false;
        for (Pending& p : fr->pending) if (p.kind == 3) scripts_pending = true;
        if (!fr->worker && fr->doc && !fr->load_fired && !scripts_pending && (!fr->js || fr->js_started)) {
            html::JsEnv* env = owner_js(fr);
            if (!env) fr->load_fired = true;
            else if (owner_started(fr)) { fr->load_fired = true; html::js_fire_event(env, fr->node, "load"); changed = true; }
        }
        return changed;
    }

    void route_messages(html::JsEnv* env, Frame* sender) {
        if (!env) return;
        Vector<html::JsPost>& posts = html::js_posts(env);
        String from = url_origin(sender ? sender->doc->url : doc_->url);
        for (size_t i = 0; i < posts.size();) {
            html::JsPost m = posts[i];
            if (!m.port.empty()) {
                bool held = js_ && js_started_ && html::js_deliver_port(js_, m.port, m.data, m.ports);
                for (size_t k = 0; !held && k < frames_.size(); k++)
                    if (frames_[k]->js && frames_[k]->js_started) held = html::js_deliver_port(frames_[k]->js, m.port, m.data, m.ports);
                // its holder may still be starting (a worker loading, a port in transit)
                if (!held && posts[i].waited++ < kPostPatience) { i++; continue; }
                klog("frames: port msg %.60s %s", m.data.c_str(), held ? "delivered" : "DROPPED (no holder)");
                posts.remove(i);
                continue;
            }
            Frame* dest = nullptr;
            html::JsEnv* to = nullptr;
            String to_url;
            html::Node* source = nullptr;
            bool may_arrive = false;   // its frame is still loading, or hasn't been created yet
            if (m.target || !m.sibling.empty()) {
                if (m.target) dest = frame_for(sender, m.target);
                else if (sender)
                    for (Frame* c : frames_)
                        if (c->parent == sender->parent && c != sender && c->node->attr("name") && m.sibling == c->node->attr("name")) dest = c;
                may_arrive = dest || (!m.target && sender);
                if (dest && dest->doc && dest->js && dest->js_started) { to = dest->js; to_url = dest->doc->url; }
            } else if (sender && owner_started(sender)) {
                to = owner_js(sender);
                to_url = owner_doc(sender)->url;
                source = sender->node;
            }
            if (!to) {
                if (may_arrive && posts[i].waited++ < kPostPatience) { i++; continue; }
                klog("frames: msg %.60s DROPPED (no receiver) sibling='%s'", m.data.c_str(), m.sibling.c_str());
                posts.remove(i);
                continue;
            }
            posts.remove(i);
            String want = m.target_origin == "/" ? from : url_origin(m.target_origin);
            bool ok = m.target_origin == "*" || want == url_origin(to_url);
            klog("frames: msg %.60s from %s want '%.60s' receiver %.60s ports=%.40s %s", m.data.c_str(), from.c_str(), m.target_origin.c_str(), to_url.c_str(), m.ports.c_str(), ok ? "delivered" : "REFUSED");
            if (ok) html::js_deliver_message(to, m.data, from, source, m.ports);
        }
    }

    bool pump_frames() {
        if (frames_.empty()) { if (js_) html::js_posts(js_).clear(); return false; }
        bool changed = false;
        for (size_t i = 0; i < frames_.size(); i++) changed |= pump_frame(frames_[i]);
        route_messages(js_, nullptr);
        for (Frame* fr : frames_) if (fr->doc) route_messages(fr->js, fr);
        main_viewport();
        return changed;
    }

    // Workers are same-origin only; each runs in its own context with no document.
    void start_workers(html::JsEnv* env, Frame* owner) {
        Vector<html::JsWorkerReq>& reqs = html::js_worker_requests(env);
        html::Document* d = owner ? owner->doc : doc_;
        for (html::JsWorkerReq& q : reqs) {
            if (q.url.empty()) {
                for (Frame* fr : frames_) if (fr->worker && fr->parent == owner && fr->worker_port == q.port) { destroy_frame(fr); break; }
                continue;
            }
            int workers = 0;
            for (Frame* fr : frames_) if (fr->worker) workers++;
            if (workers >= kMaxWorkers || url_origin(q.url) != url_origin(d->url) || !q.url.starts_with("http")) {
                klog("browser: refused worker %.160s", q.url.c_str());
                continue;
            }
            Frame* fr = new Frame();
            fr->parent = owner;
            fr->node = new html::Node();
            fr->node->tag = "worker";
            fr->worker = true;
            fr->worker_port = q.port;
            fr->worker_peer = q.peer;
            fr->url = q.url;
            frames_.push(fr);
            fr->pending.push(Pending{net::http_get(q.url.c_str()), q.url, 0});
        }
        reqs.clear();
    }

    void show_worker(Frame* fr, const char* code, size_t len, const char* url) {
        reset_frame(fr);
        fr->doc = html::parse("<html></html>", 13, url, true);
        fr->js = html::js_create(fr->doc);
        fr->js_started = true;
        fr->load_fired = true;
        klog("frames: worker start %.120s", url);
        html::js_start_worker(fr->js, fr->worker_port, fr->worker_peer, code, len, url);
    }

    void fetch_new_scripts(html::JsEnv* env, Vector<Pending>& pending) {
        Vector<html::ScriptInfo>& sc = html::js_scripts(env);
        for (int i = 0; i < (int)sc.size(); i++) {
            if (!sc[i].dynamic || sc[i].requested || sc[i].src.empty()) continue;
            sc[i].requested = true;
            pending.push(Pending{net::http_get(sc[i].src.c_str()), sc[i].src, 3, i});
        }
    }

    // Click at (x, y) in the frame's own viewport.
    void click_frame(Frame* fr, int x, int y, int button) {
        if (!fr->doc) return;
        focus_frame_ = fr;
        const html::Fragment* f = html::hit(fr->doc, x, y, fr->scroll);
        if (f && f->kind == html::Fragment::Frame) {
            Frame* inner = frame_for(fr, f->node);
            Rect r = html::fragment_view_rect(fr->doc, f, fr->scroll);
            if (inner) click_frame(inner, x - r.x, y - r.y, button);
            return;
        }
        html::Node* target = f && f->node ? f->node : html::hit_node(fr->doc, x, y, fr->scroll);
        if (fr->js && fr->js_started && target && button == MB_LEFT) {
            html::js_dispatch(fr->js, target, "mousedown");
            html::js_dispatch(fr->js, target, "mouseup");
            bool prevented = html::js_dispatch(fr->js, target, "click");
            html::restyle(fr->doc);
            layout_frame(fr);
            main_viewport();
            invalidate();
            if (prevented) return;
            f = html::hit(fr->doc, x, y, fr->scroll);
            if (f && f->node != target) {
                f = nullptr;
                for (const html::Fragment& g : fr->doc->frags) if (g.node == target && g.box_of < 0) { f = &g; break; }
            }
        }
        if (!f) { fr->doc->focused_input = nullptr; invalidate(); return; }
        if (f->kind == html::Fragment::Input) {
            html::click_input(fr->doc, f);
            const char* type = f->node->attr("type");
            if ((type && (!strcasecmp(type, "submit") || !strcasecmp(type, "image"))) || f->node->tag == "button") {
                String url = html::key_input(fr->doc, 0, true, false);
                if (!url.empty()) load_frame(fr, url);
            }
            invalidate();
            return;
        }
        fr->doc->focused_input = nullptr;
        if (!f->href.empty() && button == MB_LEFT) { String to = f->href; load_frame(fr, to); }
        invalidate();
    }

    void request_images() {
        if (!doc_) return;
        int inflight = 0;
        bool decoded = false;
        for (Pending& p : pending_) if (p.kind == 1) inflight++;
        for (html::Image& im : html::images(doc_)) {
            if (inflight >= 4) break;
            if (im.requested || im.url.empty()) continue;
            im.requested = true;
            if (im.url.starts_with("data:")) {
                im.canvas = decode_data_url(im);
                im.failed = !im.canvas;
                decoded |= im.canvas != nullptr;
                continue;
            }
            // no webp/avif decoder: ask CDNs for formats we can show
            pending_.push(Pending{net::http_get(im.url.c_str(), "Accept: image/png,image/jpeg,image/gif,image/svg+xml,*/*;q=0.1\r\n"), im.url, 1});
            inflight++;
        }
        if (decoded) { relayout(); invalidate(); }
    }

    Canvas* decode_image(html::Image& im, const uint8_t* body, size_t len, bool is_svg) {
#ifdef HAVE_SVG
        if (is_svg) {
            int w = im.want_w > 1 ? im.want_w : 0, h = im.want_h > 1 ? im.want_h : 0;
            if (!w || !h) svg::intrinsic_size((const char*)body, len, &w, &h);
            return svg::render((const char*)body, len, kmax(w, 1), kmax(h, 1));
        }
#endif
        (void)is_svg;
        return image_decode(body, len);
    }

    // data:[<type>][;base64],<payload>
    Canvas* decode_data_url(html::Image& im) {
        long comma = im.url.find(',');
        if (comma < 0) return nullptr;
        String head = im.url.substr(0, comma).lower();
        const char* p = im.url.c_str() + comma + 1;
        Vector<uint8_t> out;
        if (head.find(";base64") >= 0) {
            uint32_t acc = 0;
            int bits = 0;
            for (; *p; p++) {
                char ch = *p;
                int v = ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 : ch >= '0' && ch <= '9' ? ch - '0' + 52 :
                        ch == '+' || ch == '-' ? 62 : ch == '/' || ch == '_' ? 63 : -1;
                if (v < 0) continue;
                acc = (acc << 6) | (uint32_t)v;
                bits += 6;
                if (bits >= 8) { bits -= 8; out.push((uint8_t)((acc >> bits) & 255)); }
            }
        } else {
            auto hexv = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
            for (; *p; p++) {
                if (*p == '%' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0) { out.push((uint8_t)(hexv(p[1]) * 16 + hexv(p[2]))); p += 2; }
                else out.push((uint8_t)*p);
            }
        }
        if (out.empty()) return nullptr;
        return decode_image(im, out.data(), out.size(), im.svg || head.find("svg") >= 0);
    }

    int view_h() const { return height() - kToolbar; }

    // Run scripts once all external ones arrived; then timers, fetches, navigation.
    bool pump_js(bool& relayout_needed) {
        if (!js_started_) {
            for (html::ScriptInfo& si : html::js_scripts(js_)) if (!si.fetched && !si.failed) return false;
            js_started_ = true;
            html::js_run_scripts(js_);
            html::restyle(doc_);
            relayout_needed = true;
        }
        bool dirty = html::js_tick(js_, timer_ms());
        fetch_new_scripts(js_, pending_);
        start_workers(js_, nullptr);
        for (html::JsFetch& f : html::js_fetches(js_)) {
            if (f.started) continue;
            f.started = true;
            net::HttpRequest* r = f.method == "POST" ? net::http_post(f.url.c_str(), "application/x-www-form-urlencoded", f.body.c_str(), f.body.size())
                                                     : net::http_get(f.url.c_str());
            pending_.push(Pending{r, f.url, 4, f.id});
        }
        String nav = html::js_take_navigation(js_);
        if (!nav.empty()) { klog("browser: script navigation -> %.160s", nav.c_str()); navigate(nav.c_str(), true); return true; }
        if (dirty) {
            html::restyle(doc_);
            relayout_needed = true;
            if (!doc_->title.empty()) set_title(doc_->title.c_str());
        }
        return dirty;
    }

    // ------------------------------------------------------------ ticking
    void on_tick(uint64_t) override {
        bool changed = false, relayout_needed = false;
        for (size_t i = 0; i < pending_.size();) {
            Pending& p = pending_[i];
            net::HttpState st = net::http_state(p.req);
            if (p.kind == 0 && st != net::HttpState::Done && st != net::HttpState::Failed) {
                size_t total = net::http_content_length(p.req), got = net::http_received(p.req);
                float target = st == net::HttpState::Receiving ? (total ? 0.3f + 0.7f * got / total : 0.6f) : st == net::HttpState::Handshake ? 0.25f : 0.15f;
                if (target > progress_) { progress_ = target; changed = true; }
            }
            if (st != net::HttpState::Done && st != net::HttpState::Failed) { i++; continue; }
            Pending done = p;
            pending_.remove(i);
            if (done.kind == 0) {
                loading_ = false;
                progress_ = 1;
                if (focused_widget() == address_) focus_widget(nullptr);   // page loaded: keys go to the page
                if (st == net::HttpState::Failed) {
                    String e = error_page(done.url.c_str(), net::http_error(done.req));
                    show_html(e.c_str(), e.size(), done.url.c_str());
                } else {
                    size_t len;
                    const uint8_t* body = net::http_body(done.req, &len);
                    const char* ct = net::http_header(done.req, "content-type");
                    String final_url = net::http_final_url(done.req);
                    current_url_ = final_url;
                    if (hist_pos_ >= 0) history_[hist_pos_] = final_url;
                    address_->set_text(final_url.c_str());
                    String ctype = ct ? String(ct).lower() : String("text/html");
                    if (ctype.starts_with("image/")) {
                        String h = String("<html><body style=\"background:#222;text-align:center\"><img src=\"") + final_url + "\"></body></html>";
                        show_html(h.c_str(), h.size(), final_url.c_str());
                        if (Canvas* img = image_decode(body, len)) {
                            for (html::Image& im : html::images(doc_)) if (im.url == final_url) { im.canvas = img; im.requested = true; }
                            relayout();
                        }
                    } else {
                        bool plain = ctype.starts_with("text/plain") || ctype.find("json") >= 0;
                        show_html((const char*)body, len, final_url.c_str(), plain);
                    }
                }
                changed = true;
            } else if (done.kind == 1 && doc_) {
                Canvas* img = nullptr;
                html::Image* target = nullptr;
                for (html::Image& im : html::images(doc_)) if (im.url == done.url) target = &im;
                if (st == net::HttpState::Done && target) {
                    size_t len;
                    const uint8_t* body = net::http_body(done.req, &len);
                    const char* ct = net::http_header(done.req, "content-type");
                    bool is_svg = target->svg || (ct && strstr(ct, "svg")) || (len > 5 && (memmem_s(body, len, "<svg") != nullptr));
                    img = decode_image(*target, body, len, is_svg);
                }
                if (target && st != net::HttpState::Done && target->retries < 2) {
                    // dropped connection or timeout: queue it again
                    target->retries++;
                    target->requested = false;
                } else if (target) { target->canvas = img; target->failed = !img; }
                if (img) relayout_needed = true;
                need_images_ = true;
            } else if (done.kind == 3 && js_) {
                Vector<html::ScriptInfo>& sc = html::js_scripts(js_);
                if (done.tag < (int)sc.size()) {
                    html::ScriptInfo& si = sc[done.tag];
                    si.fetched = true;
                    size_t len = 0;
                    const uint8_t* body = st == net::HttpState::Done ? net::http_body(done.req, &len) : nullptr;
                    if (body && len && len < 4 * 1024 * 1024 && net::http_status(done.req) < 400) si.node->value = String((const char*)body, len);
                    else si.failed = true;
                }
            } else if (done.kind == 4 && js_) {
                size_t len = 0;
                const uint8_t* body = st == net::HttpState::Done ? net::http_body(done.req, &len) : nullptr;
                html::js_fetch_done(js_, done.tag, st == net::HttpState::Done ? net::http_status(done.req) : 0, body, len,
                                    net::http_header(done.req, "content-type"));
            } else if (done.kind == 2 && doc_ && st == net::HttpState::Done) {
                size_t len;
                const uint8_t* body = net::http_body(done.req, &len);
                html::add_stylesheet(doc_, (const char*)body, len);
                relayout_needed = true;
            }
            net::http_free(done.req);
        }
        if (js_) changed |= pump_js(relayout_needed);
        changed |= pump_frames();
        if (relayout_needed) { relayout(); changed = true; }
        if (need_images_) { need_images_ = false; request_images(); }
        if (!loading_ && progress_ > 0 && progress_ >= 1) { progress_ = 0; changed = true; }
        if (changed) invalidate();
    }

    // ------------------------------------------------------------ painting
    void layout_toolbar() { address_->bounds = Rect(150, 9, kmax(120, width() - 150 - 100), 32); }
    void on_resize() override { layout_toolbar(); relayout(); }

    Rect btn_rect(int i) const {
        if (i < 3) return Rect(12 + i * 40, 9, 34, 32);
        return Rect(width() - 88 + (i - 3) * 40, 9, 34, 32);
    }

    void on_paint(Canvas& c) override {
        const Theme& t = theme();
        // page
        if (doc_) {
            Canvas view(c.row(kToolbar), width(), view_h(), c.stride());
            html::paint(doc_, view, scroll_);
            // scrollbar
            if (doc_->height > view_h()) {
                int vh = view_h();
                int bar_h = kmax(30, vh * vh / doc_->height);
                int bar_y = kToolbar + (vh - bar_h) * scroll_ / kmax(1, doc_->height - vh);
                c.fill_round_rect(Rect(width() - 9, bar_y + 2, 6, bar_h - 4), 3, rgba(0, 0, 0, 90));
            }
        } else {
            c.fill_rect(Rect(0, kToolbar, width(), view_h()), hex(0xFFFFFF));
        }
        // toolbar
        c.fill_gradient(Rect(0, 0, width(), kToolbar), t.dark ? hex(0x2C2C31) : hex(0xF6F6F9), t.dark ? hex(0x26262A) : hex(0xECECF1));
        c.fill_rect(Rect(0, kToolbar - 1, width(), 1), t.separator);
        Symbol syms[5] = {Symbol::ChevronLeft, Symbol::ChevronRight, loading_ ? Symbol::Close : Symbol::Refresh, Symbol::Home, Symbol::Star};
        bool enabled[5] = {hist_pos_ > 0, hist_pos_ + 1 < (int)history_.size(), true, true, true};
        for (int i = 0; i < 5; i++) {
            Rect r = btn_rect(i);
            if (hover_btn_ == i && enabled[i]) c.fill_round_rect(r, 8, t.hover);
            draw_symbol(c, r.inset(8), syms[i], enabled[i] ? t.text : t.text_tertiary, 1.8f);
        }
        if (address_->text.empty() && !address_->focused() && doc_) address_->placeholder = "Search or enter website name";
        on_paint_widgets(c);
        // https lock
        if (!address_->focused() && current_url_.starts_with("https://"))
            draw_symbol(c, Rect(address_->bounds.right() - 24, address_->bounds.y + 9, 14, 14), Symbol::Lock, t.text_secondary, 1.3f);
        // progress
        if (progress_ > 0 && progress_ < 1)
            c.fill_rect(Rect(0, kToolbar - 3, (int)(width() * progress_), 3), t.accent);
        // link status pill
        if (!hover_.empty()) {
            int w = kmin(text_width(hover_.c_str(), 12) + 20, width() - 20);
            Rect r(8, height() - 30, w, 24);
            c.fill_round_rect(r, 12, t.dark ? rgba(50, 50, 56, 235) : rgba(250, 250, 252, 240));
            c.stroke_round_rect(r, 12, t.separator, 1);
            c.draw_text_in(r.inset(10, 0), hover_.c_str(), t.text_secondary, 12);
        }
    }

    // Window::on_paint clears + draws widgets; we need just the widgets.
    void on_paint_widgets(Canvas& c) {
        for (Widget* w : widgets()) if (w->visible) { c.save(); w->paint(c); c.restore(); }
    }

    // ------------------------------------------------------------ input
    void on_mouse_down(int x, int y, int button) override {
        if (y < kToolbar) {
            for (int i = 0; i < 5; i++)
                if (btn_rect(i).contains(x, y)) { toolbar_action(i); return; }
            Window::on_mouse_down(x, y, button);
            return;
        }
        focus_widget(nullptr);
        if (!doc_) return;
        focus_frame_ = nullptr;
        const html::Fragment* f = html::hit(doc_, x, y - kToolbar, scroll_);
        if (f && f->kind == html::Fragment::Frame) {
            doc_->focused_input = nullptr;
            Rect r = html::fragment_view_rect(doc_, f, scroll_);
            if (Frame* fr = frame_for(nullptr, f->node)) click_frame(fr, x - r.x, y - kToolbar - r.y, button);
            return;
        }
        if (!f) {
            // plain boxes aren't link or input targets, but scripts can still listen on them
            html::Node* box = js_ && js_started_ && button == MB_LEFT ? html::hit_node(doc_, x, y - kToolbar, scroll_) : nullptr;
            if (box) {
                html::js_dispatch(js_, box, "click");
                bool tmp = false;
                if (pump_js(tmp) || tmp) relayout();
            }
            if (doc_) doc_->focused_input = nullptr;
            invalidate();
            return;
        }
        if (js_ && js_started_ && f->node && button == MB_LEFT) {
            html::Node* target = f->node;
            String href = f->href;
            bool input = f->kind == html::Fragment::Input;
            bool prevented = html::js_dispatch(js_, target, "click");
            bool tmp = false;
            if (pump_js(tmp) || tmp) { relayout(); invalidate(); }
            if (prevented) return;
            // the click handler may have changed the DOM: re-hit-test
            f = html::hit(doc_, x, y - kToolbar, scroll_);
            if (!f || f->node != target) {
                // the page moved things: the click still belongs to what was pressed, not what is there now
                f = nullptr;
                for (const html::Fragment& g : doc_->frags) if (g.node == target && g.box_of < 0) { f = &g; break; }
                if (!f) return;
            }
            (void)href; (void)input;
        }
        if (f->kind == html::Fragment::Input) {
            html::click_input(doc_, f);
            const char* type = f->node->attr("type");
            if ((type && (!strcasecmp(type, "submit") || !strcasecmp(type, "image"))) || f->node->tag == "button") {
                String url = html::key_input(doc_, 0, true, false);
                if (!url.empty()) { navigate(url.c_str(), true); return; }
            }
            invalidate();
            return;
        }
        doc_->focused_input = nullptr;
        if (!f->href.empty()) {
            if (button == MB_RIGHT) {
                Vector<MenuItem> items;
                String href = f->href;
                MenuItem& a = items.emplace(); a.title = "Open Link"; a.action = [this, href] { navigate(href.c_str(), true); };
                MenuItem& b = items.emplace(); b.title = "Copy Link"; b.action = [href] { wm::clipboard_set(href.c_str()); };
                Rect cr = content_rect();
                wm::popup_menu(cr.x + x, cr.y + y, items);
                return;
            }
            String href = f->href;
            if (href.starts_with("javascript:") || href.starts_with("mailto:")) return;
            // same-page anchor
            long hash = href.find('#');
            if (hash >= 0 && href.substr(0, hash) == current_url_.substr(0, current_url_.find('#') >= 0 ? current_url_.find('#') : current_url_.size())) {
                int ay = html::anchor_y(doc_, href.c_str() + hash + 1);
                if (ay >= 0) { scroll_ = kclamp(ay, 0, kmax(0, doc_->height - view_h())); invalidate(); return; }
            }
            klog("browser: link click at %d,%d -> %.160s", x, y, href.c_str());
            navigate(href.c_str(), true);
        }
    }

    void on_mouse_move(int x, int y, int buttons) override {
        int hb = -1;
        if (y < kToolbar) for (int i = 0; i < 5; i++) if (btn_rect(i).contains(x, y)) hb = i;
        if (hb != hover_btn_) { hover_btn_ = hb; invalidate(); }
        String h;
        if (doc_ && y >= kToolbar) {
            const html::Fragment* f = html::hit(doc_, x, y - kToolbar, scroll_);
            if (f) h = f->href;
        }
        if (h != hover_) { hover_ = h; invalidate(); }
        Window::on_mouse_move(x, y, buttons);
    }

    void on_scroll(int x, int y, int delta) override {
        if (!doc_) return;
        scroll_by(delta * 60);
    }

    void scroll_by(int dy) {
        if (doc_) {
            Point m = wm::mouse_pos();
            Rect cr = content_rect();
            int mx = m.x - cr.x, my = m.y - cr.y - kToolbar;
            if (my >= 0) {
                const html::Fragment* f = html::hit(doc_, mx, my, scroll_);
                Frame* fr = f && f->kind == html::Fragment::Frame ? frame_for(nullptr, f->node) : nullptr;
                if (fr && fr->doc && fr->doc->height > fr->h) {
                    fr->scroll = kclamp(fr->scroll + dy, 0, fr->doc->height - fr->h);
                    fr->node->content_scroll = fr->scroll;
                    invalidate();
                    return;
                }
            }
            if (my >= 0 && html::scroll_at(doc_, mx, my, scroll_, dy)) { invalidate(); return; }
        }
        int maxs = kmax(0, doc_ ? doc_->height - view_h() : 0);
        int ns = kclamp(scroll_ + dy, 0, maxs);
        if (ns != scroll_) { scroll_ = ns; invalidate(); }
    }

    void toolbar_action(int i) {
        switch (i) {
        case 0: if (hist_pos_ > 0) { hist_pos_--; navigate(history_[hist_pos_].c_str(), false); } break;
        case 1: if (hist_pos_ + 1 < (int)history_.size()) { hist_pos_++; navigate(history_[hist_pos_].c_str(), false); } break;
        case 2: if (loading_) { cancel_all(); invalidate(); } else navigate(current_url_.c_str(), false); break;
        case 3: navigate("klein://start", true); break;
        case 4: {
            Vector<MenuItem> items;
            const char* marks[][2] = {{"Wikipedia", "https://en.wikipedia.org/"}, {"Hacker News", "https://news.ycombinator.com/"},
                                      {"DuckDuckGo Lite", "https://lite.duckduckgo.com/lite/"}, {"NPR Text", "https://text.npr.org/"},
                                      {"FrogFind", "http://frogfind.com/"}, {"example.com", "https://example.com/"}};
            for (auto& m : marks) {
                MenuItem& it = items.emplace();
                it.title = m[0];
                String u = m[1];
                it.action = [this, u] { navigate(u.c_str(), true); };
            }
            Rect cr = content_rect();
            wm::popup_menu(cr.x + btn_rect(4).x - 120, cr.y + kToolbar, items);
            break;
        }
        }
    }

    void on_key(const KeyEvent& e) override {
        if (e.cmd() && e.key == Key::Char) {
            uint32_t ch = e.ch | 0x20;
            if (ch == 'l') { focus_widget(address_); address_->select_all(); return; }
            if (ch == 'r') { navigate(current_url_.c_str(), false); return; }
            if (ch == 'n') { Window* w = new BrowserWindow(); w->set_app_id("browser"); wm::focus(w); return; }
            if (ch == '[') { toolbar_action(0); return; }
            if (ch == ']') { toolbar_action(1); return; }
        }
        if (focused_widget()) { Window::on_key(e); return; }
        if (focus_frame_ && focus_frame_->doc && focus_frame_->doc->focused_input) {
            Frame* fr = focus_frame_;
            String url;
            if (e.key == Key::Enter) url = html::key_input(fr->doc, 0, true, false);
            else if (e.key == Key::Backspace) html::key_input(fr->doc, 0, false, true);
            else if ((e.key == Key::Char || e.key == Key::Space) && e.ch) html::key_input(fr->doc, e.ch, false, false);
            else if (e.key == Key::Escape) fr->doc->focused_input = nullptr;
            if (fr->js && fr->js_started && fr->doc->focused_input && e.key != Key::Enter) html::js_input_changed(fr->js, fr->doc->focused_input);
            if (!url.empty()) load_frame(fr, url);
            invalidate();
            return;
        }
        if (doc_ && doc_->focused_input) {
            String url;
            if (e.key == Key::Enter) url = html::key_input(doc_, 0, true, false);
            else if (e.key == Key::Backspace) html::key_input(doc_, 0, false, true);
            else if ((e.key == Key::Char || e.key == Key::Space) && e.ch) html::key_input(doc_, e.ch, false, false);
            if (js_ && js_started_ && doc_->focused_input && e.key != Key::Enter) html::js_input_changed(js_, doc_->focused_input);
            else if (e.key == Key::Escape) doc_->focused_input = nullptr;
            if (!url.empty()) navigate(url.c_str(), true);
            invalidate();
            return;
        }
        switch (e.key) {
        case Key::Down: scroll_by(48); break;
        case Key::Up: scroll_by(-48); break;
        case Key::PageDown: case Key::Space: scroll_by(view_h() - 60); break;
        case Key::PageUp: scroll_by(-(view_h() - 60)); break;
        case Key::Home: scroll_by(-1000000); break;
        case Key::End: scroll_by(1000000); break;
        case Key::Backspace: toolbar_action(0); break;
        default: Window::on_key(e);
        }
    }

    void build_menus(Vector<Menu>& menus) override {
        Menu& f = menus.emplace();
        f.title = "File";
        f.add("New Window", [] { Window* w = new BrowserWindow(); w->set_app_id("browser"); wm::focus(w); }, "⌘N");
        f.add("Open Location…", [this] { focus_widget(address_); address_->select_all(); }, "⌘L");
        f.separator();
        f.add("Close Window", [this] { close(); }, "⌘W");
        Menu& v = menus.emplace();
        v.title = "View";
        v.add("Reload Page", [this] { navigate(current_url_.c_str(), false); }, "⌘R");
        v.add("Stop", [this] { cancel_all(); invalidate(); });
        v.separator();
        v.add(js_enabled_ ? "Disable JavaScript" : "Enable JavaScript", [this] { js_enabled_ = !js_enabled_; navigate(current_url_.c_str(), false); });
        Menu& h = menus.emplace();
        h.title = "History";
        h.add("Back", [this] { toolbar_action(0); }, "⌘[");
        h.add("Forward", [this] { toolbar_action(1); }, "⌘]");
        h.add("Home", [this] { navigate("klein://start", true); });
        h.separator();
        for (int i = (int)history_.size() - 1; i >= 0 && i >= (int)history_.size() - 10; i--) {
            String u = history_[i];
            h.add(u.c_str(), [this, u] { navigate(u.c_str(), true); });
        }
    }

    TextField* address_;
    html::Document* doc_ = nullptr;
    html::JsEnv* js_ = nullptr;
    bool js_started_ = false;
    bool js_enabled_ = true;
    Vector<Pending> pending_;
    Vector<Frame*> frames_;
    Frame* focus_frame_ = nullptr;
    Vector<String> history_;
    int hist_pos_ = -1;
    String current_url_;
    String hover_;
    int hover_btn_ = -1;
    int scroll_ = 0;
    bool loading_ = false;
    bool need_images_ = false;
    float progress_ = 0;
};

BrowserWindow* g_last;

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x3FA9FF), hex(0x1060E0));
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, R = r.w * 0.34f;
    c.fill_circle(cx, cy, R, rgba(255, 255, 255, 40));
    c.stroke_circle(cx, cy, R, hex(0xFFFFFF), r.w / 28.0f);
    // compass needle
    PointF n1[] = {{cx, cy - R * 0.85f}, {cx + R * 0.22f, cy}, {cx - R * 0.22f, cy}};
    PointF n2[] = {{cx, cy + R * 0.85f}, {cx + R * 0.22f, cy}, {cx - R * 0.22f, cy}};
    c.fill_polygon(n1, 3, hex(0xFF453A));
    c.fill_polygon(n2, 3, hex(0xFFFFFF));
    for (int i = 0; i < 12; i++) {
        float a = i * 3.14159f / 6;
        c.draw_line(cx + cosf(a) * R * 0.82f, cy + sinf(a) * R * 0.82f, cx + cosf(a) * R * 0.95f, cy + sinf(a) * R * 0.95f, hex(0xFFFFFF), 1);
    }
}

Window* create() {
    g_last = new BrowserWindow();
    return g_last;
}

struct Init {
    Init() {
        register_open_handler("browser", [](const char* path) {
            BrowserWindow* w = nullptr;
            for (Window* x : wm::windows()) if (!strcmp(x->app_id(), "browser") && !x->closing_) w = (BrowserWindow*)x;
            if (!w) { w = (BrowserWindow*)create(); w->set_app_id("browser"); }
            wm::focus(w);
            if (path[0] == '/') { String u = String("file://") + path; w->navigate(u.c_str(), true); }
            else w->go(path);
        });
    }
} g_init;

AppRegistrar reg({"browser", "Voyager", icon, create, "Internet", false, true, 25, "Browse the web"});

}  // namespace
