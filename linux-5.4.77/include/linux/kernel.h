/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KERNEL_H
#define _LINUX_KERNEL_H


#include <stdarg.h>
#include <linux/limits.h>
#include <linux/linkage.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/bitops.h>
#include <linux/log2.h>
#include <linux/typecheck.h>
#include <linux/printk.h>
#include <linux/build_bug.h>
#include <asm/byteorder.h>
#include <asm/div64.h>
#include <uapi/linux/kernel.h>
#include <asm/div64.h>

#define STACK_MAGIC	0xdeadbeef

/**
 * REPEAT_BYTE - repeat the value @x multiple times as an unsigned long value
 * @x: value to repeat
 *
 * NOTE: @x is not checked for > 0xff; larger values produce odd results.
 */
#define REPEAT_BYTE(x)	((~0ul / 0xff) * (x))

/* @a is a power of 2 value */
#define ALIGN(x, a)		__ALIGN_KERNEL((x), (a))
#define ALIGN_DOWN(x, a)	__ALIGN_KERNEL((x) - ((a) - 1), (a))
#define __ALIGN_MASK(x, mask)	__ALIGN_KERNEL_MASK((x), (mask))
#define PTR_ALIGN(p, a)		((typeof(p))ALIGN((unsigned long)(p), (a)))
#define IS_ALIGNED(x, a)		(((x) & ((typeof(x))(a) - 1)) == 0)

/* generic data direction definitions */
#define READ			0
#define WRITE			1

/**
 * ARRAY_SIZE - get the number of elements in array @arr
 * @arr: array to be sized
 */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]) + __must_be_array(arr))

#define u64_to_user_ptr(x) (		\
{					\
	typecheck(u64, (x));		\
	(void __user *)(uintptr_t)(x);	\
}					\
)

/*
 * This looks more complex than it should be. But we need to
 * get the type for the ~ right in round_down (it needs to be
 * as wide as the result!), and we want to evaluate the macro
 * arguments just once each.
 */
#define __round_mask(x, y) ((__typeof__(x))((y)-1))
/**
 * round_up - round up to next specified power of 2
 * @x: the value to round
 * @y: multiple to round up to (must be a power of 2)
 *
 * Rounds @x up to next multiple of @y (which must be a power of 2).
 * To perform arbitrary rounding up, use roundup() below.
 */
#define round_up(x, y) ((((x)-1) | __round_mask(x, y))+1)
/**
 * round_down - round down to next specified power of 2
 * @x: the value to round
 * @y: multiple to round down to (must be a power of 2)
 *
 * Rounds @x down to next multiple of @y (which must be a power of 2).
 * To perform arbitrary rounding down, use rounddown() below.
 */
#define round_down(x, y) ((x) & ~__round_mask(x, y))

/**
 * FIELD_SIZEOF - get the size of a struct's field
 * @t: the target struct
 * @f: the target struct's field
 * Return: the size of @f in the struct definition without having a
 * declared instance of @t.
 */
#define FIELD_SIZEOF(t, f) (sizeof(((t*)0)->f))

#define typeof_member(T, m)	typeof(((T*)0)->m)

#define DIV_ROUND_UP __KERNEL_DIV_ROUND_UP

#define DIV_ROUND_DOWN_ULL(ll, d) \
	({ unsigned long long _tmp = (ll); do_div(_tmp, d); _tmp; })

#define DIV_ROUND_UP_ULL(ll, d) \
	DIV_ROUND_DOWN_ULL((unsigned long long)(ll) + (d) - 1, (d))

#if BITS_PER_LONG == 32
# define DIV_ROUND_UP_SECTOR_T(ll,d) DIV_ROUND_UP_ULL(ll, d)
#else
# define DIV_ROUND_UP_SECTOR_T(ll,d) DIV_ROUND_UP(ll,d)
#endif

/**
 * roundup - round up to the next specified multiple
 * @x: the value to up
 * @y: multiple to round up to
 *
 * Rounds @x up to next multiple of @y. If @y will always be a power
 * of 2, consider using the faster round_up().
 */
#define roundup(x, y) (					\
{							\
	typeof(y) __y = y;				\
	(((x) + (__y - 1)) / __y) * __y;		\
}							\
)
/**
 * rounddown - round down to next specified multiple
 * @x: the value to round
 * @y: multiple to round down to
 *
 * Rounds @x down to next multiple of @y. If @y will always be a power
 * of 2, consider using the faster round_down().
 */
#define rounddown(x, y) (				\
{							\
	typeof(x) __x = (x);				\
	__x - (__x % (y));				\
}							\
)

/*
 * Divide positive or negative dividend by positive or negative divisor
 * and round to closest integer. Result is undefined for negative
 * divisors if the dividend variable type is unsigned and for negative
 * dividends if the divisor variable type is unsigned.
 */
