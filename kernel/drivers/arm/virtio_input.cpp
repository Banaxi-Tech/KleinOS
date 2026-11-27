// virtio-input: keyboards, mice and the absolute "tablet" pointer (QEMU: -device virtio-keyboard-device
// -device virtio-tablet-device). Polled from input_poll(). Events are Linux evdev events.
#include "drivers/input.h"
#include "drivers/arm/virtio.h"
#include "lib/heap.h"
#include "lib/log.h"
#include "lib/string.h"

void virtio_input_poll();

namespace {

enum { EV_SYN = 0, EV_KEY = 1, EV_REL = 2, EV_ABS = 3 };
enum { REL_X = 0, REL_Y = 1, REL_WHEEL = 8 };
enum { ABS_X = 0, ABS_Y = 1 };
enum { BTN_LEFT = 0x110, BTN_RIGHT = 0x111, BTN_MIDDLE = 0x112, BTN_TOUCH = 0x14A };
enum { CFG_ID_NAME = 1, CFG_EV_BITS = 0x11, CFG_ABS_INFO = 0x12 };
constexpr int kBuffers = 64;

struct [[gnu::packed]] Event { uint16_t type, code; uint32_t value; };

struct InputDev {
    virtio::Device* dev;
    virtio::Queue queue;
    Event* events;
    int16_t slot[256];
    bool absolute;
    int32_t abs_max[2] = {32767, 32767};
    int32_t abs_min[2] = {0, 0};
};
InputDev g_inputs[4];
int g_count;

// pointer state shared by every device
int g_abs_raw[2];
bool g_abs_moved;
int g_rel_dx, g_rel_dy, g_wheel;
uint8_t g_buttons;
bool g_pointer_dirty;

// Linux key codes below 89 are PS/2 set 1 make codes. The rest are the 0xE0-prefixed keys.
bool to_scancode(uint16_t code, uint8_t* sc, bool* ext) {
    *ext = false;
    if (code >= 1 && code <= 88) { *sc = (uint8_t)code; return true; }
    *ext = true;
    switch (code) {
    case 96: *sc = 0x1C; return true;    // keypad enter
    case 97: *sc = 0x1D; return true;    // right ctrl
    case 98: *sc = 0x35; return true;    // keypad /
    case 100: *sc = 0x38; return true;   // right alt
    case 102: *sc = 0x47; return true;   // home
    case 103: *sc = 0x48; return true;   // up
    case 104: *sc = 0x49; return true;   // page up
    case 105: *sc = 0x4B; return true;   // left
    case 106: *sc = 0x4D; return true;   // right
    case 107: *sc = 0x4F; return true;   // end
    case 108: *sc = 0x50; return true;   // down
    case 109: *sc = 0x51; return true;   // page down
    case 110: *sc = 0x52; return true;   // insert
    case 111: *sc = 0x53; return true;   // delete
    case 125: *sc = 0x5B; return true;   // left super
    case 126: *sc = 0x5C; return true;   // right super
    case 127: *sc = 0x5D; return true;   // menu
    }
    return false;
}

void handle(InputDev& d, const Event& e) {
    switch (e.type) {
    case EV_KEY: {
        bool pressed = e.value != 0;    // 1 = press, 2 = auto-repeat
        if (e.code == BTN_LEFT || e.code == BTN_RIGHT || e.code == BTN_MIDDLE || e.code == BTN_TOUCH) {
            uint8_t bit = e.code == BTN_RIGHT ? MB_RIGHT : e.code == BTN_MIDDLE ? MB_MIDDLE : MB_LEFT;
            g_buttons = pressed ? (g_buttons | bit) : (g_buttons & ~bit);
            g_pointer_dirty = true;
            break;
        }
        uint8_t sc;
        bool ext;
        if (to_scancode(e.code, &sc, &ext)) input_feed_key(sc, ext, pressed);
        break;
    }
    case EV_ABS:
        if (e.code <= ABS_Y) { g_abs_raw[e.code] = (int)e.value; g_abs_moved = true; g_pointer_dirty = true; }
        break;
    case EV_REL:
        if (e.code == REL_X) { g_rel_dx += (int)e.value; g_pointer_dirty = true; }
        else if (e.code == REL_Y) { g_rel_dy += (int)e.value; g_pointer_dirty = true; }
        else if (e.code == REL_WHEEL) { g_wheel -= (int)e.value; g_pointer_dirty = true; }   // evdev: + = up
        break;
    case EV_SYN:
        if (!g_pointer_dirty) break;
        g_pointer_dirty = false;
        if (d.absolute && g_abs_moved) {
            int w, h;
            input_screen_size(&w, &h);
            int64_t rx = g_abs_raw[0] - d.abs_min[0], ry = g_abs_raw[1] - d.abs_min[1];
            int64_t sx = d.abs_max[0] - d.abs_min[0] + 1, sy = d.abs_max[1] - d.abs_min[1] + 1;
            input_feed_pointer((int)(rx * w / sx), (int)(ry * h / sy), g_buttons, g_wheel, true);
        } else {
            input_feed_pointer(g_rel_dx, g_rel_dy, g_buttons, g_wheel, false);
        }
        g_abs_moved = false;
        g_rel_dx = g_rel_dy = g_wheel = 0;
        break;
    }
}

void repost(InputDev& d, int slot) {
    virtio::Segment s{&d.events[slot], sizeof(Event), true};
    int head = virtio::submit(d.queue, &s, 1);
    if (head >= 0) d.slot[head] = (int16_t)slot;
}

// Size of a config-space answer for (select, subsel); the data starts at config offset 8.
uint8_t query(virtio::Device& dev, uint8_t select, uint8_t sub) {
    dev.cfg_write8(0, select);
    dev.cfg_write8(1, sub);
    asm volatile("dmb ish" ::: "memory");
    return dev.cfg8(2);
}

}  // namespace

