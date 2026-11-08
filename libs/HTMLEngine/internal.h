#pragma once
#include "html.h"

namespace html {

Node* parse_dom(const char* src, size_t len);
void free_node(Node* n);
void free_rules(Document* d);
void parse_css(Document* d, const char* css, size_t len);
void compute_styles(Document* d);
Node* find_tag(Node* n, const char* tag);
String inner_text(Node* n);
bool query_selector(Node* root, const char* selector, bool first_only, Vector<Node*>& out);
bool matches_selector(Node* n, const char* selector);
void serialize_node(Node* n, String& out);

}  // namespace html
