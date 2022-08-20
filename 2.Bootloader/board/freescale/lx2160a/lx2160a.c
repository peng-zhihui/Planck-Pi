// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2018-2020 NXP
 */

#include <common.h>
#include <clock_legacy.h>
#include <dm.h>
#include <init.h>
#include <dm/platform_data/serial_pl01x.h>
#include <i2c.h>
#include <malloc.h>
#include <errno.h>
#include <netdev.h>
#include <fsl_ddr.h>
#include <fsl_sec.h>
#include <asm/io.h>
#include <fdt_support.h>
#include <linux/bitops.h>
#include <linux/libfdt.h>
#include <fsl-mc/fsl_mc.h>
#include <env_internal.h>
#include <efi_loader.h>
#include <asm/arch/mmu.h>
#include <hwconfig.h>
#include <asm/arch/clock.h>
#include <asm/arch/config.h>
#include <asm/arch/fsl_serdes.h>
#include <asm/arch/soc.h>
#include "../common/qixis.h"
#include "../common/vid.h"
#include <fsl_immap.h>
#include <asm/arch-fsl-layerscape/fsl_icid.h>

#ifdef CONFIG_EMC2305
#include "../common/emc2305.h"
#endif

#ifdef CONFIG_TARGET_LX2160AQDS
#define CFG_MUX_I2C_SDHC(reg, value)		((reg & 0x3f) | value)
#define SET_CFG_MUX1_SDHC1_SDHC(reg)		(reg & 0x3f)
#define SET_CFG_MUX2_SDHC1_SPI(reg, value)	((reg & 0xcf) | value)
#define SET_CFG_MUX3_SDHC1_SPI(reg, value)	((reg & 0xf8) | value)
#define SET_CFG_MUX_SDHC2_DSPI(reg, value)	((reg & 0xf8) | value)
#define SET_CFG_MUX1_SDHC1_DSPI(reg, value)	((reg & 0x3f) | value)
#define SDHC1_BASE_PMUX_DSPI			2
#define SDHC2_BASE_PMUX_DSPI			2
#define IIC5_PMUX_SPI3				3
#endif /* CONFIG_TARGET_LX2160AQDS */

DECLARE_GLOBAL_DATA_PTR;

static struct pl01x_serial_platdata serial0 = {
#if CONFIG_CONS_INDEX == 0
	.base = CONFIG_SYS_SERIAL0,
#elif CONFIG_CONS_INDEX == 1
	.base = CONFIG_SYS_SERIAL1,
#else
#error "Unsupported console index value."
#endif
	.type = TYPE_PL011,
};

U_BOOT_DEVICE(nxp_serial0) = {
	.name = "serial_pl01x",
	.platdata = &serial0,
};

static struct pl01x_serial_platdata serial1 = {
	.base = CONFIG_SYS_SERIAL1,
	.type = TYPE_PL011,
};

U_BOOT_DEVICE(nxp_serial1) = {
	.name = "serial_pl01x",
	.platdata = &serial1,
};

int select_i2c_ch_pca9547(u8 ch)
{
	int ret;

#ifndef CONFIG_DM_I2C
	ret = i2c_write(I2C_MUX_PCA_ADDR_PRI, 0, 1, &ch, 1);
#else
	struct udevice *dev;

	ret = i2c_get_chip_for_busnum(0, I2C_MUX_PCA_ADDR_PRI, 1, &dev);
	if (!ret)
		ret = dm_i2c_write(dev, 0, &ch, 1);
#endif
	if (ret) {
		puts("PCA: failed to select proper channel\n");
		return ret;
	}

	return 0;
}

static void uart_get_clock(void)
{
	serial0.clock = get_serial_clock();
	serial1.clock = get_serial_clock();
}

int board_early_init_f(void)
{
#ifdef CONFIG_SYS_I2C_EARLY_INIT
	i2c_early_init_f();
#endif
	/* get required clock for UART IP */
	uart_get_clock();

#ifdef CONFIG_EMC2305
	select_i2c_ch_pca9547(I2C_MUX_CH_EMC2305);
	emc2305_init();
	set_fan_speed(I2C_EMC2305_PWM);
	select_i2c_ch_pca9547(I2C_MUX_CH_DEFAULT);
#endif

	fsl_lsch3_early_init_f();
	return 0;
}

