# KleinOS multiboot2 entry: 32-bit protected mode -> 64-bit long mode.
# Identity-maps the first 4 GiB with 2 MiB pages and calls kmain(magic, info).

.set MB2_MAGIC, 0xE85250D6

.section .multiboot2, "a"
.align 8
mb2_header:
    .long MB2_MAGIC
    .long 0
    .long mb2_header_end - mb2_header
    .long 0x100000000 - (MB2_MAGIC + (mb2_header_end - mb2_header))
    # framebuffer tag: request 1280x800x32
    .align 8
    .short 5
    .short 0
    .long 20
    .long 1280
    .long 800
    .long 32
    # module alignment tag
    .align 8
    .short 6
    .short 0
    .long 8
    # end tag
    .align 8
    .short 0
    .short 0
    .long 8
mb2_header_end:

#ifdef __i386__
# 32-bit build: GRUB already left us in flat protected mode, so identity
# addressing needs no paging. Requires SSE2 (Pentium 4 / Athlon 64 or later).
.section .bss
.align 4096
.global boot_stack_bottom
boot_stack_bottom:
boot_stack: .skip 8 * 1024 * 1024
.global boot_stack_top
boot_stack_top:

.section .data
.align 16
gdt32:
    .quad 0
    .quad 0x00CF9A000000FFFF    # 0x08: kernel code
    .quad 0x00CF92000000FFFF    # 0x10: kernel data
gdt32_end:
gdt32_ptr:
    .short gdt32_end - gdt32 - 1
    .long gdt32

.section .text.boot, "ax"
.code32
.global _start
_start:
    cli
    mov $boot_stack_top, %esp
    mov %eax, %esi              # multiboot magic
    mov %ebx, %edi              # multiboot info
    lgdt gdt32_ptr
    ljmp $0x08, $1f
1:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %ax, %ax
    mov %ax, %fs
    mov %ax, %gs
    xor %ebp, %ebp

    # no SSE2: the kernel is compiled for it, so stop here
    mov $1, %eax
    cpuid
    test $(1 << 26), %edx
    jz 2f

    # enable SSE: CR0.EM=0, CR0.MP=1, CR4.OSFXSR=1, CR4.OSXMMEXCPT=1
    mov %cr0, %eax
    and $~0x4, %eax
    or $0x2, %eax
    mov %eax, %cr0
    mov %cr4, %eax
    or $0x600, %eax
    mov %eax, %cr4
    fninit

    and $~0xF, %esp
    sub $8, %esp
    push %edi
    push %esi
    call kmain
2:
    cli
    hlt
    jmp 2b

#else
.section .bss
.align 4096
.global boot_stack_bottom
boot_stack_bottom:
boot_stack: .skip 8 * 1024 * 1024
.global boot_stack_top
boot_stack_top:
# page tables live ABOVE the stack so a stack overflow can't corrupt them
.align 4096
pml4:       .skip 4096
pdpt:       .skip 4096
.global pd
pd:         .skip 4096 * 4

.section .data
.align 8
mb_magic:   .long 0
mb_info:    .long 0

.align 16
gdt64:
    .quad 0
    .quad 0x00AF9A000000FFFF    # 0x08: kernel code, 64-bit
    .quad 0x00CF92000000FFFF    # 0x10: kernel data
gdt64_end:
.global gdt64_ptr
gdt64_ptr:
    .short gdt64_end - gdt64 - 1
    .quad gdt64

.section .text.boot, "ax"
.code32
.global _start
_start:
    cli
    mov $boot_stack_top, %esp
    mov %eax, mb_magic
    mov %ebx, mb_info

    # PML4[0] -> PDPT
    mov $pdpt, %eax
    or $3, %eax
    mov %eax, pml4

    # PDPT[0..3] -> PD0..PD3
    mov $pd, %eax
    or $3, %eax
    mov %eax, pdpt
    add $4096, %eax
    mov %eax, pdpt + 8
    add $4096, %eax
    mov %eax, pdpt + 16
    add $4096, %eax
    mov %eax, pdpt + 24

    # 2048 x 2 MiB pages = 4 GiB identity map
    xor %ecx, %ecx
1:
    mov %ecx, %eax
    shl $21, %eax
    or $0x83, %eax
    mov %eax, pd(,%ecx,8)
    movl $0, pd + 4(,%ecx,8)
    inc %ecx
    cmp $2048, %ecx
    jne 1b

    mov $pml4, %eax
    mov %eax, %cr3

    # PAE
    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4

    # EFER.LME
    mov $0xC0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr

    # paging + protected mode
    mov %cr0, %eax
    or $0x80000001, %eax
    mov %eax, %cr0

    lgdt gdt64_ptr
    ljmp $0x08, $long_mode_entry

.code64
long_mode_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %ax, %ax
    mov %ax, %fs
    mov %ax, %gs
    mov $boot_stack_top, %rsp
    xor %rbp, %rbp

    # enable SSE: CR0.EM=0, CR0.MP=1, CR4.OSFXSR=1, CR4.OSXMMEXCPT=1
    mov %cr0, %rax
    and $~0x4, %rax
    or $0x2, %rax
    mov %rax, %cr0
    mov %cr4, %rax
    or $0x600, %rax
    mov %rax, %cr4
    fninit

    mov mb_magic(%rip), %edi
    mov mb_info(%rip), %esi
    call kmain
2:
    cli
    hlt
    jmp 2b
#endif
