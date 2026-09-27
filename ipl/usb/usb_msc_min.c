/* SPDX-License-Identifier: MIT */
/*
 * Slim GX6702/GX6706 USB pad/PHY + EHCI host + BOT MSC READ for FatFs.
 */

#include "gx_hw.h"
#include "ipl_internal.h"
#include "print.h"
#include "usb_debug.h"
#include "usb_msc_min.h"   /* USB_MSC_MIN_MAX_SECTORS */

/* --- IPL shims for the bootcode-only helpers this core uses --- */
/*
 * The core below is the bootcode USB code, which tests the bootcode's
 * `g_verbose` flag.  The IPL has no such flag: its equivalent is
 * `ipl_quiet`, set once from the IPL config by ipl_pre_mmu() before any USB
 * call.
 *
 * SENSE TRAP: `ipl_quiet` is misnamed -- it is assigned
 * ipl_config_verbose_early(), which returns 1 when the config's VERBOSE bit
 * is SET, and ipl_crumb() prints when `!ipl_quiet`.  So it actually holds
 * "verbose" (1 = verbose).  Do not invert it here: inverting compiles
 * cleanly, still links usb_debug_regs, and then suppresses every trace at
 * runtime -- which looks exactly like a dead USB port on the console.
 *
 * Do NOT declare `int g_verbose;` here.  A plain zero-initialized definition
 * lets GCC prove every `if (g_verbose)` block statically false and delete all
 * the diagnostics -- the image then builds clean, is the same size, and
 * silently contains none of the traces.  Alias the real IPL flag instead so
 * the dumps are both live and correctly gated on the config's VERBOSE bit.
 */
#define g_verbose	(ipl_quiet)

void bc_puts(const char *t) { uart_puts_at(UART_VIRT, t); }
void bc_putc(char c) { uart_puts_at(UART_VIRT, (const char[]){ c, 0 }); }
void bc_put_hex(u32 v)
{
        static const char h[] = "0123456789abcdef";
        char o[9];
        int i;

        for (i = 7; i >= 0; i--) { o[i] = h[v & 0xfu]; v >>= 4; }
        o[8] = 0;
        uart_puts_at(UART_VIRT, o);
}
void bc_put_dec(u32 v)
{
        char o[12];
        int n = 0;

        if (!v) { uart_puts_at(UART_VIRT, "0"); return; }
        while (v) { o[n++] = (char)('0' + v % 10u); v /= 10u; }
        while (n) uart_puts_at(UART_VIRT, (const char[]){ o[--n], 0 });
}

/* EHCI capability / operational registers (standard layout). */
#define EHCI_CAPLENGTH		0x00
#define EHCI_HCSPARAMS		0x04
#define EHCI_USBCMD		0x00
#define EHCI_USBSTS		0x04
#define EHCI_USBINTR		0x08
#define EHCI_FRINDEX		0x0c
#define EHCI_CTRLDSSEGMENT	0x10
#define EHCI_PERIODICLISTBASE	0x14
#define EHCI_ASYNCLISTADDR	0x18
#define EHCI_CONFIGFLAG		0x40
#define EHCI_PORTSC		0x44

#define USBCMD_RS		BIT(0)
#define USBCMD_HCRST		BIT(1)
#define USBCMD_ASE		BIT(5)
#define USBCMD_IAAD		BIT(6)
#define USBSTS_USBERRINT	BIT(1)
#define USBSTS_PCD		BIT(2)
#define USBSTS_IAA		BIT(5)
#define USBSTS_ASS		BIT(15)
#define USBSTS_HCHALTED		BIT(12)
#define PORTSC_CCS		BIT(0)
#define PORTSC_CSC		BIT(1)
#define PORTSC_PED		BIT(2)
#define PORTSC_PEC		BIT(3)
#define PORTSC_OCC		BIT(5)
#define PORTSC_PR		BIT(8)
#define PORTSC_PP		BIT(12)
#define PORTSC_PSPD(x)		(((x) >> 26) & 3u)
/* Write-1-to-clear.  A read-modify-write must not echo these. */
#define PORTSC_W1C		(PORTSC_CSC | PORTSC_PEC | PORTSC_OCC)

static void delay_loops(u32 n)
{
	volatile u32 i;
	for (i = 0; i < n; i++)
		;
}

/*
 * Post-PLL CK610 is a few hundred MHz.  200000 empty iterations is well
 * above 1 ms on both GX6702 and GX6706, which is the safe direction for
 * USB reset recovery.  delay_loops() stays for the untimed PHY settle.
 */
static void mdelay(u32 ms)
{
	while (ms--)
		delay_loops(200000);
}

static void gx_clrset(u32 addr, u32 clear, u32 set)
{
	u32 v = readl(addr);
	v = (v & ~clear) | set;
	writel(v, addr);
}

#if defined(SOC_GX6706)
/*
 * Packed route/field rows.
 *
 * The unpacked form cost 12 B and 24 B per row (420 B together), which does
 * not fit the gx6706 `usb` (SPI+USB) variant inside the 8128 B stage-1
 * window.  Nearly all of that is redundancy:
 *
 *   - `first`  is the highest set bit of `clear` in 9 of 10 field rows.
 *   - `second` is `first >> 1`              in 8 of 10 field rows.
 *   - `gate_mask` is a single bit in every route row and 7 of 10 field rows,
 *     so a one-byte shift index replaces a four-byte mask.
 *   - `target` (1..3) and `gate` (1..2) pack into one byte.
 *
 * The two exceptions live in the side tables, selected by `flags`.
 *
 * Verified lossless by tests/replay_cygnus_tables.py, which replays the old
 * and new tables through a model of cygnus_usb_clocks() and diffs every
 * register write.  A plain table roundtrip is NOT sufficient: an earlier
 * version used 0 as the "no gate_mask" sentinel, which collides with a real
 * shift index of 0 and silently dropped a gate write for gate_mask == 1.
 * Hence CYGNUS_NO_GATE (0xff), not 0.
 */
#define CYGNUS_NO_GATE		0xffu	/* gshift sentinel: gate_mask was zero */
#define CYGNUS_F_OVR_FIRST	BIT(0)
#define CYGNUS_F_OVR_SECOND	BIT(1)

struct cygnus_row {
	u8 tg;		/* (target << 4) | gate */
	u8 gshift;	/* gate_mask bit index, or CYGNUS_NO_GATE */
	u8 flags;
	u8 fshift;	/* bit index of `first`, or CYGNUS_NO_GATE when first==0.
			 * Occupies what would otherwise be alignment padding, so
			 * it is free.  Storing a shift rather than deriving the
			 * high bit with a loop saved ~230 B of .text: on this
			 * target a shift loop is not cheap. */
	u32 clear;
	u32 value;
};