#ifdef CONFIG_OF_BOARD_FIXUP
int board_fix_fdt(void *fdt)
{
	char *reg_names, *reg_name;
	int names_len, old_name_len, new_name_len, remaining_names_len;
	struct str_map {
		char *old_str;
		char *new_str;
	} reg_names_map[] = {
		{ "ccsr", "dbi" },
		{ "pf_ctrl", "ctrl" }
	};
	int off = -1, i = 0;

	if (IS_SVR_REV(get_svr(), 1, 0))
		return 0;

	off = fdt_node_offset_by_compatible(fdt, -1, "fsl,lx2160a-pcie");
	while (off != -FDT_ERR_NOTFOUND) {
		fdt_setprop(fdt, off, "compatible", "fsl,ls-pcie",
			    strlen("fsl,ls-pcie") + 1);

		reg_names = (char *)fdt_getprop(fdt, off, "reg-names",
						&names_len);
		if (!reg_names)
			continue;

		reg_name = reg_names;
		remaining_names_len = names_len - (reg_name - reg_names);
		i = 0;
		while ((i < ARRAY_SIZE(reg_names_map)) && remaining_names_len) {
			old_name_len = strlen(reg_names_map[i].old_str);
			new_name_len = strlen(reg_names_map[i].new_str);
			if (memcmp(reg_name, reg_names_map[i].old_str,
				   old_name_len) == 0) {
				/* first only leave required bytes for new_str
				 * and copy rest of the string after it
				 */
				memcpy(reg_name + new_name_len,
				       reg_name + old_name_len,
				       remaining_names_len - old_name_len);
				/* Now copy new_str */
				memcpy(reg_name, reg_names_map[i].new_str,
				       new_name_len);
				names_len -= old_name_len;
				names_len += new_name_len;
				i++;
			}

			reg_name = memchr(reg_name, '\0', remaining_names_len);
			if (!reg_name)
				break;

			reg_name += 1;

			remaining_names_len = names_len -
					      (reg_name - reg_names);
		}

		fdt_setprop(fdt, off, "reg-names", reg_names, names_len);
		off = fdt_node_offset_by_compatible(fdt, off,
						    "fsl,lx2160a-pcie");
	}

	return 0;
}
#endif

