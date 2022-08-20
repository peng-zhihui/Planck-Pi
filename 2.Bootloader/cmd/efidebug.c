// SPDX-License-Identifier: GPL-2.0+
/*
 *  UEFI Shell-like command
 *
 *  Copyright (c) 2018 AKASHI Takahiro, Linaro Limited
 */

#include <charset.h>
#include <common.h>
#include <command.h>
#include <efi_loader.h>
#include <exports.h>
#include <hexdump.h>
#include <log.h>
#include <malloc.h>
#include <mapmem.h>
#include <search.h>
#include <linux/ctype.h>

#define BS systab.boottime

/**
 * efi_get_device_handle_info() - get information of UEFI device
 *
 * @handle:		Handle of UEFI device
 * @dev_path_text:	Pointer to text of device path
 * Return:		0 on success, -1 on failure
 *
 * Currently return a formatted text of device path.
 */
static int efi_get_device_handle_info(efi_handle_t handle, u16 **dev_path_text)
{
	struct efi_device_path *dp;
	efi_status_t ret;

	ret = EFI_CALL(BS->open_protocol(handle, &efi_guid_device_path,
					 (void **)&dp, NULL /* FIXME */, NULL,
					 EFI_OPEN_PROTOCOL_GET_PROTOCOL));
	if (ret == EFI_SUCCESS) {
		*dev_path_text = efi_dp_str(dp);
		return 0;
	} else {
		return -1;
	}
}

#define EFI_HANDLE_WIDTH ((int)sizeof(efi_handle_t) * 2)

static const char spc[] = "                ";
static const char sep[] = "================";

/**
 * do_efi_show_devices() - show UEFI devices
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success, CMD_RET_RET_FAILURE on failure
 *
 * Implement efidebug "devices" sub-command.
 * Show all UEFI devices and their information.
 */
static int do_efi_show_devices(struct cmd_tbl *cmdtp, int flag,
			       int argc, char *const argv[])
{
	efi_handle_t *handles;
	efi_uintn_t num, i;
	u16 *dev_path_text;
	efi_status_t ret;

	ret = EFI_CALL(efi_locate_handle_buffer(ALL_HANDLES, NULL, NULL,
						&num, &handles));
	if (ret != EFI_SUCCESS)
		return CMD_RET_FAILURE;

	if (!num)
		return CMD_RET_SUCCESS;

	printf("Device%.*s Device Path\n", EFI_HANDLE_WIDTH - 6, spc);
	printf("%.*s ====================\n", EFI_HANDLE_WIDTH, sep);
	for (i = 0; i < num; i++) {
		if (!efi_get_device_handle_info(handles[i], &dev_path_text)) {
			printf("%p %ls\n", handles[i], dev_path_text);
			efi_free_pool(dev_path_text);
		}
	}

	efi_free_pool(handles);

	return CMD_RET_SUCCESS;
}

/**
 * efi_get_driver_handle_info() - get information of UEFI driver
 *
 * @handle:		Handle of UEFI device
 * @driver_name:	Driver name
 * @image_path:		Pointer to text of device path
 * Return:		0 on success, -1 on failure
 *
 * Currently return no useful information as all UEFI drivers are
 * built-in..
 */
static int efi_get_driver_handle_info(efi_handle_t handle, u16 **driver_name,
				      u16 **image_path)
{
	struct efi_handler *handler;
	struct efi_loaded_image *image;
	efi_status_t ret;

	/*
	 * driver name
	 * TODO: support EFI_COMPONENT_NAME2_PROTOCOL
	 */
	*driver_name = NULL;

	/* image name */
	ret = efi_search_protocol(handle, &efi_guid_loaded_image, &handler);
	if (ret != EFI_SUCCESS) {
		*image_path = NULL;
		return 0;
	}

	image = handler->protocol_interface;
	*image_path = efi_dp_str(image->file_path);

	return 0;
}

/**
 * do_efi_show_drivers() - show UEFI drivers
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success, CMD_RET_RET_FAILURE on failure
 *
 * Implement efidebug "drivers" sub-command.
 * Show all UEFI drivers and their information.
 */
static int do_efi_show_drivers(struct cmd_tbl *cmdtp, int flag,
			       int argc, char *const argv[])
{
	efi_handle_t *handles;
	efi_uintn_t num, i;
	u16 *driver_name, *image_path_text;
	efi_status_t ret;

	ret = EFI_CALL(efi_locate_handle_buffer(
				BY_PROTOCOL, &efi_guid_driver_binding_protocol,
				NULL, &num, &handles));
	if (ret != EFI_SUCCESS)
		return CMD_RET_FAILURE;

	if (!num)
		return CMD_RET_SUCCESS;

	printf("Driver%.*s Name                 Image Path\n",
	       EFI_HANDLE_WIDTH - 6, spc);
	printf("%.*s ==================== ====================\n",
	       EFI_HANDLE_WIDTH, sep);
	for (i = 0; i < num; i++) {
		if (!efi_get_driver_handle_info(handles[i], &driver_name,
						&image_path_text)) {
			if (image_path_text)
				printf("%p %-20ls %ls\n", handles[i],
				       driver_name, image_path_text);
			else
				printf("%p %-20ls <built-in>\n",
				       handles[i], driver_name);
			efi_free_pool(driver_name);
			efi_free_pool(image_path_text);
		}
	}

	efi_free_pool(handles);

	return CMD_RET_SUCCESS;
}

