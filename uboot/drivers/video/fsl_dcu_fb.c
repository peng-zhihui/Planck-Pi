// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2014 Freescale Semiconductor, Inc.
 * Copyright 2019 Toradex AG
 *
 * FSL DCU Framebuffer driver
 */

#include <init.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <common.h>
#include <dm.h>
#include <fdt_support.h>
#include <fsl_dcu_fb.h>
#include <linux/fb.h>
#include <malloc.h>
#include <video.h>
#include <video_fb.h>
#include "videomodes.h"

/* Convert the X,Y resolution pair into a single number */
#define RESOLUTION(x, y) (((u32)(x) << 16) | (y))

#ifdef CONFIG_SYS_FSL_DCU_LE
#define	dcu_read32	in_le32
#define	dcu_write32	out_le32
#elif defined(CONFIG_SYS_FSL_DCU_BE)
#define	dcu_read32	in_be32
#define	dcu_write32	out_be32
#endif

#define DCU_MODE_BLEND_ITER(x)          ((x) << 20)
#define DCU_MODE_RASTER_EN		(1 << 14)
#define DCU_MODE_NORMAL			1
#define DCU_MODE_COLORBAR               3
#define DCU_BGND_R(x)			((x) << 16)
#define DCU_BGND_G(x)			((x) << 8)
#define DCU_BGND_B(x)			(x)
#define DCU_DISP_SIZE_DELTA_Y(x)	((x) << 16)
#define DCU_DISP_SIZE_DELTA_X(x)	(x)
#define DCU_HSYN_PARA_BP(x)		((x) << 22)
#define DCU_HSYN_PARA_PW(x)		((x) << 11)
#define DCU_HSYN_PARA_FP(x)		(x)
#define DCU_VSYN_PARA_BP(x)		((x) << 22)
#define DCU_VSYN_PARA_PW(x)		((x) << 11)
#define DCU_VSYN_PARA_FP(x)		(x)
#define DCU_SYN_POL_INV_PXCK_FALL	(1 << 6)
#define DCU_SYN_POL_NEG_REMAIN		(0 << 5)
#define DCU_SYN_POL_INV_VS_LOW		(1 << 1)
#define DCU_SYN_POL_INV_HS_LOW		(1)
#define DCU_THRESHOLD_LS_BF_VS(x)	((x) << 16)
#define DCU_THRESHOLD_OUT_BUF_HIGH(x)	((x) << 8)
#define DCU_THRESHOLD_OUT_BUF_LOW(x)	(x)
#define DCU_UPDATE_MODE_MODE            (1 << 31)
#define DCU_UPDATE_MODE_READREG         (1 << 30)

#define DCU_CTRLDESCLN_1_HEIGHT(x)	((x) << 16)
#define DCU_CTRLDESCLN_1_WIDTH(x)	(x)
#define DCU_CTRLDESCLN_2_POSY(x)	((x) << 16)
#define DCU_CTRLDESCLN_2_POSX(x)	(x)
#define DCU_CTRLDESCLN_4_EN		(1 << 31)
#define DCU_CTRLDESCLN_4_TILE_EN	(1 << 30)
#define DCU_CTRLDESCLN_4_DATA_SEL_CLUT	(1 << 29)
#define DCU_CTRLDESCLN_4_SAFETY_EN	(1 << 28)
#define DCU_CTRLDESCLN_4_TRANS(x)	((x) << 20)
#define DCU_CTRLDESCLN_4_BPP(x)		((x) << 16)
#define DCU_CTRLDESCLN_4_RLE_EN		(1 << 15)
#define DCU_CTRLDESCLN_4_LUOFFS(x)	((x) << 4)
#define DCU_CTRLDESCLN_4_BB_ON		(1 << 2)
#define DCU_CTRLDESCLN_4_AB(x)		(x)
#define DCU_CTRLDESCLN_5_CKMAX_R(x)	((x) << 16)
#define DCU_CTRLDESCLN_5_CKMAX_G(x)	((x) << 8)
#define DCU_CTRLDESCLN_5_CKMAX_B(x)	(x)
#define DCU_CTRLDESCLN_6_CKMIN_R(x)	((x) << 16)
#define DCU_CTRLDESCLN_6_CKMIN_G(x)	((x) << 8)
#define DCU_CTRLDESCLN_6_CKMIN_B(x)	(x)
#define DCU_CTRLDESCLN_7_TILE_VER(x)	((x) << 16)
#define DCU_CTRLDESCLN_7_TILE_HOR(x)	(x)
#define DCU_CTRLDESCLN_8_FG_FCOLOR(x)	(x)
#define DCU_CTRLDESCLN_9_BG_BCOLOR(x)	(x)

