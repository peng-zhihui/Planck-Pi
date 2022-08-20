// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2017 NXP
 */

#include <common.h>
#include <command.h>
#include <env.h>
#include <log.h>
#include <net.h>
#include <netdev.h>
#include <asm/io.h>
#include <asm/arch/fsl_serdes.h>
#include <hwconfig.h>
#include <fsl_mdio.h>
#include <malloc.h>
#include <phy.h>
#include <fm_eth.h>
#include <i2c.h>
#include <miiphy.h>
#include <fsl-mc/fsl_mc.h>
#include <fsl-mc/ldpaa_wriop.h>
#include <linux/delay.h>

#include "../common/qixis.h"

#include "ls1088a_qixis.h"

#ifndef CONFIG_DM_ETH
#ifdef CONFIG_FSL_MC_ENET

#define SFP_TX		0

 /* - In LS1088A A there are only 16 SERDES lanes, spread across 2 SERDES banks.
 *   Bank 1 -> Lanes A, B, C, D,
 *   Bank 2 -> Lanes A,B, C, D,
 */

 /* Mapping of 8 SERDES lanes to LS1088A QDS board slots. A value of '0' here
  * means that the mapping must be determined dynamically, or that the lane
  * maps to something other than a board slot.
  */

static u8 lane_to_slot_fsm1[] = {
	0, 0, 0, 0, 0, 0, 0, 0
};

/* On the Vitesse VSC8234XHG SGMII riser card there are 4 SGMII PHYs
 * housed.
 */

static int xqsgii_riser_phy_addr[] = {
	XQSGMII_CARD_PHY1_PORT0_ADDR,
	XQSGMII_CARD_PHY2_PORT0_ADDR,
	XQSGMII_CARD_PHY3_PORT0_ADDR,
	XQSGMII_CARD_PHY4_PORT0_ADDR,
	XQSGMII_CARD_PHY3_PORT2_ADDR,
	XQSGMII_CARD_PHY1_PORT2_ADDR,
	XQSGMII_CARD_PHY4_PORT2_ADDR,
	XQSGMII_CARD_PHY2_PORT2_ADDR,
};

static int sgmii_riser_phy_addr[] = {
	SGMII_CARD_PORT1_PHY_ADDR,
	SGMII_CARD_PORT2_PHY_ADDR,
	SGMII_CARD_PORT3_PHY_ADDR,
	SGMII_CARD_PORT4_PHY_ADDR,
};

/* Slot2 does not have EMI connections */
#define EMI_NONE	0xFF
#define EMI1_RGMII1	0
#define EMI1_RGMII2	1
#define EMI1_SLOT1	2

static const char * const mdio_names[] = {
	"LS1088A_QDS_MDIO0",
	"LS1088A_QDS_MDIO1",
	"LS1088A_QDS_MDIO2",
	DEFAULT_WRIOP_MDIO2_NAME,
};

struct ls1088a_qds_mdio {
	u8 muxval;
	struct mii_dev *realbus;
};

struct reg_pair {
	uint addr;
	u8 *val;
};

