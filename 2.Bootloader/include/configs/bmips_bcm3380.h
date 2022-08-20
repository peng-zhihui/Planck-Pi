/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2017 Álvaro Fernández Rojas <noltari@gmail.com>
 */

#ifndef __CONFIG_BMIPS_BCM3380_H
#define __CONFIG_BMIPS_BCM3380_H

#include <linux/sizes.h>

/* CPU */
#define CONFIG_SYS_MIPS_TIMER_FREQ	166500000

/* RAM */
#define CONFIG_SYS_SDRAM_BASE		0x80000000

/* U-Boot */
#define CONFIG_SYS_LOAD_ADDR		CONFIG_SYS_SDRAM_BASE + SZ_1M

#if defined(CONFIG_BMIPS_BOOT_RAM)
#define CONFIG_SKIP_LOWLEVEL_INIT
#define CONFIG_SYS_INIT_SP_OFFSET	SZ_8K
#endif

#endif /* __CONFIG_BMIPS_BCM3380_H */
