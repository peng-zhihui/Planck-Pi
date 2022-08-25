/*
 * drivers\media\cedar_ve
 * (C) Copyright 2010-2016
 * Reuuimlla Technology Co., Ltd. <www.allwinnertech.com>
 * fangning<fangning@allwinnertech.com>
 *
 * some simple description for this code
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/preempt.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/rmap.h>
#include <linux/wait.h>
#include <linux/semaphore.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/scatterlist.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/dma.h>
#include <linux/mm.h>
#include <asm/siginfo.h>
#include <asm/signal.h>
#include <linux/debugfs.h>
#include <linux/sched/signal.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_device.h>
#include "cedar_ve_priv.h"
#include "cedar_ve.h"
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/soc/sunxi/sunxi_sram.h>
#include <linux/reset.h>

#define DRV_VERSION "0.01alpha"

struct dentry *ve_debugfs_root;
struct ve_debugfs_buffer ve_debug_proc_info;

int g_dev_major = CEDARDEV_MAJOR;
int g_dev_minor = CEDARDEV_MINOR;

/*S_IRUGO represent that g_dev_major can be read,but canot be write*/
module_param(g_dev_major, int, S_IRUGO);
module_param(g_dev_minor, int, S_IRUGO);

static DECLARE_WAIT_QUEUE_HEAD(wait_ve);

static struct cedar_dev *cedar_devp;

static int clk_status;
static LIST_HEAD(run_task_list);
static LIST_HEAD(del_task_list);
static spinlock_t cedar_spin_lock;

static irqreturn_t VideoEngineInterupt(int irq, void *data)
{
	unsigned long ve_int_status_reg;
	unsigned long ve_int_ctrl_reg;
	unsigned int status;
	volatile int val;
	int modual_sel;
	unsigned int interrupt_enable;
	struct cedar_dev *dev = data;

	modual_sel = readl(cedar_devp->regs_macc + 0);
	if (dev->capabilities & CEDARV_ISP_OLD) {
		if (modual_sel & 0xa) {
			if ((modual_sel & 0xb) == 0xb) {
				/*jpg enc*/
				ve_int_status_reg =
					(unsigned long)(cedar_devp->regs_macc +
							0xb00 + 0x1c);
				ve_int_ctrl_reg =
					(unsigned long)(cedar_devp->regs_macc +
							0xb00 + 0x14);
				interrupt_enable =
					readl((void *)ve_int_ctrl_reg) & (0x7);
				status = readl((void *)ve_int_status_reg);
				status &= 0xf;
			} else {
				/*isp*/
				ve_int_status_reg =
					(unsigned long)(cedar_devp->regs_macc +
							0xa00 + 0x10);
				ve_int_ctrl_reg =
					(unsigned long)(cedar_devp->regs_macc +
							0xa00 + 0x08);
				interrupt_enable =
					readl((void *)ve_int_ctrl_reg) & (0x1);
				status = readl((void *)ve_int_status_reg);
				status &= 0x1;
			}

			if (status && interrupt_enable) {
				/*disable interrupt*/
				if ((modual_sel & 0xb) == 0xb) {
					ve_int_ctrl_reg =
						(unsigned long)(cedar_devp
									->regs_macc +
								0xb00 + 0x14);
					val = readl((void *)ve_int_ctrl_reg);
					writel(val & (~0x7),
					       (void *)ve_int_ctrl_reg);
				} else {
					ve_int_ctrl_reg =
						(unsigned long)(cedar_devp
									->regs_macc +
								0xa00 + 0x08);
					val = readl((void *)ve_int_ctrl_reg);
					writel(val & (~0x1),
					       (void *)ve_int_ctrl_reg);
				}

				cedar_devp->en_irq_value =
					1; /*hx modify 2011-8-1 16:08:47*/
				cedar_devp->en_irq_flag = 1;
				/*any interrupt will wake up wait queue*/
				wake_up(&wait_ve); /*ioctl*/
			}
		}
	} else {
		if (modual_sel & (3 << 6)) {
			if (modual_sel & (1 << 7)) {
				/*avc enc*/
				ve_int_status_reg =
					(unsigned long)(cedar_devp->regs_macc +
							0xb00 + 0x1c);
				ve_int_ctrl_reg =
					(unsigned long)(cedar_devp->regs_macc +
							0xb00 + 0x14);
				interrupt_enable =
					readl((void *)ve_int_ctrl_reg) & (0x7);
				status = readl((void *)ve_int_status_reg);
				status &= 0xf;
			} else {
				/*isp*/
				ve_int_status_reg =
					(unsigned long)(cedar_devp->regs_macc +
							0xa00 + 0x10);
				ve_int_ctrl_reg =
					(unsigned long)(cedar_devp->regs_macc +
							0xa00 + 0x08);
				interrupt_enable =
					readl((void *)ve_int_ctrl_reg) & (0x1);
				status = readl((void *)ve_int_status_reg);
				status &= 0x1;
			}

			/*modify by fangning 2013-05-22*/
			if (status && interrupt_enable) {
				/*disable interrupt*/
				/*avc enc*/
				if (modual_sel & (1 << 7)) {
					ve_int_ctrl_reg =
						(unsigned long)(cedar_devp
									->regs_macc +
								0xb00 + 0x14);
					val = readl((void *)ve_int_ctrl_reg);
					writel(val & (~0x7),
					       (void *)ve_int_ctrl_reg);
				} else {
					/*isp*/
					ve_int_ctrl_reg =
						(unsigned long)(cedar_devp
									->regs_macc +
								0xa00 + 0x08);
					val = readl((void *)ve_int_ctrl_reg);
					writel(val & (~0x1),
					       (void *)ve_int_ctrl_reg);
				}
				/*hx modify 2011-8-1 16:08:47*/
				cedar_devp->en_irq_value = 1;
				cedar_devp->en_irq_flag = 1;
				/*any interrupt will wake up wait queue*/
				wake_up(&wait_ve);
			}
		}
		if (dev->capabilities & CEDARV_ISP_NEW) {
			if (modual_sel & (0x20)) {
				ve_int_status_reg =
					(unsigned long)(cedar_devp->regs_macc +
							0xe00 + 0x1c);
				ve_int_ctrl_reg =
					(unsigned long)(cedar_devp->regs_macc +
							0xe00 + 0x14);
				interrupt_enable =
					readl((void *)ve_int_ctrl_reg) & (0x38);

				status = readl((void *)ve_int_status_reg);

				if ((status & 0x7) && interrupt_enable) {
					/*disable interrupt*/
					val = readl((void *)ve_int_ctrl_reg);
					writel(val & (~0x38),
					       (void *)ve_int_ctrl_reg);

					cedar_devp->jpeg_irq_value = 1;
					cedar_devp->jpeg_irq_flag = 1;

					/*any interrupt will wake up wait queue*/
					wake_up(&wait_ve);
				}
			}
		}
	}

	modual_sel &= 0xf;
	if (modual_sel <= 4) {
		/*estimate Which video format*/
		switch (modual_sel) {
		case 0: /*mpeg124*/
			ve_int_status_reg =
				(unsigned long)(cedar_devp->regs_macc + 0x100 +
						0x1c);
			ve_int_ctrl_reg =
				(unsigned long)(cedar_devp->regs_macc + 0x100 +
						0x14);
			interrupt_enable =
				readl((void *)ve_int_ctrl_reg) & (0x7c);
			break;
		case 1: /*h264*/
			ve_int_status_reg =
				(unsigned long)(cedar_devp->regs_macc + 0x200 +
						0x28);
			ve_int_ctrl_reg =
				(unsigned long)(cedar_devp->regs_macc + 0x200 +
						0x20);
			interrupt_enable =
				readl((void *)ve_int_ctrl_reg) & (0xf);
			break;
		case 2: /*vc1*/
			ve_int_status_reg =
				(unsigned long)(cedar_devp->regs_macc + 0x300 +
						0x2c);
			ve_int_ctrl_reg =
				(unsigned long)(cedar_devp->regs_macc + 0x300 +
						0x24);
			interrupt_enable =
				readl((void *)ve_int_ctrl_reg) & (0xf);
			break;
		case 3: /*rv*/
			ve_int_status_reg =
				(unsigned long)(cedar_devp->regs_macc + 0x400 +
						0x1c);
			ve_int_ctrl_reg =
				(unsigned long)(cedar_devp->regs_macc + 0x400 +
						0x14);
			interrupt_enable =
				readl((void *)ve_int_ctrl_reg) & (0xf);
			break;

		case 4: /*hevc*/
			ve_int_status_reg =
				(unsigned long)(cedar_devp->regs_macc + 0x500 +
						0x38);
			ve_int_ctrl_reg =
				(unsigned long)(cedar_devp->regs_macc + 0x500 +
						0x30);
			interrupt_enable =
				readl((void *)ve_int_ctrl_reg) & (0xf);
			break;

		default:
			ve_int_status_reg =
				(unsigned long)(cedar_devp->regs_macc + 0x100 +
						0x1c);
			ve_int_ctrl_reg =
				(unsigned long)(cedar_devp->regs_macc + 0x100 +
						0x14);
			interrupt_enable =
				readl((void *)ve_int_ctrl_reg) & (0xf);
			dev_warn(cedar_devp->platform_dev,
				 "ve mode :%x "
				 "not defined!\n",
				 modual_sel);
			break;
		}

		status = readl((void *)ve_int_status_reg);

		/*modify by fangning 2013-05-22*/
		if ((status & 0xf) && interrupt_enable) {
			/*disable interrupt*/
			if (modual_sel == 0) {
				val = readl((void *)ve_int_ctrl_reg);
				writel(val & (~0x7c), (void *)ve_int_ctrl_reg);
			} else {
				val = readl((void *)ve_int_ctrl_reg);
				writel(val & (~0xf), (void *)ve_int_ctrl_reg);
			}

			cedar_devp->de_irq_value = 1;
			cedar_devp->de_irq_flag = 1;
			/*any interrupt will wake up wait queue*/
			wake_up(&wait_ve);
		}
	}

	return IRQ_HANDLED;
}