static void sgmii_configure_repeater(int dpmac)
{
	struct mii_dev *bus;
	uint8_t a = 0xf;
	int i, j, k, ret;
	unsigned short value;
	const char *dev = "LS1088A_QDS_MDIO2";
	int i2c_addr[] = {0x58, 0x59, 0x5a, 0x5b};
	int i2c_phy_addr = 0;
	int phy_addr = 0;

	uint8_t ch_a_eq[] = {0x1, 0x2, 0x3, 0x7};
	uint8_t ch_a_ctl2[] = {0x81, 0x82, 0x83, 0x84};
	uint8_t ch_b_eq[] = {0x1, 0x2, 0x3, 0x7};
	uint8_t ch_b_ctl2[] = {0x81, 0x82, 0x83, 0x84};

	u8 reg_val[6] = {0x18, 0x38, 0x4, 0x14, 0xb5, 0x20};
	struct reg_pair reg_pair[10] = {
		{6, &reg_val[0]}, {4, &reg_val[1]},
		{8, &reg_val[2]}, {0xf, NULL},
		{0x11, NULL}, {0x16, NULL},
		{0x18, NULL}, {0x23, &reg_val[3]},
		{0x2d, &reg_val[4]}, {4, &reg_val[5]},
	};
#ifdef CONFIG_DM_I2C
	struct udevice *udev;
#endif

	/* Set I2c to Slot 1 */
#ifndef CONFIG_DM_I2C
	ret = i2c_write(0x77, 0, 0, &a, 1);
#else
	ret = i2c_get_chip_for_busnum(0, 0x77, 1, &udev);
	if (!ret)
		ret = dm_i2c_write(udev, 0, &a, 1);
#endif
	if (ret)
		goto error;

	switch (dpmac) {
	case 1:
		i2c_phy_addr = i2c_addr[1];
		phy_addr = 4;
		break;
	case 2:
		i2c_phy_addr = i2c_addr[0];
		phy_addr = 0;
		break;
	case 3:
		i2c_phy_addr = i2c_addr[3];
		phy_addr = 0xc;
		break;
	case 7:
		i2c_phy_addr = i2c_addr[2];
		phy_addr = 8;
		break;
	}

	/* Check the PHY status */
	ret = miiphy_set_current_dev(dev);
	if (ret > 0)
		goto error;

	bus = mdio_get_current_dev();
	debug("Reading from bus %s\n", bus->name);

	ret = miiphy_write(dev, phy_addr, 0x1f, 3);
	if (ret > 0)
		goto error;

	mdelay(10);
	ret = miiphy_read(dev, phy_addr, 0x11, &value);
	if (ret > 0)
			goto error;

	mdelay(10);

	if ((value & 0xfff) == 0x401) {
		miiphy_write(dev, phy_addr, 0x1f, 0);
		printf("DPMAC %d:PHY is ..... Configured\n", dpmac);
		return;
	}

#ifdef CONFIG_DM_I2C
	i2c_get_chip_for_busnum(0, i2c_phy_addr, 1, &udev);
#endif

	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			reg_pair[3].val = &ch_a_eq[i];
			reg_pair[4].val = &ch_a_ctl2[j];
			reg_pair[5].val = &ch_b_eq[i];
			reg_pair[6].val = &ch_b_ctl2[j];
			for (k = 0; k < 10; k++) {
#ifndef CONFIG_DM_I2C
				ret = i2c_write(i2c_phy_addr,
						reg_pair[k].addr,
						1, reg_pair[k].val, 1);
#else
				ret = i2c_get_chip_for_busnum(0,
							      i2c_phy_addr,
							      1, &udev);
				if (!ret)
					ret = dm_i2c_write(udev,
							   reg_pair[k].addr,
							   reg_pair[k].val, 1);
#endif
				if (ret)
					goto error;
			}

			mdelay(100);
			ret = miiphy_read(dev, phy_addr, 0x11, &value);
			if (ret > 0)
				goto error;

			mdelay(100);
			ret = miiphy_read(dev, phy_addr, 0x11, &value);
			if (ret > 0)
				goto error;

			if ((value & 0xfff) == 0x401) {
				printf("DPMAC %d :PHY is configured ",
				       dpmac);
				printf("after setting repeater 0x%x\n",
				       value);
				i = 5;
				j = 5;
			} else {
				printf("DPMAC %d :PHY is failed to ",
				       dpmac);
				printf("configure the repeater 0x%x\n", value);
			}
		}
	}
	miiphy_write(dev, phy_addr, 0x1f, 0);
error:
	if (ret)
		printf("DPMAC %d ..... FAILED to configure PHY\n", dpmac);
	return;
}

