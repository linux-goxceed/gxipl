/* SPDX-License-Identifier: MIT */
/*
 * Stage-1.5 loader: pull the next stage off a USB stick, GRUB-style.
 *
 * The IPL is a fixed 8 KiB window in SRAM, so the bootcode that would
 * normally sit in flash cannot be built into it at the size it wants.  Instead
 * the IPL mounts a FAT16/FAT32 stick, streams one small flat binary to
 * BOOTCODE_ENTRY, and jumps to it.  That binary is the ordinary GXBC bootcode
 * image, so the flash-resident and USB-resident paths converge on the same
 * stage 2 and nothing downstream has to care where it came from.
 *
 * Why a flat binary and not an ELF: the IPL would otherwise have to carry
 * bootcode/elf.c (221 lines of validation with 26 error codes) to relocate
 * PT_LOAD segments.  The 16-byte header below already gives us a magic, a
 * length, an entry point and a checksum, which is all a stage-2 loader
 * actually needs.
 *
 * Petit FatFs constraints that shape the code below:
 *   - 8.3 names only (no LFN), so the candidates below must be valid 8.3 and
 *     a longer name is silently truncated into a bogus path.
 *   - forward-only: pf_lseek is compiled out, so the file is one open pass.
 *   - one file open at a time, ever.
 *   - a 512-entry root dir means up to 512 32-byte directory reads to find
 *     one file; disk_readp()'s 4-sector window absorbs that.
 */

#include "gx_hw.h"
#include "bootcode_hdr.h"
#include "usb_debug.h"

#if defined(IPL_STAGE15_USB)

#include "pff.h"
#include "usb_msc_min.h"
#include "ipl_config.h"
#include "ipl_config_api.h"
#include "ipl_internal.h"

/* Read granularity: one USB command per chunk, and the window refill makes
 * this cheap because consecutive chunks land in the same 4-sector window. */
#define STAGE15_CHUNK	4096u

/*
 * Candidate names, tried in order, per SoC. Keep these as uppercase 8.3
 * short names: FAT stores those name bytes uppercase even when a host displays
 * the file in lowercase. PFF has no long-filename support, so longer names
 * would be silently truncated into a path that cannot exist.
 */
static const char *const stage15_names[] = {
#if defined(SOC_GX6706)
	"/BOOT6706.BIN",
#else
	"/BOOT6702.BIN",
#endif
};
#define STAGE15_NAME_COUNT	1

static FATFS stage15_fs;

/*
 * Failure codes, reported on the UART before falling through to the next
 * boot path.  Without these, "no stick", "stick not FAT" and "file present
 * but corrupt" are all indistinguishable from the outside, and every one of
 * them ends in the same RUNGET.
 *
 * Kept to single characters so the whole set costs almost nothing.
 */
#define S15_OK          '0'
#define S15_NO_MEDIA    '1'    /* USB never enumerated */
#define S15_NO_FS       '2'    /* pf_mount() rejected the volume */
#define S15_NO_FILE     '3'    /* FAT ok, BOOTnnnn.BIN not found */
#define S15_SHORT_HDR   '4'    /* file shorter than the 16-byte header */
#define S15_BAD_MAGIC   '5'    /* magic / size out of range */
#define S15_SHORT_BODY  '6'    /* read returned fewer bytes than size */
#define S15_BAD_SUM     '7'    /* payload byte sum mismatch */
#define S15_NOT_READY   '8'    /* USB enumerated, but BOT probe failed */
#define S15_BAD_SIG     '9'    /* sector 510 signature was not 0xaa55 */
#define S15_BAD_TYPE    'A'    /* 0xaa55 ok, but no FAT16/32 filesystem tag */
#define S15_NO_PART     'B'    /* MBR partition entry empty or unreadable */

/*
 * Reporting is compiled in by default because the IPL has no other way to
 * explain a stage-1.5 failure.  The code set is deliberately small: a full
 * switch over pff_last_fail cost ~130 B on gx6706, which has only ~90 B
 * spare.  The current set identifies the failure *family* (USB vs wrong
 * sector vs not partitioned vs not FAT), which is what actually narrows a
 * hardware fault down.
 */
static void s15_report(char code)
{
	/* uart_putc_at() is file-static in ipl_common.c, and ipl_crumb() targets
	 * UART_PHYS and is suppressed under the verbose config, so emit the
	 * character through a two-byte string on the virtual UART instead. */
	char msg[2];

	msg[0] = code;
	msg[1] = '\0';
	uart_puts_at(UART_VIRT, "\r\nE15=");
	uart_puts_at(UART_VIRT, msg);
	uart_puts_at(UART_VIRT, "\r\n");
}

