// Product key library. Freestanding: only <stdint.h> and <stddef.h>.
#include "productkey.h"
#include <stddef.h>

namespace productkey {
namespace {

// 24 symbols (digits 0..23 of the base-24 number). No 0/1/5/I/L/O/U/A/E/N/S/Z.
const char kAlphabet[] = "BCDFGHJKMPQRTVWXY2346789";
constexpr int kBase = 24;
constexpr int kSymbols = 25;  // symbols in a key; 24^25 > 2^84 so the top bits are slack
constexpr uint64_t kMask40 = (1ULL << 40) - 1;

// Fixed 128-bit SipHash key for the checksum. Two 64-bit halves, not secret per se;
// they just have to stay fixed.
constexpr uint64_t kKey0 = 0x5A17C0DE2F4B9D31ULL;
constexpr uint64_t kKey1 = 0xC3B1F00D8E6A4725ULL;

// ---- SipHash-2-4 -----------------------------------------------------------

inline uint64_t rotl64(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

inline void sip_round(uint64_t& v0, uint64_t& v1, uint64_t& v2, uint64_t& v3) {
    v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0; v0 = rotl64(v0, 32);
    v2 += v3; v3 = rotl64(v3, 16); v3 ^= v2;
    v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0;
    v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2; v2 = rotl64(v2, 32);
}

// SipHash-2-4 for messages shorter than 8 bytes (one final block only).
uint64_t siphash24_short(const uint8_t* m, size_t len, uint64_t k0, uint64_t k1) {
    uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
    uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
    uint64_t v3 = k1 ^ 0x7465646279746573ULL;

    uint64_t b = static_cast<uint64_t>(len) << 56;
    for (size_t i = 0; i < len; ++i) b |= static_cast<uint64_t>(m[i]) << (8 * i);

    v3 ^= b;
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    v0 ^= b;

    v2 ^= 0xff;
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

// 40-bit keyed checksum over (edition, channel, serial).
uint64_t checksum40(uint8_t edition, uint8_t channel, uint32_t serial) {
    uint8_t m[6] = {
        edition, channel,
        static_cast<uint8_t>(serial),
        static_cast<uint8_t>(serial >> 8),
        static_cast<uint8_t>(serial >> 16),
        static_cast<uint8_t>(serial >> 24),
    };
    return siphash24_short(m, sizeof m, kKey0, kKey1) & kMask40;
}

// ---- 128-bit unsigned helpers as four little-endian uint32 limbs -----------

struct Big { uint32_t w[4]; };

// n = n * mul + add. Caller guarantees the result fits in 128 bits.
void mul_add(Big& n, uint32_t mul, uint32_t add) {
    uint64_t carry = add;
    for (int i = 0; i < 4; ++i) {
        uint64_t t = static_cast<uint64_t>(n.w[i]) * mul + carry;
        n.w[i] = static_cast<uint32_t>(t);
        carry = t >> 32;
    }
}

// n = n / d, returns n % d. Requires d < 2^32.
uint32_t div_small(Big& n, uint32_t d) {
    uint64_t rem = 0;
    for (int i = 3; i >= 0; --i) {
        uint64_t cur = (rem << 32) | n.w[i];
        n.w[i] = static_cast<uint32_t>(cur / d);
        rem = cur % d;
    }
    return static_cast<uint32_t>(rem);
}

// Symbol value 0..23 for a character (case-insensitive), or -1.
int symbol_value(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    for (int i = 0; i < kBase; ++i) {
        if (kAlphabet[i] == ch) return i;
    }
    return -1;
}

// Digit scrambling, applied after encoding and undone before decoding, so the
// leading symbols are not a run of zeros. d[0] is the most significant digit.
//   1. for i in 0..11: d[i] = (d[i] + 7*d[24-i] + 5*i) mod 24. Partners 13..24 are
//      never modified in this step, so each step inverts on its own.
//   2. fixed position permutation: out[j] = d[kPerm[j]].
constexpr int kPerm[kSymbols] = {12, 3, 20, 7, 0, 18, 9, 23, 14, 1, 21, 5, 16,
                                 10, 24, 2, 19, 8, 13, 4, 22, 11, 6, 17, 15};

void scramble(uint8_t d[kSymbols]) {
    uint8_t m[kSymbols];
    for (int i = 0; i < kSymbols; ++i) m[i] = d[i];
    for (int i = 0; i < 12; ++i) {
        m[i] = static_cast<uint8_t>((d[i] + 7 * d[24 - i] + 5 * i) % kBase);
    }
    for (int j = 0; j < kSymbols; ++j) d[j] = m[kPerm[j]];
}

void unscramble(uint8_t d[kSymbols]) {
    uint8_t t[kSymbols];
    for (int j = 0; j < kSymbols; ++j) t[kPerm[j]] = d[j];  // inverse permutation
    for (int j = 0; j < kSymbols; ++j) d[j] = t[j];
    for (int i = 0; i < 12; ++i) {
        int v = static_cast<int>(t[i]) - 7 * static_cast<int>(t[24 - i]) - 5 * i;
        v %= kBase;
        if (v < 0) v += kBase;
        d[i] = static_cast<uint8_t>(v);
    }
}

// Base-24 digits of a 128-bit value, most significant first. Value must fit in 25 digits.
void big_to_digits(Big n, uint8_t d[kSymbols]) {
    for (int i = kSymbols - 1; i >= 0; --i) d[i] = static_cast<uint8_t>(div_small(n, kBase));
}

// Symbols (no dashes) into digit values 0..23. Dashes and spaces are ignored.
bool parse_digits(const char* key, uint8_t d[kSymbols]) {
    int count = 0;
    for (const char* p = key; *p; ++p) {
        if (*p == '-' || *p == ' ') continue;
        int v = symbol_value(*p);
        if (v < 0) return false;
        if (count >= kSymbols) return false;
        d[count++] = static_cast<uint8_t>(v);
    }
    return count == kSymbols;
}

// Key text: XXXXX-XXXXX-XXXXX-XXXXX-XXXXX, out must hold 30 bytes.
void digits_to_text(const uint8_t d[kSymbols], char* out) {
    int pos = 0;
    for (int j = 0; j < kSymbols; ++j) {
        if (j > 0 && j % 5 == 0) out[pos++] = '-';
        out[pos++] = kAlphabet[d[j]];
    }
    out[pos] = '\0';
}

// Digits (most significant first) back into a 128-bit value.
void digits_to_big(const uint8_t d[kSymbols], Big& n) {
    n.w[0] = n.w[1] = n.w[2] = n.w[3] = 0;
    for (int j = 0; j < kSymbols; ++j) mul_add(n, kBase, d[j]);
}

bool valid_edition(uint8_t e) { return e >= 1 && e <= 4; }
bool valid_channel(uint8_t c) { return c <= 2; }

}  // namespace

bool validate(const char* key, KeyInfo* out) {
    if (!key) return false;

    uint8_t d[kSymbols];
    if (!parse_digits(key, d)) return false;
    unscramble(d);
    Big n;
    digits_to_big(d, n);

    // Payload is 84 bits: low 64 in w[0..1], the next 20 in w[2]. Everything above must be zero.
    if (n.w[3] != 0 || (n.w[2] >> 20) != 0) return false;

    uint64_t low = static_cast<uint64_t>(n.w[0]) | (static_cast<uint64_t>(n.w[1]) << 32);
    uint32_t high = n.w[2];

    uint64_t cs = low & kMask40;
    uint8_t channel = static_cast<uint8_t>((low >> 40) & 0xFF);
    uint32_t serial = static_cast<uint32_t>((low >> 48) | (static_cast<uint64_t>(high & 0xFFFF) << 16));
    uint8_t edition = static_cast<uint8_t>((high >> 16) & 0xF);

    if (!valid_edition(edition) || !valid_channel(channel)) return false;
    if (checksum40(edition, channel, serial) != cs) return false;

    if (out) {
        out->edition = static_cast<Edition>(edition);
        out->channel = static_cast<Channel>(channel);
        out->serial = serial;
    }
    return true;
}

bool generate(Edition edition, Channel channel, uint32_t serial, char* out) {
    if (!out) return false;
    uint8_t e = static_cast<uint8_t>(edition);
    uint8_t c = static_cast<uint8_t>(channel);
    if (!valid_edition(e) || !valid_channel(c)) return false;

    uint64_t low = checksum40(e, c, serial)
                 | (static_cast<uint64_t>(c) << 40)
                 | (static_cast<uint64_t>(serial & 0xFFFF) << 48);
    uint32_t high = (serial >> 16) | (static_cast<uint32_t>(e) << 16);

    Big n;
    n.w[0] = static_cast<uint32_t>(low);
    n.w[1] = static_cast<uint32_t>(low >> 32);
    n.w[2] = high;
    n.w[3] = 0;

    uint8_t d[kSymbols];
    big_to_digits(n, d);
    scramble(d);
    digits_to_text(d, out);
    return true;
}

void format_partial(const char* in, char* out) {
    if (!out) return;
    int pos = 0;
    int count = 0;
    if (in) {
        for (const char* p = in; *p && count < kSymbols; ++p) {
            int v = symbol_value(*p);
            if (v < 0) continue;  // drops dashes, spaces and invalid characters
            if (count > 0 && count % 5 == 0) out[pos++] = '-';
            out[pos++] = kAlphabet[v];
            ++count;
        }
    }
    out[pos] = '\0';
}

const char* edition_name(Edition e) {
    switch (e) {
        case Edition::Home:       return "KleinOS Home";
        case Edition::Pro:        return "KleinOS Pro";
        case Edition::Enterprise: return "KleinOS Enterprise";
        case Edition::Education:  return "KleinOS Education";
        default:                  return "Invalid edition";
    }
}

const char* channel_name(Channel c) {
    switch (c) {
        case Channel::Retail: return "Retail";
        case Channel::OEM:    return "OEM";
        case Channel::Volume: return "Volume";
        default:              return "Unknown channel";
    }
}

// Generated by tools/keygen with serial 0, channel Retail. keygen_test checks these validate.
const char* generic_key(Edition e) {
    switch (e) {
        case Edition::Home:       return "PDJJF-234C3-HPR97-TMB2W-WPHP7";
        case Edition::Pro:        return "439J2-XY8FX-74RF9-DJGQV-YRXH3";
        case Edition::Enterprise: return "H6JP7-CVQHF-R43KF-KJPFW-FWV8X";
        case Edition::Education:  return "W3KGR-VRCKT-748RH-XJ267-RYCKT";
        default:                  return nullptr;
    }
}

}  // namespace productkey
