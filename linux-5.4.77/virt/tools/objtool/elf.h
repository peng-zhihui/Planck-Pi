/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2015 Josh Poimboeuf <jpoimboe@redhat.com>
 */

#ifndef _OBJTOOL_ELF_H
#define _OBJTOOL_ELF_H

#include <stdio.h>
#include <gelf.h>
#include <linux/list.h>
#include <linux/hashtable.h>

#ifdef LIBELF_USE_DEPRECATED
# define elf_getshdrnum    elf_getshnum
# define elf_getshdrstrndx elf_getshstrndx
#endif

/*
 * Fallback for systems without this "read, mmaping if possible" cmd.
 */
#ifndef ELF_C_READ_MMAP
#define ELF_C_READ_MMAP ELF_C_READ
#endif

struct section {
	struct list_head list;
	GElf_Shdr sh;
	struct list_head symbol_list;
	DECLARE_HASHTABLE(symbol_hash, 8);
	struct list_head rela_list;
	DECLARE_HASHTABLE(rela_hash, 16);
	struct section *base, *rela;
	struct symbol *sym;
	Elf_Data *data;
	char *name;
	int idx;
	unsigned int len;
	bool changed, text, rodata;
};

struct symbol {
	struct list_head list;
	struct hlist_node hash;
	GElf_Sym sym;
	struct section *sec;
	char *name;
	unsigned int idx;
	unsigned char bind, type;
	unsigned long offset;
	unsigned int len;
	struct symbol *pfunc, *cfunc, *alias;
	bool uaccess_safe;
};

struct rela {
	struct list_head list;
	struct hlist_node hash;
	GElf_Rela rela;
	struct section *sec;
	struct symbol *sym;
	unsigned int type;
	unsigned long offset;
	int addend;
	bool jump_table_start;
};

struct elf {
	Elf *elf;
	GElf_Ehdr ehdr;
	int fd;
	char *name;
	struct list_head sections;
	DECLARE_HASHTABLE(rela_hash, 16);
};


struct elf *elf_read(const char *name, int flags);
struct section *find_section_by_name(struct elf *elf, const char *name);
struct symbol *find_symbol_by_offset(struct section *sec, unsigned long offset);
struct symbol *find_symbol_by_name(struct elf *elf, const char *name);
struct symbol *find_symbol_containing(struct section *sec, unsigned long offset);
struct rela *find_rela_by_dest(struct section *sec, unsigned long offset);
struct rela *find_rela_by_dest_range(struct section *sec, unsigned long offset,
				     unsigned int len);
struct symbol *find_containing_func(struct section *sec, unsigned long offset);
struct section *elf_create_section(struct elf *elf, const char *name, size_t
				   entsize, int nr);
struct section *elf_create_rela_section(struct elf *elf, struct section *base);
int elf_rebuild_rela_section(struct section *sec);
int elf_write(struct elf *elf);
void elf_close(struct elf *elf);

#define for_each_sec(file, sec)						\
	list_for_each_entry(sec, &file->elf->sections, list)

#endif /* _OBJTOOL_ELF_H */
