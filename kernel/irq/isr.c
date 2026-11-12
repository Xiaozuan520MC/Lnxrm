/* Interrupt dispatch: exception reporting, IRQ routing, IPI dispatch. */
#include <sys/cpu.h>
#include <sys/input.h>
#include <sys/apic.h>
#include <io.h>
#include <console.h>
#include <sys/sched.h>
#include <sys/percpu.h>
#include <sys/spinlock.h>
#include <mm/pagefault.h>
#include <mm/mm.h>

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
        /* 39: the unified queue gets the same decoded character plus the
         * recipe (which modifiers were engaged) -- the character is
         * already transformed, the flags say *why*.  Break codes left at
         * the `release` return above, and extended (E0) keys keep console
         * behaviour only: their codes are not ASCII a terminal renders. */
        input_push_key((int)c,
                       (u32)(shift_pressed ? INPUT_MOD_SHIFT : 0) |
                           (u32)(capslock_on ? INPUT_MOD_CAPS : 0));
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

/* ktest plumbing: one scancode byte down the exact decode path an IRQ1
 * byte takes (the state machine included: shift, capslock, the E0
 * prefix, break codes).  Deliberately *not* a bump of kbd_scans -- that
 * counter means "bytes the hardware carried" (the headless detector), and
 * injection is not hardware, exactly as kbd_inject 35 leaves it alone. */
