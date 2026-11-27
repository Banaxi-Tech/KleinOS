#include "drivers/pci.h"
#include "arch/cpu.h"
#include "lib/log.h"

static PciDevice g_devices[64];
static int g_count;

#if defined(__aarch64__)
#include "boot/dtb.h"
// ECAM: configuration space is plain memory. QEMU virt puts it at 0x4010000000 (3f000000 without highmem).
static uint64_t g_ecam = 0x4010000000ull, g_ecam_buses = 256;
static volatile uint32_t* cfg(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (volatile uint32_t*)(g_ecam + ((uint64_t)bus << 20 | slot << 15 | func << 12 | (off & 0xFC)));
}
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) { return *cfg(bus, slot, func, off); }
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) { *cfg(bus, slot, func, off) = v; }

// Without firmware (QEMU -kernel) nobody assigned the BARs: hand out space from the 32-bit MMIO window.
static uint32_t g_next_mmio = 0x10000000, g_mmio_end = 0x3EFF0000;
static void assign_bars(uint8_t bus, uint8_t slot, uint8_t func) {
    uint8_t hdr = (pci_read32(bus, slot, func, 0x0C) >> 16) & 0x7F;
    int nbars = hdr == 0 ? 6 : hdr == 1 ? 2 : 0;
    for (int i = 0; i < nbars; i++) {
        uint8_t off = 0x10 + i * 4;
        uint32_t v = pci_read32(bus, slot, func, off);
        bool is64 = (v & 1) == 0 && (v & 6) == 4;
        if (!(v & 1) && !(v & ~0xFu)) {
            pci_write32(bus, slot, func, off, 0xFFFFFFFF);
            uint32_t m = pci_read32(bus, slot, func, off) & ~0xFu;
            uint32_t size = ~m + 1;
            if (m && size && g_next_mmio + size * 2 <= g_mmio_end) {
                uint32_t base = (g_next_mmio + size - 1) & ~(size - 1);
                pci_write32(bus, slot, func, off, base);
                if (is64) pci_write32(bus, slot, func, off + 4, 0);
                g_next_mmio = base + size;
            } else {
                pci_write32(bus, slot, func, off, 0);
            }
        }
        if (is64) i++;
    }
}
#else
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (slot << 11) | (func << 8) | (off & 0xFC));
    return inl(0xCFC);
}
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (slot << 11) | (func << 8) | (off & 0xFC));
    outl(0xCFC, v);
}
#endif
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (pci_read32(bus, slot, func, off) >> ((off & 2) * 8)) & 0xFFFF;
}
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t v) {
    uint32_t cur = pci_read32(bus, slot, func, off);
    int shift = (off & 2) * 8;
    cur = (cur & ~(0xFFFFu << shift)) | ((uint32_t)v << shift);
    pci_write32(bus, slot, func, off, cur);
}

static void add(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t id = pci_read32(bus, slot, func, 0);
    if ((id & 0xFFFF) == 0xFFFF || g_count >= 64) return;
#if defined(__aarch64__)
    assign_bars(bus, slot, func);
#endif
    PciDevice& d = g_devices[g_count++];
    d.bus = bus; d.slot = slot; d.func = func;
    d.vendor = id & 0xFFFF;
    d.device = id >> 16;
    uint32_t cls = pci_read32(bus, slot, func, 8);
    d.class_code = cls >> 24;
    d.subclass = (cls >> 16) & 0xFF;
    d.prog_if = (cls >> 8) & 0xFF;
    d.irq = pci_read32(bus, slot, func, 0x3C) & 0xFF;
    for (int i = 0; i < 6; i++) d.bar[i] = pci_read32(bus, slot, func, 0x10 + i * 4);
    klog("pci: %02x:%02x.%x %04x:%04x class %02x.%02x irq %d  %s", bus, slot, func, d.vendor, d.device,
         d.class_code, d.subclass, d.irq, pci_class_name(d.class_code, d.subclass));
}

void pci_init() {
#if defined(__aarch64__)
    DtbNode n;
    if (dtb_find_compatible("pci-host-ecam-generic", 0, &n)) {
        g_ecam = n.reg_addr(0);
        g_ecam_buses = n.reg_size(0) >> 20;
    }
    if (g_ecam_buses > 256) g_ecam_buses = 256;
    for (int bus = 0; bus < (int)g_ecam_buses; bus++)
#else
    for (int bus = 0; bus < 256; bus++)
#endif
        for (int slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read32(bus, slot, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            add(bus, slot, 0);
            uint8_t hdr = (pci_read32(bus, slot, 0, 0x0C) >> 16) & 0xFF;
            if (hdr & 0x80)
                for (int f = 1; f < 8; f++) add(bus, slot, f);
        }
}

int pci_count() { return g_count; }
const PciDevice* pci_get(int i) { return (i >= 0 && i < g_count) ? &g_devices[i] : nullptr; }
const PciDevice* pci_find(uint16_t v, uint16_t d) {
    for (int i = 0; i < g_count; i++)
        if (g_devices[i].vendor == v && g_devices[i].device == d) return &g_devices[i];
    return nullptr;
}
const PciDevice* pci_find_class(uint8_t c, uint8_t s) {
    for (int i = 0; i < g_count; i++)
        if (g_devices[i].class_code == c && g_devices[i].subclass == s) return &g_devices[i];
    return nullptr;
}
void pci_enable(const PciDevice* d) {
    uint16_t cmd = pci_read16(d->bus, d->slot, d->func, 4);
    cmd |= 0x7;
    pci_write16(d->bus, d->slot, d->func, 4, cmd);
}

const char* pci_class_name(uint8_t c, uint8_t s) {
    switch (c) {
    case 0x01:
        switch (s) { case 0x01: return "IDE controller"; case 0x06: return "SATA controller"; case 0x08: return "NVMe controller"; }
        return "Storage controller";
    case 0x02: return "Network controller";
    case 0x03: return "Display controller";
    case 0x04: return "Multimedia controller";
    case 0x06:
        switch (s) { case 0x00: return "Host bridge"; case 0x01: return "ISA bridge"; case 0x04: return "PCI bridge"; }
        return "Bridge";
    case 0x0C: return s == 0x03 ? "USB controller" : "Serial bus controller";
    default: return "Device";
    }
}