void virtio_input_init() {
    virtio::scan();
    while (g_count < 4) {
        virtio::Device* dev = virtio::find(virtio::Input);
        if (!dev) break;
        InputDev& d = g_inputs[g_count];
        if (!virtio::begin(*dev, 0) || !virtio::queue_init(*dev, d.queue, 0, 64)) continue;
        d.dev = dev;
        d.events = (Event*)kcalloc(kBuffers, sizeof(Event));

        char name[33] = {};
        uint8_t n = query(*dev, CFG_ID_NAME, 0);
        for (int i = 0; i < n && i < 32; i++) name[i] = (char)dev->cfg8(8 + i);
        d.absolute = query(*dev, CFG_EV_BITS, EV_ABS) != 0;
        if (d.absolute) {
            for (int axis = 0; axis < 2; axis++) {
                if (query(*dev, CFG_ABS_INFO, (uint8_t)axis) >= 8) {
                    d.abs_min[axis] = (int32_t)dev->cfg32(8);
                    d.abs_max[axis] = (int32_t)dev->cfg32(12);
                }
            }
        }
        for (int i = 0; i < kBuffers; i++) repost(d, i);
        virtio::driver_ok(*dev);
        klog("virtio-input: %s%s", name, d.absolute ? " (absolute pointer)" : "");
        g_count++;
    }
    if (!g_count) klog("virtio-input: no devices (start QEMU with -device virtio-keyboard-device -device virtio-tablet-device)");
}

void virtio_input_poll() {
    for (int i = 0; i < g_count; i++) {
        InputDev& d = g_inputs[i];
        uint16_t head;
        uint32_t len;
        while (virtio::reap(d.queue, &head, &len)) {
            int slot = d.slot[head];
            Event e = d.events[slot];
            virtio::release(d.queue, head);
            repost(d, slot);
            handle(d, e);
        }
    }
}
