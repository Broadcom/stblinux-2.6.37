/*
 * Copyright (C) 2000-2004 Russell King
 * Copyright (C) 2010 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/bootmem.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/ioport.h>
#include <linux/list.h>
#include <linux/vmalloc.h>
#include <linux/compiler.h>
#include <linux/module.h>

#include <asm/page.h>
#include <asm/pgtable-32.h>
#include <asm/pgtable-bits.h>
#include <asm/addrspace.h>
#include <asm/tlbflush.h>
#include <asm/r4kcache.h>
#include <asm/brcmstb/brcmstb.h>

#include <spaces.h>

#if 0
#define DBG printk
#else
#define DBG(...) /* */
#endif

#define LINUX_MIN_MEM		64
#define MAX_BMEM_REGIONS	4

struct bmem_region {
	unsigned long		addr;
	unsigned long		size;
	int			valid;
};

static struct bmem_region bmem_regions[MAX_BMEM_REGIONS];
static unsigned int n_bmem_regions;

#if defined(CONFIG_BRCM_IKOS)
static unsigned int bmem_disabled = 1;
#else
static unsigned int bmem_disabled;
#endif

/***********************************************************************
 * MEMC1 handling
 ***********************************************************************/

#if defined(CONFIG_BRCM_HAS_1GB_MEMC1)

static int __init brcm_memc1_bmem(void)
{
	struct bmem_region *r = NULL;

	if (brcm_dram1_size_mb &&
			brcm_dram1_size_mb > brcm_dram1_linux_mb &&
			n_bmem_regions < MAX_BMEM_REGIONS) {
		r = &bmem_regions[n_bmem_regions++];
		r->addr = MEMC1_START + (brcm_dram1_linux_mb << 20);
		r->size = (brcm_dram1_size_mb - brcm_dram1_linux_mb) << 20;
		r->valid = 1;
		printk(KERN_INFO "memc1: adding %lu MB RESERVED region at "
			"%lu MB\n", r->size >> 20, r->addr >> 20);
	}
	return 0;
}

core_initcall(brcm_memc1_bmem);

#endif /* defined(CONFIG_BRCM_HAS_1GB_MEMC1) */

/***********************************************************************
 * BMEM (reserved A/V buffer memory) support
 ***********************************************************************/

/*
 * Parses command line for bmem= options
 */
static int __init bmem_setup(char *str)
{
	unsigned long addr = 0, size;
	unsigned long lower_mem_bytes;
	char *orig_str = str;

	lower_mem_bytes = (brcm_dram0_size_mb > BRCM_MAX_LOWER_MB) ?
		(BRCM_MAX_LOWER_MB << 20) : (brcm_dram0_size_mb << 20);

	size = (unsigned long)memparse(str, &str);
	if (*str == '@')
		addr = (unsigned long)memparse(str + 1, &str);

	if ((size > 0x40000000) || (addr > 0x80000000)) {
		printk(KERN_WARNING "bmem: argument '%s' "
			"is out of range, ignoring\n", orig_str);
		return 0;
	}

	if (size == 0) {
		printk(KERN_INFO "bmem: disabling reserved memory\n");
		bmem_disabled = 1;
		return 0;
	}

	if ((addr & ~PAGE_MASK) || (size & ~PAGE_MASK)) {
		printk(KERN_WARNING "bmem: ignoring invalid range '%s' "
			"(is it missing an 'M' suffix?)\n", orig_str);
		return 0;
	}

	if (addr == 0) {
		/*
		 * default: bmem=xxM allocates xx megabytes at the end of
		 * lower memory
		 */
		if (size >= lower_mem_bytes) {
			printk(KERN_WARNING "bmem: '%s' is larger than "
				"lower memory (%lu MB), ignoring\n",
				orig_str, brcm_dram0_size_mb);
			return 0;
		}
		addr = lower_mem_bytes - size;
	}

	if (n_bmem_regions == MAX_BMEM_REGIONS) {
		printk(KERN_WARNING "bmem: too many regions, "
			"ignoring extras\n");
		return 0;
	}

	bmem_regions[n_bmem_regions].addr = addr;
	bmem_regions[n_bmem_regions].size = size;
	n_bmem_regions++;

	return 0;
}

