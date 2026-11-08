#pragma once
// Keyboard + mouse input (PS/2, VMware absolute pointer). Interrupt handlers
// push raw events into a queue; the window server drains it each loop.
#include <stdint.h>

enum class Key : uint16_t {
    None = 0,
    Char,           // printable; see KeyEvent::ch
    Escape, Backspace, Tab, Enter, Space,
    Up, Down, Left, Right, Home, End, PageUp, PageDown, Insert, Delete,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    LShift, RShift, LCtrl, RCtrl, LAlt, RAlt, LSuper, RSuper, CapsLock, Menu,
};

enum Mod : uint8_t {
    MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4, MOD_SUPER = 8,
};

struct KeyEvent {
    Key key;
    uint32_t ch;        // Unicode code point for Key::Char / Space / Enter / Tab (else 0)
    uint8_t mods;       // Mod bits
    bool pressed;
    bool shift() const { return mods & MOD_SHIFT; }
    bool ctrl() const { return mods & MOD_CTRL; }
    bool alt() const { return mods & MOD_ALT; }
    bool super() const { return mods & MOD_SUPER; }
    // "Command" shortcut modifier: Super (Windows key) or Ctrl.
    bool cmd() const { return mods & (MOD_SUPER | MOD_CTRL); }
};

enum MouseButton : uint8_t { MB_LEFT = 1, MB_RIGHT = 2, MB_MIDDLE = 4 };

struct InputEvent {
    enum Type : uint8_t { KEY, MOUSE } type;
    KeyEvent key;
    // mouse: absolute position (screen px), buttons bitmask, wheel delta (+ = down)
    int x, y;
    uint8_t buttons;
    int wheel;
};

void input_init(int screen_w, int screen_h);
bool input_poll(InputEvent* out);
bool input_absolute_mouse();            // true if VMware absolute pointer active

enum class KeyboardLayout : uint8_t { US = 0, UK, DE, FR };
void input_set_layout(KeyboardLayout l);
KeyboardLayout input_layout();
const char* input_layout_name(KeyboardLayout l);
