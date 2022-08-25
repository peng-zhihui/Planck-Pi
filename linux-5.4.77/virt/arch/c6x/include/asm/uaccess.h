/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  Copyright (C) 2011 Texas Instruments Incorporated
 *  Author: Mark Salter <msalter@redhat.com>
 */
#ifndef _ASM_C6X_UACCESS_H
#define _ASM_C6X_UACCESS_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/string.h>

/*
 * C6X supports unaligned 32 and 64 bit loads and stores.
 */
static inline __must_check unsigned long
raw_copy_from_user(void *to, const void __user *from, unsigned long n)
{
	u32 tmp32;
	u64 tmp64;

	if (__builtin_constant_p(n)) {
		switch (n) {
		case 1:
			*(u8 *)to = *(u8 __force *)from;
			return 0;
		case 4:
			asm volatile ("ldnw .d1t1 *%2,%0\n"
				      "nop  4\n"
				      "stnw .d1t1 %0,*%1\n"
				      : "=&a"(tmp32)
				      : "A"(to), "a"(from)
				      : "memory");
			return 0;
		case 8:
			asm volatile ("ldndw .d1t1 *%2,%0\n"
				      "nop   4\n"
				      "stndw .d1t1 %0,*%1\n"
				      : "=&a"(tmp64)
				      : "a"(to), "a"(from)
				      : "memory");
			return 0;
		default:
			break;
		}
	}

	memcpy(to, (const void __force *)from, n);
	return 0;
}

static inline __must_check unsigned long
raw_copy_to_user(void __user *to, const void *from, unsigned long n)
{
	u32 tmp32;
	u64 tmp64;

	if (__builtin_constant_p(n)) {
		switch (n) {
		case 1:
			*(u8 __force *)to = *(u8 *)from;
			return 0;
		case 4:
			asm volatile ("ldnw .d1t1 *%2,%0\n"
				      "nop  4\n"
				      "stnw .d1t1 %0,*%1\n"
				      : "=&a"(tmp32)
				      : "a"(to), "a"(from)
				      : "memory");
			return 0;
		case 8:
			asm volatile ("ldndw .d1t1 *%2,%0\n"
				      "nop   4\n"
				      "stndw .d1t1 %0,*%1\n"
				      : "=&a"(tmp64)
				      : "a"(to), "a"(from)
				      : "memory");
			return 0;
		default:
			break;
		}
	}

	memcpy((void __force *)to, from, n);
	return 0;
}
#define INLINE_COPY_FROM_USER
#define INLINE_COPY_TO_USER

extern int _access_ok(unsigned long addr, unsigned long size);
#ifdef CONFIG_ACCESS_CHECK
#define __access_ok _access_ok
#endif

#include <asm-generic/uaccess.h>

#endif /* _ASM_C6X_UACCESS_H */