static void qsgmii_configure_repeater(int dpmac)
{
	uint8_t a = 0xf;
	int i, j, k;
	int i2c_phy_addr = 0;
	int phy_addr = 0;
	int i2c_addr[] = {0x58, 0x59, 0x5a, 0x5b};

	uint8_t ch_a_eq[] = {0x1, 0x2, 0x3, 0x7};
	uint8_t ch_a_ctl2[] = {0x81, 0x82, 0x83, 0x84};
	uint8_t ch_b_eq[] = {0x1, 0x2, 0x3, 0x7};
	uint8_t ch_b_ctl2[] = {0x81, 0x82, 0x83, 0x84};

	u8 reg_val[6] = {0x18, 0x38, 0x4, 0x14, 0xb5, 0x20};
	struct reg_pair reg_pair[10] = {
		{6, &reg_val[0]}, {4, &reg_val[1]},
		{8, &reg_val[2]}, {0xf, NULL},
		{0x11, NULL}, {0x16, NULL},
		{0x18, NULL}, {0x23, &reg_val[3]},
		{0x2d, &reg_val[4]}, {4, &reg_val[5]},
	};

	const char *dev = mdio_names[EMI1_SLOT1];
	int ret = 0;
	unsigned short value;
#ifdef CONFIG_DM_I2C
	struct udevice *udev;
#endif

	/* Set I2c to Slot 1 */
#ifndef CONFIG_DM_I2C
	ret = i2c_write(0x77, 0, 0, &a, 1);
#else
	ret = i2c_get_chip_for_busnum(0, 0x77, 1, &udev);
	if (!ret)
		ret = dm_i2c_write(udev, 0, &a, 1);
#endif
	if (ret)
		goto error;

	switch (dpmac) {
	case 7:
	case 8:
	case 9:
	case 10:
		i2c_phy_addr = i2c_addr[2];
		phy_addr = 8;
		break;

	case 3:
	case 4:
	case 5:
	case 6:
		i2c_phy_addr = i2c_addr[3];
		phy_addr = 0xc;
		break;
	}

	/* Check the PHY status */
	ret = miiphy_set_current_dev(dev);
	ret = miiphy_write(dev, phy_addr, 0x1f, 3);
	mdelay(10);
	ret = miiphy_read(dev, phy_addr, 0x11, &value);
	mdelay(10);
	ret = miiphy_read(dev, phy_addr, 0x11, &value);
	mdelay(10);
	if ((value & 0xf) == 0xf) {
		miiphy_write(dev, phy_addr, 0x1f, 0);
		printf("DPMAC %d :PHY is ..... Configured\n", dpmac);
		return;
	}

#ifdef CONFIG_DM_I2C
	i2c_get_chip_for_busnum(0, i2c_phy_addr, 1, &udev);
#endif

	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			reg_pair[3].val = &ch_a_eq[i];
			reg_pair[4].val = &ch_a_ctl2[j];
			reg_pair[5].val = &ch_b_eq[i];
			reg_pair[6].val = &ch_b_ctl2[j];

			for (k = 0; k < 10; k++) {
#ifndef CONFIG_DM_I2C
				ret = i2c_write(i2c_phy_addr,
						reg_pair[k].addr,
						1, reg_pair[k].val, 1);
#else
				ret = i2c_get_chip_for_busnum(0,
							      i2c_addr[dpmac],
							      1, &udev);
				if (!ret)
					ret = dm_i2c_write(udev,
							   reg_pair[k].addr,
							   reg_pair[k].val, 1);
#endif
				if (ret)
					goto error;
			}

			ret = miiphy_read(dev, phy_addr, 0x11, &value);
			if (ret > 0)
				goto error;
			mdelay(1);
			ret = miiphy_read(dev, phy_addr, 0x11, &value);
			if (ret > 0)
				goto error;
			mdelay(10);
			if ((value & 0xf) == 0xf) {
				miiphy_write(dev, phy_addr, 0x1f, 0);
				printf("DPMAC %d :PHY is ..... Configured\n",
				       dpmac);
				return;
			}
		}
	}
error:
	printf("DPMAC %d :PHY ..... FAILED to configure PHY\n", dpmac);
	return;
}

static const char *ls1088a_qds_mdio_name_for_muxval(u8 muxval)
{
	return mdio_names[muxval];
}

struct mii_dev *mii_dev_for_muxval(u8 muxval)
{
	struct mii_dev *bus;
	const char *name = ls1088a_qds_mdio_name_for_muxval(muxval);

	if (!name) {
		printf("No bus for muxval %x\n", muxval);
		return NULL;
	}

	bus = miiphy_get_dev_by_name(name);

	if (!bus) {
		printf("No bus by name %s\n", name);
		return NULL;
	}

	return bus;
}

static void ls1088a_qds_enable_SFP_TX(u8 muxval)
{
	u8 brdcfg9;

	brdcfg9 = QIXIS_READ(brdcfg[9]);
	brdcfg9 &= ~BRDCFG9_SFPTX_MASK;
	brdcfg9 |= (muxval << BRDCFG9_SFPTX_SHIFT);
	QIXIS_WRITE(brdcfg[9], brdcfg9);
}

