#include "drivers/power.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "lib/log.h"

#if defined(__aarch64__)
#include "boot/dtb.h"
#include "lib/string.h"

// PSCI (Power State Coordination Interface) through HVC or SMC, whichever the device tree names.
static bool psci_uses_smc() {
    DtbNode n;
    if (dtb_find_compatible("arm,psci-1.0", 0, &n) || dtb_find_compatible("arm,psci-0.2", 0, &n) ||
        dtb_find_compatible("arm,psci", 0, &n))
        return n.method && !strcmp(n.method, "smc");
    return false;
}

static void psci_call(uint32_t fn) {
    register uint64_t x0 asm("x0") = fn;
    if (psci_uses_smc()) asm volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
    else asm volatile("hvc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
}

[[noreturn]] void power_reboot() {
    klog("power: reboot");
    cli();
    psci_call(0x84000009);     // SYSTEM_RESET
    for (;;) hlt();
}

[[noreturn]] void power_shutdown() {
    klog("power: shutdown");
    cli();
    psci_call(0x84000008);     // SYSTEM_OFF
    for (;;) hlt();
}

// No PC speaker on ARM boards.
void speaker_tone(uint32_t) {}
void speaker_beep(uint32_t, uint32_t) {}
void speaker_tick() {}
#else
[[noreturn]] void power_reboot() {
    klog("power: reboot");
    cli();
    // 8042 reset line
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {}
    outb(0x64, 0xFE);
    for (int i = 0; i < 1000000; i++) io_wait();
    // PCI reset register
    outb(0xCF9, 0x06);
    for (int i = 0; i < 1000000; i++) io_wait();
    // triple fault
    struct { uint16_t l; uint64_t b; } __attribute__((packed)) idt{0, 0};
    asm volatile("lidt %0; int3" ::"m"(idt));
    for (;;) hlt();
}

[[noreturn]] void power_shutdown() {
    klog("power: shutdown");
    cli();
    outw(0x604, 0x2000);    // QEMU (piix4 / q35 ACPI)
    outw(0xB004, 0x2000);   // Bochs, older QEMU
    outw(0x4004, 0x3400);   // VirtualBox
    for (;;) hlt();
}

static uint64_t g_beep_end;

void speaker_tone(uint32_t hz) {
    if (!hz) {
        outb(0x61, inb(0x61) & 0xFC);
        return;
    }
    uint32_t div = 1193180 / hz;
    outb(0x43, 0xB6);
    outb(0x42, div & 0xFF);
    outb(0x42, (div >> 8) & 0xFF);
    outb(0x61, inb(0x61) | 3);
}

void speaker_beep(uint32_t hz, uint32_t ms) {
    speaker_tone(hz);
    g_beep_end = timer_ms() + ms;
}

void speaker_tick() {
    if (g_beep_end && timer_ms() >= g_beep_end) {
        speaker_tone(0);
        g_beep_end = 0;
    }
}
#endif
