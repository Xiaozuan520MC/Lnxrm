// SPDX-License-Identifier: Apache-2.0

/* The synthetic user #PF a test case builds by hand to drive the page-fix
 * path without waiting for a real fault, plus the one helper every case
 * that needs a user page mapped in shares: fault the range in and answer
 * the frame it landed on.  t_mmap.c keeps its own copy of pf_frame with a
 * wider error-code set (write / fetch codes); the cases that share this
 * header only ever raise a not-present read. */
#ifndef LNXRM_KFAULT_H
#define LNXRM_KFAULT_H

#include <types.h>
#include <console.h>     /* memset */
#include <sys/sched.h>   /* struct intr_frame, current */
#include <mm/mm.h>       /* vmm_translate_in */
#include <mm/pagefault.h> /* page_fault_try_fix */

#define PF_NP_R 0x00 /* not present, read: the fixers run, cs = 3 says
                      * ring 3 so it reads as a user access */

static inline void pf_frame(struct intr_frame *f, u64 err)
{
    memset(f, 0, sizeof(*f));
    f->cs = 3;
    f->err = err;
}

/* Fault a shared range in and answer the frame it landed on (0 on failure). */
static inline u64 map_in(long va)
{
    struct intr_frame f;
    pf_frame(&f, PF_NP_R);
    if (!page_fault_try_fix(&f, (u64)va)) return 0;
    return vmm_translate_in(current->pml4, (u64)va);
}

#endif
