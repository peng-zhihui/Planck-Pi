// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/mm/vmalloc.c
 *
 *  Copyright (C) 1993  Linus Torvalds
 *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 *  SMP-safe vmalloc/vfree/ioremap, Tigran Aivazian <tigran@veritas.com>, May 2000
 *  Major rework to support vmap/vunmap, Christoph Hellwig, SGI, August 2002
 *  Numa awareness, Christoph Lameter, SGI, June 2005
 */

#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/set_memory.h>
#include <linux/debugobjects.h>
#include <linux/kallsyms.h>
#include <linux/list.h>
#include <linux/notifier.h>
#include <linux/rbtree.h>
#include <linux/radix-tree.h>
#include <linux/rcupdate.h>
#include <linux/pfn.h>
#include <linux/kmemleak.h>
#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/llist.h>
#include <linux/bitops.h>
#include <linux/rbtree_augmented.h>
#include <linux/overflow.h>

#include <linux/uaccess.h>
#include <asm/tlbflush.h>
#include <asm/shmparam.h>

#include "internal.h"

struct vfree_deferred {
	struct llist_head list;
	struct work_struct wq;
};
static DEFINE_PER_CPU(struct vfree_deferred, vfree_deferred);

static void __vunmap(const void *, int);

static void free_work(struct work_struct *w)
{
	struct vfree_deferred *p = container_of(w, struct vfree_deferred, wq);
	struct llist_node *t, *llnode;

	llist_for_each_safe(llnode, t, llist_del_all(&p->list))
		__vunmap((void *)llnode, 1);
}

/*** Page table manipulation functions ***/

static void vunmap_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end)
{
	pte_t *pte;

	pte = pte_offset_kernel(pmd, addr);
	do {
		pte_t ptent = ptep_get_and_clear(&init_mm, addr, pte);
		WARN_ON(!pte_none(ptent) && !pte_present(ptent));
	} while (pte++, addr += PAGE_SIZE, addr != end);
}

static void vunmap_pmd_range(pud_t *pud, unsigned long addr, unsigned long end)
{
	pmd_t *pmd;
	unsigned long next;

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		if (pmd_clear_huge(pmd))
			continue;
		if (pmd_none_or_clear_bad(pmd))
			continue;
		vunmap_pte_range(pmd, addr, next);

		cond_resched();
	} while (pmd++, addr = next, addr != end);
}

static void vunmap_pud_range(p4d_t *p4d, unsigned long addr, unsigned long end)
{
	pud_t *pud;
	unsigned long next;

	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_clear_huge(pud))
			continue;
		if (pud_none_or_clear_bad(pud))
			continue;
		vunmap_pmd_range(pud, addr, next);
	} while (pud++, addr = next, addr != end);
}

static void vunmap_p4d_range(pgd_t *pgd, unsigned long addr, unsigned long end)
{
	p4d_t *p4d;
	unsigned long next;

	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);
		if (p4d_clear_huge(p4d))
			continue;
		if (p4d_none_or_clear_bad(p4d))
			continue;
		vunmap_pud_range(p4d, addr, next);
	} while (p4d++, addr = next, addr != end);
}

static void vunmap_page_range(unsigned long addr, unsigned long end)
{
	pgd_t *pgd;
	unsigned long next;

	BUG_ON(addr >= end);
	pgd = pgd_offset_k(addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(pgd))
			continue;
		vunmap_p4d_range(pgd, addr, next);
	} while (pgd++, addr = next, addr != end);
}

static int vmap_pte_range(pmd_t *pmd, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr)
{
	pte_t *pte;

	/*
	 * nr is a running index into the array which helps higher level
	 * callers keep track of where we're up to.
	 */

	pte = pte_alloc_kernel(pmd, addr);
	if (!pte)
		return -ENOMEM;
	do {
		struct page *page = pages[*nr];

		if (WARN_ON(!pte_none(*pte)))
			return -EBUSY;
		if (WARN_ON(!page))
			return -ENOMEM;
		set_pte_at(&init_mm, addr, pte, mk_pte(page, prot));
		(*nr)++;
	} while (pte++, addr += PAGE_SIZE, addr != end);
	return 0;
}

static int vmap_pmd_range(pud_t *pud, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr)
{
	pmd_t *pmd;
	unsigned long next;

	pmd = pmd_alloc(&init_mm, pud, addr);
	if (!pmd)
		return -ENOMEM;
	do {
		next = pmd_addr_end(addr, end);
		if (vmap_pte_range(pmd, addr, next, prot, pages, nr))
			return -ENOMEM;
	} while (pmd++, addr = next, addr != end);
	return 0;
}

static int vmap_pud_range(p4d_t *p4d, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr)
{
	pud_t *pud;
	unsigned long next;

	pud = pud_alloc(&init_mm, p4d, addr);
	if (!pud)
		return -ENOMEM;
	do {
		next = pud_addr_end(addr, end);
		if (vmap_pmd_range(pud, addr, next, prot, pages, nr))
			return -ENOMEM;
	} while (pud++, addr = next, addr != end);
	return 0;
}

static int vmap_p4d_range(pgd_t *pgd, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr)
{
	p4d_t *p4d;
	unsigned long next;

	p4d = p4d_alloc(&init_mm, pgd, addr);
	if (!p4d)
		return -ENOMEM;
	do {
		next = p4d_addr_end(addr, end);
		if (vmap_pud_range(p4d, addr, next, prot, pages, nr))
			return -ENOMEM;
	} while (p4d++, addr = next, addr != end);
	return 0;
}

/*
 * Set up page tables in kva (addr, end). The ptes shall have prot "prot", and
 * will have pfns corresponding to the "pages" array.
 *
 * Ie. pte at addr+N*PAGE_SIZE shall point to pfn corresponding to pages[N]
 */
static int vmap_page_range_noflush(unsigned long start, unsigned long end,
				   pgprot_t prot, struct page **pages)
{
	pgd_t *pgd;
	unsigned long next;
	unsigned long addr = start;
	int err = 0;
	int nr = 0;

	BUG_ON(addr >= end);
	pgd = pgd_offset_k(addr);
	do {
		next = pgd_addr_end(addr, end);
		err = vmap_p4d_range(pgd, addr, next, prot, pages, &nr);
		if (err)
			return err;
	} while (pgd++, addr = next, addr != end);

	return nr;
}

static int vmap_page_range(unsigned long start, unsigned long end,
			   pgprot_t prot, struct page **pages)
{
	int ret;

	ret = vmap_page_range_noflush(start, end, prot, pages);
	flush_cache_vmap(start, end);
	return ret;
}

int is_vmalloc_or_module_addr(const void *x)
{
	/*
	 * ARM, x86-64 and sparc64 put modules in a special place,
	 * and fall back on vmalloc() if that fails. Others
	 * just put it in the vmalloc space.
	 */
#if defined(CONFIG_MODULES) && defined(MODULES_VADDR)
	unsigned long addr = (unsigned long)x;
	if (addr >= MODULES_VADDR && addr < MODULES_END)
		return 1;
#endif
	return is_vmalloc_addr(x);
}

/*
 * Walk a vmap address to the struct page it maps.
 */
struct page *vmalloc_to_page(const void *vmalloc_addr)
{
	unsigned long addr = (unsigned long) vmalloc_addr;
	struct page *page = NULL;
	pgd_t *pgd = pgd_offset_k(addr);
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, pte;

	/*
	 * XXX we might need to change this if we add VIRTUAL_BUG_ON for
	 * architectures that do not vmalloc module space
	 */
	VIRTUAL_BUG_ON(!is_vmalloc_or_module_addr(vmalloc_addr));

	if (pgd_none(*pgd))
		return NULL;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d))
		return NULL;
	pud = pud_offset(p4d, addr);

	/*
	 * Don't dereference bad PUD or PMD (below) entries. This will also
	 * identify huge mappings, which we may encounter on architectures
	 * that define CONFIG_HAVE_ARCH_HUGE_VMAP=y. Such regions will be
	 * identified as vmalloc addresses by is_vmalloc_addr(), but are
	 * not [unambiguously] associated with a struct page, so there is
	 * no correct value to return for them.
	 */
	WARN_ON_ONCE(pud_bad(*pud));
	if (pud_none(*pud) || pud_bad(*pud))
		return NULL;
	pmd = pmd_offset(pud, addr);
	WARN_ON_ONCE(pmd_bad(*pmd));
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return NULL;

	ptep = pte_offset_map(pmd, addr);
	pte = *ptep;
	if (pte_present(pte))
		page = pte_page(pte);
	pte_unmap(ptep);
	return page;
}
EXPORT_SYMBOL(vmalloc_to_page);

/*
 * Map a vmalloc()-space virtual address to the physical page frame number.
 */
unsigned long vmalloc_to_pfn(const void *vmalloc_addr)
{
	return page_to_pfn(vmalloc_to_page(vmalloc_addr));
}
EXPORT_SYMBOL(vmalloc_to_pfn);


/*** Global kva allocator ***/

#define DEBUG_AUGMENT_PROPAGATE_CHECK 0
#define DEBUG_AUGMENT_LOWEST_MATCH_CHECK 0


static DEFINE_SPINLOCK(vmap_area_lock);
/* Export for kexec only */
LIST_HEAD(vmap_area_list);
static LLIST_HEAD(vmap_purge_list);
static struct rb_root vmap_area_root = RB_ROOT;
static bool vmap_initialized __read_mostly;

/*
 * This kmem_cache is used for vmap_area objects. Instead of
 * allocating from slab we reuse an object from this cache to
 * make things faster. Especially in "no edge" splitting of
 * free block.
 */
static struct kmem_cache *vmap_area_cachep;

/*
 * This linked list is used in pair with free_vmap_area_root.
 * It gives O(1) access to prev/next to perform fast coalescing.
 */
static LIST_HEAD(free_vmap_area_list);

/*
 * This augment red-black tree represents the free vmap space.
 * All vmap_area objects in this tree are sorted by va->va_start
 * address. It is used for allocation and merging when a vmap
 * object is released.
 *
 * Each vmap_area node contains a maximum available free block
 * of its sub-tree, right or left. Therefore it is possible to
 * find a lowest match of free area.
 */
static struct rb_root free_vmap_area_root = RB_ROOT;

/*
 * Preload a CPU with one object for "no edge" split case. The
 * aim is to get rid of allocations from the atomic context, thus
 * to use more permissive allocation masks.
 */
static DEFINE_PER_CPU(struct vmap_area *, ne_fit_preload_node);

static __always_inline unsigned long
va_size(struct vmap_area *va)
{
	return (va->va_end - va->va_start);
}

static __always_inline unsigned long
get_subtree_max_size(struct rb_node *node)
{
	struct vmap_area *va;

	va = rb_entry_safe(node, struct vmap_area, rb_node);
	return va ? va->subtree_max_size : 0;
}

/*
 * Gets called when remove the node and rotate.
 */
static __always_inline unsigned long
compute_subtree_max_size(struct vmap_area *va)
{
	return max3(va_size(va),
		get_subtree_max_size(va->rb_node.rb_left),
		get_subtree_max_size(va->rb_node.rb_right));
}

RB_DECLARE_CALLBACKS_MAX(static, free_vmap_area_rb_augment_cb,
	struct vmap_area, rb_node, unsigned long, subtree_max_size, va_size)

static void purge_vmap_area_lazy(void);
static BLOCKING_NOTIFIER_HEAD(vmap_notify_list);
static unsigned long lazy_max_pages(void);

static atomic_long_t nr_vmalloc_pages;

unsigned long vmalloc_nr_pages(void)
{
	return atomic_long_read(&nr_vmalloc_pages);
}

static struct vmap_area *__find_vmap_area(unsigned long addr)
{
	struct rb_node *n = vmap_area_root.rb_node;

	while (n) {
		struct vmap_area *va;

		va = rb_entry(n, struct vmap_area, rb_node);
		if (addr < va->va_start)
			n = n->rb_left;
		else if (addr >= va->va_end)
			n = n->rb_right;
		else
			return va;
	}

	return NULL;
}

/*
 * This function returns back addresses of parent node
 * and its left or right link for further processing.
 */
static __always_inline struct rb_node **
find_va_links(struct vmap_area *va,
	struct rb_root *root, struct rb_node *from,
	struct rb_node **parent)
{
	struct vmap_area *tmp_va;
	struct rb_node **link;

	if (root) {
		link = &root->rb_node;
		if (unlikely(!*link)) {
			*parent = NULL;
			return link;
		}
	} else {
		link = &from;
	}

	/*
	 * Go to the bottom of the tree. When we hit the last point
	 * we end up with parent rb_node and correct direction, i name
	 * it link, where the new va->rb_node will be attached to.
	 */
	do {
		tmp_va = rb_entry(*link, struct vmap_area, rb_node);

		/*
		 * During the traversal we also do some sanity check.
		 * Trigger the BUG() if there are sides(left/right)
		 * or full overlaps.
		 */
		if (va->va_start < tmp_va->va_end &&
				va->va_end <= tmp_va->va_start)
			link = &(*link)->rb_left;
		else if (va->va_end > tmp_va->va_start &&
				va->va_start >= tmp_va->va_end)
			link = &(*link)->rb_right;
		else
			BUG();
	} while (*link);

	*parent = &tmp_va->rb_node;
	return link;
}