#define BPP_16_RGB565			4
#define BPP_24_RGB888			5
#define BPP_32_ARGB8888			6

DECLARE_GLOBAL_DATA_PTR;

/*
 * This setting is used for the TWR_LCD_RGB card
 */
static struct fb_videomode fsl_dcu_mode_480_272 = {
	.name		= "480x272-60",
	.refresh	= 60,
	.xres		= 480,
	.yres		= 272,
	.pixclock	= 91996,
	.left_margin	= 2,
	.right_margin	= 2,
	.upper_margin	= 1,
	.lower_margin	= 1,
	.hsync_len	= 41,
	.vsync_len	= 2,
	.sync		= FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	.vmode		= FB_VMODE_NONINTERLACED
};

/*
 * This setting is used for Siliconimage SiI9022A HDMI
 */
static struct fb_videomode fsl_dcu_cea_mode_640_480 = {
	.name		= "640x480-60",
	.refresh	= 60,
	.xres		= 640,
	.yres		= 480,
	.pixclock	= 39722,
	.left_margin	= 48,
	.right_margin	= 16,
	.upper_margin	= 33,
	.lower_margin	= 10,
	.hsync_len	= 96,
	.vsync_len	= 2,
	.sync		= 0,
	.vmode		= FB_VMODE_NONINTERLACED,
};

static struct fb_videomode fsl_dcu_mode_640_480 = {
	.name		= "640x480-60",
	.refresh	= 60,
	.xres		= 640,
	.yres		= 480,
	.pixclock	= 25175,
	.left_margin	= 40,
	.right_margin	= 24,
	.upper_margin	= 32,
	.lower_margin	= 11,
	.hsync_len	= 96,
	.vsync_len	= 2,
	.sync		= 0,
	.vmode		= FB_VMODE_NONINTERLACED,
};

static struct fb_videomode fsl_dcu_mode_800_480 = {
	.name		= "800x480-60",
	.refresh	= 60,
	.xres		= 800,
	.yres		= 480,
	.pixclock	= 33260,
	.left_margin	= 216,
	.right_margin	= 40,
	.upper_margin	= 35,
	.lower_margin	= 10,
	.hsync_len	= 128,
	.vsync_len	= 2,
	.sync		= 0,
	.vmode		= FB_VMODE_NONINTERLACED,
};

static struct fb_videomode fsl_dcu_mode_1024_600 = {
	.name		= "1024x600-60",
	.refresh	= 60,
	.xres		= 1024,
	.yres		= 600,
	.pixclock	= 48000,
	.left_margin	= 104,
	.right_margin	= 43,
	.upper_margin	= 24,
	.lower_margin	= 20,
	.hsync_len	= 5,
	.vsync_len	= 5,
	.sync		= 0,
	.vmode		= FB_VMODE_NONINTERLACED,
};

/*
 * DCU register map
 */
struct dcu_reg {
	u32 desc_cursor[4];
	u32 mode;
	u32 bgnd;
	u32 disp_size;
	u32 hsyn_para;
	u32 vsyn_para;
	u32 synpol;
	u32 threshold;
	u32 int_status;
	u32 int_mask;
	u32 colbar[8];
	u32 div_ratio;
	u32 sign_calc[2];
	u32 crc_val;
	u8 res_064[0x6c-0x64];
	u32 parr_err_status1;
	u8 res_070[0x7c-0x70];
	u32 parr_err_status3;
	u32 mparr_err_status1;
	u8 res_084[0x90-0x84];
	u32 mparr_err_status3;
	u32 threshold_inp_buf[2];
	u8 res_09c[0xa0-0x9c];
	u32 luma_comp;
	u32 chroma_red;
	u32 chroma_green;
	u32 chroma_blue;
	u32 crc_pos;
	u32 lyr_intpol_en;
	u32 lyr_luma_comp;
	u32 lyr_chrm_red;
	u32 lyr_chrm_grn;
	u32 lyr_chrm_blue;
	u8 res_0c4[0xcc-0xc8];
	u32 update_mode;
	u32 underrun;
	u8 res_0d4[0x100-0xd4];
	u32 gpr;
	u32 slr_l[2];
	u32 slr_disp_size;
	u32 slr_hvsync_para;
	u32 slr_pol;
	u32 slr_l_transp[2];
	u8 res_120[0x200-0x120];
	u32 ctrldescl[DCU_LAYER_MAX_NUM][16];
};

