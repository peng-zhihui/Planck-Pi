// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2011-2015 PLUMgrid, http://plumgrid.com
 * Copyright (c) 2016 Facebook
 */
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/bpf.h>
#include <linux/bpf_perf_event.h>
#include <linux/filter.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>
#include <linux/kprobes.h>
#include <linux/syscalls.h>
#include <linux/error-injection.h>

#include <asm/tlb.h>

#include "trace_probe.h"
#include "trace.h"

#define bpf_event_rcu_dereference(p)					\
	rcu_dereference_protected(p, lockdep_is_held(&bpf_event_mutex))

#ifdef CONFIG_MODULES
struct bpf_trace_module {
	struct module *module;
	struct list_head list;
};

static LIST_HEAD(bpf_trace_modules);
static DEFINE_MUTEX(bpf_module_mutex);

static struct bpf_raw_event_map *bpf_get_raw_tracepoint_module(const char *name)
{
	struct bpf_raw_event_map *btp, *ret = NULL;
	struct bpf_trace_module *btm;
	unsigned int i;

	mutex_lock(&bpf_module_mutex);
	list_for_each_entry(btm, &bpf_trace_modules, list) {
		for (i = 0; i < btm->module->num_bpf_raw_events; ++i) {
			btp = &btm->module->bpf_raw_events[i];
			if (!strcmp(btp->tp->name, name)) {
				if (try_module_get(btm->module))
					ret = btp;
				goto out;
			}
		}
	}
out:
	mutex_unlock(&bpf_module_mutex);
	return ret;
}
#else
static struct bpf_raw_event_map *bpf_get_raw_tracepoint_module(const char *name)
{
	return NULL;
}
#endif /* CONFIG_MODULES */

u64 bpf_get_stackid(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5);
u64 bpf_get_stack(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5);

/**
 * trace_call_bpf - invoke BPF program
 * @call: tracepoint event
 * @ctx: opaque context pointer
 *
 * kprobe handlers execute BPF programs via this helper.
 * Can be used from static tracepoints in the future.
 *
 * Return: BPF programs always return an integer which is interpreted by
 * kprobe handler as:
 * 0 - return from kprobe (event is filtered out)
 * 1 - store kprobe event into ring buffer
 * Other values are reserved and currently alias to 1
 */
unsigned int trace_call_bpf(struct trace_event_call *call, void *ctx)
{
	unsigned int ret;

	if (in_nmi()) /* not supported yet */
		return 1;

	preempt_disable();

	if (unlikely(__this_cpu_inc_return(bpf_prog_active) != 1)) {
		/*
		 * since some bpf program is already running on this cpu,
		 * don't call into another bpf program (same or different)
		 * and don't send kprobe event into ring-buffer,
		 * so return zero here
		 */
		ret = 0;
		goto out;
	}

	/*
	 * Instead of moving rcu_read_lock/rcu_dereference/rcu_read_unlock
	 * to all call sites, we did a bpf_prog_array_valid() there to check
	 * whether call->prog_array is empty or not, which is
	 * a heurisitc to speed up execution.
	 *
	 * If bpf_prog_array_valid() fetched prog_array was
	 * non-NULL, we go into trace_call_bpf() and do the actual
	 * proper rcu_dereference() under RCU lock.
	 * If it turns out that prog_array is NULL then, we bail out.
	 * For the opposite, if the bpf_prog_array_valid() fetched pointer
	 * was NULL, you'll skip the prog_array with the risk of missing
	 * out of events when it was updated in between this and the
	 * rcu_dereference() which is accepted risk.
	 */
	ret = BPF_PROG_RUN_ARRAY_CHECK(call->prog_array, ctx, BPF_PROG_RUN);

 out:
	__this_cpu_dec(bpf_prog_active);
	preempt_enable();

	return ret;
}
EXPORT_SYMBOL_GPL(trace_call_bpf);

#ifdef CONFIG_BPF_KPROBE_OVERRIDE
BPF_CALL_2(bpf_override_return, struct pt_regs *, regs, unsigned long, rc)
{
	regs_set_return_value(regs, rc);
	override_function_with_return(regs);
	return 0;
}

static const struct bpf_func_proto bpf_override_return_proto = {
	.func		= bpf_override_return,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_CTX,
	.arg2_type	= ARG_ANYTHING,
};
#endif

BPF_CALL_3(bpf_probe_read, void *, dst, u32, size, const void *, unsafe_ptr)
{
	int ret;

	ret = security_locked_down(LOCKDOWN_BPF_READ);
	if (ret < 0)
		goto out;

	ret = probe_kernel_read(dst, unsafe_ptr, size);
	if (unlikely(ret < 0))
out:
		memset(dst, 0, size);

	return ret;
}

static const struct bpf_func_proto bpf_probe_read_proto = {
	.func		= bpf_probe_read,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_UNINIT_MEM,
	.arg2_type	= ARG_CONST_SIZE_OR_ZERO,
	.arg3_type	= ARG_ANYTHING,
};

BPF_CALL_3(bpf_probe_write_user, void __user *, unsafe_ptr, const void *, src,
	   u32, size)
{
	/*
	 * Ensure we're in user context which is safe for the helper to
	 * run. This helper has no business in a kthread.
	 *
	 * access_ok() should prevent writing to non-user memory, but in
	 * some situations (nommu, temporary switch, etc) access_ok() does
	 * not provide enough validation, hence the check on KERNEL_DS.
	 *
	 * nmi_uaccess_okay() ensures the probe is not run in an interim
	 * state, when the task or mm are switched. This is specifically
	 * required to prevent the use of temporary mm.
	 */

	if (unlikely(in_interrupt() ||
		     current->flags & (PF_KTHREAD | PF_EXITING)))
		return -EPERM;
	if (unlikely(uaccess_kernel()))
		return -EPERM;
	if (unlikely(!nmi_uaccess_okay()))
		return -EPERM;

	return probe_user_write(unsafe_ptr, src, size);
}

static const struct bpf_func_proto bpf_probe_write_user_proto = {
	.func		= bpf_probe_write_user,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_ANYTHING,
	.arg2_type	= ARG_PTR_TO_MEM,
	.arg3_type	= ARG_CONST_SIZE,
};

static const struct bpf_func_proto *bpf_get_probe_write_proto(void)
{
	pr_warn_ratelimited("%s[%d] is installing a program with bpf_probe_write_user helper that may corrupt user memory!",
			    current->comm, task_pid_nr(current));

	return &bpf_probe_write_user_proto;
}

/*
 * Only limited trace_printk() conversion specifiers allowed:
 * %d %i %u %x %ld %li %lu %lx %lld %lli %llu %llx %p %s
 */
