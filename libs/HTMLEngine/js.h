#pragma once
// JavaScript for pages (QuickJS from ECMAScript.dl + a DOM implemented in js.cpp).
#include "html.h"

namespace html {

struct JsEnv;

struct ScriptInfo {
    Node* node;
    String src;          // absolute URL ("" = inline)
    bool fetched = false;
    bool failed = false;
    bool module = false;
    bool dynamic = false;    // added to the DOM by a script
    bool requested = false;  // dynamic only: the browser has started fetching it
    bool ran = false;
};

JsEnv* js_create(Document* d);
void js_destroy(JsEnv* e);
// Scripts in document order. External ones must have node->value filled
// (fetched by the browser) before js_run_scripts().
Vector<ScriptInfo>& js_scripts(JsEnv* e);
void js_run_scripts(JsEnv* e);
// Run timers, animation frames, promise jobs. Returns true if the DOM changed
// (the caller should restyle + relayout).
bool js_tick(JsEnv* e, uint64_t now_ms);
// Dispatch an event to a node (bubbles). Returns true if preventDefault() was called.
bool js_dispatch(JsEnv* e, Node* target, const char* type);
// Fire a non-bubbling event (load, error) on a node.
void js_fire_event(JsEnv* e, Node* target, const char* type);
// Navigation requested by script (location.href = ...); empty if none. Clears it.
String js_take_navigation(JsEnv* e);
// Pending fetch()/XMLHttpRequest requests the browser should perform.
struct JsFetch { int id; String url; String method; String body; bool started; };
Vector<JsFetch>& js_fetches(JsEnv* e);
void js_fetch_done(JsEnv* e, int id, int status, const uint8_t* body, size_t len, const char* content_type);
// Messages from console.* (for a future devtools view); last N lines.
const Vector<String>& js_console(JsEnv* e);
// window.postMessage between frames. target = the <iframe> element addressed, nullptr = the parent page.
// port != "": addressed to a MessageChannel port held by some frame instead. ports = transferred ports (JSON).
// sibling != "": addressed to the frame of that name next to the sender (parent.frames[name]).
struct JsPost { Node* target; String data; String target_origin; String ports; String port; String sibling; int waited = 0; };   // data is JSON
Vector<JsPost>& js_posts(JsEnv* e);
// Fire 'message' on the window; source = the <iframe> it came from, nullptr = the parent page.
void js_deliver_message(JsEnv* e, const String& data, const String& origin, Node* source, const String& ports);
// Returns true if this page holds the port.
bool js_deliver_port(JsEnv* e, const String& port, const String& data, const String& ports);
// Make window.parent / window.top refer to the embedding page.
void js_set_framed(JsEnv* e, const char* name);
// Workers the page asked for. url "" = terminate the worker on that port.
struct JsWorkerReq { String url; String port; String peer; };
Vector<JsWorkerReq>& js_worker_requests(JsEnv* e);
// Turn a fresh environment into a dedicated worker talking over a port, then run its script.
void js_start_worker(JsEnv* e, const String& port, const String& peer, const char* code, size_t len, const char* url);
// Notify the page that an input's value changed (fires 'input').
void js_input_changed(JsEnv* e, Node* n);

}  // namespace html
