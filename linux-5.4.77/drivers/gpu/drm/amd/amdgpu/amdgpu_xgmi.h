/*
 * Copyright 2016 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef __AMDGPU_XGMI_H__
#define __AMDGPU_XGMI_H__

#include "amdgpu_psp.h"

struct amdgpu_hive_info {
	uint64_t		hive_id;
	struct list_head	device_list;
	int number_devices;
	struct mutex hive_lock, reset_lock;
	struct kobject *kobj;
	struct device_attribute dev_attr;
	struct amdgpu_device *adev;
	int pstate; /*0 -- low , 1 -- high , -1 unknown*/
};

struct amdgpu_hive_info *amdgpu_get_xgmi_hive(struct amdgpu_device *adev, int lock);
int amdgpu_xgmi_update_topology(struct amdgpu_hive_info *hive, struct amdgpu_device *adev);
int amdgpu_xgmi_add_device(struct amdgpu_device *adev);
void amdgpu_xgmi_remove_device(struct amdgpu_device *adev);
int amdgpu_xgmi_set_pstate(struct amdgpu_device *adev, int pstate);
int amdgpu_xgmi_get_hops_count(struct amdgpu_device *adev,
		struct amdgpu_device *peer_adev);

static inline bool amdgpu_xgmi_same_hive(struct amdgpu_device *adev,
		struct amdgpu_device *bo_adev)
{
	return (adev != bo_adev &&
		adev->gmc.xgmi.hive_id &&
		adev->gmc.xgmi.hive_id == bo_adev->gmc.xgmi.hive_id);
}

#endif
