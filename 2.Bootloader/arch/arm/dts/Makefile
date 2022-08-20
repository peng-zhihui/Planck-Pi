# SPDX-License-Identifier: GPL-2.0+
 
dtb-$(CONFIG_MACH_SUNIV) += \
	suniv-f1c100s-generic.dtb
 

targets += $(dtb-y)

# Add any required device tree compiler flags here
DTC_FLAGS +=

PHONY += dtbs
dtbs: $(addprefix $(obj)/, $(dtb-y))
	@:

clean-files := *.dtb *.dtbo *_HS
