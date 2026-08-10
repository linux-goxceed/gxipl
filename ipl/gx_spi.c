/* SPDX-License-Identifier: MIT */
/*
 * Minimal gxflash SPI NOR reader for the open IPL / bootcode.
 * Sequence matches u-boot/drivers/spi/gx6702_spi.c / BootROM @ 0x1af4.
 */

#include "gx_hw.h"

static u32 gxflash_base(void)
{
	/* Prefer uncached alias once MMU is on; physical works pre-MMU too
	 * only if identity-mapped — stage-1 calls this post-MMU. */
	return GXFLASH_VIRT;
}

static int gx_flash_wait(u32 base)
{
	u32 i;

	/* Keep SPI probe snappy so a dead controller cannot stall UART boot. */
	for (i = 0; i < 100000u; i++) {
		if (readl(base + GXFLASH_STAT) & GXFLASH_RDY)
			return 0;
	}
	return -1;
}

static int gx_flash_word(u32 base, u32 out, u32 *in)
{
	u32 ctrl;
	int ret;

	writel(out, base + GXFLASH_CMD);
	ctrl = readl(base + GXFLASH_CTRL) & ~GXFLASH_GO;
	writel(ctrl | GXFLASH_GO, base + GXFLASH_CTRL);
	ret = gx_flash_wait(base);
	writel(ctrl, base + GXFLASH_CTRL);
	if (ret)
		return ret;
	if (in)
		*in = readl(base + GXFLASH_DATA);
	return 0;
}

static void gx_flash_cs(u32 base, int assert)
{
	u32 ctrl = readl(base + GXFLASH_CTRL);

	if (assert)
		ctrl |= GXFLASH_CSHOLD;
	else
		ctrl &= ~GXFLASH_CSHOLD;
	writel(ctrl, base + GXFLASH_CTRL);
}

void gx_spi_init(void)
{
	u32 base = gxflash_base();

	/*
	 * Do not reprogram 0xa4d00204 / 0xa4d00300 here: start.S and
	 * clocks_init() already set the always-on domain.  Rewriting them
	 * after UART is live has been observed to break the U-Boot handoff.
	 * Only ensure the gxflash pin route and controller init value.
	 */
	writel(readl(SPI_ROUTE_REG_VIRT) | BIT(22), SPI_ROUTE_REG_VIRT);
	writel(0, base + GXFLASH_AUX);
	writel(GXFLASH_CTRL_INIT, base + GXFLASH_CTRL);
}

int gx_spi_read(u32 flash_off, void *dst, u32 len)
{
	u32 base = gxflash_base();
	u8 *out = (u8 *)dst;
	u32 pos = 0;
	u8 hdr[4];
	u32 total;
	int ret;

	hdr[0] = 0x03;
	hdr[1] = (flash_off >> 16) & 0xffu;
	hdr[2] = (flash_off >> 8) & 0xffu;
	hdr[3] = flash_off & 0xffu;
	total = 4 + len;

	gx_flash_cs(base, 1);
	for (pos = 0; pos < total; pos += 4) {
		u32 cmd = 0;
		u32 data = 0;
		u32 lane;
		int need_read = 0;

		for (lane = 0; lane < 4; lane++) {
			u32 p = pos + lane;
			u8 b;

			if (p >= total)
				break;
			if (p < 4)
				b = hdr[p];
			else {
				b = 0;
				need_read = 1;
			}
			cmd |= (u32)b << (24 - 8 * lane);
		}
		ret = gx_flash_word(base, cmd, need_read ? &data : 0);
		if (ret) {
			gx_flash_cs(base, 0);
			return ret;
		}
		if (need_read) {
			for (lane = 0; lane < 4; lane++) {
				u32 p = pos + lane;

				if (p >= total)
					break;
				if (p >= 4)
					out[p - 4] = (data >> (8 * lane)) & 0xffu;
			}
		}
	}
	gx_flash_cs(base, 0);
	return 0;
}
