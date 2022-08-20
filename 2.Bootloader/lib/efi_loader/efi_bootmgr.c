// SPDX-License-Identifier: GPL-2.0+
/*
 *  EFI boot manager
 *
 *  Copyright (c) 2017 Rob Clark
 */

#include <common.h>
#include <charset.h>
#include <log.h>
#include <malloc.h>
#include <efi_loader.h>
#include <asm/unaligned.h>

static const struct efi_boot_services *bs;
static const struct efi_runtime_services *rs;

/*
 * bootmgr implements the logic of trying to find a payload to boot
 * based on the BootOrder + BootXXXX variables, and then loading it.
 *
 * TODO detecting a special key held (f9?) and displaying a boot menu
 * like you would get on a PC would be clever.
 *
 * TODO if we had a way to write and persist variables after the OS
 * has started, we'd also want to check OsIndications to see if we
 * should do normal or recovery boot.
 */


/**
 * efi_deserialize_load_option() - parse serialized data
 *
 * Parse serialized data describing a load option and transform it to the
 * efi_load_option structure.
 *
 * @lo:		pointer to target
 * @data:	serialized data
 * @size:	size of the load option, on return size of the optional data
 * Return:	status code
 */
efi_status_t efi_deserialize_load_option(struct efi_load_option *lo, u8 *data,
					 efi_uintn_t *size)
{
	efi_uintn_t len;

	len = sizeof(u32);
	if (*size < len + 2 * sizeof(u16))
		return EFI_INVALID_PARAMETER;
	lo->attributes = get_unaligned_le32(data);
	data += len;
	*size -= len;

	len = sizeof(u16);
	lo->file_path_length = get_unaligned_le16(data);
	data += len;
	*size -= len;

	lo->label = (u16 *)data;
	len = u16_strnlen(lo->label, *size / sizeof(u16) - 1);
	if (lo->label[len])
		return EFI_INVALID_PARAMETER;
	len = (len + 1) * sizeof(u16);
	if (*size < len)
		return EFI_INVALID_PARAMETER;
	data += len;
	*size -= len;

	len = lo->file_path_length;
	if (*size < len)
		return EFI_INVALID_PARAMETER;
	lo->file_path = (struct efi_device_path *)data;
	 /*
	  * TODO: validate device path. There should be an end node within
	  * the indicated file_path_length.
	  */
	data += len;
	*size -= len;

	lo->optional_data = data;

	return EFI_SUCCESS;
}

/**
 * efi_serialize_load_option() - serialize load option
 *
 * Serialize efi_load_option structure into byte stream for BootXXXX.
 *
 * @data:	buffer for serialized data
 * @lo:		load option
 * Return:	size of allocated buffer
 */
unsigned long efi_serialize_load_option(struct efi_load_option *lo, u8 **data)
{
	unsigned long label_len;
	unsigned long size;
	u8 *p;

	label_len = (u16_strlen(lo->label) + 1) * sizeof(u16);

	/* total size */
	size = sizeof(lo->attributes);
	size += sizeof(lo->file_path_length);
	size += label_len;
	size += lo->file_path_length;
	if (lo->optional_data)
		size += (utf8_utf16_strlen((const char *)lo->optional_data)
					   + 1) * sizeof(u16);
	p = malloc(size);
	if (!p)
		return 0;

	/* copy data */
	*data = p;
	memcpy(p, &lo->attributes, sizeof(lo->attributes));
	p += sizeof(lo->attributes);

	memcpy(p, &lo->file_path_length, sizeof(lo->file_path_length));
	p += sizeof(lo->file_path_length);

	memcpy(p, lo->label, label_len);
	p += label_len;

	memcpy(p, lo->file_path, lo->file_path_length);
	p += lo->file_path_length;

	if (lo->optional_data) {
		utf8_utf16_strcpy((u16 **)&p, (const char *)lo->optional_data);
		p += sizeof(u16); /* size of trailing \0 */
	}
	return size;
}

/**
 * get_var() - get UEFI variable
 *
 * It is the caller's duty to free the returned buffer.
 *
 * @name:	name of variable
 * @vendor:	vendor GUID of variable
 * @size:	size of allocated buffer
 * Return:	buffer with variable data or NULL
 */
static void *get_var(u16 *name, const efi_guid_t *vendor,
		     efi_uintn_t *size)
{
	efi_guid_t *v = (efi_guid_t *)vendor;
	efi_status_t ret;
	void *buf = NULL;

	*size = 0;
	EFI_CALL(ret = rs->get_variable(name, v, NULL, size, buf));
	if (ret == EFI_BUFFER_TOO_SMALL) {
		buf = malloc(*size);
		EFI_CALL(ret = rs->get_variable(name, v, NULL, size, buf));
	}

	if (ret != EFI_SUCCESS) {
		free(buf);
		*size = 0;
		return NULL;
	}

	return buf;
}

