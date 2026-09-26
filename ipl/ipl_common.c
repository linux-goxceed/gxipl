/* SPDX-License-Identifier: MIT */
/* Shared CK610 stage-1 helpers and post-MMU loader path. */

#include "gx_hw.h"
#include "gx_chip.h"
#include "bootcode_hdr.h"
#include "ipl_config_api.h"
#include "ipl_internal.h"
#ifndef SOC_UNIVERSAL
#include "gx_spi.h"
#endif

void delay(u32 outer)
{
	volatile u32 i;
	volatile u32 j;

	for (i = 0; i < outer; i++)
		for (j = 0; j < 10; j++)
			;
}

int wait_value(u32 addr, u32 mask, u32 value)
{
	u32 count;

	for (count = 0; count < WAIT_LIMIT; count++) {
		if ((readl(addr) & mask) == value)
			return 0;
	}
	return -1;
}

/*
 * noinline: set_field is called from apply_patches, both eFuse trim loops and
 * several DDR-init sites.  LTO inlines it into every caller; keeping one
 * out-of-line copy is ~16-20 bytes smaller per SoC.
 */
__attribute__((noinline))
void set_field(u32 addr, u8 shift, u8 width, u32 value)
{
	u32 mask;
	u32 reg;

	mask = width == 32 ? 0xffffffffu : ((1u << width) - 1u);
	reg = readl(addr);
	reg &= ~(mask << shift);
	reg |= (value & mask) << shift;
	writel(reg, addr);
}

static void uart_putc_at(u32 base, u8 ch)
{
	while (!(readl(base + 0x14u) & BIT(6)))
		;
	writel(ch, base);
}

void uart_puts_at(u32 base, const char *text)
{
	while (*text)
		uart_putc_at(base, (u8)*text++);
}

/* IRUN crumbs are suppressed when verbose configuration is enabled. */
int ipl_quiet;

void ipl_crumb(char ch)
{
	if (!ipl_quiet)
		uart_putc_at(UART_PHYS, (u8)ch);
}

static u8 uart_getc_at(u32 base)
{
	while (!(readl(base + 0x14u) & BIT(0)))
		;
	return (u8)readl(base);
}

static void uart_flush_rx(u32 base)
{
	u32 n;
	u32 idle = 0;

	for (n = 0; n < 200000u; n++) {
		if (readl(base + 0x14u) & BIT(0)) {
			(void)readl(base);
			idle = 0;
		} else if (++idle > 2000u) {
			break;
		}
	}
}

void pll_program(u32 addr, u32 value)
{
	writel(0x78000000u | value, addr);
	delay(100);
	writel(value, addr);
}

void apply_patches(const u32 *patches, u32 count)
{
	u32 i;

	for (i = 0; i < count; i++) {
		u32 p = patches[i];

		set_field(DDR_BASE + (FP_REG(p) << 2),
			  FP_SHIFT(p), FP_WIDTH(p), FP_VALUE(p));
	}
}

#ifndef SOC_UNIVERSAL
/*
 * Decode an RLE register stream (see utils/gen_ddr_rle.py) straight into
 * the DDR controller window.  Token tags:
 *   0x00-0x7F LIT32 (tag+1 LE u32 words)   0x80-0x9F ZERO run
 *   0xA0-0xBF REPEAT (next byte = back-ref 1..16, LZ77-style with overlap)
 * Literal words are assembled byte-by-byte: CK610 unaligned-load behaviour
 * is not guaranteed, and the stream is byte-packed after each 1-byte tag.
 */
void ddr_apply_rle(const u8 *p, u32 reg_base, u32 count)
{
	u32 hist[16];
	u32 hn = 0;
	u32 reg = reg_base;

	while (hn < count) {
		u32 tag = *p++;
		u32 n;
		u32 back = 0;

		if (tag < 0x80u) {
			n = tag + 1u;
		} else {
			n = (tag & 0x1fu) + 1u;
			if (tag >= 0xa0u)
				back = *p++;
		}

		do {
			u32 v = 0;

			if (tag < 0x80u) {
				v = (u32)p[0] | ((u32)p[1] << 8) |
				    ((u32)p[2] << 16) | ((u32)p[3] << 24);
				p += 4;
			} else if (back) {
				v = hist[(hn - back) & 15u];
			}
			writel(v, DDR_BASE + (reg++ << 2));
			hist[hn++ & 15u] = v;
		} while (--n);
	}
}
#endif

