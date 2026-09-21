/* SPDX-License-Identifier: MIT */
#include "gx_hw.h"
#include "ipl_config.h"
#include "ipl_config_api.h"
#include "boot.h"
#include "print.h"

struct ipl_config g_ipl_cfg;
int g_verbose;

extern char __bss_start[];
extern char __bss_end[];

static void bss_clear(void)
{
	char *p = __bss_start;

	while (p < __bss_end)
		*p++ = 0;
}

void bootcode_main(void) __attribute__((section(".text.boot_entry")));

void bootcode_main(void)
{
	bss_clear();
	ipl_config_load(&g_ipl_cfg);
	g_verbose = (g_ipl_cfg.flags & IPL_CFG_VERBOSE) ? 1 : 0;

	bc_banner();

	if (g_ipl_cfg.flags & IPL_CFG_FORCE_UART)
		goto uart;

	if (!(g_ipl_cfg.flags & IPL_CFG_SKIP_USB)) {
		if (bc_usb_boot() == 0)
			goto halt;
	} else if (g_verbose) {
                #ifdef VERBOSE_MINIFY
		bc_puts("USBBOOT\r\n");
		bc_puts("ENOUSB\r\nSPIBOOT\r\n");
                #else
		bc_puts("Attempting to boot from USB...\r\n");
		bc_puts("No USB device detected, trying SPI...\r\n");
                #endif
	}

	if (!(g_ipl_cfg.flags & IPL_CFG_SKIP_SPI)) {
		if (bc_spi_load_uboot() == 0)
			goto halt;
		if (g_verbose) {
                        #ifdef VERBOSE_MINIFY                 
			bc_puts("ERRGXBOOT\r\nUARTBOOT TIMEOUT ");
			bc_put_dec(g_ipl_cfg.uart_timeout_s ?
				   g_ipl_cfg.uart_timeout_s : 60u);
			bc_puts("s\r\n");
                        #else
			bc_puts("Unable to boot from BOOT (GxLoader partition), trying UART (");
			bc_put_dec(g_ipl_cfg.uart_timeout_s ?
				   g_ipl_cfg.uart_timeout_s : 60u);
			bc_puts("s timeout)...\r\n");
                        #endif
		}
	}

uart:
	if (!(g_ipl_cfg.flags & IPL_CFG_SKIP_UART)) {
		if (g_ipl_cfg.flags & IPL_CFG_FORCE_UART)
                        #ifdef VERBOSE_MINIFY
			bc_vputs("UARTBOOT\r\n");
                        #else
			bc_vputs("Attempting to boot from UART...\r\n");
                        #endif
		if (bc_uart_load_uboot() == 0)
			goto halt;
	}

halt:
	bc_halt();
}
