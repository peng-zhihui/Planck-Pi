// SPDX-License-Identifier: GPL-2.0
/*
 * linux/kernel/seccomp.c
 *
 * Copyright 2004-2005  Andrea Arcangeli <andrea@cpushare.com>
 *
 * Copyright (C) 2012 Google, Inc.
 * Will Drewry <wad@chromium.org>
 *
 * This defines a simple but solid secure-computing facility.
 *
 * Mode 1 uses a fixed list of allowed system calls.
 * Mode 2 allows user-defined system call filters in the form
 *        of Berkeley Packet Filters/Linux Socket Filters.
 */

#include <linux/refcount.h>
#include <linux/audit.h>
#include <linux/compat.h>
#include <linux/coredump.h>
#include <linux/kmemleak.h>
#include <linux/nospec.h>
#include <linux/prctl.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/seccomp.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/sysctl.h>

#ifdef CONFIG_HAVE_ARCH_SECCOMP_FILTER
#include <asm/syscall.h>
#endif

#ifdef CONFIG_SECCOMP_FILTER
#include <linux/file.h>
#include <linux/filter.h>
#include <linux/pid.h>
#include <linux/ptrace.h>
#include <linux/security.h>
#include <linux/tracehook.h>
#include <linux/uaccess.h>
#include <linux/anon_inodes.h>

/*
 * When SECCOMP_IOCTL_NOTIF_ID_VALID was first introduced, it had the
 * wrong direction flag in the ioctl number. This is the broken one,
 * which the kernel needs to keep supporting until all userspaces stop
 * using the wrong command number.
 */
#define SECCOMP_IOCTL_NOTIF_ID_VALID_WRONG_DIR	SECCOMP_IOR(2, __u64)

enum notify_state {
	SECCOMP_NOTIFY_INIT,
	SECCOMP_NOTIFY_SENT,
	SECCOMP_NOTIFY_REPLIED,
};

struct seccomp_knotif {
	/* The struct pid of the task whose filter triggered the notification */
	struct task_struct *task;

	/* The "cookie" for this request; this is unique for this filter. */
	u64 id;

	/*
	 * The seccomp data. This pointer is valid the entire time this
	 * notification is active, since it comes from __seccomp_filter which
	 * eclipses the entire lifecycle here.
	 */
	const struct seccomp_data *data;

	/*
	 * Notification states. When SECCOMP_RET_USER_NOTIF is returned, a
	 * struct seccomp_knotif is created and starts out in INIT. Once the
	 * handler reads the notification off of an FD, it transitions to SENT.
	 * If a signal is received the state transitions back to INIT and
	 * another message is sent. When the userspace handler replies, state
	 * transitions to REPLIED.
	 */
	enum notify_state state;

	/* The return values, only valid when in SECCOMP_NOTIFY_REPLIED */
	int error;
	long val;

	/* Signals when this has entered SECCOMP_NOTIFY_REPLIED */
	struct completion ready;

	struct list_head list;
};

/**
 * struct notification - container for seccomp userspace notifications. Since
 * most seccomp filters will not have notification listeners attached and this
 * structure is fairly large, we store the notification-specific stuff in a
 * separate structure.
 *
 * @request: A semaphore that users of this notification can wait on for
 *           changes. Actual reads and writes are still controlled with
 *           filter->notify_lock.
 * @next_id: The id of the next request.
 * @notifications: A list of struct seccomp_knotif elements.
 * @wqh: A wait queue for poll.
 */
struct notification {
	struct semaphore request;
	u64 next_id;
	struct list_head notifications;
	wait_queue_head_t wqh;
};

/**
 * struct seccomp_filter - container for seccomp BPF programs
 *
 * @usage: reference count to manage the object lifetime.
 *         get/put helpers should be used when accessing an instance
 *         outside of a lifetime-guarded section.  In general, this
 *         is only needed for handling filters shared across tasks.
 * @log: true if all actions except for SECCOMP_RET_ALLOW should be logged
 * @prev: points to a previously installed, or inherited, filter
 * @prog: the BPF program to evaluate
 * @notif: the struct that holds all notification related information
 * @notify_lock: A lock for all notification-related accesses.
 *
 * seccomp_filter objects are organized in a tree linked via the @prev
 * pointer.  For any task, it appears to be a singly-linked list starting
 * with current->seccomp.filter, the most recently attached or inherited filter.
 * However, multiple filters may share a @prev node, by way of fork(), which
 * results in a unidirectional tree existing in memory.  This is similar to
 * how namespaces work.
 *
 * seccomp_filter objects should never be modified after being attached
 * to a task_struct (other than @usage).
 */
struct seccomp_filter {
	refcount_t usage;
	bool log;
	struct seccomp_filter *prev;
	struct bpf_prog *prog;
	struct notification *notif;
	struct mutex notify_lock;
};

/* Limit any path through the tree to 256KB worth of instructions. */
#define MAX_INSNS_PER_PATH ((1 << 18) / sizeof(struct sock_filter))

/*
 * Endianness is explicitly ignored and left for BPF program authors to manage
 * as per the specific architecture.
 */
static void populate_seccomp_data(struct seccomp_data *sd)
{
	struct task_struct *task = current;
	struct pt_regs *regs = task_pt_regs(task);
	unsigned long args[6];

	sd->nr = syscall_get_nr(task, regs);
	sd->arch = syscall_get_arch(task);
	syscall_get_arguments(task, regs, args);
	sd->args[0] = args[0];
	sd->args[1] = args[1];
	sd->args[2] = args[2];
	sd->args[3] = args[3];
	sd->args[4] = args[4];
	sd->args[5] = args[5];
	sd->instruction_pointer = KSTK_EIP(task);
}

/**
 *	seccomp_check_filter - verify seccomp filter code
 *	@filter: filter to verify
 *	@flen: length of filter
 *
 * Takes a previously checked filter (by bpf_check_classic) and
 * redirects all filter code that loads struct sk_buff data
 * and related data through seccomp_bpf_load.  It also
 * enforces length and alignment checking of those loads.
 *
 * Returns 0 if the rule set is legal or -EINVAL if not.
 */
