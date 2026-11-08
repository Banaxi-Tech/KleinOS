#include "arch/interrupts.h"
#include "arch/cpu.h"
#include "lib/log.h"
#include "sys/panic.h"

#ifdef __i386__
struct IdtEntry {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t offset_hi;
} __attribute__((packed));
#else
struct IdtEntry {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t zero;
} __attribute__((packed));
#endif

struct IdtPtr {
    uint16_t limit;
    uintptr_t base;
} __attribute__((packed));

extern "C" uintptr_t isr_table[256];

static IdtEntry g_idt[256];
static IrqHandler g_irq_handlers[16];
static volatile uint64_t g_ticks;

static const char* kExceptionNames[] = {
    "Divide by zero", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range", "Invalid opcode",
    "Device not available", "Double fault", "Coprocessor overrun", "Invalid TSS", "Segment not present",
    "Stack fault", "General protection fault", "Page fault", "Reserved", "x87 FPU error",
    "Alignment check", "Machine check", "SIMD exception", "Virtualization", "Control protection",
};

static void pic_remap() {
    uint8_t m1 = 0xFF, m2 = 0xFF;
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();   // master offset 32
    outb(0xA1, 0x28); io_wait();   // slave offset 40
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    outb(0x21, m1);
    outb(0xA1, m2);
}

void irq_unmask(int irq) {
    if (irq < 8) {
        outb(0x21, inb(0x21) & ~(1 << irq));
    } else {
        outb(0xA1, inb(0xA1) & ~(1 << (irq - 8)));
        outb(0x21, inb(0x21) & ~(1 << 2));   // cascade
    }
}

void irq_mask(int irq) {
    if (irq < 8) outb(0x21, inb(0x21) | (1 << irq));
    else outb(0xA1, inb(0xA1) | (1 << (irq - 8)));
}

void irq_install(int irq, IrqHandler handler) {
    g_irq_handlers[irq] = handler;
    irq_unmask(irq);
}

static void timer_irq(InterruptFrame*) { g_ticks = g_ticks + 1; }

uint64_t timer_ms() { return g_ticks; }

void sleep_ms(uint64_t ms) {
    uint64_t end = g_ticks + ms;
    while (g_ticks < end) asm volatile("sti; hlt");
}

extern "C" void isr_dispatch(InterruptFrame* f) {
    if (f->vector < 32) {
        const char* name = f->vector < sizeof(kExceptionNames) / sizeof(kExceptionNames[0])
                               ? kExceptionNames[f->vector] : "Exception";
        uintptr_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        kpanic_frame(name, f, cr2);
        return;
    }
    if (f->vector >= 32 && f->vector < 48) {
        int irq = (int)f->vector - 32;
        // spurious IRQ7 / IRQ15
        if (irq == 7) { outb(0x20, 0x0B); if (!(inb(0x20) & 0x80)) return; }
        if (irq == 15) { outb(0xA0, 0x0B); if (!(inb(0xA0) & 0x80)) { outb(0x20, 0x20); return; } }
        if (g_irq_handlers[irq]) g_irq_handlers[irq](f);
        if (irq >= 8) outb(0xA0, 0x20);
        outb(0x20, 0x20);
    }
}

void interrupts_init() {
    for (int i = 0; i < 256; i++) {
        uintptr_t addr = isr_table[i];
        g_idt[i].offset_lo = addr & 0xFFFF;
        g_idt[i].selector = 0x08;
        g_idt[i].type_attr = 0x8E;
#ifdef __i386__
        g_idt[i].zero = 0;
        g_idt[i].offset_hi = addr >> 16;
#else
        g_idt[i].ist = 0;
        g_idt[i].offset_mid = (addr >> 16) & 0xFFFF;
        g_idt[i].offset_hi = addr >> 32;
        g_idt[i].zero = 0;
#endif
    }
    IdtPtr p{sizeof(g_idt) - 1, (uintptr_t)g_idt};
    asm volatile("lidt %0" ::"m"(p));

    pic_remap();

    // PIT channel 0 at 1000 Hz
    uint16_t div = 1193182 / 1000;
    outb(0x43, 0x36);
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    irq_install(0, timer_irq);
}
