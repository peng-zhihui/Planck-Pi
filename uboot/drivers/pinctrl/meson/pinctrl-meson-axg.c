// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright (C) 2018 Neil Armstrong <narmstrong@baylibre.com>
 *
 * Based on code from Linux kernel:
 *  Copyright (c) 2017 Amlogic, Inc. All rights reserved.
 *  Author: Xingyu Chen <xingyu.chen@amlogic.com>
 */

#include <common.h>
#include <dm.h>
#include <dm/pinctrl.h>
#include <dt-bindings/gpio/meson-axg-gpio.h>

#include "pinctrl-meson-axg.h"

#define EE_OFF	15

/* emmc */
static const unsigned int emmc_nand_d0_pins[] = { PIN(BOOT_0, EE_OFF) };
static const unsigned int emmc_nand_d1_pins[] = { PIN(BOOT_1, EE_OFF) };
static const unsigned int emmc_nand_d2_pins[] = { PIN(BOOT_2, EE_OFF) };
static const unsigned int emmc_nand_d3_pins[] = { PIN(BOOT_3, EE_OFF) };
static const unsigned int emmc_nand_d4_pins[] = { PIN(BOOT_4, EE_OFF) };
static const unsigned int emmc_nand_d5_pins[] = { PIN(BOOT_5, EE_OFF) };
static const unsigned int emmc_nand_d6_pins[] = { PIN(BOOT_6, EE_OFF) };
static const unsigned int emmc_nand_d7_pins[] = { PIN(BOOT_7, EE_OFF) };

static const unsigned int emmc_clk_pins[] = { PIN(BOOT_8, EE_OFF) };
static const unsigned int emmc_cmd_pins[] = { PIN(BOOT_10, EE_OFF) };
static const unsigned int emmc_ds_pins[]  = { PIN(BOOT_13, EE_OFF) };

/* nand */
static const unsigned int nand_ce0_pins[] = { PIN(BOOT_8, EE_OFF) };
static const unsigned int nand_ale_pins[] = { PIN(BOOT_9, EE_OFF) };
static const unsigned int nand_cle_pins[] = { PIN(BOOT_10, EE_OFF) };
static const unsigned int nand_wen_clk_pins[] = { PIN(BOOT_11, EE_OFF) };
static const unsigned int nand_ren_wr_pins[] = { PIN(BOOT_12, EE_OFF) };
static const unsigned int nand_rb0_pins[] = { PIN(BOOT_13, EE_OFF) };

/* nor */
static const unsigned int nor_hold_pins[] = { PIN(BOOT_3, EE_OFF) };
static const unsigned int nor_d_pins[] = { PIN(BOOT_4, EE_OFF) };
static const unsigned int nor_q_pins[] = { PIN(BOOT_5, EE_OFF) };
static const unsigned int nor_c_pins[] = { PIN(BOOT_6, EE_OFF) };
static const unsigned int nor_wp_pins[] = { PIN(BOOT_9, EE_OFF) };
static const unsigned int nor_cs_pins[] = { PIN(BOOT_14, EE_OFF) };

/* sdio */
static const unsigned int sdio_d0_pins[] = { PIN(GPIOX_0, EE_OFF) };
static const unsigned int sdio_d1_pins[] = { PIN(GPIOX_1, EE_OFF) };
static const unsigned int sdio_d2_pins[] = { PIN(GPIOX_2, EE_OFF) };
static const unsigned int sdio_d3_pins[] = { PIN(GPIOX_3, EE_OFF) };
static const unsigned int sdio_clk_pins[] = { PIN(GPIOX_4, EE_OFF) };
static const unsigned int sdio_cmd_pins[] = { PIN(GPIOX_5, EE_OFF) };

/* spi0 */
static const unsigned int spi0_clk_pins[] = { PIN(GPIOZ_0, EE_OFF) };
static const unsigned int spi0_mosi_pins[] = { PIN(GPIOZ_1, EE_OFF) };
static const unsigned int spi0_miso_pins[] = { PIN(GPIOZ_2, EE_OFF) };
static const unsigned int spi0_ss0_pins[] = { PIN(GPIOZ_3, EE_OFF) };
static const unsigned int spi0_ss1_pins[] = { PIN(GPIOZ_4, EE_OFF) };
static const unsigned int spi0_ss2_pins[] = { PIN(GPIOZ_5, EE_OFF) };

/* spi1 */
static const unsigned int spi1_clk_x_pins[] = { PIN(GPIOX_19, EE_OFF) };
static const unsigned int spi1_mosi_x_pins[] = { PIN(GPIOX_17, EE_OFF) };
static const unsigned int spi1_miso_x_pins[] = { PIN(GPIOX_18, EE_OFF) };
static const unsigned int spi1_ss0_x_pins[] = { PIN(GPIOX_16, EE_OFF) };

static const unsigned int spi1_clk_a_pins[] = { PIN(GPIOA_4, EE_OFF) };
static const unsigned int spi1_mosi_a_pins[] = { PIN(GPIOA_2, EE_OFF) };
static const unsigned int spi1_miso_a_pins[] = { PIN(GPIOA_3, EE_OFF) };
static const unsigned int spi1_ss0_a_pins[] = { PIN(GPIOA_5, EE_OFF) };
static const unsigned int spi1_ss1_pins[] = { PIN(GPIOA_6, EE_OFF) };

/* i2c0 */
static const unsigned int i2c0_sck_pins[] = { PIN(GPIOZ_6, EE_OFF) };
static const unsigned int i2c0_sda_pins[] = { PIN(GPIOZ_7, EE_OFF) };

/* i2c1 */
static const unsigned int i2c1_sck_z_pins[] = { PIN(GPIOZ_8, EE_OFF) };
static const unsigned int i2c1_sda_z_pins[] = { PIN(GPIOZ_9, EE_OFF) };

static const unsigned int i2c1_sck_x_pins[] = { PIN(GPIOX_16, EE_OFF) };
static const unsigned int i2c1_sda_x_pins[] = { PIN(GPIOX_17, EE_OFF) };

/* i2c2 */
static const unsigned int i2c2_sck_x_pins[] = { PIN(GPIOX_18, EE_OFF) };
static const unsigned int i2c2_sda_x_pins[] = { PIN(GPIOX_19, EE_OFF) };

static const unsigned int i2c2_sda_a_pins[] = { PIN(GPIOA_17, EE_OFF) };
static const unsigned int i2c2_sck_a_pins[] = { PIN(GPIOA_18, EE_OFF) };

/* i2c3 */
static const unsigned int i2c3_sda_a6_pins[] = { PIN(GPIOA_6, EE_OFF) };
static const unsigned int i2c3_sck_a7_pins[] = { PIN(GPIOA_7, EE_OFF) };

static const unsigned int i2c3_sda_a12_pins[] = { PIN(GPIOA_12, EE_OFF) };
static const unsigned int i2c3_sck_a13_pins[] = { PIN(GPIOA_13, EE_OFF) };

