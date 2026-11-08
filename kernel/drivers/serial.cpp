#include "drivers/serial.h"
#include "arch/cpu.h"

static constexpr uint16_t COM1 = 0x3F8;
static bool g_ok;

void serial_init() {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);   // 115200 baud
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
    g_ok = inb(COM1 + 5) != 0xFF;
}

void serial_putc(char c) {
    if (!g_ok) return;
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) {}
    outb(COM1, c);
}

void serial_write(const char* s) {
    for (; *s; s++) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s);
    }
}
