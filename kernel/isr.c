/* Interrupt dispatch: exception reporting, IRQ routing, IPI dispatch. */
#include <sys/cpu.h>
#include <sys/apic.h>
#include <io.h>
#include <console.h>
#include <sys/sched.h>
#include <sys/percpu.h>
#include <mm/pagefault.h>

/* ---- keyboard: scancode set 1 -> ASCII ---- */
/* clang-format off */   /* raw lookup table */
static const char kmap[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0, '*', 0, ' ', 0,
};

static const char kmap_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0, '*', 0, ' ', 0,
};
/* clang-format on */

#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_CAPSLOCK 0x3A

static bool shift_pressed = false;
static bool capslock_on = false;

#define KBD_DATA 0x60

static bool kbd_ext; /* file-scope: the E0 prefix belongs to the next code */

static void kbd_process_sc(u8 sc)
{
    if (sc == 0xE0) {
        kbd_ext = true;
        return;
    }
    bool release = !!(sc & 0x80);
    bool ext = kbd_ext;
    u8 code = sc & 0x7f;
    kbd_ext = false; /* the E0 prefix belongs to this code only */

    if (code == SC_LSHIFT || code == SC_RSHIFT) {
        shift_pressed = !release;
        return;
    }
    if (code == SC_CAPSLOCK) {
        if (!release) capslock_on = !capslock_on;
        return;
    }
    if (release) return;

    char c = 0;
    if (!ext && code < 128 && kmap[code]) {
        if (shift_pressed)
            c = kmap_shift[code];
        else
            c = kmap[code];
        if (capslock_on && c >= 'a' && c <= 'z')
            c = c - 'a' + 'A';
        else if (capslock_on && c >= 'A' && c <= 'Z')
            c = c - 'A' + 'a';
        input_push(c);
    }
}

/* kbd_info bookkeeping: keys the ring accepted vs. lost to a full ring,
 * and how many scancodes IRQ1 ever carried (the keyboard detector).
 * All monotonic, atomic, never reset. */
static volatile u32 in_pushed, in_dropped, kbd_scans;

void kbd_irq_handler(struct intr_frame *f)
{
    u8 sc = inb(KBD_DATA);
    __atomic_add_fetch(&kbd_scans, 1u, __ATOMIC_RELAXED);
    kbd_process_sc(sc);
}

/* ---- serial COM1 receive ---- */
extern int lnxrm_uart_trygetc(void);
void serial_rx_handler(struct intr_frame *f)
{
    int c;
    while ((c = lnxrm_uart_trygetc()) >= 0) input_push((char)c);
}

/* ---- ring buffer for console input ---- */
#define INBUF_SZ 256
static volatile char inbuf[INBUF_SZ];
static volatile u32 in_r, in_w;

/* T-054: every task blocked in console_read(), linked through each task's
 * con_next.  A single slot could only ever hold one reader -- a second arm()
 * threw the first waiter away, so it fell back on its 1-jiffy timeout instead
 * of waking on the key, and two looping readers kept overwriting each other.
 * con_lock guards the list: arm()/disarm() run in process context on any CPU
 * while input_push() runs in the UART ISR (hence irqsave -- see spinlock.h).
 * The lock, not a `volatile` head, is what orders access (the old code had
 * no lock and relied on `volatile` plus a 1-jiffy timeout to stay honest). */
static spinlock_t con_lock = SPINLOCK_INIT;
static struct task *con_waiters;

void console_waiter_arm(struct task *t)
{
    u64 flags;
    spin_lock_irqsave(&con_lock, &flags);
    t->con_next = con_waiters;
    con_waiters = t;
    spin_unlock_irqrestore(&con_lock, flags);
}

void console_waiter_disarm(struct task *t)
{
    u64 flags;
    spin_lock_irqsave(&con_lock, &flags);
    struct task **pp = &con_waiters;
    while (*pp) {
        if (*pp == t) {
            *pp = t->con_next;
            t->con_next = NULL;
            break;
        }
        pp = &(*pp)->con_next;
    }
    spin_unlock_irqrestore(&con_lock, flags);
}