#if defined(CONFIG_TARGET_LX2160AQDS)
void esdhc_dspi_status_fixup(void *blob)
{
	const char esdhc0_path[] = "/soc/esdhc@2140000";
	const char esdhc1_path[] = "/soc/esdhc@2150000";
	const char dspi0_path[] = "/soc/spi@2100000";
	const char dspi1_path[] = "/soc/spi@2110000";
	const char dspi2_path[] = "/soc/spi@2120000";

	struct ccsr_gur __iomem *gur = (void *)(CONFIG_SYS_FSL_GUTS_ADDR);
	u32 sdhc1_base_pmux;
	u32 sdhc2_base_pmux;
	u32 iic5_pmux;

	/* Check RCW field sdhc1_base_pmux to enable/disable
	 * esdhc0/dspi0 DT node
	 */
	sdhc1_base_pmux = gur_in32(&gur->rcwsr[FSL_CHASSIS3_RCWSR12_REGSR - 1])
		& FSL_CHASSIS3_SDHC1_BASE_PMUX_MASK;
	sdhc1_base_pmux >>= FSL_CHASSIS3_SDHC1_BASE_PMUX_SHIFT;

	if (sdhc1_base_pmux == SDHC1_BASE_PMUX_DSPI) {
		do_fixup_by_path(blob, dspi0_path, "status", "okay",
				 sizeof("okay"), 1);
		do_fixup_by_path(blob, esdhc0_path, "status", "disabled",
				 sizeof("disabled"), 1);
	} else {
		do_fixup_by_path(blob, esdhc0_path, "status", "okay",
				 sizeof("okay"), 1);
		do_fixup_by_path(blob, dspi0_path, "status", "disabled",
				 sizeof("disabled"), 1);
	}

	/* Check RCW field sdhc2_base_pmux to enable/disable
	 * esdhc1/dspi1 DT node
	 */
	sdhc2_base_pmux = gur_in32(&gur->rcwsr[FSL_CHASSIS3_RCWSR13_REGSR - 1])
		& FSL_CHASSIS3_SDHC2_BASE_PMUX_MASK;
	sdhc2_base_pmux >>= FSL_CHASSIS3_SDHC2_BASE_PMUX_SHIFT;

	if (sdhc2_base_pmux == SDHC2_BASE_PMUX_DSPI) {
		do_fixup_by_path(blob, dspi1_path, "status", "okay",
				 sizeof("okay"), 1);
		do_fixup_by_path(blob, esdhc1_path, "status", "disabled",
				 sizeof("disabled"), 1);
	} else {
		do_fixup_by_path(blob, esdhc1_path, "status", "okay",
				 sizeof("okay"), 1);
		do_fixup_by_path(blob, dspi1_path, "status", "disabled",
				 sizeof("disabled"), 1);
	}

	/* Check RCW field IIC5 to enable dspi2 DT node */
	iic5_pmux = gur_in32(&gur->rcwsr[FSL_CHASSIS3_RCWSR12_REGSR - 1])
		& FSL_CHASSIS3_IIC5_PMUX_MASK;
	iic5_pmux >>= FSL_CHASSIS3_IIC5_PMUX_SHIFT;

	if (iic5_pmux == IIC5_PMUX_SPI3)
		do_fixup_by_path(blob, dspi2_path, "status", "okay",
				 sizeof("okay"), 1);
	else
		do_fixup_by_path(blob, dspi2_path, "status", "disabled",
				 sizeof("disabled"), 1);
}
#endif

int esdhc_status_fixup(void *blob, const char *compat)
{
#if defined(CONFIG_TARGET_LX2160AQDS)
	/* Enable esdhc and dspi DT nodes based on RCW fields */
	esdhc_dspi_status_fixup(blob);
#else
	/* Enable both esdhc DT nodes for LX2160ARDB */
	do_fixup_by_compat(blob, compat, "status", "okay",
			   sizeof("okay"), 1);
#endif
	return 0;
}

#if defined(CONFIG_VID)
int i2c_multiplexer_select_vid_channel(u8 channel)
{
	return select_i2c_ch_pca9547(channel);
}

int init_func_vid(void)
{
	int set_vid;

	if (IS_SVR_REV(get_svr(), 1, 0))
		set_vid = adjust_vdd(800);
	else
		set_vid = adjust_vdd(0);

	if (set_vid < 0)
		printf("core voltage not adjusted\n");

	return 0;
}
#endif