int enable_cedar_hw_clk(void)
{
	unsigned long flags;
	int res = -EFAULT;

	spin_lock_irqsave(&cedar_spin_lock, flags);

	if (clk_status == 1)
		goto out;

	clk_status = 1;

	reset_control_deassert(cedar_devp->rstc);
	if (clk_enable(cedar_devp->mod_clk)) {
		dev_warn(cedar_devp->platform_dev,
			 "enable cedar_devp->mod_clk failed;\n");
		goto out;
	} else {
		res = 0;
	}

	AW_MEM_INIT_LIST_HEAD(&cedar_devp->list);
	dev_dbg(cedar_devp->platform_dev, "%s,%d\n", __func__, __LINE__);

out:
	spin_unlock_irqrestore(&cedar_spin_lock, flags);
	return res;
}

int disable_cedar_hw_clk(void)
{
	unsigned long flags;
	struct aw_mem_list_head *pos, *q;
	int res = -EFAULT;

	spin_lock_irqsave(&cedar_spin_lock, flags);

	if (clk_status == 0) {
		res = 0;
		goto out;
	}
	clk_status = 0;

	if ((NULL == cedar_devp->mod_clk) || (IS_ERR(cedar_devp->mod_clk)))
		dev_warn(cedar_devp->platform_dev,
			 "cedar_devp->mod_clk is invalid\n");
	else {
		clk_disable(cedar_devp->mod_clk);
		reset_control_assert(cedar_devp->rstc);
		res = 0;
	}

	aw_mem_list_for_each_safe(pos, q, &cedar_devp->list)
	{
		struct cedarv_iommu_buffer *tmp;

		tmp = aw_mem_list_entry(pos, struct cedarv_iommu_buffer,
					i_list);
		aw_mem_list_del(pos);
		kfree(tmp);
	}
	dev_dbg(cedar_devp->platform_dev, "%s,%d\n", __func__, __LINE__);

out:
	spin_unlock_irqrestore(&cedar_spin_lock, flags);
	return res;
}

void cedardev_insert_task(struct cedarv_engine_task *new_task)
{
	struct cedarv_engine_task *task_entry;
	unsigned long flags;

	spin_lock_irqsave(&cedar_spin_lock, flags);

	if (list_empty(&run_task_list))
		new_task->is_first_task = 1;

	list_for_each_entry (task_entry, &run_task_list, list) {
		if ((task_entry->is_first_task == 0) &&
		    (task_entry->running == 0) &&
		    (task_entry->t.task_prio < new_task->t.task_prio)) {
			break;
		}
	}

	list_add(&new_task->list, task_entry->list.prev);

	dev_dbg(cedar_devp->platform_dev, "%s,%d, TASK_ID:", __func__,
		__LINE__);
	list_for_each_entry (task_entry, &run_task_list, list) {
		dev_dbg(cedar_devp->platform_dev, "%d!", task_entry->t.ID);
	}
	dev_dbg(cedar_devp->platform_dev, "\n");

	mod_timer(&cedar_devp->cedar_engine_timer, jiffies + 0);

	spin_unlock_irqrestore(&cedar_spin_lock, flags);
}

int cedardev_del_task(int task_id)
{
	struct cedarv_engine_task *task_entry;
	unsigned long flags;

	spin_lock_irqsave(&cedar_spin_lock, flags);

	list_for_each_entry (task_entry, &run_task_list, list) {
		if (task_entry->t.ID == task_id &&
		    task_entry->status != TASK_RELEASE) {
			task_entry->status = TASK_RELEASE;

			spin_unlock_irqrestore(&cedar_spin_lock, flags);
			mod_timer(&cedar_devp->cedar_engine_timer, jiffies + 0);
			return 0;
		}
	}
	spin_unlock_irqrestore(&cedar_spin_lock, flags);

	return -1;
}

int cedardev_check_delay(int check_prio)
{
	struct cedarv_engine_task *task_entry;
	int timeout_total = 0;
	unsigned long flags;

	spin_lock_irqsave(&cedar_spin_lock, flags);
	list_for_each_entry (task_entry, &run_task_list, list) {
		if ((task_entry->t.task_prio >= check_prio) ||
		    (task_entry->running == 1) ||
		    (task_entry->is_first_task == 1))
			timeout_total = timeout_total + task_entry->t.frametime;
	}

	spin_unlock_irqrestore(&cedar_spin_lock, flags);
	dev_dbg(cedar_devp->platform_dev, "%s,%d,%d\n", __func__, __LINE__,
		timeout_total);
	return timeout_total;
}

