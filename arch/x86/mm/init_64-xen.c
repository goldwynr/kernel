/*
 *  linux/arch/x86_64/mm/init.c
 *
 *  Copyright (C) 1995  Linus Torvalds
 *  Copyright (C) 2000  Pavel Machek <pavel@ucw.cz>
 *  Copyright (C) 2002,2003 Andi Kleen <ak@suse.de>
 *
 *  Jun Nakajima <jun.nakajima@intel.com>
 *	Modified for Xen.
 */

#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/smp.h>
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/pagemap.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>
#include <linux/proc_fs.h>
#include <linux/pci.h>
#include <linux/pfn.h>
#include <linux/poison.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/memory.h>
#include <linux/memory_hotplug.h>
#include <linux/nmi.h>
#include <linux/gfp.h>
#include <linux/kcore.h>

#include <asm/processor.h>
#include <asm/bios_ebda.h>
#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/dma.h>
#include <asm/fixmap.h>
#include <asm/e820.h>
#include <asm/apic.h>
#include <asm/tlb.h>
#include <asm/mmu_context.h>
#include <asm/proto.h>
#include <asm/smp.h>
#include <asm/sections.h>
#include <asm/kdebug.h>
#include <asm/numa.h>
#include <asm/cacheflush.h>
#include <asm/init.h>
#include <asm/setup.h>

#include <xen/features.h>

#include "mm_internal.h"

#if CONFIG_XEN_COMPAT <= 0x030002
unsigned int __kernel_page_user;
EXPORT_SYMBOL(__kernel_page_user);
#endif

extern pmd_t level2_fixmap_pgt[PTRS_PER_PMD];
extern pte_t level1_fixmap_pgt[PTRS_PER_PTE];

/*
 * Use this until direct mapping is established, i.e. before __va() is 
 * available in init_memory_mapping().
 */

#define addr_to_page(addr, page)				\
	(addr) &= PHYSICAL_PAGE_MASK;				\
	(page) = ((unsigned long *) ((unsigned long)		\
	(((mfn_to_pfn((addr) >> PAGE_SHIFT)) << PAGE_SHIFT) +	\
	__START_KERNEL_map)))

pmd_t *__init early_get_pmd(unsigned long va)
{
	unsigned long addr;
	unsigned long *page = (unsigned long *)init_level4_pgt;

	addr = page[pgd_index(va)];
	addr_to_page(addr, page);

	addr = page[pud_index(va)];
	addr_to_page(addr, page);

	return (pmd_t *)&page[pmd_index(va)];
}

void __init early_make_page_readonly(void *va, unsigned int feature)
{
	unsigned long addr, _va = (unsigned long)va;
	pte_t pte, *ptep;
	unsigned long *page = (unsigned long *) init_level4_pgt;

	BUG_ON(after_bootmem);

	if (xen_feature(feature))
		return;

	addr = (unsigned long) page[pgd_index(_va)];
	addr_to_page(addr, page);

	addr = page[pud_index(_va)];
	addr_to_page(addr, page);

	addr = page[pmd_index(_va)];
	addr_to_page(addr, page);

	ptep = (pte_t *) &page[pte_index(_va)];

	pte.pte = ptep->pte & ~_PAGE_RW;
	if (HYPERVISOR_update_va_mapping(_va, pte, 0))
		BUG();
}

unsigned long __init early_arbitrary_virt_to_mfn(void *v)
{
	unsigned long va = (unsigned long)v, addr, *page;

	BUG_ON(va < __START_KERNEL_map);

	page = (void *)(xen_read_cr3() + __START_KERNEL_map);

	addr = page[pgd_index(va)];
	addr_to_page(addr, page);

	addr = page[pud_index(va)];
	addr_to_page(addr, page);

	addr = page[pmd_index(va)];
	addr_to_page(addr, page);

	return (page[pte_index(va)] & PHYSICAL_PAGE_MASK) >> PAGE_SHIFT;
}

static void ident_pmd_init(unsigned long pmd_flag, pmd_t *pmd_page,
			   unsigned long addr, unsigned long end)
{
	addr &= PMD_MASK;
	for (; addr < end; addr += PMD_SIZE) {
		pmd_t *pmd = pmd_page + pmd_index(addr);

		if (!pmd_present(*pmd))
			*pmd = __pmd_ma(addr | pmd_flag);
	}
}
static int ident_pud_init(struct x86_mapping_info *info, pud_t *pud_page,
			  unsigned long addr, unsigned long end)
{
	unsigned long next;

	for (; addr < end; addr = next) {
		pud_t *pud = pud_page + pud_index(addr);
		pmd_t *pmd;

		next = (addr & PUD_MASK) + PUD_SIZE;
		if (next > end)
			next = end;

		if (pud_present(*pud)) {
			pmd = pmd_offset(pud, 0);
			ident_pmd_init(info->pmd_flag, pmd, addr, next);
			continue;
		}
		pmd = (pmd_t *)info->alloc_pgt_page(info->context);
		if (!pmd)
			return -ENOMEM;
		ident_pmd_init(info->pmd_flag, pmd, addr, next);
		*pud = __pud(__pa(pmd) | _KERNPG_TABLE);
	}

	return 0;
}

int kernel_ident_mapping_init(struct x86_mapping_info *info, pgd_t *pgd_page,
			      unsigned long addr, unsigned long end)
{
	unsigned long next;
	int result;
	int off = info->kernel_mapping ? pgd_index(__PAGE_OFFSET) : 0;

	for (; addr < end; addr = next) {
		pgd_t *pgd = pgd_page + pgd_index(addr) + off;
		pud_t *pud;

		next = (addr & PGDIR_MASK) + PGDIR_SIZE;
		if (next > end)
			next = end;

		if (pgd_present(*pgd)) {
			pud = pud_offset(pgd, 0);
			result = ident_pud_init(info, pud, addr, next);
			if (result)
				return result;
			continue;
		}

		pud = (pud_t *)info->alloc_pgt_page(info->context);
		if (!pud)
			return -ENOMEM;
		result = ident_pud_init(info, pud, addr, next);
		if (result)
			return result;
		*pgd = __pgd(__pa(pud) | _KERNPG_TABLE);
	}

	return 0;
}

#ifndef CONFIG_XEN
static int __init parse_direct_gbpages_off(char *arg)
{
	direct_gbpages = 0;
	return 0;
}
early_param("nogbpages", parse_direct_gbpages_off);

static int __init parse_direct_gbpages_on(char *arg)
{
	direct_gbpages = 1;
	return 0;
}
early_param("gbpages", parse_direct_gbpages_on);
#endif

/*
 * NOTE: pagetable_init alloc all the fixmap pagetables contiguous on the
 * physical space so we can cache the place of the first one and move
 * around without checking the pgd every time.
 */

pteval_t __supported_pte_mask __read_mostly = ~0UL;
EXPORT_SYMBOL_GPL(__supported_pte_mask);

int force_personality32;

/*
 * noexec32=on|off
 * Control non executable heap for 32bit processes.
 * To control the stack too use noexec=off
 *
 * on	PROT_READ does not imply PROT_EXEC for 32-bit processes (default)
 * off	PROT_READ implies PROT_EXEC
 */
static int __init nonx32_setup(char *str)
{
	if (!strcmp(str, "on"))
		force_personality32 &= ~READ_IMPLIES_EXEC;
	else if (!strcmp(str, "off"))
		force_personality32 |= READ_IMPLIES_EXEC;
	return 1;
}
__setup("noexec32=", nonx32_setup);

