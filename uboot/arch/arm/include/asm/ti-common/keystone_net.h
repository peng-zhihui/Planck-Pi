/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * emac definitions for keystone2 devices
 *
 * (C) Copyright 2012-2014
 *     Texas Instruments Incorporated, <www.ti.com>
 */

#ifndef _KEYSTONE_NET_H_
#define _KEYSTONE_NET_H_

#include <asm/io.h>
#include <phy.h>
#ifndef __ASSEMBLY__
#include <linux/bitops.h>
#endif

/* EMAC */
#ifdef CONFIG_KSNET_NETCP_V1_0

#define GBETH_BASE			(CONFIG_KSNET_NETCP_BASE + 0x00090000)
#define EMAC_EMACSL_BASE_ADDR		(GBETH_BASE + 0x900)
#define EMAC_MDIO_BASE_ADDR		(GBETH_BASE + 0x300)
#define EMAC_SGMII_BASE_ADDR		(GBETH_BASE + 0x100)
#define DEVICE_EMACSL_BASE(x)		(EMAC_EMACSL_BASE_ADDR + (x) * 0x040)

/* Register offsets */
#define CPGMACSL_REG_CTL		0x04
#define CPGMACSL_REG_STATUS		0x08
#define CPGMACSL_REG_RESET		0x0c
#define CPGMACSL_REG_MAXLEN		0x10

#elif defined CONFIG_KSNET_NETCP_V1_5

#define GBETH_BASE			(CONFIG_KSNET_NETCP_BASE + 0x00200000)
#define CPGMACSL_REG_RX_PRI_MAP		0x020
#define EMAC_EMACSL_BASE_ADDR		(GBETH_BASE + 0x22000)
#define EMAC_MDIO_BASE_ADDR		(GBETH_BASE + 0x00f00)
#define EMAC_SGMII_BASE_ADDR		(GBETH_BASE + 0x00100)
#define DEVICE_EMACSL_BASE(x)		(EMAC_EMACSL_BASE_ADDR + (x) * 0x1000)

/* Register offsets */
#define CPGMACSL_REG_CTL		0x330
#define CPGMACSL_REG_STATUS		0x334
#define CPGMACSL_REG_RESET		0x338
#define CPGMACSL_REG_MAXLEN		0x024

#endif

#define KEYSTONE2_EMAC_GIG_ENABLE

#define MAC_ID_BASE_ADDR		CONFIG_KSNET_MAC_ID_BASE

/* MDIO module input frequency */
#ifdef CONFIG_SOC_K2G
#define EMAC_MDIO_BUS_FREQ		(ks_clk_get_rate(sys_clk0_3_clk))
#else
#define EMAC_MDIO_BUS_FREQ		(ks_clk_get_rate(pass_pll_clk))
#endif
/* MDIO clock output frequency */
#define EMAC_MDIO_CLOCK_FREQ		2500000	/* 2.5 MHz */

#define EMAC_MACCONTROL_MIIEN_ENABLE		0x20
#define EMAC_MACCONTROL_FULLDUPLEX_ENABLE	0x1
#define EMAC_MACCONTROL_GIGABIT_ENABLE		BIT(7)
#define EMAC_MACCONTROL_GIGFORCE		BIT(17)
#define EMAC_MACCONTROL_RMIISPEED_100		BIT(15)

#define EMAC_MIN_ETHERNET_PKT_SIZE		60

struct mac_sl_cfg {
	u_int32_t max_rx_len;	/* Maximum receive packet length. */
	u_int32_t ctl;		/* Control bitfield */
};

/**
 * Definition: Control bitfields used in the ctl field of mac_sl_cfg
 */
#define GMACSL_RX_ENABLE_RCV_CONTROL_FRAMES	BIT(24)
#define GMACSL_RX_ENABLE_RCV_SHORT_FRAMES	BIT(23)
#define GMACSL_RX_ENABLE_RCV_ERROR_FRAMES	BIT(22)
#define GMACSL_RX_ENABLE_EXT_CTL		BIT(18)
#define GMACSL_RX_ENABLE_GIG_FORCE		BIT(17)
#define GMACSL_RX_ENABLE_IFCTL_B		BIT(16)
#define GMACSL_RX_ENABLE_IFCTL_A		BIT(15)
#define GMACSL_RX_ENABLE_CMD_IDLE		BIT(11)
#define GMACSL_TX_ENABLE_SHORT_GAP		BIT(10)
#define GMACSL_ENABLE_GIG_MODE			BIT(7)
#define GMACSL_TX_ENABLE_PACE			BIT(6)
#define GMACSL_ENABLE				BIT(5)
#define GMACSL_TX_ENABLE_FLOW_CTL		BIT(4)
#define GMACSL_RX_ENABLE_FLOW_CTL		BIT(3)
#define GMACSL_ENABLE_LOOPBACK			BIT(1)
#define GMACSL_ENABLE_FULL_DUPLEX		BIT(0)