static __always_inline struct list_head *
get_va_next_sibling(struct rb_node *parent, struct rb_node **link)
{
	struct list_head *list;

	if (unlikely(!parent))
		/*
		 * The red-black tree where we try to find VA neighbors
		 * before merging or inserting is empty, i.e. it means
		 * there is no free vmap space. Normally it does not
		 * happen but we handle this case anyway.
		 */
		return NULL;

	list = &rb_entry(parent, struct vmap_area, rb_node)->list;
	return (&parent->rb_right == link ? list->next : list);
}

static __always_inline void
link_va(struct vmap_area *va, struct rb_root *root,
	struct rb_node *parent, struct rb_node **link, struct list_head *head)
{
	/*
	 * VA is still not in the list, but we can
	 * identify its future previous list_head node.
	 */
	if (likely(parent)) {
		head = &rb_entry(parent, struct vmap_area, rb_node)->list;
		if (&parent->rb_right != link)
			head = head->prev;
	}

	/* Insert to the rb-tree */
	rb_link_node(&va->rb_node, parent, link);
	if (root == &free_vmap_area_root) {
		/*
		 * Some explanation here. Just perform simple insertion
		 * to the tree. We do not set va->subtree_max_size to
		 * its current size before calling rb_insert_augmented().
		 * It is because of we populate the tree from the bottom
		 * to parent levels when the node _is_ in the tree.
		 *
		 * Therefore we set subtree_max_size to zero after insertion,
		 * to let __augment_tree_propagate_from() puts everything to
		 * the correct order later on.
		 */
		rb_insert_augmented(&va->rb_node,
			root, &free_vmap_area_rb_augment_cb);
		va->subtree_max_size = 0;
	} else {
		rb_insert_color(&va->rb_node, root);
	}

	/* Address-sort this list */
	list_add(&va->list, head);
}

static __always_inline void
unlink_va(struct vmap_area *va, struct rb_root *root)
{
	if (WARN_ON(RB_EMPTY_NODE(&va->rb_node)))
		return;

	if (root == &free_vmap_area_root)
		rb_erase_augmented(&va->rb_node,
			root, &free_vmap_area_rb_augment_cb);
	else
		rb_erase(&va->rb_node, root);

	list_del(&va->list);
	RB_CLEAR_NODE(&va->rb_node);
}

#if DEBUG_AUGMENT_PROPAGATE_CHECK
static void
augment_tree_propagate_check(struct rb_node *n)
{
	struct vmap_area *va;
	struct rb_node *node;
	unsigned long size;
	bool found = false;

	if (n == NULL)
		return;

	va = rb_entry(n, struct vmap_area, rb_node);
	size = va->subtree_max_size;
	node = n;

	while (node) {
		va = rb_entry(node, struct vmap_area, rb_node);

		if (get_subtree_max_size(node->rb_left) == size) {
			node = node->rb_left;
		} else {
			if (va_size(va) == size) {
				found = true;
				break;
			}

			node = node->rb_right;
		}
	}

	if (!found) {
		va = rb_entry(n, struct vmap_area, rb_node);
		pr_emerg("tree is corrupted: %lu, %lu\n",
			va_size(va), va->subtree_max_size);
	}

	augment_tree_propagate_check(n->rb_left);
	augment_tree_propagate_check(n->rb_right);
}
#endif

/*
 * This function populates subtree_max_size from bottom to upper
 * levels starting from VA point. The propagation must be done
 * when VA size is modified by changing its va_start/va_end. Or
 * in case of newly inserting of VA to the tree.
 *
 * It means that __augment_tree_propagate_from() must be called:
 * - After VA has been inserted to the tree(free path);
 * - After VA has been shrunk(allocation path);
 * - After VA has been increased(merging path).
 *
 * Please note that, it does not mean that upper parent nodes
 * and their subtree_max_size are recalculated all the time up
 * to the root node.
 *
 *       4--8
 *        /\
 *       /  \
 *      /    \
 *    2--2  8--8
 *
 * For example if we modify the node 4, shrinking it to 2, then
 * no any modification is required. If we shrink the node 2 to 1
 * its subtree_max_size is updated only, and set to 1. If we shrink
 * the node 8 to 6, then its subtree_max_size is set to 6 and parent
 * node becomes 4--6.
 */
static __always_inline void
augment_tree_propagate_from(struct vmap_area *va)
{
	struct rb_node *node = &va->rb_node;
	unsigned long new_va_sub_max_size;

	while (node) {
		va = rb_entry(node, struct vmap_area, rb_node);
		new_va_sub_max_size = compute_subtree_max_size(va);

		/*
		 * If the newly calculated maximum available size of the
		 * subtree is equal to the current one, then it means that
		 * the tree is propagated correctly. So we have to stop at
		 * this point to save cycles.
		 */
		if (va->subtree_max_size == new_va_sub_max_size)
			break;

		va->subtree_max_size = new_va_sub_max_size;
		node = rb_parent(&va->rb_node);
	}

#if DEBUG_AUGMENT_PROPAGATE_CHECK
	augment_tree_propagate_check(free_vmap_area_root.rb_node);
#endif
}

static void
insert_vmap_area(struct vmap_area *va,
	struct rb_root *root, struct list_head *head)
{
	struct rb_node **link;
	struct rb_node *parent;

	link = find_va_links(va, root, NULL, &parent);
	link_va(va, root, parent, link, head);
}

static void
insert_vmap_area_augment(struct vmap_area *va,
	struct rb_node *from, struct rb_root *root,
	struct list_head *head)
{
	struct rb_node **link;
	struct rb_node *parent;

	if (from)
		link = find_va_links(va, NULL, from, &parent);
	else
		link = find_va_links(va, root, NULL, &parent);

	link_va(va, root, parent, link, head);
	augment_tree_propagate_from(va);
}

/*
 * Merge de-allocated chunk of VA memory with previous
 * and next free blocks. If coalesce is not done a new
 * free area is inserted. If VA has been merged, it is
 * freed.
 */
static __always_inline void
merge_or_add_vmap_area(struct vmap_area *va,
	struct rb_root *root, struct list_head *head)
{
	struct vmap_area *sibling;
	struct list_head *next;
	struct rb_node **link;
	struct rb_node *parent;
	bool merged = false;

	/*
	 * Find a place in the tree where VA potentially will be
	 * inserted, unless it is merged with its sibling/siblings.
	 */
	link = find_va_links(va, root, NULL, &parent);

	/*
	 * Get next node of VA to check if merging can be done.
	 */
	next = get_va_next_sibling(parent, link);
	if (unlikely(next == NULL))
		goto insert;

	/*
	 * start            end
	 * |                |
	 * |<------VA------>|<-----Next----->|
	 *                  |                |
	 *                  start            end
	 */
	if (next != head) {
		sibling = list_entry(next, struct vmap_area, list);
		if (sibling->va_start == va->va_end) {
			sibling->va_start = va->va_start;

			/* Check and update the tree if needed. */
			augment_tree_propagate_from(sibling);

			/* Free vmap_area object. */
			kmem_cache_free(vmap_area_cachep, va);

			/* Point to the new merged area. */
			va = sibling;
			merged = true;
		}
	}

	/*
	 * start            end
	 * |                |
	 * |<-----Prev----->|<------VA------>|
	 *                  |                |
	 *                  start            end
	 */
	if (next->prev != head) {
		sibling = list_entry(next->prev, struct vmap_area, list);
		if (sibling->va_end == va->va_start) {
			sibling->va_end = va->va_end;

			/* Check and update the tree if needed. */
			augment_tree_propagate_from(sibling);

			if (merged)
				unlink_va(va, root);

			/* Free vmap_area object. */
			kmem_cache_free(vmap_area_cachep, va);
			return;
		}
	}

insert:
	if (!merged) {
		link_va(va, root, parent, link, head);
		augment_tree_propagate_from(va);
	}
}

static __always_inline bool
is_within_this_va(struct vmap_area *va, unsigned long size,
	unsigned long align, unsigned long vstart)
{
	unsigned long nva_start_addr;

	if (va->va_start > vstart)
		nva_start_addr = ALIGN(va->va_start, align);
	else
		nva_start_addr = ALIGN(vstart, align);

	/* Can be overflowed due to big size or alignment. */
	if (nva_start_addr + size < nva_start_addr ||
			nva_start_addr < vstart)
		return false;

	return (nva_start_addr + size <= va->va_end);
}

/*
 * Find the first free block(lowest start address) in the tree,
 * that will accomplish the request corresponding to passing
 * parameters.
 */
static __always_inline struct vmap_area *
find_vmap_lowest_match(unsigned long size,
	unsigned long align, unsigned long vstart)
{
	struct vmap_area *va;
	struct rb_node *node;
	unsigned long length;

	/* Start from the root. */
	node = free_vmap_area_root.rb_node;

	/* Adjust the search size for alignment overhead. */
	length = size + align - 1;

	while (node) {
		va = rb_entry(node, struct vmap_area, rb_node);

		if (get_subtree_max_size(node->rb_left) >= length &&
				vstart < va->va_start) {
			node = node->rb_left;
		} else {
			if (is_within_this_va(va, size, align, vstart))
				return va;

			/*
			 * Does not make sense to go deeper towards the right
			 * sub-tree if it does not have a free block that is
			 * equal or bigger to the requested search length.
			 */
			if (get_subtree_max_size(node->rb_right) >= length) {
				node = node->rb_right;
				continue;
			}

			/*
			 * OK. We roll back and find the first right sub-tree,
			 * that will satisfy the search criteria. It can happen
			 * only once due to "vstart" restriction.
			 */
			while ((node = rb_parent(node))) {
				va = rb_entry(node, struct vmap_area, rb_node);
				if (is_within_this_va(va, size, align, vstart))
					return va;

				if (get_subtree_max_size(node->rb_right) >= length &&
						vstart <= va->va_start) {
					node = node->rb_right;
					break;
				}
			}
		}
	}

	return NULL;
}

#if DEBUG_AUGMENT_LOWEST_MATCH_CHECK
#include <linux/random.h>

static struct vmap_area *
find_vmap_lowest_linear_match(unsigned long size,
	unsigned long align, unsigned long vstart)
{
	struct vmap_area *va;

	list_for_each_entry(va, &free_vmap_area_list, list) {
		if (!is_within_this_va(va, size, align, vstart))
			continue;

		return va;
	}

	return NULL;
}

static void
find_vmap_lowest_match_check(unsigned long size)
{
	struct vmap_area *va_1, *va_2;
	unsigned long vstart;
	unsigned int rnd;

	get_random_bytes(&rnd, sizeof(rnd));
	vstart = VMALLOC_START + rnd;

	va_1 = find_vmap_lowest_match(size, 1, vstart);
	va_2 = find_vmap_lowest_linear_match(size, 1, vstart);

	if (va_1 != va_2)
		pr_emerg("not lowest: t: 0x%p, l: 0x%p, v: 0x%lx\n",
			va_1, va_2, vstart);
}
#endif

enum fit_type {
	NOTHING_FIT = 0,
	FL_FIT_TYPE = 1,	/* full fit */
	LE_FIT_TYPE = 2,	/* left edge fit */
	RE_FIT_TYPE = 3,	/* right edge fit */
	NE_FIT_TYPE = 4		/* no edge fit */
};

static __always_inline enum fit_type
classify_va_fit_type(struct vmap_area *va,
	unsigned long nva_start_addr, unsigned long size)
{
	enum fit_type type;

	/* Check if it is within VA. */
	if (nva_start_addr < va->va_start ||
			nva_start_addr + size > va->va_end)
		return NOTHING_FIT;

	/* Now classify. */
	if (va->va_start == nva_start_addr) {
		if (va->va_end == nva_start_addr + size)
			type = FL_FIT_TYPE;
		else
			type = LE_FIT_TYPE;
	} else if (va->va_end == nva_start_addr + size) {
		type = RE_FIT_TYPE;
	} else {
		type = NE_FIT_TYPE;
	}

	return type;
}

static __always_inline int
adjust_va_to_fit_type(struct vmap_area *va,
	unsigned long nva_start_addr, unsigned long size,
	enum fit_type type)
{
	struct vmap_area *lva = NULL;

	if (type == FL_FIT_TYPE) {
		/*
		 * No need to split VA, it fully fits.
		 *
		 * |               |
		 * V      NVA      V
		 * |---------------|
		 */
		unlink_va(va, &free_vmap_area_root);
		kmem_cache_free(vmap_area_cachep, va);
	} else if (type == LE_FIT_TYPE) {
		/*
		 * Split left edge of fit VA.
		 *
		 * |       |
		 * V  NVA  V   R
		 * |-------|-------|
		 */
		va->va_start += size;
	} else if (type == RE_FIT_TYPE) {
		/*
		 * Split right edge of fit VA.
		 *
		 *         |       |
		 *     L   V  NVA  V
		 * |-------|-------|
		 */
		va->va_end = nva_start_addr;
	} else if (type == NE_FIT_TYPE) {
		/*
		 * Split no edge of fit VA.
		 *
		 *     |       |
		 *   L V  NVA  V R
		 * |---|-------|---|
		 */
		lva = __this_cpu_xchg(ne_fit_preload_node, NULL);
		if (unlikely(!lva)) {
			/*
			 * For percpu allocator we do not do any pre-allocation
			 * and leave it as it is. The reason is it most likely
			 * never ends up with NE_FIT_TYPE splitting. In case of
			 * percpu allocations offsets and sizes are aligned to
			 * fixed align request, i.e. RE_FIT_TYPE and FL_FIT_TYPE
			 * are its main fitting cases.
			 *
			 * There are a few exceptions though, as an example it is
			 * a first allocation (early boot up) when we have "one"
			 * big free space that has to be split.
			 */
			lva = kmem_cache_alloc(vmap_area_cachep, GFP_NOWAIT);
			if (!lva)
				return -1;
		}

		/*
		 * Build the remainder.
		 */
		lva->va_start = va->va_start;
		lva->va_end = nva_start_addr;

		/*
		 * Shrink this VA to remaining size.
		 */
		va->va_start = nva_start_addr + size;
	} else {
		return -1;
	}