/**
 * try_load_entry() - try to load image for boot option
 *
 * Attempt to load load-option number 'n', returning device_path and file_path
 * if successful. This checks that the EFI_LOAD_OPTION is active (enabled)
 * and that the specified file to boot exists.
 *
 * @n:		number of the boot option, e.g. 0x0a13 for Boot0A13
 * @handle:	on return handle for the newly installed image
 * Return:	status code
 */
static efi_status_t try_load_entry(u16 n, efi_handle_t *handle)
{
	struct efi_load_option lo;
	u16 varname[] = L"Boot0000";
	u16 hexmap[] = L"0123456789ABCDEF";
	void *load_option;
	efi_uintn_t size;
	efi_status_t ret;

	varname[4] = hexmap[(n & 0xf000) >> 12];
	varname[5] = hexmap[(n & 0x0f00) >> 8];
	varname[6] = hexmap[(n & 0x00f0) >> 4];
	varname[7] = hexmap[(n & 0x000f) >> 0];

	load_option = get_var(varname, &efi_global_variable_guid, &size);
	if (!load_option)
		return EFI_LOAD_ERROR;

	ret = efi_deserialize_load_option(&lo, load_option, &size);
	if (ret != EFI_SUCCESS) {
		log_warning("Invalid load option for %ls\n", varname);
		goto error;
	}

	if (lo.attributes & LOAD_OPTION_ACTIVE) {
		u32 attributes;

		debug("%s: trying to load \"%ls\" from %pD\n",
		      __func__, lo.label, lo.file_path);

		ret = EFI_CALL(efi_load_image(true, efi_root, lo.file_path,
					      NULL, 0, handle));
		if (ret != EFI_SUCCESS) {
			printf("Loading from Boot%04X '%ls' failed\n", n,
			       lo.label);
			goto error;
		}

		attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS |
			     EFI_VARIABLE_RUNTIME_ACCESS;
		size = sizeof(n);
		ret = EFI_CALL(efi_set_variable(
				L"BootCurrent",
				(efi_guid_t *)&efi_global_variable_guid,
				attributes, size, &n));
		if (ret != EFI_SUCCESS) {
			if (EFI_CALL(efi_unload_image(*handle))
			    != EFI_SUCCESS)
				printf("Unloading image failed\n");
			goto error;
		}

		printf("Booting: %ls\n", lo.label);
	} else {
		ret = EFI_LOAD_ERROR;
	}

error:
	free(load_option);

	return ret;
}

/**
 * efi_bootmgr_load() - try to load from BootNext or BootOrder
 *
 * Attempt to load from BootNext or in the order specified by BootOrder
 * EFI variable, the available load-options, finding and returning
 * the first one that can be loaded successfully.
 *
 * @handle:	on return handle for the newly installed image
 * Return:	status code
 */
efi_status_t efi_bootmgr_load(efi_handle_t *handle)
{
	u16 bootnext, *bootorder;
	efi_uintn_t size;
	int i, num;
	efi_status_t ret;

	bs = systab.boottime;
	rs = systab.runtime;

	/* BootNext */
	bootnext = 0;
	size = sizeof(bootnext);
	ret = EFI_CALL(efi_get_variable(L"BootNext",
					(efi_guid_t *)&efi_global_variable_guid,
					NULL, &size, &bootnext));
	if (ret == EFI_SUCCESS || ret == EFI_BUFFER_TOO_SMALL) {
		/* BootNext does exist here */
		if (ret == EFI_BUFFER_TOO_SMALL || size != sizeof(u16))
			printf("BootNext must be 16-bit integer\n");

		/* delete BootNext */
		ret = EFI_CALL(efi_set_variable(
					L"BootNext",
					(efi_guid_t *)&efi_global_variable_guid,
					EFI_VARIABLE_NON_VOLATILE, 0,
					&bootnext));

		/* load BootNext */
		if (ret == EFI_SUCCESS) {
			if (size == sizeof(u16)) {
				ret = try_load_entry(bootnext, handle);
				if (ret == EFI_SUCCESS)
					return ret;
				printf("Loading from BootNext failed, falling back to BootOrder\n");
			}
		} else {
			printf("Deleting BootNext failed\n");
		}
	}

	/* BootOrder */
	bootorder = get_var(L"BootOrder", &efi_global_variable_guid, &size);
	if (!bootorder) {
		printf("BootOrder not defined\n");
		ret = EFI_NOT_FOUND;
		goto error;
	}

	num = size / sizeof(uint16_t);
	for (i = 0; i < num; i++) {
		debug("%s: trying to load Boot%04X\n", __func__, bootorder[i]);
		ret = try_load_entry(bootorder[i], handle);
		if (ret == EFI_SUCCESS)
			break;
	}

	free(bootorder);

error:
	return ret;
}
