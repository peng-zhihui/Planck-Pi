#if !defined(CONFIG_ARCH_UNIPHIER) && !defined(CONFIG_ARCH_STI) && \
	!defined(CONFIG_ARCH_K3) && !defined(CONFIG_ARCH_BCM68360) && \
	!defined(CONFIG_ARCH_BCM6858) && !defined(CONFIG_ARCH_BCM63158) && \
	!defined(CONFIG_ARCH_ROCKCHIP) && !defined(CONFIG_ARCH_LX2160A) && \
	!defined(CONFIG_ARCH_LS1028A) && !defined(CONFIG_ARCH_LS2080A) && \
	!defined(CONFIG_ARCH_LS1088A) && !defined(CONFIG_ARCH_ASPEED) && \
	!defined(CONFIG_ARCH_LS1012A) && !defined(CONFIG_ARCH_LS1043A) && \
	!defined(CONFIG_ARCH_LS1046A) && !defined(CONFIG_ARCH_U8500) && \
	!defined(CONFIG_CORTINA_PLATFORM)
#include <asm/arch/gpio.h>
#endif
#include <asm-generic/gpio.h>
