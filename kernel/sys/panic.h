#pragma once
#include <stdint.h>

struct InterruptFrame;

[[noreturn]] void kpanic(const char* msg);
[[noreturn]] void kpanic_frame(const char* msg, InterruptFrame* f, uint64_t cr2);