static void reset_total_layers(void)
{
	struct dcu_reg *regs = (struct dcu_reg *)CONFIG_SYS_DCU_ADDR;
	int i;

	for (i = 0; i < DCU_LAYER_MAX_NUM; i++) {
		dcu_write32(&regs->ctrldescl[i][0], 0);
		dcu_write32(&regs->ctrldescl[i][1], 0);
		dcu_write32(&regs->ctrldescl[i][2], 0);
		dcu_write32(&regs->ctrldescl[i][3], 0);
		dcu_write32(&regs->ctrldescl[i][4], 0);
		dcu_write32(&regs->ctrldescl[i][5], 0);
		dcu_write32(&regs->ctrldescl[i][6], 0);
		dcu_write32(&regs->ctrldescl[i][7], 0);
		dcu_write32(&regs->ctrldescl[i][8], 0);
		dcu_write32(&regs->ctrldescl[i][9], 0);
		dcu_write32(&regs->ctrldescl[i][10], 0);
	}
}

static int layer_ctrldesc_init(struct fb_info fbinfo,
			       int index, u32 pixel_format)
{
	struct dcu_reg *regs = (struct dcu_reg *)CONFIG_SYS_DCU_ADDR;
	unsigned int bpp = BPP_24_RGB888;

	dcu_write32(&regs->ctrldescl[index][0],
		    DCU_CTRLDESCLN_1_HEIGHT(fbinfo.var.yres) |
		    DCU_CTRLDESCLN_1_WIDTH(fbinfo.var.xres));

	dcu_write32(&regs->ctrldescl[index][1],
		    DCU_CTRLDESCLN_2_POSY(0) |
		    DCU_CTRLDESCLN_2_POSX(0));

	dcu_write32(&regs->ctrldescl[index][2],
		    (unsigned int)fbinfo.screen_base);

	switch (pixel_format) {
	case 16:
		bpp = BPP_16_RGB565;
		break;
	case 24:
		bpp = BPP_24_RGB888;
		break;
	case 32:
		bpp = BPP_32_ARGB8888;
		break;
	default:
		printf("unsupported color depth: %u\n", pixel_format);
	}

	dcu_write32(&regs->ctrldescl[index][3],
		    DCU_CTRLDESCLN_4_EN |
		    DCU_CTRLDESCLN_4_TRANS(0xff) |
		    DCU_CTRLDESCLN_4_BPP(bpp) |
		    DCU_CTRLDESCLN_4_AB(0));

	dcu_write32(&regs->ctrldescl[index][4],
		    DCU_CTRLDESCLN_5_CKMAX_R(0xff) |
		    DCU_CTRLDESCLN_5_CKMAX_G(0xff) |
		    DCU_CTRLDESCLN_5_CKMAX_B(0xff));
	dcu_write32(&regs->ctrldescl[index][5],
		    DCU_CTRLDESCLN_6_CKMIN_R(0) |
		    DCU_CTRLDESCLN_6_CKMIN_G(0) |
		    DCU_CTRLDESCLN_6_CKMIN_B(0));

	dcu_write32(&regs->ctrldescl[index][6],
		    DCU_CTRLDESCLN_7_TILE_VER(0) |
		    DCU_CTRLDESCLN_7_TILE_HOR(0));

	dcu_write32(&regs->ctrldescl[index][7], DCU_CTRLDESCLN_8_FG_FCOLOR(0));
	dcu_write32(&regs->ctrldescl[index][8], DCU_CTRLDESCLN_9_BG_BCOLOR(0));

	return 0;
}

