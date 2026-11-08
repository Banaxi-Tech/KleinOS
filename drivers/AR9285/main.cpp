// Qualcomm Atheros AR9285 (PCIe 802.11n, 168C:002B).
//
// Stage 1: finds the card, resets it, confirms the silicon revision and reads
// its MAC address and regulatory domain from EEPROM. The radio stays off: there
// is no transmit or receive yet, so it does not register as a network card.
// Register values follow Linux ath9k (reg.h, hw.c, eeprom_4k.c).
#include <stddef.h>
#include "arch/cpu.h"
#include "drivers/pci.h"
#include "lib/log.h"
#include "sys/driver.h"

namespace {

constexpr uint32_t AR_RC = 0x4000, AR_RC_AHB = 1;
constexpr uint32_t AR_SREV = 0x4020, AR_SREV_VERSION2 = 0xFFFC0000, AR_SREV_TYPE2_S = 12, AR_SREV_REVISION2 = 0xF00;
constexpr uint32_t AR_SREV_VERSION_9285 = 0xC0;
constexpr uint32_t AR_EEPROM_STATUS_DATA = 0x407C, AR_EEPROM_BUSY = 0x10000, AR_EEPROM_PROT = 0x40000;
constexpr uint32_t AR_RTC_RC = 0x7000, AR_RTC_RC_M = 3, AR_RTC_RC_MAC_WARM = 1;
constexpr uint32_t AR_RTC_RESET = 0x7040, AR_RTC_STATUS = 0x7044, AR_RTC_STATUS_M = 0xF, AR_RTC_STATUS_ON = 2;
constexpr uint32_t AR_RTC_FORCE_WAKE = 0x704C, AR_RTC_FORCE_WAKE_BOTH = 3;
constexpr uint32_t kEepromBase = 0x2000, kEepromMagic = 0xA55A;
constexpr int kEep4kStart = 64, kTimeoutUs = 100000;

volatile uint8_t* g_mmio;

uint32_t rd(uint32_t r) { return *(volatile uint32_t*)(g_mmio + r); }
void wr(uint32_t r, uint32_t v) { *(volatile uint32_t*)(g_mmio + r) = v; }
void delay_us(int us) { for (int i = 0; i < us; i++) io_wait(); }   // port 0x80 takes about 1us

bool wait_reg(uint32_t r, uint32_t mask, uint32_t want) {
    for (int t = 0; t < kTimeoutUs; t += 10) {
        if ((rd(r) & mask) == want) return true;
        delay_us(10);
    }
    return false;
}

bool warm_reset() {
    wr(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_BOTH);
    wr(AR_RC, AR_RC_AHB);
    wr(AR_RTC_RC, AR_RTC_RC_MAC_WARM);
    delay_us(50);
    wr(AR_RTC_RC, 0);
    if (!wait_reg(AR_RTC_RC, AR_RTC_RC_M, 0)) return false;
    wr(AR_RC, 0);
    wr(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_BOTH);
    return true;
}

bool power_on_reset() {
    wr(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_BOTH);
    wr(AR_RC, AR_RC_AHB);
    wr(AR_RTC_RESET, 0);
    delay_us(2);
    wr(AR_RC, 0);
    wr(AR_RTC_RESET, 1);
    if (!wait_reg(AR_RTC_STATUS, AR_RTC_STATUS_M, AR_RTC_STATUS_ON)) return false;
    return warm_reset();
}

bool eeprom_read(int word, uint16_t* out) {
    (void)rd(kEepromBase + (word << 2));   // the read latches the address
    if (!wait_reg(AR_EEPROM_STATUS_DATA, AR_EEPROM_BUSY | AR_EEPROM_PROT, 0)) return false;
    *out = rd(AR_EEPROM_STATUS_DATA) & 0xFFFF;
    return true;
}

bool probe(const PciDevice* d) {
    if (d->bar[0] & 1) { klog("ar9285: BAR0 is not memory-mapped"); return false; }
    if ((d->bar[0] & 6) == 4 && d->bar[1]) { klog("ar9285: BAR0 is above 4 GiB"); return false; }
    pci_enable(d);
    g_mmio = (volatile uint8_t*)(uintptr_t)(d->bar[0] & ~0xFu);
    if (!power_on_reset()) { klog("ar9285: chip did not come out of reset"); return false; }

    uint32_t srev = rd(AR_SREV);
    uint32_t version = (srev & AR_SREV_VERSION2) >> AR_SREV_TYPE2_S;
    if ((srev & 0xFF) != 0xFF || version != AR_SREV_VERSION_9285) {
        klog("ar9285: unexpected silicon revision %08x", srev);
        return false;
    }

    uint16_t magic = 0, reg = 0, w[3];
    if (!eeprom_read(0, &magic)) { klog("ar9285: EEPROM not responding"); return false; }
    bool swapped = magic == ((kEepromMagic >> 8) | ((kEepromMagic & 0xFF) << 8));
    if (magic != kEepromMagic && !swapped) { klog("ar9285: bad EEPROM magic %04x", magic); return false; }
    // base header starts at word 64; regDmn[0] is word 4 of it, macAddr word 6
    for (int i = 0; i < 3; i++) if (!eeprom_read(kEep4kStart + 6 + i, &w[i])) { klog("ar9285: EEPROM read failed"); return false; }
    if (!eeprom_read(kEep4kStart + 4, &reg)) reg = 0;
    uint8_t mac[6];
    for (int i = 0; i < 3; i++) {
        uint16_t v = swapped ? (uint16_t)((w[i] >> 8) | (w[i] << 8)) : w[i];
        mac[i * 2] = v & 0xFF;
        mac[i * 2 + 1] = v >> 8;
    }
    klog("ar9285: found rev %u, MAC %02x:%02x:%02x:%02x:%02x:%02x, regulatory domain %04x",
         (srev & AR_SREV_REVISION2) >> 8, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], reg);
    klog("ar9285: radio not implemented yet; Wi-Fi is unavailable");
    return true;
}

const DriverPciId kIds[] = {{0x168C, 0x002B}};

}  // namespace

extern "C" {
extern const DriverInfo klein_driver;
const DriverInfo klein_driver = {KLEIN_DRIVER_ABI, "Atheros AR9285", kIds, 1, probe};
}