static int seccomp_check_filter(struct sock_filter *filter, unsigned int flen)
{
	int pc;
	for (pc = 0; pc < flen; pc++) {
		struct sock_filter *ftest = &filter[pc];
		u16 code = ftest->code;
		u32 k = ftest->k;

		switch (code) {
		case BPF_LD | BPF_W | BPF_ABS:
			ftest->code = BPF_LDX | BPF_W | BPF_ABS;
			/* 32-bit aligned and not out of bounds. */
			if (k >= sizeof(struct seccomp_data) || k & 3)
				return -EINVAL;
			continue;
		case BPF_LD | BPF_W | BPF_LEN:
			ftest->code = BPF_LD | BPF_IMM;
			ftest->k = sizeof(struct seccomp_data);
			continue;
		case BPF_LDX | BPF_W | BPF_LEN:
			ftest->code = BPF_LDX | BPF_IMM;
			ftest->k = sizeof(struct seccomp_data);
			continue;
		/* Explicitly include allowed calls. */
		case BPF_RET | BPF_K:
		case BPF_RET | BPF_A:
		case BPF_ALU | BPF_ADD | BPF_K:
		case BPF_ALU | BPF_ADD | BPF_X:
		case BPF_ALU | BPF_SUB | BPF_K:
		case BPF_ALU | BPF_SUB | BPF_X:
		case BPF_ALU | BPF_MUL | BPF_K:
		case BPF_ALU | BPF_MUL | BPF_X:
		case BPF_ALU | BPF_DIV | BPF_K:
		case BPF_ALU | BPF_DIV | BPF_X:
		case BPF_ALU | BPF_AND | BPF_K:
		case BPF_ALU | BPF_AND | BPF_X:
		case BPF_ALU | BPF_OR | BPF_K:
		case BPF_ALU | BPF_OR | BPF_X:
		case BPF_ALU | BPF_XOR | BPF_K:
		case BPF_ALU | BPF_XOR | BPF_X:
		case BPF_ALU | BPF_LSH | BPF_K:
		case BPF_ALU | BPF_LSH | BPF_X:
		case BPF_ALU | BPF_RSH | BPF_K:
		case BPF_ALU | BPF_RSH | BPF_X:
		case BPF_ALU | BPF_NEG:
		case BPF_LD | BPF_IMM:
		case BPF_LDX | BPF_IMM:
		case BPF_MISC | BPF_TAX:
		case BPF_MISC | BPF_TXA:
		case BPF_LD | BPF_MEM:
		case BPF_LDX | BPF_MEM:
		case BPF_ST:
		case BPF_STX:
		case BPF_JMP | BPF_JA:
		case BPF_JMP | BPF_JEQ | BPF_K:
		case BPF_JMP | BPF_JEQ | BPF_X:
		case BPF_JMP | BPF_JGE | BPF_K:
		case BPF_JMP | BPF_JGE | BPF_X:
		case BPF_JMP | BPF_JGT | BPF_K:
		case BPF_JMP | BPF_JGT | BPF_X:
		case BPF_JMP | BPF_JSET | BPF_K:
		case BPF_JMP | BPF_JSET | BPF_X:
			continue;
		default:
			return -EINVAL;
		}
	}
	return 0;
}

/**
 * seccomp_run_filters - evaluates all seccomp filters against @sd
 * @sd: optional seccomp data to be passed to filters
 * @match: stores struct seccomp_filter that resulted in the return value,
 *         unless filter returned SECCOMP_RET_ALLOW, in which case it will
 *         be unchanged.
 *
 * Returns valid seccomp BPF response codes.
 */
#define ACTION_ONLY(ret) ((s32)((ret) & (SECCOMP_RET_ACTION_FULL)))
static u32 seccomp_run_filters(const struct seccomp_data *sd,
			       struct seccomp_filter **match)
{
	u32 ret = SECCOMP_RET_ALLOW;
	/* Make sure cross-thread synced filter points somewhere sane. */
	struct seccomp_filter *f =
			READ_ONCE(current->seccomp.filter);

	/* Ensure unexpected behavior doesn't result in failing open. */
	if (WARN_ON(f == NULL))
		return SECCOMP_RET_KILL_PROCESS;

	/*
	 * All filters in the list are evaluated and the lowest BPF return
	 * value always takes priority (ignoring the DATA).
	 */
	preempt_disable();
	for (; f; f = f->prev) {
		u32 cur_ret = BPF_PROG_RUN(f->prog, sd);

		if (ACTION_ONLY(cur_ret) < ACTION_ONLY(ret)) {
			ret = cur_ret;
			*match = f;
		}
	}
	preempt_enable();
	return ret;
}
#endif /* CONFIG_SECCOMP_FILTER */

static inline bool seccomp_may_assign_mode(unsigned long seccomp_mode)
{
	assert_spin_locked(&current->sighand->siglock);

	if (current->seccomp.mode && current->seccomp.mode != seccomp_mode)
		return false;

	return true;
}

void __weak arch_seccomp_spec_mitigate(struct task_struct *task) { }

static inline void seccomp_assign_mode(struct task_struct *task,
				       unsigned long seccomp_mode,
				       unsigned long flags)
{
	assert_spin_locked(&task->sighand->siglock);

	task->seccomp.mode = seccomp_mode;
	/*
	 * Make sure TIF_SECCOMP cannot be set before the mode (and
	 * filter) is set.
	 */
	smp_mb__before_atomic();
	/* Assume default seccomp processes want spec flaw mitigation. */
	if ((flags & SECCOMP_FILTER_FLAG_SPEC_ALLOW) == 0)
		arch_seccomp_spec_mitigate(task);
	set_tsk_thread_flag(task, TIF_SECCOMP);
}

#ifdef CONFIG_SECCOMP_FILTER
/* Returns 1 if the parent is an ancestor of the child. */
static int is_ancestor(struct seccomp_filter *parent,
		       struct seccomp_filter *child)
{
	/* NULL is the root ancestor. */
	if (parent == NULL)
		return 1;
	for (; child; child = child->prev)
		if (child == parent)
			return 1;
	return 0;
}

/**
 * seccomp_can_sync_threads: checks if all threads can be synchronized
 *
 * Expects sighand and cred_guard_mutex locks to be held.
 *
 * Returns 0 on success, -ve on error, or the pid of a thread which was
 * either not in the correct seccomp mode or did not have an ancestral
 * seccomp filter.
 */
static inline pid_t seccomp_can_sync_threads(void)
{
	struct task_struct *thread, *caller;

	BUG_ON(!mutex_is_locked(&current->signal->cred_guard_mutex));
	assert_spin_locked(&current->sighand->siglock);

	/* Validate all threads being eligible for synchronization. */
	caller = current;
	for_each_thread(caller, thread) {
		pid_t failed;

		/* Skip current, since it is initiating the sync. */
		if (thread == caller)
			continue;

		if (thread->seccomp.mode == SECCOMP_MODE_DISABLED ||
		    (thread->seccomp.mode == SECCOMP_MODE_FILTER &&
		     is_ancestor(thread->seccomp.filter,
				 caller->seccomp.filter)))
			continue;

		/* Return the first thread that cannot be synchronized. */
		failed = task_pid_vnr(thread);
		/* If the pid cannot be resolved, then return -ESRCH */
		if (WARN_ON(failed == 0))
			failed = -ESRCH;
		return failed;
	}

	return 0;
}

/**
 * seccomp_sync_threads: sets all threads to use current's filter
 *
 * Expects sighand and cred_guard_mutex locks to be held, and for
 * seccomp_can_sync_threads() to have returned success already
 * without dropping the locks.
 *
 */
static inline void seccomp_sync_threads(unsigned long flags)
{
	struct task_struct *thread, *caller;

	BUG_ON(!mutex_is_locked(&current->signal->cred_guard_mutex));
	assert_spin_locked(&current->sighand->siglock);

	/* Synchronize all threads. */
	caller = current;
	for_each_thread(caller, thread) {
		/* Skip current, since it needs no changes. */
		if (thread == caller)
			continue;

		/* Get a task reference for the new leaf node. */
		get_seccomp_filter(caller);
		/*
		 * Drop the task reference to the shared ancestor since
		 * current's path will hold a reference.  (This also
		 * allows a put before the assignment.)
		 */
		put_seccomp_filter(thread);
		smp_store_release(&thread->seccomp.filter,
				  caller->seccomp.filter);

		/*
		 * Don't let an unprivileged task work around
		 * the no_new_privs restriction by creating
		 * a thread that sets it up, enters seccomp,
		 * then dies.
		 */
		if (task_no_new_privs(caller))
			task_set_no_new_privs(thread);

		/*
		 * Opt the other thread into seccomp if needed.
		 * As threads are considered to be trust-realm
		 * equivalent (see ptrace_may_access), it is safe to
		 * allow one thread to transition the other.
		 */
		if (thread->seccomp.mode == SECCOMP_MODE_DISABLED)
			seccomp_assign_mode(thread, SECCOMP_MODE_FILTER,
					    flags);
	}
}

