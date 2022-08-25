/*
 * drivers/staging/android/ion/sunxi/ion_sunxi.h
 *
 * Copyright(c) 2015-2020 Allwinnertech Co., Ltd.
 *      http://www.allwinnertech.com
 *
 * Author: Wim Hwang <huangwei@allwinnertech.com>
 *
 * sunxi ion header file
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _LINUX_ION_SUNXI_H
#define _LINUX_ION_SUNXI_H

/**
 * ion_client_create() -  allocate a client and returns it
 * @name:	used for debugging
 */
struct ion_client *sunxi_ion_client_create(const char *name);

void sunxi_ion_probe_drm_info(u32 *drm_phy_addr, u32 *drm_tee_addr);

int  optee_probe_drm_configure(
		unsigned long *drm_base,
		size_t *drm_size,
		unsigned long  *tee_base);

#endif
