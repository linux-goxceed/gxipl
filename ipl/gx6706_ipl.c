/* SPDX-License-Identifier: MIT */
/*
 * Open GX6706 H5/S5 first-stage loader.
 *
 * The tables and register operations below are a clean-room transcription of
 * the byte-identical Cygnus 6706H5/6706S5 stage-1 behaviour.  No vendor
 * instructions or executable fragments are included.  Generic UART loading,
 * GXBC handling and post-MMU handoff are provided by the shared stage-1 core.
 */

#include "gx_hw.h"
#include "ipl_config_api.h"
#include "ipl_internal.h"

#ifdef SOC_UNIVERSAL
extern const u32 ddr_regs_0[155];
#else
static const u32 gx6706_ddr_regs_0[155] = {
	0x00000400u, 0x00000000u, 0x000208d5u, 0x00000085u, 0x0000014du, 0x02081202u,
	0x1e270702u, 0x05050a05u, 0x00b64b08u, 0x00000505u, 0x0a0a0101u, 0x0000c814u,
	0x010b1e03u, 0x0000000au, 0x00570100u, 0x00000a28u, 0x00120004u, 0x000a0005u,
	0x005e00c8u, 0x00010000u, 0x00030300u, 0x00000000u, 0x00000000u, 0x00000000u,
	0x00000000u, 0x00000000u, 0x00000000u, 0x00020300u, 0x00000000u, 0x00000000u,
	0x00040203u, 0x00000000u, 0x00000000u, 0x00010100u, 0x00000000u, 0x00000000u,
	0x00000000u, 0x00000000u, 0x01000200u, 0x02000040u, 0x00010040u, 0xff0a0103u,
	0x010101ffu, 0x01000101u, 0x010c0100u, 0x01000000u, 0x00000000u, 0x00000000u,
	0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
	0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
	0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01010000u, 0x00000202u,
	0x02020302u, 0x02000101u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
	0x00002819u, 0x00000000u, 0x00004e00u, 0x00000008u, 0x00000000u, 0x00000000u,
	0x00000000u, 0x00000000u, 0x00000000u, 0x00003030u, 0x00000000u, 0x00000000u,
	0x00030300u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000303u, 0x00000000u,
	0x00000000u, 0x00030300u, 0x0f0f0000u, 0x0f0f0f0fu, 0x0f0f0f0fu, 0xffff0f0fu,
	0x00000808u, 0x0808ffffu, 0x08ffff00u, 0xffff0008u, 0x00000808u, 0x0808ffffu,
	0x08ffff00u, 0xffff0008u, 0x00000808u, 0x0808ffffu, 0x08ffff00u, 0xffff0008u,
	0x00000808u, 0x0808ffffu, 0x08ffff00u, 0xffff0008u, 0x00000808u, 0x0808ffffu,
	0x08ffff00u, 0xffff0008u, 0x00000808u, 0x00010c0fu, 0x0c00010cu, 0x010c0001u,
	0x00010c00u, 0x0c00010cu, 0x010c0001u, 0x00010c00u, 0x0c00010cu, 0x010c0001u,
	0x00010c00u, 0x0c00010cu, 0x010c0001u, 0x00000000u, 0x000503e8u, 0x00000700u,
	0x00145000u, 0x02000200u, 0x02000200u, 0x00001450u, 0x00006590u, 0x01020809u,
	0x03800004u, 0x00040703u, 0x0000000au, 0x00000000u, 0x00000000u, 0x0010ffffu,
	0x13070303u, 0x0000000fu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
	0x00000000u, 0x00000204u, 0x00000000u, 0x00000000u, 0x00000001u,
};
#endif

#if defined(SOC_GX6706) || defined(SOC_UNIVERSAL)
static const u32 gx6706_ddr_regs_100[31] = {
	0x2627260cu, 0x263a260au, 0x212900a0u, 0x00000048u, 0x4303a003u, 0x00000000u,
	0x26272627u, 0x263a263au, 0x012100a0u, 0x00000048u, 0x6b036b03u, 0x00000000u,
	0x26272627u, 0x263a263au, 0x012100a0u, 0x0000006du, 0x6b036b03u, 0x00000000u,
	0x26272627u, 0x263a263au, 0x012100a0u, 0x0000006du, 0x6b036b03u, 0x00000000u,
	0x00004105u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
	0x00000000u,
};