void input_push(char c)
{
    if ((in_w + 1) % INBUF_SZ == in_r) {
        __atomic_add_fetch(&in_dropped, 1u, __ATOMIC_RELAXED);
        return;
    }
    inbuf[in_w] = c;
    __atomic_store_n(&in_w, (in_w + 1) % INBUF_SZ, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&in_pushed, 1u, __ATOMIC_RELAXED);

    /* T-054: wake *every* reader, not whoever held the single slot.  Detach
     * the whole chain under the lock, then wake outside it so con_lock is
     * never held across runqueue_add().  The readers race for the byte; a
     * loser finds the queue empty, arms again and sleeps -- which is exactly
     * the wait the old code could only satisfy by timeout. */
    u64 flags;
    spin_lock_irqsave(&con_lock, &flags);
    struct task *w = con_waiters;
    con_waiters = NULL;
    spin_unlock_irqrestore(&con_lock, flags);

    while (w) {
        struct task *next = w->con_next;
        w->con_next = NULL;
        if (w->state == T_SLEEPING) {
            w->state = T_RUNNABLE;
            runqueue_add(w);
        }
        w = next;
    }
}

int input_pop(void)
{
    u32 r = __atomic_load_n(&in_r, __ATOMIC_SEQ_CST);
    if (r == in_w) return LNXRM_EFAIL;
    int c = (u8)inbuf[r]; /* 0..255: a 0xff key must never read as "empty" */
    __atomic_store_n(&in_r, (r + 1) % INBUF_SZ, __ATOMIC_SEQ_CST);
    return c;
}

/* kbd_get 34: one pending key, non-blocking. */
long kbd_get_byte(void)
{
    long c = input_pop();
    return c == LNXRM_EFAIL ? LNXRM_EAGAIN : c;
}

/* kbd_inject 35: run bytes through the same input_push() a keystroke
 * would -- waiters wake, the ring is the only consumer-visible effect --
 * and answer how many were accepted (ring-full drops are honest). */
long kbd_inject_bytes(const char *kbuf, u32 len)
{
    u32 d0 = __atomic_load_n(&in_dropped, __ATOMIC_RELAXED);
    for (u32 i = 0; i < len; i++) input_push(kbuf[i]);
    u32 d1 = __atomic_load_n(&in_dropped, __ATOMIC_RELAXED);
    return (long)len - (long)(d1 - d0);
}

/* kbd_info 36: counters snapshot.  in_r/in_w are seq_cst singles like the
 * ISR writers; a concurrent SMP inject may skew this by one key, which a
 * stats read is allowed to be. */
void kbd_stats(struct lnxrm_kbd_info *k)
{
    u32 r = __atomic_load_n(&in_r, __ATOMIC_SEQ_CST);
    u32 w = __atomic_load_n(&in_w, __ATOMIC_SEQ_CST);
    if (!k) return;
    k->capacity = INBUF_SZ - 1;
    k->pending = (w - r) % INBUF_SZ;
    k->pushed_total = __atomic_load_n(&in_pushed, __ATOMIC_RELAXED);
    k->dropped = __atomic_load_n(&in_dropped, __ATOMIC_RELAXED);
    k->ps2_scans = __atomic_load_n(&kbd_scans, __ATOMIC_RELAXED);
    k->reserved[0] = k->reserved[1] = k->reserved[2] = 0;
}

u32 kbd_pending(void)
{
    return (u32)((__atomic_load_n(&in_w, __ATOMIC_SEQ_CST) -
                  __atomic_load_n(&in_r, __ATOMIC_SEQ_CST)) % INBUF_SZ);
}

/* ---- central dispatch ---- */
static const char *exc_names[32] = {
    "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM", "#DF", "?9",  "#TS",
    "#NP", "#SS", "#GP", "#PF", "?15", "#MF", "#AC", "#MC", "#XM", "#VE", "?21",
    "?22", "?23", "?24", "?25", "?26", "?27", "?28", "?29", "?30", "?31",
};

extern void ipi_dispatch(u32 vector);

