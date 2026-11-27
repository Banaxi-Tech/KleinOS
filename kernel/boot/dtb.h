#pragma once
// Minimal flattened device tree (DTB) reader for the ARM boards: just enough to find RAM, the kernel
// command line, the initrd and device base addresses.
#include <stdint.h>
#include <stddef.h>

struct DtbNode {
    const char* name = nullptr;
    const char* device_type = nullptr;
    const char* compatible = nullptr;       // NUL separated list
    uint32_t compatible_len = 0;
    const uint32_t* reg = nullptr;          // big-endian cells
    uint32_t reg_cells = 0;
    const uint32_t* interrupts = nullptr;   // big-endian cells
    uint32_t interrupt_cells = 0;
    const char* method = nullptr;           // psci: "hvc" / "smc"
    const char* bootargs = nullptr;         // /chosen
    const uint8_t* initrd_start = nullptr;  // /chosen linux,initrd-start (4 or 8 byte value)
    uint32_t initrd_start_len = 0;
    const uint8_t* initrd_end = nullptr;
    uint32_t initrd_end_len = 0;
    uint32_t addr_cells = 2, size_cells = 2;   // how `reg` is encoded (from the parent)

    bool has_compatible(const char* s) const;
    uint64_t reg_addr(int i) const;
    uint64_t reg_size(int i) const;
    int reg_count() const { return (addr_cells + size_cells) ? (int)(reg_cells / (addr_cells + size_cells)) : 0; }
    // GIC interrupt ID of the idx-th "interrupts" entry (3 cells: type, number, flags), or -1.
    int gic_irq(int idx) const;
};

// Select the blob (once, at boot). Returns false if it isn't a valid DTB.
bool dtb_init(uintptr_t addr);
uintptr_t dtb_address();
size_t dtb_size();
// The nth node (0-based) whose "compatible" list contains `compat`.
bool dtb_find_compatible(const char* compat, int nth, DtbNode* out);
// The nth node with device_type == type (e.g. "memory").
bool dtb_find_device_type(const char* type, int nth, DtbNode* out);
// The /chosen node.
bool dtb_chosen(DtbNode* out);
