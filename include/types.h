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
/* high window base = KERNEL_VMA - KERNEL_LMA */
#define PHYS_TO_VIRT(p) ((uptr)(p) + 0xffffffff80000000UL)
#define VIRT_TO_PHYS(v) ((uptr)(v) - 0xffffffff80000000UL)

/* User space occupies PML4 slot 255 exclusively; the kernel keeps its
 * identity map of low RAM in slot 0 of EVERY address space. */
#define USER_BASE      0x7f8000000000UL
#define USER_STACK_TOP 0x7fffffbff000UL
#define USER_MAX_VMA   0x7ffffffff000UL

#define PAGE_SIZE 4096UL

/* Top page of user space: the sigreturn restorer.  It has to be executable
 * (the handler `ret`s into it) and it must not live on the stack, because
 * every other user page is NX.  Nothing else maps above the stack, and brk
 * is capped a megabyte below USER_STACK_TOP, so the top page is free. */
#define USER_SIGRETURN_VA (USER_MAX_VMA - PAGE_SIZE)

#define NR_TASKS   64
#define NR_FDS     16
#define MAX_CPUS   8
#define CONFIG_SMP 1

#ifdef __cplusplus
}
#endif
