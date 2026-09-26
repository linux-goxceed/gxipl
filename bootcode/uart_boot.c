/* SPDX-License-Identifier: MIT */
#include "gx_hw.h"
#include "ipl_config.h"
#include "print.h"
#include "boot.h"

#define UART_HELO "HELO"
#define UART_HELO_ACK "OKAY"

extern struct ipl_config g_ipl_cfg;
extern int g_verbose;

/* Drop pending RX so USB/SPI-era noise cannot desync the meta header. */
static void uart_flush_rx(void)
{
	u32 n;
	u32 idle = 0;

	for (n = 0; n < 200000u; n++) {
		if (readl(UART_VIRT + 0x14u) & BIT(0)) {
			(void)readl(UART_VIRT);
			idle = 0;
		} else if (++idle > 2000u) {
			break;
		}
	}
}

static u8 uart_getc_to(u32 timeout_iters, int *timed_out)
{
	u32 n;

	*timed_out = 0;
	for (n = 0; n < timeout_iters; n++) {
		if (readl(UART_VIRT + 0x14u) & BIT(0))
			return (u8)readl(UART_VIRT);
	}
	*timed_out = 1;
	return 0;
}

static u16 read_u16_to(u32 timeout_iters, int *timed_out)
{
	u16 v = uart_getc_to(timeout_iters, timed_out);

	if (*timed_out)
		return 0;
	v |= (u16)uart_getc_to(timeout_iters, timed_out) << 8;
	return v;
}

static u32 read_u32_to(u32 timeout_iters, int *timed_out)
{
	u32 v = 0;
	u32 i;

	for (i = 0; i < 4; i++) {
		u8 b = uart_getc_to(timeout_iters, timed_out);

		if (*timed_out)
			return 0;
		v |= (u32)b << (i * 8);
	}
	return v;
}

/* Rough spin budget for ~1 second at post-PLL CPU rates. */
#define ITERS_PER_SEC 8000000u

int bc_uart_load_uboot(void)
{
	u32 timeout_s = g_ipl_cfg.uart_timeout_s ?
		g_ipl_cfg.uart_timeout_s : 60u;
	u32 iters = timeout_s * ITERS_PER_SEC;
	u32 expected, type, size, checksum, i;
	int timed_out;
	u8 prefix[4];
	u32 prefix_value;
	u8 *dst = (u8 *)UBOOT_ENTRY;

	/*
	 * Same rule as stage-1 ipl.c: flush *before* advertising ready, never
	 * after — the host may start the meta header as soon as it sees the
	 * prompt, and a post-prompt flush would drop those bytes.
	 */
	uart_flush_rx();

	if (g_verbose)
		bc_puts("waiting for download\r\n");
	else
		bc_puts("GET");

	/* Probe before the binary envelope while preserving legacy uploads. */
	for (i = 0; i < sizeof(prefix); i++) {
		prefix[i] = uart_getc_to(iters, &timed_out);
		if (timed_out)
			goto timeout;
	}
	if (prefix[0] == UART_HELO[0] && prefix[1] == UART_HELO[1] &&
	    prefix[2] == UART_HELO[2] && prefix[3] == UART_HELO[3]) {
		bc_puts(UART_HELO_ACK);
		/* A successful probe starts a fresh payload timeout window. */
		iters = timeout_s * ITERS_PER_SEC;
		expected = read_u16_to(iters, &timed_out);
		if (timed_out)
			goto timeout;
		type = read_u16_to(iters, &timed_out);
		if (timed_out)
			goto timeout;
	} else {
		prefix_value = (u32)prefix[0] |
			((u32)prefix[1] << 8) |
			((u32)prefix[2] << 16) |
			((u32)prefix[3] << 24);
		expected = prefix_value & 0xffffu;
		type = (prefix_value >> 16) & 0xffffu;
	}
	size = read_u32_to(iters, &timed_out);
	if (timed_out)
		goto timeout;

	if (type != 0x00c2u || !size) {
		if (g_verbose) {
                        #ifdef VERBOSE_MINIFY
			bc_puts("EBOOTINVALID\r\n");
                        #else
			bc_puts("Error: length error: boot image invalid");
			bc_puts(" (type=");
			bc_put_hex(type);
			bc_puts(" size=");
			bc_put_dec(size);
			bc_puts(")\r\n");
                        #endif
		}
		return -1;
	}
	if (size > UBOOT_MAX_SIZE) {
		if (g_verbose) {
                        #ifdef VERBOSE_MINIFY
                        bc_puts("EMAXLENGTH\r\n");
                        #else
			bc_puts("Error: length error: boot image ");
			bc_put_dec(size);
			bc_puts(" exceeds the maximum allowed boot size ");
			bc_put_dec(UBOOT_MAX_SIZE);
			bc_puts("\r\n");
                        #endif
		}
		return -1;
	}

	checksum = 0;
	for (i = 0; i < size; i++) {
		u8 b = uart_getc_to(iters, &timed_out);

		if (timed_out)
			goto timeout;
		dst[i] = b;
		checksum = (checksum + b) & 0xffffu;
	}
	if (checksum != expected) {
		if (g_verbose) {
                        #ifdef VERBOSE_MINIFY
                        bc_puts("ECHECKSUM\r\n");
                        #else
			bc_puts("Error: checksum error: expected ");
			bc_put_hex(expected);
			bc_puts(" but received ");
			bc_put_hex(checksum);
			bc_puts("\r\n");
                        #endif
		}
		return -1;
	}

	bc_jump(UBOOT_ENTRY);
	return 0;

timeout:
        #ifdef VERBOSE_MINIFY
        bc_puts("ETIMEOUT\r\n");
        #else
	bc_vputs("Timeout reached, bailing out.\r\n");
        #endif
	return -1;
}

void bc_halt(void)
{
        #ifdef VERBOSE_MINIFY
        bc_puts("HALT\r\n");
        #else
	bc_vputs("Halting\r\n");
        #endif
	for (;;)
		;
}
