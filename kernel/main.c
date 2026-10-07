/* lnxrm C entry point: subsystem bring-up in dependency order. */
#include <boot.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/cpu.h>
#include <sys/sched.h>
#include <sys/vfs.h>
#include <pci.h>
#include <io.h>
#include <sys/apic.h>
#include <sys/percpu.h>
#include <sys/smp.h>
#ifdef CONFIG_KTEST
#include <sys/ktest.h>
#endif
#include <framebuffer.h>

struct boot_info bootinfo;

/* C++ global constructors */
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

static void do_global_ctors(void)
{
    for (size_t i = 0; &__init_array_start[i] < __init_array_end; i++) __init_array_start[i]();
}

/* Rust side hooks (rust/lib.rs) */
int kernel_spawn(const char *path);

bool kern_text_ptr(u64 p)
{
    extern char __kernel_start[];
    extern char __kernel_end[];
    return p >= (u64)__kernel_start && p < (u64)__kernel_end;
}

/* setup.asm stashes the raw int15-e820 table here */
#define E820_COUNT_ADDR 0x6FFCUL
#define E820_TABLE_ADDR 0x7000UL

/* setup.asm stores VBE framebuffer info here (phys) */
#define VBE_FB_INFO_ADDR 0x8C00UL

/* Collect the E820 memory map into bootinfo.
 * QEMU -kernel:  setup.asm writes E820 to fixed phys addresses (0x6FFC/0x7000)
 *                and stores 0 at 0x6F00 as the "not GRUB" sentinel.
 * GRUB  linux:   setup.asm is NOT executed.  _start32 stores GRUB's
 *                boot_params pointer at 0x6F00.
 *                We check 0x6F00 to pick the right source. */
static void boot_collect_e820(void)
{
    bootinfo.map_len = 0;
    /* Both writers store a DWORD (setup.asm writes 0, entry64.S moves esi),
     * so read a DWORD: the old 8-byte load picked up whatever BIOS/GRUB left
     * in 0x6F04..0x6F07 on real hardware and turned a good low physical
     * address into a wild pointer. */
    u32 grub_params_pa = *(volatile u32 *)0x6F00;

    if (grub_params_pa) {
        /* GRUB path: read E820 from boot_params */
        const struct boot_params *grub_bp = (const struct boot_params *)(u64)grub_params_pa;
        u32 n = grub_bp->e820_entries;
        /* cap to bootinfo.map[] capacity (64), not the source array */
        if (n > 64) n = 64;
        for (u32 i = 0; i < n; i++) {
            if (!grub_bp->e820_map[i].size) continue;
            bootinfo.map[bootinfo.map_len] = grub_bp->e820_map[i];
            bootinfo.map_len++;
        }
    } else {
        /* QEMU path: read E820 from fixed addresses set by setup.asm */
        u32 n = *(volatile u32 *)E820_COUNT_ADDR;
        if (n > 64) n = 64;
        const volatile struct e820_entry *src = (const volatile struct e820_entry *)E820_TABLE_ADDR;
        for (u32 i = 0; i < n; i++) {
            if (!src[i].size) continue;
            bootinfo.map[bootinfo.map_len].addr = src[i].addr;
            bootinfo.map[bootinfo.map_len].size = src[i].size;
            bootinfo.map[bootinfo.map_len].type = src[i].type;
            bootinfo.map_len++;
        }
    }
}

/* Read the LFB descriptor the boot loader left behind.  Two sources:
 *   setup.asm path (QEMU -kernel, and loaders that run the real-mode setup):
 *       arch/setup.asm programmed a VBE mode and described it at 0x8C00.
 *   GRUB `linux` path (the normal way to boot on real hardware): the setup
 *       code never runs, GRUB itself sets the mode requested by gfxpayload
 *       and describes it in boot_params.screen_info (struct screen_lfb).
 * On the GRUB path boot_params lives above low RAM, so the same physical
 * pointer boot_collect_e820() already validated is reused. */
