/* QEMU test image: loaded by qemu-system-mipsel -kernel at 0x80100000 (kseg0) */
OUTPUT_ARCH(mips)
ENTRY(_start)

SECTIONS
{
    . = 0x80100000;
    .text : { *(.text.start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) *(.sdata .sdata.*) }
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }
    . = ALIGN(16) + 0x10000;
    _stack_top = .;
    /DISCARD/ : { *(.comment) *(.pdr) *(.note .note.*) }
}
