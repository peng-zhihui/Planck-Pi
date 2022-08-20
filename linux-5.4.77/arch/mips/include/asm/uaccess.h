/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1996, 1997, 1998, 1999, 2000, 03, 04 by Ralf Baechle
 * Copyright (C) 1999, 2000 Silicon Graphics, Inc.
 * Copyright (C) 2007  Maciej W. Rozycki
 * Copyright (C) 2014, Imagination Technologies Ltd.
 */
#ifndef _ASM_UACCESS_H
#define _ASM_UACCESS_H

#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/asm-eva.h>
#include <asm/extable.h>

/*
 * The fs value determines whether argument validity checking should be
 * performed or not.  If get_fs() == USER_DS, checking is performed, with
 * get_fs() == KERNEL_DS, checking is bypassed.
 *
 * For historical reasons, these macros are grossly misnamed.
 */
#ifdef CONFIG_32BIT

#ifdef CONFIG_KVM_GUEST
#define __UA_LIMIT 0x40000000UL
#else
#define __UA_LIMIT 0x80000000UL
#endif

#define __UA_ADDR	".word"
#define __UA_LA		"la"
#define __UA_ADDU	"addu"
#define __UA_t0		"$8"
#define __UA_t1		"$9"

#endif /* CONFIG_32BIT */

#ifdef CONFIG_64BIT

extern u64 __ua_limit;

#define __UA_LIMIT	__ua_limit

#define __UA_ADDR	".dword"
#define __UA_LA		"dla"
#define __UA_ADDU	"daddu"
#define __UA_t0		"$12"
#define __UA_t1		"$13"

#endif /* CONFIG_64BIT */

/*
 * USER_DS is a bitmask that has the bits set that may not be set in a valid
 * userspace address.  Note that we limit 32-bit userspace to 0x7fff8000 but
 * the arithmetic we're doing only works if the limit is a power of two, so
 * we use 0x80000000 here on 32-bit kernels.  If a process passes an invalid
 * address in this range it's the process's problem, not ours :-)
 */

#ifdef CONFIG_KVM_GUEST
#define KERNEL_DS	((mm_segment_t) { 0x80000000UL })
#define USER_DS		((mm_segment_t) { 0xC0000000UL })
#else
#define KERNEL_DS	((mm_segment_t) { 0UL })
#define USER_DS		((mm_segment_t) { __UA_LIMIT })
#endif

#define get_fs()	(current_thread_info()->addr_limit)
#define set_fs(x)	(current_thread_info()->addr_limit = (x))

#define segment_eq(a, b)	((a).seg == (b).seg)

/*
 * eva_kernel_access() - determine whether kernel memory access on an EVA system
 *
 * Determines whether memory accesses should be performed to kernel memory
 * on a system using Extended Virtual Addressing (EVA).
 *
 * Return: true if a kernel memory access on an EVA system, else false.
 */
static inline bool eva_kernel_access(void)
{
	if (!IS_ENABLED(CONFIG_EVA))
		return false;

	return uaccess_kernel();
}

/*
 * Is a address valid? This does a straightforward calculation rather
 * than tests.
 *
 * Address valid if:
 *  - "addr" doesn't have any high-bits set
 *  - AND "size" doesn't have any high-bits set
 *  - AND "addr+size" doesn't have any high-bits set
 *  - OR we are in kernel mode.
 *
 * __ua_size() is a trick to avoid runtime checking of positive constant
 * sizes; for those we already know at compile time that the size is ok.
 */
#define __ua_size(size)							\
	((__builtin_constant_p(size) && (signed long) (size) > 0) ? 0 : (size))

/*
 * access_ok: - Checks if a user space pointer is valid
 * @addr: User space pointer to start of block to check
 * @size: Size of block to check
 *
 * Context: User context only. This function may sleep if pagefaults are
 *          enabled.
 *
 * Checks if a pointer to a block of memory in user space is valid.
 *
 * Returns true (nonzero) if the memory block may be valid, false (zero)
 * if it is definitely invalid.
 *
 * Note that, depending on architecture, this function probably just
 * checks that the pointer is in the user space range - after calling
 * this function, memory access functions may still return -EFAULT.
 */