static void boot_read_vbe(void)
{
    memset(&bootinfo.fb, 0, sizeof(bootinfo.fb));

    u32 grub_params_pa = *(volatile u32 *)0x6F00; /* DWORD, see boot_collect_e820() */
    if (grub_params_pa) {
        const struct screen_lfb *si =
            (const struct screen_lfb *)(u64)grub_params_pa;
        bootinfo.fb.phys_addr = (u64)si->lfb_base | ((u64)si->ext_lfb_base << 32);
        bootinfo.fb.width = si->lfb_width;
        bootinfo.fb.height = si->lfb_height;
        bootinfo.fb.pitch = si->lfb_linelength;
        bootinfo.fb.bpp = (u8)si->lfb_depth;
        bootinfo.fb.r_pos = si->red_pos;
        bootinfo.fb.r_size = si->red_size;
        bootinfo.fb.g_pos = si->green_pos;
        bootinfo.fb.g_size = si->green_size;
        bootinfo.fb.b_pos = si->blue_pos;
        bootinfo.fb.b_size = si->blue_size;
        bootinfo.fb.ok = (si->lfb_base && si->lfb_width && si->lfb_height && si->lfb_depth &&
                          si->lfb_linelength)
                             ? 1
                             : 0;
        if (!bootinfo.fb.ok)
            kprintf("[fb] no linear framebuffer (loader left text mode)\n");
    } else {
        const volatile struct vbe_lfb_info *v =
            (const volatile struct vbe_lfb_info *)VBE_FB_INFO_ADDR;
        if (v->ok) {
            bootinfo.fb.phys_addr = v->phys_addr;
            bootinfo.fb.pitch = v->pitch;
            bootinfo.fb.width = v->width;
            bootinfo.fb.height = v->height;
            bootinfo.fb.bpp = v->bpp;
            bootinfo.fb.memory_model = v->memory_model;
            bootinfo.fb.r_pos = v->r_pos;
            bootinfo.fb.r_size = v->r_size;
            bootinfo.fb.g_pos = v->g_pos;
            bootinfo.fb.g_size = v->g_size;
            bootinfo.fb.b_pos = v->b_pos;
            bootinfo.fb.b_size = v->b_size;
            bootinfo.fb.mode = v->mode;
            bootinfo.fb.ok = 1;
        }
    }

    if (!bootinfo.fb.ok) return;

    struct fb_info *f = &bootinfo.fb;
    u32 depth = f->bpp;
    u32 bytespp = (depth + 7) / 8;
    /* Reject anything a broken BIOS may have left behind: the values are
     * only trusted after fb_init() maps the LFB and starts drawing. */
    if (!f->phys_addr || !f->width || !f->height || !f->pitch || f->width > FB_MAX_DIM ||
        f->height > FB_MAX_DIM || f->pitch < f->width * bytespp || f->pitch > f->width * 8 ||
        (u64)f->pitch * f->height > FB_MAX_BYTES ||
        (depth != 8 && depth != 15 && depth != 16 && depth != 24 && depth != 32)) {
        kprintf("[fb] rejecting bogus LFB: %dx%d %u bpp pitch=%u phys=0x%lx\n", f->width,
                f->height, depth, f->pitch, f->phys_addr);
        memset(f, 0, sizeof(*f));
        return;
    }
    kprintf("[fb] lfb %dx%d %u bpp phys=0x%lx pitch=%u mode=0x%x\n", f->width, f->height, depth,
            f->phys_addr, f->pitch, f->mode);
}

/* PS/2 keyboard: controller init + IOAPIC IRQ1 (vector 33) routing. */
static void ps2_kbd_init(void)
{
    while (inb(0x64) & 1) inb(0x60);
    outb(0x64, 0x20);
    while (!(inb(0x64) & 1));
    u8 cmd = inb(0x60);
    cmd |= 0x01;
    outb(0x64, 0x60);
    while (inb(0x64) & 2);
    outb(0x60, cmd);

    ioapic_set_irq(1, 33, 0);
    ioapic_unmask_irq(1);
    irq_install(1, kbd_irq_handler);
    kprintf("[kbd] PS/2 ready, IRQ1 -> vector 33\n");
}

/* COM1 serial: IOAPIC IRQ4 (vector 36) routing + rx interrupt. */
static void serial_irq_init(void)
{
    ioapic_set_irq(4, 36, 0);
    ioapic_unmask_irq(4);
    irq_install(4, serial_rx_handler);
    extern int lnxrm_uart_irq_enable(void);
    lnxrm_uart_irq_enable();
    kprintf("[serial] COM1 ready, IRQ4 -> vector 36\n");
}

/* Fill bootinfo.cmdline from the boot loader.
 *   GRUB `linux`: boot_params.hdr.cmd_line_ptr (protocol offset 0x228), reached
 *                 through the 0x6F00 sentinel.
 *   QEMU -kernel: linuxboot leaves that field zero and puts the -append string
 *                 at physical 0x20000; without -append that area is still
 *                 zeroed RAM, so the bounded copy yields an empty string. */
