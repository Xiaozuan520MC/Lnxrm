/* Frame-pointer backtrace (PP2, GAP section 5.3).
 *
 * The chain needs -fno-omit-frame-pointer (Makefile BASEFLAGS): at -Os
 * clang drops rbp otherwise and there is nothing to walk.  Deliberately
 * simple -- no DWARF (the .debug sections never travel in bzImage) and no
 * in-kernel symbol table, so addresses print raw and resolve against
 * `nm build/vmlinux.elf`.
 *
 * Every frame is validated before it is followed, because this runs from
 * panic() where a fault on a wild rbp would only buy a nested panic:
 *   - kernel-canonical, 8-aligned, and mapped (vmm_translate_in) for both
 *     words read -- a garbage rbp must stop the walk, not fault it;
 *   - the return address lies in [__text_start, __text_end): the asm
 *     stubs in entry64.S build no rbp chain, so a chain that reaches one
 *     ends here instead of wandering into data;
 *   - frames ascend (caller rbp > callee rbp -- the stack grows down).
 * User rbp values and unmapped slots simply stop the walk; the depth cap
 * bounds it even if every check were to be fooled. */
#include <console.h>
#include <mm/mm.h> /* vmm_translate_in */

int backtrace(uptr *frames, int max)
{
    if (max <= 0) return 0;

    extern char __text_start[], __text_end[];
    uptr lo = (uptr)__text_start, hi = (uptr)__text_end;
    u64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    /* Own frame: [fp] = caller's rbp, [fp+8] = return into the caller. */
    uptr fp = (uptr)__builtin_frame_address(0);
    int n = 0;
    while (n < max) {
        if (fp < 0xffff800000000000UL || (fp & 7)) break;
        if (!vmm_translate_in(cr3, fp) || !vmm_translate_in(cr3, fp + 8)) break;
        uptr next = *(uptr *)fp;
        uptr ret = *(uptr *)(fp + 8);
        if (ret < lo || ret >= hi) break;
        if (next <= fp) break;
        frames[n++] = ret;
        fp = next;
    }
    return n;
}
