// Fallback for Wi-Fi cards that have no driver of their own. It takes the card so
// the Device Manager can name it, but PCI gives no common way to send or receive
// on a network card, so this does not bring the network up and leaves the card untouched.
#include "drivers/pci.h"
#include "lib/log.h"
#include "sys/driver.h"

namespace {

bool probe(const PciDevice* d) {
    klog("generic: Wi-Fi card %04x:%04x has no driver of its own, it cannot go online yet", d->vendor, d->device);
    return true;
}

}  // namespace

extern "C" {
extern const DriverInfo klein_driver;
extern const DriverFallback klein_driver_fallback;
const DriverInfo klein_driver = {KLEIN_DRIVER_ABI, "Generic Wi-Fi Adapter", nullptr, 0, probe};
const DriverFallback klein_driver_fallback = {0x02, 0x80};
}