static const struct cygnus_route {
	u8 index;		/* 1..18: selects the 0xa0600ffc + index*4 register */
	u8 gate;		/* 1 or 2 */
	u8 gshift;	/* gate_mask bit index, or CYGNUS_NO_GATE */
	u32 value;
} cygnus_usb_routes[] = {
	{  1, 1,  0, 0x05555555u },
	{  2, 1,  1, 0x05555555u },
	{  3, 1,  9, 0x15555555u },
	{  4, 1, 10, 0x0cccccccu },
	{  5, 1, 11, 0x0cccccccu },
	{  6, 1, 12, 0x10000000u },
	{  7, 1, 13, 0x0cccccccu },
	/* Was 0x09245fd9; the working GxLoader stage-2 carries the plain
	 * 1/7-stride 0x09249249 here (verified against the cygnus-6706S5
	 * loader table).  The wrong bits left a route mis-muxed. */
	{  8, 1, 14, 0x09249249u },
	{  9, 1, 15, 0x05d1745du },
	{ 12, 2,  9, 0x0cccccccu },
	{ 13, 2, 19, 0x0aaaaaaau },
	{ 14, 2,  3, 0x08000000u },
	{ 16, 2,  0, 0x10000000u },
	{ 17, 2, 11, 0x0cccccccu },
	{ 18, 2, 10, 0x10000000u },
};

/* Rows whose first/second do not follow the derive rule. */
static const u32 cygnus_first_ovr[1] = { 0x00000000u };
static const u32 cygnus_second_ovr[2] = { 0x00000040u, 0x00000000u };

static const struct cygnus_row cygnus_usb_fields[] = {
	{ (1 << 4) | 2, CYGNUS_NO_GATE, 0, 31, 0xff800000u, 0x0a800000u },
	{ (1 << 4) | 1,  4, 0, 19, 0x000ff000u, 0x0002b000u },
	{ (1 << 4) | 1,  3, 0,  7, 0x000000ffu, 0x00000007u },
	{ (2 << 4) | 1, 19, 0, 31, 0xff000000u, 0x0e000000u },
	{ (2 << 4) | 1, 21, 0, 23, 0x00ff0000u, 0x00050000u },
	{ (2 << 4) | 1, 22, 0, 15, 0x0000ff00u, 0x00000900u },
	{ (3 << 4) | 1, 27, 0, 31, 0xff000000u, 0x03000000u },
	{ (3 << 4) | 2, 29, 0, 23, 0x00ff0000u, 0x002b0000u },
	/* first == second == 0x40, so second is not first >> 1. */
	{ (3 << 4) | 1, CYGNUS_NO_GATE, CYGNUS_F_OVR_SECOND, 6,
	  0x00000078u, 0x00000038u },
	/* first == 0, so it is not the high bit of clear. */
	{ (3 << 4) | 1, CYGNUS_NO_GATE,
	  CYGNUS_F_OVR_FIRST | CYGNUS_F_OVR_SECOND, CYGNUS_NO_GATE,
	  0x00000007u, 0x00000007u },
};

static u32 cygnus_target(u8 target)
{
	if (target == 1)
		return 0xa030a024u;
	if (target == 2)
		return 0xa030a178u;
	return 0xa030a17cu;
}

static u32 cygnus_gate(u8 gate)
{
	return gate == 2 ? 0xa030a174u : 0xa030a170u;
}

/* Reconstruct a single-bit mask from a shift index; 0xff means "no bit". */
static u32 cygnus_bit(u8 shift)
{
	return shift == CYGNUS_NO_GATE ? 0u : (1u << shift);
}

/*
 * A row's gate mask is a single bit, or absent.  Decode with a branch on the
 * sentinel: shifting by 31+ would be undefined.
 */
static void cygnus_usb_clocks(void)
{
	u32 i;

	/* USB PLL: recovered packed 0x405f7e00 maps to 0x0011102d. */
	writel(0x7811102du, 0xa030a0ccu);
	delay_loops(1000);
	writel(0x0011102du, 0xa030a0ccu);

	for (i = 0; i < ARRAY_SIZE(cygnus_usb_routes); i++) {
		const struct cygnus_route *r = &cygnus_usb_routes[i];
		u32 addr = 0xa0600ffcu + (u32)r->index * 4u;

		writel(r->value | BIT(30), addr);
		writel(r->value | BIT(30) | BIT(31), addr);
		gx_clrset(cygnus_gate(r->gate), 0, cygnus_bit(r->gshift));
	}
	for (i = 0; i < ARRAY_SIZE(cygnus_usb_fields); i++) {
		const struct cygnus_row *f = &cygnus_usb_fields[i];
		u32 addr = cygnus_target(f->tg >> 4);
		u32 first = cygnus_bit(f->fshift);
		u32 second = first >> 1;
		u32 value = readl(addr);

		if (f->flags & CYGNUS_F_OVR_FIRST)
			first = cygnus_first_ovr[0];
		if (f->flags & CYGNUS_F_OVR_SECOND)
			second = cygnus_second_ovr[i - (ARRAY_SIZE(cygnus_usb_fields) - 2)];
		value = (value & ~f->clear) | f->value | second;
		writel(value, addr);
		writel(value | first, addr);
		gx_clrset(cygnus_gate(f->tg & 0xfu), 0, cygnus_bit(f->gshift));
	}
	gx_clrset(0xa030a170u, 0,
		  0x07000000u | BIT(30) | BIT(23) | 0x300001e0u | 0x00070000u);
	gx_clrset(0xa030a174u, 0,
		  BIT(1) | BIT(2) | BIT(14) | 0x1f800000u | BIT(4));
}
#endif