static void ls1088a_qds_mux_mdio(u8 muxval)
{
	u8 brdcfg4;

	if (muxval <= 5) {
		brdcfg4 = QIXIS_READ(brdcfg[4]);
		brdcfg4 &= ~BRDCFG4_EMISEL_MASK;
		brdcfg4 |= (muxval << BRDCFG4_EMISEL_SHIFT);
		QIXIS_WRITE(brdcfg[4], brdcfg4);
	}
}

static int ls1088a_qds_mdio_read(struct mii_dev *bus, int addr,
				 int devad, int regnum)
{
	struct ls1088a_qds_mdio *priv = bus->priv;

	ls1088a_qds_mux_mdio(priv->muxval);

	return priv->realbus->read(priv->realbus, addr, devad, regnum);
}

static int ls1088a_qds_mdio_write(struct mii_dev *bus, int addr, int devad,
				  int regnum, u16 value)
{
	struct ls1088a_qds_mdio *priv = bus->priv;

	ls1088a_qds_mux_mdio(priv->muxval);

	return priv->realbus->write(priv->realbus, addr, devad, regnum, value);
}

static int ls1088a_qds_mdio_reset(struct mii_dev *bus)
{
	struct ls1088a_qds_mdio *priv = bus->priv;

	return priv->realbus->reset(priv->realbus);
}

static int ls1088a_qds_mdio_init(char *realbusname, u8 muxval)
{
	struct ls1088a_qds_mdio *pmdio;
	struct mii_dev *bus = mdio_alloc();

	if (!bus) {
		printf("Failed to allocate ls1088a_qds MDIO bus\n");
		return -1;
	}

	pmdio = malloc(sizeof(*pmdio));
	if (!pmdio) {
		printf("Failed to allocate ls1088a_qds private data\n");
		free(bus);
		return -1;
	}

	bus->read = ls1088a_qds_mdio_read;
	bus->write = ls1088a_qds_mdio_write;
	bus->reset = ls1088a_qds_mdio_reset;
	sprintf(bus->name, ls1088a_qds_mdio_name_for_muxval(muxval));

	pmdio->realbus = miiphy_get_dev_by_name(realbusname);

	if (!pmdio->realbus) {
		printf("No bus with name %s\n", realbusname);
		free(bus);
		free(pmdio);
		return -1;
	}

	pmdio->muxval = muxval;
	bus->priv = pmdio;

	return mdio_register(bus);
}

/*
 * Initialize the dpmac_info array.
 *
 */
static void initialize_dpmac_to_slot(void)
{
	struct ccsr_gur __iomem *gur = (void *)CONFIG_SYS_FSL_GUTS_ADDR;
	u32 serdes1_prtcl, cfg;

	cfg = in_le32(&gur->rcwsr[FSL_CHASSIS3_SRDS1_REGSR - 1]) &
				FSL_CHASSIS3_SRDS1_PRTCL_MASK;
	cfg >>= FSL_CHASSIS3_SRDS1_PRTCL_SHIFT;
	serdes1_prtcl = serdes_get_number(FSL_SRDS_1, cfg);

	switch (serdes1_prtcl) {
	case 0x12:
		printf("qds: WRIOP: Supported SerDes1 Protocol 0x%02x\n",
		       serdes1_prtcl);
		lane_to_slot_fsm1[0] = EMI1_SLOT1 - 1;
		lane_to_slot_fsm1[1] = EMI1_SLOT1 - 1;
		lane_to_slot_fsm1[2] = EMI1_SLOT1 - 1;
		lane_to_slot_fsm1[3] = EMI1_SLOT1 - 1;
		break;
	case 0x15:
	case 0x1D:
		printf("qds: WRIOP: Supported SerDes1 Protocol 0x%02x\n",
		       serdes1_prtcl);
		lane_to_slot_fsm1[0] = EMI1_SLOT1 - 1;
		lane_to_slot_fsm1[1] = EMI1_SLOT1 - 1;
		lane_to_slot_fsm1[2] = EMI_NONE;
		lane_to_slot_fsm1[3] = EMI_NONE;
		break;
	case 0x1E:
		printf("qds: WRIOP: Supported SerDes1 Protocol 0x%02x\n",
		       serdes1_prtcl);
		lane_to_slot_fsm1[0] = EMI1_SLOT1 - 1;
		lane_to_slot_fsm1[1] = EMI1_SLOT1 - 1;
		lane_to_slot_fsm1[2] = EMI1_SLOT1 - 1;
		lane_to_slot_fsm1[3] = EMI_NONE;
		break;
	case 0x3A:
		printf("qds: WRIOP: Supported SerDes1 Protocol 0x%02x\n",
		       serdes1_prtcl);
		lane_to_slot_fsm1[0] = EMI1_SLOT1 - 1;
		lane_to_slot_fsm1[1] = EMI_NONE;
		lane_to_slot_fsm1[2] = EMI1_SLOT1 - 1;
		lane_to_slot_fsm1[3] = EMI1_SLOT1 - 1;
		break;

	default:
		printf("%s qds: WRIOP: Unsupported SerDes1 Protocol 0x%02x\n",
		       __func__, serdes1_prtcl);
		break;
	}
}

