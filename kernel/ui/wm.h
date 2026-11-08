#pragma once
// Window manager / compositor / desktop shell services.
#include "gfx/canvas.h"
#include "ui/window.h"
#include "ui/func.h"

namespace wm {

void init();
// One iteration: input, ticks, painting, composition. Called by the main loop.
void frame();

int screen_width();
int screen_height();
Rect desktop_area();                 // screen minus menubar and dock

void add_window(Window* w);          // shows it (called by Window ctor)
void remove_window(Window* w);       // called by Window dtor
Window* active_window();
void focus(Window* w);
const Vector<Window*>& windows();    // bottom -> top
void minimize(Window* w);
void restore(Window* w);
void toggle_zoom(Window* w);

// Request recomposition of a screen region.
void damage(Rect screen_rect);
void damage_all();

// Shell
void set_shell_visible(bool visible);   // menubar + dock (off for installer/login)
bool shell_visible();
void set_wallpaper(int index);
int wallpaper();
void notify(const char* title, const char* body, const char* app_id = nullptr);
void open_spotlight();
void open_launchpad();
// Popup context menu at screen position. on_pick(index) runs when an item is chosen.
void popup_menu(int x, int y, const Vector<MenuItem>& items);
void set_glass_intensity(float k);      // 0..1, Settings slider
float glass_intensity();
// Settings hooks
void set_dock_magnification(bool on);
bool dock_magnification();
void set_reduce_transparency(bool on);
bool reduce_transparency();

// Clipboard (UTF-8 text)
void clipboard_set(const char* text);
const char* clipboard_get();

// Current mouse state
Point mouse_pos();
uint64_t frame_count();
float fps();

// Run fn on the next frame (from the main loop, safe place to delete things).
void defer(Func<void()> fn);

// Session (login/logout) callbacks for the system layer
void set_logout_handler(Func<void()> fn);
void request_logout();

}  // namespace wm
