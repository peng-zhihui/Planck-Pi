// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#include <dm.h>
#include <env.h>
#include <init.h>
#include <log.h>
#include <linux/bug.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <misc.h>
#include <spl.h>
#include <asm/arch/cache.h>

/*
 * This define is a value used for error/unknown serial.
 * If we really care about distinguishing errors and 0 is
 * valid, we'll need a different one.
 */
#define ERROR_READING_SERIAL_NUMBER       0

#ifdef CONFIG_MISC_INIT_R

#if CONFIG_IS_ENABLED(SIFIVE_OTP)
static u32 otp_read_serialnum(struct udevice *dev)
{
	int ret;
	u32 serial[2] = {0};

	for (int i = 0xfe * 4; i > 0; i -= 8) {
		ret = misc_read(dev, i, serial, sizeof(serial));

		if (ret != sizeof(serial)) {
			printf("%s: error reading serial from OTP\n", __func__);
			break;
		}

		if (serial[0] == ~serial[1])
			return serial[0];
	}

	return ERROR_READING_SERIAL_NUMBER;
}
#endif

static u32 fu540_read_serialnum(void)
{
	u32 serial = ERROR_READING_SERIAL_NUMBER;

#if CONFIG_IS_ENABLED(SIFIVE_OTP)
	struct udevice *dev;
	int ret;

	/* init OTP */
	ret = uclass_get_device_by_driver(UCLASS_MISC,
					  DM_GET_DRIVER(sifive_otp), &dev);

	if (ret) {
		debug("%s: could not find otp device\n", __func__);
		return serial;
	}

	/* read serial from OTP and set env var */
	serial = otp_read_serialnum(dev);
#endif

	return serial;
}

static void fu540_setup_macaddr(u32 serialnum)
{
	/* Default MAC address */
	unsigned char mac[6] = { 0x70, 0xb3, 0xd5, 0x92, 0xf0, 0x00 };

	/*
	 * We derive our board MAC address by ORing last three bytes
	 * of board serial number to above default MAC address.
	 *
	 * This logic of deriving board MAC address is taken from
	 * SiFive FSBL and is kept unchanged.
	 */
	mac[5] |= (serialnum >>  0) & 0xff;
	mac[4] |= (serialnum >>  8) & 0xff;
	mac[3] |= (serialnum >> 16) & 0xff;

	/* Update environment variable */
	eth_env_set_enetaddr("ethaddr", mac);
}

int misc_init_r(void)
{
	u32 serial_num;
	char buf[9] = {0};

	/* Set ethaddr environment variable from board serial number */
	if (!env_get("serial#")) {
		serial_num = fu540_read_serialnum();
		if (!serial_num) {
			WARN(true, "Board serial number should not be 0 !!\n");
			return 0;
		}
		snprintf(buf, sizeof(buf), "%08x", serial_num);
		env_set("serial#", buf);
		fu540_setup_macaddr(serial_num);
	}
	return 0;
}

#endif

int board_init(void)
{
	int ret;

	/* enable all cache ways */
	ret = cache_enable_ways();
	if (ret) {
		debug("%s: could not enable cache ways\n", __func__);
		return ret;
	}

	return 0;
}

#ifdef CONFIG_SPL
u32 spl_boot_device(void)
{
#ifdef CONFIG_SPL_MMC_SUPPORT
	return BOOT_DEVICE_MMC1;
#else
	puts("Unknown boot device\n");
	hang();
#endif
}
#endif

#ifdef CONFIG_SPL_LOAD_FIT
int board_fit_config_name_match(const char *name)
{
	/* boot using first FIT config */
	return 0;
}
#endif
