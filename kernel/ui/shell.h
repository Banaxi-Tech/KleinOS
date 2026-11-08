#pragma once
// Desktop shell internals used by the compositor (wm.cpp).
#include "gfx/canvas.h"
#include "drivers/input.h"
#include "lib/vector.h"

void shell_init();
void shell_tick(uint64_t now);
void shell_glass_rects(Vector<Rect>& out);          // glass surfaces + margins (for damage growth)
void shell_draw_desktop(Canvas& s, Rect clip);      // below windows
void shell_draw(Canvas& s, Rect clip);              // menubar + dock
void shell_draw_overlays(Canvas& s, Rect clip);     // menus, spotlight, launchpad, notifications
bool shell_mouse(const InputEvent& ev, uint8_t pressed, uint8_t released);
bool shell_key(const KeyEvent& e);
void shell_desktop_click(int x, int y, int button);
int shell_dock_height();
void shell_windows_changed();

// Session info shown by the shell (set by the system layer).
void shell_set_user(const char* full_name, const char* username);
const char* shell_user_name();
const char* shell_user_login();
