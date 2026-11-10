/* SPDX-License-Identifier: MIT */
/* Link at 0: the code is position independent, the probe adds load_addr to every symbol. */
ENTRY(u5_stream)
SECTIONS
{
    . = 0;
    .text : ALIGN(4) {
        KEEP(*(.text.0trap))
        KEEP(*(.text.1stream))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    }
    .data  : { *(.data .data.*) }
    .bss   : { *(.bss .bss.* COMMON) }
    .got   : { *(.got .got.*) }
    ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0 && SIZEOF(.got) == 0, "the loader must be pure .text")
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
