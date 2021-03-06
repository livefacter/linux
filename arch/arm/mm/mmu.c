/*
 *  linux/arch/arm/mm/mmu.c
 *
 *  Copyright (C) 1995-2005 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/memblock.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/sizes.h>

#include <asm/cp15.h>
#include <asm/cputype.h>
#include <asm/sections.h>
#include <asm/cachetype.h>
#include <asm/fixmap.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/smp_plat.h>
#include <asm/tlb.h>
#include <asm/highmem.h>
#include <asm/system_info.h>
#include <asm/traps.h>
#include <asm/procinfo.h>
#include <asm/memory.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/pci.h>
#include <asm/fixmap.h>

#include "fault.h"
#include "mm.h"
#include "tcm.h"

/*
 * empty_zero_page is a special page that is used for
 * zero-initialized data and COW.
 */
struct page *empty_zero_page;
EXPORT_SYMBOL(empty_zero_page);

/*
 * The pmd table for the upper-most set of pages.
 */
pmd_t *top_pmd;

pmdval_t user_pmd_table = _PAGE_USER_TABLE;

#define CPOLICY_UNCACHED	0
#define CPOLICY_BUFFERED	1
#define CPOLICY_WRITETHROUGH	2
#define CPOLICY_WRITEBACK	3
#define CPOLICY_WRITEALLOC	4

static unsigned int cachepolicy __initdata = CPOLICY_WRITEBACK;
static unsigned int ecc_mask __initdata = 0;
pgprot_t pgprot_user;
pgprot_t pgprot_kernel;
pgprot_t pgprot_hyp_device;
pgprot_t pgprot_s2;
pgprot_t pgprot_s2_device;

EXPORT_SYMBOL(pgprot_user);
EXPORT_SYMBOL(pgprot_kernel);

struct cachepolicy {
	const char	policy[16];
	unsigned int	cr_mask;
	pmdval_t	pmd;
	pteval_t	pte;
	pteval_t	pte_s2;
};

#ifdef CONFIG_ARM_LPAE
#define s2_policy(policy)	policy
#else
#define s2_policy(policy)	0
#endif

static struct cachepolicy cache_policies[] __initdata = {
	{
		.policy		= "uncached",
		.cr_mask	= CR_W|CR_C,
		.pmd		= PMD_SECT_UNCACHED,
		.pte		= L_PTE_MT_UNCACHED,
		.pte_s2		= s2_policy(L_PTE_S2_MT_UNCACHED),
	}, {
		.policy		= "buffered",
		.cr_mask	= CR_C,
		.pmd		= PMD_SECT_BUFFERED,
		.pte		= L_PTE_MT_BUFFERABLE,
		.pte_s2		= s2_policy(L_PTE_S2_MT_UNCACHED),
	}, {
		.policy		= "writethrough",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WT,
		.pte		= L_PTE_MT_WRITETHROUGH,
		.pte_s2		= s2_policy(L_PTE_S2_MT_WRITETHROUGH),
	}, {
		.policy		= "writeback",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WB,
		.pte		= L_PTE_MT_WRITEBACK,
		.pte_s2		= s2_policy(L_PTE_S2_MT_WRITEBACK),
	}, {
		.policy		= "writealloc",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WBWA,
		.pte		= L_PTE_MT_WRITEALLOC,
		.pte_s2		= s2_policy(L_PTE_S2_MT_WRITEBACK),
	/*@Iamroot 170422
	 * L_PTE : Linux PTE
	 */

	}
};

#ifdef CONFIG_CPU_CP15
static unsigned long initial_pmd_value __initdata = 0;

/*
 * Initialise the cache_policy variable with the initial state specified
 * via the "pmd" value.  This is used to ensure that on ARMv6 and later,
 * the C code sets the page tables up with the same policy as the head
 * assembly code, which avoids an illegal state where the TLBs can get
 * confused.  See comments in early_cachepolicy() for more information.
 */
void __init init_default_cache_policy(unsigned long pmd)
{
	int i;

	initial_pmd_value = pmd;

	pmd &= PMD_SECT_TEX(1) | PMD_SECT_BUFFERABLE | PMD_SECT_CACHEABLE;
#if 0  /* @Iamroot: 2017.01.21 */
        PMD_SECT_WBWA : (PMD_SECT_TEX(1) | PMD_SECT_CACHEABLE | PMD_SECT_BUFFERABLE)
        cache_policies[]의 각 pmd 값과 위의 pmd의 값이 일치 하는것을 for문을 이용하여 찾는다
        현재는 WBWA (arch/arm/mm/proc-v7.S - mmu_flags) 
        manual p.1367 참조 
#endif /* @Iamroot  */
	for (i = 0; i < ARRAY_SIZE(cache_policies); i++)
		if (cache_policies[i].pmd == pmd) {
			cachepolicy = i;
			break;
		}

	if (i == ARRAY_SIZE(cache_policies))
		pr_err("ERROR: could not find cache policy\n");
}

/*
 * These are useful for identifying cache coherency problems by allowing
 * the cache or the cache and writebuffer to be turned off.  (Note: the
 * write buffer should not be on and the cache off).
 */
static int __init early_cachepolicy(char *p)
{
	int i, selected = -1;

	for (i = 0; i < ARRAY_SIZE(cache_policies); i++) {
		int len = strlen(cache_policies[i].policy);

		if (memcmp(p, cache_policies[i].policy, len) == 0) {
			selected = i;
			break;
		}
	}

	if (selected == -1)
		pr_err("ERROR: unknown or unsupported cache policy\n");

	/*
	 * This restriction is partly to do with the way we boot; it is
	 * unpredictable to have memory mapped using two different sets of
	 * memory attributes (shared, type, and cache attribs).  We can not
	 * change these attributes once the initial assembly has setup the
	 * page tables.
	 */
	if (cpu_architecture() >= CPU_ARCH_ARMv6 && selected != cachepolicy) {
		pr_warn("Only cachepolicy=%s supported on ARMv6 and later\n",
			cache_policies[cachepolicy].policy);
		return 0;
	}

	if (selected != cachepolicy) {
		unsigned long cr = __clear_cr(cache_policies[selected].cr_mask);
		cachepolicy = selected;
		flush_cache_all();
		set_cr(cr);
	}
	return 0;
}
early_param("cachepolicy", early_cachepolicy);

static int __init early_nocache(char *__unused)
{
	char *p = "buffered";
	pr_warn("nocache is deprecated; use cachepolicy=%s\n", p);
	early_cachepolicy(p);
	return 0;
}
early_param("nocache", early_nocache);

static int __init early_nowrite(char *__unused)
{
	char *p = "uncached";
	pr_warn("nowb is deprecated; use cachepolicy=%s\n", p);
	early_cachepolicy(p);
	return 0;
}
early_param("nowb", early_nowrite);

#ifndef CONFIG_ARM_LPAE
static int __init early_ecc(char *p)
{
	if (memcmp(p, "on", 2) == 0)
		ecc_mask = PMD_PROTECTION;
	else if (memcmp(p, "off", 3) == 0)
		ecc_mask = 0;
	return 0;
}
early_param("ecc", early_ecc);
#endif

#else /* ifdef CONFIG_CPU_CP15 */

static int __init early_cachepolicy(char *p)
{
	pr_warn("cachepolicy kernel parameter not supported without cp15\n");
}
early_param("cachepolicy", early_cachepolicy);

static int __init noalign_setup(char *__unused)
{
	pr_warn("noalign kernel parameter not supported without cp15\n");
}
__setup("noalign", noalign_setup);

#endif /* ifdef CONFIG_CPU_CP15 / else */

#define PROT_PTE_DEVICE		L_PTE_PRESENT|L_PTE_YOUNG|L_PTE_DIRTY|L_PTE_XN
#define PROT_PTE_S2_DEVICE	PROT_PTE_DEVICE
#define PROT_SECT_DEVICE	PMD_TYPE_SECT|PMD_SECT_AP_WRITE

static struct mem_type mem_types[] = {
	[MT_DEVICE] = {		  /* Strongly ordered / ARMv6 shared device */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_SHARED |
				  L_PTE_SHARED,
		.prot_pte_s2	= s2_policy(PROT_PTE_S2_DEVICE) |
				  s2_policy(L_PTE_S2_MT_DEV_SHARED) |
				  L_PTE_SHARED,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE | PMD_SECT_S,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_NONSHARED] = { /* ARMv6 non-shared device */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_NONSHARED,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_CACHED] = {	  /* ioremap_cached */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_CACHED,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE | PMD_SECT_WB,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_WC] = {	/* ioremap_wc */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_WC,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE,
		.domain		= DOMAIN_IO,
	},
	[MT_UNCACHED] = {
		.prot_pte	= PROT_PTE_DEVICE,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PMD_TYPE_SECT | PMD_SECT_XN,
		.domain		= DOMAIN_IO,
	},
	[MT_CACHECLEAN] = {
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
#ifndef CONFIG_ARM_LPAE
	[MT_MINICLEAN] = {
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN | PMD_SECT_MINICACHE,
		.domain    = DOMAIN_KERNEL,
	},
#endif
	[MT_LOW_VECTORS] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_RDONLY,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_VECTORS,
	},
	[MT_HIGH_VECTORS] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_USER | L_PTE_RDONLY,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_VECTORS,
	},
	[MT_MEMORY_RWX] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RW] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
			     L_PTE_XN,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_ROM] = {
		.prot_sect = PMD_TYPE_SECT,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RWX_NONCACHED] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_MT_BUFFERABLE,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RW_DTCM] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_XN,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RWX_ITCM] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RW_SO] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_MT_UNCACHED | L_PTE_XN,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_S |
				PMD_SECT_UNCACHED | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_DMA_READY] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_XN,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_KERNEL,
	},
};

