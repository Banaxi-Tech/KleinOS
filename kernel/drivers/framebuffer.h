#pragma once
#include <stdint.h>

struct FramebufferInfo {
    void* address = nullptr;
    uint32_t width = 0, height = 0, pitch = 0, bpp = 0;
};

FramebufferInfo& framebuffer();
