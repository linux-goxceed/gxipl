/* SPDX-License-Identifier: MIT */
ENTRY(_start)

SECTIONS
{
	. = 0x00100000;

	.text : {
		KEEP(*(.vectors))
		*(.text.start)
		*(.text .text.*)
	}

	.rodata : {
		*(.rodata .rodata.* .srodata .srodata.*)
	}

	.data : {
		*(.data .data.* .sdata .sdata.*)
	}

	. = ALIGN(4);
	__bss_start = .;
	.bss (NOLOAD) : {
		*(.bss .bss.* .sbss .sbss.* COMMON)
	}
	__bss_end = .;

	__image_end = LOADADDR(.data) + SIZEOF(.data);
	/* 0x1fc0 = end of the 64-byte IPL config, which is itself well inside
	 * the BootROM's hard 0x1ff8 limit (4-byte stage-1 trailer). */
	ASSERT(__image_end <= 0x00101fc0,
	       "IPL exceeds code window before 64-byte config")

	/DISCARD/ : {
		*(.comment .note* .eh_frame .csky_attributes)
	}
}
