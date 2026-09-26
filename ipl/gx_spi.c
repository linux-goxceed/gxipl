/* SPDX-License-Identifier: MIT */
/*
 * Minimal gxflash SPI NOR reader for the open IPL / bootcode.
 * Sequence matches u-boot/drivers/spi/gx6702_spi.c / BootROM @ 0x1af4.
 */

#include "gx_hw.h"
#include "ipl_internal.h"

#if defined(SOC_GX6706)

static int dw_wait(u32 mask, int set)
{
	u32 i;

	for (i = 0; i < DWSPI_TIMEOUT; i++) {
		u32 value = readl(DWSPI_VIRT + DWSPI_SR);

		if (!!(value & mask) == !!set)
			return 0;
	}
	return -1;
}

static void dw_flush_rx(void)
{
	while (readl(DWSPI_VIRT + DWSPI_SR) & DWSPI_SR_RFNE)
		(void)readl(DWSPI_VIRT + DWSPI_DR);
}

static void dw_disable(void)
{
	writel(0, DWSPI_VIRT + DWSPI_SSIENR);
}

static void dw_abort(void)
{
	dw_disable();
}

/*
 * GxLoader does not rely on SSIENR/SER alone to hold chip select between
 * transfers.  It asserts this version-specific external latch for the whole
 * SPI message, allowing SSI to be disabled while switching from TX-only to
 * RX-only mode without releasing the flash.
 */
static u32 dw_cs_control(void)
{
	if (readl(DWSPI_VIRT + DWSPI_VERSION) == DWSPI_VERSION_102A)
		return DWSPI_CS_LEGACY_VIRT;
	return DWSPI_CS_NEW_VIRT;
}

static void dw_chip_select(int assert)
{
	writel(assert ? 2u : 3u, dw_cs_control());
}

/* Post-MMU reads go through the uncached MMIO alias of the eFuse window. */
static int gx6706_efuse_read(u32 address, u8 *value)
{
	return efuse_read_at(EFUSE_CMD_VIRT, EFUSE_STATUS_VIRT, address, value);
}

/*
 * GxLoader selects two pad-control bytes from eFuse.  Zero calibration bytes
 * use silicon-version defaults; the available variant-3 part reads d9/cb.
 */
static void gx6706_spi_pad_calibrate(void)
{
	u8 variant = 0;
	u8 pad_hi;
	u8 pad;
	u8 fallback_hi;
	u8 fallback;
	u32 value;

	(void)gx6706_efuse_read(0x138u, &variant);
	if ((variant & 0x0fu) == 3u) {
		fallback_hi = 0xdau;
		fallback = 0xcbu;
	} else {
		fallback_hi = 0x24u;
		fallback = 0x7bu;
	}
	if (gx6706_efuse_read(0x126u, &pad_hi) || !pad_hi)
		pad_hi = fallback_hi;
	if (gx6706_efuse_read(0x12au, &pad) || !pad)
		pad = fallback;

	value = readl(DWSPI_WRAP_VIRT + DWSPI_WRAP_PAD_HI);
	writel((value & ~0xffu) | pad_hi,
	       DWSPI_WRAP_VIRT + DWSPI_WRAP_PAD_HI);
	value = readl(DWSPI_WRAP_VIRT + DWSPI_WRAP_PAD);
	writel((value & 0xffffff0fu) | (pad & 0xf0u),
	       DWSPI_WRAP_VIRT + DWSPI_WRAP_PAD);
}

/*
 * Cygnus IO-domain routing performed immediately before the SPI route/pad
 * setup in GxLoader.  The two clock selector globals used by the vendor image
 * are both six for the generic 672 MHz H5/S5 configuration.
 */