/**
 * seccomp_prepare_filter: Prepares a seccomp filter for use.
 * @fprog: BPF program to install
 *
 * Returns filter on success or an ERR_PTR on failure.
 */
static struct seccomp_filter *seccomp_prepare_filter(struct sock_fprog *fprog)
{
	struct seccomp_filter *sfilter;
	int ret;
	const bool save_orig = IS_ENABLED(CONFIG_CHECKPOINT_RESTORE);

	if (fprog->len == 0 || fprog->len > BPF_MAXINSNS)
		return ERR_PTR(-EINVAL);

	BUG_ON(INT_MAX / fprog->len < sizeof(struct sock_filter));

	/*
	 * Installing a seccomp filter requires that the task has
	 * CAP_SYS_ADMIN in its namespace or be running with no_new_privs.
	 * This avoids scenarios where unprivileged tasks can affect the
	 * behavior of privileged children.
	 */
	if (!task_no_new_privs(current) &&
	    security_capable(current_cred(), current_user_ns(),
				     CAP_SYS_ADMIN, CAP_OPT_NOAUDIT) != 0)
		return ERR_PTR(-EACCES);

	/* Allocate a new seccomp_filter */
	sfilter = kzalloc(sizeof(*sfilter), GFP_KERNEL | __GFP_NOWARN);
	if (!sfilter)
		return ERR_PTR(-ENOMEM);

	mutex_init(&sfilter->notify_lock);
	ret = bpf_prog_create_from_user(&sfilter->prog, fprog,
					seccomp_check_filter, save_orig);
	if (ret < 0) {
		kfree(sfilter);
		return ERR_PTR(ret);
	}

	refcount_set(&sfilter->usage, 1);

	return sfilter;
}

/**
 * seccomp_prepare_user_filter - prepares a user-supplied sock_fprog
 * @user_filter: pointer to the user data containing a sock_fprog.
 *
 * Returns 0 on success and non-zero otherwise.
 */
static struct seccomp_filter *
seccomp_prepare_user_filter(const char __user *user_filter)
{
	struct sock_fprog fprog;
	struct seccomp_filter *filter = ERR_PTR(-EFAULT);

#ifdef CONFIG_COMPAT
	if (in_compat_syscall()) {
		struct compat_sock_fprog fprog32;
		if (copy_from_user(&fprog32, user_filter, sizeof(fprog32)))
			goto out;
		fprog.len = fprog32.len;
		fprog.filter = compat_ptr(fprog32.filter);
	} else /* falls through to the if below. */
#endif
	if (copy_from_user(&fprog, user_filter, sizeof(fprog)))
		goto out;
	filter = seccomp_prepare_filter(&fprog);
out:
	return filter;
}

/**
 * seccomp_attach_filter: validate and attach filter
 * @flags:  flags to change filter behavior
 * @filter: seccomp filter to add to the current process
 *
 * Caller must be holding current->sighand->siglock lock.
 *
 * Returns 0 on success, -ve on error, or
 *   - in TSYNC mode: the pid of a thread which was either not in the correct
 *     seccomp mode or did not have an ancestral seccomp filter
 *   - in NEW_LISTENER mode: the fd of the new listener
 */
static long seccomp_attach_filter(unsigned int flags,
				  struct seccomp_filter *filter)
{
	unsigned long total_insns;
	struct seccomp_filter *walker;

	assert_spin_locked(&current->sighand->siglock);

	/* Validate resulting filter length. */
	total_insns = filter->prog->len;
	for (walker = current->seccomp.filter; walker; walker = walker->prev)
		total_insns += walker->prog->len + 4;  /* 4 instr penalty */
	if (total_insns > MAX_INSNS_PER_PATH)
		return -ENOMEM;

	/* If thread sync has been requested, check that it is possible. */
	if (flags & SECCOMP_FILTER_FLAG_TSYNC) {
		int ret;

		ret = seccomp_can_sync_threads();
		if (ret)
			return ret;
	}

	/* Set log flag, if present. */
	if (flags & SECCOMP_FILTER_FLAG_LOG)
		filter->log = true;

	/*
	 * If there is an existing filter, make it the prev and don't drop its
	 * task reference.
	 */
	filter->prev = current->seccomp.filter;
	current->seccomp.filter = filter;

	/* Now that the new filter is in place, synchronize to all threads. */
	if (flags & SECCOMP_FILTER_FLAG_TSYNC)
		seccomp_sync_threads(flags);

	return 0;
}

static void __get_seccomp_filter(struct seccomp_filter *filter)
{
	refcount_inc(&filter->usage);
}

/* get_seccomp_filter - increments the reference count of the filter on @tsk */
void get_seccomp_filter(struct task_struct *tsk)
{
	struct seccomp_filter *orig = tsk->seccomp.filter;
	if (!orig)
		return;
	__get_seccomp_filter(orig);
}

static inline void seccomp_filter_free(struct seccomp_filter *filter)
{
	if (filter) {
		bpf_prog_destroy(filter->prog);
		kfree(filter);
	}
}

static void __put_seccomp_filter(struct seccomp_filter *orig)
{
	/* Clean up single-reference branches iteratively. */
	while (orig && refcount_dec_and_test(&orig->usage)) {
		struct seccomp_filter *freeme = orig;
		orig = orig->prev;
		seccomp_filter_free(freeme);
	}
}

/* put_seccomp_filter - decrements the ref count of tsk->seccomp.filter */
void put_seccomp_filter(struct task_struct *tsk)
{
	__put_seccomp_filter(tsk->seccomp.filter);
}

static void seccomp_init_siginfo(kernel_siginfo_t *info, int syscall, int reason)
{
	clear_siginfo(info);
	info->si_signo = SIGSYS;
	info->si_code = SYS_SECCOMP;
	info->si_call_addr = (void __user *)KSTK_EIP(current);
	info->si_errno = reason;
	info->si_arch = syscall_get_arch(current);
	info->si_syscall = syscall;
}

/**
 * seccomp_send_sigsys - signals the task to allow in-process syscall emulation
 * @syscall: syscall number to send to userland
 * @reason: filter-supplied reason code to send to userland (via si_errno)
 *
 * Forces a SIGSYS with a code of SYS_SECCOMP and related sigsys info.
 */
static void seccomp_send_sigsys(int syscall, int reason)
{
	struct kernel_siginfo info;
	seccomp_init_siginfo(&info, syscall, reason);
	force_sig_info(&info);
}
#endif	/* CONFIG_SECCOMP_FILTER */

/* For use with seccomp_actions_logged */
#define SECCOMP_LOG_KILL_PROCESS	(1 << 0)
#define SECCOMP_LOG_KILL_THREAD		(1 << 1)
#define SECCOMP_LOG_TRAP		(1 << 2)
#define SECCOMP_LOG_ERRNO		(1 << 3)
#define SECCOMP_LOG_TRACE		(1 << 4)
#define SECCOMP_LOG_LOG			(1 << 5)
#define SECCOMP_LOG_ALLOW		(1 << 6)
#define SECCOMP_LOG_USER_NOTIF		(1 << 7)

