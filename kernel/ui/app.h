#pragma once
// App registry. Every app lives in apps/<Name>/ and is built into
// /System/Apps/<Name>.dl; at load time its static AppRegistrar registers it.
//
//   static void draw_icon(Canvas& c, Rect r) { icon_base(c, r, hex(0x...), hex(0x...)); ... }
//   static Window* create() { return new MyWindow(); }
//   static AppRegistrar reg({"myapp", "My App", draw_icon, create, "Utilities"});
#include "gfx/canvas.h"

class Window;

struct AppInfo {
    const char* id;                        // unique, lowercase: "calculator"
    const char* name;                      // display name: "Calculator"
    void (*draw_icon)(Canvas& c, Rect r);  // draw the app icon filling square r
    Window* (*create)();                   // open a new window
    const char* category = "Utilities";    // Productivity, Utilities, Games, Internet, System, Media, Creativity
    bool single_instance = true;           // re-focus existing window instead of opening another
    bool in_dock = false;                  // pinned in the Dock by default
    int dock_order = 100;                  // lower = further left
    const char* description = "";          // one line for Launchpad/Spotlight
};

struct AppRegistrar {
    AppRegistrar(const AppInfo& info);
};

int app_count();
const AppInfo* app_get(int index);
const AppInfo* app_find(const char* id);
// Launch (or focus) an app. Returns the window (or nullptr).
Window* app_launch(const char* id);
// Which .dl file registered this app ("Calculator.dl").
const char* app_module(const char* id);
void app_set_loading_module(const char* name);   // used by the loader

// ---- icon helpers (ui/icons.cpp)
// Squircle base with vertical gradient, inner highlight and soft edge.
void icon_base(Canvas& c, Rect r, Color top, Color bottom);
// A centered glyph string on an icon (e.g. emoji-less letters/symbols).
void icon_text(Canvas& c, Rect r, const char* text, Color color, float size_ratio = 0.5f, Font font = Font::Bold);
// Generic UI symbols, drawn into r with color (line icons).
enum class Symbol {
    Close, Minus, Plus, Check, ChevronLeft, ChevronRight, ChevronUp, ChevronDown, Search, Gear, Wifi, WifiOff,
    Battery, Volume, Sun, Moon, Bell, Power, Folder, File, Home, Trash, Refresh, Share, Play, Pause, Stop,
    SkipForward, SkipBack, Grid, List, Star, Heart, Lock, User, Download, Globe, Clock, Calendar, Edit,
    Info, Warning, Sidebar, Bluetooth, Airplane, Keyboard, Display, Cpu, Music, Image, Terminal, Restart,
};
void draw_symbol(Canvas& c, Rect r, Symbol s, Color color, float stroke = 1.6f);

// ---- file opening
// Apps that open documents register a handler (called with an absolute path).
// open_file_hook launches nothing by itself; use app_open_file to launch+open.
#include "ui/func.h"
void register_open_handler(const char* app_id, Func<void(const char*)> fn);
void open_file_hook(const char* app_id, const char* path);
// Launch the right app for a path (by extension) and open the file in it.
void app_open_file(const char* path);
