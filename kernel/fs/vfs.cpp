#include "fs/vfs.h"
#include "drivers/rtc.h"
#include "lib/log.h"

namespace vfs {

static Node* g_root;
static ChangeHook g_hook;
static uint64_t g_gen;

static void changed(Node* n) {
    g_gen++;
    // only user data triggers persistence
    String p = path_of(n);
    if (g_hook && p.starts_with("/Users")) g_hook();
}

void init() {
    g_root = new Node();
    g_root->name = "";
    g_root->is_dir = true;
}

Node* root() { return g_root; }
uint64_t generation() { return g_gen; }
void set_change_hook(ChangeHook h) { g_hook = h; }

static Node* child(Node* dir, const char* name, size_t len) {
    for (Node* c : dir->children)
        if (c->name.size() == len && memcmp(c->name.c_str(), name, len) == 0) return c;
    return nullptr;
}

// Walk path; if create_dirs, create missing directories (all components when
// last_is_dir, else all but the last). Returns the node or nullptr.
static Node* walk(const char* path, bool create_dirs, bool last_is_dir) {
    if (!path || path[0] != '/') return nullptr;
    Node* cur = g_root;
    const char* p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* e = p;
        while (*e && *e != '/') e++;
        size_t len = e - p;
        bool last = true;
        for (const char* q = e; *q; q++) if (*q != '/') { last = false; break; }
        if (len == 1 && p[0] == '.') { p = e; continue; }
        if (len == 2 && p[0] == '.' && p[1] == '.') { if (cur->parent) cur = cur->parent; p = e; continue; }
        Node* c = child(cur, p, len);
        if (!c) {
            if (!create_dirs || (last && !last_is_dir)) return nullptr;
            c = new Node();
            c->name = String(p, len);
            c->is_dir = true;
            c->parent = cur;
            c->mtime = rtc_unix_time();
            cur->children.push(c);
        }
        if (!c->is_dir && !last) return nullptr;
        cur = c;
        p = e;
    }
    return cur;
}

Node* lookup(const char* path) { return walk(path, false, false); }

String path_of(const Node* n) {
    if (!n || n == g_root) return String("/");
    Vector<const Node*> chain;
    for (const Node* c = n; c && c != g_root; c = c->parent) chain.push(c);
    String s;
    for (size_t i = chain.size(); i > 0; i--) {
        s += "/";
        s += chain[i - 1]->name;
    }
    return s;
}

bool exists(const char* path) { return lookup(path) != nullptr; }
bool is_dir(const char* path) { Node* n = lookup(path); return n && n->is_dir; }

Node* mkdir(const char* path) {
    Node* n = walk(path, true, true);
    if (n) changed(n);
    return n;
}

bool read_file(const char* path, const uint8_t** data, size_t* size) {
    Node* n = lookup(path);
    if (!n || n->is_dir) return false;
    *data = n->data;
    *size = n->size;
    return true;
}

bool read_text(const char* path, String& out) {
    const uint8_t* d;
    size_t s;
    if (!read_file(path, &d, &s)) return false;
    out = String((const char*)d, s);
    return true;
}

static Node* get_or_create_file(const char* path) {
    String dir = dirname(path), name = basename(path);
    if (name.empty()) return nullptr;
    Node* parent = walk(dir.c_str(), true, true);
    if (!parent || !parent->is_dir) return nullptr;
    Node* n = child(parent, name.c_str(), name.size());
    if (n) return n->is_dir ? nullptr : n;
    n = new Node();
    n->name = name;
    n->parent = parent;
    parent->children.push(n);
    return n;
}

static void set_data(Node* n, const void* data, size_t size) {
    if (n->capacity) kfree(n->data);
    n->capacity = size ? size : 1;
    n->data = (uint8_t*)kmalloc(n->capacity);
    if (size) memcpy(n->data, data, size);
    n->size = size;
    n->readonly = false;
    n->mtime = rtc_unix_time();
}

bool write_file(const char* path, const void* data, size_t size) {
    Node* n = get_or_create_file(path);
    if (!n) return false;
    set_data(n, data, size);
    changed(n);
    return true;
}

bool write_text(const char* path, const char* text) { return write_file(path, text, strlen(text)); }

bool append_file(const char* path, const void* data, size_t size) {
    Node* n = get_or_create_file(path);
    if (!n) return false;
    if (!n->capacity || n->size + size > n->capacity) {
        size_t cap = (n->size + size) * 2 + 64;
        uint8_t* nd = (uint8_t*)kmalloc(cap);
        if (n->size) memcpy(nd, n->data, n->size);
        if (n->capacity) kfree(n->data);
        n->data = nd;
        n->capacity = cap;
    }
    memcpy(n->data + n->size, data, size);
    n->size += size;
    n->mtime = rtc_unix_time();
    changed(n);
    return true;
}

static void free_tree(Node* n) {
    for (Node* c : n->children) free_tree(c);
    if (n->capacity) kfree(n->data);
    delete n;
}

bool remove(const char* path) {
    Node* n = lookup(path);
    if (!n || n == g_root) return false;
    Node* parent = n->parent;
    parent->children.remove_value(n);
    free_tree(n);
    changed(parent);
    return true;
}

bool rename(const char* from, const char* to) {
    Node* n = lookup(from);
    if (!n || n == g_root || lookup(to)) return false;
    String dir = dirname(to), name = basename(to);
    Node* parent = walk(dir.c_str(), true, true);
    if (!parent) return false;
    for (Node* p = parent; p; p = p->parent) if (p == n) return false;   // into itself
    n->parent->children.remove_value(n);
    n->name = name;
    n->parent = parent;
    parent->children.push(n);
    changed(n);
    return true;
}

