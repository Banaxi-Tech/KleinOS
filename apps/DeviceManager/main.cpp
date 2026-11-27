// Device Manager — every PCI device and the driver bound to it.
#include "ui/window.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "drivers/pci.h"
#include "sys/driver.h"
#include "lib/string.h"
#include "lib/kstring.h"

namespace {

constexpr int kRowH = 52;

const char* vendor_name(uint16_t v) {
    switch (v) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD/ATI";
    case 0x10DE: return "NVIDIA";
    case 0x10EC: return "Realtek";
    case 0x11AB: return "Marvell";
    case 0x168C: return "Qualcomm Atheros";
    case 0x14E4: return "Broadcom";
    case 0x1814: return "Ralink";
    case 0x1106: return "VIA";
    case 0x1039: return "SiS";
    case 0x1AF4: return "Virtio";
    case 0x1234: return "QEMU";
    case 0x1B36: return "Red Hat";
    default: return "Unknown vendor";
    }
}

// what to show for a device with no .dri bound to it
const char* unbound_status(const PciDevice* d) {
    if (d->class_code == 0x06) return "No driver needed";
    if (d->class_code == 0x03) return "Firmware display";
    if (d->class_code == 0x01 || (d->class_code == 0x0C && d->subclass == 0x03)) return "Kernel, if supported";
    return "No driver";
}

class DeviceManagerWindow : public Window {
public:
    DeviceManagerWindow() : Window("Device Manager", 620, 480, WF_RESIZABLE) { set_min_size(420, 300); }

    void on_scroll(int, int, int delta) override {
        scroll_ += delta * 28;
        int max = pci_count() * kRowH + 16 - height();
        if (scroll_ > max) scroll_ = max;
        if (scroll_ < 0) scroll_ = 0;
        invalidate();
    }

    void on_paint(Canvas& c) override {
        const Theme& t = theme();
        c.clear(t.window_bg);
        int w = width();
        for (int i = 0; i < pci_count(); i++) {
            int y = 8 + i * kRowH - scroll_;
            if (y + kRowH < 0 || y > height()) continue;
            const PciDevice* d = pci_get(i);
            const char* drv = driver_for_device(d);
            bool generic = driver_is_fallback(d);
            Color dot = generic ? hex(0xF5A623) : drv ? hex(0x34C759) : t.text_secondary;
            c.fill_circle(26, y + 25, 5, dot);
            char id[64];
            snprintf(id, sizeof(id), "%s  %04x:%04x", vendor_name(d->vendor), d->vendor, d->device);
            c.draw_text_in(Rect(44, y + 7, w / 2 - 44, 20), pci_class_name(d->class_code, d->subclass), t.text, 13, Font::Medium);
            c.draw_text_in(Rect(44, y + 27, w / 2 - 44, 18), id, t.text_secondary, 12, Font::Regular);
            c.draw_text_in(Rect(w / 2, y + 7, w / 2 - 16, 20), drv ? drv : unbound_status(d), t.text, 13, Font::Regular);
            if (generic) c.draw_text_in(Rect(w / 2, y + 27, w / 2 - 16, 18), "Identified only, cannot go online", hex(0xF5A623), 12, Font::Regular);
            c.fill_rect(Rect(44, y + kRowH - 1, w - 60, 1), t.separator);
        }
    }

private:
    int scroll_ = 0;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x5AC8FA), hex(0x1F6FD6));
    float u = r.w / 96.0f;
    Rect chip(r.x + (int)(30 * u), r.y + (int)(30 * u), (int)(36 * u), (int)(36 * u));
    c.fill_round_rect(chip, (int)(5 * u), hex(0xFFFFFF));
    for (int i = 0; i < 3; i++) {
        int off = (int)((36 + i * 12) * u);
        c.fill_rect(Rect(r.x + off, r.y + (int)(20 * u), (int)(4 * u), (int)(56 * u)), hex(0xFFFFFF));
        c.fill_rect(Rect(r.x + (int)(20 * u), r.y + off, (int)(56 * u), (int)(4 * u)), hex(0xFFFFFF));
    }
}

Window* create() { return new DeviceManagerWindow(); }

AppRegistrar reg({"devicemanager", "Device Manager", icon, create, "System", true, false, 100, "PCI devices and their drivers"});

}  // namespace
