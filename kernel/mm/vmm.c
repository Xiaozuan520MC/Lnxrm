/* Virtual memory: master kernel tables (built by entry64.S) + per-process
 * user address spaces under PML4[255].
 *
 * Address-space layout:
 *   slot 0   : identity map of low RAM (kernel-only, shared by ALL ASes)
 *   slot 255 : per-process user space  [0x7f8000000000, 0x7fffffffffff]
 *   slot 511 : high-half alias of physical memory (kernel image, heap,
 *              device windows, VGA), shared by all ASes.
 *
 * vmm_new_user_aspace() copies every PML4 entry except slot 255, so kernel
 * mappings propagate to all tasks automatically. */
#include <mm/mm.h>
#include <boot.h>
#include <console.h>
#include <framebuffer.h>
#include <sys/cpu.h>

#define PD_HI  0x54000UL /* keep in sync with entry64.S */

#define USER_SLOT  255

static u64 pml4_phys = 0x50000UL;
static u64 heap_phys, heap_size;

void vmm_heap_region(u64 *phys_out, u64 *size_out)
{
    *phys_out = heap_phys;
    *size_out = heap_size;
}

u64 master_pml4_phys(void)
{ return pml4_phys; }

void kheap_place(u64 *region_top, u64 size)
{
    /* called from pmm_init BEFORE the buddy exists: reserve from the top */
    u64 top = ALIGN_DOWN(*region_top, PAGE_SIZE);
    heap_phys = ALIGN_DOWN(top - size, PAGE_SIZE);
    heap_size = size;
    *region_top = heap_phys;
}

static inline u64 *ptable_ptr(u64 pa)
{ return (u64 *)PHYS_TO_VIRT(pa); }

static u64 ensure_table(u64 *parent, u64 idx, u64 extra_flags)
{
    if (parent[idx] & PG_P) return parent[idx] & ~0xfffUL;
    u64 t = pmm_alloc();
    if (!t) return 0; /* T-004: out of frames is the caller's problem, not fatal */
    memset(ptable_ptr(t), 0, PAGE_SIZE);
    parent[idx] = t | PG_P | PG_W | extra_flags;
    return t;
}

int vmm_map_user(u64 root, u64 va, u64 pa, bool writable, bool user, bool exec)
{
    if (va < USER_BASE || va >= USER_MAX_VMA || (va & 0xfff)) return LNXRM_EFAIL;
    u64 i4 = (va >> 39) & 511, i3 = (va >> 30) & 511;
    u64 i2 = (va >> 21) & 511, i1 = (va >> 12) & 511;
    u64 uf = user ? PG_U : 0;

    u64 *pml4 = ptable_ptr(root);
    u64 pdpt_pa = ensure_table(pml4, i4, PG_U);
    if (!pdpt_pa) return LNXRM_ENOMEM;
    u64 *pdpt = ptable_ptr(pdpt_pa);
    u64 pd_pa = ensure_table(pdpt, i3, PG_U);
    if (!pd_pa) return LNXRM_ENOMEM;
    u64 *pd = ptable_ptr(pd_pa);
    u64 pt_pa = ensure_table(pd, i2, PG_U);
    if (!pt_pa) return LNXRM_ENOMEM;
    u64 *pt = ptable_ptr(pt_pa);

    if (pt[i1] & PG_P) {
        /* Already mapped: two ELF segments may share a page (text in front
         * of rodata of the next one).  Widen -- and only widen -- the
         * existing PTE instead of overwriting it; `pa` is ignored here. */
        if (writable) pt[i1] |= PG_W;
        if (exec) pt[i1] &= ~PG_NX;
    } else {
        if (!pa) return LNXRM_EFAIL;
        pt[i1] = pa | PG_P | (writable ? PG_W : 0) | uf | (exec ? 0 : PG_NX);
    }
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    return 0;
}