#define DIV_ROUND_CLOSEST(x, divisor)(			\
{							\
	typeof(x) __x = x;				\
	typeof(divisor) __d = divisor;			\
	(((typeof(x))-1) > 0 ||				\
	 ((typeof(divisor))-1) > 0 ||			\
	 (((__x) > 0) == ((__d) > 0))) ?		\
		(((__x) + ((__d) / 2)) / (__d)) :	\
		(((__x) - ((__d) / 2)) / (__d));	\
}							\
)
/*
 * Same as above but for u64 dividends. divisor must be a 32-bit
 * number.
 */
#define DIV_ROUND_CLOSEST_ULL(x, divisor)(		\
{							\
	typeof(divisor) __d = divisor;			\
	unsigned long long _tmp = (x) + (__d) / 2;	\
	do_div(_tmp, __d);				\
	_tmp;						\
}							\
)

/*
 * Multiplies an integer by a fraction, while avoiding unnecessary
 * overflow or loss of precision.
 */
#define mult_frac(x, numer, denom)(			\
{							\
	typeof(x) quot = (x) / (denom);			\
	typeof(x) rem  = (x) % (denom);			\
	(quot * (numer)) + ((rem * (numer)) / (denom));	\
}							\
)


#define _RET_IP_		(unsigned long)__builtin_return_address(0)
#define _THIS_IP_  ({ __label__ __here; __here: (unsigned long)&&__here; })

#define sector_div(a, b) do_div(a, b)

/**
 * upper_32_bits - return bits 32-63 of a number
 * @n: the number we're accessing
 *
 * A basic shift-right of a 64- or 32-bit quantity.  Use this to suppress
 * the "right shift count >= width of type" warning when that quantity is
 * 32-bits.
 */
#define upper_32_bits(n) ((u32)(((n) >> 16) >> 16))

/**
 * lower_32_bits - return bits 0-31 of a number
 * @n: the number we're accessing
 */
#define lower_32_bits(n) ((u32)(n))

struct completion;
struct pt_regs;
struct user;

#ifdef CONFIG_PREEMPT_VOLUNTARY
extern int _cond_resched(void);
# define might_resched() _cond_resched()
#else
# define might_resched() do { } while (0)
#endif

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
extern void ___might_sleep(const char *file, int line, int preempt_offset);
extern void __might_sleep(const char *file, int line, int preempt_offset);
extern void __cant_sleep(const char *file, int line, int preempt_offset);

/**
 * might_sleep - annotation for functions that can sleep
 *
 * this macro will print a stack trace if it is executed in an atomic
 * context (spinlock, irq-handler, ...). Additional sections where blocking is
 * not allowed can be annotated with non_block_start() and non_block_end()
 * pairs.
 *
 * This is a useful debugging help to be able to catch problems early and not
 * be bitten later when the calling function happens to sleep when it is not
 * supposed to.
 */
# define might_sleep() \
	do { __might_sleep(__FILE__, __LINE__, 0); might_resched(); } while (0)
/**
 * cant_sleep - annotation for functions that cannot sleep
 *
 * this macro will print a stack trace if it is executed with preemption enabled
 */
# define cant_sleep() \
	do { __cant_sleep(__FILE__, __LINE__, 0); } while (0)
# define sched_annotate_sleep()	(current->task_state_change = 0)
/**
 * non_block_start - annotate the start of section where sleeping is prohibited
 *
 * This is on behalf of the oom reaper, specifically when it is calling the mmu
 * notifiers. The problem is that if the notifier were to block on, for example,
 * mutex_lock() and if the process which holds that mutex were to perform a
 * sleeping memory allocation, the oom reaper is now blocked on completion of
 * that memory allocation. Other blocking calls like wait_event() pose similar
 * issues.
 */
# define non_block_start() (current->non_block_count++)
/**
 * non_block_end - annotate the end of section where sleeping is prohibited
 *
 * Closes a section opened by non_block_start().
 */
# define non_block_end() WARN_ON(current->non_block_count-- == 0)
#else
  static inline void ___might_sleep(const char *file, int line,
				   int preempt_offset) { }
  static inline void __might_sleep(const char *file, int line,
				   int preempt_offset) { }
# define might_sleep() do { might_resched(); } while (0)
# define cant_sleep() do { } while (0)
# define sched_annotate_sleep() do { } while (0)
# define non_block_start() do { } while (0)
# define non_block_end() do { } while (0)
#endif

#define might_sleep_if(cond) do { if (cond) might_sleep(); } while (0)

/**
 * abs - return absolute value of an argument
 * @x: the value.  If it is unsigned type, it is converted to signed type first.
 *     char is treated as if it was signed (regardless of whether it really is)
 *     but the macro's return type is preserved as char.
 *
 * Return: an absolute value of x.
 */
#define abs(x)	__abs_choose_expr(x, long long,				\
		__abs_choose_expr(x, long,				\
		__abs_choose_expr(x, int,				\
		__abs_choose_expr(x, short,				\
		__abs_choose_expr(x, char,				\
		__builtin_choose_expr(					\
			__builtin_types_compatible_p(typeof(x), char),	\
			(char)({ signed char __x = (x); __x<0?-__x:__x; }), \
			((void)0)))))))

