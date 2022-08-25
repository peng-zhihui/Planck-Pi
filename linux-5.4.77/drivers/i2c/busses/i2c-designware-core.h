/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Synopsys DesignWare I2C adapter driver.
 *
 * Based on the TI DAVINCI I2C adapter driver.
 *
 * Copyright (C) 2006 Texas Instruments.
 * Copyright (C) 2007 MontaVista Software Inc.
 * Copyright (C) 2009 Provigent Ltd.
 */

#include <linux/i2c.h>

#define DW_IC_DEFAULT_FUNCTIONALITY (I2C_FUNC_I2C |			\
					I2C_FUNC_SMBUS_BYTE |		\
					I2C_FUNC_SMBUS_BYTE_DATA |	\
					I2C_FUNC_SMBUS_WORD_DATA |	\
					I2C_FUNC_SMBUS_BLOCK_DATA |	\
					I2C_FUNC_SMBUS_I2C_BLOCK)

#define DW_IC_CON_MASTER		0x1
#define DW_IC_CON_SPEED_STD		0x2
#define DW_IC_CON_SPEED_FAST		0x4
#define DW_IC_CON_SPEED_HIGH		0x6
#define DW_IC_CON_SPEED_MASK		0x6
#define DW_IC_CON_10BITADDR_SLAVE		0x8
#define DW_IC_CON_10BITADDR_MASTER	0x10
#define DW_IC_CON_RESTART_EN		0x20
#define DW_IC_CON_SLAVE_DISABLE		0x40
#define DW_IC_CON_STOP_DET_IFADDRESSED		0x80
#define DW_IC_CON_TX_EMPTY_CTRL		0x100
#define DW_IC_CON_RX_FIFO_FULL_HLD_CTRL		0x200

/*
 * Registers offset
 */
#define DW_IC_CON		0x0
#define DW_IC_TAR		0x4
#define DW_IC_SAR		0x8
#define DW_IC_DATA_CMD		0x10
#define DW_IC_SS_SCL_HCNT	0x14
#define DW_IC_SS_SCL_LCNT	0x18
#define DW_IC_FS_SCL_HCNT	0x1c
#define DW_IC_FS_SCL_LCNT	0x20
#define DW_IC_HS_SCL_HCNT	0x24
#define DW_IC_HS_SCL_LCNT	0x28
#define DW_IC_INTR_STAT		0x2c
#define DW_IC_INTR_MASK		0x30
#define DW_IC_RAW_INTR_STAT	0x34
#define DW_IC_RX_TL		0x38
#define DW_IC_TX_TL		0x3c
#define DW_IC_CLR_INTR		0x40
#define DW_IC_CLR_RX_UNDER	0x44
#define DW_IC_CLR_RX_OVER	0x48
#define DW_IC_CLR_TX_OVER	0x4c
#define DW_IC_CLR_RD_REQ	0x50
#define DW_IC_CLR_TX_ABRT	0x54
#define DW_IC_CLR_RX_DONE	0x58
#define DW_IC_CLR_ACTIVITY	0x5c
#define DW_IC_CLR_STOP_DET	0x60
#define DW_IC_CLR_START_DET	0x64
#define DW_IC_CLR_GEN_CALL	0x68
#define DW_IC_ENABLE		0x6c
#define DW_IC_STATUS		0x70
#define DW_IC_TXFLR		0x74
#define DW_IC_RXFLR		0x78
#define DW_IC_SDA_HOLD		0x7c
#define DW_IC_TX_ABRT_SOURCE	0x80
#define DW_IC_ENABLE_STATUS	0x9c
#define DW_IC_CLR_RESTART_DET	0xa8
#define DW_IC_COMP_PARAM_1	0xf4
#define DW_IC_COMP_VERSION	0xf8
#define DW_IC_SDA_HOLD_MIN_VERS	0x3131312A
#define DW_IC_COMP_TYPE		0xfc
#define DW_IC_COMP_TYPE_VALUE	0x44570140