static int walk(u64 root, u64 va, u64 **pte_out, u64 *big_buf)
{
    u64 i4 = (va >> 39) & 511, i3 = (va >> 30) & 511;
    u64 i2 = (va >> 21) & 511, i1 = (va >> 12) & 511;
    u64 *pml4 = ptable_ptr(root);
    if (!(pml4[i4] & PG_P)) return LNXRM_EFAIL;
    u64 *pdpt = ptable_ptr(pml4[i4] & ~0xfff);
    if (!(pdpt[i3] & PG_P)) return LNXRM_EFAIL;
    u64 *pd = ptable_ptr(pdpt[i3] & ~0xfff);
    if (!(pd[i2] & PG_P)) return LNXRM_EFAIL;
    if (pd[i2] & PG_PS) {
        *big_buf = pd[i2]; /* caller only reads the phys bits */
        *pte_out = big_buf;
        return 1; /* 2 MiB leaf */
    }
    *pte_out = &ptable_ptr(pd[i2] & ~0xfff)[i1];
    return 0;
}

u64 vmm_translate_in(u64 root, u64 va)
{
    /* No "inside the high window, so phys = va - ALIAS_BASE" shortcut: that
     * window is not a 1:1 image of physical memory.  KHEAP_VMA maps the heap
     * (phys heap_phys, not 512 MiB), VGA_VMA/DEV_VMA/FB_VMA map page tables
     * of their own, and the old `ALIAS_BASE + 0x100000000UL` bound had
     * wrapped u64 down to 0x80000000, so the shortcut never fired and every
     * caller had always been served by the real walk below.  Walking the PTE
     * is correct for every window and answers 0 for unmapped addresses. */
    u64 *pte;
    u64 big_buf;
    int r = walk(root, va, &pte, &big_buf);
    if (r < 0 || !(*pte & PG_P)) return 0;
    /* strip the flag bits (PG_NX lives in bit 63, far above the frame) */
    if (r == 1) /* 2 MiB large page */
        return (*pte & PTE_PA_MASK) | (va & 0x1fffff);
    return (*pte & PTE_PA_MASK) | (va & 0xfff);
}

bool vmm_writable_in(u64 root, u64 va)
{
    u64 *pte;
    u64 big_buf;
    int r = walk(root, va, &pte, &big_buf);
    if (r < 0) return false;
    return (*pte & (PG_P | PG_W)) == (PG_P | PG_W);
}

u64 vmm_unmap_user(u64 root, u64 va)
{
    u64 *pte;
    u64 big_buf;
    int r = walk(root, va, &pte, &big_buf);
    if (r < 0 || !(*pte & PG_P)) return 0;
    u64 pa;
    if (r == 1) {
        /* 2 MiB large page: need to clear the PD entry directly */
        u64 i4 = (va >> 39) & 511, i3 = (va >> 30) & 511;
        u64 i2 = (va >> 21) & 511;
        u64 *pd = ptable_ptr(ptable_ptr(ptable_ptr(root)[i4] & ~0xfff)[i3] & ~0xfff);
        pa = pd[i2] & PTE_PA_MASK;
        pd[i2] = 0;
    } else {
        pa = *pte & PTE_PA_MASK;
        *pte = 0;
    }
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    return pa;
}

bool vmm_is_user_range(u64 lo, u64 hi)
{ return lo >= USER_BASE && hi <= USER_MAX_VMA && lo < hi; }

void vmm_switch_to(u64 root)
{ __asm__ volatile("mov %0, %%cr3" ::"r"(root) : "memory"); }

u64 vmm_new_user_aspace(void)
{
    u64 root = pmm_alloc();
    if (!root) return 0; /* T-004: fork/exec must fail, not stop the machine */
    memset(ptable_ptr(root), 0, PAGE_SIZE);
    for (int i = 0; i < 512; i++)
        if (i != USER_SLOT) ptable_ptr(root)[i] = ptable_ptr(pml4_phys)[i];
    return root;
}

