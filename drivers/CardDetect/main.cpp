// Lists the network cards on the PCI bus in the kernel log, so you can tell
// which one a machine has. Claims nothing and drives nothing.
#include "drivers/pci.h"
#include "lib/log.h"
#include "sys/driver.h"

namespace {

const char* wifi_name(const PciDevice* d) {
    if (d->vendor == 0x168C && d->device == 0x002B) return "Atheros AR9285 (driver: stage 1 only)";
    if (d->vendor == 0x168C) return "Atheros (other model)";
    if (d->vendor == 0x14E4 && d->device == 0x4727) return "Broadcom BCM4313";
    if (d->vendor == 0x14E4) return "Broadcom (other model)";
    if (d->vendor == 0x10EC) return "Realtek";
    if (d->vendor == 0x1814) return "Ralink";
    if (d->vendor == 0x8086) return "Intel";
    return "unknown make";
}

bool probe(const PciDevice*) {
    static bool done;
    if (done) return false;
    done = true;
    int wifi = 0;
    for (int i = 0; i < pci_count(); i++) {
        const PciDevice* d = pci_get(i);
        if (d->class_code != 0x02) continue;
        if (d->subclass == 0x80) {
            klog("detect: Wi-Fi card: %s, PCI id %04x:%04x", wifi_name(d), d->vendor, d->device);
            wifi++;
        } else {
            klog("detect: wired network card, PCI id %04x:%04x", d->vendor, d->device);
        }
    }
    if (!wifi) klog("detect: no Wi-Fi card on the PCI bus (a USB one would not show here)");
    return false;
}

// any chipset vendor will do: the first device seen triggers one scan of the whole bus
const DriverPciId kIds[] = {{0x8086, kPciAnyDevice}, {0x1022, kPciAnyDevice}, {0x10DE, kPciAnyDevice}, {0x1106, kPciAnyDevice}, {0x1039, kPciAnyDevice}};

}  // namespace

extern "C" {
extern const DriverInfo klein_driver;
const DriverInfo klein_driver = {KLEIN_DRIVER_ABI, "Network card detect", kIds, 5, probe};
}