#define DW_IC_INTR_RX_UNDER	0x001
#define DW_IC_INTR_RX_OVER	0x002
#define DW_IC_INTR_RX_FULL	0x004
#define DW_IC_INTR_TX_OVER	0x008
#define DW_IC_INTR_TX_EMPTY	0x010
#define DW_IC_INTR_RD_REQ	0x020
#define DW_IC_INTR_TX_ABRT	0x040
#define DW_IC_INTR_RX_DONE	0x080
#define DW_IC_INTR_ACTIVITY	0x100
#define DW_IC_INTR_STOP_DET	0x200
#define DW_IC_INTR_START_DET	0x400
#define DW_IC_INTR_GEN_CALL	0x800
#define DW_IC_INTR_RESTART_DET	0x1000

#define DW_IC_INTR_DEFAULT_MASK		(DW_IC_INTR_RX_FULL | \
					 DW_IC_INTR_TX_ABRT | \
					 DW_IC_INTR_STOP_DET)
#define DW_IC_INTR_MASTER_MASK		(DW_IC_INTR_DEFAULT_MASK | \
					 DW_IC_INTR_TX_EMPTY)
#define DW_IC_INTR_SLAVE_MASK		(DW_IC_INTR_DEFAULT_MASK | \
					 DW_IC_INTR_RX_DONE | \
					 DW_IC_INTR_RX_UNDER | \
					 DW_IC_INTR_RD_REQ)

#define DW_IC_STATUS_ACTIVITY		0x1
#define DW_IC_STATUS_TFE		BIT(2)
#define DW_IC_STATUS_MASTER_ACTIVITY	BIT(5)
#define DW_IC_STATUS_SLAVE_ACTIVITY	BIT(6)

#define DW_IC_SDA_HOLD_RX_SHIFT		16
#define DW_IC_SDA_HOLD_RX_MASK		GENMASK(23, DW_IC_SDA_HOLD_RX_SHIFT)

#define DW_IC_ERR_TX_ABRT	0x1

#define DW_IC_TAR_10BITADDR_MASTER BIT(12)

#define DW_IC_COMP_PARAM_1_SPEED_MODE_HIGH	(BIT(2) | BIT(3))
#define DW_IC_COMP_PARAM_1_SPEED_MODE_MASK	GENMASK(3, 2)

/*
 * status codes
 */
#define STATUS_IDLE			0x0
#define STATUS_WRITE_IN_PROGRESS	0x1
#define STATUS_READ_IN_PROGRESS		0x2

#define TIMEOUT			20 /* ms */

/*
 * operation modes
 */
#define DW_IC_MASTER		0
#define DW_IC_SLAVE		1

/*
 * Hardware abort codes from the DW_IC_TX_ABRT_SOURCE register
 *
 * Only expected abort codes are listed here
 * refer to the datasheet for the full list
 */
#define ABRT_7B_ADDR_NOACK	0
#define ABRT_10ADDR1_NOACK	1
#define ABRT_10ADDR2_NOACK	2
#define ABRT_TXDATA_NOACK	3
#define ABRT_GCALL_NOACK	4
#define ABRT_GCALL_READ		5
#define ABRT_SBYTE_ACKDET	7
#define ABRT_SBYTE_NORSTRT	9
#define ABRT_10B_RD_NORSTRT	10
#define ABRT_MASTER_DIS		11
#define ARB_LOST		12
#define ABRT_SLAVE_FLUSH_TXFIFO	13
#define ABRT_SLAVE_ARBLOST	14
#define ABRT_SLAVE_RD_INTX	15

