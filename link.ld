OUTPUT_FORMAT(elf32-tradlittlemips)
OUTPUT_ARCH(mips)
ENTRY(_start)

SECTIONS
{
    /* Must match the U-Boot load/go address, otherwise la/data refs break.
       Default 0x80008000; override with -Wl,--defsym=LOAD_ADDR=0x... */
    . = DEFINED(LOAD_ADDR) ? LOAD_ADDR : 0x80008000;
    .text : { KEEP(*(.text.start)) *(.text*) }
    .rodata : { *(.rodata*) }
    /* C++ global constructors, run by start.c before main () */
    .init_array : {
        . = ALIGN(4);
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) KEEP(*(.ctors))
        __init_array_end = .;
    }
    .data : { *(.data*) }
    .bss : {
        __bss_start = .;
        *(.bss*) *(.sbss*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }

    /* Keep ABI metadata out of the flat binary */
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.MIPS.options) *(.pdr) *(.comment) *(.gnu.attributes)
                  *(.eh_frame) *(.fini_array*) *(.dtors) }
}
