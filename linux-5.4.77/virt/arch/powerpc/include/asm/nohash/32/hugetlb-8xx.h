/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_NOHASH_32_HUGETLB_8XX_H
#define _ASM_POWERPC_NOHASH_32_HUGETLB_8XX_H

#define PAGE_SHIFT_8M		23

static inline pte_t *hugepd_page(hugepd_t hpd)
{
	BUG_ON(!hugepd_ok(hpd));

	return (pte_t *)__va(hpd_val(hpd) & ~HUGEPD_SHIFT_MASK);
}

static inline unsigned int hugepd_shift(hugepd_t hpd)
{
	return ((hpd_val(hpd) & _PMD_PAGE_MASK) >> 1) + 17;
}

static inline pte_t *hugepte_offset(hugepd_t hpd, unsigned long addr,
				    unsigned int pdshift)
{
	unsigned long idx = (addr & ((1UL << pdshift) - 1)) >> PAGE_SHIFT;

	return hugepd_page(hpd) + idx;
}

static inline void flush_hugetlb_page(struct vm_area_struct *vma,
				      unsigned long vmaddr)
{
	flush_tlb_page(vma, vmaddr);
}

static inline void hugepd_populate(hugepd_t *hpdp, pte_t *new, unsigned int pshift)
{
	*hpdp = __hugepd(__pa(new) | _PMD_USER | _PMD_PRESENT |
			 (pshift == PAGE_SHIFT_8M ? _PMD_PAGE_8M : _PMD_PAGE_512K));
}

static inline int check_and_get_huge_psize(int shift)
{
	return shift_to_mmu_psize(shift);
}

#endif /* _ASM_POWERPC_NOHASH_32_HUGETLB_8XX_H */