#if !defined(SOC_GX6706)
static void gx6702_pad_init(void)
{
	u32 v;

	/* Match the working U-Boot board USB pad/mux sequence. */
	gx_clrset(0xa030a068u, BIT(23), 0);
	gx_clrset(0xa030a00cu, BIT(8), 0);

	v = readl(0xa030a17cu);
	v &= ~BIT(23);
	writel(v, 0xa030a17cu);
	v = readl(0xa030a17cu);
	v |= BIT(23);
	writel(v, 0xa030a17cu);
	v = readl(0xa030a17cu);
	v = (v & 0xffc0ffffu) | 0x002b0000u;
	writel(v, 0xa030a17cu);
	v = readl(0xa030a17cu);
	v &= ~BIT(22);
	writel(v, 0xa030a17cu);
	v = readl(0xa030a17cu);
	v |= BIT(22);
	writel(v, 0xa030a17cu);
	gx_clrset(0xa030a174u, BIT(1) | BIT(2), 0);

	v = readl(0xa030a114u);
	v |= BIT(23);
	writel(v, 0xa030a114u);
	v = readl(0xa030a114u);
	v &= ~(BIT(0) | BIT(1) | BIT(9) | BIT(10) | BIT(7));
	writel(v, 0xa030a114u);
	v = readl(0xa030a114u);
	v |= BIT(8);
	writel(v, 0xa030a114u);
	v = readl(0xa030a114u);
	v &= ~0x70u;
	writel(v, 0xa030a114u);
	v = readl(0xa030a114u);
	v |= BIT(5) | BIT(6);
	writel(v, 0xa030a114u);
	v = readl(0xa030a114u);
	v &= 0xffff07ffu;
	writel(v, 0xa030a114u);
	v = readl(0xa030a114u);
	v |= BIT(11);
	writel(v, 0xa030a114u);
	v = readl(0xa030a114u);
	v &= ~BIT(3);
	writel(v, 0xa030a114u);
	v = readl(0xa030a114u);
	v &= 0xfff0ffffu;
	writel(v, 0xa030a114u);
	v = readl(0xa030a114u);
	v |= 0x00070000u;
	writel(v, 0xa030a114u);
	v = readl(0xa030a114u);
	v &= ~BIT(2);
	writel(v, 0xa030a114u);
	v = readl(0xa030a114u);
	v |= BIT(2);
	writel(v, 0xa030a114u);

	gx_clrset(0xa030a170u, 0, BIT(16) | BIT(17) | BIT(18));
	v = readl(0xa030a1b0u);
	v |= BIT(8);
	v &= ~(BIT(6) | BIT(7));
	writel(v, 0xa030a1b0u);
	writel(0, 0xa48040ccu);
	writel(0, 0xa48040dcu);
	writel(0, 0xa49040ccu);
	writel(0, 0xa49040dcu);

	v = readl(0xa030a200u);
	v &= 0xfffc03ffu;
	writel(v, 0xa030a200u);
	v = readl(0xa030a200u);
	v &= ~0x7cu;
	writel(v, 0xa030a200u);
	v = readl(0xa030a200u);
	v |= BIT(3) | BIT(4);
	writel(v, 0xa030a200u);
	v = readl(0xa030a200u);
	v &= 0xe0ffffffu;
	writel(v, 0xa030a200u);
	v = readl(0xa030a200u);
	v |= 0x1d000000u | 0xe0000000u;
	writel(v, 0xa030a200u);
	v = readl(0xa030a200u);
	v &= 0xfff3feffu;
	writel(v, 0xa030a200u);
	v = readl(0xa030a200u);
	v |= BIT(8);
	writel(v, 0xa030a200u);
	v = readl(0xa030a200u);
	v &= ~(BIT(22) | BIT(23) | BIT(29));
	writel(v, 0xa030a200u);
	v = readl(0xa030a200u);
	v |= BIT(29) | BIT(18) | BIT(19);
	writel(v, 0xa030a200u);
	v = readl(0xa030a200u);
	v &= ~(BIT(7) | BIT(20));
	writel(v, 0xa030a200u);
}

static void usb_program_phy(u32 base)
{
	writel(31, base + 0x0);
	writel(92, base + 0x8);
	writel(0xac, base + 0x14);
	writel(5, base + 0x18);
}
#endif

static void usb_phy_bits_clear_en(void)
{
	gx_clrset(USB_CFG_BASE + 0x4, (1u << 10) | (1u << 26), 0);
	gx_clrset(USB_CFG_BASE + 0x8, (1u << 26), 0);
}

static void usb_phy_bits_set_mid(void)
{
	gx_clrset(USB_CFG_BASE + 0x4, 0, (1u << 12) | (1u << 28));
	gx_clrset(USB_CFG_BASE + 0x8, 0, (1u << 28));
}

static void usb_phy_bits_clear_mid(void)
{
	gx_clrset(USB_CFG_BASE + 0x4, (1u << 12) | (1u << 28), 0);
	gx_clrset(USB_CFG_BASE + 0x8, (1u << 28), 0);
}

static void usb_phy_bits_set_en(void)
{
	gx_clrset(USB_CFG_BASE + 0x4, 0, (1u << 10) | (1u << 26));
	gx_clrset(USB_CFG_BASE + 0x8, 0, (1u << 26));
}

static int gx_usb_pad_phy(void)
{
#if defined(SOC_GX6706)
	/*
	 * Clean-room transcription of the Cygnus H5/S5 late USB setup.  The
	 * Cygnus PHY trim ports are in the 0xa0702xxx pad block; GX6702 instead
	 * exposes two PHY register banks at 0xa0908xxx.
	 */
	cygnus_usb_clocks();
	gx_clrset(0xa030a1b0u, BIT(6) | BIT(7), BIT(8));
	gx_clrset(0xa030a200u, 0, BIT(0) | BIT(1));
	writel(5, 0xa0702018u);
	writel(5, 0xa0702418u);
	gx_clrset(USB_CFG_BASE + 0x0, 0, 0x820f2000u);
	gx_clrset(USB_CFG_BASE + 0xc, 0, BIT(25));
	usb_phy_bits_clear_en();
	usb_phy_bits_set_mid();
	usb_phy_bits_clear_mid();
	usb_phy_bits_set_en();
	usb_phy_bits_clear_en();
	usb_phy_bits_set_mid();
	usb_phy_bits_clear_mid();
	delay_loops(200000);
	return 0;
#else
	u32 v;

	gx6702_pad_init();
	gx_clrset(0xa030a068u, (1u << 23), 0);
	gx_clrset(0xa030a00cu, (1u << 8), 0);
	v = readl(0xa030a17cu);
	v &= ~(1u << 23);
	writel(v, 0xa030a17cu);
	v = readl(0xa030a17cu);
	v |= (1u << 23);
	writel(v, 0xa030a17cu);
	v = readl(0xa030a17cu);
	v = (v & 0xffc0ffffu) | 0x002b0000u;
	writel(v, 0xa030a17cu);
	v = readl(0xa030a17cu);
	v &= ~(1u << 22);
	writel(v, 0xa030a17cu);
	v = readl(0xa030a17cu);
	v |= (1u << 22);
	writel(v, 0xa030a17cu);
	gx_clrset(0xa030a174u, (1u << 1) | (1u << 2), 0);

	usb_program_phy(USB_PHY0_BASE);
	usb_program_phy(USB_PHY1_BASE);
	gx_clrset(USB_CFG_BASE + 0x0, 0, 0x820f2000u);
	gx_clrset(USB_CFG_BASE + 0xc, 0, (1u << 25));
	usb_phy_bits_clear_en();
	usb_phy_bits_set_mid();
	usb_phy_bits_clear_mid();
	usb_phy_bits_set_en();
	usb_phy_bits_clear_en();
	usb_phy_bits_set_mid();
	usb_phy_bits_clear_mid();

	v = readl(0xa030aa08u);
	v &= ~((1u << 12) | (1u << 13));
	writel(v, 0xa030aa08u);
	v = readl(0xa030aa08u);
	v |= (1u << 13);
	writel(v, 0xa030aa08u);
	gx_clrset(0xa030a20cu, 0, (1u << 0));
	delay_loops(200000);
	return 0;
#endif
}

