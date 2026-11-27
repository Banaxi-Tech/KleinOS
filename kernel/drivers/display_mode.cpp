#include "drivers/display_mode.h"
#include "drivers/framebuffer.h"
#include "drivers/pci.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "boot/multiboot2.h"
#include "lib/log.h"
#include "lib/string.h"

namespace {

// ---------------------------------------------------------------- QEMU / VirtualBox
constexpr uint16_t kDispiIndex = 0x1CE, kDispiData = 0x1CF;
enum { DISPI_ID = 0, DISPI_XRES = 1, DISPI_YRES = 2, DISPI_BPP = 3, DISPI_ENABLE = 4, DISPI_VIRT_W = 6, DISPI_VIDEO_MEM = 10 };

uint16_t dispi_read(uint16_t reg) { outw(kDispiIndex, reg); return inw(kDispiData); }
void dispi_write(uint16_t reg, uint16_t v) { outw(kDispiIndex, reg); outw(kDispiData, v); }

bool set_dispi_mode() {
    uint16_t id = dispi_read(DISPI_ID);
    if (id < 0xB0C0 || id > 0xB0CF) return false;
    constexpr uint32_t w = 1280, h = 800;
    FramebufferInfo& fb = framebuffer();
    if (fb.width == w && fb.height == h) return false;
    uint32_t vram_64k = dispi_read(DISPI_VIDEO_MEM);
    if (vram_64k && w * h * 4 > vram_64k * 65536u) return false;
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, w);
    dispi_write(DISPI_YRES, h);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_ENABLE, 0x41);     // enabled, linear framebuffer
    dispi_write(DISPI_VIRT_W, w);
    if (dispi_read(DISPI_XRES) != w || dispi_read(DISPI_YRES) != h) {
        klog("display: virtual adapter refused %ux%u", w, h);
        return false;
    }
    fb.width = w;
    fb.height = h;
    fb.pitch = w * 4;
    klog("display: virtual adapter set to %ux%u", w, h);
    return true;
}

// ---------------------------------------------------------------- Intel Pineview (GMA 3150)
// What the chip reports before anything is changed, for diagnosing the switch below.
void log_pineview_state(const PciDevice* gpu) {
    volatile uint32_t* mmio = (volatile uint32_t*)(uintptr_t)(gpu->bar[0] & ~0xFu);
    auto rd = [&](uint32_t reg) { return mmio[reg / 4]; };
    uint32_t lvds = rd(0x61180), pipe = (lvds >> 30) & 1, base = pipe ? 0x61000 : 0x60000;
    uint32_t htotal = rd(base), vtotal = rd(base + 0x0C), src = rd(base + 0x1C);
    klog("display: intel lvds %08x pipe %c panel %ux%u source %ux%u scaler %08x gmch %04x", lvds, 'A' + pipe,
         (htotal & 0xFFF) + 1, (vtotal & 0xFFF) + 1, ((src >> 16) & 0xFFF) + 1, (src & 0xFFF) + 1, rd(0x61230),
         pci_read16(0, 0, 0, 0x52));
    klog("display: intel plane A %08x stride %u, plane B %08x stride %u", rd(0x70180), rd(0x70188), rd(0x71180), rd(0x71188));
}