#define DW_IC_TX_ABRT_7B_ADDR_NOACK	(1UL << ABRT_7B_ADDR_NOACK)
#define DW_IC_TX_ABRT_10ADDR1_NOACK	(1UL << ABRT_10ADDR1_NOACK)
#define DW_IC_TX_ABRT_10ADDR2_NOACK	(1UL << ABRT_10ADDR2_NOACK)
#define DW_IC_TX_ABRT_TXDATA_NOACK	(1UL << ABRT_TXDATA_NOACK)
#define DW_IC_TX_ABRT_GCALL_NOACK	(1UL << ABRT_GCALL_NOACK)
#define DW_IC_TX_ABRT_GCALL_READ	(1UL << ABRT_GCALL_READ)
#define DW_IC_TX_ABRT_SBYTE_ACKDET	(1UL << ABRT_SBYTE_ACKDET)
#define DW_IC_TX_ABRT_SBYTE_NORSTRT	(1UL << ABRT_SBYTE_NORSTRT)
#define DW_IC_TX_ABRT_10B_RD_NORSTRT	(1UL << ABRT_10B_RD_NORSTRT)
#define DW_IC_TX_ABRT_MASTER_DIS	(1UL << ABRT_MASTER_DIS)
#define DW_IC_TX_ARB_LOST		(1UL << ARB_LOST)
#define DW_IC_RX_ABRT_SLAVE_RD_INTX	(1UL << ABRT_SLAVE_RD_INTX)
#define DW_IC_RX_ABRT_SLAVE_ARBLOST	(1UL << ABRT_SLAVE_ARBLOST)
#define DW_IC_RX_ABRT_SLAVE_FLUSH_TXFIFO	(1UL << ABRT_SLAVE_FLUSH_TXFIFO)

#define DW_IC_TX_ABRT_NOACK		(DW_IC_TX_ABRT_7B_ADDR_NOACK | \
					 DW_IC_TX_ABRT_10ADDR1_NOACK | \
					 DW_IC_TX_ABRT_10ADDR2_NOACK | \
					 DW_IC_TX_ABRT_TXDATA_NOACK | \
					 DW_IC_TX_ABRT_GCALL_NOACK)


/**
 * struct dw_i2c_dev - private i2c-designware data
 * @dev: driver model device node
 * @base: IO registers pointer
 * @cmd_complete: tx completion indicator
 * @clk: input reference clock
 * @pclk: clock required to access the registers
 * @slave: represent an I2C slave device
 * @cmd_err: run time hadware error code
 * @msgs: points to an array of messages currently being transferred
 * @msgs_num: the number of elements in msgs
 * @msg_write_idx: the element index of the current tx message in the msgs
 *	array
 * @tx_buf_len: the length of the current tx buffer
 * @tx_buf: the current tx buffer
 * @msg_read_idx: the element index of the current rx message in the msgs
 *	array
 * @rx_buf_len: the length of the current rx buffer
 * @rx_buf: the current rx buffer
 * @msg_err: error status of the current transfer
 * @status: i2c master status, one of STATUS_*
 * @abort_source: copy of the TX_ABRT_SOURCE register
 * @irq: interrupt number for the i2c master
 * @adapter: i2c subsystem adapter node
 * @slave_cfg: configuration for the slave device
 * @tx_fifo_depth: depth of the hardware tx fifo
 * @rx_fifo_depth: depth of the hardware rx fifo
 * @rx_outstanding: current master-rx elements in tx fifo
 * @timings: bus clock frequency, SDA hold and other timings
 * @sda_hold_time: SDA hold value
 * @ss_hcnt: standard speed HCNT value
 * @ss_lcnt: standard speed LCNT value
 * @fs_hcnt: fast speed HCNT value
 * @fs_lcnt: fast speed LCNT value
 * @fp_hcnt: fast plus HCNT value
 * @fp_lcnt: fast plus LCNT value
 * @hs_hcnt: high speed HCNT value
 * @hs_lcnt: high speed LCNT value
 * @acquire_lock: function to acquire a hardware lock on the bus
 * @release_lock: function to release a hardware lock on the bus
 * @shared_with_punit: true if this bus is shared with the SoCs PUNIT
 * @disable: function to disable the controller
 * @disable_int: function to disable all interrupts
 * @init: function to initialize the I2C hardware
 * @mode: operation mode - DW_IC_MASTER or DW_IC_SLAVE
 * @suspended: set to true if the controller is suspended
 *
 * HCNT and LCNT parameters can be used if the platform knows more accurate
 * values than the one computed based only on the input clock frequency.
 * Leave them to be %0 if not used.
 */
