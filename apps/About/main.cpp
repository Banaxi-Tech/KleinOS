// About This KleinOS — system overview window.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "sys/system.h"
#include "sys/dl.h"
#include "boot/multiboot2.h"
#include "drivers/framebuffer.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "lib/heap.h"

namespace {

void draw_logo_mark(Canvas& c, Rect r) {
    float rad = r.w * 0.24f;
    c.draw_shadow(r, rad, r.w * 0.08f, rgba(0, 0, 0, 80), 0, r.w / 20);
    c.fill_gradient(r, hex(0x5E8BFF), hex(0x7A3CFF), rad);
    c.fill_gradient(Rect(r.x + 2, r.y + 2, r.w - 4, r.h / 2), rgba(255, 255, 255, 90), rgba(255, 255, 255, 0), rad - 2);
    c.stroke_round_rect(r, rad, rgba(255, 255, 255, 120), 1.5f);
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, u = r.w / 96.0f;
    c.draw_line(cx - 15 * u, cy - 24 * u, cx - 15 * u, cy + 24 * u, hex(0xFFFFFF), 8 * u);
    c.draw_line(cx - 11 * u, cy + 3 * u, cx + 17 * u, cy - 24 * u, hex(0xFFFFFF), 8 * u);
    c.draw_line(cx - 4 * u, cy - 3 * u, cx + 18 * u, cy + 24 * u, hex(0xFFFFFF), 8 * u);
}

class AboutWindow : public Window {
public:
    AboutWindow() : Window("", 420, 470, WF_NO_ZOOM) {
        auto* more = (Button*)add(new Button(Rect(110, 410, 200, 32), "More Info…"));
        more->on_click = [] { app_launch("settings"); };
    }

    void on_paint(Canvas& c) override {
        Window::on_paint(c);
        const Theme& t = theme();
        draw_logo_mark(c, Rect(150, 24, 120, 120));
        c.draw_text_in(Rect(0, 160, width(), 34), "KleinOS", t.text, 28, Font::Bold, Align::Center);
        char ver[96];
        snprintf(ver, sizeof(ver), "Version %s.%d %s", KLEIN_VERSION, system_build_number(), KLEIN_CODENAME);
        c.draw_text_in(Rect(0, 196, width(), 20), ver, t.text_secondary, 13, Font::Regular, Align::Center);

        FramebufferInfo& fb = framebuffer();
        char mem[64], disp[64], up[64], mods[64];
        snprintf(mem, sizeof(mem), "%llu MB", (unsigned long long)(boot_info().mem_total >> 20));
        snprintf(disp, sizeof(disp), "%u × %u, 32-bit", fb.width, fb.height);
        uint64_t s = timer_ms() / 1000;
        snprintf(up, sizeof(up), "%lluh %02llum %02llus", (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
        snprintf(mods, sizeof(mods), "%zu .dl modules, %d apps", dl_modules().size(), app_count());
        const char* rows[][2] = {
            {"Processor", cpu_brand()}, {"Memory", mem}, {"Display", disp},
            {"Modules", mods}, {"Uptime", up}, {"Mode", system_boot_mode_name()},
        };
        Rect box(30, 232, width() - 60, 6 * 26 + 12);
        draw_group_box(c, box);
        for (int i = 0; i < 6; i++) {
            int y = box.y + 6 + i * 26;
            c.draw_text_in(Rect(box.x + 14, y, 110, 26), rows[i][0], t.text, 13, Font::Medium);
            c.draw_text_in(Rect(box.x + 120, y, box.w - 134, 26), rows[i][1], t.text_secondary, 13, Font::Regular, Align::Right);
        }
        set_ticking(true);
    }

    void on_tick(uint64_t now) override {
        static uint64_t last;
        if (now / 1000 != last) { last = now / 1000; invalidate(); }
    }
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x8E9AAF), hex(0x4A5568));
    draw_symbol(c, r.inset(r.w / 4), Symbol::Info, hex(0xFFFFFF), r.w / 22.0f);
}

Window* create() { return new AboutWindow(); }

AppRegistrar reg({"about", "About KleinOS", icon, create, "System", true, false, 100, "Information about this computer"});

}  // namespace
