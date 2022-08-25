// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2013-2015 Arcturus Networks, Inc.
 *           http://www.arcturusnetworks.com/products/ucp1020/
 * based on board/freescale/p1_p2_rdb_pc/spl.c
 * original copyright follows:
 * Copyright 2013 Freescale Semiconductor, Inc.
 */

#include <common.h>
#include <clock_legacy.h>
#include <console.h>
#include <env.h>
#include <env_internal.h>
#include <init.h>
#include <ns16550.h>
#include <malloc.h>
#include <mmc.h>
#include <nand.h>
#include <i2c.h>
#include <fsl_esdhc.h>
#include <spi_flash.h>

DECLARE_GLOBAL_DATA_PTR;

static const u32 sysclk_tbl[] = {
	66666000, 7499900, 83332500, 8999900,
	99999000, 11111000, 12499800, 13333200
};

phys_size_t get_effective_memsize(void)
{
	return CONFIG_SYS_L2_SIZE;
}

void board_init_f(ulong bootflag)
{
	u32 plat_ratio, bus_clk;
	ccsr_gur_t *gur = (void *)CONFIG_SYS_MPC85xx_GUTS_ADDR;

	console_init_f();

	/* Set pmuxcr to allow both i2c1 and i2c2 */
	setbits_be32(&gur->pmuxcr, in_be32(&gur->pmuxcr) | 0x1000);
	setbits_be32(&gur->pmuxcr,
		     in_be32(&gur->pmuxcr) | MPC85xx_PMUXCR_SD_DATA);

	/* Read back the register to synchronize the write. */
	in_be32(&gur->pmuxcr);

#ifdef CONFIG_SPL_SPI_BOOT
	clrbits_be32(&gur->pmuxcr, MPC85xx_PMUXCR_SD_DATA);
#endif

	/* initialize selected port with appropriate baud rate */
	plat_ratio = in_be32(&gur->porpllsr) & MPC85xx_PORPLLSR_PLAT_RATIO;
	plat_ratio >>= 1;
	bus_clk = CONFIG_SYS_CLK_FREQ * plat_ratio;
	gd->bus_clk = bus_clk;

	NS16550_init((NS16550_t)CONFIG_SYS_NS16550_COM1,
		     bus_clk / 16 / CONFIG_BAUDRATE);
#ifdef CONFIG_SPL_MMC_BOOT
	puts("\nSD boot...\n");
#elif defined(CONFIG_SPL_SPI_BOOT)
	puts("\nSPI Flash boot...\n");
#endif

	/* copy code to RAM and jump to it - this should not return */
	/* NOTE - code has to be copied out of NAND buffer before
	 * other blocks can be read.
	 */
	relocate_code(CONFIG_SPL_RELOC_STACK, 0, CONFIG_SPL_RELOC_TEXT_BASE);
}

void board_init_r(gd_t *gd, ulong dest_addr)
{
	/* Pointer is writable since we allocated a register for it */
	gd = (gd_t *)CONFIG_SPL_GD_ADDR;
	bd_t *bd;

	memset(gd, 0, sizeof(gd_t));
	bd = (bd_t *)(CONFIG_SPL_GD_ADDR + sizeof(gd_t));
	memset(bd, 0, sizeof(bd_t));
	gd->bd = bd;
	bd->bi_memstart = CONFIG_SYS_INIT_L2_ADDR;
	bd->bi_memsize = CONFIG_SYS_L2_SIZE;

	arch_cpu_init();
	get_clocks();
	mem_malloc_init(CONFIG_SPL_RELOC_MALLOC_ADDR,
			CONFIG_SPL_RELOC_MALLOC_SIZE);

#ifndef CONFIG_SPL_NAND_BOOT
	env_init();
#endif
#ifdef CONFIG_SPL_MMC_BOOT
	mmc_initialize(bd);
#endif
	/* relocate environment function pointers etc. */
#ifdef CONFIG_SPL_NAND_BOOT
	nand_spl_load_image(CONFIG_ENV_OFFSET, CONFIG_ENV_SIZE,
			    (uchar *)CONFIG_ENV_ADDR);
	gd->env_addr = (ulong)(CONFIG_ENV_ADDR);
	gd->env_valid = ENV_VALID;
#else
	env_relocate();
#endif

#ifdef CONFIG_SYS_I2C
	i2c_init_all();
#else
	i2c_init(CONFIG_SYS_I2C_SPEED, CONFIG_SYS_I2C_SLAVE);
#endif

	dram_init();
#ifdef CONFIG_SPL_NAND_BOOT
	puts("Tertiary program loader running in sram...");
#else
	puts("Second program loader running in sram...\n");
#endif

#ifdef CONFIG_SPL_MMC_BOOT
	mmc_boot();
#elif defined(CONFIG_SPL_NAND_BOOT)
	nand_boot();
#endif
}
