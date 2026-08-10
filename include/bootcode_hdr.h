/* SPDX-License-Identifier: MIT */
#ifndef BOOTCODE_HDR_H
#define BOOTCODE_HDR_H

#include "gx_types.h"

#define BOOTCODE_MAGIC		0x43425847u	/* 'GXBC' LE */

struct bootcode_hdr {
	u32 magic;
	u32 size;		/* payload bytes following this header */
	u32 entry;		/* absolute VA to jump to */
	u32 checksum;		/* sum of payload bytes & 0xffffffff */
};

#endif