/*
 * When memory was added/removed make sure all the processes MM have
 * suitable PGD entries in the local PGD level page.
 */
void sync_global_pgds(unsigned long start, unsigned long end, int removed)
{
	unsigned long address;

	for (address = start; address <= end; address += PGDIR_SIZE) {
		const pgd_t *pgd_ref = pgd_offset_k(address);
		struct page *page;

		/*
		 * When it is called after memory hot remove, pgd_none()
		 * returns true. In this case (removed == 1), we must clear
		 * the PGD entries in the local PGD level page.
		 */
		if (pgd_none(*pgd_ref) && !removed)
			continue;

		spin_lock(&pgd_lock);
		list_for_each_entry(page, &pgd_list, lru) {
			pgd_t *pgd;
			spinlock_t *pgt_lock;

			pgd = (pgd_t *)page_address(page) + pgd_index(address);
			/* the pgt_lock only for Xen */
			pgt_lock = &pgd_page_get_mm(page)->page_table_lock;
			spin_lock(pgt_lock);

			if (!pgd_none(*pgd_ref) && !pgd_none(*pgd))
				BUG_ON(pgd_page_vaddr(*pgd)
				       != pgd_page_vaddr(*pgd_ref));

			if (removed) {
				if (pgd_none(*pgd_ref) && !pgd_none(*pgd))
					pgd_clear(pgd);
			} else {
				if (pgd_none(*pgd))
					set_pgd(pgd, *pgd_ref);
			}

			spin_unlock(pgt_lock);
		}
		spin_unlock(&pgd_lock);
	}
}

/*
 * NOTE: This function is marked __ref because it calls __init function
 * (alloc_bootmem_pages). It's safe to do it ONLY when after_bootmem == 0.
 */
static __ref void *spp_getpage(void)
{
	void *ptr;

	if (after_bootmem)
		ptr = (void *) get_zeroed_page(GFP_ATOMIC | __GFP_NOTRACK);
	else
		ptr = alloc_bootmem_pages(PAGE_SIZE);

	if (!ptr || ((unsigned long)ptr & ~PAGE_MASK)) {
		panic("set_pte_phys: cannot allocate page data %s\n",
			after_bootmem ? "after bootmem" : "");
	}

	pr_debug("spp_getpage %p\n", ptr);

	return ptr;
}

static pud_t *fill_pud(pgd_t *pgd, unsigned long vaddr)
{
	if (pgd_none(*pgd)) {
		pud_t *pud = (pud_t *)spp_getpage();
		if (!after_bootmem) {
			make_page_readonly(pud, XENFEAT_writable_page_tables);
			xen_l4_entry_update(pgd, __pgd(__pa(pud) | _PAGE_TABLE));
		} else
			pgd_populate(&init_mm, pgd, pud);
		if (pud != pud_offset(pgd, 0))
			printk(KERN_ERR "PAGETABLE BUG #00! %p <-> %p\n",
			       pud, pud_offset(pgd, 0));
	}
	return pud_offset(pgd, vaddr);
}

static pmd_t *fill_pmd(pud_t *pud, unsigned long vaddr)
{
	if (pud_none(*pud)) {
		pmd_t *pmd = (pmd_t *) spp_getpage();
		if (!after_bootmem) {
			make_page_readonly(pmd, XENFEAT_writable_page_tables);
			xen_l3_entry_update(pud, __pud(__pa(pmd) | _PAGE_TABLE));
		} else
			pud_populate(&init_mm, pud, pmd);
		if (pmd != pmd_offset(pud, 0))
			printk(KERN_ERR "PAGETABLE BUG #01! %p <-> %p\n",
			       pmd, pmd_offset(pud, 0));
	}
	return pmd_offset(pud, vaddr);
}

static pte_t *fill_pte(pmd_t *pmd, unsigned long vaddr)
{
	if (pmd_none(*pmd)) {
		pte_t *pte = (pte_t *) spp_getpage();
		make_page_readonly(pte, XENFEAT_writable_page_tables);
		pmd_populate_kernel(&init_mm, pmd, pte);
		if (pte != pte_offset_kernel(pmd, 0))
			printk(KERN_ERR "PAGETABLE BUG #02!\n");
	}
	return pte_offset_kernel(pmd, vaddr);
}

void set_pte_vaddr_pud(pud_t *pud_page, unsigned long vaddr, pte_t new_pte)
{
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pud = pud_page + pud_index(vaddr);
	pmd = fill_pmd(pud, vaddr);
	pte = fill_pte(pmd, vaddr);

	set_pte(pte, new_pte);

	/*
	 * It's enough to flush this one mapping.
	 * (PGE mappings get flushed as well)
	 */
	__flush_tlb_one(vaddr);
}

void set_pte_vaddr(unsigned long vaddr, pte_t pteval)
{
	pgd_t *pgd;
	pud_t *pud_page;

	pr_debug("set_pte_vaddr %lx to %lx\n", vaddr, __pte_val(pteval));

	pgd = pgd_offset_k(vaddr);
	if (pgd_none(*pgd)) {
		printk(KERN_ERR
			"PGD FIXMAP MISSING, it should be setup in head.S!\n");
		return;
	}
	pud_page = (pud_t*)pgd_page_vaddr(*pgd);
	set_pte_vaddr_pud(pud_page, vaddr, pteval);
}

pmd_t * __init populate_extra_pmd(unsigned long vaddr)
{
	pgd_t *pgd;
	pud_t *pud;

	pgd = pgd_offset_k(vaddr);
	pud = fill_pud(pgd, vaddr);
	return fill_pmd(pud, vaddr);
}

pte_t * __init populate_extra_pte(unsigned long vaddr)
{
	pmd_t *pmd;

	pmd = populate_extra_pmd(vaddr);
	return fill_pte(pmd, vaddr);
}

#ifndef CONFIG_XEN
/*
 * Create large page table mappings for a range of physical addresses.
 */
static void __init __init_extra_mapping(unsigned long phys, unsigned long size,
					enum page_cache_mode cache)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pgprot_t prot;

	pgprot_val(prot) = pgprot_val(PAGE_KERNEL_LARGE) |
		pgprot_val(pgprot_4k_2_large(cachemode2pgprot(cache)));
	BUG_ON((phys & ~PMD_MASK) || (size & ~PMD_MASK));
	for (; size; phys += PMD_SIZE, size -= PMD_SIZE) {
		pgd = pgd_offset_k((unsigned long)__va(phys));
		if (pgd_none(*pgd)) {
			pud = (pud_t *) spp_getpage();
			set_pgd(pgd, __pgd(__pa(pud) | _KERNPG_TABLE |
						_PAGE_USER));
		}
		pud = pud_offset(pgd, (unsigned long)__va(phys));
		if (pud_none(*pud)) {
			pmd = (pmd_t *) spp_getpage();
			set_pud(pud, __pud(__pa(pmd) | _KERNPG_TABLE |
						_PAGE_USER));
		}
		pmd = pmd_offset(pud, phys);
		BUG_ON(!pmd_none(*pmd));
		set_pmd(pmd, __pmd(phys | pgprot_val(prot)));
	}
}

void __init init_extra_mapping_wb(unsigned long phys, unsigned long size)
{
	__init_extra_mapping(phys, size, _PAGE_CACHE_MODE_WB);
}

void __init init_extra_mapping_uc(unsigned long phys, unsigned long size)
{
	__init_extra_mapping(phys, size, _PAGE_CACHE_MODE_UC);
}

