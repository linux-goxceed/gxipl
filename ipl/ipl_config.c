/* SPDX-License-Identifier: MIT */
#include "gx_hw.h"
#include "ipl_config.h"

/*
 * Sum bytes after the crc field, but skip the 4-byte BootROM stage-1 trailer
 * that lives at body offset 0x1FF8 (= config-relative 0x1F8 on 8 KiB images).
 * Flash BootROM CRC32s body[:0x1FF8] and compares to that word; keeping it out
 * of the IPL-config checksum avoids an unsatisfiable coupling.  (GxLoader's
 * TABLE partition CRCs are unrelated to this BootROM check.)
 */
#define CFG_BOOTROM_TRAILER_OFF	0x1f8u

static u16 cfg_crc(const u8 *p)
{
	u32 i;
	u32 sum = 0;

	for (i = 8; i < IPL_CONFIG_SIZE; i++) {
		if (i >= CFG_BOOTROM_TRAILER_OFF && i < CFG_BOOTROM_TRAILER_OFF + 4u)
			continue;
		sum += p[i];
	}
	return (u16)(sum & 0xffffu);
}

int ipl_config_valid(const struct ipl_config *cfg)
{
	return cfg->magic == IPL_CONFIG_MAGIC && cfg->version == 1 &&
	       cfg->crc == cfg_crc((const u8 *)cfg);
}

void ipl_config_defaults(struct ipl_config *cfg)
{
	u32 i;

	for (i = 0; i < IPL_CONFIG_SIZE; i++)
		((u8 *)cfg)[i] = 0;
	cfg->magic = IPL_CONFIG_MAGIC;
	cfg->version = 1;
	cfg->flags = IPL_CFG_DEFAULT_FLAGS;
	cfg->uart_timeout_s = IPL_CFG_DEFAULT_UART_TIMEOUT_S;
	cfg->bootcode_flash_off = FLASH_BOOTCODE_OFF;
	/* 0 = discover next-stage GXBC via on-flash GxLoader TABLE. */
	cfg->uboot_flash_off = 0;
	cfg->uboot_flash_max = 0;
	cfg->crc = cfg_crc((const u8 *)cfg);
}

int ipl_config_load(struct ipl_config *out)
{
	const struct ipl_config *src = (const struct ipl_config *)IPL_CONFIG_VA;
	u32 i;
	const u8 *p = (const u8 *)src;

	if (!ipl_config_valid(src)) {
		ipl_config_defaults(out);
		return -1;
	}
	for (i = 0; i < IPL_CONFIG_SIZE; i++)
		((u8 *)out)[i] = p[i];
	return 0;
}

int ipl_config_verbose_early(void)
{
	const struct ipl_config *src = (const struct ipl_config *)IPL_CONFIG_VA;

	if (!ipl_config_valid(src))
		return (IPL_CFG_DEFAULT_FLAGS & IPL_CFG_VERBOSE) ? 1 : 0;
	return (src->flags & IPL_CFG_VERBOSE) ? 1 : 0;
}
