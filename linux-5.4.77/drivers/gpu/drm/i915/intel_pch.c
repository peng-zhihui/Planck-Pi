// SPDX-License-Identifier: MIT
/*
 * Copyright 2019 Intel Corporation.
 */

#include "i915_drv.h"
#include "intel_pch.h"

/* Map PCH device id to PCH type, or PCH_NONE if unknown. */
static enum intel_pch
intel_pch_type(const struct drm_i915_private *dev_priv, unsigned short id)
{
	switch (id) {
	case INTEL_PCH_IBX_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found Ibex Peak PCH\n");
		WARN_ON(!IS_GEN(dev_priv, 5));
		return PCH_IBX;
	case INTEL_PCH_CPT_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found CougarPoint PCH\n");
		WARN_ON(!IS_GEN(dev_priv, 6) && !IS_IVYBRIDGE(dev_priv));
		return PCH_CPT;
	case INTEL_PCH_PPT_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found PantherPoint PCH\n");
		WARN_ON(!IS_GEN(dev_priv, 6) && !IS_IVYBRIDGE(dev_priv));
		/* PantherPoint is CPT compatible */
		return PCH_CPT;
	case INTEL_PCH_LPT_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found LynxPoint PCH\n");
		WARN_ON(!IS_HASWELL(dev_priv) && !IS_BROADWELL(dev_priv));
		WARN_ON(IS_HSW_ULT(dev_priv) || IS_BDW_ULT(dev_priv));
		return PCH_LPT;
	case INTEL_PCH_LPT_LP_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found LynxPoint LP PCH\n");
		WARN_ON(!IS_HASWELL(dev_priv) && !IS_BROADWELL(dev_priv));
		WARN_ON(!IS_HSW_ULT(dev_priv) && !IS_BDW_ULT(dev_priv));
		return PCH_LPT;
	case INTEL_PCH_WPT_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found WildcatPoint PCH\n");
		WARN_ON(!IS_HASWELL(dev_priv) && !IS_BROADWELL(dev_priv));
		WARN_ON(IS_HSW_ULT(dev_priv) || IS_BDW_ULT(dev_priv));
		/* WildcatPoint is LPT compatible */
		return PCH_LPT;
	case INTEL_PCH_WPT_LP_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found WildcatPoint LP PCH\n");
		WARN_ON(!IS_HASWELL(dev_priv) && !IS_BROADWELL(dev_priv));
		WARN_ON(!IS_HSW_ULT(dev_priv) && !IS_BDW_ULT(dev_priv));
		/* WildcatPoint is LPT compatible */
		return PCH_LPT;
	case INTEL_PCH_SPT_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found SunrisePoint PCH\n");
		WARN_ON(!IS_SKYLAKE(dev_priv) && !IS_KABYLAKE(dev_priv));
		return PCH_SPT;
	case INTEL_PCH_SPT_LP_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found SunrisePoint LP PCH\n");
		WARN_ON(!IS_SKYLAKE(dev_priv) && !IS_KABYLAKE(dev_priv));
		return PCH_SPT;
	case INTEL_PCH_KBP_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found Kaby Lake PCH (KBP)\n");
		WARN_ON(!IS_SKYLAKE(dev_priv) && !IS_KABYLAKE(dev_priv) &&
			!IS_COFFEELAKE(dev_priv));
		/* KBP is SPT compatible */
		return PCH_SPT;
	case INTEL_PCH_CNP_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found Cannon Lake PCH (CNP)\n");
		WARN_ON(!IS_CANNONLAKE(dev_priv) && !IS_COFFEELAKE(dev_priv));
		return PCH_CNP;
	case INTEL_PCH_CNP_LP_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found Cannon Lake LP PCH (CNP-LP)\n");
		WARN_ON(!IS_CANNONLAKE(dev_priv) && !IS_COFFEELAKE(dev_priv));
		return PCH_CNP;
	case INTEL_PCH_CMP_DEVICE_ID_TYPE:
	case INTEL_PCH_CMP2_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found Comet Lake PCH (CMP)\n");
		WARN_ON(!IS_COFFEELAKE(dev_priv));
		/* CometPoint is CNP Compatible */
		return PCH_CNP;
	case INTEL_PCH_ICP_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found Ice Lake PCH\n");
		WARN_ON(!IS_ICELAKE(dev_priv));
		return PCH_ICP;
	case INTEL_PCH_MCC_DEVICE_ID_TYPE:
	case INTEL_PCH_MCC2_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found Mule Creek Canyon PCH\n");
		WARN_ON(!IS_ELKHARTLAKE(dev_priv));
		return PCH_MCC;
	case INTEL_PCH_TGP_DEVICE_ID_TYPE:
		DRM_DEBUG_KMS("Found Tiger Lake LP PCH\n");
		WARN_ON(!IS_TIGERLAKE(dev_priv));
		return PCH_TGP;
	default:
		return PCH_NONE;
	}
}