int checkboard(void)
{
	enum boot_src src = get_boot_src();
	char buf[64];
	u8 sw;
#ifdef CONFIG_TARGET_LX2160AQDS
	int clock;
	static const char *const freq[] = {"100", "125", "156.25",
					   "161.13", "322.26", "", "", "",
					   "", "", "", "", "", "", "",
					   "100 separate SSCG"};
#endif

	cpu_name(buf);
#ifdef CONFIG_TARGET_LX2160AQDS
	printf("Board: %s-QDS, ", buf);
#else
	printf("Board: %s-RDB, ", buf);
#endif

	sw = QIXIS_READ(arch);
	printf("Board version: %c, boot from ", (sw & 0xf) - 1 + 'A');

	if (src == BOOT_SOURCE_SD_MMC) {
		puts("SD\n");
	} else if (src == BOOT_SOURCE_SD_MMC2) {
		puts("eMMC\n");
	} else {
		sw = QIXIS_READ(brdcfg[0]);
		sw = (sw >> QIXIS_XMAP_SHIFT) & QIXIS_XMAP_MASK;
		switch (sw) {
		case 0:
		case 4:
			puts("FlexSPI DEV#0\n");
			break;
		case 1:
			puts("FlexSPI DEV#1\n");
			break;
		case 2:
		case 3:
			puts("FlexSPI EMU\n");
			break;
		default:
			printf("invalid setting, xmap: %d\n", sw);
			break;
		}
	}
#ifdef CONFIG_TARGET_LX2160AQDS
	printf("FPGA: v%d (%s), build %d",
	       (int)QIXIS_READ(scver), qixis_read_tag(buf),
	       (int)qixis_read_minor());
	/* the timestamp string contains "\n" at the end */
	printf(" on %s", qixis_read_time(buf));

	puts("SERDES1 Reference : ");
	sw = QIXIS_READ(brdcfg[2]);
	clock = sw >> 4;
	printf("Clock1 = %sMHz ", freq[clock]);
	clock = sw & 0x0f;
	printf("Clock2 = %sMHz", freq[clock]);

	sw = QIXIS_READ(brdcfg[3]);
	puts("\nSERDES2 Reference : ");
	clock = sw >> 4;
	printf("Clock1 = %sMHz ", freq[clock]);
	clock = sw & 0x0f;
	printf("Clock2 = %sMHz", freq[clock]);

	sw = QIXIS_READ(brdcfg[12]);
	puts("\nSERDES3 Reference : ");
	clock = sw >> 4;
	printf("Clock1 = %sMHz Clock2 = %sMHz\n", freq[clock], freq[clock]);
#else
	printf("FPGA: v%d.%d\n", QIXIS_READ(scver), QIXIS_READ(tagdata));

	puts("SERDES1 Reference: Clock1 = 161.13MHz Clock2 = 161.13MHz\n");
	puts("SERDES2 Reference: Clock1 = 100MHz Clock2 = 100MHz\n");
	puts("SERDES3 Reference: Clock1 = 100MHz Clock2 = 100MHz\n");
#endif
	return 0;
}

#ifdef CONFIG_TARGET_LX2160AQDS
/*
 * implementation of CONFIG_ESDHC_DETECT_QUIRK Macro.
 */
u8 qixis_esdhc_detect_quirk(void)
{
	/* for LX2160AQDS res1[1] @ offset 0x1A is SDHC1 Control/Status (SDHC1)
	 * SDHC1 Card ID:
	 * Specifies the type of card installed in the SDHC1 adapter slot.
	 * 000= (reserved)
	 * 001= eMMC V4.5 adapter is installed.
	 * 010= SD/MMC 3.3V adapter is installed.
	 * 011= eMMC V4.4 adapter is installed.
	 * 100= eMMC V5.0 adapter is installed.
	 * 101= MMC card/Legacy (3.3V) adapter is installed.
	 * 110= SDCard V2/V3 adapter installed.
	 * 111= no adapter is installed.
	 */
	return ((QIXIS_READ(res1[1]) & QIXIS_SDID_MASK) !=
		 QIXIS_ESDHC_NO_ADAPTER);
}