	if (type != FL_FIT_TYPE) {
		augment_tree_propagate_from(va);

		if (lva)	/* type == NE_FIT_TYPE */
			insert_vmap_area_augment(lva, &va->rb_node,
				&free_vmap_area_root, &free_vmap_area_list);
	}

	return 0;
}

/*
 * Returns a start address of the newly allocated area, if success.
 * Otherwise a vend is returned that indicates failure.
 */
static __always_inline unsigned long
__alloc_vmap_area(unsigned long size, unsigned long align,
	unsigned long vstart, unsigned long vend)
{
	unsigned long nva_start_addr;
	struct vmap_area *va;
	enum fit_type type;
	int ret;

	va = find_vmap_lowest_match(size, align, vstart);
	if (unlikely(!va))
		return vend;

	if (va->va_start > vstart)
		nva_start_addr = ALIGN(va->va_start, align);
	else
		nva_start_addr = ALIGN(vstart, align);

	/* Check the "vend" restriction. */
	if (nva_start_addr + size > vend)
		return vend;

	/* Classify what we have found. */
	type = classify_va_fit_type(va, nva_start_addr, size);
	if (WARN_ON_ONCE(type == NOTHING_FIT))
		return vend;

	/* Update the free vmap_area. */
	ret = adjust_va_to_fit_type(va, nva_start_addr, size, type);
	if (ret)
		return vend;

#if DEBUG_AUGMENT_LOWEST_MATCH_CHECK
	find_vmap_lowest_match_check(size);
#endif

	return nva_start_addr;
}

/*
 * Allocate a region of KVA of the specified size and alignment, within the
 * vstart and vend.
 */
static struct vmap_area *alloc_vmap_area(unsigned long size,
				unsigned long align,
				unsigned long vstart, unsigned long vend,
				int node, gfp_t gfp_mask)
{
	struct vmap_area *va, *pva;
	unsigned long addr;
	int purged = 0;

	BUG_ON(!size);
	BUG_ON(offset_in_page(size));
	BUG_ON(!is_power_of_2(align));

	if (unlikely(!vmap_initialized))
		return ERR_PTR(-EBUSY);

	might_sleep();

	va = kmem_cache_alloc_node(vmap_area_cachep,
			gfp_mask & GFP_RECLAIM_MASK, node);
	if (unlikely(!va))
		return ERR_PTR(-ENOMEM);

	/*
	 * Only scan the relevant parts containing pointers to other objects
	 * to avoid false negatives.
	 */
	kmemleak_scan_area(&va->rb_node, SIZE_MAX, gfp_mask & GFP_RECLAIM_MASK);

retry:
	/*
	 * Preload this CPU with one extra vmap_area object to ensure
	 * that we have it available when fit type of free area is
	 * NE_FIT_TYPE.
	 *
	 * The preload is done in non-atomic context, thus it allows us
	 * to use more permissive allocation masks to be more stable under
	 * low memory condition and high memory pressure.
	 *
	 * Even if it fails we do not really care about that. Just proceed
	 * as it is. "overflow" path will refill the cache we allocate from.
	 */
	preempt_disable();
	if (!__this_cpu_read(ne_fit_preload_node)) {
		preempt_enable();
		pva = kmem_cache_alloc_node(vmap_area_cachep, GFP_KERNEL, node);
		preempt_disable();

		if (__this_cpu_cmpxchg(ne_fit_preload_node, NULL, pva)) {
			if (pva)
				kmem_cache_free(vmap_area_cachep, pva);
		}
	}

	spin_lock(&vmap_area_lock);
	preempt_enable();

	/*
	 * If an allocation fails, the "vend" address is
	 * returned. Therefore trigger the overflow path.
	 */
	addr = __alloc_vmap_area(size, align, vstart, vend);
	if (unlikely(addr == vend))
		goto overflow;

	va->va_start = addr;
	va->va_end = addr + size;
	va->vm = NULL;
	insert_vmap_area(va, &vmap_area_root, &vmap_area_list);

	spin_unlock(&vmap_area_lock);

	BUG_ON(!IS_ALIGNED(va->va_start, align));
	BUG_ON(va->va_start < vstart);
	BUG_ON(va->va_end > vend);

	return va;

overflow:
	spin_unlock(&vmap_area_lock);
	if (!purged) {
		purge_vmap_area_lazy();
		purged = 1;
		goto retry;
	}

	if (gfpflags_allow_blocking(gfp_mask)) {
		unsigned long freed = 0;
		blocking_notifier_call_chain(&vmap_notify_list, 0, &freed);
		if (freed > 0) {
			purged = 0;
			goto retry;
		}
	}

	if (!(gfp_mask & __GFP_NOWARN) && printk_ratelimit())
		pr_warn("vmap allocation for size %lu failed: use vmalloc=<size> to increase size\n",
			size);

	kmem_cache_free(vmap_area_cachep, va);
	return ERR_PTR(-EBUSY);
}

int register_vmap_purge_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&vmap_notify_list, nb);
}
EXPORT_SYMBOL_GPL(register_vmap_purge_notifier);

int unregister_vmap_purge_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&vmap_notify_list, nb);
}
EXPORT_SYMBOL_GPL(unregister_vmap_purge_notifier);

static void __free_vmap_area(struct vmap_area *va)
{
	/*
	 * Remove from the busy tree/list.
	 */
	unlink_va(va, &vmap_area_root);

	/*
	 * Merge VA with its neighbors, otherwise just add it.
	 */
	merge_or_add_vmap_area(va,
		&free_vmap_area_root, &free_vmap_area_list);
}

/*
 * Free a region of KVA allocated by alloc_vmap_area
 */
static void free_vmap_area(struct vmap_area *va)
{
	spin_lock(&vmap_area_lock);
	__free_vmap_area(va);
	spin_unlock(&vmap_area_lock);
}

/*
 * Clear the pagetable entries of a given vmap_area
 */
static void unmap_vmap_area(struct vmap_area *va)
{
	vunmap_page_range(va->va_start, va->va_end);
}

/*
 * lazy_max_pages is the maximum amount of virtual address space we gather up
 * before attempting to purge with a TLB flush.
 *
 * There is a tradeoff here: a larger number will cover more kernel page tables
 * and take slightly longer to purge, but it will linearly reduce the number of
 * global TLB flushes that must be performed. It would seem natural to scale
 * this number up linearly with the number of CPUs (because vmapping activity
 * could also scale linearly with the number of CPUs), however it is likely
 * that in practice, workloads might be constrained in other ways that mean
 * vmap activity will not scale linearly with CPUs. Also, I want to be
 * conservative and not introduce a big latency on huge systems, so go with
 * a less aggressive log scale. It will still be an improvement over the old
 * code, and it will be simple to change the scale factor if we find that it
 * becomes a problem on bigger systems.
 */
static unsigned long lazy_max_pages(void)
{
	unsigned int log;

	log = fls(num_online_cpus());

	return log * (32UL * 1024 * 1024 / PAGE_SIZE);
}

static atomic_long_t vmap_lazy_nr = ATOMIC_LONG_INIT(0);

/*
 * Serialize vmap purging.  There is no actual criticial section protected
 * by this look, but we want to avoid concurrent calls for performance
 * reasons and to make the pcpu_get_vm_areas more deterministic.
 */
static DEFINE_MUTEX(vmap_purge_lock);

/* for per-CPU blocks */
static void purge_fragmented_blocks_allcpus(void);

/*
 * called before a call to iounmap() if the caller wants vm_area_struct's
 * immediately freed.
 */
void set_iounmap_nonlazy(void)
{
	atomic_long_set(&vmap_lazy_nr, lazy_max_pages()+1);
}

/*
 * Purges all lazily-freed vmap areas.
 */
static bool __purge_vmap_area_lazy(unsigned long start, unsigned long end)
{
	unsigned long resched_threshold;
	struct llist_node *valist;
	struct vmap_area *va;
	struct vmap_area *n_va;

	lockdep_assert_held(&vmap_purge_lock);

	valist = llist_del_all(&vmap_purge_list);
	if (unlikely(valist == NULL))
		return false;

	/*
	 * First make sure the mappings are removed from all page-tables
	 * before they are freed.
	 */
	vmalloc_sync_unmappings();

	/*
	 * TODO: to calculate a flush range without looping.
	 * The list can be up to lazy_max_pages() elements.
	 */
	llist_for_each_entry(va, valist, purge_list) {
		if (va->va_start < start)
			start = va->va_start;
		if (va->va_end > end)
			end = va->va_end;
	}

	flush_tlb_kernel_range(start, end);
	resched_threshold = lazy_max_pages() << 1;

	spin_lock(&vmap_area_lock);
	llist_for_each_entry_safe(va, n_va, valist, purge_list) {
		unsigned long nr = (va->va_end - va->va_start) >> PAGE_SHIFT;

		/*
		 * Finally insert or merge lazily-freed area. It is
		 * detached and there is no need to "unlink" it from
		 * anything.
		 */
		merge_or_add_vmap_area(va,
			&free_vmap_area_root, &free_vmap_area_list);

		atomic_long_sub(nr, &vmap_lazy_nr);

		if (atomic_long_read(&vmap_lazy_nr) < resched_threshold)
			cond_resched_lock(&vmap_area_lock);
	}
	spin_unlock(&vmap_area_lock);
	return true;
}

/*
 * Kick off a purge of the outstanding lazy areas. Don't bother if somebody
 * is already purging.
 */
static void try_purge_vmap_area_lazy(void)
{
	if (mutex_trylock(&vmap_purge_lock)) {
		__purge_vmap_area_lazy(ULONG_MAX, 0);
		mutex_unlock(&vmap_purge_lock);
	}
}

/*
 * Kick off a purge of the outstanding lazy areas.
 */
static void purge_vmap_area_lazy(void)
{
	mutex_lock(&vmap_purge_lock);
	purge_fragmented_blocks_allcpus();
	__purge_vmap_area_lazy(ULONG_MAX, 0);
	mutex_unlock(&vmap_purge_lock);
}

/*
 * Free a vmap area, caller ensuring that the area has been unmapped
 * and flush_cache_vunmap had been called for the correct range
 * previously.
 */
static void free_vmap_area_noflush(struct vmap_area *va)
{
	unsigned long nr_lazy;

	spin_lock(&vmap_area_lock);
	unlink_va(va, &vmap_area_root);
	spin_unlock(&vmap_area_lock);

	nr_lazy = atomic_long_add_return((va->va_end - va->va_start) >>
				PAGE_SHIFT, &vmap_lazy_nr);

	/* After this point, we may free va at any time */
	llist_add(&va->purge_list, &vmap_purge_list);

	if (unlikely(nr_lazy > lazy_max_pages()))
		try_purge_vmap_area_lazy();
}

/*
 * Free and unmap a vmap area
 */
static void free_unmap_vmap_area(struct vmap_area *va)
{
	flush_cache_vunmap(va->va_start, va->va_end);
	unmap_vmap_area(va);
	if (debug_pagealloc_enabled_static())
		flush_tlb_kernel_range(va->va_start, va->va_end);

	free_vmap_area_noflush(va);
}

static struct vmap_area *find_vmap_area(unsigned long addr)
{
	struct vmap_area *va;

	spin_lock(&vmap_area_lock);
	va = __find_vmap_area(addr);
	spin_unlock(&vmap_area_lock);

	return va;
}

/*** Per cpu kva allocator ***/

/*
 * vmap space is limited especially on 32 bit architectures. Ensure there is
 * room for at least 16 percpu vmap blocks per CPU.
 */
/*
 * If we had a constant VMALLOC_START and VMALLOC_END, we'd like to be able
 * to #define VMALLOC_SPACE		(VMALLOC_END-VMALLOC_START). Guess
 * instead (we just need a rough idea)
 */
#if BITS_PER_LONG == 32
#define VMALLOC_SPACE		(128UL*1024*1024)
#else
#define VMALLOC_SPACE		(128UL*1024*1024*1024)
#endif

#define VMALLOC_PAGES		(VMALLOC_SPACE / PAGE_SIZE)
#define VMAP_MAX_ALLOC		BITS_PER_LONG	/* 256K with 4K pages */
#define VMAP_BBMAP_BITS_MAX	1024	/* 4MB with 4K pages */
#define VMAP_BBMAP_BITS_MIN	(VMAP_MAX_ALLOC*2)
#define VMAP_MIN(x, y)		((x) < (y) ? (x) : (y)) /* can't use min() */
#define VMAP_MAX(x, y)		((x) > (y) ? (x) : (y)) /* can't use max() */
#define VMAP_BBMAP_BITS		\
		VMAP_MIN(VMAP_BBMAP_BITS_MAX,	\
		VMAP_MAX(VMAP_BBMAP_BITS_MIN,	\
			VMALLOC_PAGES / roundup_pow_of_two(NR_CPUS) / 16))