static inline int __access_ok(const void __user *p, unsigned long size)
{
	unsigned long addr = (unsigned long)p;
	return (get_fs().seg & (addr | (addr + size) | __ua_size(size))) == 0;
}

#define access_ok(addr, size)					\
	likely(__access_ok((addr), (size)))

/*
 * put_user: - Write a simple value into user space.
 * @x:	 Value to copy to user space.
 * @ptr: Destination address, in user space.
 *
 * Context: User context only. This function may sleep if pagefaults are
 *          enabled.
 *
 * This macro copies a single simple value from kernel space to user
 * space.  It supports simple types like char and int, but not larger
 * data types like structures or arrays.
 *
 * @ptr must have pointer-to-simple-variable type, and @x must be assignable
 * to the result of dereferencing @ptr.
 *
 * Returns zero on success, or -EFAULT on error.
 */
#define put_user(x,ptr) \
	__put_user_check((x), (ptr), sizeof(*(ptr)))

/*
 * get_user: - Get a simple variable from user space.
 * @x:	 Variable to store result.
 * @ptr: Source address, in user space.
 *
 * Context: User context only. This function may sleep if pagefaults are
 *          enabled.
 *
 * This macro copies a single simple variable from user space to kernel
 * space.  It supports simple types like char and int, but not larger
 * data types like structures or arrays.
 *
 * @ptr must have pointer-to-simple-variable type, and the result of
 * dereferencing @ptr must be assignable to @x without a cast.
 *
 * Returns zero on success, or -EFAULT on error.
 * On error, the variable @x is set to zero.
 */
#define get_user(x,ptr) \
	__get_user_check((x), (ptr), sizeof(*(ptr)))

/*
 * __put_user: - Write a simple value into user space, with less checking.
 * @x:	 Value to copy to user space.
 * @ptr: Destination address, in user space.
 *
 * Context: User context only. This function may sleep if pagefaults are
 *          enabled.
 *
 * This macro copies a single simple value from kernel space to user
 * space.  It supports simple types like char and int, but not larger
 * data types like structures or arrays.
 *
 * @ptr must have pointer-to-simple-variable type, and @x must be assignable
 * to the result of dereferencing @ptr.
 *
 * Caller must check the pointer with access_ok() before calling this
 * function.
 *
 * Returns zero on success, or -EFAULT on error.
 */
#define __put_user(x,ptr) \
	__put_user_nocheck((x), (ptr), sizeof(*(ptr)))

/*
 * __get_user: - Get a simple variable from user space, with less checking.
 * @x:	 Variable to store result.
 * @ptr: Source address, in user space.
 *
 * Context: User context only. This function may sleep if pagefaults are
 *          enabled.
 *
 * This macro copies a single simple variable from user space to kernel
 * space.  It supports simple types like char and int, but not larger
 * data types like structures or arrays.
 *
 * @ptr must have pointer-to-simple-variable type, and the result of
 * dereferencing @ptr must be assignable to @x without a cast.
 *
 * Caller must check the pointer with access_ok() before calling this
 * function.
 *
 * Returns zero on success, or -EFAULT on error.
 * On error, the variable @x is set to zero.
 */
#define __get_user(x,ptr) \
	__get_user_nocheck((x), (ptr), sizeof(*(ptr)))

struct __large_struct { unsigned long buf[100]; };
#define __m(x) (*(struct __large_struct __user *)(x))

/*
 * Yuck.  We need two variants, one for 64bit operation and one
 * for 32 bit mode and old iron.
 */
#ifndef CONFIG_EVA
#define __get_kernel_common(val, size, ptr) __get_user_common(val, size, ptr)
#else
/*
 * Kernel specific functions for EVA. We need to use normal load instructions
 * to read data from kernel when operating in EVA mode. We use these macros to
 * avoid redefining __get_user_asm for EVA.
 */
