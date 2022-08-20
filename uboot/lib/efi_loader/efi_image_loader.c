// SPDX-License-Identifier: GPL-2.0+
/*
 *  EFI image loader
 *
 *  based partly on wine code
 *
 *  Copyright (c) 2016 Alexander Graf
 */

#include <common.h>
#include <cpu_func.h>
#include <efi_loader.h>
#include <malloc.h>
#include <pe.h>
#include <sort.h>
#include <crypto/pkcs7_parser.h>
#include <linux/err.h>

const efi_guid_t efi_global_variable_guid = EFI_GLOBAL_VARIABLE_GUID;
const efi_guid_t efi_guid_device_path = EFI_DEVICE_PATH_PROTOCOL_GUID;
const efi_guid_t efi_guid_loaded_image = EFI_LOADED_IMAGE_PROTOCOL_GUID;
const efi_guid_t efi_guid_loaded_image_device_path =
		EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID;
const efi_guid_t efi_simple_file_system_protocol_guid =
		EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
const efi_guid_t efi_file_info_guid = EFI_FILE_INFO_GUID;

static int machines[] = {
#if defined(__aarch64__)
	IMAGE_FILE_MACHINE_ARM64,
#elif defined(__arm__)
	IMAGE_FILE_MACHINE_ARM,
	IMAGE_FILE_MACHINE_THUMB,
	IMAGE_FILE_MACHINE_ARMNT,
#endif

#if defined(__x86_64__)
	IMAGE_FILE_MACHINE_AMD64,
#elif defined(__i386__)
	IMAGE_FILE_MACHINE_I386,
#endif

#if defined(__riscv) && (__riscv_xlen == 32)
	IMAGE_FILE_MACHINE_RISCV32,
#endif

#if defined(__riscv) && (__riscv_xlen == 64)
	IMAGE_FILE_MACHINE_RISCV64,
#endif
	0 };

/**
 * efi_print_image_info() - print information about a loaded image
 *
 * If the program counter is located within the image the offset to the base
 * address is shown.
 *
 * @obj:	EFI object
 * @image:	loaded image
 * @pc:		program counter (use NULL to suppress offset output)
 * Return:	status code
 */
static efi_status_t efi_print_image_info(struct efi_loaded_image_obj *obj,
					 struct efi_loaded_image *image,
					 void *pc)
{
	printf("UEFI image");
	printf(" [0x%p:0x%p]",
	       image->image_base, image->image_base + image->image_size - 1);
	if (pc && pc >= image->image_base &&
	    pc < image->image_base + image->image_size)
		printf(" pc=0x%zx", pc - image->image_base);
	if (image->file_path)
		printf(" '%pD'", image->file_path);
	printf("\n");
	return EFI_SUCCESS;
}

/**
 * efi_print_image_infos() - print information about all loaded images
 *
 * @pc:		program counter (use NULL to suppress offset output)
 */
void efi_print_image_infos(void *pc)
{
	struct efi_object *efiobj;
	struct efi_handler *handler;

	list_for_each_entry(efiobj, &efi_obj_list, link) {
		list_for_each_entry(handler, &efiobj->protocols, link) {
			if (!guidcmp(handler->guid, &efi_guid_loaded_image)) {
				efi_print_image_info(
					(struct efi_loaded_image_obj *)efiobj,
					handler->protocol_interface, pc);
			}
		}
	}
}

/**
 * efi_loader_relocate() - relocate UEFI binary
 *
 * @rel:		pointer to the relocation table
 * @rel_size:		size of the relocation table in bytes
 * @efi_reloc:		actual load address of the image
 * @pref_address:	preferred load address of the image
 * Return:		status code
 */