static const struct {
	const char *text;
	const efi_guid_t guid;
} guid_list[] = {
	{
		"Device Path",
		EFI_DEVICE_PATH_PROTOCOL_GUID,
	},
	{
		"Device Path To Text",
		EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID,
	},
	{
		"Device Path Utilities",
		EFI_DEVICE_PATH_UTILITIES_PROTOCOL_GUID,
	},
	{
		"Unicode Collation 2",
		EFI_UNICODE_COLLATION_PROTOCOL2_GUID,
	},
	{
		"Driver Binding",
		EFI_DRIVER_BINDING_PROTOCOL_GUID,
	},
	{
		"Simple Text Input",
		EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID,
	},
	{
		"Simple Text Input Ex",
		EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID,
	},
	{
		"Simple Text Output",
		EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID,
	},
	{
		"Block IO",
		EFI_BLOCK_IO_PROTOCOL_GUID,
	},
	{
		"Simple File System",
		EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
	},
	{
		"Loaded Image",
		EFI_LOADED_IMAGE_PROTOCOL_GUID,
	},
	{
		"Graphics Output",
		EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,
	},
	{
		"HII String",
		EFI_HII_STRING_PROTOCOL_GUID,
	},
	{
		"HII Database",
		EFI_HII_DATABASE_PROTOCOL_GUID,
	},
	{
		"HII Config Routing",
		EFI_HII_CONFIG_ROUTING_PROTOCOL_GUID,
	},
	{
		"Load File2",
		EFI_LOAD_FILE2_PROTOCOL_GUID,
	},
	{
		"Simple Network",
		EFI_SIMPLE_NETWORK_PROTOCOL_GUID,
	},
	{
		"PXE Base Code",
		EFI_PXE_BASE_CODE_PROTOCOL_GUID,
	},
	/* Configuration table GUIDs */
	{
		"ACPI table",
		EFI_ACPI_TABLE_GUID,
	},
	{
		"device tree",
		EFI_FDT_GUID,
	},
	{
		"SMBIOS table",
		SMBIOS_TABLE_GUID,
	},
	{
		"Runtime properties",
		EFI_RT_PROPERTIES_TABLE_GUID,
	},
};

/**
 * get_guid_text - get string of GUID
 *
 * Return description of GUID.
 *
 * @guid:	GUID
 * Return:	description of GUID or NULL
 */
static const char *get_guid_text(const void *guid)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(guid_list); i++) {
		/*
		 * As guidcmp uses memcmp() we can safely accept unaligned
		 * GUIDs.
		 */
		if (!guidcmp(&guid_list[i].guid, guid))
			return guid_list[i].text;
	}

	return NULL;
}

/**
 * do_efi_show_handles() - show UEFI handles
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success, CMD_RET_RET_FAILURE on failure
 *
 * Implement efidebug "dh" sub-command.
 * Show all UEFI handles and their information, currently all protocols
 * added to handle.
 */
static int do_efi_show_handles(struct cmd_tbl *cmdtp, int flag,
			       int argc, char *const argv[])
{
	efi_handle_t *handles;
	efi_guid_t **guid;
	efi_uintn_t num, count, i, j;
	const char *guid_text;
	efi_status_t ret;

	ret = EFI_CALL(efi_locate_handle_buffer(ALL_HANDLES, NULL, NULL,
						&num, &handles));
	if (ret != EFI_SUCCESS)
		return CMD_RET_FAILURE;

	if (!num)
		return CMD_RET_SUCCESS;

	printf("Handle%.*s Protocols\n", EFI_HANDLE_WIDTH - 6, spc);
	printf("%.*s ====================\n", EFI_HANDLE_WIDTH, sep);
	for (i = 0; i < num; i++) {
		printf("%p", handles[i]);
		ret = EFI_CALL(BS->protocols_per_handle(handles[i], &guid,
							&count));
		if (ret || !count) {
			putc('\n');
			continue;
		}

		for (j = 0; j < count; j++) {
			if (j)
				printf(", ");
			else
				putc(' ');

			guid_text = get_guid_text(guid[j]);
			if (guid_text)
				puts(guid_text);
			else
				printf("%pUl", guid[j]);
		}
		putc('\n');
	}

	efi_free_pool(handles);

	return CMD_RET_SUCCESS;
}

/**
 * do_efi_show_images() - show UEFI images
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success, CMD_RET_RET_FAILURE on failure
 *
 * Implement efidebug "images" sub-command.
 * Show all UEFI loaded images and their information.
 */
static int do_efi_show_images(struct cmd_tbl *cmdtp, int flag,
			      int argc, char *const argv[])
{
	efi_print_image_infos(NULL);

	return CMD_RET_SUCCESS;
}

