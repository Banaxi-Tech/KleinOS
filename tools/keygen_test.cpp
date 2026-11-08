// Host tests for the KleinOS product key library. Returns non-zero on any failure.
//
// Build and run (from the repo root):
//   g++ -std=c++17 -O2 -Wall -o /tmp/claude-1000/keygen_test tools/keygen_test.cpp && /tmp/claude-1000/keygen_test

#include "../kernel/sys/productkey.cpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using productkey::Channel;
using productkey::Edition;
using productkey::KeyInfo;

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

const Edition kEditions[] = {Edition::Home, Edition::Pro, Edition::Enterprise, Edition::Education};
const Channel kChannels[] = {Channel::Retail, Channel::OEM, Channel::Volume};
const char kAlpha[] = "BCDFGHJKMPQRTVWXY2346789";

// Canonical shape: XXXXX-XXXXX-XXXXX-XXXXX-XXXXX, every X in the alphabet.
bool has_canonical_shape(const char* s) {
    if (std::strlen(s) != 29) return false;
    for (int i = 0; i < 29; ++i) {
        if (i % 6 == 5) {
            if (s[i] != '-') return false;
        } else if (!std::strchr(kAlpha, s[i])) {
            return false;
        }
    }
    return true;
}

std::string make_key(Edition e, Channel c, uint32_t serial) {
    char buf[30];
    check(productkey::generate(e, c, serial, buf), "generate succeeds for valid input");
    return std::string(buf);
}

// Indices of the 25 symbol characters in a dashed key.
std::vector<size_t> symbol_positions(const std::string& key) {
    std::vector<size_t> pos;
    for (size_t i = 0; i < key.size(); ++i) {
        if (key[i] != '-') pos.push_back(i);
    }
    return pos;
}

bool same_info(const KeyInfo& a, Edition e, Channel c, uint32_t serial) {
    return a.edition == e && a.channel == c && a.serial == serial;
}

void test_round_trip(std::mt19937_64& rng) {
    const uint32_t edge[] = {0u, 1u, 2u, 0xFFFFu, 0x10000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu};
    size_t tested = 0;
    for (Edition e : kEditions) {
        for (Channel c : kChannels) {
            std::vector<uint32_t> serials(edge, edge + sizeof edge / sizeof edge[0]);
            for (int i = 0; i < 2000; ++i) serials.push_back(static_cast<uint32_t>(rng()));
            for (uint32_t s : serials) {
                char buf[30];
                bool ok = productkey::generate(e, c, s, buf);
                check(ok, "round trip: generate");
                if (!ok) continue;
                check(has_canonical_shape(buf), "round trip: canonical shape");
                KeyInfo info{};
                check(productkey::validate(buf, &info), "round trip: validate");
                check(same_info(info, e, c, s), "round trip: fields match");
                ++tested;
            }
        }
    }
    std::printf("round trip: %zu keys\n", tested);
}

void test_typos(std::mt19937_64& rng) {
    const int n = 10000;
    int rejected = 0;
    for (int t = 0; t < n; ++t) {
        Edition e = kEditions[rng() % 4];
        Channel c = kChannels[rng() % 3];
        std::string key = make_key(e, c, static_cast<uint32_t>(rng()));
        std::vector<size_t> pos = symbol_positions(key);
        size_t at = pos[rng() % pos.size()];
        char orig = key[at];
        char repl;
        do {
            repl = kAlpha[rng() % 24];
        } while (repl == orig);
        key[at] = repl;
        if (!productkey::validate(key.c_str(), nullptr)) ++rejected;
    }
    std::printf("single-character typos rejected: %d/%d\n", rejected, n);
    check(rejected * 1000 >= n * 999, "typos rejected at least 99.9% of the time");
}

void test_forgiving_parse() {
    std::string key = make_key(Edition::Pro, Channel::OEM, 123456u);
    KeyInfo info{};

    std::string lower = key;
    for (char& ch : lower) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    check(productkey::validate(lower.c_str(), &info), "lowercase accepted");
    check(same_info(info, Edition::Pro, Channel::OEM, 123456u), "lowercase fields match");

    std::string nodash;
    for (char ch : key) if (ch != '-') nodash += ch;
    check(productkey::validate(nodash.c_str(), &info), "missing dashes accepted");
    check(same_info(info, Edition::Pro, Channel::OEM, 123456u), "no-dash fields match");

    std::string spaces = key;
    for (char& ch : spaces) if (ch == '-') ch = ' ';
    check(productkey::validate(spaces.c_str(), nullptr), "spaces instead of dashes accepted");

    std::string odd = "  " + key + "  ";
    check(productkey::validate(odd.c_str(), nullptr), "leading/trailing spaces accepted");
}