static const unsigned int i2c3_sda_a19_pins[] = { PIN(GPIOA_19, EE_OFF) };
static const unsigned int i2c3_sck_a20_pins[] = { PIN(GPIOA_20, EE_OFF) };

/* uart_a */
static const unsigned int uart_rts_a_pins[] = { PIN(GPIOX_11, EE_OFF) };
static const unsigned int uart_cts_a_pins[] = { PIN(GPIOX_10, EE_OFF) };
static const unsigned int uart_tx_a_pins[] = { PIN(GPIOX_8, EE_OFF) };
static const unsigned int uart_rx_a_pins[] = { PIN(GPIOX_9, EE_OFF) };

/* uart_b */
static const unsigned int uart_rts_b_z_pins[] = { PIN(GPIOZ_0, EE_OFF) };
static const unsigned int uart_cts_b_z_pins[] = { PIN(GPIOZ_1, EE_OFF) };
static const unsigned int uart_tx_b_z_pins[] = { PIN(GPIOZ_2, EE_OFF) };
static const unsigned int uart_rx_b_z_pins[] = { PIN(GPIOZ_3, EE_OFF) };

static const unsigned int uart_rts_b_x_pins[] = { PIN(GPIOX_18, EE_OFF) };
static const unsigned int uart_cts_b_x_pins[] = { PIN(GPIOX_19, EE_OFF) };
static const unsigned int uart_tx_b_x_pins[] = { PIN(GPIOX_16, EE_OFF) };
static const unsigned int uart_rx_b_x_pins[] = { PIN(GPIOX_17, EE_OFF) };

/* uart_ao_b */
static const unsigned int uart_ao_tx_b_z_pins[] = { PIN(GPIOZ_8, EE_OFF) };
static const unsigned int uart_ao_rx_b_z_pins[] = { PIN(GPIOZ_9, EE_OFF) };
static const unsigned int uart_ao_cts_b_z_pins[] = { PIN(GPIOZ_6, EE_OFF) };
static const unsigned int uart_ao_rts_b_z_pins[] = { PIN(GPIOZ_7, EE_OFF) };

/* pwm_a */
static const unsigned int pwm_a_z_pins[] = { PIN(GPIOZ_5, EE_OFF) };

static const unsigned int pwm_a_x18_pins[] = { PIN(GPIOX_18, EE_OFF) };
static const unsigned int pwm_a_x20_pins[] = { PIN(GPIOX_20, EE_OFF) };

static const unsigned int pwm_a_a_pins[] = { PIN(GPIOA_14, EE_OFF) };

/* pwm_b */
static const unsigned int pwm_b_z_pins[] = { PIN(GPIOZ_4, EE_OFF) };

static const unsigned int pwm_b_x_pins[] = { PIN(GPIOX_19, EE_OFF) };

static const unsigned int pwm_b_a_pins[] = { PIN(GPIOA_15, EE_OFF) };

/* pwm_c */
static const unsigned int pwm_c_x10_pins[] = { PIN(GPIOX_10, EE_OFF) };
static const unsigned int pwm_c_x17_pins[] = { PIN(GPIOX_17, EE_OFF) };

static const unsigned int pwm_c_a_pins[] = { PIN(GPIOA_16, EE_OFF) };

/* pwm_d */
static const unsigned int pwm_d_x11_pins[] = { PIN(GPIOX_11, EE_OFF) };
static const unsigned int pwm_d_x16_pins[] = { PIN(GPIOX_16, EE_OFF) };

/* pwm_vs */
static const unsigned int pwm_vs_pins[] = { PIN(GPIOA_0, EE_OFF) };

/* spdif_in */
static const unsigned int spdif_in_z_pins[] = { PIN(GPIOZ_4, EE_OFF) };

static const unsigned int spdif_in_a1_pins[] = { PIN(GPIOA_1, EE_OFF) };
static const unsigned int spdif_in_a7_pins[] = { PIN(GPIOA_7, EE_OFF) };
static const unsigned int spdif_in_a19_pins[] = { PIN(GPIOA_19, EE_OFF) };
static const unsigned int spdif_in_a20_pins[] = { PIN(GPIOA_20, EE_OFF) };

/* spdif_out */
static const unsigned int spdif_out_z_pins[] = { PIN(GPIOZ_5, EE_OFF) };

static const unsigned int spdif_out_a1_pins[] = { PIN(GPIOA_1, EE_OFF) };
static const unsigned int spdif_out_a11_pins[] = { PIN(GPIOA_11, EE_OFF) };
static const unsigned int spdif_out_a19_pins[] = { PIN(GPIOA_19, EE_OFF) };
static const unsigned int spdif_out_a20_pins[] = { PIN(GPIOA_20, EE_OFF) };

/* jtag_ee */
static const unsigned int jtag_tdo_x_pins[] = { PIN(GPIOX_0, EE_OFF) };
static const unsigned int jtag_tdi_x_pins[] = { PIN(GPIOX_1, EE_OFF) };
static const unsigned int jtag_clk_x_pins[] = { PIN(GPIOX_4, EE_OFF) };
static const unsigned int jtag_tms_x_pins[] = { PIN(GPIOX_5, EE_OFF) };

/* eth */
static const unsigned int eth_txd0_x_pins[] = { PIN(GPIOX_8, EE_OFF) };
static const unsigned int eth_txd1_x_pins[] = { PIN(GPIOX_9, EE_OFF) };
static const unsigned int eth_txen_x_pins[] = { PIN(GPIOX_10, EE_OFF) };
static const unsigned int eth_rgmii_rx_clk_x_pins[] = { PIN(GPIOX_12, EE_OFF) };
static const unsigned int eth_rxd0_x_pins[] = { PIN(GPIOX_13, EE_OFF) };
static const unsigned int eth_rxd1_x_pins[] = { PIN(GPIOX_14, EE_OFF) };
static const unsigned int eth_rx_dv_x_pins[] = { PIN(GPIOX_15, EE_OFF) };
static const unsigned int eth_mdio_x_pins[] = { PIN(GPIOX_21, EE_OFF) };
static const unsigned int eth_mdc_x_pins[] = { PIN(GPIOX_22, EE_OFF) };

static const unsigned int eth_txd0_y_pins[] = { PIN(GPIOY_10, EE_OFF) };
static const unsigned int eth_txd1_y_pins[] = { PIN(GPIOY_11, EE_OFF) };
static const unsigned int eth_txen_y_pins[] = { PIN(GPIOY_9, EE_OFF) };
static const unsigned int eth_rgmii_rx_clk_y_pins[] = { PIN(GPIOY_2, EE_OFF) };
static const unsigned int eth_rxd0_y_pins[] = { PIN(GPIOY_4, EE_OFF) };
static const unsigned int eth_rxd1_y_pins[] = { PIN(GPIOY_5, EE_OFF) };
static const unsigned int eth_rx_dv_y_pins[] = { PIN(GPIOY_3, EE_OFF) };
static const unsigned int eth_mdio_y_pins[] = { PIN(GPIOY_0, EE_OFF) };
static const unsigned int eth_mdc_y_pins[] = { PIN(GPIOY_1, EE_OFF) };

