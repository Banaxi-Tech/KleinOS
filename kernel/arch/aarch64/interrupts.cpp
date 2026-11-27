// AArch64 interrupts: exception dispatch, GICv2/GICv3 interrupt controller and the generic timer
// (virtual timer, PPI 27) that provides the 1 kHz tick.
#include "arch/interrupts.h"
#include "arch/cpu.h"
#include "boot/dtb.h"
#include "lib/log.h"
#include "sys/panic.h"

namespace {

constexpr int kMaxIrq = 1020;
constexpr int kTimerPpi = 27;           // EL1 virtual timer
constexpr uint32_t kTickHz = 1000;

IrqHandler g_handlers[kMaxIrq];
uint64_t g_t0, g_freq, g_period, g_next;
volatile uint64_t g_ticks;

// ---------------------------------------------------------------- GIC
int g_gic_version = 2;
volatile uint8_t* g_gicd;     // distributor
volatile uint8_t* g_gicc;     // v2 CPU interface
volatile uint8_t* g_gicr;     // v3 redistributor of this CPU (RD frame)

inline uint32_t rd(volatile uint8_t* base, uint32_t off) { return *(volatile uint32_t*)(base + off); }
inline void wr(volatile uint8_t* base, uint32_t off, uint32_t v) { *(volatile uint32_t*)(base + off) = v; }

enum { GICD_CTLR = 0x000, GICD_TYPER = 0x004, GICD_IGROUPR = 0x080, GICD_ISENABLER = 0x100, GICD_ICENABLER = 0x180,
       GICD_IPRIORITYR = 0x400, GICD_ITARGETSR = 0x800, GICD_ICFGR = 0xC00, GICD_IROUTER = 0x6000 };
enum { GICC_CTLR = 0x000, GICC_PMR = 0x004, GICC_IAR = 0x00C, GICC_EOIR = 0x010 };
enum { GICR_WAKER = 0x014, GICR_TYPER = 0x008, GICR_SGI = 0x10000 };

inline uint64_t read_icc_sre() { uint64_t v; asm volatile("mrs %0, S3_0_C12_C12_5" : "=r"(v)); return v; }
inline void write_icc_sre(uint64_t v) { asm volatile("msr S3_0_C12_C12_5, %0\n\tisb" ::"r"(v)); }

// The per-CPU register block that holds PPIs: GICD itself for v2, the redistributor's SGI frame for v3.
inline volatile uint8_t* local_base() { return g_gic_version == 3 ? g_gicr + GICR_SGI : g_gicd; }

void gic_init() {
    DtbNode n;
    uint64_t dist = 0x08000000, cpu_if = 0x08010000, redist = 0x080A0000;
    bool v3 = false;
    if (dtb_find_compatible("arm,gic-v3", 0, &n)) {
        v3 = true;
        dist = n.reg_addr(0);
        redist = n.reg_addr(1);
    } else if (dtb_find_compatible("arm,cortex-a15-gic", 0, &n) || dtb_find_compatible("arm,gic-400", 0, &n) ||
               dtb_find_compatible("arm,cortex-a9-gic", 0, &n)) {
        dist = n.reg_addr(0);
        cpu_if = n.reg_addr(1);
    }
    g_gic_version = v3 ? 3 : 2;
    g_gicd = (volatile uint8_t*)dist;
    g_gicc = (volatile uint8_t*)cpu_if;

    if (!v3) {
        wr(g_gicd, GICD_CTLR, 0);
        uint32_t lines = ((rd(g_gicd, GICD_TYPER) & 0x1F) + 1) * 32;
        for (uint32_t i = 0; i < lines; i += 32) wr(g_gicd, GICD_ICENABLER + i / 8, 0xFFFFFFFF);
        for (uint32_t i = 32; i < lines; i += 4) wr(g_gicd, GICD_IPRIORITYR + i, 0xA0A0A0A0);
        for (uint32_t i = 32; i < lines; i += 4) wr(g_gicd, GICD_ITARGETSR + i, 0x01010101);   // CPU 0
        wr(g_gicd, GICD_CTLR, 1);
        wr(g_gicc, GICC_PMR, 0xFF);
        wr(g_gicc, GICC_CTLR, 1);
        return;
    }

    // GICv3: affinity routing, system register CPU interface
    wr(g_gicd, GICD_CTLR, (1 << 4) | (1 << 5));                 // ARE_NS | ARE_S
    for (int spin = 0; spin < 100000 && (rd(g_gicd, GICD_CTLR) & 0x80000000); spin++) {}
    uint32_t lines = ((rd(g_gicd, GICD_TYPER) & 0x1F) + 1) * 32;
    for (uint32_t i = 32; i < lines; i += 32) wr(g_gicd, GICD_ICENABLER + i / 8, 0xFFFFFFFF);
    for (uint32_t i = 32; i < lines; i += 4) wr(g_gicd, GICD_IPRIORITYR + i, 0xA0A0A0A0);
    for (uint32_t i = 32; i < lines; i += 32) wr(g_gicd, GICD_IGROUPR + i / 8, 0xFFFFFFFF);    // group 1
    for (uint32_t i = 32; i < lines; i++) *(volatile uint64_t*)(g_gicd + GICD_IROUTER + i * 8) = 0;   // -> CPU 0
    wr(g_gicd, GICD_CTLR, (1 << 4) | (1 << 5) | 1 | 2);          // + EnableGrp0 | EnableGrp1NS

    // find this CPU's redistributor
    uint64_t mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint32_t aff = (uint32_t)(((mpidr >> 32) & 0xFF) << 24 | (mpidr & 0xFFFFFF));
    volatile uint8_t* rd_frame = (volatile uint8_t*)redist;
    for (int i = 0; i < 256; i++, rd_frame += 0x20000) {
        uint64_t typer = *(volatile uint64_t*)(rd_frame + GICR_TYPER);
        if ((uint32_t)(typer >> 32) == aff) { g_gicr = rd_frame; break; }
        if (typer & 0x10) break;                                   // Last
    }
    if (!g_gicr) g_gicr = (volatile uint8_t*)redist;
    wr(g_gicr, GICR_WAKER, rd(g_gicr, GICR_WAKER) & ~2u);          // ProcessorSleep = 0
    for (int spin = 0; spin < 100000 && (rd(g_gicr, GICR_WAKER) & 4); spin++) {}
    volatile uint8_t* sgi = g_gicr + GICR_SGI;
    wr(sgi, GICD_IGROUPR, 0xFFFFFFFF);
    wr(sgi, GICD_ICENABLER, 0xFFFFFFFF);
    for (uint32_t i = 0; i < 32; i += 4) wr(sgi, GICD_IPRIORITYR + i, 0xA0A0A0A0);

    write_icc_sre(read_icc_sre() | 1);
    asm volatile("msr S3_0_C4_C6_0, %0" ::"r"((uint64_t)0xFF));    // ICC_PMR_EL1
    asm volatile("msr S3_0_C12_C12_3, %0" ::"r"((uint64_t)0));     // ICC_BPR1_EL1
    asm volatile("msr S3_0_C12_C12_7, %0\n\tisb" ::"r"((uint64_t)1));   // ICC_IGRPEN1_EL1
}

inline uint32_t gic_ack() {
    if (g_gic_version == 3) { uint64_t v; asm volatile("mrs %0, S3_0_C12_C12_0" : "=r"(v)); return (uint32_t)v; }
    return rd(g_gicc, GICC_IAR);
}
inline void gic_eoi(uint32_t iar) {
    if (g_gic_version == 3) asm volatile("msr S3_0_C12_C12_1, %0" ::"r"((uint64_t)iar));
    else wr(g_gicc, GICC_EOIR, iar);
}

void gic_set_enabled(int irq, bool on) {
    volatile uint8_t* base = irq < 32 ? local_base() : g_gicd;
    uint32_t reg = (on ? GICD_ISENABLER : GICD_ICENABLER) + (irq / 32) * 4;
    wr(base, reg, 1u << (irq % 32));
}

void gic_configure(int irq, bool edge) {
    volatile uint8_t* base = irq < 32 ? local_base() : g_gicd;
    uint32_t off = GICD_ICFGR + (irq / 16) * 4;
    uint32_t v = rd(base, off);
    v &= ~(3u << ((irq % 16) * 2));
    if (edge) v |= 2u << ((irq % 16) * 2);
    wr(base, off, v);
}

// ---------------------------------------------------------------- timer
void timer_irq(InterruptFrame*) {
    g_ticks = g_ticks + 1;
    uint64_t now = cpu_counter();
    g_next += g_period;
    if (g_next < now) g_next = now + g_period;      // fell behind (halted VM): don't storm
    asm volatile("msr cntv_cval_el0, %0" ::"r"(g_next));
}

bool (*g_debug_trap)(InterruptFrame*);

const char* exception_class(uint32_t ec) {
    switch (ec) {
    case 0x00: return "Unknown exception";
    case 0x07: return "FP/SIMD access trap";
    case 0x0E: return "Illegal execution state";
    case 0x15: return "SVC";
    case 0x18: return "System register trap";
    case 0x20: case 0x21: return "Instruction abort";
    case 0x22: return "PC alignment fault";
    case 0x24: case 0x25: return "Data abort";
    case 0x26: return "SP alignment fault";
    case 0x2C: return "FP exception";
    case 0x2F: return "SError";
    case 0x30: case 0x31: return "Breakpoint";
    case 0x32: case 0x33: return "Software step";
    case 0x34: case 0x35: return "Watchpoint";
    case 0x3C: return "BRK instruction";
    default: return "Exception";
    }
}

}  // namespace

