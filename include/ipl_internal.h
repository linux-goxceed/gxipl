/* SPDX-License-Identifier: MIT */
#ifndef IPL_INTERNAL_H
#define IPL_INTERNAL_H

#include "gx_types.h"

struct field_patch {
	u8 reg;
	u8 shift;
	u8 width;
	u16 value;
};

struct efuse_tweak {
	u32 addr;
	u8 shift;
	u8 width;
	u8 byte_index;
	u8 source_shift;
};

extern int ipl_quiet;

void delay(u32 outer);
int wait_value(u32 addr, u32 mask, u32 value);
void set_field(u32 addr, u8 shift, u8 width, u32 value);
void uart_puts_at(u32 base, const char *text);
void ipl_print_gxid(u32 uart);
void ipl_crumb(char ch);
void pll_program(u32 addr, u32 value);
void apply_patches(const struct field_patch *patches, u32 count);
int efuse_read(u32 address, u8 *value);

int gx6702_clocks_init(void);
int gx6702_ddr_init(void);
int gx6706_clocks_init(void);
int gx6706_ddr_init(void);

int ipl_pre_mmu(void);
void ipl_post_mmu(void);

#endif
