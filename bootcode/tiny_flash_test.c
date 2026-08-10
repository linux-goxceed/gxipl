/* SPDX-License-Identifier: MIT */
/*
 * Tiny DDR-resident payload for flash BOOT experiments.
 * Linked at BOOTCODE_ENTRY (0x93c00000).  Prints FLASHBC then GET and
 * accepts a raw U-Boot image the same way stage-1 does, so a 64 KiB
 * BOOT.bin can prove: (1) SPI-loaded bootcode ran, (2) UART still works.
 */

#include "gx_hw.h"

extern char __bss_start[];
extern char __bss_end[];

static void putc(char c)
{
	while (!(readl(UART_VIRT + 0x14u) & BIT(6)))
		;
	writel((u32)(u8)c, UART_VIRT);
}

static void puts(const char *s)
{
	while (*s)
		putc(*s++);
}

static u8 getc(void)
{
	while (!(readl(UART_VIRT + 0x14u) & BIT(0)))
		;
	return (u8)readl(UART_VIRT);
}

static void flush_rx(void)
{
	u32 n, idle = 0;

	for (n = 0; n < 200000u; n++) {
		if (readl(UART_VIRT + 0x14u) & BIT(0)) {
			(void)readl(UART_VIRT);
			idle = 0;
		} else if (++idle > 2000u) {
			break;
		}
	}
}

static void cache_flush(void)
{
	u32 op = BIT(0) | BIT(1) | BIT(4) | BIT(5);

	__asm__ __volatile__("mtcr %0, cr17\n\tidly4" : : "r"(op) : "memory");
}

void bootcode_main(void) __attribute__((section(".text.boot_entry")));

void bootcode_main(void)
{
	char *p;
	u32 expected, type, size, checksum, i;
	u8 *dst = (u8 *)UBOOT_ENTRY;

	for (p = __bss_start; p < __bss_end; p++)
		*p = 0;

	puts("FLASHBC\r\n");
	flush_rx();
	puts("GET");

	expected = getc() | ((u32)getc() << 8);
	type = getc() | ((u32)getc() << 8);
	size = 0;
	for (i = 0; i < 4; i++)
		size |= (u32)getc() << (8 * i);

	if (type != 0x00c2u || !size || size > UBOOT_MAX_SIZE) {
		puts("\r\nEMETA\r\n");
		for (;;)
			;
	}

	checksum = 0;
	for (i = 0; i < size; i++) {
		u8 b = getc();

		dst[i] = b;
		checksum = (checksum + b) & 0xffffu;
	}
	if (checksum != expected) {
		puts("\r\nECHK\r\n");
		for (;;)
			;
	}

	puts("\r\nOK\r\n");
	cache_flush();
	((void (*)(void))UBOOT_ENTRY)();
	for (;;)
		;
}
