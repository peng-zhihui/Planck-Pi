/* SPDX-License-Identifier: (GPL-2.0 or BSD-3-Clause-Clear) */
/**
 * Copyright (c) 2016-present, Yann Collet, Facebook, Inc.
 * All rights reserved.
 */

/* Note : this module is expected to remain private, do not expose it */

#ifndef ERROR_H_MODULE
#define ERROR_H_MODULE

/* ****************************************
*  Dependencies
******************************************/
#include <linux/types.h> /* size_t */
#include <linux/zstd.h>  /* enum list */

/* ****************************************
*  Compiler-specific
******************************************/
#define ERR_STATIC static __attribute__((unused))

/*-****************************************
*  Customization (error_public.h)
******************************************/
typedef ZSTD_ErrorCode ERR_enum;
#define PREFIX(name) ZSTD_error_##name

/*-****************************************
*  Error codes handling
******************************************/
#define ERROR(name) ((size_t)-PREFIX(name))

ERR_STATIC unsigned ERR_isError(size_t code) { return (code > ERROR(maxCode)); }

ERR_STATIC ERR_enum ERR_getErrorCode(size_t code)
{
	if (!ERR_isError(code))
		return (ERR_enum)0;
	return (ERR_enum)(0 - code);
}

#endif /* ERROR_H_MODULE */
