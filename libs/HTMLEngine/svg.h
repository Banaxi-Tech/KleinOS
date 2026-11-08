#pragma once
// SVG rasterizer for the HTML engine. Parses an SVG document (streaming, no
// DOM) and paints it into a premultiplied Canvas with anti-aliased fills and
// strokes. Supports the common static subset: shapes, paths (all commands),
// nested groups with transforms, presentation attributes and style="",
// viewBox + preserveAspectRatio. Gradients render as their first stop color.
#include <stddef.h>
#include "gfx/canvas.h"

namespace svg {

// Intrinsic size from width/height attributes or viewBox (defaults 24x24). Returns false if not SVG.
bool intrinsic_size(const char* src, size_t len, int* w, int* h);

// Render the SVG document scaled to fit w x h (preserveAspectRatio xMidYMid meet). Returns a new Canvas (caller deletes) or nullptr.
// current_color is used for fill="currentColor" / stroke="currentColor".
Canvas* render(const char* src, size_t len, int w, int h, Color current_color = 0xFF000000);

}  // namespace svg
