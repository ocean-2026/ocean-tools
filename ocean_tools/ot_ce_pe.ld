ENTRY(ce_main)
SECTIONS
{
    . = 0x2000000;   /* CE_LOAD_BASE */
    .text : ALIGN(4) { *(.text.start) *(.text*) }
    .rodata : ALIGN(4) { *(.rodata*) }
    .data : ALIGN(4) { *(.data*) }
    .ce_api : ALIGN(4) { *(.ce_api) }
    .bss : ALIGN(4) { *(.bss*) }
}
