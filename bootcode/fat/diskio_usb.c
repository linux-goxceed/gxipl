/* SPDX-License-Identifier: MIT */
/* FatFs disk I/O for USB MSC */

#include "ff.h"
#include "diskio.h"
#include "usb/usb_msc.h"

#define DEV_USB 0

DSTATUS disk_status(BYTE pdrv)
{
	(void)pdrv;
	return usb_msc_present() ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
	(void)pdrv;
	if (usb_msc_present())
		return 0;
	if (usb_msc_init())
		return STA_NOINIT;
	return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	UINT i;

	(void)pdrv;
	for (i = 0; i < count; i++) {
		if (usb_msc_read_sector((u32)sector + i, buff + i * 512))
			return RES_ERROR;
	}
	return RES_OK;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
	(void)pdrv;
	(void)buff;
	(void)sector;
	(void)count;
	return RES_WRPRT;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
	(void)pdrv;
	(void)buff;
	switch (cmd) {
	case CTRL_SYNC:
		return RES_OK;
	case GET_SECTOR_COUNT:
		*(LBA_t *)buff = 0;
		return RES_OK;
	case GET_SECTOR_SIZE:
		*(WORD *)buff = 512;
		return RES_OK;
	case GET_BLOCK_SIZE:
		*(DWORD *)buff = 1;
		return RES_OK;
	default:
		return RES_PARERR;
	}
}
