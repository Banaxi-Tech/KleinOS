// Kernel panic: logs to serial and paints a full-screen error page directly
// into the framebuffer (no allocation, no compositor).
#include "sys/panic.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "lib/log.h"
#include "lib/string.h"
#include "drivers/framebuffer.h"
#include "gfx/font8x16.h"

static void put_text(int x, int y, const char* s, uint32_t color, int scale) {
    FramebufferInfo& fb = framebuffer();
    if (!fb.address) return;
    for (; *s; s++, x += 8 * scale) {
        const uint8_t* g = font8x16_glyph((uint8_t)*s);
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col))
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++) {
                            int px = x + col * scale + sx, py = y + row * scale + sy;
                            if (px < (int)fb.width && py < (int)fb.height)
                                *(uint32_t*)((uint8_t*)fb.address + py * fb.pitch + px * 4) = color;
                        }
    }
}

[[noreturn]] static void panic_screen(const char* msg, const char* detail1, const char* detail2) {
    cli();
    FramebufferInfo& fb = framebuffer();
    if (fb.address) {
        for (uint32_t y = 0; y < fb.height; y++) {
            uint32_t* row = (uint32_t*)((uint8_t*)fb.address + y * fb.pitch);
            uint32_t shade = 0x16 + (y * 0x10 / fb.height);
            for (uint32_t x = 0; x < fb.width; x++) row[x] = (shade << 16) | (0x10 << 8) | 0x24;
        }
        put_text(80, 120, ":(", 0xFFFFFF, 6);
        put_text(80, 260, "KleinOS ran into a problem and needs to restart.", 0xFFFFFF, 2);
        put_text(80, 320, msg, 0xFFD0D0, 2);
        if (detail1) put_text(80, 370, detail1, 0xC8C8E0, 1);
        if (detail2) put_text(80, 392, detail2, 0xC8C8E0, 1);
        put_text(80, 450, "Press Ctrl+Alt+Del or reset the machine.", 0xA0A0C0, 1);
    }
    for (;;) hlt();
}

[[noreturn]] void kpanic(const char* msg) {
    klog("KERNEL PANIC: %s", msg);
    void** rbp = (void**)__builtin_frame_address(0);
    char trace[256];
    int n = snprintf(trace, sizeof(trace), "trace:");
    for (int i = 0; i < 8 && rbp && (uintptr_t)rbp > 0x100000 && (uintptr_t)rbp < 0x100000000ull; i++) {
        n += snprintf(trace + n, sizeof(trace) - n, " %p", rbp[1]);
        rbp = (void**)rbp[0];
    }
    klog("%s", trace);
    panic_screen(msg, trace, nullptr);
}

[[noreturn]] void kpanic_frame(const char* msg, InterruptFrame* f, uint64_t cr2) {
    char d1[200], d2[200];
    snprintf(d1, sizeof(d1), "RIP=%p  ERR=%llx  CR2=%p  RSP=%p", (void*)f->rip,
             (unsigned long long)f->error, (void*)cr2, (void*)f->rsp);
    snprintf(d2, sizeof(d2), "RAX=%llx RBX=%llx RCX=%llx RDX=%llx RSI=%llx RDI=%llx",
             (unsigned long long)f->rax, (unsigned long long)f->rbx, (unsigned long long)f->rcx,
             (unsigned long long)f->rdx, (unsigned long long)f->rsi, (unsigned long long)f->rdi);
    klog("EXCEPTION: %s  %s", msg, d1);
    klog("%s", d2);
    void** rbp = (void**)f->rbp;
    for (int i = 0; i < 10 && rbp && (uintptr_t)rbp > 0x100000 && (uintptr_t)rbp < 0x100000000ull; i++) {
        klog("  frame %d: %p", i, rbp[1]);
        rbp = (void**)rbp[0];
    }
    panic_screen(msg, d1, d2);
}
