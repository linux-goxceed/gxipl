/* SPDX-License-Identifier: MIT */
/*
 * Petit FatFs disk I/O for the stage-1 IPL, backed by the minimal USB MSC
 * core.  Read-only; the stick is never written.
 *
 * The window below is the whole point of this file.  Petit FatFs has NO
 * sector cache of its own, so it calls disk_readp() for every small access:
 *
 *   - dir_find()/dir_read()  -> one 32-byte read per root directory entry
 *     (a 512-entry root dir is 512 USB commands just to find one file), and
 *   - pf_read()              -> one read per 512-byte sector of file data.
 *
 * Serving each of those straight from the stick would be unusably slow.  So we
 * keep a 4-sector window in DDR, keyed by LBA, and refill it with a single
 * 4-sector READ(10).  That one mechanism covers both the directory scan and
 * the file data, and 4 sectors is exactly what usb_msc_min_read_sectors()
 * can transfer in one command.
 *
 * PFF never issues a read that crosses a sector boundary, so the window is
 * always enough to satisfy a request; no cross-boundary stitching is needed.
 */

#include "gx_types.h"
#include "pff.h"
#include "diskio.h"
#include "usb_msc_min.h"

#define WINDOW_SECTORS	USB_MSC_MIN_MAX_SECTORS
#define WINDOW_BYTES	(WINDOW_SECTORS * 512u)

/* Aligned to 4 KiB so the EHCI page-stride buffer pointers stay natural. */
static u8 __attribute__((aligned(4096))) window[WINDOW_BYTES];
static DWORD window_lba;
static int window_valid;

DSTATUS disk_initialize(void)
{
	window_valid = 0;
	return usb_msc_min_init() ? STA_NOINIT : 0;
}

/*
 * Refill the window with WINDOW_SECTORS sectors starting at lba, aligned down
 * to a WINDOW_SECTORS boundary so the key is a simple comparison.
 */
static int window_fill(DWORD lba)
{
	DWORD base = lba & ~((DWORD)WINDOW_SECTORS - 1u);

	if (window_valid && window_lba == base)
		return 0;
	if (usb_msc_min_read_sectors(base, window))
		return -1;
	window_lba = base;
	window_valid = 1;
	return 0;
}

DRESULT disk_readp(BYTE *buff, DWORD sector, UINT offset, UINT count)
{
	DWORD off;
	UINT i;

	if (!buff || !count)
		return RES_ERROR;
	if (offset > 511u || count > 512u - offset)
		return RES_ERROR;		/* PFF never asks for more than this */
	if (window_fill(sector))
		return RES_ERROR;

	off = (sector - window_lba) * 512u + offset;
	for (i = 0; i < count; i++)
		buff[i] = window[off + i];
	return RES_OK;
}

/* Read-only volume: writes are rejected rather than silently dropped. */
DRESULT disk_writep(const BYTE *buff, DWORD sc)
{
	(void)buff;
	(void)sc;
	return RES_ERROR;
}
