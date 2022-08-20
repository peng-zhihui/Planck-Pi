/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Marvell 88E6xxx SERDES manipulation, via SMI bus
 *
 * Copyright (c) 2008 Marvell Semiconductor
 *
 * Copyright (c) 2016 Andrew Lunn <andrew@lunn.ch>
 */

#ifndef _MV88E6XXX_SERDES_H
#define _MV88E6XXX_SERDES_H

#include "chip.h"

#define MV88E6352_ADDR_SERDES		0x0f
#define MV88E6352_SERDES_PAGE_FIBER	0x01
#define MV88E6352_SERDES_IRQ		0x0b
#define MV88E6352_SERDES_INT_ENABLE	0x12
#define MV88E6352_SERDES_INT_SPEED_CHANGE	BIT(14)
#define MV88E6352_SERDES_INT_DUPLEX_CHANGE	BIT(13)
#define MV88E6352_SERDES_INT_PAGE_RX		BIT(12)
#define MV88E6352_SERDES_INT_AN_COMPLETE	BIT(11)
#define MV88E6352_SERDES_INT_LINK_CHANGE	BIT(10)
#define MV88E6352_SERDES_INT_SYMBOL_ERROR	BIT(9)
#define MV88E6352_SERDES_INT_FALSE_CARRIER	BIT(8)
#define MV88E6352_SERDES_INT_FIFO_OVER_UNDER	BIT(7)
#define MV88E6352_SERDES_INT_FIBRE_ENERGY	BIT(4)
#define MV88E6352_SERDES_INT_STATUS	0x13


#define MV88E6341_PORT5_LANE		0x15

#define MV88E6390_PORT9_LANE0		0x09
#define MV88E6390_PORT9_LANE1		0x12
#define MV88E6390_PORT9_LANE2		0x13
#define MV88E6390_PORT9_LANE3		0x14
#define MV88E6390_PORT10_LANE0		0x0a
#define MV88E6390_PORT10_LANE1		0x15
#define MV88E6390_PORT10_LANE2		0x16
#define MV88E6390_PORT10_LANE3		0x17

/* 10GBASE-R and 10GBASE-X4/X2 */
#define MV88E6390_PCS_CONTROL_1		0x1000
#define MV88E6390_PCS_CONTROL_1_RESET		BIT(15)
#define MV88E6390_PCS_CONTROL_1_LOOPBACK	BIT(14)
#define MV88E6390_PCS_CONTROL_1_SPEED		BIT(13)
#define MV88E6390_PCS_CONTROL_1_PDOWN		BIT(11)

/* 1000BASE-X and SGMII */
#define MV88E6390_SGMII_CONTROL		0x2000
#define MV88E6390_SGMII_CONTROL_RESET		BIT(15)
#define MV88E6390_SGMII_CONTROL_LOOPBACK	BIT(14)
#define MV88E6390_SGMII_CONTROL_PDOWN		BIT(11)
#define MV88E6390_SGMII_STATUS		0x2001
#define MV88E6390_SGMII_STATUS_AN_DONE		BIT(5)
#define MV88E6390_SGMII_STATUS_REMOTE_FAULT	BIT(4)
#define MV88E6390_SGMII_STATUS_LINK		BIT(2)
#define MV88E6390_SGMII_INT_ENABLE	0xa001
#define MV88E6390_SGMII_INT_SPEED_CHANGE	BIT(14)
#define MV88E6390_SGMII_INT_DUPLEX_CHANGE	BIT(13)
#define MV88E6390_SGMII_INT_PAGE_RX		BIT(12)
#define MV88E6390_SGMII_INT_AN_COMPLETE		BIT(11)
#define MV88E6390_SGMII_INT_LINK_DOWN		BIT(10)
#define MV88E6390_SGMII_INT_LINK_UP		BIT(9)
#define MV88E6390_SGMII_INT_SYMBOL_ERROR	BIT(8)
#define MV88E6390_SGMII_INT_FALSE_CARRIER	BIT(7)
#define MV88E6390_SGMII_INT_STATUS	0xa002
#define MV88E6390_SGMII_PHY_STATUS	0xa003
#define MV88E6390_SGMII_PHY_STATUS_SPEED_MASK	GENMASK(15, 14)
#define MV88E6390_SGMII_PHY_STATUS_SPEED_1000	0x8000
#define MV88E6390_SGMII_PHY_STATUS_SPEED_100	0x4000
#define MV88E6390_SGMII_PHY_STATUS_SPEED_10	0x0000
#define MV88E6390_SGMII_PHY_STATUS_DUPLEX_FULL	BIT(13)
#define MV88E6390_SGMII_PHY_STATUS_SPD_DPL_VALID BIT(11)
#define MV88E6390_SGMII_PHY_STATUS_LINK		BIT(10)