static void gx6706_spi_io_init(void)
{
	u32 value;

	value = readl(0xa4809000u) & 0xc1ffffffu;
	writel(value | (6u << 25), 0xa4809000u);
	value = readl(0xa4808000u) & 0xc1ffffffu;
	writel(value | (6u << 25), 0xa4808000u);

	value = readl(0xa4808070u) & 0xff00ffffu;
	writel(value | 0x00590000u, 0xa4808070u);
	writel(1u, 0xa4800138u);
	writel(0u, 0xa4800140u);
	writel(0u, 0xa4800144u);
	writel(0x00010000u, 0xa4800148u);
	writel(0u, 0xa480014cu);
	writel(readl(0xa4804164u) & ~0xffu, 0xa4804164u);
	writel(readl(0xa4804168u) & ~0xffu, 0xa4804168u);
	writel(0x000a0320u, 0xa480900cu);
	writel(0x00163863u, 0xa48081b0u);
	writel(0x00163863u, 0xa48081b8u);
	writel(0x10000001u, 0xa4809110u);
	writel(0x10000001u, 0xa4809130u);
	writel(0x10000001u, 0xa4809150u);
}

static int dw_tx_header(u8 command, u32 address, u32 address_bytes)
{
	u32 header = 1u + address_bytes;
	u32 tx = 0;

	if (dw_wait(DWSPI_SR_BUSY, 0))
		return -1;
	dw_disable();
	/* GxLoader uses TMOD=TX-only for the opcode/address transfer. */
	writel(DWSPI_RX_SAMPLE_DLY_VENDOR,
	       DWSPI_VIRT + DWSPI_RX_SAMPLE_DLY);
	writel(DWSPI_CTRLR0_WRITE, DWSPI_VIRT + DWSPI_CTRLR0);
	writel(0, DWSPI_VIRT + DWSPI_CTRLR1);
	dw_flush_rx();
	(void)readl(DWSPI_VIRT + DWSPI_ICR);
	writel(0x202u, DWSPI_VIRT + DWSPI_SPI_CTRLR0);
	writel(1, DWSPI_VIRT + DWSPI_SER);
	writel(1, DWSPI_VIRT + DWSPI_SSIENR);

	while (tx < header) {
		u8 value;

		if (dw_wait(DWSPI_SR_TFNF, 1)) {
			dw_abort();
			return -2;
		}
		if (tx == 0) {
			value = command;
		} else {
			u32 shift = (address_bytes - tx) * 8u;
			value = (u8)(address >> shift);
		}
		writel(value, DWSPI_VIRT + DWSPI_DR);
		tx++;
	}

	if (dw_wait(DWSPI_SR_TFE, 1) || dw_wait(DWSPI_SR_BUSY, 0)) {
		dw_abort();
		return -3;
	}
	/* Retain SER while switching the following transfer to RX-only. */
	dw_disable();
	return 0;
}

static int dw_rx_data(u8 *output, u32 len)
{
	while (len) {
		u32 chunk = len > DWSPI_RX_CHUNK ? DWSPI_RX_CHUNK : len;
		u32 rx = 0;
		u32 idle = 0;

		if (dw_wait(DWSPI_SR_BUSY, 0))
			return -4;
		dw_disable();
		dw_flush_rx();
		/* Exact PIO receive setup used by GxLoader on the 1.02a core. */
		writel(DWSPI_RX_SAMPLE_DLY_VENDOR,
		       DWSPI_VIRT + DWSPI_RX_SAMPLE_DLY);
		writel(DWSPI_CTRLR0_READ, DWSPI_VIRT + DWSPI_CTRLR0);
		writel(chunk - 1u, DWSPI_VIRT + DWSPI_CTRLR1);
		writel(0, DWSPI_VIRT + DWSPI_RXFTLR);
		writel(0x202u, DWSPI_VIRT + DWSPI_SPI_CTRLR0);
		writel(1, DWSPI_VIRT + DWSPI_SER);
		writel(1, DWSPI_VIRT + DWSPI_SSIENR);
		/* One DR write starts the programmed RX-only receive count. */
		writel(0, DWSPI_VIRT + DWSPI_DR);

		while (rx < chunk) {
			if (readl(DWSPI_VIRT + DWSPI_SR) & DWSPI_SR_RFNE) {
				output[rx++] = (u8)readl(DWSPI_VIRT + DWSPI_DR);
				idle = 0;
			} else if (++idle == DWSPI_TIMEOUT) {
				dw_abort();
				return -5;
			}
		}
		if (dw_wait(DWSPI_SR_BUSY, 0)) {
			dw_abort();
			return -6;
		}
		dw_disable();
		output += chunk;
		len -= chunk;
	}
	return 0;
}

