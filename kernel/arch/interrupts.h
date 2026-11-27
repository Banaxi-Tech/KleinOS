#pragma once
#include <stdint.h>

#if defined(__aarch64__)
// Saved by the exception vectors (kernel/arch/aarch64/vectors.S); the layout is shared with that file.
struct InterruptFrame {
    uint64_t x[31];                 // x0..x30 (x29 = frame pointer, x30 = link register)
    uint64_t sp, pc, pstate;        // interrupted stack pointer, ELR_EL1, SPSR_EL1
    uint64_t esr, far;              // ESR_EL1 / FAR_EL1 (synchronous exceptions)
    uint64_t vector, error;         // vector slot 0..15 (see vectors.S), ESR again
};
#elif defined(__i386__)
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
// Called for #BP and #DB before they panic; returns true if it handled the trap.
void set_debug_trap(bool (*handler)(InterruptFrame*));

#if defined(__aarch64__)
void interrupts_init();                         // vectors + GIC + generic timer
// irq = GIC interrupt ID (PPI 16..31, SPI 32..); level-triggered unless edge is set.
void irq_install(int irq, IrqHandler handler, bool edge = false);
#else
void interrupts_init();                         // IDT + PIC remap + PIT
void irq_install(int irq, IrqHandler handler);  // irq 0..15
// Called from the 1 kHz timer interrupt after each tick (sound uses it to keep playing).
void timer_set_irq_hook(void (*hook)());
#endif
void irq_unmask(int irq);
void irq_mask(int irq);

// Milliseconds since boot (PIT at 1000 Hz on x86, the generic counter on ARM).
uint64_t timer_ms();
// Busy-wait (halts between ticks; interrupts must be enabled).
void sleep_ms(uint64_t ms);
