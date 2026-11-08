#pragma once
// Liquid glass, after glisse (github.com/0kzh/glisse): a rounded-rect lens
// whose rim band bends what's behind it inward along the slope of a shallow
// dome, splits colours slightly (dispersion), catches light on its edge, and
// frosts (blurs + saturates) the backdrop.
#include "gfx/canvas.h"

struct GlassStyle {
    float radius = 16;          // corner radius (px)
    float blur = 8;             // frost radius (px), 0 = clear lens
    float depth = 0.25f;        // width of the bending rim band (share of shorter side, or px)
    float dome = 0.5f;          // sag of the dome whose slope sets the bend (share or px)
    float strength = 0.45f;     // how far the rim reaches inward (share of shorter side, or px)
    bool relative = true;       // depth/dome/strength are shares of the shorter side
    float dispersion = 0.4f;    // red bends further than blue (1.0 = a fifth further)
    float saturation = 1.5f;    // backdrop saturation boost
    Color tint = rgba(255, 255, 255, 46);   // frost tint over the backdrop
    float shine = 0.55f;        // strength of the lit rim
    float light_angle = 45;     // degrees
};

// Built-in looks used by the shell.
GlassStyle glass_style_panel(bool dark);    // dock, menus, control center
GlassStyle glass_style_bar(bool dark);      // menubar (thin)
GlassStyle glass_style_window(bool dark);   // window titlebars / sidebars
GlassStyle glass_style_control(bool dark);  // small controls (buttons, toggles)

// Render glass over `dst` within shape rect `r` (device coords of dst), using
// the current contents of dst under it as the backdrop. Only pixels inside
// `clip` are written. The backdrop needs a margin of ~blur*2 px around r to be
// valid (the compositor guarantees this).
void glass_render(Canvas& dst, Rect r, const GlassStyle& style, Rect clip);

// Margin around a glass rect that must be valid backdrop.
int glass_margin(const GlassStyle& style);

// Cheaper frosting for slow CPUs: coarser backdrop, no colour dispersion.
void glass_set_lite(bool on);
