/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_CACHEFLUSH_H
#define __ASM_CACHEFLUSH_H

/* Keep includes the same across arches.  */
#include <linux/mm.h>

#define ARCH_IMPLEMENTS_FLUSH_DCACHE_PAGE 0

/*
 * The cache doesn't need to be flushed when TLB entries change when
 * the cache is mapped to physical memory, not virtual memory
 */
#ifndef flush_cache_all
static inline void flush_cache_all(void)
{
}
#endif

#ifndef flush_cache_mm
static inline void flush_cache_mm(struct mm_struct *mm)
{
}
#endif

#ifndef flush_cache_dup_mm
static inline void flush_cache_dup_mm(struct mm_struct *mm)
{
}
#endif

#ifndef flush_cache_range
static inline void flush_cache_range(struct vm_area_struct *vma,
				     unsigned long start,
				     unsigned long end)
{
}
#endif

#ifndef flush_cache_page
static inline void flush_cache_page(struct vm_area_struct *vma,
				    unsigned long vmaddr,
				    unsigned long pfn)
{
}
#endif

#ifndef flush_dcache_page
static inline void flush_dcache_page(struct page *page)
{
}
#endif

#ifndef flush_dcache_mmap_lock
static inline void flush_dcache_mmap_lock(struct address_space *mapping)
{
}
#endif

#ifndef flush_dcache_mmap_unlock
static inline void flush_dcache_mmap_unlock(struct address_space *mapping)
{
}
#endif

#ifndef flush_icache_range
static inline void flush_icache_range(unsigned long start, unsigned long end)
{
}
#endif

#ifndef flush_icache_page
static inline void flush_icache_page(struct vm_area_struct *vma,
				     struct page *page)
{
}
#endif

#ifndef flush_icache_user_range
static inline void flush_icache_user_range(struct vm_area_struct *vma,
					   struct page *page,
					   unsigned long addr, int len)
{
}
#endif

#ifndef flush_cache_vmap
static inline void flush_cache_vmap(unsigned long start, unsigned long end)
{
}
#endif

#ifndef flush_cache_vunmap
static inline void flush_cache_vunmap(unsigned long start, unsigned long end)
{
}
#endif

#ifndef copy_to_user_page
#define copy_to_user_page(vma, page, vaddr, dst, src, len)	\
	do { \
		memcpy(dst, src, len); \
		flush_icache_user_range(vma, page, vaddr, len); \
	} while (0)
#endif

#ifndef copy_from_user_page
#define copy_from_user_page(vma, page, vaddr, dst, src, len) \
	memcpy(dst, src, len)
#endif

#endif /* __ASM_CACHEFLUSH_H */