#undef _loadd
#undef _loadw
#undef _loadh
#undef _loadb
#ifdef CONFIG_32BIT
#define _loadd			_loadw
#else
#define _loadd(reg, addr)	"ld " reg ", " addr
#endif
#define _loadw(reg, addr)	"lw " reg ", " addr
#define _loadh(reg, addr)	"lh " reg ", " addr
#define _loadb(reg, addr)	"lb " reg ", " addr

#define __get_kernel_common(val, size, ptr)				\
do {									\
	switch (size) {							\
	case 1: __get_data_asm(val, _loadb, ptr); break;		\
	case 2: __get_data_asm(val, _loadh, ptr); break;		\
	case 4: __get_data_asm(val, _loadw, ptr); break;		\
	case 8: __GET_DW(val, _loadd, ptr); break;			\
	default: __get_user_unknown(); break;				\
	}								\
} while (0)
#endif

#ifdef CONFIG_32BIT
#define __GET_DW(val, insn, ptr) __get_data_asm_ll32(val, insn, ptr)
#endif
#ifdef CONFIG_64BIT
#define __GET_DW(val, insn, ptr) __get_data_asm(val, insn, ptr)
#endif

extern void __get_user_unknown(void);

#define __get_user_common(val, size, ptr)				\
do {									\
	switch (size) {							\
	case 1: __get_data_asm(val, user_lb, ptr); break;		\
	case 2: __get_data_asm(val, user_lh, ptr); break;		\
	case 4: __get_data_asm(val, user_lw, ptr); break;		\
	case 8: __GET_DW(val, user_ld, ptr); break;			\
	default: __get_user_unknown(); break;				\
	}								\
} while (0)

#define __get_user_nocheck(x, ptr, size)				\
({									\
	int __gu_err;							\
									\
	if (eva_kernel_access()) {					\
		__get_kernel_common((x), size, ptr);			\
	} else {							\
		__chk_user_ptr(ptr);					\
		__get_user_common((x), size, ptr);			\
	}								\
	__gu_err;							\
})

#define __get_user_check(x, ptr, size)					\
({									\
	int __gu_err = -EFAULT;						\
	const __typeof__(*(ptr)) __user * __gu_ptr = (ptr);		\
									\
	might_fault();							\
	if (likely(access_ok( __gu_ptr, size))) {		\
		if (eva_kernel_access())				\
			__get_kernel_common((x), size, __gu_ptr);	\
		else							\
			__get_user_common((x), size, __gu_ptr);		\
	} else								\
		(x) = 0;						\
									\
	__gu_err;							\
})

#define __get_data_asm(val, insn, addr)					\
{									\
	long __gu_tmp;							\
									\
	__asm__ __volatile__(						\
	"1:	"insn("%1", "%3")"				\n"	\
	"2:							\n"	\
	"	.insn						\n"	\
	"	.section .fixup,\"ax\"				\n"	\
	"3:	li	%0, %4					\n"	\
	"	move	%1, $0					\n"	\
	"	j	2b					\n"	\
	"	.previous					\n"	\
	"	.section __ex_table,\"a\"			\n"	\
	"	"__UA_ADDR "\t1b, 3b				\n"	\
	"	.previous					\n"	\
	: "=r" (__gu_err), "=r" (__gu_tmp)				\
	: "0" (0), "o" (__m(addr)), "i" (-EFAULT));			\
									\
	(val) = (__typeof__(*(addr))) __gu_tmp;				\
}

/*
 * Get a long long 64 using 32 bit registers.
 */
