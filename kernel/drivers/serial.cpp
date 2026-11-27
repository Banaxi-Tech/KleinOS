#include "drivers/serial.h"
#include "arch/cpu.h"

static bool g_ok;

#if defined(__aarch64__)
// ARM PrimeCell PL011: the QEMU "virt" board's UART, also common on real boards.
static volatile uint32_t* const kUart = (volatile uint32_t*)0x09000000;
enum { UART_DR = 0x00 / 4, UART_FR = 0x18 / 4, UART_CR = 0x30 / 4 };

void serial_init() {
    kUart[UART_CR] = (1 << 0) | (1 << 8) | (1 << 9);   // UARTEN | TXE | RXE (divisors stay as the firmware left them)
    g_ok = true;
}

void serial_putc(char c) {
    if (!g_ok) return;
    for (int i = 0; i < 100000 && (kUart[UART_FR] & (1 << 5)); i++) {}   // TXFF
    kUart[UART_DR] = (uint8_t)c;
}
#else
static constexpr uint16_t COM1 = 0x3F8;

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
#endif

void serial_write(const char* s) {
    for (; *s; s++) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s);
    }
}