static efi_status_t efi_loader_relocate(const IMAGE_BASE_RELOCATION *rel,
			unsigned long rel_size, void *efi_reloc,
			unsigned long pref_address)
{
	unsigned long delta = (unsigned long)efi_reloc - pref_address;
	const IMAGE_BASE_RELOCATION *end;
	int i;

	if (delta == 0)
		return EFI_SUCCESS;

	end = (const IMAGE_BASE_RELOCATION *)((const char *)rel + rel_size);
	while (rel < end && rel->SizeOfBlock) {
		const uint16_t *relocs = (const uint16_t *)(rel + 1);
		i = (rel->SizeOfBlock - sizeof(*rel)) / sizeof(uint16_t);
		while (i--) {
			uint32_t offset = (uint32_t)(*relocs & 0xfff) +
					  rel->VirtualAddress;
			int type = *relocs >> EFI_PAGE_SHIFT;
			uint64_t *x64 = efi_reloc + offset;
			uint32_t *x32 = efi_reloc + offset;
			uint16_t *x16 = efi_reloc + offset;

			switch (type) {
			case IMAGE_REL_BASED_ABSOLUTE:
				break;
			case IMAGE_REL_BASED_HIGH:
				*x16 += ((uint32_t)delta) >> 16;
				break;
			case IMAGE_REL_BASED_LOW:
				*x16 += (uint16_t)delta;
				break;
			case IMAGE_REL_BASED_HIGHLOW:
				*x32 += (uint32_t)delta;
				break;
			case IMAGE_REL_BASED_DIR64:
				*x64 += (uint64_t)delta;
				break;
#ifdef __riscv
			case IMAGE_REL_BASED_RISCV_HI20:
				*x32 = ((*x32 & 0xfffff000) + (uint32_t)delta) |
					(*x32 & 0x00000fff);
				break;
			case IMAGE_REL_BASED_RISCV_LOW12I:
			case IMAGE_REL_BASED_RISCV_LOW12S:
				/* We know that we're 4k aligned */
				if (delta & 0xfff) {
					printf("Unsupported reloc offset\n");
					return EFI_LOAD_ERROR;
				}
				break;
#endif
			default:
				printf("Unknown Relocation off %x type %x\n",
				       offset, type);
				return EFI_LOAD_ERROR;
			}
			relocs++;
		}
		rel = (const IMAGE_BASE_RELOCATION *)relocs;
	}
	return EFI_SUCCESS;
}

void __weak invalidate_icache_all(void)
{
	/* If the system doesn't support icache_all flush, cross our fingers */
}

/**
 * efi_set_code_and_data_type() - determine the memory types to be used for code
 *				  and data.
 *
 * @loaded_image_info:	image descriptor
 * @image_type:		field Subsystem of the optional header for
 *			Windows specific field
 */
static void efi_set_code_and_data_type(
			struct efi_loaded_image *loaded_image_info,
			uint16_t image_type)
{
	switch (image_type) {
	case IMAGE_SUBSYSTEM_EFI_APPLICATION:
		loaded_image_info->image_code_type = EFI_LOADER_CODE;
		loaded_image_info->image_data_type = EFI_LOADER_DATA;
		break;
	case IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER:
		loaded_image_info->image_code_type = EFI_BOOT_SERVICES_CODE;
		loaded_image_info->image_data_type = EFI_BOOT_SERVICES_DATA;
		break;
	case IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER:
	case IMAGE_SUBSYSTEM_EFI_ROM:
		loaded_image_info->image_code_type = EFI_RUNTIME_SERVICES_CODE;
		loaded_image_info->image_data_type = EFI_RUNTIME_SERVICES_DATA;
		break;
	default:
		printf("%s: invalid image type: %u\n", __func__, image_type);
		/* Let's assume it is an application */
		loaded_image_info->image_code_type = EFI_LOADER_CODE;
		loaded_image_info->image_data_type = EFI_LOADER_DATA;
		break;
	}
}

