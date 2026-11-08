#include "lib/log.h"
#include "lib/string.h"
#include "drivers/serial.h"
#include "arch/interrupts.h"
#include "arch/cpu.h"

static char g_ring[64 * 1024];
static size_t g_head;      // total bytes written
static size_t g_gen;

static void ring_put(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) g_ring[(g_head + i) % sizeof(g_ring)] = s[i];
    g_head += n;
}

void klog(const char* fmt, ...) {
    char line[1024];
    uint64_t t = timer_ms();
    int p = snprintf(line, sizeof(line), "[%5llu.%03llu] ", (unsigned long long)(t / 1000), (unsigned long long)(t % 1000));
    va_list ap;
    va_start(ap, fmt);
    p += vsnprintf(line + p, sizeof(line) - p, fmt, ap);
    va_end(ap);
    if (p > (int)sizeof(line) - 2) p = sizeof(line) - 2;
    line[p++] = '\n';
    line[p] = 0;
    IrqGuard g;
    serial_write(line);
    ring_put(line, p);
    g_gen++;
}

size_t klog_snapshot(char* buf, size_t max) {
    IrqGuard g;
    size_t avail = g_head < sizeof(g_ring) ? g_head : sizeof(g_ring);
    size_t n = avail < max - 1 ? avail : max - 1;
    size_t start = g_head - n;
    for (size_t i = 0; i < n; i++) buf[i] = g_ring[(start + i) % sizeof(g_ring)];
    buf[n] = 0;
    return n;
}

size_t klog_generation() { return g_gen; }
