#pragma once
// Desktop icons: the files in ~/Desktop, drawn on the wallpaper below all windows.
#include "gfx/canvas.h"
#include "drivers/input.h"

void desktop_tick();                                                      // rescan when files change
void desktop_draw(Canvas& s, Rect clip);
bool desktop_dragging();
// Mouse on the bare desktop (nothing else took it). True if an icon used it.
bool desktop_mouse(const InputEvent& ev, uint8_t pressed, uint8_t released);
// Keys while no window is focused (Enter opens, Delete trashes, arrows move the selection).
bool desktop_key(const KeyEvent& e);
