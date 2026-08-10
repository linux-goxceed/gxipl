/* SPDX-License-Identifier: MIT */
#ifndef GX_HW_H
#define GX_HW_H

#include "gx_types.h"

#define SYS_BASE		0x0030a000u
#define DDR_BASE		0x00c00000u
#define UART_PHYS		0x00402000u
#define UART_VIRT		0xa0402000u
#define EFUSE_CMD		0x00f80080u
#define EFUSE_STATUS		0x00f80088u

#define GXFLASH_PHYS		0x00302000u
#define GXFLASH_VIRT		0xa0302000u
#define GXFLASH_CTRL		0x00u
#define GXFLASH_STAT		0x04u
#define GXFLASH_CMD		0x08u
#define GXFLASH_DATA		0x0cu
#define GXFLASH_AUX		0x10u
#define GXFLASH_CTRL_INIT	0x0029c043u
#define GXFLASH_GO		BIT(12)
#define GXFLASH_CSHOLD		BIT(17)
#define GXFLASH_RDY		BIT(0)

#define SPI_ROUTE_REG_PHYS	0x0010221cu
#define SPI_ROUTE_REG_VIRT	0xa010221cu
#define SPI_CLK_A		0x04d00204u
#define SPI_CLK_B		0x04d00300u

#define USB_EHCI_BASE		0xa0904000u
#define USB_PHY0_BASE		0xa0908000u
#define USB_PHY1_BASE		0xa0908400u
#define USB_CFG_BASE		0xa090a000u

/* DDR-resident bootcode link/load address. */
#define BOOTCODE_ENTRY		0x93c00000u
#define BOOTCODE_MAX_SIZE	(512u * 1024u)

/* U-Boot raw entry used by the existing UART path. */
#define UBOOT_ENTRY		0x93ce8420u
#define UBOOT_MAX_SIZE		(16u * 1024u * 1024u)

/* Staging buffer for UART downloads (start of mapped DDR). */
#define STAGE_BUF		0x90000000u

#define WAIT_LIMIT		0x01000000u

/* Flash layout hints (BOOT partition / GxLoader map). */
#define FLASH_IPL_FILE_OFF	0x4u
#define FLASH_BOOTCODE_OFF	0x4000u
/* Legacy absolute U-Boot probe offset; prefer TABLE auto-discovery (config 0). */
#define FLASH_UBOOT_OFF		0x10000u
#define FLASH_BOOT_MTD_SIZE	(512u * 1024u)

static inline u32 readl(u32 addr)
{
	return *(volatile u32 *)addr;
}

static inline void writel(u32 value, u32 addr)
{
	*(volatile u32 *)addr = value;
}

#endif
