#pragma once
#include "gfx/canvas.h"

int wallpaper_count();
const char* wallpaper_name(int index);
// Render wallpaper `index` into c (full size of c).
void wallpaper_render(Canvas& c, int index);
