#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- physical frame buddy allocator (mm/pmm.c) ---- */

/* The identity map AND the high-half alias cover exactly 2 GiB: entry64.S
 * fills PDPT_M[0..1] and PDPT_HI[510..511] (one 512 x 2 MiB page directory
 * per GiB), and PHYS_TO_VIRT(p) = p + 0xffffffff80000000 wraps into the low
 * canonical half at p == 2 GiB.  The buddy dereferences frames as identity
 * addresses and vmm_init() aliases them one 2 MiB slot per page directory,
 * so neither the managed window nor the alias loop may cross this line.
 * It used to be 4 GiB in pmm.c and 3 GiB in vmm.c: on any machine with more
 * RAM than the window the seeding loop wrote past the mapped alias, #PF'd
 * before idt_init() had run, and the box triple-faulted into a silent reset
 * right after "[ktest] stage early".  Pushing past 2 GiB needs a piecewise
 * alias (the linear PHYS_TO_VIRT formula has no address room above the top
 * of the high half), not merely another page directory. */
#define PMM_WINDOW_TOP 0x80000000UL
void pmm_init(void);
void pmm_reserve(u64 lo, u64 hi); /* keep [lo,hi) out of the free lists (MMIO) */
u64 pmm_alloc_order(int order); /* phys addr or 0 */
void pmm_free_order(u64 pa, int order);
u64 pmm_free_bytes(void);  /* bytes currently on the buddy free lists */
/* ktest only: make the next `n` frame allocations fail (0 = off).  This is
 * how the OOM branches are tested without actually running the machine out
 * of memory; every caller must turn the arm back off before asserting. */
void pmm_inject_oom(int n);
#define pmm_alloc()  pmm_alloc_order(0)
#define pmm_free(pa) pmm_free_order((pa), 0)

/* ---- kernel heap (mm/kheap.c) ----
 * NBINS power-of-two free lists: bin 0 serves 16 bytes, bin 8 serves 4096.
 * The count and the bin function are published because the boot self-test
 * pins both boundaries (ktest/t_heap.c). */
#define KHEAP_NBINS 9
void kheap_init(void);
/* Returns NULL when the request cannot be satisfied (T-004): OOM is an
 * error the caller handles, not a reason to stop the machine. */
void *kmalloc(size_t n);
/* kmalloc(), panicking on failure -- for bring-up paths that have no error
 * path of their own (C++ operator new).  Never use it on a user-reachable
 * path. */
void *kmalloc_or_panic(size_t n);
void kfree(void *p);
int kheap_bin_of(size_t n);  /* bin a size maps to; >= KHEAP_NBINS = no bin */
u64 kheap_used_bytes(void);  /* bytes handed out (test snapshots this) */

/* ---- virtual memory (mm/vmm.c) ----
 * Master tables are shared by every address space (PML4 entries copied).
 * User space is a per-process tree under PML4[0]. */
void vmm_init(void); /* fills heap window, switches cr3 */
u64 master_pml4_phys(void);
/* Physical extent backing the KHEAP_VMA window.  Declared here rather than
 * re-declared at the call site: a file-local prototype silently gets C++
 * linkage once the caller is a .cpp, which is how renaming kheap.c broke the
 * link. */
void vmm_heap_region(u64 *phys_out, u64 *size_out);
u64 vmm_new_user_aspace(void); /* pml4 phys for a process; 0 if there is no
                                * frame for the root (T-004) */
void vmm_destroy_user_aspace(u64 pml4);
/* Map one user page.  `exec == false` marks it NX (PG_NX): everything that
 * is not code -- stack, heap, data segments -- is non-executable, which is
 * what makes a stray jump into user data fault instead of run.  If the page
 * is already mapped the new permissions are merged in (widening only) and
 * `pa` is ignored; pass 0 in that case.
 * Returns LNXRM_ENOMEM (mapping nothing) when the page tables that reach
 * `va` cannot be allocated: a growing heap runs out of frames long before
 * it runs out of address space (T-004). */
