// KleinOS product key generator (host tool, not part of the kernel).
//
// Build and run (from the repo root):
//   g++ -std=c++17 -O2 -Wall -o /tmp/claude-1000/keygen tools/keygen.cpp && g++ -std=c++17 -O2 -Wall -o /tmp/claude-1000/keygen_test tools/keygen_test.cpp && /tmp/claude-1000/keygen_test && /tmp/claude-1000/keygen
//
// Usage: keygen [home|pro|enterprise|education] [count] [--channel retail|oem|volume] [--serial N]
//   No arguments: one key per edition. Without --serial, serials come from std::random_device.
//   With --serial N, key i gets serial N+i.

#include "../kernel/sys/productkey.cpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace {

void usage() {
    std::fprintf(stderr,
        "usage: keygen [home|pro|enterprise|education] [count] "
        "[--channel retail|oem|volume] [--serial N]\n");
}

bool parse_edition(const char* s, productkey::Edition* out) {
    struct Name { const char* name; productkey::Edition ed; };
    static const Name names[] = {
        {"home", productkey::Edition::Home},
        {"pro", productkey::Edition::Pro},
        {"enterprise", productkey::Edition::Enterprise},
        {"education", productkey::Edition::Education},
    };
    for (const Name& n : names) {
        if (std::strcmp(s, n.name) == 0) { *out = n.ed; return true; }
    }
    return false;
}

bool parse_channel(const char* s, productkey::Channel* out) {
    if (std::strcmp(s, "retail") == 0) { *out = productkey::Channel::Retail; return true; }
    if (std::strcmp(s, "oem") == 0)    { *out = productkey::Channel::OEM; return true; }
    if (std::strcmp(s, "volume") == 0) { *out = productkey::Channel::Volume; return true; }
    return false;
}

bool parse_u32(const char* s, uint32_t* out) {
    if (!s || !*s) return false;
    errno = 0;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 0);
    if (errno != 0 || *end != '\0' || v > 0xFFFFFFFFULL) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    using productkey::Channel;
    using productkey::Edition;

    bool have_edition = false;
    Edition edition = Edition::Invalid;
    unsigned long count = 1;
    Channel channel = Channel::Retail;
    bool have_serial = false;
    uint32_t serial_base = 0;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--channel") == 0) {
            if (i + 1 >= argc || !parse_channel(argv[++i], &channel)) { usage(); return 2; }
        } else if (std::strcmp(a, "--serial") == 0) {
            if (i + 1 >= argc || !parse_u32(argv[++i], &serial_base)) { usage(); return 2; }
            have_serial = true;
        } else if (parse_edition(a, &edition)) {
            have_edition = true;
        } else if (a[0] >= '0' && a[0] <= '9') {
            char* end = nullptr;
            count = std::strtoul(a, &end, 10);
            if (*end != '\0' || count < 1 || count > 10000) { usage(); return 2; }
        } else {
            usage();
            return 2;
        }
    }

    Edition editions_all[] = {Edition::Home, Edition::Pro, Edition::Enterprise, Edition::Education};
    const Edition* list = editions_all;
    unsigned long list_len = 4;
    if (have_edition) { list = &edition; list_len = 1; }

    if (have_serial && serial_base + (count - 1) > 0xFFFFFFFFULL) {
        std::fprintf(stderr, "keygen: serial range overflows 32 bits\n");
        return 2;
    }

    std::random_device rd;
    char key[30];
    for (unsigned long li = 0; li < list_len; ++li) {
        for (unsigned long k = 0; k < count; ++k) {
            uint32_t serial = have_serial
                ? static_cast<uint32_t>(serial_base + k)
                : static_cast<uint32_t>(rd());
            if (!productkey::generate(list[li], channel, serial, key)) {
                std::fprintf(stderr, "keygen: generate failed\n");
                return 1;
            }
            std::printf("%s  %s, %s, serial %u\n", key,
                        productkey::edition_name(list[li]),
                        productkey::channel_name(channel),
                        static_cast<unsigned>(serial));
        }
    }
    return 0;
}
