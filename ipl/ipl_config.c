/* SPDX-License-Identifier: MIT */
#include "gx_hw.h"
#include "ipl_config.h"

/*
 * Sum bytes after the crc field, but skip the 4-byte BootROM stage-1 trailer
 * that lives at body offset 0x1FF8.  Because the config is parked in the
 * body tail, that word falls inside it at config-relative 0x1FF8 - 0x1FC0
 * == 0x38.  Flash BootROM CRC32s body[:0x1FF8] and compares to that word;
 * keeping it out of the IPL-config checksum avoids an unsatisfiable coupling.
 * (GxLoader's TABLE partition CRCs are unrelated to this BootROM check.)
 *
 * Kept numerically explicit to match the Python sealers (mkboot/iplcfg):
 * 0x1FF8 - (0x1FF8 - IPL_CONFIG_SIZE) == IPL_CONFIG_SIZE - 8.
 *
 * The FINAL 4 bytes (config-relative 0x3C..0x3F) must also be excluded: the
 * BootROM copies only BOOT[4:0x2000] == 8188 bytes, i.e. SRAM up to
 * 0x00101FFB, but the config runs to 0x00101FFF, so bytes 60..63 hold
 * whatever was already in SRAM rather than the file contents.
 *
 * NOTE this is only half the story, and the config blob is still not usable
 * on the UART path: the uploader transmits file bytes 0x20..0x201B while the
 * config occupies 0x1FE0..0x201F, so the blob is truncated in transit and
 * arrives as all zeros.  Excluding the tail makes the checksum correct for
 * the FLASH path only.  The stage-1 -> stage-2 handoff must not rely on this
 * blob at all; it uses STAGE_HANDOFF_VA (see gx_hw.h).
 *
 * Mirrored by utils/mkboot.py _cfg_crc and utils/iplcfg.py crc16: keep all
 * three in step.
 */
#define CFG_BOOTROM_TRAILER_OFF	(IPL_CONFIG_SIZE - 8u)   /* 0x38 */
#define CFG_UNCOPIED_OFF		(IPL_CONFIG_SIZE - 4u)   /* 0x3C */

static u16 cfg_crc(const u8 *p)
{
	u32 i;
	u32 sum = 0;

	for (i = 8; i < CFG_UNCOPIED_OFF; i++) {
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

/*
 * Set (or clear) one flag bit in the live config blob and repair the CRC.
 *
 * The stage-1 IPL and the stage-2 bootcode read the SAME blob at
 * IPL_CONFIG_VA in SRAM, so this is the handoff channel between them: stage 1
 * mutates a bit, the bootcode sees it.  ipl_config_valid() checks the CRC, so
 * the CRC must be recomputed or the bootcode rejects the blob and the bit is
 * silently lost.
 *
 * No validity guard, and none is needed.  An earlier version returned early if
 * the blob did not validate, and that made the setter a silent no-op on real
 * hardware -- the CRC used to cover the 4 config bytes the BootROM never
 * writes (0x00101FFC..0x00101FFF), so the blob always failed validation in
 * SRAM even though it was correct in the .boot file.  That made
 * ipl_config_load() fall back to defaults on every boot, silently ignoring
 * VERBOSE / SKIP_* / UART_DIRECT / the flash offsets.  cfg_crc() now excludes
 * those bytes, the blob validates, and the bit is written unconditionally.
 */
void ipl_config_set_flag(u32 bit)
{
	struct ipl_config *cfg = (struct ipl_config *)IPL_CONFIG_VA;

	cfg->flags |= bit;
	cfg->crc = cfg_crc((const u8 *)cfg);
}

int ipl_config_verbose_early(void)
{
	const struct ipl_config *src = (const struct ipl_config *)IPL_CONFIG_VA;

	if (!ipl_config_valid(src))
		return (IPL_CFG_DEFAULT_FLAGS & IPL_CFG_VERBOSE) ? 1 : 0;
	return (src->flags & IPL_CFG_VERBOSE) ? 1 : 0;
}
