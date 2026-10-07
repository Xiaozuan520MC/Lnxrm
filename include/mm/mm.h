#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- physical frame buddy allocator (mm/pmm.c) ---- */

/* The identity map AND the high-half alias cover exactly 1 GiB: entry64.S
 * fills PDPT_M[0] and PDPT_HI[510] only (512 x 2 MiB each), and
 * PHYS_TO_VIRT(p) = p + 0xffffffff80000000 wraps into the low canonical
 * half at p == 2 GiB.  The buddy dereferences frames as identity addresses
 * and vmm_init() aliases them one 2 MiB slot per 1 GiB of physical, so
 * neither the managed window nor the alias loop may cross this line.
 * It used to be 4 GiB in pmm.c and 3 GiB in vmm.c: on any machine with
 * more than ~1.1 GiB of RAM the seeding loop wrote at 0x40000000, #PF'd
 * before idt_init() had run, and the box triple-faulted into a silent
 * reset right after "[ktest] stage early".  Every QEMU run was -m 256. */
#define PMM_WINDOW_TOP 0x40000000UL
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
#define PG_PCD 0x010
#define PG_PS  0x080
#define PG_NX  0x8000000000000000ULL /* XD: needs EFER.NXE */

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