#define VMAP_BLOCK_SIZE		(VMAP_BBMAP_BITS * PAGE_SIZE)

struct vmap_block_queue {
	spinlock_t lock;
	struct list_head free;
};

struct vmap_block {
	spinlock_t lock;
	struct vmap_area *va;
	unsigned long free, dirty;
	unsigned long dirty_min, dirty_max; /*< dirty range */
	struct list_head free_list;
	struct rcu_head rcu_head;
	struct list_head purge;
};

/* Queue of free and dirty vmap blocks, for allocation and flushing purposes */
static DEFINE_PER_CPU(struct vmap_block_queue, vmap_block_queue);

/*
 * Radix tree of vmap blocks, indexed by address, to quickly find a vmap block
 * in the free path. Could get rid of this if we change the API to return a
 * "cookie" from alloc, to be passed to free. But no big deal yet.
 */
static DEFINE_SPINLOCK(vmap_block_tree_lock);
static RADIX_TREE(vmap_block_tree, GFP_ATOMIC);

/*
 * We should probably have a fallback mechanism to allocate virtual memory
 * out of partially filled vmap blocks. However vmap block sizing should be
 * fairly reasonable according to the vmalloc size, so it shouldn't be a
 * big problem.
 */

static unsigned long addr_to_vb_idx(unsigned long addr)
{
	addr -= VMALLOC_START & ~(VMAP_BLOCK_SIZE-1);
	addr /= VMAP_BLOCK_SIZE;
	return addr;
}

static void *vmap_block_vaddr(unsigned long va_start, unsigned long pages_off)
{
	unsigned long addr;

	addr = va_start + (pages_off << PAGE_SHIFT);
	BUG_ON(addr_to_vb_idx(addr) != addr_to_vb_idx(va_start));
	return (void *)addr;
}

/**
 * new_vmap_block - allocates new vmap_block and occupies 2^order pages in this
 *                  block. Of course pages number can't exceed VMAP_BBMAP_BITS
 * @order:    how many 2^order pages should be occupied in newly allocated block
 * @gfp_mask: flags for the page level allocator
 *
 * Return: virtual address in a newly allocated block or ERR_PTR(-errno)
 */
static void *new_vmap_block(unsigned int order, gfp_t gfp_mask)
{
	struct vmap_block_queue *vbq;
	struct vmap_block *vb;
	struct vmap_area *va;
	unsigned long vb_idx;
	int node, err;
	void *vaddr;

	node = numa_node_id();

	vb = kmalloc_node(sizeof(struct vmap_block),
			gfp_mask & GFP_RECLAIM_MASK, node);
	if (unlikely(!vb))
		return ERR_PTR(-ENOMEM);

	va = alloc_vmap_area(VMAP_BLOCK_SIZE, VMAP_BLOCK_SIZE,
					VMALLOC_START, VMALLOC_END,
					node, gfp_mask);
	if (IS_ERR(va)) {
		kfree(vb);
		return ERR_CAST(va);
	}

	err = radix_tree_preload(gfp_mask);
	if (unlikely(err)) {
		kfree(vb);
		free_vmap_area(va);
		return ERR_PTR(err);
	}

	vaddr = vmap_block_vaddr(va->va_start, 0);
	spin_lock_init(&vb->lock);
	vb->va = va;
	/* At least something should be left free */
	BUG_ON(VMAP_BBMAP_BITS <= (1UL << order));
	vb->free = VMAP_BBMAP_BITS - (1UL << order);
	vb->dirty = 0;
	vb->dirty_min = VMAP_BBMAP_BITS;
	vb->dirty_max = 0;
	INIT_LIST_HEAD(&vb->free_list);

	vb_idx = addr_to_vb_idx(va->va_start);
	spin_lock(&vmap_block_tree_lock);
	err = radix_tree_insert(&vmap_block_tree, vb_idx, vb);
	spin_unlock(&vmap_block_tree_lock);
	BUG_ON(err);
	radix_tree_preload_end();

	vbq = &get_cpu_var(vmap_block_queue);
	spin_lock(&vbq->lock);
	list_add_tail_rcu(&vb->free_list, &vbq->free);
	spin_unlock(&vbq->lock);
	put_cpu_var(vmap_block_queue);

	return vaddr;
}

static void free_vmap_block(struct vmap_block *vb)
{
	struct vmap_block *tmp;
	unsigned long vb_idx;

	vb_idx = addr_to_vb_idx(vb->va->va_start);
	spin_lock(&vmap_block_tree_lock);
	tmp = radix_tree_delete(&vmap_block_tree, vb_idx);
	spin_unlock(&vmap_block_tree_lock);
	BUG_ON(tmp != vb);

	free_vmap_area_noflush(vb->va);
	kfree_rcu(vb, rcu_head);
}

static void purge_fragmented_blocks(int cpu)
{
	LIST_HEAD(purge);
	struct vmap_block *vb;
	struct vmap_block *n_vb;
	struct vmap_block_queue *vbq = &per_cpu(vmap_block_queue, cpu);

	rcu_read_lock();
	list_for_each_entry_rcu(vb, &vbq->free, free_list) {

		if (!(vb->free + vb->dirty == VMAP_BBMAP_BITS && vb->dirty != VMAP_BBMAP_BITS))
			continue;

		spin_lock(&vb->lock);
		if (vb->free + vb->dirty == VMAP_BBMAP_BITS && vb->dirty != VMAP_BBMAP_BITS) {
			vb->free = 0; /* prevent further allocs after releasing lock */
			vb->dirty = VMAP_BBMAP_BITS; /* prevent purging it again */
			vb->dirty_min = 0;
			vb->dirty_max = VMAP_BBMAP_BITS;
			spin_lock(&vbq->lock);
			list_del_rcu(&vb->free_list);
			spin_unlock(&vbq->lock);
			spin_unlock(&vb->lock);
			list_add_tail(&vb->purge, &purge);
		} else
			spin_unlock(&vb->lock);
	}
	rcu_read_unlock();

	list_for_each_entry_safe(vb, n_vb, &purge, purge) {
		list_del(&vb->purge);
		free_vmap_block(vb);
	}
}

static void purge_fragmented_blocks_allcpus(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		purge_fragmented_blocks(cpu);
}

static void *vb_alloc(unsigned long size, gfp_t gfp_mask)
{
	struct vmap_block_queue *vbq;
	struct vmap_block *vb;
	void *vaddr = NULL;
	unsigned int order;

	BUG_ON(offset_in_page(size));
	BUG_ON(size > PAGE_SIZE*VMAP_MAX_ALLOC);
	if (WARN_ON(size == 0)) {
		/*
		 * Allocating 0 bytes isn't what caller wants since
		 * get_order(0) returns funny result. Just warn and terminate
		 * early.
		 */
		return NULL;
	}
	order = get_order(size);

	rcu_read_lock();
	vbq = &get_cpu_var(vmap_block_queue);
	list_for_each_entry_rcu(vb, &vbq->free, free_list) {
		unsigned long pages_off;

		spin_lock(&vb->lock);
		if (vb->free < (1UL << order)) {
			spin_unlock(&vb->lock);
			continue;
		}

		pages_off = VMAP_BBMAP_BITS - vb->free;
		vaddr = vmap_block_vaddr(vb->va->va_start, pages_off);
		vb->free -= 1UL << order;
		if (vb->free == 0) {
			spin_lock(&vbq->lock);
			list_del_rcu(&vb->free_list);
			spin_unlock(&vbq->lock);
		}

		spin_unlock(&vb->lock);
		break;
	}

	put_cpu_var(vmap_block_queue);
	rcu_read_unlock();

	/* Allocate new block if nothing was found */
	if (!vaddr)
		vaddr = new_vmap_block(order, gfp_mask);

	return vaddr;
}

static void vb_free(const void *addr, unsigned long size)
{
	unsigned long offset;
	unsigned long vb_idx;
	unsigned int order;
	struct vmap_block *vb;

	BUG_ON(offset_in_page(size));
	BUG_ON(size > PAGE_SIZE*VMAP_MAX_ALLOC);

	flush_cache_vunmap((unsigned long)addr, (unsigned long)addr + size);

	order = get_order(size);

	offset = (unsigned long)addr & (VMAP_BLOCK_SIZE - 1);
	offset >>= PAGE_SHIFT;

	vb_idx = addr_to_vb_idx((unsigned long)addr);
	rcu_read_lock();
	vb = radix_tree_lookup(&vmap_block_tree, vb_idx);
	rcu_read_unlock();
	BUG_ON(!vb);

	vunmap_page_range((unsigned long)addr, (unsigned long)addr + size);

	if (debug_pagealloc_enabled_static())
		flush_tlb_kernel_range((unsigned long)addr,
					(unsigned long)addr + size);

	spin_lock(&vb->lock);

	/* Expand dirty range */
	vb->dirty_min = min(vb->dirty_min, offset);
	vb->dirty_max = max(vb->dirty_max, offset + (1UL << order));

	vb->dirty += 1UL << order;
	if (vb->dirty == VMAP_BBMAP_BITS) {
		BUG_ON(vb->free);
		spin_unlock(&vb->lock);
		free_vmap_block(vb);
	} else
		spin_unlock(&vb->lock);
}

static void _vm_unmap_aliases(unsigned long start, unsigned long end, int flush)
{
	int cpu;

	if (unlikely(!vmap_initialized))
		return;

	might_sleep();

	for_each_possible_cpu(cpu) {
		struct vmap_block_queue *vbq = &per_cpu(vmap_block_queue, cpu);
		struct vmap_block *vb;

		rcu_read_lock();
		list_for_each_entry_rcu(vb, &vbq->free, free_list) {
			spin_lock(&vb->lock);
			if (vb->dirty) {
				unsigned long va_start = vb->va->va_start;
				unsigned long s, e;

				s = va_start + (vb->dirty_min << PAGE_SHIFT);
				e = va_start + (vb->dirty_max << PAGE_SHIFT);

				start = min(s, start);
				end   = max(e, end);

				flush = 1;
			}
			spin_unlock(&vb->lock);
		}
		rcu_read_unlock();
	}

	mutex_lock(&vmap_purge_lock);
	purge_fragmented_blocks_allcpus();
	if (!__purge_vmap_area_lazy(start, end) && flush)
		flush_tlb_kernel_range(start, end);
	mutex_unlock(&vmap_purge_lock);
}

/**
 * vm_unmap_aliases - unmap outstanding lazy aliases in the vmap layer
 *
 * The vmap/vmalloc layer lazily flushes kernel virtual mappings primarily
 * to amortize TLB flushing overheads. What this means is that any page you
 * have now, may, in a former life, have been mapped into kernel virtual
 * address by the vmap layer and so there might be some CPUs with TLB entries
 * still referencing that page (additional to the regular 1:1 kernel mapping).
 *
 * vm_unmap_aliases flushes all such lazy mappings. After it returns, we can
 * be sure that none of the pages we have control over will have any aliases
 * from the vmap layer.
 */
void vm_unmap_aliases(void)
{
	unsigned long start = ULONG_MAX, end = 0;
	int flush = 0;

	_vm_unmap_aliases(start, end, flush);
}
EXPORT_SYMBOL_GPL(vm_unmap_aliases);

/**
 * vm_unmap_ram - unmap linear kernel address space set up by vm_map_ram
 * @mem: the pointer returned by vm_map_ram
 * @count: the count passed to that vm_map_ram call (cannot unmap partial)
 */
void vm_unmap_ram(const void *mem, unsigned int count)
{
	unsigned long size = (unsigned long)count << PAGE_SHIFT;
	unsigned long addr = (unsigned long)mem;
	struct vmap_area *va;

	might_sleep();
	BUG_ON(!addr);
	BUG_ON(addr < VMALLOC_START);
	BUG_ON(addr > VMALLOC_END);
	BUG_ON(!PAGE_ALIGNED(addr));

	if (likely(count <= VMAP_MAX_ALLOC)) {
		debug_check_no_locks_freed(mem, size);
		vb_free(mem, size);
		return;
	}

	va = find_vmap_area(addr);
	BUG_ON(!va);
	debug_check_no_locks_freed((void *)va->va_start,
				    (va->va_end - va->va_start));
	free_unmap_vmap_area(va);
}
EXPORT_SYMBOL(vm_unmap_ram);

/**
 * vm_map_ram - map pages linearly into kernel virtual address (vmalloc space)
 * @pages: an array of pointers to the pages to be mapped
 * @count: number of pages
 * @node: prefer to allocate data structures on this node
 * @prot: memory protection to use. PAGE_KERNEL for regular RAM
 *
 * If you use this function for less than VMAP_MAX_ALLOC pages, it could be
 * faster than vmap so it's good.  But if you mix long-life and short-life
 * objects with vm_map_ram(), it could consume lots of address space through
 * fragmentation (especially on a 32bit machine).  You could see failures in
 * the end.  Please use this function for short-lived objects.
 *
 * Returns: a pointer to the address that has been mapped, or %NULL on failure
 */