static int dw_transaction(u8 command, u32 address, u32 address_bytes,
			  u8 *output, u32 len)
{
	int ret;

	dw_chip_select(1);
	ret = dw_tx_header(command, address, address_bytes);
	if (!ret && len)
		ret = dw_rx_data(output, len);
	dw_chip_select(0);
	return ret;
}

void gx_spi_init(void)
{
	u32 route;
	u32 wrapper;
	u32 version;

	/* Exact clock/reset wrapper sequence recovered from the S5 GxLoader. */
	writel(0, DWSPI_WRAP_VIRT + DWSPI_WRAP_CTRL);
	writel(0xc0u, DWSPI_WRAP_VIRT + DWSPI_WRAP_CTRL);
	gx6706_spi_io_init();

	route = readl(SPI_ROUTE_REG_VIRT);
	route &= ~BIT(0);
	route |= BIT(1);
	route &= ~0x00000f70u;
	writel(route, SPI_ROUTE_REG_VIRT);
	writeb(5u, 0xa030a794u);
	gx6706_spi_pad_calibrate();

	wrapper = readl(DWSPI_WRAP_VIRT + DWSPI_WRAP_PAD);
	wrapper |= BIT(0);
	writel(wrapper, DWSPI_WRAP_VIRT + DWSPI_WRAP_PAD);
	wrapper = readl(DWSPI_WRAP_VIRT + DWSPI_WRAP_PAD);
	wrapper |= BIT(1);
	writel(wrapper, DWSPI_WRAP_VIRT + DWSPI_WRAP_PAD);

	/* Exact registration gate and idle external-CS value used by GxLoader. */
	writel(1, DWSPI_GATE_VIRT);
	version = readl(DWSPI_VIRT + DWSPI_VERSION);
	if (version == DWSPI_VERSION_102A)
		writel(3, DWSPI_CS_LEGACY_VIRT);
	else
		writel(3, DWSPI_CS_NEW_VIRT);

	/* Exact 8-bit read setup left by the stock NOR probe. */
	writel(0, DWSPI_VIRT + DWSPI_SSIENR);
	writel(0, DWSPI_VIRT + DWSPI_IMR);
	writel(DWSPI_CTRLR0_READ, DWSPI_VIRT + DWSPI_CTRLR0);
	writel(0, DWSPI_VIRT + DWSPI_CTRLR1);
	writel(DWSPI_BAUDR_VENDOR, DWSPI_VIRT + DWSPI_BAUDR);
	writel(DWSPI_RX_SAMPLE_DLY_VENDOR,
	       DWSPI_VIRT + DWSPI_RX_SAMPLE_DLY);
	/* Extended controller word written by GxLoader for normal SPI frames. */
	writel(0x202u, DWSPI_VIRT + DWSPI_SPI_CTRLR0);
	(void)readl(DWSPI_VIRT + DWSPI_ICR);
}

int gx_spi_read(u32 flash_off, void *dst, u32 len)
{
	return dw_transaction(0x03, flash_off, 3, (u8 *)dst, len);
}

int gx_spi_read_id(u8 id[3])
{
	int ret = dw_transaction(0x9f, 0, 0, id, 3);

	if (ret)
		return ret;
	/* A completed transaction with a stuck/floating MISO is still a failure. */
	if ((id[0] == 0x00u && id[1] == 0x00u && id[2] == 0x00u) ||
	    (id[0] == 0xffu && id[1] == 0xffu && id[2] == 0xffu))
		return -4;
	return 0;
}

u32 gx_spi_status(void)
{
	return readl(DWSPI_VIRT + DWSPI_SR);
}

u32 gx_spi_wrapper_status(void)
{
	return readl(DWSPI_WRAP_VIRT + DWSPI_WRAP_CTRL);
}

u32 gx_spi_pad_status(void)
{
	return readl(DWSPI_WRAP_VIRT + DWSPI_WRAP_PAD);
}

u32 gx_spi_debug_reg(u32 offset)
{
	return readl(DWSPI_VIRT + offset);
}

#else

static u32 gxflash_base(void)
{
	/* Prefer uncached alias once MMU is on; physical works pre-MMU too
	 * only if identity-mapped — stage-1 calls this post-MMU. */
	return GXFLASH_VIRT;
}

