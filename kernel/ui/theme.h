#pragma once
// System appearance. Always read colors through theme() so apps follow
// light/dark mode and the accent color.
#include "gfx/color.h"

struct Theme {
    bool dark;
    Color accent;            // selection / primary buttons
    Color accent_text;       // text on accent
    Color window_bg;         // window content background
    Color sidebar_bg;        // (translucent) sidebar tint
    Color control_bg;        // buttons, fields
    Color control_border;
    Color field_bg;          // text fields
    Color text;              // primary text
    Color text_secondary;    // labels, captions
    Color text_tertiary;     // placeholders, disabled
    Color separator;
    Color selection;         // selected list row bg (accent-based)
    Color hover;             // hovered row/control
    Color shadow;            // window shadow color
    Color destructive;       // red
    Color success;           // green
    Color warning;           // orange
};

const Theme& theme();
void theme_set_dark(bool dark);
void theme_set_accent(Color accent);
bool theme_is_dark();

// Standard metrics
constexpr int kTitlebarHeight = 40;
constexpr int kWindowRadius = 14;
constexpr int kMenubarHeight = 30;
constexpr int kControlHeight = 30;
constexpr int kControlRadius = 8;
constexpr int kFontSizeSmall = 12;
constexpr int kFontSize = 14;
constexpr int kFontSizeLarge = 17;
constexpr int kFontSizeTitle = 24;

// Accent palette (Settings offers these)
constexpr Color kAccentBlue = hex(0x0A84FF);
constexpr Color kAccentPurple = hex(0xA550F7);
constexpr Color kAccentPink = hex(0xF74F9E);
constexpr Color kAccentRed = hex(0xFF453A);
constexpr Color kAccentOrange = hex(0xFF9F0A);
constexpr Color kAccentYellow = hex(0xFFD60A);
constexpr Color kAccentGreen = hex(0x30D158);
constexpr Color kAccentGraphite = hex(0x8E8E93);