void *vm_map_ram(struct page **pages, unsigned int count, int node, pgprot_t prot)
{
	unsigned long size = (unsigned long)count << PAGE_SHIFT;
	unsigned long addr;
	void *mem;

	if (likely(count <= VMAP_MAX_ALLOC)) {
		mem = vb_alloc(size, GFP_KERNEL);
		if (IS_ERR(mem))
			return NULL;
		addr = (unsigned long)mem;
	} else {
		struct vmap_area *va;
		va = alloc_vmap_area(size, PAGE_SIZE,
				VMALLOC_START, VMALLOC_END, node, GFP_KERNEL);
		if (IS_ERR(va))
			return NULL;

		addr = va->va_start;
		mem = (void *)addr;
	}
	if (vmap_page_range(addr, addr + size, prot, pages) < 0) {
		vm_unmap_ram(mem, count);
		return NULL;
	}
	return mem;
}
EXPORT_SYMBOL(vm_map_ram);

static struct vm_struct *vmlist __initdata;

/**
 * vm_area_add_early - add vmap area early during boot
 * @vm: vm_struct to add
 *
 * This function is used to add fixed kernel vm area to vmlist before
 * vmalloc_init() is called.  @vm->addr, @vm->size, and @vm->flags
 * should contain proper values and the other fields should be zero.
 *
 * DO NOT USE THIS FUNCTION UNLESS YOU KNOW WHAT YOU'RE DOING.
 */
void __init vm_area_add_early(struct vm_struct *vm)
{
	struct vm_struct *tmp, **p;

	BUG_ON(vmap_initialized);
	for (p = &vmlist; (tmp = *p) != NULL; p = &tmp->next) {
		if (tmp->addr >= vm->addr) {
			BUG_ON(tmp->addr < vm->addr + vm->size);
			break;
		} else
			BUG_ON(tmp->addr + tmp->size > vm->addr);
	}
	vm->next = *p;
	*p = vm;
}

/**
 * vm_area_register_early - register vmap area early during boot
 * @vm: vm_struct to register
 * @align: requested alignment
 *
 * This function is used to register kernel vm area before
 * vmalloc_init() is called.  @vm->size and @vm->flags should contain
 * proper values on entry and other fields should be zero.  On return,
 * vm->addr contains the allocated address.
 *
 * DO NOT USE THIS FUNCTION UNLESS YOU KNOW WHAT YOU'RE DOING.
 */
void __init vm_area_register_early(struct vm_struct *vm, size_t align)
{
	static size_t vm_init_off __initdata;
	unsigned long addr;

	addr = ALIGN(VMALLOC_START + vm_init_off, align);
	vm_init_off = PFN_ALIGN(addr + vm->size) - VMALLOC_START;

	vm->addr = (void *)addr;

	vm_area_add_early(vm);
}

static void vmap_init_free_space(void)
{
	unsigned long vmap_start = 1;
	const unsigned long vmap_end = ULONG_MAX;
	struct vmap_area *busy, *free;

	/*
	 *     B     F     B     B     B     F
	 * -|-----|.....|-----|-----|-----|.....|-
	 *  |           The KVA space           |
	 *  |<--------------------------------->|
	 */
	list_for_each_entry(busy, &vmap_area_list, list) {
		if (busy->va_start - vmap_start > 0) {
			free = kmem_cache_zalloc(vmap_area_cachep, GFP_NOWAIT);
			if (!WARN_ON_ONCE(!free)) {
				free->va_start = vmap_start;
				free->va_end = busy->va_start;

				insert_vmap_area_augment(free, NULL,
					&free_vmap_area_root,
						&free_vmap_area_list);
			}
		}

		vmap_start = busy->va_end;
	}

	if (vmap_end - vmap_start > 0) {
		free = kmem_cache_zalloc(vmap_area_cachep, GFP_NOWAIT);
		if (!WARN_ON_ONCE(!free)) {
			free->va_start = vmap_start;
			free->va_end = vmap_end;

			insert_vmap_area_augment(free, NULL,
				&free_vmap_area_root,
					&free_vmap_area_list);
		}
	}
}

void __init vmalloc_init(void)
{
	struct vmap_area *va;
	struct vm_struct *tmp;
	int i;

	/*
	 * Create the cache for vmap_area objects.
	 */
	vmap_area_cachep = KMEM_CACHE(vmap_area, SLAB_PANIC);

	for_each_possible_cpu(i) {
		struct vmap_block_queue *vbq;
		struct vfree_deferred *p;

		vbq = &per_cpu(vmap_block_queue, i);
		spin_lock_init(&vbq->lock);
		INIT_LIST_HEAD(&vbq->free);
		p = &per_cpu(vfree_deferred, i);
		init_llist_head(&p->list);
		INIT_WORK(&p->wq, free_work);
	}

	/* Import existing vmlist entries. */
	for (tmp = vmlist; tmp; tmp = tmp->next) {
		va = kmem_cache_zalloc(vmap_area_cachep, GFP_NOWAIT);
		if (WARN_ON_ONCE(!va))
			continue;

		va->va_start = (unsigned long)tmp->addr;
		va->va_end = va->va_start + tmp->size;
		va->vm = tmp;
		insert_vmap_area(va, &vmap_area_root, &vmap_area_list);
	}

	/*
	 * Now we can initialize a free vmap space.
	 */
	vmap_init_free_space();
	vmap_initialized = true;
}

/**
 * map_kernel_range_noflush - map kernel VM area with the specified pages
 * @addr: start of the VM area to map
 * @size: size of the VM area to map
 * @prot: page protection flags to use
 * @pages: pages to map
 *
 * Map PFN_UP(@size) pages at @addr.  The VM area @addr and @size
 * specify should have been allocated using get_vm_area() and its
 * friends.
 *
 * NOTE:
 * This function does NOT do any cache flushing.  The caller is
 * responsible for calling flush_cache_vmap() on to-be-mapped areas
 * before calling this function.
 *
 * RETURNS:
 * The number of pages mapped on success, -errno on failure.
 */
int map_kernel_range_noflush(unsigned long addr, unsigned long size,
			     pgprot_t prot, struct page **pages)
{
	return vmap_page_range_noflush(addr, addr + size, prot, pages);
}

/**
 * unmap_kernel_range_noflush - unmap kernel VM area
 * @addr: start of the VM area to unmap
 * @size: size of the VM area to unmap
 *
 * Unmap PFN_UP(@size) pages at @addr.  The VM area @addr and @size
 * specify should have been allocated using get_vm_area() and its
 * friends.
 *
 * NOTE:
 * This function does NOT do any cache flushing.  The caller is
 * responsible for calling flush_cache_vunmap() on to-be-mapped areas
 * before calling this function and flush_tlb_kernel_range() after.
 */
void unmap_kernel_range_noflush(unsigned long addr, unsigned long size)
{
	vunmap_page_range(addr, addr + size);
}
EXPORT_SYMBOL_GPL(unmap_kernel_range_noflush);

/**
 * unmap_kernel_range - unmap kernel VM area and flush cache and TLB
 * @addr: start of the VM area to unmap
 * @size: size of the VM area to unmap
 *
 * Similar to unmap_kernel_range_noflush() but flushes vcache before
 * the unmapping and tlb after.
 */
void unmap_kernel_range(unsigned long addr, unsigned long size)
{
	unsigned long end = addr + size;

	flush_cache_vunmap(addr, end);
	vunmap_page_range(addr, end);
	flush_tlb_kernel_range(addr, end);
}
EXPORT_SYMBOL_GPL(unmap_kernel_range);

int map_vm_area(struct vm_struct *area, pgprot_t prot, struct page **pages)
{
	unsigned long addr = (unsigned long)area->addr;
	unsigned long end = addr + get_vm_area_size(area);
	int err;

	err = vmap_page_range(addr, end, prot, pages);

	return err > 0 ? 0 : err;
}
EXPORT_SYMBOL_GPL(map_vm_area);

static void setup_vmalloc_vm(struct vm_struct *vm, struct vmap_area *va,
			      unsigned long flags, const void *caller)
{
	spin_lock(&vmap_area_lock);
	vm->flags = flags;
	vm->addr = (void *)va->va_start;
	vm->size = va->va_end - va->va_start;
	vm->caller = caller;
	va->vm = vm;
	spin_unlock(&vmap_area_lock);
}

static void clear_vm_uninitialized_flag(struct vm_struct *vm)
{
	/*
	 * Before removing VM_UNINITIALIZED,
	 * we should make sure that vm has proper values.
	 * Pair with smp_rmb() in show_numa_info().
	 */
	smp_wmb();
	vm->flags &= ~VM_UNINITIALIZED;
}

static struct vm_struct *__get_vm_area_node(unsigned long size,
		unsigned long align, unsigned long flags, unsigned long start,
		unsigned long end, int node, gfp_t gfp_mask, const void *caller)
{
	struct vmap_area *va;
	struct vm_struct *area;

	BUG_ON(in_interrupt());
	size = PAGE_ALIGN(size);
	if (unlikely(!size))
		return NULL;

	if (flags & VM_IOREMAP)
		align = 1ul << clamp_t(int, get_count_order_long(size),
				       PAGE_SHIFT, IOREMAP_MAX_ORDER);

	area = kzalloc_node(sizeof(*area), gfp_mask & GFP_RECLAIM_MASK, node);
	if (unlikely(!area))
		return NULL;

	if (!(flags & VM_NO_GUARD))
		size += PAGE_SIZE;

	va = alloc_vmap_area(size, align, start, end, node, gfp_mask);
	if (IS_ERR(va)) {
		kfree(area);
		return NULL;
	}

	setup_vmalloc_vm(area, va, flags, caller);

	return area;
}

struct vm_struct *__get_vm_area(unsigned long size, unsigned long flags,
				unsigned long start, unsigned long end)
{
	return __get_vm_area_node(size, 1, flags, start, end, NUMA_NO_NODE,
				  GFP_KERNEL, __builtin_return_address(0));
}
EXPORT_SYMBOL_GPL(__get_vm_area);

struct vm_struct *__get_vm_area_caller(unsigned long size, unsigned long flags,
				       unsigned long start, unsigned long end,
				       const void *caller)
{
	return __get_vm_area_node(size, 1, flags, start, end, NUMA_NO_NODE,
				  GFP_KERNEL, caller);
}

/**
 * get_vm_area - reserve a contiguous kernel virtual area
 * @size:	 size of the area
 * @flags:	 %VM_IOREMAP for I/O mappings or VM_ALLOC
 *
 * Search an area of @size in the kernel virtual mapping area,
 * and reserved it for out purposes.  Returns the area descriptor
 * on success or %NULL on failure.
 *
 * Return: the area descriptor on success or %NULL on failure.
 */
struct vm_struct *get_vm_area(unsigned long size, unsigned long flags)
{
	return __get_vm_area_node(size, 1, flags, VMALLOC_START, VMALLOC_END,
				  NUMA_NO_NODE, GFP_KERNEL,
				  __builtin_return_address(0));
}

struct vm_struct *get_vm_area_caller(unsigned long size, unsigned long flags,
				const void *caller)
{
	return __get_vm_area_node(size, 1, flags, VMALLOC_START, VMALLOC_END,
				  NUMA_NO_NODE, GFP_KERNEL, caller);
}

/**
 * find_vm_area - find a continuous kernel virtual area
 * @addr:	  base address
 *
 * Search for the kernel VM area starting at @addr, and return it.
 * It is up to the caller to do all required locking to keep the returned
 * pointer valid.
 *
 * Return: pointer to the found area or %NULL on faulure
 */
struct vm_struct *find_vm_area(const void *addr)
{
	struct vmap_area *va;

	va = find_vmap_area((unsigned long)addr);
	if (!va)
		return NULL;

	return va->vm;
}

/**
 * remove_vm_area - find and remove a continuous kernel virtual area
 * @addr:	    base address
 *
 * Search for the kernel VM area starting at @addr, and remove it.
 * This function returns the found VM area, but using it is NOT safe
 * on SMP machines, except for its size or flags.
 *
 * Return: pointer to the found area or %NULL on faulure
 */
struct vm_struct *remove_vm_area(const void *addr)
{
	struct vmap_area *va;

	might_sleep();

	spin_lock(&vmap_area_lock);
	va = __find_vmap_area((unsigned long)addr);
	if (va && va->vm) {
		struct vm_struct *vm = va->vm;

		va->vm = NULL;
		spin_unlock(&vmap_area_lock);

		kasan_free_shadow(vm);
		free_unmap_vmap_area(va);

		return vm;
	}

	spin_unlock(&vmap_area_lock);
	return NULL;
}

static inline void set_area_direct_map(const struct vm_struct *area,
				       int (*set_direct_map)(struct page *page))
{
	int i;

	for (i = 0; i < area->nr_pages; i++)
		if (page_address(area->pages[i]))
			set_direct_map(area->pages[i]);
}