void vmm_destroy_user_aspace(u64 root)
{
    u64 *pml4 = ptable_ptr(root);
    for (int i4 = 0; i4 < 512; i4++) {
        if (!(pml4[i4] & PG_P) || i4 == 0 || i4 == 511) continue;
        u64 *pdpt = ptable_ptr(pml4[i4] & ~0xfff);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PG_P)) continue;
            u64 *pd = ptable_ptr(pdpt[i3] & ~0xfff);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PG_P)) continue;
                if (pd[i2] & PG_PS) {
                    pmm_free_order(pd[i2] & PTE_PA_MASK, 9);
                    continue;
                }
                u64 *pt = ptable_ptr(pd[i2] & ~0xfff);
                for (int i1 = 0; i1 < 512; i1++)
                    if (pt[i1] & PG_P) pmm_free(pt[i1] & PTE_PA_MASK);
                pmm_free(pd[i2] & ~0xfff);
            }
            pmm_free(pdpt[i3] & ~0xfff);
        }
        pmm_free(pml4[i4] & ~0xfff);
    }
    pmm_free(root);
}

extern void load_cr3(u64);

void vmm_init(void)
{
    volatile u64 *pdhi = (volatile u64 *)PHYS_TO_VIRT(PD_HI);

    /* Map EVERY managed physical frame through the high alias. Slots 0..31
     * already hold the kernel image; fill the rest of the buddy/heap range
     * plus any gap so PHYS_TO_VIRT() is valid for all of RAM.
     * Use the actual kernel end symbol instead of a hardcoded address. */
    extern u8 __kernel_end[];
    u64 kern_end_pa = ALIGN_UP(VIRT_TO_PHYS((uptr)__kernel_end), 0x200000UL);
    u64 lo = kern_end_pa;
    u64 hi = ALIGN_UP(heap_phys + heap_size, 0x200000UL);

    /* the framebuffer is MMIO: fb_init() maps it uncached at FB_VMA, so it
     * must not also get a cacheable high-half alias here */
    u64 fb_lo = 0, fb_hi = 0;
    if (bootinfo.fb.ok && bootinfo.fb.phys_addr) {
        fb_lo = bootinfo.fb.phys_addr;
        fb_hi = fb_lo + (u64)bootinfo.fb.pitch * bootinfo.fb.height;
    }

    /* ... and the PD slots the framebuffer window occupies must stay free so
     * fb_init() can install its own page tables there (a 1080p LFB spans
     * several 2 MiB slots).  start_kernel() keeps their physical pages out
     * of the buddy: they have no alias left. */
    u64 fb_slots = 0;
    if (fb_hi > fb_lo) {
        fb_slots = ((fb_hi - fb_lo) + 0x1FFFFFUL) >> 21;
        if (fb_slots > FB_PD_SLOTS) fb_slots = FB_PD_SLOTS;
    }

    /* PMM_WINDOW_TOP, not 3 GiB: past 1 GiB (pa >> 21) & 511 wraps back to
     * slot 0 and this loop would point the kernel's own alias at foreign
     * physical memory -- load_cr3() below would then fetch from it. */
    for (u64 pa = lo; pa < hi && pa < PMM_WINDOW_TOP; pa += 0x200000UL) {
        u64 idx = (pa >> 21) & 511;
        /* Only slots with a window installed by entry64.S (VGA=96,
         * devices=128) must not double as frame aliases.  Slots 97 and
         * 112 used to belong to the fixmap window -- that concept is gone
         * (FIXMAP_VA removed, zero users), so every managed frame gets
         * its alias again, as this loop's contract demands. */
        if (idx == 96 || idx == 128)
            continue; /* VGA / device MMIO windows */
        if (idx >= FB_PD_BASE && idx < FB_PD_BASE + fb_slots) continue; /* LFB window */
        if (fb_hi && pa < fb_hi && pa + 0x200000UL > fb_lo)
            continue; /* LFB page range */
        pdhi[idx] = pa | PG_P | PG_W | PG_PS;
    }

    /* kernel heap window (KHEAP_VMA -> PD_HI[256..271], 2 MiB pages) */
    for (int i = 0; i < 16; i++) pdhi[256 + i] = (heap_phys + i * 0x200000UL) | PG_P | PG_W | PG_PS;

    load_cr3(pml4_phys);
    kprintf("[vmm] CR3=0x%lx, RAM aliased up to 0x%lx\n", pml4_phys, hi);

    /* EFER.NXE is what gives PG_NX (PTE bit 63) its meaning.  The early asm
     * paths set it on the BSP and on every AP, but if one were missed the
     * whole user-page NX policy would silently stop working (a PTE with XD
     * set is simply executable then), so make sure it is on here. */
    u64 efer = rdmsr(MSR_IA32_EFER);
    if (!(efer & EFER_NXE)) wrmsr(MSR_IA32_EFER, efer | EFER_NXE);
    kprintf("[vmm] EFER.NXE=%d, user pages NX\n",
            (int)((rdmsr(MSR_IA32_EFER) & EFER_NXE) != 0));
}

