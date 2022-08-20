// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2007-2008
 * Stelian Pop <stelian@popies.net>
 * Lead Tech Design <www.leadtechdesign.com>
 * Copyright (C) 2008 Ronetix Ilko Iliev (www.ronetix.at)
 * Copyright (C) 2009 Jean-Christophe PLAGNIOL-VILLARD <plagnioj@jcrosoft.com>
 */

#include <common.h>
#include <init.h>
#include <linux/sizes.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <asm/arch/at91sam9_smc.h>
#include <asm/arch/at91_common.h>
#include <asm/arch/at91_rstc.h>
#include <asm/arch/at91_matrix.h>
#include <asm/arch/clk.h>
#include <asm/arch/gpio.h>
#if defined(CONFIG_RESET_PHY_R) && defined(CONFIG_MACB)
#include <net.h>
#endif
#include <netdev.h>
#include <asm/mach-types.h>

DECLARE_GLOBAL_DATA_PTR;

/* ------------------------------------------------------------------------- */
/*
 * Miscelaneous platform dependent initialisations
 */

#ifdef CONFIG_CMD_NAND
static void pm9263_nand_hw_init(void)
{
	unsigned long csa;
	struct at91_smc *smc = (struct at91_smc *)ATMEL_BASE_SMC0;
	struct at91_matrix *matrix = (struct at91_matrix *)ATMEL_BASE_MATRIX;

	/* Enable CS3 */
	csa = readl(&matrix->csa[0]) | AT91_MATRIX_CSA_EBI_CS3A;
	writel(csa, &matrix->csa[0]);

	/* Configure SMC CS3 for NAND/SmartMedia */
	writel(AT91_SMC_SETUP_NWE(1) | AT91_SMC_SETUP_NCS_WR(1) |
		AT91_SMC_SETUP_NRD(1) | AT91_SMC_SETUP_NCS_RD(1),
		&smc->cs[3].setup);

	writel(AT91_SMC_PULSE_NWE(3) | AT91_SMC_PULSE_NCS_WR(3) |
		AT91_SMC_PULSE_NRD(3) | AT91_SMC_PULSE_NCS_RD(3),
		&smc->cs[3].pulse);

	writel(AT91_SMC_CYCLE_NWE(5) | AT91_SMC_CYCLE_NRD(5),
		&smc->cs[3].cycle);

	writel(AT91_SMC_MODE_RM_NRD | AT91_SMC_MODE_WM_NWE |
		AT91_SMC_MODE_EXNW_DISABLE |
#ifdef CONFIG_SYS_NAND_DBW_16
		AT91_SMC_MODE_DBW_16 |
#else /* CONFIG_SYS_NAND_DBW_8 */
		AT91_SMC_MODE_DBW_8 |
#endif
		AT91_SMC_MODE_TDF_CYCLE(2),
		&smc->cs[3].mode);

	/* Configure RDY/BSY */
	gpio_direction_input(CONFIG_SYS_NAND_READY_PIN);

	/* Enable NandFlash */
	gpio_direction_output(CONFIG_SYS_NAND_ENABLE_PIN, 1);
}
#endif

#ifdef CONFIG_MACB
static void pm9263_macb_hw_init(void)
{
	/*
	 * PB27 enables the 50MHz oscillator for Ethernet PHY
	 * 1 - enable
	 * 0 - disable
	 */
	at91_set_pio_output(AT91_PIO_PORTB, 27, 1);
	at91_set_pio_value(AT91_PIO_PORTB, 27, 1); /* 1- enable, 0 - disable */

	at91_periph_clk_enable(ATMEL_ID_EMAC);

	/*
	 * Disable pull-up on:
	 *	RXDV (PC25) => PHY normal mode (not Test mode)
	 *	ERX0 (PE25) => PHY ADDR0
	 *	ERX1 (PE26) => PHY ADDR1 => PHYADDR = 0x0
	 *
	 * PHY has internal pull-down
	 */

	at91_set_pio_pullup(AT91_PIO_PORTC, 25, 0);
	at91_set_pio_pullup(AT91_PIO_PORTE, 25, 0);
	at91_set_pio_pullup(AT91_PIO_PORTE, 26, 0);

	/* Re-enable pull-up */
	at91_set_pio_pullup(AT91_PIO_PORTC, 25, 1);
	at91_set_pio_pullup(AT91_PIO_PORTE, 25, 1);
	at91_set_pio_pullup(AT91_PIO_PORTE, 26, 1);

	at91_macb_hw_init();
}
#endif

#ifdef CONFIG_LCD

#ifdef CONFIG_LCD_IN_PSRAM

#define PSRAM_CRE_PIN	AT91_PIO_PORTB, 29
#define PSRAM_CTRL_REG	(PHYS_PSRAM + PHYS_PSRAM_SIZE - 2)

