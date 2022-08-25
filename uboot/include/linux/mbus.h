/*
 * Marvell MBUS common definitions.
 *
 * Copyright (C) 2008 Marvell Semiconductor
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __LINUX_MBUS_H
#define __LINUX_MBUS_H

struct resource;

struct mbus_dram_target_info {
	/*
	 * The 4-bit MBUS target ID of the DRAM controller.
	 */
	u8		mbus_dram_target_id;

	/*
	 * The base address, size, and MBUS attribute ID for each
	 * of the possible DRAM chip selects.  Peripherals are
	 * required to support at least 4 decode windows.
	 */
	int		num_cs;
	struct mbus_dram_window {
		u8	cs_index;
		u8	mbus_attr;
		u32	base;
		u32	size;
	} cs[4];
};

struct mvebu_mbus_state {
	void __iomem *mbuswins_base;
	void __iomem *sdramwins_base;
	struct dentry *debugfs_root;
	struct dentry *debugfs_sdram;
	struct dentry *debugfs_devs;
	const struct mvebu_mbus_soc_data *soc;
	int hw_io_coherency;
};

/* Flags for PCI/PCIe address decoding regions */
#define MVEBU_MBUS_PCI_IO  0x1
#define MVEBU_MBUS_PCI_MEM 0x2
#define MVEBU_MBUS_PCI_WA  0x3

/*
 * Magic value that explicits that we don't need a remapping-capable
 * address decoding window.
 */
#define MVEBU_MBUS_NO_REMAP (0xffffffff)

/* Maximum size of a mbus window name */
#define MVEBU_MBUS_MAX_WINNAME_SZ 32

const struct mbus_dram_target_info *mvebu_mbus_dram_info(void);
void mvebu_mbus_get_pcie_mem_aperture(struct resource *res);
void mvebu_mbus_get_pcie_io_aperture(struct resource *res);
int mvebu_mbus_add_window_remap_by_id(unsigned int target,
				      unsigned int attribute,
				      phys_addr_t base, size_t size,
				      phys_addr_t remap);
int mvebu_mbus_add_window_by_id(unsigned int target, unsigned int attribute,
				phys_addr_t base, size_t size);
int mvebu_mbus_del_window(phys_addr_t base, size_t size);
int mbus_dt_setup_win(struct mvebu_mbus_state *mbus,
		      u32 base, u32 size, u8 target, u8 attr);

#endif /* __LINUX_MBUS_H */
