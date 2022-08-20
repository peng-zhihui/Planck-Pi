// SPDX-License-Identifier: GPL-2.0+
/*
 * The 'exception' command can be used for testing exception handling.
 *
 * Copyright (c) 2018, Heinrich Schuchardt <xypron.glpk@gmx.de>
 */

#include <common.h>
#include <command.h>

static int do_undefined(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	asm volatile (".word 0xffffffff\n");
	return CMD_RET_FAILURE;
}

static struct cmd_tbl cmd_sub[] = {
	U_BOOT_CMD_MKENT(undefined, CONFIG_SYS_MAXARGS, 1, do_undefined,
			 "", ""),
};

static char exception_help_text[] =
	"<ex>\n"
	"  The following exceptions are available:\n"
	"  undefined  - undefined instruction\n"
	;

#include <exception.h>