#define __abs_choose_expr(x, type, other) __builtin_choose_expr(	\
	__builtin_types_compatible_p(typeof(x),   signed type) ||	\
	__builtin_types_compatible_p(typeof(x), unsigned type),		\
	({ signed type __x = (x); __x < 0 ? -__x : __x; }), other)

/**
 * reciprocal_scale - "scale" a value into range [0, ep_ro)
 * @val: value
 * @ep_ro: right open interval endpoint
 *
 * Perform a "reciprocal multiplication" in order to "scale" a value into
 * range [0, @ep_ro), where the upper interval endpoint is right-open.
 * This is useful, e.g. for accessing a index of an array containing
 * @ep_ro elements, for example. Think of it as sort of modulus, only that
 * the result isn't that of modulo. ;) Note that if initial input is a
 * small value, then result will return 0.
 *
 * Return: a result based on @val in interval [0, @ep_ro).
 */
static inline u32 reciprocal_scale(u32 val, u32 ep_ro)
{
	return (u32)(((u64) val * ep_ro) >> 32);
}

#if defined(CONFIG_MMU) && \
	(defined(CONFIG_PROVE_LOCKING) || defined(CONFIG_DEBUG_ATOMIC_SLEEP))
#define might_fault() __might_fault(__FILE__, __LINE__)
void __might_fault(const char *file, int line);
#else
static inline void might_fault(void) { }
#endif

extern struct atomic_notifier_head panic_notifier_list;
extern long (*panic_blink)(int state);
__printf(1, 2)
void panic(const char *fmt, ...) __noreturn __cold;
void nmi_panic(struct pt_regs *regs, const char *msg);
extern void oops_enter(void);
extern void oops_exit(void);
void print_oops_end_marker(void);
extern int oops_may_print(void);
void do_exit(long error_code) __noreturn;
void complete_and_exit(struct completion *, long) __noreturn;

#ifdef CONFIG_ARCH_HAS_REFCOUNT
void refcount_error_report(struct pt_regs *regs, const char *err);
#else
static inline void refcount_error_report(struct pt_regs *regs, const char *err)
{ }
#endif

/* Internal, do not use. */
int __must_check _kstrtoul(const char *s, unsigned int base, unsigned long *res);
int __must_check _kstrtol(const char *s, unsigned int base, long *res);

int __must_check kstrtoull(const char *s, unsigned int base, unsigned long long *res);
int __must_check kstrtoll(const char *s, unsigned int base, long long *res);

/**
 * kstrtoul - convert a string to an unsigned long
 * @s: The start of the string. The string must be null-terminated, and may also
 *  include a single newline before its terminating null. The first character
 *  may also be a plus sign, but not a minus sign.
 * @base: The number base to use. The maximum supported base is 16. If base is
 *  given as 0, then the base of the string is automatically detected with the
 *  conventional semantics - If it begins with 0x the number will be parsed as a
 *  hexadecimal (case insensitive), if it otherwise begins with 0, it will be
 *  parsed as an octal number. Otherwise it will be parsed as a decimal.
 * @res: Where to write the result of the conversion on success.
 *
 * Returns 0 on success, -ERANGE on overflow and -EINVAL on parsing error.
 * Used as a replacement for the obsolete simple_strtoull. Return code must
 * be checked.
*/
static inline int __must_check kstrtoul(const char *s, unsigned int base, unsigned long *res)
{
	/*
	 * We want to shortcut function call, but
	 * __builtin_types_compatible_p(unsigned long, unsigned long long) = 0.
	 */
	if (sizeof(unsigned long) == sizeof(unsigned long long) &&
	    __alignof__(unsigned long) == __alignof__(unsigned long long))
		return kstrtoull(s, base, (unsigned long long *)res);
	else
		return _kstrtoul(s, base, res);
}

/**
 * kstrtol - convert a string to a long
 * @s: The start of the string. The string must be null-terminated, and may also
 *  include a single newline before its terminating null. The first character
 *  may also be a plus sign or a minus sign.
 * @base: The number base to use. The maximum supported base is 16. If base is
 *  given as 0, then the base of the string is automatically detected with the
 *  conventional semantics - If it begins with 0x the number will be parsed as a
 *  hexadecimal (case insensitive), if it otherwise begins with 0, it will be
 *  parsed as an octal number. Otherwise it will be parsed as a decimal.
 * @res: Where to write the result of the conversion on success.
 *
 * Returns 0 on success, -ERANGE on overflow and -EINVAL on parsing error.
 * Used as a replacement for the obsolete simple_strtoull. Return code must
 * be checked.
 */