early_param("bmem", bmem_setup);

/*
 * Returns index if the supplied range falls entirely within a bmem region
 */
int bmem_find_region(unsigned long addr, unsigned long size)
{
	int i, idx = 0;

	for (i = 0; i < n_bmem_regions; i++) {
		if (!bmem_regions[i].valid)
			continue;
		if ((addr >= bmem_regions[i].addr) &&
		    ((addr + size) <=
			(bmem_regions[i].addr + bmem_regions[i].size))) {
			return idx;
		}
		idx++;
	}
	return -ENOENT;
}
EXPORT_SYMBOL(bmem_find_region);

/*
 * Finds the IDX'th valid bmem region, and fills in addr/size if it exists.
 * Returns 0 on success, <0 on failure.
 */
int bmem_region_info(int idx, unsigned long *addr, unsigned long *size)
{
	int i;

	for (i = 0; i < n_bmem_regions; i++) {
		if (!bmem_regions[i].valid)
			continue;
		if (!idx) {
			*addr = bmem_regions[i].addr;
			*size = bmem_regions[i].size;
			return 0;
		}
		idx--;
	}
	return -ENOENT;
}
EXPORT_SYMBOL(bmem_region_info);

/*
 * Special handling for __get_user_pages() on BMEM reserved memory:
 *
 * 1) Override the VM_IO | VM_PFNMAP sanity checks
 * 2) No cache flushes (this is explicitly under application control)
 * 3) vm_normal_page() does not work on these regions
 * 4) Don't need to worry about any kinds of faults; pages are always present
 *
 * The vanilla kernel behavior was to prohibit O_DIRECT operations on our
 * BMEM regions, but direct I/O is absolutely required for PVR and video
 * playback from SATA/USB.
 */
int bmem_get_page(struct mm_struct *mm, struct vm_area_struct *vma,
	unsigned long start, struct page **page)
{
	unsigned long pg = start & PAGE_MASK, pfn;
	int ret = -EFAULT;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset(mm, pg);
	BUG_ON(pgd_none(*pgd));
	pud = pud_offset(pgd, pg);
	BUG_ON(pud_none(*pud));
	pmd = pmd_offset(pud, pg);
	if (pmd_none(*pmd))
		return ret;

	pte = pte_offset_map(pmd, pg);
	if (pte_none(*pte))
		goto out;

	pfn = pte_pfn(*pte);
	if (likely(bmem_find_region(pfn << PAGE_SHIFT, PAGE_SIZE) < 0))
		goto out;

	if (page) {
		*page = pfn_to_page(pfn);
		get_page(*page);
	}
	ret = 0;

out:
	pte_unmap(pte);
	return ret;
}

static int __initdata bmem_defaults_set;

static void __init brcm_set_default_bmem(void)
{
	/*
	 * Default: (no valid bmem= options specified)
	 *
	 * Lower memory is the first 256MB of system RAM.
	 * bmem gets all but (LINUX_MIN_MEM) megabytes of lower memory.
	 * Linux gets all upper and high memory (if present).
	 *
	 * Options:
	 *
	 * Define one or more custom bmem regions, e.g.:
	 *   bmem=128M (128MB region at the end of lower memory)
	 *   bmem=128M@64M (128MB hole at 64MB mark)
	 *   bmem=16M@64M bmem=4M@128M bmem=16M@200M (multiple holes)
	 *
	 * Disable bmem; give all system RAM to the Linux VM:
	 *   bmem=0
	 *
	 * Overlapping or invalid regions will usually be silently dropped.
	 * If you are doing something tricky, watch the boot messages to
	 * make sure it turns out the way you intended.
	 */
	if (bmem_disabled) {
		n_bmem_regions = 0;
	} else {
		if (n_bmem_regions == 0 &&
		    (brcm_dram0_size_mb > LINUX_MIN_MEM)) {
			n_bmem_regions = 1;
			bmem_regions[0].addr = LINUX_MIN_MEM << 20;
			bmem_regions[0].size =
				(((brcm_dram0_size_mb <= BRCM_MAX_LOWER_MB) ?
				  brcm_dram0_size_mb : BRCM_MAX_LOWER_MB) -
				 LINUX_MIN_MEM) << 20;
		}
	}
}

