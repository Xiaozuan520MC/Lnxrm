#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* returns nonzero when the device is claimed */
typedef int (*pci_match_fn)(void *ctx, u8 bus, u8 dev, u8 fn, u16 vendor, u16 devid, u8 base_class);

void pci_scan(pci_match_fn fn, void *ctx);
u32 pci_read(u8 bus, u8 dev, u8 fn, u8 off);
void pci_write(u8 bus, u8 dev, u8 fn, u8 off, u32 val);

/* AHCI/SATA controller (drivers/ahci.c) — a PCI class 01h/06h device. */
void ahci_init(void);

static inline void pci_enable_bm(u8 b, u8 d, u8 f)
{
    u32 cmd = pci_read(b, d, f, 0x04);
    pci_write(b, d, f, 0x04, cmd | 0x7); /* IO+MEM+BUSMASTER */
}

#ifdef __cplusplus
}
#endif
