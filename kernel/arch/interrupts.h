#pragma once
#include <stdint.h>

#ifdef __i386__
// Field names match the 64-bit frame so shared code (panic) reads either one.
struct InterruptFrame {
    uint32_t rdi, rsi, rbp, rsp, rbx, rdx, rcx, rax;   // pusha
    uint32_t vector, error;
    uint32_t rip, cs, rflags;
};
#else
struct InterruptFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};
#endif

using IrqHandler = void (*)(InterruptFrame*);

void interrupts_init();                         // IDT + PIC remap + PIT
void irq_install(int irq, IrqHandler handler);  // irq 0..15
void irq_unmask(int irq);
void irq_mask(int irq);

// Milliseconds since boot (PIT at 1000 Hz).
uint64_t timer_ms();
// Busy-wait (halts between ticks; interrupts must be enabled).
void sleep_ms(uint64_t ms);
