// C++ runtime support for a freestanding kernel.
#include <stdint.h>
#include "lib/log.h"
#include "sys/panic.h"

extern "C" {
void* __dso_handle = nullptr;
int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
void __cxa_pure_virtual() { kpanic("pure virtual call"); }
void __stack_chk_fail() { kpanic("stack smashing detected"); }
uintptr_t __stack_chk_guard = 0x595e9fbd94fda766;
int* __errno_location() { static int e; return &e; }
void abort() { kpanic("abort()"); for (;;) {} }
void __assert_fail(const char* expr, const char* file, unsigned line, const char*) {
    klog("assertion failed: %s at %s:%u", expr, file, line);
    kpanic("assertion failed");
}
}

typedef void (*CtorFn)();
extern "C" CtorFn __init_array_start[];
extern "C" CtorFn __init_array_end[];

void run_global_constructors() {
    for (CtorFn* f = __init_array_start; f < __init_array_end; f++) (*f)();
}