void kbd_inject_scancode(u8 sc)
{
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

/* ---- IPI vectors (0xF0..0xF2) ---- */
static bool handle_ipi(struct intr_frame *f)
{
    if (f->intno < 0xF0 || f->intno > 0xF2) return false;
    ipi_dispatch((u32)f->intno);
    apic_eoi();
    /* Reschedule IPI (runqueue_add on this CPU) must preempt a
     * returning-to-user task; without this, APs have no PIT and would only
     * schedule when the current task voluntarily blocks. */
    if (current && (f->cs & 3) == 3) sched_maybe_preempt(f);
    return true;
}

static u64 cr2_read(void)
{
    u64 v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

/* ---- #PF page-table diagnostics ----
 * The report wants the same four-slot walk twice: once BEFORE anything is
 * printed (kprintf costs serial milliseconds and any CPU can write memory
 * in that window, so the first pass is the closest thing to the fault
 * moment) and once after, so the two lines can be compared.  Both passes
 * keep the table pointer of every level, not just the entry, so "read it
 * again" reads through the snapshot's own pointers rather than through
 * whatever the root points at now. */
struct pf_walk {
    u64 idx[4];  /* slot at pml4 / pdp / pd / pt */
    u64 e[4];    /* entry read there -- 0 when the level above was absent */
    u64 *tbl[4]; /* table read there -- tbl[0] is the pml4 itself */
};

static struct pf_walk pf_walk(u64 root, u64 va)
{
    struct pf_walk w = {0};
    w.tbl[0] = (u64 *)PHYS_TO_VIRT(root);
    w.idx[0] = (va >> 39) & 511;
    w.idx[1] = (va >> 30) & 511;
    w.idx[2] = (va >> 21) & 511;
    w.idx[3] = (va >> 12) & 511;
    w.e[0] = w.tbl[0][w.idx[0]];
    for (int lvl = 1; lvl < 4; lvl++) {
        if (!(w.e[lvl - 1] & 1)) break; /* absent: this slot stays 0 */
        w.tbl[lvl] = (u64 *)PHYS_TO_VIRT(w.e[lvl - 1] & ~0xfffUL);
        w.e[lvl] = w.tbl[lvl][w.idx[lvl]];
    }
    return w;
}

/* One slot re-read after the snapshot line, through the table the
 * snapshot captured.  Level 0 is read unconditionally -- it is the pml4
 * itself, which has no level above it to gate on. */
static u64 pf_slot_again(const struct pf_walk *w, int lvl)
{
    if (lvl == 0) return w->tbl[0][w->idx[0]];
    return w->e[lvl] ? w->tbl[lvl][w->idx[lvl]] : 0;
}

/* How much of a leaf page is alive: a wholesale zeroing means the frame
 * was repurposed (double alloc / memset), a lone zero entry means a
 * precise unmap hit it. */
static int pf_leaf_alive(const u64 *l1)
{
    int alive = 0;
    for (int i = 0; i < 512; i++) alive += !!(l1[i] & 1);
    return alive;
}

/* `pfx` labels whose root this walk came from: "" for current->pml4 (the
 * shape the log has always had) and "cr3 " for the table the CPU is
 * actually paging with, printed alongside it on a desync. */
static void pf_walk_print(const struct pf_walk *w, const char *pfx)
{
    static const char *lvl_name[4] = {"pml4", "pdp", "pd", "pt"};
    for (int lvl = 0; lvl < 4; lvl++) {
        if (lvl && !(w->e[lvl - 1] & 1)) break; /* never reached: stop */
        kprintf("[exception] walk %s%s[%u]=%p\n", pfx, lvl_name[lvl],
                (unsigned)w->idx[lvl], (void *)w->e[lvl]);
        if (lvl == 3) /* leaf page: how much of it survived */
            kprintf("[exception] %sL1@%p alive=%d/512\n", pfx,
                    (void *)PHYS_TO_VIRT(w->e[2] & ~0xfffUL),
                    pf_leaf_alive(w->tbl[3]));
    }
}

/* Decode the #PF error code so a WP/SMAP regression is readable straight
 * from the log: P=1+W=1+U=0 is a ring-0 store to a read-only page OR a
 * SMAP violation -- identical error codes.  Tell them apart: f->rflags is
 * the hardware-pushed pre-clac value, so AC=1 means a STAC window was
 * open (SMAP disarmed -> must be WP); AC=0 plus a user-space cr2 means
 * the store/load had no window at all -> SMAP (or WP against a RO page). */
static void pf_error_print(struct intr_frame *f, u64 cr2)
{
    kprintf("[exception] #PF %s%s%s%s%s\n",
            (f->err & 1) ? "protection" : "not-present",
            (f->err & 2) ? ", write" : ", read",
            (f->err & 4) ? ", user" : ", supervisor",
            (f->err & 8) ? ", reserved-bit" : "",
            (f->err & 16) ? ", instr-fetch" : "");
    if ((f->err & 7) != 3) return;

    if (f->rflags & (1UL << 18))
        kprintf("[exception] ring-0 store to a read-only page (CR0.WP, "
                "AC=1 so SMAP was open)\n");
    else if (cr2 >= USER_BASE && cr2 <= USER_MAX_VMA)
        kprintf("[exception] supervisor %s to a user page with AC=0: "
                "missing STAC window (SMAP) or store to a RO user page "
                "(CR0.WP)\n",
                (f->err & 2) ? "store" : "load");
    else
        kprintf("[exception] ring-0 store to a read-only page "
                "(CR0.WP)\n");
}

/* The table state itself, read twice with the serial traffic in between
 * so a printout can be compared line by line, plus the hardware state
 * around the walk (PCID leftovers, cache mode).  Diagnostic only: nothing
 * here decides what the fault does. */
static void pf_tables_dump(u64 cr2)
{
    if (!current) return;

    u64 cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    /* BOTH roots, walked at the same instant: the task's own table and the
     * one the CPU is really paging with.  The report used to walk only
     * current->pml4, so on a CR3 desync one log printed "present" (from
     * this walk) and "not-present" (from the hardware) for the same
     * address -- the walk that mattered was never shown. */
    struct pf_walk snap = pf_walk(current->pml4, cr2);
    struct pf_walk csnap = pf_walk(cr3, cr2);
    kprintf("[exception] snapshot cr3=%p match=%d pml4[e]: %p %p %p %p\n",
            (void *)cr3, (int)(cr3 == current->pml4), (void *)snap.e[0],
            (void *)snap.e[1], (void *)snap.e[2], (void *)snap.e[3]);

    __asm__ volatile("mfence" ::: "memory");
    kprintf("[exception] live-now gfns: %p %p %p %p\n",
            (void *)pf_slot_again(&snap, 0), (void *)pf_slot_again(&snap, 1),
            (void *)pf_slot_again(&snap, 2), (void *)pf_slot_again(&snap, 3));
    kprintf("[exception] cr3=%p current->pml4=%p%s\n", (void *)cr3,
            (void *)current->pml4,
            cr3 == current->pml4 ? " (match)" : " (*MISMATCH*)");

    /* The walk again, live, through the same high-half alias the C
     * walkers use (a raw low-half read is the identity alias, which is not
     * a guarantee of what the C walk actually reads). */
    struct pf_walk now = pf_walk(current->pml4, cr2);
    pf_walk_print(&now, "");
    kprintf("[exception] vmm_translate_in=%p\n",
            (void *)vmm_translate_in(current->pml4, cr2));

    if (cr3 != current->pml4) {
        /* The table the hardware actually used, plus who owns it.  A named
         * owner = paging with another task's live tables; NONE = no live
         * task claims that root, i.e. it was destroyed (execve frees the
         * old pml4) and the frame has been recycled -- the CPU is walking
         * garbage and every answer below this line describes the wrong
         * table. */
        pf_walk_print(&csnap, "cr3 ");
        kprintf("[exception] vmm_translate_in(cr3)=%p\n",
                (void *)vmm_translate_in(cr3, cr2));
        struct task *owner = task_find_by_pml4(cr3);
        if (owner)
            kprintf("[exception] cr3 owner: pid=%u name=%s state=%d%s\n",
                    owner->pid, owner->name, (int)owner->state,
                    owner == current ? " (=current)" : "");
        else
            kprintf("[exception] cr3 owner: NONE -- root %p belongs to no "
                    "live task (destroyed / recycled)\n", (void *)cr3);
    }

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

    if (!(now.e[2] & 1)) return;
    /* The same L1 read through BOTH mappings of the frame: WB alias vs
     * the boot identity map.  A disagreement means the CPU is serving one
     * of them a stale cache line.  Then a WBINVD and one re-read says
     * which side is the truth. */
    u64 pa1 = now.e[2] & ~0xfffUL;
    volatile u64 *via_alias = (volatile u64 *)PHYS_TO_VIRT(pa1 + now.idx[3] * 8);
    volatile u64 *via_low = (volatile u64 *)(pa1 + now.idx[3] * 8);
    u64 a1 = *via_alias, l1 = *via_low;
    __asm__ volatile("wbinvd" ::: "memory");
    u64 a2 = *via_alias;
    kprintf("[exception] leaf pa=%p pt[%u]: alias=%p low=%p; "
            "after wbinvd alias=%p\n",
            (void *)pa1, (unsigned)now.idx[3], (void *)a1, (void *)l1,
            (void *)a2);
}

/* Everything an exception prints, in the order the log has always had it:
 * header, #PF decode, #PF tables, the task, and -- for a #UD -- the bytes
 * the CPU actually decoded from.  The last one is what tells "the text is
 * bad" from "control was transferred there (bad frame, wild jump)" in a
 * single line. */
static void exception_report(struct intr_frame *f, u64 cr2)
{
    kprintf("\n[exception] %s (%d) err=%#lx rip=%#lx rsp=%#lx cr2=%#lx\n",
            exc_names[f->intno & 31], (int)f->intno, f->err, f->rip, f->ussp,
            cr2);
    if (f->intno == 14) {
        pf_error_print(f, cr2);
        pf_tables_dump(cr2);
    }
    if (current)
        kprintf("[exception] task pid=%u name=%s cs=%#lx ss=%#lx\n",
                current->pid, current->name, f->cs, f->usss);

    if (f->intno == 6) {
        unsigned char b[8];
        if (copy_from_user(b, (const void *)f->rip, sizeof b) != 0) return;
        kprintf("[exception] text@rip:");
        for (unsigned i = 0; i < sizeof b; i++) kprintf(" %02x", b[i]);
        kprintf("\n");
    }
}

/* A fault from user mode must kill the offending process, not panic the
 * whole kernel (this is what SIGSEGV/SIGILL/... are for).  Returns true
 * when the frame was redirected into a handler -- the caller iretq's back
 * to user mode and runs it there; false means "not a user-mode fault",
 * after which the only thing left to do is panic.  sys_exit() is noreturn,
 * so every path that kills falls out of the bottom of this function. */
static bool fault_to_signal(struct intr_frame *f)
{
    if (!current || (f->cs & 3) != 3) return false;

    int sig = SIGSEGV;
    if (f->intno == 0 || f->intno == 16)
        sig = SIGFPE;
    else if (f->intno == 6)
        sig = SIGILL;
    else if (f->intno == 7 || f->intno == 17)
        sig = SIGBUS;
    kprintf("[exception] user fault -> sig %d pid=%u\n", sig, current->pid);

    /* A handler installed with sigaction() must win over the default
     * disposition, otherwise SIGSEGV/SIGILL/SIGFPE are uncatchable and
     * sigaction() is a lie.  Redirect to the handler the same way any
     * other deliverable signal does.  sig_in_handler keeps a fault inside
     * the handler from recursing forever: the second one kills it. */
    void *h = current->sig_handlers[sig];
    bool catchable = h != SIG_DFL && h != SIG_IGN && !current->sig_in_handler &&
                     !(current->sig_blocked & (1ULL << sig));
    /* sigreturn puts rip back on the faulting instruction.  If it faults
     * again at the very same RIP the handler did not fix the problem, so
     * kill instead of looping forever. */
    if (!catchable || current->sig_fault_rip == f->rip) sys_exit(-sig);

    current->sig_fault_rip = f->rip;
    current->tf = f;
    send_signal(current, sig);
    do_signal_check(current); /* redirects f->rip/ussp */
    return true;
}

static void kernel_context_dump(struct intr_frame *f)
{
    u64 gs_rd = rdmsr(0xC0000101);
    struct cpu_info *ci = this_cpu_data();
    kprintf("[exception] GS_MSR=%p this_cpu_data=%p\n", gs_rd, (void *)ci);

    u64 *sp = (u64 *)f;
    kprintf("[stack]");
    for (int q = 22; q < 44; q++) kprintf(" %lx", sp[q]);
    kprintf("\n");
}

/* ---- hardware interrupt path ---- */
static void irq_path(struct intr_frame *f)
{
    int irq = f->intno - 32;
    /* current->tf is the frame this task will iretq THROUGH to get back to
     * user mode -- sys_fork() copies it, sys_execve() rewrites its rip/ussp,
     * signal.c() redirects it (see the contract at signal.c:159).  An IRQ
     * that arrived in ring 0 pushed a frame that is discarded on the way
     * back INTO the kernel, so publishing it here hands those callers a
     * frame nobody returns through.  C49 made the HBA interrupt-driven,
     * which put an IRQ inside exactly the syscall that matters:
     * sys_execve() then wrote {rip=entry, ussp=sp} into the dead IRQ
     * frame, the live syscall frame kept /bin/init's "int 0x80" return
     * address, and iretq re-entered that address in the address space
     * execve had just installed -- /bin/sh died with #UD at a rip that was
     * not even an instruction boundary in its own text.  Only a user-mode
     * frame IS that frame, so only one is published. */
    if (current && current->pid != 0 && (f->cs & 3) == 3) current->tf = f;

    if (irq >= 0 && irq < 16) {
        /* EOI BEFORE the handler: handlers such as the PIT tick may call
         * schedule() and switch away; a delayed EOI would leave the LAPIC
         * ISR bit set and stall this vector until the interrupted task
         * resumes (losing ticks / hanging idle's hlt). */
        apic_eoi();
        irq_handler_t h = irq_get_handler(irq);
        if (h) h(f);
    } else {
        kprintf("stray vector %d\n", (int)f->intno);
    }

    /* deliver pending signals on every return path to user mode */
    if (current && (f->cs & 3) == 3) do_signal_check(current);
}

void isr_common(struct intr_frame *f)
{
    /* Ring-0 entry: drop EFLAGS.AC unconditionally (when SMAP exists).
     * Userspace owns AC and may have set it with POPF; entering the kernel
     * with AC=1 would let the very next user-page access bypass SMAP.
     * Hardware preserves AC across the iretq back (the frame carries the
     * user's original flags), so userspace loses nothing.  CLAC #UDs while
     * CR4.SMAP=0, hence the gate. */
    if (g_smap) __asm__ volatile("clac" ::: "memory");

    if (handle_ipi(f)) return;

    if (f->intno < 32) {
        u64 cr2 = 0;
        if (f->intno == 14) {
            cr2 = cr2_read();
            /* T-090: try to RESOLVE the fault (lazy stack growth) before
             * reporting or killing.  Only a user-mode fault is a
             * candidate; a kernel #PF (a bug) or an unfixable fault falls
             * through to the safe report+kill below.  A fixed fault is
             * silent -- iretq simply retries the instruction, now that
             * the page exists. */
            if ((f->cs & 3) == 3 && page_fault_try_fix(f, cr2)) return;
        }
        exception_report(f, cr2);
        if (fault_to_signal(f)) return; /* -> POPALL; iretq into handler */
        kernel_context_dump(f);
        panic("unrecoverable exception");
    }

    irq_path(f);
}