/* Initialize the PSRAM memory */
static int pm9263_lcd_hw_psram_init(void)
{
	unsigned long csa;
	struct at91_smc *smc = (struct at91_smc *)ATMEL_BASE_SMC1;
	struct at91_matrix *matrix = (struct at91_matrix *)ATMEL_BASE_MATRIX;

	/* Enable CS3  3.3v, no pull-ups */
	csa = readl(&matrix->csa[1]) | AT91_MATRIX_CSA_DBPUC |
		AT91_MATRIX_CSA_VDDIOMSEL_3_3V;

	writel(csa, &matrix->csa[1]);

	/* Configure SMC1 CS0 for PSRAM - 16-bit */
	writel(AT91_SMC_SETUP_NWE(0) | AT91_SMC_SETUP_NCS_WR(0) |
		AT91_SMC_SETUP_NRD(0) | AT91_SMC_SETUP_NCS_RD(0),
		&smc->cs[0].setup);

	writel(AT91_SMC_PULSE_NWE(7) | AT91_SMC_PULSE_NCS_WR(7) |
		AT91_SMC_PULSE_NRD(2) | AT91_SMC_PULSE_NCS_RD(7),
		&smc->cs[0].pulse);

	writel(AT91_SMC_CYCLE_NWE(8) | AT91_SMC_CYCLE_NRD(8),
		&smc->cs[0].cycle);

	writel(AT91_SMC_MODE_DBW_16 | AT91_SMC_MODE_PMEN | AT91_SMC_MODE_PS_32,
		&smc->cs[0].mode);

	/* setup PB29 as output */
	at91_set_pio_output(PSRAM_CRE_PIN, 1);

	at91_set_pio_value(PSRAM_CRE_PIN, 0);	/* set PSRAM_CRE_PIN to '0' */

	/* PSRAM: write BCR */
	readw(PSRAM_CTRL_REG);
	readw(PSRAM_CTRL_REG);
	writew(1, PSRAM_CTRL_REG);	/* 0 - RCR,1 - BCR */
	writew(0x9d4f, PSRAM_CTRL_REG);	/* write the BCR */

	/* write RCR of the PSRAM */
	readw(PSRAM_CTRL_REG);
	readw(PSRAM_CTRL_REG);
	writew(0, PSRAM_CTRL_REG);	/* 0 - RCR,1 - BCR */
	/* set RCR; 0x10-async mode,0x90-page mode */
	writew(0x90, PSRAM_CTRL_REG);

	/*
	 * test to see if the PSRAM is MT45W2M16A or MT45W2M16B
	 * MT45W2M16B - CRE must be 0
	 * MT45W2M16A - CRE must be 1
	 */
	writew(0x1234, PHYS_PSRAM);
	writew(0x5678, PHYS_PSRAM + 2);

	/* test if the chip is MT45W2M16B */
	if ((readw(PHYS_PSRAM) != 0x1234) || (readw(PHYS_PSRAM+2) != 0x5678)) {
		/* try with CRE=1 (MT45W2M16A) */
		at91_set_pio_value(PSRAM_CRE_PIN, 1); /* set PSRAM_CRE_PIN to '1' */

		/* write RCR of the PSRAM */
		readw(PSRAM_CTRL_REG);
		readw(PSRAM_CTRL_REG);
		writew(0, PSRAM_CTRL_REG);	/* 0 - RCR,1 - BCR */
		/* set RCR;0x10-async mode,0x90-page mode */
		writew(0x90, PSRAM_CTRL_REG);


		writew(0x1234, PHYS_PSRAM);
		writew(0x5678, PHYS_PSRAM+2);
		if ((readw(PHYS_PSRAM) != 0x1234)
		  || (readw(PHYS_PSRAM + 2) != 0x5678))
			return 1;

	}

	/* Bus matrix */
	writel(AT91_MATRIX_PRA_M5(3), &matrix->pr[5].a);
	writel(CONFIG_PSRAM_SCFG, &matrix->scfg[5]);

	return 0;
}
#endif

static void pm9263_lcd_hw_init(void)
{
	/* Power Control */
	at91_set_pio_output(AT91_PIO_PORTA, 22, 1);
	at91_set_pio_value(AT91_PIO_PORTA, 22, 0);	/* power down */

#ifdef CONFIG_LCD_IN_PSRAM
	/* initialize te PSRAM */
	int stat = pm9263_lcd_hw_psram_init();

	gd->fb_base = (stat == 0) ? PHYS_PSRAM : ATMEL_BASE_SRAM0;
#else
	gd->fb_base = ATMEL_BASE_SRAM0;
#endif

}

#endif /* CONFIG_LCD */

int board_early_init_f(void)
{
	return 0;
}

int board_init(void)
{
	/* arch number of AT91SAM9263EK-Board */
	gd->bd->bi_arch_number = MACH_TYPE_PM9263;

	/* adress of boot parameters */
	gd->bd->bi_boot_params = PHYS_SDRAM + 0x100;

#ifdef CONFIG_CMD_NAND
	pm9263_nand_hw_init();
#endif
#ifdef CONFIG_MACB
	pm9263_macb_hw_init();
#endif
#ifdef CONFIG_USB_OHCI_NEW
	at91_uhp_hw_init();
#endif
#ifdef CONFIG_LCD
	pm9263_lcd_hw_init();
#endif
	return 0;
}

int dram_init(void)
{
	/* dram_init must store complete ramsize in gd->ram_size */
	gd->ram_size = get_ram_size((void *)PHYS_SDRAM,
				PHYS_SDRAM_SIZE);
	return 0;
}

int dram_init_banksize(void)
{
	gd->bd->bi_dram[0].start = PHYS_SDRAM;
	gd->bd->bi_dram[0].size = PHYS_SDRAM_SIZE;

	return 0;
}

#ifdef CONFIG_RESET_PHY_R
void reset_phy(void)
{
}
#endif

int board_eth_init(bd_t *bis)
{
	int rc = 0;
#ifdef CONFIG_MACB
	rc = macb_eth_initialize(0, (void *)ATMEL_BASE_EMAC, 0x01);
#endif
	return rc;
}

#ifdef CONFIG_DISPLAY_BOARDINFO
int checkboard (void)
{
	char *ss;

	printf ("Board : Ronetix PM9263\n");

	switch (gd->fb_base) {
	case PHYS_PSRAM:
		ss = "(PSRAM)";
		break;

	case ATMEL_BASE_SRAM0:
		ss = "(Internal SRAM)";
		break;

	default:
		ss = "";
		break;
	}
	printf("Video memory : 0x%08lX %s\n", gd->fb_base, ss );

	printf ("\n");
	return 0;
}
#endif