/* ---- U-Boot-compatible EHCI schedule objects (64 bytes each) ---- */
#define QH_SIZE		64
#define QTD_SIZE	64
#define QTD_NEXT_TERMINATE	1u
#define QTD_STATUS_ACTIVE	0x80u
#define QTD_STATUS_ERRORS	0x7fu
#define QTD_TOKEN_DT(x)		(((x) & 1u) << 31)
#define QTD_TOKEN_BYTES(x)	(((x) & 0x7fffu) << 16)
#define QTD_TOKEN_IOC		BIT(15)
#define QTD_TOKEN_CERR(x)	(((x) & 3u) << 10)
#define QTD_TOKEN_PID(x)		(((x) & 3u) << 8)
#define QTD_PID_OUT		0u
#define QTD_PID_IN		1u
#define QTD_PID_SETUP		2u
#define QTD_STATUS_HALTED	0x40u
#define QH_LINK_QH		2u
#define QH_EPCHAR_DTC		BIT(14)
#define QH_EPCHAR_H		BIT(15)
#define QH_EPCHAR_CTRL		BIT(27)
#define QH_EPCAP_MULT1		BIT(30)

/*
 * EHCI DMA buffers MUST live in DDR, not in the IPL's own .bss.
 *
 * usb_dma_addr() only translates the DDR_VIRT_BASE window, so a pool left in
 * ordinary RAM is handed to the controller with its *load* address
 * (e.g. 0x001029a0), which the EHCI cannot reach: the schedule goes live,
 * the qTD never completes, and the first SET_ADDRESS times out.  The bootcode
 * never hits this because ld/linker-bootcode.ld places .bss at 0x93c00000,
 * inside the DDR window; the IPL's ld/linker-8k.ld does not, so the pools
 * are anchored into DDR here as pointers instead of arrays.
 *
 * STAGE_BUF (DDR_VIRT_BASE) is the stage-1 receive buffer, unused by the time
 * stage-1.5 runs, so the low DDR is free.  bot_buf is placed past the qTD
 * pool on a 4 KiB boundary: the BOT CSW read wants a naturally aligned
 * buffer, and the whole arena stays inside one contiguous region.
 */
static u8 *const qh_pool  = (u8 *)DDR_VIRT_BASE;
static u8 *const qtd_pool = (u8 *)DDR_VIRT_BASE + QH_SIZE * 2;
static u8 *const bot_buf  = (u8 *)DDR_VIRT_BASE + 4096u;

static u32 ehci_op;
static const u8 msc_addr = 1;
static const u8 msc_ep_in = 1;
static const u8 msc_ep_out = 2;
static const u8 msc_config = 1;
static u8 msc_interface;   /* parsed no longer; unused by IPL path */
static u32 msc_tag;
static u8 msc_toggle_in;
static u8 msc_toggle_out;
static int msc_ready;
static const u16 msc_max_packet = 512;  /* HS bulk MPS; the stick advertises this */

static u32 usb_dma_addr(const void *ptr)
{
	u32 addr = (u32)ptr;

	if (addr >= DDR_VIRT_BASE && addr < DDR_VIRT_BASE + DDR_SIZE)
		return addr - DDR_VIRT_BASE + DDR_PHYS_BASE;
	return addr;
}

#if USB_DEBUG
/*
 * Register trace.
 *
 * Deliberately minimal: the release image is 7762 B of the 8128 B window, so
 * the entire USB_DEBUG build has only ~366 B to spend.  The earlier form of
 * this dumped nine registers behind nine label strings and cost ~1330 B --
 * more than the ROM has spare, and it could not be linked at all.  A stage
 * character plus the three registers that actually decide enumeration
 * (PORTSC for attach/reset/enable, USBCMD for run/async, USBSTS for halt)
 * costs a fraction of that and still localises any failure to one step.
 *
 * The stage argument is a character literal, not a string, so the nine call
 * sites do not each keep a unique .rodata entry alive.
 */
static void usb_debug_regs(char stage)
{
        if (!g_verbose)
                return;
        bc_puts("U");
        /*
         * Write the single stage byte directly.  Do NOT use bc_puts(&stage):
         * `stage` is one char, not a NUL-terminated string, so bc_puts walks
         * off the end of it and prints adjacent stack bytes -- which is what
         * made the first trace runs come out as "Ub\xff\xff...".
         */
        bc_putc(stage);
        bc_put_hex(readl(ehci_op + EHCI_PORTSC));
        bc_puts("/");
        bc_put_hex(readl(ehci_op + EHCI_USBCMD));
        bc_puts("/");
        bc_put_hex(readl(ehci_op + EHCI_USBSTS));
        bc_puts("\r\n");
}

/* `stage` is a short literal; only printed on the failure paths. */
static int usb_error(const char *stage)
{
	if (g_verbose) {
		bc_puts("USBERR ");
		bc_puts(stage);
		bc_puts("\r\n");
	}
	return -1;
}
#endif /* USB_DEBUG */

/*
 * usb_fail() is the release-build form of usb_error(): it drops the stage
 * string entirely so the nine call sites do not each keep a unique literal
 * ("PHY", "EHCI", "SET_ADDRESS", ...) alive in .rodata.  Use it everywhere
 * instead of `return usb_error(...)`.
 */
#if USB_DEBUG
#define usb_fail(stage)	usb_error(stage)
#else
#define usb_fail(stage)	(-1)
#endif