#define CMDLINE_QEMU_PA 0x20000UL

static void boot_read_cmdline(void)
{
    bootinfo.cmdline[0] = 0;
    u32 params = *(volatile u32 *)0x6F00; /* DWORD, see boot_collect_e820() */
    u32 ptr = params ? *(volatile u32 *)(u64)(params + 0x228) : 0;
    if (!ptr)
        ptr = CMDLINE_QEMU_PA;
    const volatile char *s = (const volatile char *)(u64)ptr;
    u32 i;
    for (i = 0; i < sizeof(bootinfo.cmdline) - 1 && s[i]; i++)
        bootinfo.cmdline[i] = s[i];
    bootinfo.cmdline[i] = 0;
}

void start_kernel(void)
{
    u64 tsc_boot = rdtsc();

    /* Console + GDT + IDT before anything that can fault.  Until idt_init()
     * runs, any exception escalates to a triple fault and the machine resets
     * without printing a single line -- which is exactly what an early-boot
     * bug looks like from the outside ("some output, then a reboot").
     * gdt_init() comes first because both boot paths reach _start64 with
     * CS=0x18 while every IDT gate selects 0x08: a fault taken before the
     * kernel GDT is loaded would #GP on the gate itself and still reset. */
    console_init();
    gdt_init();
    cpu_set_gs_base((u64)&cpu_table[0]); /* get_current() must not walk GS base 0 */
    idt_init();

    boot_collect_e820();

    kprintf("\nlnxrm v0.08-TEST -- x86-64\n");
    kprintf("[boot] %d usable e820 entries\n", bootinfo.map_len);

    /* Physical memory map straight from e820: one line per region, then a
     * summary. Reserved regions at/above 4 GiB are address-space
     * placeholders (QEMU reserves 12 GiB there) and are not counted. */
    u64 e820_ram = 0, e820_rsv = 0;
    for (int i = 0; i < bootinfo.map_len; i++) {
        struct e820_entry *e = &bootinfo.map[i];
        const char *kind = "reserved";
        if (e->type == E820_RAM) kind = "usable";
        else if (e->type == 3) kind = "ACPI reclaim";
        else if (e->type == 4) kind = "ACPI NVS";
        else if (e->type == 5) kind = "bad";
        kprintf("[mem] e820 %d: 0x%lx..0x%lx %s\n", i, e->addr, e->addr + e->size - 1, kind);
        if (e->type == E820_RAM)
            e820_ram += e->size;
        else if (e->addr < 0x100000000UL)
            e820_rsv += e->size;
    }
    kprintf("[mem] e820 usable %lu.%lu MiB, reserved %lu.%lu MiB (below 4 GiB)\n",
            e820_ram >> 20, ((e820_ram & 0xFFFFF) * 10) >> 20, e820_rsv >> 20,
            ((e820_rsv & 0xFFFFF) * 10) >> 20);

    boot_read_vbe();
    boot_read_cmdline();
    if (bootinfo.cmdline[0])
        kprintf("[boot] cmdline='%s'\n", bootinfo.cmdline);

    /* Kernel footprint: .image + .bss (incl. boot stack), physical range. */
    extern char __kernel_start[], __bss_end[];
    u64 klo = VIRT_TO_PHYS((uptr)__kernel_start);
    u64 khi = ALIGN_UP(VIRT_TO_PHYS((uptr)__bss_end), PAGE_SIZE);
    kprintf("[boot] kernel image phys 0x%lx..0x%lx (%lu KiB)\n", klo, khi, (khi - klo) >> 10);

#ifdef CONFIG_KTEST
    /* Self-test stage 1 of 3: everything that needs nothing but the console
     * (C library, ANSI tokenizer, ABI/asm layout, the e820 map itself). */
    ktest_run(KTEST_EARLY);
#endif

    /* init per-cpu data for BSP (cpu 0) */
    cpu_init_percpu(0, 0);

    /* The LFB is MMIO, never RAM: keep the buddy allocator away from it. */
    if (bootinfo.fb.ok && bootinfo.fb.phys_addr) {
        u64 fb_bytes = (u64)bootinfo.fb.pitch * bootinfo.fb.height;
        pmm_reserve(bootinfo.fb.phys_addr, bootinfo.fb.phys_addr + fb_bytes);
        /* fb_init() maps the LFB into the high-half PD slots
         * [FB_PD_BASE, FB_PD_BASE + n).  Those PD entries are consumed by
         * the framebuffer, so the physical pages they would normally alias
         * become unreachable through PHYS_TO_VIRT() and must not be handed
         * out as frames (a 1920x1080x32 LFB needs four slots = 8 MiB). */
        u64 fb_slots = (fb_bytes + 0x1FFFFFUL) >> 21;
        if (fb_slots > FB_PD_SLOTS) fb_slots = FB_PD_SLOTS;
        if (fb_slots)
            pmm_reserve((u64)FB_PD_BASE << 21, ((u64)FB_PD_BASE + fb_slots) << 21);
    }

    /* Slot 96 is the VGA text window (entry64.S: PD_HI[96] -> PT 0x64000
     * -> phys 0xB8000), so physical 192..194 MiB can never get a
     * high-half alias.  Once RAM grows past 192 MiB (-m 256) those
     * frames fall inside the buddy range: PHYS_TO_VIRT() on them would
     * land on the VGA page and scribble it, so keep them out of the
     * buddy.  No effect on small-RAM boots (above top_addr). */
    pmm_reserve(96UL << 21, 97UL << 21);

    /* Two more ranges whose high-half alias slot is consumed by a fixed
     * window, so PHYS_TO_VIRT() on them would land on something else and
     * the frames must stay out of the buddy:
     *   slot 128    (256..258 MiB)  -> DEV_VMA: the LAPIC/IOAPIC pages
     *   slots 256..271 (512..544 MiB) -> KHEAP_VMA: filled with the heap
     * vmm_init() skips slot 128 outright and overwrites 256..271 after the
     * alias loop, but nothing reserved the frames behind them.  Harmless at
     * -m 256 (the window ends below 256 MiB, so they were never handed
     * out); live as soon as the buddy manages more, i.e. always on real
     * hardware. */
    pmm_reserve(128UL << 21, 129UL << 21);
    pmm_reserve(256UL << 21, 272UL << 21);

    pmm_init();
    vmm_init();
    kheap_init();
    cpu_init();
    pit_init(HZ);

#ifdef CONFIG_KTEST
    /* Self-test stage 2 of 3: the allocators and the page tables, each
     * expected to give back exactly what it took. */
    ktest_run(KTEST_MM);
#endif

    /* Map framebuffer into kernel VA (needs pmm + vmm ready). */
    if (bootinfo.fb.ok && bootinfo.fb.phys_addr) fb_init();

    ps2_kbd_init();
    serial_irq_init();

    vfs_init();

    /* try IDE first (QEMU built-in), then AHCI */
    extern void ide_init(void);
    ide_init();
    ahci_init();
    if (vfs_try_mount_disk() == 0)
        kprintf("[vfs] disk mounted at /\n");
    else
        kprintf("[vfs] no disk found\n");

    do_global_ctors();

    sched_init();

#ifdef CONFIG_KTEST
    /* Self-test stage 3 of 3: the scheduler's own task table and the root
     * filesystem, while nothing else has run yet.  Then the verdict. */
    ktest_run(KTEST_LATE);
    ktest_summary();
#endif

    if (kernel_spawn("/bin/init") < 0) {
        kprintf("\033[34mStop!\033[0m\n");
        for (;;) __asm__ volatile("hlt");
    }

    /* ---- start SMP: launch Application Processors ---- */
    /* Measure the LAPIC period + TSC rate ONCE on the BSP first: APs
     * arm their timers from the cached period (calibrating per-AP would
     * race this CPU for the PIT latch port). */
    lapic_timer_calibrate();
    smp_init();

    /* Periodic tick: BSP LAPIC timer -> vector 32 -> pit_handler.
     * (PIT/IOAPIC IRQ0 does not set LAPIC IRR on this platform.) */
    lapic_timer_start(32, HZ);

    /* Boot summary: what is still free after every init allocation ran,
     * and how long the C entry -> SMP bring-up path took. */
    u64 avail = pmm_free_bytes();
    kprintf("[mem] available memory %lu.%lu MiB\n", avail >> 20,
            ((avail & 0xFFFFF) * 10) >> 20);
    u64 tpm = tsc_per_ms();
    kprintf("[boot] ready in %lu ms\n", tpm ? (rdtsc() - tsc_boot) / tpm : 0);

    __asm__ volatile("sti");
    idle_loop();
}
