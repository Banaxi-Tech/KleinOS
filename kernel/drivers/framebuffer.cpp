#include "drivers/framebuffer.h"

static FramebufferInfo g_fb;
FramebufferInfo& framebuffer() { return g_fb; }
