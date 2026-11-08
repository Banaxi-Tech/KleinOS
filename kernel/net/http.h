#pragma once
// Asynchronous HTTP/1.1 client (http:// and https:// via TLS 1.3).
// Poll from on_tick:
//   req = net::http_get("https://example.com/");
//   ... if (net::http_state(req) == net::HttpState::Done) { body = net::http_body(req, &len); ... }
//   net::http_free(req);
#include <stdint.h>
#include <stddef.h>

namespace net {

enum class HttpState { Resolving, Connecting, Handshake, Sending, Receiving, Done, Failed };

struct HttpRequest;

HttpRequest* http_get(const char* url, const char* extra_headers = nullptr);
HttpRequest* http_post(const char* url, const char* content_type, const void* body, size_t len);
HttpState http_state(HttpRequest* r);
int http_status(HttpRequest* r);                         // e.g. 200
const char* http_header(HttpRequest* r, const char* name);   // case-insensitive, nullptr if absent
const uint8_t* http_body(HttpRequest* r, size_t* len);   // decoded body (de-chunked)
const char* http_final_url(HttpRequest* r);              // after redirects
const char* http_error(HttpRequest* r);
size_t http_received(HttpRequest* r);                    // body bytes so far
size_t http_content_length(HttpRequest* r);              // 0 if unknown
void http_free(HttpRequest* r);                          // cancels if still running

// URL helpers
struct Url {
    char scheme[8];      // "http" / "https"
    char host[256];
    uint16_t port;
    char path[2048];     // includes query, starts with '/'
};
bool url_parse(const char* url, Url* out);
// Resolve a possibly-relative reference against a base URL into out (size bytes).
void url_resolve(const char* base, const char* ref, char* out, size_t size);

}  // namespace net