static u32 read_u32(u32 uart)
{
	u32 value = 0;
	u32 i;

	for (i = 0; i < 4; i++)
		value |= (u32)uart_getc_at(uart) << (i * 8);
	return value;
}

#ifdef SOC_UNIVERSAL
void ipl_print_gxid(u32 uart)
{
	if (!gx_detected_name[0])
		gx_chip_probe(GX_CHIP_NAME_VIRT);
	uart_puts_at(uart, "GXID family=");
	uart_puts_at(uart, gx_family_tag(gx_detected_family));
	uart_puts_at(uart, " name=");
	uart_puts_at(uart, gx_detected_name);
	uart_puts_at(uart, "\r\n");
}
#endif
static void cache_writeback_invalidate_all(void)
{
	u32 op = BIT(0) | BIT(1) | BIT(4) | BIT(5);

	__asm__ __volatile__("mtcr %0, cr17\n\tidly4" : : "r"(op) : "memory");
}

/*
 * Both call sites pass 4-byte-aligned SRAM/DDR buffers, so copy whole words
 * and only mop up a 0-3 byte tail.  Faster and smaller than a byte loop.
 */
static void copy_mem(u8 *dst, const u8 *src, u32 n)
{
	u32 *d32 = (u32 *)dst;
	const u32 *s32 = (const u32 *)src;

	while (n >= 4u) {
		*d32++ = *s32++;
		n -= 4u;
	}
	dst = (u8 *)d32;
	src = (const u8 *)s32;
	while (n--)
		*dst++ = *src++;
}

static int uart_recv_image(u8 *destination, u32 max_size, u32 *out_size)
{
	u32 expected;
	u32 size;
	u32 checksum;
	u32 i;

	uart_flush_rx(UART_VIRT);
	#ifdef SOC_UNIVERSAL
	ipl_print_gxid(UART_VIRT);
	#endif
	/* Vendor-compatible ready marker; newer/raw uploaders also match GET. */
	uart_puts_at(UART_VIRT, "RUNGET");
	/* The host transmits immediately, so do not flush after the marker. */
	expected = read_u32(UART_VIRT);
	size = read_u32(UART_VIRT);
	if (!size || size > max_size) {
		uart_puts_at(UART_VIRT, "\r\nEMETA\r\n");
		return -1;
	}

	checksum = 0;
	for (i = 0; i < size; i++) {
		u8 byte = uart_getc_at(UART_VIRT);

		destination[i] = byte;
		checksum += byte;
	}
#ifndef IPL_MINIMAL
	/* Retain compatibility with the bring-up uploader's old metadata. */
	if (checksum != expected &&
	    !((expected >> 16) == 0x00c2u &&
	      (checksum & 0xffffu) == (expected & 0xffffu))) {
		uart_puts_at(UART_VIRT, "\r\nECHK\r\n");
		return -1;
	}
#else
	if (checksum != expected) {
		uart_puts_at(UART_VIRT, "\r\nECHK\r\n");
		return -1;
	}
#endif
	*out_size = size;
	uart_puts_at(UART_VIRT, "\r\nOK\r\n");
	return 0;
}

static void jump_to(u32 entry)
{
	cache_writeback_invalidate_all();
	((void (*)(void))entry)();
	for (;;)
		;
}

#ifndef SOC_UNIVERSAL
static int try_spi_bootcode(u32 cfg_off)
{
	struct bootcode_hdr hdr;
	u32 off = cfg_off ? cfg_off : FLASH_BOOTCODE_OFF;
	u32 sum;
	u32 i;
	u8 *dst = (u8 *)BOOTCODE_ENTRY;

	gx_spi_init();
	if (gx_spi_read(off, &hdr, sizeof(hdr)))
		return -1;
	if (hdr.magic != BOOTCODE_MAGIC || !hdr.size ||
	    hdr.size > BOOTCODE_MAX_SIZE)
		return -1;
	if (gx_spi_read(off + sizeof(hdr), dst, hdr.size))
		return -1;
	sum = 0;
	for (i = 0; i < hdr.size; i++)
		sum += dst[i];
	if (sum != hdr.checksum)
		return -1;
	jump_to(hdr.entry ? hdr.entry : BOOTCODE_ENTRY);
	return 0;
}
#endif