#ifdef CONFIG_EFI_SECURE_BOOT
/**
 * cmp_pe_section() - compare virtual addresses of two PE image sections
 * @arg1:	pointer to pointer to first section header
 * @arg2:	pointer to pointer to second section header
 *
 * Compare the virtual addresses of two sections of an portable executable.
 * The arguments are defined as const void * to allow usage with qsort().
 *
 * Return:	-1 if the virtual address of arg1 is less than that of arg2,
 *		0 if the virtual addresses are equal, 1 if the virtual address
 *		of arg1 is greater than that of arg2.
 */
static int cmp_pe_section(const void *arg1, const void *arg2)
{
	const IMAGE_SECTION_HEADER *section1, *section2;

	section1 = *((const IMAGE_SECTION_HEADER **)arg1);
	section2 = *((const IMAGE_SECTION_HEADER **)arg2);

	if (section1->VirtualAddress < section2->VirtualAddress)
		return -1;
	else if (section1->VirtualAddress == section2->VirtualAddress)
		return 0;
	else
		return 1;
}

/**
 * efi_image_parse() - parse a PE image
 * @efi:	Pointer to image
 * @len:	Size of @efi
 * @regp:	Pointer to a list of regions
 * @auth:	Pointer to a pointer to authentication data in PE
 * @auth_len:	Size of @auth
 *
 * Parse image binary in PE32(+) format, assuming that sanity of PE image
 * has been checked by a caller.
 * On success, an address of authentication data in @efi and its size will
 * be returned in @auth and @auth_len, respectively.
 *
 * Return:	true on success, false on error
 */