/* Handle removing and resetting vm mappings related to the vm_struct. */
static void vm_remove_mappings(struct vm_struct *area, int deallocate_pages)
{
	unsigned long start = ULONG_MAX, end = 0;
	int flush_reset = area->flags & VM_FLUSH_RESET_PERMS;
	int flush_dmap = 0;
	int i;

	remove_vm_area(area->addr);

	/* If this is not VM_FLUSH_RESET_PERMS memory, no need for the below. */
	if (!flush_reset)
		return;

	/*
	 * If not deallocating pages, just do the flush of the VM area and
	 * return.
	 */
	if (!deallocate_pages) {
		vm_unmap_aliases();
		return;
	}

	/*
	 * If execution gets here, flush the vm mapping and reset the direct
	 * map. Find the start and end range of the direct mappings to make sure
	 * the vm_unmap_aliases() flush includes the direct map.
	 */
	for (i = 0; i < area->nr_pages; i++) {
		unsigned long addr = (unsigned long)page_address(area->pages[i]);
		if (addr) {
			start = min(addr, start);
			end = max(addr + PAGE_SIZE, end);
			flush_dmap = 1;
		}
	}

	/*
	 * Set direct map to something invalid so that it won't be cached if
	 * there are any accesses after the TLB flush, then flush the TLB and
	 * reset the direct map permissions to the default.
	 */
	set_area_direct_map(area, set_direct_map_invalid_noflush);
	_vm_unmap_aliases(start, end, flush_dmap);
	set_area_direct_map(area, set_direct_map_default_noflush);
}

static void __vunmap(const void *addr, int deallocate_pages)
{
	struct vm_struct *area;

	if (!addr)
		return;

	if (WARN(!PAGE_ALIGNED(addr), "Trying to vfree() bad address (%p)\n",
			addr))
		return;

	area = find_vm_area(addr);
	if (unlikely(!area)) {
		WARN(1, KERN_ERR "Trying to vfree() nonexistent vm area (%p)\n",
				addr);
		return;
	}

	debug_check_no_locks_freed(area->addr, get_vm_area_size(area));
	debug_check_no_obj_freed(area->addr, get_vm_area_size(area));

	vm_remove_mappings(area, deallocate_pages);

	if (deallocate_pages) {
		int i;

		for (i = 0; i < area->nr_pages; i++) {
			struct page *page = area->pages[i];

			BUG_ON(!page);
			__free_pages(page, 0);
		}
		atomic_long_sub(area->nr_pages, &nr_vmalloc_pages);

		kvfree(area->pages);
	}

	kfree(area);
	return;
}

static inline void __vfree_deferred(const void *addr)
{
	/*
	 * Use raw_cpu_ptr() because this can be called from preemptible
	 * context. Preemption is absolutely fine here, because the llist_add()
	 * implementation is lockless, so it works even if we are adding to
	 * nother cpu's list.  schedule_work() should be fine with this too.
	 */
	struct vfree_deferred *p = raw_cpu_ptr(&vfree_deferred);

	if (llist_add((struct llist_node *)addr, &p->list))
		schedule_work(&p->wq);
}

/**
 * vfree_atomic - release memory allocated by vmalloc()
 * @addr:	  memory base address
 *
 * This one is just like vfree() but can be called in any atomic context
 * except NMIs.
 */
void vfree_atomic(const void *addr)
{
	BUG_ON(in_nmi());

	kmemleak_free(addr);

	if (!addr)
		return;
	__vfree_deferred(addr);
}

static void __vfree(const void *addr)
{
	if (unlikely(in_interrupt()))
		__vfree_deferred(addr);
	else
		__vunmap(addr, 1);
}

/**
 * vfree - release memory allocated by vmalloc()
 * @addr:  memory base address
 *
 * Free the virtually continuous memory area starting at @addr, as
 * obtained from vmalloc(), vmalloc_32() or __vmalloc(). If @addr is
 * NULL, no operation is performed.
 *
 * Must not be called in NMI context (strictly speaking, only if we don't
 * have CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG, but making the calling
 * conventions for vfree() arch-depenedent would be a really bad idea)
 *
 * May sleep if called *not* from interrupt context.
 *
 * NOTE: assumes that the object at @addr has a size >= sizeof(llist_node)
 */
void vfree(const void *addr)
{
	BUG_ON(in_nmi());

	kmemleak_free(addr);

	might_sleep_if(!in_interrupt());

	if (!addr)
		return;

	__vfree(addr);
}
EXPORT_SYMBOL(vfree);

/**
 * vunmap - release virtual mapping obtained by vmap()
 * @addr:   memory base address
 *
 * Free the virtually contiguous memory area starting at @addr,
 * which was created from the page array passed to vmap().
 *
 * Must not be called in interrupt context.
 */
void vunmap(const void *addr)
{
	BUG_ON(in_interrupt());
	might_sleep();
	if (addr)
		__vunmap(addr, 0);
}
EXPORT_SYMBOL(vunmap);

/**
 * vmap - map an array of pages into virtually contiguous space
 * @pages: array of page pointers
 * @count: number of pages to map
 * @flags: vm_area->flags
 * @prot: page protection for the mapping
 *
 * Maps @count pages from @pages into contiguous kernel virtual
 * space.
 *
 * Return: the address of the area or %NULL on failure
 */
void *vmap(struct page **pages, unsigned int count,
	   unsigned long flags, pgprot_t prot)
{
	struct vm_struct *area;
	unsigned long size;		/* In bytes */

	might_sleep();

	if (count > totalram_pages())
		return NULL;

	size = (unsigned long)count << PAGE_SHIFT;
	area = get_vm_area_caller(size, flags, __builtin_return_address(0));
	if (!area)
		return NULL;

	if (map_vm_area(area, prot, pages)) {
		vunmap(area->addr);
		return NULL;
	}

	return area->addr;
}
EXPORT_SYMBOL(vmap);

static void *__vmalloc_node(unsigned long size, unsigned long align,
			    gfp_t gfp_mask, pgprot_t prot,
			    int node, const void *caller);
static void *__vmalloc_area_node(struct vm_struct *area, gfp_t gfp_mask,
				 pgprot_t prot, int node)
{
	struct page **pages;
	unsigned int nr_pages, array_size, i;
	const gfp_t nested_gfp = (gfp_mask & GFP_RECLAIM_MASK) | __GFP_ZERO;
	const gfp_t alloc_mask = gfp_mask | __GFP_NOWARN;
	const gfp_t highmem_mask = (gfp_mask & (GFP_DMA | GFP_DMA32)) ?
					0 :
					__GFP_HIGHMEM;

	nr_pages = get_vm_area_size(area) >> PAGE_SHIFT;
	array_size = (nr_pages * sizeof(struct page *));

	/* Please note that the recursion is strictly bounded. */
	if (array_size > PAGE_SIZE) {
		pages = __vmalloc_node(array_size, 1, nested_gfp|highmem_mask,
				PAGE_KERNEL, node, area->caller);
	} else {
		pages = kmalloc_node(array_size, nested_gfp, node);
	}

	if (!pages) {
		remove_vm_area(area->addr);
		kfree(area);
		return NULL;
	}

	area->pages = pages;
	area->nr_pages = nr_pages;

	for (i = 0; i < area->nr_pages; i++) {
		struct page *page;

		if (node == NUMA_NO_NODE)
			page = alloc_page(alloc_mask|highmem_mask);
		else
			page = alloc_pages_node(node, alloc_mask|highmem_mask, 0);

		if (unlikely(!page)) {
			/* Successfully allocated i pages, free them in __vunmap() */
			area->nr_pages = i;
			atomic_long_add(area->nr_pages, &nr_vmalloc_pages);
			goto fail;
		}
		area->pages[i] = page;
		if (gfpflags_allow_blocking(gfp_mask|highmem_mask))
			cond_resched();
	}
	atomic_long_add(area->nr_pages, &nr_vmalloc_pages);

	if (map_vm_area(area, prot, pages))
		goto fail;
	return area->addr;

fail:
	warn_alloc(gfp_mask, NULL,
			  "vmalloc: allocation failure, allocated %ld of %ld bytes",
			  (area->nr_pages*PAGE_SIZE), area->size);
	__vfree(area->addr);
	return NULL;
}

/**
 * __vmalloc_node_range - allocate virtually contiguous memory
 * @size:		  allocation size
 * @align:		  desired alignment
 * @start:		  vm area range start
 * @end:		  vm area range end
 * @gfp_mask:		  flags for the page level allocator
 * @prot:		  protection mask for the allocated pages
 * @vm_flags:		  additional vm area flags (e.g. %VM_NO_GUARD)
 * @node:		  node to use for allocation or NUMA_NO_NODE
 * @caller:		  caller's return address
 *
 * Allocate enough pages to cover @size from the page level
 * allocator with @gfp_mask flags.  Map them into contiguous
 * kernel virtual space, using a pagetable protection of @prot.
 *
 * Return: the address of the area or %NULL on failure
 */
void *__vmalloc_node_range(unsigned long size, unsigned long align,
			unsigned long start, unsigned long end, gfp_t gfp_mask,
			pgprot_t prot, unsigned long vm_flags, int node,
			const void *caller)
{
	struct vm_struct *area;
	void *addr;
	unsigned long real_size = size;

	size = PAGE_ALIGN(size);
	if (!size || (size >> PAGE_SHIFT) > totalram_pages())
		goto fail;

	area = __get_vm_area_node(size, align, VM_ALLOC | VM_UNINITIALIZED |
				vm_flags, start, end, node, gfp_mask, caller);
	if (!area)
		goto fail;

	addr = __vmalloc_area_node(area, gfp_mask, prot, node);
	if (!addr)
		return NULL;

	/*
	 * In this function, newly allocated vm_struct has VM_UNINITIALIZED
	 * flag. It means that vm_struct is not fully initialized.
	 * Now, it is fully initialized, so remove this flag here.
	 */
	clear_vm_uninitialized_flag(area);

	kmemleak_vmalloc(area, size, gfp_mask);

	return addr;

fail:
	warn_alloc(gfp_mask, NULL,
			  "vmalloc: allocation failure: %lu bytes", real_size);
	return NULL;
}

/*
 * This is only for performance analysis of vmalloc and stress purpose.
 * It is required by vmalloc test module, therefore do not use it other
 * than that.
 */
#ifdef CONFIG_TEST_VMALLOC_MODULE
EXPORT_SYMBOL_GPL(__vmalloc_node_range);
#endif

/**
 * __vmalloc_node - allocate virtually contiguous memory
 * @size:	    allocation size
 * @align:	    desired alignment
 * @gfp_mask:	    flags for the page level allocator
 * @prot:	    protection mask for the allocated pages
 * @node:	    node to use for allocation or NUMA_NO_NODE
 * @caller:	    caller's return address
 *
 * Allocate enough pages to cover @size from the page level
 * allocator with @gfp_mask flags.  Map them into contiguous
 * kernel virtual space, using a pagetable protection of @prot.
 *
 * Reclaim modifiers in @gfp_mask - __GFP_NORETRY, __GFP_RETRY_MAYFAIL
 * and __GFP_NOFAIL are not supported
 *
 * Any use of gfp flags outside of GFP_KERNEL should be consulted
 * with mm people.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
static void *__vmalloc_node(unsigned long size, unsigned long align,
			    gfp_t gfp_mask, pgprot_t prot,
			    int node, const void *caller)
{
	return __vmalloc_node_range(size, align, VMALLOC_START, VMALLOC_END,
				gfp_mask, prot, 0, node, caller);
}

void *__vmalloc(unsigned long size, gfp_t gfp_mask, pgprot_t prot)
{
	return __vmalloc_node(size, 1, gfp_mask, prot, NUMA_NO_NODE,
				__builtin_return_address(0));
}
EXPORT_SYMBOL(__vmalloc);

static inline void *__vmalloc_node_flags(unsigned long size,
					int node, gfp_t flags)
{
	return __vmalloc_node(size, 1, flags, PAGE_KERNEL,
					node, __builtin_return_address(0));
}


void *__vmalloc_node_flags_caller(unsigned long size, int node, gfp_t flags,
				  void *caller)
{
	return __vmalloc_node(size, 1, flags, PAGE_KERNEL, node, caller);
}

/**
 * vmalloc - allocate virtually contiguous memory
 * @size:    allocation size
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc(unsigned long size)
{
	return __vmalloc_node_flags(size, NUMA_NO_NODE,
				    GFP_KERNEL);
}
EXPORT_SYMBOL(vmalloc);

/**
 * vzalloc - allocate virtually contiguous memory with zero fill
 * @size:    allocation size
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 * The memory allocated is set to zero.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vzalloc(unsigned long size)
{
	return __vmalloc_node_flags(size, NUMA_NO_NODE,
				GFP_KERNEL | __GFP_ZERO);
}
EXPORT_SYMBOL(vzalloc);

/**
 * vmalloc_user - allocate zeroed virtually contiguous memory for userspace
 * @size: allocation size
 *
 * The resulting memory area is zeroed so it can be mapped to userspace
 * without leaking data.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_user(unsigned long size)
{
	return __vmalloc_node_range(size, SHMLBA,  VMALLOC_START, VMALLOC_END,
				    GFP_KERNEL | __GFP_ZERO, PAGE_KERNEL,
				    VM_USERMAP, NUMA_NO_NODE,
				    __builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_user);

/**
 * vmalloc_node - allocate memory on a specific node
 * @size:	  allocation size
 * @node:	  numa node
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_node(unsigned long size, int node)
{
	return __vmalloc_node(size, 1, GFP_KERNEL, PAGE_KERNEL,
					node, __builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_node);

/**
 * vzalloc_node - allocate memory on a specific node with zero fill
 * @size:	allocation size
 * @node:	numa node
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 * The memory allocated is set to zero.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc_node() instead.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vzalloc_node(unsigned long size, int node)
{
	return __vmalloc_node_flags(size, node,
			 GFP_KERNEL | __GFP_ZERO);
}
EXPORT_SYMBOL(vzalloc_node);

/**
 * vmalloc_exec - allocate virtually contiguous, executable memory
 * @size:	  allocation size
 *
 * Kernel-internal function to allocate enough pages to cover @size
 * the page level allocator and map them into contiguous and
 * executable kernel virtual space.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_exec(unsigned long size)
{
	return __vmalloc_node_range(size, 1, VMALLOC_START, VMALLOC_END,
			GFP_KERNEL, PAGE_KERNEL_EXEC, VM_FLUSH_RESET_PERMS,
			NUMA_NO_NODE, __builtin_return_address(0));
}

#if defined(CONFIG_64BIT) && defined(CONFIG_ZONE_DMA32)
#define GFP_VMALLOC32 (GFP_DMA32 | GFP_KERNEL)
#elif defined(CONFIG_64BIT) && defined(CONFIG_ZONE_DMA)
#define GFP_VMALLOC32 (GFP_DMA | GFP_KERNEL)
#else
/*
 * 64b systems should always have either DMA or DMA32 zones. For others
 * GFP_DMA32 should do the right thing and use the normal zone.
 */
