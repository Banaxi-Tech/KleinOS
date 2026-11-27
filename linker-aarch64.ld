/* KleinOS for AArch64.
 *
 * One file, three ways to boot it:
 *   - a Linux-style arm64 Image (QEMU -kernel, U-Boot booti): the first 64 bytes are the Image header;
 *   - a UEFI application (BOOTAA64.EFI): the same header carries a PE/COFF header, so firmware loads the
 *     very same bytes; kernel/arch/aarch64/efi_stub.cpp then loads the initrd and exits boot services;
 *   - either way the image can land anywhere in RAM. It is linked at address 0 as a position independent
 *     executable and relocates itself (boot.S), so every address below is relative to the load address.
 *
 * Section layout matches the PE sections: page 0 = headers, then [.text .rodata ...] read/execute and
 * [.data .bss ...] read/write, all 4 KiB aligned, so file offset == RVA == link address.
 */
ENTRY(_start)

SECTIONS
{
    . = 0;
    __kernel_start = .;

    .header : { KEEP(*(.text.header)) }

    . = 0x1000;
    __text_start = .;
    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }
    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
    }
    .dynsym   : { *(.dynsym) }
    .dynstr   : { *(.dynstr) }
    .hash     : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .rela.dyn : ALIGN(8) {
        __rela_start = .;
        *(.rela.dyn)
        __rela_end = .;
    }
    . = ALIGN(4K);
    __text_end = .;

    __data_start = .;
    .init_array : ALIGN(8) {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        __init_array_end = .;
    }
    .dynamic : { *(.dynamic) }
    .got : { *(.got) *(.got.plt) }
    .data : ALIGN(4K) {
        *(.data .data.*)
    }
    . = ALIGN(4K);
    __data_end = .;

    .bss : ALIGN(4K) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    . = ALIGN(4K);
    __kernel_end = .;

    /* Absolute values for the headers (the PE header holds sizes and RVAs, which are link addresses) */
    __image_size   = ABSOLUTE(__kernel_end - __kernel_start);
    __pe_text_rva  = ABSOLUTE(__text_start);
    __pe_text_size = ABSOLUTE(__text_end - __text_start);
    __pe_data_rva  = ABSOLUTE(__data_start);
    __pe_data_file = ABSOLUTE(__data_end - __data_start);
    __pe_data_virt = ABSOLUTE(__kernel_end - __data_start);
    __pe_entry_rva = ABSOLUTE(efi_entry);

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
        *(.interp)
    }
}
