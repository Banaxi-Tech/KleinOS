#include "boot/multiboot2.h"
#include "drivers/framebuffer.h"
#include "lib/string.h"
#include "lib/log.h"
#include "sys/panic.h"

extern "C" char __kernel_end[];

struct Tag { uint32_t type, size; };

void multiboot2_parse(uint32_t magic, uintptr_t info, BootInfo& out) {
    if (magic != 0x36D76289) kpanic("not booted by a multiboot2 loader");
    out = BootInfo{};
    uint32_t total = *(uint32_t*)info;
    uintptr_t reserved_end = (uintptr_t)__kernel_end;
    if (info + total > reserved_end) reserved_end = info + total;

    struct Region { uint64_t base, len; } regions[64];
    int nregions = 0;

    for (uintptr_t p = info + 8; p < info + total;) {
        Tag* t = (Tag*)p;
        if (t->type == 0) break;
        switch (t->type) {
        case 14: case 15: {   // ACPI RSDP (old, new)
            static uint8_t rsdp[64];
            uint32_t n = t->size - 8 < sizeof(rsdp) ? t->size - 8 : sizeof(rsdp);
            memcpy(rsdp, (const uint8_t*)t + 8, n);
            out.acpi_rsdp = (uintptr_t)rsdp;
            break;
        }
        case 1:   // cmdline
            out.cmdline = (const char*)(p + 8);
            break;
        case 3: { // module
            uint32_t start = *(uint32_t*)(p + 8), end = *(uint32_t*)(p + 12);
            if (out.module_count < 8) {
                BootModule& m = out.modules[out.module_count++];
                m.data = (uint8_t*)(uintptr_t)start;
                m.size = end - start;
                m.name = (const char*)(p + 16);
            }
            if (end > reserved_end) reserved_end = end;
            break;
        }
        case 6: { // memory map
            uint32_t esize = *(uint32_t*)(p + 8);
            for (uintptr_t e = p + 16; e < p + t->size; e += esize) {
                uint64_t base = *(uint64_t*)e, len = *(uint64_t*)(e + 8);
                uint32_t type = *(uint32_t*)(e + 16);
                if (type == 1) {
                    out.mem_total += len;
                    if (nregions < 64) regions[nregions++] = {base, len};
                }
            }
            break;
        }
        case 8: { // framebuffer
            FramebufferInfo& fb = framebuffer();
            fb.address = (void*)(uintptr_t)*(uint64_t*)(p + 8);
            fb.pitch = *(uint32_t*)(p + 16);
            fb.width = *(uint32_t*)(p + 20);
            fb.height = *(uint32_t*)(p + 24);
            fb.bpp = *(uint8_t*)(p + 28);
            break;
        }
        }
        p += (t->size + 7) & ~7u;
    }

    // Heap: biggest usable region above the kernel/modules, below 4 GiB.
    reserved_end = (reserved_end + 0xFFF) & ~0xFFFull;
    for (int i = 0; i < nregions; i++) {
        uint64_t b = regions[i].base, e = regions[i].base + regions[i].len;
        if (e > 0x100000000ull) e = 0x100000000ull;
        if (b < reserved_end) b = reserved_end;
        if (e <= b) continue;
        if (e - b > out.heap_size) { out.heap_start = b; out.heap_size = e - b; }
    }
}

void boot_parse(uint32_t magic, uintptr_t info, BootInfo& out) { multiboot2_parse(magic, info, out); }
