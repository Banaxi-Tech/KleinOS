#pragma once
#include <stdint.h>

// Product keys for KleinOS. Freestanding: no libc, no heap, no floats.
//
// Format: XXXXX-XXXXX-XXXXX-XXXXX-XXXXX, 25 symbols from a 24-symbol base-24
// alphabet. The 84-bit payload is edition (4) | serial (32) | channel (8) |
// checksum (40), where the checksum is a keyed SipHash-2-4 of the other fields.

namespace productkey {

enum class Edition : uint8_t { Invalid = 0, Home = 1, Pro = 2, Enterprise = 3, Education = 4 };
enum class Channel : uint8_t { Retail = 0, OEM = 1, Volume = 2 };
struct KeyInfo { Edition edition; Channel channel; uint32_t serial; };

// Validate a key typed by the user (dashes and spaces ignored, case-insensitive).
// Returns true and fills *out (if non-null) when valid.
bool validate(const char* key, KeyInfo* out);

// Generate a key into out (must hold at least 30 bytes, gets "XXXXX-XXXXX-XXXXX-XXXXX-XXXXX\0").
bool generate(Edition edition, Channel channel, uint32_t serial, char* out);

// Normalize partially-typed input for a key entry box: uppercases, drops invalid chars,
// inserts dashes every 5 chars, max 29 output chars. out must hold 30 bytes.
void format_partial(const char* in, char* out);

const char* edition_name(Edition e);   // "KleinOS Home", "KleinOS Pro", ...
const char* channel_name(Channel c);   // "Retail", "OEM", "Volume"

// Generic install keys (like Windows' generic keys): one fixed valid key per edition,
// generated with serial 0, channel Retail. Returns nullptr for Invalid.
const char* generic_key(Edition e);

}  // namespace productkey
