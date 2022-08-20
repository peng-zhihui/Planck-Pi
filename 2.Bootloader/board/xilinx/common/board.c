// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2014 - 2019 Xilinx, Inc.
 * Michal Simek <michal.simek@xilinx.com>
 */

#include <common.h>
#include <env.h>
#include <log.h>
#include <asm/sections.h>
#include <dm/uclass.h>
#include <i2c.h>
#include <linux/sizes.h>
#include "board.h"

int zynq_board_read_rom_ethaddr(unsigned char *ethaddr)
{
	int ret = -EINVAL;

#if defined(CONFIG_ZYNQ_GEM_I2C_MAC_OFFSET)
	struct udevice *dev;
	ofnode eeprom;

	eeprom = ofnode_get_chosen_node("xlnx,eeprom");
	if (!ofnode_valid(eeprom))
		return -ENODEV;

	debug("%s: Path to EEPROM %s\n", __func__,
	      ofnode_read_chosen_string("xlnx,eeprom"));

	ret = uclass_get_device_by_ofnode(UCLASS_I2C_EEPROM, eeprom, &dev);
	if (ret)
		return ret;

	ret = dm_i2c_read(dev, CONFIG_ZYNQ_GEM_I2C_MAC_OFFSET, ethaddr, 6);
	if (ret)
		debug("%s: I2C EEPROM MAC address read failed\n", __func__);
	else
		debug("%s: I2C EEPROM MAC %pM\n", __func__, ethaddr);
#endif

	return ret;
}

#if defined(CONFIG_OF_BOARD) || defined(CONFIG_OF_SEPARATE)
void *board_fdt_blob_setup(void)
{
	static void *fdt_blob;

#if !defined(CONFIG_VERSAL_NO_DDR) && !defined(CONFIG_ZYNQMP_NO_DDR)
	fdt_blob = (void *)CONFIG_XILINX_OF_BOARD_DTB_ADDR;

	if (fdt_magic(fdt_blob) == FDT_MAGIC)
		return fdt_blob;

	debug("DTB is not passed via %p\n", fdt_blob);
#endif

#ifdef CONFIG_SPL_BUILD
	/* FDT is at end of BSS unless it is in a different memory region */
	if (IS_ENABLED(CONFIG_SPL_SEPARATE_BSS))
		fdt_blob = (ulong *)&_image_binary_end;
	else
		fdt_blob = (ulong *)&__bss_end;
#else
	/* FDT is at end of image */
	fdt_blob = (ulong *)&_end;
#endif

	if (fdt_magic(fdt_blob) == FDT_MAGIC)
		return fdt_blob;

	debug("DTB is also not passed via %p\n", fdt_blob);

	return NULL;
}
#endif

int board_late_init_xilinx(void)
{
	ulong initrd_hi;

	env_set_hex("script_offset_f", CONFIG_BOOT_SCRIPT_OFFSET);

	initrd_hi = gd->start_addr_sp - CONFIG_STACK_SIZE;
	initrd_hi = round_down(initrd_hi, SZ_16M);
	env_set_addr("initrd_high", (void *)initrd_hi);

	return 0;
}
