/* SPDX-License-Identifier: MIT */
/*
 * Shared eFuse byte reader.  One implementation parameterized by the
 * command/status register pair so the pre-MMU physical window and the
 * post-MMU uncached alias do not each carry a private copy.
 */

#include "gx_hw.h"
#include "ipl_internal.h"

/*
 * noclone keeps LTO from emitting one specialized copy per constant register
 * base (which would undo the sharing on GX6706); inlining is still allowed so
 * the single-caller GX6702 path can fold it into ipl_pre_mmu as before.
 */
__attribute__((noclone))
int efuse_read_at(u32 cmd_reg, u32 status_reg, u32 address, u8 *value)
{
	u32 count;
	u32 cmd;
	volatile u32 settle;

	for (count = 0; count < WAIT_LIMIT; count++) {
		if (readl(status_reg) & BIT(10))
			break;
	}
	if (count == WAIT_LIMIT)
		return -1;
	for (count = 0; count < WAIT_LIMIT; count++) {
		if (!(readl(status_reg) & BIT(8)))
			break;
	}
	if (count == WAIT_LIMIT)
		return -1;

	cmd = ((address & 0x7ffu) << 3) | BIT(14);
	writel(cmd, cmd_reg);
	for (settle = 0; settle < 0x1000u; settle++)
		;
	writel(cmd & ~BIT(14), cmd_reg);
	for (count = 0; count < WAIT_LIMIT; count++) {
		if (readl(status_reg) & BIT(9)) {
			*value = (u8)readl(status_reg);
			writel(0, cmd_reg);
			return 0;
		}
	}
	writel(0, cmd_reg);
	return -1;
}

int efuse_read(u32 address, u8 *value)
{
	return efuse_read_at(EFUSE_CMD, EFUSE_STATUS, address, value);
}