static int gx_flash_wait(u32 base)
{
	u32 i;

	/* Keep SPI probe snappy so a dead controller cannot stall UART boot. */
	for (i = 0; i < 100000u; i++) {
		if (readl(base + GXFLASH_STAT) & GXFLASH_RDY)
			return 0;
	}
	return -1;
}

static int gx_flash_word(u32 base, u32 out, u32 *in)
{
	u32 ctrl;
	int ret;

	writel(out, base + GXFLASH_CMD);
	ctrl = readl(base + GXFLASH_CTRL) & ~GXFLASH_GO;
	writel(ctrl | GXFLASH_GO, base + GXFLASH_CTRL);
	ret = gx_flash_wait(base);
	writel(ctrl, base + GXFLASH_CTRL);
	if (ret)
		return ret;
	if (in)
		*in = readl(base + GXFLASH_DATA);
	return 0;
}

static void gx_flash_cs(u32 base, int assert)
{
	u32 ctrl = readl(base + GXFLASH_CTRL);

	if (assert)
		ctrl |= GXFLASH_CSHOLD;
	else
		ctrl &= ~GXFLASH_CSHOLD;
	writel(ctrl, base + GXFLASH_CTRL);
}

void gx_spi_init(void)
{
	u32 base = gxflash_base();

	/*
	 * Do not reprogram 0xa4d00204 / 0xa4d00300 here: start.S and
	 * clocks_init() already set the always-on domain.  Rewriting them
	 * after UART is live has been observed to break the U-Boot handoff.
	 * Only ensure the gxflash pin route and controller init value.
	 */
	writel(readl(SPI_ROUTE_REG_VIRT) | BIT(22), SPI_ROUTE_REG_VIRT);
	writel(0, base + GXFLASH_AUX);
	writel(GXFLASH_CTRL_INIT, base + GXFLASH_CTRL);
}

int gx_spi_read(u32 flash_off, void *dst, u32 len)
{
	u32 base = gxflash_base();
	u8 *out = (u8 *)dst;
	u32 pos = 0;
	u8 hdr[4];
	u32 total;
	int ret;

	hdr[0] = 0x03;
	hdr[1] = (flash_off >> 16) & 0xffu;
	hdr[2] = (flash_off >> 8) & 0xffu;
	hdr[3] = flash_off & 0xffu;
	total = 4 + len;

	gx_flash_cs(base, 1);
	for (pos = 0; pos < total; pos += 4) {
		u32 cmd = 0;
		u32 data = 0;
		u32 lane;
		int need_read = 0;

		for (lane = 0; lane < 4; lane++) {
			u32 p = pos + lane;
			u8 b;

			if (p >= total)
				break;
			if (p < 4)
				b = hdr[p];
			else {
				b = 0;
				need_read = 1;
			}
			cmd |= (u32)b << (24 - 8 * lane);
		}
		ret = gx_flash_word(base, cmd, need_read ? &data : 0);
		if (ret) {
			gx_flash_cs(base, 0);
			return ret;
		}
		if (need_read) {
			for (lane = 0; lane < 4; lane++) {
				u32 p = pos + lane;

				if (p >= total)
					break;
				if (p >= 4)
					out[p - 4] = (data >> (8 * lane)) & 0xffu;
			}
		}
	}
	gx_flash_cs(base, 0);
	return 0;
}

int gx_spi_read_id(u8 id[3])
{
	u32 base = gxflash_base();
	u32 data = 0;
	int ret;

	/*
	 * gxflash always shifts four bytes. The byte received while sending 0x9f
	 * occupies DATA[7:0]; the three JEDEC bytes follow in lanes 1..3.
	 */
	gx_flash_cs(base, 1);
	ret = gx_flash_word(base, 0x9f000000u, &data);
	gx_flash_cs(base, 0);
	if (ret)
		return ret;
	id[0] = (u8)(data >> 8);
	id[1] = (u8)(data >> 16);
	id[2] = (u8)(data >> 24);
	if ((id[0] == 0x00u && id[1] == 0x00u && id[2] == 0x00u) ||
	    (id[0] == 0xffu && id[1] == 0xffu && id[2] == 0xffu))
		return -4;
	return 0;
}

#endif