/* Map a single physical page into the kernel's DEV_VMA window (PD slot 128).
 * va must be in [DEV_VMA, DEV_VMA + 2 MiB).  flags: PG_PCD etc. */
void vmm_map_kernel_page(u64 va, u64 pa, u64 flags)
{
    u64 *pdhi = (u64 *)PHYS_TO_VIRT(PD_HI);
    u64 pd_idx = (va >> 21) & 511;

    if (!(pdhi[pd_idx] & PG_P)) {
        /* allocate a fresh page table for this PD slot */
        u64 pt_pa = pmm_alloc();
        if (!pt_pa) panic("vmm: no frame for MMIO pt");
        memset(ptable_ptr(pt_pa), 0, PAGE_SIZE);
        pdhi[pd_idx] = pt_pa | PG_P | PG_W;
    }
    /* split: if it's a 2 MiB page we must not clobber it;
     * callers only target fresh slots so this is fine. */
    u64 *pt = ptable_ptr(pdhi[pd_idx] & ~0xfffUL);
    u64 pt_idx = (va >> 12) & 511;
    pt[pt_idx] = (pa & ~0xfffUL) | PG_P | PG_W | flags;
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
}

/* Deep-copy the user subtree of `src` into `dst` (both PML4 phys).
 * Source pages are reachable through the CURRENT address space. */

/* cleanup helper: free any partially-built page tables in dst's user slot */
static void dup_cleanup(u64 dst)
{
    u64 *d4 = ptable_ptr(dst);
    if (!(d4[USER_SLOT] & PG_P)) return;
    /* walk and free everything under USER_SLOT, then clear the entry */
    u64 *ddpt = ptable_ptr(d4[USER_SLOT] & ~0xfff);
    for (int i3 = 0; i3 < 512; i3++) {
        if (!(ddpt[i3] & PG_P)) continue;
        u64 *dpd = ptable_ptr(ddpt[i3] & ~0xfff);
        for (int i2 = 0; i2 < 512; i2++) {
            if (!(dpd[i2] & PG_P)) continue;
            if (dpd[i2] & PG_PS) {
                pmm_free_order(dpd[i2] & PTE_PA_MASK, 9);
                continue;
            }
            u64 *dpt = ptable_ptr(dpd[i2] & ~0xfff);
            for (int i1 = 0; i1 < 512; i1++)
                if (dpt[i1] & PG_P) pmm_free(dpt[i1] & PTE_PA_MASK);
            pmm_free(dpd[i2] & ~0xfff);
        }
        pmm_free(ddpt[i3] & ~0xfff);
    }
    pmm_free(d4[USER_SLOT] & ~0xfff);
    d4[USER_SLOT] = 0;
}

