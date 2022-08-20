// SPDX-License-Identifier: BSD-3-Clause
/*
 * Reference to the ARM TF Project,
 * plat/arm/common/arm_bl2_setup.c
 * Portions copyright (c) 2013-2016, ARM Limited and Contributors. All rights
 * reserved.
 * Copyright (C) 2016 Rockchip Electronic Co.,Ltd
 * Written by Kever Yang <kever.yang@rock-chips.com>
 * Copyright (C) 2017 Theobroma Systems Design und Consulting GmbH
 */

#include <common.h>
#include <atf_common.h>
#include <cpu_func.h>
#include <errno.h>
#include <image.h>
#include <log.h>
#include <spl.h>
#include <asm/cache.h>

static struct bl2_to_bl31_params_mem bl31_params_mem;
static struct bl31_params *bl2_to_bl31_params;

__weak struct bl31_params *bl2_plat_get_bl31_params(uintptr_t bl32_entry,
						    uintptr_t bl33_entry,
						    uintptr_t fdt_addr)
{
	struct entry_point_info *bl32_ep_info;
	struct entry_point_info *bl33_ep_info;

	/*
	 * Initialise the memory for all the arguments that needs to
	 * be passed to BL31
	 */
	memset(&bl31_params_mem, 0, sizeof(struct bl2_to_bl31_params_mem));

	/* Assign memory for TF related information */
	bl2_to_bl31_params = &bl31_params_mem.bl31_params;
	SET_PARAM_HEAD(bl2_to_bl31_params, ATF_PARAM_BL31, ATF_VERSION_1, 0);

	/* Fill BL31 related information */
	bl2_to_bl31_params->bl31_image_info = &bl31_params_mem.bl31_image_info;
	SET_PARAM_HEAD(bl2_to_bl31_params->bl31_image_info,
		       ATF_PARAM_IMAGE_BINARY, ATF_VERSION_1, 0);

	/* Fill BL32 related information */
	bl2_to_bl31_params->bl32_ep_info = &bl31_params_mem.bl32_ep_info;
	bl32_ep_info = &bl31_params_mem.bl32_ep_info;
	SET_PARAM_HEAD(bl32_ep_info, ATF_PARAM_EP, ATF_VERSION_1,
		       ATF_EP_SECURE);

	/* secure payload is optional, so set pc to 0 if absent */
	bl32_ep_info->args.arg3 = fdt_addr;
	bl32_ep_info->pc = bl32_entry ? bl32_entry : 0;
	bl32_ep_info->spsr = SPSR_64(MODE_EL1, MODE_SP_ELX,
				     DISABLE_ALL_EXECPTIONS);

	bl2_to_bl31_params->bl32_image_info = &bl31_params_mem.bl32_image_info;
	SET_PARAM_HEAD(bl2_to_bl31_params->bl32_image_info,
		       ATF_PARAM_IMAGE_BINARY, ATF_VERSION_1, 0);

	/* Fill BL33 related information */
	bl2_to_bl31_params->bl33_ep_info = &bl31_params_mem.bl33_ep_info;
	bl33_ep_info = &bl31_params_mem.bl33_ep_info;
	SET_PARAM_HEAD(bl33_ep_info, ATF_PARAM_EP, ATF_VERSION_1,
		       ATF_EP_NON_SECURE);

	/* BL33 expects to receive the primary CPU MPID (through x0) */
	bl33_ep_info->args.arg0 = 0xffff & read_mpidr();
	bl33_ep_info->pc = bl33_entry;
	bl33_ep_info->spsr = SPSR_64(MODE_EL2, MODE_SP_ELX,
				     DISABLE_ALL_EXECPTIONS);

	bl2_to_bl31_params->bl33_image_info = &bl31_params_mem.bl33_image_info;
	SET_PARAM_HEAD(bl2_to_bl31_params->bl33_image_info,
		       ATF_PARAM_IMAGE_BINARY, ATF_VERSION_1, 0);

	return bl2_to_bl31_params;
}

static inline void raw_write_daif(unsigned int daif)
{
	__asm__ __volatile__("msr DAIF, %0\n\t" : : "r" (daif) : "memory");
}

typedef void (*atf_entry_t)(struct bl31_params *params, void *plat_params);

static void bl31_entry(uintptr_t bl31_entry, uintptr_t bl32_entry,
		       uintptr_t bl33_entry, uintptr_t fdt_addr)
{
	struct bl31_params *bl31_params;
	atf_entry_t  atf_entry = (atf_entry_t)bl31_entry;

	bl31_params = bl2_plat_get_bl31_params(bl32_entry, bl33_entry,
					       fdt_addr);

	raw_write_daif(SPSR_EXCEPTION_MASK);
	dcache_disable();

	atf_entry((void *)bl31_params, (void *)fdt_addr);
}

static int spl_fit_images_find(void *blob, int os)
{
	int parent, node, ndepth = 0;
	const void *data;

	if (!blob)
		return -FDT_ERR_BADMAGIC;

	parent = fdt_path_offset(blob, "/fit-images");
	if (parent < 0)
		return -FDT_ERR_NOTFOUND;

	for (node = fdt_next_node(blob, parent, &ndepth);
	     (node >= 0) && (ndepth > 0);
	     node = fdt_next_node(blob, node, &ndepth)) {
		if (ndepth != 1)
			continue;

		data = fdt_getprop(blob, node, FIT_OS_PROP, NULL);
		if (!data)
			continue;

		if (genimg_get_os_id(data) == os)
			return node;
	};

	return -FDT_ERR_NOTFOUND;
}

uintptr_t spl_fit_images_get_entry(void *blob, int node)
{
	ulong  val;

	val = fdt_getprop_u32(blob, node, "entry-point");
	if (val == FDT_ERROR)
		val = fdt_getprop_u32(blob, node, "load-addr");

	debug("%s: entry point 0x%lx\n", __func__, val);
	return val;
}

void spl_invoke_atf(struct spl_image_info *spl_image)
{
	uintptr_t  bl32_entry = 0;
	uintptr_t  bl33_entry = CONFIG_SYS_TEXT_BASE;
	void *blob = spl_image->fdt_addr;
	uintptr_t platform_param = (uintptr_t)blob;
	int node;

	/*
	 * Find the OP-TEE binary (in /fit-images) load address or
	 * entry point (if different) and pass it as the BL3-2 entry
	 * point, this is optional.
	 */
	node = spl_fit_images_find(blob, IH_OS_TEE);
	if (node >= 0)
		bl32_entry = spl_fit_images_get_entry(blob, node);

	/*
	 * Find the U-Boot binary (in /fit-images) load addreess or
	 * entry point (if different) and pass it as the BL3-3 entry
	 * point.
	 * This will need to be extended to support Falcon mode.
	 */

	node = spl_fit_images_find(blob, IH_OS_U_BOOT);
	if (node >= 0)
		bl33_entry = spl_fit_images_get_entry(blob, node);

	/*
	 * If ATF_NO_PLATFORM_PARAM is set, we override the platform
	 * parameter and always pass 0.  This is a workaround for
	 * older ATF versions that have insufficiently robust (or
	 * overzealous) argument validation.
	 */
	if (CONFIG_IS_ENABLED(ATF_NO_PLATFORM_PARAM))
		platform_param = 0;

	/*
	 * We don't provide a BL3-2 entry yet, but this will be possible
	 * using similar logic.
	 */
	bl31_entry(spl_image->entry_point, bl32_entry,
		   bl33_entry, platform_param);
}
