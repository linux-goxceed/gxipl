/* SPDX-License-Identifier: MIT */
/*
 * Slim GX6702/GX6706 USB pad/PHY + EHCI host + BOT MSC READ for FatFs.
 */

#include "gx_hw.h"
#include "print.h"

extern int g_verbose;

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
struct cygnus_route {
	u8 index;
	u8 gate;
	u32 value;
	u32 gate_mask;
};

struct cygnus_field {
	u8 target;
	u8 gate;
	u32 clear;
	u32 first;
	u32 second;
	u32 value;
	u32 gate_mask;
};

static const struct cygnus_route cygnus_usb_routes[] = {
	{ 1, 1, 0x05555555u, 0x00000001u },
	{ 2, 1, 0x05555555u, 0x00000002u },
	{ 3, 1, 0x15555555u, 0x00000200u },
	{ 4, 1, 0x0cccccccu, 0x00000400u },
	{ 5, 1, 0x0cccccccu, 0x00000800u },
	{ 6, 1, 0x10000000u, 0x00001000u },
	{ 7, 1, 0x0cccccccu, 0x00002000u },
	/* Was 0x09245fd9; the working GxLoader stage-2 carries the plain
	 * 1/7-stride 0x09249249 here (verified against the cygnus-6706S5
	 * loader table).  The wrong bits left a route mis-muxed. */
	{ 8, 1, 0x09249249u, 0x00004000u },
	{ 9, 1, 0x05d1745du, 0x00008000u },
	{ 12, 2, 0x0cccccccu, 0x00000200u },
	{ 13, 2, 0x0aaaaaaau, 0x00080000u },
	{ 14, 2, 0x08000000u, 0x00000008u },
	{ 16, 2, 0x10000000u, 0x00000001u },
	{ 17, 2, 0x0cccccccu, 0x00000800u },
	{ 18, 2, 0x10000000u, 0x00000400u },
};