static const unsigned int eth_rxd2_rgmii_pins[] = { PIN(GPIOY_6, EE_OFF) };
static const unsigned int eth_rxd3_rgmii_pins[] = { PIN(GPIOY_7, EE_OFF) };
static const unsigned int eth_rgmii_tx_clk_pins[] = { PIN(GPIOY_8, EE_OFF) };
static const unsigned int eth_txd2_rgmii_pins[] = { PIN(GPIOY_12, EE_OFF) };
static const unsigned int eth_txd3_rgmii_pins[] = { PIN(GPIOY_13, EE_OFF) };

/* pdm */
static const unsigned int pdm_dclk_a14_pins[] = { PIN(GPIOA_14, EE_OFF) };
static const unsigned int pdm_dclk_a19_pins[] = { PIN(GPIOA_19, EE_OFF) };
static const unsigned int pdm_din0_pins[] = { PIN(GPIOA_15, EE_OFF) };
static const unsigned int pdm_din1_pins[] = { PIN(GPIOA_16, EE_OFF) };
static const unsigned int pdm_din2_pins[] = { PIN(GPIOA_17, EE_OFF) };
static const unsigned int pdm_din3_pins[] = { PIN(GPIOA_18, EE_OFF) };

/* mclk */
static const unsigned int mclk_c_pins[] = { PIN(GPIOA_0, EE_OFF) };
static const unsigned int mclk_b_pins[] = { PIN(GPIOA_1, EE_OFF) };

/* tdm */
static const unsigned int tdma_sclk_pins[] = { PIN(GPIOX_12, EE_OFF) };
static const unsigned int tdma_sclk_slv_pins[] = { PIN(GPIOX_12, EE_OFF) };
static const unsigned int tdma_fs_pins[] = { PIN(GPIOX_13, EE_OFF) };
static const unsigned int tdma_fs_slv_pins[] = { PIN(GPIOX_13, EE_OFF) };
static const unsigned int tdma_din0_pins[] = { PIN(GPIOX_14, EE_OFF) };
static const unsigned int tdma_dout0_x14_pins[] = { PIN(GPIOX_14, EE_OFF) };
static const unsigned int tdma_dout0_x15_pins[] = { PIN(GPIOX_15, EE_OFF) };
static const unsigned int tdma_dout1_pins[] = { PIN(GPIOX_15, EE_OFF) };
static const unsigned int tdma_din1_pins[] = { PIN(GPIOX_15, EE_OFF) };

static const unsigned int tdmc_sclk_pins[] = { PIN(GPIOA_2, EE_OFF) };
static const unsigned int tdmc_sclk_slv_pins[] = { PIN(GPIOA_2, EE_OFF) };
static const unsigned int tdmc_fs_pins[] = { PIN(GPIOA_3, EE_OFF) };
static const unsigned int tdmc_fs_slv_pins[] = { PIN(GPIOA_3, EE_OFF) };
static const unsigned int tdmc_din0_pins[] = { PIN(GPIOA_4, EE_OFF) };
static const unsigned int tdmc_dout0_pins[] = { PIN(GPIOA_4, EE_OFF) };
static const unsigned int tdmc_din1_pins[] = { PIN(GPIOA_5, EE_OFF) };
static const unsigned int tdmc_dout1_pins[] = { PIN(GPIOA_5, EE_OFF) };
static const unsigned int tdmc_din2_pins[] = { PIN(GPIOA_6, EE_OFF) };
static const unsigned int tdmc_dout2_pins[] = { PIN(GPIOA_6, EE_OFF) };
static const unsigned int tdmc_din3_pins[] = { PIN(GPIOA_7, EE_OFF) };
static const unsigned int tdmc_dout3_pins[] = { PIN(GPIOA_7, EE_OFF) };

static const unsigned int tdmb_sclk_pins[] = { PIN(GPIOA_8, EE_OFF) };
static const unsigned int tdmb_sclk_slv_pins[] = { PIN(GPIOA_8, EE_OFF) };
static const unsigned int tdmb_fs_pins[] = { PIN(GPIOA_9, EE_OFF) };
static const unsigned int tdmb_fs_slv_pins[] = { PIN(GPIOA_9, EE_OFF) };
static const unsigned int tdmb_din0_pins[] = { PIN(GPIOA_10, EE_OFF) };
static const unsigned int tdmb_dout0_pins[] = { PIN(GPIOA_10, EE_OFF) };
static const unsigned int tdmb_din1_pins[] = { PIN(GPIOA_11, EE_OFF) };
static const unsigned int tdmb_dout1_pins[] = { PIN(GPIOA_11, EE_OFF) };
static const unsigned int tdmb_din2_pins[] = { PIN(GPIOA_12, EE_OFF) };
static const unsigned int tdmb_dout2_pins[] = { PIN(GPIOA_12, EE_OFF) };
static const unsigned int tdmb_din3_pins[] = { PIN(GPIOA_13, EE_OFF) };
static const unsigned int tdmb_dout3_pins[] = { PIN(GPIOA_13, EE_OFF) };

static struct meson_pmx_group meson_axg_periphs_groups[] = {
	GPIO_GROUP(GPIOZ_0, EE_OFF),
	GPIO_GROUP(GPIOZ_1, EE_OFF),
	GPIO_GROUP(GPIOZ_2, EE_OFF),
	GPIO_GROUP(GPIOZ_3, EE_OFF),
	GPIO_GROUP(GPIOZ_4, EE_OFF),
	GPIO_GROUP(GPIOZ_5, EE_OFF),
	GPIO_GROUP(GPIOZ_6, EE_OFF),
	GPIO_GROUP(GPIOZ_7, EE_OFF),
	GPIO_GROUP(GPIOZ_8, EE_OFF),
	GPIO_GROUP(GPIOZ_9, EE_OFF),
	GPIO_GROUP(GPIOZ_10, EE_OFF),

	GPIO_GROUP(BOOT_0, EE_OFF),
	GPIO_GROUP(BOOT_1, EE_OFF),
	GPIO_GROUP(BOOT_2, EE_OFF),
	GPIO_GROUP(BOOT_3, EE_OFF),
	GPIO_GROUP(BOOT_4, EE_OFF),
	GPIO_GROUP(BOOT_5, EE_OFF),
	GPIO_GROUP(BOOT_6, EE_OFF),
	GPIO_GROUP(BOOT_7, EE_OFF),
	GPIO_GROUP(BOOT_8, EE_OFF),
	GPIO_GROUP(BOOT_9, EE_OFF),
	GPIO_GROUP(BOOT_10, EE_OFF),
	GPIO_GROUP(BOOT_11, EE_OFF),
	GPIO_GROUP(BOOT_12, EE_OFF),
	GPIO_GROUP(BOOT_13, EE_OFF),
	GPIO_GROUP(BOOT_14, EE_OFF),

