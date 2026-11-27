// KleinOS AArch64 entry.
//
// Two ways in, both ending in boot_entry with x0 = device tree, x1 = EFI handoff block (0 if none):
//   * Linux arm64 boot protocol (QEMU -kernel, U-Boot booti): _start, MMU off, x0 = DTB, x1 = 0.
//   * UEFI: firmware calls efi_entry (PE/COFF entry point), which relocates the image, loads the initrd and
//     exits boot services (efi_stub.cpp), then jumps to boot_entry with the MMU still on.
// boot_entry relocates the image to wherever it was loaded, drops from EL2 to EL1 if needed, enables FP/SIMD
// and the MMU (identity map: 0-1 GiB and 256-512 GiB device memory, the rest RAM) and calls kmain(0, dtb).

#define SCTLR_RESET   0x30D00800        // RES1 bits, MMU/caches off, little endian
#define SCTLR_M       (1 << 0)
#define SCTLR_A       (1 << 1)
#define SCTLR_C       (1 << 2)
#define SCTLR_SA      (1 << 3)
#define SCTLR_I       (1 << 12)

// Memory attributes (MAIR_EL1 indices)
#define MT_DEVICE     0                 // Device-nGnRE
#define MT_NORMAL     1                 // Write-back cacheable
#define MT_NORMAL_NC  2                 // Non-cacheable

// Level-1 block descriptors (1 GiB): valid | block | AttrIndx | AP=RW@EL1 | SH | AF
#define BLK_COMMON    (1 | (1 << 10))
#define BLK_DEVICE    (BLK_COMMON | (MT_DEVICE << 2) | (1 << 53) | (1 << 54))      // PXN, UXN
#define BLK_NORMAL    (BLK_COMMON | (MT_NORMAL << 2) | (3 << 8))                   // inner shareable

#define R_AARCH64_RELATIVE 1027

// ---------------------------------------------------------------------------- headers (page 0)
// Linux arm64 Image header whose first two bytes, "MZ", also start a DOS/PE header, followed by the PE/COFF
// header that makes this file a UEFI application.
.section .text.header, "ax"
.global _start
_start:
    add     x13, x18, #0x16             // "MZ" + a harmless instruction (code0)
    b       boot_entry                  // code1
    .quad   0                           // text_offset (the image is relocatable)
    .quad   __image_size                // image_size
    .quad   0xA                         // flags: little endian, 4K pages, place anywhere
    .quad   0                           // reserved
    .quad   0
    .quad   0
    .long   0x644D5241                  // "ARM\x64"
    .long   pe_header - _start          // offset of the PE header

pe_header:
    .ascii  "PE\0\0"
    .short  0xAA64                      // IMAGE_FILE_MACHINE_ARM64
    .short  2                           // sections
    .long   0, 0, 0                     // timestamp, symbol table
    .short  section_table - optional_header
    .short  0x206                       // executable, no line numbers, no debug
optional_header:
    .short  0x20B                       // PE32+
    .byte   0, 0
    .long   __pe_text_size              // SizeOfCode
    .long   __pe_data_file              // SizeOfInitializedData
    .long   0
    .long   __pe_entry_rva              // AddressOfEntryPoint
    .long   __pe_text_rva               // BaseOfCode
    .quad   0                           // ImageBase (relocatable; the image applies its own relocations)
    .long   4096                        // SectionAlignment
    .long   4096                        // FileAlignment (file offset == RVA)
    .short  0, 0, 0, 0, 0, 0            // versions
    .long   0
    .long   __image_size                // SizeOfImage
    .long   4096                        // SizeOfHeaders
    .long   0                           // checksum
    .short  10                          // EFI application
    .short  0                           // DllCharacteristics
    .quad   0, 0, 0, 0                  // stack/heap sizes
    .long   0
    .long   6                           // data directories (none used)
    .fill   6 * 8, 1, 0
section_table:
    .ascii  ".text\0\0\0"
    .long   __pe_text_size              // VirtualSize
    .long   __pe_text_rva               // VirtualAddress
    .long   __pe_text_size              // SizeOfRawData
    .long   __pe_text_rva               // PointerToRawData
    .long   0, 0
    .short  0, 0
    .long   0x60000020                  // code, execute, read
    .ascii  ".data\0\0\0"
    .long   __pe_data_virt              // VirtualSize (includes .bss, which firmware zeroes)
    .long   __pe_data_rva
    .long   __pe_data_file
    .long   __pe_data_rva
    .long   0, 0
    .short  0, 0
    .long   0xC0000040                  // initialised data, read, write

// ---------------------------------------------------------------------------- code
.section .text.boot, "ax"

// Apply R_AARCH64_RELATIVE relocations for the address the image was loaded at. Idempotent. Clobbers x0-x5.
relocate_image:
    adrp    x5, __kernel_start          // link address of the image start is 0, so this is the load slide
    adrp    x0, __rela_start
    add     x0, x0, :lo12:__rela_start
    adrp    x1, __rela_end
    add     x1, x1, :lo12:__rela_end