const struct mem_type *get_mem_type(unsigned int type)
{
	return type < ARRAY_SIZE(mem_types) ? &mem_types[type] : NULL;
}
EXPORT_SYMBOL(get_mem_type);

static pte_t *(*pte_offset_fixmap)(pmd_t *dir, unsigned long addr);

static pte_t bm_pte[PTRS_PER_PTE + PTE_HWTABLE_PTRS]
	__aligned(PTE_HWTABLE_OFF + PTE_HWTABLE_SIZE) __initdata;

static pte_t * __init pte_offset_early_fixmap(pmd_t *dir, unsigned long addr)
{
	return &bm_pte[pte_index(addr)];
}

static pte_t *pte_offset_late_fixmap(pmd_t *dir, unsigned long addr)
{
	return pte_offset_kernel(dir, addr);
}

static inline pmd_t * __init fixmap_pmd(unsigned long addr)
{
	pgd_t *pgd = pgd_offset_k(addr);
	pud_t *pud = pud_offset(pgd, addr);
	pmd_t *pmd = pmd_offset(pud, addr);

	return pmd;
}

void __init early_fixmap_init(void)
{
	pmd_t *pmd;
	/*@Iamroot 170311
	 * pmd : page middle directory
	 */

	/*
	 * The early fixmap range spans multiple pmds, for which
	 * we are not prepared:
	 */
	BUILD_BUG_ON((__fix_to_virt(__end_of_early_ioremap_region) >> PMD_SHIFT)
		     != FIXADDR_TOP >> PMD_SHIFT);
	
	/*@Iamroot 170311
	 * fix_pmd()는 pgd, pud, pmd, pte's offset를 가져온다.
	 * raspberry pi2는 2-level page table이므로 pgd, pte만 사용한다.
	 */
	pmd = fixmap_pmd(FIXADDR_TOP);
	pmd_populate_kernel(&init_mm, pmd, bm_pte);

	pte_offset_fixmap = pte_offset_early_fixmap;
}

/*
 * To avoid TLB flush broadcasts, this uses local_flush_tlb_kernel_range().
 * As a result, this can only be called with preemption disabled, as under
 * stop_machine().
 */
void __set_fixmap(enum fixed_addresses idx, phys_addr_t phys, pgprot_t prot)
{
	unsigned long vaddr = __fix_to_virt(idx);
	pte_t *pte = pte_offset_fixmap(pmd_off_k(vaddr), vaddr);

	/* Make sure fixmap region does not exceed available allocation. */
	BUILD_BUG_ON(FIXADDR_START + (__end_of_fixed_addresses * PAGE_SIZE) >
		     FIXADDR_END);
	BUG_ON(idx >= __end_of_fixed_addresses);

	if (pgprot_val(prot))
		set_pte_at(NULL, vaddr, pte,
			pfn_pte(phys >> PAGE_SHIFT, prot));
	else
		pte_clear(NULL, vaddr, pte);
	local_flush_tlb_kernel_range(vaddr, vaddr + PAGE_SIZE);
}

/*
 * Adjust the PMD section entries according to the CPU in use.
 */

#if 0  /* @Iamroot: 2017.04.29 */
		HW/linux pagetable properties 
#endif /* @Iamroot  */
static void __init build_mem_type_table(void)
{
	struct cachepolicy *cp;
	unsigned int cr = get_cr();
	pteval_t user_pgprot, kern_pgprot, vecs_pgprot;
	pteval_t hyp_device_pgprot, s2_pgprot, s2_device_pgprot;
	int cpu_arch = cpu_architecture();
	int i;

	if (cpu_arch < CPU_ARCH_ARMv6) {
#if defined(CONFIG_CPU_DCACHE_DISABLE)
		if (cachepolicy > CPOLICY_BUFFERED)
			cachepolicy = CPOLICY_BUFFERED;
#elif defined(CONFIG_CPU_DCACHE_WRITETHROUGH)
		if (cachepolicy > CPOLICY_WRITETHROUGH)
			cachepolicy = CPOLICY_WRITETHROUGH;
#endif
	}
	if (cpu_arch < CPU_ARCH_ARMv5) {
		if (cachepolicy >= CPOLICY_WRITEALLOC)
			cachepolicy = CPOLICY_WRITEBACK;
		ecc_mask = 0;
	}

	if (is_smp()) {
		if (cachepolicy != CPOLICY_WRITEALLOC) {
			pr_warn("Forcing write-allocate cache policy for SMP\n");
			cachepolicy = CPOLICY_WRITEALLOC;
		}
		if (!(initial_pmd_value & PMD_SECT_S)) {
			pr_warn("Forcing shared mappings for SMP\n");
			initial_pmd_value |= PMD_SECT_S;
#if 0  /* @Iamroot: 2017.04.15 */
                        SMP에서 각 CPU가 해당 page를 공유할수 있도록 S bit를 활성화 함
                        p.1329
#endif /* @Iamroot  */
		}
	}

	/*
	 * Strip out features not present on earlier architectures.
	 * Pre-ARMv5 CPUs don't have TEX bits.  Pre-ARMv6 CPUs or those
	 * without extended page tables don't have the 'Shared' bit.
	 */
	if (cpu_arch < CPU_ARCH_ARMv5)
		for (i = 0; i < ARRAY_SIZE(mem_types); i++)
			mem_types[i].prot_sect &= ~PMD_SECT_TEX(7);
	if ((cpu_arch < CPU_ARCH_ARMv6 || !(cr & CR_XP)) && !cpu_is_xsc3())
		for (i = 0; i < ARRAY_SIZE(mem_types); i++)
			mem_types[i].prot_sect &= ~PMD_SECT_S;

	/*
	 * ARMv5 and lower, bit 4 must be set for page tables (was: cache
	 * "update-able on write" bit on ARM610).  However, Xscale and
	 * Xscale3 require this bit to be cleared.
	 */
	if (cpu_is_xscale_family()) {
		for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
			mem_types[i].prot_sect &= ~PMD_BIT4;
			mem_types[i].prot_l1 &= ~PMD_BIT4;
		}
	} else if (cpu_arch < CPU_ARCH_ARMv6) {
		for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
			if (mem_types[i].prot_l1)
				mem_types[i].prot_l1 |= PMD_BIT4;
			if (mem_types[i].prot_sect)
				mem_types[i].prot_sect |= PMD_BIT4;
		}
	}

	/*
	 * Mark the device areas according to the CPU/architecture.
	 */
	if (cpu_is_xsc3() || (cpu_arch >= CPU_ARCH_ARMv6 && (cr & CR_XP))) {
		if (!cpu_is_xsc3()) {
			/*
			 * Mark device regions on ARMv6+ as execute-never
			 * to prevent speculative instruction fetches.
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_CACHED].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_XN;
#if 0  /* @Iamroot: 2017.04.15 */
                        xsc3를 제외한 나머지에서는 해당 디바이스 메모리를 사용할수 없도록
                        설정한다
#endif /* @Iamroot  */
			/* Also setup NX memory mapping */
			mem_types[MT_MEMORY_RW].prot_sect |= PMD_SECT_XN;
		}
		if (cpu_arch >= CPU_ARCH_ARMv7 && (cr & CR_TRE)) {
			/*
			 * For ARMv7 with TEX remapping,
			 * - shared device is SXCB=1100
			 * - nonshared device is SXCB=0100
			 * - write combine device mem is SXCB=0001
			 * (Uncached Normal memory)
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_TEX(1);
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(1);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_BUFFERABLE;
#if 0  /* @Iamroot: 2017.04.15 */
			S : Sharable bit
			X : TEX[0] bit
			C : Cacheable bit
			B : Bufferable bit
			mem_types table : overhead를 줄이기 위해 memory type's property을 미리 설정한다.
			(shared, nonshared, write combine)
#endif /* @Iamroot  */
		} else if (cpu_is_xsc3()) {
			/*
			 * For Xscale3,
			 * - shared device is TEXCB=00101
			 * - nonshared device is TEXCB=01000
			 * - write combine device mem is TEXCB=00100
			 * (Inner/Outer Uncacheable in xsc3 parlance)
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_TEX(1) | PMD_SECT_BUFFERED;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(2);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_TEX(1);
		} else {
			/*
			 * For ARMv6 and ARMv7 without TEX remapping,
			 * - shared device is TEXCB=00001
			 * - nonshared device is TEXCB=01000
			 * - write combine device mem is TEXCB=00100
			 * (Uncached Normal in ARMv6 parlance).
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_BUFFERED;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(2);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_TEX(1);
		}
	} else {
		/*
		 * On others, write combining is "Uncached/Buffered"
		 */
		mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_BUFFERABLE;
	}

	/*
	 * Now deal with the memory-type mappings
	 */
	cp = &cache_policies[cachepolicy];
	vecs_pgprot = kern_pgprot = user_pgprot = cp->pte;
#if 0  /* @Iamroot: 2017.04.15 */
vecs : vectors
		   vectors. kernel, user property를 L_PTA_WRITE_ALLOC로 설정
#endif /* @Iamroot  */
	s2_pgprot = cp->pte_s2;
	hyp_device_pgprot = mem_types[MT_DEVICE].prot_pte;
	s2_device_pgprot = mem_types[MT_DEVICE].prot_pte_s2;
	/*@Iamroot 170422
	 * s2는 stage2 약어로 hypervisor에서 구동되는 mmu table 중 하나다.
	 * app <-> stage1 mmu <-> OS <-> stage2 mmu <-> phys memory
	 */

#ifndef CONFIG_ARM_LPAE
	/*
	 * We don't use domains on ARMv6 (since this causes problems with
	 * v6/v7 kernels), so we must use a separate memory type for user
	 * r/o, kernel r/w to map the vectors page.
	 */
	if (cpu_arch == CPU_ARCH_ARMv6)
		vecs_pgprot |= L_PTE_MT_VECTORS;

	/*
	 * Check is it with support for the PXN bit
	 * in the Short-descriptor translation table format descriptors.
	 */
	if (cpu_arch == CPU_ARCH_ARMv7 &&
		(read_cpuid_ext(CPUID_EXT_MMFR0) & 0xF) >= 4) {
		user_pmd_table |= PMD_PXNTABLE;
	}
	/*@Iamroot 170422
	 * kernel모드일 때 user영역의 코드를 실행못하게 설정
	 */

#endif

	/*
	 * ARMv6 and above have extended page tables.
	 */
	if (cpu_arch >= CPU_ARCH_ARMv6 && (cr & CR_XP)) {
#ifndef CONFIG_ARM_LPAE
		/*
		 * Mark cache clean areas and XIP ROM read only
		 * from SVC mode and no access from userspace.
		 */
		mem_types[MT_ROM].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
		mem_types[MT_MINICLEAN].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
	/*@Iamroot 170422
	 * MT : Memory Type
	 * MT_ROM을 Read-only, only at PL1 or higher로 설정
	 * MINCLEAN과 CACHECLEAN은 Cache Flush Region 아키텍처에 사용
	 */

#endif

		/*
		 * If the initial page tables were created with the S bit
		 * set, then we need to do the same here for the same
		 * reasons given in early_cachepolicy().
		 */
		/*@Iamroot 170422
		 *다음 시간에...
		 */
		if (initial_pmd_value & PMD_SECT_S) {
			user_pgprot |= L_PTE_SHARED;
			kern_pgprot |= L_PTE_SHARED;
			vecs_pgprot |= L_PTE_SHARED;
			s2_pgprot |= L_PTE_SHARED;
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_S;
			mem_types[MT_DEVICE_WC].prot_pte |= L_PTE_SHARED;
			mem_types[MT_DEVICE_CACHED].prot_sect |= PMD_SECT_S;
			mem_types[MT_DEVICE_CACHED].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_RWX].prot_sect |= PMD_SECT_S;
			mem_types[MT_MEMORY_RWX].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_RW].prot_sect |= PMD_SECT_S;
			mem_types[MT_MEMORY_RW].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_DMA_READY].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |= PMD_SECT_S;
			mem_types[MT_MEMORY_RWX_NONCACHED].prot_pte |= L_PTE_SHARED;
		}
	}

	/*
	 * Non-cacheable Normal - intended for memory areas that must
	 * not cause dirty cache line writebacks when used
	 */
	if (cpu_arch >= CPU_ARCH_ARMv6) {
		if (cpu_arch >= CPU_ARCH_ARMv7 && (cr & CR_TRE)) {
			/* Non-cacheable Normal is XCB = 001 */
			mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |=
				PMD_SECT_BUFFERED;
		} else {
			/* For both ARMv6 and non-TEX-remapping ARMv7 */
			mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |=
				PMD_SECT_TEX(1);
		}
	} else {
		mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |= PMD_SECT_BUFFERABLE;
	}

