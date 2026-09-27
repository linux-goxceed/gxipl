/* SPDX-License-Identifier: MIT */
/*
 * Petit FatFs configuration for the stage-1 IPL.
 *
 * This shadows fatfs-petit/pffconf.h via -I ordering, so the checked-in
 * upstream file stays pristine and the stage-2 FatFs build is unaffected.
 *
 * Rev ID is unchanged at 8088 -- the only differences are the switches below.
 * pff.h requires PF_DEFINED == PFCONF_DEF, so both are set together.
 */
#ifndef PFCONF_DEF
#define PFCONF_DEF	8088	/* Revision ID (must match PF_DEFINED) */

/*---------------------------------------------------------------------------
/ Function Configurations (0:Disable, 1:Enable)
/---------------------------------------------------------------------------*/

#define PF_USE_READ		1	/* pf_read() -- the only one we need */
#define PF_USE_DIR		0	/* no pf_opendir/pf_readdir */
#define PF_USE_LSEEK		0	/* no pf_lseek: strictly forward-only */
#define PF_USE_WRITE		0	/* read-only volume */

#define PF_FS_FAT12		0
/* FAT16 and FAT32 are supported by all IPL USB profiles. */
#define PF_FS_FAT16		1
#define PF_FS_FAT32		1

/*---------------------------------------------------------------------------
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

/*
 * Must stay 0.  With PF_USE_LCC == 0 the 128-byte case-conversion table and
 * the DBCS branches are compiled out entirely; with it == 1 pff.c wants a
 * per-code-page table that would cost more than the whole rest of the file.
 */
#define PF_USE_LCC		0

#define PF_CODE_PAGE	437	/* no effect while PF_USE_LCC == 0 */

#endif /* PFCONF_DEF */
