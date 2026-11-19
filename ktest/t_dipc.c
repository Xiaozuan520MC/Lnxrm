/* S1a: dipc_open(53) / dipc_recv(54) / dipc_send(55) -- driver
 * inter-process communication channels (kernel/ipc/dipc.c).
 *
 * NOTHING HERE MAY PARK (the LAPIC timer does not run at KTEST_LATE, so a
 * task that goes to sleep never comes back), which is why every recv below
 * passes LNXRM_DIPC_NONBLOCK: the blocking half of the park protocol --
 * pushing onto waiters, T_SLEEPING, runqueue_remove, schedule(), the
 * unlink on wake -- is the same shape pipe and input_event already test,
 * so this file walks it with hand-built fake tasks instead of a real
 * sleeper (ktest/t_pipe.c's pattern, which is itself copied from
 * t_input.c's).
 *
 * The contract these cases make is the one t_shm.c makes: they start on
 * `current->vmas == NULL` and restore it -- plus vmm_count_user_pages(),
 * pmm_free_bytes() and the filepage cache -- before their last check, so
 * the suite's order cannot matter.
 *
 * The two cases that matter are test_dipc_data (the ring and the mapping a
 * driver gets are literally the same physical frame: push into the kernel
 * side, read the bytes back out of a MAP_SHARED mapping of the descriptor)
 * and test_dipc_gate (all three rows are CAP_SYS_ADMIN, checked by dropping
 * this task's cred to the user tier and putting it straight back).
 *
 * Names and payloads are written into user pages the way t_shm.c writes
 * them: map one, fault it in, then store through the frame's high-half
 * alias -- a kernel self-test has no user-mode store of its own.
 */
#include <sys/ktest.h>
#include <sys/dipc.h>
#include <sys/vfs.h>
#include <sys/sched.h>
#include <console.h> /* memset / memcpy / memcmp / strlen */
#include <mm/mm.h>
#include <mm/vma.h>
#include <mm/filepage.h>
#include <mm/pagefault.h>

/* A synthetic user #PF, the same shape t_mmap.c builds: bit0 clear (not
 * present) so the fixers run, cs = 3 so it reads as a ring-3 access. */
#define PF_NP_R 0x00
static void pf_frame(struct intr_frame *f, u64 err)
{
    memset(f, 0, sizeof(*f));
    f->cs = 3;
    f->err = err;
}

/* ---- the two user pages every case below writes through ---- */

static u64 name_va; /* holds the name a syscall is pointed at */
static u64 buf_va;  /* holds what recv writes / send reads */

/* Map one anonymous page and fault it in.  On ANY failure it unmaps what it
 * made and answers false, so a caller that returns on `false` leaves the
 * address space exactly as it found it. */
static bool upage(u64 *slot)
{
    if (*slot) return true;

    long p = syscall_probe6(SYS_mmap, 0, PAGE_SIZE, LNXRM_PROT_READ | LNXRM_PROT_WRITE,
                            0, (u64)-1, 0);
    u64 va = p > 0 ? (u64)p : 0;
    if (va) {
        struct intr_frame f;
        pf_frame(&f, PF_NP_R);
        if (!page_fault_try_fix(&f, va)) va = 0;
    }
    if (!va) {
        if (p > 0) syscall_probe(SYS_munmap, (u64)p, PAGE_SIZE, 0);
        K_EXPECT(va != 0);
        return false;
    }
    *slot = va;
    return true;
}

/* Hand both pages back.  Every case calls this before its last check. */
static void pages_done(void)
{
    if (name_va) {
        K_EXPECT_EQ(syscall_probe(SYS_munmap, name_va, PAGE_SIZE, 0), 0);
        name_va = 0;
    }
    if (buf_va) {
        K_EXPECT_EQ(syscall_probe(SYS_munmap, buf_va, PAGE_SIZE, 0), 0);
        buf_va = 0;
    }
}

/* The frame behind a user address, through the high-half alias.  copy_*_user
 * does NOT fault a page in -- it refuses a range that is not mapped -- so
 * both pages are always faulted in before anybody reads them back.  NULL is
 * possible (a translate that failed), so nothing below dereferences without
 * asking. */