void test_rejects_bad_input() {
    std::string key = make_key(Edition::Home, Channel::Retail, 42u);
    std::vector<size_t> pos = symbol_positions(key);

    // Wrong length.
    std::string shorter = key;
    shorter.erase(pos[7], 1);
    check(!productkey::validate(shorter.c_str(), nullptr), "24 symbols rejected");
    std::string longer = key;
    longer.insert(pos[7], 1, 'B');
    check(!productkey::validate(longer.c_str(), nullptr), "26 symbols rejected");
    check(!productkey::validate("", nullptr), "empty string rejected");
    check(!productkey::validate("-----", nullptr), "dashes only rejected");
    check(!productkey::validate(nullptr, nullptr), "null rejected");

    // Invalid characters, in place of one symbol.
    const char bad[] = {'O', '0', '1', 'I', 'L', '5', 'U', 'A', 'Z', 'o', 'i', '@', '.', '\t', '\n', 'a'};
    for (char b : bad) {
        std::string k = key;
        k[pos[11]] = b;
        check(!productkey::validate(k.c_str(), nullptr), "invalid character rejected");
    }

    // Raw values that do not fit the 84-bit payload, pushed through the same encoding
    // pipeline (base-24 digits, scramble, text). Must be rejected.
    const uint32_t raw_values[][4] = {
        {0u, 0u, 1u << 20, 0u},  // exactly 2^84
        {0u, 0u, 0xFFFFFFFFu, 0u},
        {0u, 0u, 0u, 1u},        // 2^96
        {0u, 0u, 0xFFu, 0u},     // 2^104
    };
    for (const auto& r : raw_values) {
        productkey::Big n;
        for (int i = 0; i < 4; ++i) n.w[i] = r[i];
        uint8_t d[25];
        productkey::big_to_digits(n, d);
        productkey::scramble(d);
        char text[30];
        productkey::digits_to_text(d, text);
        check(!productkey::validate(text, nullptr), "value above 84 bits rejected");
    }

    // All-B key: after unscrambling it is some fixed value that has no valid checksum.
    check(!productkey::validate("BBBBB-BBBBB-BBBBB-BBBBB-BBBBB", nullptr), "all-B key rejected");
}

void test_scramble_inverse() {
    // kPerm must be a permutation of 0..24.
    bool seen[25] = {};
    for (int j = 0; j < 25; ++j) {
        check(productkey::kPerm[j] >= 0 && productkey::kPerm[j] < 25, "kPerm in range");
        if (productkey::kPerm[j] >= 0 && productkey::kPerm[j] < 25) seen[productkey::kPerm[j]] = true;
    }
    for (bool s : seen) check(s, "kPerm is a permutation");

    // scramble / unscramble are inverses on arbitrary digit strings.
    std::mt19937 rng(7);
    for (int t = 0; t < 1000; ++t) {
        uint8_t d[25], orig[25];
        for (int j = 0; j < 25; ++j) orig[j] = d[j] = static_cast<uint8_t>(rng() % 24);
        productkey::scramble(d);
        productkey::unscramble(d);
        bool same = true;
        for (int j = 0; j < 25; ++j) same = same && (d[j] == orig[j]);
        check(same, "unscramble(scramble(d)) == d");
    }
}

void test_no_zero_prefix(std::mt19937_64& rng) {
    // Keys must not start with a run of zero symbols ('B'). Chance of 5 leading B's
    // for a random key is 24^-5, so 2000 keys should show none.
    int with_prefix = 0;
    for (int t = 0; t < 2000; ++t) {
        std::string key = make_key(kEditions[rng() % 4], kChannels[rng() % 3], static_cast<uint32_t>(rng()));
        if (key.compare(0, 5, "BBBBB") == 0) ++with_prefix;
    }
    check(with_prefix == 0, "keys do not start with BBBBB");
}

