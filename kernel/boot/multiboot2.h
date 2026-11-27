#pragma once
#include <stdint.h>
#include <stddef.h>

struct BootModule {
    const char* name;      // module command line (e.g. "initrd", "system.img")
    uint8_t* data;
    size_t size;
};

struct BootInfo {
    const char* cmdline = "";
    uint64_t mem_total = 0;         // usable RAM in bytes
    uintptr_t heap_start = 0;       // largest free region chosen for the heap
    size_t heap_size = 0;
    BootModule modules[8];
    int module_count = 0;
    uintptr_t acpi_rsdp = 0;        // copy of the ACPI RSDP on UEFI, 0 = scan low memory
    uintptr_t dtb = 0;              // device tree blob (ARM boards)
    uintptr_t fb_start = 0;         // RAM set aside for a framebuffer the kernel programs itself (ARM)
    size_t fb_size = 0;
};

// Parse what the loader handed over: Multiboot2 tags on x86, the flattened device tree on ARM.
void boot_parse(uint32_t magic, uintptr_t info, BootInfo& out);
void multiboot2_parse(uint32_t magic, uintptr_t info, BootInfo& out);   // x86
BootInfo& boot_info();
const BootModule* boot_module(const char* name);
// Look up "key=value" in the kernel command line. Returns false if absent.
bool boot_arg(const char* key, char* value, size_t size);
void boot_resolution(uint32_t* w, uint32_t* h);   // "resolution=WxH", default 1280x800