/*
 * Invokes free_bootmem(), but truncates ranges where necessary to
 * avoid releasing the bmem region(s) back to the VM
 */
void __init brcm_free_bootmem(unsigned long addr, unsigned long size)
{
	if (!bmem_defaults_set) {
		brcm_set_default_bmem();
		bmem_defaults_set = 1;
	}

	while (size) {
		unsigned long chunksize = size;
		int i;
		struct bmem_region *r = NULL;

		/*
		 * Find the first bmem region (if any) that fits entirely
		 * within the current bootmem address range.
		 */
		for (i = 0; i < n_bmem_regions; i++) {
			if ((bmem_regions[i].addr >= addr) &&
			    ((bmem_regions[i].addr + bmem_regions[i].size) <=
			     (addr + size))) {
				if (!r || (r->addr > bmem_regions[i].addr))
					r = &bmem_regions[i];
			}
		}

		/*
		 * Skip over every bmem region; call free_bootmem() for
		 * every Linux region.  A Linux region is created for
		 * each chunk of the memory map that is not reserved
		 * for bmem.
		 */
		if (r) {
			if (addr == r->addr) {
				printk(KERN_INFO "bmem: adding %lu MB "
					"RESERVED region at %lu MB "
					"(0x%08lx@0x%08lx)\n",
					r->size >> 20, r->addr >> 20,
					r->size, r->addr);
				chunksize = r->size;
				r->valid = 1;
				goto skip;
			} else {
				BUG_ON(addr > r->addr);
				chunksize = r->addr - addr;
			}
		}
		BUG_ON(chunksize > size);

		printk(KERN_DEBUG "bmem: adding %lu MB LINUX region at %lu MB "
			"(0x%08lx@0x%08lx)\n", chunksize >> 20, addr >> 20,
			chunksize, addr);
		free_bootmem(addr, chunksize);

skip:
		addr += chunksize;
		size -= chunksize;
	}
}

/*
 * Create /proc/iomem entries for bmem
 */
static int __init bmem_region_setup(void)
{
	int i, idx = 0;

	for (i = 0; i < n_bmem_regions; i++) {
		struct resource *r;
		char *name;

		if (!bmem_regions[i].valid)
			continue;

		r = kzalloc(sizeof(*r), GFP_KERNEL);
		name = kzalloc(16, GFP_KERNEL);
		if (!r || !name)
			break;

		sprintf(name, "bmem.%d", idx);
		r->start = bmem_regions[i].addr;
		r->end = bmem_regions[i].addr + bmem_regions[i].size - 1;
		r->flags = IORESOURCE_MEM;
		r->name = name;

		insert_resource(&iomem_resource, r);
		idx++;
	}
	return 0;
}

arch_initcall(bmem_region_setup);

/*
 * Override default behavior to allow cached access to all valid DRAM ranges
 */
int __uncached_access(struct file *file, unsigned long addr)
{
	if (file->f_flags & O_SYNC)
		return 1;
	if (addr >= BCHP_PHYSICAL_OFFSET && addr < UPPERMEM_START)
		return 1;
	if (addr >= PCIE_MEM_START)
		return 1;
	return 0;
}

/***********************************************************************
 * Wired TLB mappings for upper memory support
 ***********************************************************************/

#define UNIQUE_ENTRYHI(idx) (CKSEG0 + ((idx) << (PAGE_SHIFT + 1)))

/* (PFN << 6) | GLOBAL | VALID | DIRTY | cacheability */
#define ENTRYLO_CACHED(paddr)	(((paddr) >> 6) | (0x07) | (0x03 << 3))
#define ENTRYLO_UNCACHED(paddr)	(((paddr) >> 6) | (0x07) | (0x02 << 3))

/* GLOBAL | !VALID */
#define ENTRYLO_INVALID()	(0x01)

