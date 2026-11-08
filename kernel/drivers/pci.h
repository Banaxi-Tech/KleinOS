#pragma once
#include <stdint.h>

struct PciDevice {
    uint8_t bus, slot, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if;
    uint8_t irq;
    uint32_t bar[6];
};

void pci_init();                                    // enumerate bus 0..255
int pci_count();
const PciDevice* pci_get(int index);
const PciDevice* pci_find(uint16_t vendor, uint16_t device);
const PciDevice* pci_find_class(uint8_t class_code, uint8_t subclass);
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
// Enable bus mastering + memory + I/O decoding.
void pci_enable(const PciDevice* d);
const char* pci_class_name(uint8_t class_code, uint8_t subclass);