	GPIO_GROUP(GPIOA_0, EE_OFF),
	GPIO_GROUP(GPIOA_1, EE_OFF),
	GPIO_GROUP(GPIOA_2, EE_OFF),
	GPIO_GROUP(GPIOA_3, EE_OFF),
	GPIO_GROUP(GPIOA_4, EE_OFF),
	GPIO_GROUP(GPIOA_5, EE_OFF),
	GPIO_GROUP(GPIOA_6, EE_OFF),
	GPIO_GROUP(GPIOA_7, EE_OFF),
	GPIO_GROUP(GPIOA_8, EE_OFF),
	GPIO_GROUP(GPIOA_9, EE_OFF),
	GPIO_GROUP(GPIOA_10, EE_OFF),
	GPIO_GROUP(GPIOA_11, EE_OFF),
	GPIO_GROUP(GPIOA_12, EE_OFF),
	GPIO_GROUP(GPIOA_13, EE_OFF),
	GPIO_GROUP(GPIOA_14, EE_OFF),
	GPIO_GROUP(GPIOA_15, EE_OFF),
	GPIO_GROUP(GPIOA_16, EE_OFF),
	GPIO_GROUP(GPIOA_17, EE_OFF),
	GPIO_GROUP(GPIOA_19, EE_OFF),
	GPIO_GROUP(GPIOA_20, EE_OFF),

	GPIO_GROUP(GPIOX_0, EE_OFF),
	GPIO_GROUP(GPIOX_1, EE_OFF),
	GPIO_GROUP(GPIOX_2, EE_OFF),
	GPIO_GROUP(GPIOX_3, EE_OFF),
	GPIO_GROUP(GPIOX_4, EE_OFF),
	GPIO_GROUP(GPIOX_5, EE_OFF),
	GPIO_GROUP(GPIOX_6, EE_OFF),
	GPIO_GROUP(GPIOX_7, EE_OFF),
	GPIO_GROUP(GPIOX_8, EE_OFF),
	GPIO_GROUP(GPIOX_9, EE_OFF),
	GPIO_GROUP(GPIOX_10, EE_OFF),
	GPIO_GROUP(GPIOX_11, EE_OFF),
	GPIO_GROUP(GPIOX_12, EE_OFF),
	GPIO_GROUP(GPIOX_13, EE_OFF),
	GPIO_GROUP(GPIOX_14, EE_OFF),
	GPIO_GROUP(GPIOX_15, EE_OFF),
	GPIO_GROUP(GPIOX_16, EE_OFF),
	GPIO_GROUP(GPIOX_17, EE_OFF),
	GPIO_GROUP(GPIOX_18, EE_OFF),
	GPIO_GROUP(GPIOX_19, EE_OFF),
	GPIO_GROUP(GPIOX_20, EE_OFF),
	GPIO_GROUP(GPIOX_21, EE_OFF),
	GPIO_GROUP(GPIOX_22, EE_OFF),

	GPIO_GROUP(GPIOY_0, EE_OFF),
	GPIO_GROUP(GPIOY_1, EE_OFF),
	GPIO_GROUP(GPIOY_2, EE_OFF),
	GPIO_GROUP(GPIOY_3, EE_OFF),
	GPIO_GROUP(GPIOY_4, EE_OFF),
	GPIO_GROUP(GPIOY_5, EE_OFF),
	GPIO_GROUP(GPIOY_6, EE_OFF),
	GPIO_GROUP(GPIOY_7, EE_OFF),
	GPIO_GROUP(GPIOY_8, EE_OFF),
	GPIO_GROUP(GPIOY_9, EE_OFF),
	GPIO_GROUP(GPIOY_10, EE_OFF),
	GPIO_GROUP(GPIOY_11, EE_OFF),
	GPIO_GROUP(GPIOY_12, EE_OFF),
	GPIO_GROUP(GPIOY_13, EE_OFF),
	GPIO_GROUP(GPIOY_14, EE_OFF),
	GPIO_GROUP(GPIOY_15, EE_OFF),

	/* bank BOOT */
	GROUP(emmc_nand_d0, 1),
	GROUP(emmc_nand_d1, 1),
	GROUP(emmc_nand_d2, 1),
	GROUP(emmc_nand_d3, 1),
	GROUP(emmc_nand_d4, 1),
	GROUP(emmc_nand_d5, 1),
	GROUP(emmc_nand_d6, 1),
	GROUP(emmc_nand_d7, 1),
	GROUP(emmc_clk, 1),
	GROUP(emmc_cmd, 1),
	GROUP(emmc_ds, 1),
	GROUP(nand_ce0, 2),
	GROUP(nand_ale, 2),
	GROUP(nand_cle, 2),
	GROUP(nand_wen_clk, 2),
	GROUP(nand_ren_wr, 2),
	GROUP(nand_rb0, 2),
	GROUP(nor_hold, 3),
	GROUP(nor_d, 3),
	GROUP(nor_q, 3),
	GROUP(nor_c, 3),
	GROUP(nor_wp, 3),
	GROUP(nor_cs, 3),

	/* bank GPIOZ */
	GROUP(spi0_clk, 1),
	GROUP(spi0_mosi, 1),
	GROUP(spi0_miso, 1),
	GROUP(spi0_ss0, 1),
	GROUP(spi0_ss1, 1),
	GROUP(spi0_ss2, 1),
	GROUP(i2c0_sck, 1),
	GROUP(i2c0_sda, 1),
	GROUP(i2c1_sck_z, 1),
	GROUP(i2c1_sda_z, 1),
	GROUP(uart_rts_b_z, 2),
	GROUP(uart_cts_b_z, 2),
	GROUP(uart_tx_b_z, 2),
	GROUP(uart_rx_b_z, 2),
	GROUP(pwm_a_z, 2),
	GROUP(pwm_b_z, 2),
	GROUP(spdif_in_z, 3),
	GROUP(spdif_out_z, 3),
	GROUP(uart_ao_tx_b_z, 2),
	GROUP(uart_ao_rx_b_z, 2),
	GROUP(uart_ao_cts_b_z, 2),
	GROUP(uart_ao_rts_b_z, 2),