int fsl_dcu_init(struct fb_info *fbinfo, unsigned int xres,
		 unsigned int yres, unsigned int pixel_format)
{
	struct dcu_reg *regs = (struct dcu_reg *)CONFIG_SYS_DCU_ADDR;
	unsigned int div, mode;
/*
 * When DM_VIDEO is enabled reservation of framebuffer is done
 * in advance during bind() call.
 */
#if !CONFIG_IS_ENABLED(DM_VIDEO)
	fbinfo->screen_size = fbinfo->var.xres * fbinfo->var.yres *
			     (fbinfo->var.bits_per_pixel / 8);

	if (fbinfo->screen_size > CONFIG_VIDEO_FSL_DCU_MAX_FB_SIZE_MB) {
		fbinfo->screen_size = 0;
		return -ENOMEM;
	}
	/* Reserve framebuffer at the end of memory */
	gd->fb_base = gd->bd->bi_dram[0].start +
			gd->bd->bi_dram[0].size - fbinfo->screen_size;
	fbinfo->screen_base = (char *)gd->fb_base;

	memset(fbinfo->screen_base, 0, fbinfo->screen_size);
#endif

	reset_total_layers();

	dcu_write32(&regs->disp_size,
		    DCU_DISP_SIZE_DELTA_Y(fbinfo->var.yres) |
		    DCU_DISP_SIZE_DELTA_X(fbinfo->var.xres / 16));

	dcu_write32(&regs->hsyn_para,
		    DCU_HSYN_PARA_BP(fbinfo->var.left_margin) |
		    DCU_HSYN_PARA_PW(fbinfo->var.hsync_len) |
		    DCU_HSYN_PARA_FP(fbinfo->var.right_margin));

	dcu_write32(&regs->vsyn_para,
		    DCU_VSYN_PARA_BP(fbinfo->var.upper_margin) |
		    DCU_VSYN_PARA_PW(fbinfo->var.vsync_len) |
		    DCU_VSYN_PARA_FP(fbinfo->var.lower_margin));

	dcu_write32(&regs->synpol,
		    DCU_SYN_POL_INV_PXCK_FALL |
		    DCU_SYN_POL_NEG_REMAIN |
		    DCU_SYN_POL_INV_VS_LOW |
		    DCU_SYN_POL_INV_HS_LOW);

	dcu_write32(&regs->bgnd,
		    DCU_BGND_R(0) | DCU_BGND_G(0) | DCU_BGND_B(0));

	dcu_write32(&regs->mode,
		    DCU_MODE_BLEND_ITER(2) |
		    DCU_MODE_RASTER_EN);

	dcu_write32(&regs->threshold,
		    DCU_THRESHOLD_LS_BF_VS(0x3) |
		    DCU_THRESHOLD_OUT_BUF_HIGH(0x78) |
		    DCU_THRESHOLD_OUT_BUF_LOW(0));

	mode = dcu_read32(&regs->mode);
	dcu_write32(&regs->mode, mode | DCU_MODE_NORMAL);

	layer_ctrldesc_init(*fbinfo, 0, pixel_format);

	div = dcu_set_pixel_clock(fbinfo->var.pixclock);
	dcu_write32(&regs->div_ratio, (div - 1));

	dcu_write32(&regs->update_mode, DCU_UPDATE_MODE_READREG);

	return 0;
}

ulong board_get_usable_ram_top(ulong total_size)
{
	return gd->ram_top - CONFIG_VIDEO_FSL_DCU_MAX_FB_SIZE_MB;
}

int fsl_probe_common(struct fb_info *fbinfo, unsigned int *win_x,
		     unsigned int *win_y)
{
	const char *options;
	unsigned int depth = 0, freq = 0;

	struct fb_videomode *fsl_dcu_mode_db = &fsl_dcu_mode_480_272;

	if (!video_get_video_mode(win_x, win_y, &depth, &freq,
				  &options))
		return -EINVAL;

	/* Find the monitor port, which is a required option */
	if (!options)
		return -EINVAL;

	if (strncmp(options, "monitor=", 8) != 0)
		return -EINVAL;

	switch (RESOLUTION(*win_x, *win_y)) {
	case RESOLUTION(480, 272):
		fsl_dcu_mode_db = &fsl_dcu_mode_480_272;
		break;
	case RESOLUTION(640, 480):
		if (!strncmp(options, "monitor=hdmi", 12))
			fsl_dcu_mode_db = &fsl_dcu_cea_mode_640_480;
		else
			fsl_dcu_mode_db = &fsl_dcu_mode_640_480;
		break;
	case RESOLUTION(800, 480):
		fsl_dcu_mode_db = &fsl_dcu_mode_800_480;
		break;
	case RESOLUTION(1024, 600):
		fsl_dcu_mode_db = &fsl_dcu_mode_1024_600;
		break;
	default:
		printf("unsupported resolution %ux%u\n",
		       *win_x, *win_y);
	}

	fbinfo->var.xres = fsl_dcu_mode_db->xres;
	fbinfo->var.yres = fsl_dcu_mode_db->yres;
	fbinfo->var.bits_per_pixel = 32;
	fbinfo->var.pixclock = fsl_dcu_mode_db->pixclock;
	fbinfo->var.left_margin = fsl_dcu_mode_db->left_margin;
	fbinfo->var.right_margin = fsl_dcu_mode_db->right_margin;
	fbinfo->var.upper_margin = fsl_dcu_mode_db->upper_margin;
	fbinfo->var.lower_margin = fsl_dcu_mode_db->lower_margin;
	fbinfo->var.hsync_len = fsl_dcu_mode_db->hsync_len;
	fbinfo->var.vsync_len = fsl_dcu_mode_db->vsync_len;
	fbinfo->var.sync = fsl_dcu_mode_db->sync;
	fbinfo->var.vmode = fsl_dcu_mode_db->vmode;
	fbinfo->fix.line_length = fbinfo->var.xres *
				  fbinfo->var.bits_per_pixel / 8;

	return platform_dcu_init(fbinfo, *win_x, *win_y,
				 options + 8, fsl_dcu_mode_db);
}