static const char * const efi_mem_type_string[] = {
	[EFI_RESERVED_MEMORY_TYPE] = "RESERVED",
	[EFI_LOADER_CODE] = "LOADER CODE",
	[EFI_LOADER_DATA] = "LOADER DATA",
	[EFI_BOOT_SERVICES_CODE] = "BOOT CODE",
	[EFI_BOOT_SERVICES_DATA] = "BOOT DATA",
	[EFI_RUNTIME_SERVICES_CODE] = "RUNTIME CODE",
	[EFI_RUNTIME_SERVICES_DATA] = "RUNTIME DATA",
	[EFI_CONVENTIONAL_MEMORY] = "CONVENTIONAL",
	[EFI_UNUSABLE_MEMORY] = "UNUSABLE MEM",
	[EFI_ACPI_RECLAIM_MEMORY] = "ACPI RECLAIM MEM",
	[EFI_ACPI_MEMORY_NVS] = "ACPI NVS",
	[EFI_MMAP_IO] = "IO",
	[EFI_MMAP_IO_PORT] = "IO PORT",
	[EFI_PAL_CODE] = "PAL",
	[EFI_PERSISTENT_MEMORY_TYPE] = "PERSISTENT",
};

static const struct efi_mem_attrs {
	const u64 bit;
	const char *text;
} efi_mem_attrs[] = {
	{EFI_MEMORY_UC, "UC"},
	{EFI_MEMORY_UC, "UC"},
	{EFI_MEMORY_WC, "WC"},
	{EFI_MEMORY_WT, "WT"},
	{EFI_MEMORY_WB, "WB"},
	{EFI_MEMORY_UCE, "UCE"},
	{EFI_MEMORY_WP, "WP"},
	{EFI_MEMORY_RP, "RP"},
	{EFI_MEMORY_XP, "WP"},
	{EFI_MEMORY_NV, "NV"},
	{EFI_MEMORY_MORE_RELIABLE, "REL"},
	{EFI_MEMORY_RO, "RO"},
	{EFI_MEMORY_SP, "SP"},
	{EFI_MEMORY_RUNTIME, "RT"},
};

/**
 * print_memory_attributes() - print memory map attributes
 *
 * @attributes:	Attribute value
 *
 * Print memory map attributes
 */
static void print_memory_attributes(u64 attributes)
{
	int sep, i;

	for (sep = 0, i = 0; i < ARRAY_SIZE(efi_mem_attrs); i++)
		if (attributes & efi_mem_attrs[i].bit) {
			if (sep) {
				putc('|');
			} else {
				putc(' ');
				sep = 1;
			}
			puts(efi_mem_attrs[i].text);
		}
}

#define EFI_PHYS_ADDR_WIDTH (int)(sizeof(efi_physical_addr_t) * 2)

/**
 * do_efi_show_memmap() - show UEFI memory map
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success, CMD_RET_RET_FAILURE on failure
 *
 * Implement efidebug "memmap" sub-command.
 * Show UEFI memory map.
 */
static int do_efi_show_memmap(struct cmd_tbl *cmdtp, int flag,
			      int argc, char *const argv[])
{
	struct efi_mem_desc *memmap = NULL, *map;
	efi_uintn_t map_size = 0;
	const char *type;
	int i;
	efi_status_t ret;

	ret = efi_get_memory_map(&map_size, memmap, NULL, NULL, NULL);
	if (ret == EFI_BUFFER_TOO_SMALL) {
		map_size += sizeof(struct efi_mem_desc); /* for my own */
		ret = efi_allocate_pool(EFI_LOADER_DATA, map_size,
					(void *)&memmap);
		if (ret != EFI_SUCCESS)
			return CMD_RET_FAILURE;
		ret = efi_get_memory_map(&map_size, memmap, NULL, NULL, NULL);
	}
	if (ret != EFI_SUCCESS) {
		efi_free_pool(memmap);
		return CMD_RET_FAILURE;
	}

	printf("Type             Start%.*s End%.*s Attributes\n",
	       EFI_PHYS_ADDR_WIDTH - 5, spc, EFI_PHYS_ADDR_WIDTH - 3, spc);
	printf("================ %.*s %.*s ==========\n",
	       EFI_PHYS_ADDR_WIDTH, sep, EFI_PHYS_ADDR_WIDTH, sep);
	/*
	 * Coverity check: dereferencing null pointer "map."
	 * This is a false positive as memmap will always be
	 * populated by allocate_pool() above.
	 */
	for (i = 0, map = memmap; i < map_size / sizeof(*map); map++, i++) {
		if (map->type < ARRAY_SIZE(efi_mem_type_string))
			type = efi_mem_type_string[map->type];
		else
			type = "(unknown)";

		printf("%-16s %.*llx-%.*llx", type,
		       EFI_PHYS_ADDR_WIDTH,
		       (u64)map_to_sysmem((void *)(uintptr_t)
					  map->physical_start),
		       EFI_PHYS_ADDR_WIDTH,
		       (u64)map_to_sysmem((void *)(uintptr_t)
					  (map->physical_start +
					   map->num_pages * EFI_PAGE_SIZE)));

		print_memory_attributes(map->attribute);
		putc('\n');
	}

	efi_free_pool(memmap);

	return CMD_RET_SUCCESS;
}