static void cedar_engine_for_timer_rel(struct timer_list *arg)
{
	unsigned long flags;
	int ret = 0;
	spin_lock_irqsave(&cedar_spin_lock, flags);

	if (list_empty(&run_task_list)) {
		ret = disable_cedar_hw_clk();
		if (ret < 0) {
			dev_warn(cedar_devp->platform_dev,
				 "clk disable error!\n");
		}
	} else {
		dev_warn(cedar_devp->platform_dev, "clk disable time out "
						   "but task left\n");
		mod_timer(&cedar_devp->cedar_engine_timer,
			  jiffies + msecs_to_jiffies(TIMER_CIRCLE));
	}

	spin_unlock_irqrestore(&cedar_spin_lock, flags);
}

static void cedar_engine_for_events(struct timer_list *arg)
{
	struct cedarv_engine_task *task_entry, *task_entry_tmp;
	struct kernel_siginfo info;
	unsigned long flags;

	spin_lock_irqsave(&cedar_spin_lock, flags);

	list_for_each_entry_safe (task_entry, task_entry_tmp, &run_task_list,
				  list) {
		mod_timer(&cedar_devp->cedar_engine_timer_rel,
			  jiffies + msecs_to_jiffies(CLK_REL_TIME));
		if (task_entry->status == TASK_RELEASE ||
		    time_after(jiffies, task_entry->t.timeout)) {
			if (task_entry->status == TASK_INIT)
				task_entry->status = TASK_TIMEOUT;
			list_move(&task_entry->list, &del_task_list);
		}
	}

	list_for_each_entry_safe (task_entry, task_entry_tmp, &del_task_list,
				  list) {
		info.si_signo = SIG_CEDAR;
		info.si_code = task_entry->t.ID;
		if (task_entry->status == TASK_TIMEOUT) {
			info.si_errno = TASK_TIMEOUT;
			send_sig_info(SIG_CEDAR, &info,
				      task_entry->task_handle);
		} else if (task_entry->status == TASK_RELEASE) {
			info.si_errno = TASK_RELEASE;
			send_sig_info(SIG_CEDAR, &info,
				      task_entry->task_handle);
		}
		list_del(&task_entry->list);
		kfree(task_entry);
	}

	if (!list_empty(&run_task_list)) {
		task_entry = list_entry(run_task_list.next,
					struct cedarv_engine_task, list);
		if (task_entry->running == 0) {
			task_entry->running = 1;
			info.si_signo = SIG_CEDAR;
			info.si_code = task_entry->t.ID;
			info.si_errno = TASK_INIT;
			send_sig_info(SIG_CEDAR, &info,
				      task_entry->task_handle);
		}

		mod_timer(&cedar_devp->cedar_engine_timer,
			  jiffies + msecs_to_jiffies(TIMER_CIRCLE));
	}

	spin_unlock_irqrestore(&cedar_spin_lock, flags);
}