static void *uat(u64 va)
{
    u64 pa = vmm_translate_in(current->pml4, va);
    K_EXPECT(pa != 0);
    return pa ? (void *)PHYS_TO_VIRT(pa) : NULL;
}

static void set_bytes(u64 va, const void *src, size_t n)
{
    void *p = uat(va);
    if (p) memcpy(p, src, n);
}

/* What a syscall wrote into `va`, compared.  Answers false (and records the
 * failed translate) instead of dereferencing NULL. */
static bool got_bytes(u64 va, const void *expect, size_t n)
{
    void *p = uat(va);
    return p && memcmp(p, expect, n) == 0;
}

/* Fault a shared range in and answer the frame it landed on (0 on failure). */
static u64 map_in(long va)
{
    struct intr_frame f;
    pf_frame(&f, PF_NP_R);
    if (!page_fault_try_fix(&f, (u64)va)) return 0;
    return vmm_translate_in(current->pml4, (u64)va);
}

/* ---- the sink one channel is created with ---- */
static u8 sink_buf[64];
static u32 sink_len;
static u32 sink_calls;

static long t_sink(const u8 *buf, u32 len)
{
    if (len <= sizeof(sink_buf)) {
        memcpy(sink_buf, buf, len);
        sink_len = len;
    }
    sink_calls++;
    return (long)len; /* the value dipc_send must hand straight back */
}

/* ================= the gate and the refusals ================= */
static void test_dipc_gate(void)
{
    K_EXPECT(current->vmas == NULL);
    u32 live0 = dipc_live_objects();

    if (!upage(&name_va)) return;
    if (!upage(&buf_va)) {
        pages_done();
        return;
    }

    /* ---- all three rows are CAP_SYS_ADMIN ----
     * Drop this task to the user tier with no capabilities, ask, and put it
     * straight back: the gate runs in syscall_do BEFORE any wrapper, so the
     * answer is -EACCES whether or not a channel of that name exists. */
    struct cred saved_cred = current->cred;
    current->cred.kind = CRED_USER;
    current->cred.caps = 0;

    set_bytes(name_va, "dipc_gate", 10);
    K_EXPECT_EQ(syscall_probe(SYS_dipc_open, name_va, 0, 0), LNXRM_EACCES);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_recv, 1, buf_va, 4, LNXRM_DIPC_NONBLOCK, 0, 0),
                LNXRM_EACCES);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_send, 1, buf_va, 4, 0, 0, 0), LNXRM_EACCES);

    current->cred = saved_cred;
    K_EXPECT_EQ((u64)current->cred.kind, (u64)saved_cred.kind); /* put back */
    K_EXPECT_EQ(current->cred.caps, saved_cred.caps);
    K_EXPECT_EQ(dipc_live_objects(), live0);

    /* ---- a pointer ring 3 could not have named ---- */
    K_EXPECT_EQ(syscall_probe(SYS_dipc_open, 0xdeadbeef, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(dipc_live_objects(), live0);

    /* ---- names that are not names (each costs nothing) ---- */
    set_bytes(name_va, "", 1);
    K_EXPECT_EQ(syscall_probe(SYS_dipc_open, name_va, 0, 0), LNXRM_EINVAL);
    set_bytes(name_va, "////", 5);
    K_EXPECT_EQ(syscall_probe(SYS_dipc_open, name_va, 0, 0), LNXRM_EINVAL);
    set_bytes(name_va, "a/b", 4); /* an interior slash: this is not a path */
    K_EXPECT_EQ(syscall_probe(SYS_dipc_open, name_va, 0, 0), LNXRM_EINVAL);
    set_bytes(name_va, "01234567890123456789012345678901", 33); /* 32 + NUL */
    K_EXPECT_EQ(syscall_probe(SYS_dipc_open, name_va, 0, 0), LNXRM_ENAMETOOLONG);
    K_EXPECT_EQ(dipc_live_objects(), live0);

    /* ---- a flag, on a name that does not exist: the flag is refused
     * before the lookup, so the answer is EINVAL either way ---- */
    set_bytes(name_va, "dipc_fl", 8);
    K_EXPECT_EQ(syscall_probe(SYS_dipc_open, name_va, 1, 0), LNXRM_EINVAL);
    K_EXPECT_EQ(dipc_live_objects(), live0);

    /* ---- a channel nobody created ---- */
    set_bytes(name_va, "dipc_none", 10);
    K_EXPECT_EQ(syscall_probe(SYS_dipc_open, name_va, 0, 0), LNXRM_ENOENT);
    K_EXPECT_EQ(dipc_live_objects(), live0);

    /* ---- recv's bounds, in the order it checks them ---- */
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_recv, (u64)-1, buf_va, 0, 0, 0, 0), LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_recv, (u64)-1, buf_va, LNXRM_DIPC_RECV_MAX + 1, 0, 0, 0),
                LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_recv, (u64)-1, buf_va, 4, 0x100, 0, 0), LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_recv, (u64)-1, (u64)0, 4, 0, 0, 0), LNXRM_EFAULT);
    /* ... then "is that a channel at all" */
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_recv, (u64)-1, buf_va, 4, 0, 0, 0), LNXRM_EBADF);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_recv, NR_FDS, buf_va, 4, 0, 0, 0), LNXRM_EBADF);

    /* ---- and send's ---- */
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_send, (u64)-1, buf_va, 4, 1, 0, 0), LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_send, (u64)-1, buf_va, 0, 0, 0, 0), LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_send, (u64)-1, buf_va, LNXRM_DIPC_SEND_MAX + 1, 0, 0, 0),
                LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_send, (u64)-1, buf_va, 4, 0, 0, 0), LNXRM_EBADF);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_send, NR_FDS, buf_va, 4, 0, 0, 0), LNXRM_EBADF);

    pages_done();
    K_EXPECT(current->vmas == NULL);
}
KTEST("dipc", KTEST_LATE, test_dipc_gate);