void ls1088a_handle_phy_interface_sgmii(int dpmac_id)
{
	struct mii_dev *bus;
	struct ccsr_gur __iomem *gur = (void *)CONFIG_SYS_FSL_GUTS_ADDR;
	u32 serdes1_prtcl, cfg;

	cfg = in_le32(&gur->rcwsr[FSL_CHASSIS3_SRDS1_REGSR - 1]) &
				FSL_CHASSIS3_SRDS1_PRTCL_MASK;
	cfg >>= FSL_CHASSIS3_SRDS1_PRTCL_SHIFT;
	serdes1_prtcl = serdes_get_number(FSL_SRDS_1, cfg);

	int *riser_phy_addr;
	char *env_hwconfig = env_get("hwconfig");

	if (hwconfig_f("xqsgmii", env_hwconfig))
		riser_phy_addr = &xqsgii_riser_phy_addr[0];
	else
		riser_phy_addr = &sgmii_riser_phy_addr[0];

	switch (serdes1_prtcl) {
	case 0x12:
	case 0x15:
	case 0x1E:
	case 0x3A:
		switch (dpmac_id) {
		case 1:
			wriop_set_phy_address(dpmac_id, 0, riser_phy_addr[1]);
			break;
		case 2:
			wriop_set_phy_address(dpmac_id, 0, riser_phy_addr[0]);
			break;
		case 3:
			wriop_set_phy_address(dpmac_id, 0, riser_phy_addr[3]);
			break;
		case 7:
			wriop_set_phy_address(dpmac_id, 0, riser_phy_addr[2]);
			break;
		default:
			printf("WRIOP: Wrong DPMAC%d set to SGMII", dpmac_id);
			break;
		}
		break;
	default:
		printf("%s qds: WRIOP: Unsupported SerDes1 Protocol 0x%02x\n",
		       __func__, serdes1_prtcl);
		return;
	}
	dpmac_info[dpmac_id].board_mux = EMI1_SLOT1;
	bus = mii_dev_for_muxval(EMI1_SLOT1);
	wriop_set_mdio(dpmac_id, bus);
}

void ls1088a_handle_phy_interface_qsgmii(int dpmac_id)
{
	struct mii_dev *bus;
	struct ccsr_gur __iomem *gur = (void *)CONFIG_SYS_FSL_GUTS_ADDR;
	u32 serdes1_prtcl, cfg;

	cfg = in_le32(&gur->rcwsr[FSL_CHASSIS3_SRDS1_REGSR - 1]) &
				FSL_CHASSIS3_SRDS1_PRTCL_MASK;
	cfg >>= FSL_CHASSIS3_SRDS1_PRTCL_SHIFT;
	serdes1_prtcl = serdes_get_number(FSL_SRDS_1, cfg);

	switch (serdes1_prtcl) {
	case 0x1D:
	case 0x1E:
		switch (dpmac_id) {
		case 3:
		case 4:
		case 5:
		case 6:
			wriop_set_phy_address(dpmac_id, 0, dpmac_id + 9);
			break;
		case 7:
		case 8:
		case 9:
		case 10:
			wriop_set_phy_address(dpmac_id, 0, dpmac_id + 1);
			break;
		}

		dpmac_info[dpmac_id].board_mux = EMI1_SLOT1;
		bus = mii_dev_for_muxval(EMI1_SLOT1);
		wriop_set_mdio(dpmac_id, bus);
		break;
	default:
		printf("qds: WRIOP: Unsupported SerDes Protocol 0x%02x\n",
		       serdes1_prtcl);
		break;
	}
}

