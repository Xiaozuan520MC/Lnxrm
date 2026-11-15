#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- physical frame buddy allocator (mm/pmm.c) ---- */

/* Identity map and high-half alias cover exactly 2 GiB: PHYS_TO_VIRT(p) wraps
 * into the low canonical half at p == 2 GiB, so neither the buddy window nor
 * the vmm_init() alias loop may cross it (past it the seeding loop wrote
 * beyond the mapping and triple-faulted before idt_init()).  Going further
 * needs a piecewise alias, not another page directory. */
#define PMM_WINDOW_TOP 0x80000000UL
void pmm_init(void);
void pmm_reserve(u64 lo, u64 hi); /* keep [lo,hi) out of the free lists (MMIO) */
u64 pmm_alloc_order(int order); /* phys addr or 0 */
void pmm_free_order(u64 pa, int order);
u64 pmm_free_bytes(void);  /* bytes currently on the buddy free lists */
int pmm_max_order(void);   /* first refused order (0..max-1 are valid) */

/* ---- order-0 frame reference counts (mm/pmm.c) ----
 *
 * Only order-0 frames are tracked: they are the only ones a user mapping can
 * hold, and a COW page shared by two address spaces is one frame with two
 * owners.  pmm_alloc() starts the count at 1, pmm_free() drops it, and the
 * frame returns to the buddy only at 0 -- "unmap in one space" merely drops
 * that space's reference.  A count of 0 at free time means nothing tracked it
 * and is handed back as always; the counter must never make a frame
 * un-freeable.  Higher orders bypass it. */
void pmm_ref(u64 pa);      /* one more owner (fork shares a frame) */
u16 pmm_refcount(u64 pa);  /* current owners (ktest pins the semantics) */
/* ktest only: make the next `n` frame allocations fail (0 = off), so the OOM
 * branches can be tested without running the machine out of memory. */
void pmm_inject_oom(int n);
#define pmm_alloc()  pmm_alloc_order(0)
#define pmm_free(pa) pmm_free_order((pa), 0)

/* ---- out-of-memory killer (mm/oom.c) ----
 *
 * No reclaim, no swap: a user fault that cannot get a frame ends the task
 * holding the most user pages.  A failure that can be ANSWERED -- brk's
 * ENOMEM, kmalloc's NULL -- keeps answering and never kills.  One victim per
 * shortage: the latch set by oom_kill_largest() is released only when a frame
 * is handed out again, which pmm_alloc_order() reports to oom_frame_granted(). */
struct task;
struct task *oom_select_victim(void); /* the heuristic, pure: biggest mem_pages */
bool oom_kill_largest(void);          /* choose + SIGKILL + report + latch */
void oom_frame_granted(void);         /* a frame went out: the next shortage may kill */

/* ---- kernel heap (mm/kheap.c) ----
 * NBINS power-of-two free lists: bin 0 serves 16 bytes, bin 8 serves 4096.
 * Both boundaries are pinned by ktest/t_heap.c. */
#define KHEAP_NBINS 9
void kheap_init(void);
/* NULL when the request cannot be satisfied (T-004): OOM is an error the
 * caller handles, not a reason to stop the machine. */
void *kmalloc(size_t n);
/* kmalloc(), panicking on failure -- only for bring-up paths with no error
 * path of their own.  Never on a user-reachable path. */
void *kmalloc_or_panic(size_t n);
void kfree(void *p);
int kheap_bin_of(size_t n);  /* bin a size maps to; >= KHEAP_NBINS = no bin */
u64 kheap_used_bytes(void);  /* bytes handed out (test snapshots this) */

/* ---- virtual memory (mm/vmm.c) ----
 * Master tables are shared by every address space (PML4 entries copied).
 * User space is a per-process tree under PML4[0]. */
void vmm_init(void); /* fills heap window, switches cr3 */
u64 master_pml4_phys(void);
/* Physical extent backing the KHEAP_VMA window.  Declared here, not at the
 * call site: a file-local prototype silently gets C++ linkage once the caller
 * is a .cpp, which is how renaming kheap.c broke the link. */
void vmm_heap_region(u64 *phys_out, u64 *size_out);
u64 vmm_new_user_aspace(void); /* pml4 phys for a process; 0 if there is no
                                * frame for the root (T-004) */
void vmm_destroy_user_aspace(u64 pml4);
/* Map one user page.  `exec == false` marks it NX (PG_NX), so a stray jump
 * into user data faults instead of running.  If the page is already mapped
 * the new permissions are merged in (widening only) and `pa` is ignored.
 * Returns LNXRM_ENOMEM when the page tables reaching `va` cannot be
 * allocated: a growing heap runs out of frames long before address space. */
int vmm_map_user(u64 pml4, u64 va, u64 pa, bool writable, bool user, bool exec);
u64 vmm_unmap_user(u64 pml4, u64 va); /* returns freed pa or 0 */
/* Drop the page TABLES above `va` once unmapping it empties every level.
 * Without this each freshly touched 2 MiB region strands two table frames for
 * the life of the address space.  Never touches a level still holding a
 * present entry. */
void vmm_reclaim_empty_tables(u64 pml4, u64 va);
u64 vmm_translate_in(u64 pml4, u64 va);
/* Raw 4 KiB leaf PTE for `va` (0 = not present / no table).  Callers that
 * need the flags -- PG_COW above all -- read them here. */
u64 vmm_user_pte(u64 pml4, u64 va);
/* Overwrite that leaf PTE and flush the TLB entry: how PG_W/PG_COW are
 * flipped on a live page (fork's COW conversion, the COW fault fixer, ktest
 * restoring borrowed pages).  LNXRM_EFAIL if there is no such leaf. */
int vmm_user_pte_write(u64 pml4, u64 va, u64 pte);
/* Present AND writable at `va`.  With CR0.WP on this is the precondition for
 * every ring-0 store through a user address: the R/W bit decides whether the
 * store lands or raises #PF. */
bool vmm_writable_in(u64 root, u64 va);
void vmm_switch_to(u64 pml4);
bool vmm_is_user_range(u64 lo, u64 hi);
int dup_user_aspace(u64 src, u64 dst);
/* T-032: user pages `root` actually maps right now.  It walks the leaf tables,
 * so it is right for every path that maps a page without any of them having
 * to bump a counter (hand-maintained counters drift).  One scan per populated
 * table; the only callers do it once per exec, fork or brk. */
u64 vmm_count_user_pages(u64 root);

#define PG_P   0x001
#define PG_W   0x002
#define PG_U   0x004
#define PG_PWT 0x008
#define PG_PCD 0x010
/* Hardware-set: the CPU raises Accessed on any translation through this entry
 * and Dirty on a WRITE through it; only a write to the PTE clears them.
 * Nothing reads PG_A; PG_D is the whole dirty story for MAP_SHARED file
 * mappings -- a shared page is written back only when bit 6 says somebody
 * stored to it, so a read-only page costs no I/O on unmap/exit. */
#define PG_A   0x020
#define PG_D   0x040
#define PG_PS  0x080
/* Software bit 9 of a 4 KiB PTE: Copy-On-Write (bits 9..11 are ignored by the
 * CPU).  Set ONLY alongside a cleared PG_W, on both sides of a forked frame:
 * PG_W clear + PG_COW set is a COW break (copy, take it writable); PG_W clear
 * WITHOUT PG_COW is a genuinely read-only page (text/.rodata) and stays a
 * SIGSEGV.  Lives here so the page-table vocabulary stays in one header. */
#define PG_COW 0x200
/* In a PDE this bit means "big page"; in a 4 KiB PTE it is the third PAT index
 * bit, which is how a mapping reaches a memory type outside PAT slots 0-3. */
#define PG_PAT PG_PS

#define PG_NX  0x8000000000000000ULL /* XD: needs EFER.NXE */

/* PAT index for MMIO: {PCD,PWT} pick the PAT entry (i = 2*PCD+PWT), which the
 * MTRRs combine with.  PG_PCD|PG_PWT (PAT[3] = UC) overrides every firmware
 * MTRR -- use it for a register block that must work unattended.  PG_PCD
 * alone (UC-) inherits the firmware's WC, right for a display aperture and
 * wrong for registers, where a combined store can land after the doorbell it
 * was meant to precede; framebuffer.c programs IA32_PAT when it needs WC. */

/* Physical frame bits of a PTE: bits 51..12.  Everything else is a flag,
 * including PG_NX in bit 63 -- masking with ~0xfff would leak it into
 * addresses handed to the frame allocator. */
#define PTE_PA_MASK 0x000FFFFFFFFFF000ULL

/* kernel-only MMIO mapping into the DEV_VMA window (PD slot 128, 2 MiB):
 * DEV_VMA + offset -> phys, one 4 KiB page. */
void vmm_map_kernel_page(u64 va, u64 pa, u64 flags);

#ifdef __cplusplus
}
#endif