static u32 seccomp_actions_logged = SECCOMP_LOG_KILL_PROCESS |
				    SECCOMP_LOG_KILL_THREAD  |
				    SECCOMP_LOG_TRAP  |
				    SECCOMP_LOG_ERRNO |
				    SECCOMP_LOG_USER_NOTIF |
				    SECCOMP_LOG_TRACE |
				    SECCOMP_LOG_LOG;

static inline void seccomp_log(unsigned long syscall, long signr, u32 action,
			       bool requested)
{
	bool log = false;

	switch (action) {
	case SECCOMP_RET_ALLOW:
		break;
	case SECCOMP_RET_TRAP:
		log = requested && seccomp_actions_logged & SECCOMP_LOG_TRAP;
		break;
	case SECCOMP_RET_ERRNO:
		log = requested && seccomp_actions_logged & SECCOMP_LOG_ERRNO;
		break;
	case SECCOMP_RET_TRACE:
		log = requested && seccomp_actions_logged & SECCOMP_LOG_TRACE;
		break;
	case SECCOMP_RET_USER_NOTIF:
		log = requested && seccomp_actions_logged & SECCOMP_LOG_USER_NOTIF;
		break;
	case SECCOMP_RET_LOG:
		log = seccomp_actions_logged & SECCOMP_LOG_LOG;
		break;
	case SECCOMP_RET_KILL_THREAD:
		log = seccomp_actions_logged & SECCOMP_LOG_KILL_THREAD;
		break;
	case SECCOMP_RET_KILL_PROCESS:
	default:
		log = seccomp_actions_logged & SECCOMP_LOG_KILL_PROCESS;
	}

	/*
	 * Emit an audit message when the action is RET_KILL_*, RET_LOG, or the
	 * FILTER_FLAG_LOG bit was set. The admin has the ability to silence
	 * any action from being logged by removing the action name from the
	 * seccomp_actions_logged sysctl.
	 */
	if (!log)
		return;

	audit_seccomp(syscall, signr, action);
}

/*
 * Secure computing mode 1 allows only read/write/exit/sigreturn.
 * To be fully secure this must be combined with rlimit
 * to limit the stack allocations too.
 */
static const int mode1_syscalls[] = {
	__NR_seccomp_read, __NR_seccomp_write, __NR_seccomp_exit, __NR_seccomp_sigreturn,
	0, /* null terminated */
};

static void __secure_computing_strict(int this_syscall)
{
	const int *syscall_whitelist = mode1_syscalls;
#ifdef CONFIG_COMPAT
	if (in_compat_syscall())
		syscall_whitelist = get_compat_mode1_syscalls();
#endif
	do {
		if (*syscall_whitelist == this_syscall)
			return;
	} while (*++syscall_whitelist);

#ifdef SECCOMP_DEBUG
	dump_stack();
#endif
	seccomp_log(this_syscall, SIGKILL, SECCOMP_RET_KILL_THREAD, true);
	do_exit(SIGKILL);
}

#ifndef CONFIG_HAVE_ARCH_SECCOMP_FILTER
void secure_computing_strict(int this_syscall)
{
	int mode = current->seccomp.mode;

	if (IS_ENABLED(CONFIG_CHECKPOINT_RESTORE) &&
	    unlikely(current->ptrace & PT_SUSPEND_SECCOMP))
		return;

	if (mode == SECCOMP_MODE_DISABLED)
		return;
	else if (mode == SECCOMP_MODE_STRICT)
		__secure_computing_strict(this_syscall);
	else
		BUG();
}
#else

#ifdef CONFIG_SECCOMP_FILTER
static u64 seccomp_next_notify_id(struct seccomp_filter *filter)
{
	/*
	 * Note: overflow is ok here, the id just needs to be unique per
	 * filter.
	 */
	lockdep_assert_held(&filter->notify_lock);
	return filter->notif->next_id++;
}

static void seccomp_do_user_notification(int this_syscall,
					 struct seccomp_filter *match,
					 const struct seccomp_data *sd)
{
	int err;
	long ret = 0;
	struct seccomp_knotif n = {};

	mutex_lock(&match->notify_lock);
	err = -ENOSYS;
	if (!match->notif)
		goto out;

	n.task = current;
	n.state = SECCOMP_NOTIFY_INIT;
	n.data = sd;
	n.id = seccomp_next_notify_id(match);
	init_completion(&n.ready);
	list_add(&n.list, &match->notif->notifications);

	up(&match->notif->request);
	wake_up_poll(&match->notif->wqh, EPOLLIN | EPOLLRDNORM);
	mutex_unlock(&match->notify_lock);

	/*
	 * This is where we wait for a reply from userspace.
	 */
	err = wait_for_completion_interruptible(&n.ready);
	mutex_lock(&match->notify_lock);
	if (err == 0) {
		ret = n.val;
		err = n.error;
	}

	/*
	 * Note that it's possible the listener died in between the time when
	 * we were notified of a respons (or a signal) and when we were able to
	 * re-acquire the lock, so only delete from the list if the
	 * notification actually exists.
	 *
	 * Also note that this test is only valid because there's no way to
	 * *reattach* to a notifier right now. If one is added, we'll need to
	 * keep track of the notif itself and make sure they match here.
	 */
	if (match->notif)
		list_del(&n.list);
out:
	mutex_unlock(&match->notify_lock);
	syscall_set_return_value(current, task_pt_regs(current),
				 err, ret);
}