int config_board_mux(void)
{
	u8 reg11, reg5, reg13;
	struct ccsr_gur __iomem *gur = (void *)(CONFIG_SYS_FSL_GUTS_ADDR);
	u32 sdhc1_base_pmux;
	u32 sdhc2_base_pmux;
	u32 iic5_pmux;

	/* Routes {I2C2_SCL, I2C2_SDA} to SDHC1 as {SDHC1_CD_B, SDHC1_WP}.
	 * Routes {I2C3_SCL, I2C3_SDA} to CAN transceiver as {CAN1_TX,CAN1_RX}.
	 * Routes {I2C4_SCL, I2C4_SDA} to CAN transceiver as {CAN2_TX,CAN2_RX}.
	 * Qixis and remote systems are isolated from the I2C1 bus.
	 * Processor connections are still available.
	 * SPI2 CS2_B controls EN25S64 SPI memory device.
	 * SPI3 CS2_B controls EN25S64 SPI memory device.
	 * EC2 connects to PHY #2 using RGMII protocol.
	 * CLK_OUT connects to FPGA for clock measurement.
	 */

	reg5 = QIXIS_READ(brdcfg[5]);
	reg5 = CFG_MUX_I2C_SDHC(reg5, 0x40);
	QIXIS_WRITE(brdcfg[5], reg5);

	/* Check RCW field sdhc1_base_pmux
	 * esdhc0 : sdhc1_base_pmux = 0
	 * dspi0  : sdhc1_base_pmux = 2
	 */
	sdhc1_base_pmux = gur_in32(&gur->rcwsr[FSL_CHASSIS3_RCWSR12_REGSR - 1])
		& FSL_CHASSIS3_SDHC1_BASE_PMUX_MASK;
	sdhc1_base_pmux >>= FSL_CHASSIS3_SDHC1_BASE_PMUX_SHIFT;

	if (sdhc1_base_pmux == SDHC1_BASE_PMUX_DSPI) {
		reg11 = QIXIS_READ(brdcfg[11]);
		reg11 = SET_CFG_MUX1_SDHC1_DSPI(reg11, 0x40);
		QIXIS_WRITE(brdcfg[11], reg11);
	} else {
		/* - Routes {SDHC1_CMD, SDHC1_CLK } to SDHC1 adapter slot.
		 *          {SDHC1_DAT3, SDHC1_DAT2} to SDHC1 adapter slot.
		 *          {SDHC1_DAT1, SDHC1_DAT0} to SDHC1 adapter slot.
		 */
		reg11 = QIXIS_READ(brdcfg[11]);
		reg11 = SET_CFG_MUX1_SDHC1_SDHC(reg11);
		QIXIS_WRITE(brdcfg[11], reg11);
	}

	/* Check RCW field sdhc2_base_pmux
	 * esdhc1 : sdhc2_base_pmux = 0 (default)
	 * dspi1  : sdhc2_base_pmux = 2
	 */
	sdhc2_base_pmux = gur_in32(&gur->rcwsr[FSL_CHASSIS3_RCWSR13_REGSR - 1])
		& FSL_CHASSIS3_SDHC2_BASE_PMUX_MASK;
	sdhc2_base_pmux >>= FSL_CHASSIS3_SDHC2_BASE_PMUX_SHIFT;

	if (sdhc2_base_pmux == SDHC2_BASE_PMUX_DSPI) {
		reg13 = QIXIS_READ(brdcfg[13]);
		reg13 = SET_CFG_MUX_SDHC2_DSPI(reg13, 0x01);
		QIXIS_WRITE(brdcfg[13], reg13);
	} else {
		reg13 = QIXIS_READ(brdcfg[13]);
		reg13 = SET_CFG_MUX_SDHC2_DSPI(reg13, 0x00);
		QIXIS_WRITE(brdcfg[13], reg13);
	}

	/* Check RCW field IIC5 to enable dspi2 DT nodei
	 * dspi2: IIC5 = 3
	 */
	iic5_pmux = gur_in32(&gur->rcwsr[FSL_CHASSIS3_RCWSR12_REGSR - 1])
		& FSL_CHASSIS3_IIC5_PMUX_MASK;
	iic5_pmux >>= FSL_CHASSIS3_IIC5_PMUX_SHIFT;

	if (iic5_pmux == IIC5_PMUX_SPI3) {
		/* - Routes {SDHC1_DAT4} to SPI3 devices as {SPI3_M_CS0_B}. */
		reg11 = QIXIS_READ(brdcfg[11]);
		reg11 = SET_CFG_MUX2_SDHC1_SPI(reg11, 0x10);
		QIXIS_WRITE(brdcfg[11], reg11);

		/* - Routes {SDHC1_DAT5, SDHC1_DAT6} nowhere.
		 * {SDHC1_DAT7, SDHC1_DS } to {nothing, SPI3_M0_CLK }.
		 * {I2C5_SCL, I2C5_SDA } to {SPI3_M0_MOSI, SPI3_M0_MISO}.
		 */
		reg11 = QIXIS_READ(brdcfg[11]);
		reg11 = SET_CFG_MUX3_SDHC1_SPI(reg11, 0x01);
		QIXIS_WRITE(brdcfg[11], reg11);
	} else {
		/*
		 * If {SDHC1_DAT4} has been configured to route to SDHC1_VS,
		 * do not change it.
		 * Otherwise route {SDHC1_DAT4} to SDHC1 adapter slot.
		 */
		reg11 = QIXIS_READ(brdcfg[11]);
		if ((reg11 & 0x30) != 0x30) {
			reg11 = SET_CFG_MUX2_SDHC1_SPI(reg11, 0x00);
			QIXIS_WRITE(brdcfg[11], reg11);
		}

		/* - Routes {SDHC1_DAT5, SDHC1_DAT6} to SDHC1 adapter slot.
		 * {SDHC1_DAT7, SDHC1_DS } to SDHC1 adapter slot.
		 * {I2C5_SCL, I2C5_SDA } to SDHC1 adapter slot.
		 */
		reg11 = QIXIS_READ(brdcfg[11]);
		reg11 = SET_CFG_MUX3_SDHC1_SPI(reg11, 0x00);
		QIXIS_WRITE(brdcfg[11], reg11);
	}

	return 0;
}
#elif defined(CONFIG_TARGET_LX2160ARDB)
int config_board_mux(void)
{
	u8 brdcfg;

	brdcfg = QIXIS_READ(brdcfg[4]);
	/* The BRDCFG4 register controls general board configuration.
	 *|-------------------------------------------|
	 *|Field  | Function                          |
	 *|-------------------------------------------|
	 *|5      | CAN I/O Enable (net CFG_CAN_EN_B):|
	 *|CAN_EN | 0= CAN transceivers are disabled. |
	 *|       | 1= CAN transceivers are enabled.  |
	 *|-------------------------------------------|
	 */
	brdcfg |= BIT_MASK(5);
	QIXIS_WRITE(brdcfg[4], brdcfg);

	return 0;
}
#else
int config_board_mux(void)
{
	return 0;
}
#endif

