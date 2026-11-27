// Laptop batteries have no common hardware interface: the firmware describes them
// with small AML programs in its ACPI tables (usually reading the embedded
// controller). This is a small AML interpreter, just enough to run _BIF, _BST,
// _STA and _PSR, with step and depth limits so bad firmware can't hang the system.
#include <stdint.h>
#include <initializer_list>
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "boot/multiboot2.h"
#include "drivers/battery.h"
#include "drivers/pci.h"
#include "lib/heap.h"
#include "lib/log.h"
#include "lib/string.h"
#include "sys/driver.h"
#include "sys/system.h"

namespace {

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

constexpr u32 kBatteryHid = 0x0A0CD041, kEcHid = 0x090CD041;   // PNP0C0A, PNP0C09 (EISA encoded)
constexpr uint64_t kPollMs = 30000;
constexpr u32 kLoadSteps = 40000000, kCallSteps = 3000000, kMaxLoop = 1u << 20;
constexpr int kMaxCallDepth = 12, kMaxEvalDepth = 96;
constexpr size_t kArenaSize = 128 * 1024, kMaxBuffer = 64 * 1024;

struct Node;
enum class T : u8 { None, Int, Str, Buf, Pkg, Node, ElemRef, ByteRef };

struct Value {
    T t = T::None;
    u32 len = 0;
    union {
        u64 i = 0;
        const char* s;
        u8* b;
        Value* p;
        Node* n;
    };
};

enum class K : u8 { Scope, Device, Name, Method, Region, Field, IndexField, BufField, Mutex, Alias };

struct Node {
    char name[4];
    Node* parent, *child, *next;
    K kind;
    Value val;
    const u8* body;      // method
    u32 body_len;
    u8 args, builtin;
    u8 space;            // region
    u64 base, length;
    Node* region, *index, *data, *bank, *target;   // fields, alias
    u64 bank_value;
    u32 bit_off, bit_len;
    u8 flags;
};

u8 g_arena[kArenaSize];
size_t g_arena_pos;
bool g_abort, g_loading, g_int32, g_ec_bad;
u32 g_steps, g_step_limit;
int g_call_depth, g_eval_depth;
Node* g_root;

Value make_int(u64 v) { Value r; r.t = T::Int; r.i = g_int32 ? (v & 0xFFFFFFFFu) : v; return r; }
u64 all_ones() { return g_int32 ? 0xFFFFFFFFu : ~(u64)0; }

void* temp_alloc(size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (g_arena_pos + n > kArenaSize) { g_abort = true; return nullptr; }
    void* p = g_arena + g_arena_pos;
    g_arena_pos += n;
    memset(p, 0, n);
    return p;
}

// ---------------------------------------------------------------- namespace
Node* new_node(Node* parent, const char* name, K kind) {
    Node* n = (Node*)kmalloc(sizeof(Node));
    memset((void*)n, 0, sizeof(Node));
    memcpy(n->name, name, 4);
    n->parent = parent;
    n->kind = kind;
    if (parent) { n->next = parent->child; parent->child = n; }
    return n;
}

Node* child_of(Node* n, const char* name) {
    for (Node* c = n->child; c; c = c->next)
        if (!memcmp(c->name, name, 4)) return c->kind == K::Alias ? c->target : c;
    return nullptr;
}

struct Cur { const u8* p, *end; };

u8 next(Cur& c) {
    if (c.p >= c.end) { g_abort = true; return 0; }
    return *c.p++;
}

const u8* pkg_end(Cur& c) {
    const u8* start = c.p;
    u8 b = next(c);
    int more = b >> 6;
    u32 len = more ? b & 0x0F : b & 0x3F;
    for (int i = 0; i < more; i++) len |= (u32)next(c) << (4 + 8 * i);
    const u8* e = start + len;
    if (e > c.end || e < c.p) { g_abort = true; return c.end; }
    return e;
}

struct NameRef { bool root = false; int up = 0, count = 0; const u8* segs = nullptr; };

bool is_name_lead(u8 b) { return b == '\\' || b == '^' || b == 0x2E || b == 0x2F || (b >= 'A' && b <= 'Z') || b == '_'; }

NameRef parse_name(Cur& c) {
    NameRef r;
    if (c.p < c.end && *c.p == '\\') { r.root = true; c.p++; }
    while (c.p < c.end && *c.p == '^') { r.up++; c.p++; }
    u8 b = next(c);
    if (b == 0) return r;
    if (b == 0x2E) r.count = 2;
    else if (b == 0x2F) r.count = next(c);
    else { r.count = 1; c.p--; }
    r.segs = c.p;
    if ((size_t)(c.end - c.p) < (size_t)r.count * 4) { g_abort = true; r.count = 0; return r; }
    c.p += r.count * 4;
    return r;
}

Node* start_of(Node* scope, const NameRef& r) {
    Node* n = r.root ? g_root : scope;
    for (int i = 0; i < r.up && n; i++) n = n->parent;
    return n;
}

Node* descend(Node* n, const NameRef& r) {
    for (int i = 0; i < r.count && n; i++) n = child_of(n, (const char*)r.segs + i * 4);
    return n;
}

Node* lookup(Node* scope, const NameRef& r) {
    if (r.count == 0) return start_of(scope, r);
    Node* n = start_of(scope, r);
    if (r.root || r.up) return descend(n, r);
    for (; n; n = n->parent)
        if (Node* found = descend(n, r)) return found;
    return nullptr;
}

Node* create(Node* scope, const NameRef& r, K kind) {
    Node* n = start_of(scope, r);
    if (!n || r.count == 0) { g_abort = !n; return n; }
    for (int i = 0; i < r.count; i++) {
        const char* seg = (const char*)r.segs + i * 4;
        Node* c = nullptr;
        for (Node* k = n->child; k; k = k->next) if (!memcmp(k->name, seg, 4)) c = k;
        if (!c) c = new_node(n, seg, i + 1 == r.count ? kind : K::Scope);
        n = c;
    }
    return n;
}

// ---------------------------------------------------------------- hardware
bool ec_wait(u8 mask, bool want_set) {
    for (int i = 0; i < 100000; i++) {
        bool set = inb(0x66) & mask;
        if (set == want_set) return true;
        io_wait();
    }
    g_ec_bad = true;
    return false;
}

u8 ec_read(u8 addr) {
    if (g_ec_bad || !ec_wait(2, false)) return 0;
    outb(0x66, 0x80);
    if (!ec_wait(2, false)) return 0;
    outb(0x62, addr);
    if (!ec_wait(1, true)) return 0;
    return inb(0x62);
}

void ec_write(u8 addr, u8 v) {
    if (g_ec_bad || !ec_wait(2, false)) return;
    outb(0x66, 0x81);
    if (!ec_wait(2, false)) return;
    outb(0x62, addr);
    if (!ec_wait(2, false)) return;
    outb(0x62, v);
}

Value eval(Cur& c, struct Frame& f);
u64 to_int(const Value& v);
Value call(Node* m, Value* args, int n);
Value read_node(Node* n);

// bus 0 is what firmware uses for the regions batteries touch
bool pci_address(Node* region, u8* dev, u8* func) {
    for (Node* p = region->parent; p; p = p->parent)
        if (Node* adr = child_of(p, "_ADR")) {
            u64 v = to_int(read_node(adr));
            *dev = (u8)(v >> 16);
            *func = (u8)v;
            return true;
        }
    return false;
}

u64 region_read(Node* r, u64 off, int width) {
    u64 a = r->base + off;
    switch (r->space) {
    case 0:
        if (a + width > 0xFFFFFFFFull) return 0;
        if (width == 1) return *(volatile u8*)(uintptr_t)a;
        if (width == 2) return *(volatile uint16_t*)(uintptr_t)a;
        if (width == 4) return *(volatile u32*)(uintptr_t)a;
        return *(volatile u32*)(uintptr_t)a | (u64)*(volatile u32*)(uintptr_t)(a + 4) << 32;
    case 1:
        if (a > 0xFFFF) return 0;
        return width == 1 ? inb((uint16_t)a) : width == 2 ? inw((uint16_t)a) : inl((uint16_t)a);
    case 2: {
        u8 dev = 0, func = 0;
        if (!pci_address(r, &dev, &func) || a > 255) return 0;
        u32 v = pci_read32(0, dev, func, (u8)(a & ~3u));
        return width >= 4 ? v : (v >> ((a & 3) * 8)) & ((1u << (width * 8)) - 1);
    }
    case 3: {
        u64 v = 0;
        for (int i = 0; i < width && a + i < 256; i++) v |= (u64)ec_read((u8)(a + i)) << (i * 8);
        return v;
    }
    default: return 0;
    }
}

void region_write(Node* r, u64 off, int width, u64 v) {
    u64 a = r->base + off;
    switch (r->space) {
    case 0:
        if (a + width > 0xFFFFFFFFull) return;
        if (width == 1) *(volatile u8*)(uintptr_t)a = (u8)v;
        else if (width == 2) *(volatile uint16_t*)(uintptr_t)a = (uint16_t)v;
        else { *(volatile u32*)(uintptr_t)a = (u32)v; if (width == 8) *(volatile u32*)(uintptr_t)(a + 4) = (u32)(v >> 32); }
        return;
    case 1:
        if (a > 0xFFFF) return;
        if (width == 1) outb((uint16_t)a, (u8)v); else if (width == 2) outw((uint16_t)a, (uint16_t)v); else outl((uint16_t)a, (u32)v);
        return;
    case 3:
        for (int i = 0; i < width && a + i < 256; i++) ec_write((u8)(a + i), (u8)(v >> (i * 8)));
        return;
    default: return;   // PCI config writes are never needed to read a battery
    }
}

// ---------------------------------------------------------------- fields
int access_width(const Node* f) {
    if (f->region && f->region->space == 3) return 1;
    switch (f->flags & 0x0F) {
    case 2: return 2;
    case 3: return 4;
    case 4: return 8;
    default: return 1;
    }
}

u64 field_get(Node* f);
void field_put(Node* f, u64 v);

u64 unit_read(Node* f, u64 off, int w) {
    if (f->kind == K::IndexField) { field_put(f->index, off); return field_get(f->data); }
    if (f->bank) field_put(f->bank, f->bank_value);
    return f->region ? region_read(f->region, off, w) : 0;
}

void unit_write(Node* f, u64 off, int w, u64 v) {
    if (f->kind == K::IndexField) { field_put(f->index, off); field_put(f->data, v); return; }
    if (f->bank) field_put(f->bank, f->bank_value);
    if (f->region) region_write(f->region, off, w, v);
}

u64 low_mask(u32 bits) { return bits >= 64 ? ~(u64)0 : ((u64)1 << bits) - 1; }

u64 bits_read(Node* f, u32 pos, u32 bits) {
    int w = access_width(f);
    u64 out = 0;
    u32 got = 0;
    while (got < bits && !g_abort) {
        u32 at = pos + got, unit = at / (w * 8), shift = at - unit * w * 8;
        u32 take = bits - got < (u32)w * 8 - shift ? bits - got : w * 8 - shift;
        u64 v = unit_read(f, (u64)unit * w, w);
        out |= ((v >> shift) & low_mask(take)) << got;
        got += take;
    }
    return out;
}

void bits_write(Node* f, u32 pos, u32 bits, u64 v) {
    int w = access_width(f);
    u32 done = 0;
    while (done < bits && !g_abort) {
        u32 at = pos + done, unit = at / (w * 8), shift = at - unit * w * 8;
        u32 take = bits - done < (u32)w * 8 - shift ? bits - done : w * 8 - shift;
        u64 mask = low_mask(take) << shift, cur = 0;
        if (take != (u32)w * 8) {
            int rule = (f->flags >> 5) & 3;
            cur = rule == 1 ? ~(u64)0 : rule == 2 ? 0 : unit_read(f, (u64)unit * w, w);
        }
        unit_write(f, (u64)unit * w, w, (cur & ~mask) | ((v >> done) << shift & mask));
        done += take;
    }
}

u64 field_get(Node* f) { return bits_read(f, f->bit_off, f->bit_len > 64 ? 64 : f->bit_len); }
void field_put(Node* f, u64 v) { bits_write(f, f->bit_off, f->bit_len > 64 ? 64 : f->bit_len, v); }

// ---------------------------------------------------------------- values
Value persist(const Value& v) {
    Value r = v;
    switch (v.t) {
    case T::Buf: r.b = (u8*)kmalloc(v.len + 1); memcpy(r.b, v.b, v.len); break;
    case T::Pkg:
        r.p = (Value*)kmalloc(sizeof(Value) * (v.len + 1));
        for (u32 i = 0; i < v.len; i++) r.p[i] = persist(v.p[i]);
        break;
    default: break;
    }
    return r;
}

Value resolve(const Value& v) {
    if (v.t == T::ElemRef) return *v.p;
    if (v.t == T::ByteRef) return make_int(*v.b);
    if (v.t == T::Node) return read_node(v.n);
    return v;
}

u64 to_int(const Value& v0) {
    Value v = resolve(v0);
    switch (v.t) {
    case T::Int: return v.i;
    case T::Buf: {
        u64 r = 0;
        for (u32 i = 0; i < v.len && i < 8; i++) r |= (u64)v.b[i] << (i * 8);
        return r;
    }
    case T::Str: return strtoul(v.s, nullptr, 0);
    default: return 0;
    }
}

Value read_node(Node* n) {
    switch (n->kind) {
    case K::Name: return n->val;
    case K::Field: case K::IndexField:
        if (n->bit_len > 64) {
            Value b;
            b.t = T::Buf;
            b.len = (n->bit_len + 7) / 8;
            if (b.len > kMaxBuffer || !(b.b = (u8*)temp_alloc(b.len))) { g_abort = true; return Value(); }
            for (u32 i = 0; i < b.len; i++) b.b[i] = (u8)bits_read(n, n->bit_off + i * 8, n->bit_len - i * 8 < 8 ? n->bit_len - i * 8 : 8);
            return b;
        }
        return make_int(field_get(n));
    case K::BufField: {
        u64 out = 0;
        for (u32 i = 0; i < n->bit_len && i < 64; i++) {
            u32 bit = n->bit_off + i;
            if (bit / 8 < n->val.len && (n->val.b[bit / 8] >> (bit % 8) & 1)) out |= (u64)1 << i;
        }
        return make_int(out);
    }
    case K::Method: return call(n, nullptr, 0);
    default: { Value r; r.t = T::Node; r.n = n; return r; }
    }
}

void write_node(Node* n, const Value& v0) {
    Value v = resolve(v0);
    switch (n->kind) {
    case K::Name:
        if (n->val.t == v.t && v.t == T::Buf && n->val.len == v.len) memcpy(n->val.b, v.b, v.len);
        else n->val = persist(v);
        break;
    case K::Field: case K::IndexField: field_put(n, to_int(v)); break;
    case K::BufField:
        for (u32 i = 0; i < n->bit_len && i < 64; i++) {
            u32 bit = n->bit_off + i;
            if (bit / 8 >= n->val.len) break;
            u8 m = (u8)(1 << (bit % 8));
            if (to_int(v) >> i & 1) n->val.b[bit / 8] |= m; else n->val.b[bit / 8] &= (u8)~m;
        }
        break;
    default: break;
    }
}

void write_ref(const Value& r, const Value& v) {
    if (r.t == T::ElemRef) *r.p = persist(resolve(v));
    else if (r.t == T::ByteRef) *r.b = (u8)to_int(v);
    else if (r.t == T::Node) write_node(r.n, v);
}

// ---------------------------------------------------------------- execution
enum class Flow : u8 { Run, Return, Break, Continue };

struct Frame {
    Value locals[8], args[7];
    Node* scope = nullptr;
    Flow flow = Flow::Run;
    Value ret;
    bool container = false;   // walking a table, not running a method
};

void exec(const u8* p, const u8* end, Frame& f) {
    Cur c{p, end};
    while (c.p < end && !g_abort && f.flow == Flow::Run) {
        eval(c, f);
        if (f.container) g_arena_pos = 0;   // nothing temporary outlives a statement here
    }
}

void store_target(Cur& c, Frame& f, const Value& v) {
    if (c.p >= c.end) { g_abort = true; return; }
    u8 b = *c.p;
    if (b == 0) { c.p++; return; }
    if (b >= 0x60 && b <= 0x67) { c.p++; f.locals[b - 0x60] = resolve(v); return; }
    if (b >= 0x68 && b <= 0x6E) {
        c.p++;
        Value& a = f.args[b - 0x68];
        if (a.t == T::ElemRef || a.t == T::ByteRef || a.t == T::Node) write_ref(a, v); else a = resolve(v);
        return;
    }
    if (b == 0x5B && c.p + 1 < c.end && c.p[1] == 0x31) { c.p += 2; return; }   // Debug
    if (is_name_lead(b)) {
        NameRef r = parse_name(c);
        if (Node* n = lookup(f.scope, r)) write_node(n, v);
        return;
    }
    write_ref(eval(c, f), v);
}

// A name without reading or calling it.
Value super_name(Cur& c, Frame& f) {
    if (c.p < c.end && is_name_lead(*c.p)) {
        Value r;
        NameRef nr = parse_name(c);
        if (Node* n = lookup(f.scope, nr)) { r.t = T::Node; r.n = n; }
        return r;
    }
    return eval(c, f);
}

void parse_fields(Cur& c, const u8* end, Frame& f, Node* region, Node* index, Node* data, Node* bank, u64 bank_value, u8 flags, K kind) {
    u32 bit = 0;
    (void)f;
    while (c.p < end && !g_abort) {
        u8 b = *c.p;
        if (b == 0x00) { c.p++; const u8* s = c.p; u8 l = next(c); int more = l >> 6; u32 n = more ? l & 0xF : l & 0x3F; for (int i = 0; i < more; i++) n |= (u32)next(c) << (4 + 8 * i); (void)s; bit += n; continue; }
        if (b == 0x01) { c.p++; flags = (u8)((flags & 0xF0) | (next(c) & 0x0F)); next(c); continue; }
        if (b == 0x02) { c.p++; if (c.p < end && is_name_lead(*c.p)) parse_name(c); else { const u8* e = pkg_end(c); c.p = e; } continue; }
        if (b == 0x03) { c.p++; next(c); next(c); next(c); continue; }
        char name[4];
        for (int i = 0; i < 4; i++) name[i] = (char)next(c);
        u8 l = next(c);
        int more = l >> 6;
        u32 n = more ? l & 0xF : l & 0x3F;
        for (int i = 0; i < more; i++) n |= (u32)next(c) << (4 + 8 * i);
        Node* node = child_of(f.scope, name);
        if (!node) node = new_node(f.scope, name, kind);
        node->kind = kind;
        node->region = region; node->index = index; node->data = data; node->bank = bank; node->bank_value = bank_value;
        node->bit_off = bit; node->bit_len = n; node->flags = flags;
        bit += n;
    }
    c.p = end;
}

void make_buf_field(Cur& c, Frame& f, u32 fixed_bits, bool byte_index) {
    Value src = resolve(eval(c, f));
    u64 idx = to_int(eval(c, f));
    u32 bits = fixed_bits ? fixed_bits : (u32)to_int(eval(c, f));
    NameRef nr = parse_name(c);
    if (g_abort) return;
    Node* n = create(f.scope, nr, K::BufField);
    if (!n) return;
    n->kind = K::BufField;
    n->val = src;
    n->bit_off = (u32)(byte_index ? idx * 8 : idx);
    n->bit_len = bits;
}

Value concat(const Value& a0, const Value& b0) {
    Value a = resolve(a0), b = resolve(b0), r;
    if (a.t == T::Str) {
        const char* bs = b.t == T::Str ? b.s : "";
        size_t la = strlen(a.s), lb = strlen(bs);
        char* s = (char*)temp_alloc(la + lb + 1);
        if (!s) return r;
        memcpy(s, a.s, la);
        memcpy(s + la, bs, lb);
        r.t = T::Str;
        r.s = s;
        return r;
    }
    if (a.t == T::Buf) {
        u32 lb = b.t == T::Buf ? b.len : 0;
        u8* d = (u8*)temp_alloc(a.len + lb + 1);
        if (!d) return r;
        memcpy(d, a.b, a.len);
        if (lb) memcpy(d + a.len, b.b, lb);
        r.t = T::Buf;
        r.len = a.len + lb;
        r.b = d;
    }
    return r;
}

int compare(const Value& a0, const Value& b0) {
    Value a = resolve(a0), b = resolve(b0);
    if (a.t == T::Str && b.t == T::Str) return strcmp(a.s, b.s);
    if (a.t == T::Buf && b.t == T::Buf) {
        u32 n = a.len < b.len ? a.len : b.len;
        int d = memcmp(a.b, b.b, n);
        return d ? d : (int)a.len - (int)b.len;
    }
    u64 x = to_int(a), y = to_int(b);
    return x < y ? -1 : x > y;
}

Value binary(Cur& c, Frame& f, u8 op) {
    u64 a = to_int(eval(c, f)), b = to_int(eval(c, f)), r = 0;
    switch (op) {
    case 0x72: r = a + b; break;
    case 0x74: r = a - b; break;
    case 0x77: r = a * b; break;
    case 0x79: r = b >= 64 ? 0 : a << b; break;
    case 0x7A: r = b >= 64 ? 0 : a >> b; break;
    case 0x7B: r = a & b; break;
    case 0x7C: r = ~(a & b); break;
    case 0x7D: r = a | b; break;
    case 0x7E: r = ~(a | b); break;
    case 0x7F: r = a ^ b; break;
    case 0x85: r = b ? a % b : 0; break;
    }
    Value v = make_int(r);
    store_target(c, f, v);
    return v;
}

Value container(Cur& c, Frame& f, K kind, int skip) {
    const u8* end = pkg_end(c);
    NameRef nr = parse_name(c);
    for (int i = 0; i < skip; i++) next(c);
    if (g_abort) { return Value(); }
    Node* n = lookup(f.scope, nr);
    if (!n || (nr.count == 1 && !nr.root && !nr.up && n->parent != f.scope && kind != K::Scope)) n = create(f.scope, nr, kind);
    if (!n) return Value();
    if (kind != K::Scope) n->kind = kind;
    Node* saved = f.scope;
    f.scope = n;
    exec(c.p, end, f);
    f.scope = saved;
    if (g_abort && g_loading && g_steps < g_step_limit) g_abort = false;   // keep the rest of the table
    c.p = end;
    return Value();
}

Value eval_ext(Cur& c, Frame& f) {
    u8 op = next(c);
    switch (op) {
    case 0x01: { NameRef r = parse_name(c); next(c); if (!g_abort) if (Node* n = create(f.scope, r, K::Mutex)) n->kind = K::Mutex; return Value(); }
    case 0x02: { NameRef r = parse_name(c); if (!g_abort) if (Node* n = create(f.scope, r, K::Mutex)) n->kind = K::Mutex; return Value(); }
    case 0x12: {
        Value s = super_name(c, f);
        bool exists = s.t != T::None;
        if (exists) store_target(c, f, s); else store_target(c, f, Value());
        return make_int(exists ? all_ones() : 0);
    }
    case 0x13: make_buf_field(c, f, 0, false); return Value();
    case 0x21: case 0x22: eval(c, f); return Value();           // Stall, Sleep: nothing here waits on them
    case 0x23: super_name(c, f); next(c); next(c); return make_int(0);   // Acquire
    case 0x24: case 0x26: case 0x27: super_name(c, f); return Value();   // Signal, Reset, Release
    case 0x25: super_name(c, f); eval(c, f); return make_int(0);         // Wait
    case 0x28: case 0x29: { Value v = eval(c, f); store_target(c, f, v); return v; }
    case 0x30: return make_int(2);
    case 0x31: return Value();
    case 0x32: next(c); next(c); next(c); next(c); next(c); eval(c, f); return Value();
    case 0x33: return make_int(timer_ms() * 10000);
    case 0x80: {
        NameRef r = parse_name(c);
        u8 space = next(c);
        u64 base = to_int(eval(c, f)), len = to_int(eval(c, f));
        if (g_abort) return Value();
        if (Node* n = create(f.scope, r, K::Region)) { n->kind = K::Region; n->space = space; n->base = base; n->length = len; }
        return Value();
    }
    case 0x81: {
        const u8* end = pkg_end(c);
        NameRef r = parse_name(c);
        u8 flags = next(c);
        Node* region = lookup(f.scope, r);
        if (region) parse_fields(c, end, f, region, nullptr, nullptr, nullptr, 0, flags, K::Field);
        c.p = end;
        return Value();
    }
    case 0x82: return container(c, f, K::Device, 0);
    case 0x83: return container(c, f, K::Device, 6);   // Processor
    case 0x84: return container(c, f, K::Device, 3);   // PowerResource
    case 0x85: return container(c, f, K::Device, 0);   // ThermalZone
    case 0x86: {
        const u8* end = pkg_end(c);
        NameRef a = parse_name(c), b = parse_name(c);
        u8 flags = next(c);
        Node* index = lookup(f.scope, a), *data = lookup(f.scope, b);
        if (index && data) parse_fields(c, end, f, nullptr, index, data, nullptr, 0, flags, K::IndexField);
        c.p = end;
        return Value();
    }
    case 0x87: {
        const u8* end = pkg_end(c);
        NameRef a = parse_name(c), b = parse_name(c);
        u64 bank_value = to_int(eval(c, f));
        u8 flags = next(c);
        Node* region = lookup(f.scope, a), *bank = lookup(f.scope, b);
        if (region && bank) parse_fields(c, end, f, region, nullptr, nullptr, bank, bank_value, flags, K::Field);
        c.p = end;
        return Value();
    }
    case 0x88: { parse_name(c); eval(c, f); eval(c, f); eval(c, f); return Value(); }
    default:
        klog("battery: unsupported AML opcode 5B %02x", op);
        g_abort = true;
        return Value();
    }
}

Value eval_inner(Cur& c, Frame& f) {
    u8 op = next(c);
    if (g_abort) return Value();
    if (is_name_lead(op)) {
        c.p--;
        NameRef r = parse_name(c);
        Node* n = lookup(f.scope, r);
        if (!n) return Value();
        if (n->kind != K::Method) return read_node(n);
        Value args[7];
        for (int i = 0; i < n->args; i++) args[i] = eval(c, f);
        return call(n, args, n->args);
    }
    if (op >= 0x60 && op <= 0x67) return f.locals[op - 0x60];
    if (op >= 0x68 && op <= 0x6E) return f.args[op - 0x68];
    switch (op) {
    case 0x00: return make_int(0);
    case 0x01: return make_int(1);
    case 0xFF: return make_int(all_ones());
    case 0x0A: return make_int(next(c));
    case 0x0B: { u64 v = next(c); v |= (u64)next(c) << 8; return make_int(v); }
    case 0x0C: { u64 v = 0; for (int i = 0; i < 4; i++) v |= (u64)next(c) << (i * 8); return make_int(v); }
    case 0x0E: { u64 v = 0; for (int i = 0; i < 8; i++) v |= (u64)next(c) << (i * 8); return make_int(v); }
    case 0x0D: { Value v; v.t = T::Str; v.s = (const char*)c.p; while (next(c) && !g_abort) {} return v; }
    case 0x06: {
        NameRef src = parse_name(c), dst = parse_name(c);
        if (g_abort) return Value();
        Node* target = lookup(f.scope, src);
        Node* n = create(f.scope, dst, K::Alias);
        if (n && target && n != target) { n->kind = K::Alias; n->target = target; }
        return Value();
    }
    case 0x08: {
        NameRef r = parse_name(c);
        Value v = resolve(eval(c, f));
        if (g_abort) return Value();
        if (Node* n = create(f.scope, r, K::Name)) { n->kind = K::Name; n->val = persist(v); }
        return Value();
    }
    case 0x10: return container(c, f, K::Scope, 0);
    case 0x11: {
        const u8* end = pkg_end(c);
        u64 size = to_int(eval(c, f));
        Value v;
        if (size > kMaxBuffer || g_abort) { g_abort = true; return v; }
        v.b = (u8*)temp_alloc((size_t)size + 1);
        if (!v.b) return Value();
        v.t = T::Buf;
        v.len = (u32)size;
        size_t have = (size_t)(end - c.p);
        memcpy(v.b, c.p, have < size ? have : (size_t)size);
        c.p = end;
        return v;
    }
    case 0x12: case 0x13: {
        const u8* end = pkg_end(c);
        u32 count = op == 0x12 ? next(c) : (u32)to_int(eval(c, f));
        Value v;
        if (count > 4096 || g_abort) { g_abort = true; return v; }
        v.p = (Value*)temp_alloc(sizeof(Value) * (count + 1));
        if (!v.p) return Value();
        v.t = T::Pkg;
        v.len = count;
        Cur s{c.p, end};
        for (u32 i = 0; i < count && s.p < end && !g_abort; i++) {
            if (is_name_lead(*s.p)) v.p[i] = super_name(s, f);
            else v.p[i] = eval(s, f);
        }
        c.p = end;
        return v;
    }
    case 0x14: {
        const u8* end = pkg_end(c);
        NameRef r = parse_name(c);
        u8 flags = next(c);
        if (g_abort) return Value();
        if (Node* n = create(f.scope, r, K::Method)) { n->kind = K::Method; n->args = flags & 7; n->body = c.p; n->body_len = (u32)(end - c.p); }
        c.p = end;
        return Value();
    }
    case 0x15: parse_name(c); next(c); next(c); return Value();   // External
    case 0x5B: return eval_ext(c, f);
    case 0x70: { Value v = resolve(eval(c, f)); store_target(c, f, v); return v; }
    case 0x9D: { Value v = resolve(eval(c, f)); store_target(c, f, v); return v; }
    case 0x71: return super_name(c, f);
    case 0x72: case 0x74: case 0x77: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: case 0x85: return binary(c, f, op);
    case 0x73: case 0x84: { Value a = eval(c, f), b = eval(c, f); Value v = concat(a, b); store_target(c, f, v); return v; }
    case 0x75: case 0x76: {
        Cur again = c;
        u64 v = to_int(eval(again, f));
        Value nv = make_int(op == 0x75 ? v + 1 : v - 1);
        store_target(c, f, nv);
        return nv;
    }
    case 0x78: {
        u64 a = to_int(eval(c, f)), b = to_int(eval(c, f));
        store_target(c, f, make_int(b ? a % b : 0));
        Value q = make_int(b ? a / b : 0);
        store_target(c, f, q);
        return q;
    }
    case 0x80: { Value v = make_int(~to_int(eval(c, f))); store_target(c, f, v); return v; }
    case 0x81: case 0x82: {
        u64 a = to_int(eval(c, f));
        int r = 0;
        for (int i = 0; i < 64; i++) if (a >> i & 1) { r = i + 1; if (op == 0x82) break; }
        Value v = make_int(r);
        store_target(c, f, v);
        return v;
    }
    case 0x83: return resolve(eval(c, f));
    case 0x86: super_name(c, f); eval(c, f); return Value();   // Notify
    case 0x87: {
        Value v = resolve(super_name(c, f));
        return make_int(v.t == T::Str ? strlen(v.s) : v.len);
    }
    case 0x88: {
        Value src = resolve(eval(c, f));
        u64 i = to_int(eval(c, f));
        Value r;
        if (src.t == T::Pkg && i < src.len) { r.t = T::ElemRef; r.p = &src.p[i]; }
        else if (src.t == T::Buf && i < src.len) { r.t = T::ByteRef; r.b = &src.b[i]; }
        store_target(c, f, r);
        return r;
    }
    case 0x89: eval(c, f); next(c); eval(c, f); next(c); eval(c, f); eval(c, f); return make_int(all_ones());   // Match
    case 0x8A: make_buf_field(c, f, 32, true); return Value();
    case 0x8B: make_buf_field(c, f, 16, true); return Value();
    case 0x8C: make_buf_field(c, f, 8, true); return Value();
    case 0x8D: make_buf_field(c, f, 1, false); return Value();
    case 0x8F: make_buf_field(c, f, 64, true); return Value();
    case 0x8E: {
        Value v = resolve(super_name(c, f));
        return make_int(v.t == T::Int ? 1 : v.t == T::Str ? 2 : v.t == T::Buf ? 3 : v.t == T::Pkg ? 4 : 0);
    }
    case 0x90: { u64 a = to_int(eval(c, f)), b = to_int(eval(c, f)); return make_int(a && b ? all_ones() : 0); }
    case 0x91: { u64 a = to_int(eval(c, f)), b = to_int(eval(c, f)); return make_int(a || b ? all_ones() : 0); }
    case 0x92: return make_int(to_int(eval(c, f)) ? 0 : all_ones());
    case 0x93: { Value a = eval(c, f), b = eval(c, f); return make_int(compare(a, b) == 0 ? all_ones() : 0); }
    case 0x94: { Value a = eval(c, f), b = eval(c, f); return make_int(compare(a, b) > 0 ? all_ones() : 0); }
    case 0x95: { Value a = eval(c, f), b = eval(c, f); return make_int(compare(a, b) < 0 ? all_ones() : 0); }
    case 0x96: case 0x97: case 0x98: case 0x99: {
        Value v = resolve(eval(c, f));
        if (op == 0x99) v = make_int(to_int(v));
        store_target(c, f, v);
        return v;
    }
    case 0x9C: { Value v = resolve(eval(c, f)); eval(c, f); store_target(c, f, v); return v; }
    case 0x9E: { Value v = resolve(eval(c, f)); eval(c, f); eval(c, f); store_target(c, f, v); return v; }
    case 0x9F: f.flow = Flow::Continue; return Value();
    case 0xA0: {
        const u8* end = pkg_end(c);
        Cur s{c.p, end};
        bool cond = to_int(eval(s, f)) != 0;
        if (cond && !g_abort) exec(s.p, end, f);
        c.p = end;
        if (c.p < c.end && *c.p == 0xA1) {
            c.p++;
            const u8* else_end = pkg_end(c);
            if (!cond && !g_abort) exec(c.p, else_end, f);
            c.p = else_end;
        }
        return Value();
    }
    case 0xA1: { const u8* end = pkg_end(c); c.p = end; return Value(); }
    case 0xA2: {
        const u8* end = pkg_end(c);
        const u8* cond = c.p;
        for (u32 n = 0; n < kMaxLoop && !g_abort; n++) {
            Cur s{cond, end};
            if (!to_int(eval(s, f)) || g_abort) break;
            exec(s.p, end, f);
            if (f.flow == Flow::Break) { f.flow = Flow::Run; break; }
            if (f.flow == Flow::Continue) f.flow = Flow::Run;
            if (f.flow == Flow::Return) break;
        }
        c.p = end;
        return Value();
    }
    case 0xA3: case 0xCC: return Value();
    case 0xA4: f.ret = resolve(eval(c, f)); f.flow = Flow::Return; return Value();
    case 0xA5: f.flow = Flow::Break; return Value();
    default:
        klog("battery: unsupported AML opcode %02x", op);
        g_abort = true;
        return Value();
    }
}

Value eval(Cur& c, Frame& f) {
    if (g_abort) return Value();
    if (++g_steps > g_step_limit || g_eval_depth >= kMaxEvalDepth) { g_abort = true; return Value(); }
    g_eval_depth++;
    Value v = eval_inner(c, f);
    g_eval_depth--;
    return v;
}

Value call(Node* m, Value* args, int n) {
    if (m->builtin == 1) {
        // _OSI: firmware hides features from systems it doesn't recognise
        Value a = n ? resolve(args[0]) : Value();
        return make_int(a.t == T::Str && !strncmp(a.s, "Windows", 7) ? all_ones() : 0);
    }
    if (g_call_depth >= kMaxCallDepth) { g_abort = true; return Value(); }
    g_call_depth++;
    Frame f;
    f.scope = m;
    for (int i = 0; i < n && i < 7; i++) f.args[i] = args[i];
    exec(m->body, m->body + m->body_len, f);
    g_call_depth--;
    return f.ret;
}

// Runs a method or reads an object from outside AML.
Value run_child(Node* dev, const char* name, bool* found = nullptr, Value* args = nullptr, int nargs = 0) {
    Node* n = child_of(dev, name);
    if (found) *found = n != nullptr;
    if (!n) return Value();
    g_steps = 0;
    g_step_limit = kCallSteps;
    g_abort = false;
    g_call_depth = g_eval_depth = 0;
    if (n->kind == K::Method) return resolve(call(n, args, nargs));
    return resolve(read_node(n));
}

// ---------------------------------------------------------------- tables
struct Sdt { char sig[4]; u32 length; u8 revision; };

const Sdt* table_at(u64 addr) {
    if (!addr || addr > 0xFFFFF000ull) return nullptr;
    const Sdt* t = (const Sdt*)(uintptr_t)addr;
    return t->length >= 36 && t->length < 32 * 1024 * 1024 ? t : nullptr;
}

const u8* find_rsdp() {
    if (boot_info().acpi_rsdp) return (const u8*)boot_info().acpi_rsdp;
    auto scan = [](uintptr_t from, uintptr_t to) -> const u8* {
        for (uintptr_t a = from; a + 20 <= to; a += 16) {
            const u8* p = (const u8*)a;
            if (memcmp(p, "RSD PTR ", 8)) continue;
            u8 sum = 0;
            for (int i = 0; i < 20; i++) sum += p[i];
            if (!sum) return p;
        }
        return nullptr;
    };
    uintptr_t ebda = (uintptr_t)*(volatile uint16_t*)0x40E << 4;
    const u8* found = ebda ? scan(ebda, ebda + 1024) : nullptr;
    return found ? found : scan(0xE0000, 0x100000);
}

void load_table(const Sdt* t) {
    const u8* start = (const u8*)t + 36, *end = (const u8*)t + t->length;
    Frame f;
    f.scope = g_root;
    f.container = true;
    g_abort = false;
    g_steps = 0;
    g_step_limit = kLoadSteps;
    g_loading = true;
    exec(start, end, f);
    g_loading = false;
    if (g_abort) klog("battery: stopped early in table %.4s", t->sig);
    g_abort = false;
    g_arena_pos = 0;
}

bool load_tables() {
    const u8* rsdp = find_rsdp();
    if (!rsdp) { klog("battery: no ACPI tables found"); return false; }
    u64 root;
    int entry = 4;
    if (rsdp[15] >= 2 && *(const u64*)(rsdp + 24)) { root = *(const u64*)(rsdp + 24); entry = 8; }
    else root = *(const u32*)(rsdp + 16);
    const Sdt* top = table_at(root);
    if (!top) { klog("battery: bad ACPI root table"); return false; }
    int count = (int)(top->length - 36) / entry;
    const u8* list = (const u8*)top + 36;
    auto entry_at = [&](int i) -> u64 { return entry == 8 ? *(const u64*)(list + i * 8) : *(const u32*)(list + i * 4); };
    const Sdt* dsdt = nullptr;
    for (int i = 0; i < count; i++) {
        const Sdt* t = table_at(entry_at(i));
        if (!t || memcmp(t->sig, "FACP", 4)) continue;
        const u8* fadt = (const u8*)t;
        u64 x = t->length >= 148 ? *(const u64*)(fadt + 140) : 0;
        dsdt = table_at(x ? x : *(const u32*)(fadt + 40));
    }
    if (!dsdt) { klog("battery: no DSDT"); return false; }
    g_int32 = dsdt->revision < 2;
    load_table(dsdt);
    for (int i = 0; i < count; i++) {
        const Sdt* t = table_at(entry_at(i));
        if (t && !memcmp(t->sig, "SSDT", 4)) load_table(t);
    }
    return true;
}

// ---------------------------------------------------------------- batteries
constexpr int kMaxDevices = 4;
Node* g_batteries[kMaxDevices];
int g_battery_count;
Node* g_adapter;
uint64_t g_next_poll;
u64 g_last_full[kMaxDevices];
bool g_logged;

bool hid_is(Node* dev, u32 eisa, const char* text) {
    Value v = run_child(dev, "_HID");
    if (v.t == T::Int) return (u32)v.i == eisa;
    return v.t == T::Str && text && !strcmp(v.s, text);
}

void find_devices(Node* n) {
    for (Node* c = n->child; c; c = c->next) {
        if (c->kind == K::Device && child_of(c, "_HID")) {
            if (hid_is(c, kBatteryHid, "PNP0C0A") && g_battery_count < kMaxDevices) g_batteries[g_battery_count++] = c;
            else if (hid_is(c, 0, "ACPI0003") && !g_adapter) g_adapter = c;
            else if (hid_is(c, kEcHid, "PNP0C09") && child_of(c, "_REG")) {
                // firmware keeps the EC region off limits until told it is usable
                Value args[2] = {make_int(3), make_int(1)};
                run_child(c, "_REG", nullptr, args, 2);
                g_arena_pos = 0;
            }
        }
        if (c->kind != K::Method) find_devices(c);
    }
}

bool present(Node* bat) {
    bool has = false;
    u64 sta = to_int(run_child(bat, "_STA", &has));
    return !has || (sta & 0x10);
}

u64 usable(const Value& v, u32 i) {
    if (v.t != T::Pkg || i >= v.len) return 0xFFFFFFFFu;
    return to_int(v.p[i]) & 0xFFFFFFFFu;
}

// The capacity a full charge holds, from _BIF or the newer _BIX.
u64 read_full_capacity(Node* bat) {
    bool has = false;
    Value info = run_child(bat, "_BIF", &has);
    u64 last = usable(info, 2), design = usable(info, 1);
    if (!has) {
        info = run_child(bat, "_BIX", &has);
        if (has) { last = usable(info, 3); design = usable(info, 2); }
    }
    g_arena_pos = 0;
    if (last && last != 0xFFFFFFFFu) return last;
    return design != 0xFFFFFFFFu ? design : 0;
}

void poll_battery() {
    uint64_t now = timer_ms();
    if (now < g_next_poll) return;
    g_next_poll = now + kPollMs;
    BatteryStatus s;
    u64 total = 0, full = 0;
    for (int i = 0; i < g_battery_count; i++) {
        Node* bat = g_batteries[i];
        bool here = present(bat);
        g_arena_pos = 0;
        if (!here) { g_last_full[i] = 0; continue; }
        if (!g_last_full[i]) g_last_full[i] = read_full_capacity(bat);
        Value st = run_child(bat, "_BST");
        u64 state = usable(st, 0), remaining = usable(st, 2);
        g_arena_pos = 0;
        if (g_abort || !g_last_full[i] || remaining == 0xFFFFFFFFu) continue;
        s.present = true;
        if (state & 2) s.charging = true;
        total += remaining;
        full += g_last_full[i];
    }
    if (full) {
        u64 p = total * 100 / full;
        s.percent = p > 100 ? 100 : (int)p;
    }
    s.on_ac = s.charging;
    if (g_adapter) {
        bool has = false;
        u64 psr = to_int(run_child(g_adapter, "_PSR", &has));
        if (has && !g_abort) s.on_ac = psr != 0;
        g_arena_pos = 0;
    }
    if (!g_logged && s.present) {
        klog("battery: %d%%, %s", s.percent, s.charging ? "charging" : s.on_ac ? "on AC" : "on battery");
        g_logged = true;
    }
    battery_report(s);
}

void add_predefined() {
    g_root = new_node(nullptr, "\\___", K::Scope);
    for (const char* n : {"_GPE", "_PR_", "_SB_", "_SI_", "_TZ_"}) new_node(g_root, n, K::Scope);
    Node* os = new_node(g_root, "_OS_", K::Name);
    os->val.t = T::Str;
    os->val.s = "Microsoft Windows NT";
    Node* rev = new_node(g_root, "_REV", K::Name);
    rev->val.t = T::Int;
    rev->val.i = 2;
    new_node(g_root, "_GL_", K::Mutex);
    Node* osi = new_node(g_root, "_OSI", K::Method);
    osi->args = 1;
    osi->builtin = 1;
}

bool probe(const PciDevice*) {
    static bool done;
    if (done) return false;
    done = true;
    add_predefined();
    if (!load_tables()) return false;
    find_devices(g_root);
    if (!g_battery_count) { klog("battery: none in the ACPI tables"); return false; }
    klog("battery: %d found", g_battery_count);
    poll_battery();
    system_add_poll_hook(poll_battery);
    return false;
}

// any chipset vendor will do: the first device seen triggers the one setup
const DriverPciId kIds[] = {{0x8086, kPciAnyDevice}, {0x1022, kPciAnyDevice}, {0x10DE, kPciAnyDevice}, {0x1106, kPciAnyDevice}, {0x1039, kPciAnyDevice}};

}  // namespace

extern "C" {
extern const DriverInfo klein_driver;
const DriverInfo klein_driver = {KLEIN_DRIVER_ABI, "ACPI battery", kIds, 5, probe};
}