static int __seccomp_filter(int this_syscall, const struct seccomp_data *sd,
			    const bool recheck_after_trace)
{
	u32 filter_ret, action;
	struct seccomp_filter *match = NULL;
	int data;
	struct seccomp_data sd_local;

	/*
	 * Make sure that any changes to mode from another thread have
	 * been seen after TIF_SECCOMP was seen.
	 */
	rmb();

	if (!sd) {
		populate_seccomp_data(&sd_local);
		sd = &sd_local;
	}

	filter_ret = seccomp_run_filters(sd, &match);
	data = filter_ret & SECCOMP_RET_DATA;
	action = filter_ret & SECCOMP_RET_ACTION_FULL;

	switch (action) {
	case SECCOMP_RET_ERRNO:
		/* Set low-order bits as an errno, capped at MAX_ERRNO. */
		if (data > MAX_ERRNO)
			data = MAX_ERRNO;
		syscall_set_return_value(current, task_pt_regs(current),
					 -data, 0);
		goto skip;

	case SECCOMP_RET_TRAP:
		/* Show the handler the original registers. */
		syscall_rollback(current, task_pt_regs(current));
		/* Let the filter pass back 16 bits of data. */
		seccomp_send_sigsys(this_syscall, data);
		goto skip;

	case SECCOMP_RET_TRACE:
		/* We've been put in this state by the ptracer already. */
		if (recheck_after_trace)
			return 0;

		/* ENOSYS these calls if there is no tracer attached. */
		if (!ptrace_event_enabled(current, PTRACE_EVENT_SECCOMP)) {
			syscall_set_return_value(current,
						 task_pt_regs(current),
						 -ENOSYS, 0);
			goto skip;
		}

		/* Allow the BPF to provide the event message */
		ptrace_event(PTRACE_EVENT_SECCOMP, data);
		/*
		 * The delivery of a fatal signal during event
		 * notification may silently skip tracer notification,
		 * which could leave us with a potentially unmodified
		 * syscall that the tracer would have liked to have
		 * changed. Since the process is about to die, we just
		 * force the syscall to be skipped and let the signal
		 * kill the process and correctly handle any tracer exit
		 * notifications.
		 */
		if (fatal_signal_pending(current))
			goto skip;
		/* Check if the tracer forced the syscall to be skipped. */
		this_syscall = syscall_get_nr(current, task_pt_regs(current));
		if (this_syscall < 0)
			goto skip;

		/*
		 * Recheck the syscall, since it may have changed. This
		 * intentionally uses a NULL struct seccomp_data to force
		 * a reload of all registers. This does not goto skip since
		 * a skip would have already been reported.
		 */
		if (__seccomp_filter(this_syscall, NULL, true))
			return -1;

		return 0;

	case SECCOMP_RET_USER_NOTIF:
		seccomp_do_user_notification(this_syscall, match, sd);
		goto skip;

	case SECCOMP_RET_LOG:
		seccomp_log(this_syscall, 0, action, true);
		return 0;

	case SECCOMP_RET_ALLOW:
		/*
		 * Note that the "match" filter will always be NULL for
		 * this action since SECCOMP_RET_ALLOW is the starting
		 * state in seccomp_run_filters().
		 */
		return 0;

	case SECCOMP_RET_KILL_THREAD:
	case SECCOMP_RET_KILL_PROCESS:
	default:
		seccomp_log(this_syscall, SIGSYS, action, true);
		/* Dump core only if this is the last remaining thread. */
		if (action == SECCOMP_RET_KILL_PROCESS ||
		    get_nr_threads(current) == 1) {
			kernel_siginfo_t info;

			/* Show the original registers in the dump. */
			syscall_rollback(current, task_pt_regs(current));
			/* Trigger a manual coredump since do_exit skips it. */
			seccomp_init_siginfo(&info, this_syscall, data);
			do_coredump(&info);
		}
		if (action == SECCOMP_RET_KILL_PROCESS)
			do_group_exit(SIGSYS);
		else
			do_exit(SIGSYS);
	}

	unreachable();

skip:
	seccomp_log(this_syscall, 0, action, match ? match->log : false);
	return -1;
}
#else
static int __seccomp_filter(int this_syscall, const struct seccomp_data *sd,
			    const bool recheck_after_trace)
{
	BUG();
}
#endif

int __secure_computing(const struct seccomp_data *sd)
{
	int mode = current->seccomp.mode;
	int this_syscall;

	if (IS_ENABLED(CONFIG_CHECKPOINT_RESTORE) &&
	    unlikely(current->ptrace & PT_SUSPEND_SECCOMP))
		return 0;

	this_syscall = sd ? sd->nr :
		syscall_get_nr(current, task_pt_regs(current));

	switch (mode) {
	case SECCOMP_MODE_STRICT:
		__secure_computing_strict(this_syscall);  /* may call do_exit */
		return 0;
	case SECCOMP_MODE_FILTER:
		return __seccomp_filter(this_syscall, sd, false);
	default:
		BUG();
	}
}
#endif /* CONFIG_HAVE_ARCH_SECCOMP_FILTER */

long prctl_get_seccomp(void)
{
	return current->seccomp.mode;
}

/**
 * seccomp_set_mode_strict: internal function for setting strict seccomp
 *
 * Once current->seccomp.mode is non-zero, it may not be changed.
 *
 * Returns 0 on success or -EINVAL on failure.
 */
static long seccomp_set_mode_strict(void)
{
	const unsigned long seccomp_mode = SECCOMP_MODE_STRICT;
	long ret = -EINVAL;

	spin_lock_irq(&current->sighand->siglock);

	if (!seccomp_may_assign_mode(seccomp_mode))
		goto out;

#ifdef TIF_NOTSC
	disable_TSC();
#endif
	seccomp_assign_mode(current, seccomp_mode, 0);
	ret = 0;

out:
	spin_unlock_irq(&current->sighand->siglock);

	return ret;
}

#ifdef CONFIG_SECCOMP_FILTER
static int seccomp_notify_release(struct inode *inode, struct file *file)
{
	struct seccomp_filter *filter = file->private_data;
	struct seccomp_knotif *knotif;

	if (!filter)
		return 0;

	mutex_lock(&filter->notify_lock);

	/*
	 * If this file is being closed because e.g. the task who owned it
	 * died, let's wake everyone up who was waiting on us.
	 */
	list_for_each_entry(knotif, &filter->notif->notifications, list) {
		if (knotif->state == SECCOMP_NOTIFY_REPLIED)
			continue;

		knotif->state = SECCOMP_NOTIFY_REPLIED;
		knotif->error = -ENOSYS;
		knotif->val = 0;

		complete(&knotif->ready);
	}

	kfree(filter->notif);
	filter->notif = NULL;
	mutex_unlock(&filter->notify_lock);
	__put_seccomp_filter(filter);
	return 0;
}

static long seccomp_notify_recv(struct seccomp_filter *filter,
				void __user *buf)
{
	struct seccomp_knotif *knotif = NULL, *cur;
	struct seccomp_notif unotif;
	ssize_t ret;

	/* Verify that we're not given garbage to keep struct extensible. */
	ret = check_zeroed_user(buf, sizeof(unotif));
	if (ret < 0)
		return ret;
	if (!ret)
		return -EINVAL;

	memset(&unotif, 0, sizeof(unotif));

	ret = down_interruptible(&filter->notif->request);
	if (ret < 0)
		return ret;

	mutex_lock(&filter->notify_lock);
	list_for_each_entry(cur, &filter->notif->notifications, list) {
		if (cur->state == SECCOMP_NOTIFY_INIT) {
			knotif = cur;
			break;
		}
	}

	/*
	 * If we didn't find a notification, it could be that the task was
	 * interrupted by a fatal signal between the time we were woken and
	 * when we were able to acquire the rw lock.
	 */
	if (!knotif) {
		ret = -ENOENT;
		goto out;
	}

	unotif.id = knotif->id;
	unotif.pid = task_pid_vnr(knotif->task);
	unotif.data = *(knotif->data);

	knotif->state = SECCOMP_NOTIFY_SENT;
	wake_up_poll(&filter->notif->wqh, EPOLLOUT | EPOLLWRNORM);
	ret = 0;
out:
	mutex_unlock(&filter->notify_lock);

	if (ret == 0 && copy_to_user(buf, &unotif, sizeof(unotif))) {
		ret = -EFAULT;

		/*
		 * Userspace screwed up. To make sure that we keep this
		 * notification alive, let's reset it back to INIT. It
		 * may have died when we released the lock, so we need to make
		 * sure it's still around.
		 */
		knotif = NULL;
		mutex_lock(&filter->notify_lock);
		list_for_each_entry(cur, &filter->notif->notifications, list) {
			if (cur->id == unotif.id) {
				knotif = cur;
				break;
			}
		}

		if (knotif) {
			knotif->state = SECCOMP_NOTIFY_INIT;
			up(&filter->notif->request);
		}
		mutex_unlock(&filter->notify_lock);
	}

	return ret;
}