struct tlb_entry {
	unsigned long entrylo0;
	unsigned long entrylo1;
	unsigned long entryhi;
	unsigned long pagemask;
};

static struct tlb_entry __maybe_unused uppermem_mappings[] = {
{
#if   defined(CONFIG_BRCM_UPPER_256MB)
	.entrylo0		= ENTRYLO_CACHED(UPPERMEM_START),
	.entrylo1		= ENTRYLO_UNCACHED(UPPERMEM_START),
	.entryhi		= CAC_BASE_UPPER,
	.pagemask		= PM_256M,
#elif defined(CONFIG_BRCM_UPPER_768MB)
	.entrylo0		= ENTRYLO_CACHED(TLB_UPPERMEM_PA),
	.entrylo1		= ENTRYLO_INVALID(),
	.entryhi		= TLB_UPPERMEM_VA,
	.pagemask		= PM_256M,
#endif
},
};

/*
 * This function is used instead of add_wired_entry(), because it does not
 * have any external dependencies and is not marked __init
 */
static inline void __cpuinit brcm_add_wired_entry(unsigned long entrylo0,
	unsigned long entrylo1, unsigned long entryhi, unsigned long pagemask)
{
	int i = read_c0_wired();

	write_c0_entrylo0(entrylo0);
	write_c0_entrylo1(entrylo1);
	write_c0_entryhi(entryhi);
	write_c0_pagemask(pagemask);
	write_c0_index(i);
	write_c0_wired(i + 1);
	mtc0_tlbw_hazard();
	tlb_write_indexed();
	tlbw_use_hazard();
}

extern void tlb_init(void);
extern void build_tlb_refill_handler(void);

void __cpuinit brcm_tlb_init(void)
{
#ifdef CONFIG_BRCM_UPPER_MEMORY
	if (smp_processor_id() == 0) {
		int i;
		struct tlb_entry *e = uppermem_mappings;

		tlb_init();
		for (i = 0; i < ARRAY_SIZE(uppermem_mappings); i++, e++)
			brcm_add_wired_entry(e->entrylo0, e->entrylo1,
				e->entryhi, e->pagemask);
		write_c0_pagemask(PM_DEFAULT_MASK);
	} else {
		/* bypass tlb_init() / probe_tlb() for secondary CPU */
		cpu_data[smp_processor_id()].tlbsize = cpu_data[0].tlbsize;
		build_tlb_refill_handler();
	}
#else
	tlb_init();
#endif
}

/*
 * Initialize upper memory TLB entries
 *
 * On TP1 this must happen before we set up $sp/$gp .  It is always
 * possible for stacks, task_structs, thread_info's, and other
 * important structures to be allocated out of upper memory so
 * this happens early on.
 */
asmlinkage __cpuinit void brcm_upper_tlb_setup(void)
{
#ifdef CONFIG_BRCM_UPPER_MEMORY
	int i, tlbsz;

	/* Flush TLB.  local_flush_tlb_all() is not available yet. */
	write_c0_entrylo0(0);
	write_c0_entrylo1(0);
	write_c0_pagemask(PM_DEFAULT_MASK);
	write_c0_wired(0);

	tlbsz = (read_c0_config1() >> 25) & 0x3f;
	for (i = 0; i <= tlbsz; i++) {
		write_c0_entryhi(UNIQUE_ENTRYHI(i));
		write_c0_index(i);
		mtc0_tlbw_hazard();
		tlb_write_indexed();
		tlbw_use_hazard();
	}

	write_c0_wired(0);
	mtc0_tlbw_hazard();

	for (i = 0; i < ARRAY_SIZE(uppermem_mappings); i++) {
		struct tlb_entry *e = &uppermem_mappings[i];
		brcm_add_wired_entry(e->entrylo0, e->entrylo1, e->entryhi,
			e->pagemask);
	}

	write_c0_pagemask(PM_DEFAULT_MASK);
#endif
}

#ifdef CONFIG_BRCM_UPPER_768MB

