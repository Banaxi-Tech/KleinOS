#pragma once
// HTMLEngine.dl — a small HTML/CSS rendering engine for KleinOS.
// Parses HTML into a DOM, applies a user-agent stylesheet + page CSS
// (<style>, style="", simple selectors), lays out block/inline flow with
// word wrapping, lists, tables, images and form controls, and paints into a
// Canvas. No JavaScript.
#include "gfx/canvas.h"
#include "lib/kstring.h"
#include "lib/vector.h"

namespace html {

struct Attr { String name, value; };

struct VarMap;   // CSS custom properties (css.cpp)

constexpr int kUnset = -2147483647 - 1;   // "auto"/unset length

struct Style {
    Color color = 0xFF000000;
    Color background = 0;          // 0 = transparent
    bool bg_set = false;           // page set a background (form controls keep theirs otherwise)
    bool border_set = false;       // page set a border
    int font_size = 16;
    int weight = 400;
    bool bold = false;             // weight >= 600
    bool italic = false;
    bool mono = false;
    bool underline = false;
    bool strike = false;
    uint8_t text_transform = 0;    // 1 upper, 2 lower, 3 capitalize
    enum Display : uint8_t { Inline, Block, None, ListItem, Table, TableRow, TableCell, InlineBlock, Flex, Grid, Contents } display = Inline;
    enum Align : uint8_t { Left, Center, Right, Justify } text_align = Left;
    uint8_t valign = 0;            // vertical-align: 0 baseline, 1 top, 2 middle, 3 bottom
    enum Position : uint8_t { Static, Relative, Absolute, Fixed, Sticky } position = Static;
    bool pre = false;              // white-space: pre / pre-wrap
    bool nowrap = false;
    bool hidden = false;           // visibility: hidden
    int margin[4] = {0, 0, 0, 0};  // top right bottom left (px); kUnset = auto
    int padding[4] = {0, 0, 0, 0};
    int border_width[4] = {0, 0, 0, 0};
    Color border_color = 0;
    int border_radius = 0;
    int width = -1, max_width = -1, min_width = -1, height = -1, min_height = -1;   // px; -1 = auto
    int width_pct = -1, max_width_pct = -1, height_pct = -1;
    int offset[4] = {kUnset, kUnset, kUnset, kUnset};   // top right bottom left (positioned)
    int line_height_pct = 0;       // 0 = normal
    bool list_ordered = false;
    bool list_none = false;
    // flexbox
    bool flex_row = true;          // flex-direction: row (default) vs column
    bool flex_reverse = false;
    bool flex_wrap = false;
    uint8_t justify = 0;           // 0 start 1 center 2 end 3 space-between 4 space-around 5 space-evenly
    uint8_t align_items = 0;       // 0 stretch 1 start 2 center 3 end 4 baseline
    uint8_t align_self = 255;      // 255 = auto
    int gap_row = 0, gap_col = 0;
    float flex_grow = 0, flex_shrink = 1;
    int flex_basis = -1, flex_basis_pct = -1;
    int order = 0;
    // grid
    int grid_ncols = 0;
    int grid_col_px[16];
    float grid_col_fr[16];
    int grid_span = 1;             // grid-column: span N (-1 = full row)
    // decoration
    bool shadow = false;
    int shadow_x = 0, shadow_y = 0, shadow_blur = 0;
    Color shadow_color = 0;
    Color grad_a = 0, grad_b = 0;  // linear-gradient endpoints (0 = none)
    bool grad_horizontal = false;
    int bg_image = -1;             // index into Document::images (background-image: url())
    float opacity = 1.0f;
    float aspect_ratio = 0;        // width / height
    bool overflow_hidden = false;
    bool overflow_scroll = false;  // overflow: auto | scroll
    int max_height = -1;
    int max_height_pct = -1;
    VarMap* vars = nullptr;        // custom properties visible to this element (not owned)
};

struct Document;

struct Node {
    enum Type : uint8_t { Element, Text, Document } type = Element;
    String tag;                    // lowercase
    String text;                   // for Text nodes (entities decoded)
    Vector<Attr> attrs;
    Vector<Node*> children;
    Node* parent = nullptr;
    Style style;
    // form state
    String value;
    bool checked = false;
    int list_index = 0;            // for <li> numbering
    int layout_y = -1;             // set by layout (anchors)
    VarMap* own_vars = nullptr;    // custom properties declared on this element (owned)
    bool script_seen = false;      // <script>: already picked up (or must not run, e.g. from innerHTML)
    int js_id = -1;                // handle in the page's JavaScript environment
    int scroll_y = 0;              // scroll offset when this element is a scroll container
    struct Document* content_doc = nullptr;   // <iframe>: its document (owned by the app)
    int content_scroll = 0;
    const char* attr(const char* name) const;
    bool has_class(const char* cls) const;
};

// A positioned piece of output (document coordinates).
struct Fragment {
    enum Kind : uint8_t { Text, Box, Image, Input, Bullet, HRule, Svg, Frame } kind;
    Rect r;
    String text;
    Color color = 0, bg = 0, border_color = 0;
    int size = 16;
    Font font = Font::Regular;
    bool underline = false, strike = false;
    int radius = 0, border = 0;
    Node* node = nullptr;          // owning element (inputs, images)
    String href;                   // link target (absolute) if inside <a>
    String src;                    // image URL (absolute)
    int image = -1;                // index into Document::images (Image / background)
    int shadow_x = 0, shadow_y = 0, shadow_blur = 0;
    Color shadow_color = 0;
    Color grad_a = 0, grad_b = 0;
    bool grad_horizontal = false;
    int scroller = -1;             // index into Document::scrollers (clipped + scrolled), -1 = none
    bool fixed = false;            // position: fixed (doesn't move with page scroll)
    int box_of = -1;               // marker fragment carrying the box of scroller #box_of
};

struct Image {
    String url;
    Canvas* canvas = nullptr;      // nullptr until loaded
    bool requested = false, failed = false;
    bool svg = false;              // decode as SVG
    int retries = 0;               // network failures retried so far
    int want_w = 0, want_h = 0;    // render size for SVG
};

struct Document {
    Node* root = nullptr;
    String url;
    String title;
    Color background = 0xFFFFFFFF;
    Vector<Fragment> frags;
    int width = 0, height = 0;     // laid-out size
    Vector<Image> images;
    Node* focused_input = nullptr;
    struct Rule;                   // css
    Vector<Rule*> rules;
    Vector<String> stylesheet_urls;   // <link rel=stylesheet> to fetch
    Vector<Canvas*> inline_svgs;      // rendered inline <svg> elements (owned)
    int viewport_w = 1024;
    int viewport_h = 640;
    struct Scroller { Node* node; Rect box; int content_h; int parent; bool fixed; };
    Vector<Scroller> scrollers;
    bool scripting = false;           // JavaScript enabled: <noscript> is hidden
    Vector<Node*> orphans;            // nodes created/detached by scripts (freed with the document)
    ~Document();
};

// Parse HTML into a new document (url is used to resolve links).
Document* parse(const char* html, size_t len, const char* url, bool scripting = false);
// Plain text document (shown in a <pre>).
Document* parse_text(const char* text, size_t len, const char* url);
// Add an external stylesheet (after fetching it) and re-style.
void add_stylesheet(Document* d, const char* css, size_t len);
// Viewport size used for vw/vh units and media queries (call before parse()).
void set_viewport(int w, int h);
// Recompute styles after DOM changes (scripts).
void restyle(Document* d);
// Compute layout for a viewport width. Call again after resize / image loads.
void layout(Document* d, int width);
// Paint the region of the document that is visible at scroll offset (0,scroll_y) into c (viewport size).
void paint(Document* d, Canvas& c, int scroll_y);
// Hit testing in viewport coordinates + page scroll (x, y are document coords of the point; page_scroll is the page offset).
const Fragment* hit(Document* d, int x, int y, int page_scroll = 0);
// Element under the point, including plain boxes that hit() skips (for click events).
Node* hit_node(Document* d, int x, int y, int page_scroll = 0);
// Where a fragment is on screen, in viewport coordinates.
Rect fragment_view_rect(Document* d, const Fragment* f, int page_scroll);
// Scroll the innermost scroll container under the point. Returns false if none can scroll (then scroll the page).
bool scroll_at(Document* d, int x, int y, int page_scroll, int delta);
// Images needing download. The app sets Image::canvas (or failed) and calls layout() again.
Vector<Image>& images(Document* d);
// Forms: click on an input focuses it; key input edits it; returns a URL to navigate to when submitted (else empty).
bool click_input(Document* d, const Fragment* f);
String key_input(Document* d, uint32_t ch, bool enter, bool backspace);
// Find the y position of an element with id/name == anchor (for #fragments); -1 if none.
int anchor_y(Document* d, const char* anchor);
// Decode HTML entities in-place helper.
String decode_entities(const char* s, size_t len);

}  // namespace html