/*
 * The head.S code sets up the kernel high mapping:
 *
 *   from __START_KERNEL_map to __START_KERNEL_map + size (== _end-_text)
 *
 * phys_base holds the negative offset to the kernel, which is added
 * to the compile time generated pmds. This results in invalid pmds up
 * to the point where we hit the physaddr 0 mapping.
 *
 * We limit the mappings to the region from _text to _brk_end.  _brk_end
 * is rounded up to the 2MB boundary. This catches the invalid pmds as
 * well, as they are located before _text:
 */
void __init cleanup_highmap(void)
{
	unsigned long vaddr = __START_KERNEL_map;
	unsigned long vaddr_end = __START_KERNEL_map + KERNEL_IMAGE_SIZE;
	unsigned long end = roundup((unsigned long)_brk_end, PMD_SIZE) - 1;
	pmd_t *pmd = level2_kernel_pgt;

	/*
	 * Native path, max_pfn_mapped is not set yet.
	 * Xen has valid max_pfn_mapped set in
	 *	arch/x86/xen/mmu.c:xen_setup_kernel_pagetable().
	 */
	if (max_pfn_mapped)
		vaddr_end = __START_KERNEL_map + (max_pfn_mapped << PAGE_SHIFT);

	for (; vaddr + PMD_SIZE - 1 < vaddr_end; pmd++, vaddr += PMD_SIZE) {
		if (pmd_none(*pmd))
			continue;
		if (vaddr < (unsigned long) _text || vaddr > end)
			set_pmd(pmd, __pmd(0));
	}
}
#endif

static inline int __meminit make_readonly(unsigned long paddr)
{
	int readonly = 0;

	/* Make page tables inside the kernel image read-only. */
	if (!xen_feature(XENFEAT_writable_page_tables) && in_pgt_buf(paddr))
		readonly = 1;
	/* Make old page tables read-only. */
	if (!xen_feature(XENFEAT_writable_page_tables)
	    && (paddr >= (xen_start_info->pt_base - __START_KERNEL_map))
	    && (paddr < (xen_start_info->pt_base +
			 PFN_PHYS(xen_start_info->nr_pt_frames) -
			 __START_KERNEL_map)))
		readonly = 1;
	/* Make P->M table (and its page tables) read-only. */
	if (!xen_feature(XENFEAT_writable_page_tables)
	    && xen_start_info->mfn_list < __START_KERNEL_map
	    && paddr >= (xen_start_info->first_p2m_pfn << PAGE_SHIFT)
	    && paddr < (xen_start_info->first_p2m_pfn
			+ xen_start_info->nr_p2m_frames) << PAGE_SHIFT)
		readonly = 1;

	/*
	 * No need for writable mapping of kernel image. This also ensures that
	 * page and descriptor tables embedded inside don't have writable
	 * mappings. The range must be in sync with that passed to
	 * memblock_reserve() (covering kernel code and data), since all other
	 * regions can be allocated from under CONFIG_NO_BOOTMEM and thus must
	 * be writable.
	 */
	if ((paddr >= __pa_symbol(&_text))
            && (paddr < (__pa_symbol(__bss_stop) & PAGE_MASK)))
		readonly = 1;

	return readonly;
}

static unsigned long __meminit
phys_pte_init(pte_t *pte_page, unsigned long addr, unsigned long end,
	      pgprot_t prot)
{
	unsigned long pages = 0, next;
	unsigned long last_map_addr = end;
	int i;

	pte_t *pte = pte_page + pte_index(addr);

	for (i = pte_index(addr); i < PTRS_PER_PTE; i++, addr = next, pte++) {
		unsigned long pteval = addr | pgprot_val(prot);

		next = (addr & PAGE_MASK) + PAGE_SIZE;
		if (addr >= end ||
		    (!after_bootmem &&
		     (addr >> PAGE_SHIFT) >= xen_start_info->nr_pages))
			break;

		/*
		 * We will re-use the existing mapping.
		 * Xen for example has some special requirements, like mapping
		 * pagetable pages as RO. So assume someone who pre-setup
		 * these mappings are more intelligent.
		 */
		if (__pte_val(*pte)) {
			if (!after_bootmem)
				pages++;
			continue;
		}

		if (make_readonly(addr))
			pteval &= ~_PAGE_RW;
		if (0)
			printk("   pte=%p addr=%lx pte=%016lx\n",
			       pte, addr, pteval);
		pages++;
		if (!after_bootmem)
			*pte = __pte(pteval & __supported_pte_mask);
		else
			set_pte(pte, __pte(pteval & __supported_pte_mask));
		last_map_addr = (addr & PAGE_MASK) + PAGE_SIZE;
	}

	update_page_count(PG_LEVEL_4K, pages);

	return last_map_addr;
}

static unsigned long __meminit
phys_pmd_init(pmd_t *pmd_page, unsigned long address, unsigned long end,
	      unsigned long page_size_mask, pgprot_t prot)
{
	unsigned long pages = 0, next;
	unsigned long last_map_addr = end;

	int i = pmd_index(address);

	for (; i < PTRS_PER_PMD; i++, address = next) {
		pmd_t *pmd = pmd_page + pmd_index(address);
		pte_t *pte;
		pgprot_t new_prot = prot;

		next = (address & PMD_MASK) + PMD_SIZE;
		if (address >= end)
			break;

		if (__pmd_val(*pmd)) {
			if (!pmd_large(*pmd)) {
				spin_lock(&init_mm.page_table_lock);
				pte = (pte_t *)pmd_page_vaddr(*pmd);
				last_map_addr = phys_pte_init(pte, address,
								end, prot);
				spin_unlock(&init_mm.page_table_lock);
				continue;
			}
			/*
			 * If we are ok with PG_LEVEL_2M mapping, then we will
			 * use the existing mapping,
			 *
			 * Otherwise, we will split the large page mapping but
			 * use the same existing protection bits except for
			 * large page, so that we don't violate Intel's TLB
			 * Application note (317080) which says, while changing
			 * the page sizes, new and old translations should
			 * not differ with respect to page frame and
			 * attributes.
			 */
			if (page_size_mask & (1 << PG_LEVEL_2M)) {
				if (!after_bootmem)
					pages++;
				last_map_addr = next;
				continue;
			}
			new_prot = pte_pgprot(pte_clrhuge(*(pte_t *)pmd));
		}

		if (page_size_mask & (1<<PG_LEVEL_2M)) {
			pages++;
			spin_lock(&init_mm.page_table_lock);
			set_pte((pte_t *)pmd,
				pfn_pte((address & PMD_MASK) >> PAGE_SHIFT,
					__pgprot(pgprot_val(prot) | _PAGE_PSE)));
			spin_unlock(&init_mm.page_table_lock);
			last_map_addr = next;
			continue;
		}

		pte = alloc_low_page();
		last_map_addr = phys_pte_init(pte, address, end, new_prot);

		if (!after_bootmem) {
			make_page_readonly(pte, XENFEAT_writable_page_tables);
			if (page_size_mask & (1 << PG_LEVEL_NUM)) {
				mmu_update_t u;

				u.ptr = arbitrary_virt_to_machine(pmd);
				u.val = phys_to_machine(__pa(pte)) | _PAGE_TABLE;
				if (HYPERVISOR_mmu_update(&u, 1, NULL,
							  DOMID_SELF) < 0)
					BUG();
			} else
				*pmd = __pmd(__pa(pte) | _PAGE_TABLE);
		} else {
			spin_lock(&init_mm.page_table_lock);
			pmd_populate_kernel(&init_mm, pmd, pte);
			spin_unlock(&init_mm.page_table_lock);
		}
	}
	update_page_count(PG_LEVEL_2M, pages);
	return last_map_addr;
}