bool efi_image_parse(void *efi, size_t len, struct efi_image_regions **regp,
		     WIN_CERTIFICATE **auth, size_t *auth_len)
{
	struct efi_image_regions *regs;
	IMAGE_DOS_HEADER *dos;
	IMAGE_NT_HEADERS32 *nt;
	IMAGE_SECTION_HEADER *sections, **sorted;
	int num_regions, num_sections, i;
	int ctidx = IMAGE_DIRECTORY_ENTRY_SECURITY;
	u32 align, size, authsz, authoff;
	size_t bytes_hashed;

	dos = (void *)efi;
	nt = (void *)(efi + dos->e_lfanew);

	/*
	 * Count maximum number of regions to be digested.
	 * We don't have to have an exact number here.
	 * See efi_image_region_add()'s in parsing below.
	 */
	num_regions = 3; /* for header */
	num_regions += nt->FileHeader.NumberOfSections;
	num_regions++; /* for extra */

	regs = calloc(sizeof(*regs) + sizeof(struct image_region) * num_regions,
		      1);
	if (!regs)
		goto err;
	regs->max = num_regions;

	/*
	 * Collect data regions for hash calculation
	 * 1. File headers
	 */
	if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		IMAGE_NT_HEADERS64 *nt64 = (void *)nt;
		IMAGE_OPTIONAL_HEADER64 *opt = &nt64->OptionalHeader;

		/* Skip CheckSum */
		efi_image_region_add(regs, efi, &opt->CheckSum, 0);
		if (nt64->OptionalHeader.NumberOfRvaAndSizes <= ctidx) {
			efi_image_region_add(regs,
					     &opt->Subsystem,
					     efi + opt->SizeOfHeaders, 0);
		} else {
			/* Skip Certificates Table */
			efi_image_region_add(regs,
					     &opt->Subsystem,
					     &opt->DataDirectory[ctidx], 0);
			efi_image_region_add(regs,
					     &opt->DataDirectory[ctidx] + 1,
					     efi + opt->SizeOfHeaders, 0);
		}

		bytes_hashed = opt->SizeOfHeaders;
		align = opt->FileAlignment;
		authoff = opt->DataDirectory[ctidx].VirtualAddress;
		authsz = opt->DataDirectory[ctidx].Size;
	} else if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
		IMAGE_OPTIONAL_HEADER32 *opt = &nt->OptionalHeader;

		efi_image_region_add(regs, efi, &opt->CheckSum, 0);
		efi_image_region_add(regs, &opt->Subsystem,
				     &opt->DataDirectory[ctidx], 0);
		efi_image_region_add(regs, &opt->DataDirectory[ctidx] + 1,
				     efi + opt->SizeOfHeaders, 0);

		bytes_hashed = opt->SizeOfHeaders;
		align = opt->FileAlignment;
		authoff = opt->DataDirectory[ctidx].VirtualAddress;
		authsz = opt->DataDirectory[ctidx].Size;
	} else {
		debug("%s: Invalid optional header magic %x\n", __func__,
		      nt->OptionalHeader.Magic);
		goto err;
	}

	/* 2. Sections */
	num_sections = nt->FileHeader.NumberOfSections;
	sections = (void *)((uint8_t *)&nt->OptionalHeader +
			    nt->FileHeader.SizeOfOptionalHeader);
	sorted = calloc(sizeof(IMAGE_SECTION_HEADER *), num_sections);
	if (!sorted) {
		debug("%s: Out of memory\n", __func__);
		goto err;
	}

	/*
	 * Make sure the section list is in ascending order.
	 */
	for (i = 0; i < num_sections; i++)
		sorted[i] = &sections[i];
	qsort(sorted, num_sections, sizeof(sorted[0]), cmp_pe_section);

	for (i = 0; i < num_sections; i++) {
		if (!sorted[i]->SizeOfRawData)
			continue;

		size = (sorted[i]->SizeOfRawData + align - 1) & ~(align - 1);
		efi_image_region_add(regs, efi + sorted[i]->PointerToRawData,
				     efi + sorted[i]->PointerToRawData + size,
				     0);
		debug("section[%d](%s): raw: 0x%x-0x%x, virt: %x-%x\n",
		      i, sorted[i]->Name,
		      sorted[i]->PointerToRawData,
		      sorted[i]->PointerToRawData + size,
		      sorted[i]->VirtualAddress,
		      sorted[i]->VirtualAddress
			+ sorted[i]->Misc.VirtualSize);

		bytes_hashed += size;
	}
	free(sorted);

	/* 3. Extra data excluding Certificates Table */
	if (bytes_hashed + authsz < len) {
		debug("extra data for hash: %zu\n",
		      len - (bytes_hashed + authsz));
		efi_image_region_add(regs, efi + bytes_hashed,
				     efi + len - authsz, 0);
	}

	/* Return Certificates Table */
	if (authsz) {
		if (len < authoff + authsz) {
			debug("%s: Size for auth too large: %u >= %zu\n",
			      __func__, authsz, len - authoff);
			goto err;
		}
		if (authsz < sizeof(*auth)) {
			debug("%s: Size for auth too small: %u < %zu\n",
			      __func__, authsz, sizeof(*auth));
			goto err;
		}
		*auth = efi + authoff;
		*auth_len = authsz;
		debug("WIN_CERTIFICATE: 0x%x, size: 0x%x\n", authoff, authsz);
	} else {
		*auth = NULL;
		*auth_len = 0;
	}

	*regp = regs;

	return true;

err:
	free(regs);

	return false;
}

/**
 * efi_image_unsigned_authenticate() - authenticate unsigned image with
 * SHA256 hash
 * @regs:	List of regions to be verified
 *
 * If an image is not signed, it doesn't have a signature. In this case,
 * its message digest is calculated and it will be compared with one of
 * hash values stored in signature databases.
 *
 * Return:	true if authenticated, false if not
 */
static bool efi_image_unsigned_authenticate(struct efi_image_regions *regs)
{
	struct efi_signature_store *db = NULL, *dbx = NULL;
	bool ret = false;

	dbx = efi_sigstore_parse_sigdb(L"dbx");
	if (!dbx) {
		debug("Getting signature database(dbx) failed\n");
		goto out;
	}

	db = efi_sigstore_parse_sigdb(L"db");
	if (!db) {
		debug("Getting signature database(db) failed\n");
		goto out;
	}

	/* try black-list first */
	if (efi_signature_verify_with_sigdb(regs, NULL, dbx, NULL)) {
		debug("Image is not signed and rejected by \"dbx\"\n");
		goto out;
	}

	/* try white-list */
	if (efi_signature_verify_with_sigdb(regs, NULL, db, NULL))
		ret = true;
	else
		debug("Image is not signed and not found in \"db\" or \"dbx\"\n");

out:
	efi_sigstore_free(db);
	efi_sigstore_free(dbx);

	return ret;
}

/**
 * efi_image_authenticate() - verify a signature of signed image
 * @efi:	Pointer to image
 * @efi_size:	Size of @efi
 *
 * A signed image should have its signature stored in a table of its PE header.
 * So if an image is signed and only if if its signature is verified using
 * signature databases, an image is authenticated.
 * If an image is not signed, its validity is checked by using
 * efi_image_unsigned_authenticated().
 * TODO:
 * When AuditMode==0, if the image's signature is not found in
 * the authorized database, or is found in the forbidden database,
 * the image will not be started and instead, information about it
 * will be placed in this table.
 * When AuditMode==1, an EFI_IMAGE_EXECUTION_INFO element is created
 * in the EFI_IMAGE_EXECUTION_INFO_TABLE for every certificate found
 * in the certificate table of every image that is validated.
 *
 * Return:	true if authenticated, false if not
 */
static bool efi_image_authenticate(void *efi, size_t efi_size)
{
	struct efi_image_regions *regs = NULL;
	WIN_CERTIFICATE *wincerts = NULL, *wincert;
	size_t wincerts_len;
	struct pkcs7_message *msg = NULL;
	struct efi_signature_store *db = NULL, *dbx = NULL;
	struct x509_certificate *cert = NULL;
	void *new_efi = NULL;
	size_t new_efi_size;
	bool ret = false;

	if (!efi_secure_boot_enabled())
		return true;

	/*
	 * Size must be 8-byte aligned and the trailing bytes must be
	 * zero'ed. Otherwise hash value may be incorrect.
	 */
	if (efi_size & 0x7) {
		new_efi_size = (efi_size + 0x7) & ~0x7ULL;
		new_efi = calloc(new_efi_size, 1);
		if (!new_efi)
			return false;
		memcpy(new_efi, efi, efi_size);
		efi = new_efi;
		efi_size = new_efi_size;
	}

	if (!efi_image_parse(efi, efi_size, &regs, &wincerts,
			     &wincerts_len)) {
		debug("Parsing PE executable image failed\n");
		goto err;
	}

	if (!wincerts) {
		/* The image is not signed */
		ret = efi_image_unsigned_authenticate(regs);

		goto err;
	}

	/*
	 * verify signature using db and dbx
	 */
	db = efi_sigstore_parse_sigdb(L"db");
	if (!db) {
		debug("Getting signature database(db) failed\n");
		goto err;
	}

	dbx = efi_sigstore_parse_sigdb(L"dbx");
	if (!dbx) {
		debug("Getting signature database(dbx) failed\n");
		goto err;
	}

	/* go through WIN_CERTIFICATE list */
	for (wincert = wincerts;
	     (void *)wincert < (void *)wincerts + wincerts_len;
	     wincert = (void *)wincert + ALIGN(wincert->dwLength, 8)) {
		if (wincert->dwLength < sizeof(*wincert)) {
			debug("%s: dwLength too small: %u < %zu\n",
			      __func__, wincert->dwLength, sizeof(*wincert));
			goto err;
		}
		msg = pkcs7_parse_message((void *)wincert + sizeof(*wincert),
					  wincert->dwLength - sizeof(*wincert));
		if (IS_ERR(msg)) {
			debug("Parsing image's signature failed\n");
			msg = NULL;
			goto err;
		}

		/* try black-list first */
		if (efi_signature_verify_with_sigdb(regs, msg, dbx, NULL)) {
			debug("Signature was rejected by \"dbx\"\n");
			goto err;
		}

		if (!efi_signature_verify_signers(msg, dbx)) {
			debug("Signer was rejected by \"dbx\"\n");
			goto err;
		} else {
			ret = true;
		}

		/* try white-list */
		if (!efi_signature_verify_with_sigdb(regs, msg, db, &cert)) {
			debug("Verifying signature with \"db\" failed\n");
			goto err;
		} else {
			ret = true;
		}

		if (!efi_signature_verify_cert(cert, dbx)) {
			debug("Certificate was rejected by \"dbx\"\n");
			goto err;
		} else {
			ret = true;
		}
	}

err:
	x509_free_certificate(cert);
	efi_sigstore_free(db);
	efi_sigstore_free(dbx);
	pkcs7_free_message(msg);
	free(regs);
	free(new_efi);

	return ret;
}
#else
static bool efi_image_authenticate(void *efi, size_t efi_size)
{
	return true;
}
#endif /* CONFIG_EFI_SECURE_BOOT */