static const struct field_patch gx6706_ddr_fields[] = {
	{ 0x4a, 0, 1, 1 }, { 0x4d, 24, 1, 1 }, { 0x95, 8, 1, 1 },
	{ 0x5f, 16, 16, 0xffff }, { 0x61, 0, 16, 0xffff },
	{ 0x62, 8, 16, 0xffff }, { 0x63, 16, 16, 0xffff },
	{ 0x65, 0, 16, 0xffff }, { 0x66, 8, 16, 0xffff },
	{ 0x67, 16, 16, 0xffff }, { 0x69, 0, 16, 0xffff },
	{ 0x6a, 8, 16, 0xffff }, { 0x6b, 16, 16, 0xffff },
	{ 0x6d, 0, 16, 0xffff }, { 0x6e, 8, 16, 0xffff },
	{ 0x6f, 16, 16, 0xffff }, { 0x71, 0, 16, 0xffff },
	{ 0x72, 8, 16, 0xffff }, { 0x73, 16, 16, 0xffff },
	{ 0x60, 0, 4, 6 }, { 0x61, 16, 4, 8 }, { 0x62, 24, 4, 10 },
	{ 0x64, 0, 4, 2 }, { 0x65, 16, 4, 8 }, { 0x66, 24, 4, 1 },
	{ 0x68, 0, 4, 3 }, { 0x69, 16, 4, 8 }, { 0x6a, 24, 4, 3 },
	{ 0x6c, 0, 4, 5 }, { 0x6d, 16, 4, 2 }, { 0x6e, 24, 4, 9 },
	{ 0x70, 0, 4, 8 }, { 0x71, 16, 4, 8 }, { 0x72, 24, 4, 5 },
	{ 0x74, 0, 4, 8 }, { 0x60, 8, 4, 6 }, { 0x61, 24, 4, 8 },
	{ 0x63, 0, 4, 10 }, { 0x64, 8, 4, 2 }, { 0x65, 24, 4, 8 },
	{ 0x67, 0, 4, 1 }, { 0x68, 8, 4, 3 }, { 0x69, 24, 4, 8 },
	{ 0x6b, 0, 4, 3 }, { 0x6c, 8, 4, 5 }, { 0x6d, 24, 4, 2 },
	{ 0x6f, 0, 4, 9 }, { 0x70, 8, 4, 8 }, { 0x71, 24, 4, 8 },
	{ 0x73, 0, 4, 5 }, { 0x74, 8, 4, 8 }, { 0x2b, 16, 1, 0 },
};

static const struct efuse_tweak gx6706_efuse_tweaks[] = {
	{ 0x0030a120u, 3, 3, 0, 5 }, { 0x0030a124u, 11, 3, 0, 5 },
	{ 0x0030a124u, 0, 3, 0, 5 }, { 0x0030a12cu, 16, 3, 0, 5 },
	{ 0x0030a12cu, 20, 3, 0, 5 }, { 0x0030a12cu, 24, 3, 0, 5 },
	{ 0x0030a12cu, 28, 3, 0, 5 }, { 0x0030a120u, 16, 3, 0, 5 },
	{ 0x0030a120u, 19, 3, 0, 5 }, { 0x0030a210u, 0, 4, 1, 0 },
	{ 0x0030a210u, 8, 4, 1, 0 }, { 0x0030a210u, 16, 4, 1, 0 },
	{ 0x0030a210u, 24, 4, 1, 0 }, { 0x0030a124u, 3, 4, 1, 0 },
	{ 0x0030a124u, 14, 4, 1, 0 }, { 0x0030a128u, 3, 4, 1, 0 },
	{ 0x0030a210u, 4, 4, 1, 4 }, { 0x0030a210u, 12, 4, 1, 4 },
	{ 0x0030a210u, 20, 4, 1, 4 }, { 0x0030a210u, 28, 4, 1, 4 },
	{ 0x0030a124u, 7, 4, 1, 4 }, { 0x0030a124u, 18, 4, 1, 4 },
	{ 0x0030a128u, 7, 4, 1, 4 }, { 0x00c00070u, 1, 1, 2, 0 },
	{ 0x00c00070u, 2, 1, 2, 1 }, { 0x00c00070u, 6, 1, 2, 2 },
	{ 0x00c00070u, 9, 1, 2, 3 }, { 0x00c00464u, 16, 3, 2, 4 },
	{ 0x00c00464u, 8, 3, 2, 4 }, { 0x00c00464u, 0, 3, 2, 4 },
	{ 0x00c00144u, 0, 8, 3, 0 }, { 0x00c00410u, 8, 8, 4, 0 },
	{ 0x00c00468u, 0, 3, 5, 0 }, { 0x00c00468u, 4, 3, 5, 0 },
	{ 0x00c00468u, 8, 3, 5, 0 }, { 0x00c00468u, 12, 3, 5, 0 },
	{ 0x00c00468u, 16, 3, 5, 0 }, { 0x00c00468u, 20, 3, 5, 0 },
	{ 0x00c00468u, 24, 3, 5, 0 }, { 0x00c00468u, 28, 3, 5, 0 },
	{ 0x00c0046cu, 0, 3, 5, 3 }, { 0x00c0046cu, 4, 3, 5, 3 },
	{ 0x00c0046cu, 8, 3, 5, 3 }, { 0x00c0046cu, 12, 3, 5, 3 },
	{ 0x00c0046cu, 16, 3, 5, 3 }, { 0x00c0046cu, 20, 3, 5, 3 },
	{ 0x00c0046cu, 24, 3, 5, 3 }, { 0x00c0046cu, 28, 3, 5, 3 },
	{ 0x00c00460u, 9, 1, 6, 0 }, { 0x00c00408u, 19, 3, 6, 1 },
};

