/* SPDX-License-Identifier: MIT */
#ifndef BOOTCODE_HDR_H
#define BOOTCODE_HDR_H

#include "gx_types.h"

#define BOOTCODE_MAGIC		0x43425847u	/* 'GXBC' LE */
#define UBOOT_BUNDLE_MAGIC	0x42555847u	/* 'GXUB' LE */

struct bootcode_hdr {
	u32 magic;
	u32 size;		/* payload bytes following this header */
	u32 entry;		/* absolute VA to jump to */
	u32 checksum;		/* sum of payload bytes & 0xffffffff */
};

/*
 * Single-file UART bundle produced by U-Boot's mkgxboot.py.  On the stage-2
 * wire the uploader removes the 0x20-byte .boot header, leaving "toob", the
 * fixed 8 KiB IPL body, this header, and the raw U-Boot payload.
 */
struct uboot_bundle_hdr {
	u32 magic;
	u32 size;		/* raw U-Boot bytes following this header */
	u32 entry;		/* U-Boot link address */
	u32 checksum;		/* sum of raw U-Boot bytes & 0xffffffff */
};

#endif