static void run_uart_payload(u8 *buf, u32 size)
{
	const struct bootcode_hdr *hdr = (const struct bootcode_hdr *)buf;
	u32 i;
	u32 sum;

#ifndef IPL_MINIMAL
	{
		const struct uboot_bundle_hdr *bundle;
		const u8 *payload;

		/* A normal uploader can append a GXUB record to an IPL container. */
		if (size >= 4u + 0x2000u + sizeof(*bundle) &&
		    buf[0] == 't' && buf[1] == 'o' &&
		    buf[2] == 'o' && buf[3] == 'b') {
			bundle = (const struct uboot_bundle_hdr *)(buf + 4u + 0x2000u);
			payload = (const u8 *)(bundle + 1);
			if (bundle->magic != UBOOT_BUNDLE_MAGIC ||
			    !bundle->size || bundle->size > UBOOT_MAX_SIZE ||
			    bundle->entry != UBOOT_ENTRY ||
			    bundle->size > size - (4u + 0x2000u + sizeof(*bundle))) {
				uart_puts_at(UART_VIRT, "\r\nEBUNDLE\r\n");
				for (;;)
					;
			}
			sum = 0;
			for (i = 0; i < bundle->size; i++)
				sum += payload[i];
			if (sum != bundle->checksum) {
				uart_puts_at(UART_VIRT, "\r\nEBUNDLECHK\r\n");
				for (;;)
					;
			}
			copy_mem((u8 *)UBOOT_ENTRY, payload, bundle->size);
			jump_to(UBOOT_ENTRY);
		}
	}
#endif

	if (size >= sizeof(*hdr) && hdr->magic == BOOTCODE_MAGIC) {
		if (hdr->size + sizeof(*hdr) > size ||
		    hdr->size > BOOTCODE_MAX_SIZE) {
			uart_puts_at(UART_VIRT, "\r\nEBC\r\n");
			for (;;)
				;
		}
		sum = 0;
		for (i = 0; i < hdr->size; i++)
			sum += buf[sizeof(*hdr) + i];
		if (sum != hdr->checksum) {
			uart_puts_at(UART_VIRT, "\r\nEBCCHK\r\n");
			for (;;)
				;
		}
		copy_mem((u8 *)BOOTCODE_ENTRY, buf + sizeof(*hdr), hdr->size);
		jump_to(hdr->entry ? hdr->entry : BOOTCODE_ENTRY);
	}

	/* Raw U-Boot was received directly at its link address. */
	if (buf != (u8 *)UBOOT_ENTRY)
		copy_mem((u8 *)UBOOT_ENTRY, buf, size);
	jump_to(UBOOT_ENTRY);
}

__attribute__((used, externally_visible))
void ipl_post_mmu(void)
{
	const struct ipl_config *cfg = (const struct ipl_config *)IPL_CONFIG_VA;
	u32 flags = IPL_CFG_DEFAULT_FLAGS;
	u32 bootcode_off = FLASH_BOOTCODE_OFF;
	u32 size;

	/*
	 * Read the SRAM config in place instead of copying 512 bytes to the
	 * stack.  An invalid blob falls back to the compiled-in defaults;
	 * only the two fields the IPL consumes are ever fetched.
	 */
	if (ipl_config_valid(cfg)) {
		flags = cfg->flags;
		bootcode_off = cfg->bootcode_flash_off;
	}
	if (flags & IPL_CFG_UART_DIRECT)
		goto uart_load;
#ifndef SOC_UNIVERSAL
	if (!(flags & IPL_CFG_SKIP_SPI) && try_spi_bootcode(bootcode_off) == 0)
		return;
#endif

uart_load:
	if (uart_recv_image((u8 *)UBOOT_ENTRY, UBOOT_MAX_SIZE, &size)) {
		for (;;)
			;
	}
	run_uart_payload((u8 *)UBOOT_ENTRY, size);
}
