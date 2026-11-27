// AArch64 exception vectors. Everything taken from EL1 (SP_EL1) is saved into an InterruptFrame
// (see arch/interrupts.h) plus the FP/SIMD registers, then handed to isr_dispatch().

#define FRAME_SIZE   304
#define FP_SIZE      528

.macro VENTRY n
.balign 0x80
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1, [sp]
    mov     x0, #\n
    b       vector_common
.endm

.section .text, "ax"
.balign 0x800
.global vectors
vectors:
    VENTRY 0       // current EL, SP_EL0: sync
    VENTRY 1       //                     IRQ
    VENTRY 2       //                     FIQ
    VENTRY 3       //                     SError
    VENTRY 4       // current EL, SP_ELx: sync
    VENTRY 5       //                     IRQ
    VENTRY 6       //                     FIQ
    VENTRY 7       //                     SError
    VENTRY 8       // lower EL, AArch64
    VENTRY 9
    VENTRY 10
    VENTRY 11
    VENTRY 12      // lower EL, AArch32
    VENTRY 13
    VENTRY 14
    VENTRY 15

vector_common:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]
    add     x2, sp, #FRAME_SIZE         // interrupted SP
    mrs     x3, elr_el1
    mrs     x4, spsr_el1
    mrs     x5, esr_el1
    mrs     x6, far_el1
    str     x2, [sp, #248]
    stp     x3, x4, [sp, #256]
    stp     x5, x6, [sp, #272]
    stp     x0, x5, [sp, #288]          // vector slot, error = ESR

    sub     sp, sp, #FP_SIZE
    stp     q0, q1, [sp, #0]
    stp     q2, q3, [sp, #32]
    stp     q4, q5, [sp, #64]
    stp     q6, q7, [sp, #96]
    stp     q8, q9, [sp, #128]
    stp     q10, q11, [sp, #160]
    stp     q12, q13, [sp, #192]
    stp     q14, q15, [sp, #224]
    stp     q16, q17, [sp, #256]
    stp     q18, q19, [sp, #288]
    stp     q20, q21, [sp, #320]
    stp     q22, q23, [sp, #352]
    stp     q24, q25, [sp, #384]
    stp     q26, q27, [sp, #416]
    stp     q28, q29, [sp, #448]
    stp     q30, q31, [sp, #480]
    mrs     x2, fpsr
    mrs     x3, fpcr
    str     x2, [sp, #512]
    str     x3, [sp, #520]

    add     x0, sp, #FP_SIZE            // InterruptFrame*
    bl      isr_dispatch

    ldr     x2, [sp, #512]
    ldr     x3, [sp, #520]
    msr     fpsr, x2
    msr     fpcr, x3
    ldp     q0, q1, [sp, #0]
    ldp     q2, q3, [sp, #32]
    ldp     q4, q5, [sp, #64]
    ldp     q6, q7, [sp, #96]
    ldp     q8, q9, [sp, #128]
    ldp     q10, q11, [sp, #160]
    ldp     q12, q13, [sp, #192]
    ldp     q14, q15, [sp, #224]
    ldp     q16, q17, [sp, #256]
    ldp     q18, q19, [sp, #288]
    ldp     q20, q21, [sp, #320]
    ldp     q22, q23, [sp, #352]
    ldp     q24, q25, [sp, #384]
    ldp     q26, q27, [sp, #416]
    ldp     q28, q29, [sp, #448]
    ldp     q30, q31, [sp, #480]
    add     sp, sp, #FP_SIZE

    ldp     x3, x4, [sp, #256]
    msr     elr_el1, x3
    msr     spsr_el1, x4
    ldp     x0, x1, [sp]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    add     sp, sp, #FRAME_SIZE
    eret