/**
 * do_efi_show_tables() - show UEFI configuration tables
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success, CMD_RET_RET_FAILURE on failure
 *
 * Implement efidebug "tables" sub-command.
 * Show UEFI configuration tables.
 */
static int do_efi_show_tables(struct cmd_tbl *cmdtp, int flag,
			      int argc, char *const argv[])
{
	efi_uintn_t i;
	const char *guid_str;

	for (i = 0; i < systab.nr_tables; ++i) {
		guid_str = get_guid_text(&systab.tables[i].guid);
		if (!guid_str)
			guid_str = "";
		printf("%pUl %s\n", &systab.tables[i].guid, guid_str);
	}

	return CMD_RET_SUCCESS;
}

/**
 * do_efi_boot_add() - set UEFI load option
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success,
 *		CMD_RET_USAGE or CMD_RET_RET_FAILURE on failure
 *
 * Implement efidebug "boot add" sub-command. Create or change UEFI load option.
 *
 *     efidebug boot add <id> <label> <interface> <devnum>[:<part>] <file> <options>
 */
static int do_efi_boot_add(struct cmd_tbl *cmdtp, int flag,
			   int argc, char *const argv[])
{
	int id;
	char *endp;
	char var_name[9];
	u16 var_name16[9], *p;
	efi_guid_t guid;
	size_t label_len, label_len16;
	u16 *label;
	struct efi_device_path *device_path = NULL, *file_path = NULL;
	struct efi_load_option lo;
	void *data = NULL;
	efi_uintn_t size;
	efi_status_t ret;
	int r = CMD_RET_SUCCESS;

	if (argc < 6 || argc > 7)
		return CMD_RET_USAGE;

	id = (int)simple_strtoul(argv[1], &endp, 16);
	if (*endp != '\0' || id > 0xffff)
		return CMD_RET_USAGE;

	sprintf(var_name, "Boot%04X", id);
	p = var_name16;
	utf8_utf16_strncpy(&p, var_name, 9);

	guid = efi_global_variable_guid;

	/* attributes */
	lo.attributes = LOAD_OPTION_ACTIVE; /* always ACTIVE */

	/* label */
	label_len = strlen(argv[2]);
	label_len16 = utf8_utf16_strnlen(argv[2], label_len);
	label = malloc((label_len16 + 1) * sizeof(u16));
	if (!label)
		return CMD_RET_FAILURE;
	lo.label = label; /* label will be changed below */
	utf8_utf16_strncpy(&label, argv[2], label_len);

	/* file path */
	ret = efi_dp_from_name(argv[3], argv[4], argv[5], &device_path,
			       &file_path);
	if (ret != EFI_SUCCESS) {
		printf("Cannot create device path for \"%s %s\"\n",
		       argv[3], argv[4]);
		r = CMD_RET_FAILURE;
		goto out;
	}
	lo.file_path = file_path;
	lo.file_path_length = efi_dp_size(file_path)
				+ sizeof(struct efi_device_path); /* for END */

	/* optional data */
	if (argc == 6)
		lo.optional_data = NULL;
	else
		lo.optional_data = (const u8 *)argv[6];

	size = efi_serialize_load_option(&lo, (u8 **)&data);
	if (!size) {
		r = CMD_RET_FAILURE;
		goto out;
	}

	ret = EFI_CALL(efi_set_variable(var_name16, &guid,
					EFI_VARIABLE_NON_VOLATILE |
					EFI_VARIABLE_BOOTSERVICE_ACCESS |
					EFI_VARIABLE_RUNTIME_ACCESS,
					size, data));
	if (ret != EFI_SUCCESS) {
		printf("Cannot set %ls\n", var_name16);
		r = CMD_RET_FAILURE;
	}
out:
	free(data);
	efi_free_pool(device_path);
	efi_free_pool(file_path);
	free(lo.label);

	return r;
}

/**
 * do_efi_boot_rm() - delete UEFI load options
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success, CMD_RET_RET_FAILURE on failure
 *
 * Implement efidebug "boot rm" sub-command.
 * Delete UEFI load options.
 *
 *     efidebug boot rm <id> ...
 */
static int do_efi_boot_rm(struct cmd_tbl *cmdtp, int flag,
			  int argc, char *const argv[])
{
	efi_guid_t guid;
	int id, i;
	char *endp;
	char var_name[9];
	u16 var_name16[9], *p;
	efi_status_t ret;

	if (argc == 1)
		return CMD_RET_USAGE;

	guid = efi_global_variable_guid;
	for (i = 1; i < argc; i++, argv++) {
		id = (int)simple_strtoul(argv[1], &endp, 16);
		if (*endp != '\0' || id > 0xffff)
			return CMD_RET_FAILURE;

		sprintf(var_name, "Boot%04X", id);
		p = var_name16;
		utf8_utf16_strncpy(&p, var_name, 9);

		ret = EFI_CALL(efi_set_variable(var_name16, &guid, 0, 0, NULL));
		if (ret) {
			printf("Cannot remove %ls\n", var_name16);
			return CMD_RET_FAILURE;
		}
	}

	return CMD_RET_SUCCESS;
}

/**
 * show_efi_boot_opt_data() - dump UEFI load option
 *
 * @varname16:	variable name
 * @data:	value of UEFI load option variable
 * @size:	size of the boot option
 *
 * Decode the value of UEFI load option variable and print information.
 */
static void show_efi_boot_opt_data(u16 *varname16, void *data, size_t *size)
{
	struct efi_load_option lo;
	char *label, *p;
	size_t label_len16, label_len;
	u16 *dp_str;
	efi_status_t ret;

	ret = efi_deserialize_load_option(&lo, data, size);
	if (ret != EFI_SUCCESS) {
		printf("%ls: invalid load option\n", varname16);
		return;
	}

	label_len16 = u16_strlen(lo.label);
	label_len = utf16_utf8_strnlen(lo.label, label_len16);
	label = malloc(label_len + 1);
	if (!label)
		return;
	p = label;
	utf16_utf8_strncpy(&p, lo.label, label_len16);

	printf("%ls:\nattributes: %c%c%c (0x%08x)\n",
	       varname16,
	       /* ACTIVE */
	       lo.attributes & LOAD_OPTION_ACTIVE ? 'A' : '-',
	       /* FORCE RECONNECT */
	       lo.attributes & LOAD_OPTION_FORCE_RECONNECT ? 'R' : '-',
	       /* HIDDEN */
	       lo.attributes & LOAD_OPTION_HIDDEN ? 'H' : '-',
	       lo.attributes);
	printf("  label: %s\n", label);

	dp_str = efi_dp_str(lo.file_path);
	printf("  file_path: %ls\n", dp_str);
	efi_free_pool(dp_str);

	printf("  data:\n");
	print_hex_dump("    ", DUMP_PREFIX_OFFSET, 16, 1,
		       lo.optional_data, *size, true);
	free(label);
}

/**
 * show_efi_boot_opt() - dump UEFI load option
 *
 * @varname16:	variable name
 *
 * Dump information defined by UEFI load option.
 */
static void show_efi_boot_opt(u16 *varname16)
{
	void *data;
	efi_uintn_t size;
	efi_status_t ret;

	size = 0;
	ret = EFI_CALL(efi_get_variable(varname16, &efi_global_variable_guid,
					NULL, &size, NULL));
	if (ret == EFI_BUFFER_TOO_SMALL) {
		data = malloc(size);
		if (!data) {
			printf("ERROR: Out of memory\n");
			return;
		}
		ret = EFI_CALL(efi_get_variable(varname16,
						&efi_global_variable_guid,
						NULL, &size, data));
		if (ret == EFI_SUCCESS)
			show_efi_boot_opt_data(varname16, data, &size);
		free(data);
	}
}

static int u16_tohex(u16 c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	/* not hexadecimal */
	return -1;
}

/**
 * show_efi_boot_dump() - dump all UEFI load options
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success, CMD_RET_RET_FAILURE on failure
 *
 * Implement efidebug "boot dump" sub-command.
 * Dump information of all UEFI load options defined.
 *
 *     efidebug boot dump
 */
static int do_efi_boot_dump(struct cmd_tbl *cmdtp, int flag,
			    int argc, char *const argv[])
{
	u16 *var_name16, *p;
	efi_uintn_t buf_size, size;
	efi_guid_t guid;
	int id, i, digit;
	efi_status_t ret;

	if (argc > 1)
		return CMD_RET_USAGE;

	buf_size = 128;
	var_name16 = malloc(buf_size);
	if (!var_name16)
		return CMD_RET_FAILURE;

	var_name16[0] = 0;
	for (;;) {
		size = buf_size;
		ret = EFI_CALL(efi_get_next_variable_name(&size, var_name16,
							  &guid));
		if (ret == EFI_NOT_FOUND)
			break;
		if (ret == EFI_BUFFER_TOO_SMALL) {
			buf_size = size;
			p = realloc(var_name16, buf_size);
			if (!p) {
				free(var_name16);
				return CMD_RET_FAILURE;
			}
			var_name16 = p;
			ret = EFI_CALL(efi_get_next_variable_name(&size,
								  var_name16,
								  &guid));
		}
		if (ret != EFI_SUCCESS) {
			free(var_name16);
			return CMD_RET_FAILURE;
		}

		if (memcmp(var_name16, L"Boot", 8))
			continue;

		for (id = 0, i = 0; i < 4; i++) {
			digit = u16_tohex(var_name16[4 + i]);
			if (digit < 0)
				break;
			id = (id << 4) + digit;
		}
		if (i == 4 && !var_name16[8])
			show_efi_boot_opt(var_name16);
	}

	free(var_name16);

	return CMD_RET_SUCCESS;
}

/**
 * show_efi_boot_order() - show order of UEFI load options
 *
 * Return:	CMD_RET_SUCCESS on success, CMD_RET_RET_FAILURE on failure
 *
 * Show order of UEFI load options defined by BootOrder variable.
 */