void test_random_strings_rejected(std::mt19937_64& rng) {
    int accepted = 0;
    for (int t = 0; t < 10000; ++t) {
        char buf[30];
        for (int i = 0; i < 29; ++i) {
            buf[i] = (i % 6 == 5) ? '-' : kAlpha[rng() % 24];
        }
        buf[29] = '\0';
        if (productkey::validate(buf, nullptr)) ++accepted;
    }
    check(accepted == 0, "random 25-symbol strings are rejected");
}

void test_generate_rejects() {
    char buf[30];
    check(!productkey::generate(Edition::Invalid, Channel::Retail, 0, buf), "Invalid edition not generated");
    check(!productkey::generate(Edition::Home, static_cast<Channel>(3), 0, buf), "bad channel not generated");
    check(!productkey::generate(Edition::Home, Channel::Retail, 0, nullptr), "null out not generated");
}

void test_generic_keys() {
    for (Edition e : kEditions) {
        const char* g = productkey::generic_key(e);
        check(g != nullptr, "generic key exists");
        if (!g) continue;
        KeyInfo info{};
        check(productkey::validate(g, &info), "generic key validates");
        check(same_info(info, e, Channel::Retail, 0u), "generic key fields");
        check(has_canonical_shape(g), "generic key shape");
        char buf[30];
        check(productkey::generate(e, Channel::Retail, 0u, buf) && std::strcmp(buf, g) == 0,
              "generic key matches generate(serial 0, Retail)");
    }
    check(productkey::generic_key(Edition::Invalid) == nullptr, "generic_key(Invalid) is nullptr");
}

void test_format_partial() {
    char out[30];
    auto fmt = [&](const char* in) {
        productkey::format_partial(in, out);
        return std::string(out);
    };
    check(fmt("bcdfg") == "BCDFG", "partial: 5 chars, no dash");
    check(fmt("bcdfg-hjkm") == "BCDFG-HJKM", "partial: dash in input dropped and re-added");
    check(fmt("bcdfgh") == "BCDFG-H", "partial: dash after 5th symbol");
    check(fmt("O0I1L5U") == "", "partial: invalid chars dropped");
    check(fmt("  bc df ") == "BCDF", "partial: spaces dropped");
    check(fmt("") == "", "partial: empty");
    productkey::format_partial(nullptr, out);
    check(out[0] == '\0', "partial: null input gives empty output");

    std::string long_in;
    for (int i = 0; i < 40; ++i) long_in += kAlpha[i % 24];
    std::string got = fmt(long_in.c_str());
    check(got.size() == 29, "partial: capped at 29 output chars");
    check(got == "BCDFG-HJKMP-QRTVW-XY234-6789B", "partial: 25-symbol cap");

    // Round trip: typing a full key (no dashes, lowercase) gives the canonical key.
    std::string key = make_key(Edition::Education, Channel::Volume, 999999u);
    std::string typed;
    for (char ch : key) if (ch != '-') typed += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    check(fmt(typed.c_str()) == key, "partial: full key round trip");
}

void test_names() {
    check(std::strcmp(productkey::edition_name(Edition::Home), "KleinOS Home") == 0, "name: Home");
    check(std::strcmp(productkey::edition_name(Edition::Pro), "KleinOS Pro") == 0, "name: Pro");
    check(std::strcmp(productkey::edition_name(Edition::Enterprise), "KleinOS Enterprise") == 0, "name: Enterprise");
    check(std::strcmp(productkey::edition_name(Edition::Education), "KleinOS Education") == 0, "name: Education");
    check(std::strcmp(productkey::channel_name(Channel::Retail), "Retail") == 0, "name: Retail");
    check(std::strcmp(productkey::channel_name(Channel::OEM), "OEM") == 0, "name: OEM");
    check(std::strcmp(productkey::channel_name(Channel::Volume), "Volume") == 0, "name: Volume");
}

}  // namespace

int main() {
    std::random_device rd;
    std::mt19937_64 rng((static_cast<uint64_t>(rd()) << 32) ^ rd());

    test_round_trip(rng);
    test_typos(rng);
    test_forgiving_parse();
    test_rejects_bad_input();
    test_random_strings_rejected(rng);
    test_scramble_inverse();
    test_no_zero_prefix(rng);
    test_generate_rejects();
    test_generic_keys();
    test_format_partial();
    test_names();

    if (g_failures) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
