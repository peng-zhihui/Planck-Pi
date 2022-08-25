/*
 *	  Filename: ve_mem_list.h
 *	   Version: 0.01alpha
 * Description: Video engine driver memory list management.
 *	   License: GPLv2
 *
 *		Author	: yangcaoyuan <yangcaoyuan@allwinnertech.com>
 *		Date	: 2017/04/04
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program;
 *
 */
#ifndef _VE_MEM__LIST_H
#define _VE_MEM__LIST_H

#define ion_offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)

#define aw_container_of(aw_ptr, type, member) ({ \
const typeof(((type *)0)->member)*__mptr = (aw_ptr); \
(type *)((char *)__mptr - ion_offsetof(type, member)); })

static inline void aw_prefetch(const void *x) {(void)x; }
static inline void aw_prefetchw(const void *x) {(void)x; }

#define AW_LIST_LOCATION1  ((void *) 0x00100100)
#define AW_LIST_LOCATION2  ((void *) 0x00200200)

struct aw_mem_list_head {
struct aw_mem_list_head *aw_next, *aw_prev;
};

#define AW_MEM_LIST_HEAD_INIT(aw_name) { &(aw_name), &(aw_name) }

#define VE_LIST_HEAD(aw_name) \
struct aw_mem_list_head aw_name = AW_MEM_LIST_HEAD_INIT(aw_name)

#define AW_MEM_INIT_LIST_HEAD(aw_ptr) do { \
(aw_ptr)->aw_next = (aw_ptr); (aw_ptr)->aw_prev = (aw_ptr); \
} while (0)

/*
 * Insert a new entry between two known consecutive entries.
 *
 * This is only for internal list manipulation where we know
 * the aw_prev/aw_next entries already!
 */
static inline void __aw_list_add(struct aw_mem_list_head *newList,
	  struct aw_mem_list_head *aw_prev,
	  struct aw_mem_list_head *aw_next)
{
	aw_next->aw_prev = newList;
	newList->aw_next = aw_next;
	newList->aw_prev = aw_prev;
	aw_prev->aw_next = newList;
}

/**
 * list_add - add a new entry
 * @new: new entry to be added
 * @head: list head to add it after
 *
 * Insert a new entry after the specified head.
 * This is good for implementing stacks.
 */
static inline void aw_mem_list_add(struct aw_mem_list_head *newList,
										struct aw_mem_list_head *head)
{
	__aw_list_add(newList, head, head->aw_next);
}

/**
 * aw_mem_list_add_tail - add a new entry
 * @new: new entry to be added
 * @head: list head to add it before
 *
 * Insert a new entry before the specified head.
 * This is useful for implementing queues.
 */
static inline void aw_mem_list_add_tail(struct aw_mem_list_head *newList,
		 struct aw_mem_list_head *head)
{
	__aw_list_add(newList, head->aw_prev, head);
}

static inline void __aw_mem_list_del(struct aw_mem_list_head *aw_prev,
		struct aw_mem_list_head *aw_next)
{
	aw_next->aw_prev = aw_prev;
	aw_prev->aw_next = aw_next;
}

static inline void aw_mem_list_del(struct aw_mem_list_head *entry)
{
	__aw_mem_list_del(entry->aw_prev, entry->aw_next);
	entry->aw_next = AW_LIST_LOCATION1;
	entry->aw_prev = AW_LIST_LOCATION2;
}

#define aw_mem_list_entry(aw_ptr, type, member) aw_container_of(aw_ptr, type, member)

#define aw_mem_list_for_each_safe(aw_pos, aw_n, aw_head) \
for (aw_pos = (aw_head)->aw_next, aw_n = aw_pos->aw_next; aw_pos != (aw_head); \
aw_pos = aw_n, aw_n = aw_pos->aw_next)

#define aw_mem_list_for_each_entry(aw_pos, aw_head, member) \
for (aw_pos = aw_mem_list_entry((aw_head)->aw_next, typeof(*aw_pos), member); \
	aw_prefetch(aw_pos->member.aw_next), &aw_pos->member != (aw_head);  \
	aw_pos = aw_mem_list_entry(aw_pos->member.aw_next, typeof(*aw_pos), member))

#endif