#define __get_data_asm_ll32(val, insn, addr)				\
{									\
	union {								\
		unsigned long long	l;				\
		__typeof__(*(addr))	t;				\
	} __gu_tmp;							\
									\
	__asm__ __volatile__(						\
	"1:	" insn("%1", "(%3)")"				\n"	\
	"2:	" insn("%D1", "4(%3)")"				\n"	\
	"3:							\n"	\
	"	.insn						\n"	\
	"	.section	.fixup,\"ax\"			\n"	\
	"4:	li	%0, %4					\n"	\
	"	move	%1, $0					\n"	\
	"	move	%D1, $0					\n"	\
	"	j	3b					\n"	\
	"	.previous					\n"	\
	"	.section	__ex_table,\"a\"		\n"	\
	"	" __UA_ADDR "	1b, 4b				\n"	\
	"	" __UA_ADDR "	2b, 4b				\n"	\
	"	.previous					\n"	\
	: "=r" (__gu_err), "=&r" (__gu_tmp.l)				\
	: "0" (0), "r" (addr), "i" (-EFAULT));				\
									\
	(val) = __gu_tmp.t;						\
}

#ifndef CONFIG_EVA
#define __put_kernel_common(ptr, size) __put_user_common(ptr, size)
#else
/*
 * Kernel specific functions for EVA. We need to use normal load instructions
 * to read data from kernel when operating in EVA mode. We use these macros to
 * avoid redefining __get_data_asm for EVA.
 */
#undef _stored
#undef _storew
#undef _storeh
#undef _storeb
#ifdef CONFIG_32BIT
#define _stored			_storew
#else
#define _stored(reg, addr)	"ld " reg ", " addr
#endif

#define _storew(reg, addr)	"sw " reg ", " addr
#define _storeh(reg, addr)	"sh " reg ", " addr
#define _storeb(reg, addr)	"sb " reg ", " addr

#define __put_kernel_common(ptr, size)					\
do {									\
	switch (size) {							\
	case 1: __put_data_asm(_storeb, ptr); break;			\
	case 2: __put_data_asm(_storeh, ptr); break;			\
	case 4: __put_data_asm(_storew, ptr); break;			\
	case 8: __PUT_DW(_stored, ptr); break;				\
	default: __put_user_unknown(); break;				\
	}								\
} while(0)
#endif

/*
 * Yuck.  We need two variants, one for 64bit operation and one
 * for 32 bit mode and old iron.
 */
#ifdef CONFIG_32BIT
#define __PUT_DW(insn, ptr) __put_data_asm_ll32(insn, ptr)
#endif
#ifdef CONFIG_64BIT
#define __PUT_DW(insn, ptr) __put_data_asm(insn, ptr)
#endif

#define __put_user_common(ptr, size)					\
do {									\
	switch (size) {							\
	case 1: __put_data_asm(user_sb, ptr); break;			\
	case 2: __put_data_asm(user_sh, ptr); break;			\
	case 4: __put_data_asm(user_sw, ptr); break;			\
	case 8: __PUT_DW(user_sd, ptr); break;				\
	default: __put_user_unknown(); break;				\
	}								\
} while (0)

#define __put_user_nocheck(x, ptr, size)				\
({									\
	__typeof__(*(ptr)) __pu_val;					\
	int __pu_err = 0;						\
									\
	__pu_val = (x);							\
	if (eva_kernel_access()) {					\
		__put_kernel_common(ptr, size);				\
	} else {							\
		__chk_user_ptr(ptr);					\
		__put_user_common(ptr, size);				\
	}								\
	__pu_err;							\
})

#define __put_user_check(x, ptr, size)					\
({									\
	__typeof__(*(ptr)) __user *__pu_addr = (ptr);			\
	__typeof__(*(ptr)) __pu_val = (x);				\
	int __pu_err = -EFAULT;						\
									\
	might_fault();							\
	if (likely(access_ok( __pu_addr, size))) {	\
		if (eva_kernel_access())				\
			__put_kernel_common(__pu_addr, size);		\
		else							\
			__put_user_common(__pu_addr, size);		\
	}								\
									\
	__pu_err;							\
})

