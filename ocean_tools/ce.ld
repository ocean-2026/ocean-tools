OUTPUT_FORMAT("elf32-i386")
ENTRY(ce_main)
SECTIONS
{
    . = 0x02000000;
    .text.start : { *(.text.start) }
    .text : { *(.text) }
    .rodata : { *(.rodata*) }
    .data : { *(.data) }
    .ce_api : { *(.ce_api) }
    .bss : { *(.bss) *(COMMON) }
}
