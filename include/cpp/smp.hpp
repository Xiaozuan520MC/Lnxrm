/* SMP management interface (C++ header).
 * Provides the SMP Manager class. IPI vectors and the C entry points
 * (smp_init/ap_main) live in sys/smp.h, which includes this header. */
#pragma once
#ifdef __cplusplus

#include <types.h>

/* AP startup timeout in milliseconds */
/* How long to wait for an AP to signal ready. QEMU APs respond within
 * ~20 ms; a dead APIC ID probe burns this full budget twice (2-fail
 * scan rule), so keep it tight — 100 ms is still 5x the observed time. */
#define AP_STARTUP_TIMEOUT_MS 100

namespace smp
{

class Manager
{
  public:
    /* Initialize SMP: enumerate and start all APs. Called from start_kernel(). */
    static void init();
};

} // namespace smp

#endif