unsigned long get_board_sys_clk(void)
{
#ifdef CONFIG_TARGET_LX2160AQDS
	u8 sysclk_conf = QIXIS_READ(brdcfg[1]);

	switch (sysclk_conf & 0x03) {
	case QIXIS_SYSCLK_100:
		return 100000000;
	case QIXIS_SYSCLK_125:
		return 125000000;
	case QIXIS_SYSCLK_133:
		return 133333333;
	}
	return 100000000;
#else
	return 100000000;
#endif
}

unsigned long get_board_ddr_clk(void)
{
#ifdef CONFIG_TARGET_LX2160AQDS
	u8 ddrclk_conf = QIXIS_READ(brdcfg[1]);

	switch ((ddrclk_conf & 0x30) >> 4) {
	case QIXIS_DDRCLK_100:
		return 100000000;
	case QIXIS_DDRCLK_125:
		return 125000000;
	case QIXIS_DDRCLK_133:
		return 133333333;
	}
	return 100000000;
#else
	return 100000000;
#endif
}

int board_init(void)
{
#if defined(CONFIG_FSL_MC_ENET) && defined(CONFIG_TARGET_LX2160ARDB)
	u32 __iomem *irq_ccsr = (u32 __iomem *)ISC_BASE;
#endif
#ifdef CONFIG_ENV_IS_NOWHERE
	gd->env_addr = (ulong)&default_environment[0];
#endif

	select_i2c_ch_pca9547(I2C_MUX_CH_DEFAULT);

#if defined(CONFIG_FSL_MC_ENET) && defined(CONFIG_TARGET_LX2160ARDB)
	/* invert AQR107 IRQ pins polarity */
	out_le32(irq_ccsr + IRQCR_OFFSET / 4, AQR107_IRQ_MASK);
#endif

#ifdef CONFIG_FSL_CAAM
	sec_init();
#endif

#if !defined(CONFIG_SYS_EARLY_PCI_INIT) && defined(CONFIG_DM_ETH)
	pci_init();
#endif
	return 0;
}

