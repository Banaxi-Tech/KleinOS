#include "boot/dtb.h"
#include "boot/handoff.h"
#include "boot/multiboot2.h"
#include "lib/string.h"
#include "lib/log.h"
#include "sys/panic.h"

namespace {

constexpr uint32_t kMagic = 0xD00DFEED;
enum { FDT_BEGIN_NODE = 1, FDT_END_NODE = 2, FDT_PROP = 3, FDT_NOP = 4, FDT_END = 9 };

struct Header {
    uint32_t magic, totalsize, off_dt_struct, off_dt_strings, off_mem_rsvmap, version, last_comp_version,
        boot_cpuid_phys, size_dt_strings, size_dt_struct;
};

const uint8_t* g_blob;
size_t g_size;

inline uint32_t be32(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return ((uint32_t)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}
inline uint64_t be64(const void* p) { return ((uint64_t)be32(p) << 32) | be32((const uint8_t*)p + 4); }

// Calls fn for every node once all of its properties have been seen (children come after the parent's
// properties in a DTB, so a node is complete when its first child or its end tag shows up).
template <typename Fn>
void walk(Fn fn) {
    if (!g_blob) return;
    const Header* h = (const Header*)g_blob;
    const uint8_t* p = g_blob + be32(&h->off_dt_struct);
    const char* strings = (const char*)g_blob + be32(&h->off_dt_strings);

    struct Frame { DtbNode node; uint32_t child_addr, child_size; bool emitted; };
    static Frame stack[16];
    int depth = -1;

    auto emit = [&](Frame& f) -> bool {
        if (f.emitted) return true;
        f.emitted = true;
        return fn(f.node);
    };

    for (;;) {
        uint32_t tag = be32(p);
        p += 4;
        if (tag == FDT_BEGIN_NODE) {
            if (depth >= 0 && !emit(stack[depth])) return;
            if (++depth >= 16) return;
            Frame& f = stack[depth];
            f = Frame{};
            f.node.name = (const char*)p;
            f.node.addr_cells = depth ? stack[depth - 1].child_addr : 2;
            f.node.size_cells = depth ? stack[depth - 1].child_size : 1;
            f.child_addr = 2;   // defaults per the spec
            f.child_size = 1;
            size_t n = strlen((const char*)p) + 1;
            p += (n + 3) & ~3u;
        } else if (tag == FDT_END_NODE) {
            if (depth < 0) return;
            if (!emit(stack[depth])) return;
            depth--;
        } else if (tag == FDT_PROP) {
            uint32_t len = be32(p), nameoff = be32(p + 4);
            const uint8_t* data = p + 8;
            p += 8 + ((len + 3) & ~3u);
            if (depth < 0) continue;
            Frame& f = stack[depth];
            const char* name = strings + nameoff;
            DtbNode& n = f.node;
            if (!strcmp(name, "compatible")) { n.compatible = (const char*)data; n.compatible_len = len; }
            else if (!strcmp(name, "device_type")) n.device_type = (const char*)data;
            else if (!strcmp(name, "reg")) { n.reg = (const uint32_t*)data; n.reg_cells = len / 4; }
            else if (!strcmp(name, "interrupts")) { n.interrupts = (const uint32_t*)data; n.interrupt_cells = len / 4; }
            else if (!strcmp(name, "method")) n.method = (const char*)data;
            else if (!strcmp(name, "bootargs")) n.bootargs = (const char*)data;
            else if (!strcmp(name, "linux,initrd-start")) { n.initrd_start = data; n.initrd_start_len = len; }
            else if (!strcmp(name, "linux,initrd-end")) { n.initrd_end = data; n.initrd_end_len = len; }
            else if (!strcmp(name, "#address-cells")) f.child_addr = be32(data);
            else if (!strcmp(name, "#size-cells")) f.child_size = be32(data);
        } else if (tag == FDT_NOP) {
        } else {
            return;   // FDT_END or garbage
        }
    }
}

}  // namespace

bool DtbNode::has_compatible(const char* s) const {
    size_t n = strlen(s);
    for (uint32_t i = 0; compatible && i < compatible_len;) {
        size_t l = strlen(compatible + i);
        if (l == n && !memcmp(compatible + i, s, n)) return true;
        i += l + 1;
    }
    return false;
}

uint64_t DtbNode::reg_addr(int i) const {
    uint32_t stride = addr_cells + size_cells;
    if (!reg || (uint32_t)(i + 1) * stride > reg_cells) return 0;
    const uint32_t* c = reg + i * stride;
    uint64_t v = 0;
    for (uint32_t k = 0; k < addr_cells; k++) v = (v << 32) | be32(c + k);
    return v;
}

uint64_t DtbNode::reg_size(int i) const {
    uint32_t stride = addr_cells + size_cells;
    if (!reg || (uint32_t)(i + 1) * stride > reg_cells) return 0;
    const uint32_t* c = reg + i * stride + addr_cells;
    uint64_t v = 0;
    for (uint32_t k = 0; k < size_cells; k++) v = (v << 32) | be32(c + k);
    return v;
}

int DtbNode::gic_irq(int idx) const {
    if (!interrupts || (uint32_t)(idx + 1) * 3 > interrupt_cells) return -1;
    uint32_t type = be32(interrupts + idx * 3), num = be32(interrupts + idx * 3 + 1);
    return (int)(type == 0 ? num + 32 : num + 16);
}

bool dtb_init(uintptr_t addr) {
    const Header* h = (const Header*)addr;
    if (!addr || be32(&h->magic) != kMagic) return false;
    g_blob = (const uint8_t*)addr;
    g_size = be32(&h->totalsize);
    return true;
}

uintptr_t dtb_address() { return (uintptr_t)g_blob; }
size_t dtb_size() { return g_size; }

bool dtb_find_compatible(const char* compat, int nth, DtbNode* out) {
    bool found = false;
    walk([&](const DtbNode& n) {
        if (n.has_compatible(compat) && nth-- == 0) { *out = n; found = true; return false; }
        return true;
    });
    return found;
}

bool dtb_find_device_type(const char* type, int nth, DtbNode* out) {
    bool found = false;
    walk([&](const DtbNode& n) {
        if (n.device_type && !strcmp(n.device_type, type) && nth-- == 0) { *out = n; found = true; return false; }
        return true;
    });
    return found;
}

bool dtb_chosen(DtbNode* out) {
    bool found = false;
    walk([&](const DtbNode& n) {
        if (!strcmp(n.name, "chosen")) { *out = n; found = true; return false; }
        return true;
    });
    return found;
}

// ---------------------------------------------------------------- BootInfo from the device tree
extern "C" char __kernel_start[], __kernel_end[];

namespace {

struct Range { uint64_t base, end; };

uint64_t read_cells(const uint8_t* p, uint32_t len) { return len >= 8 ? be64(p) : len >= 4 ? be32(p) : 0; }

}  // namespace

void boot_parse(uint32_t, uintptr_t info, BootInfo& out) {
    if (!dtb_init(info)) kpanic("no device tree passed by the boot loader (x0)");
    out = BootInfo{};
    out.dtb = info;

    Range reserved[12];
    int nres = 0;
    reserved[nres++] = {(uint64_t)(uintptr_t)__kernel_start, ((uint64_t)(uintptr_t)__kernel_end + 0xFFF) & ~0xFFFull};
    reserved[nres++] = {info, (info + dtb_size() + 0xFFF) & ~0xFFFull};

    const BootHandoff* ho = (boot_handoff && ((const BootHandoff*)boot_handoff)->magic == kHandoffMagic)
                                ? (const BootHandoff*)boot_handoff : nullptr;
    if (ho) {   // booted by the UEFI stub
        out.cmdline = ho->cmdline;
        reserved[nres++] = {boot_handoff, boot_handoff + ho->size};
        for (uint32_t i = 0; i < ho->module_count && i < 4; i++) {
            out.modules[out.module_count++] = {ho->modules[i].name, (uint8_t*)ho->modules[i].addr, (size_t)ho->modules[i].size};
            reserved[nres++] = {ho->modules[i].addr, (ho->modules[i].addr + ho->modules[i].size + 0xFFF) & ~0xFFFull};
        }
        if (ho->fb_addr) reserved[nres++] = {ho->fb_addr, ho->fb_addr + (uint64_t)ho->fb_pitch * ho->fb_height};
    }
    DtbNode chosen;
    if (!ho && dtb_chosen(&chosen)) {
        if (chosen.bootargs) out.cmdline = chosen.bootargs;
        if (chosen.initrd_start && chosen.initrd_end) {
            uint64_t s = read_cells(chosen.initrd_start, chosen.initrd_start_len);
            uint64_t e = read_cells(chosen.initrd_end, chosen.initrd_end_len);
            if (e > s) {
                out.modules[out.module_count++] = {"initrd", (uint8_t*)(uintptr_t)s, (size_t)(e - s)};
                reserved[nres++] = {s, (e + 0xFFF) & ~0xFFFull};
            }
        }
    }

    // The kernel programs the display itself (ramfb), so it needs memory for the pixels.
    uint32_t w, h;
    boot_resolution(&w, &h);
    out.fb_size = ((size_t)w * h * 4 + 0xFFFF) & ~(size_t)0xFFFF;

    // Heap: the biggest free stretch of RAM once the kernel, DTB and initrd are taken out; the frame
    // buffer comes from the end of it.
    uint64_t best_b = 0, best_e = 0;
    auto consider = [&](uint64_t b, uint64_t e) {
        out.mem_total += e - b;
        if (e > 0x100000000ull) e = 0x100000000ull;      // DMA engines (EHCI) are 32-bit
        if (b >= e) return;
        Range pieces[16] = {{b, e}};
        int np = 1;
        for (int k = 0; k < nres; k++) {
            for (int i = 0; i < np; i++) {
                Range pc = pieces[i];
                if (reserved[k].end <= pc.base || reserved[k].base >= pc.end) continue;
                pieces[i] = pieces[--np];
                if (reserved[k].base > pc.base && np < 16) pieces[np++] = {pc.base, reserved[k].base};
                if (reserved[k].end < pc.end && np < 16) pieces[np++] = {reserved[k].end, pc.end};
                i--;
            }
        }
        for (int i = 0; i < np; i++)
            if (pieces[i].end - pieces[i].base > best_e - best_b) { best_b = pieces[i].base; best_e = pieces[i].end; }
    };
    if (ho) {
        for (uint32_t i = 0; i < ho->region_count; i++) consider(ho->regions[i].base, ho->regions[i].base + ho->regions[i].size);
    } else {
        DtbNode mem;
        for (int n = 0; dtb_find_device_type("memory", n, &mem); n++)
            for (int r = 0; r < mem.reg_count(); r++) consider(mem.reg_addr(r), mem.reg_addr(r) + mem.reg_size(r));
    }
    if (best_e - best_b <= out.fb_size + (16u << 20)) kpanic("not enough RAM");
    best_b = (best_b + 0xFFF) & ~0xFFFull;
    best_e = best_e & ~0xFFFull;
    out.fb_start = (uintptr_t)(best_e - out.fb_size);
    out.heap_start = (uintptr_t)best_b;
    out.heap_size = (size_t)(out.fb_start - best_b);
}
