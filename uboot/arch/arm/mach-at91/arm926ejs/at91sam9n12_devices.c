// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2013 Atmel Corporation
 * Josh Wu <josh.wu@atmel.com>
 */

#include <common.h>
#include <asm/io.h>
#include <asm/arch/at91_common.h>
#include <asm/arch/at91_pio.h>
#include <asm/arch/clk.h>

unsigned int has_lcdc()
{
	return 1;
}

void at91_serial0_hw_init(void)
{
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 0, 1);		/* TXD0 */
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 1, 0);		/* RXD0 */
	at91_periph_clk_enable(ATMEL_ID_USART0);
}

void at91_serial1_hw_init(void)
{
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 5, 1);		/* TXD1 */
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 6, 0);		/* RXD1 */
	at91_periph_clk_enable(ATMEL_ID_USART1);
}

void at91_serial2_hw_init(void)
{
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 7, 1);		/* TXD2 */
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 8, 0);		/* RXD2 */
	at91_periph_clk_enable(ATMEL_ID_USART2);
}

void at91_serial3_hw_init(void)
{
	at91_pio3_set_b_periph(AT91_PIO_PORTC, 22, 1);		/* TXD3 */
	at91_pio3_set_b_periph(AT91_PIO_PORTC, 23, 0);		/* RXD3 */
	at91_periph_clk_enable(ATMEL_ID_USART3);
}

void at91_seriald_hw_init(void)
{
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 10, 1);		/* DTXD */
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 9, 0);		/* DRXD */
	at91_periph_clk_enable(ATMEL_ID_SYS);
}

#ifdef CONFIG_ATMEL_SPI
void at91_spi0_hw_init(unsigned long cs_mask)
{
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 11, 0);	/* SPI0_MISO */
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 12, 0);	/* SPI0_MOSI */
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 13, 0);	/* SPI0_SPCK */

	at91_periph_clk_enable(ATMEL_ID_SPI0);

	if (cs_mask & (1 << 0))
		at91_set_pio_output(AT91_PIO_PORTA, 14, 1);
	if (cs_mask & (1 << 1))
		at91_set_pio_output(AT91_PIO_PORTA, 7, 1);
	if (cs_mask & (1 << 2))
		at91_set_pio_output(AT91_PIO_PORTA, 1, 1);
	if (cs_mask & (1 << 3))
		at91_set_pio_output(AT91_PIO_PORTB, 3, 1);
}

void at91_spi1_hw_init(unsigned long cs_mask)
{
	at91_pio3_set_b_periph(AT91_PIO_PORTA, 21, 0);	/* SPI1_MISO */
	at91_pio3_set_b_periph(AT91_PIO_PORTA, 22, 0);	/* SPI1_MOSI */
	at91_pio3_set_b_periph(AT91_PIO_PORTA, 23, 0);	/* SPI1_SPCK */

	at91_periph_clk_enable(ATMEL_ID_SPI1);

	if (cs_mask & (1 << 0))
		at91_set_pio_output(AT91_PIO_PORTA, 8, 1);
	if (cs_mask & (1 << 1))
		at91_set_pio_output(AT91_PIO_PORTA, 0, 1);
	if (cs_mask & (1 << 2))
		at91_set_pio_output(AT91_PIO_PORTA, 31, 1);
	if (cs_mask & (1 << 3))
		at91_set_pio_output(AT91_PIO_PORTA, 30, 1);
}
#endif

void at91_mci_hw_init(void)
{
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 17, 0);	/* MCCK */
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 16, 0);	/* MCCDA */
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 15, 0);	/* MCDA0 */
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 18, 0);	/* MCDA1 */
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 19, 0);	/* MCDA2 */
	at91_pio3_set_a_periph(AT91_PIO_PORTA, 20, 0);	/* MCDA3 */

	at91_periph_clk_enable(ATMEL_ID_HSMCI0);
}

#ifdef CONFIG_LCD
void at91_lcd_hw_init(void)
{
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 24, 0);	/* LCDDPWR */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 26, 0);	/* LCDVSYNC */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 27, 0);	/* LCDHSYNC */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 28, 0);	/* LCDDOTCK */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 29, 0);	/* LCDDEN */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 30, 0);	/* LCDDOTCK */

	at91_pio3_set_a_periph(AT91_PIO_PORTC, 0, 0);	/* LCDD0 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 1, 0);	/* LCDD1 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 2, 0);	/* LCDD2 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 3, 0);	/* LCDD3 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 4, 0);	/* LCDD4 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 5, 0);	/* LCDD5 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 6, 0);	/* LCDD6 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 7, 0);	/* LCDD7 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 8, 0);	/* LCDD8 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 9, 0);	/* LCDD9 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 10, 0);	/* LCDD10 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 11, 0);	/* LCDD11 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 12, 0);	/* LCDD12 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 13, 0);	/* LCDD13 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 14, 0);	/* LCDD14 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 15, 0);	/* LCDD15 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 16, 0);	/* LCDD16 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 17, 0);	/* LCDD17 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 18, 0);	/* LCDD18 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 19, 0);	/* LCDD19 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 20, 0);	/* LCDD20 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 21, 0);	/* LCDD21 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 22, 0);	/* LCDD22 */
	at91_pio3_set_a_periph(AT91_PIO_PORTC, 23, 0);	/* LCDD23 */

	at91_periph_clk_enable(ATMEL_ID_LCDC);
}
#endif
