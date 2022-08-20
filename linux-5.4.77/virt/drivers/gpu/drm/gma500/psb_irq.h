/* SPDX-License-Identifier: GPL-2.0-only */
/**************************************************************************
 * Copyright (c) 2009-2011, Intel Corporation.
 * All Rights Reserved.
 *
 * Authors:
 *    Benjamin Defnet <benjamin.r.defnet@intel.com>
 *    Rajesh Poornachandran <rajesh.poornachandran@intel.com>
 *
 **************************************************************************/

#ifndef _PSB_IRQ_H_
#define _PSB_IRQ_H_

struct drm_device;

bool sysirq_init(struct drm_device *dev);
void sysirq_uninit(struct drm_device *dev);

void psb_irq_preinstall(struct drm_device *dev);
int  psb_irq_postinstall(struct drm_device *dev);
void psb_irq_uninstall(struct drm_device *dev);
irqreturn_t psb_irq_handler(int irq, void *arg);

int psb_irq_enable_dpst(struct drm_device *dev);
int psb_irq_disable_dpst(struct drm_device *dev);
void psb_irq_turn_on_dpst(struct drm_device *dev);
void psb_irq_turn_off_dpst(struct drm_device *dev);
int  psb_enable_vblank(struct drm_device *dev, unsigned int pipe);
void psb_disable_vblank(struct drm_device *dev, unsigned int pipe);
u32  psb_get_vblank_counter(struct drm_device *dev, unsigned int pipe);

int mdfld_enable_te(struct drm_device *dev, int pipe);
void mdfld_disable_te(struct drm_device *dev, int pipe);
#endif /* _PSB_IRQ_H_ */