1:  cmp     x0, x1
    b.hs    3f
    ldp     x2, x3, [x0]                // r_offset, r_info
    ldr     x4, [x0, #16]               // r_addend
    add     x0, x0, #24
    cmp     w3, #R_AARCH64_RELATIVE
    b.ne    1b
    add     x4, x4, x5
    str     x4, [x5, x2]
    b       1b
3:  dsb     ish
    ret

// UEFI entry: x0 = ImageHandle, x1 = EFI_SYSTEM_TABLE*. Runs wherever the firmware loaded the image, MMU on.
.global efi_entry
efi_entry:
    stp     x29, x30, [sp, #-32]!
    stp     x19, x20, [sp, #16]
    mov     x19, x0
    mov     x20, x1
    bl      relocate_image
    mov     x0, x19
    mov     x1, x20
    bl      efi_main                    // returns only on failure (EFI_STATUS)
    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret

.global boot_entry
boot_entry:
    mov     x19, x0                     // device tree
    mov     x20, x1                     // EFI handoff block or 0
    msr     daifset, #0xf

    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    1f

    // EL2 -> EL1 (AArch64), no traps on the timer/FP
    mov     x0, #(1 << 31)              // HCR_EL2.RW
    msr     hcr_el2, x0
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3                  // EL1PCTEN | EL1PCEN
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mov     x0, #0x33FF                 // CPTR_EL2: nothing trapped
    msr     cptr_el2, x0
    msr     hstr_el2, xzr
    mov     x0, #(SCTLR_RESET & 0xFFFF)
    movk    x0, #(SCTLR_RESET >> 16), lsl #16
    msr     sctlr_el1, x0
    mov     x0, #0x3C5                  // EL1h, DAIF masked
    msr     spsr_el2, x0
    adr     x0, 1f
    msr     elr_el2, x0
    eret

1:  // ---- EL1
    // Firmware (UEFI) hands over with its own MMU and caches on. The stub already cleaned everything we
    // care about to the point of coherency, so just switch them off and carry on with our own tables.
    mrs     x0, sctlr_el1
    bic     x0, x0, #SCTLR_M
    bic     x0, x0, #SCTLR_C
    bic     x0, x0, #SCTLR_I
    msr     sctlr_el1, x0
    isb
    ic      iallu
    dsb     nsh
    isb

    bl      relocate_image

    mov     x0, #(3 << 20)              // CPACR_EL1.FPEN: no FP/SIMD traps
    msr     cpacr_el1, x0
    isb
    adr     x0, vectors
    msr     vbar_el1, x0

    // memory attributes
    ldr     x0, =((0x04 << (8 * MT_DEVICE)) | (0xFF << (8 * MT_NORMAL)) | (0x44 << (8 * MT_NORMAL_NC)))
    msr     mair_el1, x0

    // 39-bit VA (T0SZ=25), 4K granule, write-back inner-shareable walks, physical range from the CPU
    mrs     x1, id_aa64mmfr0_el1
    and     x1, x1, #0xF
    cmp     x1, #5
    mov     x2, #5
    csel    x1, x1, x2, lo              // IPS = min(PARange, 48 bit)
    lsl     x1, x1, #32
    ldr     x0, =(25 | (1 << 8) | (1 << 10) | (3 << 12) | (1 << 23))
    orr     x0, x0, x1
    msr     tcr_el1, x0

    adrp    x0, l1_table
    msr     ttbr0_el1, x0
    isb
    tlbi    vmalle1
    dsb     nsh
    isb

    mrs     x0, sctlr_el1
    orr     x0, x0, #SCTLR_M
    orr     x0, x0, #SCTLR_C
    orr     x0, x0, #SCTLR_I
    bic     x0, x0, #SCTLR_A
    bic     x0, x0, #SCTLR_SA
    msr     sctlr_el1, x0
    isb

    // stack, then clear .bss (caches are on now)
    adrp    x0, boot_stack_top
    add     x0, x0, :lo12:boot_stack_top
    mov     sp, x0
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
2:  cmp     x0, x1
    b.hs    3f
    stp     xzr, xzr, [x0], #16
    b       2b
3:
    adrp    x0, boot_handoff            // remember the EFI handoff block (after .bss was cleared)
    str     x20, [x0, :lo12:boot_handoff]
    mov     x29, xzr
    mov     x30, xzr
    mov     x0, xzr                     // "magic": unused on ARM
    mov     x1, x19                     // device tree
    bl      kmain
4:  msr     daifset, #0xf
    wfi
    b       4b

.section .data
.balign 4096
l1_table:
    .quad   BLK_DEVICE                  // 0 - 1 GiB: MMIO (UART, GIC, virtio, fw_cfg, PCI window)
    .set    gib, 1
    .rept   255                         // 1 - 256 GiB: RAM
    .quad   (gib << 30) | BLK_NORMAL
    .set    gib, gib + 1
    .endr
    .rept   256                         // 256 - 512 GiB: high PCI ECAM / MMIO
    .quad   (gib << 30) | BLK_DEVICE
    .set    gib, gib + 1
    .endr

.section .bss
.balign 8
.global boot_handoff
boot_handoff:
    .skip   8

.balign 4096
.global boot_stack_bottom
boot_stack_bottom:
    .skip   8 * 1024 * 1024
.global boot_stack_top
boot_stack_top:
