/* SPDX-License-Identifier: MIT */
#ifndef GX_TABLE_H
#define GX_TABLE_H

#include "gx_types.h"

/* NationalChip GxLoader TABLE (partition version 102), 512 bytes. */
#define GX_TABLE_MAGIC0		0xaau
#define GX_TABLE_MAGIC1		0xbcu
#define GX_TABLE_MAGIC2		0xdeu
#define GX_TABLE_MAGIC3		0xfau
#define GX_TABLE_SIZE		512u
#define GX_TABLE_REC_SIZE	24u
#define GX_TABLE_MAX_PARTS	16u
#define GX_TABLE_NAME_LEN	8u

/* Common absolute flash offsets for TABLE (64 KiB / 128 KiB BOOT). */
#define GX_TABLE_OFF_64K	0x10000u
#define GX_TABLE_OFF_128K	0x20000u

struct gx_part {
	char name[GX_TABLE_NAME_LEN + 1];
	u32 total_size;
	u32 used_size;
	u32 start;
	u8 fs_type;
	u8 flags;
	u8 id;
};

struct gx_table {
	u32 flash_off;		/* where TABLE was read from */
	u32 count;
	struct gx_part part[GX_TABLE_MAX_PARTS];
};

int gx_table_load(struct gx_table *out);
const struct gx_part *gx_table_find(const struct gx_table *tbl,
				    const char *name);
const struct gx_part *gx_table_part_at(const struct gx_table *tbl, u32 off);

#endif
