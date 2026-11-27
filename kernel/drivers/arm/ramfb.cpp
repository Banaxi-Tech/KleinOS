// QEMU "ramfb": a framebuffer in guest RAM that the guest configures through fw_cfg. No PCI or firmware
// driver needed, which makes it the simplest display for a bare-metal ARM guest.
#include "drivers/framebuffer.h"
#include "boot/dtb.h"
#include "boot/multiboot2.h"
#include "boot/handoff.h"
#include "lib/log.h"
#include "lib/string.h"
#include "sys/panic.h"

namespace {

enum { FW_CFG_FILE_DIR = 0x19 };
constexpr uint32_t kDmaError = 1, kDmaSelect = 8, kDmaWrite = 0x10;

volatile uint8_t* g_fwcfg;

inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }

void select(uint16_t key) { *(volatile uint16_t*)(g_fwcfg + 8) = __builtin_bswap16(key); }
uint8_t read8() { return *(volatile uint8_t*)g_fwcfg; }
uint32_t read_be32() { uint32_t v = 0; for (int i = 0; i < 4; i++) v = (v << 8) | read8(); return v; }

struct [[gnu::packed]] DmaAccess { uint32_t control, length; uint64_t address; };
struct [[gnu::packed]] RamfbConfig { uint64_t addr; uint32_t fourcc, flags, width, height, stride; };

bool dma_write(uint16_t key, const void* data, uint32_t len) {
    alignas(16) static volatile DmaAccess acc;
    acc.control = bswap32((uint32_t)key << 16 | kDmaSelect | kDmaWrite);
    acc.length = bswap32(len);
    acc.address = __builtin_bswap64((uint64_t)(uintptr_t)data);
    asm volatile("dsb sy" ::: "memory");
    uint64_t a = (uint64_t)(uintptr_t)&acc;
    *(volatile uint32_t*)(g_fwcfg + 0x10) = bswap32((uint32_t)(a >> 32));
    *(volatile uint32_t*)(g_fwcfg + 0x14) = bswap32((uint32_t)a);       // the low half starts the transfer
    for (int spin = 0; spin < 1000000; spin++) {
        asm volatile("dsb sy" ::: "memory");
        uint32_t c = bswap32(acc.control);
        if (c & kDmaError) return false;
        if (!(c & ~kDmaError)) return true;
    }
    return false;
}

}  // namespace

// A framebuffer the UEFI firmware set up (BGRX), used when there is no ramfb.
static void use_gop() {
    const BootHandoff* h = (const BootHandoff*)boot_handoff;
    if (!h || h->magic != kHandoffMagic || !h->fb_addr) return;
    FramebufferInfo& fb = framebuffer();
    fb.address = (void*)h->fb_addr;
    fb.width = h->fb_width;
    fb.height = h->fb_height;
    fb.pitch = h->fb_pitch;
    fb.bpp = 32;
    klog("display: firmware framebuffer %ux%u", fb.width, fb.height);
}

void ramfb_init(const BootInfo& bi) {
    DtbNode n;
    uint64_t base = 0x09020000;
    if (dtb_find_compatible("qemu,fw-cfg-mmio", 0, &n)) base = n.reg_addr(0);
    g_fwcfg = (volatile uint8_t*)base;

    // fw_cfg signature check: selector 0 reads "QEMU"
    select(0);
    if (read8() != 'Q' || read8() != 'E' || read8() != 'M' || read8() != 'U') {
        klog("ramfb: no fw_cfg device at %p", (void*)base);
        use_gop();
        return;
    }

    select(FW_CFG_FILE_DIR);
    uint32_t count = read_be32();
    int ramfb_sel = -1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t size = read_be32();
        uint16_t sel = (uint16_t)((read8() << 8) | read8());
        read8(); read8();
        char name[56];
        for (int k = 0; k < 56; k++) name[k] = (char)read8();
        (void)size;
        if (!strcmp(name, "etc/ramfb")) { ramfb_sel = sel; break; }
    }
    if (ramfb_sel < 0) {
        klog("ramfb: device not present");
        use_gop();
        return;
    }

    uint32_t w, h;
    boot_resolution(&w, &h);

    static RamfbConfig cfg;
    cfg.addr = __builtin_bswap64(bi.fb_start);
    cfg.fourcc = bswap32(0x34325258);        // 'XR24' = XRGB8888
    cfg.flags = 0;
    cfg.width = bswap32(w);
    cfg.height = bswap32(h);
    cfg.stride = bswap32(w * 4);
    if (!dma_write((uint16_t)ramfb_sel, &cfg, sizeof(cfg))) {
        klog("ramfb: configuration failed");
        return;
    }
    memset((void*)bi.fb_start, 0, (size_t)w * h * 4);

    FramebufferInfo& fb = framebuffer();
    fb.address = (void*)bi.fb_start;
    fb.width = w;
    fb.height = h;
    fb.pitch = w * 4;
    fb.bpp = 32;
}