	/* bank GPIOX */
	GROUP(sdio_d0, 1),
	GROUP(sdio_d1, 1),
	GROUP(sdio_d2, 1),
	GROUP(sdio_d3, 1),
	GROUP(sdio_clk, 1),
	GROUP(sdio_cmd, 1),
	GROUP(i2c1_sck_x, 1),
	GROUP(i2c1_sda_x, 1),
	GROUP(i2c2_sck_x, 1),
	GROUP(i2c2_sda_x, 1),
	GROUP(uart_rts_a, 1),
	GROUP(uart_cts_a, 1),
	GROUP(uart_tx_a, 1),
	GROUP(uart_rx_a, 1),
	GROUP(uart_rts_b_x, 2),
	GROUP(uart_cts_b_x, 2),
	GROUP(uart_tx_b_x, 2),
	GROUP(uart_rx_b_x, 2),
	GROUP(jtag_tdo_x, 2),
	GROUP(jtag_tdi_x, 2),
	GROUP(jtag_clk_x, 2),
	GROUP(jtag_tms_x, 2),
	GROUP(spi1_clk_x, 4),
	GROUP(spi1_mosi_x, 4),
	GROUP(spi1_miso_x, 4),
	GROUP(spi1_ss0_x, 4),
	GROUP(pwm_a_x18, 3),
	GROUP(pwm_a_x20, 1),
	GROUP(pwm_b_x, 3),
	GROUP(pwm_c_x10, 3),
	GROUP(pwm_c_x17, 3),
	GROUP(pwm_d_x11, 3),
	GROUP(pwm_d_x16, 3),
	GROUP(eth_txd0_x, 4),
	GROUP(eth_txd1_x, 4),
	GROUP(eth_txen_x, 4),
	GROUP(eth_rgmii_rx_clk_x, 4),
	GROUP(eth_rxd0_x, 4),
	GROUP(eth_rxd1_x, 4),
	GROUP(eth_rx_dv_x, 4),
	GROUP(eth_mdio_x, 4),
	GROUP(eth_mdc_x, 4),
	GROUP(tdma_sclk, 1),
	GROUP(tdma_sclk_slv, 2),
	GROUP(tdma_fs, 1),
	GROUP(tdma_fs_slv, 2),
	GROUP(tdma_din0, 1),
	GROUP(tdma_dout0_x14, 2),
	GROUP(tdma_dout0_x15, 1),
	GROUP(tdma_dout1, 2),
	GROUP(tdma_din1, 3),

	/* bank GPIOY */
	GROUP(eth_txd0_y, 1),
	GROUP(eth_txd1_y, 1),
	GROUP(eth_txen_y, 1),
	GROUP(eth_rgmii_rx_clk_y, 1),
	GROUP(eth_rxd0_y, 1),
	GROUP(eth_rxd1_y, 1),
	GROUP(eth_rx_dv_y, 1),
	GROUP(eth_mdio_y, 1),
	GROUP(eth_mdc_y, 1),
	GROUP(eth_rxd2_rgmii, 1),
	GROUP(eth_rxd3_rgmii, 1),
	GROUP(eth_rgmii_tx_clk, 1),
	GROUP(eth_txd2_rgmii, 1),
	GROUP(eth_txd3_rgmii, 1),

	/* bank GPIOA */
	GROUP(spdif_out_a1, 4),
	GROUP(spdif_out_a11, 3),
	GROUP(spdif_out_a19, 2),
	GROUP(spdif_out_a20, 1),
	GROUP(spdif_in_a1, 3),
	GROUP(spdif_in_a7, 3),
	GROUP(spdif_in_a19, 1),
	GROUP(spdif_in_a20, 2),
	GROUP(spi1_clk_a, 3),
	GROUP(spi1_mosi_a, 3),
	GROUP(spi1_miso_a, 3),
	GROUP(spi1_ss0_a, 3),
	GROUP(spi1_ss1, 3),
	GROUP(pwm_a_a, 3),
	GROUP(pwm_b_a, 3),
	GROUP(pwm_c_a, 3),
	GROUP(pwm_vs, 2),
	GROUP(i2c2_sda_a, 3),
	GROUP(i2c2_sck_a, 3),
	GROUP(i2c3_sda_a6, 4),
	GROUP(i2c3_sck_a7, 4),
	GROUP(i2c3_sda_a12, 4),
	GROUP(i2c3_sck_a13, 4),
	GROUP(i2c3_sda_a19, 4),
	GROUP(i2c3_sck_a20, 4),
	GROUP(pdm_dclk_a14, 1),
	GROUP(pdm_dclk_a19, 3),
	GROUP(pdm_din0, 1),
	GROUP(pdm_din1, 1),
	GROUP(pdm_din2, 1),
	GROUP(pdm_din3, 1),
	GROUP(mclk_c, 1),
	GROUP(mclk_b, 1),
	GROUP(tdmc_sclk, 1),
	GROUP(tdmc_sclk_slv, 2),
	GROUP(tdmc_fs, 1),
	GROUP(tdmc_fs_slv, 2),
	GROUP(tdmc_din0, 2),
	GROUP(tdmc_dout0, 1),
	GROUP(tdmc_din1, 2),
	GROUP(tdmc_dout1, 1),
	GROUP(tdmc_din2, 2),
	GROUP(tdmc_dout2, 1),
	GROUP(tdmc_din3, 2),
	GROUP(tdmc_dout3, 1),
	GROUP(tdmb_sclk, 1),
	GROUP(tdmb_sclk_slv, 2),
	GROUP(tdmb_fs, 1),
	GROUP(tdmb_fs_slv, 2),
	GROUP(tdmb_din0, 2),
	GROUP(tdmb_dout0, 1),
	GROUP(tdmb_din1, 2),
	GROUP(tdmb_dout1, 1),
	GROUP(tdmb_din2, 2),
	GROUP(tdmb_dout2, 1),
	GROUP(tdmb_din3, 2),
	GROUP(tdmb_dout3, 1),
};

/* uart_ao_a */
static const unsigned int uart_ao_tx_a_pins[] = {GPIOAO_0};
static const unsigned int uart_ao_rx_a_pins[] = {GPIOAO_1};
static const unsigned int uart_ao_cts_a_pins[] = {GPIOAO_2};
static const unsigned int uart_ao_rts_a_pins[] = {GPIOAO_3};

/* uart_ao_b */
static const unsigned int uart_ao_tx_b_pins[] = {GPIOAO_4};
static const unsigned int uart_ao_rx_b_pins[] = {GPIOAO_5};
static const unsigned int uart_ao_cts_b_pins[] = {GPIOAO_2};
static const unsigned int uart_ao_rts_b_pins[] = {GPIOAO_3};

/* i2c_ao */
static const unsigned int i2c_ao_sck_4_pins[] = {GPIOAO_4};
static const unsigned int i2c_ao_sda_5_pins[] = {GPIOAO_5};
static const unsigned int i2c_ao_sck_8_pins[] = {GPIOAO_8};
static const unsigned int i2c_ao_sda_9_pins[] = {GPIOAO_9};
static const unsigned int i2c_ao_sck_10_pins[] = {GPIOAO_10};
static const unsigned int i2c_ao_sda_11_pins[] = {GPIOAO_11};

/* i2c_ao_slave */
static const unsigned int i2c_ao_slave_sck_pins[] = {GPIOAO_10};
static const unsigned int i2c_ao_slave_sda_pins[] = {GPIOAO_11};

/* ir_in */
static const unsigned int remote_input_ao_pins[] = {GPIOAO_6};

