// UEFI stub: efi_main() runs as the firmware's application entry (relocated by boot.S). It loads
// \boot\initrd.tar (+ optional \boot\system.img, \boot\cmdline.txt) from the volume it was started from, finds the
// device tree, exits boot services and enters the kernel (boot_entry) with a BootHandoff block.
#include "boot/handoff.h"
#include "lib/string.h"

namespace {

typedef uint64_t UINTN;
typedef void* Handle;
struct Guid { uint32_t a; uint16_t b, c; uint8_t d[8]; };
struct TableHeader { uint64_t sig; uint32_t rev, size, crc, res; };
struct ConOut { void* reset; UINTN (*output)(ConOut*, const char16_t*); };
struct ConfigTable { Guid guid; void* table; };
struct BootServices {
    TableHeader hdr;
    void* fn[38];
};
struct SystemTable {
    TableHeader hdr;
    char16_t* vendor; uint32_t rev, pad;
    Handle con_in_h; void* con_in; Handle con_out_h; ConOut* con_out; Handle err_h; void* err;
    void* runtime; BootServices* boot; UINTN n_tables; ConfigTable* tables;
};
struct MemDesc { uint32_t type, pad; uint64_t phys, virt, pages, attr; };
struct File {
    uint64_t rev;
    UINTN (*open)(File*, File**, const char16_t*, uint64_t, uint64_t);
    UINTN (*close)(File*);
    void* del;
    UINTN (*read)(File*, UINTN*, void*);
    void* write; void* getpos; void* setpos;
    UINTN (*getinfo)(File*, const Guid*, UINTN*, void*);
};
struct SimpleFs { uint64_t rev; UINTN (*open_volume)(SimpleFs*, File**); };
struct LoadedImage { uint32_t rev, pad; Handle parent; SystemTable* st; Handle device; };
struct GopInfo { uint32_t version, hres, vres, format, mask[4], stride; };
struct GopMode { uint32_t max, mode; GopInfo* info; UINTN info_size; uint64_t base, size; };
struct Gop { void* q; void* s; void* blt; GopMode* mode; };

const Guid kDtbGuid = {0xb1b621d5, 0xf19c, 0x41a5, {0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0}};
const Guid kFsGuid = {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
const Guid kLoadedGuid = {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
const Guid kInfoGuid = {0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
const Guid kGopGuid = {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};

enum { AllocAny = 0, LoaderData = 2 };
enum { kAllocatePages = 2, kGetMemoryMap = 4, kHandleProtocol = 16, kExitBootServices = 26, kLocateProtocol = 37 };
template <typename F> F bs(BootServices* b, int i) { return (F)b->fn[i]; }

SystemTable* g_st;
BootServices* g_bs;

void print(const char* s) {
    char16_t buf[128];
    int n = 0;
    for (; *s && n < 125; s++) { if (*s == '\n') buf[n++] = '\r'; buf[n++] = (uint8_t)*s; }
    buf[n] = 0;
    if (g_st->con_out) g_st->con_out->output(g_st->con_out, buf);
}

void* alloc_pages(UINTN bytes) {
    uint64_t addr = 0;
    UINTN pages = (bytes + 4095) / 4096;
    if (bs<UINTN (*)(int, int, UINTN, uint64_t*)>(g_bs, kAllocatePages)(AllocAny, LoaderData, pages, &addr)) return nullptr;
    return (void*)addr;
}

// Clean + invalidate to the point of coherency: the kernel turns the caches off and must see what we wrote.
void clean(const void* p, UINTN n) {
    uintptr_t a = (uintptr_t)p & ~63ull, e = (uintptr_t)p + n;
    for (; a < e; a += 64) asm volatile("dc civac, %0" ::"r"(a) : "memory");
    asm volatile("dsb sy" ::: "memory");
}

File* g_root;

bool load(const char16_t* path, void** data, UINTN* size) {
    File* f;
    if (g_root->open(g_root, &f, path, 1, 0)) return false;
    uint8_t info[512];
    UINTN isz = sizeof(info);
    if (f->getinfo(f, &kInfoGuid, &isz, info)) { f->close(f); return false; }
    UINTN len = *(uint64_t*)(info + 8);
    void* mem = alloc_pages(len ? len : 1);
    if (!mem) { f->close(f); return false; }
    UINTN done = 0;
    while (done < len) {      // chunked: some firmware file drivers dislike giant reads
        UINTN n = len - done > (4u << 20) ? (4u << 20) : len - done;
        if (f->read(f, &n, (uint8_t*)mem + done) || !n) { f->close(f); return false; }
        done += n;
    }
    f->close(f);
    clean(mem, len);
    *data = mem;
    *size = len;
    return true;
}

bool same(const Guid& a, const Guid& b) { return !memcmp(&a, &b, sizeof(Guid)); }

}  // namespace

extern "C" [[noreturn]] void boot_entry(uintptr_t dtb, void* handoff);
extern "C" char __kernel_start[];

extern "C" UINTN efi_main(Handle image, SystemTable* st) {
    g_st = st;
    g_bs = st->boot;
    print("KleinOS: starting\n");

    LoadedImage* li = nullptr;
    SimpleFs* fs = nullptr;
    if (bs<UINTN (*)(Handle, const Guid*, void**)>(g_bs, kHandleProtocol)(image, &kLoadedGuid, (void**)&li) ||
        bs<UINTN (*)(Handle, const Guid*, void**)>(g_bs, kHandleProtocol)(li->device, &kFsGuid, (void**)&fs) ||
        fs->open_volume(fs, &g_root)) {
        print("KleinOS: cannot open the boot volume\n");
        return 1;
    }

    BootHandoff* h = (BootHandoff*)alloc_pages(sizeof(BootHandoff));
    if (!h) return 1;
    memset(h, 0, sizeof(*h));
    h->magic = kHandoffMagic;
    h->size = (sizeof(BootHandoff) + 4095) & ~4095ull;

    void* data;
    UINTN size;
    if (!load(u"\\boot\\initrd.tar", &data, &size)) { print("KleinOS: \\boot\\initrd.tar missing\n"); return 1; }
    strcpy(h->modules[0].name, "initrd");
    h->modules[0].addr = (uint64_t)data;
    h->modules[0].size = size;
    h->module_count = 1;
    if (load(u"\\boot\\system.img", &data, &size)) {       // installer media
        strcpy(h->modules[1].name, "system.img");
        h->modules[1].addr = (uint64_t)data;
        h->modules[1].size = size;
        h->module_count = 2;
    }
    if (load(u"\\boot\\cmdline.txt", &data, &size)) {
        UINTN n = size < sizeof(h->cmdline) - 1 ? size : sizeof(h->cmdline) - 1;
        memcpy(h->cmdline, data, n);
        for (UINTN i = 0; i < n; i++) if (h->cmdline[i] == '\n' || h->cmdline[i] == '\r') h->cmdline[i] = ' ';
    } else {
        strcpy(h->cmdline, "mode=installed");
    }

    uintptr_t dtb = 0;
    for (UINTN i = 0; i < st->n_tables; i++)
        if (same(st->tables[i].guid, kDtbGuid)) dtb = (uintptr_t)st->tables[i].table;
    // edk2 on QEMU only publishes the table without ACPI; the original blob still sits at the start of RAM
    if (!dtb && __builtin_bswap32(*(uint32_t*)0x40000000) == 0xD00DFEED) dtb = 0x40000000;
    if (!dtb) { print("KleinOS: firmware gave no device tree\n"); return 1; }
    uint32_t dtb_size = __builtin_bswap32(*(uint32_t*)(dtb + 4));

    Gop* gop = nullptr;
    if (!bs<UINTN (*)(const Guid*, void*, void**)>(g_bs, kLocateProtocol)(&kGopGuid, nullptr, (void**)&gop) && gop &&
        gop->mode && gop->mode->info && gop->mode->info->format == 1) {          // BGRX
        h->fb_addr = gop->mode->base;
        h->fb_width = gop->mode->info->hres;
        h->fb_height = gop->mode->info->vres;
        h->fb_pitch = gop->mode->info->stride * 4;
    }

    // Everything the kernel reads must be in RAM, not in our data cache.
    clean(h, h->size);
    clean(__kernel_start, 0x900000);
    clean((void*)dtb, dtb_size);

    UINTN map_cap = 64 * 1024, map_size, key, desc_size;
    uint32_t desc_ver;
    uint8_t* map = (uint8_t*)alloc_pages(map_cap);
    for (int attempt = 0; attempt < 8; attempt++) {
        map_size = map_cap;
        if (bs<UINTN (*)(UINTN*, void*, UINTN*, UINTN*, uint32_t*)>(g_bs, kGetMemoryMap)(&map_size, map, &key, &desc_size, &desc_ver))
            return 1;
        if (!bs<UINTN (*)(Handle, UINTN)>(g_bs, kExitBootServices)(image, key)) {
            for (UINTN o = 0; o + desc_size <= map_size; o += desc_size) {
                MemDesc* d = (MemDesc*)(map + o);
                bool free = d->type == 7 || (d->type >= 1 && d->type <= 4);
                if (!free) continue;
                uint64_t bytes = d->pages * 4096;
                if (h->region_count && h->regions[h->region_count - 1].base + h->regions[h->region_count - 1].size == d->phys)
                    h->regions[h->region_count - 1].size += bytes;
                else if (h->region_count < 96)
                    h->regions[h->region_count++] = {d->phys, bytes};
            }
            clean(h, h->size);
            boot_entry(dtb, h);
        }
    }
    return 1;
}
