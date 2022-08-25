/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_MEMORY_HOTPLUG_H
#define __LINUX_MEMORY_HOTPLUG_H

#include <linux/mmzone.h>
#include <linux/spinlock.h>
#include <linux/notifier.h>
#include <linux/bug.h>

struct page;
struct zone;
struct pglist_data;
struct mem_section;
struct memory_block;
struct resource;
struct vmem_altmap;

#ifdef CONFIG_MEMORY_HOTPLUG
/*
 * Return page for the valid pfn only if the page is online. All pfn
 * walkers which rely on the fully initialized page->flags and others
 * should use this rather than pfn_valid && pfn_to_page
 */
#define pfn_to_online_page(pfn)					   \
({								   \
	struct page *___page = NULL;				   \
	unsigned long ___pfn = pfn;				   \
	unsigned long ___nr = pfn_to_section_nr(___pfn);	   \
								   \
	if (___nr < NR_MEM_SECTIONS && online_section_nr(___nr) && \
	    pfn_valid_within(___pfn))				   \
		___page = pfn_to_page(___pfn);			   \
	___page;						   \
})

/*
 * Types for free bootmem stored in page->lru.next. These have to be in
 * some random range in unsigned long space for debugging purposes.
 */
enum {
	MEMORY_HOTPLUG_MIN_BOOTMEM_TYPE = 12,
	SECTION_INFO = MEMORY_HOTPLUG_MIN_BOOTMEM_TYPE,
	MIX_SECTION_INFO,
	NODE_INFO,
	MEMORY_HOTPLUG_MAX_BOOTMEM_TYPE = NODE_INFO,
};

/* Types for control the zone type of onlined and offlined memory */
enum {
	MMOP_OFFLINE = -1,
	MMOP_ONLINE_KEEP,
	MMOP_ONLINE_KERNEL,
	MMOP_ONLINE_MOVABLE,
};

/*
 * Restrictions for the memory hotplug:
 * flags:  MHP_ flags
 * altmap: alternative allocator for memmap array
 */
struct mhp_restrictions {
	unsigned long flags;
	struct vmem_altmap *altmap;
};

/*
 * Zone resizing functions
 *
 * Note: any attempt to resize a zone should has pgdat_resize_lock()
 * zone_span_writelock() both held. This ensure the size of a zone
 * can't be changed while pgdat_resize_lock() held.
 */
static inline unsigned zone_span_seqbegin(struct zone *zone)
{
	return read_seqbegin(&zone->span_seqlock);
}
static inline int zone_span_seqretry(struct zone *zone, unsigned iv)
{
	return read_seqretry(&zone->span_seqlock, iv);
}
static inline void zone_span_writelock(struct zone *zone)
{
	write_seqlock(&zone->span_seqlock);
}
static inline void zone_span_writeunlock(struct zone *zone)
{
	write_sequnlock(&zone->span_seqlock);
}
static inline void zone_seqlock_init(struct zone *zone)
{
	seqlock_init(&zone->span_seqlock);
}
extern int zone_grow_free_lists(struct zone *zone, unsigned long new_nr_pages);
extern int zone_grow_waitqueues(struct zone *zone, unsigned long nr_pages);
extern int add_one_highpage(struct page *page, int pfn, int bad_ppro);
/* VM interface that may be used by firmware interface */
extern int online_pages(unsigned long, unsigned long, int);
extern int test_pages_in_a_zone(unsigned long start_pfn, unsigned long end_pfn,
	unsigned long *valid_start, unsigned long *valid_end);
extern unsigned long __offline_isolated_pages(unsigned long start_pfn,
						unsigned long end_pfn);

typedef void (*online_page_callback_t)(struct page *page, unsigned int order);

extern int set_online_page_callback(online_page_callback_t callback);
extern int restore_online_page_callback(online_page_callback_t callback);

extern void __online_page_set_limits(struct page *page);
extern void __online_page_increment_counters(struct page *page);
extern void __online_page_free(struct page *page);

extern int try_online_node(int nid);

extern int arch_add_memory(int nid, u64 start, u64 size,
			struct mhp_restrictions *restrictions);
extern u64 max_mem_size;

extern bool memhp_auto_online;
/* If movable_node boot option specified */
extern bool movable_node_enabled;
static inline bool movable_node_is_enabled(void)
{
	return movable_node_enabled;
}

extern void arch_remove_memory(int nid, u64 start, u64 size,
			       struct vmem_altmap *altmap);
