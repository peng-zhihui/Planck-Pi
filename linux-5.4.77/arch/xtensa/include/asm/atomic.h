/*
 * include/asm-xtensa/atomic.h
 *
 * Atomic operations that C can't guarantee us.  Useful for resource counting..
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2001 - 2008 Tensilica Inc.
 */

#ifndef _XTENSA_ATOMIC_H
#define _XTENSA_ATOMIC_H

#include <linux/stringify.h>
#include <linux/types.h>
#include <asm/processor.h>
#include <asm/cmpxchg.h>
#include <asm/barrier.h>

#define ATOMIC_INIT(i)	{ (i) }

/*
 * This Xtensa implementation assumes that the right mechanism
 * for exclusion is for locking interrupts to level EXCM_LEVEL.
 *
 * Locking interrupts looks like this:
 *
 *    rsil a15, TOPLEVEL
 *    <code>
 *    wsr  a15, PS
 *    rsync
 *
 * Note that a15 is used here because the register allocation
 * done by the compiler is not guaranteed and a window overflow
 * may not occur between the rsil and wsr instructions. By using
 * a15 in the rsil, the machine is guaranteed to be in a state
 * where no register reference will cause an overflow.
 */

/**
 * atomic_read - read atomic variable
 * @v: pointer of type atomic_t
 *
 * Atomically reads the value of @v.
 */
#define atomic_read(v)		READ_ONCE((v)->counter)

/**
 * atomic_set - set atomic variable
 * @v: pointer of type atomic_t
 * @i: required value
 *
 * Atomically sets the value of @v to @i.
 */
#define atomic_set(v,i)		WRITE_ONCE((v)->counter, (i))

#if XCHAL_HAVE_EXCLUSIVE
#define ATOMIC_OP(op)							\
static inline void atomic_##op(int i, atomic_t *v)			\
{									\
	unsigned long tmp;						\
	int result;							\
									\
	__asm__ __volatile__(						\
			"1:     l32ex   %1, %3\n"			\
			"       " #op " %0, %1, %2\n"			\
			"       s32ex   %0, %3\n"			\
			"       getex   %0\n"				\
			"       beqz    %0, 1b\n"			\
			: "=&a" (result), "=&a" (tmp)			\
			: "a" (i), "a" (v)				\
			: "memory"					\
			);						\
}									\

#define ATOMIC_OP_RETURN(op)						\
static inline int atomic_##op##_return(int i, atomic_t *v)		\
{									\
	unsigned long tmp;						\
	int result;							\
									\
	__asm__ __volatile__(						\
			"1:     l32ex   %1, %3\n"			\
			"       " #op " %0, %1, %2\n"			\
			"       s32ex   %0, %3\n"			\
			"       getex   %0\n"				\
			"       beqz    %0, 1b\n"			\
			"       " #op " %0, %1, %2\n"			\
			: "=&a" (result), "=&a" (tmp)			\
			: "a" (i), "a" (v)				\
			: "memory"					\
			);						\
									\
	return result;							\
}

#define ATOMIC_FETCH_OP(op)						\
static inline int atomic_fetch_##op(int i, atomic_t *v)			\
{									\
	unsigned long tmp;						\
	int result;							\
									\
	__asm__ __volatile__(						\
			"1:     l32ex   %1, %3\n"			\
			"       " #op " %0, %1, %2\n"			\
			"       s32ex   %0, %3\n"			\
			"       getex   %0\n"				\
			"       beqz    %0, 1b\n"			\
			: "=&a" (result), "=&a" (tmp)			\
			: "a" (i), "a" (v)				\
			: "memory"					\
			);						\
									\
	return tmp;							\
}

#elif XCHAL_HAVE_S32C1I
#define ATOMIC_OP(op)							\
static inline void atomic_##op(int i, atomic_t * v)			\
{									\
	unsigned long tmp;						\
	int result;							\
									\
	__asm__ __volatile__(						\
			"1:     l32i    %1, %3, 0\n"			\
			"       wsr     %1, scompare1\n"		\
			"       " #op " %0, %1, %2\n"			\
			"       s32c1i  %0, %3, 0\n"			\
			"       bne     %0, %1, 1b\n"			\
			: "=&a" (result), "=&a" (tmp)			\
			: "a" (i), "a" (v)				\
			: "memory"					\
			);						\
}									\

