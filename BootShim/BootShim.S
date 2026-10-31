/*
 * BootShim for Samsung Exynos5420 - Galaxy Tab Pro 12.2 WiFi (v1awifi).
 *
 * Milestone 2: hand off to the EDK2 firmware device.  The Samsung
 * bootloader (LK) loads the KERNEL partition as a raw zImage-style image
 * and jumps to its start.  This shim:
 *
 *   - carries a valid zImage magic (0x016f2818 @ 0x24) so the BL is happy,
 *   - puts the CPU into a known state (SVC, IRQ/FIQ off, MMU/caches off),
 *   - stops the Samsung software watchdog,
 *   - preserves the Linux boot ABI registers (r1 = machine type,
 *     r2 = ATAG list) at BOOT_PARAMS_BASE for a later kernel boot,
 *   - discovers the framebuffer the BL already programmed by reading the
 *     FIMD1 window registers (v1awifi's panel is driven by FIMD1 @
 *     0x14400000, unlike Exynos4412's FIMD0 @ 0x11C00000),
 *   - relocates the attached EDK2 firmware device (FD) to UEFI_BASE
 *     (position independent: works no matter where the BL loaded us;
 *     copies backwards when the destination is above the source so an
 *     overlapping copy cannot corrupt itself),
 *   - jumps to the FD entry stub that GenFv placed at FD offset 0.
 *
 * Modelled on Exynos441xPkg's BootShim.
 */

/* Exynos5420 peripheral bases (see android_kernel_samsung_exynos5420
 * arch/arm/mach-exynos/include/mach/map.h and u-boot cpu.h). */
#define FIMD1_BASE      0x14400000
#define WATCHDOG_BASE   0x101D0000

/* Must match Include/ExynosBootParams.h. */
#define BOOT_PARAMS_BASE   0x4FEF0000
#define BOOT_PARAMS_MAGIC  0x41505442	/* 'BTPA' */

/* Stage colours, STR'd as 32-bit words into an XRGB8888 buffer: the word
 * 0x00RRGGBB lands as memory bytes [BB,GG,RR,00] = that colour on screen. */
#define COLOR_RED       0x00FF0000
#define COLOR_GREEN     0x0000FF00
#define COLOR_MAGENTA   0x00FF00FF

#define DSB .byte 0x4f, 0xf0, 0x7f, 0xf5
#define ISB .byte 0x6f, 0xf0, 0x7f, 0xf5

.text
.globl _start
_start:
	b	_Reset

	/* ARM zImage header: magic number at offset 0x24 */
.org 0x24
	.word 0x016f2818
	.word 0x00000000          /* zImage "start" / end-of-setup words  */
	.word 0x00000000
	.word 0x00000000
	.word 0x00000000
	.word 0x00000000
	.word 0x00000000          /* zImage ends at 0x38..0x3C           */

.org 0x40

