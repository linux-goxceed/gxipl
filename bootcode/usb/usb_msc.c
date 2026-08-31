/* SPDX-License-Identifier: MIT */
/*
 * Slim GX6702/GX6706 USB pad/PHY + EHCI host + BOT MSC READ for FatFs.
 */

#include "gx_hw.h"
#include "print.h"

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
#define USBSTS_HCHALTED		BIT(12)
#define PORTSC_CCS		BIT(0)
#define PORTSC_PED		BIT(2)
#define PORTSC_PR		BIT(8)
#define PORTSC_PP		BIT(12)

static void delay_loops(u32 n)
{
	volatile u32 i;
	for (i = 0; i < n; i++)
		;
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
	{ 8, 1, 0x09245fd9u, 0x00004000u },
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
		if (r->index == 8)
			gx_clrset(0xa030a174u, 0, BIT(6) | BIT(7));
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

/* ---- Queue heads / qTDs in DDR (aligned) ---- */
#define QH_SIZE		64
#define QTD_SIZE	64

static u8 __attribute__((aligned(32))) qh_pool[QH_SIZE * 4];
static u8 __attribute__((aligned(32))) qtd_pool[QTD_SIZE * 8];
static u8 __attribute__((aligned(32))) ctrl_buf[512];
static u8 __attribute__((aligned(32))) bot_buf[512];

static u32 ehci_op;

static void cache_clean_invalidate(void *p, u32 n)
{
	(void)p;
	(void)n;
	/* Whole-cache flush is fine at this stage. */
	{
		u32 op = BIT(0) | BIT(1) | BIT(4) | BIT(5);
		__asm__ __volatile__("mtcr %0, cr17\n\tidly4" : : "r"(op) : "memory");
	}
}

static int ehci_init(void)
{
	u32 cap = USB_EHCI_BASE;
	u8 caplen = (u8)readl(cap + EHCI_CAPLENGTH);
	u32 i, sts, cmd;

	ehci_op = cap + caplen;

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
	return 0;
}

static int ehci_port_reset(void)
{
	u32 port = ehci_op + EHCI_PORTSC;
	u32 v, i;

	v = readl(port);
	v |= PORTSC_PP;
	writel(v, port);
	delay_loops(100000);

	v = readl(port);
	if (!(v & PORTSC_CCS))
		return -1;

	v |= PORTSC_PR;
	writel(v, port);
	delay_loops(500000);
	v = readl(port);
	v &= ~PORTSC_PR;
	writel(v, port);
	for (i = 0; i < 100000; i++) {
		v = readl(port);
		if (v & PORTSC_PED)
			return 0;
	}
	return (v & PORTSC_CCS) ? 0 : -1;
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
};

struct qh {
	u32 horiz;
	u32 epchar;
	u32 epcap;
	u32 cur_qtd;
	u32 next;
	u32 alt;
	u32 token;
	u32 buf[5];
};

static int ehci_async_xfer(u8 addr, u8 ep, u8 pid, void *data, u32 len, u8 toggle)
{
	struct qh *qh = (struct qh *)qh_pool;
	struct qtd *td = (struct qtd *)qtd_pool;
	u32 i, token;
	u32 phys_qh = (u32)qh;
	u32 phys_td = (u32)td;

	for (i = 0; i < sizeof(qh_pool); i++)
		qh_pool[i] = 0;
	for (i = 0; i < sizeof(qtd_pool); i++)
		qtd_pool[i] = 0;

	td->next = 1; /* terminate */
	td->alt = 1;
	token = (len << 16) | (toggle ? BIT(31) : 0) | (pid << 8) | BIT(7);
	td->token = token;
	td->buf[0] = (u32)data;

	qh->horiz = phys_qh | 0x2; /* QH type, point to self */
	qh->epchar = (addr & 0x7f) | ((ep & 0xf) << 8) | (64 << 16) | BIT(14);
	qh->epcap = 1 << 30; /* Mult = 1 */
	qh->cur_qtd = 0;
	qh->next = phys_td;
	qh->alt = 1;
	qh->token = 0;

	cache_clean_invalidate(qh_pool, sizeof(qh_pool));
	cache_clean_invalidate(qtd_pool, sizeof(qtd_pool));
	cache_clean_invalidate(data, len ? len : 1);

	writel(phys_qh, ehci_op + EHCI_ASYNCLISTADDR);
	writel(readl(ehci_op + EHCI_USBCMD) | USBCMD_ASE, ehci_op + EHCI_USBCMD);

	for (i = 0; i < 2000000; i++) {
		cache_clean_invalidate(td, sizeof(*td));
		if (!(td->token & BIT(7)))
			break;
	}
	writel(readl(ehci_op + EHCI_USBCMD) & ~USBCMD_ASE, ehci_op + EHCI_USBCMD);

	if (td->token & BIT(7))
		return -1;
	if (td->token & 0x7c)
		return -1;
	return 0;
}

static int ctrl_req(u8 addr, u8 bmRequestType, u8 bRequest, u16 wValue,
		    u16 wIndex, u16 wLength, void *data)
{
	u8 setup[8];

	setup[0] = bmRequestType;
	setup[1] = bRequest;
	setup[2] = wValue & 0xff;
	setup[3] = (wValue >> 8) & 0xff;
	setup[4] = wIndex & 0xff;
	setup[5] = (wIndex >> 8) & 0xff;
	setup[6] = wLength & 0xff;
	setup[7] = (wLength >> 8) & 0xff;

	if (ehci_async_xfer(addr, 0, 2 /* SETUP */, setup, 8, 0))
		return -1;
	if (wLength) {
		u8 pid = (bmRequestType & 0x80) ? 1 /* IN */ : 0 /* OUT */;
		if (ehci_async_xfer(addr, 0, pid, data, wLength, 1))
			return -1;
	}
	{
		u8 pid = (bmRequestType & 0x80) ? 0 : 1;
		if (ehci_async_xfer(addr, 0, pid, ctrl_buf, 0, 1))
			return -1;
	}
	return 0;
}

/* BOT */
static u8 msc_addr = 1;
static u8 msc_ep_in = 1;
static u8 msc_ep_out = 2;
static u32 msc_tag;
static int msc_ready;

static int bot_cmd(u8 *cb, u8 cblen, void *data, u32 len, int data_in)
{
	u8 cbw[31];
	u8 csw[13];
	u32 i;

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

	if (ehci_async_xfer(msc_addr, msc_ep_out, 0, cbw, 31, 0))
		return -1;
	if (len && data) {
		if (ehci_async_xfer(msc_addr, data_in ? msc_ep_in : msc_ep_out,
				    data_in ? 1 : 0, data, len, 0))
			return -1;
	}
	if (ehci_async_xfer(msc_addr, msc_ep_in, 1, csw, 13, 0))
		return -1;
	if (csw[12] != 0)
		return -1;
	return 0;
}

int usb_msc_init(void)
{
	u8 cb[16];
	u32 i;

	msc_ready = 0;
	if (gx_usb_pad_phy())
		return -1;
	if (ehci_init())
		return -1;
	if (ehci_port_reset())
		return -1;

	/* SET_ADDRESS */
	if (ctrl_req(0, 0x00, 0x05, msc_addr, 0, 0, 0))
		return -1;
	delay_loops(50000);

	/* GET_DESCRIPTOR device — best-effort */
	if (ctrl_req(msc_addr, 0x80, 0x06, 0x0100, 0, 18, ctrl_buf))
		return -1;

	/* SET_CONFIGURATION 1 */
	if (ctrl_req(msc_addr, 0x00, 0x09, 1, 0, 0, 0))
		return -1;

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
	return bot_cmd(cb, 10, buf, 512, 1);
}

int usb_msc_present(void)
{
	return msc_ready;
}
