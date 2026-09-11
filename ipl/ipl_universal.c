/* SPDX-License-Identifier: MIT */
/*
 * UART chip-probe stub: identify silicon, train that family's DDR, then
 * print GXID and wait for RUNGET.  Not a flash IPL.
 */

#include "gx_hw.h"
#include "gx_chip.h"
#include "ipl_config_api.h"
#include "ipl_internal.h"

static int uart_init_unknown(u32 clock_hz)
{
	u32 divisor = (clock_hz + 921600u) / 1843200u;
	u32 reg;

	reg = readl(SYS_BASE + 0x150u) | BIT(22) | BIT(23);
	writel(reg, SYS_BASE + 0x150u);
	reg = readl(SYS_BASE + 0x480u) | BIT(0) | BIT(1);
	writel(reg, SYS_BASE + 0x480u);
	writel(0, UART_PHYS + 0x04u);
	writel(3, UART_PHYS + 0x10u);
	if (wait_value(UART_PHYS + 0x7cu, BIT(0), 0))
		return -1;
	writel(0x80, UART_PHYS + 0x0cu);
	writel(divisor & 0xffu, UART_PHYS + 0x00u);
	writel((divisor >> 8) & 0xffu, UART_PHYS + 0x04u);
	if (wait_value(UART_PHYS + 0x7cu, BIT(0), 0))
		return -1;
	writel(3, UART_PHYS + 0x0cu);
	writel(1, UART_PHYS + 0x08u);
	writel(readl(UART_PHYS + 0x7cu), UART_PHYS + 0xc0u);
	return 0;
}

__attribute__((used, externally_visible))
int ipl_pre_mmu(void)
{
	int family;

	ipl_quiet = ipl_config_verbose_early();
	ipl_crumb('I');
	family = gx_chip_probe(GX_CHIP_NAME_PHYS);
	if (family == GX_FAMILY_GEMINI) {
		if (gx6702_clocks_init()) {
			uart_puts_at(UART_PHYS, "EPLL\r\n");
			return -1;
		}
		if (gx6702_ddr_init()) {
			uart_puts_at(UART_PHYS, "EDDR\r\n");
			return -1;
		}
		return 0;
	}
	if (family == GX_FAMILY_CYGNUS) {
		if (gx6706_clocks_init()) {
			uart_puts_at(UART_PHYS, "EPLL\r\n");
			return -1;
		}
		if (gx6706_ddr_init()) {
			uart_puts_at(UART_PHYS, "EDDR\r\n");
			return -1;
		}
		return 0;
	}

	/* GX6616/GX3211/GX6612: identify only. No open DDR init. */
	(void)uart_init_unknown(24000000u);
	ipl_print_gxid(UART_PHYS);
	if (family == GX_FAMILY_UNKNOWN)
		uart_puts_at(UART_PHYS, "ECHIP\r\n");
	else
		uart_puts_at(UART_PHYS, "ENODDR\r\n");
	return -1;
}