void detail_board_ddr_info(void)
{
	int i;
	u64 ddr_size = 0;

	puts("\nDDR    ");
	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++)
		ddr_size += gd->bd->bi_dram[i].size;
	print_size(ddr_size, "");
	print_ddr_info(0);
}

#ifdef CONFIG_MISC_INIT_R
int misc_init_r(void)
{
	config_board_mux();

	return 0;
}
#endif

#ifdef CONFIG_FSL_MC_ENET
extern int fdt_fixup_board_phy(void *fdt);

void fdt_fixup_board_enet(void *fdt)
{
	int offset;

	offset = fdt_path_offset(fdt, "/soc/fsl-mc");

	if (offset < 0)
		offset = fdt_path_offset(fdt, "/fsl-mc");

	if (offset < 0) {
		printf("%s: fsl-mc node not found in device tree (error %d)\n",
		       __func__, offset);
		return;
	}

	if (get_mc_boot_status() == 0 &&
	    (is_lazy_dpl_addr_valid() || get_dpl_apply_status() == 0)) {
		fdt_status_okay(fdt, offset);
#ifndef CONFIG_DM_ETH
		fdt_fixup_board_phy(fdt);
#endif
	} else {
		fdt_status_fail(fdt, offset);
	}
}

void board_quiesce_devices(void)
{
	fsl_mc_ldpaa_exit(gd->bd);
}
#endif

#ifdef CONFIG_OF_BOARD_SETUP
int ft_board_setup(void *blob, bd_t *bd)
{
	int i;
	u16 mc_memory_bank = 0;

	u64 *base;
	u64 *size;
	u64 mc_memory_base = 0;
	u64 mc_memory_size = 0;
	u16 total_memory_banks;

	ft_cpu_setup(blob, bd);

	fdt_fixup_mc_ddr(&mc_memory_base, &mc_memory_size);

	if (mc_memory_base != 0)
		mc_memory_bank++;

	total_memory_banks = CONFIG_NR_DRAM_BANKS + mc_memory_bank;

	base = calloc(total_memory_banks, sizeof(u64));
	size = calloc(total_memory_banks, sizeof(u64));

	/* fixup DT for the three GPP DDR banks */
	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		base[i] = gd->bd->bi_dram[i].start;
		size[i] = gd->bd->bi_dram[i].size;
	}

#ifdef CONFIG_RESV_RAM
	/* reduce size if reserved memory is within this bank */
	if (gd->arch.resv_ram >= base[0] &&
	    gd->arch.resv_ram < base[0] + size[0])
		size[0] = gd->arch.resv_ram - base[0];
	else if (gd->arch.resv_ram >= base[1] &&
		 gd->arch.resv_ram < base[1] + size[1])
		size[1] = gd->arch.resv_ram - base[1];
	else if (gd->arch.resv_ram >= base[2] &&
		 gd->arch.resv_ram < base[2] + size[2])
		size[2] = gd->arch.resv_ram - base[2];
#endif

	if (mc_memory_base != 0) {
		for (i = 0; i <= total_memory_banks; i++) {
			if (base[i] == 0 && size[i] == 0) {
				base[i] = mc_memory_base;
				size[i] = mc_memory_size;
				break;
			}
		}
	}

	fdt_fixup_memory_banks(blob, base, size, total_memory_banks);

#ifdef CONFIG_USB
	fsl_fdt_fixup_dr_usb(blob, bd);
#endif

#ifdef CONFIG_FSL_MC_ENET
	fdt_fsl_mc_fixup_iommu_map_entry(blob);
	fdt_fixup_board_enet(blob);
#endif
	fdt_fixup_icid(blob);

	return 0;
}
#endif

void qixis_dump_switch(void)
{
	int i, nr_of_cfgsw;

	QIXIS_WRITE(cms[0], 0x00);
	nr_of_cfgsw = QIXIS_READ(cms[1]);

	puts("DIP switch settings dump:\n");
	for (i = 1; i <= nr_of_cfgsw; i++) {
		QIXIS_WRITE(cms[0], i);
		printf("SW%d = (0x%02x)\n", i, QIXIS_READ(cms[1]));
	}
}
