// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2013-2017 Oracle Corporation
 * This file is based on ast_fb.c
 * Copyright 2012 Red Hat Inc.
 * Authors: Dave Airlie <airlied@redhat.com>
 *          Michael Thayer <michael.thayer@oracle.com,
 */
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/sysrq.h>
#include <linux/tty.h>

#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>

#include "vbox_drv.h"
#include "vboxvideo.h"

#ifdef CONFIG_DRM_KMS_FB_HELPER
static struct fb_deferred_io vbox_defio = {
	.delay = HZ / 30,
	.deferred_io = drm_fb_helper_deferred_io,
};
#endif

static struct fb_ops vboxfb_ops = {
	.owner = THIS_MODULE,
	DRM_FB_HELPER_DEFAULT_OPS,
	.fb_fillrect = drm_fb_helper_sys_fillrect,
	.fb_copyarea = drm_fb_helper_sys_copyarea,
	.fb_imageblit = drm_fb_helper_sys_imageblit,
};

int vboxfb_create(struct drm_fb_helper *helper,
		  struct drm_fb_helper_surface_size *sizes)
{
	struct vbox_private *vbox =
		container_of(helper, struct vbox_private, fb_helper);
	struct pci_dev *pdev = vbox->ddev.pdev;
	struct drm_mode_fb_cmd2 mode_cmd;
	struct drm_framebuffer *fb;
	struct fb_info *info;
	struct drm_gem_object *gobj;
	struct drm_gem_vram_object *gbo;
	int size, ret;
	s64 gpu_addr;
	u32 pitch;

	mode_cmd.width = sizes->surface_width;
	mode_cmd.height = sizes->surface_height;
	pitch = mode_cmd.width * ((sizes->surface_bpp + 7) / 8);
	mode_cmd.pixel_format = drm_mode_legacy_fb_format(sizes->surface_bpp,
							  sizes->surface_depth);
	mode_cmd.pitches[0] = pitch;

	size = pitch * mode_cmd.height;

	ret = vbox_gem_create(vbox, size, true, &gobj);
	if (ret) {
		DRM_ERROR("failed to create fbcon backing object %d\n", ret);
		return ret;
	}

	ret = vbox_framebuffer_init(vbox, &vbox->afb, &mode_cmd, gobj);
	if (ret)
		return ret;

	gbo = drm_gem_vram_of_gem(gobj);

	ret = drm_gem_vram_pin(gbo, DRM_GEM_VRAM_PL_FLAG_VRAM);
	if (ret)
		return ret;

	info = drm_fb_helper_alloc_fbi(helper);
	if (IS_ERR(info))
		return PTR_ERR(info);

	info->screen_size = size;
	info->screen_base = (char __iomem *)drm_gem_vram_kmap(gbo, true, NULL);
	if (IS_ERR(info->screen_base))
		return PTR_ERR(info->screen_base);

	fb = &vbox->afb.base;
	helper->fb = fb;

	info->fbops = &vboxfb_ops;

	/*
	 * This seems to be done for safety checking that the framebuffer
	 * is not registered twice by different drivers.
	 */
	info->apertures->ranges[0].base = pci_resource_start(pdev, 0);
	info->apertures->ranges[0].size = pci_resource_len(pdev, 0);

	drm_fb_helper_fill_info(info, helper, sizes);

	gpu_addr = drm_gem_vram_offset(gbo);
	if (gpu_addr < 0)
		return (int)gpu_addr;
	info->fix.smem_start = info->apertures->ranges[0].base + gpu_addr;
	info->fix.smem_len = vbox->available_vram_size - gpu_addr;

#ifdef CONFIG_DRM_KMS_FB_HELPER
	info->fbdefio = &vbox_defio;
	fb_deferred_io_init(info);
#endif

	info->pixmap.flags = FB_PIXMAP_SYSTEM;

	DRM_DEBUG_KMS("allocated %dx%d\n", fb->width, fb->height);

	return 0;
}

void vbox_fbdev_fini(struct vbox_private *vbox)
{
	struct vbox_framebuffer *afb = &vbox->afb;

#ifdef CONFIG_DRM_KMS_FB_HELPER
	if (vbox->fb_helper.fbdev && vbox->fb_helper.fbdev->fbdefio)
		fb_deferred_io_cleanup(vbox->fb_helper.fbdev);
#endif

	drm_fb_helper_unregister_fbi(&vbox->fb_helper);

	if (afb->obj) {
		struct drm_gem_vram_object *gbo = drm_gem_vram_of_gem(afb->obj);

		drm_gem_vram_kunmap(gbo);
		drm_gem_vram_unpin(gbo);

		drm_gem_object_put_unlocked(afb->obj);
		afb->obj = NULL;
	}
	drm_fb_helper_fini(&vbox->fb_helper);

	drm_framebuffer_unregister_private(&afb->base);
	drm_framebuffer_cleanup(&afb->base);
}