/* EMAC SL function return values */
#define GMACSL_RET_OK				0
#define GMACSL_RET_INVALID_PORT			-1
#define GMACSL_RET_WARN_RESET_INCOMPLETE	-2
#define GMACSL_RET_WARN_MAXLEN_TOO_BIG		-3
#define GMACSL_RET_CONFIG_FAIL_RESET_ACTIVE	-4

/* EMAC SL register definitions */
#define DEVICE_EMACSL_RESET_POLL_COUNT		100

/* Soft reset register values */
#define CPGMAC_REG_RESET_VAL_RESET_MASK		BIT(0)
#define CPGMAC_REG_RESET_VAL_RESET		BIT(0)
#define CPGMAC_REG_MAXLEN_LEN			0x3fff

/* CPSW */
/* Control bitfields */
#define CPSW_CTL_P2_PASS_PRI_TAGGED		BIT(5)
#define CPSW_CTL_P1_PASS_PRI_TAGGED		BIT(4)
#define CPSW_CTL_P0_PASS_PRI_TAGGED		BIT(3)
#define CPSW_CTL_P0_ENABLE			BIT(2)
#define CPSW_CTL_VLAN_AWARE			BIT(1)
#define CPSW_CTL_FIFO_LOOPBACK			BIT(0)

#define DEVICE_CPSW_NUM_PORTS			CONFIG_KSNET_CPSW_NUM_PORTS
#define DEVICE_N_GMACSL_PORTS			(DEVICE_CPSW_NUM_PORTS - 1)

#ifdef CONFIG_KSNET_NETCP_V1_0

#define DEVICE_CPSW_BASE			(GBETH_BASE + 0x800)
#define CPSW_REG_CTL				0x004
#define CPSW_REG_STAT_PORT_EN			0x00c
#define CPSW_REG_MAXLEN				0x040
#define CPSW_REG_ALE_CONTROL			0x608
#define CPSW_REG_ALE_PORTCTL(x)			(0x640 + (x) * 4)
#define CPSW_REG_VAL_STAT_ENABLE_ALL		0xf

#elif defined CONFIG_KSNET_NETCP_V1_5

#define DEVICE_CPSW_BASE			(GBETH_BASE + 0x20000)
#define CPSW_REG_CTL				0x00004
#define CPSW_REG_STAT_PORT_EN			0x00014
#define CPSW_REG_MAXLEN				0x01024
#define CPSW_REG_ALE_CONTROL			0x1e008
#define CPSW_REG_ALE_PORTCTL(x)			(0x1e040 + (x) * 4)
#define CPSW_REG_VAL_STAT_ENABLE_ALL		0x1ff

#endif

#define CPSW_REG_VAL_ALE_CTL_RESET_AND_ENABLE	((u_int32_t)0xc0000000)
#define CPSW_REG_VAL_ALE_CTL_BYPASS		((u_int32_t)0x00000010)
#define CPSW_REG_VAL_PORTCTL_FORWARD_MODE	0x3

#define target_get_switch_ctl()			CPSW_CTL_P0_ENABLE
#define SWITCH_MAX_PKT_SIZE			9000

/* SGMII */
#define SGMII_REG_STATUS_LOCK			BIT(4)
#define SGMII_REG_STATUS_LINK			BIT(0)
#define SGMII_REG_STATUS_AUTONEG		BIT(2)
#define SGMII_REG_CONTROL_AUTONEG		BIT(0)
#define SGMII_REG_CONTROL_MASTER		BIT(5)
#define SGMII_REG_MR_ADV_ENABLE			BIT(0)
#define SGMII_REG_MR_ADV_LINK			BIT(15)
#define SGMII_REG_MR_ADV_FULL_DUPLEX		BIT(12)
#define SGMII_REG_MR_ADV_GIG_MODE		BIT(11)

