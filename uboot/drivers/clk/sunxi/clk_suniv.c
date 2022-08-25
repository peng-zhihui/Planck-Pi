// SPDX-License-Identifier: (GPL-2.0+ OR MIT)

#include <common.h>
#include <clk-uclass.h>
#include <dm.h>
#include <errno.h>
#include <asm/arch/ccu.h>
#include <dt-bindings/clock/suniv-ccu.h>
#include <dt-bindings/reset/suniv-ccu.h>
#include <linux/bitops.h>

static struct ccu_clk_gate suniv_gates[] = {
	[CLK_BUS_MMC0]		= GATE(0x060, BIT(8)),
	[CLK_BUS_MMC1]		= GATE(0x060, BIT(9)),

	[CLK_BUS_SPI0]		= GATE(0x060, BIT(20)),
	[CLK_BUS_SPI1]		= GATE(0x060, BIT(21)),
	[CLK_BUS_OTG]		= GATE(0x060, BIT(24)),

	[CLK_USB_PHY0]		= GATE(0x0cc, BIT(1)),
};

static struct ccu_reset suniv_resets[] = {
	[RST_USB_PHY0]		=  RESET(0x0cc, BIT(0)),

	[RST_BUS_DMA]		=  RESET(0x2c0, BIT(6)),
	[RST_BUS_MMC0]		=  RESET(0x2c0, BIT(8)),
	[RST_BUS_MMC1]		=  RESET(0x2c0, BIT(9)),
	[RST_BUS_DRAM]		=  RESET(0x2c0, BIT(14)),
	[RST_BUS_SPI0]		=  RESET(0x2c0, BIT(20)),
	[RST_BUS_SPI1]		=  RESET(0x2c0, BIT(21)),
	[RST_BUS_OTG]		=  RESET(0x2c0, BIT(24)),

	[RST_BUS_VE]		=  RESET(0x2c4, BIT(0)),
	[RST_BUS_LCD]		=  RESET(0x2c4, BIT(4)),
	[RST_BUS_DEINTERLACE]	=  RESET(0x2c4, BIT(5)),
	[RST_BUS_CSI]		=  RESET(0x2c4, BIT(8)),
	[RST_BUS_TVD]		=  RESET(0x2c4, BIT(9)),
	[RST_BUS_TVE]		=  RESET(0x2c4, BIT(10)),
	[RST_BUS_DE_BE]		=  RESET(0x2c4, BIT(12)),
	[RST_BUS_DE_FE]		=  RESET(0x2c4, BIT(14)),

	[RST_BUS_CODEC]		=  RESET(0x2d0, BIT(0)),
	[RST_BUS_SPDIF]		=  RESET(0x2d0, BIT(1)),
	[RST_BUS_IR]		=  RESET(0x2d0, BIT(2)),
	[RST_BUS_RSB]		=  RESET(0x2d0, BIT(3)),
	[RST_BUS_I2S0]		=  RESET(0x2d0, BIT(12)),
	[RST_BUS_I2C0]		=  RESET(0x2d0, BIT(16)),
	[RST_BUS_I2C1]		=  RESET(0x2d0, BIT(17)),
	[RST_BUS_I2C2]		=  RESET(0x2d0, BIT(18)),
	[RST_BUS_UART0]		=  RESET(0x2d0, BIT(20)),
	[RST_BUS_UART1]		=  RESET(0x2d0, BIT(21)),
	[RST_BUS_UART2]		=  RESET(0x2d0, BIT(22)),
};

static const struct ccu_desc suniv_ccu_desc = {
	.gates = suniv_gates,
	.resets = suniv_resets,
};

static int suniv_clk_bind(struct udevice *dev)
{
	return sunxi_reset_bind(dev, ARRAY_SIZE(suniv_resets));
}

static const struct udevice_id suniv_ccu_ids[] = {
	{ .compatible = "allwinner,suniv-ccu",
	  .data = (ulong)&suniv_ccu_desc },
	{ }
};

U_BOOT_DRIVER(clk_suniv) = {
	.name		= "suniv_ccu",
	.id		= UCLASS_CLK,
	.of_match	= suniv_ccu_ids,
	.priv_auto_alloc_size	= sizeof(struct ccu_priv),
	.ops		= &sunxi_clk_ops,
	.probe		= sunxi_clk_probe,
	.bind		= suniv_clk_bind,
};