int dup_user_aspace(u64 src, u64 dst)
{
    u64 slot = (u64)USER_SLOT << 39;

    u64 *s4 = ptable_ptr(src);
    if (!(s4[USER_SLOT] & PG_P)) return 0; /* nothing mapped yet */

    u64 *d4 = ptable_ptr(dst);
    u64 d3_pa = pmm_alloc(), s3 = s4[USER_SLOT] & ~0xfffUL;
    if (!d3_pa) return LNXRM_EFAIL;
    memset(ptable_ptr(d3_pa), 0, PAGE_SIZE);
    d4[USER_SLOT] = d3_pa | PG_P | PG_W | PG_U;

    u64 *sdpt = ptable_ptr(s3), *ddpt = ptable_ptr(d3_pa);
    for (int i3 = 0; i3 < 512; i3++) {
        if (!(sdpt[i3] & PG_P)) continue;
        u64 d2_pa = pmm_alloc();
        if (!d2_pa) goto fail;
        memset(ptable_ptr(d2_pa), 0, PAGE_SIZE);
        ddpt[i3] = d2_pa | PG_P | PG_W | PG_U;
        u64 *spd = ptable_ptr(sdpt[i3] & ~0xfff), *dpd = ptable_ptr(d2_pa);

        for (int i2 = 0; i2 < 512; i2++) {
            if (!(spd[i2] & PG_P)) continue;
            u64 d1_pa = pmm_alloc();
            if (!d1_pa) goto fail;
            memset(ptable_ptr(d1_pa), 0, PAGE_SIZE);
            dpd[i2] = d1_pa | PG_P | PG_W | PG_U;
            u64 *spt = ptable_ptr(spd[i2] & ~0xfff), *dpt = ptable_ptr(d1_pa);

            for (int i1 = 0; i1 < 512; i1++) {
                if (!(spt[i1] & PG_P)) continue;
                u64 va = slot | ((u64)i3 << 30) | ((u64)i2 << 21) | ((u64)i1 << 12);
                /* defensive: only copy pages that truly resolve */
                u64 spa = vmm_translate_in(src, va);
                if (!spa) continue;
                u64 npa = pmm_alloc();
                if (!npa) goto fail;
                /* Read the source through the high-half alias of the frame
                 * the SRC page table names -- never through `va`.  `va`
                 * would resolve against the live CR3 (wrong address space
                 * if it is not `src`) and, when it does resolve to a user
                 * page, SMAP faults a ring-0 load with no STAC window.  The
                 * alias is a kernel page: neither hazard applies. */
                memcpy((void *)PHYS_TO_VIRT(npa), (void *)PHYS_TO_VIRT(spa), PAGE_SIZE);
                /* copy the flags, PG_NX included: fork must not turn an
                 * executable page into a data page (or vice versa) */
                dpt[i1] = npa | (spt[i1] & (0xFFFUL | PG_NX));
            }
        }
    }
    return 0;

fail:
    dup_cleanup(dst);
    return LNXRM_EFAIL;
}

/* T-032: count the user pages really mapped under `root`.
 *
 * This is the number the memory account is refreshed from (exec, fork,
 * brk) and therefore the number ps prints.  Counting leaf entries instead
 * of adding up what each mapping site claims means a page can never be
 * forgotten: a shared image page mapped twice still counts once, because
 * there is one leaf entry for one VA, and a mapping site that nobody
 * remembered to instrument still shows up on the next refresh.
 *
 * User space hangs off one PML4 slot (USER_SLOT), and every leaf in it is
 * a 4 KiB page -- vmm_map_user never builds a huge page -- so a present
 * entry at PD level is a table to descend, not 512 pages to count. */
u64 vmm_count_user_pages(u64 root)
{
    if (!root) return 0;
    u64 *d4 = ptable_ptr(root);
    if (!(d4[USER_SLOT] & PG_P)) return 0; /* no user space at all */
    u64 *d3 = ptable_ptr(d4[USER_SLOT] & ~0xfffUL);
    u64 n = 0;

    for (int i3 = 0; i3 < 512; i3++) {
        if (!(d3[i3] & PG_P)) continue;
        u64 *d2 = ptable_ptr(d3[i3] & ~0xfffUL);
        for (int i2 = 0; i2 < 512; i2++) {
            if (!(d2[i2] & PG_P)) continue;
            u64 *d1 = ptable_ptr(d2[i2] & ~0xfffUL);
            for (int i1 = 0; i1 < 512; i1++)
                if (d1[i1] & PG_P) n++;
        }
    }
    return n;
}