#define __put_data_asm(insn, ptr)					\
{									\
	__asm__ __volatile__(						\
	"1:	"insn("%z2", "%3")"	# __put_data_asm	\n"	\
	"2:							\n"	\
	"	.insn						\n"	\
	"	.section	.fixup,\"ax\"			\n"	\
	"3:	li	%0, %4					\n"	\
	"	j	2b					\n"	\
	"	.previous					\n"	\
	"	.section	__ex_table,\"a\"		\n"	\
	"	" __UA_ADDR "	1b, 3b				\n"	\
	"	.previous					\n"	\
	: "=r" (__pu_err)						\
	: "0" (0), "Jr" (__pu_val), "o" (__m(ptr)),			\
	  "i" (-EFAULT));						\
}

#define __put_data_asm_ll32(insn, ptr)					\
{									\
	__asm__ __volatile__(						\
	"1:	"insn("%2", "(%3)")"	# __put_data_asm_ll32	\n"	\
	"2:	"insn("%D2", "4(%3)")"				\n"	\
	"3:							\n"	\
	"	.insn						\n"	\
	"	.section	.fixup,\"ax\"			\n"	\
	"4:	li	%0, %4					\n"	\
	"	j	3b					\n"	\
	"	.previous					\n"	\
	"	.section	__ex_table,\"a\"		\n"	\
	"	" __UA_ADDR "	1b, 4b				\n"	\
	"	" __UA_ADDR "	2b, 4b				\n"	\
	"	.previous"						\
	: "=r" (__pu_err)						\
	: "0" (0), "r" (__pu_val), "r" (ptr),				\
	  "i" (-EFAULT));						\
}

extern void __put_user_unknown(void);

/*
 * We're generating jump to subroutines which will be outside the range of
 * jump instructions
 */
#ifdef MODULE
#define __MODULE_JAL(destination)					\
	".set\tnoat\n\t"						\
	__UA_LA "\t$1, " #destination "\n\t"				\
	"jalr\t$1\n\t"							\
	".set\tat\n\t"
#else
#define __MODULE_JAL(destination)					\
	"jal\t" #destination "\n\t"
#endif

#if defined(CONFIG_CPU_DADDI_WORKAROUNDS) || (defined(CONFIG_EVA) &&	\
					      defined(CONFIG_CPU_HAS_PREFETCH))
#define DADDI_SCRATCH "$3"
#else
#define DADDI_SCRATCH "$0"
#endif

extern size_t __copy_user(void *__to, const void *__from, size_t __n);

#define __invoke_copy_from(func, to, from, n)				\
({									\
	register void *__cu_to_r __asm__("$4");				\
	register const void __user *__cu_from_r __asm__("$5");		\
	register long __cu_len_r __asm__("$6");				\
									\
	__cu_to_r = (to);						\
	__cu_from_r = (from);						\
	__cu_len_r = (n);						\
	__asm__ __volatile__(						\
	".set\tnoreorder\n\t"						\
	__MODULE_JAL(func)						\
	".set\tnoat\n\t"						\
	__UA_ADDU "\t$1, %1, %2\n\t"					\
	".set\tat\n\t"							\
	".set\treorder"							\
	: "+r" (__cu_to_r), "+r" (__cu_from_r), "+r" (__cu_len_r)	\
	:								\
	: "$8", "$9", "$10", "$11", "$12", "$14", "$15", "$24", "$31",	\
	  DADDI_SCRATCH, "memory");					\
	__cu_len_r;							\
})

#define __invoke_copy_to(func, to, from, n)				\
({									\
	register void __user *__cu_to_r __asm__("$4");			\
	register const void *__cu_from_r __asm__("$5");			\
	register long __cu_len_r __asm__("$6");				\
									\
	__cu_to_r = (to);						\
	__cu_from_r = (from);						\
	__cu_len_r = (n);						\
	__asm__ __volatile__(						\
	__MODULE_JAL(func)						\
	: "+r" (__cu_to_r), "+r" (__cu_from_r), "+r" (__cu_len_r)	\
	:								\
	: "$8", "$9", "$10", "$11", "$12", "$14", "$15", "$24", "$31",	\
	  DADDI_SCRATCH, "memory");					\
	__cu_len_r;							\
})