void ls1088a_handle_phy_interface_xsgmii(int i)
{
	struct ccsr_gur __iomem *gur = (void *)CONFIG_SYS_FSL_GUTS_ADDR;
	u32 serdes1_prtcl, cfg;

	cfg = in_le32(&gur->rcwsr[FSL_CHASSIS3_SRDS1_REGSR - 1]) &
				FSL_CHASSIS3_SRDS1_PRTCL_MASK;
	cfg >>= FSL_CHASSIS3_SRDS1_PRTCL_SHIFT;
	serdes1_prtcl = serdes_get_number(FSL_SRDS_1, cfg);

	switch (serdes1_prtcl) {
	case 0x15:
	case 0x1D:
	case 0x1E:
		wriop_set_phy_address(i, 0, i + 26);
		ls1088a_qds_enable_SFP_TX(SFP_TX);
		break;
	default:
		printf("qds: WRIOP: Unsupported SerDes Protocol 0x%02x\n",
		       serdes1_prtcl);
		break;
	}
}

static void ls1088a_handle_phy_interface_rgmii(int dpmac_id)
{
	struct ccsr_gur __iomem *gur = (void *)CONFIG_SYS_FSL_GUTS_ADDR;
	u32 serdes1_prtcl, cfg;
	struct mii_dev *bus;

	cfg = in_le32(&gur->rcwsr[FSL_CHASSIS3_SRDS1_REGSR - 1]) &
				FSL_CHASSIS3_SRDS1_PRTCL_MASK;
	cfg >>= FSL_CHASSIS3_SRDS1_PRTCL_SHIFT;
	serdes1_prtcl = serdes_get_number(FSL_SRDS_1, cfg);

	switch (dpmac_id) {
	case 4:
		wriop_set_phy_address(dpmac_id, 0, RGMII_PHY1_ADDR);
		dpmac_info[dpmac_id].board_mux = EMI1_RGMII1;
		bus = mii_dev_for_muxval(EMI1_RGMII1);
		wriop_set_mdio(dpmac_id, bus);
		break;
	case 5:
		wriop_set_phy_address(dpmac_id, 0, RGMII_PHY2_ADDR);
		dpmac_info[dpmac_id].board_mux = EMI1_RGMII2;
		bus = mii_dev_for_muxval(EMI1_RGMII2);
		wriop_set_mdio(dpmac_id, bus);
		break;
	default:
		printf("qds: WRIOP: Unsupported RGMII SerDes Protocol 0x%02x\n",
		       serdes1_prtcl);
		break;
	}
}
#endif

int board_eth_init(bd_t *bis)
{
	int error = 0, i;
#ifdef CONFIG_FSL_MC_ENET
	struct memac_mdio_info *memac_mdio0_info;
	char *env_hwconfig = env_get("hwconfig");

	initialize_dpmac_to_slot();

	memac_mdio0_info = (struct memac_mdio_info *)malloc(
					sizeof(struct memac_mdio_info));
	memac_mdio0_info->regs =
		(struct memac_mdio_controller *)
					CONFIG_SYS_FSL_WRIOP1_MDIO1;
	memac_mdio0_info->name = DEFAULT_WRIOP_MDIO1_NAME;

	/* Register the real MDIO1 bus */
	fm_memac_mdio_init(bis, memac_mdio0_info);
	/* Register the muxing front-ends to the MDIO buses */
	ls1088a_qds_mdio_init(DEFAULT_WRIOP_MDIO1_NAME, EMI1_RGMII1);
	ls1088a_qds_mdio_init(DEFAULT_WRIOP_MDIO1_NAME, EMI1_RGMII2);
	ls1088a_qds_mdio_init(DEFAULT_WRIOP_MDIO1_NAME, EMI1_SLOT1);

	for (i = WRIOP1_DPMAC1; i < NUM_WRIOP_PORTS; i++) {
		switch (wriop_get_enet_if(i)) {
		case PHY_INTERFACE_MODE_RGMII:
		case PHY_INTERFACE_MODE_RGMII_ID:
			ls1088a_handle_phy_interface_rgmii(i);
			break;
		case PHY_INTERFACE_MODE_QSGMII:
			ls1088a_handle_phy_interface_qsgmii(i);
			break;
		case PHY_INTERFACE_MODE_SGMII:
			ls1088a_handle_phy_interface_sgmii(i);
			break;
		case PHY_INTERFACE_MODE_XGMII:
			ls1088a_handle_phy_interface_xsgmii(i);
			break;
		default:
			break;

		if (i == 16)
			i = NUM_WRIOP_PORTS;
		}
	}

	error = cpu_eth_init(bis);

	if (hwconfig_f("xqsgmii", env_hwconfig)) {
		for (i = WRIOP1_DPMAC1; i < NUM_WRIOP_PORTS; i++) {
			switch (wriop_get_enet_if(i)) {
			case PHY_INTERFACE_MODE_QSGMII:
				qsgmii_configure_repeater(i);
				break;
			case PHY_INTERFACE_MODE_SGMII:
				sgmii_configure_repeater(i);
				break;
			default:
				break;
			}

			if (i == 16)
				i = NUM_WRIOP_PORTS;
		}
	}
#endif
	error = pci_eth_init(bis);
	return error;
}
#endif // !CONFIG_DM_ETH