int vmm_map_user(u64 pml4, u64 va, u64 pa, bool writable, bool user, bool exec);
u64 vmm_unmap_user(u64 pml4, u64 va); /* returns freed pa or 0 */
u64 vmm_translate_in(u64 pml4, u64 va);
/* Present AND writable at `va` in `root`.  With CR0.WP on this is the
 * precondition for every ring-0 store through a user address: the R/W bit
 * is what decides whether the store lands or raises #PF. */
bool vmm_writable_in(u64 root, u64 va);
void vmm_switch_to(u64 pml4);
bool vmm_is_user_range(u64 lo, u64 hi);
int dup_user_aspace(u64 src, u64 dst);
/* T-032: how many user pages does `root` actually map right now?  It walks
 * the leaf tables, so it is right for every path that maps a page -- image,
 * stack, sigreturn restorer, heap -- without any of them having to remember
 * to bump a counter (hand-maintained counters drift; C24 was two of them
 * disagreeing).  Cost is one scan per populated table, i.e. one per 2 MiB of
 * heap, and the only callers do it once per exec, fork or brk. */
u64 vmm_count_user_pages(u64 root);

#define PG_P   0x001
#define PG_W   0x002
#define PG_U   0x004
#define PG_PWT 0x008
#define PG_PCD 0x010
#define PG_PS  0x080
/* In a PDE this bit means "big page"; in a 4 KiB PTE it is the third PAT
 * index bit (index = PAT*4 + PCD*2 + PWT), which is what reaches a memory
 * type outside slots 0-3 of IA32_PAT.  vmm.c already relies on PG_PS ==
 * PTE.PAT when it splits a 2 MiB page. */
#define PG_PAT PG_PS

#define PG_NX  0x8000000000000000ULL /* XD: needs EFER.NXE */

/* Cache type for MMIO.  {PCD,PWT} index the PAT (i = 2*PCD+PWT) and that
 * entry is then combined with the MTRRs.  Intel SDM Table 9-2 ("Effective
 * Memory Type"):
 *
 *     PAT    MTRR=WB/WT    MTRR=WC    MTRR=UC
 *     WB     WB            WC         UC
 *     WC     WC            WC         UC
 *     UC-    UC            WC         UC
 *     UC     UC            UC         UC
 *
 * So PG_PCD alone (PAT[2] = UC-) already means uncacheable when nothing
 * covers the range -- it is NOT a silent fall back to WB.  What it does
 * inherit is the firmware's WC MTRR (fine for an aperture, wrong for a
 * register block), and MTRR=WP leaves it architecturally undefined.
 * PG_PCD|PG_PWT (PAT[3] = UC) overrides every MTRR type, which is the only
 * encoding whose answer does not depend on firmware this kernel never
 * programs: use it for a register block that has to work unattended.
 *
 * WC combines *stores* into full bursts, so it is right for a display
 * aperture that occasionally has to be rewritten wholesale (framebuffer.c
 * maps the LFB that way) and wrong for a register block, where a combined,
 * reordered store can land after the doorbell it was supposed to precede.
 *
 * Getting WC does not come for free, though.  A PTE can reach all eight PAT
 * slots -- index = PG_PAT*4 + PG_PCD*2 + PG_PWT -- so when a slot holds WC it
 * is there for the asking; but the value firmware leaves behind normally
 * holds none at all (the reset layout is [WB, WT, UC-, UC] twice over, which
 * is exactly what QEMU reports, and it is why Linux's PAT init exists at all:
 * "the PAT MSR must be updated by Linux in order to support WC and WT
 * attributes").  In that case IA32_PAT really is the obstacle, and rewriting
 * it is per logical processor, which is the part this kernel has not done.
 * Whichever half of the combination ends up deciding, an MTRR the firmware
 * set to UC still wins -- so framebuffer.c reports what actually came out of
 * it instead of promising it. */

/* Physical frame bits of a PTE: bits 51..12 (everything else is a flag,
 * including PG_NX in bit 63 -- masking with ~0xfff would leak it into
 * addresses handed to the frame allocator). */
#define PTE_PA_MASK 0x000FFFFFFFFFF000ULL

/* kernel-only MMIO mapping into DEV_VMA window (PD slot 128, 2 MiB).
 * Maps one 4 KiB page: DEV_VMA + offset → phys. */
void vmm_map_kernel_page(u64 va, u64 pa, u64 flags);

#ifdef __cplusplus
}
#endif
