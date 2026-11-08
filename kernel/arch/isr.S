# Interrupt stubs. Every vector pushes (error code or 0, vector number),
# saves general registers + SSE state and calls isr_dispatch(InterruptFrame*).

#ifdef __i386__
.macro ISR_NOERR n
.global isr\n
isr\n:
    pushl $0
    pushl $\n
    jmp isr_common
.endm

.macro ISR_ERR n
.global isr\n
isr\n:
    pushl $\n
    jmp isr_common
.endm

.text
.code32

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
.altmacro
.set i, 22
.rept 256 - 22
    ISR_NOERR %i
    .set i, i + 1
.endr

isr_common:
    pusha
    mov %esp, %ebx            # InterruptFrame* (callee-saved)
    sub $528, %esp
    and $~0xF, %esp
    fxsave 16(%esp)
    mov %ebx, (%esp)
    cld
    call isr_dispatch
    fxrstor 16(%esp)
    mov %ebx, %esp
    popa
    add $8, %esp
    iret

.section .rodata
.align 4
.global isr_table
isr_table:
.macro ISR_ADDR n
    .long isr\n
.endm
.set i, 0
.rept 256
    ISR_ADDR %i
    .set i, i + 1
.endr
#else
.macro ISR_NOERR n
.global isr\n
isr\n:
    pushq $0
    pushq $\n
    jmp isr_common
.endm

.macro ISR_ERR n
.global isr\n
isr\n:
    pushq $\n
    jmp isr_common
.endm

.text
.code64

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
.altmacro
.set i, 22
.rept 256 - 22
    ISR_NOERR %i
    .set i, i + 1
.endr

isr_common:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, %rdi            # InterruptFrame*
    mov %rsp, %rbx            # keep original rsp (callee-saved)
    sub $512, %rsp
    and $~0xF, %rsp
    fxsave (%rsp)
    cld
    call isr_dispatch
    fxrstor (%rsp)
    mov %rbx, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    add $16, %rsp
    iretq

.section .rodata
.align 8
.global isr_table
isr_table:
.macro ISR_ADDR n
    .quad isr\n
.endm
.set i, 0
.rept 256
    ISR_ADDR %i
    .set i, i + 1
.endr
#endif