/**
 * efi_load_pe() - relocate EFI binary
 *
 * This function loads all sections from a PE binary into a newly reserved
 * piece of memory. On success the entry point is returned as handle->entry.
 *
 * @handle:		loaded image handle
 * @efi:		pointer to the EFI binary
 * @efi_size:		size of @efi binary
 * @loaded_image_info:	loaded image protocol
 * Return:		status code
 */
efi_status_t efi_load_pe(struct efi_loaded_image_obj *handle,
			 void *efi, size_t efi_size,
			 struct efi_loaded_image *loaded_image_info)
{
	IMAGE_NT_HEADERS32 *nt;
	IMAGE_DOS_HEADER *dos;
	IMAGE_SECTION_HEADER *sections;
	int num_sections;
	void *efi_reloc;
	int i;
	const IMAGE_BASE_RELOCATION *rel;
	unsigned long rel_size;
	int rel_idx = IMAGE_DIRECTORY_ENTRY_BASERELOC;
	uint64_t image_base;
	unsigned long virt_size = 0;
	int supported = 0;
	efi_status_t ret;

	/* Sanity check for a file header */
	if (efi_size < sizeof(*dos)) {
		printf("%s: Truncated DOS Header\n", __func__);
		ret = EFI_LOAD_ERROR;
		goto err;
	}

