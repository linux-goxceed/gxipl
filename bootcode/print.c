/* SPDX-License-Identifier: MIT */
#include "gx_hw.h"
#include "ipl_config.h"
#include "ipl_version.h"

extern struct ipl_config g_ipl_cfg;
extern int g_verbose;

void bc_putc(char c)
{
	while (!(readl(UART_VIRT + 0x14u) & BIT(6)))
		;
	writel((u32)(u8)c, UART_VIRT);
}

void bc_puts(const char *s)
{
	while (*s)
		bc_putc(*s++);
}

void bc_put_hex(u32 v)
{
	static const char hex[] = "0123456789abcdef";
	int i;

	bc_puts("0x");
	for (i = 7; i >= 0; i--)
		bc_putc(hex[(v >> (i * 4)) & 0xf]);
}

void bc_put_dec(u32 v)
{
	char buf[11];
	int n = 0;

	if (!v) {
		bc_putc('0');
		return;
	}
	while (v) {
		buf[n++] = '0' + (v % 10u);
		v /= 10u;
	}
	while (n--)
		bc_putc(buf[n]);
}

void bc_vputs(const char *s)
{
	if (g_verbose)
		bc_puts(s);
}

void bc_banner(void)
{
	if (!g_verbose)
		return;
	/* BootROM handshake (e.g. B0 B8 X) has no trailing newline. */
	bc_puts("\r\nNationalChip GoXceed Bootcode\r\n");
	bc_puts("version: ");
	bc_puts(IPL_VERSION_STR);
	bc_puts(" (");
	bc_puts(IPL_CHIP);
	bc_puts(")\r\n");
}