_Reset:
	/* Enter SVC mode, IRQ + FIQ masked */
	mov	r0, #0xD3		/* CPSR_MODE_SVC | I | F */
	msr	CPSR_cxsf, r0

	/*
	 * Force physical, uncached accesses before touching the display:
	 * turn MMU + D-cache off and flush the TLB.  (If the BL ran with
	 * caches on, dirty lines are abandoned on purpose - it never
	 * comes back here.)
	 */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 0)	/* M - MMU off */
	bic	r0, r0, #(1 << 2)	/* C - D-cache off */
	mcr	p15, 0, r0, c1, c0, 0
	mov	r0, #0
	mcr	p15, 0, r0, c8, c7, 0	/* TLBIALL */
	dsb
	isb

	/*
	 * Preserve the Linux boot ABI registers the bootloader entered with
	 * (r1 = machine type, r2 = ATAG list) for a later downstream-kernel
	 * boot.  Done only now, with the D-cache off, so the stores reach
	 * DRAM; r1/r2 are reused as scratch right below.  Layout: Include/
	 * ExynosBootParams.h (Magic, MachType, AtagsBase, AtagsSize); PrePi
	 * copies the ATAG list behind it.
	 */
	ldr	r3, =BOOT_PARAMS_BASE
	ldr	r4, =BOOT_PARAMS_MAGIC
	str	r4, [r3, #0]
	str	r1, [r3, #4]
	str	r2, [r3, #8]
	mov	r4, #0
	str	r4, [r3, #12]
	dsb

	/*
	 * Stop the Samsung bootloader's software watchdog so slow phases
	 * cannot trigger a reset.  Exynos5420 watchdog @ 0x101D0000:
	 * WTCON bit 5 = 0 disables the watchdog.
	 */
	ldr	r1, =WATCHDOG_BASE
	ldr	r0, [r1]
	bic	r0, r0, #(1 << 5)
	str	r0, [r1]

	/*
	 * Set up a flat, unaligned-tolerant environment: MMU off, D-cache
	 * off, alignment off, low vectors, branch prediction + I-cache on.
	 */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 0)		/* M   - MMU off           */
	bic	r0, r0, #(1 << 2)		/* C   - D-cache off       */
	bic	r0, r0, #(1 << 1)		/* A   - alignment off     */
	bic	r0, r0, #(1 << 13)		/* V   - low vectors       */
	bic	r0, r0, #(1 << 28)		/* TRE                     */
	bic	r0, r0, #(1 << 29)		/* AFE                     */
	orr	r0, r0, #(1 << 12)		/* I   - I-cache on        */
	orr	r0, r0, #(1 << 11)		/* Z   - branch prediction */
	mcr	p15, 0, r0, c1, c0, 0
	isb

	/* Invalidate TLBs, I-cache and BTB */
	mov	r0, #0
	mcr	p15, 0, r0, c8, c7, 0		/* TLBIALL   */
	mcr	p15, 0, r0, c7, c5, 6		/* BPIALL    */
	mcr	p15, 0, r0, c7, c5, 0		/* ICIALLU   */
	dsb
	isb

	/*
	 * Discover the framebuffer the BL programmed.  The S3C-FB register
	 * layout on Exynos5 FIMD1 is identical to Exynos4 FIMD0, so we scan
	 * the window registers the same way (offsets from u-boot's struct
	 * exynos_fb):
	 *
	 *   WINCON<n>       0x20 + 4n   bit 0 = window enabled
	 *   vidw00add0b0+n  0xA0 + 8n   buffer start address
	 *   vidw00add1b0+n  0xD0 + 8n   buffer end address
	 *
	 * Windows 0/1 pack the address registers with an 8-byte stride;
	 * windows 2..4 pack them differently.  Handle both.
	 *
	 * On success r12 = fb start, r14 = buffer size in bytes.
	 */
	mov	r12, #0			/* saved fb start (0 = none yet) */
	mov	r14, #0			/* saved fb size                */
	ldr	r6, =FIMD1_BASE
	mov	r7, #0			/* window index 0..4 */
_ScanFbWindow:
	/* WINCON<n> = 0x20 + 4n */
	add	r8, r6, #0x20
	add	r8, r8, r7, lsl #2
	ldr	r2, [r8]
	tst	r2, #1			/* ENWIN? */
	beq	_ScanFbNext

	/* start address register: windows 0/1 at 0xA0 + 8n, 2..4 at 0xB0 + 8(n-2) */
	cmp	r7, #2
	addlo	r3, r6, #0xA0
	addlo	r3, r3, r7, lsl #3
	addhs	r3, r6, #0xB0
	subhs	r9, r7, #2
	addhs	r3, r3, r9, lsl #3
	ldr	r0, [r3]		/* start */

	/* end address register: windows 0/1 at 0xD0 + 8n, 2..4 at 0xE0 + 8(n-2) */
	cmp	r7, #2
	addlo	r3, r6, #0xD0
	addlo	r3, r3, r7, lsl #3
	addhs	r3, r6, #0xE0
	subhs	r9, r7, #2
	addhs	r3, r3, r9, lsl #3
	ldr	r1, [r3]		/* end */

	/* Sanity: start in DRAM (0x20000000..0xF0000000) and end > start */
	ldr	r4, =0x20000000
	cmp	r0, r4
	blo	_ScanFbNext
	ldr	r4, =0xF0000000
	cmp	r0, r4
	bhs	_ScanFbNext
	cmp	r1, r0
	bls	_ScanFbNext		/* end <= start: nonsense, skip */

	/* size = end - start (+4: end is inclusive on some revisions) */
	sub	r14, r1, r0
	add	r14, r14, #4
	mov	r12, r0			/* remember the first valid window */
	b	_ScanFbDone
_ScanFbNext:
	add	r7, r7, #1
	cmp	r7, #5
	blo	_ScanFbWindow
_ScanFbDone:

	/* Stage RED: framebuffer discovered and we are executing. */
	mov	r0, r12			/* fb start */
	mov	r1, r14			/* buffer size */
	ldr	r2, =COLOR_RED
	bl	_Paint

	ldr	r5, =0x10000
_Spin1:
	subs	r5, r5, #1
	bne	_Spin1

	/* Stage GREEN: CPU cleanup done, about to relocate the FD. */
	mov	r0, r12
	mov	r1, r14
	ldr	r2, =COLOR_GREEN
	bl	_Paint

	/*
	 * Relocate the attached FD to UEFI_BASE.  r0 = runtime address of
	 * _Payload (i.e. wherever the BL loaded us, position independent),
	 * r1 = destination.  If the copy destination is above the source the
	 * two ranges can overlap (the payload the BL loaded is only ~1 MB
	 * below), so copy backwards - highest word first - which is always
	 * safe for an overlapping range.  Destination below source cannot
	 * self-overwrite, so a plain forward copy is fine there.
	 */
	adr	r0, _Payload
	ldr	r1, =UEFI_BASE
	cmp	r0, r1
	beq	_Entry			/* already in place, nothing to do */
	ldr	r2, =UEFI_SIZE

	cmp	r1, r0
	bhi	_CopyBack

_CopyFwd:
	ldr	r3, [r0], #4
	str	r3, [r1], #4
	subs	r2, r2, #4
	bgt	_CopyFwd
	b	_Entry

_CopyBack:
	add	r0, r0, r2		/* src end */
	add	r1, r1, r2		/* dst end */
1:
	ldr	r3, [r0, #-4]!
	str	r3, [r1, #-4]!
	subs	r2, r2, #4
	bgt	1b

_Entry:
	/* Stage MAGENTA: FD relocated to UEFI_BASE, about to jump in. */
	mov	r0, r12
	mov	r1, r14
	ldr	r2, =COLOR_MAGENTA
	bl	_Paint

	/* The freshly copied code at UEFI_BASE must not be fetched from any
	 * stale I-cache/BTB lines: invalidate before jumping into it. */
	mov	r0, #0
	mcr	p15, 0, r0, c8, c7, 0		/* TLBIALL   */
	mcr	p15, 0, r0, c7, c5, 6		/* BPIALL    */
	mcr	p15, 0, r0, c7, c5, 0		/* ICIALLU   */
	dsb
	isb

	/* Pass the framebuffer range to the payload in r0/r1 (PrePi ignores
	 * r0/r1 and rediscovers the buffer itself; kept for any bare-metal
	 * payload that wants it). */
	mov	r0, r12
	mov	r1, r14
	ldr	r5, =UEFI_BASE
	bx	r5

/*
 * _Paint: fill [r0, r0+r1) with the 32-bit word in r2, uncached.
 * Assumes 32bpp (the BL programs the splash in XRGB8888).  Uses r3/r4/r5.
 */
_Paint:
	cmp	r0, #0
	moveq	pc, lr
	cmp	r1, #0
	moveq	pc, lr
	/* Clamp a runaway size to 64 MB so a bad END register cannot walk
	 * all of DRAM. */
	ldr	r3, =0x04000000
	cmp	r1, r3
	movhi	r1, r3
	add	r1, r0, r1		/* r1 = exclusive end */
	mov	r3, r0			/* cursor */
1:
	str	r2, [r3], #4
	cmp	r3, r1
	blo	1b
	dsb
	mov	pc, lr

.ltorg

	.align 5
_Payload:
