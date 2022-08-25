#if __LINUX_ARM_ARCH__ == 7
#include "cache-v7.S"
#else
#warning "[sunxi-cedar] No BSP asm cache flush support, use dmac flush"
#endif
