/* SMP initialization: AP startup via INIT-SIPI-SIPI.
 * C-compatible header — includes smp.hpp when compiled as C++. */
#pragma once
#include <types.h>

#ifndef IPI_VECTOR_RESCHEDULE
#define IPI_VECTOR_RESCHEDULE 0xF0
#define IPI_VECTOR_STOP       0xF2
#endif

#ifdef __cplusplus
#include "cpp/smp.hpp"
extern "C" {
#endif

/* AP trampoline code (must be at physical address < 1 MiB). */
extern u8 trampoline_start[];
extern u8 trampoline_end[];

/*
 * Trampoline shared data layout — all fields u64-aligned so the assembly
 * can use `mov rax, [base + offset]` without crossing field boundaries.
 *
 * Physical base address: 0x9000 (below 1 MiB, identity-mapped).
 *
 * Offset  Size  Field           Description
 * ------  ----  -----           -----------
 * 0x00      8   page_dir        physical PML4 address
 * 0x08      8   stack_top       64-bit virtual address of per-AP stack top
 * 0x10      8   ready           AP sets to 1 when initialized
 * 0x18      8   ap_id           logical CPU id assigned by BSP
 * 0x20      8   gdt_packed      bits [15:0] = GDT limit for lgdt
 *                               bits [63:16] = GDT physical base (>> 16)
 * 0x28      8   ap_main_phys    physical address of ap_main()
 * 0x30      8   (reserved)
 * 0x38      8   (reserved)
 */
struct trampoline_data {
    volatile u64 page_dir;     /* 0x00 */
    volatile u64 stack_top;    /* 0x08 */
    volatile u64 ready;        /* 0x10 */
    volatile u64 ap_id;        /* 0x18 */
    volatile u64 gdt_packed;   /* 0x20 */
    volatile u64 ap_main_phys; /* 0x28 */
    volatile u64 _rsvd0;       /* 0x30 */
    volatile u64 _rsvd1;       /* 0x38 */
};

/* Located at a fixed physical address for trampoline access. */
#define TRAMPOLINE_CODE_PHYS 0x8000UL

/* AP GDT placed right after trampoline code (physical, below 1 MiB).
 * Layout:
 *   0x00  null
 *   0x08  32-bit kernel code  (L=0, D=1) — PM entry target
 *   0x10  kernel data         (D/B=1)
 *   0x18  64-bit kernel code  (L=1, D=0) — long mode jump target
 *   0x20  kernel data         (same as 0x10, for DS/ES/SS in LM)
 */
#define AP_GDT_PHYS  0x8800UL
#define AP_GDT_LIMIT 0x27U /* 5 entries × 8 bytes − 1 */

/* Called by BSP during boot to enumerate and start all APs. */
void smp_init(void);

/* Entry point for each AP once it wakes up (in trampoline.S). */
void ap_main(u32 ap_id);

#ifdef __cplusplus
}
#endif
