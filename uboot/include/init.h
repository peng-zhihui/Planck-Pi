/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2000-2009
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * Copy the startup prototype, previously defined in common.h
 * Copyright (C) 2018, STMicroelectronics - All Rights Reserved
 */

#ifndef __INIT_H_
#define __INIT_H_	1

#include <linux/types.h>

struct global_data;

#ifndef __ASSEMBLY__		/* put C only stuff in this section */

/* Avoid using CONFIG_EFI_STUB directly as we may boot from other loaders */
#ifdef CONFIG_EFI_STUB
#define ll_boot_init()	false
#else
#define ll_boot_init()	(!(gd->flags & GD_FLG_SKIP_LL_INIT))
#endif

/*
 * Function Prototypes
 */

/* common/board_f.c */
void board_init_f(ulong dummy);

/**
 * arch_cpu_init() - basic cpu-dependent setup for an architecture
 *
 * This is called after early malloc is available. It should handle any
 * CPU- or SoC- specific init needed to continue the init sequence. See
 * board_f.c for where it is called. If this is not provided, a default
 * version (which does nothing) will be used.
 *
 * Return: 0 on success, otherwise error
 */
int arch_cpu_init(void);

/**
 * arch_cpu_init_dm() - init CPU after driver model is available
 *
 * This is called immediately after driver model is available before
 * relocation. This is similar to arch_cpu_init() but is able to reference
 * devices
 *
 * Return: 0 if OK, -ve on error
 */
int arch_cpu_init_dm(void);

/**
 * mach_cpu_init() - SoC/machine dependent CPU setup
 *
 * This is called after arch_cpu_init(). It should handle any
 * SoC or machine specific init needed to continue the init sequence. See
 * board_f.c for where it is called. If this is not provided, a default
 * version (which does nothing) will be used.
 *
 * Return: 0 on success, otherwise error
 */
int mach_cpu_init(void);

/**
 * arch_fsp_init() - perform firmware support package init
 *
 * Where U-Boot relies on binary blobs to handle part of the system init, this
 * function can be used to set up the blobs. This is used on some Intel
 * platforms.
 *
 * Return: 0
 */
int arch_fsp_init(void);

/**
 * arch_fsp_init() - perform post-relocation firmware support package init
 *
 * Where U-Boot relies on binary blobs to handle part of the system init, this
 * function can be used to set up the blobs. This is used on some Intel
 * platforms.
 *
 * Return: 0
 */
int arch_fsp_init_r(void);

int dram_init(void);

/**
 * dram_init_banksize() - Set up DRAM bank sizes
 *
 * This can be implemented by boards to set up the DRAM bank information in
 * gd->bd->bi_dram(). It is called just before relocation, after dram_init()
 * is called.
 *
 * If this is not provided, a default implementation will try to set up a
 * single bank. It will do this if CONFIG_NR_DRAM_BANKS and
 * CONFIG_SYS_SDRAM_BASE are set. The bank will have a start address of
 * CONFIG_SYS_SDRAM_BASE and the size will be determined by a call to
 * get_effective_memsize().
 *
 * Return: 0 if OK, -ve on error
 */
int dram_init_banksize(void);

long get_ram_size(long *base, long size);
phys_size_t get_effective_memsize(void);

int testdram(void);

/**
 * arch_reserve_stacks() - Reserve all necessary stacks
 *
 * This is used in generic board init sequence in common/board_f.c. Each
 * architecture could provide this function to tailor the required stacks.
 *
 * On entry gd->start_addr_sp is pointing to the suggested top of the stack.
 * The callee ensures gd->start_add_sp is 16-byte aligned, so architectures
 * require only this can leave it untouched.
 *
 * On exit gd->start_addr_sp and gd->irq_sp should be set to the respective
 * positions of the stack. The stack pointer(s) will be set to this later.
 * gd->irq_sp is only required, if the architecture needs it.
 *
 * Return: 0 if no error
 */
int arch_reserve_stacks(void);

/**
 * arch_reserve_mmu() - Reserve memory for MMU TLB table
 *
 * Architecture-specific routine for reserving memory for the MMU TLB table.
 * This is used in generic board init sequence in common/board_f.c.
 *
 * If an implementation is not provided, it will just be a nop stub.
 *
 * Return: 0 if OK
 */
int arch_reserve_mmu(void);

/**
 * init_cache_f_r() - Turn on the cache in preparation for relocation
 *
 * Return: 0 if OK, -ve on error
 */
int init_cache_f_r(void);

#if !CONFIG_IS_ENABLED(CPU)
/**
 * print_cpuinfo() - Display information about the CPU
 *
 * Return: 0 if OK, -ve on error
 */
int print_cpuinfo(void);
#endif
int timer_init(void);
int misc_init_f(void);

#if defined(CONFIG_DTB_RESELECT)
int embedded_dtb_select(void);
#endif

/* common/init/board_init.c */
extern ulong monitor_flash_len;

/**
 * ulong board_init_f_alloc_reserve - allocate reserved area
 * @top: top of the reserve area, growing down.
 *
 * This function is called by each architecture very early in the start-up
 * code to allow the C runtime to reserve space on the stack for writable
 * 'globals' such as GD and the malloc arena.
 *
 * Return: bottom of reserved area
 */
ulong board_init_f_alloc_reserve(ulong top);

/**
 * board_init_f_init_reserve - initialize the reserved area(s)
 * @base:	top from which reservation was done
 *
 * This function is called once the C runtime has allocated the reserved
 * area on the stack. It must initialize the GD at the base of that area.
 */
void board_init_f_init_reserve(ulong base);

struct global_data;

/**
 * arch_setup_gd() - Set up the global_data pointer
 * @gd_ptr: Pointer to global data
 *
 * This pointer is special in some architectures and cannot easily be assigned
 * to. For example on x86 it is implemented by adding a specific record to its
 * Global Descriptor Table! So we we provide a function to carry out this task.
 * For most architectures this can simply be:
 *
 *    gd = gd_ptr;
 */
void arch_setup_gd(struct global_data *gd_ptr);

/* common/board_r.c */
void board_init_r(struct global_data *id, ulong dest_addr)
	__attribute__ ((noreturn));

int cpu_init_r(void);
int last_stage_init(void);
int mac_read_from_eeprom(void);
int set_cpu_clk_info(void);
int update_flash_size(int flash_size);
int arch_early_init_r(void);
void pci_init(void);
int misc_init_r(void);
#if defined(CONFIG_VID)
int init_func_vid(void);
#endif

/* common/board_info.c */
int checkboard(void);
int show_board_info(void);

/**
 * Get the uppermost pointer that is valid to access
 *
 * Some systems may not map all of their address space. This function allows
 * boards to indicate what their highest support pointer value is for DRAM
 * access.
 *
 * @param total_size	Size of U-Boot (unused?)
 */
ulong board_get_usable_ram_top(ulong total_size);

int board_early_init_f(void);

/* manipulate the U-Boot fdt before its relocation */
int board_fix_fdt(void *rw_fdt_blob);
int board_late_init(void);
int board_postclk_init(void); /* after clocks/timebase, before env/serial */
int board_early_init_r(void);

/* TODO(sjg@chromium.org): Drop this when DM_PCI migration is completed */
void pci_init_board(void);

void trap_init(unsigned long reloc_addr);

/**
 * main_loop() - Enter the main loop of U-Boot
 *
 * This normally runs the command line.
 */
void main_loop(void);

#if defined(CONFIG_ARM)
void relocate_code(ulong addr_moni);
#else
void relocate_code(ulong start_addr_sp, struct global_data *new_gd,
		   ulong relocaddr)
	__attribute__ ((noreturn));
#endif

#endif	/* __ASSEMBLY__ */
/* Put only stuff here that the assembler can digest */

#endif	/* __INIT_H_ */
