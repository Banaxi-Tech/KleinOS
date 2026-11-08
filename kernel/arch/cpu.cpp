#include "arch/cpu.h"
#include "lib/string.h"

static char g_brand[49];

const char* cpu_brand() {
    if (!g_brand[0]) {
        uint32_t a, b, c, d;
        cpuid(0x80000000, 0, &a, &b, &c, &d);
        if (a >= 0x80000004) {
            uint32_t* p = (uint32_t*)g_brand;
            for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
                cpuid(leaf, 0, &p[0], &p[1], &p[2], &p[3]);
                p += 4;
            }
            g_brand[48] = 0;
            // trim leading spaces
            char* s = g_brand;
            while (*s == ' ') s++;
            if (s != g_brand) memmove(g_brand, s, strlen(s) + 1);
        } else {
            #ifdef __i386__
            strcpy(g_brand, "x86 processor");
#else
            strcpy(g_brand, "x86-64 processor");
#endif
        }
    }
    return g_brand;
}

uint64_t cpu_random64() {
    static bool checked, has_rdrand;
    static uint64_t state = 0x9E3779B97F4A7C15ull;
    if (!checked) {
        uint32_t a, b, c, d;
        cpuid(1, 0, &a, &b, &c, &d);
        has_rdrand = c & (1u << 30);
        checked = true;
    }
    if (has_rdrand) {
        uintptr_t v;
        unsigned char ok;
#ifdef __i386__
        uint64_t both = 0;
        int got = 0;
        for (int i = 0; i < 20 && got < 2; i++) {
            asm volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
            if (ok) { both = (both << 32) | v; got++; }
        }
        if (got == 2) return both ^ rdtsc();
#else
        for (int i = 0; i < 10; i++) {
            asm volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
            if (ok) return v ^ rdtsc();
        }
#endif
    }
    // splitmix64 over TSC
    state += 0x9E3779B97F4A7C15ull ^ rdtsc();
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

#ifdef __i386__
alignas(4096) static uint32_t g_page_dir[1024];
static constexpr int kLargePageShift = 22;
#else
extern "C" uint64_t pd[];
static constexpr int kLargePageShift = 21;
#endif

static inline uintptr_t read_cr0() { uintptr_t v; asm volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline void write_cr0(uintptr_t v) { asm volatile("mov %0, %%cr0" :: "r"(v)); }
static inline uintptr_t read_cr3() { uintptr_t v; asm volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void write_cr3(uintptr_t v) { asm volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }

bool cpu_framebuffer_wc(uintptr_t addr, size_t size) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!addr || !size || !(d & (1u << 16))) return false;      // PAT
    uint64_t end = (uint64_t)addr + size - 1;
    if (end >> 32) return false;                                // beyond the mapped 4 GiB
    #ifdef __i386__
    if (!(d & (1u << 3))) return false;                         // PSE
    #endif
    size_t first = addr >> kLargePageShift, last = (size_t)(end >> kLargePageShift);

    IrqGuard guard;
    uintptr_t cr0 = read_cr0();
    write_cr0((cr0 | (1u << 30)) & ~(uintptr_t)(1u << 29));     // CD=1, NW=0
    asm volatile("wbinvd");
    // PAT entry 1 (selected by PWT) becomes write-combining
    wrmsr(0x277, (rdmsr(0x277) & ~0xFF00ull) | 0x0100ull);
    #ifdef __i386__
    for (size_t i = 0; i < 1024; i++)
        g_page_dir[i] = (uint32_t)(i << kLargePageShift) | 0x83 | (i >= first && i <= last ? 0x08 : 0);
    write_cr3((uintptr_t)g_page_dir);
    uintptr_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    asm volatile("mov %0, %%cr4" :: "r"(cr4 | 0x10));           // PSE
    cr0 |= 0x80000000u;
    #else
    for (size_t i = first; i <= last; i++) pd[i] |= 0x08;
    #endif
    asm volatile("wbinvd");
    write_cr0(cr0);
    write_cr3(read_cr3());
    return true;
}