static inline int __must_check kstrtol(const char *s, unsigned int base, long *res)
{
	/*
	 * We want to shortcut function call, but
	 * __builtin_types_compatible_p(long, long long) = 0.
	 */
	if (sizeof(long) == sizeof(long long) &&
	    __alignof__(long) == __alignof__(long long))
		return kstrtoll(s, base, (long long *)res);
	else
		return _kstrtol(s, base, res);
}

int __must_check kstrtouint(const char *s, unsigned int base, unsigned int *res);
int __must_check kstrtoint(const char *s, unsigned int base, int *res);

static inline int __must_check kstrtou64(const char *s, unsigned int base, u64 *res)
{
	return kstrtoull(s, base, res);
}

static inline int __must_check kstrtos64(const char *s, unsigned int base, s64 *res)
{
	return kstrtoll(s, base, res);
}

static inline int __must_check kstrtou32(const char *s, unsigned int base, u32 *res)
{
	return kstrtouint(s, base, res);
}

static inline int __must_check kstrtos32(const char *s, unsigned int base, s32 *res)
{
	return kstrtoint(s, base, res);
}

int __must_check kstrtou16(const char *s, unsigned int base, u16 *res);
int __must_check kstrtos16(const char *s, unsigned int base, s16 *res);
int __must_check kstrtou8(const char *s, unsigned int base, u8 *res);
int __must_check kstrtos8(const char *s, unsigned int base, s8 *res);
int __must_check kstrtobool(const char *s, bool *res);

int __must_check kstrtoull_from_user(const char __user *s, size_t count, unsigned int base, unsigned long long *res);
int __must_check kstrtoll_from_user(const char __user *s, size_t count, unsigned int base, long long *res);
int __must_check kstrtoul_from_user(const char __user *s, size_t count, unsigned int base, unsigned long *res);
int __must_check kstrtol_from_user(const char __user *s, size_t count, unsigned int base, long *res);
int __must_check kstrtouint_from_user(const char __user *s, size_t count, unsigned int base, unsigned int *res);
int __must_check kstrtoint_from_user(const char __user *s, size_t count, unsigned int base, int *res);
int __must_check kstrtou16_from_user(const char __user *s, size_t count, unsigned int base, u16 *res);
int __must_check kstrtos16_from_user(const char __user *s, size_t count, unsigned int base, s16 *res);
int __must_check kstrtou8_from_user(const char __user *s, size_t count, unsigned int base, u8 *res);
int __must_check kstrtos8_from_user(const char __user *s, size_t count, unsigned int base, s8 *res);
int __must_check kstrtobool_from_user(const char __user *s, size_t count, bool *res);

static inline int __must_check kstrtou64_from_user(const char __user *s, size_t count, unsigned int base, u64 *res)
{
	return kstrtoull_from_user(s, count, base, res);
}

static inline int __must_check kstrtos64_from_user(const char __user *s, size_t count, unsigned int base, s64 *res)
{
	return kstrtoll_from_user(s, count, base, res);
}

static inline int __must_check kstrtou32_from_user(const char __user *s, size_t count, unsigned int base, u32 *res)
{
	return kstrtouint_from_user(s, count, base, res);
}

static inline int __must_check kstrtos32_from_user(const char __user *s, size_t count, unsigned int base, s32 *res)
{
	return kstrtoint_from_user(s, count, base, res);
}

/* Obsolete, do not use.  Use kstrto<foo> instead */

extern unsigned long simple_strtoul(const char *,char **,unsigned int);
extern long simple_strtol(const char *,char **,unsigned int);
extern unsigned long long simple_strtoull(const char *,char **,unsigned int);
extern long long simple_strtoll(const char *,char **,unsigned int);

extern int num_to_str(char *buf, int size,
		      unsigned long long num, unsigned int width);

/* lib/printf utilities */

extern __printf(2, 3) int sprintf(char *buf, const char * fmt, ...);
extern __printf(2, 0) int vsprintf(char *buf, const char *, va_list);
extern __printf(3, 4)
int snprintf(char *buf, size_t size, const char *fmt, ...);
extern __printf(3, 0)
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
extern __printf(3, 4)
int scnprintf(char *buf, size_t size, const char *fmt, ...);
extern __printf(3, 0)
int vscnprintf(char *buf, size_t size, const char *fmt, va_list args);
extern __printf(2, 3) __malloc
char *kasprintf(gfp_t gfp, const char *fmt, ...);
extern __printf(2, 0) __malloc
char *kvasprintf(gfp_t gfp, const char *fmt, va_list args);
extern __printf(2, 0)
const char *kvasprintf_const(gfp_t gfp, const char *fmt, va_list args);

extern __scanf(2, 3)
int sscanf(const char *, const char *, ...);
extern __scanf(2, 0)
int vsscanf(const char *, const char *, va_list);

extern int get_option(char **str, int *pint);
extern char *get_options(const char *str, int nints, int *ints);
extern unsigned long long memparse(const char *ptr, char **retptr);
extern bool parse_option_str(const char *str, const char *option);
extern char *next_arg(char *args, char **param, char **val);