void isr_common(struct intr_frame *f)
{
    /* Ring-0 entry: drop EFLAGS.AC unconditionally (when SMAP exists).
     * Userspace owns AC and may have set it with POPF; entering the kernel
     * with AC=1 would let the very next user-page access bypass SMAP.
     * Hardware preserves AC across the iretq back (the frame carries the
     * user's original flags), so userspace loses nothing.  CLAC #UDs while
     * CR4.SMAP=0, hence the gate. */
    if (g_smap) __asm__ volatile("clac" ::: "memory");

    /* ---- IPI vectors (0xF0..0xF2) ---- */
    if (f->intno >= 0xF0 && f->intno <= 0xF2) {
        ipi_dispatch((u32)f->intno);
        apic_eoi();
        /* Reschedule IPI (runqueue_add on this CPU) must preempt a
         * returning-to-user task; without this, APs have no PIT and
         * would only schedule when the current task voluntarily blocks. */
        if (current && (f->cs & 3) == 3) sched_maybe_preempt(f);
        return;
    }

    if (f->intno < 32) {
        u64 cr2 = 0;
        if (f->intno == 14) {
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            /* T-090: try to RESOLVE the fault (lazy stack growth) before
             * reporting or killing.  Only a user-mode fault is a candidate;
             * a kernel #PF (a bug) or an unfixable fault falls through to
             * the safe report+kill below.  A fixed fault is silent -- iretq
             * simply retries the instruction, now that the page exists. */
            if ((f->cs & 3) == 3 && page_fault_try_fix(f, cr2)) return;
        }
        kprintf("\n[exception] %s (%d) err=%#lx rip=%#lx rsp=%#lx cr2=%#lx\n",
                exc_names[f->intno & 31], f->intno, f->err, f->rip, f->ussp, cr2);
        if (f->intno == 14) {
            /* Decode the #PF error code so a WP/SMAP regression is readable
             * straight from the log: P=1+W=1+U=0 is a ring-0 store to a
             * read-only page OR a SMAP violation -- identical error codes.
             * Tell them apart: f->rflags is the hardware-pushed pre-clac
             * value, so AC=1 means a STAC window was open (SMAP disarmed ->
             * must be WP); AC=0 plus a user-space cr2 means the store/load
             * had no window at all -> SMAP (or WP against a RO user page). */
            kprintf("[exception] #PF %s%s%s%s%s\n",
                    (f->err & 1) ? "protection" : "not-present",
                    (f->err & 2) ? ", write" : ", read",
                    (f->err & 4) ? ", user" : ", supervisor",
                    (f->err & 8) ? ", reserved-bit" : "",
                    (f->err & 16) ? ", instr-fetch" : "");
            if ((f->err & 7) == 3) {
                bool ac_at_entry = (f->rflags & (1UL << 18)) != 0;
                bool user_va = cr2 >= USER_BASE && cr2 <= USER_MAX_VMA;
                if (ac_at_entry)
                    kprintf("[exception] ring-0 store to a read-only page (CR0.WP, "
                            "AC=1 so SMAP was open)\n");
                else if (user_va)
                    kprintf("[exception] supervisor %s to a user page with AC=0: "
                            "missing STAC window (SMAP) or store to a RO user page "
                            "(CR0.WP)\n",
                            (f->err & 2) ? "store" : "load");
                else
                    kprintf("[exception] ring-0 store to a read-only page "
                            "(CR0.WP)\n");
            }
        }
        if (f->intno == 14 && current) {
            /* Capture the table state FIRST: the prints below cost serial
             * milliseconds, and any CPU can write memory in that window.
             * The snapshot is the closest thing to the fault moment. */
            u64 cr3 = 0, snap_e4 = 0, snap_e3 = 0, snap_e2 = 0, snap_e1 = 0;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            u64 sva = cr2;
            u64 si4 = (sva >> 39) & 511, si3 = (sva >> 30) & 511;
            u64 si2 = (sva >> 21) & 511, si1 = (sva >> 12) & 511;
            u64 *sr4 = (u64 *)PHYS_TO_VIRT(current->pml4);
            snap_e4 = sr4[si4];
            if (snap_e4 & 1) {
                u64 *sr3 = (u64 *)PHYS_TO_VIRT(snap_e4 & ~0xfffUL);
                snap_e3 = sr3[si3];
                if (snap_e3 & 1) {
                    u64 *sr2 = (u64 *)PHYS_TO_VIRT(snap_e3 & ~0xfffUL);
                    snap_e2 = sr2[si2];
                    if (snap_e2 & 1) {
                        u64 *sr1 = (u64 *)PHYS_TO_VIRT(snap_e2 & ~0xfffUL);
                        snap_e1 = sr1[si1];
                    }
                }
            }
            kprintf("[exception] snapshot cr3=%p match=%d pml4[e]: %p %p %p %p\n",
                    (void *)cr3, (int)(cr3 == current->pml4),
                    (void *)snap_e4, (void *)snap_e3, (void *)snap_e2,
                    (void *)snap_e1);
            __asm__ volatile("mfence" ::: "memory");
            kprintf("[exception] live-now gfns: %p %p %p %p\n",
                    (void *)sr4[si4],
                    (void *)(snap_e3 ? ((u64 *)PHYS_TO_VIRT(snap_e4 & ~0xfffUL))[si3] : 0),
                    (void *)(snap_e2 ? ((u64 *)PHYS_TO_VIRT(snap_e3 & ~0xfffUL))[si2] : 0),
                    (void *)(snap_e1 ? ((u64 *)PHYS_TO_VIRT(snap_e2 & ~0xfffUL))[si1] : 0));
            kprintf("[exception] cr3=%p current->pml4=%p%s\n", (void *)cr3,
                    (void *)current->pml4, cr3 == current->pml4 ? " (match)" : " (*MISMATCH*)");
            /* page-table walk of cr2 under current->pml4, through the same
             * high-half alias the C walkers use (a raw low-half read is the
             * identity alias, which is not a guarantee of what the C walk
             * actually reads). */
            u64 idx4 = si4, idx3 = si3, idx2 = si2, idx1 = si1;
            u64 *r4 = (u64 *)PHYS_TO_VIRT(current->pml4);
            u64 e4 = r4[idx4];
            u64 e2 = 0; /* L2 slot, kept out here for the leaf-counter */
            kprintf("[exception] walk pml4[%u]=%p\n", (unsigned)idx4, (void *)e4);
            if (e4 & 1) {
                u64 *r3 = (u64 *)PHYS_TO_VIRT(e4 & ~0xfffUL);
                u64 e3 = r3[idx3];
                kprintf("[exception] walk pdp[%u]=%p\n", (unsigned)idx3, (void *)e3);
                if (e3 & 1) {
                    u64 *r2 = (u64 *)PHYS_TO_VIRT(e3 & ~0xfffUL);
                    e2 = r2[idx2];
                    kprintf("[exception] walk pd[%u]=%p\n", (unsigned)idx2, (void *)e2);
                    if (e2 & 1) {
                        u64 *r1 = (u64 *)PHYS_TO_VIRT(e2 & ~0xfffUL);
                        kprintf("[exception] walk pt[%u]=%p\n", (unsigned)idx1,
                                (void *)r1[idx1]);
                        /* how much of this L1 page is alive: a wholesale
                         * zeroing means the frame was repurposed (double
                         * alloc / memset), a lone zero entry means a
                         * precise unmap hit it */
                        int alive = 0;
                        for (int i = 0; i < 512; i++)
                            alive += !!(r1[i] & 1);
                        kprintf("[exception] L1@%p alive=%d/512\n",
                                (void *)PHYS_TO_VIRT(e2 & ~0xfffUL), alive);
                    }
                }
            }
            extern u64 vmm_translate_in(u64 root, u64 va);
            kprintf("[exception] vmm_translate_in=%p\n",
                    (void *)vmm_translate_in(current->pml4, cr2));
            /* hardware state around the walk: PCID leftovers, cache mode */
            u64 cr0 = 0, cr4 = 0;
            __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
            __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
            kprintf("[exception] cr3.low=%#lx cr0=%p (CD=%d NW=%d) cr4=%p "
                    "(PCIDE=%d SMAP=%d PSE=%d PAE=%d)\n",
                    (unsigned long)(cr3 & 0xfff), (void *)cr0,
                    (int)(!!(cr0 & (1UL << 30))), (int)(!!(cr0 & (1UL << 29))),
                    (void *)cr4, (int)(!!(cr4 & (1UL << 17))),
                    (int)(!!(cr4 & (1UL << 21))), (int)(!!(cr4 & (1UL << 4))),
                    (int)(!!(cr4 & (1UL << 5))));
            if (e2 & 1) {
                /* the same L1 read through BOTH mappings of the frame: WB
                 * alias vs the boot identity map.  A disagreement means the
                 * CPU is serving one of them a stale cache line.  Then a
                 * WBINVD and one re-read says which side is the truth. */
                u64 pa1 = e2 & ~0xfffUL;
                volatile u64 *via_alias =
                    (volatile u64 *)PHYS_TO_VIRT(pa1 + idx1 * 8);
                volatile u64 *via_low = (volatile u64 *)(pa1 + idx1 * 8);
                u64 a1 = *via_alias, l1 = *via_low;
                __asm__ volatile("wbinvd" ::: "memory");
                u64 a2 = *via_alias;
                kprintf("[exception] leaf pa=%p pt[%u]: alias=%p low=%p; "
                        "after wbinvd alias=%p\n",
                        (void *)pa1, (unsigned)idx1, (void *)a1, (void *)l1,
                        (void *)a2);
            }
        }
        if (current)
            kprintf("[exception] task pid=%u name=%s cs=%#lx ss=%#lx\n", current->pid,
                    current->name, f->cs, f->usss);

        /* A fault from user mode must kill the offending process, not
         * panic the whole kernel (this is what SIGSEGV/SIGILL/... are for). */
        if (current && (f->cs & 3) == 3) {
            int sig = SIGSEGV;
            if (f->intno == 0 || f->intno == 16)
                sig = SIGFPE;
            else if (f->intno == 6)
                sig = SIGILL;
            else if (f->intno == 7 || f->intno == 17)
                sig = SIGBUS;
            kprintf("[exception] user fault -> sig %d pid=%u\n", sig, current->pid);

            /* A handler installed with sigaction() must win over the
             * default disposition, otherwise SIGSEGV/SIGILL/SIGFPE are
             * uncatchable and sigaction() is a lie.  Redirect to the
             * handler the same way any other deliverable signal does.
             * sig_in_handler keeps a fault inside the handler from
             * recursing forever: the second one kills the process. */
            void *h = current->sig_handlers[sig];
            if (h != SIG_DFL && h != SIG_IGN && !current->sig_in_handler &&
                !(current->sig_blocked & (1ULL << sig))) {
                /* sigreturn puts rip back on the faulting instruction. If
                 * it faults again at the very same RIP the handler did not
                 * fix the problem, so kill instead of looping forever. */
                if (current->sig_fault_rip == f->rip) sys_exit(-sig);
                current->sig_fault_rip = f->rip;
                current->tf = f;
                send_signal(current, sig);
                do_signal_check(current); /* redirects f->rip/ussp */
                return;                   /* -> POPALL; iretq into handler */
            }
            sys_exit(-sig);
            /* not reached */
        }

        {
            u64 gs_rd = rdmsr(0xC0000101);
            struct cpu_info *ci = this_cpu_data();
            kprintf("[exception] GS_MSR=%p this_cpu_data=%p\n", gs_rd, (void *)ci);
        }
        {
            u64 *sp = (u64 *)f;
            kprintf("[stack]");
            for (int q = 22; q < 44; q++) kprintf(" %lx", sp[q]);
            kprintf("\n");
        }
        panic("unrecoverable exception");
    }

    int irq = f->intno - 32;
    if (current && current->pid != 0) current->tf = f;

    if (irq >= 0 && irq < 16) {
        /* spurious IRQ7/15 check (still valid with IOAPIC) */
        if (irq == 7 || irq == 15) {
            if (f->intno == 0xFF) return;
        }
        /* EOI BEFORE the handler: handlers such as the PIT tick may call
         * schedule() and switch away; a delayed EOI would leave the LAPIC
         * ISR bit set and stall this vector until the interrupted task
         * resumes (losing ticks / hanging idle's hlt). */
        apic_eoi();
        irq_handler_t h = irq_get_handler(irq);
        if (h) h(f);
    } else {
        kprintf("stray vector %d\n", f->intno);
    }

    /* deliver pending signals on every return path to user mode */
    if (current && (f->cs & 3) == 3) do_signal_check(current);
}