void set_debug_trap(bool (*handler)(InterruptFrame*)) { g_debug_trap = handler; }

uint64_t timer_ms() { return g_freq ? (cpu_counter() - g_t0) * 1000 / g_freq : 0; }

void sleep_ms(uint64_t ms) {
    uint64_t end = timer_ms() + ms;
    while (timer_ms() < end) cpu_idle();
}

void irq_unmask(int irq) { if (irq >= 0 && irq < kMaxIrq) gic_set_enabled(irq, true); }
void irq_mask(int irq) { if (irq >= 0 && irq < kMaxIrq) gic_set_enabled(irq, false); }

void irq_install(int irq, IrqHandler handler, bool edge) {
    if (irq < 0 || irq >= kMaxIrq) return;
    g_handlers[irq] = handler;
    // priority and CPU target of every SPI were set up in gic_init()
    gic_configure(irq, edge);
    gic_set_enabled(irq, true);
}

extern "C" void isr_dispatch(InterruptFrame* f) {
    if (f->vector == 5 || f->vector == 1 || f->vector == 9) {      // IRQ
        for (int guard = 0; guard < 64; guard++) {
            uint32_t iar = gic_ack();
            uint32_t id = iar & 0x3FF;
            if (id >= 1020) break;                                  // spurious / nothing pending
            if (g_handlers[id]) g_handlers[id](f);
            gic_eoi(iar);
        }
        return;
    }
    uint32_t ec = (uint32_t)(f->esr >> 26);
    if (f->vector == 4 && ec == 0x3C && g_debug_trap && g_debug_trap(f)) return;
    const char* name = f->vector == 4 || f->vector == 0 ? exception_class(ec) : "Unexpected exception vector";
    kpanic_frame(name, f, f->far);
}

void interrupts_init() {
    g_freq = cpu_counter_freq();
    if (!g_freq) g_freq = 62500000;
    g_period = g_freq / kTickHz;
    g_t0 = cpu_counter();

    gic_init();

    g_next = cpu_counter() + g_period;
    asm volatile("msr cntv_cval_el0, %0" ::"r"(g_next));
    irq_install(kTimerPpi, timer_irq);
    asm volatile("msr cntv_ctl_el0, %0" ::"r"((uint64_t)1));         // enable, unmasked
    klog("irq: GICv%d at %p, generic timer %llu Hz", g_gic_version, (void*)g_gicd, (unsigned long long)g_freq);
}