/* ir_out */
static const unsigned int remote_out_ao_pins[] = {GPIOAO_7};

/* pwm_ao_a */
static const unsigned int pwm_ao_a_pins[] = {GPIOAO_3};

/* pwm_ao_b */
static const unsigned int pwm_ao_b_ao2_pins[] = {GPIOAO_2};
static const unsigned int pwm_ao_b_ao12_pins[] = {GPIOAO_12};

/* pwm_ao_c */
static const unsigned int pwm_ao_c_ao8_pins[] = {GPIOAO_8};
static const unsigned int pwm_ao_c_ao13_pins[] = {GPIOAO_13};

/* pwm_ao_d */
static const unsigned int pwm_ao_d_pins[] = {GPIOAO_9};

/* jtag_ao */
static const unsigned int jtag_ao_tdi_pins[] = {GPIOAO_3};
static const unsigned int jtag_ao_tdo_pins[] = {GPIOAO_4};
static const unsigned int jtag_ao_clk_pins[] = {GPIOAO_5};
static const unsigned int jtag_ao_tms_pins[] = {GPIOAO_7};

static struct meson_pmx_group meson_axg_aobus_groups[] = {
	GPIO_GROUP(GPIOAO_0, 0),
	GPIO_GROUP(GPIOAO_1, 0),
	GPIO_GROUP(GPIOAO_2, 0),
	GPIO_GROUP(GPIOAO_3, 0),
	GPIO_GROUP(GPIOAO_4, 0),
	GPIO_GROUP(GPIOAO_5, 0),
	GPIO_GROUP(GPIOAO_6, 0),
	GPIO_GROUP(GPIOAO_7, 0),
	GPIO_GROUP(GPIOAO_8, 0),
	GPIO_GROUP(GPIOAO_9, 0),
	GPIO_GROUP(GPIOAO_10, 0),
	GPIO_GROUP(GPIOAO_11, 0),
	GPIO_GROUP(GPIOAO_12, 0),
	GPIO_GROUP(GPIOAO_13, 0),
	GPIO_GROUP(GPIO_TEST_N, 0),

	/* bank AO */
	GROUP(uart_ao_tx_a, 1),
	GROUP(uart_ao_rx_a, 1),
	GROUP(uart_ao_cts_a, 2),
	GROUP(uart_ao_rts_a, 2),
	GROUP(uart_ao_tx_b, 1),
	GROUP(uart_ao_rx_b, 1),
	GROUP(uart_ao_cts_b, 1),
	GROUP(uart_ao_rts_b, 1),
	GROUP(i2c_ao_sck_4, 2),
	GROUP(i2c_ao_sda_5, 2),
	GROUP(i2c_ao_sck_8, 2),
	GROUP(i2c_ao_sda_9, 2),
	GROUP(i2c_ao_sck_10, 2),
	GROUP(i2c_ao_sda_11, 2),
	GROUP(i2c_ao_slave_sck, 1),
	GROUP(i2c_ao_slave_sda, 1),
	GROUP(remote_input_ao, 1),
	GROUP(remote_out_ao, 1),
	GROUP(pwm_ao_a, 3),
	GROUP(pwm_ao_b_ao2, 3),
	GROUP(pwm_ao_b_ao12, 3),
	GROUP(pwm_ao_c_ao8, 3),
	GROUP(pwm_ao_c_ao13, 3),
	GROUP(pwm_ao_d, 3),
	GROUP(jtag_ao_tdi, 4),
	GROUP(jtag_ao_tdo, 4),
	GROUP(jtag_ao_clk, 4),
	GROUP(jtag_ao_tms, 4),
};

static const char * const gpio_periphs_groups[] = {
	"GPIOZ_0", "GPIOZ_1", "GPIOZ_2", "GPIOZ_3", "GPIOZ_4",
	"GPIOZ_5", "GPIOZ_6", "GPIOZ_7", "GPIOZ_8", "GPIOZ_9",
	"GPIOZ_10",

	"BOOT_0", "BOOT_1", "BOOT_2", "BOOT_3", "BOOT_4",
	"BOOT_5", "BOOT_6", "BOOT_7", "BOOT_8", "BOOT_9",
	"BOOT_10", "BOOT_11", "BOOT_12", "BOOT_13", "BOOT_14",

	"GPIOA_0", "GPIOA_1", "GPIOA_2", "GPIOA_3", "GPIOA_4",
	"GPIOA_5", "GPIOA_6", "GPIOA_7", "GPIOA_8", "GPIOA_9",
	"GPIOA_10", "GPIOA_11", "GPIOA_12", "GPIOA_13", "GPIOA_14",
	"GPIOA_15", "GPIOA_16", "GPIOA_17", "GPIOA_18", "GPIOA_19",
	"GPIOA_20",

	"GPIOX_0", "GPIOX_1", "GPIOX_2", "GPIOX_3", "GPIOX_4",
	"GPIOX_5", "GPIOX_6", "GPIOX_7", "GPIOX_8", "GPIOX_9",
	"GPIOX_10", "GPIOX_11", "GPIOX_12", "GPIOX_13", "GPIOX_14",
	"GPIOX_15", "GPIOX_16", "GPIOX_17", "GPIOX_18", "GPIOX_19",
	"GPIOX_20", "GPIOX_21", "GPIOX_22",

	"GPIOY_0", "GPIOY_1", "GPIOY_2", "GPIOY_3", "GPIOY_4",
	"GPIOY_5", "GPIOY_6", "GPIOY_7", "GPIOY_8", "GPIOY_9",
	"GPIOY_10", "GPIOY_11", "GPIOY_12", "GPIOY_13", "GPIOY_14",
	"GPIOY_15",
};

static const char * const emmc_groups[] = {
	"emmc_nand_d0", "emmc_nand_d1", "emmc_nand_d2",
	"emmc_nand_d3", "emmc_nand_d4", "emmc_nand_d5",
	"emmc_nand_d6", "emmc_nand_d7",
	"emmc_clk", "emmc_cmd", "emmc_ds",
};

static const char * const nand_groups[] = {
	"emmc_nand_d0", "emmc_nand_d1", "emmc_nand_d2",
	"emmc_nand_d3", "emmc_nand_d4", "emmc_nand_d5",
	"emmc_nand_d6", "emmc_nand_d7",
	"nand_ce0", "nand_ale", "nand_cle",
	"nand_wen_clk", "nand_ren_wr", "nand_rb0",
};

static const char * const nor_groups[] = {
	"nor_d", "nor_q", "nor_c", "nor_cs",
	"nor_hold", "nor_wp",
};

static const char * const sdio_groups[] = {
	"sdio_d0", "sdio_d1", "sdio_d2", "sdio_d3",
	"sdio_cmd", "sdio_clk",
};

static const char * const spi0_groups[] = {
	"spi0_clk", "spi0_mosi", "spi0_miso", "spi0_ss0",
	"spi0_ss1", "spi0_ss2"
};

