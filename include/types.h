#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <abi/lnxrm_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uintptr_t uptr;
typedef intptr_t iptr;

#ifndef NULL
#define NULL ((void *)0)
#endif
#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define MIN(a, b)        ((a) < (b) ? (a) : (b))
#define MAX(a, b)        ((a) > (b) ? (a) : (b))

/* Virtual address map (high half, PML4[511] -> PDPT[510]) */
/* High-half special windows. Each sits at offset 0 of its own 2 MiB
 * PD slot (slot number = (VMA>>21)&511), so the leaf PTE is always PT[0]:
 *   kernel image : slots 0..31
 *   VGA          : slot 96
 *   devices      : slot 128
 *   framebuffer  : slots 104..111
 *   kernel heap  : slots 256..271 */
#define VGA_VMA   0xffffffff8c000000UL /* PD_HI[96]     */
#define DEV_VMA   0xffffffff90000000UL /* PD_HI[128]    */
#define KHEAP_VMA 0xffffffffa0000000UL /* PD_HI[256...] */

/* The PD slot of a high-half window is (VMA >> 21) & 511.  These asserts pin
 * the comment above to reality: the slot numbers were once 98/97 in a stale
 * comment and nothing caught it.  If a VMA moves, the matching page-directory
 * wiring in entry64.S must move too -- and this fails the build if it is
 * forgotten.  (Macro because types.h is included from both C and C++ TUs.) */
#define PD_SLOT_OF(vma) (((vma) >> 21) & 511)
#ifdef __cplusplus
#define LNXRM_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define LNXRM_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
LNXRM_STATIC_ASSERT(PD_SLOT_OF(VGA_VMA) == 96, "VGA_VMA must sit in PD_HI[96]");
LNXRM_STATIC_ASSERT(PD_SLOT_OF(DEV_VMA) == 128, "DEV_VMA must sit in PD_HI[128]");
LNXRM_STATIC_ASSERT(PD_SLOT_OF(KHEAP_VMA) == 256, "KHEAP_VMA must sit in PD_HI[256]");
/* high window base = KERNEL_VMA - KERNEL_LMA */
#define PHYS_TO_VIRT(p) ((uptr)(p) + 0xffffffff80000000UL)
#define VIRT_TO_PHYS(v) ((uptr)(v) - 0xffffffff80000000UL)

/* User space occupies PML4 slot 255 exclusively; the kernel keeps its
 * identity map of low RAM in slot 0 of EVERY address space. */
#define USER_BASE      0x7f8000000000UL
#define USER_STACK_TOP 0x7fffffbff000UL
#define USER_MAX_VMA   0x7ffffffff000UL

/* Anonymous-mmap window, top-down: [MMAP_LO, MMAP_TOP).
 *
 * MMAP_TOP stops STACK_GUARD below the stack growth floor (USER_STACK_TOP
 * minus the 512 KiB USTACK_MAX_GROW in mm/pagefault.c); those STACK_GUARD
 * bytes are the stack guard, mapped by NOBODY -- no VMA may cover them (the
 * window ends here) and demand_page() refuses them outright -- so a stack
 * overflow crossing the floor dies as SIGSEGV instead of being demand-paged
 * into an mmap VMA sitting at the top of the window.  The guard is exactly
 * the reach stack growth accepts below the running RSP (STACK_GROW_WINDOW
 * in pagefault.c, static-asserted equal), so no fault growth would treat as
 * a stack access can land past it.
 *
 * The window is 64 GiB wide -- address space here is free, physical frames
 * are not, and demand paging (page_fault.c) meters the frames one page at a
 * time through cred_mem_quota(), so a huge mapping costs nothing until it
 * is touched.
 *
 * sys_brk's ceiling is MMAP_LO: the old cap, one MiB below
 * USER_STACK_TOP, sat INSIDE this window -- a heap and an mmap region could
 * then have claimed the same pages. */
#define STACK_GUARD 0x10000UL /* 64 KiB: [MMAP_TOP, stack floor), never mapped */
#define MMAP_TOP (USER_STACK_TOP - 0x80000UL - STACK_GUARD)
#define MMAP_LO  (MMAP_TOP - 0x1000000000UL)          /* 64 GiB below */

#define PAGE_SIZE 4096UL

/* Top page of user space: the sigreturn restorer.  It has to be executable
 * (the handler `ret`s into it) and it must not live on the stack, because
 * every other user page is NX.  Nothing else maps above the stack: brk
 * stops at MMAP_LO and the mmap window stops at MMAP_TOP (the stack
 * floor), so the top page is free. */
#define USER_SIGRETURN_VA (USER_MAX_VMA - PAGE_SIZE)

#define NR_TASKS   64
#define NR_FDS     16
#define MAX_CPUS   8
#define CONFIG_SMP 1

#ifdef __cplusplus
}
#endif
