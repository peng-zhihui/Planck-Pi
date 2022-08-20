
#ifndef __ASSEMBLY__
#define __ASSEMBLY__
#endif
#include <linux/linkage.h>
#include <asm/assembler.h>

/*
 * c code declared as follows:
 * int flush_clean_user_range(long start, long end);
 */
ENTRY(flush_clean_user_range)
	.macro	dcache_line_size, reg, tmp
	mrc	p15, 1, \tmp, c0, c0, 0		@ read CSIDR
	and	\tmp, \tmp, #7			@ cache line size encoding
	mov	\reg, #16			@ size offset
	mov	\reg, \reg, lsl \tmp		@ actual cache line size
	.endm

    .text
    .globl flush_clean_user_range
    .globl flush_dcache_all
flush_clean_user_range:
    dcache_line_size r2, r3
    sub	r3, r2, #1
    bic	r0, r0, r3
1:
    USER(	mcr	p15, 0, r0, c7, c14, 1	)	@ clean and flush D line to the point of unification
    add	r0, r0, r2
2:
    cmp	r0, r1
    blo	1b
    mov	r0, #0
    dsb
    mov	pc, lr

/*
 * Fault handling for the cache operation above. If the virtual address in r0
 * is not mapped, just try the next page.
 */
9001:
    mov	r0, r0, lsr #12
    mov	r0, r0, lsl #12
    add	r0, r0, #4096
    b	2b
ENDPROC(flush_clean_user_range)

/*
 *	flush_dcache_all()
 *
 *	Flush the whole D-cache.
 *
 *	Corrupted registers: r0-r7, r9-r11 (r6 only in Thumb mode)
 *
 *	- mm    - mm_struct describing address space
 */
ENTRY(flush_dcache_all)
	stmfd	sp!, {r0 - r12, lr}
	dmb					@ ensure ordering with previous memory accesses
	mrc	p15, 1, r0, c0, c0, 1		@ read clidr
	ands	r3, r0, #0x7000000		@ extract loc from clidr
	mov	r3, r3, lsr #23			@ left align loc bit field
	beq	finished			@ if loc is 0, then no need to clean
	mov	r10, #0				@ start clean at cache level 0
loop1:
	add	r2, r10, r10, lsr #1		@ work out 3x current cache level
	mov	r1, r0, lsr r2			@ extract cache type bits from clidr
	and	r1, r1, #7			@ mask of the bits for current cache only
	cmp	r1, #2				@ see what cache we have at this level
	blt	skip				@ skip if no cache, or just i-cache
	mcr	p15, 2, r10, c0, c0, 0		@ select current cache level in cssr
	isb					@ isb to sych the new cssr&csidr
	mrc	p15, 1, r1, c0, c0, 0		@ read the new csidr
	and	r2, r1, #7			@ extract the length of the cache lines
	add	r2, r2, #4			@ add 4 (line length offset)
	ldr	r4, =0x3ff
	ands	r4, r4, r1, lsr #3		@ find maximum number on the way size
	clz	r5, r4				@ find bit position of way size increment
	ldr	r7, =0x7fff
	ands	r7, r7, r1, lsr #13		@ extract max number of the index size
loop2:
	mov	r9, r4				@ create working copy of max way size
loop3:
 ARM(	orr	r11, r10, r9, lsl r5	)	@ factor way and cache number into r11
 THUMB(	lsl	r6, r9, r5		)
 THUMB(	orr	r11, r10, r6		)	@ factor way and cache number into r11
 ARM(	orr	r11, r11, r7, lsl r2	)	@ factor index number into r11
 THUMB(	lsl	r6, r7, r2		)
 THUMB(	orr	r11, r11, r6		)	@ factor index number into r11
	mcr	p15, 0, r11, c7, c14, 2		@ clean & invalidate by set/way
	subs	r9, r9, #1			@ decrement the way
	bge	loop3
	subs	r7, r7, #1			@ decrement the index
	bge	loop2
skip:
	add	r10, r10, #2			@ increment cache number
	cmp	r3, r10
	bgt	loop1
finished:
	mov	r10, #0				@ swith back to cache level 0
	mcr	p15, 2, r10, c0, c0, 0		@ select current cache level in cssr
	dsb
	isb

	ldmfd	sp!, {r0 - r12, lr}
	mov	pc, lr
ENDPROC(flush_dcache_all)

/*
 * int flush_user_range(long start, long end);
 *
 * flush user space range. just flush, not write back.
 */
ENTRY(flush_user_range)
	dcache_line_size r2, r3
	sub	r3, r2, #1

	tst	r0, r3
	bic	r0, r0, r3
	USER(mcrne p15, 0, r0, c7, c14, 1)	@ clean & invalidate D / U line

	tst	r1, r3
	bic	r1, r1, r3
	USER(mcrne p15, 0, r1, c7, c14, 1)	@ clean & invalidate D / U line

1:
	USER(mcr p15, 0, r0, c7, c6, 1)		@ invalidate D / U line
	add	r0, r0, r2
2:
	cmp	r0, r1
	blo	1b
	mov	r0, #0
	dsb
	mov	pc, lr

	/*
	 * Fault handling for the cache operation above. If the virtual address in r0
	 * is not mapped, just try the next page.
	 */
9001:
	mov	r0, r0, lsr #12
	mov	r0, r0, lsl #12
	add	r0, r0, #4096
	b	2b
ENDPROC(flush_user_range)
