// SPDX-License-Identifier: GPL-2.0
/*
 * This is for all the tests related to logic bugs (e.g. bad dereferences,
 * bad alignment, bad loops, bad locking, bad scheduling, deep stacks, and
 * lockups) along with other things that don't fit well into existing LKDTM
 * test source files.
 */
#include "lkdtm.h"
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task_stack.h>
#include <linux/uaccess.h>

struct lkdtm_list {
	struct list_head node;
};

/*
 * Make sure our attempts to over run the kernel stack doesn't trigger
 * a compiler warning when CONFIG_FRAME_WARN is set. Then make sure we
 * recurse past the end of THREAD_SIZE by default.
 */
#if defined(CONFIG_FRAME_WARN) && (CONFIG_FRAME_WARN > 0)
#define REC_STACK_SIZE (_AC(CONFIG_FRAME_WARN, UL) / 2)
#else
#define REC_STACK_SIZE (THREAD_SIZE / 8)
#endif
#define REC_NUM_DEFAULT ((THREAD_SIZE / REC_STACK_SIZE) * 2)

static int recur_count = REC_NUM_DEFAULT;

static DEFINE_SPINLOCK(lock_me_up);

/*
 * Make sure compiler does not optimize this function or stack frame away:
 * - function marked noinline
 * - stack variables are marked volatile
 * - stack variables are written (memset()) and read (pr_info())
 * - function has external effects (pr_info())
 * */
static int noinline recursive_loop(int remaining)
{
	volatile char buf[REC_STACK_SIZE];

	memset((void *)buf, remaining & 0xFF, sizeof(buf));
	pr_info("loop %d/%d ...\n", (int)buf[remaining % sizeof(buf)],
		recur_count);
	if (!remaining)
		return 0;
	else
		return recursive_loop(remaining - 1);
}

/* If the depth is negative, use the default, otherwise keep parameter. */
void __init lkdtm_bugs_init(int *recur_param)
{
	if (*recur_param < 0)
		*recur_param = recur_count;
	else
		recur_count = *recur_param;
}

void lkdtm_PANIC(void)
{
	panic("dumptest");
}

void lkdtm_BUG(void)
{
	BUG();
}

static int warn_counter;

void lkdtm_WARNING(void)
{
	WARN_ON(++warn_counter);
}

void lkdtm_WARNING_MESSAGE(void)
{
	WARN(1, "Warning message trigger count: %d\n", ++warn_counter);
}

void lkdtm_EXCEPTION(void)
{
	*((volatile int *) 0) = 0;
}

void lkdtm_LOOP(void)
{
	for (;;)
		;
}

void lkdtm_EXHAUST_STACK(void)
{
	pr_info("Calling function with %lu frame size to depth %d ...\n",
		REC_STACK_SIZE, recur_count);
	recursive_loop(recur_count);
	pr_info("FAIL: survived without exhausting stack?!\n");
}

static noinline void __lkdtm_CORRUPT_STACK(void *stack)
{
	memset(stack, '\xff', 64);
}

/* This should trip the stack canary, not corrupt the return address. */
noinline void lkdtm_CORRUPT_STACK(void)
{
	/* Use default char array length that triggers stack protection. */
	char data[8] __aligned(sizeof(void *));

	__lkdtm_CORRUPT_STACK(&data);

	pr_info("Corrupted stack containing char array ...\n");
}

/* Same as above but will only get a canary with -fstack-protector-strong */
noinline void lkdtm_CORRUPT_STACK_STRONG(void)
{
	union {
		unsigned short shorts[4];
		unsigned long *ptr;
	} data __aligned(sizeof(void *));

	__lkdtm_CORRUPT_STACK(&data);

	pr_info("Corrupted stack containing union ...\n");
}

void lkdtm_UNALIGNED_LOAD_STORE_WRITE(void)
{
	static u8 data[5] __attribute__((aligned(4))) = {1, 2, 3, 4, 5};
	u32 *p;
	u32 val = 0x12345678;

	p = (u32 *)(data + 1);
	if (*p == 0)
		val = 0x87654321;
	*p = val;
}

void lkdtm_SOFTLOCKUP(void)
{
	preempt_disable();
	for (;;)
		cpu_relax();
}

void lkdtm_HARDLOCKUP(void)
{
	local_irq_disable();
	for (;;)
		cpu_relax();
}

void lkdtm_SPINLOCKUP(void)
{
	/* Must be called twice to trigger. */
	spin_lock(&lock_me_up);
	/* Let sparse know we intended to exit holding the lock. */
	__release(&lock_me_up);
}

void lkdtm_HUNG_TASK(void)
{
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule();
}

