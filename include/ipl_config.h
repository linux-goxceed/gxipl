/* SPDX-License-Identifier: MIT */
#ifndef IPL_CONFIG_H
#define IPL_CONFIG_H

#include "gx_types.h"

#define IPL_CONFIG_SIZE		512u
#define IPL_CONFIG_MAGIC	0x47464331u	/* 'GFCl' little-endian '1CFG' */

/* Flags */
#define IPL_CFG_VERBOSE		BIT(0)	/* banner / path logs (default on) */
#define IPL_CFG_SKIP_USB	BIT(1)
#define IPL_CFG_SKIP_SPI	BIT(2)
#define IPL_CFG_SKIP_UART	BIT(3)
#define IPL_CFG_UART_DIRECT	BIT(4)	/* stage-1 UART loads U-Boot, skip bootcode */
#define IPL_CFG_FORCE_UART	BIT(5)	/* bootcode: try UART first */

#define IPL_CFG_DEFAULT_FLAGS	(IPL_CFG_VERBOSE | IPL_CFG_UART_DIRECT)
#define IPL_CFG_DEFAULT_UART_TIMEOUT_S	60u

struct ipl_config {
	u32 magic;
	u16 version;		/* format version: 1 */
	u16 crc;		/* sum of bytes after crc field, & 0xffff */
	u32 flags;
	u32 uart_timeout_s;
	u32 bootcode_flash_off;
	u32 uboot_flash_off;
	u32 uboot_flash_max;
	u8 reserved[IPL_CONFIG_SIZE - 28];
};

/* Absolute VA of the config blob (last 512 bytes of the 8 KiB IPL window). */
#define IPL_CONFIG_VA_8K	0x00101e00u

#ifndef IPL_CONFIG_VA
#define IPL_CONFIG_VA		IPL_CONFIG_VA_8K
#endif

#endif
