#include "drivers/sound.h"
#include "sys/session.h"
#include "ui/wm.h"
#include "ui/app.h"
#include "ui/shell.h"
#include "lib/kstring.h"
#include "lib/log.h"
#include "sys/sysconfig.h"
#include "sys/update.h"
#include "drivers/input.h"
#include "drivers/rtc.h"
#include "ui/theme.h"

static String g_login = "guest", g_full = "Guest";

const char* session_user() { return g_login.c_str(); }
const char* session_full_name() { return g_full.c_str(); }

void session_enter_desktop(const char* full_name, const char* login) {
    g_full = full_name;
    g_login = login;
    system_create_home(login);
    shell_set_user(full_name, login);
    wm::set_shell_visible(true);
    klog("session: %s (%s) logged in", full_name, login);
    sound_effect(SoundEffect::Startup);
    String msg = String("Welcome, ") + full_name + ". Press Ctrl+Space to search.";
    wm::notify("Welcome to KleinOS", msg.c_str());
    uint64_t reset_at = 0;
    if (sysconfig_take_reset_notice(login, &reset_at)) {
        DateTime t = unix_to_datetime(reset_at + rtc_utc_offset_minutes() * 60);
        String body = String::format("Reset in Recovery on %d %s at %02d:%02d. Not you? Change it in Settings.",
                                     t.day, month_name(t.month), t.hour, t.minute);
        wm::notify("Your password was reset", body.c_str(), "settings");
    }
}

void session_logout() {
    // close every window, then show the login window (or reset the live desktop)
    Vector<Window*> ws = wm::windows();
    for (Window* w : ws) w->close();
    wm::defer([] {
        if (system_boot_mode() == BootMode::Installed && app_find("login")) {
            wm::set_shell_visible(false);
            app_launch("login");
        } else {
            session_enter_desktop(g_full.c_str(), g_login.c_str());
        }
    });
}

void session_start(BootMode mode) {
    wm::set_logout_handler([] { session_logout(); });
    if (mode == BootMode::Installer && app_find("setup")) {
        wm::set_shell_visible(false);
        app_launch("setup");
        return;
    }
    if (mode == BootMode::Installed) {
        if (sysconfig_load()) {
            SysConfig& c = sysconfig();
            input_set_layout((KeyboardLayout)c.keyboard_layout);
            rtc_set_utc_offset_minutes(c.utc_offset_min);
            if (c.accent) theme_set_accent(c.accent);
            theme_set_dark(c.dark_mode);
            wm::set_wallpaper(c.wallpaper);
            // After 3 failed starts GRUB boots Recovery: open it before loading anything
            // else from the disk, in case that is what keeps failing.
            if (update::recovery_boot() && app_find("recovery")) {
                klog("session: starting Recovery");
                wm::set_shell_visible(false);
                app_launch("recovery");
                return;
            }
            userdata_load();
            userdata_enable_autosave();
            sysconfig_take_recovery_request();
            const char* first = c.setup_complete ? "login" : "oobe";
            if (app_find(first)) {
                wm::set_shell_visible(false);
                app_launch(first);
                return;
            }
            if (c.user_count) { session_enter_desktop(c.users[0].full_name, c.users[0].login); return; }
        }
    }
    session_enter_desktop("Guest", "guest");
}