static int gx6706_uart_init(u32 clock_hz)
{
	u32 divisor = (clock_hz + 921600u) / 1843200u;
	u32 reg = readl(SYS_BASE + 0x480u) | BIT(0) | BIT(1);

	writel(reg, SYS_BASE + 0x480u);
	writel(0, UART_PHYS + 0x04u);
	writel(3, UART_PHYS + 0x10u);
	if (wait_value(UART_PHYS + 0x7cu, BIT(0), 0))
		return -1;
	writel(0x80, UART_PHYS + 0x0cu);
	writel(divisor & 0xffu, UART_PHYS);
	writel((divisor >> 8) & 0xffu, UART_PHYS + 0x04u);
	if (wait_value(UART_PHYS + 0x7cu, BIT(0), 0))
		return -1;
	writel(3, UART_PHYS + 0x0cu);
	writel(1, UART_PHYS + 0x08u);
	writel(readl(UART_PHYS + 0x7cu), UART_PHYS + 0xc0u);
	return 0;
}

static void gx6706_clock_route(u8 index, u32 value, u32 gate_offset,
			       u32 gate_mask)
{
	u32 route = (0x001803ffu + index) << 2;
	u32 gate = SYS_BASE + gate_offset;

	writel(value | BIT(30), route);
	writel(value | BIT(30) | BIT(31), route);
	writel(readl(gate) | gate_mask, gate);
}

int gx6706_clocks_init(void)
{
	u32 reg;

	writel(0, SYS_BASE + 0x170u);
	writel(0, SYS_BASE + 0x174u);
	if (gx6706_uart_init(24000000u))
		return -1;
	ipl_crumb('R');
	pll_program(SYS_BASE + 0x0c0u, 0x00211063u);
	pll_program(SYS_BASE + 0x0c4u, 0x00112038u);
	pll_program(SYS_BASE + 0x0c8u, 0x00112038u);
	if (wait_value(SYS_BASE + 0x0e8u, 7, 7))
		return -1;

	reg = readl(SYS_BASE + 0x024u);
	writel((reg & ~0x00000f00u) | 0x00000200u, SYS_BASE + 0x024u);
	reg = readl(SYS_BASE + 0x178u);
	writel((reg & ~0x0000003fu) | 0x00000003u, SYS_BASE + 0x178u);
	/* Descriptor gate selector 1 means SYS+0x170; 2 means SYS+0x174. */
	gx6706_clock_route(10, 0x01745d17u, 0x174u, 0x00020000u);
	gx6706_clock_route(11, 0x01999999u, 0x174u, 0x00040000u);
	gx6706_clock_route(15, 0x08000000u, 0x170u, 0x00000004u);
	gx6706_clock_route(19, 0x10000000u, 0x174u, 0x00008000u);
	writel(readl(SYS_BASE + 0x174u) | BIT(20) | BIT(21), SYS_BASE + 0x174u);

	reg = readl(SYS_BASE);
	writel(reg | 0x0030000du, SYS_BASE);
	delay(1);
	writel(readl(SYS_BASE) & 0xffcffff2u, SYS_BASE);
	if (gx6706_uart_init(29491200u))
		return -1;
	ipl_crumb('U');
	return 0;
}

static void gx6706_ddr_crg_init(void)
{
	writel(0, SYS_BASE + 0x120u);
	writel(0, SYS_BASE + 0x124u);
	writel(0, SYS_BASE + 0x128u);
	writel(0, SYS_BASE + 0x12cu);
	writel(0, SYS_BASE + 0x210u);
	writel(0, SYS_BASE + 0x214u);
	writel(0, SYS_BASE + 0x218u);
	writel(readl(SYS_BASE + 0x120u) | 0x36f601b0u, SYS_BASE + 0x120u);
	writel(readl(SYS_BASE + 0x124u) | 0x00003006u, SYS_BASE + 0x124u);
	writel(readl(SYS_BASE + 0x128u) | BIT(11) | BIT(12), SYS_BASE + 0x128u);
	writel(readl(SYS_BASE + 0x12cu) | 0x66660000u, SYS_BASE + 0x12cu);
	writel(readl(SYS_BASE + 0x214u) | 0x1b6db6dbu, SYS_BASE + 0x214u);
	writel(readl(SYS_BASE + 0x218u) | 0x00db36dbu, SYS_BASE + 0x218u);
}

