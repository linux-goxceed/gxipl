/* SPDX-License-Identifier: MIT */
ENTRY(bootcode_main)

SECTIONS
{
	. = 0x93c00000;

	.text : {
		*(.text.boot_entry)
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

	__bootcode_end = .;

	/DISCARD/ : {
		*(.comment .note* .eh_frame .csky_attributes)
	}
}