#ifndef CONFIG_DM_VIDEO
static struct fb_info info;

#if defined(CONFIG_OF_BOARD_SETUP)
int fsl_dcu_fixedfb_setup(void *blob)
{
	u64 start, size;
	int ret;

	start = gd->bd->bi_dram[0].start;
	size = gd->bd->bi_dram[0].size - info.screen_size;

	/*
	 * Align size on section size (1 MiB).
	 */
	size &= 0xfff00000;
	ret = fdt_fixup_memory_banks(blob, &start, &size, 1);
	if (ret) {
		eprintf("Cannot setup fb: Error reserving memory\n");
		return ret;
	}

	return 0;
}
#endif

void *video_hw_init(void)
{
	static GraphicDevice ctfb;

	if (fsl_probe_common(&info, &ctfb.winSizeX, &ctfb.winSizeY) < 0)
		return NULL;

	ctfb.frameAdrs = (unsigned int)info.screen_base;
	ctfb.plnSizeX = ctfb.winSizeX;
	ctfb.plnSizeY = ctfb.winSizeY;

	ctfb.gdfBytesPP = 4;
	ctfb.gdfIndex = GDF_32BIT_X888RGB;

	ctfb.memSize = info.screen_size;

	return &ctfb;
}

#else /* ifndef CONFIG_DM_VIDEO */

static int fsl_dcu_video_probe(struct udevice *dev)
{
	struct video_uc_platdata *plat = dev_get_uclass_platdata(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct fb_info fbinfo = { 0 };
	unsigned int win_x;
	unsigned int win_y;
	u32 fb_start, fb_end;
	int ret = 0;

	fb_start = plat->base & ~(MMU_SECTION_SIZE - 1);
	fb_end = plat->base + plat->size;
	fb_end = ALIGN(fb_end, 1 << MMU_SECTION_SHIFT);

	fbinfo.screen_base = (char *)fb_start;
	fbinfo.screen_size = plat->size;

	ret = fsl_probe_common(&fbinfo, &win_x, &win_y);
	if (ret < 0)
		return ret;

	uc_priv->bpix = VIDEO_BPP32;
	uc_priv->xsize = win_x;
	uc_priv->ysize = win_y;

	/* Enable dcache for the frame buffer */
	mmu_set_region_dcache_behaviour(fb_start, fb_end - fb_start,
					DCACHE_WRITEBACK);
	video_set_flush_dcache(dev, true);
	return ret;
}

static int fsl_dcu_video_bind(struct udevice *dev)
{
	struct video_uc_platdata *plat = dev_get_uclass_platdata(dev);
	unsigned int win_x;
	unsigned int win_y;
	unsigned int depth = 0, freq = 0;
	const char *options;
	int ret = 0;

	ret = video_get_video_mode(&win_x, &win_y, &depth, &freq, &options);
	if (ret < 0)
		return ret;

	plat->size = win_x * win_y * 32;

	return 0;
}

static const struct udevice_id fsl_dcu_video_ids[] = {
	{ .compatible = "fsl,vf610-dcu" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(fsl_dcu_video) = {
	.name	= "fsl_dcu_video",
	.id	= UCLASS_VIDEO,
	.of_match = fsl_dcu_video_ids,
	.bind	= fsl_dcu_video_bind,
	.probe	= fsl_dcu_video_probe,
	.flags	= DM_FLAG_PRE_RELOC,
};
#endif /* ifndef CONFIG_DM_VIDEO */
