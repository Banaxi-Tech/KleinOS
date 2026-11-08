// PS/2 keyboard (scancode set 1, translated by the i8042), PS/2 mouse with
// wheel, and the VMware backdoor absolute pointer (QEMU's vmmouse) so the host
// cursor moves seamlessly without grabbing.
#include "drivers/input.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "lib/log.h"

namespace {

constexpr int kQueueSize = 512;
InputEvent g_queue[kQueueSize];
volatile int g_head, g_tail;

int g_screen_w = 1280, g_screen_h = 800;
int g_mx, g_my;
uint8_t g_buttons;
uint8_t g_mods;
bool g_caps;
bool g_e0;
KeyboardLayout g_layout = KeyboardLayout::US;

bool g_vmmouse;
uint8_t g_packet[4];
int g_packet_idx;
bool g_wheel_mouse;

void push(const InputEvent& e) {
    int next = (g_head + 1) % kQueueSize;
    if (next == g_tail) return;   // drop on overflow
    g_queue[g_head] = e;
    g_head = next;
}

// ------------------------------------------------------------ keymaps (set 1)
// index = scancode, value = unshifted / shifted character
struct KeyMap { const char* normal; const char* shifted; };

//                         0123456789ABCDEF
const char kUsNormal[]  = "\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`" "\0\\" "zxcvbnm,./" "\0*\0 ";
const char kUsShifted[] = "\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~" "\0|" "ZXCVBNM<>?" "\0*\0 ";
const char kUkNormal[]  = "\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`" "\0#" "zxcvbnm,./" "\0*\0 ";
const char kUkShifted[] = "\0\0" "!\"#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:@~" "\0~" "ZXCVBNM<>?" "\0*\0 ";
// German QWERTZ (umlauts as Latin-1 code points, encoded later)
const uint32_t kDeNormal[] = {0,0,'1','2','3','4','5','6','7','8','9','0',0xDF,0xB4,0,0,'q','w','e','r','t','z','u','i','o','p',0xFC,'+',0,0,
    'a','s','d','f','g','h','j','k','l',0xF6,0xE4,'^',0,'#','y','x','c','v','b','n','m',',','.','-',0,'*',0,' '};
const uint32_t kDeShifted[] = {0,0,'!','"',0xA7,'$','%','&','/','(',')','=','?','`',0,0,'Q','W','E','R','T','Z','U','I','O','P',0xDC,'*',0,0,
    'A','S','D','F','G','H','J','K','L',0xD6,0xC4,0xB0,0,'\'','Y','X','C','V','B','N','M',';',':','_',0,'*',0,' '};
const uint32_t kFrNormal[] = {0,0,'&',0xE9,'"','\'','(','-',0xE8,'_',0xE7,0xE0,')','=',0,0,'a','z','e','r','t','y','u','i','o','p','^','$',0,0,
    'q','s','d','f','g','h','j','k','l','m',0xF9,0xB2,0,'*','w','x','c','v','b','n',',',';',':','!',0,'*',0,' '};
const uint32_t kFrShifted[] = {0,0,'1','2','3','4','5','6','7','8','9','0',0xB0,'+',0,0,'A','Z','E','R','T','Y','U','I','O','P',0xA8,0xA3,0,0,
    'Q','S','D','F','G','H','J','K','L','M','%',0,0,0xB5,'W','X','C','V','B','N','?','.','/',0xA7,0,'*',0,' '};

uint32_t translate(uint8_t sc, bool shift) {
    if (sc >= 58) return 0;
    switch (g_layout) {
    case KeyboardLayout::DE: return shift ? kDeShifted[sc] : kDeNormal[sc];
    case KeyboardLayout::FR: return shift ? kFrShifted[sc] : kFrNormal[sc];
    case KeyboardLayout::UK: return (uint8_t)(shift ? kUkShifted[sc] : kUkNormal[sc]);
    default: return (uint8_t)(shift ? kUsShifted[sc] : kUsNormal[sc]);
    }
}

void emit_key(Key k, uint32_t ch, bool pressed) {
    InputEvent e{};
    e.type = InputEvent::KEY;
    e.key.key = k;
    e.key.ch = ch;
    e.key.mods = g_mods;
    e.key.pressed = pressed;
    push(e);
}

void keyboard_irq(InterruptFrame*) {
    uint8_t sc = inb(0x60);
    if (sc == 0xE0) { g_e0 = true; return; }
    bool released = sc & 0x80;
    sc &= 0x7F;
    bool ext = g_e0;
    g_e0 = false;
    bool pressed = !released;

    auto mod = [&](uint8_t bit) { if (pressed) g_mods |= bit; else g_mods &= ~bit; };
    Key k = Key::None;
    if (ext) {
        switch (sc) {
        case 0x48: k = Key::Up; break;
        case 0x50: k = Key::Down; break;
        case 0x4B: k = Key::Left; break;
        case 0x4D: k = Key::Right; break;
        case 0x47: k = Key::Home; break;
        case 0x4F: k = Key::End; break;
        case 0x49: k = Key::PageUp; break;
        case 0x51: k = Key::PageDown; break;
        case 0x52: k = Key::Insert; break;
        case 0x53: k = Key::Delete; break;
        case 0x1C: emit_key(Key::Enter, '\n', pressed); return;
        case 0x1D: mod(MOD_CTRL); k = Key::RCtrl; break;
        case 0x38: mod(MOD_ALT); k = Key::RAlt; break;
        case 0x5B: mod(MOD_SUPER); k = Key::LSuper; break;
        case 0x5C: mod(MOD_SUPER); k = Key::RSuper; break;
        case 0x5D: k = Key::Menu; break;
        case 0x35: emit_key(Key::Char, '/', pressed); return;
        default: return;
        }
        emit_key(k, 0, pressed);
        return;
    }
    switch (sc) {
    case 0x01: k = Key::Escape; break;
    case 0x0E: k = Key::Backspace; break;
    case 0x0F: emit_key(Key::Tab, '\t', pressed); return;
    case 0x1C: emit_key(Key::Enter, '\n', pressed); return;
    case 0x39: emit_key(Key::Space, ' ', pressed); return;
    case 0x2A: mod(MOD_SHIFT); k = Key::LShift; break;
    case 0x36: mod(MOD_SHIFT); k = Key::RShift; break;
    case 0x1D: mod(MOD_CTRL); k = Key::LCtrl; break;
    case 0x38: mod(MOD_ALT); k = Key::LAlt; break;
    case 0x3A: if (pressed) g_caps = !g_caps; k = Key::CapsLock; break;
    case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44:
        k = (Key)((int)Key::F1 + (sc - 0x3B)); break;
    case 0x57: k = Key::F11; break;
    case 0x58: k = Key::F12; break;
    // keypad without numlock handling: map to navigation / digits
    case 0x47: k = Key::Home; break;
    case 0x48: k = Key::Up; break;
    case 0x49: k = Key::PageUp; break;
    case 0x4B: k = Key::Left; break;
    case 0x4D: k = Key::Right; break;
    case 0x4F: k = Key::End; break;
    case 0x50: k = Key::Down; break;
    case 0x51: k = Key::PageDown; break;
    case 0x53: k = Key::Delete; break;
    case 0x4A: emit_key(Key::Char, '-', pressed); return;
    case 0x4E: emit_key(Key::Char, '+', pressed); return;
    case 0x37: emit_key(Key::Char, '*', pressed); return;
    case 0x56: {   // ISO key between left shift and Z
        uint32_t ch = (g_layout == KeyboardLayout::DE || g_layout == KeyboardLayout::FR)
                          ? ((g_mods & MOD_SHIFT) ? '>' : '<')
                          : ((g_mods & MOD_SHIFT) ? '|' : '\\');
        emit_key(Key::Char, ch, pressed);
        return;
    }
    default: {
        bool shift = g_mods & MOD_SHIFT;
        uint32_t ch = translate(sc, shift);
        if (ch && g_caps && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')))
            ch = translate(sc, !shift);
        if (ch) emit_key(Key::Char, ch, pressed);
        return;
    }
    }
    emit_key(k, 0, pressed);
}

// ------------------------------------------------------------ mouse
void ps2_wait_write() { for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {} }
void ps2_wait_read() { for (int i = 0; i < 100000 && !(inb(0x64) & 1); i++) {} }
void mouse_write(uint8_t v) {
    ps2_wait_write(); outb(0x64, 0xD4);
    ps2_wait_write(); outb(0x60, v);
    ps2_wait_read(); inb(0x60);   // ACK
}

// VMware backdoor
constexpr uint32_t VM_MAGIC = 0x564D5868;
constexpr uint16_t VM_PORT = 0x5658;
struct VmRegs { uint32_t a, b, c, d; };
VmRegs vmcall(uint32_t cmd, uint32_t arg) {
    VmRegs r;
    asm volatile("inl %%dx, %%eax"
                 : "=a"(r.a), "=b"(r.b), "=c"(r.c), "=d"(r.d)
                 : "a"(VM_MAGIC), "b"(arg), "c"(cmd), "d"(VM_PORT)
                 : "memory");
    return r;
}
constexpr uint32_t CMD_GETVERSION = 10, CMD_ABSPOINTER_DATA = 39, CMD_ABSPOINTER_STATUS = 40,
                   CMD_ABSPOINTER_COMMAND = 41;
constexpr uint32_t ABS_READ_ID = 0x45414552, ABS_REQUEST_ABSOLUTE = 0x53424152, ABS_VERSION = 0x3442554a;

bool vmmouse_init() {
    VmRegs v = vmcall(CMD_GETVERSION, 0);
    if (v.b != VM_MAGIC) return false;
    vmcall(CMD_ABSPOINTER_COMMAND, ABS_READ_ID);
    VmRegs s = vmcall(CMD_ABSPOINTER_STATUS, 0);
    if ((s.a & 0xFFFF) < 1) return false;
    VmRegs id = vmcall(CMD_ABSPOINTER_DATA, 1);
    if (id.a != ABS_VERSION) return false;
    vmcall(CMD_ABSPOINTER_COMMAND, ABS_REQUEST_ABSOLUTE);
    return true;
}

void emit_mouse(int wheel) {
    InputEvent e{};
    e.type = InputEvent::MOUSE;
    e.x = g_mx;
    e.y = g_my;
    e.buttons = g_buttons;
    e.wheel = wheel;
    e.key.mods = g_mods;
    push(e);
}

void vmmouse_read() {
    for (int guard = 0; guard < 16; guard++) {
        VmRegs s = vmcall(CMD_ABSPOINTER_STATUS, 0);
        if (s.a == 0xFFFF0000) { vmmouse_init(); return; }
        if ((s.a & 0xFFFF) < 4) return;
        VmRegs d = vmcall(CMD_ABSPOINTER_DATA, 4);
        uint32_t flags = d.a;
        g_mx = (int)((uint64_t)d.b * g_screen_w / 65536);
        g_my = (int)((uint64_t)d.c * g_screen_h / 65536);
        uint8_t b = 0;
        if (flags & 0x20) b |= MB_LEFT;
        if (flags & 0x10) b |= MB_RIGHT;
        if (flags & 0x08) b |= MB_MIDDLE;
        g_buttons = b;
        int wheel = (int8_t)(d.d & 0xFF);
        emit_mouse(wheel);
    }
}

void mouse_irq(InterruptFrame*) {
    uint8_t st = inb(0x64);
    if (!(st & 0x20)) return;
    uint8_t data = inb(0x60);
    if (g_vmmouse) { vmmouse_read(); return; }
    if (g_packet_idx == 0 && !(data & 0x08)) return;   // resync
    g_packet[g_packet_idx++] = data;
    int len = g_wheel_mouse ? 4 : 3;
    if (g_packet_idx < len) return;
    g_packet_idx = 0;
    int dx = g_packet[1] - ((g_packet[0] & 0x10) ? 256 : 0);
    int dy = g_packet[2] - ((g_packet[0] & 0x20) ? 256 : 0);
    g_mx += dx;
    g_my -= dy;
    if (g_mx < 0) g_mx = 0;
    if (g_my < 0) g_my = 0;
    if (g_mx >= g_screen_w) g_mx = g_screen_w - 1;
    if (g_my >= g_screen_h) g_my = g_screen_h - 1;
    g_buttons = (g_packet[0] & 1 ? MB_LEFT : 0) | (g_packet[0] & 2 ? MB_RIGHT : 0) | (g_packet[0] & 4 ? MB_MIDDLE : 0);
    int wheel = g_wheel_mouse ? (int8_t)(g_packet[3] << 4) >> 4 : 0;
    emit_mouse(wheel);
}

}  // namespace

void input_init(int w, int h) {
    g_screen_w = w;
    g_screen_h = h;
    g_mx = w / 2;
    g_my = h / 2;

    // flush controller
    while (inb(0x64) & 1) inb(0x60);
    // enable aux port + IRQ12
    ps2_wait_write(); outb(0x64, 0xA8);
    ps2_wait_write(); outb(0x64, 0x20);
    ps2_wait_read();
    uint8_t cfg = inb(0x60);
    cfg |= 0x03;          // IRQ1 + IRQ12
    cfg &= ~0x20;         // enable mouse clock
    cfg |= 0x40;          // translation to set 1
    ps2_wait_write(); outb(0x64, 0x60);
    ps2_wait_write(); outb(0x60, cfg);

    mouse_write(0xF6);    // defaults
    // IntelliMouse wheel magic: sample rate 200, 100, 80 then read ID
    mouse_write(0xF3); mouse_write(200);
    mouse_write(0xF3); mouse_write(100);
    mouse_write(0xF3); mouse_write(80);
    ps2_wait_write(); outb(0x64, 0xD4);
    ps2_wait_write(); outb(0x60, 0xF2);
    ps2_wait_read(); inb(0x60);
    ps2_wait_read();
    uint8_t id = inb(0x60);
    g_wheel_mouse = id == 3 || id == 4;
    mouse_write(0xF4);    // enable streaming

    g_vmmouse = vmmouse_init();
    klog("input: ps/2 keyboard, mouse id=%d wheel=%d, vmware absolute pointer=%s", id, g_wheel_mouse,
         g_vmmouse ? "yes" : "no");

    irq_install(1, keyboard_irq);
    irq_install(12, mouse_irq);
}

bool input_poll(InputEvent* out) {
    IrqGuard g;
    if (g_tail == g_head) return false;
    *out = g_queue[g_tail];
    g_tail = (g_tail + 1) % kQueueSize;
    return true;
}

bool input_absolute_mouse() { return g_vmmouse; }
void input_set_layout(KeyboardLayout l) { g_layout = l; }
KeyboardLayout input_layout() { return g_layout; }
const char* input_layout_name(KeyboardLayout l) {
    switch (l) {
    case KeyboardLayout::UK: return "British";
    case KeyboardLayout::DE: return "German";
    case KeyboardLayout::FR: return "French";
    default: return "U.S.";
    }
}
