/* Cached DDR, clear of the ELF staging buffer at 0x90000000 and of the
 * bootcode image at 0x93c00000.  Must stay inside the 64 MiB window. */
OUTPUT_FORMAT("elf32-csky-little", "elf32-csky-little", "elf32-csky-little")
OUTPUT_ARCH(csky)
ENTRY(hello)

SECTIONS
{
	. = 0x91000000;

	.text : {
		*(.text .text.*)
	}

	.rodata : {
		*(.rodata .rodata.*)
	}

	.data : {
		*(.data .data.*)
	}

	.bss : {
		*(.bss .bss.* COMMON)
	}

	/DISCARD/ : {
		*(.comment .note* .eh_frame .csky_attributes)
	}
}
