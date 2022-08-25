/*
 * Copyright (C) 2013, Boundary Devices <info@boundarydevices.com>
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., http://www.fsf.org/about/contact/
 *
 */

#include <common.h>
#include <env.h>
#include <splash.h>
#include <lcd.h>

static struct splash_location default_splash_locations[] = {
	{
		.name = "sf",
		.storage = SPLASH_STORAGE_SF,
		.flags = SPLASH_STORAGE_RAW,
		.offset = 0x0,
	},
	{
		.name = "mmc_fs",
		.storage = SPLASH_STORAGE_MMC,
		.flags = SPLASH_STORAGE_FS,
		.devpart = "0:1",
	},
	{
		.name = "usb_fs",
		.storage = SPLASH_STORAGE_USB,
		.flags = SPLASH_STORAGE_FS,
		.devpart = "0:1",
	},
	{
		.name = "sata_fs",
		.storage = SPLASH_STORAGE_SATA,
		.flags = SPLASH_STORAGE_FS,
		.devpart = "0:1",
	},
};

#if defined(CONFIG_DM_VIDEO) && defined(CONFIG_VIDEO_LOGO)

#include <bmp_logo_data.h>

static int splash_video_logo_load(void)
{
	char *splashimage;
	ulong bmp_load_addr;

	splashimage = env_get("splashimage");
	if (!splashimage)
		return -ENOENT;

	bmp_load_addr = simple_strtoul(splashimage, 0, 16);
	if (!bmp_load_addr) {
		printf("Error: bad 'splashimage' address\n");
		return -EFAULT;
	}

	memcpy((void *)bmp_load_addr, bmp_logo_bitmap,
	       ARRAY_SIZE(bmp_logo_bitmap));

	return 0;
}
#else
static inline int splash_video_logo_load(void) { return -ENOSYS; }
#endif

__weak int splash_screen_prepare(void)
{
	if (CONFIG_IS_ENABLED(SPLASH_SOURCE))
		return splash_source_load(default_splash_locations,
					  ARRAY_SIZE(default_splash_locations));

	return splash_video_logo_load();
}

#ifdef CONFIG_SPLASH_SCREEN_ALIGN
void splash_get_pos(int *x, int *y)
{
	char *s = env_get("splashpos");

	if (!s)
		return;

	if (s[0] == 'm')
		*x = BMP_ALIGN_CENTER;
	else
		*x = simple_strtol(s, NULL, 0);

	s = strchr(s + 1, ',');
	if (s != NULL) {
		if (s[1] == 'm')
			*y = BMP_ALIGN_CENTER;
		else
			*y = simple_strtol(s + 1, NULL, 0);
	}
}
#endif /* CONFIG_SPLASH_SCREEN_ALIGN */

#if defined(CONFIG_DM_VIDEO) && !defined(CONFIG_HIDE_LOGO_VERSION)

#ifdef CONFIG_VIDEO_LOGO
#include <bmp_logo.h>
#endif
#include <dm.h>
#include <video_console.h>
#include <video_font.h>

void splash_display_banner(void)
{
	struct udevice *dev;
	char buf[DISPLAY_OPTIONS_BANNER_LENGTH];
	int col, row, ret;

	ret = uclass_get_device(UCLASS_VIDEO_CONSOLE, 0, &dev);
	if (ret)
		return;

#ifdef CONFIG_VIDEO_LOGO
	col = BMP_LOGO_WIDTH / VIDEO_FONT_WIDTH + 1;
	row = BMP_LOGO_HEIGHT / VIDEO_FONT_HEIGHT + 1;
#else
	col = 0;
	row = 0;
#endif

	display_options_get_banner(false, buf, sizeof(buf));
	vidconsole_position_cursor(dev, col, 1);
	vidconsole_put_string(dev, buf);
	vidconsole_position_cursor(dev, 0, row);
}
#endif /* CONFIG_DM_VIDEO && !CONFIG_HIDE_LOGO_VERSION */

/*
 * Common function to show a splash image if env("splashimage") is set.
 * Is used for both dm_video and lcd video stacks. For additional
 * details please refer to doc/README.splashprepare.
 */
#if defined(CONFIG_SPLASH_SCREEN) && defined(CONFIG_CMD_BMP)
int splash_display(void)
{
	ulong addr;
	char *s;
	int x = 0, y = 0, ret;

	s = env_get("splashimage");
	if (!s)
		return -EINVAL;

	addr = simple_strtoul(s, NULL, 16);
	ret = splash_screen_prepare();
	if (ret)
		return ret;

	splash_get_pos(&x, &y);

	ret = bmp_display(addr, x, y);

	/* Skip banner output on video console if the logo is not at 0,0 */
	if (x || y)
		goto end;

#if defined(CONFIG_DM_VIDEO) && !defined(CONFIG_HIDE_LOGO_VERSION)
	splash_display_banner();
#endif
end:
	return ret;
}
#endif
