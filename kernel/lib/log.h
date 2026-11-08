#pragma once
// Kernel log: goes to COM1 serial and an in-memory ring buffer (Console app).
#include <stddef.h>

void klog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Copy the last up to `max` bytes of the log into buf (NUL-terminated). Returns length.
size_t klog_snapshot(char* buf, size_t max);
// Monotonic counter that changes whenever a new line is logged.
size_t klog_generation();
