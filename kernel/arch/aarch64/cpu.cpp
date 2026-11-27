// AArch64 CPU identification, entropy and cache maintenance.
#include "arch/cpu.h"
#include "lib/string.h"

static char g_brand[64];

const char* cpu_brand() {
    if (!g_brand[0]) {
        uint64_t midr;
        asm volatile("mrs %0, midr_el1" : "=r"(midr));
        unsigned impl = (midr >> 24) & 0xFF, part = (midr >> 4) & 0xFFF;
        const char* name = nullptr;
        if (impl == 0x41) {   // Arm Ltd.
            switch (part) {
            case 0xD03: name = "Cortex-A53"; break;
            case 0xD04: name = "Cortex-A35"; break;
            case 0xD05: name = "Cortex-A55"; break;
            case 0xD07: name = "Cortex-A57"; break;
            case 0xD08: name = "Cortex-A72"; break;
            case 0xD09: name = "Cortex-A73"; break;
            case 0xD0A: name = "Cortex-A75"; break;
            case 0xD0B: name = "Cortex-A76"; break;
            case 0xD0C: name = "Neoverse N1"; break;
            case 0xD0D: name = "Cortex-A77"; break;
            case 0xD40: name = "Neoverse V1"; break;
            case 0xD41: name = "Cortex-A78"; break;
            case 0xD44: name = "Cortex-X1"; break;
            case 0xD46: name = "Cortex-A510"; break;
            case 0xD47: name = "Cortex-A710"; break;
            case 0xD48: name = "Cortex-X2"; break;
            case 0xD49: name = "Neoverse N2"; break;
            case 0xD4F: name = "Neoverse V2"; break;
            case 0xD80: name = "Cortex-A520"; break;
            case 0xD81: name = "Cortex-A720"; break;
            }
        }
        if (name) snprintf(g_brand, sizeof(g_brand), "Arm %s", name);
        else if (impl == 0x61) strcpy(g_brand, "Apple Silicon");
        else snprintf(g_brand, sizeof(g_brand), "ARMv8 processor (implementer 0x%02x, part 0x%03x)", impl, part);
    }
    return g_brand;
}

uint64_t cpu_random64() {
    static bool checked, has_rndr;
    static uint64_t state = 0x9E3779B97F4A7C15ull;
    if (!checked) {
        uint64_t isar0;
        asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
        has_rndr = ((isar0 >> 60) & 0xF) != 0;
        checked = true;
    }
    if (has_rndr) {
        for (int i = 0; i < 10; i++) {
            uint64_t v, nzcv;
            asm volatile("mrs %0, s3_3_c2_c4_0\n\tmrs %1, nzcv" : "=r"(v), "=r"(nzcv));   // RNDR; Z set on failure
            if (!(nzcv & (1ull << 30))) return v ^ cpu_counter();
        }
    }
    // splitmix64 over the generic counter
    state += 0x9E3779B97F4A7C15ull ^ cpu_counter();
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// RAM is already mapped as ordinary cacheable memory and the display scans it directly.
bool cpu_framebuffer_wc(uintptr_t, size_t) { return false; }

void cpu_flush_icache(void* p, size_t n) {
    uint64_t ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t dline = 4u << ((ctr >> 16) & 0xF), iline = 4u << (ctr & 0xF);
    uintptr_t start = (uintptr_t)p, end = start + n;
    for (uintptr_t a = start & ~(uintptr_t)(dline - 1); a < end; a += dline) asm volatile("dc cvau, %0" ::"r"(a) : "memory");
    asm volatile("dsb ish" ::: "memory");
    for (uintptr_t a = start & ~(uintptr_t)(iline - 1); a < end; a += iline) asm volatile("ic ivau, %0" ::"r"(a) : "memory");
    asm volatile("dsb ish\n\tisb" ::: "memory");
}
