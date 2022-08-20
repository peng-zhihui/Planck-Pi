// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2020 Stefan Roese <sr@denx.de>
 */

#include <common.h>
#include <command.h>
#include <cpu_func.h>

DECLARE_GLOBAL_DATA_PTR;

unsigned long do_go_exec(ulong (*entry)(int, char * const []),
			 int argc, char * const argv[])
{
	/*
	 * Flush cache before jumping to application. Let's flush the
	 * whole SDRAM area, since we don't know the size of the image
	 * that was loaded.
	 */
	flush_cache(gd->bd->bi_memstart, gd->ram_top - gd->bd->bi_memstart);

	return entry(argc, argv);
}
