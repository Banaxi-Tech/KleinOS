#pragma once
// Low-level CPU helpers: port I/O, interrupts flag, MSRs, TSC.
#include <stdint.h>
#include <stddef.h>

static inline void outb(uint16_t port, uint8_t v) { asm volatile("outb %0, %1" ::"a"(v), "Nd"(port)); }
static inline void outw(uint16_t port, uint16_t v) { asm volatile("outw %0, %1" ::"a"(v), "Nd"(port)); }
static inline void outl(uint16_t port, uint32_t v) { asm volatile("outl %0, %1" ::"a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint16_t inw(uint16_t port) { uint16_t v; asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint32_t inl(uint16_t port) { uint32_t v; asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void io_wait() { outb(0x80, 0); }

// Repeated string I/O (fast path for ATA PIO).
static inline void insw(uint16_t port, void* buf, size_t count) {
    asm volatile("rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}
static inline void outsw(uint16_t port, const void* buf, size_t count) {
    asm volatile("rep outsw" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
}

static inline void cli() { asm volatile("cli" ::: "memory"); }
static inline void sti() { asm volatile("sti" ::: "memory"); }
static inline void hlt() { asm volatile("hlt"); }
static inline void cpu_relax() { asm volatile("pause"); }

#ifdef __i386__
static inline uintptr_t read_flags() { uintptr_t f; asm volatile("pushfl; popl %0" : "=r"(f)); return f; }
#else
static inline uintptr_t read_flags() { uintptr_t f; asm volatile("pushfq; popq %0" : "=r"(f)); return f; }
#endif

// Save/restore interrupt flag around a critical section.
struct IrqGuard {
    uintptr_t flags;
    IrqGuard() { flags = read_flags(); cli(); }
    ~IrqGuard() { if (flags & 0x200) sti(); }
};

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    asm volatile("wrmsr" ::"c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    asm volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

// Hardware random number if RDRAND exists, else TSC-mixed fallback.
uint64_t cpu_random64();
// CPU brand string (e.g. "AMD Ryzen 9 ..."), filled at boot.
const char* cpu_brand();

// Map the framebuffer write-combining through PAT. On 32-bit this also turns on
// paging (identity mapped with 4 MiB pages). Returns false if the CPU can't.
bool cpu_framebuffer_wc(uintptr_t addr, size_t size);