/***********************************************************************
 * Special allocator for coherent (uncached) memory
 * (Required for >256MB upper memory)
 ***********************************************************************/

#define CONSISTENT_DMA_SIZE	(CONSISTENT_END - CONSISTENT_BASE)
#define CONSISTENT_OFFSET(x)	(((unsigned long)(x) - CONSISTENT_BASE) >> \
	PAGE_SHIFT)
#define CONSISTENT_PTE_INDEX(x) (((unsigned long)(x) - CONSISTENT_BASE) >> \
	PGDIR_SHIFT)
#define NUM_CONSISTENT_PTES	(CONSISTENT_DMA_SIZE >> PGDIR_SHIFT)

/*
 * These are the page tables (4MB each) covering uncached, DMA consistent
 * allocations
 */
static pte_t *consistent_pte[NUM_CONSISTENT_PTES];
static DEFINE_SPINLOCK(consistent_lock);

struct arm_vm_region {
	struct list_head	vm_list;
	unsigned long		vm_start;
	unsigned long		vm_end;
	void			*vm_cac_va;
	int			vm_active;
};

static struct arm_vm_region consistent_head = {
	.vm_list	= LIST_HEAD_INIT(consistent_head.vm_list),
	.vm_start	= CONSISTENT_BASE,
	.vm_end		= CONSISTENT_END,
};

static struct arm_vm_region *
arm_vm_region_alloc(struct arm_vm_region *head, size_t size, gfp_t gfp)
{
	unsigned long addr = head->vm_start, end = head->vm_end - size;
	unsigned long flags;
	struct arm_vm_region *c, *new;

	new = kmalloc(sizeof(struct arm_vm_region), gfp);
	if (!new)
		goto out;

	spin_lock_irqsave(&consistent_lock, flags);

	list_for_each_entry(c, &head->vm_list, vm_list) {
		if ((addr + size) < addr)
			goto nospc;
		if ((addr + size) <= c->vm_start)
			goto found;
		addr = c->vm_end;
		if (addr > end)
			goto nospc;
	}

found:
	/*
	 * Insert this entry _before_ the one we found.
	 */
	list_add_tail(&new->vm_list, &c->vm_list);
	new->vm_start = addr;
	new->vm_end = addr + size;
	new->vm_active = 1;

	spin_unlock_irqrestore(&consistent_lock, flags);
	return new;

nospc:
	spin_unlock_irqrestore(&consistent_lock, flags);
	kfree(new);
out:
	return NULL;
}

static struct arm_vm_region *arm_vm_region_find(struct arm_vm_region *head,
	unsigned long addr)
{
	struct arm_vm_region *c;

	list_for_each_entry(c, &head->vm_list, vm_list) {
		if (c->vm_active && c->vm_start == addr)
			goto out;
	}
	c = NULL;
out:
	return c;
}

static int __init consistent_init(void)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	int ret = 0, i = 0;
	u32 base = CONSISTENT_BASE;

	do {
		pgd = pgd_offset(&init_mm, base);
		pud = pud_alloc(&init_mm, pgd, base);
		if (!pud) {
			printk(KERN_ERR "%s: no pud tables\n", __func__);
			ret = -ENOMEM;
			break;
		}
		pmd = pmd_alloc(&init_mm, pud, base);
		if (!pmd) {
			printk(KERN_ERR "%s: no pmd tables\n", __func__);
			ret = -ENOMEM;
			break;
		}

		pte = pte_alloc_kernel(pmd, base);
		if (!pte) {
			printk(KERN_ERR "%s: no pte tables\n", __func__);
			ret = -ENOMEM;
			break;
		}

		consistent_pte[i++] = pte;
		base += (1 << PGDIR_SHIFT);
	} while (base < CONSISTENT_END);

	return ret;
}

core_initcall(consistent_init);