static bool intel_is_virt_pch(unsigned short id,
			      unsigned short svendor, unsigned short sdevice)
{
	return (id == INTEL_PCH_P2X_DEVICE_ID_TYPE ||
		id == INTEL_PCH_P3X_DEVICE_ID_TYPE ||
		(id == INTEL_PCH_QEMU_DEVICE_ID_TYPE &&
		 svendor == PCI_SUBVENDOR_ID_REDHAT_QUMRANET &&
		 sdevice == PCI_SUBDEVICE_ID_QEMU));
}

static unsigned short
intel_virt_detect_pch(const struct drm_i915_private *dev_priv)
{
	unsigned short id = 0;

	/*
	 * In a virtualized passthrough environment we can be in a
	 * setup where the ISA bridge is not able to be passed through.
	 * In this case, a south bridge can be emulated and we have to
	 * make an educated guess as to which PCH is really there.
	 */

	if (IS_TIGERLAKE(dev_priv))
		id = INTEL_PCH_TGP_DEVICE_ID_TYPE;
	else if (IS_ELKHARTLAKE(dev_priv))
		id = INTEL_PCH_MCC_DEVICE_ID_TYPE;
	else if (IS_ICELAKE(dev_priv))
		id = INTEL_PCH_ICP_DEVICE_ID_TYPE;
	else if (IS_CANNONLAKE(dev_priv) || IS_COFFEELAKE(dev_priv))
		id = INTEL_PCH_CNP_DEVICE_ID_TYPE;
	else if (IS_KABYLAKE(dev_priv) || IS_SKYLAKE(dev_priv))
		id = INTEL_PCH_SPT_DEVICE_ID_TYPE;
	else if (IS_HSW_ULT(dev_priv) || IS_BDW_ULT(dev_priv))
		id = INTEL_PCH_LPT_LP_DEVICE_ID_TYPE;
	else if (IS_HASWELL(dev_priv) || IS_BROADWELL(dev_priv))
		id = INTEL_PCH_LPT_DEVICE_ID_TYPE;
	else if (IS_GEN(dev_priv, 6) || IS_IVYBRIDGE(dev_priv))
		id = INTEL_PCH_CPT_DEVICE_ID_TYPE;
	else if (IS_GEN(dev_priv, 5))
		id = INTEL_PCH_IBX_DEVICE_ID_TYPE;

	if (id)
		DRM_DEBUG_KMS("Assuming PCH ID %04x\n", id);
	else
		DRM_DEBUG_KMS("Assuming no PCH\n");

	return id;
}

void intel_detect_pch(struct drm_i915_private *dev_priv)
{
	struct pci_dev *pch = NULL;

	/*
	 * The reason to probe ISA bridge instead of Dev31:Fun0 is to
	 * make graphics device passthrough work easy for VMM, that only
	 * need to expose ISA bridge to let driver know the real hardware
	 * underneath. This is a requirement from virtualization team.
	 *
	 * In some virtualized environments (e.g. XEN), there is irrelevant
	 * ISA bridge in the system. To work reliably, we should scan trhough
	 * all the ISA bridge devices and check for the first match, instead
	 * of only checking the first one.
	 */
	while ((pch = pci_get_class(PCI_CLASS_BRIDGE_ISA << 8, pch))) {
		unsigned short id;
		enum intel_pch pch_type;

		if (pch->vendor != PCI_VENDOR_ID_INTEL)
			continue;

		id = pch->device & INTEL_PCH_DEVICE_ID_MASK;

		pch_type = intel_pch_type(dev_priv, id);
		if (pch_type != PCH_NONE) {
			dev_priv->pch_type = pch_type;
			dev_priv->pch_id = id;
			break;
		} else if (intel_is_virt_pch(id, pch->subsystem_vendor,
					     pch->subsystem_device)) {
			id = intel_virt_detect_pch(dev_priv);
			pch_type = intel_pch_type(dev_priv, id);

			/* Sanity check virtual PCH id */
			if (WARN_ON(id && pch_type == PCH_NONE))
				id = 0;

			dev_priv->pch_type = pch_type;
			dev_priv->pch_id = id;
			break;
		}
	}

	/*
	 * Use PCH_NOP (PCH but no South Display) for PCH platforms without
	 * display.
	 */
	if (pch && !HAS_DISPLAY(dev_priv)) {
		DRM_DEBUG_KMS("Display disabled, reverting to NOP PCH\n");
		dev_priv->pch_type = PCH_NOP;
		dev_priv->pch_id = 0;
	}

	if (!pch)
		DRM_DEBUG_KMS("No PCH found.\n");

	pci_dev_put(pch);
}