static bool copy_node(Node* src, const char* to) {
    if (src->is_dir) {
        if (!mkdir(to)) return false;
        for (Node* c : src->children) copy_node(c, join(to, c->name.c_str()).c_str());
        return true;
    }
    return write_file(to, src->data, src->size);
}

bool copy(const char* from, const char* to) {
    Node* n = lookup(from);
    if (!n || lookup(to)) return false;
    return copy_node(n, to);
}

Node* add_external(const char* path, const uint8_t* data, size_t size) {
    Node* n = get_or_create_file(path);
    if (!n) return nullptr;
    if (n->capacity) kfree(n->data);
    n->data = (uint8_t*)data;
    n->size = size;
    n->capacity = 0;
    n->readonly = true;
    return n;
}

String join(const char* dir, const char* name) {
    String s(dir);
    if (!s.ends_with("/")) s += "/";
    s += name;
    return s;
}

String dirname(const char* path) {
    String s(path);
    while (s.size() > 1 && s.ends_with("/")) s.erase(s.size() - 1, 1);
    long i = s.rfind('/');
    if (i <= 0) return String("/");
    return s.substr(0, i);
}

String basename(const char* path) {
    String s(path);
    while (s.size() > 1 && s.ends_with("/")) s.erase(s.size() - 1, 1);
    long i = s.rfind('/');
    return i < 0 ? s : s.substr(i + 1);
}

String extension(const char* path) {
    String b = basename(path);
    long i = b.rfind('.');
    if (i <= 0) return String();
    return b.substr(i + 1).lower();
}

// ------------------------------------------------------------ serialization
// Format: "KVFS" u32 version, then records: u8 type(1=dir,2=file) u16 pathlen path u64 mtime [u64 size data]
static void put(Vector<uint8_t>& o, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) o.push(b[i]);
}

static void ser(Node* n, const String& rel, Vector<uint8_t>& o) {
    for (Node* c : n->children) {
        String p = rel + "/" + c->name;
        uint8_t type = c->is_dir ? 1 : 2;
        uint16_t len = (uint16_t)p.size();
        put(o, &type, 1);
        put(o, &len, 2);
        put(o, p.c_str(), len);
        put(o, &c->mtime, 8);
        if (!c->is_dir) {
            uint64_t sz = c->size;
            put(o, &sz, 8);
            put(o, c->data, c->size);
        } else {
            ser(c, p, o);
        }
    }
}

bool serialize(const char* path, Vector<uint8_t>& out) {
    Node* n = lookup(path);
    if (!n || !n->is_dir) return false;
    out.clear();
    put(out, "KVFS", 4);
    uint32_t ver = 1;
    put(out, &ver, 4);
    ser(n, String(), out);
    uint8_t end = 0;
    put(out, &end, 1);
    return true;
}

bool deserialize(const char* path, const uint8_t* d, size_t size) {
    if (size < 9 || memcmp(d, "KVFS", 4) != 0) return false;
    size_t p = 8;
    ChangeHook saved = g_hook;
    g_hook = nullptr;
    while (p < size) {
        uint8_t type = d[p++];
        if (type == 0) break;
        if (p + 2 > size) break;
        uint16_t len;
        memcpy(&len, d + p, 2); p += 2;
        if (p + len + 8 > size) break;
        String full = String(path) + String((const char*)d + p, len);
        p += len;
        uint64_t mtime;
        memcpy(&mtime, d + p, 8); p += 8;
        if (type == 1) {
            Node* n = mkdir(full.c_str());
            if (n) n->mtime = mtime;
        } else {
            uint64_t sz;
            if (p + 8 > size) break;
            memcpy(&sz, d + p, 8); p += 8;
            if (p + sz > size) break;
            write_file(full.c_str(), d + p, sz);
            Node* n = lookup(full.c_str());
            if (n) n->mtime = mtime;
            p += sz;
        }
    }
    g_hook = saved;
    return true;
}

// ------------------------------------------------------------ initrd (ustar)
static uint64_t octal(const char* s, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n && s[i]; i++) {
        if (s[i] < '0' || s[i] > '7') continue;
        v = v * 8 + (s[i] - '0');
    }
    return v;
}

void mount_initrd(const uint8_t* tar, size_t size) {
    size_t off = 0;
    int files = 0;
    while (off + 512 <= size) {
        const char* h = (const char*)tar + off;
        if (!h[0]) break;
        char name[256];
        const char* prefix = h + 345;
        if (prefix[0]) snprintf(name, sizeof(name), "%.155s/%.100s", prefix, h);
        else snprintf(name, sizeof(name), "%.100s", h);
        uint64_t fsize = octal(h + 124, 12);
        char type = h[156];
        // normalize "./foo" -> "/foo"
        const char* n = name;
        while (n[0] == '.' && n[1] == '/') n += 2;
        char path[260];
        snprintf(path, sizeof(path), "/%s", n);
        size_t pl = strlen(path);
        while (pl > 1 && path[pl - 1] == '/') path[--pl] = 0;
        if (type == '5') {
            if (strcmp(path, "/") != 0 && strcmp(path, "/.") != 0) walk(path, true, true);
        } else if (type == '0' || type == 0) {
            Node* f = add_external(path, tar + off + 512, fsize);
            if (f) f->mtime = octal(h + 136, 12);
            files++;
        }
        off += 512 + ((fsize + 511) & ~511ull);
    }
    klog("vfs: initrd mounted, %d files", files);
}

}  // namespace vfs
