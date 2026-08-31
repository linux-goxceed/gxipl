/* SPDX-License-Identifier: MIT */
/*
 * Open GX6702 first-stage loader.
 *
 * Register sequences and DDR values were clean-room recovered from the
 * Gemini 6702H5 vendor IPL.  The implementation, error handling and UART
 * loader are new code.  All addresses below are physical until the CK610 MMU
 * is enabled by start.S.
 */

#include "gx_hw.h"
#include "ipl_config_api.h"
#include "ipl_internal.h"

static const u32 ddr_regs_0[155] = {
	0x00000400u, 0x00000000u, 0x000208d5u, 0x00000085u, 0x0000014du, 0x02081202u,
	0x1e270702u, 0x05050a05u, 0x00b64b08u, 0x00000505u, 0x0a0a0101u, 0x0000c814u,
	0x010b1e03u, 0x0000000au, 0x00570100u, 0x00000a28u, 0x00120004u, 0x000a0005u,
	0x005e00c8u, 0x00010000u, 0x00030300u, 0x00000000u, 0x00000000u, 0x00000000u,
	0x00000000u, 0x00000000u, 0x00000000u, 0x00020300u, 0x00000002u, 0x00000000u,
	0x00040203u, 0x00000000u, 0x00000000u, 0x00010100u, 0x00000000u, 0x00000000u,
	0x00000000u, 0x00000000u, 0x01000200u, 0x02000040u, 0x00010040u, 0xff0a0103u,
	0x010101ffu, 0x01000101u, 0x010c0100u, 0x01000000u, 0x00000000u, 0x00000000u,
	0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
	0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
	0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01010000u, 0x00000202u,
	0x02020302u, 0x02000101u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
	0x00002819u, 0x00000000u, 0x00000800u, 0x00000008u, 0x00000000u, 0x00000000u,
	0x00000000u, 0x00000000u, 0x00000000u, 0x00002424u, 0x00000000u, 0x00000000u,
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

static const u32 ddr_regs_100[26] = {
	0x26272627u, 0x263a263au, 0x012100a0u, 0x00000048u, 0x43035603u, 0x00000000u,
	0x26272627u, 0x263a263au, 0x012100a0u, 0x00000048u, 0x6b036b03u, 0x00000000u,
	0x26272627u, 0x263a263au, 0x012100a0u, 0x0000006du, 0x6b036b03u, 0x00000000u,
	0x26272627u, 0x263a263au, 0x012100a0u, 0x0000006du, 0x6b036b03u, 0x00000000u,
	0x00004005u, 0x00000000u,
};

static const struct field_patch ddr_fields_0[] = {
	{ 0x4a,  0,  1, 0x0001 }, { 0x4d, 24,  1, 0x0001 },
	{ 0x95,  8,  1, 0x0001 }, { 0x5f, 16, 16, 0xffff },
	{ 0x61,  0, 16, 0xffff }, { 0x62,  8, 16, 0xffff },
	{ 0x63, 16, 16, 0xffff }, { 0x65,  0, 16, 0xffff },
	{ 0x66,  8, 16, 0xffff }, { 0x67, 16, 16, 0xffff },
	{ 0x69,  0, 16, 0xffff }, { 0x6a,  8, 16, 0xffff },
	{ 0x6b, 16, 16, 0xffff }, { 0x6d,  0, 16, 0xffff },
	{ 0x6e,  8, 16, 0xffff }, { 0x6f, 16, 16, 0xffff },
	{ 0x71,  0, 16, 0xffff }, { 0x72,  8, 16, 0xffff },
	{ 0x73, 16, 16, 0xffff }, { 0x60,  0,  4, 0x0005 },
	{ 0x61, 16,  4, 0x0006 }, { 0x62, 24,  4, 0x0009 },
	{ 0x64,  0,  4, 0x0008 }, { 0x65, 16,  4, 0x0008 },
	{ 0x66, 24,  4, 0x0004 }, { 0x68,  0,  4, 0x0003 },
	{ 0x69, 16,  4, 0x0008 }, { 0x6a, 24,  4, 0x0003 },
	{ 0x6c,  0,  4, 0x0005 }, { 0x6d, 16,  4, 0x0003 },
	{ 0x6e, 24,  4, 0x0008 }, { 0x70,  0,  4, 0x0008 },
	{ 0x71, 16,  4, 0x0008 }, { 0x72, 24,  4, 0x0008 },
	{ 0x74,  0,  4, 0x0008 }, { 0x60,  8,  4, 0x0005 },
	{ 0x61, 24,  4, 0x0006 }, { 0x63,  0,  4, 0x0009 },
	{ 0x64,  8,  4, 0x0008 }, { 0x65, 24,  4, 0x0008 },
	{ 0x67,  0,  4, 0x0004 }, { 0x68,  8,  4, 0x0003 },
	{ 0x69, 24,  4, 0x0008 }, { 0x6b,  0,  4, 0x0003 },
	{ 0x6c,  8,  4, 0x0005 }, { 0x6d, 24,  4, 0x0003 },
	{ 0x6f,  0,  4, 0x0008 }, { 0x70,  8,  4, 0x0008 },
	{ 0x71, 24,  4, 0x0008 }, { 0x73,  0,  4, 0x0008 },
	{ 0x74,  8,  4, 0x0008 }, { 0x2b, 16,  1, 0x0000 },
};

static const struct field_patch ddr_fields_1[] = {
	{ 0x64,  0, 4, 2 }, { 0x66, 24, 4, 1 },
	{ 0x72, 24, 4, 5 }, { 0x6d, 16, 4, 2 },
	{ 0x64,  8, 4, 2 }, { 0x67,  0, 4, 1 },
	{ 0x73,  0, 4, 5 }, { 0x6d, 24, 4, 2 },
};

static const struct efuse_tweak efuse_tweaks[] = {
	{ 0x0030a120u,  3, 3, 0, 5 }, { 0x0030a124u, 11, 3, 0, 5 },
	{ 0x0030a124u,  0, 3, 0, 5 }, { 0x0030a120u,  6, 3, 0, 5 },
	{ 0x0030a120u, 24, 3, 0, 5 }, { 0x0030a120u, 16, 4, 1, 0 },
	{ 0x0030a124u,  3, 4, 1, 0 }, { 0x0030a124u, 14, 4, 1, 0 },
	{ 0x0030a128u,  3, 4, 1, 0 }, { 0x0030a120u, 20, 4, 1, 4 },
	{ 0x0030a124u,  7, 4, 1, 4 }, { 0x0030a124u, 18, 4, 1, 4 },
	{ 0x0030a128u,  7, 4, 1, 4 }, { 0x00c00070u,  1, 1, 2, 0 },
	{ 0x00c00070u,  2, 1, 2, 1 }, { 0x00c00070u,  6, 1, 2, 2 },
	{ 0x00c00070u,  9, 1, 2, 3 }, { 0x00c00464u, 16, 3, 2, 4 },
	{ 0x00c00464u,  8, 3, 2, 4 }, { 0x00c00464u,  0, 3, 2, 4 },
	{ 0x00c00144u,  0, 8, 3, 0 }, { 0x00c00410u,  8, 8, 4, 0 },
	{ 0x00c00468u,  0, 3, 5, 0 }, { 0x00c0046cu,  0, 3, 5, 3 },
	{ 0x0030a208u,  0, 2, 5, 6 }, { 0x0030a208u,  2, 1, 6, 0 },
	{ 0x00c00408u, 19, 3, 6, 1 }, { 0x0030a128u, 11, 3, 6, 4 },
	{ 0x0030a128u,  0, 3, 6, 4 },
};

static int uart_init_physical(u32 clock_hz)
{
	u32 divisor = (clock_hz + 921600u) / 1843200u;
	u32 reg;

	reg = readl(SYS_BASE + 0x150u) | BIT(22) | BIT(23);
	writel(reg, SYS_BASE + 0x150u);
	writel(0, UART_PHYS + 0x04u);
	writel(3, UART_PHYS + 0x10u);
	if (wait_value(UART_PHYS + 0x7cu, BIT(0), 0))
		return -1;
	writel(0x80, UART_PHYS + 0x0cu);
	writel(divisor & 0xffu, UART_PHYS + 0x00u);
	writel((divisor >> 8) & 0xffu, UART_PHYS + 0x04u);
	if (wait_value(UART_PHYS + 0x7cu, BIT(0), 0))
		return -1;
	writel(3, UART_PHYS + 0x0cu);
	writel(1, UART_PHYS + 0x08u);
	writel(readl(UART_PHYS + 0x7cu), UART_PHYS + 0xc0u);
	return 0;
}

static void clock_route(u8 index, u32 value, u32 gate_mask)
{
	/* The route table is a separate block at 0x00601000, not SYS_BASE. */
	u32 addr = (0x001803ffu + index) << 2;

	writel(value | BIT(30), addr);
	writel(value | BIT(30) | BIT(31), addr);
	writel(readl(SYS_BASE + 0x174u) | gate_mask, SYS_BASE + 0x174u);
}

struct peripheral_route {
	u8 index;
	u32 value;
	u32 gate_mask;
};

/*
 * Routes installed by the working USB GxLoader before it hands control to a
 * payload.  The vendor IPL exposed SRAM helpers for this late setup; the
 * open IPL replaces that SRAM, so install the state while clock changes are
 * still safe.
 */
static const struct peripheral_route peripheral_routes[] = {
	/* Stock flash BOOT uses 0x05555555 for route 1 (golden a0601000). */
	{  1, 0x05555555u, 0x00000001u },
	{  2, 0x05555555u, 0x00000002u },
	{  3, 0x10000000u, 0x00000200u },
	{  4, 0x08000000u, 0x00000400u },
	{  5, 0x08000000u, 0x00000800u },
	{  6, 0x0cccccccu, 0x00001000u },
	{  7, 0x09245fd9u, 0x00002000u },
	{  8, 0x09245fd9u, 0x00004000u },
	{  9, 0x05d1745du, 0x00008000u },
	{ 15, 0x09245fd9u, 0x00000004u },
	{ 16, 0x10000000u, 0x00000001u },
};

static void peripheral_clocks_init(void)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(peripheral_routes); i++)
		clock_route(peripheral_routes[i].index,
			    peripheral_routes[i].value,
			    peripheral_routes[i].gate_mask);

	/* Exact live state from stock golden-reg-dumps.txt (A030A). */
	writel(0xca8ca247u, SYS_BASE + 0x024u);
	writel(0xffefffffu, SYS_BASE + 0x170u);
	writel(0x1fff01c9u, SYS_BASE + 0x174u);
	writel(0xd1c8cb07u, SYS_BASE + 0x178u);
	writel(0xc3ebc27fu, SYS_BASE + 0x17cu);
}