#define __invoke_copy_from_kernel(to, from, n)				\
	__invoke_copy_from(__copy_user, to, from, n)

#define __invoke_copy_to_kernel(to, from, n)				\
	__invoke_copy_to(__copy_user, to, from, n)

#define ___invoke_copy_in_kernel(to, from, n)				\
	__invoke_copy_from(__copy_user, to, from, n)

#ifndef CONFIG_EVA
#define __invoke_copy_from_user(to, from, n)				\
	__invoke_copy_from(__copy_user, to, from, n)

#define __invoke_copy_to_user(to, from, n)				\
	__invoke_copy_to(__copy_user, to, from, n)

#define ___invoke_copy_in_user(to, from, n)				\
	__invoke_copy_from(__copy_user, to, from, n)

#else

/* EVA specific functions */

extern size_t __copy_from_user_eva(void *__to, const void *__from,
				   size_t __n);
extern size_t __copy_to_user_eva(void *__to, const void *__from,
				 size_t __n);
extern size_t __copy_in_user_eva(void *__to, const void *__from, size_t __n);

/*
 * Source or destination address is in userland. We need to go through
 * the TLB
 */
#define __invoke_copy_from_user(to, from, n)				\
	__invoke_copy_from(__copy_from_user_eva, to, from, n)

#define __invoke_copy_to_user(to, from, n)				\
	__invoke_copy_to(__copy_to_user_eva, to, from, n)

#define ___invoke_copy_in_user(to, from, n)				\
	__invoke_copy_from(__copy_in_user_eva, to, from, n)

#endif /* CONFIG_EVA */

static inline unsigned long
raw_copy_to_user(void __user *to, const void *from, unsigned long n)
{
	if (eva_kernel_access())
		return __invoke_copy_to_kernel(to, from, n);
	else
		return __invoke_copy_to_user(to, from, n);
}

static inline unsigned long
raw_copy_from_user(void *to, const void __user *from, unsigned long n)
{
	if (eva_kernel_access())
		return __invoke_copy_from_kernel(to, from, n);
	else
		return __invoke_copy_from_user(to, from, n);
}

#define INLINE_COPY_FROM_USER
#define INLINE_COPY_TO_USER

static inline unsigned long
raw_copy_in_user(void __user*to, const void __user *from, unsigned long n)
{
	if (eva_kernel_access())
		return ___invoke_copy_in_kernel(to, from, n);
	else
		return ___invoke_copy_in_user(to, from,	n);
}

extern __kernel_size_t __bzero_kernel(void __user *addr, __kernel_size_t size);
extern __kernel_size_t __bzero(void __user *addr, __kernel_size_t size);

/*
 * __clear_user: - Zero a block of memory in user space, with less checking.
 * @to:	  Destination address, in user space.
 * @n:	  Number of bytes to zero.
 *
 * Zero a block of memory in user space.  Caller must check
 * the specified block with access_ok() before calling this function.
 *
 * Returns number of bytes that could not be cleared.
 * On success, this will be zero.
 */
static inline __kernel_size_t
__clear_user(void __user *addr, __kernel_size_t size)
{
	__kernel_size_t res;

#ifdef CONFIG_CPU_MICROMIPS
/* micromips memset / bzero also clobbers t7 & t8 */
#define bzero_clobbers "$4", "$5", "$6", __UA_t0, __UA_t1, "$15", "$24", "$31"
#else
#define bzero_clobbers "$4", "$5", "$6", __UA_t0, __UA_t1, "$31"
#endif /* CONFIG_CPU_MICROMIPS */

	if (eva_kernel_access()) {
		__asm__ __volatile__(
			"move\t$4, %1\n\t"
			"move\t$5, $0\n\t"
			"move\t$6, %2\n\t"
			__MODULE_JAL(__bzero_kernel)
			"move\t%0, $6"
			: "=r" (res)
			: "r" (addr), "r" (size)
			: bzero_clobbers);
	} else {
		might_fault();
		__asm__ __volatile__(
			"move\t$4, %1\n\t"
			"move\t$5, $0\n\t"
			"move\t$6, %2\n\t"
			__MODULE_JAL(__bzero)
			"move\t%0, $6"
			: "=r" (res)
			: "r" (addr), "r" (size)
			: bzero_clobbers);
	}

	return res;
}

