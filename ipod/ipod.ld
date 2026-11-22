/*
 * Linker script for injected iPod Classic UI in SL6801 yp3_2.0.43 (240x320).
 * Uses the 128 KB zero-filled free cave inside FIRM at flash offset 0x019000
 * (XIP VMA 0x00C19000..0x00C39000).
 */
MEMORY
{
    CODE (rx) : ORIGIN = 0x00C19000, LENGTH = 0x20000
}

SECTIONS
{
    .text : {
        *(.text .text.*)
        *(.rodata .rodata.*)
    } > CODE

    .data : { *(.data .data.*) } > CODE
    .bss  : { *(.bss .bss.* COMMON) } > CODE

    /DISCARD/ : { *(.ARM.exidx* .ARM.extab* .comment .note*) }
}

ASSERT(SIZEOF(.data) == 0, "writable globals not allowed: keep state in lv_mem_alloc")
ASSERT(SIZEOF(.bss)  == 0, "writable globals not allowed: keep state in lv_mem_alloc")