static const char * const spi1_groups[] = {
	"spi1_clk_x", "spi1_mosi_x", "spi1_miso_x", "spi1_ss0_x",
	"spi1_clk_a", "spi1_mosi_a", "spi1_miso_a", "spi1_ss0_a",
	"spi1_ss1"
};

static const char * const uart_a_groups[] = {
	"uart_tx_a", "uart_rx_a", "uart_cts_a", "uart_rts_a",
};

static const char * const uart_b_groups[] = {
	"uart_tx_b_z", "uart_rx_b_z", "uart_cts_b_z", "uart_rts_b_z",
	"uart_tx_b_x", "uart_rx_b_x", "uart_cts_b_x", "uart_rts_b_x",
};

static const char * const uart_ao_b_z_groups[] = {
	"uart_ao_tx_b_z", "uart_ao_rx_b_z",
	"uart_ao_cts_b_z", "uart_ao_rts_b_z",
};

static const char * const i2c0_groups[] = {
	"i2c0_sck", "i2c0_sda",
};

static const char * const i2c1_groups[] = {
	"i2c1_sck_z", "i2c1_sda_z",
	"i2c1_sck_x", "i2c1_sda_x",
};

static const char * const i2c2_groups[] = {
	"i2c2_sck_x", "i2c2_sda_x",
	"i2c2_sda_a", "i2c2_sck_a",
};

static const char * const i2c3_groups[] = {
	"i2c3_sda_a6", "i2c3_sck_a7",
	"i2c3_sda_a12", "i2c3_sck_a13",
	"i2c3_sda_a19", "i2c3_sck_a20",
};

static const char * const eth_groups[] = {
	"eth_rxd2_rgmii", "eth_rxd3_rgmii", "eth_rgmii_tx_clk",
	"eth_txd2_rgmii", "eth_txd3_rgmii",
	"eth_txd0_x", "eth_txd1_x", "eth_txen_x", "eth_rgmii_rx_clk_x",
	"eth_rxd0_x", "eth_rxd1_x", "eth_rx_dv_x", "eth_mdio_x",
	"eth_mdc_x",
	"eth_txd0_y", "eth_txd1_y", "eth_txen_y", "eth_rgmii_rx_clk_y",
	"eth_rxd0_y", "eth_rxd1_y", "eth_rx_dv_y", "eth_mdio_y",
	"eth_mdc_y",
};

static const char * const pwm_a_groups[] = {
	"pwm_a_z", "pwm_a_x18", "pwm_a_x20", "pwm_a_a",
};

static const char * const pwm_b_groups[] = {
	"pwm_b_z", "pwm_b_x", "pwm_b_a",
};

static const char * const pwm_c_groups[] = {
	"pwm_c_x10", "pwm_c_x17", "pwm_c_a",
};

static const char * const pwm_d_groups[] = {
	"pwm_d_x11", "pwm_d_x16",
};

static const char * const pwm_vs_groups[] = {
	"pwm_vs",
};

static const char * const spdif_out_groups[] = {
	"spdif_out_z", "spdif_out_a1", "spdif_out_a11",
	"spdif_out_a19", "spdif_out_a20",
};

static const char * const spdif_in_groups[] = {
	"spdif_in_z", "spdif_in_a1", "spdif_in_a7",
	"spdif_in_a19", "spdif_in_a20",
};

static const char * const jtag_ee_groups[] = {
	"jtag_tdo_x", "jtag_tdi_x", "jtag_clk_x",
	"jtag_tms_x",
};

static const char * const pdm_groups[] = {
	"pdm_din0", "pdm_din1", "pdm_din2", "pdm_din3",
	"pdm_dclk_a14", "pdm_dclk_a19",
};

static const char * const gpio_aobus_groups[] = {
	"GPIOAO_0", "GPIOAO_1", "GPIOAO_2", "GPIOAO_3", "GPIOAO_4",
	"GPIOAO_5", "GPIOAO_6", "GPIOAO_7", "GPIOAO_8", "GPIOAO_9",
	"GPIOAO_10", "GPIOAO_11", "GPIOAO_12", "GPIOAO_13",
	"GPIO_TEST_N",
};

static const char * const uart_ao_a_groups[] = {
	"uart_ao_tx_a", "uart_ao_rx_a", "uart_ao_cts_a", "uart_ao_rts_a",
};

static const char * const uart_ao_b_groups[] = {
	"uart_ao_tx_b", "uart_ao_rx_b", "uart_ao_cts_b", "uart_ao_rts_b",
};

static const char * const i2c_ao_groups[] = {
	"i2c_ao_sck_4", "i2c_ao_sda_5",
	"i2c_ao_sck_8", "i2c_ao_sda_9",
	"i2c_ao_sck_10", "i2c_ao_sda_11",
};

static const char * const i2c_ao_slave_groups[] = {
	"i2c_ao_slave_sck", "i2c_ao_slave_sda",
};

static const char * const remote_input_ao_groups[] = {
	"remote_input_ao",
};

static const char * const remote_out_ao_groups[] = {
	"remote_out_ao",
};

static const char * const pwm_ao_a_groups[] = {
	"pwm_ao_a",
};

static const char * const pwm_ao_b_groups[] = {
	"pwm_ao_b_ao2", "pwm_ao_b_ao12",
};

static const char * const pwm_ao_c_groups[] = {
	"pwm_ao_c_ao8", "pwm_ao_c_ao13",
};

static const char * const pwm_ao_d_groups[] = {
	"pwm_ao_d",
};

static const char * const jtag_ao_groups[] = {
	"jtag_ao_tdi", "jtag_ao_tdo", "jtag_ao_clk", "jtag_ao_tms",
};

static const char * const mclk_c_groups[] = {
	"mclk_c",
};

static const char * const mclk_b_groups[] = {
	"mclk_b",
};

static const char * const tdma_groups[] = {
	"tdma_sclk", "tdma_sclk_slv", "tdma_fs", "tdma_fs_slv",
	"tdma_din0", "tdma_dout0_x14", "tdma_dout0_x15", "tdma_dout1",
	"tdma_din1",
};

static const char * const tdmc_groups[] = {
	"tdmc_sclk", "tdmc_sclk_slv", "tdmc_fs", "tdmc_fs_slv",
	"tdmc_din0", "tdmc_dout0", "tdmc_din1",	"tdmc_dout1",
	"tdmc_din2", "tdmc_dout2", "tdmc_din3",	"tdmc_dout3",
};

static const char * const tdmb_groups[] = {
	"tdmb_sclk", "tdmb_sclk_slv", "tdmb_fs", "tdmb_fs_slv",
	"tdmb_din0", "tdmb_dout0", "tdmb_din1",	"tdmb_dout1",
	"tdmb_din2", "tdmb_dout2", "tdmb_din3",	"tdmb_dout3",
};