BPF_CALL_5(bpf_trace_printk, char *, fmt, u32, fmt_size, u64, arg1,
	   u64, arg2, u64, arg3)
{
	bool str_seen = false;
	int mod[3] = {};
	int fmt_cnt = 0;
	u64 unsafe_addr;
	char buf[64];
	int i;

	/*
	 * bpf_check()->check_func_arg()->check_stack_boundary()
	 * guarantees that fmt points to bpf program stack,
	 * fmt_size bytes of it were initialized and fmt_size > 0
	 */
	if (fmt[--fmt_size] != 0)
		return -EINVAL;

	/* check format string for allowed specifiers */
	for (i = 0; i < fmt_size; i++) {
		if ((!isprint(fmt[i]) && !isspace(fmt[i])) || !isascii(fmt[i]))
			return -EINVAL;

		if (fmt[i] != '%')
			continue;

		if (fmt_cnt >= 3)
			return -EINVAL;

		/* fmt[i] != 0 && fmt[last] == 0, so we can access fmt[i + 1] */
		i++;
		if (fmt[i] == 'l') {
			mod[fmt_cnt]++;
			i++;
		} else if (fmt[i] == 'p' || fmt[i] == 's') {
			mod[fmt_cnt]++;
			/* disallow any further format extensions */
			if (fmt[i + 1] != 0 &&
			    !isspace(fmt[i + 1]) &&
			    !ispunct(fmt[i + 1]))
				return -EINVAL;
			fmt_cnt++;
			if (fmt[i] == 's') {
				if (str_seen)
					/* allow only one '%s' per fmt string */
					return -EINVAL;
				str_seen = true;

				switch (fmt_cnt) {
				case 1:
					unsafe_addr = arg1;
					arg1 = (long) buf;
					break;
				case 2:
					unsafe_addr = arg2;
					arg2 = (long) buf;
					break;
				case 3:
					unsafe_addr = arg3;
					arg3 = (long) buf;
					break;
				}
				buf[0] = 0;
				strncpy_from_unsafe(buf,
						    (void *) (long) unsafe_addr,
						    sizeof(buf));
			}
			continue;
		}

		if (fmt[i] == 'l') {
			mod[fmt_cnt]++;
			i++;
		}

		if (fmt[i] != 'i' && fmt[i] != 'd' &&
		    fmt[i] != 'u' && fmt[i] != 'x')
			return -EINVAL;
		fmt_cnt++;
	}

/* Horrid workaround for getting va_list handling working with different
 * argument type combinations generically for 32 and 64 bit archs.
 */
#define __BPF_TP_EMIT()	__BPF_ARG3_TP()
#define __BPF_TP(...)							\
	__trace_printk(0 /* Fake ip */,					\
		       fmt, ##__VA_ARGS__)

#define __BPF_ARG1_TP(...)						\
	((mod[0] == 2 || (mod[0] == 1 && __BITS_PER_LONG == 64))	\
	  ? __BPF_TP(arg1, ##__VA_ARGS__)				\
	  : ((mod[0] == 1 || (mod[0] == 0 && __BITS_PER_LONG == 32))	\
	      ? __BPF_TP((long)arg1, ##__VA_ARGS__)			\
	      : __BPF_TP((u32)arg1, ##__VA_ARGS__)))

#define __BPF_ARG2_TP(...)						\
	((mod[1] == 2 || (mod[1] == 1 && __BITS_PER_LONG == 64))	\
	  ? __BPF_ARG1_TP(arg2, ##__VA_ARGS__)				\
	  : ((mod[1] == 1 || (mod[1] == 0 && __BITS_PER_LONG == 32))	\
	      ? __BPF_ARG1_TP((long)arg2, ##__VA_ARGS__)		\
	      : __BPF_ARG1_TP((u32)arg2, ##__VA_ARGS__)))

#define __BPF_ARG3_TP(...)						\
	((mod[2] == 2 || (mod[2] == 1 && __BITS_PER_LONG == 64))	\
	  ? __BPF_ARG2_TP(arg3, ##__VA_ARGS__)				\
	  : ((mod[2] == 1 || (mod[2] == 0 && __BITS_PER_LONG == 32))	\
	      ? __BPF_ARG2_TP((long)arg3, ##__VA_ARGS__)		\
	      : __BPF_ARG2_TP((u32)arg3, ##__VA_ARGS__)))

	return __BPF_TP_EMIT();
}

static const struct bpf_func_proto bpf_trace_printk_proto = {
	.func		= bpf_trace_printk,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_MEM,
	.arg2_type	= ARG_CONST_SIZE,
};

const struct bpf_func_proto *bpf_get_trace_printk_proto(void)
{
	/*
	 * this program might be calling bpf_trace_printk,
	 * so allocate per-cpu printk buffers
	 */
	trace_printk_init_buffers();

	return &bpf_trace_printk_proto;
}

static __always_inline int
get_map_perf_counter(struct bpf_map *map, u64 flags,
		     u64 *value, u64 *enabled, u64 *running)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	unsigned int cpu = smp_processor_id();
	u64 index = flags & BPF_F_INDEX_MASK;
	struct bpf_event_entry *ee;

	if (unlikely(flags & ~(BPF_F_INDEX_MASK)))
		return -EINVAL;
	if (index == BPF_F_CURRENT_CPU)
		index = cpu;
	if (unlikely(index >= array->map.max_entries))
		return -E2BIG;

	ee = READ_ONCE(array->ptrs[index]);
	if (!ee)
		return -ENOENT;

	return perf_event_read_local(ee->event, value, enabled, running);
}

BPF_CALL_2(bpf_perf_event_read, struct bpf_map *, map, u64, flags)
{
	u64 value = 0;
	int err;

	err = get_map_perf_counter(map, flags, &value, NULL, NULL);
	/*
	 * this api is ugly since we miss [-22..-2] range of valid
	 * counter values, but that's uapi
	 */
	if (err)
		return err;
	return value;
}

static const struct bpf_func_proto bpf_perf_event_read_proto = {
	.func		= bpf_perf_event_read,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_CONST_MAP_PTR,
	.arg2_type	= ARG_ANYTHING,
};

BPF_CALL_4(bpf_perf_event_read_value, struct bpf_map *, map, u64, flags,
	   struct bpf_perf_event_value *, buf, u32, size)
{
	int err = -EINVAL;

	if (unlikely(size != sizeof(struct bpf_perf_event_value)))
		goto clear;
	err = get_map_perf_counter(map, flags, &buf->counter, &buf->enabled,
				   &buf->running);
	if (unlikely(err))
		goto clear;
	return 0;
clear:
	memset(buf, 0, size);
	return err;
}

static const struct bpf_func_proto bpf_perf_event_read_value_proto = {
	.func		= bpf_perf_event_read_value,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_CONST_MAP_PTR,
	.arg2_type	= ARG_ANYTHING,
	.arg3_type	= ARG_PTR_TO_UNINIT_MEM,
	.arg4_type	= ARG_CONST_SIZE,
};

static __always_inline u64
__bpf_perf_event_output(struct pt_regs *regs, struct bpf_map *map,
			u64 flags, struct perf_sample_data *sd)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	unsigned int cpu = smp_processor_id();
	u64 index = flags & BPF_F_INDEX_MASK;
	struct bpf_event_entry *ee;
	struct perf_event *event;

	if (index == BPF_F_CURRENT_CPU)
		index = cpu;
	if (unlikely(index >= array->map.max_entries))
		return -E2BIG;

	ee = READ_ONCE(array->ptrs[index]);
	if (!ee)
		return -ENOENT;

	event = ee->event;
	if (unlikely(event->attr.type != PERF_TYPE_SOFTWARE ||
		     event->attr.config != PERF_COUNT_SW_BPF_OUTPUT))
		return -EINVAL;

	if (unlikely(event->oncpu != cpu))
		return -EOPNOTSUPP;

	return perf_event_output(event, sd, regs);
}

/*
 * Support executing tracepoints in normal, irq, and nmi context that each call
 * bpf_perf_event_output
 */
struct bpf_trace_sample_data {
	struct perf_sample_data sds[3];
};

static DEFINE_PER_CPU(struct bpf_trace_sample_data, bpf_trace_sds);
static DEFINE_PER_CPU(int, bpf_trace_nest_level);
BPF_CALL_5(bpf_perf_event_output, struct pt_regs *, regs, struct bpf_map *, map,
	   u64, flags, void *, data, u64, size)
{
	struct bpf_trace_sample_data *sds = this_cpu_ptr(&bpf_trace_sds);
	int nest_level = this_cpu_inc_return(bpf_trace_nest_level);
	struct perf_raw_record raw = {
		.frag = {
			.size = size,
			.data = data,
		},
	};
	struct perf_sample_data *sd;
	int err;

	if (WARN_ON_ONCE(nest_level > ARRAY_SIZE(sds->sds))) {
		err = -EBUSY;
		goto out;
	}

	sd = &sds->sds[nest_level - 1];

	if (unlikely(flags & ~(BPF_F_INDEX_MASK))) {
		err = -EINVAL;
		goto out;
	}

	perf_sample_data_init(sd, 0, 0);
	sd->raw = &raw;

	err = __bpf_perf_event_output(regs, map, flags, sd);

out:
	this_cpu_dec(bpf_trace_nest_level);
	return err;
}

static const struct bpf_func_proto bpf_perf_event_output_proto = {
	.func		= bpf_perf_event_output,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_CTX,
	.arg2_type	= ARG_CONST_MAP_PTR,
	.arg3_type	= ARG_ANYTHING,
	.arg4_type	= ARG_PTR_TO_MEM,
	.arg5_type	= ARG_CONST_SIZE_OR_ZERO,
};

static DEFINE_PER_CPU(int, bpf_event_output_nest_level);
struct bpf_nested_pt_regs {
	struct pt_regs regs[3];
};
static DEFINE_PER_CPU(struct bpf_nested_pt_regs, bpf_pt_regs);
static DEFINE_PER_CPU(struct bpf_trace_sample_data, bpf_misc_sds);

u64 bpf_event_output(struct bpf_map *map, u64 flags, void *meta, u64 meta_size,
		     void *ctx, u64 ctx_size, bpf_ctx_copy_t ctx_copy)
{
	int nest_level = this_cpu_inc_return(bpf_event_output_nest_level);
	struct perf_raw_frag frag = {
		.copy		= ctx_copy,
		.size		= ctx_size,
		.data		= ctx,
	};
	struct perf_raw_record raw = {
		.frag = {
			{
				.next	= ctx_size ? &frag : NULL,
			},
			.size	= meta_size,
			.data	= meta,
		},
	};
	struct perf_sample_data *sd;
	struct pt_regs *regs;
	u64 ret;

	if (WARN_ON_ONCE(nest_level > ARRAY_SIZE(bpf_misc_sds.sds))) {
		ret = -EBUSY;
		goto out;
	}
	sd = this_cpu_ptr(&bpf_misc_sds.sds[nest_level - 1]);
	regs = this_cpu_ptr(&bpf_pt_regs.regs[nest_level - 1]);

	perf_fetch_caller_regs(regs);
	perf_sample_data_init(sd, 0, 0);
	sd->raw = &raw;

	ret = __bpf_perf_event_output(regs, map, flags, sd);
out:
	this_cpu_dec(bpf_event_output_nest_level);
	return ret;
}

BPF_CALL_0(bpf_get_current_task)
{
	return (long) current;
}

static const struct bpf_func_proto bpf_get_current_task_proto = {
	.func		= bpf_get_current_task,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
};

BPF_CALL_2(bpf_current_task_under_cgroup, struct bpf_map *, map, u32, idx)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	struct cgroup *cgrp;

	if (unlikely(idx >= array->map.max_entries))
		return -E2BIG;

	cgrp = READ_ONCE(array->ptrs[idx]);
	if (unlikely(!cgrp))
		return -EAGAIN;

	return task_under_cgroup_hierarchy(current, cgrp);
}

static const struct bpf_func_proto bpf_current_task_under_cgroup_proto = {
	.func           = bpf_current_task_under_cgroup,
	.gpl_only       = false,
	.ret_type       = RET_INTEGER,
	.arg1_type      = ARG_CONST_MAP_PTR,
	.arg2_type      = ARG_ANYTHING,
};

BPF_CALL_3(bpf_probe_read_str, void *, dst, u32, size,
	   const void *, unsafe_ptr)
{
	int ret;

	ret = security_locked_down(LOCKDOWN_BPF_READ);
	if (ret < 0)
		goto out;

	/*
	 * The strncpy_from_unsafe() call will likely not fill the entire
	 * buffer, but that's okay in this circumstance as we're probing
	 * arbitrary memory anyway similar to bpf_probe_read() and might
	 * as well probe the stack. Thus, memory is explicitly cleared
	 * only in error case, so that improper users ignoring return
	 * code altogether don't copy garbage; otherwise length of string
	 * is returned that can be used for bpf_perf_event_output() et al.
	 */
	ret = strncpy_from_unsafe(dst, unsafe_ptr, size);
	if (unlikely(ret < 0))
out:
		memset(dst, 0, size);

	return ret;
}

static const struct bpf_func_proto bpf_probe_read_str_proto = {
	.func		= bpf_probe_read_str,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_UNINIT_MEM,
	.arg2_type	= ARG_CONST_SIZE_OR_ZERO,
	.arg3_type	= ARG_ANYTHING,
};

struct send_signal_irq_work {
	struct irq_work irq_work;
	struct task_struct *task;
	u32 sig;
};

static DEFINE_PER_CPU(struct send_signal_irq_work, send_signal_work);

static void do_bpf_send_signal(struct irq_work *entry)
{
	struct send_signal_irq_work *work;

	work = container_of(entry, struct send_signal_irq_work, irq_work);
	group_send_sig_info(work->sig, SEND_SIG_PRIV, work->task, PIDTYPE_TGID);
}

BPF_CALL_1(bpf_send_signal, u32, sig)
{
	struct send_signal_irq_work *work = NULL;

	/* Similar to bpf_probe_write_user, task needs to be
	 * in a sound condition and kernel memory access be
	 * permitted in order to send signal to the current
	 * task.
	 */
	if (unlikely(current->flags & (PF_KTHREAD | PF_EXITING)))
		return -EPERM;
	if (unlikely(uaccess_kernel()))
		return -EPERM;
	if (unlikely(!nmi_uaccess_okay()))
		return -EPERM;

	if (irqs_disabled()) {
		/* Do an early check on signal validity. Otherwise,
		 * the error is lost in deferred irq_work.
		 */
		if (unlikely(!valid_signal(sig)))
			return -EINVAL;

		work = this_cpu_ptr(&send_signal_work);
		if (work->irq_work.flags & IRQ_WORK_BUSY)
			return -EBUSY;

		/* Add the current task, which is the target of sending signal,
		 * to the irq_work. The current task may change when queued
		 * irq works get executed.
		 */
		work->task = current;
		work->sig = sig;
		irq_work_queue(&work->irq_work);
		return 0;
	}

	return group_send_sig_info(sig, SEND_SIG_PRIV, current, PIDTYPE_TGID);
}

static const struct bpf_func_proto bpf_send_signal_proto = {
	.func		= bpf_send_signal,
	.gpl_only	= false,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_ANYTHING,
};

static const struct bpf_func_proto *
tracing_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
	switch (func_id) {
	case BPF_FUNC_map_lookup_elem:
		return &bpf_map_lookup_elem_proto;
	case BPF_FUNC_map_update_elem:
		return &bpf_map_update_elem_proto;
	case BPF_FUNC_map_delete_elem:
		return &bpf_map_delete_elem_proto;
	case BPF_FUNC_map_push_elem:
		return &bpf_map_push_elem_proto;
	case BPF_FUNC_map_pop_elem:
		return &bpf_map_pop_elem_proto;
	case BPF_FUNC_map_peek_elem:
		return &bpf_map_peek_elem_proto;
	case BPF_FUNC_probe_read:
		return &bpf_probe_read_proto;
	case BPF_FUNC_ktime_get_ns:
		return &bpf_ktime_get_ns_proto;
	case BPF_FUNC_tail_call:
		return &bpf_tail_call_proto;
	case BPF_FUNC_get_current_pid_tgid:
		return &bpf_get_current_pid_tgid_proto;
	case BPF_FUNC_get_current_task:
		return &bpf_get_current_task_proto;
	case BPF_FUNC_get_current_uid_gid:
		return &bpf_get_current_uid_gid_proto;
	case BPF_FUNC_get_current_comm:
		return &bpf_get_current_comm_proto;
	case BPF_FUNC_trace_printk:
		return bpf_get_trace_printk_proto();
	case BPF_FUNC_get_smp_processor_id:
		return &bpf_get_smp_processor_id_proto;
	case BPF_FUNC_get_numa_node_id:
		return &bpf_get_numa_node_id_proto;
	case BPF_FUNC_perf_event_read:
		return &bpf_perf_event_read_proto;
	case BPF_FUNC_probe_write_user:
		return bpf_get_probe_write_proto();
	case BPF_FUNC_current_task_under_cgroup:
		return &bpf_current_task_under_cgroup_proto;
	case BPF_FUNC_get_prandom_u32:
		return &bpf_get_prandom_u32_proto;
	case BPF_FUNC_probe_read_str:
		return &bpf_probe_read_str_proto;
#ifdef CONFIG_CGROUPS
	case BPF_FUNC_get_current_cgroup_id:
		return &bpf_get_current_cgroup_id_proto;
#endif
	case BPF_FUNC_send_signal:
		return &bpf_send_signal_proto;
	default:
		return NULL;
	}
}

static const struct bpf_func_proto *
kprobe_prog_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
	switch (func_id) {
	case BPF_FUNC_perf_event_output:
		return &bpf_perf_event_output_proto;
	case BPF_FUNC_get_stackid:
		return &bpf_get_stackid_proto;
	case BPF_FUNC_get_stack:
		return &bpf_get_stack_proto;
	case BPF_FUNC_perf_event_read_value:
		return &bpf_perf_event_read_value_proto;
#ifdef CONFIG_BPF_KPROBE_OVERRIDE
	case BPF_FUNC_override_return:
		return &bpf_override_return_proto;
#endif
	default:
		return tracing_func_proto(func_id, prog);
	}
}

/* bpf+kprobe programs can access fields of 'struct pt_regs' */
static bool kprobe_prog_is_valid_access(int off, int size, enum bpf_access_type type,
					const struct bpf_prog *prog,
					struct bpf_insn_access_aux *info)
{
	if (off < 0 || off >= sizeof(struct pt_regs))
		return false;
	if (type != BPF_READ)
		return false;
	if (off % size != 0)
		return false;
	/*
	 * Assertion for 32 bit to make sure last 8 byte access
	 * (BPF_DW) to the last 4 byte member is disallowed.
	 */
	if (off + size > sizeof(struct pt_regs))
		return false;

	return true;
}

const struct bpf_verifier_ops kprobe_verifier_ops = {
	.get_func_proto  = kprobe_prog_func_proto,
	.is_valid_access = kprobe_prog_is_valid_access,
};

const struct bpf_prog_ops kprobe_prog_ops = {
};

BPF_CALL_5(bpf_perf_event_output_tp, void *, tp_buff, struct bpf_map *, map,
	   u64, flags, void *, data, u64, size)
{
	struct pt_regs *regs = *(struct pt_regs **)tp_buff;

	/*
	 * r1 points to perf tracepoint buffer where first 8 bytes are hidden
	 * from bpf program and contain a pointer to 'struct pt_regs'. Fetch it
	 * from there and call the same bpf_perf_event_output() helper inline.
	 */
	return ____bpf_perf_event_output(regs, map, flags, data, size);
}

static const struct bpf_func_proto bpf_perf_event_output_proto_tp = {
	.func		= bpf_perf_event_output_tp,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_CTX,
	.arg2_type	= ARG_CONST_MAP_PTR,
	.arg3_type	= ARG_ANYTHING,
	.arg4_type	= ARG_PTR_TO_MEM,
	.arg5_type	= ARG_CONST_SIZE_OR_ZERO,
};

BPF_CALL_3(bpf_get_stackid_tp, void *, tp_buff, struct bpf_map *, map,
	   u64, flags)
{
	struct pt_regs *regs = *(struct pt_regs **)tp_buff;

	/*
	 * Same comment as in bpf_perf_event_output_tp(), only that this time
	 * the other helper's function body cannot be inlined due to being
	 * external, thus we need to call raw helper function.
	 */
	return bpf_get_stackid((unsigned long) regs, (unsigned long) map,
			       flags, 0, 0);
}

static const struct bpf_func_proto bpf_get_stackid_proto_tp = {
	.func		= bpf_get_stackid_tp,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_CTX,
	.arg2_type	= ARG_CONST_MAP_PTR,
	.arg3_type	= ARG_ANYTHING,
};

BPF_CALL_4(bpf_get_stack_tp, void *, tp_buff, void *, buf, u32, size,
	   u64, flags)
{
	struct pt_regs *regs = *(struct pt_regs **)tp_buff;

	return bpf_get_stack((unsigned long) regs, (unsigned long) buf,
			     (unsigned long) size, flags, 0);
}

static const struct bpf_func_proto bpf_get_stack_proto_tp = {
	.func		= bpf_get_stack_tp,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_CTX,
	.arg2_type	= ARG_PTR_TO_UNINIT_MEM,
	.arg3_type	= ARG_CONST_SIZE_OR_ZERO,
	.arg4_type	= ARG_ANYTHING,
};

static const struct bpf_func_proto *
tp_prog_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
	switch (func_id) {
	case BPF_FUNC_perf_event_output:
		return &bpf_perf_event_output_proto_tp;
	case BPF_FUNC_get_stackid:
		return &bpf_get_stackid_proto_tp;
	case BPF_FUNC_get_stack:
		return &bpf_get_stack_proto_tp;
	default:
		return tracing_func_proto(func_id, prog);
	}
}

static bool tp_prog_is_valid_access(int off, int size, enum bpf_access_type type,
				    const struct bpf_prog *prog,
				    struct bpf_insn_access_aux *info)
{
	if (off < sizeof(void *) || off >= PERF_MAX_TRACE_SIZE)
		return false;
	if (type != BPF_READ)
		return false;
	if (off % size != 0)
		return false;

	BUILD_BUG_ON(PERF_MAX_TRACE_SIZE % sizeof(__u64));
	return true;
}

const struct bpf_verifier_ops tracepoint_verifier_ops = {
	.get_func_proto  = tp_prog_func_proto,
	.is_valid_access = tp_prog_is_valid_access,
};

const struct bpf_prog_ops tracepoint_prog_ops = {
};

BPF_CALL_3(bpf_perf_prog_read_value, struct bpf_perf_event_data_kern *, ctx,
	   struct bpf_perf_event_value *, buf, u32, size)
{
	int err = -EINVAL;

	if (unlikely(size != sizeof(struct bpf_perf_event_value)))
		goto clear;
	err = perf_event_read_local(ctx->event, &buf->counter, &buf->enabled,
				    &buf->running);
	if (unlikely(err))
		goto clear;
	return 0;
clear:
	memset(buf, 0, size);
	return err;
}

static const struct bpf_func_proto bpf_perf_prog_read_value_proto = {
         .func           = bpf_perf_prog_read_value,
         .gpl_only       = true,
         .ret_type       = RET_INTEGER,
         .arg1_type      = ARG_PTR_TO_CTX,
         .arg2_type      = ARG_PTR_TO_UNINIT_MEM,
         .arg3_type      = ARG_CONST_SIZE,
};

static const struct bpf_func_proto *
pe_prog_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
	switch (func_id) {
	case BPF_FUNC_perf_event_output:
		return &bpf_perf_event_output_proto_tp;
	case BPF_FUNC_get_stackid:
		return &bpf_get_stackid_proto_tp;
	case BPF_FUNC_get_stack:
		return &bpf_get_stack_proto_tp;
	case BPF_FUNC_perf_prog_read_value:
		return &bpf_perf_prog_read_value_proto;
	default:
		return tracing_func_proto(func_id, prog);
	}
}

/*
 * bpf_raw_tp_regs are separate from bpf_pt_regs used from skb/xdp
 * to avoid potential recursive reuse issue when/if tracepoints are added
 * inside bpf_*_event_output, bpf_get_stackid and/or bpf_get_stack.
 *
 * Since raw tracepoints run despite bpf_prog_active, support concurrent usage
 * in normal, irq, and nmi context.
 */
struct bpf_raw_tp_regs {
	struct pt_regs regs[3];
};
static DEFINE_PER_CPU(struct bpf_raw_tp_regs, bpf_raw_tp_regs);
static DEFINE_PER_CPU(int, bpf_raw_tp_nest_level);
static struct pt_regs *get_bpf_raw_tp_regs(void)
{
	struct bpf_raw_tp_regs *tp_regs = this_cpu_ptr(&bpf_raw_tp_regs);
	int nest_level = this_cpu_inc_return(bpf_raw_tp_nest_level);

	if (WARN_ON_ONCE(nest_level > ARRAY_SIZE(tp_regs->regs))) {
		this_cpu_dec(bpf_raw_tp_nest_level);
		return ERR_PTR(-EBUSY);
	}

	return &tp_regs->regs[nest_level - 1];
}

static void put_bpf_raw_tp_regs(void)
{
	this_cpu_dec(bpf_raw_tp_nest_level);
}

BPF_CALL_5(bpf_perf_event_output_raw_tp, struct bpf_raw_tracepoint_args *, args,
	   struct bpf_map *, map, u64, flags, void *, data, u64, size)
{
	struct pt_regs *regs = get_bpf_raw_tp_regs();
	int ret;

	if (IS_ERR(regs))
		return PTR_ERR(regs);

	perf_fetch_caller_regs(regs);
	ret = ____bpf_perf_event_output(regs, map, flags, data, size);

	put_bpf_raw_tp_regs();
	return ret;
}

static const struct bpf_func_proto bpf_perf_event_output_proto_raw_tp = {
	.func		= bpf_perf_event_output_raw_tp,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_CTX,
	.arg2_type	= ARG_CONST_MAP_PTR,
	.arg3_type	= ARG_ANYTHING,
	.arg4_type	= ARG_PTR_TO_MEM,
	.arg5_type	= ARG_CONST_SIZE_OR_ZERO,
};

BPF_CALL_3(bpf_get_stackid_raw_tp, struct bpf_raw_tracepoint_args *, args,
	   struct bpf_map *, map, u64, flags)
{
	struct pt_regs *regs = get_bpf_raw_tp_regs();
	int ret;

	if (IS_ERR(regs))
		return PTR_ERR(regs);

	perf_fetch_caller_regs(regs);
	/* similar to bpf_perf_event_output_tp, but pt_regs fetched differently */
	ret = bpf_get_stackid((unsigned long) regs, (unsigned long) map,
			      flags, 0, 0);
	put_bpf_raw_tp_regs();
	return ret;
}

static const struct bpf_func_proto bpf_get_stackid_proto_raw_tp = {
	.func		= bpf_get_stackid_raw_tp,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_CTX,
	.arg2_type	= ARG_CONST_MAP_PTR,
	.arg3_type	= ARG_ANYTHING,
};

BPF_CALL_4(bpf_get_stack_raw_tp, struct bpf_raw_tracepoint_args *, args,
	   void *, buf, u32, size, u64, flags)
{
	struct pt_regs *regs = get_bpf_raw_tp_regs();
	int ret;

	if (IS_ERR(regs))
		return PTR_ERR(regs);

	perf_fetch_caller_regs(regs);
	ret = bpf_get_stack((unsigned long) regs, (unsigned long) buf,
			    (unsigned long) size, flags, 0);
	put_bpf_raw_tp_regs();
	return ret;
}

static const struct bpf_func_proto bpf_get_stack_proto_raw_tp = {
	.func		= bpf_get_stack_raw_tp,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_CTX,
	.arg2_type	= ARG_PTR_TO_MEM,
	.arg3_type	= ARG_CONST_SIZE_OR_ZERO,
	.arg4_type	= ARG_ANYTHING,
};

static const struct bpf_func_proto *
raw_tp_prog_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
	switch (func_id) {
	case BPF_FUNC_perf_event_output:
		return &bpf_perf_event_output_proto_raw_tp;
	case BPF_FUNC_get_stackid:
		return &bpf_get_stackid_proto_raw_tp;
	case BPF_FUNC_get_stack:
		return &bpf_get_stack_proto_raw_tp;
	default:
		return tracing_func_proto(func_id, prog);
	}
}

static bool raw_tp_prog_is_valid_access(int off, int size,
					enum bpf_access_type type,
					const struct bpf_prog *prog,
					struct bpf_insn_access_aux *info)
{
	/* largest tracepoint in the kernel has 12 args */
	if (off < 0 || off >= sizeof(__u64) * 12)
		return false;
	if (type != BPF_READ)
		return false;
	if (off % size != 0)
		return false;
	return true;
}

const struct bpf_verifier_ops raw_tracepoint_verifier_ops = {
	.get_func_proto  = raw_tp_prog_func_proto,
	.is_valid_access = raw_tp_prog_is_valid_access,
};

const struct bpf_prog_ops raw_tracepoint_prog_ops = {
};

static bool raw_tp_writable_prog_is_valid_access(int off, int size,
						 enum bpf_access_type type,
						 const struct bpf_prog *prog,
						 struct bpf_insn_access_aux *info)
{
	if (off == 0) {
		if (size != sizeof(u64) || type != BPF_READ)
			return false;
		info->reg_type = PTR_TO_TP_BUFFER;
	}
	return raw_tp_prog_is_valid_access(off, size, type, prog, info);
}

const struct bpf_verifier_ops raw_tracepoint_writable_verifier_ops = {
	.get_func_proto  = raw_tp_prog_func_proto,
	.is_valid_access = raw_tp_writable_prog_is_valid_access,
};

const struct bpf_prog_ops raw_tracepoint_writable_prog_ops = {
};

static bool pe_prog_is_valid_access(int off, int size, enum bpf_access_type type,
				    const struct bpf_prog *prog,
				    struct bpf_insn_access_aux *info)
{
	const int size_u64 = sizeof(u64);

	if (off < 0 || off >= sizeof(struct bpf_perf_event_data))
		return false;
	if (type != BPF_READ)
		return false;
	if (off % size != 0) {
		if (sizeof(unsigned long) != 4)
			return false;
		if (size != 8)
			return false;
		if (off % size != 4)
			return false;
	}

	switch (off) {
	case bpf_ctx_range(struct bpf_perf_event_data, sample_period):
		bpf_ctx_record_field_size(info, size_u64);
		if (!bpf_ctx_narrow_access_ok(off, size, size_u64))
			return false;
		break;
	case bpf_ctx_range(struct bpf_perf_event_data, addr):
		bpf_ctx_record_field_size(info, size_u64);
		if (!bpf_ctx_narrow_access_ok(off, size, size_u64))
			return false;
		break;
	default:
		if (size != sizeof(long))
			return false;
	}

	return true;
}

static u32 pe_prog_convert_ctx_access(enum bpf_access_type type,
				      const struct bpf_insn *si,
				      struct bpf_insn *insn_buf,
				      struct bpf_prog *prog, u32 *target_size)
{
	struct bpf_insn *insn = insn_buf;

	switch (si->off) {
	case offsetof(struct bpf_perf_event_data, sample_period):
		*insn++ = BPF_LDX_MEM(BPF_FIELD_SIZEOF(struct bpf_perf_event_data_kern,
						       data), si->dst_reg, si->src_reg,
				      offsetof(struct bpf_perf_event_data_kern, data));
		*insn++ = BPF_LDX_MEM(BPF_DW, si->dst_reg, si->dst_reg,
				      bpf_target_off(struct perf_sample_data, period, 8,
						     target_size));
		break;
	case offsetof(struct bpf_perf_event_data, addr):
		*insn++ = BPF_LDX_MEM(BPF_FIELD_SIZEOF(struct bpf_perf_event_data_kern,
						       data), si->dst_reg, si->src_reg,
				      offsetof(struct bpf_perf_event_data_kern, data));
		*insn++ = BPF_LDX_MEM(BPF_DW, si->dst_reg, si->dst_reg,
				      bpf_target_off(struct perf_sample_data, addr, 8,
						     target_size));
		break;
	default:
		*insn++ = BPF_LDX_MEM(BPF_FIELD_SIZEOF(struct bpf_perf_event_data_kern,
						       regs), si->dst_reg, si->src_reg,
				      offsetof(struct bpf_perf_event_data_kern, regs));
		*insn++ = BPF_LDX_MEM(BPF_SIZEOF(long), si->dst_reg, si->dst_reg,
				      si->off);
		break;
	}

	return insn - insn_buf;
}

const struct bpf_verifier_ops perf_event_verifier_ops = {
	.get_func_proto		= pe_prog_func_proto,
	.is_valid_access	= pe_prog_is_valid_access,
	.convert_ctx_access	= pe_prog_convert_ctx_access,
};

const struct bpf_prog_ops perf_event_prog_ops = {
};

static DEFINE_MUTEX(bpf_event_mutex);

#define BPF_TRACE_MAX_PROGS 64

int perf_event_attach_bpf_prog(struct perf_event *event,
			       struct bpf_prog *prog)
{
	struct bpf_prog_array *old_array;
	struct bpf_prog_array *new_array;
	int ret = -EEXIST;

	/*
	 * Kprobe override only works if they are on the function entry,
	 * and only if they are on the opt-in list.
	 */
	if (prog->kprobe_override &&
	    (!trace_kprobe_on_func_entry(event->tp_event) ||
	     !trace_kprobe_error_injectable(event->tp_event)))
		return -EINVAL;

	mutex_lock(&bpf_event_mutex);

	if (event->prog)
		goto unlock;

	old_array = bpf_event_rcu_dereference(event->tp_event->prog_array);
	if (old_array &&
	    bpf_prog_array_length(old_array) >= BPF_TRACE_MAX_PROGS) {
		ret = -E2BIG;
		goto unlock;
	}

	ret = bpf_prog_array_copy(old_array, NULL, prog, &new_array);
	if (ret < 0)
		goto unlock;

	/* set the new array to event->tp_event and set event->prog */
	event->prog = prog;
	rcu_assign_pointer(event->tp_event->prog_array, new_array);
	bpf_prog_array_free(old_array);

unlock:
	mutex_unlock(&bpf_event_mutex);
	return ret;
}

void perf_event_detach_bpf_prog(struct perf_event *event)
{
	struct bpf_prog_array *old_array;
	struct bpf_prog_array *new_array;
	int ret;

	mutex_lock(&bpf_event_mutex);

	if (!event->prog)
		goto unlock;

	old_array = bpf_event_rcu_dereference(event->tp_event->prog_array);
	ret = bpf_prog_array_copy(old_array, event->prog, NULL, &new_array);
	if (ret == -ENOENT)
		goto unlock;
	if (ret < 0) {
		bpf_prog_array_delete_safe(old_array, event->prog);
	} else {
		rcu_assign_pointer(event->tp_event->prog_array, new_array);
		bpf_prog_array_free(old_array);
	}

	bpf_prog_put(event->prog);
	event->prog = NULL;

unlock:
	mutex_unlock(&bpf_event_mutex);
}

int perf_event_query_prog_array(struct perf_event *event, void __user *info)
{
	struct perf_event_query_bpf __user *uquery = info;
	struct perf_event_query_bpf query = {};
	struct bpf_prog_array *progs;
	u32 *ids, prog_cnt, ids_len;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (event->attr.type != PERF_TYPE_TRACEPOINT)
		return -EINVAL;
	if (copy_from_user(&query, uquery, sizeof(query)))
		return -EFAULT;

	ids_len = query.ids_len;
	if (ids_len > BPF_TRACE_MAX_PROGS)
		return -E2BIG;
	ids = kcalloc(ids_len, sizeof(u32), GFP_USER | __GFP_NOWARN);
	if (!ids)
		return -ENOMEM;
	/*
	 * The above kcalloc returns ZERO_SIZE_PTR when ids_len = 0, which
	 * is required when user only wants to check for uquery->prog_cnt.
	 * There is no need to check for it since the case is handled
	 * gracefully in bpf_prog_array_copy_info.
	 */

	mutex_lock(&bpf_event_mutex);
	progs = bpf_event_rcu_dereference(event->tp_event->prog_array);
	ret = bpf_prog_array_copy_info(progs, ids, ids_len, &prog_cnt);
	mutex_unlock(&bpf_event_mutex);

	if (copy_to_user(&uquery->prog_cnt, &prog_cnt, sizeof(prog_cnt)) ||
	    copy_to_user(uquery->ids, ids, ids_len * sizeof(u32)))
		ret = -EFAULT;

	kfree(ids);
	return ret;
}

extern struct bpf_raw_event_map __start__bpf_raw_tp[];
extern struct bpf_raw_event_map __stop__bpf_raw_tp[];

struct bpf_raw_event_map *bpf_get_raw_tracepoint(const char *name)
{
	struct bpf_raw_event_map *btp = __start__bpf_raw_tp;

	for (; btp < __stop__bpf_raw_tp; btp++) {
		if (!strcmp(btp->tp->name, name))
			return btp;
	}

	return bpf_get_raw_tracepoint_module(name);
}

void bpf_put_raw_tracepoint(struct bpf_raw_event_map *btp)
{
	struct module *mod = __module_address((unsigned long)btp);

	if (mod)
		module_put(mod);
}

static __always_inline
void __bpf_trace_run(struct bpf_prog *prog, u64 *args)
{
	rcu_read_lock();
	preempt_disable();
	(void) BPF_PROG_RUN(prog, args);
	preempt_enable();
	rcu_read_unlock();
}

#define UNPACK(...)			__VA_ARGS__
#define REPEAT_1(FN, DL, X, ...)	FN(X)
#define REPEAT_2(FN, DL, X, ...)	FN(X) UNPACK DL REPEAT_1(FN, DL, __VA_ARGS__)
#define REPEAT_3(FN, DL, X, ...)	FN(X) UNPACK DL REPEAT_2(FN, DL, __VA_ARGS__)
#define REPEAT_4(FN, DL, X, ...)	FN(X) UNPACK DL REPEAT_3(FN, DL, __VA_ARGS__)
#define REPEAT_5(FN, DL, X, ...)	FN(X) UNPACK DL REPEAT_4(FN, DL, __VA_ARGS__)
#define REPEAT_6(FN, DL, X, ...)	FN(X) UNPACK DL REPEAT_5(FN, DL, __VA_ARGS__)
#define REPEAT_7(FN, DL, X, ...)	FN(X) UNPACK DL REPEAT_6(FN, DL, __VA_ARGS__)
#define REPEAT_8(FN, DL, X, ...)	FN(X) UNPACK DL REPEAT_7(FN, DL, __VA_ARGS__)
#define REPEAT_9(FN, DL, X, ...)	FN(X) UNPACK DL REPEAT_8(FN, DL, __VA_ARGS__)
#define REPEAT_10(FN, DL, X, ...)	FN(X) UNPACK DL REPEAT_9(FN, DL, __VA_ARGS__)
#define REPEAT_11(FN, DL, X, ...)	FN(X) UNPACK DL REPEAT_10(FN, DL, __VA_ARGS__)
#define REPEAT_12(FN, DL, X, ...)	FN(X) UNPACK DL REPEAT_11(FN, DL, __VA_ARGS__)
#define REPEAT(X, FN, DL, ...)		REPEAT_##X(FN, DL, __VA_ARGS__)

#define SARG(X)		u64 arg##X
#define COPY(X)		args[X] = arg##X

#define __DL_COM	(,)
#define __DL_SEM	(;)

#define __SEQ_0_11	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11

#define BPF_TRACE_DEFN_x(x)						\
	void bpf_trace_run##x(struct bpf_prog *prog,			\
			      REPEAT(x, SARG, __DL_COM, __SEQ_0_11))	\
	{								\
		u64 args[x];						\
		REPEAT(x, COPY, __DL_SEM, __SEQ_0_11);			\
		__bpf_trace_run(prog, args);				\
	}								\
	EXPORT_SYMBOL_GPL(bpf_trace_run##x)
BPF_TRACE_DEFN_x(1);
BPF_TRACE_DEFN_x(2);
BPF_TRACE_DEFN_x(3);
BPF_TRACE_DEFN_x(4);
BPF_TRACE_DEFN_x(5);
BPF_TRACE_DEFN_x(6);
BPF_TRACE_DEFN_x(7);
BPF_TRACE_DEFN_x(8);
BPF_TRACE_DEFN_x(9);
BPF_TRACE_DEFN_x(10);
BPF_TRACE_DEFN_x(11);
BPF_TRACE_DEFN_x(12);

static int __bpf_probe_register(struct bpf_raw_event_map *btp, struct bpf_prog *prog)
{
	struct tracepoint *tp = btp->tp;

	/*
	 * check that program doesn't access arguments beyond what's
	 * available in this tracepoint
	 */
	if (prog->aux->max_ctx_offset > btp->num_args * sizeof(u64))
		return -EINVAL;

	if (prog->aux->max_tp_access > btp->writable_size)
		return -EINVAL;

	return tracepoint_probe_register(tp, (void *)btp->bpf_func, prog);
}

int bpf_probe_register(struct bpf_raw_event_map *btp, struct bpf_prog *prog)
{
	return __bpf_probe_register(btp, prog);
}

int bpf_probe_unregister(struct bpf_raw_event_map *btp, struct bpf_prog *prog)
{
	return tracepoint_probe_unregister(btp->tp, (void *)btp->bpf_func, prog);
}

int bpf_get_perf_event_info(const struct perf_event *event, u32 *prog_id,
			    u32 *fd_type, const char **buf,
			    u64 *probe_offset, u64 *probe_addr)
{
	bool is_tracepoint, is_syscall_tp;
	struct bpf_prog *prog;
	int flags, err = 0;

	prog = event->prog;
	if (!prog)
		return -ENOENT;

	/* not supporting BPF_PROG_TYPE_PERF_EVENT yet */
	if (prog->type == BPF_PROG_TYPE_PERF_EVENT)
		return -EOPNOTSUPP;

	*prog_id = prog->aux->id;
	flags = event->tp_event->flags;
	is_tracepoint = flags & TRACE_EVENT_FL_TRACEPOINT;
	is_syscall_tp = is_syscall_trace_event(event->tp_event);

	if (is_tracepoint || is_syscall_tp) {
		*buf = is_tracepoint ? event->tp_event->tp->name
				     : event->tp_event->name;
		*fd_type = BPF_FD_TYPE_TRACEPOINT;
		*probe_offset = 0x0;
		*probe_addr = 0x0;
	} else {
		/* kprobe/uprobe */
		err = -EOPNOTSUPP;
#ifdef CONFIG_KPROBE_EVENTS
		if (flags & TRACE_EVENT_FL_KPROBE)
			err = bpf_get_kprobe_info(event, fd_type, buf,
						  probe_offset, probe_addr,
						  event->attr.type == PERF_TYPE_TRACEPOINT);
#endif
#ifdef CONFIG_UPROBE_EVENTS
		if (flags & TRACE_EVENT_FL_UPROBE)
			err = bpf_get_uprobe_info(event, fd_type, buf,
						  probe_offset,
						  event->attr.type == PERF_TYPE_TRACEPOINT);
#endif
	}

	return err;
}

static int __init send_signal_irq_work_init(void)
{
	int cpu;
	struct send_signal_irq_work *work;

	for_each_possible_cpu(cpu) {
		work = per_cpu_ptr(&send_signal_work, cpu);
		init_irq_work(&work->irq_work, do_bpf_send_signal);
	}
	return 0;
}

subsys_initcall(send_signal_irq_work_init);

#ifdef CONFIG_MODULES
static int bpf_event_notify(struct notifier_block *nb, unsigned long op,
			    void *module)
{
	struct bpf_trace_module *btm, *tmp;
	struct module *mod = module;

	if (mod->num_bpf_raw_events == 0 ||
	    (op != MODULE_STATE_COMING && op != MODULE_STATE_GOING))
		return 0;

	mutex_lock(&bpf_module_mutex);

	switch (op) {
	case MODULE_STATE_COMING:
		btm = kzalloc(sizeof(*btm), GFP_KERNEL);
		if (btm) {
			btm->module = module;
			list_add(&btm->list, &bpf_trace_modules);
		}
		break;
	case MODULE_STATE_GOING:
		list_for_each_entry_safe(btm, tmp, &bpf_trace_modules, list) {
			if (btm->module == module) {
				list_del(&btm->list);
				kfree(btm);
				break;
			}
		}
		break;
	}

	mutex_unlock(&bpf_module_mutex);

	return 0;
}

static struct notifier_block bpf_module_nb = {
	.notifier_call = bpf_event_notify,
};

static int __init bpf_event_init(void)
{
	register_module_notifier(&bpf_module_nb);
	return 0;
}

fs_initcall(bpf_event_init);
#endif /* CONFIG_MODULES */