static int clocks_init(void)
{
	u32 reg;

	writel(0, SYS_BASE + 0x170u);
	writel(0, SYS_BASE + 0x174u);
	pll_program(SYS_BASE + 0x0c0u, 0x00211063u);
	pll_program(SYS_BASE + 0x0c4u, 0x0011101cu);
	pll_program(SYS_BASE + 0x0c8u, 0x00111033u);
	if (wait_value(SYS_BASE + 0x0e8u, 0xffffffffu, 7))
		return -1;

	if (uart_init_physical(24000000u))
		return -1;
	ipl_crumb('R');

	reg = readl(SYS_BASE + 0x024u);
	writel((reg & ~0x00000f00u) | 0x00000200u,
	       SYS_BASE + 0x024u);
	reg = readl(SYS_BASE + 0x178u);
	writel((reg & ~0x0000003fu) | 0x00000007u,
	       SYS_BASE + 0x178u);
	clock_route(10, 0x01745d17u, 0x00020000u);
	clock_route(11, 0x01999999u, 0x00040000u);
	clock_route(12, 0x20000000u, 0x00100000u);
	clock_route(13, 0x0aaaaaaau, 0x00080000u);
	clock_route(14, 0x09245fd9u, 0x00000008u);
	writel(readl(SYS_BASE + 0x174u) | BIT(21), SYS_BASE + 0x174u);
	peripheral_clocks_init();

	reg = readl(SYS_BASE);
	writel(reg | 0x0030000du, SYS_BASE);
	delay(1);
	reg = readl(SYS_BASE);
	writel(reg & 0xffcffff2u, SYS_BASE);

	if (uart_init_physical(29491200u))
		return -1;
	ipl_crumb('U');
	return 0;
}

