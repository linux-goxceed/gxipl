/* SPDX-License-Identifier: MIT */
/*
 * SPI next-stage load — GxLoader model: IPL + bootloader live in one BOOT
 * partition.  Stage-1 already ran the GXBC bootcode from bootcode_flash_off;
 * here we load a second GXBC (U-Boot) from within the same BOOT image.
 */
#include "gx_hw.h"
#include "gx_spi.h"
#include "gx_table.h"
#include "bootcode_hdr.h"
#include "ipl_config.h"
#include "print.h"

extern struct ipl_config g_ipl_cfg;
extern int g_verbose;

#if defined(SOC_GX6706)
static void gx6706_spi_dump_abs(const char *name, u32 address)
{
	bc_puts("SPIABS name=");
	bc_puts(name);
	bc_puts(" address=");
	bc_put_hex(address);
	bc_puts(" value=");
	bc_put_hex(readl(address));
	bc_puts("\r\n");
}

static void gx6706_spi_dump_regs(void)
{
	u32 i;

	bc_puts("SPI SOURCE open post-jedec\r\n");
	bc_puts("SPI REGDUMP BEGIN\r\n");
	gx6706_spi_dump_abs("route", SPI_ROUTE_REG_VIRT);
	gx6706_spi_dump_abs("pad-byte", 0xa030a794u);
	gx6706_spi_dump_abs("gate", DWSPI_GATE_VIRT);
	gx6706_spi_dump_abs("cs-legacy", DWSPI_CS_LEGACY_VIRT);
	gx6706_spi_dump_abs("wrap-ctrl", DWSPI_WRAP_VIRT + DWSPI_WRAP_CTRL);
	gx6706_spi_dump_abs("wrap-pad", DWSPI_WRAP_VIRT + DWSPI_WRAP_PAD);
	gx6706_spi_dump_abs("wrap-pad-hi", DWSPI_WRAP_VIRT + DWSPI_WRAP_PAD + 4u);
	gx6706_spi_dump_abs("io-9000", 0xa4809000u);
	gx6706_spi_dump_abs("io-8000", 0xa4808000u);
	gx6706_spi_dump_abs("io-8070", 0xa4808070u);
	gx6706_spi_dump_abs("io-0138", 0xa4800138u);
	gx6706_spi_dump_abs("io-0140", 0xa4800140u);
	gx6706_spi_dump_abs("io-0144", 0xa4800144u);
	gx6706_spi_dump_abs("io-0148", 0xa4800148u);
	gx6706_spi_dump_abs("io-014c", 0xa480014cu);
	gx6706_spi_dump_abs("io-4164", 0xa4804164u);
	gx6706_spi_dump_abs("io-4168", 0xa4804168u);
	gx6706_spi_dump_abs("io-900c", 0xa480900cu);
	gx6706_spi_dump_abs("io-81b0", 0xa48081b0u);
	gx6706_spi_dump_abs("io-81b8", 0xa48081b8u);
	gx6706_spi_dump_abs("io-9110", 0xa4809110u);
	gx6706_spi_dump_abs("io-9130", 0xa4809130u);
	gx6706_spi_dump_abs("io-9150", 0xa4809150u);
	/* Dump each 32-bit DW-SSI register slot through the data register. */
	for (i = 0; i <= DWSPI_DR; i += 4) {
		bc_puts("SPIREG offset=");
		bc_put_hex(i);
		bc_puts(" value=");
		bc_put_hex(gx_spi_debug_reg(i));
		bc_puts("\r\n");
	}
	bc_puts("SPIREG offset=");
	bc_put_hex(DWSPI_RX_SAMPLE_DLY);
	bc_puts(" value=");
	bc_put_hex(gx_spi_debug_reg(DWSPI_RX_SAMPLE_DLY));
	bc_puts("\r\n");
	bc_puts("SPIREG offset=");
	bc_put_hex(DWSPI_SPI_CTRLR0);
	bc_puts(" value=");
	bc_put_hex(gx_spi_debug_reg(DWSPI_SPI_CTRLR0));
	bc_puts("\r\n");
	bc_puts("SPI REGDUMP END\r\n");
}
#endif

static void cache_flush(void)
{
	u32 op = BIT(0) | BIT(1) | BIT(4) | BIT(5);

	__asm__ __volatile__("mtcr %0, cr17\n\tidly4" : : "r"(op) : "memory");
}

void bc_jump(u32 entry)
{
	if (g_verbose) {
		bc_puts("Jumping to ");
		bc_put_hex(entry);
		bc_puts("...\r\n");
	}
	cache_flush();
	((void (*)(void))entry)();
	for (;;)
		;
}

static void bc_put_part_size(u32 bytes)
{
	if (bytes >= 1024u && (bytes % 1024u) == 0) {
		bc_put_dec(bytes / 1024u);
		bc_puts("KiB");
	} else {
		bc_put_dec(bytes);
		bc_puts(" bytes");
	}
}

static void bc_announce_part(const struct gx_part *part)
{
	if (!g_verbose || !part)
		return;
	bc_puts("Reading: ");
	bc_puts(part->name);
	bc_puts(" (GxLoader partition on ");
	bc_put_hex(part->start);
	bc_puts(", size ");
	bc_put_part_size(part->total_size);
	bc_puts(")\r\n");
}

static int peek_gxbc(u32 flash_off, u32 max_window, struct bootcode_hdr *hdr)
{
	if (!max_window || max_window < sizeof(*hdr))
		return -1;
	if (gx_spi_read(flash_off, hdr, sizeof(*hdr)))
		return -1;
	if (hdr->magic != BOOTCODE_MAGIC || !hdr->size ||
	    hdr->size > UBOOT_MAX_SIZE ||
	    hdr->size > max_window - sizeof(*hdr))
		return -1;
	return 0;
}

