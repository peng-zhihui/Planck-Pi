#ifndef _SUNXI_ION_PRIV_H
#define _SUNXI_ION_PRIV_H

#include "cache.h"

#define ION_IOC_SUNXI_FLUSH_RANGE           5
#define ION_IOC_SUNXI_FLUSH_ALL             6
#define ION_IOC_SUNXI_PHYS_ADDR             7
#define ION_IOC_SUNXI_DMA_COPY              8
#define ION_IOC_SUNXI_DUMP                  9
#define ION_IOC_SUNXI_POOL_FREE             10

typedef struct {
	long 	start;
	long 	end;
}sunxi_cache_range;

typedef struct {
	struct ion_handle_data handle;
	unsigned int phys_addr;
	unsigned int size;
}sunxi_phys_data;

struct ion_cma_buffer_info {
	void *cpu_addr;
	dma_addr_t handle;
	struct sg_table *table;
};

#endif