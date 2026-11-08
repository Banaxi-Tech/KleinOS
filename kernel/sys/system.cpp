#include "sys/system.h"
#include "boot/multiboot2.h"
#include "drivers/framebuffer.h"
#include "drivers/power.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "lib/string.h"
#include "lib/log.h"
#include "build_number.h"
#include "fs/vfs.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "sys/dl.h"
#include "sys/driver.h"
#include "sys/panic.h"
#include "sys/session.h"
#include "ui/wm.h"
#include "ui/app.h"
#include "net/net.h"
#include "lib/math.h"

static BootMode g_mode = BootMode::Live;
static PollHook g_hooks[16];
static int g_hook_count;

void system_create_home(const char* login);

BootMode system_boot_mode() { return g_mode; }
const char* system_boot_mode_name() {
    switch (g_mode) {
    case BootMode::Installer: return "installer";
    case BootMode::Installed: return "installed";
    default: return "live";
    }
}
int system_build_number() { return KLEIN_BUILD_NUMBER; }
bool system_is_dev_build() { return KLEIN_DEV_BUILD; }
const char* system_version_string() {
    static char buf[96];
    if (!buf[0])
        snprintf(buf, sizeof(buf), "KleinOS %s.%d %s%s", KLEIN_VERSION, KLEIN_BUILD_NUMBER, KLEIN_CODENAME,
                 KLEIN_DEV_BUILD ? " (development)" : "");
    return buf;
}

void system_add_poll_hook(PollHook h) {
    if (g_hook_count < 16) g_hooks[g_hook_count++] = h;
}

void system_create_home(const char* login) {
    String home = String("/Users/") + login;
    const char* dirs[] = {"Desktop", "Documents", "Downloads", "Pictures", "Music"};
    for (const char* d : dirs) vfs::mkdir(vfs::join(home.c_str(), d).c_str());
    String welcome = home + "/Documents/Welcome.txt";
    if (!vfs::exists(welcome.c_str()))
        vfs::write_text(welcome.c_str(),
                        "Welcome to KleinOS!\n\n"
                        "- Press Super+Space (or Ctrl+Space) for Spotlight search.\n"
                        "- Press F4 or click the grid icon in the Dock for Launchpad.\n"
                        "- Drag windows by their glass title bar; double-click it to zoom.\n"
                        "- Right-click the desktop to change the wallpaper.\n"
                        "- Open Control Center from the menu bar to toggle Dark Mode and\n"
                        "  adjust the Liquid Glass intensity.\n\n"
                        "Apps live in /System/Apps as .dl dynamic libraries.\n");
}

static void boot_splash(const char* status, float progress) {
    FramebufferInfo& fb = framebuffer();
    static Canvas* splash;
    if (!splash) splash = new Canvas(fb.width, fb.height);
    Canvas& c = *splash;
    c.clear(hex(0x000000));
    int cx = fb.width / 2, cy = fb.height / 2 - 40;
    // logo: glass squircle with K
    Rect lr(cx - 48, cy - 48, 96, 96);
    c.fill_gradient(lr, hex(0x5E8BFF), hex(0x7A3CFF), 26);
    c.fill_gradient(Rect(lr.x + 2, lr.y + 2, lr.w - 4, lr.h / 2), rgba(255, 255, 255, 90), rgba(255, 255, 255, 0), 24);
    c.stroke_round_rect(lr, 26, rgba(255, 255, 255, 120), 1.5f);
    c.draw_line(cx - 15, cy - 24, cx - 15, cy + 24, hex(0xFFFFFF), 8);
    c.draw_line(cx - 11, cy + 3, cx + 17, cy - 24, hex(0xFFFFFF), 8);
    c.draw_line(cx - 4, cy - 3, cx + 18, cy + 24, hex(0xFFFFFF), 8);
    Rect bar(cx - 110, cy + 110, 220, 6);
    c.fill_round_rect(bar, 3, rgba(255, 255, 255, 50));
    c.fill_round_rect(Rect(bar.x, bar.y, kmax(6, (int)(bar.w * progress)), bar.h), 3, hex(0xFFFFFF));
    c.draw_text_in(Rect(0, cy + 130, fb.width, 20), status, rgba(255, 255, 255, 120), 12, Font::Regular, Align::Center);
    c.draw_text_in(Rect(0, fb.height - 40, fb.width, 20), system_version_string(), rgba(255, 255, 255, 70), 11, Font::Regular, Align::Center);
    for (uint32_t y = 0; y < fb.height; y++) memcpy((uint8_t*)fb.address + y * fb.pitch, c.row(y), fb.width * 4);
}

[[noreturn]] void system_main() {
    char mode[32] = "live";
    boot_arg("mode", mode, sizeof(mode));
    if (strcmp(mode, "installer") == 0) g_mode = BootMode::Installer;
    else if (strcmp(mode, "installed") == 0) g_mode = BootMode::Installed;
    klog("%s, boot mode: %s", system_version_string(), system_boot_mode_name());

    vfs::init();
    const BootModule* initrd = boot_module("initrd");
    if (!initrd) kpanic("initrd module missing (check grub.cfg module2 line)");
    vfs::mount_initrd(initrd->data, initrd->size);
    if (!font_init()) kpanic("could not load system fonts");

    boot_splash("Starting KleinOS…", 0.1f);
    dl_init();
    boot_splash("Loading libraries…", 0.25f);
    int libs = dl_load_dir("/System/Libraries", true);
    boot_splash("Loading applications…", 0.45f);
    int apps = dl_load_dir("/System/Apps", false);
    app_set_loading_module(nullptr);
    klog("system: %d libraries, %d app modules, %d apps registered", libs, apps, app_count());

    boot_splash("Loading drivers…", 0.6f);
    int drivers = driver_load_dir("/System/Drivers");
    app_set_loading_module(nullptr);
    klog("system: %d drivers", drivers);

    boot_splash("Starting network…", 0.7f);
    net::init();
    boot_splash("Starting desktop…", 0.9f);

    wm::init();
    session_start(g_mode);

    char autostart[64];
    if (boot_arg("autostart", autostart, sizeof(autostart))) {
        // comma separated list of app ids
        char* p = autostart;
        while (*p) {
            char* e = strchr(p, ',');
            if (e) *e = 0;
            app_launch(p);
            if (!e) break;
            p = e + 1;
        }
    }

    for (;;) {
        wm::frame();
        net::poll();
        for (int i = 0; i < g_hook_count; i++) g_hooks[i]();
        speaker_tick();
        asm volatile("sti; hlt");
    }
}