static unsigned long __meminit
phys_pud_init(pud_t *pud_page, unsigned long addr, unsigned long end,
			 unsigned long page_size_mask)
{
	unsigned long pages = 0, next;
	unsigned long last_map_addr = end;
	int i = pud_index(addr);

	for (; i < PTRS_PER_PUD; i++, addr = next) {
		pud_t *pud = pud_page + pud_index(addr);
		pmd_t *pmd;
		pgprot_t prot = PAGE_KERNEL;

		next = (addr & PUD_MASK) + PUD_SIZE;
		if (addr >= end)
			break;

		if (__pud_val(*pud)) {
			if (!pud_large(*pud)) {
				pmd = pmd_offset(pud, 0);
				last_map_addr = phys_pmd_init(pmd, addr, end,
					page_size_mask | (1 << PG_LEVEL_NUM),
					prot);
				__flush_tlb_all();
				continue;
			}
			/*
			 * If we are ok with PG_LEVEL_1G mapping, then we will
			 * use the existing mapping.
			 *
			 * Otherwise, we will split the gbpage mapping but use
			 * the same existing protection  bits except for large
			 * page, so that we don't violate Intel's TLB
			 * Application note (317080) which says, while changing
			 * the page sizes, new and old translations should
			 * not differ with respect to page frame and
			 * attributes.
			 */
			if (page_size_mask & (1 << PG_LEVEL_1G)) {
				if (!after_bootmem)
					pages++;
				last_map_addr = next;
				continue;
			}
			prot = pte_pgprot(pte_clrhuge(*(pte_t *)pud));
		}

		if (page_size_mask & (1<<PG_LEVEL_1G)) {
			pages++;
			spin_lock(&init_mm.page_table_lock);
			set_pte((pte_t *)pud,
				pfn_pte((addr & PUD_MASK) >> PAGE_SHIFT,
					PAGE_KERNEL_LARGE));
			spin_unlock(&init_mm.page_table_lock);
			last_map_addr = next;
			continue;
		}

		pmd = alloc_low_page();
		last_map_addr = phys_pmd_init(pmd, addr, end,
					      page_size_mask & ~(1 << PG_LEVEL_NUM),
					      prot);

		if (!after_bootmem) {
			make_page_readonly(pmd, XENFEAT_writable_page_tables);
			if (page_size_mask & (1 << PG_LEVEL_NUM)) {
				mmu_update_t u;

				u.ptr = arbitrary_virt_to_machine(pud);
				u.val = phys_to_machine(__pa(pmd)) | _PAGE_TABLE;
				if (HYPERVISOR_mmu_update(&u, 1, NULL,
							  DOMID_SELF) < 0)
					BUG();
			} else
				*pud = __pud(__pa(pmd) | _PAGE_TABLE);
		} else {
			spin_lock(&init_mm.page_table_lock);
			pud_populate(&init_mm, pud, pmd);
			spin_unlock(&init_mm.page_table_lock);
		}
	}
	__flush_tlb_all();

	update_page_count(PG_LEVEL_1G, pages);

	return last_map_addr;
}

RESERVE_BRK(kernel_pgt_alloc,
	    (1
	     + (PUD_SIZE - 1 - __START_KERNEL_map) / PUD_SIZE
	     + -__START_KERNEL_map / PMD_SIZE) * PAGE_SIZE);

void __init xen_init_pt(void)
{
	unsigned long addr, *page, end, pmd_sz, pmd_pa, pte_sz, pte_pa;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	/* Find the initial pte page that was built for us. */
	page = (unsigned long *)xen_start_info->pt_base;
	addr = page[pgd_index(__START_KERNEL_map)];
	addr_to_page(addr, page);

#if CONFIG_XEN_COMPAT <= 0x030002
	/* On Xen 3.0.2 and older we may need to explicitly specify _PAGE_USER
	   in kernel PTEs. We check that here. */
	if (HYPERVISOR_xen_version(XENVER_version, NULL) <= 0x30000) {
		unsigned long *pg;
		pte_t pte;

		/* Mess with the initial mapping of page 0. It's not needed. */
		BUILD_BUG_ON(__START_KERNEL <= __START_KERNEL_map);
		addr = page[pud_index(__START_KERNEL_map)];
		addr_to_page(addr, pg);
		addr = pg[pmd_index(__START_KERNEL_map)];
		addr_to_page(addr, pg);
		pte.pte = pg[pte_index(__START_KERNEL_map)];
		BUG_ON(!(pte.pte & _PAGE_PRESENT));

		/* If _PAGE_USER isn't set, we obviously do not need it. */
		if (pte.pte & _PAGE_USER) {
			/* _PAGE_USER is needed, but is it set implicitly? */
			pte.pte &= ~_PAGE_USER;
			if ((HYPERVISOR_update_va_mapping(__START_KERNEL_map,
							  pte, 0) != 0) ||
			    !(pg[pte_index(__START_KERNEL_map)] & _PAGE_USER))
				/* We need to explicitly specify _PAGE_USER. */
				__kernel_page_user = _PAGE_USER;
		}
	}
#endif

	/* Construct mapping of initial pte page in our own directories. */
	init_level4_pgt[pgd_index(__START_KERNEL_map)] = 
		__pgd(__pa_symbol(level3_kernel_pgt) | _PAGE_TABLE);
	memcpy(level3_kernel_pgt + pud_index(__START_KERNEL_map),
	       page + pud_index(__START_KERNEL_map),
	       (PTRS_PER_PUD - pud_index(__START_KERNEL_map))
	       * sizeof(*level3_kernel_pgt));

	/* Copy the initial P->M table mappings if necessary. */
	addr = pgd_index(xen_start_info->mfn_list);
	if (addr < pgd_index(__START_KERNEL_map))
		init_level4_pgt[addr] =
			((pgd_t *)xen_start_info->pt_base)[addr];

	/* Do an early initialization of the fixmap area. */
	addr = __fix_to_virt(FIX_EARLYCON_MEM_BASE);
	if (pud_present(level3_kernel_pgt[pud_index(addr)])) {
		unsigned long adr = page[pud_index(addr)];

		addr_to_page(adr, page);
		copy_page(level2_fixmap_pgt, page);
	}
	level3_kernel_pgt[pud_index(addr)] =
		__pud(__pa_symbol(level2_fixmap_pgt) | _PAGE_TABLE);
	level2_fixmap_pgt[pmd_index(addr)] =
		__pmd(__pa_symbol(level1_fixmap_pgt) | _PAGE_TABLE);

	/* Construct 1:1 mapping of the initial allocation. */
	pud = extend_brk(PAGE_SIZE, PAGE_SIZE);
	addr = __pa_symbol(pud);
	printk(KERN_DEBUG "BRK [%#010lx, %#010lx] PUD\n",
	       addr, addr + PAGE_SIZE - 1);
	init_level4_pgt[pgd_index(PAGE_OFFSET)] = __pgd(addr | _PAGE_TABLE);

	BUILD_BUG_ON(pgd_index(PAGE_OFFSET)
		     != pgd_index(PAGE_OFFSET - __START_KERNEL_map - 1));
	BUILD_BUG_ON(pmd_index(__START_KERNEL_map));

	end = __pa_symbol(xen_start_info->pt_base)
	      + (xen_start_info->nr_pt_frames << PAGE_SHIFT);
	pmd_sz = ((end + PUD_SIZE - 1) >> PUD_SHIFT) << PAGE_SHIFT;
	pmd = extend_brk(pmd_sz, PAGE_SIZE);
	pmd_pa = __pa_symbol(pmd);
	printk(KERN_DEBUG "BRK [%#010lx, %#010lx] PMD\n",
	       pmd_pa, pmd_pa + pmd_sz - 1);
	for (addr = 0; addr < end; addr += PUD_SIZE)
		pud[pud_index(PAGE_OFFSET + addr)]
			= __pud((pmd_pa + (addr >> (PUD_SHIFT - PAGE_SHIFT)))
				| _PAGE_TABLE);

	pte_sz = ((end + PMD_SIZE - 1) >> PMD_SHIFT) << PAGE_SHIFT;
	pte = extend_brk(pte_sz, PAGE_SIZE);
	pte_pa = __pa_symbol(pte);
	printk(KERN_DEBUG "BRK [%#010lx, %#010lx] PTE\n",
	       pte_pa, pte_pa + pte_sz - 1);
	for (addr = 0; addr < end; addr += PMD_SIZE) {
		unsigned int i;
		unsigned long pa = pte_pa + (addr >> (PMD_SHIFT - PAGE_SHIFT));
		const pte_t *pte_k = (void *)__START_KERNEL_map
			+ (pmd_val(*early_get_pmd(__START_KERNEL_map + addr))
			   & PTE_PFN_MASK);

		pmd[addr >> PMD_SHIFT] = __pmd(pa | _PAGE_TABLE);
		for (i = 0; i < PTRS_PER_PTE; ++i, ++pte) {
			pa = addr + (i << PAGE_SHIFT);
			*pte = make_readonly(pa)
			       || pa == __pa_symbol(pud)
			       || (pa >= pmd_pa && pa < pmd_pa + pmd_sz)
			       || (pa >= pte_pa && pa < pte_pa + pte_sz)
			       ? pte_wrprotect(pte_k[i]) : pte_k[i];
		}
		early_make_page_readonly(pte - PTRS_PER_PTE,
					 XENFEAT_writable_page_tables);
	}
	max_pfn_mapped = PFN_DOWN(addr);
	for (addr = 0; addr < end; addr += PUD_SIZE)
		early_make_page_readonly(pmd + PTRS_PER_PMD
					       * (addr >> PUD_SHIFT),
					 XENFEAT_writable_page_tables);
	early_make_page_readonly(pud, XENFEAT_writable_page_tables);

	early_make_page_readonly(init_level4_pgt,
				 XENFEAT_writable_page_tables);
	early_make_page_readonly(level3_kernel_pgt,
				 XENFEAT_writable_page_tables);
#ifdef CONFIG_X86_VSYSCALL_EMULATION
	early_make_page_readonly(level3_user_pgt,
				 XENFEAT_writable_page_tables);
#endif
	early_make_page_readonly(level2_fixmap_pgt,
				 XENFEAT_writable_page_tables);
	early_make_page_readonly(level1_fixmap_pgt,
				 XENFEAT_writable_page_tables);

	if (!xen_feature(XENFEAT_writable_page_tables))
		xen_pgd_pin(init_level4_pgt);
}

