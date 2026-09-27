/* SPDX-License-Identifier: MIT */
#ifndef IPL_CONFIG_H
#define IPL_CONFIG_H

#include "gx_types.h"

/*
 * The BootROM loads 8188 B and CRC32s body[:0x1ff8] against the word at
 * 0x1ff8, so the stage-1 code window ends at 0x1ff8.  The config is parked
 * in the tail of that window.  It only ever carried 28 real bytes, so it was
 * sized down from 512 to 64 to hand 448 B back to code: 0x1fc0 + 64 = 0x2000,
 * still ending exactly at the body end, which keeps the BootROM trailer
 * (body 0x1ff8..0x1ffb, i.e. config-relative 0x38) inside the config so the
 * existing skip-window logic still applies.
 */
#define IPL_CONFIG_SIZE		64u
#define IPL_CONFIG_MAGIC	0x47464331u	/* 'GFCl' little-endian '1CFG' */

/* Flags */
#define IPL_CFG_VERBOSE		BIT(0)	/* banner / path logs (default on) */
#define IPL_CFG_SKIP_USB	BIT(1)
#define IPL_CFG_SKIP_SPI	BIT(2)
#define IPL_CFG_SKIP_UART	BIT(3)
#define IPL_CFG_UART_DIRECT	BIT(4)	/* stage-1 UART loads U-Boot, skip bootcode */
#define IPL_CFG_FORCE_UART	BIT(5)	/* bootcode: try UART first */

/*
 * Set by stage 1 when it reached the bootcode over USB.  The gx6706 PHY
 * bring-up (cygnus_usb_clocks(), which reprograms the USB PLL) must NOT
 * be run a second time while the controller and PHY are already live:
 * on gx6706 that wedges the PHY and the bootcode never returns from
 * gx_usb_pad_phy(), so it dies right after the banner with no further
 * output.  Stage 1 already did the work, so the bootcode skips it.
 *
 * Runtime bit, not a build flag: the same bootcode binary serves the SPI,
 * UART and USB handoffs, and only the USB handoff has touched the PHY.
 */
#define IPL_CFG_USB_PHY_READY	BIT(6)

/*
 * Fallback used when no valid config blob is present in SRAM.  It must NOT
 * set IPL_CFG_UART_DIRECT: that makes ipl_post_mmu() jump straight to the
 * UART loader, skipping both the SPI bootcode attempt and the USB stage-1.5
 * attempt, so an unreadable or corrupt config would present as "USB is
 * broken" when in fact that branch never ran.  Verbose stays on so the
 * boot-path diagnostics remain visible when this fallback is in effect.
 */
#define IPL_CFG_DEFAULT_FLAGS	(IPL_CFG_VERBOSE)
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

/* Absolute VA of the config blob (last 64 bytes of the 8 KiB IPL window). */
#define IPL_CONFIG_VA_8K	0x00101fc0u

#ifndef IPL_CONFIG_VA
#define IPL_CONFIG_VA		IPL_CONFIG_VA_8K
#endif

int ipl_config_verbose_early(void);
void ipl_config_set_flag(u32 bit);	/* stage-1 -> stage-2 handoff; fixes CRC */

#endif