#define GFP_VMALLOC32 GFP_DMA32 | GFP_KERNEL
#endif

/**
 * vmalloc_32 - allocate virtually contiguous memory (32bit addressable)
 * @size:	allocation size
 *
 * Allocate enough 32bit PA addressable pages to cover @size from the
 * page level allocator and map them into contiguous kernel virtual space.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_32(unsigned long size)
{
	return __vmalloc_node(size, 1, GFP_VMALLOC32, PAGE_KERNEL,
			      NUMA_NO_NODE, __builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_32);

/**
 * vmalloc_32_user - allocate zeroed virtually contiguous 32bit memory
 * @size:	     allocation size
 *
 * The resulting memory area is 32bit addressable and zeroed so it can be
 * mapped to userspace without leaking data.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_32_user(unsigned long size)
{
	return __vmalloc_node_range(size, SHMLBA,  VMALLOC_START, VMALLOC_END,
				    GFP_VMALLOC32 | __GFP_ZERO, PAGE_KERNEL,
				    VM_USERMAP, NUMA_NO_NODE,
				    __builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_32_user);

/*
 * small helper routine , copy contents to buf from addr.
 * If the page is not present, fill zero.
 */

static int aligned_vread(char *buf, char *addr, unsigned long count)
{
	struct page *p;
	int copied = 0;

	while (count) {
		unsigned long offset, length;

		offset = offset_in_page(addr);
		length = PAGE_SIZE - offset;
		if (length > count)
			length = count;
		p = vmalloc_to_page(addr);
		/*
		 * To do safe access to this _mapped_ area, we need
		 * lock. But adding lock here means that we need to add
		 * overhead of vmalloc()/vfree() calles for this _debug_
		 * interface, rarely used. Instead of that, we'll use
		 * kmap() and get small overhead in this access function.
		 */
		if (p) {
			/*
			 * we can expect USER0 is not used (see vread/vwrite's
			 * function description)
			 */
			void *map = kmap_atomic(p);
			memcpy(buf, map + offset, length);
			kunmap_atomic(map);
		} else
			memset(buf, 0, length);

		addr += length;
		buf += length;
		copied += length;
		count -= length;
	}
	return copied;
}

static int aligned_vwrite(char *buf, char *addr, unsigned long count)
{
	struct page *p;
	int copied = 0;

	while (count) {
		unsigned long offset, length;

		offset = offset_in_page(addr);
		length = PAGE_SIZE - offset;
		if (length > count)
			length = count;
		p = vmalloc_to_page(addr);
		/*
		 * To do safe access to this _mapped_ area, we need
		 * lock. But adding lock here means that we need to add
		 * overhead of vmalloc()/vfree() calles for this _debug_
		 * interface, rarely used. Instead of that, we'll use
		 * kmap() and get small overhead in this access function.
		 */
		if (p) {
			/*
			 * we can expect USER0 is not used (see vread/vwrite's
			 * function description)
			 */
			void *map = kmap_atomic(p);
			memcpy(map + offset, buf, length);
			kunmap_atomic(map);
		}
		addr += length;
		buf += length;
		copied += length;
		count -= length;
	}
	return copied;
}

/**
 * vread() - read vmalloc area in a safe way.
 * @buf:     buffer for reading data
 * @addr:    vm address.
 * @count:   number of bytes to be read.
 *
 * This function checks that addr is a valid vmalloc'ed area, and
 * copy data from that area to a given buffer. If the given memory range
 * of [addr...addr+count) includes some valid address, data is copied to
 * proper area of @buf. If there are memory holes, they'll be zero-filled.
 * IOREMAP area is treated as memory hole and no copy is done.
 *
 * If [addr...addr+count) doesn't includes any intersects with alive
 * vm_struct area, returns 0. @buf should be kernel's buffer.
 *
 * Note: In usual ops, vread() is never necessary because the caller
 * should know vmalloc() area is valid and can use memcpy().
 * This is for routines which have to access vmalloc area without
 * any information, as /dev/kmem.
 *
 * Return: number of bytes for which addr and buf should be increased
 * (same number as @count) or %0 if [addr...addr+count) doesn't
 * include any intersection with valid vmalloc area
 */
long vread(char *buf, char *addr, unsigned long count)
{
	struct vmap_area *va;
	struct vm_struct *vm;
	char *vaddr, *buf_start = buf;
	unsigned long buflen = count;
	unsigned long n;

	/* Don't allow overflow */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;

	spin_lock(&vmap_area_lock);
	list_for_each_entry(va, &vmap_area_list, list) {
		if (!count)
			break;

		if (!va->vm)
			continue;

		vm = va->vm;
		vaddr = (char *) vm->addr;
		if (addr >= vaddr + get_vm_area_size(vm))
			continue;
		while (addr < vaddr) {
			if (count == 0)
				goto finished;
			*buf = '\0';
			buf++;
			addr++;
			count--;
		}
		n = vaddr + get_vm_area_size(vm) - addr;
		if (n > count)
			n = count;
		if (!(vm->flags & VM_IOREMAP))
			aligned_vread(buf, addr, n);
		else /* IOREMAP area is treated as memory hole */
			memset(buf, 0, n);
		buf += n;
		addr += n;
		count -= n;
	}
finished:
	spin_unlock(&vmap_area_lock);

	if (buf == buf_start)
		return 0;
	/* zero-fill memory holes */
	if (buf != buf_start + buflen)
		memset(buf, 0, buflen - (buf - buf_start));

	return buflen;
}

/**
 * vwrite() - write vmalloc area in a safe way.
 * @buf:      buffer for source data
 * @addr:     vm address.
 * @count:    number of bytes to be read.
 *
 * This function checks that addr is a valid vmalloc'ed area, and
 * copy data from a buffer to the given addr. If specified range of
 * [addr...addr+count) includes some valid address, data is copied from
 * proper area of @buf. If there are memory holes, no copy to hole.
 * IOREMAP area is treated as memory hole and no copy is done.
 *
 * If [addr...addr+count) doesn't includes any intersects with alive
 * vm_struct area, returns 0. @buf should be kernel's buffer.
 *
 * Note: In usual ops, vwrite() is never necessary because the caller
 * should know vmalloc() area is valid and can use memcpy().
 * This is for routines which have to access vmalloc area without
 * any information, as /dev/kmem.
 *
 * Return: number of bytes for which addr and buf should be
 * increased (same number as @count) or %0 if [addr...addr+count)
 * doesn't include any intersection with valid vmalloc area
 */
long vwrite(char *buf, char *addr, unsigned long count)
{
	struct vmap_area *va;
	struct vm_struct *vm;
	char *vaddr;
	unsigned long n, buflen;
	int copied = 0;

	/* Don't allow overflow */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;
	buflen = count;

	spin_lock(&vmap_area_lock);
	list_for_each_entry(va, &vmap_area_list, list) {
		if (!count)
			break;

		if (!va->vm)
			continue;

		vm = va->vm;
		vaddr = (char *) vm->addr;
		if (addr >= vaddr + get_vm_area_size(vm))
			continue;
		while (addr < vaddr) {
			if (count == 0)
				goto finished;
			buf++;
			addr++;
			count--;
		}
		n = vaddr + get_vm_area_size(vm) - addr;
		if (n > count)
			n = count;
		if (!(vm->flags & VM_IOREMAP)) {
			aligned_vwrite(buf, addr, n);
			copied++;
		}
		buf += n;
		addr += n;
		count -= n;
	}
finished:
	spin_unlock(&vmap_area_lock);
	if (!copied)
		return 0;
	return buflen;
}

/**
 * remap_vmalloc_range_partial - map vmalloc pages to userspace
 * @vma:		vma to cover
 * @uaddr:		target user address to start at
 * @kaddr:		virtual address of vmalloc kernel memory
 * @pgoff:		offset from @kaddr to start at
 * @size:		size of map area
 *
 * Returns:	0 for success, -Exxx on failure
 *
 * This function checks that @kaddr is a valid vmalloc'ed area,
 * and that it is big enough to cover the range starting at
 * @uaddr in @vma. Will return failure if that criteria isn't
 * met.
 *
 * Similar to remap_pfn_range() (see mm/memory.c)
 */
int remap_vmalloc_range_partial(struct vm_area_struct *vma, unsigned long uaddr,
				void *kaddr, unsigned long pgoff,
				unsigned long size)
{
	struct vm_struct *area;
	unsigned long off;
	unsigned long end_index;

	if (check_shl_overflow(pgoff, PAGE_SHIFT, &off))
		return -EINVAL;

	size = PAGE_ALIGN(size);

	if (!PAGE_ALIGNED(uaddr) || !PAGE_ALIGNED(kaddr))
		return -EINVAL;

	area = find_vm_area(kaddr);
	if (!area)
		return -EINVAL;

	if (!(area->flags & (VM_USERMAP | VM_DMA_COHERENT)))
		return -EINVAL;

	if (check_add_overflow(size, off, &end_index) ||
	    end_index > get_vm_area_size(area))
		return -EINVAL;
	kaddr += off;

	do {
		struct page *page = vmalloc_to_page(kaddr);
		int ret;

		ret = vm_insert_page(vma, uaddr, page);
		if (ret)
			return ret;

		uaddr += PAGE_SIZE;
		kaddr += PAGE_SIZE;
		size -= PAGE_SIZE;
	} while (size > 0);

	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;

	return 0;
}
EXPORT_SYMBOL(remap_vmalloc_range_partial);

/**
 * remap_vmalloc_range - map vmalloc pages to userspace
 * @vma:		vma to cover (map full range of vma)
 * @addr:		vmalloc memory
 * @pgoff:		number of pages into addr before first page to map
 *
 * Returns:	0 for success, -Exxx on failure
 *
 * This function checks that addr is a valid vmalloc'ed area, and
 * that it is big enough to cover the vma. Will return failure if
 * that criteria isn't met.
 *
 * Similar to remap_pfn_range() (see mm/memory.c)
 */
int remap_vmalloc_range(struct vm_area_struct *vma, void *addr,
						unsigned long pgoff)
{
	return remap_vmalloc_range_partial(vma, vma->vm_start,
					   addr, pgoff,
					   vma->vm_end - vma->vm_start);
}
EXPORT_SYMBOL(remap_vmalloc_range);

/*
 * Implement stubs for vmalloc_sync_[un]mappings () if the architecture chose
 * not to have one.
 *
 * The purpose of this function is to make sure the vmalloc area
 * mappings are identical in all page-tables in the system.
 */
void __weak vmalloc_sync_mappings(void)
{
}

void __weak vmalloc_sync_unmappings(void)
{
}

static int f(pte_t *pte, unsigned long addr, void *data)
{
	pte_t ***p = data;

	if (p) {
		*(*p) = pte;
		(*p)++;
	}
	return 0;
}

/**
 * alloc_vm_area - allocate a range of kernel address space
 * @size:	   size of the area
 * @ptes:	   returns the PTEs for the address space
 *
 * Returns:	NULL on failure, vm_struct on success
 *
 * This function reserves a range of kernel address space, and
 * allocates pagetables to map that range.  No actual mappings
 * are created.
 *
 * If @ptes is non-NULL, pointers to the PTEs (in init_mm)
 * allocated for the VM area are returned.
 */
struct vm_struct *alloc_vm_area(size_t size, pte_t **ptes)
{
	struct vm_struct *area;

	area = get_vm_area_caller(size, VM_IOREMAP,
				__builtin_return_address(0));
	if (area == NULL)
		return NULL;

	/*
	 * This ensures that page tables are constructed for this region
	 * of kernel virtual address space and mapped into init_mm.
	 */
	if (apply_to_page_range(&init_mm, (unsigned long)area->addr,
				size, f, ptes ? &ptes : NULL)) {
		free_vm_area(area);
		return NULL;
	}