extern void __remove_pages(unsigned long start_pfn, unsigned long nr_pages,
			   struct vmem_altmap *altmap);

/* reasonably generic interface to expand the physical pages */
extern int __add_pages(int nid, unsigned long start_pfn, unsigned long nr_pages,
		       struct mhp_restrictions *restrictions);

#ifndef CONFIG_ARCH_HAS_ADD_PAGES
static inline int add_pages(int nid, unsigned long start_pfn,
		unsigned long nr_pages, struct mhp_restrictions *restrictions)
{
	return __add_pages(nid, start_pfn, nr_pages, restrictions);
}
#else /* ARCH_HAS_ADD_PAGES */
int add_pages(int nid, unsigned long start_pfn, unsigned long nr_pages,
	      struct mhp_restrictions *restrictions);
#endif /* ARCH_HAS_ADD_PAGES */

#ifdef CONFIG_NUMA
extern int memory_add_physaddr_to_nid(u64 start);
#else
static inline int memory_add_physaddr_to_nid(u64 start)
{
	return 0;
}
#endif

#ifdef CONFIG_HAVE_ARCH_NODEDATA_EXTENSION
/*
 * For supporting node-hotadd, we have to allocate a new pgdat.
 *
 * If an arch has generic style NODE_DATA(),
 * node_data[nid] = kzalloc() works well. But it depends on the architecture.
 *
 * In general, generic_alloc_nodedata() is used.
 * Now, arch_free_nodedata() is just defined for error path of node_hot_add.
 *
 */
extern pg_data_t *arch_alloc_nodedata(int nid);
extern void arch_free_nodedata(pg_data_t *pgdat);
extern void arch_refresh_nodedata(int nid, pg_data_t *pgdat);

#else /* CONFIG_HAVE_ARCH_NODEDATA_EXTENSION */

#define arch_alloc_nodedata(nid)	generic_alloc_nodedata(nid)
#define arch_free_nodedata(pgdat)	generic_free_nodedata(pgdat)

#ifdef CONFIG_NUMA
/*
 * If ARCH_HAS_NODEDATA_EXTENSION=n, this func is used to allocate pgdat.
 * XXX: kmalloc_node() can't work well to get new node's memory at this time.
 *	Because, pgdat for the new node is not allocated/initialized yet itself.
 *	To use new node's memory, more consideration will be necessary.
 */
#define generic_alloc_nodedata(nid)				\
({								\
	kzalloc(sizeof(pg_data_t), GFP_KERNEL);			\
})
/*
 * This definition is just for error path in node hotadd.
 * For node hotremove, we have to replace this.
 */
#define generic_free_nodedata(pgdat)	kfree(pgdat)

extern pg_data_t *node_data[];
static inline void arch_refresh_nodedata(int nid, pg_data_t *pgdat)
{
	node_data[nid] = pgdat;
}

#else /* !CONFIG_NUMA */

/* never called */
static inline pg_data_t *generic_alloc_nodedata(int nid)
{
	BUG();
	return NULL;
}
static inline void generic_free_nodedata(pg_data_t *pgdat)
{
}
static inline void arch_refresh_nodedata(int nid, pg_data_t *pgdat)
{
}
#endif /* CONFIG_NUMA */
#endif /* CONFIG_HAVE_ARCH_NODEDATA_EXTENSION */

#ifdef CONFIG_HAVE_BOOTMEM_INFO_NODE
extern void __init register_page_bootmem_info_node(struct pglist_data *pgdat);
#else
static inline void register_page_bootmem_info_node(struct pglist_data *pgdat)
{
}
#endif
extern void put_page_bootmem(struct page *page);
extern void get_page_bootmem(unsigned long ingo, struct page *page,
			     unsigned long type);

void get_online_mems(void);
void put_online_mems(void);

void mem_hotplug_begin(void);
void mem_hotplug_done(void);

extern void set_zone_contiguous(struct zone *zone);
extern void clear_zone_contiguous(struct zone *zone);

#else /* ! CONFIG_MEMORY_HOTPLUG */
#define pfn_to_online_page(pfn)			\
({						\
	struct page *___page = NULL;		\
	if (pfn_valid(pfn))			\
		___page = pfn_to_page(pfn);	\
	___page;				\
 })

static inline unsigned zone_span_seqbegin(struct zone *zone)
{
	return 0;
}
static inline int zone_span_seqretry(struct zone *zone, unsigned iv)
{
	return 0;
}
static inline void zone_span_writelock(struct zone *zone) {}
static inline void zone_span_writeunlock(struct zone *zone) {}
static inline void zone_seqlock_init(struct zone *zone) {}

