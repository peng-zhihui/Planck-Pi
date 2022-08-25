/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */

/*
 * Internal libbpf helpers.
 *
 * Copyright (c) 2019 Facebook
 */

#ifndef __LIBBPF_LIBBPF_INTERNAL_H
#define __LIBBPF_LIBBPF_INTERNAL_H

#include "libbpf.h"

#define BTF_INFO_ENC(kind, kind_flag, vlen) \
	((!!(kind_flag) << 31) | ((kind) << 24) | ((vlen) & BTF_MAX_VLEN))
#define BTF_TYPE_ENC(name, info, size_or_type) (name), (info), (size_or_type)
#define BTF_INT_ENC(encoding, bits_offset, nr_bits) \
	((encoding) << 24 | (bits_offset) << 16 | (nr_bits))
#define BTF_TYPE_INT_ENC(name, encoding, bits_offset, bits, sz) \
	BTF_TYPE_ENC(name, BTF_INFO_ENC(BTF_KIND_INT, 0, 0), sz), \
	BTF_INT_ENC(encoding, bits_offset, bits)
#define BTF_MEMBER_ENC(name, type, bits_offset) (name), (type), (bits_offset)
#define BTF_PARAM_ENC(name, type) (name), (type)
#define BTF_VAR_SECINFO_ENC(type, offset, size) (type), (offset), (size)

#ifndef min
# define min(x, y) ((x) < (y) ? (x) : (y))
#endif
#ifndef max
# define max(x, y) ((x) < (y) ? (y) : (x))
#endif
#ifndef offsetofend
# define offsetofend(TYPE, FIELD) \
	(offsetof(TYPE, FIELD) + sizeof(((TYPE *)0)->FIELD))
#endif

/* Symbol versioning is different between static and shared library.
 * Properly versioned symbols are needed for shared library, but
 * only the symbol of the new version is needed for static library.
 */
#ifdef SHARED
# define COMPAT_VERSION(internal_name, api_name, version) \
	asm(".symver " #internal_name "," #api_name "@" #version);
# define DEFAULT_VERSION(internal_name, api_name, version) \
	asm(".symver " #internal_name "," #api_name "@@" #version);
#else
# define COMPAT_VERSION(internal_name, api_name, version)
# define DEFAULT_VERSION(internal_name, api_name, version) \
	extern typeof(internal_name) api_name \
	__attribute__((alias(#internal_name)));
#endif

extern void libbpf_print(enum libbpf_print_level level,
			 const char *format, ...)
	__attribute__((format(printf, 2, 3)));

#define __pr(level, fmt, ...)	\
do {				\
	libbpf_print(level, "libbpf: " fmt, ##__VA_ARGS__);	\
} while (0)

#define pr_warning(fmt, ...)	__pr(LIBBPF_WARN, fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)	__pr(LIBBPF_INFO, fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)	__pr(LIBBPF_DEBUG, fmt, ##__VA_ARGS__)

int parse_cpu_mask_str(const char *s, bool **mask, int *mask_sz);
int parse_cpu_mask_file(const char *fcpu, bool **mask, int *mask_sz);
int libbpf__load_raw_btf(const char *raw_types, size_t types_len,
			 const char *str_sec, size_t str_len);

struct btf_ext_info {
	/*
	 * info points to the individual info section (e.g. func_info and
	 * line_info) from the .BTF.ext. It does not include the __u32 rec_size.
	 */
	void *info;
	__u32 rec_size;
	__u32 len;
};

#define for_each_btf_ext_sec(seg, sec)					\
	for (sec = (seg)->info;						\
	     (void *)sec < (seg)->info + (seg)->len;			\
	     sec = (void *)sec + sizeof(struct btf_ext_info_sec) +	\
		   (seg)->rec_size * sec->num_info)

#define for_each_btf_ext_rec(seg, sec, i, rec)				\
	for (i = 0, rec = (void *)&(sec)->data;				\
	     i < (sec)->num_info;					\
	     i++, rec = (void *)rec + (seg)->rec_size)

struct btf_ext {
	union {
		struct btf_ext_header *hdr;
		void *data;
	};
	struct btf_ext_info func_info;
	struct btf_ext_info line_info;
	struct btf_ext_info offset_reloc_info;
	__u32 data_size;
};

struct btf_ext_info_sec {
	__u32	sec_name_off;
	__u32	num_info;
	/* Followed by num_info * record_size number of bytes */
	__u8	data[0];
};

/* The minimum bpf_func_info checked by the loader */
struct bpf_func_info_min {
	__u32   insn_off;
	__u32   type_id;
};

/* The minimum bpf_line_info checked by the loader */
struct bpf_line_info_min {
	__u32	insn_off;
	__u32	file_name_off;
	__u32	line_off;
	__u32	line_col;
};

/* The minimum bpf_offset_reloc checked by the loader
 *
 * Offset relocation captures the following data:
 * - insn_off - instruction offset (in bytes) within a BPF program that needs
 *   its insn->imm field to be relocated with actual offset;
 * - type_id - BTF type ID of the "root" (containing) entity of a relocatable
 *   offset;
 * - access_str_off - offset into corresponding .BTF string section. String
 *   itself encodes an accessed field using a sequence of field and array
 *   indicies, separated by colon (:). It's conceptually very close to LLVM's
 *   getelementptr ([0]) instruction's arguments for identifying offset to 
 *   a field.
 *
 * Example to provide a better feel.
 *
 *   struct sample {
 *       int a;
 *       struct {
 *           int b[10];
 *       };
 *   };
 *
 *   struct sample *s = ...;
 *   int x = &s->a;     // encoded as "0:0" (a is field #0)
 *   int y = &s->b[5];  // encoded as "0:1:0:5" (anon struct is field #1, 
 *                      // b is field #0 inside anon struct, accessing elem #5)
 *   int z = &s[10]->b; // encoded as "10:1" (ptr is used as an array)
 *
 * type_id for all relocs in this example  will capture BTF type id of
 * `struct sample`.
 *
 * Such relocation is emitted when using __builtin_preserve_access_index()
 * Clang built-in, passing expression that captures field address, e.g.:
 *
 * bpf_probe_read(&dst, sizeof(dst),
 *		  __builtin_preserve_access_index(&src->a.b.c));
 *
 * In this case Clang will emit offset relocation recording necessary data to
 * be able to find offset of embedded `a.b.c` field within `src` struct.
 *
 *   [0] https://llvm.org/docs/LangRef.html#getelementptr-instruction
 */
struct bpf_offset_reloc {
	__u32   insn_off;
	__u32   type_id;
	__u32   access_str_off;
};

#endif /* __LIBBPF_LIBBPF_INTERNAL_H */