extern int core_kernel_text(unsigned long addr);
extern int init_kernel_text(unsigned long addr);
extern int core_kernel_data(unsigned long addr);
extern int __kernel_text_address(unsigned long addr);
extern int kernel_text_address(unsigned long addr);
extern int func_ptr_is_kernel_text(void *ptr);

u64 int_pow(u64 base, unsigned int exp);
unsigned long int_sqrt(unsigned long);

#if BITS_PER_LONG < 64
u32 int_sqrt64(u64 x);
#else
static inline u32 int_sqrt64(u64 x)
{
	return (u32)int_sqrt(x);
}
#endif

extern void bust_spinlocks(int yes);
extern int oops_in_progress;		/* If set, an oops, panic(), BUG() or die() is in progress */
extern int panic_timeout;
extern unsigned long panic_print;
extern int panic_on_oops;
extern int panic_on_unrecovered_nmi;
extern int panic_on_io_nmi;
extern int panic_on_warn;
extern int sysctl_panic_on_rcu_stall;
extern int sysctl_panic_on_stackoverflow;

extern bool crash_kexec_post_notifiers;

/*
 * panic_cpu is used for synchronizing panic() and crash_kexec() execution. It
 * holds a CPU number which is executing panic() currently. A value of
 * PANIC_CPU_INVALID means no CPU has entered panic() or crash_kexec().
 */
extern atomic_t panic_cpu;
#define PANIC_CPU_INVALID	-1

/*
 * Only to be used by arch init code. If the user over-wrote the default
 * CONFIG_PANIC_TIMEOUT, honor it.
 */
static inline void set_arch_panic_timeout(int timeout, int arch_default_timeout)
{
	if (panic_timeout == arch_default_timeout)
		panic_timeout = timeout;
}
extern const char *print_tainted(void);
enum lockdep_ok {
	LOCKDEP_STILL_OK,
	LOCKDEP_NOW_UNRELIABLE
};
extern void add_taint(unsigned flag, enum lockdep_ok);
extern int test_taint(unsigned flag);
extern unsigned long get_taint(void);
extern int root_mountflags;

extern bool early_boot_irqs_disabled;

/*
 * Values used for system_state. Ordering of the states must not be changed
 * as code checks for <, <=, >, >= STATE.
 */
extern enum system_states {
	SYSTEM_BOOTING,
	SYSTEM_SCHEDULING,
	SYSTEM_RUNNING,
	SYSTEM_HALT,
	SYSTEM_POWER_OFF,
	SYSTEM_RESTART,
	SYSTEM_SUSPEND,
} system_state;

/* This cannot be an enum because some may be used in assembly source. */
#define TAINT_PROPRIETARY_MODULE	0
#define TAINT_FORCED_MODULE		1
#define TAINT_CPU_OUT_OF_SPEC		2
#define TAINT_FORCED_RMMOD		3
#define TAINT_MACHINE_CHECK		4
#define TAINT_BAD_PAGE			5
#define TAINT_USER			6
#define TAINT_DIE			7
#define TAINT_OVERRIDDEN_ACPI_TABLE	8
#define TAINT_WARN			9
#define TAINT_CRAP			10
#define TAINT_FIRMWARE_WORKAROUND	11
#define TAINT_OOT_MODULE		12
#define TAINT_UNSIGNED_MODULE		13
#define TAINT_SOFTLOCKUP		14
#define TAINT_LIVEPATCH			15
#define TAINT_AUX			16
#define TAINT_RANDSTRUCT		17
#define TAINT_FLAGS_COUNT		18

struct taint_flag {
	char c_true;	/* character printed when tainted */
	char c_false;	/* character printed when not tainted */
	bool module;	/* also show as a per-module taint flag */
};

extern const struct taint_flag taint_flags[TAINT_FLAGS_COUNT];

extern const char hex_asc[];
#define hex_asc_lo(x)	hex_asc[((x) & 0x0f)]
#define hex_asc_hi(x)	hex_asc[((x) & 0xf0) >> 4]

static inline char *hex_byte_pack(char *buf, u8 byte)
{
	*buf++ = hex_asc_hi(byte);
	*buf++ = hex_asc_lo(byte);
	return buf;
}

extern const char hex_asc_upper[];
#define hex_asc_upper_lo(x)	hex_asc_upper[((x) & 0x0f)]
#define hex_asc_upper_hi(x)	hex_asc_upper[((x) & 0xf0) >> 4]

static inline char *hex_byte_pack_upper(char *buf, u8 byte)
{
	*buf++ = hex_asc_upper_hi(byte);
	*buf++ = hex_asc_upper_lo(byte);
	return buf;
}

extern int hex_to_bin(char ch);
extern int __must_check hex2bin(u8 *dst, const char *src, size_t count);
extern char *bin2hex(char *dst, const void *src, size_t count);

bool mac_pton(const char *s, u8 *mac);

