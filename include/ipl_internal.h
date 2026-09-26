/* SPDX-License-Identifier: MIT */
#ifndef IPL_INTERNAL_H
#define IPL_INTERNAL_H

#include "gx_types.h"

/*
 * Size-golfed packed table entries.  The readable FP()/ET() initializer
 * macros keep the source tables inspectable while each entry compiles to a
 * single u32 instead of a 6/8-byte struct.
 *
 * field patch: reg(8) | shift(5) | log2(width)(3) | value(16)
 *   width must be a power of two (all DDR tables use 1/4/16).
 */
#define FP_WCODE(w)	((w) == 1 ? 0u : (w) == 2 ? 1u : (w) == 4 ? 2u : \
			 (w) == 8 ? 3u : 4u)
#define FP(reg, shift, width, value) \
	((u32)(reg) | ((u32)(shift) << 8) | (FP_WCODE(width) << 13) | \
	 ((u32)(value) << 16))
#define FP_REG(p)	((u32)((p) & 0xffu))
#define FP_SHIFT(p)	((u32)(((p) >> 8) & 0x1fu))
#define FP_WIDTH(p)	(1u << (((p) >> 13) & 7u))
#define FP_VALUE(p)	((u32)((p) >> 16))

/*
 * efuse tweak: base(1: 0=SYS_BASE, 1=DDR_BASE) | off(12) | shift(5) |
 *              width(4) | byte_index(3) | source_shift(3)
 *   All tweak addresses are SYS_BASE+0x000..0xfff or DDR_BASE+0x000..0xfff.
 */
#define ET_BASE(addr)	((((addr) & 0x00f00000u) == 0x00c00000u) ? 1u : 0u)
#define ET(addr, shift, width, byte_index, source_shift) \
	((ET_BASE(addr) << 27) | (((addr) & 0xfffu) << 15) | \
	 ((u32)(shift) << 10) | ((u32)(width) << 6) | \
	 ((u32)(byte_index) << 3) | (u32)(source_shift))
#define ET_ADDR(p)	(((p) & BIT(27)) ? \
			 (0x00c00000u | (((p) >> 15) & 0xfffu)) : \
			 (0x0030a000u | (((p) >> 15) & 0xfffu)))
#define ET_SHIFT(p)	((u32)(((p) >> 10) & 0x1fu))
#define ET_WIDTH(p)	((u32)(((p) >> 6) & 0xfu))
#define ET_BYTE(p)	((u32)(((p) >> 3) & 7u))
#define ET_SSHIFT(p)	((u32)((p) & 7u))

extern int ipl_quiet;

void delay(u32 outer);
int wait_value(u32 addr, u32 mask, u32 value);
void set_field(u32 addr, u8 shift, u8 width, u32 value);
void uart_puts_at(u32 base, const char *text);
void ipl_print_gxid(u32 uart);
void ipl_crumb(char ch);
void pll_program(u32 addr, u32 value);
void apply_patches(const u32 *patches, u32 count);
#ifndef SOC_UNIVERSAL
/* Decode an RLE DDR register stream into DDR_BASE+(reg<<2). */
void ddr_apply_rle(const u8 *p, u32 reg_base, u32 count);
#endif
int efuse_read(u32 address, u8 *value);
/* Shared reader parameterized by register window (phys pre-MMU / virt post). */
int efuse_read_at(u32 cmd_reg, u32 status_reg, u32 address, u8 *value);

#if defined(SOC_GX6702) || defined(SOC_UNIVERSAL)
int gx6702_clocks_init(void);
int gx6702_ddr_init(void);
#endif
#if defined(SOC_GX6706) || defined(SOC_UNIVERSAL)
int gx6706_clocks_init(void);
int gx6706_ddr_init(void);
#endif
int ipl_pre_mmu(void);
void ipl_post_mmu(void);

#endif