u8 mv88e6341_serdes_get_lane(struct mv88e6xxx_chip *chip, int port);
u8 mv88e6352_serdes_get_lane(struct mv88e6xxx_chip *chip, int port);
u8 mv88e6390_serdes_get_lane(struct mv88e6xxx_chip *chip, int port);
u8 mv88e6390x_serdes_get_lane(struct mv88e6xxx_chip *chip, int port);
unsigned int mv88e6352_serdes_irq_mapping(struct mv88e6xxx_chip *chip,
					  int port);
unsigned int mv88e6390_serdes_irq_mapping(struct mv88e6xxx_chip *chip,
					  int port);
int mv88e6352_serdes_power(struct mv88e6xxx_chip *chip, int port, u8 lane,
			   bool on);
int mv88e6390_serdes_power(struct mv88e6xxx_chip *chip, int port, u8 lane,
			   bool on);
int mv88e6352_serdes_irq_enable(struct mv88e6xxx_chip *chip, int port, u8 lane,
				bool enable);
int mv88e6390_serdes_irq_enable(struct mv88e6xxx_chip *chip, int port, u8 lane,
				bool enable);
irqreturn_t mv88e6352_serdes_irq_status(struct mv88e6xxx_chip *chip, int port,
					u8 lane);
irqreturn_t mv88e6390_serdes_irq_status(struct mv88e6xxx_chip *chip, int port,
					u8 lane);
int mv88e6352_serdes_get_sset_count(struct mv88e6xxx_chip *chip, int port);
int mv88e6352_serdes_get_strings(struct mv88e6xxx_chip *chip,
				 int port, uint8_t *data);
int mv88e6352_serdes_get_stats(struct mv88e6xxx_chip *chip, int port,
			       uint64_t *data);

/* Return the (first) SERDES lane address a port is using, 0 otherwise. */
static inline u8 mv88e6xxx_serdes_get_lane(struct mv88e6xxx_chip *chip,
					   int port)
{
	if (!chip->info->ops->serdes_get_lane)
		return 0;

	return chip->info->ops->serdes_get_lane(chip, port);
}

static inline int mv88e6xxx_serdes_power_up(struct mv88e6xxx_chip *chip,
					    int port, u8 lane)
{
	if (!chip->info->ops->serdes_power)
		return -EOPNOTSUPP;

	return chip->info->ops->serdes_power(chip, port, lane, true);
}

static inline int mv88e6xxx_serdes_power_down(struct mv88e6xxx_chip *chip,
					      int port, u8 lane)
{
	if (!chip->info->ops->serdes_power)
		return -EOPNOTSUPP;

	return chip->info->ops->serdes_power(chip, port, lane, false);
}

static inline unsigned int
mv88e6xxx_serdes_irq_mapping(struct mv88e6xxx_chip *chip, int port)
{
	if (!chip->info->ops->serdes_irq_mapping)
		return 0;

	return chip->info->ops->serdes_irq_mapping(chip, port);
}

static inline int mv88e6xxx_serdes_irq_enable(struct mv88e6xxx_chip *chip,
					      int port, u8 lane)
{
	if (!chip->info->ops->serdes_irq_enable)
		return -EOPNOTSUPP;

	return chip->info->ops->serdes_irq_enable(chip, port, lane, true);
}

static inline int mv88e6xxx_serdes_irq_disable(struct mv88e6xxx_chip *chip,
					       int port, u8 lane)
{
	if (!chip->info->ops->serdes_irq_enable)
		return -EOPNOTSUPP;

	return chip->info->ops->serdes_irq_enable(chip, port, lane, false);
}

static inline irqreturn_t
mv88e6xxx_serdes_irq_status(struct mv88e6xxx_chip *chip, int port, u8 lane)
{
	if (!chip->info->ops->serdes_irq_status)
		return IRQ_NONE;

	return chip->info->ops->serdes_irq_status(chip, port, lane);
}

#endif