/*
 * General tracing related utility functions - trace_printk(),
 * tracing_on/tracing_off and tracing_start()/tracing_stop
 *
 * Use tracing_on/tracing_off when you want to quickly turn on or off
 * tracing. It simply enables or disables the recording of the trace events.
 * This also corresponds to the user space /sys/kernel/debug/tracing/tracing_on
 * file, which gives a means for the kernel and userspace to interact.
 * Place a tracing_off() in the kernel where you want tracing to end.
 * From user space, examine the trace, and then echo 1 > tracing_on
 * to continue tracing.
 *
 * tracing_stop/tracing_start has slightly more overhead. It is used
 * by things like suspend to ram where disabling the recording of the
 * trace is not enough, but tracing must actually stop because things
 * like calling smp_processor_id() may crash the system.
 *
 * Most likely, you want to use tracing_on/tracing_off.
 */

enum ftrace_dump_mode {
	DUMP_NONE,
	DUMP_ALL,
	DUMP_ORIG,
};

#ifdef CONFIG_TRACING
void tracing_on(void);
void tracing_off(void);
int tracing_is_on(void);
void tracing_snapshot(void);
void tracing_snapshot_alloc(void);

extern void tracing_start(void);
extern void tracing_stop(void);

static inline __printf(1, 2)
void ____trace_printk_check_format(const char *fmt, ...)
{
}
#define __trace_printk_check_format(fmt, args...)			\
do {									\
	if (0)								\
		____trace_printk_check_format(fmt, ##args);		\
} while (0)

/**
 * trace_printk - printf formatting in the ftrace buffer
 * @fmt: the printf format for printing
 *
 * Note: __trace_printk is an internal function for trace_printk() and
 *       the @ip is passed in via the trace_printk() macro.
 *
 * This function allows a kernel developer to debug fast path sections
 * that printk is not appropriate for. By scattering in various
 * printk like tracing in the code, a developer can quickly see
 * where problems are occurring.
 *
 * This is intended as a debugging tool for the developer only.
 * Please refrain from leaving trace_printks scattered around in
 * your code. (Extra memory is used for special buffers that are
 * allocated when trace_printk() is used.)
 *
 * A little optimization trick is done here. If there's only one
 * argument, there's no need to scan the string for printf formats.
 * The trace_puts() will suffice. But how can we take advantage of
 * using trace_puts() when trace_printk() has only one argument?
 * By stringifying the args and checking the size we can tell
 * whether or not there are args. __stringify((__VA_ARGS__)) will
 * turn into "()\0" with a size of 3 when there are no args, anything
 * else will be bigger. All we need to do is define a string to this,
 * and then take its size and compare to 3. If it's bigger, use
 * do_trace_printk() otherwise, optimize it to trace_puts(). Then just
 * let gcc optimize the rest.
 */

#define trace_printk(fmt, ...)				\
do {							\
	char _______STR[] = __stringify((__VA_ARGS__));	\
	if (sizeof(_______STR) > 3)			\
		do_trace_printk(fmt, ##__VA_ARGS__);	\
	else						\
		trace_puts(fmt);			\
} while (0)

#define do_trace_printk(fmt, args...)					\
do {									\
	static const char *trace_printk_fmt __used			\
		__attribute__((section("__trace_printk_fmt"))) =	\
		__builtin_constant_p(fmt) ? fmt : NULL;			\
									\
	__trace_printk_check_format(fmt, ##args);			\
									\
	if (__builtin_constant_p(fmt))					\
		__trace_bprintk(_THIS_IP_, trace_printk_fmt, ##args);	\
	else								\
		__trace_printk(_THIS_IP_, fmt, ##args);			\
} while (0)

extern __printf(2, 3)
int __trace_bprintk(unsigned long ip, const char *fmt, ...);

extern __printf(2, 3)
int __trace_printk(unsigned long ip, const char *fmt, ...);

/**
 * trace_puts - write a string into the ftrace buffer
 * @str: the string to record
 *
 * Note: __trace_bputs is an internal function for trace_puts and
 *       the @ip is passed in via the trace_puts macro.
 *
 * This is similar to trace_printk() but is made for those really fast
 * paths that a developer wants the least amount of "Heisenbug" effects,
 * where the processing of the print format is still too much.
 *
 * This function allows a kernel developer to debug fast path sections
 * that printk is not appropriate for. By scattering in various
 * printk like tracing in the code, a developer can quickly see
 * where problems are occurring.
 *
 * This is intended as a debugging tool for the developer only.
 * Please refrain from leaving trace_puts scattered around in
 * your code. (Extra memory is used for special buffers that are
 * allocated when trace_puts() is used.)
 *
 * Returns: 0 if nothing was written, positive # if string was.
 *  (1 when __trace_bputs is used, strlen(str) when __trace_puts is used)
 */

#define trace_puts(str) ({						\
	static const char *trace_printk_fmt __used			\
		__attribute__((section("__trace_printk_fmt"))) =	\
		__builtin_constant_p(str) ? str : NULL;			\
									\
	if (__builtin_constant_p(str))					\
		__trace_bputs(_THIS_IP_, trace_printk_fmt);		\
	else								\
		__trace_puts(_THIS_IP_, str, strlen(str));		\
})
extern int __trace_bputs(unsigned long ip, const char *str);
extern int __trace_puts(unsigned long ip, const char *str, int size);

extern void trace_dump_stack(int skip);

/*
 * The double __builtin_constant_p is because gcc will give us an error
 * if we try to allocate the static variable to fmt if it is not a
 * constant. Even with the outer if statement.
 */
#define ftrace_vprintk(fmt, vargs)					\
do {									\
	if (__builtin_constant_p(fmt)) {				\
		static const char *trace_printk_fmt __used		\
		  __attribute__((section("__trace_printk_fmt"))) =	\
			__builtin_constant_p(fmt) ? fmt : NULL;		\
									\
		__ftrace_vbprintk(_THIS_IP_, trace_printk_fmt, vargs);	\
	} else								\
		__ftrace_vprintk(_THIS_IP_, fmt, vargs);		\
} while (0)

extern __printf(2, 0) int
__ftrace_vbprintk(unsigned long ip, const char *fmt, va_list ap);

extern __printf(2, 0) int
__ftrace_vprintk(unsigned long ip, const char *fmt, va_list ap);

extern void ftrace_dump(enum ftrace_dump_mode oops_dump_mode);
#else
static inline void tracing_start(void) { }
static inline void tracing_stop(void) { }
static inline void trace_dump_stack(int skip) { }

static inline void tracing_on(void) { }
static inline void tracing_off(void) { }
static inline int tracing_is_on(void) { return 0; }
static inline void tracing_snapshot(void) { }
static inline void tracing_snapshot_alloc(void) { }

static inline __printf(1, 2)
int trace_printk(const char *fmt, ...)
{
	return 0;
}
static __printf(1, 0) inline int
ftrace_vprintk(const char *fmt, va_list ap)
{
	return 0;
}
static inline void ftrace_dump(enum ftrace_dump_mode oops_dump_mode) { }
#endif /* CONFIG_TRACING */

/*
 * min()/max()/clamp() macros must accomplish three things:
 *
 * - avoid multiple evaluations of the arguments (so side-effects like
 *   "x++" happen only once) when non-constant.
 * - perform strict type-checking (to generate warnings instead of
 *   nasty runtime surprises). See the "unnecessary" pointer comparison
 *   in __typecheck().
 * - retain result as a constant expressions when called with only
 *   constant expressions (to avoid tripping VLA warnings in stack
 *   allocation usage).
 */
#define __typecheck(x, y) \
		(!!(sizeof((typeof(x) *)1 == (typeof(y) *)1)))

/*
 * This returns a constant expression while determining if an argument is
 * a constant expression, most importantly without evaluating the argument.
 * Glory to Martin Uecker <Martin.Uecker@med.uni-goettingen.de>
 */
#define __is_constexpr(x) \
	(sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))

#define __no_side_effects(x, y) \
		(__is_constexpr(x) && __is_constexpr(y))

#define __safe_cmp(x, y) \
		(__typecheck(x, y) && __no_side_effects(x, y))

#define __cmp(x, y, op)	((x) op (y) ? (x) : (y))

#define __cmp_once(x, y, unique_x, unique_y, op) ({	\
		typeof(x) unique_x = (x);		\
		typeof(y) unique_y = (y);		\
		__cmp(unique_x, unique_y, op); })