#ifdef CONFIG_ARM_LPAE
	/*
	 * Do not generate access flag faults for the kernel mappings.
	 */
	for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
		mem_types[i].prot_pte |= PTE_EXT_AF;
		if (mem_types[i].prot_sect)
			mem_types[i].prot_sect |= PMD_SECT_AF;
	}
	kern_pgprot |= PTE_EXT_AF;
	vecs_pgprot |= PTE_EXT_AF;

	/*
	 * Set PXN for user mappings
	 */
	user_pgprot |= PTE_EXT_PXN;
#endif

	for (i = 0; i < 16; i++) {
		pteval_t v = pgprot_val(protection_map[i]);
		protection_map[i] = __pgprot(v | user_pgprot);
	}

	mem_types[MT_LOW_VECTORS].prot_pte |= vecs_pgprot;
	mem_types[MT_HIGH_VECTORS].prot_pte |= vecs_pgprot;

	pgprot_user   = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG | user_pgprot);
	pgprot_kernel = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG |
				 L_PTE_DIRTY | kern_pgprot);
	pgprot_s2  = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG | s2_pgprot);
	pgprot_s2_device  = __pgprot(s2_device_pgprot);
	pgprot_hyp_device  = __pgprot(hyp_device_pgprot);

	mem_types[MT_LOW_VECTORS].prot_l1 |= ecc_mask;
	mem_types[MT_HIGH_VECTORS].prot_l1 |= ecc_mask;
	mem_types[MT_MEMORY_RWX].prot_sect |= ecc_mask | cp->pmd;
	mem_types[MT_MEMORY_RWX].prot_pte |= kern_pgprot;
	mem_types[MT_MEMORY_RW].prot_sect |= ecc_mask | cp->pmd;
	mem_types[MT_MEMORY_RW].prot_pte |= kern_pgprot;
	mem_types[MT_MEMORY_DMA_READY].prot_pte |= kern_pgprot;
	mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |= ecc_mask;
	mem_types[MT_ROM].prot_sect |= cp->pmd;

	switch (cp->pmd) {
	case PMD_SECT_WT:
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_WT;
		break;
	case PMD_SECT_WB:
	case PMD_SECT_WBWA:
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_WB;
		break;
	}
	pr_info("Memory policy: %sData cache %s\n",
		ecc_mask ? "ECC enabled, " : "", cp->policy);

	for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
		struct mem_type *t = &mem_types[i];
		if (t->prot_l1)
			t->prot_l1 |= PMD_DOMAIN(t->domain);
		if (t->prot_sect)
			t->prot_sect |= PMD_DOMAIN(t->domain);
	}
}

#ifdef CONFIG_ARM_DMA_MEM_BUFFERABLE
pgprot_t phys_mem_access_prot(struct file *file, unsigned long pfn,
			      unsigned long size, pgprot_t vma_prot)
{
	if (!pfn_valid(pfn))
		return pgprot_noncached(vma_prot);
	else if (file->f_flags & O_SYNC)
		return pgprot_writecombine(vma_prot);
	return vma_prot;
}
EXPORT_SYMBOL(phys_mem_access_prot);
#endif

#define vectors_base()	(vectors_high() ? 0xffff0000 : 0)

static void __init *early_alloc_aligned(unsigned long sz, unsigned long align)
{
	void *ptr = __va(memblock_alloc(sz, align));
	memset(ptr, 0, sz);
	return ptr;
}

static void __init *early_alloc(unsigned long sz)
{
	return early_alloc_aligned(sz, sz);
}

static void *__init late_alloc(unsigned long sz)
{
	void *ptr = (void *)__get_free_pages(PGALLOC_GFP, get_order(sz));

	BUG_ON(!ptr);
	return ptr;
}

static pte_t * __init arm_pte_alloc(pmd_t *pmd, unsigned long addr,
				unsigned long prot,
				void *(*alloc)(unsigned long sz))
{
	if (pmd_none(*pmd)) {
		pte_t *pte = alloc(PTE_HWTABLE_OFF + PTE_HWTABLE_SIZE);
#if 0  /* @Iamroot: 2017.05.13 */
                (512 * 4byte) + (512 * 4byte) 
                 pte 에 메모리 할당을 해준다
#endif /* @Iamroot  */
		__pmd_populate(pmd, __pa(pte), prot);
	}
	BUG_ON(pmd_bad(*pmd));
	return pte_offset_kernel(pmd, addr);
#if 0  /* @Iamroot: 2017.05.13 */
        aligned 된 pmd의 주소를 불러온다
#endif /* @Iamroot  */
}