static long compat_cedardev_ioctl(struct file *filp, unsigned int cmd,
				  unsigned long arg)
{
	long ret = 0;
	int ve_timeout = 0;
	/*struct cedar_dev *devp;*/
	unsigned long flags;
	struct ve_info *info;

	info = filp->private_data;

	switch (cmd) {
	case IOCTL_ENGINE_REQ:
		if (down_interruptible(&cedar_devp->sem))
			return -ERESTARTSYS;
		cedar_devp->ref_count++;
		if (1 == cedar_devp->ref_count) {
			cedar_devp->last_min_freq = 0;
			enable_cedar_hw_clk();
		}
		up(&cedar_devp->sem);
		break;
	case IOCTL_ENGINE_REL:
		if (down_interruptible(&cedar_devp->sem))
			return -ERESTARTSYS;
		cedar_devp->ref_count--;
		if (0 == cedar_devp->ref_count) {
			ret = disable_cedar_hw_clk();
			if (ret < 0) {
				dev_warn(cedar_devp->platform_dev,
					 "IOCTL_ENGINE_REL "
					 "clk disable error!\n");
				up(&cedar_devp->sem);
				return -EFAULT;
			}
		}
		up(&cedar_devp->sem);
		return ret;
	case IOCTL_ENGINE_CHECK_DELAY: {
		struct cedarv_engine_task_info task_info;

		if (copy_from_user(&task_info, (void __user *)arg,
				   sizeof(struct cedarv_engine_task_info))) {
			dev_warn(cedar_devp->platform_dev,
				 "%d "
				 "copy_from_user fail\n",
				 IOCTL_ENGINE_CHECK_DELAY);
			return -EFAULT;
		}
		task_info.total_time =
			cedardev_check_delay(task_info.task_prio);
		dev_dbg(cedar_devp->platform_dev, "%s,%d,%d\n", __func__,
			__LINE__, task_info.total_time);
		task_info.frametime = 0;
		spin_lock_irqsave(&cedar_spin_lock, flags);
		if (!list_empty(&run_task_list)) {
			struct cedarv_engine_task *task_entry;
			dev_dbg(cedar_devp->platform_dev, "%s,%d\n", __func__,
				__LINE__);
			task_entry =
				list_entry(run_task_list.next,
					   struct cedarv_engine_task, list);
			if (task_entry->running == 1)
				task_info.frametime = task_entry->t.frametime;
			dev_dbg(cedar_devp->platform_dev, "%s,%d,%d\n",
				__func__, __LINE__, task_info.frametime);
		}
		spin_unlock_irqrestore(&cedar_spin_lock, flags);

		if (copy_to_user((void *)arg, &task_info,
				 sizeof(struct cedarv_engine_task_info))) {
			dev_warn(cedar_devp->platform_dev,
				 "%d "
				 "copy_to_user fail\n",
				 IOCTL_ENGINE_CHECK_DELAY);
			return -EFAULT;
		}
	} break;
	case IOCTL_WAIT_VE_DE:
		ve_timeout = (int)arg;
		cedar_devp->de_irq_value = 0;

		spin_lock_irqsave(&cedar_spin_lock, flags);
		if (cedar_devp->de_irq_flag)
			cedar_devp->de_irq_value = 1;
		spin_unlock_irqrestore(&cedar_spin_lock, flags);
		wait_event_timeout(wait_ve, cedar_devp->de_irq_flag,
				   ve_timeout * HZ);
		cedar_devp->de_irq_flag = 0;

		return cedar_devp->de_irq_value;

	case IOCTL_WAIT_VE_EN:

		ve_timeout = (int)arg;
		cedar_devp->en_irq_value = 0;

		spin_lock_irqsave(&cedar_spin_lock, flags);
		if (cedar_devp->en_irq_flag)
			cedar_devp->en_irq_value = 1;
		spin_unlock_irqrestore(&cedar_spin_lock, flags);

		wait_event_timeout(wait_ve, cedar_devp->en_irq_flag,
				   ve_timeout * HZ);
		cedar_devp->en_irq_flag = 0;

		return cedar_devp->en_irq_value;

	case IOCTL_WAIT_JPEG_DEC:
		ve_timeout = (int)arg;
		cedar_devp->jpeg_irq_value = 0;

		spin_lock_irqsave(&cedar_spin_lock, flags);
		if (cedar_devp->jpeg_irq_flag)
			cedar_devp->jpeg_irq_value = 1;
		spin_unlock_irqrestore(&cedar_spin_lock, flags);

		wait_event_timeout(wait_ve, cedar_devp->jpeg_irq_flag,
				   ve_timeout * HZ);
		cedar_devp->jpeg_irq_flag = 0;
		return cedar_devp->jpeg_irq_value;

	case IOCTL_ENABLE_VE:
		if (clk_prepare_enable(cedar_devp->mod_clk)) {
			dev_warn(cedar_devp->platform_dev,
				 "IOCTL_ENABLE_VE "
				 "enable cedar_devp->mod_clk failed!\n");
		}
		break;

	case IOCTL_DISABLE_VE:
		if ((NULL == cedar_devp->mod_clk) ||
		    IS_ERR(cedar_devp->mod_clk)) {
			dev_warn(cedar_devp->platform_dev,
				 "IOCTL_DISABLE_VE "
				 "cedar_devp->mod_clk is invalid\n");
			return -EFAULT;
		} else {
			clk_disable_unprepare(cedar_devp->mod_clk);
		}
		break;

	case IOCTL_RESET_VE:
		reset_control_assert(cedar_devp->rstc);
		reset_control_deassert(cedar_devp->rstc);
		break;

	case IOCTL_SET_DRAM_HIGH_CHANNAL: {
		dev_err(cedar_devp->platform_dev,
			"IOCTL_SET_DRAM_HIGH_CHANNAL NOT IMPL\n");
		break;
	}

	case IOCTL_SET_VE_FREQ: {
		int arg_rate = (int)arg;

		if (0 == cedar_devp->last_min_freq) {
			cedar_devp->last_min_freq = arg_rate;
		} else {
			if (arg_rate > cedar_devp->last_min_freq) {
				arg_rate = cedar_devp->last_min_freq;
			} else {
				cedar_devp->last_min_freq = arg_rate;
			}
		}
		if (arg_rate >= VE_CLK_LOW_WATER &&
		    arg_rate <= VE_CLK_HIGH_WATER &&
		    clk_get_rate(cedar_devp->mod_clk) / 1000000 != arg_rate) {
			clk_get_rate(cedar_devp->ahb_clk);
			if (clk_set_rate(cedar_devp->mod_clk,
					 arg_rate * 1000000)) {
				dev_warn(cedar_devp->platform_dev,
					 "set ve clock failed\n");
			}
		}
		ret = clk_get_rate(cedar_devp->mod_clk);
		break;
	}
	case IOCTL_GETVALUE_AVS2:
	case IOCTL_ADJUST_AVS2:
	case IOCTL_ADJUST_AVS2_ABS:
	case IOCTL_CONFIG_AVS2:
	case IOCTL_RESET_AVS2:
	case IOCTL_PAUSE_AVS2:
	case IOCTL_START_AVS2:
		dev_warn(cedar_devp->platform_dev,
			 "do not supprot this ioctrl now\n");
		break;

	case IOCTL_GET_ENV_INFO: {
		struct cedarv_env_infomation_compat env_info;

		env_info.phymem_start = 0;
		env_info.phymem_total_size = 0;
		env_info.address_macc = 0;
		if (copy_to_user((char *)arg, &env_info,
				 sizeof(struct cedarv_env_infomation_compat)))
			return -EFAULT;
	} break;
	case IOCTL_GET_IC_VER: {
		return 0;
	}
	case IOCTL_SET_REFCOUNT:
		cedar_devp->ref_count = (int)arg;
		break;
	case IOCTL_SET_VOL: {
		break;
	}
	case IOCTL_GET_LOCK: {
		int lock_ctl_ret = 0;
		u32 lock_type = arg;
		struct ve_info *vi = filp->private_data;

		if (lock_type == VE_LOCK_VDEC)
			mutex_lock(&cedar_devp->lock_vdec);
		else if (lock_type == VE_LOCK_VENC)
			mutex_lock(&cedar_devp->lock_venc);
		else if (lock_type == VE_LOCK_JDEC)
			mutex_lock(&cedar_devp->lock_jdec);
		else if (lock_type == VE_LOCK_00_REG)
			mutex_lock(&cedar_devp->lock_00_reg);
		else if (lock_type == VE_LOCK_04_REG)
			mutex_lock(&cedar_devp->lock_04_reg);
		else
			dev_err(cedar_devp->platform_dev,
				"invalid lock type '%d'", lock_type);

		if ((vi->lock_flags & lock_type) != 0)
			dev_err(cedar_devp->platform_dev,
				"when get lock, this should be 0!!!");

		mutex_lock(&vi->lock_flag_io);
		vi->lock_flags |= lock_type;
		mutex_unlock(&vi->lock_flag_io);

		return lock_ctl_ret;
	}
	case IOCTL_SET_PROC_INFO: {
		struct VE_PROC_INFO ve_info;
		unsigned char channel_id = 0;

		mutex_lock(&ve_debug_proc_info.lock_proc);
		if (copy_from_user(&ve_info, (void __user *)arg,
				   sizeof(struct VE_PROC_INFO))) {
			dev_warn(cedar_devp->platform_dev,
				 "IOCTL_SET_PROC_INFO copy_from_user fail\n");
			mutex_unlock(&ve_debug_proc_info.lock_proc);
			return -EFAULT;
		}

		channel_id = ve_info.channel_id;
		if (channel_id >= VE_DEBUGFS_MAX_CHANNEL) {
			dev_warn(
				cedar_devp->platform_dev,
				"set channel[%c] is bigger than max channel[%d]\n",
				channel_id, VE_DEBUGFS_MAX_CHANNEL);
			mutex_unlock(&ve_debug_proc_info.lock_proc);
			return -EFAULT;
		}

		ve_debug_proc_info.cur_channel_id = ve_info.channel_id;
		ve_debug_proc_info.proc_len[channel_id] = ve_info.proc_info_len;
		ve_debug_proc_info.proc_buf[channel_id] =
			ve_debug_proc_info.data +
			channel_id * VE_DEBUGFS_BUF_SIZE;
		break;
	}
	case IOCTL_COPY_PROC_INFO: {
		unsigned char channel_id;

		channel_id = ve_debug_proc_info.cur_channel_id;
		if (copy_from_user(ve_debug_proc_info.proc_buf[channel_id],
				   (void __user *)arg,
				   ve_debug_proc_info.proc_len[channel_id])) {
			dev_err(cedar_devp->platform_dev,
				"IOCTL_COPY_PROC_INFO copy_from_user fail\n");
			mutex_unlock(&ve_debug_proc_info.lock_proc);
			return -EFAULT;
		}
		mutex_unlock(&ve_debug_proc_info.lock_proc);
		break;
	}
	case IOCTL_STOP_PROC_INFO: {
		unsigned char channel_id;

		channel_id = arg;
		ve_debug_proc_info.proc_buf[channel_id] = NULL;

		break;
	}
	case IOCTL_RELEASE_LOCK: {
		int lock_ctl_ret = 0;
		do {
			u32 lock_type = arg;
			struct ve_info *vi = filp->private_data;

			if (!(vi->lock_flags & lock_type)) {
				dev_err(cedar_devp->platform_dev,
					"Not lock? flags: '%x/%x'.",
					vi->lock_flags, lock_type);
				lock_ctl_ret = -1;
				break; /* break 'do...while' */
			}

			mutex_lock(&vi->lock_flag_io);
			vi->lock_flags &= (~lock_type);
			mutex_unlock(&vi->lock_flag_io);

			if (lock_type == VE_LOCK_VDEC)
				mutex_unlock(&cedar_devp->lock_vdec);
			else if (lock_type == VE_LOCK_VENC)
				mutex_unlock(&cedar_devp->lock_venc);
			else if (lock_type == VE_LOCK_JDEC)
				mutex_unlock(&cedar_devp->lock_jdec);
			else if (lock_type == VE_LOCK_00_REG)
				mutex_unlock(&cedar_devp->lock_00_reg);
			else if (lock_type == VE_LOCK_04_REG)
				mutex_unlock(&cedar_devp->lock_04_reg);
			else
				dev_err(cedar_devp->platform_dev,
					"invalid lock type '%d'", lock_type);
		} while (0);
		return lock_ctl_ret;
	}
	case IOCTL_GET_IOMMU_ADDR: {
		int ret, i;
		struct sg_table *sgt, *sgt_bak;
		struct scatterlist *sgl, *sgl_bak;
		struct user_iommu_param sUserIommuParam;
		struct cedarv_iommu_buffer *pVeIommuBuf = NULL;

		pVeIommuBuf = (struct cedarv_iommu_buffer *)kmalloc(
			sizeof(struct cedarv_iommu_buffer), GFP_KERNEL);
		if (pVeIommuBuf == NULL) {
			dev_err(cedar_devp->platform_dev,
				"IOCTL_GET_IOMMU_ADDR malloc cedarv_iommu_buffererror\n");
			return -EFAULT;
		}
		if (copy_from_user(&sUserIommuParam, (void __user *)arg,
				   sizeof(struct user_iommu_param))) {
			dev_err(cedar_devp->platform_dev,
				"IOCTL_GET_IOMMU_ADDR copy_from_user error");
			return -EFAULT;
		}

		pVeIommuBuf->fd = sUserIommuParam.fd;
		pVeIommuBuf->dma_buf = dma_buf_get(pVeIommuBuf->fd);
		if (pVeIommuBuf->dma_buf < 0) {
			dev_err(cedar_devp->platform_dev,
				"ve get dma_buf error");
			return -EFAULT;
		}

		pVeIommuBuf->attachment = dma_buf_attach(
			pVeIommuBuf->dma_buf, cedar_devp->platform_dev);
		if (pVeIommuBuf->attachment < 0) {
			dev_err(cedar_devp->platform_dev,
				"ve get dma_buf_attachment error");
			goto RELEASE_DMA_BUF;
		}

		sgt = dma_buf_map_attachment(pVeIommuBuf->attachment,
					     DMA_BIDIRECTIONAL);

		sgt_bak = kmalloc(sizeof(struct sg_table),
				  GFP_KERNEL | __GFP_ZERO);
		if (sgt_bak == NULL)
			dev_err(cedar_devp->platform_dev, "alloc sgt fail\n");

		ret = sg_alloc_table(sgt_bak, sgt->nents, GFP_KERNEL);
		if (ret != 0)
			dev_err(cedar_devp->platform_dev, "alloc sgt fail\n");

		sgl_bak = sgt_bak->sgl;
		for_each_sg (sgt->sgl, sgl, sgt->nents, i) {
			sg_set_page(sgl_bak, sg_page(sgl), sgl->length,
				    sgl->offset);
			sgl_bak = sg_next(sgl_bak);
		}

		pVeIommuBuf->sgt = sgt_bak;
		if (pVeIommuBuf->sgt < 0) {
			dev_err(cedar_devp->platform_dev,
				"ve get sg_table error\n");
			goto RELEASE_DMA_BUF;
		}

		ret = dma_map_sg(cedar_devp->platform_dev,
				 pVeIommuBuf->sgt->sgl, pVeIommuBuf->sgt->nents,
				 DMA_BIDIRECTIONAL);
		if (ret != 1) {
			dev_err(cedar_devp->platform_dev,
				"ve dma_map_sg error\n");
			goto RELEASE_DMA_BUF;
		}

		pVeIommuBuf->iommu_addr = sg_dma_address(pVeIommuBuf->sgt->sgl);
		sUserIommuParam.iommu_addr =
			(unsigned int)(pVeIommuBuf->iommu_addr & 0xffffffff);

		if (copy_to_user((void __user *)arg, &sUserIommuParam,
				 sizeof(struct user_iommu_param))) {
			dev_err(cedar_devp->platform_dev,
				"ve get iommu copy_to_user error\n");
			goto RELEASE_DMA_BUF;
		}

		pVeIommuBuf->p_id = current->tgid;
		dev_dbg(cedar_devp->platform_dev,
			"fd:%d, iommu_addr:%lx, dma_buf:%p, dma_buf_attach:%p, sg_table:%p, nents:%d, pid:%d\n",
			pVeIommuBuf->fd, pVeIommuBuf->iommu_addr,
			pVeIommuBuf->dma_buf, pVeIommuBuf->attachment,
			pVeIommuBuf->sgt, pVeIommuBuf->sgt->nents,
			pVeIommuBuf->p_id);

		mutex_lock(&cedar_devp->lock_mem);
		aw_mem_list_add_tail(&pVeIommuBuf->i_list, &cedar_devp->list);
		mutex_unlock(&cedar_devp->lock_mem);
		break;

	RELEASE_DMA_BUF:
		if (pVeIommuBuf->dma_buf > 0) {
			if (pVeIommuBuf->attachment > 0) {
				if (pVeIommuBuf->sgt > 0) {
					dma_unmap_sg(cedar_devp->platform_dev,
						     pVeIommuBuf->sgt->sgl,
						     pVeIommuBuf->sgt->nents,
						     DMA_BIDIRECTIONAL);
					dma_buf_unmap_attachment(
						pVeIommuBuf->attachment,
						pVeIommuBuf->sgt,
						DMA_BIDIRECTIONAL);
					sg_free_table(pVeIommuBuf->sgt);
					kfree(pVeIommuBuf->sgt);
				}

				dma_buf_detach(pVeIommuBuf->dma_buf,
					       pVeIommuBuf->attachment);
			}

			dma_buf_put(pVeIommuBuf->dma_buf);
			return -1;
		}
		kfree(pVeIommuBuf);
		break;
	}
	case IOCTL_FREE_IOMMU_ADDR: {
		struct user_iommu_param sUserIommuParam;
		struct cedarv_iommu_buffer *pVeIommuBuf;

		if (copy_from_user(&sUserIommuParam, (void __user *)arg,
				   sizeof(struct user_iommu_param))) {
			dev_err(cedar_devp->platform_dev,
				"IOCTL_FREE_IOMMU_ADDR copy_from_user error");
			return -EFAULT;
		}
		aw_mem_list_for_each_entry(pVeIommuBuf, &cedar_devp->list,
					   i_list)
		{
			if (pVeIommuBuf->fd == sUserIommuParam.fd &&
			    pVeIommuBuf->p_id == current->tgid) {
				dev_dbg(cedar_devp->platform_dev,
					"free: fd:%d, iommu_addr:%lx, dma_buf:%p, dma_buf_attach:%p, sg_table:%p nets:%d, pid:%d\n",
					pVeIommuBuf->fd,
					pVeIommuBuf->iommu_addr,
					pVeIommuBuf->dma_buf,
					pVeIommuBuf->attachment,
					pVeIommuBuf->sgt,
					pVeIommuBuf->sgt->nents,
					pVeIommuBuf->p_id);

				if (pVeIommuBuf->dma_buf > 0) {
					if (pVeIommuBuf->attachment > 0) {
						if (pVeIommuBuf->sgt > 0) {
							dma_unmap_sg(
								cedar_devp
									->platform_dev,
								pVeIommuBuf->sgt
									->sgl,
								pVeIommuBuf->sgt
									->nents,
								DMA_BIDIRECTIONAL);
							dma_buf_unmap_attachment(
								pVeIommuBuf
									->attachment,
								pVeIommuBuf->sgt,
								DMA_BIDIRECTIONAL);
							sg_free_table(
								pVeIommuBuf
									->sgt);
							kfree(pVeIommuBuf->sgt);
						}

						dma_buf_detach(
							pVeIommuBuf->dma_buf,
							pVeIommuBuf->attachment);
					}

					dma_buf_put(pVeIommuBuf->dma_buf);
				}

				mutex_lock(&cedar_devp->lock_mem);
				aw_mem_list_del(&pVeIommuBuf->i_list);
				kfree(pVeIommuBuf);
				mutex_unlock(&cedar_devp->lock_mem);
				break;
			}
		}
		break;
	}
	default:
		return -1;
	}
	return ret;
}