static int load_gxbc(u32 flash_off, const struct bootcode_hdr *hdr)
{
	u8 *dst = (u8 *)UBOOT_ENTRY;
	u32 sum, i;

	if (gx_spi_read(flash_off + sizeof(*hdr), dst, hdr->size))
		return -1;
	sum = 0;
	for (i = 0; i < hdr->size; i++)
		sum += dst[i];
	if (sum != hdr->checksum)
		return -1;
	bc_jump(hdr->entry ? hdr->entry : UBOOT_ENTRY);
	return 0;
}

/* True if this GXBC is the stage-2 bootcode we are already running. */
static int is_stage2_gxbc(u32 flash_off, const struct bootcode_hdr *hdr)
{
	u32 bc = g_ipl_cfg.bootcode_flash_off ?
		g_ipl_cfg.bootcode_flash_off : FLASH_BOOTCODE_OFF;

	if (flash_off == bc)
		return 1;
	if (hdr->entry == BOOTCODE_ENTRY)
		return 1;
	return 0;
}

static int try_gxbc_in_boot(u32 flash_off, u32 boot_end)
{
	struct bootcode_hdr hdr;
	u32 window;

	if (flash_off >= boot_end)
		return -1;
	window = boot_end - flash_off;
	if (peek_gxbc(flash_off, window, &hdr))
		return -1;
	if (is_stage2_gxbc(flash_off, &hdr))
		return -1;
	return load_gxbc(flash_off, &hdr);
}

/*
 * Default U-Boot GXBC placement inside BOOT (same idea as splice_boot.py):
 * after stage-2, prefer absolute 0x10000 when it still lies in BOOT; otherwise
 * the next 4 KiB boundary past the stage-2 image.
 */
static u32 default_uboot_off(u32 boot_start, u32 boot_end)
{
	struct bootcode_hdr bc_hdr;
	u32 bc = g_ipl_cfg.bootcode_flash_off ?
		g_ipl_cfg.bootcode_flash_off : FLASH_BOOTCODE_OFF;
	u32 after;
	u32 classic = boot_start + FLASH_UBOOT_OFF;

	if (classic != bc && classic + sizeof(bc_hdr) < boot_end)
		return classic;

	if (peek_gxbc(bc, boot_end - bc, &bc_hdr) == 0) {
		after = bc + sizeof(bc_hdr) + bc_hdr.size;
		after = (after + 0xfffu) & ~0xfffu;
		if (after == bc)
			after += 0x1000u;
		if (after + sizeof(bc_hdr) < boot_end)
			return after;
	}
	return 0;
}

int bc_spi_load_uboot(void)
{
	struct gx_table tbl;
	const struct gx_part *boot;
	u32 boot_start, boot_end, off, step;

	gx_spi_init();
	{
		u8 jedec[3] = { 0xffu, 0xffu, 0xffu };
		int ret;

		ret = gx_spi_read_id(jedec);
		if (ret == 0) {
			if (g_verbose) {
				bc_puts("SPI JEDEC ID: ");
				bc_put_hex(((u32)jedec[0] << 16) |
					   ((u32)jedec[1] << 8) | jedec[2]);
				bc_puts("\r\n");
			}
		} else if (g_verbose) {
			bc_puts("SPI JEDEC read failed rc=");
			bc_put_hex((u32)ret);
			bc_puts(" id=");
			bc_put_hex(((u32)jedec[0] << 16) |
				   ((u32)jedec[1] << 8) | jedec[2]);
#if defined(SOC_GX6706)
			bc_puts(" sr=");
			bc_put_hex(gx_spi_status());
			bc_puts(" wrap=");
			bc_put_hex(gx_spi_wrapper_status());
			bc_puts(" pad=");
			bc_put_hex(gx_spi_pad_status());
#else
			bc_puts(" ctrl=");
			bc_put_hex(readl(GXFLASH_VIRT + GXFLASH_CTRL));
			bc_puts(" stat=");
			bc_put_hex(readl(GXFLASH_VIRT + GXFLASH_STAT));
			bc_puts(" data=");
			bc_put_hex(readl(GXFLASH_VIRT + GXFLASH_DATA));
			bc_puts(" route=");
			bc_put_hex(readl(SPI_ROUTE_REG_VIRT));
#endif
			bc_puts("\r\n");
#if defined(SOC_GX6706)
			gx6706_spi_dump_regs();
#endif
		}
	}

	if (gx_table_load(&tbl))
		return -1;

	boot = gx_table_find(&tbl, "BOOT");
	if (!boot && tbl.count)
		boot = &tbl.part[0];
	if (!boot || !boot->total_size)
		return -1;

	boot_start = boot->start;
	boot_end = boot_start + boot->total_size;

	/* GxLoader-style: always present as a BOOT partition read. */
	bc_announce_part(boot);

	/* Explicit override (absolute flash offset), must sit inside BOOT. */
	if (g_ipl_cfg.uboot_flash_off) {
		off = g_ipl_cfg.uboot_flash_off;
		if (off < boot_start || off >= boot_end)
			return -1;
		if (g_ipl_cfg.uboot_flash_max &&
		    off + g_ipl_cfg.uboot_flash_max < boot_end)
			boot_end = off + g_ipl_cfg.uboot_flash_max;
		return try_gxbc_in_boot(off, boot_end);
	}

	off = default_uboot_off(boot_start, boot_end);
	if (off && try_gxbc_in_boot(off, boot_end) == 0)
		return 0;

	/* Sparse scan for a packed GXBC that is not stage-2. */
	for (step = 0; boot_start + step + 16u < boot_end; step += 0x1000u) {
		off = boot_start + step;
		if (try_gxbc_in_boot(off, boot_end) == 0)
			return 0;
	}
	return -1;
}