static void cache_op(u32 op)
{
	__asm__ __volatile__("mtcr %0, cr17\n\tidly4" : : "r"(op) : "memory");
}

/* cr17 bit 5 writes dirty lines back; bit 4 drops them. */
static void cache_clean_invalidate(void *p, u32 n)
{
	(void)p;
	(void)n;
	cache_op(BIT(0) | BIT(1) | BIT(4) | BIT(5));
}

static void cache_invalidate(void *p, u32 n)
{
	(void)p;
	(void)n;
	cache_op(BIT(0) | BIT(1) | BIT(4));
}

static void ehci_async_prepare(void);

static int ehci_init(void)
{
	u32 cap = USB_EHCI_BASE;
	u8 caplen = (u8)readl(cap + EHCI_CAPLENGTH);
	u32 i, sts, cmd;

	ehci_op = cap + caplen;
	#if USB_DEBUG
	usb_debug_regs('b');   /* before USBCMD_HCRST */
	#endif

	writel(USBCMD_HCRST, ehci_op + EHCI_USBCMD);
	for (i = 0; i < 100000; i++) {
		if (!(readl(ehci_op + EHCI_USBCMD) & USBCMD_HCRST))
			break;
	}
	writel(0, ehci_op + EHCI_USBINTR);
	writel(0, ehci_op + EHCI_CTRLDSSEGMENT);
	writel(1, ehci_op + EHCI_CONFIGFLAG);

	cmd = readl(ehci_op + EHCI_USBCMD);
	writel(cmd | USBCMD_RS, ehci_op + EHCI_USBCMD);
	for (i = 0; i < 100000; i++) {
		sts = readl(ehci_op + EHCI_USBSTS);
		if (!(sts & USBSTS_HCHALTED))
			break;
	}
	ehci_async_prepare();
	#if USB_DEBUG
	usb_debug_regs('s');   /* run started, schedule queued */
	#endif
	return 0;
}

/* U-Boot ehci_submit_root PORT_RESET: drop PE, hold PR, then wait for PED. */
static int ehci_port_reset_once(void)
{
	u32 port = ehci_op + EHCI_PORTSC;
	u32 v, i;

	v = readl(port) & ~PORTSC_W1C;
	v |= PORTSC_PP;
	writel(v, port);
	mdelay(20);

	v = readl(port);
	#if USB_DEBUG
	usb_debug_regs('p');   /* port powered, checking CCS */
	#endif
	if (!(v & PORTSC_CCS))
		return -1;

	v = (v & ~PORTSC_W1C & ~PORTSC_PED) | PORTSC_PR;
	writel(v, port);
	mdelay(50);
	v = (readl(port) & ~PORTSC_W1C) & ~PORTSC_PR;
	writel(v, port);
	for (i = 0; i < 100000; i++) {
		if (!(readl(port) & PORTSC_PR))
			break;
	}
	#if USB_DEBUG
	usb_debug_regs('r');   /* PR released, waiting for PED */
	#endif
	for (i = 0; i < 100000; i++) {
		v = readl(port);
		if (v & PORTSC_PED) {
			/* USB 2.0 7.1.7.5: no device traffic for 10 ms. */
			mdelay(10);
			#if USB_DEBUG
			usb_debug_regs('y');   /* PED set, 10 ms settling */
			#endif
			return 0;
		}
	}
	#if USB_DEBUG
	usb_debug_regs('t');   /* PED never came up */
	#endif
	return -1;
}

/*
 * Endpoint-speed field (QH Endpt1 bits 13:12) -> HIGH SPEED.
 *
 * Do NOT take this from PORTSC bits 27:26 (PSPD) on this silicon.  Measured
 * on a real 6706S5 with a high-speed stick: PSPD reads 0 (full speed) even
 * though the link is genuinely running high speed, and driving the control
 * QH with EPS=FS makes the device reject the very first SETUP with
 * XACT_ERR.  The stick advertises a 512-byte bulk MPS, which USB 2.0 allows
 * only at high speed, so the link really is HS.  (PORTSC bit 24, PFSC, is
 * "disable HS chirping" and does appear to be latched the wrong way here --
 * that is the remaining suspect for why PSPD reads 0.)
 * Encoded per U-Boot drivers/usb/host/ehci-hcd.c:ehci_encode_speed().
 */
#define QH_EPS_FS	0u
#define QH_EPS_LS	1u
#define QH_EPS_HS	2u

static u32 msc_eps = QH_EPS_HS;

static int ehci_port_reset(void)
{
	u32 port = ehci_op + EHCI_PORTSC;
    #if USB_DEBUG
	usb_debug_regs('o');   /* entering port reset */
	#endif
	if (!ehci_port_reset_once())
		return 0;
	/* One more power cycle.  A marginal chirp is not a missing stick. */
	writel((readl(port) & ~PORTSC_W1C) & ~PORTSC_PP, port);
	mdelay(20);
	return ehci_port_reset_once();
}

/*
 * Very small async schedule: one QH + qTDs. This is intentionally minimal and
 * may need tuning on hardware; the FatFs path fails soft if enumeration fails.
 */

struct qtd {
	u32 next;
	u32 alt;
	u32 token;
	u32 buf[5];
	u32 buf_hi[5];
	u32 unused[3];
};

/*
 * EHCI Queue Head (QH) is 64 bytes total: 16-byte header, 32-byte qTD
 * overlay (next/alt/token + 5 buffer pointers) and 16 bytes reserved.
 * The overlay is only the first 32 bytes of a qTD -- not the full 64-byte
 * struct qtd -- otherwise the QH would be 96 bytes and corrupt the schedule.
 */
struct qh {
	u32 horiz;
	u32 epchar;
	u32 epcap;
	u32 cur_qtd;
	struct {
		u32 next;
		u32 alt;
		u32 token;
		u32 buf[5];
	} overlay;
	u32 fill[4];
};

typedef char qtd_size_must_be_64[(sizeof(struct qtd) == QTD_SIZE) ? 1 : -1];
typedef char qh_size_must_be_64[(sizeof(struct qh) == QH_SIZE) ? 1 : -1];

static struct qh *ehci_reclaim_qh(void)
{
	return (struct qh *)qh_pool;
}

static struct qh *ehci_xfer_qh(void)
{
	return (struct qh *)(qh_pool + QH_SIZE);
}

/*
 * Permanent halted reclaim head.  ASYNCLISTADDR is written once, while ASE
 * is off, and later transfers only rewrite this QH's horizontal link.
 */
