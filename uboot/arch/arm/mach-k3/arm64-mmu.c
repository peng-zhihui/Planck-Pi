// SPDX-License-Identifier:     GPL-2.0+
/*
 * K3: ARM64 MMU setup
 *
 * Copyright (C) 2018 Texas Instruments Incorporated - http://www.ti.com/
 *	Lokesh Vutla <lokeshvutla@ti.com>
 * (This file is derived from arch/arm/mach-zynqmp/cpu.c)
 *
 */

#include <common.h>
#include <asm/system.h>
#include <asm/armv8/mmu.h>

#ifdef CONFIG_SOC_K3_AM6
/* NR_DRAM_BANKS + 32bit IO + 64bit IO + terminator */
#define NR_MMU_REGIONS	(CONFIG_NR_DRAM_BANKS + 5)

/* ToDo: Add 64bit IO */
struct mm_region am654_mem_map[NR_MMU_REGIONS] = {
	{
		.virt = 0x0UL,
		.phys = 0x0UL,
		.size = 0x80000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		.virt = 0x80000000UL,
		.phys = 0x80000000UL,
		.size = 0x20000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		.virt = 0xa0000000UL,
		.phys = 0xa0000000UL,
		.size = 0x02100000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		.virt = 0xa2100000UL,
		.phys = 0xa2100000UL,
		.size = 0x5df00000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		.virt = 0x880000000UL,
		.phys = 0x880000000UL,
		.size = 0x80000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		.virt = 0x500000000UL,
		.phys = 0x500000000UL,
		.size = 0x400000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* List terminator */
		0,
	}
};

struct mm_region *mem_map = am654_mem_map;
#endif /* CONFIG_SOC_K3_AM6 */

#ifdef CONFIG_SOC_K3_J721E
/* NR_DRAM_BANKS + 32bit IO + 64bit IO + terminator */
#define NR_MMU_REGIONS	(CONFIG_NR_DRAM_BANKS + 6)

/* ToDo: Add 64bit IO */
struct mm_region j721e_mem_map[NR_MMU_REGIONS] = {
	{
		.virt = 0x0UL,
		.phys = 0x0UL,
		.size = 0x80000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		.virt = 0x80000000UL,
		.phys = 0x80000000UL,
		.size = 0x20000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		.virt = 0xa0000000UL,
		.phys = 0xa0000000UL,
		.size = 0x1bc00000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) |
			 PTE_BLOCK_NON_SHARE
	}, {
		.virt = 0xbbc00000UL,
		.phys = 0xbbc00000UL,
		.size = 0x44400000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		.virt = 0x880000000UL,
		.phys = 0x880000000UL,
		.size = 0x80000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		.virt = 0x500000000UL,
		.phys = 0x500000000UL,
		.size = 0x400000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		.virt = 0x4d80000000UL,
		.phys = 0x4d80000000UL,
		.size = 0x0002000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* List terminator */
		0,
	}
};

struct mm_region *mem_map = j721e_mem_map;
#endif /* CONFIG_SOC_K3_J721E */