#define clear_user(addr,n)						\
({									\
	void __user * __cl_addr = (addr);				\
	unsigned long __cl_size = (n);					\
	if (__cl_size && access_ok(__cl_addr, __cl_size))		\
		__cl_size = __clear_user(__cl_addr, __cl_size);		\
	__cl_size;							\
})

extern long __strncpy_from_kernel_asm(char *__to, const char __user *__from, long __len);
extern long __strncpy_from_user_asm(char *__to, const char __user *__from, long __len);

/*
 * strncpy_from_user: - Copy a NUL terminated string from userspace.
 * @dst:   Destination address, in kernel space.  This buffer must be at
 *	   least @count bytes long.
 * @src:   Source address, in user space.
 * @count: Maximum number of bytes to copy, including the trailing NUL.
 *
 * Copies a NUL-terminated string from userspace to kernel space.
 *
 * On success, returns the length of the string (not including the trailing
 * NUL).
 *
 * If access to userspace fails, returns -EFAULT (some data may have been
 * copied).
 *
 * If @count is smaller than the length of the string, copies @count bytes
 * and returns @count.
 */
static inline long
strncpy_from_user(char *__to, const char __user *__from, long __len)
{
	long res;

	if (eva_kernel_access()) {
		__asm__ __volatile__(
			"move\t$4, %1\n\t"
			"move\t$5, %2\n\t"
			"move\t$6, %3\n\t"
			__MODULE_JAL(__strncpy_from_kernel_asm)
			"move\t%0, $2"
			: "=r" (res)
			: "r" (__to), "r" (__from), "r" (__len)
			: "$2", "$3", "$4", "$5", "$6", __UA_t0, "$31", "memory");
	} else {
		might_fault();
		__asm__ __volatile__(
			"move\t$4, %1\n\t"
			"move\t$5, %2\n\t"
			"move\t$6, %3\n\t"
			__MODULE_JAL(__strncpy_from_user_asm)
			"move\t%0, $2"
			: "=r" (res)
			: "r" (__to), "r" (__from), "r" (__len)
			: "$2", "$3", "$4", "$5", "$6", __UA_t0, "$31", "memory");
	}

	return res;
}

extern long __strnlen_kernel_asm(const char __user *s, long n);
extern long __strnlen_user_asm(const char __user *s, long n);

/*
 * strnlen_user: - Get the size of a string in user space.
 * @str: The string to measure.
 *
 * Context: User context only. This function may sleep if pagefaults are
 *          enabled.
 *
 * Get the size of a NUL-terminated string in user space.
 *
 * Returns the size of the string INCLUDING the terminating NUL.
 * On exception, returns 0.
 * If the string is too long, returns a value greater than @n.
 */
static inline long strnlen_user(const char __user *s, long n)
{
	long res;

	might_fault();
	if (eva_kernel_access()) {
		__asm__ __volatile__(
			"move\t$4, %1\n\t"
			"move\t$5, %2\n\t"
			__MODULE_JAL(__strnlen_kernel_asm)
			"move\t%0, $2"
			: "=r" (res)
			: "r" (s), "r" (n)
			: "$2", "$4", "$5", __UA_t0, "$31");
	} else {
		__asm__ __volatile__(
			"move\t$4, %1\n\t"
			"move\t$5, %2\n\t"
			__MODULE_JAL(__strnlen_user_asm)
			"move\t%0, $2"
			: "=r" (res)
			: "r" (s), "r" (n)
			: "$2", "$4", "$5", __UA_t0, "$31");
	}

	return res;
}

#endif /* _ASM_UACCESS_H */