/* ================= name, file, frame: the whole lifetime ================= */
static void test_dipc_lifecycle(void)
{
    K_EXPECT(current->vmas == NULL);
    u64 fp0 = filepage_resident();
    u64 pages0 = vmm_count_user_pages(current->pml4);
    u64 free0 = pmm_free_bytes();
    u32 live0 = dipc_live_objects();

    if (!upage(&name_va)) return;

    /* ---- create: the leading slash is not part of the name ---- */
    K_EXPECT_EQ(dipc_create("/dipc_lc", NULL), 0);
    K_EXPECT_EQ(dipc_live_objects(), live0 + 1);
    K_EXPECT_EQ(dipc_create("dipc_lc", NULL), LNXRM_EEXIST);
    K_EXPECT_EQ(dipc_live_objects(), live0 + 1); /* EEXIST allocated nothing */
    K_EXPECT_EQ(dipc_destroy("a/b"), LNXRM_EINVAL);
    K_EXPECT_EQ(dipc_destroy(""), LNXRM_EINVAL);
    K_EXPECT_EQ(dipc_destroy("dipc_absent"), LNXRM_ENOENT);

    /* ---- the capacity valve: eight channels, the ninth refused ---- */
    static const char *const extra[] = {"dipc_x1", "dipc_x2", "dipc_x3",
                                        "dipc_x4", "dipc_x5", "dipc_x6", "dipc_x7"};
    for (u32 i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) {
        K_EXPECT_EQ(dipc_create(extra[i], NULL), 0);
        K_EXPECT_EQ(dipc_live_objects(), live0 + 2 + i);
    }
    K_EXPECT_EQ(dipc_live_objects(), (u32)(live0 + 8));
    K_EXPECT_EQ(dipc_create("dipc_x9", NULL), LNXRM_ENOSPC);
    K_EXPECT_EQ(dipc_live_objects(), (u32)(live0 + 8));
    for (u32 i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
        K_EXPECT_EQ(dipc_destroy(extra[i]), 0);
    K_EXPECT_EQ(dipc_live_objects(), live0 + 1);

    /* ---- two INDEPENDENT opens share one struct file, which is the whole
     * reason two mappings cannot end up on two frames ---- */
    set_bytes(name_va, "/dipc_lc", 9);
    long fd1 = syscall_probe(SYS_dipc_open, name_va, 0, 0);
    K_EXPECT(fd1 >= 0);
    long fd2 = fd1 >= 0 ? syscall_probe(SYS_dipc_open, name_va, 0, 0) : -1;
    K_EXPECT(fd2 >= 0);

    long m1 = -1, m2 = -1;
    if (fd1 >= 0 && fd2 >= 0) {
        K_EXPECT(fd1 != fd2);
        K_EXPECT(current->fds[fd1] == current->fds[fd2]);

        m1 = syscall_probe6(SYS_mmap, 0, PAGE_SIZE, LNXRM_PROT_READ | LNXRM_PROT_WRITE,
                            LNXRM_MAP_SHARED, (u64)fd1, 0);
        m2 = syscall_probe6(SYS_mmap, 0, PAGE_SIZE, LNXRM_PROT_READ | LNXRM_PROT_WRITE,
                            LNXRM_MAP_SHARED, (u64)fd2, 0);
        K_EXPECT(m1 > 0 && m2 > 0 && m1 != m2);
    }

    if (m1 > 0 && m2 > 0) {
        u64 pa1 = map_in(m1);
        u64 pa2 = map_in(m2);
        K_EXPECT(pa1 != 0);
        K_EXPECT_EQ(pa2, pa1);                                    /* ONE frame */
        K_EXPECT_EQ(filepage_resident(), fp0 + 1);                /* one entry */
        K_EXPECT(vmm_user_pte(current->pml4, (u64)m1) & PG_W);     /* shared */
        K_EXPECT(!(vmm_user_pte(current->pml4, (u64)m1) & PG_COW)); /* not COW */
    }

    /* ---- teardown on every path, in one place ---- */
    if (m1 > 0) K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)m1, PAGE_SIZE, 0), 0);
    if (m2 > 0) K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)m2, PAGE_SIZE, 0), 0);
    if (fd1 >= 0) K_EXPECT_EQ(syscall_probe(SYS_close, (u64)fd1, 0, 0), 0);
    if (fd2 >= 0) K_EXPECT_EQ(syscall_probe(SYS_close, (u64)fd2, 0, 0), 0);

    /* Everything is closed but the name still holds the channel: the entry's
     * own reference is the one only dipc_destroy drops. */
    K_EXPECT_EQ(dipc_live_objects(), live0 + 1);
    K_EXPECT_EQ(filepage_resident(), fp0 + 1);

    /* ... and now it goes, frame and all. */
    K_EXPECT_EQ(dipc_destroy("dipc_lc"), 0);
    K_EXPECT_EQ(dipc_live_objects(), live0);
    K_EXPECT_EQ(dipc_destroy("dipc_lc"), LNXRM_ENOENT);
    K_EXPECT_EQ(syscall_probe(SYS_dipc_open, name_va, 0, 0), LNXRM_ENOENT);

    pages_done();
    K_EXPECT_EQ(filepage_resident(), fp0);
    K_EXPECT(current->vmas == NULL);
    K_EXPECT_EQ(vmm_count_user_pages(current->pml4), pages0);
    K_EXPECT_EQ(current->mem_pages, pages0);
    K_EXPECT_EQ(pmm_free_bytes(), free0);
}
KTEST("dipc", KTEST_LATE, test_dipc_lifecycle);

