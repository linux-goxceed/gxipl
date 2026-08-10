/* SPDX-License-Identifier: MIT */
/*
 * Parse the on-flash GxLoader TABLE so SPI boot can reuse the same partition
 * map that stock GxLoader / U-Boot gxpart / serialdown use.
 *
 * Record layout (24 bytes, multi-byte fields big-endian):
 *   +0x00 name[8]  +0x08 total  +0x0c used  +0x10 start  +0x14 fs/flags/id
 */
#include "gx_table.h"
#include "gx_spi.h"

static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
	       ((u32)p[2] << 8) | (u32)p[3];
}

static int name_eq(const char *a, const char *b)
{
	while (*b) {
		if (*a != *b)
			return 0;
		a++;
		b++;
	}
	return *a == '\0';
}

static int parse_table(const u8 *buf, u32 flash_off, struct gx_table *out)
{
	u32 count, i, pos, expect;

	if (buf[0] != GX_TABLE_MAGIC0 || buf[1] != GX_TABLE_MAGIC1 ||
	    buf[2] != GX_TABLE_MAGIC2 || buf[3] != GX_TABLE_MAGIC3)
		return -1;

	count = buf[4];
	if (!count || count > GX_TABLE_MAX_PARTS)
		return -1;
	if (5u + count * GX_TABLE_REC_SIZE > GX_TABLE_SIZE)
		return -1;

	out->flash_off = flash_off;
	out->count = count;
	expect = 0;
	pos = 5;
	for (i = 0; i < count; i++) {
		struct gx_part *p = &out->part[i];
		u32 n;

		for (n = 0; n < GX_TABLE_NAME_LEN; n++)
			p->name[n] = (char)buf[pos + n];
		p->name[GX_TABLE_NAME_LEN] = '\0';
		for (n = 0; n < GX_TABLE_NAME_LEN; n++) {
			if (p->name[n] == '\0') {
				while (n < GX_TABLE_NAME_LEN)
					p->name[n++] = '\0';
				break;
			}
		}
		p->total_size = be32(&buf[pos + 8]);
		p->used_size = be32(&buf[pos + 12]);
		p->start = be32(&buf[pos + 16]);
		p->fs_type = buf[pos + 20];
		p->flags = buf[pos + 21];
		p->id = buf[pos + 22];
		if (!p->total_size)
			return -1;
		/* Stock tables are contiguous from 0; reject obvious garbage. */
		if (p->start != expect)
			return -1;
		expect += p->total_size;
		pos += GX_TABLE_REC_SIZE;
	}
	return 0;
}

int gx_table_load(struct gx_table *out)
{
	u8 buf[GX_TABLE_SIZE];
	u32 off;

	/*
	 * TABLE sits immediately after BOOT.  Stock 64 KiB BOOT -> 0x10000;
	 * 128 KiB BOOT -> 0x20000; a BOOT grown to pack U-Boot moves further.
	 * Scan 64 KiB steps through the first 1 MiB.
	 */
	for (off = 0x10000u; off < 0x100000u; off += 0x10000u) {
		if (gx_spi_read(off, buf, sizeof(buf)))
			continue;
		if (parse_table(buf, off, out) == 0)
			return 0;
	}
	return -1;
}

const struct gx_part *gx_table_find(const struct gx_table *tbl,
				    const char *name)
{
	u32 i;

	if (!tbl || !name)
		return 0;
	for (i = 0; i < tbl->count; i++) {
		if (name_eq(tbl->part[i].name, name))
			return &tbl->part[i];
	}
	return 0;
}

const struct gx_part *gx_table_part_at(const struct gx_table *tbl, u32 off)
{
	u32 i;

	if (!tbl)
		return 0;
	for (i = 0; i < tbl->count; i++) {
		const struct gx_part *p = &tbl->part[i];

		if (off >= p->start && off < p->start + p->total_size)
			return p;
	}
	return 0;
}