static long seccomp_notify_send(struct seccomp_filter *filter,
				void __user *buf)
{
	struct seccomp_notif_resp resp = {};
	struct seccomp_knotif *knotif = NULL, *cur;
	long ret;

	if (copy_from_user(&resp, buf, sizeof(resp)))
		return -EFAULT;

	if (resp.flags)
		return -EINVAL;

	ret = mutex_lock_interruptible(&filter->notify_lock);
	if (ret < 0)
		return ret;

	list_for_each_entry(cur, &filter->notif->notifications, list) {
		if (cur->id == resp.id) {
			knotif = cur;
			break;
		}
	}

	if (!knotif) {
		ret = -ENOENT;
		goto out;
	}

	/* Allow exactly one reply. */
	if (knotif->state != SECCOMP_NOTIFY_SENT) {
		ret = -EINPROGRESS;
		goto out;
	}

	ret = 0;
	knotif->state = SECCOMP_NOTIFY_REPLIED;
	knotif->error = resp.error;
	knotif->val = resp.val;
	complete(&knotif->ready);
out:
	mutex_unlock(&filter->notify_lock);
	return ret;
}

static long seccomp_notify_id_valid(struct seccomp_filter *filter,
				    void __user *buf)
{
	struct seccomp_knotif *knotif = NULL;
	u64 id;
	long ret;

	if (copy_from_user(&id, buf, sizeof(id)))
		return -EFAULT;

	ret = mutex_lock_interruptible(&filter->notify_lock);
	if (ret < 0)
		return ret;

	ret = -ENOENT;
	list_for_each_entry(knotif, &filter->notif->notifications, list) {
		if (knotif->id == id) {
			if (knotif->state == SECCOMP_NOTIFY_SENT)
				ret = 0;
			goto out;
		}
	}

out:
	mutex_unlock(&filter->notify_lock);
	return ret;
}

static long seccomp_notify_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct seccomp_filter *filter = file->private_data;
	void __user *buf = (void __user *)arg;

	switch (cmd) {
	case SECCOMP_IOCTL_NOTIF_RECV:
		return seccomp_notify_recv(filter, buf);
	case SECCOMP_IOCTL_NOTIF_SEND:
		return seccomp_notify_send(filter, buf);
	case SECCOMP_IOCTL_NOTIF_ID_VALID_WRONG_DIR:
	case SECCOMP_IOCTL_NOTIF_ID_VALID:
		return seccomp_notify_id_valid(filter, buf);
	default:
		return -EINVAL;
	}
}

static __poll_t seccomp_notify_poll(struct file *file,
				    struct poll_table_struct *poll_tab)
{
	struct seccomp_filter *filter = file->private_data;
	__poll_t ret = 0;
	struct seccomp_knotif *cur;

	poll_wait(file, &filter->notif->wqh, poll_tab);

	if (mutex_lock_interruptible(&filter->notify_lock) < 0)
		return EPOLLERR;

	list_for_each_entry(cur, &filter->notif->notifications, list) {
		if (cur->state == SECCOMP_NOTIFY_INIT)
			ret |= EPOLLIN | EPOLLRDNORM;
		if (cur->state == SECCOMP_NOTIFY_SENT)
			ret |= EPOLLOUT | EPOLLWRNORM;
		if ((ret & EPOLLIN) && (ret & EPOLLOUT))
			break;
	}

	mutex_unlock(&filter->notify_lock);

	return ret;
}

static const struct file_operations seccomp_notify_ops = {
	.poll = seccomp_notify_poll,
	.release = seccomp_notify_release,
	.unlocked_ioctl = seccomp_notify_ioctl,
	.compat_ioctl = seccomp_notify_ioctl,
};

static struct file *init_listener(struct seccomp_filter *filter)
{
	struct file *ret;

	ret = ERR_PTR(-ENOMEM);
	filter->notif = kzalloc(sizeof(*(filter->notif)), GFP_KERNEL);
	if (!filter->notif)
		goto out;

	sema_init(&filter->notif->request, 0);
	filter->notif->next_id = get_random_u64();
	INIT_LIST_HEAD(&filter->notif->notifications);
	init_waitqueue_head(&filter->notif->wqh);

	ret = anon_inode_getfile("seccomp notify", &seccomp_notify_ops,
				 filter, O_RDWR);
	if (IS_ERR(ret))
		goto out_notif;

	/* The file has a reference to it now */
	__get_seccomp_filter(filter);

out_notif:
	if (IS_ERR(ret))
		kfree(filter->notif);
out:
	return ret;
}

/*
 * Does @new_child have a listener while an ancestor also has a listener?
 * If so, we'll want to reject this filter.
 * This only has to be tested for the current process, even in the TSYNC case,
 * because TSYNC installs @child with the same parent on all threads.
 * Note that @new_child is not hooked up to its parent at this point yet, so
 * we use current->seccomp.filter.
 */
static bool has_duplicate_listener(struct seccomp_filter *new_child)
{
	struct seccomp_filter *cur;

	/* must be protected against concurrent TSYNC */
	lockdep_assert_held(&current->sighand->siglock);

	if (!new_child->notif)
		return false;
	for (cur = current->seccomp.filter; cur; cur = cur->prev) {
		if (cur->notif)
			return true;
	}

	return false;
}

/**
 * seccomp_set_mode_filter: internal function for setting seccomp filter
 * @flags:  flags to change filter behavior
 * @filter: struct sock_fprog containing filter
 *
 * This function may be called repeatedly to install additional filters.
 * Every filter successfully installed will be evaluated (in reverse order)
 * for each system call the task makes.
 *
 * Once current->seccomp.mode is non-zero, it may not be changed.
 *
 * Returns 0 on success or -EINVAL on failure.
 */
static long seccomp_set_mode_filter(unsigned int flags,
				    const char __user *filter)
{
	const unsigned long seccomp_mode = SECCOMP_MODE_FILTER;
	struct seccomp_filter *prepared = NULL;
	long ret = -EINVAL;
	int listener = -1;
	struct file *listener_f = NULL;

	/* Validate flags. */
	if (flags & ~SECCOMP_FILTER_FLAG_MASK)
		return -EINVAL;

	/*
	 * In the successful case, NEW_LISTENER returns the new listener fd.
	 * But in the failure case, TSYNC returns the thread that died. If you
	 * combine these two flags, there's no way to tell whether something
	 * succeeded or failed. So, let's disallow this combination.
	 */
	if ((flags & SECCOMP_FILTER_FLAG_TSYNC) &&
	    (flags & SECCOMP_FILTER_FLAG_NEW_LISTENER))
		return -EINVAL;

	/* Prepare the new filter before holding any locks. */
	prepared = seccomp_prepare_user_filter(filter);
	if (IS_ERR(prepared))
		return PTR_ERR(prepared);

	if (flags & SECCOMP_FILTER_FLAG_NEW_LISTENER) {
		listener = get_unused_fd_flags(O_CLOEXEC);
		if (listener < 0) {
			ret = listener;
			goto out_free;
		}

		listener_f = init_listener(prepared);
		if (IS_ERR(listener_f)) {
			put_unused_fd(listener);
			ret = PTR_ERR(listener_f);
			goto out_free;
		}
	}

	/*
	 * Make sure we cannot change seccomp or nnp state via TSYNC
	 * while another thread is in the middle of calling exec.
	 */
	if (flags & SECCOMP_FILTER_FLAG_TSYNC &&
	    mutex_lock_killable(&current->signal->cred_guard_mutex))
		goto out_put_fd;

	spin_lock_irq(&current->sighand->siglock);

	if (!seccomp_may_assign_mode(seccomp_mode))
		goto out;

	if (has_duplicate_listener(prepared)) {
		ret = -EBUSY;
		goto out;
	}

	ret = seccomp_attach_filter(flags, prepared);
	if (ret)
		goto out;
	/* Do not free the successfully attached filter. */
	prepared = NULL;

	seccomp_assign_mode(current, seccomp_mode, flags);
out:
	spin_unlock_irq(&current->sighand->siglock);
	if (flags & SECCOMP_FILTER_FLAG_TSYNC)
		mutex_unlock(&current->signal->cred_guard_mutex);
out_put_fd:
	if (flags & SECCOMP_FILTER_FLAG_NEW_LISTENER) {
		if (ret) {
			listener_f->private_data = NULL;
			fput(listener_f);
			put_unused_fd(listener);
		} else {
			fd_install(listener, listener_f);
			ret = listener;
		}
	}
out_free:
	seccomp_filter_free(prepared);
	return ret;
}
#else
static inline long seccomp_set_mode_filter(unsigned int flags,
					   const char __user *filter)
{
	return -EINVAL;
}
#endif