/* ================= bytes in, bytes out, bytes refused ================= */
static void test_dipc_data(void)
{
    K_EXPECT(current->vmas == NULL);
    u64 fp0 = filepage_resident();
    u64 pages0 = vmm_count_user_pages(current->pml4);
    u64 free0 = pmm_free_bytes();
    u32 live0 = dipc_live_objects();

    if (!upage(&name_va)) return;
    if (!upage(&buf_va)) {
        pages_done();
        return;
    }

    sink_calls = 0;
    sink_len = 0;
    K_EXPECT_EQ(dipc_create("dipc_dt", t_sink), 0);
    set_bytes(name_va, "dipc_dt", 8);
    long fd = syscall_probe(SYS_dipc_open, name_va, 0, 0);
    K_EXPECT(fd >= 0);
    if (fd < 0) {
        K_EXPECT_EQ(dipc_destroy("dipc_dt"), 0);
        pages_done();
        return;
    }

    /* ---- an empty channel answers EAGAIN rather than parking ---- */
    K_EXPECT_EQ(dipc_pending("dipc_dt"), 0);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_recv, (u64)fd, buf_va, 64, LNXRM_DIPC_NONBLOCK, 0, 0),
                LNXRM_EAGAIN);

    /* ---- round trip: push on the kernel side, read on the descriptor ---- */
    static const u8 hi[] = {'H', 'i'};
    K_EXPECT_EQ(dipc_push("dipc_dt", hi, 2), 2);
    K_EXPECT_EQ(dipc_pending("dipc_dt"), 2);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_recv, (u64)fd, buf_va, 64, LNXRM_DIPC_NONBLOCK, 0, 0), 2);
    K_EXPECT(got_bytes(buf_va, "Hi", 2));
    K_EXPECT_EQ(dipc_pending("dipc_dt"), 0);

    /* ---- THE assertion: the ring and the mapping a driver gets are the
     * same physical frame.  The bytes pushed from ring 0 show up, unasked,
     * at the READ index of a MAP_SHARED mapping of the descriptor -- pay[rd],
     * not pay[0]: this ring has already moved, so its live bytes sit where
     * the header says they do, and the header itself must read back exactly
     * as the kernel last wrote it (rd=2 after the "Hi" read, wr=4 after
     * this push). ---- */
    static const u8 ab[] = {'A', 'B'};
    K_EXPECT_EQ(dipc_push("dipc_dt", ab, 2), 2);
    long m1 = syscall_probe6(SYS_mmap, 0, PAGE_SIZE, LNXRM_PROT_READ | LNXRM_PROT_WRITE,
                             LNXRM_MAP_SHARED, (u64)fd, 0);
    K_EXPECT(m1 > 0);
    if (m1 > 0) {
        u64 pa = map_in(m1);
        K_EXPECT(pa != 0);
        K_EXPECT_EQ(filepage_resident(), fp0 + 1); /* the one create made */
        if (pa) {
            const u8 *page = (const u8 *)PHYS_TO_VIRT(pa);
            const struct dipc_hdr *h = (const struct dipc_hdr *)page;
            K_EXPECT_EQ(h->rd, 2); /* read and write indices are shared too */
            K_EXPECT_EQ(h->wr, 4);
            if (h->rd == 2 && h->wr == 4) /* bounds before indexing the frame */
                K_EXPECT_EQ(memcmp(page + LNXRM_DIPC_HDR_SIZE + h->rd, "AB", 2), 0);
        }
        K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)m1, PAGE_SIZE, 0), 0);
    }
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_recv, (u64)fd, buf_va, 64, LNXRM_DIPC_NONBLOCK, 0, 0), 2);
    K_EXPECT(got_bytes(buf_va, "AB", 2));

    /* ---- a bad destination costs the caller nothing ---- */
    K_EXPECT_EQ(dipc_push("dipc_dt", hi, 2), 2);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_recv, (u64)fd, 0xdeadbeef, 64, LNXRM_DIPC_NONBLOCK, 0, 0),
                LNXRM_EFAULT);
    K_EXPECT_EQ(dipc_pending("dipc_dt"), 2); /* rd did not move */

    /* ... and now that stream is drained, so the flood below starts from an
     * empty ring and its drop count is the flood's and nothing else's. */
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_recv, (u64)fd, buf_va, 64, LNXRM_DIPC_NONBLOCK, 0, 0), 2);
    K_EXPECT_EQ(dipc_pending("dipc_dt"), 0);
    K_EXPECT_EQ(dipc_dropped("dipc_dt"), 0);

    /* ---- flood: a full ring drops, counts, and never wraps into the
     * header -- push 4096 bytes into a ring that holds 4087 ---- */
    static u8 flood[LNXRM_DIPC_RECV_MAX];
    memset(flood, 'x', sizeof(flood));
    u32 cap = LNXRM_DIPC_CAPACITY;
    K_EXPECT_EQ(dipc_push("dipc_dt", flood, sizeof(flood)), cap);
    K_EXPECT_EQ(dipc_pending("dipc_dt"), cap);
    K_EXPECT_EQ(dipc_dropped("dipc_dt"), sizeof(flood) - (u64)cap);
    K_EXPECT_EQ(dipc_push("dipc_dt", flood, 1), 0); /* full: stored nothing */
    K_EXPECT_EQ(dipc_dropped("dipc_dt"), sizeof(flood) - (u64)cap + 1);
    K_EXPECT_EQ(dipc_pending("dipc_dt"), cap);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_recv, (u64)fd, buf_va, LNXRM_DIPC_RECV_MAX,
                               LNXRM_DIPC_NONBLOCK, 0, 0),
                (long)cap);
    K_EXPECT_EQ(dipc_pending("dipc_dt"), 0);
    K_EXPECT_EQ(dipc_dropped("dipc_dt"), sizeof(flood) - (u64)cap + 1); /* kept */

    /* ---- send: straight through to the sink, return value included ---- */
    set_bytes(buf_va, "KEY!", 5);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_send, (u64)fd, buf_va, 4, 0, 0, 0), 4);
    K_EXPECT_EQ(sink_calls, 1);
    K_EXPECT_EQ(sink_len, 4);
    K_EXPECT_EQ(memcmp(sink_buf, "KEY!", 4), 0);

    /* ---- and every refusal in front of it, none of which reaches the
     * sink -- the call counter is the proof ---- */
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_send, (u64)fd, buf_va, 0, 0, 0, 0), LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_send, (u64)fd, buf_va, LNXRM_DIPC_SEND_MAX + 1, 0, 0, 0),
                LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_send, (u64)fd, buf_va, 4, 0x100, 0, 0), LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe6(SYS_dipc_send, (u64)fd, 0xdeadbeef, 4, 0, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(sink_calls, 1);

    /* ---- a descriptor that is NOT a channel: shm's file has the same V_REG
     * vnode and the same reg_close door, and still is not one of ours ---- */
    set_bytes(name_va, "dipc_sf", 8);
    long shmfd = syscall_probe(SYS_shm_open, name_va, LNXRM_O_CREAT | LNXRM_O_EXCL, PAGE_SIZE);
    K_EXPECT(shmfd >= 0);
    if (shmfd >= 0) {
        K_EXPECT_EQ(syscall_probe6(SYS_dipc_recv, (u64)shmfd, buf_va, 4,
                                   LNXRM_DIPC_NONBLOCK, 0, 0),
                    LNXRM_EBADF);
        K_EXPECT_EQ(syscall_probe6(SYS_dipc_send, (u64)shmfd, buf_va, 4, 0, 0, 0), LNXRM_EBADF);
        K_EXPECT_EQ(syscall_probe(SYS_close, (u64)shmfd, 0, 0), 0);
        K_EXPECT_EQ(syscall_probe(SYS_shm_unlink, name_va, 0, 0), 0);
    }

    /* ---- a channel with no sink: accepted, and refused at send time ---- */
    K_EXPECT_EQ(dipc_create("dipc_ns", NULL), 0);
    set_bytes(name_va, "dipc_ns", 8);
    long fd2 = syscall_probe(SYS_dipc_open, name_va, 0, 0);
    K_EXPECT(fd2 >= 0);
    if (fd2 >= 0) {
        K_EXPECT_EQ(syscall_probe6(SYS_dipc_send, (u64)fd2, buf_va, 4, 0, 0, 0), LNXRM_EINVAL);
        K_EXPECT_EQ(syscall_probe(SYS_close, (u64)fd2, 0, 0), 0);
    }
    K_EXPECT_EQ(dipc_destroy("dipc_ns"), 0);

    K_EXPECT_EQ(syscall_probe(SYS_close, (u64)fd, 0, 0), 0);
    K_EXPECT_EQ(dipc_destroy("dipc_dt"), 0);
    K_EXPECT_EQ(dipc_live_objects(), live0);

    pages_done();
    K_EXPECT_EQ(filepage_resident(), fp0);
    K_EXPECT(current->vmas == NULL);
    K_EXPECT_EQ(vmm_count_user_pages(current->pml4), pages0);
    K_EXPECT_EQ(current->mem_pages, pages0);
    K_EXPECT_EQ(pmm_free_bytes(), free0);
}
KTEST("dipc", KTEST_LATE, test_dipc_data);