static void gx6706_apply_efuse(void)
{
	u8 bytes[7];
	u32 i;

	for (i = 0; i < ARRAY_SIZE(bytes); i++) {
		if (efuse_read(0x159u + i, &bytes[i]))
			return;
	}
	if ((bytes[0] & 1u) && !(bytes[0] & 0x1eu)) {
		for (i = 0; i < ARRAY_SIZE(gx6706_efuse_tweaks); i++) {
			const struct efuse_tweak *t = &gx6706_efuse_tweaks[i];
			u32 value = bytes[t->byte_index] >> t->source_shift;

			set_field(t->addr, t->shift, t->width, value);
		}
		/* The vendor applies this signed trim only for valid calibration. */
		set_field(SYS_BASE + 0x128u, 16, 4,
			  (bytes[2] & 0x80u) ? 10u : 0u);
	}
}

int gx6706_ddr_init(void)
{
	u8 variant = 0;
	u32 geometry;
	u32 i;
	u32 status;

	(void)efuse_read(0x138u, &variant);
	variant &= 0x0fu;
	writel(readl(SYS_BASE + 0x174u) | BIT(21), SYS_BASE + 0x174u);
	writel(readl(SYS_BASE + 0x068u) & ~(BIT(2) | BIT(3)), SYS_BASE + 0x068u);
	writel(0xffffffffu, SYS_BASE + 0x580u);
	writel(readl(SYS_BASE) | BIT(0), SYS_BASE);
	writel(readl(SYS_BASE) & ~BIT(0), SYS_BASE);
	gx6706_ddr_crg_init();

#ifdef SOC_UNIVERSAL
	geometry = (((ddr_regs_0[5] >> 16) & 0x1fu) - 4u) >> 1;
#else
	geometry = (((gx6706_ddr_regs_0[5] >> 16) & 0x1fu) - 4u) >> 1;
#endif
	writel(readl(SYS_BASE + 0x120u) | BIT(0) | BIT(31), SYS_BASE + 0x120u);
	writel(readl(SYS_BASE + 0x124u) | 0x00220440u |
	       (variant == 1u ? BIT(29) : 0), SYS_BASE + 0x124u);
	writel(readl(SYS_BASE + 0x128u) | BIT(6) | BIT(10), SYS_BASE + 0x128u);
	writel(readl(SYS_BASE + 0x12cu) | geometry, SYS_BASE + 0x12cu);
	writel(readl(SYS_BASE + 0x210u) | 0x88888888u, SYS_BASE + 0x210u);

#ifdef SOC_UNIVERSAL
	for (i = 0; i < 155u; i++)
		writel(ddr_regs_0[i], DDR_BASE + (i << 2));
	writel(0x00000000u, DDR_BASE + (28u << 2));
	writel(0x00004e00u, DDR_BASE + (74u << 2));
	writel(0x00003030u, DDR_BASE + (81u << 2));
#else
	for (i = 0; i < ARRAY_SIZE(gx6706_ddr_regs_0); i++)
		writel(gx6706_ddr_regs_0[i], DDR_BASE + (i << 2));
#endif
	for (i = 0; i < ARRAY_SIZE(gx6706_ddr_regs_100); i++)
		writel(gx6706_ddr_regs_100[i], DDR_BASE + ((0x100u + i) << 2));
	if (variant == 3u) {
		writel(0x00002828u, DDR_BASE + 0x144u);
		writel(0x43039e03u, DDR_BASE + 0x410u);
		writel(0x00004305u, DDR_BASE + 0x460u);
	}
	apply_patches(gx6706_ddr_fields, ARRAY_SIZE(gx6706_ddr_fields));
	gx6706_apply_efuse();
	/* A dead/gated controller reads as all ones; never accept that as ready. */
	if (readl(DDR_BASE + 0x008u) == 0xffffffffu)
		return -1;
	set_field(DDR_BASE, 0, 1, 1);
	for (i = 0; i < WAIT_LIMIT; i++) {
		status = readl(DDR_BASE + 0x0b8u);
		if (status != 0xffffffffu && (status & BIT(5)))
			break;
	}
	if (i == WAIT_LIMIT)
		return -1;
	ipl_crumb('N');
	return 0;
}

#endif

#ifndef SOC_UNIVERSAL
__attribute__((used, externally_visible))
int ipl_pre_mmu(void)
{
	ipl_quiet = ipl_config_verbose_early();
	ipl_crumb('I');
	if (gx6706_clocks_init()) {
		uart_puts_at(UART_PHYS, "EPLL\r\n");
		return -1;
	}
	if (gx6706_ddr_init()) {
		uart_puts_at(UART_PHYS, "EDDR\r\n");
		return -1;
	}
	return 0;
}
#endif