	dos = efi;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		printf("%s: Invalid DOS Signature\n", __func__);
		ret = EFI_LOAD_ERROR;
		goto err;
	}

	/*
	 * Check if the image section header fits into the file. Knowing that at
	 * least one section header follows we only need to check for the length
	 * of the 64bit header which is longer than the 32bit header.
	 */
	if (efi_size < dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64)) {
		printf("%s: Invalid offset for Extended Header\n", __func__);
		ret = EFI_LOAD_ERROR;
		goto err;
	}

	nt = (void *) ((char *)efi + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		printf("%s: Invalid NT Signature\n", __func__);
		ret = EFI_LOAD_ERROR;
		goto err;
	}

	for (i = 0; machines[i]; i++)
		if (machines[i] == nt->FileHeader.Machine) {
			supported = 1;
			break;
		}

	if (!supported) {
		printf("%s: Machine type 0x%04x is not supported\n",
		       __func__, nt->FileHeader.Machine);
		ret = EFI_LOAD_ERROR;
		goto err;
	}

	num_sections = nt->FileHeader.NumberOfSections;
	sections = (void *)&nt->OptionalHeader +
			    nt->FileHeader.SizeOfOptionalHeader;

	if (efi_size < ((void *)sections + sizeof(sections[0]) * num_sections
			- efi)) {
		printf("%s: Invalid number of sections: %d\n",
		       __func__, num_sections);
		ret = EFI_LOAD_ERROR;
		goto err;
	}

	/* Authenticate an image */
	if (efi_image_authenticate(efi, efi_size))
		handle->auth_status = EFI_IMAGE_AUTH_PASSED;
	else
		handle->auth_status = EFI_IMAGE_AUTH_FAILED;

	/* Calculate upper virtual address boundary */
	for (i = num_sections - 1; i >= 0; i--) {
		IMAGE_SECTION_HEADER *sec = &sections[i];
		virt_size = max_t(unsigned long, virt_size,
				  sec->VirtualAddress + sec->Misc.VirtualSize);
	}

	/* Read 32/64bit specific header bits */
	if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		IMAGE_NT_HEADERS64 *nt64 = (void *)nt;
		IMAGE_OPTIONAL_HEADER64 *opt = &nt64->OptionalHeader;
		image_base = opt->ImageBase;
		efi_set_code_and_data_type(loaded_image_info, opt->Subsystem);
		handle->image_type = opt->Subsystem;
		efi_reloc = efi_alloc(virt_size,
				      loaded_image_info->image_code_type);
		if (!efi_reloc) {
			printf("%s: Could not allocate %lu bytes\n",
			       __func__, virt_size);
			ret = EFI_OUT_OF_RESOURCES;
			goto err;
		}
		handle->entry = efi_reloc + opt->AddressOfEntryPoint;
		rel_size = opt->DataDirectory[rel_idx].Size;
		rel = efi_reloc + opt->DataDirectory[rel_idx].VirtualAddress;
		virt_size = ALIGN(virt_size, opt->SectionAlignment);
	} else if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
		IMAGE_OPTIONAL_HEADER32 *opt = &nt->OptionalHeader;
		image_base = opt->ImageBase;
		efi_set_code_and_data_type(loaded_image_info, opt->Subsystem);
		handle->image_type = opt->Subsystem;
		efi_reloc = efi_alloc(virt_size,
				      loaded_image_info->image_code_type);
		if (!efi_reloc) {
			printf("%s: Could not allocate %lu bytes\n",
			       __func__, virt_size);
			ret = EFI_OUT_OF_RESOURCES;
			goto err;
		}
		handle->entry = efi_reloc + opt->AddressOfEntryPoint;
		rel_size = opt->DataDirectory[rel_idx].Size;
		rel = efi_reloc + opt->DataDirectory[rel_idx].VirtualAddress;
		virt_size = ALIGN(virt_size, opt->SectionAlignment);
	} else {
		printf("%s: Invalid optional header magic %x\n", __func__,
		       nt->OptionalHeader.Magic);
		ret = EFI_LOAD_ERROR;
		goto err;
	}

	/* Copy PE headers */
	memcpy(efi_reloc, efi,
	       sizeof(*dos)
		 + sizeof(*nt)
		 + nt->FileHeader.SizeOfOptionalHeader
		 + num_sections * sizeof(IMAGE_SECTION_HEADER));

	/* Load sections into RAM */
	for (i = num_sections - 1; i >= 0; i--) {
		IMAGE_SECTION_HEADER *sec = &sections[i];
		memset(efi_reloc + sec->VirtualAddress, 0,
		       sec->Misc.VirtualSize);
		memcpy(efi_reloc + sec->VirtualAddress,
		       efi + sec->PointerToRawData,
		       sec->SizeOfRawData);
	}

	/* Run through relocations */
	if (efi_loader_relocate(rel, rel_size, efi_reloc,
				(unsigned long)image_base) != EFI_SUCCESS) {
		efi_free_pages((uintptr_t) efi_reloc,
			       (virt_size + EFI_PAGE_MASK) >> EFI_PAGE_SHIFT);
		ret = EFI_LOAD_ERROR;
		goto err;
	}

	/* Flush cache */
	flush_cache((ulong)efi_reloc,
		    ALIGN(virt_size, EFI_CACHELINE_SIZE));
	invalidate_icache_all();

	/* Populate the loaded image interface bits */
	loaded_image_info->image_base = efi_reloc;
	loaded_image_info->image_size = virt_size;

	if (handle->auth_status == EFI_IMAGE_AUTH_PASSED)
		return EFI_SUCCESS;
	else
		return EFI_SECURITY_VIOLATION;

err:
	return ret;
}