void __init xen_finish_init_mapping(void)
{
	unsigned long va;
	struct mmuext_op mmuext;
	pud_t *pud;

	/* Re-vector virtual addresses pointing into the initial
	   mapping to the just-established permanent ones. */
	xen_start_info = __va(__pa(xen_start_info));
	xen_start_info->pt_base = (unsigned long)
		__va(__pa(xen_start_info->pt_base));
	if (!xen_feature(XENFEAT_auto_translated_physmap)
	    && xen_start_info->mfn_list >= __START_KERNEL_map)
		phys_to_machine_mapping =
			__va(__pa_symbol(xen_start_info->mfn_list));

	/* Unpin the no longer used Xen provided page tables. */
	mmuext.cmd = MMUEXT_UNPIN_TABLE;
	mmuext.arg1.mfn = virt_to_mfn(xen_start_info->pt_base);
	if (HYPERVISOR_mmuext_op(&mmuext, 1, NULL, DOMID_SELF))
		BUG();

	/* Kill mapping of memory below _text. */
	va = __START_KERNEL_map;
	while (va < (unsigned long)&_text) {
		if (HYPERVISOR_update_va_mapping(va, __pte_ma(0), 0))
			BUG();
		va += PAGE_SIZE;
	}

	/* Destroy the Xen-created mappings beyond the kernel image. */
	va = PAGE_ALIGN(_brk_end);
	pud = pud_offset(pgd_offset_k(va), va);
	while (!pmd_none(*pmd_offset(pud, va))) {
		if (HYPERVISOR_update_va_mapping(va, __pte_ma(0), 0))
			BUG();
		va += PAGE_SIZE;
		if (!(va & (PUD_SIZE - 1)))
			pud = pud_offset(pgd_offset_k(va), va);
	}
}

unsigned long __meminit
kernel_physical_mapping_init(unsigned long start,
			     unsigned long end,
			     unsigned long page_size_mask)
{
	bool pgd_changed = false;
	unsigned long next, last_map_addr = end;
	unsigned long addr;

	start = (unsigned long)__va(start);
	end = (unsigned long)__va(end);
	addr = start;

	for (; start < end; start = next) {
		pgd_t *pgd = pgd_offset_k(start);
		pud_t *pud;

		next = (start & PGDIR_MASK) + PGDIR_SIZE;

		if (__pgd_val(*pgd)) {
			pud = (pud_t *)pgd_page_vaddr(*pgd);
			last_map_addr = phys_pud_init(pud, __pa(start),
				__pa(end), page_size_mask | (1 << PG_LEVEL_NUM));
			continue;
		}

		pud = alloc_low_page();
		last_map_addr = phys_pud_init(pud, __pa(start), __pa(end),
						 page_size_mask);

		if (!after_bootmem) {
			make_page_readonly(pud, XENFEAT_writable_page_tables);
			xen_l4_entry_update(pgd, __pgd(__pa(pud) | _PAGE_TABLE));
		} else {
			spin_lock(&init_mm.page_table_lock);
			pgd_populate(&init_mm, pgd, pud);
			spin_unlock(&init_mm.page_table_lock);
			pgd_changed = true;
		}
	}

	if (pgd_changed)
		sync_global_pgds(addr, end - 1, 0);

	return last_map_addr;
}

#ifndef CONFIG_NUMA
void __init initmem_init(void)
{
	memblock_set_node(0, (phys_addr_t)ULLONG_MAX, &memblock.memory, 0);
}
#endif

void __init paging_init(void)
{
	sparse_memory_present_with_active_regions(MAX_NUMNODES);
	sparse_init();

	/*
	 * clear the default setting with node 0
	 * note: don't use nodes_clear here, that is really clearing when
	 *	 numa support is not compiled in, and later node_set_state
	 *	 will not set it back.
	 */
	node_clear_state(0, N_MEMORY);
	if (N_MEMORY != N_NORMAL_MEMORY)
		node_clear_state(0, N_NORMAL_MEMORY);

	zone_sizes_init();
}

/*
 * Memory hotplug specific functions
 */
#ifdef CONFIG_MEMORY_HOTPLUG
/*
 * After memory hotplug the variables max_pfn, max_low_pfn and high_memory need
 * updating.
 */