static long seccomp_get_action_avail(const char __user *uaction)
{
	u32 action;

	if (copy_from_user(&action, uaction, sizeof(action)))
		return -EFAULT;

	switch (action) {
	case SECCOMP_RET_KILL_PROCESS:
	case SECCOMP_RET_KILL_THREAD:
	case SECCOMP_RET_TRAP:
	case SECCOMP_RET_ERRNO:
	case SECCOMP_RET_USER_NOTIF:
	case SECCOMP_RET_TRACE:
	case SECCOMP_RET_LOG:
	case SECCOMP_RET_ALLOW:
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static long seccomp_get_notif_sizes(void __user *usizes)
{
	struct seccomp_notif_sizes sizes = {
		.seccomp_notif = sizeof(struct seccomp_notif),
		.seccomp_notif_resp = sizeof(struct seccomp_notif_resp),
		.seccomp_data = sizeof(struct seccomp_data),
	};

	if (copy_to_user(usizes, &sizes, sizeof(sizes)))
		return -EFAULT;

	return 0;
}

/* Common entry point for both prctl and syscall. */
static long do_seccomp(unsigned int op, unsigned int flags,
		       void __user *uargs)
{
	switch (op) {
	case SECCOMP_SET_MODE_STRICT:
		if (flags != 0 || uargs != NULL)
			return -EINVAL;
		return seccomp_set_mode_strict();
	case SECCOMP_SET_MODE_FILTER:
		return seccomp_set_mode_filter(flags, uargs);
	case SECCOMP_GET_ACTION_AVAIL:
		if (flags != 0)
			return -EINVAL;

		return seccomp_get_action_avail(uargs);
	case SECCOMP_GET_NOTIF_SIZES:
		if (flags != 0)
			return -EINVAL;

		return seccomp_get_notif_sizes(uargs);
	default:
		return -EINVAL;
	}
}

SYSCALL_DEFINE3(seccomp, unsigned int, op, unsigned int, flags,
			 void __user *, uargs)
{
	return do_seccomp(op, flags, uargs);
}

/**
 * prctl_set_seccomp: configures current->seccomp.mode
 * @seccomp_mode: requested mode to use
 * @filter: optional struct sock_fprog for use with SECCOMP_MODE_FILTER
 *
 * Returns 0 on success or -EINVAL on failure.
 */
long prctl_set_seccomp(unsigned long seccomp_mode, void __user *filter)
{
	unsigned int op;
	void __user *uargs;

	switch (seccomp_mode) {
	case SECCOMP_MODE_STRICT:
		op = SECCOMP_SET_MODE_STRICT;
		/*
		 * Setting strict mode through prctl always ignored filter,
		 * so make sure it is always NULL here to pass the internal
		 * check in do_seccomp().
		 */
		uargs = NULL;
		break;
	case SECCOMP_MODE_FILTER:
		op = SECCOMP_SET_MODE_FILTER;
		uargs = filter;
		break;
	default:
		return -EINVAL;
	}

	/* prctl interface doesn't have flags, so they are always zero. */
	return do_seccomp(op, 0, uargs);
}

#if defined(CONFIG_SECCOMP_FILTER) && defined(CONFIG_CHECKPOINT_RESTORE)
static struct seccomp_filter *get_nth_filter(struct task_struct *task,
					     unsigned long filter_off)
{
	struct seccomp_filter *orig, *filter;
	unsigned long count;

	/*
	 * Note: this is only correct because the caller should be the (ptrace)
	 * tracer of the task, otherwise lock_task_sighand is needed.
	 */
	spin_lock_irq(&task->sighand->siglock);

	if (task->seccomp.mode != SECCOMP_MODE_FILTER) {
		spin_unlock_irq(&task->sighand->siglock);
		return ERR_PTR(-EINVAL);
	}

	orig = task->seccomp.filter;
	__get_seccomp_filter(orig);
	spin_unlock_irq(&task->sighand->siglock);

	count = 0;
	for (filter = orig; filter; filter = filter->prev)
		count++;

	if (filter_off >= count) {
		filter = ERR_PTR(-ENOENT);
		goto out;
	}

	count -= filter_off;
	for (filter = orig; filter && count > 1; filter = filter->prev)
		count--;

	if (WARN_ON(count != 1 || !filter)) {
		filter = ERR_PTR(-ENOENT);
		goto out;
	}

	__get_seccomp_filter(filter);

out:
	__put_seccomp_filter(orig);
	return filter;
}

long seccomp_get_filter(struct task_struct *task, unsigned long filter_off,
			void __user *data)
{
	struct seccomp_filter *filter;
	struct sock_fprog_kern *fprog;
	long ret;

	if (!capable(CAP_SYS_ADMIN) ||
	    current->seccomp.mode != SECCOMP_MODE_DISABLED) {
		return -EACCES;
	}

	filter = get_nth_filter(task, filter_off);
	if (IS_ERR(filter))
		return PTR_ERR(filter);

	fprog = filter->prog->orig_prog;
	if (!fprog) {
		/* This must be a new non-cBPF filter, since we save
		 * every cBPF filter's orig_prog above when
		 * CONFIG_CHECKPOINT_RESTORE is enabled.
		 */
		ret = -EMEDIUMTYPE;
		goto out;
	}

	ret = fprog->len;
	if (!data)
		goto out;

	if (copy_to_user(data, fprog->filter, bpf_classic_proglen(fprog)))
		ret = -EFAULT;

out:
	__put_seccomp_filter(filter);
	return ret;
}

long seccomp_get_metadata(struct task_struct *task,
			  unsigned long size, void __user *data)
{
	long ret;
	struct seccomp_filter *filter;
	struct seccomp_metadata kmd = {};

	if (!capable(CAP_SYS_ADMIN) ||
	    current->seccomp.mode != SECCOMP_MODE_DISABLED) {
		return -EACCES;
	}

	size = min_t(unsigned long, size, sizeof(kmd));

	if (size < sizeof(kmd.filter_off))
		return -EINVAL;

	if (copy_from_user(&kmd.filter_off, data, sizeof(kmd.filter_off)))
		return -EFAULT;

	filter = get_nth_filter(task, kmd.filter_off);
	if (IS_ERR(filter))
		return PTR_ERR(filter);

	if (filter->log)
		kmd.flags |= SECCOMP_FILTER_FLAG_LOG;

	ret = size;
	if (copy_to_user(data, &kmd, size))
		ret = -EFAULT;

	__put_seccomp_filter(filter);
	return ret;
}
#endif

#ifdef CONFIG_SYSCTL

/* Human readable action names for friendly sysctl interaction */
#define SECCOMP_RET_KILL_PROCESS_NAME	"kill_process"
#define SECCOMP_RET_KILL_THREAD_NAME	"kill_thread"
#define SECCOMP_RET_TRAP_NAME		"trap"
#define SECCOMP_RET_ERRNO_NAME		"errno"
#define SECCOMP_RET_USER_NOTIF_NAME	"user_notif"
#define SECCOMP_RET_TRACE_NAME		"trace"
#define SECCOMP_RET_LOG_NAME		"log"
#define SECCOMP_RET_ALLOW_NAME		"allow"

static const char seccomp_actions_avail[] =
				SECCOMP_RET_KILL_PROCESS_NAME	" "
				SECCOMP_RET_KILL_THREAD_NAME	" "
				SECCOMP_RET_TRAP_NAME		" "
				SECCOMP_RET_ERRNO_NAME		" "
				SECCOMP_RET_USER_NOTIF_NAME     " "
				SECCOMP_RET_TRACE_NAME		" "
				SECCOMP_RET_LOG_NAME		" "
				SECCOMP_RET_ALLOW_NAME;

struct seccomp_log_name {
	u32		log;
	const char	*name;
};

static const struct seccomp_log_name seccomp_log_names[] = {
	{ SECCOMP_LOG_KILL_PROCESS, SECCOMP_RET_KILL_PROCESS_NAME },
	{ SECCOMP_LOG_KILL_THREAD, SECCOMP_RET_KILL_THREAD_NAME },
	{ SECCOMP_LOG_TRAP, SECCOMP_RET_TRAP_NAME },
	{ SECCOMP_LOG_ERRNO, SECCOMP_RET_ERRNO_NAME },
	{ SECCOMP_LOG_USER_NOTIF, SECCOMP_RET_USER_NOTIF_NAME },
	{ SECCOMP_LOG_TRACE, SECCOMP_RET_TRACE_NAME },
	{ SECCOMP_LOG_LOG, SECCOMP_RET_LOG_NAME },
	{ SECCOMP_LOG_ALLOW, SECCOMP_RET_ALLOW_NAME },
	{ }
};

static bool seccomp_names_from_actions_logged(char *names, size_t size,
					      u32 actions_logged,
					      const char *sep)
{
	const struct seccomp_log_name *cur;
	bool append_sep = false;

	for (cur = seccomp_log_names; cur->name && size; cur++) {
		ssize_t ret;

		if (!(actions_logged & cur->log))
			continue;

		if (append_sep) {
			ret = strscpy(names, sep, size);
			if (ret < 0)
				return false;

			names += ret;
			size -= ret;
		} else
			append_sep = true;

		ret = strscpy(names, cur->name, size);
		if (ret < 0)
			return false;

		names += ret;
		size -= ret;
	}

	return true;
}

static bool seccomp_action_logged_from_name(u32 *action_logged,
					    const char *name)
{
	const struct seccomp_log_name *cur;

	for (cur = seccomp_log_names; cur->name; cur++) {
		if (!strcmp(cur->name, name)) {
			*action_logged = cur->log;
			return true;
		}
	}

	return false;
}

static bool seccomp_actions_logged_from_names(u32 *actions_logged, char *names)
{
	char *name;

	*actions_logged = 0;
	while ((name = strsep(&names, " ")) && *name) {
		u32 action_logged = 0;

		if (!seccomp_action_logged_from_name(&action_logged, name))
			return false;

		*actions_logged |= action_logged;
	}

	return true;
}

static int read_actions_logged(struct ctl_table *ro_table, void __user *buffer,
			       size_t *lenp, loff_t *ppos)
{
	char names[sizeof(seccomp_actions_avail)];
	struct ctl_table table;

	memset(names, 0, sizeof(names));

	if (!seccomp_names_from_actions_logged(names, sizeof(names),
					       seccomp_actions_logged, " "))
		return -EINVAL;

	table = *ro_table;
	table.data = names;
	table.maxlen = sizeof(names);
	return proc_dostring(&table, 0, buffer, lenp, ppos);
}

static int write_actions_logged(struct ctl_table *ro_table, void __user *buffer,
				size_t *lenp, loff_t *ppos, u32 *actions_logged)
{
	char names[sizeof(seccomp_actions_avail)];
	struct ctl_table table;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	memset(names, 0, sizeof(names));

	table = *ro_table;
	table.data = names;
	table.maxlen = sizeof(names);
	ret = proc_dostring(&table, 1, buffer, lenp, ppos);
	if (ret)
		return ret;

	if (!seccomp_actions_logged_from_names(actions_logged, table.data))
		return -EINVAL;

	if (*actions_logged & SECCOMP_LOG_ALLOW)
		return -EINVAL;

	seccomp_actions_logged = *actions_logged;
	return 0;
}

static void audit_actions_logged(u32 actions_logged, u32 old_actions_logged,
				 int ret)
{
	char names[sizeof(seccomp_actions_avail)];
	char old_names[sizeof(seccomp_actions_avail)];
	const char *new = names;
	const char *old = old_names;

	if (!audit_enabled)
		return;

	memset(names, 0, sizeof(names));
	memset(old_names, 0, sizeof(old_names));

	if (ret)
		new = "?";
	else if (!actions_logged)
		new = "(none)";
	else if (!seccomp_names_from_actions_logged(names, sizeof(names),
						    actions_logged, ","))
		new = "?";

	if (!old_actions_logged)
		old = "(none)";
	else if (!seccomp_names_from_actions_logged(old_names,
						    sizeof(old_names),
						    old_actions_logged, ","))
		old = "?";

	return audit_seccomp_actions_logged(new, old, !ret);
}

static int seccomp_actions_logged_handler(struct ctl_table *ro_table, int write,
					  void __user *buffer, size_t *lenp,
					  loff_t *ppos)
{
	int ret;

	if (write) {
		u32 actions_logged = 0;
		u32 old_actions_logged = seccomp_actions_logged;

		ret = write_actions_logged(ro_table, buffer, lenp, ppos,
					   &actions_logged);
		audit_actions_logged(actions_logged, old_actions_logged, ret);
	} else
		ret = read_actions_logged(ro_table, buffer, lenp, ppos);

	return ret;
}

static struct ctl_path seccomp_sysctl_path[] = {
	{ .procname = "kernel", },
	{ .procname = "seccomp", },
	{ }
};

static struct ctl_table seccomp_sysctl_table[] = {
	{
		.procname	= "actions_avail",
		.data		= (void *) &seccomp_actions_avail,
		.maxlen		= sizeof(seccomp_actions_avail),
		.mode		= 0444,
		.proc_handler	= proc_dostring,
	},
	{
		.procname	= "actions_logged",
		.mode		= 0644,
		.proc_handler	= seccomp_actions_logged_handler,
	},
	{ }
};

static int __init seccomp_sysctl_init(void)
{
	struct ctl_table_header *hdr;

	hdr = register_sysctl_paths(seccomp_sysctl_path, seccomp_sysctl_table);
	if (!hdr)
		pr_warn("seccomp: sysctl registration failed\n");
	else
		kmemleak_not_leak(hdr);

	return 0;
}

device_initcall(seccomp_sysctl_init)

#endif /* CONFIG_SYSCTL */
