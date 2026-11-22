/* Injected code lives in free flash (offset 0x1D0000) and runs in place at 0xC00000 + offset.
 * build.py sets the entry symbol to the first patch target. */

MEMORY
{
	CODE (rx) : ORIGIN = 0x00DD0000, LENGTH = 0x2C000 /* up to the PSMP settings at 0x1FC000 */
}

SECTIONS
{
	.text : {
		KEEP(*(.text.entry*))
		*(.text .text.*)
		*(.rodata .rodata.*)
		/* FW_MENU_ITEM entries, in the order the apps were given to build.py */
		. = ALIGN(4);
		__fw_menu_start = .;
		KEEP(*(.fw_menu))
		__fw_menu_end = .;
	} > CODE

	.data : { *(.data .data.*) } > CODE
	.bss : { *(.bss .bss.* COMMON) } > CODE

	/DISCARD/ : { *(.ARM.exidx* .ARM.extab* .comment .note*) }
}

ASSERT(SIZEOF(.data) == 0, "writable globals are not supported: .data must be empty")
ASSERT(SIZEOF(.bss) == 0, "writable globals are not supported: .bss must be empty")
