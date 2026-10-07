/* Global C++ runtime `new`, routed to the kernel heap.
 * Freestanding build: no libc++ provides this symbol, so every C++
 * `new` in the kernel lands here.  (The kernel never `delete`s, so no
 * `delete`/`new[]`/`delete[]` overrides are needed.)
 *
 * `new` has no exception to throw here, so it is the one place that turns
 * an allocation failure into a panic -- which is why every C++ allocation
 * in this kernel sits on a bring-up path (AHCI probe).  Anything a user
 * program can reach uses kmalloc() and handles NULL instead (T-004). */
#include <mm/mm.h>

extern "C" {
void panic(const char *fmt, ...);
}

void *operator new(size_t sz)
{
    return kmalloc_or_panic(sz);
}
