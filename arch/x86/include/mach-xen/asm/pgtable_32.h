#ifndef _ASM_X86_PGTABLE_32_H
#define _ASM_X86_PGTABLE_32_H

#include <asm/pgtable_32_types.h>

/*
 * The Linux memory management assumes a three-level page table setup. On
 * the i386, we use that, but "fold" the mid level into the top-level page
 * table, so that we physically have the same two-level page table as the
 * i386 mmu expects.
 *
 * This file contains the functions and defines necessary to modify and use
 * the i386 page table tree.
 */
#ifndef __ASSEMBLY__
#include <asm/processor.h>
#include <asm/fixmap.h>
#include <linux/threads.h>

#include <linux/bitops.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/sched.h>

struct vm_area_struct;

extern pgd_t *swapper_pg_dir;
extern pgd_t initial_page_table[1024];

static inline void pgtable_cache_init(void) { }
static inline void check_pgt_cache(void) { }
void paging_init(void);

/*
 * Define this if things work differently on an i386 and an i486:
 * it will (on an i486) warn about kernel memory accesses that are
 * done without a 'access_ok(VERIFY_WRITE,..)'
 */
#undef TEST_ACCESS_OK

#ifdef CONFIG_X86_PAE
# include <asm/pgtable-3level.h>
#else
# include <asm/pgtable-2level.h>
#endif

#if defined(CONFIG_HIGHPTE)
#define pte_offset_map(dir, address)					\
	((pte_t *)kmap_atomic_pte(pmd_page(*(dir))) +		\
	 pte_index((address)))
#define pte_unmap(pte) kunmap_atomic((pte))
#else
#define pte_offset_map(dir, address)					\
	((pte_t *)page_address(pmd_page(*(dir))) + pte_index((address)))
#define pte_unmap(pte) do { } while (0)
#endif

/* Clear a kernel PTE and flush it from the TLB */
#define kpte_clear_flush(ptep, vaddr)					\
do {									\
	if (HYPERVISOR_update_va_mapping(vaddr, __pte(0), UVMF_INVLPG)) \
		BUG(); \
} while (0)

void make_lowmem_page_readonly(void *va, unsigned int feature);
void make_lowmem_page_writable(void *va, unsigned int feature);

#endif /* !__ASSEMBLY__ */

/*
 * kern_addr_valid() is (1) for FLATMEM and (0) for
 * SPARSEMEM and DISCONTIGMEM
 */
#ifdef CONFIG_FLATMEM
#define kern_addr_valid(addr)	(1)
#else
#define kern_addr_valid(kaddr)	(0)
#endif

#endif /* _ASM_X86_PGTABLE_32_H */