static void ehci_async_prepare(void)
{
	struct qh *head = ehci_reclaim_qh();
	u32 i, phys = usb_dma_addr(head);

	for (i = 0; i < QH_SIZE; i++)
		qh_pool[i] = 0;
	head->horiz = phys | QH_LINK_QH;
	head->epchar = QH_EPCHAR_H | QH_EPCHAR_DTC;
	head->epcap = QH_EPCAP_MULT1;
	head->overlay.next = QTD_NEXT_TERMINATE;
	head->overlay.alt = QTD_NEXT_TERMINATE;
	/* Halted so the empty reclaim head is never executed. */
	head->overlay.token = QTD_STATUS_HALTED;
	cache_clean_invalidate(head, QH_SIZE);

	writel(readl(ehci_op + EHCI_USBCMD) & ~USBCMD_ASE,
	       ehci_op + EHCI_USBCMD);
	for (i = 0; i < 100000; i++)
		if (!(readl(ehci_op + EHCI_USBSTS) & USBSTS_ASS))
			break;
	writel(phys, ehci_op + EHCI_ASYNCLISTADDR);
}

static void qtd_fill(struct qtd *td, u32 next, u8 pid, void *data, u32 len,
		     u8 toggle, int ioc)
{
	u32 i;

	td->next = next;
	td->alt = QTD_NEXT_TERMINATE;
	td->token = QTD_TOKEN_BYTES(len) | QTD_TOKEN_DT(toggle) |
		QTD_TOKEN_CERR(3) | QTD_TOKEN_PID(pid) | QTD_STATUS_ACTIVE;
	if (ioc)
		td->token |= QTD_TOKEN_IOC;
	if (!len)
		return;
	td->buf[0] = usb_dma_addr(data);
	/*
	 * Bulk qTDs are split at the fixed 512-byte max packet size; all
	 * control requests in this IPL carry no data. A nonempty qTD can
	 * therefore touch at most two 4 KiB pages. Leave the unused page
	 * slots alone instead of clearing pointers the controller cannot reach.
	 */
	td->buf[1] = usb_dma_addr((void *)
		(((u32)data + 0x1000u) & ~0xfffu));
	for (i = 0; i < 2; i++)
		td->buf_hi[i] = 0;
}

#if USB_DEBUG
static int ehci_qtd_error(struct qtd *td, const char *why)
{
	if (g_verbose) {
		bc_puts("USBERR EHCI ");
		bc_puts(why);
		bc_puts(" token=");
		bc_put_hex(td->token);
		bc_puts(" sts=");
		bc_put_hex(readl(ehci_op + EHCI_USBSTS));
		bc_puts("\r\n");
	}
	return -1;
}
#endif

/*
 * Link a transfer QH in front of the reclaim head, run it to completion,
 * then retire it with the async-advance doorbell.  ASE stays on across the
 * qTDs of one transfer so SETUP and its status stage are not separate
 * transactions.
 */
static int ehci_submit(u8 addr, u8 ep, struct qtd *first, struct qtd *last,
		       u32 nqtd)
{
	struct qh *head = ehci_reclaim_qh();
	struct qh *qh = ehci_xfer_qh();
	u32 i, sts, cmd;
	u32 phys_qh = usb_dma_addr(qh);
	u16 mps = ep ? msc_max_packet : 64u;

	for (i = 0; i < QH_SIZE; i++)
		((u8 *)qh)[i] = 0;
	qh->horiz = usb_dma_addr(head) | QH_LINK_QH;
	qh->epchar = (addr & 0x7f) | ((ep & 0xf) << 8) | (msc_eps << 12) |
		QH_EPCHAR_DTC | ((u32)mps << 16) | (8u << 28);
	if (ep == 0)
		qh->epchar |= QH_EPCHAR_CTRL;
	qh->epcap = QH_EPCAP_MULT1;
	qh->overlay.next = usb_dma_addr(first);
	qh->overlay.alt = QTD_NEXT_TERMINATE;
	/*
	 * Leave the overlay token inactive and not halted.  A halted overlay
	 * stops the controller from advancing to the linked qTDs; U-Boot
	 * leaves this word zero and only the qTDs are Active.
	 */
	qh->overlay.token = 0;

	head->horiz = phys_qh | QH_LINK_QH;
	cache_clean_invalidate(qh_pool, QH_SIZE * 2);
	cache_clean_invalidate(qtd_pool, nqtd * QTD_SIZE);

	sts = readl(ehci_op + EHCI_USBSTS);
	writel(sts & 0x3fu, ehci_op + EHCI_USBSTS);
	cmd = readl(ehci_op + EHCI_USBCMD);
	if (!(cmd & USBCMD_ASE)) {
		writel(cmd | USBCMD_ASE, ehci_op + EHCI_USBCMD);
		for (i = 0; i < 100000; i++)
			if (readl(ehci_op + EHCI_USBSTS) & USBSTS_ASS)
				break;
	}

	for (i = 0; i < 2000000; i++) {
		/* Invalidate only.  A clean here writes Active back over
		 * the token the controller just retired. */
		cache_invalidate(qtd_pool, nqtd * QTD_SIZE);
		if (!(last->token & QTD_STATUS_ACTIVE))
			break;
	}
#if USB_DEBUG
	if ((last->token & QTD_STATUS_ACTIVE) && g_verbose) {
		bc_puts("USBERR EHCI live cmd=");
		bc_put_hex(readl(ehci_op + EHCI_USBCMD));
		bc_puts(" sts=");
		bc_put_hex(readl(ehci_op + EHCI_USBSTS));
		bc_puts(" async=");
		bc_put_hex(readl(ehci_op + EHCI_ASYNCLISTADDR));
		bc_puts(" setup=");
		bc_put_hex(first->token);
		bc_puts("\r\n");
	}
#endif

	head->horiz = usb_dma_addr(head) | QH_LINK_QH;
	cache_clean_invalidate(head, QH_SIZE);
	writel(readl(ehci_op + EHCI_USBCMD) | USBCMD_IAAD,
	       ehci_op + EHCI_USBCMD);
	for (i = 0; i < 100000; i++) {
		sts = readl(ehci_op + EHCI_USBSTS);
		if (sts & USBSTS_IAA) {
			writel(USBSTS_IAA, ehci_op + EHCI_USBSTS);
			break;
		}
	}
	writel(readl(ehci_op + EHCI_USBCMD) & ~USBCMD_ASE,
	       ehci_op + EHCI_USBCMD);
	for (i = 0; i < 100000; i++)
		if (!(readl(ehci_op + EHCI_USBSTS) & USBSTS_ASS))
			break;

	cache_invalidate(qtd_pool, nqtd * QTD_SIZE);
	if (last->token & QTD_STATUS_ACTIVE)
	    #if USB_DEBUG
		return ehci_qtd_error(last, "active");
		#else
		return -1;
		#endif
	if (last->token & QTD_STATUS_ERRORS)
	    #if USB_DEBUG
		return ehci_qtd_error(last, "token");
		#else
		return -1;
		#endif
	return 0;
}