static void  update_end_of_memory_vars(u64 start, u64 size)
{
	unsigned long end_pfn = PFN_UP(start + size);

	if (end_pfn > max_pfn) {
		max_pfn = end_pfn;
		max_low_pfn = end_pfn;
		high_memory = (void *)__va(max_pfn * PAGE_SIZE - 1) + 1;
	}
}

/*
 * Memory is added always to NORMAL zone. This means you will never get
 * additional DMA/DMA32 memory.
 */
int arch_add_memory(int nid, u64 start, u64 size)
{
	struct pglist_data *pgdat = NODE_DATA(nid);
	struct zone *zone = pgdat->node_zones +
		zone_for_memory(nid, start, size, ZONE_NORMAL);
	unsigned long start_pfn = start >> PAGE_SHIFT;
	unsigned long nr_pages = size >> PAGE_SHIFT;
	int ret;

	init_memory_mapping(start, start + size);

	ret = __add_pages(nid, zone, start_pfn, nr_pages);
	WARN_ON_ONCE(ret);

	/* update max_pfn, max_low_pfn and high_memory */
	update_end_of_memory_vars(start, size);

	return ret;
}
EXPORT_SYMBOL_GPL(arch_add_memory);

#define PAGE_INUSE 0xFD

static void __meminit free_pagetable(struct page *page, int order)
{
	unsigned long magic;
	unsigned int nr_pages = 1 << order;

	/* bootmem page has reserved flag */
	if (PageReserved(page)) {
		__ClearPageReserved(page);

		magic = (unsigned long)page->lru.next;
		if (magic == SECTION_INFO || magic == MIX_SECTION_INFO) {
			while (nr_pages--)
				put_page_bootmem(page++);
		} else
			while (nr_pages--)
				free_reserved_page(page++);
	} else
		free_pages((unsigned long)page_address(page), order);
}

static void __meminit free_pte_table(pte_t *pte_start, pmd_t *pmd)
{
	pte_t *pte;
	int i;

	for (i = 0; i < PTRS_PER_PTE; i++) {
		pte = pte_start + i;
		if (pte_val(*pte))
			return;
	}

	/* free a pte talbe */
	free_pagetable(pmd_page(*pmd), 0);
	spin_lock(&init_mm.page_table_lock);
	pmd_clear(pmd);
	spin_unlock(&init_mm.page_table_lock);
}

static void __meminit free_pmd_table(pmd_t *pmd_start, pud_t *pud)
{
	pmd_t *pmd;
	int i;

	for (i = 0; i < PTRS_PER_PMD; i++) {
		pmd = pmd_start + i;
		if (pmd_val(*pmd))
			return;
	}

	/* free a pmd talbe */
	free_pagetable(pud_page(*pud), 0);
	spin_lock(&init_mm.page_table_lock);
	pud_clear(pud);
	spin_unlock(&init_mm.page_table_lock);
}

/* Return true if pgd is changed, otherwise return false. */
static bool __meminit free_pud_table(pud_t *pud_start, pgd_t *pgd)
{
	pud_t *pud;
	int i;

	for (i = 0; i < PTRS_PER_PUD; i++) {
		pud = pud_start + i;
		if (pud_val(*pud))
			return false;
	}

	/* free a pud table */
	free_pagetable(pgd_page(*pgd), 0);
	spin_lock(&init_mm.page_table_lock);
	pgd_clear(pgd);
	spin_unlock(&init_mm.page_table_lock);

	return true;
}

static void __meminit
remove_pte_table(pte_t *pte_start, unsigned long addr, unsigned long end,
		 bool direct)
{
	unsigned long next, pages = 0;
	pte_t *pte;
	void *page_addr;
	phys_addr_t phys_addr;

	pte = pte_start + pte_index(addr);
	for (; addr < end; addr = next, pte++) {
		next = (addr + PAGE_SIZE) & PAGE_MASK;
		if (next > end)
			next = end;

		if (!pte_present(*pte))
			continue;

		/*
		 * We mapped [0,1G) memory as identity mapping when
		 * initializing, in arch/x86/kernel/head_64.S. These
		 * pagetables cannot be removed.
		 */
		phys_addr = pte_val(*pte) + (addr & PAGE_MASK);
		if (phys_addr < (phys_addr_t)0x40000000)
			return;

		if (IS_ALIGNED(addr, PAGE_SIZE) &&
		    IS_ALIGNED(next, PAGE_SIZE)) {
			/*
			 * Do not free direct mapping pages since they were
			 * freed when offlining, or simplely not in use.
			 */
			if (!direct)
				free_pagetable(pte_page(*pte), 0);

			spin_lock(&init_mm.page_table_lock);
			pte_clear(&init_mm, addr, pte);
			spin_unlock(&init_mm.page_table_lock);

			/* For non-direct mapping, pages means nothing. */
			pages++;
		} else {
			/*
			 * If we are here, we are freeing vmemmap pages since
			 * direct mapped memory ranges to be freed are aligned.
			 *
			 * If we are not removing the whole page, it means
			 * other page structs in this page are being used and
			 * we canot remove them. So fill the unused page_structs
			 * with 0xFD, and remove the page when it is wholly
			 * filled with 0xFD.
			 */
			memset((void *)addr, PAGE_INUSE, next - addr);

			page_addr = page_address(pte_page(*pte));
			if (!memchr_inv(page_addr, PAGE_INUSE, PAGE_SIZE)) {
				free_pagetable(pte_page(*pte), 0);

				spin_lock(&init_mm.page_table_lock);
				pte_clear(&init_mm, addr, pte);
				spin_unlock(&init_mm.page_table_lock);
			}
		}
	}

	/* Call free_pte_table() in remove_pmd_table(). */
	flush_tlb_all();
	if (direct)
		update_page_count(PG_LEVEL_4K, -pages);
}

static void __meminit
remove_pmd_table(pmd_t *pmd_start, unsigned long addr, unsigned long end,
		 bool direct)
{
	unsigned long next, pages = 0;
	pte_t *pte_base;
	pmd_t *pmd;
	void *page_addr;

	pmd = pmd_start + pmd_index(addr);
	for (; addr < end; addr = next, pmd++) {
		next = pmd_addr_end(addr, end);

		if (!pmd_present(*pmd))
			continue;

		if (pmd_large(*pmd)) {
			if (IS_ALIGNED(addr, PMD_SIZE) &&
			    IS_ALIGNED(next, PMD_SIZE)) {
				if (!direct)
					free_pagetable(pmd_page(*pmd),
						       get_order(PMD_SIZE));

				spin_lock(&init_mm.page_table_lock);
				pmd_clear(pmd);
				spin_unlock(&init_mm.page_table_lock);
				pages++;
			} else {
				/* If here, we are freeing vmemmap pages. */
				memset((void *)addr, PAGE_INUSE, next - addr);

				page_addr = page_address(pmd_page(*pmd));
				if (!memchr_inv(page_addr, PAGE_INUSE,
						PMD_SIZE)) {
					free_pagetable(pmd_page(*pmd),
						       get_order(PMD_SIZE));

					spin_lock(&init_mm.page_table_lock);
					pmd_clear(pmd);
					spin_unlock(&init_mm.page_table_lock);
				}
			}

			continue;
		}

		pte_base = (pte_t *)pmd_page_vaddr(*pmd);
		remove_pte_table(pte_base, addr, next, direct);
		free_pte_table(pte_base, pmd);
	}

	/* Call free_pmd_table() in remove_pud_table(). */
	if (direct)
		update_page_count(PG_LEVEL_2M, -pages);
}