static const struct cygnus_field cygnus_usb_fields[] = {
	{ 1, 2, 0xff800000u, 0x80000000u, 0x40000000u, 0x0a800000u, 0 },
	{ 1, 1, 0x000ff000u, 0x00080000u, 0x00040000u, 0x0002b000u, BIT(4) },
	{ 1, 1, 0x000000ffu, 0x00000080u, 0x00000040u, 0x00000007u, BIT(3) },
	{ 2, 1, 0xff000000u, 0x80000000u, 0x40000000u, 0x0e000000u, BIT(19) },
	{ 2, 1, 0x00ff0000u, 0x00800000u, 0x00400000u, 0x00050000u, BIT(21) },
	{ 2, 1, 0x0000ff00u, 0x00008000u, 0x00004000u, 0x00000900u, BIT(22) },
	{ 3, 1, 0xff000000u, 0x80000000u, 0x40000000u, 0x03000000u, BIT(27) },
	{ 3, 2, 0x00ff0000u, 0x00800000u, 0x00400000u, 0x002b0000u, BIT(29) },
	{ 3, 1, 0x00000078u, 0x00000040u, 0x00000040u, 0x00000038u, 0 },
	{ 3, 1, 0x00000007u, 0, 0, 0x00000007u, 0 },
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
		gx_clrset(cygnus_gate(r->gate), 0, r->gate_mask);
	}
	for (i = 0; i < ARRAY_SIZE(cygnus_usb_fields); i++) {
		const struct cygnus_field *f = &cygnus_usb_fields[i];
		u32 addr = cygnus_target(f->target);
		u32 value = readl(addr);

		value = (value & ~f->clear) | f->value | f->second;
		writel(value, addr);
		writel(value | f->first, addr);
		gx_clrset(cygnus_gate(f->gate), 0, f->gate_mask);
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

static u8 __attribute__((aligned(32))) qh_pool[QH_SIZE * 4];
static u8 __attribute__((aligned(32))) qtd_pool[QTD_SIZE * 8];
static u8 __attribute__((aligned(32))) ctrl_buf[512];
static u8 __attribute__((aligned(4096))) bot_buf[512];

static u32 ehci_op;
static u8 msc_addr = 1;
static u8 msc_ep_in = 1;
static u8 msc_ep_out = 2;
static u8 msc_config;
static u8 msc_interface;
static u32 msc_tag;
static u8 msc_toggle_in;
static u8 msc_toggle_out;
static int msc_ready;
static u16 msc_max_packet = 64;

static u32 usb_dma_addr(const void *ptr)
{
	u32 addr = (u32)ptr;

	if (addr >= DDR_VIRT_BASE && addr < DDR_VIRT_BASE + DDR_SIZE)
		return addr - DDR_VIRT_BASE + DDR_PHYS_BASE;
	return addr;
}

static void usb_debug_regs(const char *stage)
{
	if (!g_verbose)
		return;
	bc_puts("USBDBG ");
	bc_puts(stage);
	bc_puts(" ehci=");
	bc_put_hex(USB_EHCI_BASE);
	bc_puts(" cap=");
	bc_put_hex(readl(USB_EHCI_BASE + EHCI_HCSPARAMS));
	bc_puts(" cmd=");
	bc_put_hex(readl(ehci_op + EHCI_USBCMD));
	bc_puts(" sts=");
	bc_put_hex(readl(ehci_op + EHCI_USBSTS));
	bc_puts(" cfg=");
	bc_put_hex(readl(ehci_op + EHCI_CONFIGFLAG));
	bc_puts(" port=");
	bc_put_hex(readl(ehci_op + EHCI_PORTSC));
	bc_puts(" pad=");
	bc_put_hex(readl(0xa030a114u));
	bc_puts(" mux=");
	bc_put_hex(readl(0xa030a200u));
	bc_puts(" vbus=");
	bc_put_hex(readl(0xa030aa08u));
	bc_puts("\r\n");
}

static int usb_error(const char *stage)
{
	if (g_verbose) {
		bc_puts("USBERR ");
		bc_puts(stage);
		bc_puts("\r\n");
	}
	return -1;
}

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
	usb_debug_regs("before");

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
	usb_debug_regs("start");
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
	usb_debug_regs("power");
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
	usb_debug_regs("reset");
	for (i = 0; i < 100000; i++) {
		v = readl(port);
		if (v & PORTSC_PED) {
			/* USB 2.0 7.1.7.5: no device traffic for 10 ms. */
			mdelay(10);
			usb_debug_regs("ready");
			return 0;
		}
	}
	usb_debug_regs("reset-timeout");
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

	usb_debug_regs("port");
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
	for (i = 1; i < ARRAY_SIZE(td->buf); i++)
		td->buf[i] = usb_dma_addr((void *)
			((((u32)data + i * 0x1000u) & ~0xfffu)));
	for (i = 0; i < ARRAY_SIZE(td->buf_hi); i++)
		td->buf_hi[i] = 0;
}

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
		return ehci_qtd_error(last, "active");
	if (last->token & QTD_STATUS_ERRORS)
		return ehci_qtd_error(last, "token");
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
	u32 i, n;

	for (i = 0; i < QTD_SIZE; i++)
		qtd_pool[i] = 0;
	if (len > msc_max_packet && (ep == msc_ep_in || ep == msc_ep_out)) {
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
	u8 setup[8];
	u32 n = 2;
	u8 status_pid;

	setup[0] = bmRequestType;
	setup[1] = bRequest;
	setup[2] = wValue & 0xff;
	setup[3] = (wValue >> 8) & 0xff;
	setup[4] = wIndex & 0xff;
	setup[5] = (wIndex >> 8) & 0xff;
	setup[6] = wLength & 0xff;
	setup[7] = (wLength >> 8) & 0xff;

	for (n = 0; n < QTD_SIZE * 3; n++)
		qtd_pool[n] = 0;
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

static int msc_parse_config(void)
{
	u16 total;
	u32 pos = 0;
	int mass_storage = 0;

	total = (u16)ctrl_buf[2] | ((u16)ctrl_buf[3] << 8);
	if (total > sizeof(ctrl_buf))
		total = sizeof(ctrl_buf);
	while (pos + 2u <= total) {
		u8 length = ctrl_buf[pos];
		u8 type = ctrl_buf[pos + 1];

		if (length < 2 || pos + length > total)
			break;
		if (type == 4 && length >= 9) {
			msc_interface = ctrl_buf[pos + 2];
			mass_storage = ctrl_buf[pos + 5] == 8 &&
				ctrl_buf[pos + 6] == 6 &&
				ctrl_buf[pos + 7] == 0x50;
		} else if (type == 5 && length >= 7 && mass_storage) {
			u8 endpoint = ctrl_buf[pos + 2];
			if (ctrl_buf[pos + 3] == 2) {
				if (endpoint & 0x80)
					msc_ep_in = endpoint & 0x0f;
				else
					msc_ep_out = endpoint & 0x0f;
				msc_max_packet = (u16)ctrl_buf[pos + 4] |
					((u16)ctrl_buf[pos + 5] << 8);
				msc_max_packet &= 0x07ffu;
			}
		}
		pos += length;
	}
	if (!msc_ep_in || !msc_ep_out)
		return -1;
	/*
	 * Speed clamp, currently inactive because msc_eps is fixed at HS (see
	 * the comment on msc_eps for why PSPD is not trusted on this silicon).
	 * Kept because it is the correct handling if a genuine FS/LS device is
	 * ever attached: a bulk endpoint is capped at 64 bytes at full speed
	 * and 8 at low speed, and 512 is only legal at high speed.  A too-large
	 * MAXPKTLEN in the QH makes the host issue an over-length packet the
	 * device babbles on, so clamping plus msc_fill_seg_qtds() splitting is
	 * what keeps the schedule legal.
	 */
	if (msc_eps != QH_EPS_HS) {
		u32 cap = (msc_eps == QH_EPS_LS) ? 8u : 64u;

		if (msc_max_packet > cap)
			msc_max_packet = (u16)cap;
	}
	msc_config = ctrl_buf[5];
	if (g_verbose) {
		bc_puts("USBMSC cfg=");
		bc_put_dec(msc_config);
		bc_puts(" in=");
		bc_put_dec(msc_ep_in);
		bc_puts(" out=");
		bc_put_dec(msc_ep_out);
		bc_puts(" max=");
		bc_put_dec(msc_max_packet);
		bc_puts("\r\n");
	}
	return mass_storage ? 0 : -1;
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
	u8 cbw[31];
	u8 csw[13];
	u32 i;

	for (i = 0; i < sizeof(csw); i++)
		csw[i] = 0;
	for (i = 0; i < 31; i++)
		cbw[i] = 0;
	cbw[0] = 0x55;
	cbw[1] = 0x53;
	cbw[2] = 0x42;
	cbw[3] = 0x43;
	msc_tag++;
	cbw[4] = msc_tag & 0xff;
	cbw[5] = (msc_tag >> 8) & 0xff;
	cbw[6] = (msc_tag >> 16) & 0xff;
	cbw[7] = (msc_tag >> 24) & 0xff;
	cbw[8] = len & 0xff;
	cbw[9] = (len >> 8) & 0xff;
	cbw[10] = (len >> 16) & 0xff;
	cbw[11] = (len >> 24) & 0xff;
	cbw[12] = data_in ? 0x80 : 0x00;
	cbw[13] = 0;
	cbw[14] = cblen;
	for (i = 0; i < cblen && i < 16; i++)
		cbw[15 + i] = cb[i];

	if (ehci_async_xfer(msc_addr, msc_ep_out, 0, cbw, 31, msc_toggle_out)) {
		if (g_verbose)
			bc_puts("USBERR BOT CBW\r\n");
		return -1;
	}
	msc_advance_toggle(&msc_toggle_out, 31);
	if (len && data) {
		u8 *toggle = data_in ? &msc_toggle_in : &msc_toggle_out;
		if (ehci_async_xfer(msc_addr, data_in ? msc_ep_in : msc_ep_out,
				    data_in ? 1 : 0, data, len, *toggle)) {
			if (g_verbose)
				bc_puts("USBERR BOT DATA\r\n");
			return -1;
		}
		msc_advance_toggle(toggle, len);
	}
	if (ehci_async_xfer(msc_addr, msc_ep_in, 1, csw, 13, msc_toggle_in)) {
		if (g_verbose)
			bc_puts("USBERR BOT CSW-XFER\r\n");
		return -1;
	}
	msc_advance_toggle(&msc_toggle_in, 13);
	if (csw[0] != 0x55 || csw[1] != 0x53 || csw[2] != 0x42 ||
	    csw[3] != 0x53 || csw[12] != 0) {
		if (g_verbose) {
			bc_puts("USBERR BOT CSW status=");
			bc_put_dec(csw[12]);
			bc_puts("\r\n");
		}
		return -1;
	}
	return 0;
}

int usb_msc_init(void)
{
	u8 cb[16];
	u32 i;

	msc_ready = 0;
	msc_tag = 0;
	msc_toggle_in = 0;
	msc_toggle_out = 0;
	msc_ep_in = 0;
	msc_ep_out = 0;
	msc_interface = 0;
	msc_max_packet = 64;
	if (gx_usb_pad_phy())
		return usb_error("PHY");
	if (ehci_init())
		return usb_error("EHCI");
	if (ehci_port_reset())
		return usb_error("PORT");

	/*
	 * Deliberately NOT latching the port speed from PORTSC bits 27:26.
	 * Measured: PSPD reads 0 (full speed) on this silicon even for a
	 * stick that advertises a 512-byte bulk MPS (i.e. truly high speed),
	 * and driving the control QH with EPS=FS makes the device reject the
	 * very first SETUP with XACT_ERR.  So the QH speed field stays HS
	 * (see msc_eps) and MAXPKTLEN is taken from the descriptor as-is.
	 */

	/* SET_ADDRESS, then USB 2.0 9.2.6.3 recovery before the next token. */
	if (ctrl_req(0, 0x00, 0x05, msc_addr, 0, 0, 0))
		return usb_error("SET_ADDRESS");
	mdelay(10);

	/* GET_DESCRIPTOR device — best-effort */
	if (ctrl_req(msc_addr, 0x80, 0x06, 0x0100, 0, 18, ctrl_buf))
		return usb_error("DEVICE_DESCRIPTOR");

	if (ctrl_req(msc_addr, 0x80, 0x06, 0x0200, 0, sizeof(ctrl_buf), ctrl_buf))
		return usb_error("CONFIG_DESCRIPTOR");
	if (msc_parse_config())
		return usb_error("MSC_DESCRIPTOR");

	/* SET_CONFIGURATION from the discovered descriptor. */
	if (ctrl_req(msc_addr, 0x00, 0x09, msc_config, 0, 0, 0))
		return usb_error("SET_CONFIGURATION");
	if (msc_reset_transport())
		return usb_error("BOT_RESET");

	/* SCSI TEST UNIT READY / INQUIRY soft */
	for (i = 0; i < 16; i++)
		cb[i] = 0;
	cb[0] = 0x00; /* TEST UNIT READY */
	bot_cmd(cb, 6, 0, 0, 0);

	for (i = 0; i < 16; i++)
		cb[i] = 0;
	cb[0] = 0x12;
	cb[4] = 36;
	bot_cmd(cb, 6, bot_buf, 36, 1);

	msc_ready = 1;
	return 0;
}

int usb_msc_read_sector(u32 lba, u8 *buf)
{
	u8 cb[16];
	u32 i;

	if (!msc_ready)
		return -1;
	for (i = 0; i < 16; i++)
		cb[i] = 0;
	cb[0] = 0x28; /* READ(10) */
	cb[2] = (lba >> 24) & 0xff;
	cb[3] = (lba >> 16) & 0xff;
	cb[4] = (lba >> 8) & 0xff;
	cb[5] = lba & 0xff;
	cb[8] = 1;
	if (bot_cmd(cb, 10, bot_buf, 512, 1)) {
		if (g_verbose) {
			bc_puts("USBERR READ10 lba=");
			bc_put_dec(lba);
			bc_puts("\r\n");
		}
		return -1;
	}
	for (i = 0; i < 512; i++)
		buf[i] = bot_buf[i];
	return 0;
}

int usb_msc_present(void)
{
	return msc_ready;
}
