/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Generic RTC interface.
 * This version contains the part of the user interface to the Real Time Clock
 * service. It is used with both the legacy mc146818 and also  EFI
 * Struct rtc_time and first 12 ioctl by Paul Gortmaker, 1996 - separated out
 * from <linux/mc146818rtc.h> to this file for 2.4 kernels.
 *
 * Copyright (C) 1999 Hewlett-Packard Co.
 * Copyright (C) 1999 Stephane Eranian <eranian@hpl.hp.com>
 */
#ifndef _LINUX_RTC_H_
#define _LINUX_RTC_H_


#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/nvmem-provider.h>
#include <uapi/linux/rtc.h>

extern int rtc_month_days(unsigned int month, unsigned int year);
extern int rtc_year_days(unsigned int day, unsigned int month, unsigned int year);
extern int rtc_valid_tm(struct rtc_time *tm);
extern time64_t rtc_tm_to_time64(struct rtc_time *tm);
extern void rtc_time64_to_tm(time64_t time, struct rtc_time *tm);
ktime_t rtc_tm_to_ktime(struct rtc_time tm);
struct rtc_time rtc_ktime_to_tm(ktime_t kt);

/*
 * rtc_tm_sub - Return the difference in seconds.
 */
static inline time64_t rtc_tm_sub(struct rtc_time *lhs, struct rtc_time *rhs)
{
	return rtc_tm_to_time64(lhs) - rtc_tm_to_time64(rhs);
}

static inline void rtc_time_to_tm(unsigned long time, struct rtc_time *tm)
{
	rtc_time64_to_tm(time, tm);
}

static inline int rtc_tm_to_time(struct rtc_time *tm, unsigned long *time)
{
	*time = rtc_tm_to_time64(tm);

	return 0;
}

#include <linux/device.h>
#include <linux/seq_file.h>
#include <linux/cdev.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/timerqueue.h>
#include <linux/workqueue.h>

extern struct class *rtc_class;

/*
 * For these RTC methods the device parameter is the physical device
 * on whatever bus holds the hardware (I2C, Platform, SPI, etc), which
 * was passed to rtc_device_register().  Its driver_data normally holds
 * device state, including the rtc_device pointer for the RTC.
 *
 * Most of these methods are called with rtc_device.ops_lock held,
 * through the rtc_*(struct rtc_device *, ...) calls.
 *
 * The (current) exceptions are mostly filesystem hooks:
 *   - the proc() hook for procfs
 *   - non-ioctl() chardev hooks:  open(), release()
 *
 * REVISIT those periodic irq calls *do* have ops_lock when they're
 * issued through ioctl() ...
 */
struct rtc_class_ops {
	int (*ioctl)(struct device *, unsigned int, unsigned long);
	int (*read_time)(struct device *, struct rtc_time *);
	int (*set_time)(struct device *, struct rtc_time *);
	int (*read_alarm)(struct device *, struct rtc_wkalrm *);
	int (*set_alarm)(struct device *, struct rtc_wkalrm *);
	int (*proc)(struct device *, struct seq_file *);
	int (*alarm_irq_enable)(struct device *, unsigned int enabled);
	int (*read_offset)(struct device *, long *offset);
	int (*set_offset)(struct device *, long offset);
};

struct rtc_device;

struct rtc_timer {
	struct timerqueue_node node;
	ktime_t period;
	void (*func)(struct rtc_device *rtc);
	struct rtc_device *rtc;
	int enabled;
};

/* flags */
#define RTC_DEV_BUSY 0

struct rtc_device {
	struct device dev;
	struct module *owner;

	int id;

	const struct rtc_class_ops *ops;
	struct mutex ops_lock;

	struct cdev char_dev;
	unsigned long flags;

	unsigned long irq_data;
	spinlock_t irq_lock;
	wait_queue_head_t irq_queue;
	struct fasync_struct *async_queue;

	int irq_freq;
	int max_user_freq;

	struct timerqueue_head timerqueue;
	struct rtc_timer aie_timer;
	struct rtc_timer uie_rtctimer;
	struct hrtimer pie_timer; /* sub second exp, so needs hrtimer */
	int pie_enabled;
	struct work_struct irqwork;
	/* Some hardware can't support UIE mode */
	int uie_unsupported;

	/* Number of nsec it takes to set the RTC clock. This influences when
	 * the set ops are called. An offset:
	 *   - of 0.5 s will call RTC set for wall clock time 10.0 s at 9.5 s
	 *   - of 1.5 s will call RTC set for wall clock time 10.0 s at 8.5 s
	 *   - of -0.5 s will call RTC set for wall clock time 10.0 s at 10.5 s
	 */
	long set_offset_nsec;

	bool registered;

	/* Old ABI support */
	bool nvram_old_abi;
	struct bin_attribute *nvram;

	time64_t range_min;
	timeu64_t range_max;
	time64_t start_secs;
	time64_t offset_secs;
	bool set_start_time;

#ifdef CONFIG_RTC_INTF_DEV_UIE_EMUL
	struct work_struct uie_task;
	struct timer_list uie_timer;
	/* Those fields are protected by rtc->irq_lock */
	unsigned int oldsecs;
	unsigned int uie_irq_active:1;
	unsigned int stop_uie_polling:1;
	unsigned int uie_task_active:1;
	unsigned int uie_timer_active:1;
#endif
};
#define to_rtc_device(d) container_of(d, struct rtc_device, dev)