#define __careful_cmp(x, y, op) \
	__builtin_choose_expr(__safe_cmp(x, y), \
		__cmp(x, y, op), \
		__cmp_once(x, y, __UNIQUE_ID(__x), __UNIQUE_ID(__y), op))

/**
 * min - return minimum of two values of the same or compatible types
 * @x: first value
 * @y: second value
 */
#define min(x, y)	__careful_cmp(x, y, <)

/**
 * max - return maximum of two values of the same or compatible types
 * @x: first value
 * @y: second value
 */
#define max(x, y)	__careful_cmp(x, y, >)

/**
 * min3 - return minimum of three values
 * @x: first value
 * @y: second value
 * @z: third value
 */
#define min3(x, y, z) min((typeof(x))min(x, y), z)

/**
 * max3 - return maximum of three values
 * @x: first value
 * @y: second value
 * @z: third value
 */
#define max3(x, y, z) max((typeof(x))max(x, y), z)

/**
 * min_not_zero - return the minimum that is _not_ zero, unless both are zero
 * @x: value1
 * @y: value2
 */
#define min_not_zero(x, y) ({			\
	typeof(x) __x = (x);			\
	typeof(y) __y = (y);			\
	__x == 0 ? __y : ((__y == 0) ? __x : min(__x, __y)); })

