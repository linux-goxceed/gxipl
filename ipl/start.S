/* SPDX-License-Identifier: MIT */
/*
 * Shared GX6702/GX6706 open IPL entry, C-SKY CK610 ABIv1, little endian.
 *
 * The BootROM enters at 0x00100000.  A relative branch in the first vector
 * slot transfers to the implementation at 0x00100040; exception slots point
 * at a safe spin handler.
 */

	.section .vectors, "ax"
	.global _vectors
_vectors:
	/*
	 * The vendor's jsri opcodes use a PC-relative literal pool.  Copying those
	 * opcodes into a newly linked image silently picks up the wrong target.
	 */
	br _start
	br _start
	.rept 12
	.long _fault
	.endr
	.space 12, 0

	.section .text.start, "ax"
	.global _start
_start:
	/*
	 * Early always-on/reset controls written by the known-good IPL.
	 * Verbose builds suppress IRUN crumbs from C after the stack is up;
	 * keep this path silent so the GoXceed banner is the first IPL text.
	 */
	lrw   r1, 0x04d00204
	lrw   r2, 0xffffffff
	st.w  r2, (r1, 0)
	lrw   r1, 0x04d00300
	movi  r2, 16
	st.w  r2, (r1, 0)

	/* Disable exceptions while clock, DDR and the MMU are transitional. */
	psrclr ie, fe, ee
	psrclr af
	lrw   r3, _vectors
	mtcr  r3, cr1

	/* Invalidate I/D cache, then select the vendor-proven CK610 cache mode. */
	mfcr  r4, cr17
	bseti r4, r4, 0
	bseti r4, r4, 1
	bseti r4, r4, 4
	mtcr  r4, cr17
	mfcr  r4, cr18
	bclri r4, r4, 0
	bclri r4, r4, 1
	bseti r4, r4, 2
	bclri r4, r4, 3
	bseti r4, r4, 6
	mtcr  r4, cr18
	mov   r0, r0
	mov   r0, r0
	mov   r0, r0
	mov   r0, r0

	/* Disable all interrupt sources and use the top of internal SRAM. */
	lrw   r1, 0x00500050
	lrw   r2, 0xffffffff
	st.w  r2, (r1, 0)
	lrw   r1, 0x00500054
	st.w  r2, (r1, 0)
	lrw   r1, 0x00104000
	mov   r0, r1

	/* Zero NOLOAD state before entering C. */
	lrw   r5, __bss_start
	lrw   r6, __bss_end
	movi  r1, 0
2:
	cmpne r5, r6
	bf    3f
	st.w  r1, (r5, 0)
	addi  r5, 4
	br    2b
3:
	jsri  ipl_pre_mmu
	cmpnei r2, 0
	bt    _fault

	/*
	 * Recovered CK610 MMU layout.  It preserves an identity mapping for this
	 * SRAM IPL, maps DDR at 0x90000000, and exposes uncached MMIO at 0xa0000000.
	 */
	bgeni r2, 27
	cpseti cp15
	cpwcr r2, cpcr8
	bgeni r2, 26
	cpwcr r2, cpcr8
	movi  r2, 0
	cpseti cp15
	cpwcr r2, cpcr5
	lrw   r2, 0xff
	lsli  r2, r2, 13
	cpwcr r2, cpcr6
	movi  r2, 0
	cpwcr r2, cpcr4
	movi  r2, 31
	cpwcr r2, cpcr2
	lrw   r2, 0x00100000
	lsri  r2, r2, 6
	movi  r3, 31
	or    r2, r2, r3
	cpwcr r2, cpcr3
	bgeni r2, 28
	cpwcr r2, cpcr8
	movi  r2, 0
	cpwcr r2, cpcr8

	/* Enable the MMU/cache mode and move the stack into mapped DDR. */
	mfcr  r4, cr30
	bseti r4, r4, 2
	bseti r4, r4, 3
	mtcr  r4, cr30
	mfcr  r4, cr18
	bclri r4, r4, 1
	bseti r4, r4, 0
	bseti r4, r4, 2
	bseti r4, r4, 3
	bseti r4, r4, 4
	bseti r4, r4, 5
	bseti r4, r4, 6
	bseti r4, r4, 7
	bclri r4, r4, 18
	mtcr  r4, cr18

	lrw   r1, 0x93ce6400
	mov   r0, r1
	jsri  ipl_post_mmu

_fault:
	br    _fault