void lkdtm_CORRUPT_LIST_ADD(void)
{
	/*
	 * Initially, an empty list via LIST_HEAD:
	 *	test_head.next = &test_head
	 *	test_head.prev = &test_head
	 */
	LIST_HEAD(test_head);
	struct lkdtm_list good, bad;
	void *target[2] = { };
	void *redirection = &target;

	pr_info("attempting good list addition\n");

	/*
	 * Adding to the list performs these actions:
	 *	test_head.next->prev = &good.node
	 *	good.node.next = test_head.next
	 *	good.node.prev = test_head
	 *	test_head.next = good.node
	 */
	list_add(&good.node, &test_head);

	pr_info("attempting corrupted list addition\n");
	/*
	 * In simulating this "write what where" primitive, the "what" is
	 * the address of &bad.node, and the "where" is the address held
	 * by "redirection".
	 */
	test_head.next = redirection;
	list_add(&bad.node, &test_head);

	if (target[0] == NULL && target[1] == NULL)
		pr_err("Overwrite did not happen, but no BUG?!\n");
	else
		pr_err("list_add() corruption not detected!\n");
}

void lkdtm_CORRUPT_LIST_DEL(void)
{
	LIST_HEAD(test_head);
	struct lkdtm_list item;
	void *target[2] = { };
	void *redirection = &target;

	list_add(&item.node, &test_head);

	pr_info("attempting good list removal\n");
	list_del(&item.node);

	pr_info("attempting corrupted list removal\n");
	list_add(&item.node, &test_head);

	/* As with the list_add() test above, this corrupts "next". */
	item.node.next = redirection;
	list_del(&item.node);

	if (target[0] == NULL && target[1] == NULL)
		pr_err("Overwrite did not happen, but no BUG?!\n");
	else
		pr_err("list_del() corruption not detected!\n");
}

/* Test if unbalanced set_fs(KERNEL_DS)/set_fs(USER_DS) check exists. */
void lkdtm_CORRUPT_USER_DS(void)
{
	pr_info("setting bad task size limit\n");
	set_fs(KERNEL_DS);

	/* Make sure we do not keep running with a KERNEL_DS! */
	force_sig(SIGKILL);
}

/* Test that VMAP_STACK is actually allocating with a leading guard page */
void lkdtm_STACK_GUARD_PAGE_LEADING(void)
{
	const unsigned char *stack = task_stack_page(current);
	const unsigned char *ptr = stack - 1;
	volatile unsigned char byte;

	pr_info("attempting bad read from page below current stack\n");

	byte = *ptr;

	pr_err("FAIL: accessed page before stack!\n");
}

/* Test that VMAP_STACK is actually allocating with a trailing guard page */
void lkdtm_STACK_GUARD_PAGE_TRAILING(void)
{
	const unsigned char *stack = task_stack_page(current);
	const unsigned char *ptr = stack + THREAD_SIZE;
	volatile unsigned char byte;

	pr_info("attempting bad read from page above current stack\n");

	byte = *ptr;

	pr_err("FAIL: accessed page after stack!\n");
}

void lkdtm_UNSET_SMEP(void)
{
#if IS_ENABLED(CONFIG_X86_64) && !IS_ENABLED(CONFIG_UML)
#define MOV_CR4_DEPTH	64
	void (*direct_write_cr4)(unsigned long val);
	unsigned char *insn;
	unsigned long cr4;
	int i;

	cr4 = native_read_cr4();

	if ((cr4 & X86_CR4_SMEP) != X86_CR4_SMEP) {
		pr_err("FAIL: SMEP not in use\n");
		return;
	}
	cr4 &= ~(X86_CR4_SMEP);

	pr_info("trying to clear SMEP normally\n");
	native_write_cr4(cr4);
	if (cr4 == native_read_cr4()) {
		pr_err("FAIL: pinning SMEP failed!\n");
		cr4 |= X86_CR4_SMEP;
		pr_info("restoring SMEP\n");
		native_write_cr4(cr4);
		return;
	}
	pr_info("ok: SMEP did not get cleared\n");

	/*
	 * To test the post-write pinning verification we need to call
	 * directly into the middle of native_write_cr4() where the
	 * cr4 write happens, skipping any pinning. This searches for
	 * the cr4 writing instruction.
	 */
	insn = (unsigned char *)native_write_cr4;
	for (i = 0; i < MOV_CR4_DEPTH; i++) {
		/* mov %rdi, %cr4 */
		if (insn[i] == 0x0f && insn[i+1] == 0x22 && insn[i+2] == 0xe7)
			break;
		/* mov %rdi,%rax; mov %rax, %cr4 */
		if (insn[i]   == 0x48 && insn[i+1] == 0x89 &&
		    insn[i+2] == 0xf8 && insn[i+3] == 0x0f &&
		    insn[i+4] == 0x22 && insn[i+5] == 0xe0)
			break;
	}
	if (i >= MOV_CR4_DEPTH) {
		pr_info("ok: cannot locate cr4 writing call gadget\n");
		return;
	}
	direct_write_cr4 = (void *)(insn + i);

	pr_info("trying to clear SMEP with call gadget\n");
	direct_write_cr4(cr4);
	if (native_read_cr4() & X86_CR4_SMEP) {
		pr_info("ok: SMEP removal was reverted\n");
	} else {
		pr_err("FAIL: cleared SMEP not detected!\n");
		cr4 |= X86_CR4_SMEP;
		pr_info("restoring SMEP\n");
		native_write_cr4(cr4);
	}
#else
	pr_err("FAIL: this test is x86_64-only\n");
#endif
}
