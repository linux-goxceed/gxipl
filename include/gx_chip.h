/* SPDX-License-Identifier: MIT */
#ifndef GX_CHIP_H
#define GX_CHIP_H

#include "gx_hw.h"
#include "gx_types.h"

#define GX_CHIP_NAME_LEN	12

enum gx_family {
	GX_FAMILY_UNKNOWN = 0,
	GX_FAMILY_GEMINI = 1,
	GX_FAMILY_CYGNUS = 2,
	/* Untested: 8 KiB UART IPL window, no DDR training. */
	GX_FAMILY_GX6616 = 3,
	GX_FAMILY_GX3211 = 4,
	/* Untested: 10 KiB IPL in a 16 KiB BootROM window, no DDR training. */
	GX_FAMILY_GX6612 = 5,
};

/*
 * Decode the 12-byte reversed silicon name field.
 * name_out is NUL-terminated, at most 12 printable chars, or "unavailable".
 * Returns GX_FAMILY_*.
 */
int gx_family_from_raw(const u8 raw[GX_CHIP_NAME_LEN], char name_out[GX_CHIP_NAME_LEN + 1]);

/* Read MMIO at name_base (PHYS or VIRT), decode, and stash gx_detected_*. */
int gx_chip_probe(u32 name_base);

/* GXID family= token: gemini, cygnus, gx6616, gx3211, gx6612, or unknown. */
const char *gx_family_tag(int family);

/* Gemini and Cygnus are the only families with open DDR init. */
int gx_family_trains_ddr(int family);

extern int gx_detected_family;
extern char gx_detected_name[GX_CHIP_NAME_LEN + 1];

#endif