static int show_efi_boot_order(void)
{
	u16 *bootorder;
	efi_uintn_t size;
	int num, i;
	char var_name[9];
	u16 var_name16[9], *p16;
	void *data;
	struct efi_load_option lo;
	char *label, *p;
	size_t label_len16, label_len;
	efi_status_t ret;

	size = 0;
	ret = EFI_CALL(efi_get_variable(L"BootOrder", &efi_global_variable_guid,
					NULL, &size, NULL));
	if (ret != EFI_BUFFER_TOO_SMALL) {
		if (ret == EFI_NOT_FOUND) {
			printf("BootOrder not defined\n");
			return CMD_RET_SUCCESS;
		} else {
			return CMD_RET_FAILURE;
		}
	}
	bootorder = malloc(size);
	if (!bootorder) {
		printf("ERROR: Out of memory\n");
		return CMD_RET_FAILURE;
	}
	ret = EFI_CALL(efi_get_variable(L"BootOrder", &efi_global_variable_guid,
					NULL, &size, bootorder));
	if (ret != EFI_SUCCESS) {
		ret = CMD_RET_FAILURE;
		goto out;
	}

	num = size / sizeof(u16);
	for (i = 0; i < num; i++) {
		sprintf(var_name, "Boot%04X", bootorder[i]);
		p16 = var_name16;
		utf8_utf16_strncpy(&p16, var_name, 9);

		size = 0;
		ret = EFI_CALL(efi_get_variable(var_name16,
						&efi_global_variable_guid, NULL,
						&size, NULL));
		if (ret != EFI_BUFFER_TOO_SMALL) {
			printf("%2d: %s: (not defined)\n", i + 1, var_name);
			continue;
		}

		data = malloc(size);
		if (!data) {
			ret = CMD_RET_FAILURE;
			goto out;
		}
		ret = EFI_CALL(efi_get_variable(var_name16,
						&efi_global_variable_guid, NULL,
						&size, data));
		if (ret != EFI_SUCCESS) {
			free(data);
			ret = CMD_RET_FAILURE;
			goto out;
		}

		ret = efi_deserialize_load_option(&lo, data, &size);
		if (ret != EFI_SUCCESS) {
			printf("%ls: invalid load option\n", var_name16);
			ret = CMD_RET_FAILURE;
			goto out;
		}

		label_len16 = u16_strlen(lo.label);
		label_len = utf16_utf8_strnlen(lo.label, label_len16);
		label = malloc(label_len + 1);
		if (!label) {
			free(data);
			ret = CMD_RET_FAILURE;
			goto out;
		}
		p = label;
		utf16_utf8_strncpy(&p, lo.label, label_len16);
		printf("%2d: %s: %s\n", i + 1, var_name, label);
		free(label);

		free(data);
	}
out:
	free(bootorder);

	return ret;
}

/**
 * do_efi_boot_next() - manage UEFI BootNext variable
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success,
 *		CMD_RET_USAGE or CMD_RET_RET_FAILURE on failure
 *
 * Implement efidebug "boot next" sub-command.
 * Set BootNext variable.
 *
 *     efidebug boot next <id>
 */
static int do_efi_boot_next(struct cmd_tbl *cmdtp, int flag,
			    int argc, char *const argv[])
{
	u16 bootnext;
	efi_uintn_t size;
	char *endp;
	efi_guid_t guid;
	efi_status_t ret;
	int r = CMD_RET_SUCCESS;

	if (argc != 2)
		return CMD_RET_USAGE;

	bootnext = (u16)simple_strtoul(argv[1], &endp, 16);
	if (*endp) {
		printf("invalid value: %s\n", argv[1]);
		r = CMD_RET_FAILURE;
		goto out;
	}

	guid = efi_global_variable_guid;
	size = sizeof(u16);
	ret = EFI_CALL(efi_set_variable(L"BootNext", &guid,
					EFI_VARIABLE_NON_VOLATILE |
					EFI_VARIABLE_BOOTSERVICE_ACCESS |
					EFI_VARIABLE_RUNTIME_ACCESS,
					size, &bootnext));
	if (ret != EFI_SUCCESS) {
		printf("Cannot set BootNext\n");
		r = CMD_RET_FAILURE;
	}
out:
	return r;
}

/**
 * do_efi_boot_order() - manage UEFI BootOrder variable
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success, CMD_RET_RET_FAILURE on failure
 *
 * Implement efidebug "boot order" sub-command.
 * Show order of UEFI load options, or change it in BootOrder variable.
 *
 *     efidebug boot order [<id> ...]
 */
static int do_efi_boot_order(struct cmd_tbl *cmdtp, int flag,
			     int argc, char *const argv[])
{
	u16 *bootorder = NULL;
	efi_uintn_t size;
	int id, i;
	char *endp;
	efi_guid_t guid;
	efi_status_t ret;
	int r = CMD_RET_SUCCESS;

	if (argc == 1)
		return show_efi_boot_order();

	argc--;
	argv++;

	size = argc * sizeof(u16);
	bootorder = malloc(size);
	if (!bootorder)
		return CMD_RET_FAILURE;

	for (i = 0; i < argc; i++) {
		id = (int)simple_strtoul(argv[i], &endp, 16);
		if (*endp != '\0' || id > 0xffff) {
			printf("invalid value: %s\n", argv[i]);
			r = CMD_RET_FAILURE;
			goto out;
		}

		bootorder[i] = (u16)id;
	}

	guid = efi_global_variable_guid;
	ret = EFI_CALL(efi_set_variable(L"BootOrder", &guid,
					EFI_VARIABLE_NON_VOLATILE |
					EFI_VARIABLE_BOOTSERVICE_ACCESS |
					EFI_VARIABLE_RUNTIME_ACCESS,
					size, bootorder));
	if (ret != EFI_SUCCESS) {
		printf("Cannot set BootOrder\n");
		r = CMD_RET_FAILURE;
	}
out:
	free(bootorder);

	return r;
}