static int cedardev_open(struct inode *inode, struct file *filp)
{
	struct ve_info *info;

	info = kmalloc(sizeof(struct ve_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->set_vol_flag = 0;

	filp->private_data = info;
	if (down_interruptible(&cedar_devp->sem)) {
		return -ERESTARTSYS;
	}

	/* init other resource here */
	if (0 == cedar_devp->ref_count) {
		cedar_devp->de_irq_flag = 0;
		cedar_devp->en_irq_flag = 0;
		cedar_devp->jpeg_irq_flag = 0;
	}

	up(&cedar_devp->sem);
	nonseekable_open(inode, filp);

	mutex_init(&info->lock_flag_io);
	info->lock_flags = 0;

	return 0;
}

static int cedardev_release(struct inode *inode, struct file *filp)
{
	struct ve_info *info;

	info = filp->private_data;

	mutex_lock(&info->lock_flag_io);
	/* lock status */
	if (info->lock_flags) {
		dev_warn(cedar_devp->platform_dev, "release lost-lock...");
		if (info->lock_flags & VE_LOCK_VDEC)
			mutex_unlock(&cedar_devp->lock_vdec);

		if (info->lock_flags & VE_LOCK_VENC)
			mutex_unlock(&cedar_devp->lock_venc);

		if (info->lock_flags & VE_LOCK_JDEC)
			mutex_unlock(&cedar_devp->lock_jdec);

		if (info->lock_flags & VE_LOCK_00_REG)
			mutex_unlock(&cedar_devp->lock_00_reg);

		if (info->lock_flags & VE_LOCK_04_REG)
			mutex_unlock(&cedar_devp->lock_04_reg);

		info->lock_flags = 0;
	}

	mutex_unlock(&info->lock_flag_io);
	mutex_destroy(&info->lock_flag_io);

	if (down_interruptible(&cedar_devp->sem)) {
		return -ERESTARTSYS;
	}

	/* release other resource here */
	if (0 == cedar_devp->ref_count) {
		cedar_devp->de_irq_flag = 1;
		cedar_devp->en_irq_flag = 1;
		cedar_devp->jpeg_irq_flag = 1;
	}
	up(&cedar_devp->sem);

	kfree(info);
	return 0;
}

static void cedardev_vma_open(struct vm_area_struct *vma)
{
}

static void cedardev_vma_close(struct vm_area_struct *vma)
{
}

static struct vm_operations_struct cedardev_remap_vm_ops = {
	.open = cedardev_vma_open,
	.close = cedardev_vma_close,
};

#ifdef CONFIG_PM
static int snd_sw_cedar_suspend(struct platform_device *pdev,
				pm_message_t state)
{
	int ret = 0;

	printk("[cedar] standby suspend\n");
	ret = disable_cedar_hw_clk();

	if (ret < 0) {
		dev_warn(cedar_devp->platform_dev,
			 "cedar clk disable somewhere error!\n");
		return -EFAULT;
	}

	return 0;
}

static int snd_sw_cedar_resume(struct platform_device *pdev)
{
	int ret = 0;

	printk("[cedar] standby resume\n");

	if (cedar_devp->ref_count == 0) {
		return 0;
	}

	ret = enable_cedar_hw_clk();
	if (ret < 0) {
		dev_warn(cedar_devp->platform_dev,
			 "cedar clk enable somewhere error!\n");
		return -EFAULT;
	}
	return 0;
}
#endif

static int cedardev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long temp_pfn;

	if (vma->vm_end - vma->vm_start == 0) {
		dev_warn(cedar_devp->platform_dev,
			 "vma->vm_end is equal vma->vm_start : %lx\n",
			 vma->vm_start);
		return 0;
	}
	if (vma->vm_pgoff > (~0UL >> PAGE_SHIFT)) {
		dev_err(cedar_devp->platform_dev,
			"the vma->vm_pgoff is %lx,it is large than the largest page number\n",
			vma->vm_pgoff);
		return -EINVAL;
	}

	temp_pfn = cedar_devp->phy_addr >> 12;

	/* Set reserved and I/O flag for the area. */
	vma->vm_flags |= /*VM_RESERVED | */ VM_IO;
	/* Select uncached access. */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (io_remap_pfn_range(vma, vma->vm_start, temp_pfn,
			       vma->vm_end - vma->vm_start,
			       vma->vm_page_prot)) {
		return -EAGAIN;
	}

	vma->vm_ops = &cedardev_remap_vm_ops;
	cedardev_vma_open(vma);

	return 0;
}

static const struct file_operations cedardev_fops = {
	.owner = THIS_MODULE,
	.mmap = cedardev_mmap,
	.open = cedardev_open,
	.release = cedardev_release,
	.llseek = no_llseek,
	.unlocked_ioctl = compat_cedardev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_cedardev_ioctl,
#endif
};

static int ve_debugfs_open(struct inode *inode, struct file *file)
{
	int i = 0;
	char *pData;
	struct ve_debugfs_proc *pVeProc;

	pVeProc = kmalloc(sizeof(struct ve_debugfs_proc), GFP_KERNEL);
	if (pVeProc == NULL) {
		dev_err(cedar_devp->platform_dev, "kmalloc pVeProc fail\n");
		return -ENOMEM;
	}
	pVeProc->len = 0;
	memset(pVeProc->data, 0, VE_DEBUGFS_BUF_SIZE * VE_DEBUGFS_MAX_CHANNEL);

	pData = pVeProc->data;
	mutex_lock(&ve_debug_proc_info.lock_proc);
	for (i = 0; i < VE_DEBUGFS_MAX_CHANNEL; i++) {
		if (ve_debug_proc_info.proc_buf[i] != NULL) {
			memcpy(pData, ve_debug_proc_info.proc_buf[i],
			       ve_debug_proc_info.proc_len[i]);
			pData += ve_debug_proc_info.proc_len[i];
			pVeProc->len += ve_debug_proc_info.proc_len[i];
		}
	}
	mutex_unlock(&ve_debug_proc_info.lock_proc);

	file->private_data = pVeProc;
	return 0;
}

static ssize_t ve_debugfs_read(struct file *file, char __user *user_buf,
			       size_t nbytes, loff_t *ppos)
{
	struct ve_debugfs_proc *pVeProc = file->private_data;

	if (pVeProc->len == 0) {
		dev_dbg(cedar_devp->platform_dev,
			"there is no any codec working currently\n");
		return 0;
	}

	return simple_read_from_buffer(user_buf, nbytes, ppos, pVeProc->data,
				       pVeProc->len);
}

static int ve_debugfs_release(struct inode *inode, struct file *file)
{
	struct ve_debugfs_proc *pVeProc = file->private_data;

	kfree(pVeProc);
	pVeProc = NULL;
	file->private_data = NULL;

	return 0;
}

static const struct file_operations ve_debugfs_fops = {
	.owner = THIS_MODULE,
	.open = ve_debugfs_open,
	.llseek = no_llseek,
	.read = ve_debugfs_read,
	.release = ve_debugfs_release,
};

int sunxi_ve_debug_register_driver(void)
{
	struct dentry *dent;

	ve_debugfs_root = debugfs_create_dir("mpp", 0);

	if (ve_debugfs_root == NULL) {
		dev_err(cedar_devp->platform_dev,
			"get debugfs_mpp_root is NULL, please check mpp\n");
		return -ENOENT;
	}

	dent = debugfs_create_file("ve", 0444, ve_debugfs_root, NULL,
				   &ve_debugfs_fops);
	if (IS_ERR_OR_NULL(dent)) {
		dev_err(cedar_devp->platform_dev,
			"Unable to create debugfs status file.\n");
		debugfs_remove_recursive(ve_debugfs_root);
		ve_debugfs_root = NULL;
		return -ENODEV;
	}

	return 0;
}

void sunxi_ve_debug_unregister_driver(void)
{
	if (ve_debugfs_root == NULL)
		return;
	debugfs_remove_recursive(ve_debugfs_root);
	ve_debugfs_root = NULL;
}

static int cedardev_init(struct platform_device *pdev)
{
	int ret = 0;
	int i = 0;
	int devno;
	struct device_node *node;
	dev_t dev;
	const struct cedar_variant *variant;
	struct resource *res;

	node = pdev->dev.of_node;
	dev = 0;
	dev_info(&pdev->dev, "sunxi cedar version " DRV_VERSION "\n");
	dev_dbg(cedar_devp->platform_dev, "install start!!!\n");

	variant = of_device_get_match_data(&pdev->dev);
	if (!variant)
		return -EINVAL;

	spin_lock_init(&cedar_spin_lock);
	cedar_devp = kcalloc(1, sizeof(struct cedar_dev), GFP_KERNEL);
	if (cedar_devp == NULL) {
		dev_warn(cedar_devp->platform_dev,
			 "malloc mem for cedar device err\n");
		return -ENOMEM;
	}

	cedar_devp->platform_dev = &pdev->dev;
	cedar_devp->capabilities = variant->capabilities;

	ret = sunxi_sram_claim(cedar_devp->platform_dev);
	if (ret) {
		dev_err(cedar_devp->platform_dev, "Failed to claim SRAM\n");
		goto err_mem;
	}

	cedar_devp->ahb_clk = devm_clk_get(cedar_devp->platform_dev, "ahb");
	if (IS_ERR(cedar_devp->ahb_clk)) {
		dev_err(cedar_devp->platform_dev, "Failed to get AHB clock\n");
		ret = PTR_ERR(cedar_devp->ahb_clk);
		goto err_sram;
	}

	cedar_devp->mod_clk = devm_clk_get(cedar_devp->platform_dev, "mod");
	if (IS_ERR(cedar_devp->mod_clk)) {
		dev_err(cedar_devp->platform_dev, "Failed to get MOD clock\n");
		ret = PTR_ERR(cedar_devp->mod_clk);
		goto err_sram;
	}

	cedar_devp->ram_clk = devm_clk_get(cedar_devp->platform_dev, "ram");
	if (IS_ERR(cedar_devp->ram_clk)) {
		dev_err(cedar_devp->platform_dev, "Failed to get RAM clock\n");
		ret = PTR_ERR(cedar_devp->ram_clk);
		goto err_sram;
	}

	cedar_devp->rstc =
		devm_reset_control_get(cedar_devp->platform_dev, NULL);
	if (IS_ERR(cedar_devp->rstc)) {
		dev_err(cedar_devp->platform_dev,
			"Failed to get reset control\n");
		ret = PTR_ERR(cedar_devp->rstc);
		goto err_sram;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	cedar_devp->regs_macc =
		devm_ioremap_resource(cedar_devp->platform_dev, res);
	if (IS_ERR(cedar_devp->regs_macc)) {
		dev_err(cedar_devp->platform_dev, "Failed to map registers\n");
		ret = PTR_ERR(cedar_devp->regs_macc);
		goto err_sram;
	}
	cedar_devp->phy_addr = res->start;

	ret = clk_set_rate(cedar_devp->mod_clk, variant->mod_rate);
	if (ret) {
		dev_err(cedar_devp->platform_dev, "Failed to set clock rate\n");
		goto err_sram;
	}

	ret = clk_prepare_enable(cedar_devp->ahb_clk);
	if (ret) {
		dev_err(cedar_devp->platform_dev,
			"Failed to enable AHB clock\n");
		goto err_sram;
	}

	ret = clk_prepare_enable(cedar_devp->mod_clk);
	if (ret) {
		dev_err(cedar_devp->platform_dev,
			"Failed to enable MOD clock\n");
		goto err_ahb_clk;
	}

	ret = clk_prepare_enable(cedar_devp->ram_clk);
	if (ret) {
		dev_err(cedar_devp->platform_dev,
			"Failed to enable RAM clock\n");
		goto err_mod_clk;
	}

	ret = reset_control_reset(cedar_devp->rstc);
	if (ret) {
		dev_err(cedar_devp->platform_dev, "Failed to apply reset\n");
		goto err_ram_clk;
	}

	cedar_devp->irq = irq_of_parse_and_map(node, 0);
	dev_info(cedar_devp->platform_dev, "cedar-ve the get irq is %d\n",
		 cedar_devp->irq);
	if (cedar_devp->irq <= 0)
		dev_err(cedar_devp->platform_dev, "Can't parse IRQ");

	sema_init(&cedar_devp->sem, 1);
	init_waitqueue_head(&cedar_devp->wq);

	ret = request_irq(cedar_devp->irq, VideoEngineInterupt, 0, "cedar_dev",
			  cedar_devp);
	if (ret < 0) {
		dev_err(cedar_devp->platform_dev, "request irq err\n");
		return -EINVAL;
	}

	/*register or alloc the device number.*/
	if (g_dev_major) {
		dev = MKDEV(g_dev_major, g_dev_minor);
		ret = register_chrdev_region(dev, 1, "cedar_dev");
	} else {
		ret = alloc_chrdev_region(&dev, g_dev_minor, 1, "cedar_dev");
		g_dev_major = MAJOR(dev);
		g_dev_minor = MINOR(dev);
	}
	if (ret < 0) {
		dev_err(cedar_devp->platform_dev,
			"cedar_dev: can't get major %d\n", g_dev_major);
		return ret;
	}

	/* Create char device */
	devno = MKDEV(g_dev_major, g_dev_minor);
	cdev_init(&cedar_devp->cdev, &cedardev_fops);
	cedar_devp->cdev.owner = THIS_MODULE;
	/* cedar_devp->cdev.ops = &cedardev_fops; */
	ret = cdev_add(&cedar_devp->cdev, devno, 1);
	if (ret) {
		dev_warn(cedar_devp->platform_dev, "Err:%d add cedardev", ret);
	}
	cedar_devp->class = class_create(THIS_MODULE, "cedar_dev");
	cedar_devp->dev = device_create(cedar_devp->class, NULL, devno, NULL,
					"cedar_dev");

	timer_setup(&cedar_devp->cedar_engine_timer, cedar_engine_for_events,
		    0);
	timer_setup(&cedar_devp->cedar_engine_timer_rel,
		    cedar_engine_for_timer_rel, 0);

	mutex_init(&cedar_devp->lock_vdec);
	mutex_init(&cedar_devp->lock_venc);
	mutex_init(&cedar_devp->lock_jdec);
	mutex_init(&cedar_devp->lock_00_reg);
	mutex_init(&cedar_devp->lock_04_reg);
	mutex_init(&cedar_devp->lock_mem);

	ret = sunxi_ve_debug_register_driver();
	if (ret) {
		dev_err(cedar_devp->platform_dev,
			"sunxi ve debug register driver failed!\n");
		return ret;
	}

	memset(&ve_debug_proc_info, 0, sizeof(struct ve_debugfs_buffer));
	for (i = 0; i < VE_DEBUGFS_MAX_CHANNEL; i++) {
		ve_debug_proc_info.proc_buf[i] = NULL;
	}
	ve_debug_proc_info.data = kmalloc(
		VE_DEBUGFS_BUF_SIZE * VE_DEBUGFS_MAX_CHANNEL, GFP_KERNEL);
	if (!ve_debug_proc_info.data) {
		dev_err(cedar_devp->platform_dev,
			"kmalloc proc buffer failed!\n");
		return -ENOMEM;
	}
	mutex_init(&ve_debug_proc_info.lock_proc);
	dev_dbg(cedar_devp->platform_dev,
		"ve_debug_proc_info:%p, data:%p, lock:%p\n",
		&ve_debug_proc_info, ve_debug_proc_info.data,
		&ve_debug_proc_info.lock_proc);

	dev_dbg(cedar_devp->platform_dev, "install end!!!\n");
	return 0;

err_ram_clk:
	clk_disable_unprepare(cedar_devp->ram_clk);
err_mod_clk:
	clk_disable_unprepare(cedar_devp->mod_clk);
err_ahb_clk:
	clk_disable_unprepare(cedar_devp->ahb_clk);
err_sram:
	sunxi_sram_release(cedar_devp->platform_dev);
err_mem:
	kfree(cedar_devp);
	return ret;
}

static void cedardev_exit(void)
{
	dev_t dev;
	dev = MKDEV(g_dev_major, g_dev_minor);

	free_irq(cedar_devp->irq, cedar_devp);

	/* Destroy char device */
	if (cedar_devp) {
		cdev_del(&cedar_devp->cdev);
		device_destroy(cedar_devp->class, dev);
		class_destroy(cedar_devp->class);
	}

	reset_control_assert(cedar_devp->rstc);
	clk_disable_unprepare(cedar_devp->ram_clk);
	clk_disable_unprepare(cedar_devp->mod_clk);
	clk_disable_unprepare(cedar_devp->ahb_clk);
	sunxi_sram_release(cedar_devp->platform_dev);

	unregister_chrdev_region(dev, 1);
	if (cedar_devp) {
		kfree(cedar_devp);
	}

	sunxi_ve_debug_unregister_driver();
	kfree(ve_debug_proc_info.data);
}

static int sunxi_cedar_remove(struct platform_device *pdev)
{
	cedardev_exit();
	return 0;
}

static int sunxi_cedar_probe(struct platform_device *pdev)
{
	cedardev_init(pdev);
	return 0;
}

static struct cedar_variant sun8i_v3_quirk = {
	.capabilities = CEDARV_ISP_NEW,
	.mod_rate = 402000000,
};

static struct cedar_variant sun8i_h3_quirk = {
	.capabilities = 0,
	.mod_rate = 402000000,
};

static struct cedar_variant suniv_f1c100s_quirk = {
	.capabilities = CEDARV_ISP_OLD,
	.mod_rate = 300000000,
};

static struct of_device_id sunxi_cedar_ve_match[] = {
	{ .compatible = "allwinner,sun8i-v3-cedar", .data = &sun8i_v3_quirk },
	{ .compatible = "allwinner,sun8i-h3-cedar", .data = &sun8i_h3_quirk },
	{ .compatible = "allwinner,suniv-f1c100s-cedar", .data = &suniv_f1c100s_quirk },
	{}
};
MODULE_DEVICE_TABLE(of, sunxi_cedar_ve_match);

static struct platform_driver sunxi_cedar_driver = {
	.probe = sunxi_cedar_probe,
	.remove = sunxi_cedar_remove,
#if defined(CONFIG_PM)
	.suspend = snd_sw_cedar_suspend,
	.resume = snd_sw_cedar_resume,
#endif
	.driver = {
		.name = "sunxi-cedar",
		.owner = THIS_MODULE,
		.of_match_table = sunxi_cedar_ve_match,
	},
};

static int __init sunxi_cedar_init(void)
{
	return platform_driver_register(&sunxi_cedar_driver);
}

static void __exit sunxi_cedar_exit(void)
{
	platform_driver_unregister(&sunxi_cedar_driver);
}

module_init(sunxi_cedar_init);
module_exit(sunxi_cedar_exit);

MODULE_AUTHOR("Soft-Reuuimlla");
MODULE_DESCRIPTION("User mode CEDAR device interface");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
MODULE_ALIAS("platform:cedarx-sunxi");
