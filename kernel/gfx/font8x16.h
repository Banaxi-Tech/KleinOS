#pragma once
#include <stdint.h>
// 8x16 1-bit bitmap font (ASCII), used before the TrueType renderer is up.
const uint8_t* font8x16_glyph(uint8_t c);