/* useful timestamps */
#define RTC_TIMESTAMP_BEGIN_1900	-2208988800LL /* 1900-01-01 00:00:00 */
#define RTC_TIMESTAMP_BEGIN_2000	946684800LL /* 2000-01-01 00:00:00 */
#define RTC_TIMESTAMP_END_2063		2966371199LL /* 2063-12-31 23:59:59 */
#define RTC_TIMESTAMP_END_2099		4102444799LL /* 2099-12-31 23:59:59 */
#define RTC_TIMESTAMP_END_9999		253402300799LL /* 9999-12-31 23:59:59 */

extern struct rtc_device *devm_rtc_device_register(struct device *dev,
					const char *name,
					const struct rtc_class_ops *ops,
					struct module *owner);
struct rtc_device *devm_rtc_allocate_device(struct device *dev);
int __rtc_register_device(struct module *owner, struct rtc_device *rtc);

extern int rtc_read_time(struct rtc_device *rtc, struct rtc_time *tm);
extern int rtc_set_time(struct rtc_device *rtc, struct rtc_time *tm);
extern int rtc_set_ntp_time(struct timespec64 now, unsigned long *target_nsec);
int __rtc_read_alarm(struct rtc_device *rtc, struct rtc_wkalrm *alarm);
extern int rtc_read_alarm(struct rtc_device *rtc,
			struct rtc_wkalrm *alrm);
extern int rtc_set_alarm(struct rtc_device *rtc,
				struct rtc_wkalrm *alrm);
extern int rtc_initialize_alarm(struct rtc_device *rtc,
				struct rtc_wkalrm *alrm);
extern void rtc_update_irq(struct rtc_device *rtc,
			unsigned long num, unsigned long events);

extern struct rtc_device *rtc_class_open(const char *name);
extern void rtc_class_close(struct rtc_device *rtc);

extern int rtc_irq_set_state(struct rtc_device *rtc, int enabled);
extern int rtc_irq_set_freq(struct rtc_device *rtc, int freq);
extern int rtc_update_irq_enable(struct rtc_device *rtc, unsigned int enabled);
extern int rtc_alarm_irq_enable(struct rtc_device *rtc, unsigned int enabled);
extern int rtc_dev_update_irq_enable_emul(struct rtc_device *rtc,
						unsigned int enabled);

void rtc_handle_legacy_irq(struct rtc_device *rtc, int num, int mode);
void rtc_aie_update_irq(struct rtc_device *rtc);
void rtc_uie_update_irq(struct rtc_device *rtc);
enum hrtimer_restart rtc_pie_update_irq(struct hrtimer *timer);

void rtc_timer_init(struct rtc_timer *timer, void (*f)(struct rtc_device *r),
		    struct rtc_device *rtc);
int rtc_timer_start(struct rtc_device *rtc, struct rtc_timer *timer,
		    ktime_t expires, ktime_t period);
void rtc_timer_cancel(struct rtc_device *rtc, struct rtc_timer *timer);
int rtc_read_offset(struct rtc_device *rtc, long *offset);
int rtc_set_offset(struct rtc_device *rtc, long offset);
void rtc_timer_do_work(struct work_struct *work);

static inline bool is_leap_year(unsigned int year)
{
	return (!(year % 4) && (year % 100)) || !(year % 400);
}

/* Determine if we can call to driver to set the time. Drivers can only be
 * called to set a second aligned time value, and the field set_offset_nsec
 * specifies how far away from the second aligned time to call the driver.
 *
 * This also computes 'to_set' which is the time we are trying to set, and has
 * a zero in tv_nsecs, such that:
 *    to_set - set_delay_nsec == now +/- FUZZ
 *
 */
static inline bool rtc_tv_nsec_ok(s64 set_offset_nsec,
				  struct timespec64 *to_set,
				  const struct timespec64 *now)
{
	/* Allowed error in tv_nsec, arbitarily set to 5 jiffies in ns. */
	const unsigned long TIME_SET_NSEC_FUZZ = TICK_NSEC * 5;
	struct timespec64 delay = {.tv_sec = 0,
				   .tv_nsec = set_offset_nsec};

	*to_set = timespec64_add(*now, delay);

	if (to_set->tv_nsec < TIME_SET_NSEC_FUZZ) {
		to_set->tv_nsec = 0;
		return true;
	}

	if (to_set->tv_nsec > NSEC_PER_SEC - TIME_SET_NSEC_FUZZ) {
		to_set->tv_sec++;
		to_set->tv_nsec = 0;
		return true;
	}
	return false;
}

#define rtc_register_device(device) \
	__rtc_register_device(THIS_MODULE, device)

#ifdef CONFIG_RTC_HCTOSYS_DEVICE
extern int rtc_hctosys_ret;
#else
#define rtc_hctosys_ret -ENODEV
#endif

#ifdef CONFIG_RTC_NVMEM
int rtc_nvmem_register(struct rtc_device *rtc,
		       struct nvmem_config *nvmem_config);
void rtc_nvmem_unregister(struct rtc_device *rtc);
#else
static inline int rtc_nvmem_register(struct rtc_device *rtc,
				     struct nvmem_config *nvmem_config)
{
	return 0;
}
static inline void rtc_nvmem_unregister(struct rtc_device *rtc) {}
#endif

#ifdef CONFIG_RTC_INTF_SYSFS
int rtc_add_group(struct rtc_device *rtc, const struct attribute_group *grp);
int rtc_add_groups(struct rtc_device *rtc, const struct attribute_group **grps);
#else
static inline
int rtc_add_group(struct rtc_device *rtc, const struct attribute_group *grp)
{
	return 0;
}

static inline
int rtc_add_groups(struct rtc_device *rtc, const struct attribute_group **grps)
{
	return 0;
}
#endif
#endif /* _LINUX_RTC_H_ */
