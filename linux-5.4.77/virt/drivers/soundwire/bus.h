/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/* Copyright(c) 2015-17 Intel Corporation. */

#ifndef __SDW_BUS_H
#define __SDW_BUS_H

#define DEFAULT_BANK_SWITCH_TIMEOUT 3000

#if IS_ENABLED(CONFIG_ACPI)
int sdw_acpi_find_slaves(struct sdw_bus *bus);
#else
static inline int sdw_acpi_find_slaves(struct sdw_bus *bus)
{
	return -ENOTSUPP;
}
#endif

int sdw_of_find_slaves(struct sdw_bus *bus);
void sdw_extract_slave_id(struct sdw_bus *bus,
			  u64 addr, struct sdw_slave_id *id);

#ifdef CONFIG_DEBUG_FS
void sdw_bus_debugfs_init(struct sdw_bus *bus);
void sdw_bus_debugfs_exit(struct sdw_bus *bus);
void sdw_slave_debugfs_init(struct sdw_slave *slave);
void sdw_slave_debugfs_exit(struct sdw_slave *slave);
void sdw_debugfs_init(void);
void sdw_debugfs_exit(void);
#else
static inline void sdw_bus_debugfs_init(struct sdw_bus *bus) {}
static inline void sdw_bus_debugfs_exit(struct sdw_bus *bus) {}
static inline void sdw_slave_debugfs_init(struct sdw_slave *slave) {}
static inline void sdw_slave_debugfs_exit(struct sdw_slave *slave) {}
static inline void sdw_debugfs_init(void) {}
static inline void sdw_debugfs_exit(void) {}
#endif

enum {
	SDW_MSG_FLAG_READ = 0,
	SDW_MSG_FLAG_WRITE,
};

/**
 * struct sdw_msg - Message structure
 * @addr: Register address accessed in the Slave
 * @len: number of messages
 * @dev_num: Slave device number
 * @addr_page1: SCP address page 1 Slave register
 * @addr_page2: SCP address page 2 Slave register
 * @flags: transfer flags, indicate if xfer is read or write
 * @buf: message data buffer
 * @ssp_sync: Send message at SSP (Stream Synchronization Point)
 * @page: address requires paging
 */
struct sdw_msg {
	u16 addr;
	u16 len;
	u8 dev_num;
	u8 addr_page1;
	u8 addr_page2;
	u8 flags;
	u8 *buf;
	bool ssp_sync;
	bool page;
};

#define SDW_DOUBLE_RATE_FACTOR		2

extern int sdw_rows[SDW_FRAME_ROWS];
extern int sdw_cols[SDW_FRAME_COLS];

int sdw_find_row_index(int row);
int sdw_find_col_index(int col);

/**
 * sdw_port_runtime: Runtime port parameters for Master or Slave
 *
 * @num: Port number. For audio streams, valid port number ranges from
 * [1,14]
 * @ch_mask: Channel mask
 * @transport_params: Transport parameters
 * @port_params: Port parameters
 * @port_node: List node for Master or Slave port_list
 *
 * SoundWire spec has no mention of ports for Master interface but the
 * concept is logically extended.
 */
struct sdw_port_runtime {
	int num;
	int ch_mask;
	struct sdw_transport_params transport_params;
	struct sdw_port_params port_params;
	struct list_head port_node;
};

/**
 * sdw_slave_runtime: Runtime Stream parameters for Slave
 *
 * @slave: Slave handle
 * @direction: Data direction for Slave
 * @ch_count: Number of channels handled by the Slave for
 * this stream
 * @m_rt_node: sdw_master_runtime list node
 * @port_list: List of Slave Ports configured for this stream
 */
struct sdw_slave_runtime {
	struct sdw_slave *slave;
	enum sdw_data_direction direction;
	unsigned int ch_count;
	struct list_head m_rt_node;
	struct list_head port_list;
};

/**
 * sdw_master_runtime: Runtime stream parameters for Master
 *
 * @bus: Bus handle
 * @stream: Stream runtime handle
 * @direction: Data direction for Master
 * @ch_count: Number of channels handled by the Master for
 * this stream, can be zero.
 * @slave_rt_list: Slave runtime list
 * @port_list: List of Master Ports configured for this stream, can be zero.
 * @stream_node: sdw_stream_runtime master_list node
 * @bus_node: sdw_bus m_rt_list node
 */
struct sdw_master_runtime {
	struct sdw_bus *bus;
	struct sdw_stream_runtime *stream;
	enum sdw_data_direction direction;
	unsigned int ch_count;
	struct list_head slave_rt_list;
	struct list_head port_list;
	struct list_head stream_node;
	struct list_head bus_node;
};

struct sdw_dpn_prop *sdw_get_slave_dpn_prop(struct sdw_slave *slave,
					    enum sdw_data_direction direction,
					    unsigned int port_num);
int sdw_configure_dpn_intr(struct sdw_slave *slave, int port,
			   bool enable, int mask);

int sdw_transfer(struct sdw_bus *bus, struct sdw_msg *msg);
int sdw_transfer_defer(struct sdw_bus *bus, struct sdw_msg *msg,
		       struct sdw_defer *defer);

#define SDW_READ_INTR_CLEAR_RETRY	10

int sdw_fill_msg(struct sdw_msg *msg, struct sdw_slave *slave,
		 u32 addr, size_t count, u16 dev_num, u8 flags, u8 *buf);

/* Read-Modify-Write Slave register */
static inline int
sdw_update(struct sdw_slave *slave, u32 addr, u8 mask, u8 val)
{
	int tmp;

	tmp = sdw_read(slave, addr);
	if (tmp < 0)
		return tmp;

	tmp = (tmp & ~mask) | val;
	return sdw_write(slave, addr, tmp);
}

#endif /* __SDW_BUS_H */