static pte_t * __init early_pte_alloc(pmd_t *pmd, unsigned long addr,
				      unsigned long prot)
{
	return arm_pte_alloc(pmd, addr, prot, early_alloc);
}
//alloc_init_pte(pmd, addr, next,
//        __phys_to_pfn(phys), type, alloc, ng);
static void __init alloc_init_pte(pmd_t *pmd, unsigned long addr,
				  unsigned long end, unsigned long pfn,
				  const struct mem_type *type,
				  void *(*alloc)(unsigned long sz),
				  bool ng)
{
	pte_t *pte = arm_pte_alloc(pmd, addr, type->prot_l1, alloc);
	do {
		set_pte_ext(pte, pfn_pte(pfn, __pgprot(type->prot_pte)),
			    ng ? PTE_EXT_NG : 0);
#if 0  /* @Iamroot: 2017.05.13 */
                set_pte_ext의 2번째 인자가 linux pte 이기 때문에 
                h/w pte와 linux pte가 서로 다른 부분 10~5,2~1 비트부문을 클리어 한후
                다시 세팅한다
#endif /* @Iamroot  */
		pfn++;
	} while (pte++, addr += PAGE_SIZE, addr != end);
#if 0  /* @Iamroot: 2017.05.13 */
        512 번 반복한다 
        while문에서 ,를 사용할 경우 가장 마지막 항목이 조건문이 된다
        참고 : https://stackoverflow.com/questions/41611557/c-programming-comma-operator-within-while-loop
#endif /* @Iamroot  */
}

static void __init __map_init_section(pmd_t *pmd, unsigned long addr,
			unsigned long end, phys_addr_t phys,
			const struct mem_type *type, bool ng)
{
	pmd_t *p = pmd;

#ifndef CONFIG_ARM_LPAE
	/*
	 * In classic MMU format, puds and pmds are folded in to
	 * the pgds. pmd_offset gives the PGD entry. PGDs refer to a
	 * group of L1 entries making up one logical pointer to
	 * an L2 table (2MB), where as PMDs refer to the individual
	 * L1 entries (1MB). Hence increment to get the correct
	 * offset for odd 1MB sections.
	 * (See arch/arm/include/asm/pgtable-2level.h)
	 */
	if (addr & SECTION_SIZE)
		pmd++;
#if 0  /* @Iamroot: 2017.05.13 */
        pgd는 2Mb를 관리 그리고 PGD는 안에 둘로 나누어 1Mb씩 관리를 한다
        이때 addr의 인덱스 수가 홀 수 일경우 pmd의 값을 1올려 
        pgd의 인덱스를 올린다.
#endif /* @Iamroot  */
#endif
	do {
		*pmd = __pmd(phys | type->prot_sect | (ng ? PMD_SECT_nG : 0));
#if 0  /* @Iamroot: 2017.05.13 */
                pmd의 값을 mapping 한다
#endif /* @Iamroot  */
		phys += SECTION_SIZE;
	} while (pmd++, addr += SECTION_SIZE, addr != end);

	flush_pmd_entry(p);
}

//alloc_init_pmd(pud, addr, next, phys, type, alloc, ng);
static void __init alloc_init_pmd(pud_t *pud, unsigned long addr,
				      unsigned long end, phys_addr_t phys,
				      const struct mem_type *type,
				      void *(*alloc)(unsigned long sz), bool ng)
{
	pmd_t *pmd = pmd_offset(pud, addr);
	unsigned long next;

	do {
		/*
		 * With LPAE, we must loop over to map
		 * all the pmds for the given range.
		 */
		next = pmd_addr_end(addr, end);

		/*
		 * Try a section mapping - addr, next and phys must all be
		 * aligned to a section boundary.
		 */
		if (type->prot_sect &&
				((addr | next | phys) & ~SECTION_MASK) == 0) {
			__map_init_section(pmd, addr, next, phys, type, ng);
#if 0  /* @Iamroot: 2017.05.13 */
                        pgd에서 전체 1MB를 관리함
                        pgd에서 1MB 전체를 관리해야 하기때문에 반드시 
                        addr, next, phys가 전부 aligned 되어 있어야 함
#endif /* @Iamroot  */
		} else {
			alloc_init_pte(pmd, addr, next,
				       __phys_to_pfn(phys), type, alloc, ng);
		}

		phys += next - addr;

	} while (pmd++, addr = next, addr != end);
#if 0  /* @Iamroot: 2017.05.13 */
        2번 반복한다
#endif /* @Iamroot  */
}

//		alloc_init_pud(pgd, addr, next, phys, type, alloc, ng);
static void __init alloc_init_pud(pgd_t *pgd, unsigned long addr,
				  unsigned long end, phys_addr_t phys,
				  const struct mem_type *type,
				  void *(*alloc)(unsigned long sz), bool ng)
{
	pud_t *pud = pud_offset(pgd, addr);
#if 0  /* @Iamroot: 2017.05.13 */
        2 level 이기 때문에 pud == pgd
#endif /* @Iamroot  */
	unsigned long next;

	do {
		next = pud_addr_end(addr, end);
#if 0  /* @Iamroot: 2017.05.13 */
                end == next (2 level이기 때문에)
#endif /* @Iamroot  */
		alloc_init_pmd(pud, addr, next, phys, type, alloc, ng);
		phys += next - addr;
	} while (pud++, addr = next, addr != end);
}

