// Boot information shared by every boot protocol: the kernel command line and boot modules.
#include "boot/multiboot2.h"
#include "lib/string.h"

static BootInfo g_boot;
BootInfo& boot_info() { return g_boot; }

const BootModule* boot_module(const char* name) {
    for (int i = 0; i < g_boot.module_count; i++)
        if (strcmp(g_boot.modules[i].name, name) == 0) return &g_boot.modules[i];
    return nullptr;
}

bool boot_arg(const char* key, char* value, size_t size) {
    size_t klen = strlen(key);
    const char* s = g_boot.cmdline;
    while (s && *s) {
        while (*s == ' ') s++;
        if (strncmp(s, key, klen) == 0 && s[klen] == '=') {
            s += klen + 1;
            size_t n = 0;
            while (s[n] && s[n] != ' ') n++;
            if (value && size) {
                size_t c = n < size - 1 ? n : size - 1;
                memcpy(value, s, c);
                value[c] = 0;
            }
            return true;
        }
        while (*s && *s != ' ') s++;
    }
    return false;
}

// resolution=WxH on the command line (ARM boards pick the display mode themselves). Defaults to 1280x800.
void boot_resolution(uint32_t* w, uint32_t* h) {
    *w = 1280;
    *h = 800;
    char res[24];
    if (!boot_arg("resolution", res, sizeof(res))) return;
    char* x = strchr(res, 'x');
    if (!x) return;
    uint32_t rw = (uint32_t)strtoul(res, nullptr, 10), rh = (uint32_t)strtoul(x + 1, nullptr, 10);
    if (rw >= 640 && rw <= 3840 && rh >= 480 && rh <= 2160) { *w = rw; *h = rh; }
}
