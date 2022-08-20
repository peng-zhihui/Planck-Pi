// SPDX-License-Identifier: GPL-2.0
/*
 * Re-map IO memory to kernel address space so that we can access it.
 * This is needed for high PCI addresses that aren't mapped in the
 * 640k-1MB IO memory area on PC's
 *
 * (C) Copyright 1995 1996 Linus Torvalds
 */
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/io.h>
#include <linux/export.h>
#include <asm/cacheflush.h>
#include <asm/pgtable.h>

#ifdef CONFIG_HAVE_ARCH_HUGE_VMAP
static int __read_mostly ioremap_p4d_capable;
static int __read_mostly ioremap_pud_capable;
static int __read_mostly ioremap_pmd_capable;
static int __read_mostly ioremap_huge_disabled;

static int __init set_nohugeiomap(char *str)
{
	ioremap_huge_disabled = 1;
	return 0;
}
early_param("nohugeiomap", set_nohugeiomap);

void __init ioremap_huge_init(void)
{
	if (!ioremap_huge_disabled) {
		if (arch_ioremap_p4d_supported())
			ioremap_p4d_capable = 1;
		if (arch_ioremap_pud_supported())
			ioremap_pud_capable = 1;
		if (arch_ioremap_pmd_supported())
			ioremap_pmd_capable = 1;
	}
}

static inline int ioremap_p4d_enabled(void)
{
	return ioremap_p4d_capable;
}

static inline int ioremap_pud_enabled(void)
{
	return ioremap_pud_capable;
}

static inline int ioremap_pmd_enabled(void)
{
	return ioremap_pmd_capable;
}

#else	/* !CONFIG_HAVE_ARCH_HUGE_VMAP */
static inline int ioremap_p4d_enabled(void) { return 0; }
static inline int ioremap_pud_enabled(void) { return 0; }
static inline int ioremap_pmd_enabled(void) { return 0; }
#endif	/* CONFIG_HAVE_ARCH_HUGE_VMAP */

static int ioremap_pte_range(pmd_t *pmd, unsigned long addr,
		unsigned long end, phys_addr_t phys_addr, pgprot_t prot)
{
	pte_t *pte;
	u64 pfn;

	pfn = phys_addr >> PAGE_SHIFT;
	pte = pte_alloc_kernel(pmd, addr);
	if (!pte)
		return -ENOMEM;
	do {
		BUG_ON(!pte_none(*pte));
		set_pte_at(&init_mm, addr, pte, pfn_pte(pfn, prot));
		pfn++;
	} while (pte++, addr += PAGE_SIZE, addr != end);
	return 0;
}

static int ioremap_try_huge_pmd(pmd_t *pmd, unsigned long addr,
				unsigned long end, phys_addr_t phys_addr,
				pgprot_t prot)
{
	if (!ioremap_pmd_enabled())
		return 0;

	if ((end - addr) != PMD_SIZE)
		return 0;

	if (!IS_ALIGNED(addr, PMD_SIZE))
		return 0;

	if (!IS_ALIGNED(phys_addr, PMD_SIZE))
		return 0;

	if (pmd_present(*pmd) && !pmd_free_pte_page(pmd, addr))
		return 0;

	return pmd_set_huge(pmd, phys_addr, prot);
}

static inline int ioremap_pmd_range(pud_t *pud, unsigned long addr,
		unsigned long end, phys_addr_t phys_addr, pgprot_t prot)
{
	pmd_t *pmd;
	unsigned long next;

	pmd = pmd_alloc(&init_mm, pud, addr);
	if (!pmd)
		return -ENOMEM;
	do {
		next = pmd_addr_end(addr, end);

		if (ioremap_try_huge_pmd(pmd, addr, next, phys_addr, prot))
			continue;

		if (ioremap_pte_range(pmd, addr, next, phys_addr, prot))
			return -ENOMEM;
	} while (pmd++, phys_addr += (next - addr), addr = next, addr != end);
	return 0;
}

static int ioremap_try_huge_pud(pud_t *pud, unsigned long addr,
				unsigned long end, phys_addr_t phys_addr,
				pgprot_t prot)
{
	if (!ioremap_pud_enabled())
		return 0;

	if ((end - addr) != PUD_SIZE)
		return 0;

	if (!IS_ALIGNED(addr, PUD_SIZE))
		return 0;

	if (!IS_ALIGNED(phys_addr, PUD_SIZE))
		return 0;

	if (pud_present(*pud) && !pud_free_pmd_page(pud, addr))
		return 0;

	return pud_set_huge(pud, phys_addr, prot);
}

static inline int ioremap_pud_range(p4d_t *p4d, unsigned long addr,
		unsigned long end, phys_addr_t phys_addr, pgprot_t prot)
{
	pud_t *pud;
	unsigned long next;

	pud = pud_alloc(&init_mm, p4d, addr);
	if (!pud)
		return -ENOMEM;
	do {
		next = pud_addr_end(addr, end);

		if (ioremap_try_huge_pud(pud, addr, next, phys_addr, prot))
			continue;

		if (ioremap_pmd_range(pud, addr, next, phys_addr, prot))
			return -ENOMEM;
	} while (pud++, phys_addr += (next - addr), addr = next, addr != end);
	return 0;
}

static int ioremap_try_huge_p4d(p4d_t *p4d, unsigned long addr,
				unsigned long end, phys_addr_t phys_addr,
				pgprot_t prot)
{
	if (!ioremap_p4d_enabled())
		return 0;

	if ((end - addr) != P4D_SIZE)
		return 0;

	if (!IS_ALIGNED(addr, P4D_SIZE))
		return 0;

	if (!IS_ALIGNED(phys_addr, P4D_SIZE))
		return 0;

	if (p4d_present(*p4d) && !p4d_free_pud_page(p4d, addr))
		return 0;

	return p4d_set_huge(p4d, phys_addr, prot);
}

static inline int ioremap_p4d_range(pgd_t *pgd, unsigned long addr,
		unsigned long end, phys_addr_t phys_addr, pgprot_t prot)
{
	p4d_t *p4d;
	unsigned long next;

	p4d = p4d_alloc(&init_mm, pgd, addr);
	if (!p4d)
		return -ENOMEM;
	do {
		next = p4d_addr_end(addr, end);

		if (ioremap_try_huge_p4d(p4d, addr, next, phys_addr, prot))
			continue;

		if (ioremap_pud_range(p4d, addr, next, phys_addr, prot))
			return -ENOMEM;
	} while (p4d++, phys_addr += (next - addr), addr = next, addr != end);
	return 0;
}

int ioremap_page_range(unsigned long addr,
		       unsigned long end, phys_addr_t phys_addr, pgprot_t prot)
{
	pgd_t *pgd;
	unsigned long start;
	unsigned long next;
	int err;

	might_sleep();
	BUG_ON(addr >= end);

	start = addr;
	pgd = pgd_offset_k(addr);
	do {
		next = pgd_addr_end(addr, end);
		err = ioremap_p4d_range(pgd, addr, next, phys_addr, prot);
		if (err)
			break;
	} while (pgd++, phys_addr += (next - addr), addr = next, addr != end);

	flush_cache_vmap(start, end);

	return err;
}
