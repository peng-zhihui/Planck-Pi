/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_BUG_H
#define _ASM_X86_BUG_H

#include <linux/stringify.h>

/*
 * Despite that some emulators terminate on UD2, we use it for WARN().
 *
 * Since various instruction decoders/specs disagree on the encoding of
 * UD0/UD1.
 */

#define ASM_UD0		".byte 0x0f, 0xff" /* + ModRM (for Intel) */
#define ASM_UD1		".byte 0x0f, 0xb9" /* + ModRM */
#define ASM_UD2		".byte 0x0f, 0x0b"

#define INSN_UD0	0xff0f
#define INSN_UD2	0x0b0f

#define LEN_UD2		2

#ifdef CONFIG_GENERIC_BUG

#ifdef CONFIG_X86_32
# define __BUG_REL(val)	".long " __stringify(val)
#else
# define __BUG_REL(val)	".long " __stringify(val) " - 2b"
#endif

#ifdef CONFIG_DEBUG_BUGVERBOSE

#define _BUG_FLAGS(ins, flags)						\
do {									\
	asm_inline volatile("1:\t" ins "\n"				\
		     ".pushsection __bug_table,\"aw\"\n"		\
		     "2:\t" __BUG_REL(1b) "\t# bug_entry::bug_addr\n"	\
		     "\t"  __BUG_REL(%c0) "\t# bug_entry::file\n"	\
		     "\t.word %c1"        "\t# bug_entry::line\n"	\
		     "\t.word %c2"        "\t# bug_entry::flags\n"	\
		     "\t.org 2b+%c3\n"					\
		     ".popsection"					\
		     : : "i" (__FILE__), "i" (__LINE__),		\
			 "i" (flags),					\
			 "i" (sizeof(struct bug_entry)));		\
} while (0)

#else /* !CONFIG_DEBUG_BUGVERBOSE */

#define _BUG_FLAGS(ins, flags)						\
do {									\
	asm_inline volatile("1:\t" ins "\n"				\
		     ".pushsection __bug_table,\"aw\"\n"		\
		     "2:\t" __BUG_REL(1b) "\t# bug_entry::bug_addr\n"	\
		     "\t.word %c0"        "\t# bug_entry::flags\n"	\
		     "\t.org 2b+%c1\n"					\
		     ".popsection"					\
		     : : "i" (flags),					\
			 "i" (sizeof(struct bug_entry)));		\
} while (0)

#endif /* CONFIG_DEBUG_BUGVERBOSE */

#else

#define _BUG_FLAGS(ins, flags)  asm volatile(ins)

#endif /* CONFIG_GENERIC_BUG */

#define HAVE_ARCH_BUG
#define BUG()							\
do {								\
	_BUG_FLAGS(ASM_UD2, 0);					\
	unreachable();						\
} while (0)

#define __WARN_FLAGS(flags)					\
do {								\
	_BUG_FLAGS(ASM_UD2, BUGFLAG_WARNING|(flags));		\
	annotate_reachable();					\
} while (0)

#include <asm-generic/bug.h>

#endif /* _ASM_X86_BUG_H */