// The BIOS keeps the panel at its real timing and scales a smaller picture up to it,
// so going native needs a bigger source picture and the scaler off, with no clock
// changes. The pipe must be stopped for that, and the panel's registers are
// write-protected until unlocked. Every write is checked and undone on failure.
bool set_pineview_mode(const PciDevice* gpu) {
    FramebufferInfo& fb = framebuffer();
    if (fb.bpp != 32) return false;
    volatile uint32_t* mmio = (volatile uint32_t*)(uintptr_t)(gpu->bar[0] & ~0xFu);
    auto rd = [&](uint32_t reg) { return mmio[reg / 4]; };
    auto wr = [&](uint32_t reg, uint32_t v) { mmio[reg / 4] = v; };
    constexpr uint32_t kEnable = 0x80000000u;
    constexpr uint32_t kPfit = 0x61230, kPanelControl = 0x61204, kSelfRefresh = 0x7003C, kPlane = 0x71180;
    constexpr uint32_t kPipeConf = 0x71008, kPipeSrcB = 0x6101C;

    // Only the arrangement measured on the N220: panel on pipe B, fed by plane B in BGRX.
    uint32_t lvds = rd(0x61180);
    uint32_t control = rd(kPlane);
    if (!(lvds & kEnable) || !((lvds >> 30) & 1)) { klog("display: panel is not on pipe B, leaving it"); return false; }
    if (!(control & kEnable) || ((control >> 24) & 3) != 1 || ((control >> 26) & 0xF) != 6) {
        klog("display: plane B is not set up as expected, leaving it");
        return false;
    }
    uint32_t htotal = rd(0x61000), vtotal = rd(0x6100C), src = rd(kPipeSrcB), pfit = rd(kPfit);
    uint32_t panel_w = (htotal & 0xFFF) + 1, panel_h = (vtotal & 0xFFF) + 1;
    uint32_t src_w = ((src >> 16) & 0xFFF) + 1, src_h = (src & 0xFFF) + 1;
    if (src_w != fb.width || src_h != fb.height) { klog("display: source is not the boot mode, leaving it"); return false; }
    if (panel_w == fb.width && panel_h == fb.height) return false;
    if (panel_w < fb.width || panel_h < fb.height || panel_w > 1920 || panel_h > 1200) { klog("display: panel size looks wrong, leaving it"); return false; }
    if (((pci_read16(0, 0, 0, 0x52) >> 4) & 7) < 3) { klog("display: too little graphics memory, leaving it"); return false; }

    uint32_t base = rd(kPlane + 4), stride = rd(kPlane + 8), size = rd(kPlane + 0x10), conf = rd(kPipeConf);
    uint32_t panel_control = rd(kPanelControl), self_refresh = rd(kSelfRefresh);
    uint32_t new_src = ((panel_w - 1) << 16) | (panel_h - 1), new_size = ((panel_h - 1) << 16) | (panel_w - 1);

    wr(kPanelControl, (panel_control & 0xFFFFu) | 0xABCD0000u);     // unlock
    wr(kSelfRefresh, self_refresh & ~(1u << 30));                   // its FIFO settings were for 800 wide
    wr(kPlane, control & ~kEnable);
    wr(kPlane + 4, base);
    sleep_ms(40);
    wr(kPipeConf, conf & ~kEnable);
    sleep_ms(60);

    wr(kPfit, 0);
    wr(kPipeSrcB, new_src);
    wr(kPlane + 8, panel_w * 4);
    wr(kPlane + 0x10, new_size);
    bool ok = rd(kPipeSrcB) == new_src && rd(kPlane + 8) == panel_w * 4 && rd(kPlane + 0x10) == new_size && !(rd(kPfit) & kEnable);
    if (!ok) {
        klog("display: chip refused the new size (source %08x stride %u size %08x scaler %08x), restoring",
             rd(kPipeSrcB), rd(kPlane + 8), rd(kPlane + 0x10), rd(kPfit));
        wr(kPipeSrcB, src);
        wr(kPlane + 8, stride);
        wr(kPlane + 0x10, size);
        wr(kPfit, pfit);
        wr(kSelfRefresh, self_refresh);
    }
    else {
        // the larger picture reaches graphics memory nothing has drawn to yet
        memset(fb.address, 0, (size_t)panel_w * 4 * panel_h);
    }
    wr(kPipeConf, conf);
    sleep_ms(40);
    wr(kPlane, control);
    wr(kPlane + 4, base);
    sleep_ms(40);
    wr(kPanelControl, panel_control);
    if (!ok) return false;

    fb.width = panel_w;
    fb.height = panel_h;
    fb.pitch = panel_w * 4;
    klog("display: panel set to %ux%u", panel_w, panel_h);
    return true;
}

}  // namespace

bool display_set_native_mode() {
    char arg[8];
    if (boot_arg("nativeres", arg, sizeof(arg)) && !strcmp(arg, "off")) {
        klog("display: staying at boot mode (nativeres=off)");
        return false;
    }
    const PciDevice* bochs = pci_find(0x1234, 0x1111);
    if (!bochs) bochs = pci_find(0x80EE, 0xBEEF);
    if (bochs) return set_dispi_mode();
    const uint16_t pineview[] = {0xA001, 0xA011};
    for (uint16_t id : pineview)
        if (const PciDevice* gpu = pci_find(0x8086, id)) { log_pineview_state(gpu); return set_pineview_mode(gpu); }
    return false;
}
