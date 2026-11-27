#pragma once
// What the UEFI stub (arch/aarch64/efi_stub.cpp) tells the kernel after exiting boot services: the files it
// loaded from the boot volume, free RAM, and an optional firmware framebuffer. boot.S stores the pointer in
// boot_handoff (0 when booted without UEFI, e.g. QEMU -kernel).
#include <stdint.h>

constexpr uint64_t kHandoffMagic = 0x4946454E49454C4Bull;   // "KLEINEFI"

struct BootHandoff {
    uint64_t magic;
    uint64_t size;
    char cmdline[256];
    uint32_t module_count, region_count;
    struct Module { char name[32]; uint64_t addr, size; } modules[4];
    struct Region { uint64_t base, size; } regions[96];
    uint64_t fb_addr;                   // GOP framebuffer (32-bit BGRX), 0 = none
    uint32_t fb_width, fb_height, fb_pitch, pad;
};

extern "C" uintptr_t boot_handoff;
