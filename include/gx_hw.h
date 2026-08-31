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

/* Reversed-byte identification fields shared by Gemini and Cygnus. */
#define GX_CHIP_NAME_VIRT	0xa030a190u
#define GX_PUBLIC_ID_VIRT	0xa030a560u

#if defined(SOC_GX6706)
/*
 * Cygnus uses a conventional 32-bit DesignWare SSI register file.  The
 * 0xa0700000 block is its clock/pad wrapper; the SSI itself is at 0xa0e00000.
 */
#define DWSPI_PHYS		0x00e00000u
#define DWSPI_VIRT		0xa0e00000u
#define DWSPI_CTRLR0		0x00u
#define DWSPI_CTRLR1		0x04u
#define DWSPI_SSIENR		0x08u
#define DWSPI_SER		0x10u
#define DWSPI_BAUDR		0x14u
#define DWSPI_RXFTLR		0x1cu
#define DWSPI_TXFLR		0x20u
#define DWSPI_RXFLR		0x24u
#define DWSPI_SR		0x28u
#define DWSPI_IMR		0x2cu
#define DWSPI_RISR		0x34u
#define DWSPI_RXOICR		0x3cu
#define DWSPI_ICR		0x48u
#define DWSPI_VERSION		0x5cu
#define DWSPI_DR		0x60u
#define DWSPI_RX_SAMPLE_DLY	0xf0u
#define DWSPI_SPI_CTRLR0	0xf4u
#define DWSPI_SR_BUSY		BIT(0)
#define DWSPI_SR_TFNF		BIT(1)
#define DWSPI_SR_TFE		BIT(2)
#define DWSPI_SR_RFNE		BIT(3)
#define DWSPI_TIMEOUT		1000000u
#define DWSPI_CTRLR0_READ	0x00000807u
#define DWSPI_CTRLR0_WRITE	0x00000407u
#define DWSPI_BAUDR_VENDOR	6u
#define DWSPI_RX_SAMPLE_DLY_VENDOR 4u
#define DWSPI_RX_CHUNK		0x4000u

/* Controller registration gate plus the version-specific external CS latch. */
#define DWSPI_GATE_VIRT		0xa030a900u
#define DWSPI_CS_LEGACY_VIRT	0xa030a904u
#define DWSPI_CS_NEW_VIRT	0xa0e00404u
#define DWSPI_VERSION_102A	0x3130322au
#define DWSPI_VERSION_1039	0x31303329u

/* Cygnus clock/reset and pad wrapper used by the stock SSI initializer. */
#define DWSPI_WRAP_VIRT		0xa0701000u
#define DWSPI_WRAP_CTRL		0x000u
#define DWSPI_WRAP_PAD		0x280u
#define DWSPI_WRAP_PAD_HI	0x284u

/* GPIO/pad route used by the stock Cygnus flash loader. */
#define SPI_ROUTE_REG_PHYS	0x0030a1f0u
#define SPI_ROUTE_REG_VIRT	0xa030a1f0u
#else
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
#endif
#define SPI_CLK_A		0x04d00204u
#define SPI_CLK_B		0x04d00300u

#define USB_EHCI_BASE		0xa0904000u
#if !defined(SOC_GX6706)
#define USB_PHY0_BASE		0xa0908000u
#define USB_PHY1_BASE		0xa0908400u
#endif
#define USB_CFG_BASE		0xa090a000u

#define DDR_PHYS_BASE		0x10000000u
#define DDR_VIRT_BASE		0x90000000u
#define DDR_SIZE		(64u * 1024u * 1024u)

/* DDR-resident bootcode link/load address. */
#define BOOTCODE_ENTRY		0x93c00000u
#define BOOTCODE_MAX_SIZE	(512u * 1024u)

/* U-Boot raw entry used by the existing UART path. */
#define UBOOT_ENTRY		0x93ce8420u
#define UBOOT_MAX_SIZE		(16u * 1024u * 1024u)

/* Staging buffer for UART downloads (start of mapped DDR). */
#define STAGE_BUF		DDR_VIRT_BASE

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

static inline u8 readb(u32 addr)
{
	return *(volatile u8 *)addr;
}

static inline void writel(u32 value, u32 addr)
{
	*(volatile u32 *)addr = value;
}

static inline void writeb(u8 value, u32 addr)
{
	*(volatile u8 *)addr = value;
}

#endif