	return area;
}
EXPORT_SYMBOL_GPL(alloc_vm_area);

void free_vm_area(struct vm_struct *area)
{
	struct vm_struct *ret;
	ret = remove_vm_area(area->addr);
	BUG_ON(ret != area);
	kfree(area);
}
EXPORT_SYMBOL_GPL(free_vm_area);

#ifdef CONFIG_SMP
static struct vmap_area *node_to_va(struct rb_node *n)
{
	return rb_entry_safe(n, struct vmap_area, rb_node);
}

/**
 * pvm_find_va_enclose_addr - find the vmap_area @addr belongs to
 * @addr: target address
 *
 * Returns: vmap_area if it is found. If there is no such area
 *   the first highest(reverse order) vmap_area is returned
 *   i.e. va->va_start < addr && va->va_end < addr or NULL
 *   if there are no any areas before @addr.
 */
static struct vmap_area *
pvm_find_va_enclose_addr(unsigned long addr)
{
	struct vmap_area *va, *tmp;
	struct rb_node *n;

	n = free_vmap_area_root.rb_node;
	va = NULL;

	while (n) {
		tmp = rb_entry(n, struct vmap_area, rb_node);
		if (tmp->va_start <= addr) {
			va = tmp;
			if (tmp->va_end >= addr)
				break;

			n = n->rb_right;
		} else {
			n = n->rb_left;
		}
	}

	return va;
}

/**
 * pvm_determine_end_from_reverse - find the highest aligned address
 * of free block below VMALLOC_END
 * @va:
 *   in - the VA we start the search(reverse order);
 *   out - the VA with the highest aligned end address.
 *
 * Returns: determined end address within vmap_area
 */
static unsigned long
pvm_determine_end_from_reverse(struct vmap_area **va, unsigned long align)
{
	unsigned long vmalloc_end = VMALLOC_END & ~(align - 1);
	unsigned long addr;

	if (likely(*va)) {
		list_for_each_entry_from_reverse((*va),
				&free_vmap_area_list, list) {
			addr = min((*va)->va_end & ~(align - 1), vmalloc_end);
			if ((*va)->va_start < addr)
				return addr;
		}
	}

	return 0;
}

/**
 * pcpu_get_vm_areas - allocate vmalloc areas for percpu allocator
 * @offsets: array containing offset of each area
 * @sizes: array containing size of each area
 * @nr_vms: the number of areas to allocate
 * @align: alignment, all entries in @offsets and @sizes must be aligned to this
 *
 * Returns: kmalloc'd vm_struct pointer array pointing to allocated
 *	    vm_structs on success, %NULL on failure
 *
 * Percpu allocator wants to use congruent vm areas so that it can
 * maintain the offsets among percpu areas.  This function allocates
 * congruent vmalloc areas for it with GFP_KERNEL.  These areas tend to
 * be scattered pretty far, distance between two areas easily going up
 * to gigabytes.  To avoid interacting with regular vmallocs, these
 * areas are allocated from top.
 *
 * Despite its complicated look, this allocator is rather simple. It
 * does everything top-down and scans free blocks from the end looking
 * for matching base. While scanning, if any of the areas do not fit the
 * base address is pulled down to fit the area. Scanning is repeated till
 * all the areas fit and then all necessary data structures are inserted
 * and the result is returned.
 */
struct vm_struct **pcpu_get_vm_areas(const unsigned long *offsets,
				     const size_t *sizes, int nr_vms,
				     size_t align)
{
	const unsigned long vmalloc_start = ALIGN(VMALLOC_START, align);
	const unsigned long vmalloc_end = VMALLOC_END & ~(align - 1);
	struct vmap_area **vas, *va;
	struct vm_struct **vms;
	int area, area2, last_area, term_area;
	unsigned long base, start, size, end, last_end;
	bool purged = false;
	enum fit_type type;

	/* verify parameters and allocate data structures */
	BUG_ON(offset_in_page(align) || !is_power_of_2(align));
	for (last_area = 0, area = 0; area < nr_vms; area++) {
		start = offsets[area];
		end = start + sizes[area];

		/* is everything aligned properly? */
		BUG_ON(!IS_ALIGNED(offsets[area], align));
		BUG_ON(!IS_ALIGNED(sizes[area], align));

		/* detect the area with the highest address */
		if (start > offsets[last_area])
			last_area = area;

		for (area2 = area + 1; area2 < nr_vms; area2++) {
			unsigned long start2 = offsets[area2];
			unsigned long end2 = start2 + sizes[area2];

			BUG_ON(start2 < end && start < end2);
		}
	}
	last_end = offsets[last_area] + sizes[last_area];

	if (vmalloc_end - vmalloc_start < last_end) {
		WARN_ON(true);
		return NULL;
	}

	vms = kcalloc(nr_vms, sizeof(vms[0]), GFP_KERNEL);
	vas = kcalloc(nr_vms, sizeof(vas[0]), GFP_KERNEL);
	if (!vas || !vms)
		goto err_free2;

	for (area = 0; area < nr_vms; area++) {
		vas[area] = kmem_cache_zalloc(vmap_area_cachep, GFP_KERNEL);
		vms[area] = kzalloc(sizeof(struct vm_struct), GFP_KERNEL);
		if (!vas[area] || !vms[area])
			goto err_free;
	}
retry:
	spin_lock(&vmap_area_lock);

	/* start scanning - we scan from the top, begin with the last area */
	area = term_area = last_area;
	start = offsets[area];
	end = start + sizes[area];

	va = pvm_find_va_enclose_addr(vmalloc_end);
	base = pvm_determine_end_from_reverse(&va, align) - end;

	while (true) {
		/*
		 * base might have underflowed, add last_end before
		 * comparing.
		 */
		if (base + last_end < vmalloc_start + last_end)
			goto overflow;

		/*
		 * Fitting base has not been found.
		 */
		if (va == NULL)
			goto overflow;

		/*
		 * If required width exeeds current VA block, move
		 * base downwards and then recheck.
		 */
		if (base + end > va->va_end) {
			base = pvm_determine_end_from_reverse(&va, align) - end;
			term_area = area;
			continue;
		}

		/*
		 * If this VA does not fit, move base downwards and recheck.
		 */
		if (base + start < va->va_start) {
			va = node_to_va(rb_prev(&va->rb_node));
			base = pvm_determine_end_from_reverse(&va, align) - end;
			term_area = area;
			continue;
		}

		/*
		 * This area fits, move on to the previous one.  If
		 * the previous one is the terminal one, we're done.
		 */
		area = (area + nr_vms - 1) % nr_vms;
		if (area == term_area)
			break;

		start = offsets[area];
		end = start + sizes[area];
		va = pvm_find_va_enclose_addr(base + end);
	}

	/* we've found a fitting base, insert all va's */
	for (area = 0; area < nr_vms; area++) {
		int ret;

		start = base + offsets[area];
		size = sizes[area];

		va = pvm_find_va_enclose_addr(start);
		if (WARN_ON_ONCE(va == NULL))
			/* It is a BUG(), but trigger recovery instead. */
			goto recovery;

		type = classify_va_fit_type(va, start, size);
		if (WARN_ON_ONCE(type == NOTHING_FIT))
			/* It is a BUG(), but trigger recovery instead. */
			goto recovery;

		ret = adjust_va_to_fit_type(va, start, size, type);
		if (unlikely(ret))
			goto recovery;

		/* Allocated area. */
		va = vas[area];
		va->va_start = start;
		va->va_end = start + size;

		insert_vmap_area(va, &vmap_area_root, &vmap_area_list);
	}

	spin_unlock(&vmap_area_lock);

	/* insert all vm's */
	for (area = 0; area < nr_vms; area++)
		setup_vmalloc_vm(vms[area], vas[area], VM_ALLOC,
				 pcpu_get_vm_areas);

	kfree(vas);
	return vms;

recovery:
	/* Remove previously inserted areas. */
	while (area--) {
		__free_vmap_area(vas[area]);
		vas[area] = NULL;
	}

overflow:
	spin_unlock(&vmap_area_lock);
	if (!purged) {
		purge_vmap_area_lazy();
		purged = true;

		/* Before "retry", check if we recover. */
		for (area = 0; area < nr_vms; area++) {
			if (vas[area])
				continue;

			vas[area] = kmem_cache_zalloc(
				vmap_area_cachep, GFP_KERNEL);
			if (!vas[area])
				goto err_free;
		}

		goto retry;
	}

err_free:
	for (area = 0; area < nr_vms; area++) {
		if (vas[area])
			kmem_cache_free(vmap_area_cachep, vas[area]);

		kfree(vms[area]);
	}
err_free2:
	kfree(vas);
	kfree(vms);
	return NULL;
}

/**
 * pcpu_free_vm_areas - free vmalloc areas for percpu allocator
 * @vms: vm_struct pointer array returned by pcpu_get_vm_areas()
 * @nr_vms: the number of allocated areas
 *
 * Free vm_structs and the array allocated by pcpu_get_vm_areas().
 */
void pcpu_free_vm_areas(struct vm_struct **vms, int nr_vms)
{
	int i;

	for (i = 0; i < nr_vms; i++)
		free_vm_area(vms[i]);
	kfree(vms);
}
#endif	/* CONFIG_SMP */

#ifdef CONFIG_PROC_FS
static void *s_start(struct seq_file *m, loff_t *pos)
	__acquires(&vmap_area_lock)
{
	spin_lock(&vmap_area_lock);
	return seq_list_start(&vmap_area_list, *pos);
}

static void *s_next(struct seq_file *m, void *p, loff_t *pos)
{
	return seq_list_next(p, &vmap_area_list, pos);
}

static void s_stop(struct seq_file *m, void *p)
	__releases(&vmap_area_lock)
{
	spin_unlock(&vmap_area_lock);
}

static void show_numa_info(struct seq_file *m, struct vm_struct *v)
{
	if (IS_ENABLED(CONFIG_NUMA)) {
		unsigned int nr, *counters = m->private;

		if (!counters)
			return;

		if (v->flags & VM_UNINITIALIZED)
			return;
		/* Pair with smp_wmb() in clear_vm_uninitialized_flag() */
		smp_rmb();

		memset(counters, 0, nr_node_ids * sizeof(unsigned int));

		for (nr = 0; nr < v->nr_pages; nr++)
			counters[page_to_nid(v->pages[nr])]++;

		for_each_node_state(nr, N_HIGH_MEMORY)
			if (counters[nr])
				seq_printf(m, " N%u=%u", nr, counters[nr]);
	}
}

static void show_purge_info(struct seq_file *m)
{
	struct llist_node *head;
	struct vmap_area *va;

	head = READ_ONCE(vmap_purge_list.first);
	if (head == NULL)
		return;

	llist_for_each_entry(va, head, purge_list) {
		seq_printf(m, "0x%pK-0x%pK %7ld unpurged vm_area\n",
			(void *)va->va_start, (void *)va->va_end,
			va->va_end - va->va_start);
	}
}

static int s_show(struct seq_file *m, void *p)
{
	struct vmap_area *va;
	struct vm_struct *v;

	va = list_entry(p, struct vmap_area, list);

	/*
	 * s_show can encounter race with remove_vm_area, !vm on behalf
	 * of vmap area is being tear down or vm_map_ram allocation.
	 */
	if (!va->vm) {
		seq_printf(m, "0x%pK-0x%pK %7ld vm_map_ram\n",
			(void *)va->va_start, (void *)va->va_end,
			va->va_end - va->va_start);

		return 0;
	}

	v = va->vm;

	seq_printf(m, "0x%pK-0x%pK %7ld",
		v->addr, v->addr + v->size, v->size);

	if (v->caller)
		seq_printf(m, " %pS", v->caller);

	if (v->nr_pages)
		seq_printf(m, " pages=%d", v->nr_pages);

	if (v->phys_addr)
		seq_printf(m, " phys=%pa", &v->phys_addr);

	if (v->flags & VM_IOREMAP)
		seq_puts(m, " ioremap");

	if (v->flags & VM_ALLOC)
		seq_puts(m, " vmalloc");

	if (v->flags & VM_MAP)
		seq_puts(m, " vmap");

	if (v->flags & VM_USERMAP)
		seq_puts(m, " user");

	if (v->flags & VM_DMA_COHERENT)
		seq_puts(m, " dma-coherent");

	if (is_vmalloc_addr(v->pages))
		seq_puts(m, " vpages");

	show_numa_info(m, v);
	seq_putc(m, '\n');

	/*
	 * As a final step, dump "unpurged" areas. Note,
	 * that entire "/proc/vmallocinfo" output will not
	 * be address sorted, because the purge list is not
	 * sorted.
	 */
	if (list_is_last(&va->list, &vmap_area_list))
		show_purge_info(m);

	return 0;
}

static const struct seq_operations vmalloc_op = {
	.start = s_start,
	.next = s_next,
	.stop = s_stop,
	.show = s_show,
};

static int __init proc_vmalloc_init(void)
{
	if (IS_ENABLED(CONFIG_NUMA))
		proc_create_seq_private("vmallocinfo", 0400, NULL,
				&vmalloc_op,
				nr_node_ids * sizeof(unsigned int), NULL);
	else
		proc_create_seq("vmallocinfo", 0400, NULL, &vmalloc_op);
	return 0;
}
module_init(proc_vmalloc_init);

#endif