/*
 * A single qTD may not span more than MAXPKTLEN bytes.  A 512-byte sector
 * read on a full-speed bulk pipe therefore has to be queued as one qTD per
 * 64-byte TRB inside a single QH, so the toggle and handshake stay on one
 * logical transfer.  qtd_fill() already lays out the 0x1000-stride buffer
 * pointers, so the extra TRBs just need to be chained and the buffer
 * advanced.  Returns the number of qTDs written, or 0 if len is zero.
 */
static u32 msc_fill_seg_qtds(struct qtd *td, u8 pid, u8 *data, u32 len,
			     u8 toggle)
{
	u32 n = 0;
	u32 mps = msc_max_packet ? msc_max_packet : 64u;

	while (len && n < (QTD_SIZE * 8) - 1) {
		u32 chunk = len;

		if (chunk > mps)
			chunk = mps;
		qtd_fill(&td[n], usb_dma_addr(&td[n + 1]), pid, data, chunk,
			 toggle, 1);
		toggle ^= 1u;
		data += chunk;
		len -= chunk;
		n++;
	}
	return n;
}

static int ehci_async_xfer(u8 addr, u8 ep, u8 pid, void *data, u32 len,
			   u8 toggle)
{
	struct qtd *td = (struct qtd *)qtd_pool;
	u32 n;

	/* All callers here are BOT bulk endpoints; control uses ctrl_req(). */
	if (len > msc_max_packet) {
		n = msc_fill_seg_qtds(td, pid, (u8 *)data, len, toggle);
		if (!n)
			return -1;
		td[n - 1].next = QTD_NEXT_TERMINATE;
		if (len)
			cache_clean_invalidate(data, len);
		if (ehci_submit(addr, ep, td, &td[n - 1], n))
			return -1;
		if (pid == QTD_PID_IN && len)
			cache_clean_invalidate(data, len);
		return 0;
	}
	qtd_fill(td, QTD_NEXT_TERMINATE, pid, data, len, toggle, 1);
	if (len)
		cache_clean_invalidate(data, len);
	if (ehci_submit(addr, ep, td, td, 1))
		return -1;
	if (pid == QTD_PID_IN && len)
		cache_clean_invalidate(data, len);
	return 0;
}

static int ctrl_req(u8 addr, u8 bmRequestType, u8 bRequest, u16 wValue,
		    u16 wIndex, u16 wLength, void *data)
{
	struct qtd *td = (struct qtd *)qtd_pool;
	u32 setup[2];
	u32 n = 2;
	u8 status_pid;

	/* Target is little-endian: USB setup fields are packed byte-for-byte. */
	setup[0] = (u32)bmRequestType | ((u32)bRequest << 8) |
		((u32)wValue << 16);
	setup[1] = (u32)wIndex | ((u32)wLength << 16);
	status_pid = (bmRequestType & 0x80) ? QTD_PID_OUT : QTD_PID_IN;
	if (wLength) {
		u8 pid = (bmRequestType & 0x80) ? QTD_PID_IN : QTD_PID_OUT;

		qtd_fill(&td[0], usb_dma_addr(&td[1]), QTD_PID_SETUP,
			 setup, 8, 0, 0);
		qtd_fill(&td[1], usb_dma_addr(&td[2]), pid, data, wLength, 1, 0);
		qtd_fill(&td[2], QTD_NEXT_TERMINATE, status_pid, 0, 0, 1, 1);
		n = 3;
		cache_clean_invalidate(data, wLength);
	} else {
		qtd_fill(&td[0], usb_dma_addr(&td[1]), QTD_PID_SETUP,
			 setup, 8, 0, 0);
		qtd_fill(&td[1], QTD_NEXT_TERMINATE, status_pid, 0, 0, 1, 1);
		n = 2;
	}
	cache_clean_invalidate(setup, sizeof(setup));
	if (ehci_submit(addr, 0, &td[0], &td[n - 1], n))
		return -1;
	if ((bmRequestType & 0x80) && wLength)
		cache_clean_invalidate(data, wLength);
	return 0;
}

static void msc_advance_toggle(u8 *toggle, u32 len)
{
	if (len && (((len + msc_max_packet - 1u) / msc_max_packet) & 1u))
		*toggle ^= 1;
}


static int msc_reset_transport(void)
{
	if (ctrl_req(msc_addr, 0x21, 0xff, 0, msc_interface, 0, 0))
		return -1;
	if (ctrl_req(msc_addr, 0x02, 0x01, 0, 0x80 | msc_ep_in, 0, 0))
		return -1;
	if (ctrl_req(msc_addr, 0x02, 0x01, 0, msc_ep_out, 0, 0))
		return -1;
	msc_toggle_in = 0;
	msc_toggle_out = 0;
	return 0;
}

static int bot_cmd(u8 *cb, u8 cblen, void *data, u32 len, int data_in)
{
	u32 cbw_csw[12];
	u8 *cbw = (u8 *)cbw_csw;
	u8 *csw = (u8 *)&cbw_csw[8];
	u32 i;

	/* CBW/CSW are little-endian wire structures; initialize by words. */
	for (i = 4; i < 12; i++)
		cbw_csw[i] = 0;
	cbw_csw[0] = 0x43425355u;
	cbw_csw[1] = ++msc_tag;
	cbw_csw[2] = len;
	cbw_csw[3] = (data_in ? 0x80u : 0u) | ((u32)cblen << 16);
	for (i = 0; i < cblen && i < 16; i++)
		cbw[15 + i] = cb[i];

	if (ehci_async_xfer(msc_addr, msc_ep_out, 0, cbw, 31, msc_toggle_out)) {
#if USB_DEBUG
		if (g_verbose)
			bc_puts("USBERR BOT CBW\r\n");
#endif
		return -1;
	}
	msc_advance_toggle(&msc_toggle_out, 31);
	if (len && data) {
		u8 *toggle = data_in ? &msc_toggle_in : &msc_toggle_out;
		if (ehci_async_xfer(msc_addr, data_in ? msc_ep_in : msc_ep_out,
				    data_in ? 1 : 0, data, len, *toggle)) {
#if USB_DEBUG
			if (g_verbose)
				bc_puts("USBERR BOT DATA\r\n");
#endif
			return -1;
		}
		msc_advance_toggle(toggle, len);
	}
	if (ehci_async_xfer(msc_addr, msc_ep_in, 1, csw, 13, msc_toggle_in)) {
#if USB_DEBUG
		if (g_verbose)
			bc_puts("USBERR BOT CSW-XFER\r\n");
#endif
		return -1;
	}
	msc_advance_toggle(&msc_toggle_in, 13);
	if (csw[0] != 0x55 || csw[1] != 0x53 || csw[2] != 0x42 ||
	    csw[3] != 0x53 || csw[12] != 0) {
#if USB_DEBUG
		if (g_verbose) {
			bc_puts("USBERR BOT CSW status=");
			bc_put_dec(csw[12]);
			bc_puts("\r\n");
		}
#endif
		return -1;
	}
	return 0;
}