static struct cmd_tbl cmd_efidebug_boot_sub[] = {
	U_BOOT_CMD_MKENT(add, CONFIG_SYS_MAXARGS, 1, do_efi_boot_add, "", ""),
	U_BOOT_CMD_MKENT(rm, CONFIG_SYS_MAXARGS, 1, do_efi_boot_rm, "", ""),
	U_BOOT_CMD_MKENT(dump, CONFIG_SYS_MAXARGS, 1, do_efi_boot_dump, "", ""),
	U_BOOT_CMD_MKENT(next, CONFIG_SYS_MAXARGS, 1, do_efi_boot_next, "", ""),
	U_BOOT_CMD_MKENT(order, CONFIG_SYS_MAXARGS, 1, do_efi_boot_order,
			 "", ""),
};

/**
 * do_efi_boot_opt() - manage UEFI load options
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success,
 *		CMD_RET_USAGE or CMD_RET_RET_FAILURE on failure
 *
 * Implement efidebug "boot" sub-command.
 */
static int do_efi_boot_opt(struct cmd_tbl *cmdtp, int flag,
			   int argc, char *const argv[])
{
	struct cmd_tbl *cp;

	if (argc < 2)
		return CMD_RET_USAGE;

	argc--; argv++;

	cp = find_cmd_tbl(argv[0], cmd_efidebug_boot_sub,
			  ARRAY_SIZE(cmd_efidebug_boot_sub));
	if (!cp)
		return CMD_RET_USAGE;

	return cp->cmd(cmdtp, flag, argc, argv);
}

/**
 * do_efi_test_bootmgr() - run simple bootmgr for test
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success,
 *		CMD_RET_USAGE or CMD_RET_RET_FAILURE on failure
 *
 * Implement efidebug "test bootmgr" sub-command.
 * Run simple bootmgr for test.
 *
 *     efidebug test bootmgr
 */
static int do_efi_test_bootmgr(struct cmd_tbl *cmdtp, int flag,
			       int argc, char * const argv[])
{
	efi_handle_t image;
	efi_uintn_t exit_data_size = 0;
	u16 *exit_data = NULL;
	efi_status_t ret;

	ret = efi_bootmgr_load(&image);
	printf("efi_bootmgr_load() returned: %ld\n", ret & ~EFI_ERROR_MASK);

	/* We call efi_start_image() even if error for test purpose. */
	ret = EFI_CALL(efi_start_image(image, &exit_data_size, &exit_data));
	printf("efi_start_image() returned: %ld\n", ret & ~EFI_ERROR_MASK);
	if (ret && exit_data)
		efi_free_pool(exit_data);

	efi_restore_gd();

	return CMD_RET_SUCCESS;
}

static struct cmd_tbl cmd_efidebug_test_sub[] = {
	U_BOOT_CMD_MKENT(bootmgr, CONFIG_SYS_MAXARGS, 1, do_efi_test_bootmgr,
			 "", ""),
};

/**
 * do_efi_test() - manage UEFI load options
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success,
 *		CMD_RET_USAGE or CMD_RET_RET_FAILURE on failure
 *
 * Implement efidebug "test" sub-command.
 */
static int do_efi_test(struct cmd_tbl *cmdtp, int flag,
		       int argc, char * const argv[])
{
	struct cmd_tbl *cp;

	if (argc < 2)
		return CMD_RET_USAGE;

	argc--; argv++;

	cp = find_cmd_tbl(argv[0], cmd_efidebug_test_sub,
			  ARRAY_SIZE(cmd_efidebug_test_sub));
	if (!cp)
		return CMD_RET_USAGE;

	return cp->cmd(cmdtp, flag, argc, argv);
}

/**
 * do_efi_query_info() - QueryVariableInfo EFI service
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success,
 *		CMD_RET_USAGE or CMD_RET_FAILURE on failure
 *
 * Implement efidebug "test" sub-command.
 */

