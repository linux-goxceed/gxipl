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

static void bc_put_hex_word(u32 v)
{
	static const char hex[] = "0123456789abcdef";
	int i;

	for (i = 7; i >= 0; i--)
		bc_putc(hex[(v >> (i * 4)) & 0xf]);
}

static void bc_chip_name(void)
{
	int i;
	int first = 0;
	int valid = 1;

	/* Short Gemini names are zero-padded at the start of the raw field. */
	while (first < 12 && readb(GX_CHIP_NAME_VIRT + (u32)first) == 0)
		first++;
	if (first == 12)
		valid = 0;
	for (i = first; i < 12; i++) {
		u8 c = readb(GX_CHIP_NAME_VIRT + (u32)i);

		if (c < 0x20u || c > 0x7eu)
			valid = 0;
	}
        #ifdef VERBOSE_MINIFY
	bc_puts("GXCHIP ");
        #else
	bc_puts("chip name: ");        
        #endif
	if (!valid) {
                #ifdef VERBOSE_MINIFY
		bc_puts("?");
                #else
		bc_puts("unavailable");
                #endif
	} else {
		/* Reverse the field, omitting what becomes trailing NUL padding. */
		for (i = 11; i >= first; i--)
			bc_putc((char)readb(GX_CHIP_NAME_VIRT + (u32)i));
	}
	bc_puts("\r\n");
}

static void bc_public_id(void)
{
	u32 low = readl(GX_PUBLIC_ID_VIRT);
	u32 high = readl(GX_PUBLIC_ID_VIRT + 4u);

        #ifdef VERBOSE_MINIFY
	bc_puts("GXID ");
        #else
	bc_puts("public id: ");        
        #endif
	if ((!low && !high) || (low == 0xffffffffu && high == 0xffffffffu)) {
                #ifdef VERBOSE_MINIFY
		bc_puts("?");
                #else
		bc_puts("unavailable");
                #endif
	} else {
		/* Match the byte-reversed display order used by stock GxLoader. */
		bc_put_hex_word(high);
		bc_put_hex_word(low);
	}
	bc_puts("\r\n");
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
        #ifdef VERBOSE_MINIFY
	bc_puts("\r\nGXBOOTCODE\r\n");
	bc_puts("VER: ");
	bc_puts(IPL_VERSION_STR);
	bc_puts(" (");
	bc_puts(IPL_CHIP);
	bc_puts(")\r\n");
	bc_chip_name();
	bc_public_id();
       #else
       	bc_puts("\r\nNationalChip GoXceed Bootcode\r\n");
	bc_puts("version: ");
	bc_puts(IPL_VERSION_STR);
	bc_puts(" (");
	bc_puts(IPL_CHIP);
	bc_puts(")\r\n");
	bc_chip_name();
	bc_public_id();
       #endif
}
