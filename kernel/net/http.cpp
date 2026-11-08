#include "net/http.h"
#include "net/net.h"
#include "net/tls.h"
#include "sys/system.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "lib/log.h"
#include "lib/kstring.h"
#include "lib/vector.h"
#include "arch/interrupts.h"

namespace net {

struct HttpRequest {
    HttpState state = HttpState::Resolving;
    Url url;
    String full_url;
    String method = "GET";
    String extra;
    bool resolved = false;   // r->ip is valid
    String content_type;
    Vector<uint8_t> post_body;
    int dns = -1;
    uint32_t ip = 0;
    int sock = -1;
    tls::Client* tls = nullptr;
    bool request_sent = false;
    uint64_t started = 0, last_activity = 0;
    int redirects = 0;
    bool waiting_net = false;
    // response
    Vector<uint8_t> raw;          // header bytes until parsed, then body bytes (raw / chunked)
    bool headers_done = false;
    int status = 0;
    Vector<String> header_names, header_values;
    bool chunked = false;
    size_t content_length = 0;
    bool has_length = false;
    Vector<uint8_t> body;
    String error;
};

bool url_parse(const char* url, Url* u) {
    memset(u, 0, sizeof(*u));
    const char* p = strstr(url, "://");
    if (p) {
        size_t n = p - url;
        if (n >= sizeof(u->scheme)) return false;
        memcpy(u->scheme, url, n);
        for (size_t i = 0; i < n; i++) u->scheme[i] = (char)tolower(u->scheme[i]);
        p += 3;
    } else {
        strcpy(u->scheme, "http");
        p = url;
    }
    if (strcmp(u->scheme, "http") && strcmp(u->scheme, "https")) return false;
    u->port = strcmp(u->scheme, "https") == 0 ? 443 : 80;
    const char* hs = p;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') p++;
    size_t hl = p - hs;
    if (!hl || hl >= sizeof(u->host)) return false;
    memcpy(u->host, hs, hl);
    for (size_t i = 0; i < hl; i++) u->host[i] = (char)tolower(u->host[i]);
    if (*p == ':') { p++; u->port = (uint16_t)strtoul(p, (char**)&p, 10); }
    if (*p == '/' || *p == '?') {
        size_t n = 0;
        if (*p == '?') u->path[n++] = '/';
        while (*p && *p != '#' && n < sizeof(u->path) - 1) {
            char c = *p++;
            if (c == ' ') { if (n + 3 < sizeof(u->path)) { memcpy(u->path + n, "%20", 3); n += 3; } }
            else u->path[n++] = c;
        }
        u->path[n] = 0;
    } else {
        strcpy(u->path, "/");
    }
    return true;
}

void url_resolve(const char* base, const char* ref, char* out, size_t size) {
    while (*ref == ' ' || *ref == '\t' || *ref == '\n') ref++;
    if (strstr(ref, "://") || strncmp(ref, "about:", 6) == 0 || strncmp(ref, "klein:", 6) == 0 ||
        strncmp(ref, "mailto:", 7) == 0 || strncmp(ref, "javascript:", 11) == 0) {
        strlcpy(out, ref, size);
        return;
    }
    Url b;
    if (!url_parse(base, &b)) { strlcpy(out, ref, size); return; }
    char port[16] = "";
    if (!((b.port == 80 && !strcmp(b.scheme, "http")) || (b.port == 443 && !strcmp(b.scheme, "https"))))
        snprintf(port, sizeof(port), ":%u", b.port);
    if (ref[0] == '/' && ref[1] == '/') { snprintf(out, size, "%s:%s", b.scheme, ref); return; }
    if (ref[0] == '/') { snprintf(out, size, "%s://%s%s%s", b.scheme, b.host, port, ref); return; }
    if (ref[0] == '#') {
        char path[2048];
        strlcpy(path, b.path, sizeof(path));
        snprintf(out, size, "%s://%s%s%s%s", b.scheme, b.host, port, path, ref);
        return;
    }
    if (ref[0] == '?') {
        char path[2048];
        strlcpy(path, b.path, sizeof(path));
        char* q = strchr(path, '?');
        if (q) *q = 0;
        snprintf(out, size, "%s://%s%s%s%s", b.scheme, b.host, port, path, ref);
        return;
    }
    // relative path: directory of base path + ref, then normalize ./ and ../
    char path[4096];
    strlcpy(path, b.path, sizeof(path));
    char* q = strchr(path, '?');
    if (q) *q = 0;
    char* slash = strrchr(path, '/');
    if (slash) slash[1] = 0;
    strlcpy(path + strlen(path), ref, sizeof(path) - strlen(path));
    // normalize
    char norm[4096];
    size_t n = 0;
    const char* s = path;
    while (*s) {
        if (s[0] == '/' && s[1] == '.' && (s[2] == '/' || s[2] == 0)) { s += 2; continue; }
        if (s[0] == '/' && s[1] == '.' && s[2] == '.' && (s[3] == '/' || s[3] == 0)) {
            while (n > 0 && norm[n - 1] != '/') n--;
            if (n > 0) n--;
            s += 3;
            continue;
        }
        if (n < sizeof(norm) - 1) norm[n++] = *s;
        s++;
    }
    norm[n] = 0;
    if (!n) strcpy(norm, "/");
    snprintf(out, size, "%s://%s%s%s", b.scheme, b.host, port, norm);
}

// ---------------------------------------------------------------- cookie jar (in memory)
struct Cookie { String domain, path, name, value; bool host_only; };
static Vector<Cookie> g_cookies;

static bool domain_match(const char* host, const Cookie& c) {
    if (c.host_only) return strcasecmp(host, c.domain.c_str()) == 0;
    size_t hl = strlen(host), dl = c.domain.size();
    if (hl < dl) return false;
    if (strcasecmp(host + hl - dl, c.domain.c_str()) != 0) return false;
    return hl == dl || host[hl - dl - 1] == '.';
}

static void store_cookie(const char* host, const char* header) {
    String h(header);
    long semi = h.find(';');
    String kv = (semi >= 0 ? h.substr(0, semi) : h).trim();
    long eq = kv.find('=');
    if (eq <= 0) return;
    Cookie c;
    c.name = kv.substr(0, eq).trim();
    c.value = kv.substr(eq + 1).trim();
    c.domain = host;
    c.host_only = true;
    c.path = "/";
    bool expired = false;
    String attrs = semi >= 0 ? h.substr(semi + 1) : String();
    long pos = 0;
    while (pos < (long)attrs.size()) {
        long next = attrs.find(';', pos);
        if (next < 0) next = attrs.size();
        String a = attrs.substr(pos, next - pos).trim();
        String al = a.lower();
        if (al.starts_with("domain=")) {
            String d = a.substr(7).trim();
            while (d.starts_with(".")) d = d.substr(1);
            c.domain = d.lower();
            c.host_only = false;
        } else if (al.starts_with("path=")) c.path = a.substr(5).trim();
        else if (al.starts_with("max-age=") && atoi(a.c_str() + 8) <= 0) expired = true;
        pos = next + 1;
    }
    for (size_t i = 0; i < g_cookies.size(); i++)
        if (g_cookies[i].name == c.name && g_cookies[i].domain == c.domain && g_cookies[i].path == c.path) { g_cookies.remove(i); break; }
    if (!expired && g_cookies.size() < 400) g_cookies.push(c);
}

static String cookie_header(const char* host, const char* path) {
    String out;
    for (Cookie& c : g_cookies) {
        if (!domain_match(host, c)) continue;
        if (strncmp(path, c.path.c_str(), c.path.size()) != 0) continue;
        if (!out.empty()) out += "; ";
        out += c.name;
        out += "=";
        out += c.value;
    }
    return out;
}

static int tls_send_cb(void* ctx, const uint8_t* data, size_t len) {
    HttpRequest* r = (HttpRequest*)ctx;
    return tcp_send(r->sock, data, len);
}

static void fail(HttpRequest* r, const char* why) {
    r->state = HttpState::Failed;
    r->error = why;
    klog("http: %s failed: %s", r->full_url.c_str(), why);
}

static void reset_connection(HttpRequest* r) {
    delete r->tls;
    r->tls = nullptr;
    if (r->sock >= 0) tcp_close(r->sock);
    r->sock = -1;
    r->request_sent = false;
    r->raw.clear();
    r->body.clear();
    r->headers_done = false;
    r->header_names.clear();
    r->header_values.clear();
    r->chunked = r->has_length = false;
    r->content_length = 0;
    r->status = 0;
}

static bool start(HttpRequest* r, const char* url) {
    r->full_url = url;
    if (!url_parse(url, &r->url)) { fail(r, "invalid URL"); return false; }
    r->state = HttpState::Resolving;
    r->dns = dns_resolve(r->url.host);
    r->started = r->last_activity = timer_ms();
    r->resolved = false;
    return true;   // no free DNS slot yet (dns < 0): pump retries
}

static HttpRequest* make(const char* url) {
    HttpRequest* r = new HttpRequest();
    if (!is_up()) {
        // network may still be coming up (DHCP): wait for it in pump()
        r->full_url = url;
        r->waiting_net = true;
        r->started = r->last_activity = timer_ms();
        return r;
    }
    start(r, url);
    return r;
}

HttpRequest* http_get(const char* url, const char* extra) {
    HttpRequest* r = make(url);
    if (extra) r->extra = extra;
    return r;
}

HttpRequest* http_post(const char* url, const char* ctype, const void* body, size_t len) {
    HttpRequest* r = make(url);
    r->method = "POST";
    r->content_type = ctype;
    for (size_t i = 0; i < len; i++) r->post_body.push(((const uint8_t*)body)[i]);
    return r;
}

static void send_request(HttpRequest* r) {
    String req = String::format("%s %s HTTP/1.1\r\nHost: %s\r\n", r->method.c_str(), r->url.path, r->url.host);
    req += "User-Agent: Mozilla/5.0 (X11; KleinOS x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36 Voyager/1.0\r\n";
    if (r->extra.find("Accept:") < 0) req += "Accept: text/html,application/xhtml+xml,text/plain;q=0.9,image/png,image/jpeg,image/gif,*/*;q=0.5\r\n";
    req += "Accept-Language: en-US,en;q=0.8\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
    if (r->method == "POST") req += String::format("Content-Type: %s\r\nContent-Length: %zu\r\n", r->content_type.c_str(), r->post_body.size());
    String cookies = cookie_header(r->url.host, r->url.path);
    if (!cookies.empty()) req += String("Cookie: ") + cookies + "\r\n";
    if (!r->extra.empty()) req += r->extra;
    req += "\r\n";
    if (r->tls) {
        r->tls->write((const uint8_t*)req.c_str(), req.size());
        if (!r->post_body.empty()) r->tls->write(r->post_body.data(), r->post_body.size());
    } else {
        tcp_send(r->sock, req.c_str(), req.size());
        if (!r->post_body.empty()) tcp_send(r->sock, r->post_body.data(), r->post_body.size());
    }
    r->request_sent = true;
    r->state = HttpState::Receiving;
}

static bool parse_headers(HttpRequest* r) {
    // find \r\n\r\n
    size_t n = r->raw.size();
    const uint8_t* d = r->raw.data();
    size_t end = 0;
    for (size_t i = 3; i < n; i++)
        if (d[i - 3] == '\r' && d[i - 2] == '\n' && d[i - 1] == '\r' && d[i] == '\n') { end = i + 1; break; }
    if (!end) return false;
    String head((const char*)d, end);
    // status line
    long sp = head.find(' ');
    r->status = sp > 0 ? atoi(head.c_str() + sp + 1) : 0;
    long pos = head.find("\r\n");
    while (pos >= 0 && (size_t)pos + 2 < head.size()) {
        long next = head.find("\r\n", pos + 2);
        if (next < 0) break;
        String line = head.substr(pos + 2, next - pos - 2);
        long colon = line.find(':');
        if (colon > 0) {
            String name = line.substr(0, colon).lower();
            String value = line.substr(colon + 1).trim();
            if (name == "set-cookie") store_cookie(r->url.host, value.c_str());
            r->header_names.push(name);
            r->header_values.push(value);
        }
        pos = next;
    }
    const char* te = http_header(r, "transfer-encoding");
    r->chunked = te && strstr(te, "chunked");
    const char* cl = http_header(r, "content-length");
    if (cl) { r->content_length = strtoul(cl, nullptr, 10); r->has_length = true; }
    // keep only body bytes in raw
    Vector<uint8_t> rest;
    for (size_t i = end; i < n; i++) rest.push(d[i]);
    r->raw = std::move(rest);
    r->headers_done = true;
    return true;
}

// Decode as much chunked data as available. Returns true when the final chunk was seen.
static bool dechunk(HttpRequest* r) {
    size_t off = 0;
    const uint8_t* d = r->raw.data();
    size_t n = r->raw.size();
    bool finished = false;
    while (true) {
        size_t line_end = off;
        while (line_end + 1 < n && !(d[line_end] == '\r' && d[line_end + 1] == '\n')) line_end++;
        if (line_end + 1 >= n) break;
        size_t len = strtoul((const char*)d + off, nullptr, 16);
        // guard: strtoul stops at non-hex; chunk extensions are ignored
        size_t data_start = line_end + 2;
        if (len == 0) { finished = true; off = n; break; }
        if (data_start + len + 2 > n) break;
        for (size_t i = 0; i < len; i++) r->body.push(d[data_start + i]);
        off = data_start + len + 2;
    }
    if (off) {
        Vector<uint8_t> rest;
        for (size_t i = off; i < n; i++) rest.push(d[i]);
        r->raw = std::move(rest);
    }
    return finished;
}

static void finish(HttpRequest* r) {
    if (!r->chunked) {
        r->body = std::move(r->raw);
        r->raw = Vector<uint8_t>();
    }
    // redirects
    if (r->status >= 300 && r->status < 400 && r->status != 304) {
        const char* loc = http_header(r, "location");
        if (loc && r->redirects < 8) {
            char next[2300];
            url_resolve(r->full_url.c_str(), loc, next, sizeof(next));
            r->redirects++;
            reset_connection(r);
            klog("http: redirect -> %s", next);
            start(r, next);
            return;
        }
    }
    delete r->tls;
    r->tls = nullptr;
    if (r->sock >= 0) tcp_close(r->sock);
    r->sock = -1;
    r->state = HttpState::Done;
}

static void pump(HttpRequest* r) {
    uint64_t now = timer_ms();
    if (r->state == HttpState::Done || r->state == HttpState::Failed) return;
    if (r->waiting_net) {
        if (is_up()) { r->waiting_net = false; String u = r->full_url; start(r, u.c_str()); }
        else if (now - r->started > 15000) fail(r, "No network connection");
        return;
    }
    if (now - r->last_activity > 20000) { fail(r, "Connection timed out"); return; }
    switch (r->state) {
    case HttpState::Resolving: {
        if (!r->resolved) {
            // a page can start more requests than there are DNS slots or sockets: wait for one
            if (r->dns < 0 && (r->dns = dns_resolve(r->url.host)) < 0) return;
            int res = dns_result(r->dns, &r->ip);
            if (res == 0) return;
            if (res < 0) { fail(r, "Server not found (DNS lookup failed)"); return; }
            r->resolved = true;
        }
        r->sock = tcp_connect(r->ip, r->url.port);
        if (r->sock < 0) return;
        r->state = HttpState::Connecting;
        r->last_activity = now;
        return;
    }
    case HttpState::Connecting: {
        TcpState ts = tcp_state(r->sock);
        if (ts == TcpState::Connecting) return;
        if (ts != TcpState::Open) { fail(r, "Connection refused"); return; }
        r->last_activity = now;
        if (!strcmp(r->url.scheme, "https")) {
            r->tls = new tls::Client(r->url.host, tls_send_cb, r);
            r->tls->start();
            r->state = HttpState::Handshake;
        } else {
            send_request(r);
        }
        return;
    }
    default: break;
    }
    // read socket
    uint8_t buf[8192];
    size_t got;
    while ((got = tcp_recv(r->sock, buf, sizeof(buf))) > 0) {
        r->last_activity = now;
        if (r->tls) {
            r->tls->on_receive(buf, got);
            if (r->tls->failed()) { fail(r, r->tls->error()); return; }
            size_t n;
            while ((n = r->tls->read(buf, sizeof(buf))) > 0)
                for (size_t i = 0; i < n; i++) r->raw.push(buf[i]);
        } else {
            for (size_t i = 0; i < got; i++) r->raw.push(buf[i]);
        }
    }
    if (r->state == HttpState::Handshake && r->tls && r->tls->ready()) send_request(r);
    if (!r->headers_done && !parse_headers(r)) {
        if (tcp_peer_closed(r->sock)) fail(r, r->raw.empty() ? "Empty response from server" : "Malformed response");
        return;
    }
    if (r->chunked) {
        if (dechunk(r)) { finish(r); return; }
    } else if (r->has_length && r->raw.size() >= r->content_length) {
        finish(r);
        return;
    }
    bool closed = tcp_peer_closed(r->sock) || (r->tls && r->tls->closed());
    if (closed) finish(r);
}

static Vector<HttpRequest*> g_active;
static bool g_hooked;

static void poll_all() {
    for (HttpRequest* r : g_active) pump(r);
}

HttpState http_state(HttpRequest* r) {
    if (!g_hooked) { system_add_poll_hook(poll_all); g_hooked = true; }
    if (!g_active.contains(r)) g_active.push(r);
    pump(r);
    return r->state;
}

int http_status(HttpRequest* r) { return r->status; }

const char* http_header(HttpRequest* r, const char* name) {
    for (size_t i = 0; i < r->header_names.size(); i++)
        if (strcasecmp(r->header_names[i].c_str(), name) == 0) return r->header_values[i].c_str();
    return nullptr;
}

const uint8_t* http_body(HttpRequest* r, size_t* len) {
    *len = r->body.size();
    return r->body.data();
}

const char* http_final_url(HttpRequest* r) { return r->full_url.c_str(); }
const char* http_error(HttpRequest* r) { return r->error.c_str(); }
size_t http_received(HttpRequest* r) { return r->chunked ? r->body.size() : r->raw.size(); }
size_t http_content_length(HttpRequest* r) { return r->content_length; }

void http_free(HttpRequest* r) {
    if (!r) return;
    g_active.remove_value(r);
    delete r->tls;
    if (r->sock >= 0) tcp_close(r->sock);
    delete r;
}

}  // namespace net