static int do_efi_query_info(struct cmd_tbl *cmdtp, int flag,
			     int argc, char * const argv[])
{
	efi_status_t ret;
	u32 attr = 0;
	u64 max_variable_storage_size;
	u64 remain_variable_storage_size;
	u64 max_variable_size;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-bs"))
			attr |= EFI_VARIABLE_BOOTSERVICE_ACCESS;
		else if (!strcmp(argv[i], "-rt"))
			attr |= EFI_VARIABLE_RUNTIME_ACCESS;
		else if (!strcmp(argv[i], "-nv"))
			attr |= EFI_VARIABLE_NON_VOLATILE;
		else if (!strcmp(argv[i], "-at"))
			attr |=
				EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS;
	}

	ret = EFI_CALL(efi_query_variable_info(attr,
					       &max_variable_storage_size,
					       &remain_variable_storage_size,
					       &max_variable_size));
	if (ret != EFI_SUCCESS) {
		printf("Error: Cannot query UEFI variables, r = %lu\n",
		       ret & ~EFI_ERROR_MASK);
		return CMD_RET_FAILURE;
	}

	printf("Max storage size %llu\n", max_variable_storage_size);
	printf("Remaining storage size %llu\n", remain_variable_storage_size);
	printf("Max variable size %llu\n", max_variable_size);

	return CMD_RET_SUCCESS;
}

static struct cmd_tbl cmd_efidebug_sub[] = {
	U_BOOT_CMD_MKENT(boot, CONFIG_SYS_MAXARGS, 1, do_efi_boot_opt, "", ""),
	U_BOOT_CMD_MKENT(devices, CONFIG_SYS_MAXARGS, 1, do_efi_show_devices,
			 "", ""),
	U_BOOT_CMD_MKENT(drivers, CONFIG_SYS_MAXARGS, 1, do_efi_show_drivers,
			 "", ""),
	U_BOOT_CMD_MKENT(dh, CONFIG_SYS_MAXARGS, 1, do_efi_show_handles,
			 "", ""),
	U_BOOT_CMD_MKENT(images, CONFIG_SYS_MAXARGS, 1, do_efi_show_images,
			 "", ""),
	U_BOOT_CMD_MKENT(memmap, CONFIG_SYS_MAXARGS, 1, do_efi_show_memmap,
			 "", ""),
	U_BOOT_CMD_MKENT(tables, CONFIG_SYS_MAXARGS, 1, do_efi_show_tables,
			 "", ""),
	U_BOOT_CMD_MKENT(test, CONFIG_SYS_MAXARGS, 1, do_efi_test,
			 "", ""),
	U_BOOT_CMD_MKENT(query, CONFIG_SYS_MAXARGS, 1, do_efi_query_info,
			 "", ""),
};

/**
 * do_efidebug() - display and configure UEFI environment
 *
 * @cmdtp:	Command table
 * @flag:	Command flag
 * @argc:	Number of arguments
 * @argv:	Argument array
 * Return:	CMD_RET_SUCCESS on success,
 *		CMD_RET_USAGE or CMD_RET_RET_FAILURE on failure
 *
 * Implement efidebug command which allows us to display and
 * configure UEFI environment.
 */
static int do_efidebug(struct cmd_tbl *cmdtp, int flag,
		       int argc, char *const argv[])
{
	struct cmd_tbl *cp;
	efi_status_t r;

	if (argc < 2)
		return CMD_RET_USAGE;

	argc--; argv++;

	/* Initialize UEFI drivers */
	r = efi_init_obj_list();
	if (r != EFI_SUCCESS) {
		printf("Error: Cannot initialize UEFI sub-system, r = %lu\n",
		       r & ~EFI_ERROR_MASK);
		return CMD_RET_FAILURE;
	}

	cp = find_cmd_tbl(argv[0], cmd_efidebug_sub,
			  ARRAY_SIZE(cmd_efidebug_sub));
	if (!cp)
		return CMD_RET_USAGE;

	return cp->cmd(cmdtp, flag, argc, argv);
}

#ifdef CONFIG_SYS_LONGHELP
static char efidebug_help_text[] =
	"  - UEFI Shell-like interface to configure UEFI environment\n"
	"\n"
	"efidebug boot add <bootid> <label> <interface> <devnum>[:<part>] <file path> [<load options>]\n"
	"  - set UEFI BootXXXX variable\n"
	"    <load options> will be passed to UEFI application\n"
	"efidebug boot rm <bootid#1> [<bootid#2> [<bootid#3> [...]]]\n"
	"  - delete UEFI BootXXXX variables\n"
	"efidebug boot dump\n"
	"  - dump all UEFI BootXXXX variables\n"
	"efidebug boot next <bootid>\n"
	"  - set UEFI BootNext variable\n"
	"efidebug boot order [<bootid#1> [<bootid#2> [<bootid#3> [...]]]]\n"
	"  - set/show UEFI boot order\n"
	"\n"
	"efidebug devices\n"
	"  - show UEFI devices\n"
	"efidebug drivers\n"
	"  - show UEFI drivers\n"
	"efidebug dh\n"
	"  - show UEFI handles\n"
	"efidebug images\n"
	"  - show loaded images\n"
	"efidebug memmap\n"
	"  - show UEFI memory map\n"
	"efidebug tables\n"
	"  - show UEFI configuration tables\n"
	"efidebug test bootmgr\n"
	"  - run simple bootmgr for test\n"
	"efidebug query [-nv][-bs][-rt][-at]\n"
	"  - show size of UEFI variables store\n";
#endif

U_BOOT_CMD(
	efidebug, 10, 0, do_efidebug,
	"Configure UEFI environment",
	efidebug_help_text
);
