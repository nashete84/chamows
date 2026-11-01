OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 1M;
    _load_start = .;

    .text : ALIGN(4K) {
        *(.multiboot)
        *(.text .text.*)
    }
    _text_end = .;
    .rodata : ALIGN(4K) { *(.rodata .rodata.*) }
    .data : ALIGN(4K) { *(.data .data.*) }
    _load_end = .;

    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss .bss.*)
    }
    _bss_end = .;
    _kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