struct dw_i2c_dev {
	struct device		*dev;
	void __iomem		*base;
	void __iomem		*ext;
	struct completion	cmd_complete;
	struct clk		*clk;
	struct clk		*pclk;
	struct reset_control	*rst;
	struct i2c_client		*slave;
	u32			(*get_clk_rate_khz) (struct dw_i2c_dev *dev);
	struct dw_pci_controller *controller;
	int			cmd_err;
	struct i2c_msg		*msgs;
	int			msgs_num;
	int			msg_write_idx;
	u32			tx_buf_len;
	u8			*tx_buf;
	int			msg_read_idx;
	u32			rx_buf_len;
	u8			*rx_buf;
	int			msg_err;
	unsigned int		status;
	u32			abort_source;
	int			irq;
	u32			flags;
	struct i2c_adapter	adapter;
	u32			functionality;
	u32			master_cfg;
	u32			slave_cfg;
	unsigned int		tx_fifo_depth;
	unsigned int		rx_fifo_depth;
	int			rx_outstanding;
	struct i2c_timings	timings;
	u32			sda_hold_time;
	u16			ss_hcnt;
	u16			ss_lcnt;
	u16			fs_hcnt;
	u16			fs_lcnt;
	u16			fp_hcnt;
	u16			fp_lcnt;
	u16			hs_hcnt;
	u16			hs_lcnt;
	int			(*acquire_lock)(void);
	void			(*release_lock)(void);
	bool			shared_with_punit;
	void			(*disable)(struct dw_i2c_dev *dev);
	void			(*disable_int)(struct dw_i2c_dev *dev);
	int			(*init)(struct dw_i2c_dev *dev);
	int			(*set_sda_hold_time)(struct dw_i2c_dev *dev);
	int			mode;
	struct i2c_bus_recovery_info rinfo;
	bool			suspended;
};

#define ACCESS_SWAP		0x00000001
#define ACCESS_16BIT		0x00000002
#define ACCESS_INTR_MASK	0x00000004
#define ACCESS_NO_IRQ_SUSPEND	0x00000008

#define MODEL_CHERRYTRAIL	0x00000100
#define MODEL_MSCC_OCELOT	0x00000200
#define MODEL_MASK		0x00000f00

u32 dw_readl(struct dw_i2c_dev *dev, int offset);
void dw_writel(struct dw_i2c_dev *dev, u32 b, int offset);
int i2c_dw_set_reg_access(struct dw_i2c_dev *dev);
u32 i2c_dw_scl_hcnt(u32 ic_clk, u32 tSYMBOL, u32 tf, int cond, int offset);
u32 i2c_dw_scl_lcnt(u32 ic_clk, u32 tLOW, u32 tf, int offset);
int i2c_dw_set_sda_hold(struct dw_i2c_dev *dev);
unsigned long i2c_dw_clk_rate(struct dw_i2c_dev *dev);
int i2c_dw_prepare_clk(struct dw_i2c_dev *dev, bool prepare);
int i2c_dw_acquire_lock(struct dw_i2c_dev *dev);
void i2c_dw_release_lock(struct dw_i2c_dev *dev);
int i2c_dw_wait_bus_not_busy(struct dw_i2c_dev *dev);
int i2c_dw_handle_tx_abort(struct dw_i2c_dev *dev);
u32 i2c_dw_func(struct i2c_adapter *adap);
void i2c_dw_disable(struct dw_i2c_dev *dev);
void i2c_dw_disable_int(struct dw_i2c_dev *dev);

static inline void __i2c_dw_enable(struct dw_i2c_dev *dev)
{
	dw_writel(dev, 1, DW_IC_ENABLE);
}

static inline void __i2c_dw_disable_nowait(struct dw_i2c_dev *dev)
{
	dw_writel(dev, 0, DW_IC_ENABLE);
}

void __i2c_dw_disable(struct dw_i2c_dev *dev);

extern u32 i2c_dw_read_comp_param(struct dw_i2c_dev *dev);
extern int i2c_dw_probe(struct dw_i2c_dev *dev);
#if IS_ENABLED(CONFIG_I2C_DESIGNWARE_SLAVE)
extern int i2c_dw_probe_slave(struct dw_i2c_dev *dev);
#else
static inline int i2c_dw_probe_slave(struct dw_i2c_dev *dev) { return -EINVAL; }
#endif

#if IS_ENABLED(CONFIG_I2C_DESIGNWARE_BAYTRAIL)
extern int i2c_dw_probe_lock_support(struct dw_i2c_dev *dev);
#else
static inline int i2c_dw_probe_lock_support(struct dw_i2c_dev *dev) { return 0; }
#endif
