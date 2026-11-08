#pragma once
// .dri hardware drivers: ELF64 shared objects (same format as .dl) kept in
// /System/Drivers. Each exports one DriverInfo named klein_driver; at boot its
// probe() is called for every PCI device its id table matches.
// Drivers run in the kernel with full hardware access.
#include <stdint.h>
#include "drivers/pci.h"
#include "lib/kstring.h"

constexpr uint32_t KLEIN_DRIVER_ABI = 1;
constexpr uint16_t kPciAnyDevice = 0xFFFF;

struct DriverPciId {
    uint16_t vendor, device;   // device may be kPciAnyDevice
};

struct DriverInfo {
    uint32_t abi;              // KLEIN_DRIVER_ABI
    const char* name;
    const DriverPciId* ids;
    int id_count;
    bool (*probe)(const PciDevice* dev);   // true = device claimed
};

// Load one .dri and bind it to matching PCI devices. Returns false if it could not be loaded.
bool driver_load(const char* path, String* error = nullptr);
// Load every .dri in a directory (sorted by name). Returns number loaded.
int driver_load_dir(const char* dir);