static struct meson_pmx_func meson_axg_periphs_functions[] = {
	FUNCTION(gpio_periphs),
	FUNCTION(emmc),
	FUNCTION(nor),
	FUNCTION(spi0),
	FUNCTION(spi1),
	FUNCTION(sdio),
	FUNCTION(nand),
	FUNCTION(uart_a),
	FUNCTION(uart_b),
	FUNCTION(uart_ao_b_z),
	FUNCTION(i2c0),
	FUNCTION(i2c1),
	FUNCTION(i2c2),
	FUNCTION(i2c3),
	FUNCTION(eth),
	FUNCTION(pwm_a),
	FUNCTION(pwm_b),
	FUNCTION(pwm_c),
	FUNCTION(pwm_d),
	FUNCTION(pwm_vs),
	FUNCTION(spdif_out),
	FUNCTION(spdif_in),
	FUNCTION(jtag_ee),
	FUNCTION(pdm),
	FUNCTION(mclk_b),
	FUNCTION(mclk_c),
	FUNCTION(tdma),
	FUNCTION(tdmb),
	FUNCTION(tdmc),
};

static struct meson_pmx_func meson_axg_aobus_functions[] = {
	FUNCTION(gpio_aobus),
	FUNCTION(uart_ao_a),
	FUNCTION(uart_ao_b),
	FUNCTION(i2c_ao),
	FUNCTION(i2c_ao_slave),
	FUNCTION(remote_input_ao),
	FUNCTION(remote_out_ao),
	FUNCTION(pwm_ao_a),
	FUNCTION(pwm_ao_b),
	FUNCTION(pwm_ao_c),
	FUNCTION(pwm_ao_d),
	FUNCTION(jtag_ao),
};

static struct meson_bank meson_axg_periphs_banks[] = {
	/*   name    first                      last                    pullen  pull    dir     out     in  */
	BANK("Z",    PIN(GPIOZ_0, EE_OFF),	PIN(GPIOZ_10, EE_OFF), 3,  0,  3,  0,  9,  0,  10, 0,  11, 0),
	BANK("BOOT", PIN(BOOT_0, EE_OFF),	PIN(BOOT_14, EE_OFF),  4,  0,  4,  0,  12, 0,  13, 0,  14, 0),
	BANK("A",    PIN(GPIOA_0, EE_OFF),	PIN(GPIOA_20, EE_OFF), 0,  0,  0,  0,  0,  0,  1,  0,  2,  0),
	BANK("X",    PIN(GPIOX_0, EE_OFF),	PIN(GPIOX_22, EE_OFF), 2,  0,  2,  0,  6,  0,  7,  0,  8,  0),
	BANK("Y",    PIN(GPIOY_0, EE_OFF),	PIN(GPIOY_15, EE_OFF), 1,  0,  1,  0,  3,  0,  4,  0,  5,  0),
};

static struct meson_bank meson_axg_aobus_banks[] = {
	/*   name    first              last              pullen  pull    dir     out     in  */
	BANK("AO",   PIN(GPIOAO_0, 0),  PIN(GPIOAO_13, 0), 0,  16,  0, 0,  0,  0,  0, 16,  1,  0),
};

static struct meson_pmx_bank meson_axg_periphs_pmx_banks[] = {
	/*	 name	 first			last	   	      reg  offset  */
	BANK_PMX("Z",	 PIN(GPIOZ_0, EE_OFF),	PIN(GPIOZ_10, EE_OFF), 0x2, 0),
	BANK_PMX("BOOT", PIN(BOOT_0, EE_OFF),	PIN(BOOT_14, EE_OFF),  0x0, 0),
	BANK_PMX("A",	 PIN(GPIOA_0, EE_OFF),	PIN(GPIOA_20, EE_OFF), 0xb, 0),
	BANK_PMX("X",	 PIN(GPIOX_0, EE_OFF),	PIN(GPIOX_22, EE_OFF), 0x4, 0),
	BANK_PMX("Y",	 PIN(GPIOY_0, EE_OFF),	PIN(GPIOY_15, EE_OFF), 0x8, 0),
};

static struct meson_axg_pmx_data meson_axg_periphs_pmx_banks_data = {
	.pmx_banks	= meson_axg_periphs_pmx_banks,
	.num_pmx_banks = ARRAY_SIZE(meson_axg_periphs_pmx_banks),
};

static struct meson_pmx_bank meson_axg_aobus_pmx_banks[] = {
	BANK_PMX("AO", GPIOAO_0, GPIOAO_13, 0x0, 0),
};

static struct meson_axg_pmx_data meson_axg_aobus_pmx_banks_data = {
	.pmx_banks	= meson_axg_aobus_pmx_banks,
	.num_pmx_banks = ARRAY_SIZE(meson_axg_aobus_pmx_banks),
};

struct meson_pinctrl_data meson_axg_periphs_pinctrl_data = {
	.name		= "periphs-banks",
	.pin_base	= EE_OFF,
	.groups		= meson_axg_periphs_groups,
	.funcs		= meson_axg_periphs_functions,
	.banks		= meson_axg_periphs_banks,
	.num_pins	= 86,
	.num_groups	= ARRAY_SIZE(meson_axg_periphs_groups),
	.num_funcs	= ARRAY_SIZE(meson_axg_periphs_functions),
	.num_banks	= ARRAY_SIZE(meson_axg_periphs_banks),
	.gpio_driver	= &meson_axg_gpio_driver,
	.pmx_data	= &meson_axg_periphs_pmx_banks_data,
};

struct meson_pinctrl_data meson_axg_aobus_pinctrl_data = {
	.name		= "aobus-banks",
	.pin_base	= 0,
	.groups		= meson_axg_aobus_groups,
	.funcs		= meson_axg_aobus_functions,
	.banks		= meson_axg_aobus_banks,
	.num_pins	= 14,
	.num_groups	= ARRAY_SIZE(meson_axg_aobus_groups),
	.num_funcs	= ARRAY_SIZE(meson_axg_aobus_functions),
	.num_banks	= ARRAY_SIZE(meson_axg_aobus_banks),
	.gpio_driver	= &meson_axg_gpio_driver,
	.pmx_data	= &meson_axg_aobus_pmx_banks_data,
};

static const struct udevice_id meson_axg_pinctrl_match[] = {
	{
		.compatible = "amlogic,meson-axg-periphs-pinctrl",
		.data = (ulong)&meson_axg_periphs_pinctrl_data,
	},
	{
		.compatible = "amlogic,meson-axg-aobus-pinctrl",
		.data = (ulong)&meson_axg_aobus_pinctrl_data,
	},
	{ /* sentinel */ }
};

U_BOOT_DRIVER(meson_axg_pinctrl) = {
	.name = "meson-axg-pinctrl",
	.id = UCLASS_PINCTRL,
	.of_match = of_match_ptr(meson_axg_pinctrl_match),
	.probe = meson_pinctrl_probe,
	.priv_auto_alloc_size = sizeof(struct meson_pinctrl),
	.ops = &meson_axg_pinctrl_ops,
};
