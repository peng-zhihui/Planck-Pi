// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2002
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 */

#include <common.h>
#include <irq_func.h>

/*
 * CPU test
 * Integer compare instructions:	cmpwi, cmplwi
 *
 * To verify these instructions the test runs them with
 * different combinations of operands, reads the condition
 * register value and compares it with the expected one.
 * The test contains a pre-built table
 * containing the description of each test case: the instruction,
 * the values of the operands, the condition field to save
 * the result in and the expected result.
 */

#include <post.h>
#include "cpu_asm.h"

#if CONFIG_POST & CONFIG_SYS_POST_CPU

extern void cpu_post_exec_11 (ulong *code, ulong *res, ulong op1);

static struct cpu_post_cmpi_s
{
    ulong cmd;
    ulong op1;
    ushort op2;
    ulong cr;
    ulong res;
} cpu_post_cmpi_table[] =
{
    {
	OP_CMPWI,
	123,
	123,
	2,
	0x02
    },
    {
	OP_CMPWI,
	123,
	133,
	3,
	0x08
    },
    {
	OP_CMPWI,
	123,
	-133,
	4,
	0x04
    },
    {
	OP_CMPLWI,
	123,
	123,
	2,
	0x02
    },
    {
	OP_CMPLWI,
	123,
	-133,
	3,
	0x08
    },
    {
	OP_CMPLWI,
	123,
	113,
	4,
	0x04
    },
};
static unsigned int cpu_post_cmpi_size = ARRAY_SIZE(cpu_post_cmpi_table);

int cpu_post_test_cmpi (void)
{
    int ret = 0;
    unsigned int i;
    int flag = disable_interrupts();

    for (i = 0; i < cpu_post_cmpi_size && ret == 0; i++)
    {
	struct cpu_post_cmpi_s *test = cpu_post_cmpi_table + i;
	unsigned long code[] =
	{
	    ASM_1IC(test->cmd, test->cr, 3, test->op2),
	    ASM_MFCR(3),
	    ASM_BLR
	};
	ulong res;

	cpu_post_exec_11 (code, & res, test->op1);

	ret = ((res >> (28 - 4 * test->cr)) & 0xe) == test->res ? 0 : -1;

	if (ret != 0)
	{
	    post_log ("Error at cmpi test %d !\n", i);
	}
    }

    if (flag)
	enable_interrupts();

    return ret;
}

#endif
