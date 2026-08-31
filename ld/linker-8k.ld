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
	ASSERT(__image_end <= 0x00101e00,
	       "IPL exceeds 8KiB code window before 512-byte config")

	/DISCARD/ : {
		*(.comment .note* .eh_frame .csky_attributes)
	}
}
