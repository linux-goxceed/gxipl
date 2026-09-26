/* SPDX-License-Identifier: MIT */
/*
 * USB ELF smoke payload for the GX6706 (and GX6702) bootcode loader.
 *
 * bc_elf_load() accepts a static little-endian ELF32, EM_CSKY, ABIv1, ET_EXEC,
 * with e_entry and every PT_LOAD inside the cached DDR window 0x90000000
 * (64 MiB).  The default FAT name with no config.txt is start6706.elf.
 */
typedef unsigned int u32;

#define UART_VIRT 0xa0402000u

static void putc(char c)
{
	while (!(*(volatile u32 *)(UART_VIRT + 0x14u) & (1u << 6)))
		;
	*(volatile u32 *)UART_VIRT = (u32)(unsigned char)c;
}

static void puts(const char *s)
{
	while (*s)
		putc(*s++);
}

void hello(void)
{
	puts("\r\nGX6706 USB ELF hello\r\n");
	for (;;)
		;
}