#if defined(CONFIG_RESET_PHY_R)
void reset_phy(void)
{
	mc_env_boot();
}
#endif /* CONFIG_RESET_PHY_R */

#if defined(CONFIG_DM_ETH) && defined(CONFIG_MULTI_DTB_FIT)

/* Structure to hold SERDES protocols supported in case of
 * CONFIG_DM_ETH enabled (network interfaces are described in the DTS).
 *
 * @serdes_block: the index of the SERDES block
 * @serdes_protocol: the decimal value of the protocol supported
 * @dts_needed: DTS notes describing the current configuration are needed
 *
 * When dts_needed is true, the board_fit_config_name_match() function
 * will try to exactly match the current configuration of the block with a DTS
 * name provided.
 */
static struct serdes_configuration {
	u8 serdes_block;
	u32 serdes_protocol;
	bool dts_needed;
} supported_protocols[] = {
	/* Serdes block #1 */
	{1, 21, true},
	{1, 29, true},
};

#define SUPPORTED_SERDES_PROTOCOLS ARRAY_SIZE(supported_protocols)

static bool protocol_supported(u8 serdes_block, u32 protocol)
{
	struct serdes_configuration serdes_conf;
	int i;

	for (i = 0; i < SUPPORTED_SERDES_PROTOCOLS; i++) {
		serdes_conf = supported_protocols[i];
		if (serdes_conf.serdes_block == serdes_block &&
		    serdes_conf.serdes_protocol == protocol)
			return true;
	}

	return false;
}

static void get_str_protocol(u8 serdes_block, u32 protocol, char *str)
{
	struct serdes_configuration serdes_conf;
	int i;

	for (i = 0; i < SUPPORTED_SERDES_PROTOCOLS; i++) {
		serdes_conf = supported_protocols[i];
		if (serdes_conf.serdes_block == serdes_block &&
		    serdes_conf.serdes_protocol == protocol) {
			if (serdes_conf.dts_needed == true)
				sprintf(str, "%u", protocol);
			else
				sprintf(str, "x");
			return;
		}
	}
}

int board_fit_config_name_match(const char *name)
{
	struct ccsr_gur *gur = (void *)(CONFIG_SYS_FSL_GUTS_ADDR);
	char expected_dts[100];
	char srds_s1_str[2];
	u32 srds_s1, cfg;

	cfg = in_le32(&gur->rcwsr[FSL_CHASSIS3_SRDS1_REGSR - 1]) &
		      FSL_CHASSIS3_SRDS1_PRTCL_MASK;
	cfg >>= FSL_CHASSIS3_SRDS1_PRTCL_SHIFT;
	srds_s1 = serdes_get_number(FSL_SRDS_1, cfg);

	/* Check for supported protocols. The default DTS will be used
	 * in this case
	 */
	if (!protocol_supported(1, srds_s1))
		return -1;

	get_str_protocol(1, srds_s1, srds_s1_str);

	sprintf(expected_dts, "fsl-ls1088a-qds-%s-x", srds_s1_str);

	if (!strcmp(name, expected_dts))
		return 0;

	return -1;
}
#endif
