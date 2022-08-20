/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2017 Facebook
 */
#ifndef __MAP_IN_MAP_H__
#define __MAP_IN_MAP_H__

#include <linux/types.h>

struct file;
struct bpf_map;

struct bpf_map *bpf_map_meta_alloc(int inner_map_ufd);
void bpf_map_meta_free(struct bpf_map *map_meta);
bool bpf_map_meta_equal(const struct bpf_map *meta0,
			const struct bpf_map *meta1);
void *bpf_map_fd_get_ptr(struct bpf_map *map, struct file *map_file,
			 int ufd);
void bpf_map_fd_put_ptr(void *ptr);
u32 bpf_map_fd_sys_lookup_elem(void *ptr);

#endif