#define SGMII_LINK_MAC_MAC_AUTONEG		0
#define SGMII_LINK_MAC_PHY			1
#define SGMII_LINK_MAC_MAC_FORCED		2
#define SGMII_LINK_MAC_FIBER			3
#define SGMII_LINK_MAC_PHY_FORCED		4

#ifdef CONFIG_KSNET_NETCP_V1_0
#define SGMII_OFFSET(x)		((x <= 1) ? (x * 0x100) : ((x * 0x100) + 0x100))
#elif defined CONFIG_KSNET_NETCP_V1_5
#define SGMII_OFFSET(x)		((x) * 0x100)
#endif

#define SGMII_IDVER_REG(x)	(EMAC_SGMII_BASE_ADDR + SGMII_OFFSET(x) + 0x000)
#define SGMII_SRESET_REG(x)	(EMAC_SGMII_BASE_ADDR + SGMII_OFFSET(x) + 0x004)
#define SGMII_CTL_REG(x)	(EMAC_SGMII_BASE_ADDR + SGMII_OFFSET(x) + 0x010)
#define SGMII_STATUS_REG(x)	(EMAC_SGMII_BASE_ADDR + SGMII_OFFSET(x) + 0x014)
#define SGMII_MRADV_REG(x)	(EMAC_SGMII_BASE_ADDR + SGMII_OFFSET(x) + 0x018)
#define SGMII_LPADV_REG(x)	(EMAC_SGMII_BASE_ADDR + SGMII_OFFSET(x) + 0x020)
#define SGMII_TXCFG_REG(x)	(EMAC_SGMII_BASE_ADDR + SGMII_OFFSET(x) + 0x030)
#define SGMII_RXCFG_REG(x)	(EMAC_SGMII_BASE_ADDR + SGMII_OFFSET(x) + 0x034)
#define SGMII_AUXCFG_REG(x)	(EMAC_SGMII_BASE_ADDR + SGMII_OFFSET(x) + 0x038)

/* RGMII */
#define RGMII_REG_STATUS_LINK		BIT(0)

#define RGMII_STATUS_REG		(GBETH_BASE + 0x18)

/* PSS */
#ifdef CONFIG_KSNET_NETCP_V1_0

#define DEVICE_PSTREAM_CFG_REG_ADDR	(CONFIG_KSNET_NETCP_BASE + 0x604)
#define DEVICE_PSTREAM_CFG_VAL_ROUTE_CPPI	0x06060606
#define hw_config_streaming_switch()\
	writel(DEVICE_PSTREAM_CFG_VAL_ROUTE_CPPI, DEVICE_PSTREAM_CFG_REG_ADDR);

#elif defined CONFIG_KSNET_NETCP_V1_5

#define DEVICE_PSTREAM_CFG_REG_ADDR	(CONFIG_KSNET_NETCP_BASE + 0x500)
#define DEVICE_PSTREAM_CFG_VAL_ROUTE_CPPI	0x0

#define hw_config_streaming_switch()\
	writel(DEVICE_PSTREAM_CFG_VAL_ROUTE_CPPI,\
	       DEVICE_PSTREAM_CFG_REG_ADDR);\
	writel(DEVICE_PSTREAM_CFG_VAL_ROUTE_CPPI,\
	       DEVICE_PSTREAM_CFG_REG_ADDR+4);\
	writel(DEVICE_PSTREAM_CFG_VAL_ROUTE_CPPI,\
	       DEVICE_PSTREAM_CFG_REG_ADDR+8);\
	writel(DEVICE_PSTREAM_CFG_VAL_ROUTE_CPPI,\
	       DEVICE_PSTREAM_CFG_REG_ADDR+12);

#endif

/* EMAC MDIO Registers Structure */
struct mdio_regs {
	u32 version;
	u32 control;
	u32 alive;
	u32 link;
	u32 linkintraw;
	u32 linkintmasked;
	u32 rsvd0[2];
	u32 userintraw;
	u32 userintmasked;
	u32 userintmaskset;
	u32 userintmaskclear;
	u32 rsvd1[20];
	u32 useraccess0;
	u32 userphysel0;
	u32 useraccess1;
	u32 userphysel1;
};

#endif  /* _KEYSTONE_NET_H_ */
