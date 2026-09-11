/* SPDX-License-Identifier: MIT */
/*
 * Silicon chip-name decode shared by the UART stub and (after MMU) stage-1.
 * The 12-byte field is stored reversed; short Gemini names are NUL-padded
 * at the start of the raw window (see REVERSE_ENGINEERING.md).
 */

#include "gx_hw.h"
#include "gx_chip.h"

int gx_detected_family;
char gx_detected_name[GX_CHIP_NAME_LEN + 1];

static int name_contains(const char *name, const char *key)
{
	int i;
	int n;
	int k;

	for (n = 0; name[n]; n++)
		;
	for (k = 0; key[k]; k++)
		;
	if (k > n)
		return 0;
	for (i = 0; i + k <= n; i++) {
		int j;
		int match = 1;

		for (j = 0; j < k; j++) {
			if (name[i + j] != key[j]) {
				match = 0;
				break;
			}
		}
		if (match)
			return 1;
	}
	return 0;
}

int gx_family_from_raw(const u8 raw[GX_CHIP_NAME_LEN],
		       char name_out[GX_CHIP_NAME_LEN + 1])
{
	int first = 0;
	int i;
	int o;
	int valid = 1;

	while (first < GX_CHIP_NAME_LEN && raw[first] == 0)
		first++;
	if (first == GX_CHIP_NAME_LEN)
		valid = 0;
	for (i = first; i < GX_CHIP_NAME_LEN; i++) {
		if (raw[i] < 0x20u || raw[i] > 0x7eu)
			valid = 0;
	}

	if (!valid) {
		name_out[0] = 'u';
		name_out[1] = 'n';
		name_out[2] = 'a';
		name_out[3] = 'v';
		name_out[4] = 'a';
		name_out[5] = 'i';
		name_out[6] = 'l';
		name_out[7] = 'a';
		name_out[8] = 'b';
		name_out[9] = 'l';
		name_out[10] = 'e';
		name_out[11] = 0;
		return GX_FAMILY_UNKNOWN;
	}

	o = 0;
	for (i = GX_CHIP_NAME_LEN - 1; i >= first; i--)
		name_out[o++] = (char)raw[i];
	name_out[o] = 0;

	if (name_contains(name_out, "6701") ||
	    name_contains(name_out, "6702") ||
	    name_contains(name_out, "6703"))
		return GX_FAMILY_GEMINI;
	if (name_contains(name_out, "6705") ||
	    name_contains(name_out, "6706"))
		return GX_FAMILY_CYGNUS;
	if (name_contains(name_out, "6616"))
		return GX_FAMILY_GX6616;
	if (name_contains(name_out, "3211"))
		return GX_FAMILY_GX3211;
	if (name_contains(name_out, "6612"))
		return GX_FAMILY_GX6612;
	return GX_FAMILY_UNKNOWN;
}

const char *gx_family_tag(int family)
{
	if (family == GX_FAMILY_GEMINI)
		return "gemini";
	if (family == GX_FAMILY_CYGNUS)
		return "cygnus";
	if (family == GX_FAMILY_GX6616)
		return "gx6616";
	if (family == GX_FAMILY_GX3211)
		return "gx3211";
	if (family == GX_FAMILY_GX6612)
		return "gx6612";
	return "unknown";
}

int gx_family_trains_ddr(int family)
{
	return family == GX_FAMILY_GEMINI || family == GX_FAMILY_CYGNUS;
}

int gx_chip_probe(u32 name_base)
{
	u8 raw[GX_CHIP_NAME_LEN];
	u32 i;

	for (i = 0; i < GX_CHIP_NAME_LEN; i++)
		raw[i] = readb(name_base + i);
	gx_detected_family = gx_family_from_raw(raw, gx_detected_name);
	return gx_detected_family;
}
