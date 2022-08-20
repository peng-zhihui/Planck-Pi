/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2015-2016 Wills Wang <wills.wang@live.com>
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#define CONFIG_SYS_HZ                   1000
#define CONFIG_SYS_MHZ                  325
#define CONFIG_SYS_MIPS_TIMER_FREQ      (CONFIG_SYS_MHZ * 1000000)

#define CONFIG_SYS_MONITOR_BASE         CONFIG_SYS_TEXT_BASE

#define CONFIG_SYS_MALLOC_LEN           0x40000
#define CONFIG_SYS_BOOTPARAMS_LEN       0x20000

#define CONFIG_SYS_SDRAM_BASE           0x80000000
#define CONFIG_SYS_LOAD_ADDR            0x81000000

#define CONFIG_SYS_INIT_RAM_ADDR        0xbd000000
#define CONFIG_SYS_INIT_RAM_SIZE        0x2000
#define CONFIG_SYS_INIT_SP_ADDR \
	(CONFIG_SYS_INIT_RAM_ADDR + CONFIG_SYS_INIT_RAM_SIZE - 1)

/*
 * Serial Port
 */
#define CONFIG_SYS_NS16550_CLK          25000000
#define CONFIG_SYS_BAUDRATE_TABLE \
	{9600, 19200, 38400, 57600, 115200}

#define CONFIG_BOOTCOMMAND              "sf probe;" \
					"mtdparts default;" \
					"bootm 0x9f680000"

/* Miscellaneous configurable options */

/*
 * Diagnostics
 */

#endif  /* __CONFIG_H */
