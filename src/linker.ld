ENTRY(_start)

SECTIONS
{
    . = 0x100000;
    __kernel_start = .;

    .text BLOCK(4K) : ALIGN(4K)
    {
        __text_start = .;
        *(.multiboot)          /* Multiboot2 header must come first */
        *(.text .text.*)
        __text_end = .;
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data .data.*)
    }
    __kernel_load_end = .;

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss .bss.*)
    }
    __kernel_bss_end = .;

    /DISCARD/ : { *(.eh_frame) *(.eh_frame_hdr) *(.comment) *(.note*) }
}
