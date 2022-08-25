// SPDX-License-Identifier: GPL-2.0
#ifndef _CEDAR_VE_PRIV_H_
#define _CEDAR_VE_PRIV_H_
#include "ve_mem_list.h"

#ifndef CEDARDEV_MAJOR
#define CEDARDEV_MAJOR (150)
#endif
#ifndef CEDARDEV_MINOR
#define CEDARDEV_MINOR (0)
#endif

#define VE_CLK_HIGH_WATER (900)
#define VE_CLK_LOW_WATER (100)

#define PRINTK_IOMMU_ADDR 0

#define VE_DEBUGFS_MAX_CHANNEL 16
#define VE_DEBUGFS_BUF_SIZE 1024

#define CEDAR_RUN_LIST_NONULL -1
#define CEDAR_NONBLOCK_TASK 0
#define CEDAR_BLOCK_TASK 1
#define CLK_REL_TIME 10000
#define TIMER_CIRCLE 50
#define TASK_INIT 0x00
#define TASK_TIMEOUT 0x55
#define TASK_RELEASE 0xaa
#define SIG_CEDAR 35

struct ve_debugfs_proc
{
	unsigned int len;
	char data[VE_DEBUGFS_BUF_SIZE * VE_DEBUGFS_MAX_CHANNEL];
};

struct ve_debugfs_buffer
{
	unsigned char cur_channel_id;
	unsigned int proc_len[VE_DEBUGFS_MAX_CHANNEL];
	char *proc_buf[VE_DEBUGFS_MAX_CHANNEL];
	char *data;
	struct mutex lock_proc;
};

struct __cedarv_task
{
	int task_prio;
	int ID;
	unsigned long timeout;
	unsigned int frametime;
	unsigned int block_mode;
};

struct cedarv_engine_task
{
	struct __cedarv_task t;
	struct list_head list;
	struct task_struct *task_handle;
	unsigned int status;
	unsigned int running;
	unsigned int is_first_task;
};

struct cedarv_engine_task_info
{
	int task_prio;
	unsigned int frametime;
	unsigned int total_time;
};

struct cedarv_regop
{
	unsigned long addr;
	unsigned int value;
};

struct cedarv_env_infomation_compat
{
	unsigned int phymem_start;
	int phymem_total_size;
	uint32_t address_macc;
};

struct __cedarv_task_compat
{
	int task_prio;
	int ID;
	uint32_t timeout;
	unsigned int frametime;
	unsigned int block_mode;
};

struct cedarv_regop_compat
{
	uint32_t addr;
	unsigned int value;
};

struct VE_PROC_INFO
{
	unsigned char channel_id;
	unsigned int proc_info_len;
};

struct cedar_dev
{
	struct cdev cdev;			 /* char device struct*/
	struct device *dev;			 /* ptr to class device struct*/
	struct device *platform_dev; /* ptr to class device struct */
	struct class *class;		 /* class for auto create device node */

	struct semaphore sem; /* mutual exclusion semaphore */

	wait_queue_head_t wq; /* wait queue for poll ops */

	struct timer_list cedar_engine_timer;
	struct timer_list cedar_engine_timer_rel;

	uint32_t irq;		   /* cedar video engine irq number */
	uint32_t de_irq_flag;  /* flag of video decoder engine irq generated */
	uint32_t de_irq_value; /* value of video decoder engine irq */
	uint32_t en_irq_flag;  /* flag of video encoder engine irq generated */
	uint32_t en_irq_value; /* value of video encoder engine irq */
	uint32_t irq_has_enable;
	uint32_t ref_count;
	int last_min_freq;

	uint32_t jpeg_irq_flag;	 /* flag of video jpeg dec irq generated */
	uint32_t jpeg_irq_value; /* value of video jpeg dec  irq */

	struct mutex lock_vdec;
	struct mutex lock_jdec;
	struct mutex lock_venc;
	struct mutex lock_00_reg;
	struct mutex lock_04_reg;
	struct aw_mem_list_head list; /* buffer list */
	struct mutex lock_mem;

	struct clk *ahb_clk;
	struct clk *mod_clk;
	struct clk *ram_clk;
	struct reset_control *rstc;
	int capabilities;
	phys_addr_t phy_addr;

	void __iomem *regs_macc;
};

struct ve_info
{ /* each object will bind a new file handler */
	unsigned int set_vol_flag;
	struct mutex lock_flag_io;
	uint32_t lock_flags; /* if flags is 0, means unlock status */
};

struct user_iommu_param
{
	int fd;
	unsigned int iommu_addr;
};

struct cedarv_iommu_buffer
{
	struct aw_mem_list_head i_list;
	int fd;
	unsigned long iommu_addr;
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attachment;
	struct sg_table *sgt;
	int p_id;
};

struct cedar_variant
{
	int capabilities;
	unsigned long mod_rate;
};

#define CEDARV_ISP_OLD (1 << 0)
#define CEDARV_ISP_NEW (1 << 1)

#endif