#define ATOMIC_OP_RETURN(op)						\
static inline int atomic_##op##_return(int i, atomic_t * v)		\
{									\
	unsigned long tmp;						\
	int result;							\
									\
	__asm__ __volatile__(						\
			"1:     l32i    %1, %3, 0\n"			\
			"       wsr     %1, scompare1\n"		\
			"       " #op " %0, %1, %2\n"			\
			"       s32c1i  %0, %3, 0\n"			\
			"       bne     %0, %1, 1b\n"			\
			"       " #op " %0, %0, %2\n"			\
			: "=&a" (result), "=&a" (tmp)			\
			: "a" (i), "a" (v)				\
			: "memory"					\
			);						\
									\
	return result;							\
}

#define ATOMIC_FETCH_OP(op)						\
static inline int atomic_fetch_##op(int i, atomic_t * v)		\
{									\
	unsigned long tmp;						\
	int result;							\
									\
	__asm__ __volatile__(						\
			"1:     l32i    %1, %3, 0\n"			\
			"       wsr     %1, scompare1\n"		\
			"       " #op " %0, %1, %2\n"			\
			"       s32c1i  %0, %3, 0\n"			\
			"       bne     %0, %1, 1b\n"			\
			: "=&a" (result), "=&a" (tmp)			\
			: "a" (i), "a" (v)				\
			: "memory"					\
			);						\
									\
	return result;							\
}

#else /* XCHAL_HAVE_S32C1I */

#define ATOMIC_OP(op)							\
static inline void atomic_##op(int i, atomic_t * v)			\
{									\
	unsigned int vval;						\
									\
	__asm__ __volatile__(						\
			"       rsil    a15, "__stringify(TOPLEVEL)"\n"\
			"       l32i    %0, %2, 0\n"			\
			"       " #op " %0, %0, %1\n"			\
			"       s32i    %0, %2, 0\n"			\
			"       wsr     a15, ps\n"			\
			"       rsync\n"				\
			: "=&a" (vval)					\
			: "a" (i), "a" (v)				\
			: "a15", "memory"				\
			);						\
}									\

#define ATOMIC_OP_RETURN(op)						\
static inline int atomic_##op##_return(int i, atomic_t * v)		\
{									\
	unsigned int vval;						\
									\
	__asm__ __volatile__(						\
			"       rsil    a15,"__stringify(TOPLEVEL)"\n"	\
			"       l32i    %0, %2, 0\n"			\
			"       " #op " %0, %0, %1\n"			\
			"       s32i    %0, %2, 0\n"			\
			"       wsr     a15, ps\n"			\
			"       rsync\n"				\
			: "=&a" (vval)					\
			: "a" (i), "a" (v)				\
			: "a15", "memory"				\
			);						\
									\
	return vval;							\
}

#define ATOMIC_FETCH_OP(op)						\
static inline int atomic_fetch_##op(int i, atomic_t * v)		\
{									\
	unsigned int tmp, vval;						\
									\
	__asm__ __volatile__(						\
			"       rsil    a15,"__stringify(TOPLEVEL)"\n"	\
			"       l32i    %0, %3, 0\n"			\
			"       " #op " %1, %0, %2\n"			\
			"       s32i    %1, %3, 0\n"			\
			"       wsr     a15, ps\n"			\
			"       rsync\n"				\
			: "=&a" (vval), "=&a" (tmp)			\
			: "a" (i), "a" (v)				\
			: "a15", "memory"				\
			);						\
									\
	return vval;							\
}

#endif /* XCHAL_HAVE_S32C1I */

#define ATOMIC_OPS(op) ATOMIC_OP(op) ATOMIC_FETCH_OP(op) ATOMIC_OP_RETURN(op)

ATOMIC_OPS(add)
ATOMIC_OPS(sub)

#undef ATOMIC_OPS
#define ATOMIC_OPS(op) ATOMIC_OP(op) ATOMIC_FETCH_OP(op)

ATOMIC_OPS(and)
ATOMIC_OPS(or)
ATOMIC_OPS(xor)

#undef ATOMIC_OPS
#undef ATOMIC_FETCH_OP
#undef ATOMIC_OP_RETURN
#undef ATOMIC_OP

#define atomic_cmpxchg(v, o, n) ((int)cmpxchg(&((v)->counter), (o), (n)))
#define atomic_xchg(v, new) (xchg(&((v)->counter), new))

#endif /* _XTENSA_ATOMIC_H */