/*
 * Quiesce the controller before handing off to stage 2.
 *
 * The stage-1.5 bootcode runs its OWN USB driver (bootcode/usb/usb_msc.c),
 * starting with a USBCMD_HCRST.  When stage 1 reached us over USB it left the
 * controller running: ASE set, an async schedule live in ASYNCLISTADDR, and a
 * port that still has PED latched and the device at the address we assigned.
 * HCRST against that state does not reliably converge, and the bootcode then
 * blocks waiting for a reset/port condition that never arrives.
 *
 * On SPI and UART handoff paths the controller has never been started, so
 * this is a no-op there -- it only matters for the USB->stage-2 case, which is
 * exactly where the gx6706 board hung with no output after
 * "Attempting to boot from USB...".
 *
 * Order matters: drop ASE first and wait for ASS to clear (so the schedule
 * stops touching memory), then clear the port, then HCRST and wait for it to
 * fall out of reset.
 */
void usb_msc_min_quiesce(void)
{
	static const u32 spins = 100000;
	u32 i;

	if (!ehci_op)
		return;
	/* 1. stop the async schedule */
	writel(readl(ehci_op + EHCI_USBCMD) & ~USBCMD_ASE,
	       ehci_op + EHCI_USBCMD);
	for (i = 0; i < spins; i++)
		if (!(readl(ehci_op + EHCI_USBSTS) & USBSTS_ASS))
			break;
	/* 2. drop the port enable and the bus reset */
	writel(readl(ehci_op + EHCI_PORTSC) & ~PORTSC_PED & ~PORTSC_PR,
	       ehci_op + EHCI_PORTSC);
	/* 3. controller reset, bounded so a wedged controller cannot hang us */
	writel(USBCMD_HCRST, ehci_op + EHCI_USBCMD);
	for (i = 0; i < spins; i++)
		if (!(readl(ehci_op + EHCI_USBCMD) & USBCMD_HCRST))
			break;
	msc_ready = 0;
}

int usb_msc_min_init(void)
{
        u32 cb[4];

        msc_ready = 0;
        msc_tag = 0;
        msc_toggle_in = 0;
        msc_toggle_out = 0;

        if (gx_usb_pad_phy())
                return usb_fail("PHY");
        if (ehci_init())
                return usb_fail("EHCI");
        if (ehci_port_reset())
                return usb_fail("PORT");

        /*
         * Deliberately NOT latching the port speed from PORTSC bits 27:26.
         * Measured: PSPD reads 0 (full speed) on this silicon even for a
         * stick that advertises a 512-byte bulk MPS (i.e. truly high speed),
         * and driving the control QH with EPS=FS makes the device reject the
         * very first SETUP with XACT_ERR.  So the QH speed field stays HS
         * (see msc_eps).
         *
         * The MSC topology is FIXED rather than parsed from the configuration
         * descriptor, exactly as the original cutdown core did: msc_parse_config()
         * and its 512-byte ctrl_buf cost ~400 B of the 8 KiB window, and a
         * boot stick is built by this tree, so the endpoints are known.
         * SET_CONFIGURATION is still issued, with msc_config == 1.
         */

        /* SET_ADDRESS, then USB 2.0 9.2.6.3 recovery before the next token. */
        if (ctrl_req(0, 0x00, 0x05, msc_addr, 0, 0, 0))
                return usb_fail("SET_ADDRESS");
        mdelay(10);

        if (ctrl_req(msc_addr, 0x00, 0x09, msc_config, 0, 0, 0))
                return usb_fail("SET_CONFIGURATION");
        if (msc_reset_transport())
                return usb_fail("BOT_RESET");

        /* TEST UNIT READY: six zero CDB bytes. */
        cb[0] = 0;
        cb[1] = 0;
        bot_cmd((u8 *)cb, 6, 0, 0, 0);

        /* INQUIRY: allocation length 36 in CDB byte 4. */
        cb[0] = 0x12;
        cb[1] = 36;
        bot_cmd((u8 *)cb, 6, bot_buf, 36, 1);

        msc_ready = 1;
        return 0;
}

int usb_msc_min_read_sectors(u32 lba, u8 *buf)
{
	u32 cb[4];
	u32 i;

	if (!msc_ready)
		return -1;
	cb[0] = 0x28u | (((lba >> 24) & 0xffu) << 16) |
		(((lba >> 16) & 0xffu) << 24);
	cb[1] = ((lba >> 8) & 0xffu) | ((lba & 0xffu) << 8);
	cb[2] = 0;
	/*
	 * Multi-sector READ(10).  The IPL's diskio layer declares a window of
	 * USB_MSC_MIN_MAX_SECTORS sectors aligned to that boundary, so the
	 * transfer must cover the whole window -- transferring only sector 0
	 * left the rest of the window uninitialised while window_lba still
	 * claimed the aligned base, so every read past the first sector
	 * returned garbage and pf_open() failed with FR_NO_FILE.
	 */
	cb[2] = USB_MSC_MIN_MAX_SECTORS;
	if (bot_cmd((u8 *)cb, 10, bot_buf, USB_MSC_MIN_MAX_SECTORS * 512u, 1)) {
#if USB_DEBUG
		if (g_verbose) {
			bc_puts("USBERR READ10 lba=");
			bc_put_dec(lba);
			bc_puts("\r\n");
		}
#endif
		return -1;
	}
	for (i = 0; i < USB_MSC_MIN_MAX_SECTORS * 512u; i++)
		buf[i] = bot_buf[i];
	return 0;
}

int usb_msc_present(void)
{
	return msc_ready;
}