static void __meminit
remove_pud_table(pud_t *pud_start, unsigned long addr, unsigned long end,
		 bool direct)
{
	unsigned long next, pages = 0;
	pmd_t *pmd_base;
	pud_t *pud;
	void *page_addr;

	pud = pud_start + pud_index(addr);
	for (; addr < end; addr = next, pud++) {
		next = pud_addr_end(addr, end);

		if (!pud_present(*pud))
			continue;

		if (pud_large(*pud)) {
			if (IS_ALIGNED(addr, PUD_SIZE) &&
			    IS_ALIGNED(next, PUD_SIZE)) {
				if (!direct)
					free_pagetable(pud_page(*pud),
						       get_order(PUD_SIZE));

				spin_lock(&init_mm.page_table_lock);
				pud_clear(pud);
				spin_unlock(&init_mm.page_table_lock);
				pages++;
			} else {
				/* If here, we are freeing vmemmap pages. */
				memset((void *)addr, PAGE_INUSE, next - addr);

				page_addr = page_address(pud_page(*pud));
				if (!memchr_inv(page_addr, PAGE_INUSE,
						PUD_SIZE)) {
					free_pagetable(pud_page(*pud),
						       get_order(PUD_SIZE));

					spin_lock(&init_mm.page_table_lock);
					pud_clear(pud);
					spin_unlock(&init_mm.page_table_lock);
				}
			}

			continue;
		}

		pmd_base = (pmd_t *)pud_page_vaddr(*pud);
		remove_pmd_table(pmd_base, addr, next, direct);
		free_pmd_table(pmd_base, pud);
	}

	if (direct)
		update_page_count(PG_LEVEL_1G, -pages);
}

/* start and end are both virtual address. */
static void __meminit
remove_pagetable(unsigned long start, unsigned long end, bool direct)
{
	unsigned long next;
	unsigned long addr;
	pgd_t *pgd;
	pud_t *pud;
	bool pgd_changed = false;

	for (addr = start; addr < end; addr = next) {
		next = pgd_addr_end(addr, end);

		pgd = pgd_offset_k(addr);
		if (!pgd_present(*pgd))
			continue;

		pud = (pud_t *)pgd_page_vaddr(*pgd);
		remove_pud_table(pud, addr, next, direct);
		if (free_pud_table(pud, pgd))
			pgd_changed = true;
	}

	if (pgd_changed)
		sync_global_pgds(start, end - 1, 1);

	flush_tlb_all();
}

void __ref vmemmap_free(unsigned long start, unsigned long end)
{
	remove_pagetable(start, end, false);
}

#ifdef CONFIG_MEMORY_HOTREMOVE
static void __meminit
kernel_physical_mapping_remove(unsigned long start, unsigned long end)
{
	start = (unsigned long)__va(start);
	end = (unsigned long)__va(end);

	remove_pagetable(start, end, true);
}

int __ref arch_remove_memory(u64 start, u64 size)
{
	unsigned long start_pfn = start >> PAGE_SHIFT;
	unsigned long nr_pages = size >> PAGE_SHIFT;
	struct zone *zone;
	int ret;

	zone = page_zone(pfn_to_page(start_pfn));
	kernel_physical_mapping_remove(start, start + size);
	ret = __remove_pages(zone, start_pfn, nr_pages);
	WARN_ON_ONCE(ret);

	return ret;
}
#endif
#endif /* CONFIG_MEMORY_HOTPLUG */

static struct kcore_list kcore_vsyscall;

static void __init register_page_bootmem_info(void)
{
#ifdef CONFIG_NUMA
	int i;

	for_each_online_node(i)
		register_page_bootmem_info_node(NODE_DATA(i));
#endif
}

void __init mem_init(void)
{
	unsigned long pfn;

	pci_iommu_alloc();

	/* clear_bss() already clear the empty_zero_page */

	register_page_bootmem_info();

	/* this will put all memory onto the freelists */
	free_all_bootmem();

	/* XEN: init pages outside initial allocation. */
	for (pfn = xen_start_info->nr_pages; pfn < max_pfn; pfn++) {
		ClearPageReserved(pfn_to_page(pfn));
		init_page_count(pfn_to_page(pfn));
	}

	after_bootmem = 1;

	/* Register memory areas for /proc/kcore */
	kclist_add(&kcore_vsyscall, (void *)VSYSCALL_ADDR,
			 PAGE_SIZE, KCORE_OTHER);

	mem_init_print_info(NULL);
}

#ifdef CONFIG_DEBUG_RODATA
const int rodata_test_data = 0xC3;
EXPORT_SYMBOL_GPL(rodata_test_data);

int kernel_set_to_readonly;

void set_kernel_text_rw(void)
{
	unsigned long start = PFN_ALIGN(_text);
	unsigned long end = PFN_ALIGN(__stop___ex_table);

	if (!kernel_set_to_readonly)
		return;

	pr_debug("Set kernel text: %lx - %lx for read write\n",
		 start, end);

	/*
	 * Make the kernel identity mapping for text RW. Kernel text
	 * mapping will always be RO. Refer to the comment in
	 * static_protections() in pageattr.c
	 */
	set_memory_rw(start, (end - start) >> PAGE_SHIFT);
}

void set_kernel_text_ro(void)
{
	unsigned long start = PFN_ALIGN(_text);
	unsigned long end = PFN_ALIGN(__stop___ex_table);

	if (!kernel_set_to_readonly)
		return;

	pr_debug("Set kernel text: %lx - %lx for read only\n",
		 start, end);

	/*
	 * Set the kernel identity mapping for text RO.
	 */
	set_memory_ro(start, (end - start) >> PAGE_SHIFT);
}

void mark_rodata_ro(void)
{
	unsigned long start = PFN_ALIGN(_text);
	unsigned long rodata_start = PFN_ALIGN(__start_rodata);
	unsigned long end = (unsigned long) &__end_rodata;
	unsigned long text_end = PFN_ALIGN(&__stop___ex_table);
	unsigned long rodata_end = PFN_ALIGN(&__end_rodata);
	unsigned long all_end;

	printk(KERN_INFO "Write protecting the kernel read-only data: %luk\n",
	       (end - start) >> 10);
	set_memory_ro(start, (end - start) >> PAGE_SHIFT);

	kernel_set_to_readonly = 1;

	/*
	 * The rodata/data/bss/brk section (but not the kernel text!)
	 * should also be not-executable.
	 *
	 * We align all_end to PMD_SIZE because the existing mapping
	 * is a full PMD. If we would align _brk_end to PAGE_SIZE we
	 * split the PMD and the reminder between _brk_end and the end
	 * of the PMD will remain mapped executable.
	 *
	 * Any PMD which was setup after the one which covers _brk_end
	 * has been zapped already via cleanup_highmem().
	 */
#ifdef CONFIG_XEN /* Except on Xen, where we don't use 2Mb mappings. */
	all_end = PFN_ALIGN(_brk_end);
#else
	all_end = roundup((unsigned long)_brk_end, PMD_SIZE);
#endif
	set_memory_nx(rodata_start, (all_end - rodata_start) >> PAGE_SHIFT);

	rodata_test();

#ifdef CONFIG_CPA_DEBUG
	printk(KERN_INFO "Testing CPA: undo %lx-%lx\n", start, end);
	set_memory_rw(start, (end-start) >> PAGE_SHIFT);

	printk(KERN_INFO "Testing CPA: again\n");
	set_memory_ro(start, (end-start) >> PAGE_SHIFT);
#endif

	free_init_pages("unused kernel",
			(unsigned long) __va(__pa_symbol(text_end)),
			(unsigned long) __va(__pa_symbol(rodata_start)));
	free_init_pages("unused kernel",
			(unsigned long) __va(__pa_symbol(rodata_end)),
			(unsigned long) __va(__pa_symbol(_sdata)));
}