static inline int mhp_notimplemented(const char *func)
{
	printk(KERN_WARNING "%s() called, with CONFIG_MEMORY_HOTPLUG disabled\n", func);
	dump_stack();
	return -ENOSYS;
}

static inline void register_page_bootmem_info_node(struct pglist_data *pgdat)
{
}

static inline int try_online_node(int nid)
{
	return 0;
}

static inline void get_online_mems(void) {}
static inline void put_online_mems(void) {}

static inline void mem_hotplug_begin(void) {}
static inline void mem_hotplug_done(void) {}

static inline bool movable_node_is_enabled(void)
{
	return false;
}
#endif /* ! CONFIG_MEMORY_HOTPLUG */

#if defined(CONFIG_MEMORY_HOTPLUG) || defined(CONFIG_DEFERRED_STRUCT_PAGE_INIT)
/*
 * pgdat resizing functions
 */
static inline
void pgdat_resize_lock(struct pglist_data *pgdat, unsigned long *flags)
{
	spin_lock_irqsave(&pgdat->node_size_lock, *flags);
}
static inline
void pgdat_resize_unlock(struct pglist_data *pgdat, unsigned long *flags)
{
	spin_unlock_irqrestore(&pgdat->node_size_lock, *flags);
}
static inline
void pgdat_resize_init(struct pglist_data *pgdat)
{
	spin_lock_init(&pgdat->node_size_lock);
}
#else /* !(CONFIG_MEMORY_HOTPLUG || CONFIG_DEFERRED_STRUCT_PAGE_INIT) */
/*
 * Stub functions for when hotplug is off
 */
static inline void pgdat_resize_lock(struct pglist_data *p, unsigned long *f) {}
static inline void pgdat_resize_unlock(struct pglist_data *p, unsigned long *f) {}
static inline void pgdat_resize_init(struct pglist_data *pgdat) {}
#endif /* !(CONFIG_MEMORY_HOTPLUG || CONFIG_DEFERRED_STRUCT_PAGE_INIT) */

#ifdef CONFIG_MEMORY_HOTREMOVE

extern bool is_mem_section_removable(unsigned long pfn, unsigned long nr_pages);
extern void try_offline_node(int nid);
extern int offline_pages(unsigned long start_pfn, unsigned long nr_pages);
extern int remove_memory(int nid, u64 start, u64 size);
extern void __remove_memory(int nid, u64 start, u64 size);

#else
static inline bool is_mem_section_removable(unsigned long pfn,
					unsigned long nr_pages)
{
	return false;
}

static inline void try_offline_node(int nid) {}

static inline int offline_pages(unsigned long start_pfn, unsigned long nr_pages)
{
	return -EINVAL;
}

static inline int remove_memory(int nid, u64 start, u64 size)
{
	return -EBUSY;
}

static inline void __remove_memory(int nid, u64 start, u64 size) {}
#endif /* CONFIG_MEMORY_HOTREMOVE */

extern void __ref free_area_init_core_hotplug(int nid);
extern int __add_memory(int nid, u64 start, u64 size);
extern int add_memory(int nid, u64 start, u64 size);
extern int add_memory_resource(int nid, struct resource *resource);
extern void move_pfn_range_to_zone(struct zone *zone, unsigned long start_pfn,
		unsigned long nr_pages, struct vmem_altmap *altmap);
extern void remove_pfn_range_from_zone(struct zone *zone,
				       unsigned long start_pfn,
				       unsigned long nr_pages);
extern bool is_memblock_offlined(struct memory_block *mem);
extern int sparse_add_section(int nid, unsigned long pfn,
		unsigned long nr_pages, struct vmem_altmap *altmap);
extern void sparse_remove_section(struct mem_section *ms,
		unsigned long pfn, unsigned long nr_pages,
		unsigned long map_offset, struct vmem_altmap *altmap);
extern struct page *sparse_decode_mem_map(unsigned long coded_mem_map,
					  unsigned long pnum);
extern bool allow_online_pfn_range(int nid, unsigned long pfn, unsigned long nr_pages,
		int online_type);
extern struct zone *zone_for_pfn_range(int online_type, int nid, unsigned start_pfn,
		unsigned long nr_pages);
#endif /* __LINUX_MEMORY_HOTPLUG_H */