#ifndef CONFIG_ARM_LPAE
static void __init create_36bit_mapping(struct mm_struct *mm,
					struct map_desc *md,
					const struct mem_type *type,
					bool ng)
{
	unsigned long addr, length, end;
	phys_addr_t phys;
	pgd_t *pgd;

	addr = md->virtual;
	phys = __pfn_to_phys(md->pfn);
	length = PAGE_ALIGN(md->length);

	if (!(cpu_architecture() >= CPU_ARCH_ARMv6 || cpu_is_xsc3())) {
		pr_err("MM: CPU does not support supersection mapping for 0x%08llx at 0x%08lx\n",
		       (long long)__pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	/* N.B.	ARMv6 supersections are only defined to work with domain 0.
	 *	Since domain assignments can in fact be arbitrary, the
	 *	'domain == 0' check below is required to insure that ARMv6
	 *	supersections are only allocated for domain 0 regardless
	 *	of the actual domain assignments in use.
	 */
	if (type->domain) {
		pr_err("MM: invalid domain in supersection mapping for 0x%08llx at 0x%08lx\n",
		       (long long)__pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	if ((addr | length | __pfn_to_phys(md->pfn)) & ~SUPERSECTION_MASK) {
		pr_err("MM: cannot create mapping for 0x%08llx at 0x%08lx invalid alignment\n",
		       (long long)__pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	/*
	 * Shift bits [35:32] of address into bits [23:20] of PMD
	 * (See ARMv6 spec).
	 */
	phys |= (((md->pfn >> (32 - PAGE_SHIFT)) & 0xF) << 20);

	pgd = pgd_offset(mm, addr);
	end = addr + length;
	do {
		pud_t *pud = pud_offset(pgd, addr);
		pmd_t *pmd = pmd_offset(pud, addr);
		int i;

		for (i = 0; i < 16; i++)
			*pmd++ = __pmd(phys | type->prot_sect | PMD_SECT_SUPER |
				       (ng ? PMD_SECT_nG : 0));

		addr += SUPERSECTION_SIZE;
		phys += SUPERSECTION_SIZE;
		pgd += SUPERSECTION_SIZE >> PGDIR_SHIFT;
	} while (addr != end);
}
#endif	/* !CONFIG_ARM_LPAE */
//early_alloc
static void __init __create_mapping(struct mm_struct *mm, struct map_desc *md,
				    void *(*alloc)(unsigned long sz),
				    bool ng)
{
	unsigned long addr, length, end;
	phys_addr_t phys;
	const struct mem_type *type;
	pgd_t *pgd;

	type = &mem_types[md->type];

#ifndef CONFIG_ARM_LPAE
	/*
	 * Catch 36-bit addresses
	 */
	if (md->pfn >= 0x100000) {
		create_36bit_mapping(mm, md, type, ng);
		return;
	}
#endif

	addr = md->virtual & PAGE_MASK;
    /* ex) addr = 0xabcd1234 => addr = 0xabcd1000 */
	phys = __pfn_to_phys(md->pfn);
    /* phys format = 0xabcd1000 */
	length = PAGE_ALIGN(md->length + (md->virtual & ~PAGE_MASK));
    /*
        Ex)
        md->length : 10K,
        page_offset(2K)
        md->length + page_offset = 12K

        So, 3 pages should be mapped
    */

	if (type->prot_l1 == 0 && ((addr | phys | length) & ~SECTION_MASK)) {
		pr_warn("BUG: map for 0x%08llx at 0x%08lx can not be mapped using pages, ignoring.\n",
			(long long)__pfn_to_phys(md->pfn), addr);
		return;
	}

	pgd = pgd_offset(mm, addr);
#if 0  /* @Iamroot: 2017.05.13 */
        pgd_offset에서는 mm에서 pgd의 시작주소와 addr이 위치한 pgd의 인덱스 번호를 구하여 
        더한다 즉, addr이 pgd에서 맵핑된 주소를 구한다 
#endif /* @Iamroot  */
	end = addr + length;
    /*
        pgd: translation base address + offset
        end: end virtual address
    */
	do {
        /* next cannot be over the end of pgd */
		unsigned long next = pgd_addr_end(addr, end);
#if 0  /* @Iamroot: 2017.05.13 */
                addr에 2Mb 씩 더하여 next에 대입한다. 
#endif /* @Iamroot  */
		alloc_init_pud(pgd, addr, next, phys, type, alloc, ng);

		phys += next - addr;
		addr = next;
	} while (pgd++, addr != end);
}

/*
 * Create the page directory entries and any necessary
 * page tables for the mapping specified by `md'.  We
 * are able to cope here with varying sizes and address
 * offsets, and we take full advantage of sections and
 * supersections.
 */
static void __init create_mapping(struct map_desc *md)
{
#if 0  /* @Iamroot: 2017.06.03 */
    Assign the proper value to PGD and PTE through __create_mapping()
#endif /* @Iamroot  */
	if (md->virtual != vectors_base() && md->virtual < TASK_SIZE) {
		pr_warn("BUG: not creating mapping for 0x%08llx at 0x%08lx in user region\n",
			(long long)__pfn_to_phys((u64)md->pfn), md->virtual);
		return;
	}

	if ((md->type == MT_DEVICE || md->type == MT_ROM) &&
	    md->virtual >= PAGE_OFFSET && md->virtual < FIXADDR_START &&
	    (md->virtual < VMALLOC_START || md->virtual >= VMALLOC_END)) {
		pr_warn("BUG: mapping for 0x%08llx at 0x%08lx out of vmalloc space\n",
			(long long)__pfn_to_phys((u64)md->pfn), md->virtual);
	}

#if 0  /* @Iamroot: 2017.06.03 */
    TODO: HAVE TO WATCH "early_alloc", memblock allocation routine.
#endif /* @Iamroot  */
	__create_mapping(&init_mm, md, early_alloc, false);
}

void __init create_mapping_late(struct mm_struct *mm, struct map_desc *md,
				bool ng)
{
#ifdef CONFIG_ARM_LPAE
	pud_t *pud = pud_alloc(mm, pgd_offset(mm, md->virtual), md->virtual);
	if (WARN_ON(!pud))
		return;
	pmd_alloc(mm, pud, 0);
#endif
	__create_mapping(mm, md, late_alloc, ng);
}

/*
 * Create the architecture specific mappings
 */

#if 0  /* @Iamroot: 2017.07.08 */
	문씨 블로그 참고 : 
	요청 받은 맵 정보 배열 수 만큼 static_vm 구조체 배열로 reserve memblock에 할당 받는다. 
	그리고 요청 맵 영역들을 페이지 테이블에 매핑 구성하고 
	요청 맵 영역 각각을 관리하는 구조체 엔트리인 static_vm을 구성하고 
	그 엔트리들을 static_vmlist에 추가한다.

#endif /* @Iamroot  */

void __init iotable_init(struct map_desc *io_desc, int nr)
{
	struct map_desc *md;
	struct vm_struct *vm;
	struct static_vm *svm;

	if (!nr)
		return;
#if 0  /* @Iamroot: 2017.07.08 */
	
	svm 구조체에 early_alloc_aligned함수 이용해 할당, size내 값은 모두 0
	-> 초기화한 영역 할당

#endif /* @Iamroot  */

	svm = early_alloc_aligned(sizeof(*svm) * nr, __alignof__(*svm));

#if 0  /* @Iamroot: 2017.07.08 */
    
	iotable_init(&map,1) : *io_desc = map_desc map&, nr = 1 받아옴
	결국 io_desc 는 map_desc와 동일

    nr횟수만큼(맵 요청수) md++, nr--
	create_mapping(md) : 해당하는 init_page_table값 할당 
	자세한 사항은 오른쪽 주소참고 : http://jake.dothome.co.kr/create_mapping/

#endif /* @Iamroot  */

	for (md = io_desc; nr; md++, nr--) {
		create_mapping(md);

#if 0  /* @Iamroot: 2017.07.08 */

	문씨 블로그 참고
	flags = VM_IOREMAP | VM_ARM_STATIC_MAPPING | VM_ARM_MTYPE(md->type); 
	IO 리매핑 속성, static 매핑 속성 및 메모리 타입을 플래그에 설정한다.

    ** VM_IOREMAP (web : stackoverflow) **
    VM_IOREMAP means this virtual memory region is created by ioremap(), 
	* ususally * ( but * not limited to *) to map a I/O memory region 
	( featured by its physical address ) of a hardware device ( like a PCI device) 
	into kernel virtual address range, so we can access the I/O memory by simple read / write. 

    VM : map_desc(io_desc 속성)
	svm : vm_list
#endif /* @Iamroot  */

		vm = &svm->vm;
		vm->addr = (void *)(md->virtual & PAGE_MASK);
		vm->size = PAGE_ALIGN(md->length + (md->virtual & ~PAGE_MASK));
		vm->phys_addr = __pfn_to_phys(md->pfn);
		vm->flags = VM_IOREMAP | VM_ARM_STATIC_MAPPING;
		vm->flags |= VM_ARM_MTYPE(md->type);
		vm->caller = iotable_init;
		add_static_vm_early(svm++);
	}
}

void __init vm_reserve_area_early(unsigned long addr, unsigned long size,
				  void *caller)
{
	struct vm_struct *vm;
	struct static_vm *svm;

	svm = early_alloc_aligned(sizeof(*svm), __alignof__(*svm));

	vm = &svm->vm;
	vm->addr = (void *)addr;
	vm->size = size;
	vm->flags = VM_IOREMAP | VM_ARM_EMPTY_MAPPING;
	vm->caller = caller;
	add_static_vm_early(svm);
}

#ifndef CONFIG_ARM_LPAE

/*
 * The Linux PMD is made of two consecutive section entries covering 2MB
 * (see definition in include/asm/pgtable-2level.h).  However a call to
 * create_mapping() may optimize static mappings by using individual
 * 1MB section mappings.  This leaves the actual PMD potentially half
 * initialized if the top or bottom section entry isn't used, leaving it
 * open to problems if a subsequent ioremap() or vmalloc() tries to use
 * the virtual space left free by that unused section entry.
 *
 * Let's avoid the issue by inserting dummy vm entries covering the unused
 * PMD halves once the static mappings are in place.
 */

static void __init pmd_empty_section_gap(unsigned long addr)
{
	vm_reserve_area_early(addr, SECTION_SIZE, pmd_empty_section_gap);
}
#if 0  /* @Iamroot: 2018.01.20 */
	위의  주석  번역 : 
	<Linux PMD는 2MB를 포함하는 두 개의 연속적인 섹션 항목으로 구성됩니다 
	(include / asm / pgtable-2level.h의 정의 참조).
	그러나 create_mapping ()을 호출하면 개별 1MB 섹션 매핑을 사용하여 정적 매핑을 최적화 할 수 있습니다.
	이로 인해 실제 PMD는 맨 위 또는 맨 아래 항목 항목이 사용되지 않으면 초기화되고 나머지 ioremap () 
	또는 vmalloc ()은 사용되지 않은 항목 항목에 의해 사용 가능한 가상 공간을 사용하려고 시도 할 때 
	문제가 발생하게됩니다.

	일단 정적 매핑이 제자리에 있으면 사용되지 않는 PMD 절반을 덮는 더미 VM 엔트리를 삽입하여이 
	문제를 피하십시오.>
	
    Rasberry pi는 3단계 paging 시스템
	위에서 언급한 이유로 인해 pmd의 gap을 메우는 작업진행

#endif /* @Iamroot  */
static void __init fill_pmd_gaps(void)
{
	struct static_vm *svm;
	struct vm_struct *vm;
	unsigned long addr, next = 0;
	pmd_t *pmd;

	list_for_each_entry(svm, &static_vmlist, list) {
		vm = &svm->vm;
		addr = (unsigned long)vm->addr;
		if (addr < next)
			continue;

#if 0  /* @Iamroot: 2017.08.26 */

	vector page size : 8KB, 4KB씩  pair -> 항상 짝을 이뤄야 함

	홀수 section이 비었을때의 시작 주소를 가져와서 
	홀수 section이 비어있는 마지막 주소를  탐색 
	-> 처음~마지막 주소의 홀수 section을 채운다
	
    next = (addr + PMD_SIZE - 1) & PMD_MASK;
	-> 현재 매핑한 주소의 pmd 크기(2MB)만큼 round up한 주소이상을 찾게함

#endif /* @Iamroot  */

		/*
		 * Check if this vm starts on an odd section boundary.
		 * If so and the first section entry for this PMD is free
		 * then we block the corresponding virtual address.
		 */
		if ((addr & ~PMD_MASK) == SECTION_SIZE) {
			pmd = pmd_off_k(addr);
			if (pmd_none(*pmd))
				pmd_empty_section_gap(addr & PMD_MASK);
		}

		/*
		 * Then check if this vm ends on an odd section boundary.
		 * If so and the second section entry for this PMD is empty
		 * then we block the corresponding virtual address.
		 */
		addr += vm->size;
		if ((addr & ~PMD_MASK) == SECTION_SIZE) {
			pmd = pmd_off_k(addr) + 1;
			if (pmd_none(*pmd))
				pmd_empty_section_gap(addr);
		}

		/* no need to look at any vm entry until we hit the next PMD */
		next = (addr + PMD_SIZE - 1) & PMD_MASK;
	}
}

#else
#define fill_pmd_gaps() do { } while (0)
#endif

#if 0  /* @Iamroot: 2018.01.20 */

   static_vmlist에 pci용 reserve영역을 나타내는 static_vm entry를 추가

#endif /* @Iamroot  */

#if defined(CONFIG_PCI) && !defined(CONFIG_NEED_MACH_IO_H)
static void __init pci_reserve_io(void)
{
	struct static_vm *svm;

	svm = find_static_vm_vaddr((void *)PCI_IO_VIRT_BASE);
	if (svm)
		return;

	vm_reserve_area_early(PCI_IO_VIRT_BASE, SZ_2M, pci_reserve_io);
}
#else
#define pci_reserve_io() do { } while (0)
#endif

#ifdef CONFIG_DEBUG_LL
void __init debug_ll_io_init(void)
{
	struct map_desc map;

	debug_ll_addr(&map.pfn, &map.virtual);
	if (!map.pfn || !map.virtual)
		return;
	map.pfn = __phys_to_pfn(map.pfn);
	map.virtual &= PAGE_MASK;
	map.length = PAGE_SIZE;
	map.type = MT_DEVICE;
	iotable_init(&map, 1);
}
#endif

static void * __initdata vmalloc_min =
	(void *)(VMALLOC_END - (240 << 20) - VMALLOC_OFFSET);
#if 0  /* @Iamroot: 2017.03.25 */
VMALLOC_END : 0xff800000UL
240 << 20 :   0x0f000000
VMALLOC_OFFSET : 8*1024*1024
              0x00800000

              = 0xf0000000
#endif /* @Iamroot  */
/*
 * vmalloc=size forces the vmalloc area to be exactly 'size'
 * bytes. This can be used to increase (or decrease) the vmalloc
 * area - the default is 240m.
 */
static int __init early_vmalloc(char *arg)
{
	unsigned long vmalloc_reserve = memparse(arg, NULL);

	if (vmalloc_reserve < SZ_16M) {
		vmalloc_reserve = SZ_16M;
		pr_warn("vmalloc area too small, limiting to %luMB\n",
			vmalloc_reserve >> 20);
	}

	if (vmalloc_reserve > VMALLOC_END - (PAGE_OFFSET + SZ_32M)) {
		vmalloc_reserve = VMALLOC_END - (PAGE_OFFSET + SZ_32M);
		pr_warn("vmalloc area is too big, limiting to %luMB\n",
			vmalloc_reserve >> 20);
	}

	vmalloc_min = (void *)(VMALLOC_END - vmalloc_reserve);
	return 0;
}
early_param("vmalloc", early_vmalloc);

phys_addr_t arm_lowmem_limit __initdata = 0;

void __init sanity_check_meminfo(void)
{
	phys_addr_t memblock_limit = 0;
	int highmem = 0;
	phys_addr_t vmalloc_limit = __pa(vmalloc_min - 1) + 1;
#if 0  /* @Iamroot: 2017.03.25 */
vmalloc_min : 0xF0000000
#endif /* @Iamroot  */
	struct memblock_region *reg;
	bool should_use_highmem = false;

	for_each_memblock(memory, reg) {
		phys_addr_t block_start = reg->base;
		phys_addr_t block_end = reg->base + reg->size;
		phys_addr_t size_limit = reg->size;

		if (reg->base >= vmalloc_limit)
			highmem = 1;
		else
			size_limit = vmalloc_limit - reg->base;
#if 0  /* @Iamroot: 2017.03.25 */
                현재 memblock이 vmalloc_limit을 초과할경우 base에서 vmalloc_limit 까지의 사이즈 구함
#endif /* @Iamroot  */


		if (!IS_ENABLED(CONFIG_HIGHMEM) || cache_is_vipt_aliasing()) {

			if (highmem) {
				pr_notice("Ignoring RAM at %pa-%pa (!CONFIG_HIGHMEM)\n",
					  &block_start, &block_end);
				memblock_remove(reg->base, reg->size);
				should_use_highmem = true;
				continue;
			}

			if (reg->size > size_limit) {
				phys_addr_t overlap_size = reg->size - size_limit;

				pr_notice("Truncating RAM at %pa-%pa to -%pa",
					  &block_start, &block_end, &vmalloc_limit);
				memblock_remove(vmalloc_limit, overlap_size);
				block_end = vmalloc_limit;
				should_use_highmem = true;
#if 0  /* @Iamroot: 2017.03.25 */
                                reg->base + reg->size가 vmalloc_min을 초과하였을경우 
                                vmalloc 이상 사용하면 안되므로 윗부분을 삭제 한다 
#endif /* @Iamroot  */
			}
		}

		if (!highmem) {
			if (block_end > arm_lowmem_limit) {
				if (reg->size > size_limit)
					arm_lowmem_limit = vmalloc_limit;
				else
					arm_lowmem_limit = block_end;
			}
#if 0  /* @Iamroot: 2017.03.25 */
                        for문을 돌면서 단 한번만 분기함
#endif /* @Iamroot  */

			/*
			 * Find the first non-pmd-aligned page, and point
			 * memblock_limit at it. This relies on rounding the
			 * limit down to be pmd-aligned, which happens at the
			 * end of this function.
			 *
			 * With this algorithm, the start or end of almost any
			 * bank can be non-pmd-aligned. The only exception is
			 * that the start of the bank 0 must be section-
			 * aligned, since otherwise memory would need to be
			 * allocated when mapping the start of bank 0, which
			 * occurs before any free memory is mapped.
			 */
			if (!memblock_limit) {
				if (!IS_ALIGNED(block_start, PMD_SIZE))
					memblock_limit = block_start;
				else if (!IS_ALIGNED(block_end, PMD_SIZE))
					memblock_limit = arm_lowmem_limit;
			}

		}
	}

	if (should_use_highmem)
		pr_notice("Consider using a HIGHMEM enabled kernel.\n");

	high_memory = __va(arm_lowmem_limit - 1) + 1;

#if 0  /* @Iamroot: 2017.03.25 */
        arm_lowmem_limit의 경우 memblock을 for문으로 검사하면서 각 memblock의 
        end 주소로 최신화 하다 마지막 memblock의 end주소가 됨
        (단 vmalloc를 초과할경우 vmalloc가 arm_lowmem_limit으로 됨
#endif /* @Iamroot  */
	/*
	 * Round the memblock limit down to a pmd size.  This
	 * helps to ensure that we will allocate memory from the
	 * last full pmd, which should be mapped.
	 */
	if (memblock_limit)
		memblock_limit = round_down(memblock_limit, PMD_SIZE);
	if (!memblock_limit)
		memblock_limit = arm_lowmem_limit;
#if 0  /* @Iamroot: 2017.03.25 */
                memblock_limit를 round_down
#endif /* @Iamroot  */
	memblock_set_current_limit(memblock_limit);
#if 0  /* @Iamroot: 2017.03.25 */
        membloc.current_limit 라는 전역변수에 현재 구한 memblock_limit값을 삽입
        memory memblock들이 2M 단위로 align되어 있어야 커널 설정 초기에 사용되는 
        early memory allocator에서 2M 영역을 할당하여 사용하는 reserve memblock을 운영하여야 하므로
        각 memory memblock 들이 2M align되어 있지 않은 memblock이 있는 경우 
        그 지점의 2M round down 주소까지로 사용을 제한하도록 memblock_limit를 설정한다.
        - 문 C 블로그 -
#endif /* @Iamroot  */
}

static inline void prepare_page_table(void)
{
	unsigned long addr;
	phys_addr_t end;

	/*
	 * Clear out all the mappings below the kernel image.
	 */
	for (addr = 0; addr < MODULES_VADDR; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));

#ifdef CONFIG_XIP_KERNEL
	/* The XIP kernel is mapped in the module area -- skip over it */
	addr = ((unsigned long)_exiprom + PMD_SIZE - 1) & PMD_MASK;
#endif
	for ( ; addr < PAGE_OFFSET; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));

	/*
	 * Find the end of the first block of lowmem.
	 */
	end = memblock.memory.regions[0].base + memblock.memory.regions[0].size;
	if (end >= arm_lowmem_limit)
		end = arm_lowmem_limit;

	/*
	 * Clear out all the kernel space mappings, except for the first
	 * memory bank, up to the vmalloc region.
	 */
	for (addr = __phys_to_virt(end);
	     addr < VMALLOC_START; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));
#if 0  /* @Iamroot: 2017.06.03 */
        각 메모리 영역을 클리어 한다
#endif /* @Iamroot  */
}

#ifdef CONFIG_ARM_LPAE
/* the first page is reserved for pgd */
#define SWAPPER_PG_DIR_SIZE	(PAGE_SIZE + \
				 PTRS_PER_PGD * PTRS_PER_PMD * sizeof(pmd_t))
#else
#define SWAPPER_PG_DIR_SIZE	(PTRS_PER_PGD * sizeof(pgd_t))
#endif

#if 0  /* @Iamroot: 2017.03.25 */
PTRS_PER_PGD : 2048  
#endif /* @Iamroot  */
/*
 * Reserve the special regions of memory
 */
void __init arm_mm_memblock_reserve(void)
{
	/*
	 * Reserve the page tables.  These are already in use,
	 * and can only be in node 0.
	 */
	memblock_reserve(__pa(swapper_pg_dir), SWAPPER_PG_DIR_SIZE);
#if 0  /* @Iamroot: 2017.03.25 */
        페이지테이블을 reserve에 추가한다
#endif /* @Iamroot  */
#ifdef CONFIG_SA1111
	/*
	 * Because of the SA1111 DMA bug, we want to preserve our
	 * precious DMA-able memory...
	 */
	memblock_reserve(PHYS_OFFSET, __pa(swapper_pg_dir) - PHYS_OFFSET);
#endif
}

/*
 * Set up the device mappings.  Since we clear out the page tables for all
 * mappings above VMALLOC_START, except early fixmap, we might remove debug
 * device mappings.  This means earlycon can be used to debug this function
 * Any other function or debugging method which may touch any device _will_
 * crash the kernel.
 */
static void __init devicemaps_init(const struct machine_desc *mdesc)
{
	struct map_desc map;
	unsigned long addr;
	void *vectors;

	/*
	 * Allocate the vector page early.
	 */
	vectors = early_alloc(PAGE_SIZE * 2);

	early_trap_init(vectors);

	/*
	 * Clear page table except top pmd used by early fixmaps
	 */
	
#if 0  /* @Iamroot: 2017.08.26 */
	VMALLOC_START = HIGHMEM + 8MB
	VMALLOC_START ~ FIXADDR_TOP까지 초기화(PMD_SIZE = 1MB 단위로 초기화)

    pmd_clear() : pmd의 해당 entry 초기화 	
#endif /* @Iamroot  */

#if 0  /* @Iamroot: 2018.01.06 */
    2018년 스터디 재개
    
	VMALLOC + FIXMAP 영역 초기화

    FIXMAP : 부트업 타임에 요긴하기 쓰기위해 공간확보. 상세한 내용은 문씨블로그 참고 
	http://jake.dothome.co.kr/fixmap/

#endif /* @Iamroot  */

	for (addr = VMALLOC_START; addr < (FIXADDR_TOP & PMD_MASK); addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));

	/*
	 * Map the kernel if it is XIP.
	 * It is always first in the modulearea.
	 */
#ifdef CONFIG_XIP_KERNEL
	map.pfn = __phys_to_pfn(CONFIG_XIP_PHYS_ADDR & SECTION_MASK);
	map.virtual = MODULES_VADDR;
	map.length = ((unsigned long)_exiprom - map.virtual + ~SECTION_MASK) & SECTION_MASK;
	map.type = MT_ROM;
	create_mapping(&map);
#endif

	/*
	 * Map the cache flushing regions.
	 */
#ifdef FLUSH_BASE
	map.pfn = __phys_to_pfn(FLUSH_BASE_PHYS);
	map.virtual = FLUSH_BASE;
	map.length = SZ_1M;
	map.type = MT_CACHECLEAN;
	create_mapping(&map);
#endif
#ifdef FLUSH_BASE_MINICACHE
	map.pfn = __phys_to_pfn(FLUSH_BASE_PHYS + SZ_1M);
	map.virtual = FLUSH_BASE_MINICACHE;
	map.length = SZ_1M;
	map.type = MT_MINICLEAN;
	create_mapping(&map);
#endif

	/*
	 * Create a mapping for the machine vectors at the high-vectors
	 * location (0xffff0000).  If we aren't using high-vectors, also
	 * create a mapping at the low-vectors virtual address.
	 */

#if 0  /* @Iamroot: 2017.08.26 */
	
	vectors의 VA to PA로 mapping & PA를 page frame number로 mapping
	KUSER_HELPERS not define -> low_vector만 사용

#endif /* @Iamroot  */

	map.pfn = __phys_to_pfn(virt_to_phys(vectors));
	map.virtual = 0xffff0000;
	map.length = PAGE_SIZE;
#ifdef CONFIG_KUSER_HELPERS
	map.type = MT_HIGH_VECTORS;
#else
	map.type = MT_LOW_VECTORS;
#endif
	create_mapping(&map);


#if 0  /* @Iamroot: 2017.08.26 */

	high_vector를 사용하지 않기 때문에
	low_vector X 2 만큼 map.length 할당

#endif /* @Iamroot  */


	if (!vectors_high()) {
		map.virtual = 0;
		map.length = PAGE_SIZE * 2;
		map.type = MT_LOW_VECTORS;
		create_mapping(&map);
	}

#if 0  /* @Iamroot: 2017.08.26 */

	vector의 2nd page (entry-armvs.s 참고)를 0xffff0000 + PAGE_SIZE에 mapping
	0xffff000 : high vector location -> 위 주석 참고
    
#endif /* @Iamroot  */

	/* Now create a kernel read-only mapping */
	map.pfn += 1;
	map.virtual = 0xffff0000 + PAGE_SIZE;
	map.length = PAGE_SIZE;
	map.type = MT_LOW_VECTORS;
	create_mapping(&map);

	/*
	 * Ask the machine support to map in the statically mapped devices.
	 */



	if (mdesc->map_io)
		mdesc->map_io();
	else
		debug_ll_io_init();
	fill_pmd_gaps();

	/* Reserve fixed i/o space in VMALLOC region */
	pci_reserve_io();

	/*
	 * Finally flush the caches and tlb to ensure that we're in a
	 * consistent state wrt the writebuffer.  This also ensures that
	 * any write-allocated cache lines in the vector page are written
	 * back.  After this point, we can start to touch devices again.
	 */
	local_flush_tlb_all();
	flush_cache_all();

	/* Enable asynchronous aborts */
	early_abt_enable();
}

#if 0  /* @Iamroot: 2018.01.20 */
FIXADDR_START : 0xffc00000UL
#endif /* @Iamroot  */


#if 0  /* @Iamroot: 2018.01.27 */

**1.
static inline pmd_t *pmd_off_k(unsigned long virt){
      return pmd_offset(pud_offset(pgd_offset_k(virt), virt), virt);}

**2. 
문C 블로그 참고 (http://jake.dothome.co.kr/kmap) 

rpi2에서는 기본 설정에서 ZONE_HIGHMEM을 사용하지 않는다. 
rpi에서는 PAGE_OFFSET가 0xC000_0000(user space=3G, kernel space=1G) 이었는데 
rpi2에서는 HIGHMEM을 사용하지 않으려 PAGE_OFFSET를 

0x8000_0000(user space=2G, kernel space=2G)으로 하였다.

rpi 같은 임베디드 application이 큰 가상주소를 요구하는 경우가 많지 않아 메모리가 
1G 이상의 시스템에서 PAGE_OFFSET를 0x8000_0000으로 사용하는 경우가 있다. 
만일 rpi2의 경우도 rpi같이 최대 메모리가 512M 였으면 PAGE_OFFSET를 0xC000_0000으로 설정하였을 것이다


**3.PKMAP_BASE(persistant kernel mapping)
    : PAGE_OFFSET - PMD_SIZE (PAGE_OFFSET : 0x8000_0000, ri2 ver.)
	  (PMD_SIZE : 1UL << PMD_SHIFT(21) = 2MB) 

static pte_t * __init early_pte_alloc(pmd_t *pmd, unsigned long addr, unsigned long prot){
     return arm_pte_alloc(pmd, addr, prot, early_alloc); }


**4.early_pte_alloc() 을 통해 (중간과정생략) 할당된 pmd 주소를 가져옴 

** 결론적으로 pkmap 영역을 나타내는 page table의 시작주소를 pkmap_page_table에 할당함 

#endif /* @Iamroot  */

static void __init kmap_init(void)
{
#ifdef CONFIG_HIGHMEM
	pkmap_page_table = early_pte_alloc(pmd_off_k(PKMAP_BASE),
		PKMAP_BASE, _PAGE_KERNEL_TABLE);
#endif

	early_pte_alloc(pmd_off_k(FIXADDR_START), FIXADDR_START,
		_PAGE_KERNEL_TABLE);
}

static void __init map_lowmem(void)
{
	struct memblock_region *reg;
#ifdef CONFIG_XIP_KERNEL
	phys_addr_t kernel_x_start = round_down(__pa(_sdata), SECTION_SIZE);
#else
	phys_addr_t kernel_x_start = round_down(__pa(_stext), SECTION_SIZE);
#endif
	phys_addr_t kernel_x_end = round_up(__pa(__init_end), SECTION_SIZE);
#if 0  /* @Iamroot: 2017.06.03 */
        코드영역까지를 범위로 잡느다(데이터 영역 전까지)
#endif /* @Iamroot  */
	/* Map all the lowmem memory banks. */
	for_each_memblock(memory, reg) {
        /* "reg" variable is assigned from every single memblock of memory*/
		phys_addr_t start = reg->base;
		phys_addr_t end = start + reg->size;
		struct map_desc map;

		if (memblock_is_nomap(reg))
			continue;

		if (end > arm_lowmem_limit)
			end = arm_lowmem_limit;
		if (start >= end)
			break;
#if 0  /* @Iamroot: 2017.06.03 */
                memblock 중 arm_lowmem_limit 이하에 있는 memblock들만 아래의 작업을 수행한다
#endif /* @Iamroot  */

        /*
            kernel_x_start: the start point of _stext
            kernel_x_end: the end point of init_datas
        */

		if (end < kernel_x_start) {
			map.pfn = __phys_to_pfn(start);
			map.virtual = __phys_to_virt(start);
			map.length = end - start;
			map.type = MT_MEMORY_RWX;

			create_mapping(&map);

		} else if (start >= kernel_x_end) {
			map.pfn = __phys_to_pfn(start);
			map.virtual = __phys_to_virt(start);
			map.length = end - start;
			map.type = MT_MEMORY_RW;

			create_mapping(&map);
		} else {
			/* This better cover the entire kernel */
#if 0  /* @Iamroot: 2017.06.03 */
    create mapping except the region of kernel memory
    in below two if statements
#endif /* @Iamroot  */
			if (start < kernel_x_start) {
				map.pfn = __phys_to_pfn(start);
				map.virtual = __phys_to_virt(start);
				map.length = kernel_x_start - start;
				map.type = MT_MEMORY_RW;

				create_mapping(&map);
			}

			map.pfn = __phys_to_pfn(kernel_x_start);
			map.virtual = __phys_to_virt(kernel_x_start);
			map.length = kernel_x_end - kernel_x_start;
			map.type = MT_MEMORY_RWX;

			create_mapping(&map);

			if (kernel_x_end < end) {
				map.pfn = __phys_to_pfn(kernel_x_end);
				map.virtual = __phys_to_virt(kernel_x_end);
				map.length = end - kernel_x_end;
				map.type = MT_MEMORY_RW;

				create_mapping(&map);
			}
		}
	}
}

#ifdef CONFIG_ARM_PV_FIXUP
extern unsigned long __atags_pointer;
typedef void pgtables_remap(long long offset, unsigned long pgd, void *bdata);
pgtables_remap lpae_pgtables_remap_asm;

/*
 * early_paging_init() recreates boot time page table setup, allowing machines
 * to switch over to a high (>4G) address space on LPAE systems
 */
void __init early_paging_init(const struct machine_desc *mdesc)
{
	pgtables_remap *lpae_pgtables_remap;
	unsigned long pa_pgd;
	unsigned int cr, ttbcr;
	long long offset;
	void *boot_data;

	if (!mdesc->pv_fixup)
		return;

	offset = mdesc->pv_fixup();
	if (offset == 0)
		return;

	/*
	 * Get the address of the remap function in the 1:1 identity
	 * mapping setup by the early page table assembly code.  We
	 * must get this prior to the pv update.  The following barrier
	 * ensures that this is complete before we fixup any P:V offsets.
	 */
	lpae_pgtables_remap = (pgtables_remap *)(unsigned long)__pa(lpae_pgtables_remap_asm);
	pa_pgd = __pa(swapper_pg_dir);
	boot_data = __va(__atags_pointer);
	barrier();

	pr_info("Switching physical address space to 0x%08llx\n",
		(u64)PHYS_OFFSET + offset);

	/* Re-set the phys pfn offset, and the pv offset */
	__pv_offset += offset;
	__pv_phys_pfn_offset += PFN_DOWN(offset);

	/* Run the patch stub to update the constants */
	fixup_pv_table(&__pv_table_begin,
		(&__pv_table_end - &__pv_table_begin) << 2);

	/*
	 * We changing not only the virtual to physical mapping, but also
	 * the physical addresses used to access memory.  We need to flush
	 * all levels of cache in the system with caching disabled to
	 * ensure that all data is written back, and nothing is prefetched
	 * into the caches.  We also need to prevent the TLB walkers
	 * allocating into the caches too.  Note that this is ARMv7 LPAE
	 * specific.
	 */
	cr = get_cr();
	set_cr(cr & ~(CR_I | CR_C));
	asm("mrc p15, 0, %0, c2, c0, 2" : "=r" (ttbcr));
	asm volatile("mcr p15, 0, %0, c2, c0, 2"
		: : "r" (ttbcr & ~(3 << 8 | 3 << 10)));
	flush_cache_all();

	/*
	 * Fixup the page tables - this must be in the idmap region as
	 * we need to disable the MMU to do this safely, and hence it
	 * needs to be assembly.  It's fairly simple, as we're using the
	 * temporary tables setup by the initial assembly code.
	 */
	lpae_pgtables_remap(offset, pa_pgd, boot_data);

	/* Re-enable the caches and cacheable TLB walks */
	asm volatile("mcr p15, 0, %0, c2, c0, 2" : : "r" (ttbcr));
	set_cr(cr);
}

#else

void __init early_paging_init(const struct machine_desc *mdesc)
{
	long long offset;

	if (!mdesc->pv_fixup)
		return;

	offset = mdesc->pv_fixup();
	if (offset == 0)
		return;

	pr_crit("Physical address space modification is only to support Keystone2.\n");
	pr_crit("Please enable ARM_LPAE and ARM_PATCH_PHYS_VIRT support to use this\n");
	pr_crit("feature. Your kernel may crash now, have a good day.\n");
	add_taint(TAINT_CPU_OUT_OF_SPEC, LOCKDEP_STILL_OK);
}

#endif

static void __init early_fixmap_shutdown(void)
{
	int i;
	unsigned long va = fix_to_virt(__end_of_permanent_fixed_addresses - 1);
#if 0  /* @Iamroot: 2017.08.05 */
    url of explaination for fixmap : https://0xax.gitbooks.io/linux-insides/content/mm/linux-mm-2.html
    Get a address of first page frame to 'va' variable
#endif /* @Iamroot  */

#if 0  /* @Iamroot: 2017.08.05 */
    clear the pmd of upper va and also flush corresponding area of va
#endif /* @Iamroot  */
	pte_offset_fixmap = pte_offset_late_fixmap;
	pmd_clear(fixmap_pmd(va));
	local_flush_tlb_kernel_page(va);

#if 0  /* @Iamroot: 2017.08.05 */
    __end_of_permanent_fixed_addresses is set 1.
#endif /* @Iamroot  */
	for (i = 0; i < __end_of_permanent_fixed_addresses; i++) {
		pte_t *pte;
		struct map_desc map;

#if 0  /* @Iamroot: 2017.08.05 */
    i == 0, get an address mapped in fixmap area.
    map.virtual = va
#endif /* @Iamroot  */
		map.virtual = fix_to_virt(i);
#if 0  /* @Iamroot: 2017.08.05 */
    Get a linux pte mapping to upper fixmap address
#endif /* @Iamroot  */
		pte = pte_offset_early_fixmap(pmd_off_k(map.virtual), map.virtual);

		/* Only i/o device mappings are supported ATM */
		if (pte_none(*pte) ||
		    (pte_val(*pte) & L_PTE_MT_MASK) != L_PTE_MT_DEV_SHARED)
			continue;

		map.pfn = pte_pfn(*pte);
		map.type = MT_DEVICE;
		map.length = PAGE_SIZE;

#if 0  /* @Iamroot: 2017.08.05 */
    1. create page table mapping
    2. memory mapping to systemcal global variable(init_mm)
#endif /* @Iamroot  */
		create_mapping(&map);
	}
}

/*
 * paging_init() sets up the page tables, initialises the zone memory
 * maps, and sets up the zero page, bad page and bad page tables.
 */
void __init paging_init(const struct machine_desc *mdesc)
{
	void *zero_page;

	build_mem_type_table();
	prepare_page_table();
	map_lowmem();
	memblock_set_current_limit(arm_lowmem_limit);
	dma_contiguous_remap();
    // Why this function developer named like sucks about 'shutdown'?
	early_fixmap_shutdown();
	devicemaps_init(mdesc);
	kmap_init();
	tcm_init();

	top_pmd = pmd_off_k(0xffff0000);

	/* allocate the zero page. */
	zero_page = early_alloc(PAGE_SIZE);

	bootmem_init();

	empty_zero_page = virt_to_page(zero_page);
	__flush_dcache_page(NULL, empty_zero_page);
}
