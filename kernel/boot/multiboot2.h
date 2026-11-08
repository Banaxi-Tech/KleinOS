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
};

void multiboot2_parse(uint32_t magic, uintptr_t info, BootInfo& out);
BootInfo& boot_info();
const BootModule* boot_module(const char* name);
// Look up "key=value" in the kernel command line. Returns false if absent.
bool boot_arg(const char* key, char* value, size_t size);