int brcm_map_coherent(dma_addr_t dma_handle, void *cac_va, size_t size,
	void **uncac_va, gfp_t gfp)
{
	struct arm_vm_region *c;
	struct page *page;
	pte_t *pte;
	int idx;
	u32 off;

	c = arm_vm_region_alloc(&consistent_head, size, gfp);
	if (!c)
		return -EINVAL;

	c->vm_cac_va = cac_va;

	page = virt_to_page(cac_va);
	idx = CONSISTENT_PTE_INDEX(c->vm_start);
	off = CONSISTENT_OFFSET(c->vm_start) & (PTRS_PER_PTE-1);
	pte = consistent_pte[idx] + off;

	DBG("map addr %08lx idx %x off %x pte %p\n", c->vm_start, idx, off,
		pte);

	do {
		BUG_ON(!pte_none(*pte));
		set_pte(pte, mk_pte(page, PAGE_KERNEL_UNCACHED));
		page++;
		pte++;
		off++;
		if (off >= PTRS_PER_PTE) {
			off = 0;
			pte = consistent_pte[++idx];
		}
	} while (size -= PAGE_SIZE);

	*uncac_va = (void *)c->vm_start;
	return 0;
}

void *brcm_unmap_coherent(void *vaddr)
{
	struct arm_vm_region *c;
	unsigned long flags, addr;
	void *ret = NULL;
	pte_t *pte;
	int idx;
	u32 off;

	spin_lock_irqsave(&consistent_lock, flags);
	c = arm_vm_region_find(&consistent_head, (unsigned long)vaddr);
	if (!c) {
		spin_unlock_irqrestore(&consistent_lock, flags);
		printk(KERN_ERR "%s: invalid VA %p\n", __func__, vaddr);
		return NULL;
	}
	c->vm_active = 0;
	spin_unlock_irqrestore(&consistent_lock, flags);

	ret = c->vm_cac_va;
	addr = c->vm_start;

	idx = CONSISTENT_PTE_INDEX(addr);
	off = CONSISTENT_OFFSET(addr) & (PTRS_PER_PTE-1);
	pte = consistent_pte[idx] + off;

	DBG("unmap addr %08lx idx %x off %x pte %p\n", addr, idx, off, pte);

	do {
		pte_clear(&init_mm, addr, pte);
		pte++;
		off++;
		if (off >= PTRS_PER_PTE) {
			off = 0;
			pte = consistent_pte[++idx];
		}
		addr += PAGE_SIZE;
	} while (addr < c->vm_end);
	flush_tlb_kernel_range(c->vm_start, c->vm_end);

	spin_lock_irqsave(&consistent_lock, flags);
	list_del(&c->vm_list);
	spin_unlock_irqrestore(&consistent_lock, flags);

	kfree(c);

	return ret;
}

#endif /* CONFIG_BRCM_UPPER_768MB */

void __iomem *plat_ioremap(phys_t offset, unsigned long size,
	unsigned long flags)
{
	/* sanity check */
	if ((offset + size - 1) < offset ||
	    !size ||
	    offset > max(KSEG0_SIZE, KSEG1_SIZE))
		return NULL;

	/* !XKS01, XKS01: uncached access to EBI/registers @ PA 1000_0000 */
	if (offset >= 0x10000000 &&
	    (offset + size) <= 0x1fffffff &&
	    flags == _CACHE_UNCACHED)
		return (void *)(KSEG1 + offset);

	/* !XKS01, XKS01: easy cached access to some DRAM */
	if ((offset + size) <= KSEG0_SIZE &&
	    flags == _CACHE_CACHABLE_NONCOHERENT)
		return (void *)(KSEG0 + offset);

	/* !XKS01 only: easy uncached access to some DRAM */
	if ((offset + size) <= KSEG1_SIZE &&
	    flags == _CACHE_UNCACHED)
		return (void *)(KSEG1 + offset);

	/* anything else gets mapped using page tables */
	return NULL;
}
EXPORT_SYMBOL(plat_ioremap);

int plat_iounmap(const volatile void __iomem *addr)
{
	phys_t va = (unsigned long)addr;

	if (va >= KSEG0 && va < (KSEG0 + KSEG0_SIZE))
		return 1;
	if (va >= KSEG1 && va < (KSEG1 + KSEG1_SIZE))
		return 1;
	return 0;
}
EXPORT_SYMBOL(plat_iounmap);