/*
 * Stream one candidate to BOOTCODE_ENTRY, verifying the 16-byte header and
 * the trailing 32-bit byte sum.  Returns 0 and jumps on success.
 */
static int stage15_try(const char *name)
{
	struct bootcode_hdr hdr;
	UINT br;
	const u8 *p;
	u8 *dst = (u8 *)BOOTCODE_ENTRY;
	u32 sum = 0;
	u32 left;
	u32 n;

	if (pf_open(name) != FR_OK) {
		s15_report(S15_NO_FILE);
		return -1;
	}
	/*
	 * Read the header, not the file size from pf_open: PFF exposes the
	 * size through the volume object, and the header is what we validate.
	 */
	if (pf_read(&hdr, sizeof(hdr), &br) != FR_OK || br != sizeof(hdr)) {
		s15_report(S15_SHORT_HDR);
		return -1;
	}
	if (hdr.magic != BOOTCODE_MAGIC || !hdr.size ||
	    hdr.size > BOOTCODE_MAX_SIZE) {
		s15_report(S15_BAD_MAGIC);
		return -1;
	}

	/*
	 * Checksum covers the PAYLOAD ONLY, matching try_spi_bootcode() in
	 * ipl_common.c and splice_boot.py.  Folding the header bytes in here
	 * would be unsatisfiable: the stored checksum field is part of the sum,
	 * so the check reduces to sum(H_other) + sum(P) == 0 mod 2^32, which
	 * does not hold for a real image.
	 */
	left = hdr.size;
	while (left) {
		UINT want = left > STAGE15_CHUNK ? STAGE15_CHUNK : left;

		if (pf_read(dst, want, &br) != FR_OK || br != want) {
			s15_report(S15_SHORT_BODY);
			return -1;
		}
		for (p = dst, n = 0; n < want; n++)
			sum += *p++;
		dst += want;
		left -= want;
	}
	if (sum != hdr.checksum) {
		s15_report(S15_BAD_SUM);
		return -1;
	}
	/*
	 * Hand the controller back in a known state.  The stage-2 bootcode
	 * starts its own USB driver with USBCMD_HCRST, which cannot converge
	 * if we leave ASE running with a live async schedule and the port
	 * still enabled -- the bootcode then hangs with no output at all.
	 *
	 * Only reached when stage 1 got here over USB; the SPI and UART handoffs
	 * never started the controller, so the call is a no-op for them.
	 */
	usb_msc_min_quiesce();
	/*
	 * Tell the bootcode that we already brought the USB PHY up.  It lives
	 * at the same SRAM address we read, so setting the bit here is how the
	 * handoff is communicated.  Without it the bootcode re-runs
	 * cygnus_usb_clocks() against a live PHY and wedges on gx6706.
	 *
	 * Update the CRC too: ipl_config_valid() checks it, and a mutated
	 * blob with a stale CRC would be rejected and the bit lost.
	 */
	/*
	 * Tell the bootcode we already brought the PHY up.  This goes to the
	 * dedicated handoff slot, NOT the IPL config blob: over UART the
	 * BootROM window stops at file 0x201B and the config (0x1FE0..0x201F)
	 * is never transmitted, so a flag written there is simply lost.
	 */
	*(volatile u32 *)STAGE_HANDOFF_VA = STAGE_HANDOFF_USB_PHY_READY;
	jump_to(hdr.entry ? hdr.entry : BOOTCODE_ENTRY);
	return 0;
}

int try_usb_stage15(void)
{
	u32 i;
	FRESULT mres;

	/*
	 * Distinguish the three mount outcomes.  FR_NOT_READY means the MSC
	 * device never became ready, which is a USB-enumeration problem, not a
	 * FAT-format one -- the commonest case by far, and previously
	 * indistinguishable from "wrong filesystem".
	 */
	mres = pf_mount(&stage15_fs);
	if (mres != FR_OK) {
		/*
		 * Split the one distinction that actually localises the fault,
		 * and costs almost nothing:
		 *   FR_NOT_READY   the MSC device never became ready -> USB
		 *                  enumeration / stick not detected at all
		 *   anything else  the volume was read and rejected     -> FAT
		 *                  format, super-floppy, FAT12, etc.
		 * The finer pff_mount() failure codes cost ~100 B on gx6706,
		 * which has under 90 B spare, so they are not carried.
		 */
		s15_report(mres == FR_NOT_READY ? S15_NOT_READY : S15_NO_FS);
		return -1;
	}
	for (i = 0; i < STAGE15_NAME_COUNT; i++) {
		if (stage15_try(stage15_names[i]) == 0)
			return 0;
	}
	return -1;
}

#else /* !IPL_STAGE15_USB */

int try_usb_stage15(void)
{
	return -1;
}

#endif /* IPL_STAGE15_USB */