/**
 * clamp - return a value clamped to a given range with strict typechecking
 * @val: current value
 * @lo: lowest allowable value
 * @hi: highest allowable value
 *
 * This macro does strict typechecking of @lo/@hi to make sure they are of the
 * same type as @val.  See the unnecessary pointer comparisons.
 */
#define clamp(val, lo, hi) min((typeof(val))max(val, lo), hi)

/*
 * ..and if you can't take the strict
 * types, you can specify one yourself.
 *
 * Or not use min/max/clamp at all, of course.
 */

/**
 * min_t - return minimum of two values, using the specified type
 * @type: data type to use
 * @x: first value
 * @y: second value
 */
#define min_t(type, x, y)	__careful_cmp((type)(x), (type)(y), <)

/**
 * max_t - return maximum of two values, using the specified type
 * @type: data type to use
 * @x: first value
 * @y: second value
 */
#define max_t(type, x, y)	__careful_cmp((type)(x), (type)(y), >)

/**
 * clamp_t - return a value clamped to a given range using a given type
 * @type: the type of variable to use
 * @val: current value
 * @lo: minimum allowable value
 * @hi: maximum allowable value
 *
 * This macro does no typechecking and uses temporary variables of type
 * @type to make all the comparisons.
 */
#define clamp_t(type, val, lo, hi) min_t(type, max_t(type, val, lo), hi)

/**
 * clamp_val - return a value clamped to a given range using val's type
 * @val: current value
 * @lo: minimum allowable value
 * @hi: maximum allowable value
 *
 * This macro does no typechecking and uses temporary variables of whatever
 * type the input argument @val is.  This is useful when @val is an unsigned
 * type and @lo and @hi are literals that will otherwise be assigned a signed
 * integer type.
 */
#define clamp_val(val, lo, hi) clamp_t(typeof(val), val, lo, hi)


/**
 * swap - swap values of @a and @b
 * @a: first value
 * @b: second value
 */
#define swap(a, b) \
	do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

/* This counts to 12. Any more, it will return 13th argument. */
#define __COUNT_ARGS(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _n, X...) _n
#define COUNT_ARGS(X...) __COUNT_ARGS(, ##X, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)

#define __CONCAT(a, b) a ## b
#define CONCATENATE(a, b) __CONCAT(a, b)

/**
 * container_of - cast a member of a structure out to the containing structure
 * @ptr:	the pointer to the member.
 * @type:	the type of the container struct this is embedded in.
 * @member:	the name of the member within the struct.
 *
 */
#define container_of(ptr, type, member) ({				\
	void *__mptr = (void *)(ptr);					\
	BUILD_BUG_ON_MSG(!__same_type(*(ptr), ((type *)0)->member) &&	\
			 !__same_type(*(ptr), void),			\
			 "pointer type mismatch in container_of()");	\
	((type *)(__mptr - offsetof(type, member))); })

/**
 * container_of_safe - cast a member of a structure out to the containing structure
 * @ptr:	the pointer to the member.
 * @type:	the type of the container struct this is embedded in.
 * @member:	the name of the member within the struct.
 *
 * If IS_ERR_OR_NULL(ptr), ptr is returned unchanged.
 */
#define container_of_safe(ptr, type, member) ({				\
	void *__mptr = (void *)(ptr);					\
	BUILD_BUG_ON_MSG(!__same_type(*(ptr), ((type *)0)->member) &&	\
			 !__same_type(*(ptr), void),			\
			 "pointer type mismatch in container_of()");	\
	IS_ERR_OR_NULL(__mptr) ? ERR_CAST(__mptr) :			\
		((type *)(__mptr - offsetof(type, member))); })

/* Rebuild everything on CONFIG_FTRACE_MCOUNT_RECORD */
#ifdef CONFIG_FTRACE_MCOUNT_RECORD
# define REBUILD_DUE_TO_FTRACE_MCOUNT_RECORD
#endif

/* Permissions on a sysfs file: you didn't miss the 0 prefix did you? */
#define VERIFY_OCTAL_PERMISSIONS(perms)						\
	(BUILD_BUG_ON_ZERO((perms) < 0) +					\
	 BUILD_BUG_ON_ZERO((perms) > 0777) +					\
	 /* USER_READABLE >= GROUP_READABLE >= OTHER_READABLE */		\
	 BUILD_BUG_ON_ZERO((((perms) >> 6) & 4) < (((perms) >> 3) & 4)) +	\
	 BUILD_BUG_ON_ZERO((((perms) >> 3) & 4) < ((perms) & 4)) +		\
	 /* USER_WRITABLE >= GROUP_WRITABLE */					\
	 BUILD_BUG_ON_ZERO((((perms) >> 6) & 2) < (((perms) >> 3) & 2)) +	\
	 /* OTHER_WRITABLE?  Generally considered a bad idea. */		\
	 BUILD_BUG_ON_ZERO((perms) & 2) +					\
	 (perms))
#endif