static void ddr_apply_efuse(void)
{
	u8 bytes[7];
	u32 i;
	u32 address = 0x154u;
	u32 value;

	for (i = 0; i < ARRAY_SIZE(bytes); i++) {
		if (efuse_read(address, &bytes[i]))
			return;
		address++;
		if (address == 0x158u)
			address = 0x15du;
	}

	if ((bytes[0] & 1u) && !(bytes[0] & 0x1eu)) {
		for (i = 0; i < ARRAY_SIZE(efuse_tweaks); i++) {
			const struct efuse_tweak *t = &efuse_tweaks[i];

			value = bytes[t->byte_index] >> t->source_shift;
			set_field(t->addr, t->shift, t->width, value);
		}
	}

	/* The vendor code uses 0xa for a negative signed trim, zero otherwise. */
	set_field(SYS_BASE + 0x128u, 16, 4,
		  (bytes[2] & 0x80u) ? 10u : 0u);
}

static int ddr_init(void)
{
	u32 i;
	u32 geometry;

	writel(readl(SYS_BASE + 0x174u) | BIT(21), SYS_BASE + 0x174u);
	writel(readl(SYS_BASE + 0x068u) & ~(BIT(2) | BIT(3)),
	       SYS_BASE + 0x068u);
	writel(0xffffffffu, SYS_BASE + 0x580u);
	writel(readl(SYS_BASE) | BIT(0), SYS_BASE);
	writel(readl(SYS_BASE) & ~BIT(0), SYS_BASE);

	writel(0x060001b0u, SYS_BASE + 0x120u);
	writel(0x00003006u, SYS_BASE + 0x124u);
	writel(0x00001803u, SYS_BASE + 0x128u);
	writel(0, SYS_BASE + 0x12cu);
	writel(readl(SYS_BASE + 0x120u) | 0x80220001u,
	       SYS_BASE + 0x120u);
	writel(readl(SYS_BASE + 0x124u) | 0x00088110u,
	       SYS_BASE + 0x124u);
	writel(readl(SYS_BASE + 0x128u) | BIT(4) | BIT(8),
	       SYS_BASE + 0x128u);
	geometry = (((ddr_regs_0[5] >> 16) & 0x1fu) - 4u) >> 1;
	writel(readl(SYS_BASE + 0x12cu) | geometry, SYS_BASE + 0x12cu);

	for (i = 0; i < ARRAY_SIZE(ddr_regs_0); i++)
		writel(ddr_regs_0[i], DDR_BASE + (i << 2));
	for (i = 0; i < ARRAY_SIZE(ddr_regs_100); i++)
		writel(ddr_regs_100[i], DDR_BASE + ((0x100u + i) << 2));

	apply_patches(ddr_fields_0, ARRAY_SIZE(ddr_fields_0));
	apply_patches(ddr_fields_1, ARRAY_SIZE(ddr_fields_1));
	ddr_apply_efuse();
	set_field(DDR_BASE, 0, 1, 1);
	if (wait_value(DDR_BASE + 0x0b8u, BIT(5), BIT(5)))
		return -1;

	ipl_crumb('N');
	return 0;
}

int ipl_pre_mmu(void)
{
	/* Config lives in the already-mapped IPL image; peek before crumbs. */
	ipl_quiet = ipl_config_verbose_early();
	ipl_crumb('I');

	if (clocks_init()) {
		uart_puts_at(UART_PHYS, "EPLL\r\n");
		return -1;
	}
	if (ddr_init()) {
		uart_puts_at(UART_PHYS, "EDDR\r\n");
		return -1;
	}
	return 0;
}
