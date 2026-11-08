#include "ui/theme.h"
#include "ui/wm.h"

static Theme g_theme;
static Color g_accent = kAccentBlue;
static bool g_dark = false;

static void rebuild() {
    Theme& t = g_theme;
    t.dark = g_dark;
    t.accent = g_accent;
    t.accent_text = hex(0xFFFFFF);
    if (g_dark) {
        t.window_bg = hex(0x1E1E22);
        t.sidebar_bg = rgba(40, 40, 46, 120);
        t.control_bg = hex(0x3A3A40);
        t.control_border = rgba(255, 255, 255, 30);
        t.field_bg = hex(0x2A2A2F);
        t.text = hex(0xF2F2F7);
        t.text_secondary = rgba(235, 235, 245, 160);
        t.text_tertiary = rgba(235, 235, 245, 90);
        t.separator = rgba(255, 255, 255, 28);
        t.hover = rgba(255, 255, 255, 18);
        t.shadow = rgba(0, 0, 0, 150);
    } else {
        t.window_bg = hex(0xFBFBFD);
        t.sidebar_bg = rgba(240, 240, 245, 110);
        t.control_bg = hex(0xFFFFFF);
        t.control_border = rgba(0, 0, 0, 34);
        t.field_bg = hex(0xFFFFFF);
        t.text = hex(0x1C1C1E);
        t.text_secondary = rgba(60, 60, 67, 170);
        t.text_tertiary = rgba(60, 60, 67, 100);
        t.separator = rgba(0, 0, 0, 24);
        t.hover = rgba(0, 0, 0, 12);
        t.shadow = rgba(0, 0, 0, 90);
    }
    t.selection = with_alpha(g_accent, g_dark ? 200 : 230);
    t.destructive = hex(0xFF453A);
    t.success = hex(0x30D158);
    t.warning = hex(0xFF9F0A);
}

const Theme& theme() {
    if (!g_theme.accent) rebuild();
    return g_theme;
}

void theme_set_dark(bool dark) {
    g_dark = dark;
    rebuild();
    for (Window* w : wm::windows()) {
        w->invalidate();
        for (Widget* x : w->widgets()) (void)x;
    }
    wm::damage_all();
}

void theme_set_accent(Color a) {
    g_accent = a;
    rebuild();
    for (Window* w : wm::windows()) w->invalidate();
    wm::damage_all();
}

bool theme_is_dark() { return g_dark; }