/* ================= the wait list: link shapes, then the REAL wake ========
 *
 * The blocking half of dipc_recv cannot run here -- no timer, so a task
 * that parks stays parked -- but the list it would park on and the wake
 * that would let it out are exactly these operations, driven directly. */
static void test_dipc_waiters(void)
{
    K_EXPECT(current->vmas == NULL);
    u32 live0 = dipc_live_objects();

    K_EXPECT_EQ(dipc_create("dipc_wq", NULL), 0);

    /* Two fake sleepers, never on any runqueue (t_pipe.c's pattern).
     * sleep_until is "never" so no tick -- were one running -- could
     * mistake them for timed sleepers and wake them behind our back. */
    struct task *t1 = task_alloc_slot();
    struct task *t2 = task_alloc_slot();
    K_ASSERT(t1 != NULL && t2 != NULL);
    t1->state = T_SLEEPING;
    t1->wait_next = NULL;
    t1->sleep_until = ~0ULL;
    t2->state = T_SLEEPING;
    t2->wait_next = NULL;
    t2->sleep_until = ~0ULL;

    u64 f;
    struct dipc *d;
    spin_lock_irqsave(&dipc_lock, &f);
    d = dipc_find("dipc_wq");
    spin_unlock_irqrestore(&dipc_lock, f);
    K_ASSERT(d != NULL);
    K_EXPECT(d->waiters == NULL);

    /* push-front, asserted OUTSIDE the lock: the checks print, and a
     * kprintf under dipc_lock is a lock-order accident waiting to happen. */
    struct task *head;
    spin_lock_irqsave(&dipc_lock, &f);
    t1->wait_next = d->waiters;
    d->waiters = t1;
    t2->wait_next = d->waiters;
    d->waiters = t2;
    head = d->waiters;
    spin_unlock_irqrestore(&dipc_lock, f);
    K_EXPECT_EQ(head, t2);  /* last one in is the head */
    K_EXPECT_EQ(t2->wait_next, t1);
    K_EXPECT(t1->wait_next == NULL);

    /* unlink is idempotent: the second attempt finds nothing and leaves the
     * chain alone rather than corrupting what it already removed */
    spin_lock_irqsave(&dipc_lock, &f);
    {
        struct task **p = &d->waiters;
        while (*p && *p != t1) p = &(*p)->wait_next;
        if (*p == t1) *p = t1->wait_next;
        t1->wait_next = NULL;
    }
    head = d->waiters;
    spin_unlock_irqrestore(&dipc_lock, f);
    K_EXPECT_EQ(head, t2); /* t2 no longer links to a removed node */

    spin_lock_irqsave(&dipc_lock, &f);
    {
        struct task **p = &d->waiters;
        while (*p && *p != t1) p = &(*p)->wait_next;
        if (*p == t1) *p = t1->wait_next;
        t1->wait_next = NULL;
    }
    head = d->waiters;
    spin_unlock_irqrestore(&dipc_lock, f);
    K_EXPECT_EQ(head, t2);

    /* put t1 back so both are queued for the wake */
    spin_lock_irqsave(&dipc_lock, &f);
    t1->wait_next = d->waiters;
    d->waiters = t1;
    spin_unlock_irqrestore(&dipc_lock, f);
    K_EXPECT_EQ(d->waiters, t1);

    /* THE wake path, through the real producer: the list is detached under
     * the lock and woken outside it, because sched_wake_task takes runq_lock
     * and must never nest under a queue lock. */
    K_EXPECT_EQ(dipc_push("dipc_wq", (const u8 *)"x", 1), 1);
    K_EXPECT(d->waiters == NULL);
    K_EXPECT_EQ(t1->state, T_RUNNABLE);
    K_EXPECT_EQ(t2->state, T_RUNNABLE);
    K_EXPECT(t1->wait_next == NULL);
    K_EXPECT(t2->wait_next == NULL);
    K_EXPECT(t1->rq_cpu != -1); /* a runqueue entry behind each wake */
    K_EXPECT(t2->rq_cpu != -1);
    task_free_slot(t1); /* pulls them off that runqueue again */
    task_free_slot(t2);

    /* The byte the wake carried is still there for whoever opens the
     * channel, and destroy drops the whole thing in one step. */
    K_EXPECT_EQ(dipc_pending("dipc_wq"), 1);
    K_EXPECT_EQ(dipc_destroy("dipc_wq"), 0);
    K_EXPECT_EQ(dipc_live_objects(), live0);
}
KTEST("dipc", KTEST_LATE, test_dipc_waiters);
