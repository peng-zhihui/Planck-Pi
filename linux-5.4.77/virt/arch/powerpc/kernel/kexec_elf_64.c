// SPDX-License-Identifier: GPL-2.0-only
/*
 * Load ELF vmlinux file for the kexec_file_load syscall.
 *
 * Copyright (C) 2004  Adam Litke (agl@us.ibm.com)
 * Copyright (C) 2004  IBM Corp.
 * Copyright (C) 2005  R Sharada (sharada@in.ibm.com)
 * Copyright (C) 2006  Mohan Kumar M (mohan@in.ibm.com)
 * Copyright (C) 2016  IBM Corporation
 *
 * Based on kexec-tools' kexec-elf-exec.c and kexec-elf-ppc64.c.
 * Heavily modified for the kernel by
 * Thiago Jung Bauermann <bauerman@linux.vnet.ibm.com>.
 */

#define pr_fmt(fmt)	"kexec_elf: " fmt

#include <linux/elf.h>
#include <linux/kexec.h>
#include <linux/libfdt.h>
#include <linux/module.h>
#include <linux/of_fdt.h>
#include <linux/slab.h>
#include <linux/types.h>

static void *elf64_load(struct kimage *image, char *kernel_buf,
			unsigned long kernel_len, char *initrd,
			unsigned long initrd_len, char *cmdline,
			unsigned long cmdline_len)
{
	int ret;
	unsigned int fdt_size;
	unsigned long kernel_load_addr;
	unsigned long initrd_load_addr = 0, fdt_load_addr;
	void *fdt;
	const void *slave_code;
	struct elfhdr ehdr;
	struct kexec_elf_info elf_info;
	struct kexec_buf kbuf = { .image = image, .buf_min = 0,
				  .buf_max = ppc64_rma_size };
	struct kexec_buf pbuf = { .image = image, .buf_min = 0,
				  .buf_max = ppc64_rma_size, .top_down = true,
				  .mem = KEXEC_BUF_MEM_UNKNOWN };

	ret = kexec_build_elf_info(kernel_buf, kernel_len, &ehdr, &elf_info);
	if (ret)
		goto out;

	ret = kexec_elf_load(image, &ehdr, &elf_info, &kbuf, &kernel_load_addr);
	if (ret)
		goto out;

	pr_debug("Loaded the kernel at 0x%lx\n", kernel_load_addr);

	ret = kexec_load_purgatory(image, &pbuf);
	if (ret) {
		pr_err("Loading purgatory failed.\n");
		goto out;
	}

	pr_debug("Loaded purgatory at 0x%lx\n", pbuf.mem);

	if (initrd != NULL) {
		kbuf.buffer = initrd;
		kbuf.bufsz = kbuf.memsz = initrd_len;
		kbuf.buf_align = PAGE_SIZE;
		kbuf.top_down = false;
		kbuf.mem = KEXEC_BUF_MEM_UNKNOWN;
		ret = kexec_add_buffer(&kbuf);
		if (ret)
			goto out;
		initrd_load_addr = kbuf.mem;

		pr_debug("Loaded initrd at 0x%lx\n", initrd_load_addr);
	}

	fdt_size = fdt_totalsize(initial_boot_params) * 2;
	fdt = kmalloc(fdt_size, GFP_KERNEL);
	if (!fdt) {
		pr_err("Not enough memory for the device tree.\n");
		ret = -ENOMEM;
		goto out;
	}
	ret = fdt_open_into(initial_boot_params, fdt, fdt_size);
	if (ret < 0) {
		pr_err("Error setting up the new device tree.\n");
		ret = -EINVAL;
		goto out;
	}

	ret = setup_new_fdt(image, fdt, initrd_load_addr, initrd_len, cmdline);
	if (ret)
		goto out;

	fdt_pack(fdt);

	kbuf.buffer = fdt;
	kbuf.bufsz = kbuf.memsz = fdt_size;
	kbuf.buf_align = PAGE_SIZE;
	kbuf.top_down = true;
	kbuf.mem = KEXEC_BUF_MEM_UNKNOWN;
	ret = kexec_add_buffer(&kbuf);
	if (ret)
		goto out;
	fdt_load_addr = kbuf.mem;

	pr_debug("Loaded device tree at 0x%lx\n", fdt_load_addr);

	slave_code = elf_info.buffer + elf_info.proghdrs[0].p_offset;
	ret = setup_purgatory(image, slave_code, fdt, kernel_load_addr,
			      fdt_load_addr);
	if (ret)
		pr_err("Error setting up the purgatory.\n");

out:
	kexec_free_elf_info(&elf_info);

	/* Make kimage_file_post_load_cleanup free the fdt buffer for us. */
	return ret ? ERR_PTR(ret) : fdt;
}

const struct kexec_file_ops kexec_elf64_ops = {
	.probe = kexec_elf_probe,
	.load = elf64_load,
};