#endif

int kern_addr_valid(unsigned long addr)
{
	unsigned long above = ((long)addr) >> __VIRTUAL_MASK_SHIFT;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	if (above != 0 && above != -1UL)
		return 0;

#ifdef CONFIG_XEN
	/*
	 * Don't walk page tables for hypervisor addresses, but allow
	 * the M2P table to be accessed through e.g. /proc/kcore.
	 */
	if (addr >= (unsigned long)machine_to_phys_mapping &&
	    addr < (unsigned long)(machine_to_phys_mapping +
				   machine_to_phys_nr))
		return 1;
	if (addr >= HYPERVISOR_VIRT_START && addr < HYPERVISOR_VIRT_END)
		return 0;
#endif

	pgd = pgd_offset_k(addr);
	if (pgd_none(*pgd))
		return 0;

	pud = pud_offset(pgd, addr);
	if (pud_none(*pud))
		return 0;

	if (pud_large(*pud))
		return pfn_valid(pud_pfn(*pud));

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return 0;

	if (pmd_large(*pmd))
		return pfn_valid(pmd_pfn(*pmd));

	pte = pte_offset_kernel(pmd, addr);
	if (pte_none(*pte))
		return 0;

	return pfn_valid(pte_pfn(*pte));
}

#ifdef CONFIG_MEMORY_HOTPLUG_SPARSE
static unsigned long probe_memory_block_size(void)
{
	/* start from 2g */
	unsigned long bz = 1UL<<31;

	if (totalram_pages >= (64ULL << (30 - PAGE_SHIFT))) {
		pr_info("Using 2GB memory block size for large-memory system\n");
		return 2UL * 1024 * 1024 * 1024;
	}

	/* less than 64g installed */
	if ((max_pfn << PAGE_SHIFT) < (16UL << 32))
		return MIN_MEMORY_BLOCK_SIZE;

	/* get the tail size */
	while (bz > MIN_MEMORY_BLOCK_SIZE) {
		if (!((max_pfn << PAGE_SHIFT) & (bz - 1)))
			break;
		bz >>= 1;
	}

	printk(KERN_DEBUG "memory block size : %ldMB\n", bz >> 20);

	return bz;
}

static unsigned long memory_block_size_probed;
unsigned long memory_block_size_bytes(void)
{
	if (!memory_block_size_probed)
		memory_block_size_probed = probe_memory_block_size();

	return memory_block_size_probed;
}
#endif

#ifdef CONFIG_SPARSEMEM_VMEMMAP
/*
 * Initialise the sparsemem vmemmap using huge-pages at the PMD level.
 */
static long __meminitdata addr_start, addr_end;
static void __meminitdata *p_start, *p_end;
static int __meminitdata node_start;

static int __meminit vmemmap_populate_hugepages(unsigned long start,
						unsigned long end, int node)
{
	unsigned long addr;
	unsigned long next;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

	for (addr = start; addr < end; addr = next) {
		next = pmd_addr_end(addr, end);

		pgd = vmemmap_pgd_populate(addr, node);
		if (!pgd)
			return -ENOMEM;

		pud = vmemmap_pud_populate(pgd, addr, node);
		if (!pud)
			return -ENOMEM;

		pmd = pmd_offset(pud, addr);
		if (pmd_none(*pmd)) {
			void *p;

			p = vmemmap_alloc_block_buf(PMD_SIZE, node);
			if (p) {
				pte_t entry;

				entry = pfn_pte(__pa(p) >> PAGE_SHIFT,
						PAGE_KERNEL_LARGE);
				set_pmd(pmd, __pmd_ma(__pte_val(entry)));

				/* check to see if we have contiguous blocks */
				if (p_end != p || node_start != node) {
					if (p_start)
						printk(KERN_DEBUG " [%lx-%lx] PMD -> [%p-%p] on node %d\n",
						       addr_start, addr_end-1, p_start, p_end-1, node_start);
					addr_start = addr;
					node_start = node;
					p_start = p;
				}

				addr_end = addr + PMD_SIZE;
				p_end = p + PMD_SIZE;
				continue;
			}
		} else if (pmd_large(*pmd)) {
			vmemmap_verify((pte_t *)pmd, node, addr, next);
			continue;
		}
		pr_warn_once("vmemmap: falling back to regular page backing\n");
		if (vmemmap_populate_basepages(addr, next, node))
			return -ENOMEM;
	}
	return 0;
}

int __meminit vmemmap_populate(unsigned long start, unsigned long end, int node)
{
	int err;

	if (cpu_has_pse)
		err = vmemmap_populate_hugepages(start, end, node);
	else
		err = vmemmap_populate_basepages(start, end, node);
	if (!err)
		sync_global_pgds(start, end - 1, 0);
	return err;
}

#if defined(CONFIG_MEMORY_HOTPLUG_SPARSE) && defined(CONFIG_HAVE_BOOTMEM_INFO_NODE)
void register_page_bootmem_memmap(unsigned long section_nr,
				  struct page *start_page, unsigned long size)
{
	unsigned long addr = (unsigned long)start_page;
	unsigned long end = (unsigned long)(start_page + size);
	unsigned long next;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	unsigned int nr_pages;
	struct page *page;

	for (; addr < end; addr = next) {
		pte_t *pte = NULL;

		pgd = pgd_offset_k(addr);
		if (pgd_none(*pgd)) {
			next = (addr + PAGE_SIZE) & PAGE_MASK;
			continue;
		}
		get_page_bootmem(section_nr, pgd_page(*pgd), MIX_SECTION_INFO);

		pud = pud_offset(pgd, addr);
		if (pud_none(*pud)) {
			next = (addr + PAGE_SIZE) & PAGE_MASK;
			continue;
		}
		get_page_bootmem(section_nr, pud_page(*pud), MIX_SECTION_INFO);

		if (!cpu_has_pse) {
			next = (addr + PAGE_SIZE) & PAGE_MASK;
			pmd = pmd_offset(pud, addr);
			if (pmd_none(*pmd))
				continue;
			get_page_bootmem(section_nr, pmd_page(*pmd),
					 MIX_SECTION_INFO);

			pte = pte_offset_kernel(pmd, addr);
			if (pte_none(*pte))
				continue;
			get_page_bootmem(section_nr, pte_page(*pte),
					 SECTION_INFO);
		} else {
			next = pmd_addr_end(addr, end);

			pmd = pmd_offset(pud, addr);
			if (pmd_none(*pmd))
				continue;

			nr_pages = 1 << (get_order(PMD_SIZE));
			page = pmd_page(*pmd);
			while (nr_pages--)
				get_page_bootmem(section_nr, page++,
						 SECTION_INFO);
		}
	}
}
#endif

void __meminit vmemmap_populate_print_last(void)
{
	if (p_start) {
		printk(KERN_DEBUG " [%lx-%lx] PMD -> [%p-%p] on node %d\n",
			addr_start, addr_end-1, p_start, p_end-1, node_start);
		p_start = NULL;
		p_end = NULL;
		node_start = 0;
	}
}
#endif
