/* End-to-end coverage of every number in include/abi/lnxrm_syscalls.def.
 *
 * Live calls (32, numbered 0..31 with no holes), all exercised below:
 *    0 read      1 write     2 open      3 close     4 lseek     5 brk
 *    6 getdent   7 dup2      8 nanosleep 9 getpid   10 fork    11 execve
 *   12 exit     13 wait4    14 kill     15 uname    16 sigaction 17 sigprocmask
 *   18 getppid  19 ps       20 sigreturn 21 diskinfo 22 mkdir
 *   23 fb_info  24 fb_clear 25 fb_fill  26 fb_char  27 fb_puts
 *   28 unlink   29 rmdir    30 rename   31 setuid
 *
 * Numbers above LNXRM_SYSCALL_LAST (31) are not in the table and must
 * answer LNXRM_ENOSYS (-38) -- checked at the end.
 *
 * Expected values are printed as tag=value for an out-of-tree checker.
 */
#include "ulib.h"

static volatile int got_sig;
static volatile int got_no;

static void on_sigusr1(int sig)
{
    got_sig = 1;
    got_no = sig;
}

static void on_segv(int sig)
{
    xputs("segv.caught=");
    xprinti(sig);
    xputs("\n");
}

static void say(const char *tag, long v)
{
    xputs(tag);
    xputs("=");
    xprinti(v);
    xputs("\n");
}

/* T-032: what the kernel says `pid`'s user space costs, in KiB -- the
 * answer to "who owns the RAM".  `peak_kib` and `state` are optional
 * out-parameters (-1 when the task is not in the table).  Only root may
 * call kps (T-031), so every user of this helper runs as root. */
static long ps_mem_kib(long pid, long *peak_kib, int *state)
{
    struct lnxrm_ps_entry p[32];
    long n = kps(p, 32);

    if (peak_kib) *peak_kib = -1;
    if (state) *state = -1;
    for (long i = 0; i < n; i++) {
        if (p[i].pid != pid) continue;
        if (peak_kib) *peak_kib = p[i].peak_kib;
        if (state) *state = p[i].state;
        return p[i].mem_kib;
    }
    return -1;
}

int main(int argc, char **argv)
{
    extern char **environ;

    if (argc >= 2 && argv[1][0] == 'e') {
        /* child re-entered via execve with a populated envp */
        xputs("envchild environ[0]=");
        xputs(environ && environ[0] ? environ[0] : "(null)");
        xputs(" environ[1]=");
        xputs(environ && environ[1] ? environ[1] : "(null)");
        xputs("\n");
        kexit(0);
    }

    /* ---- 15 uname ---- */
    {
        struct lnxrm_utsname u;
        long r = kuname(&u);
        say("uname.ret", r);
        xputs("uname ");
        xputs(u.sysname);
        xputs(" ");
        xputs(u.release);
        xputs(" ");
        xputs(u.machine);
        xputs("\n");
        say("uname.sysname", u.sysname[0] != 0);
        say("uname.nodename", u.nodename[0] != 0);
        say("uname.version", u.version[0] != 0);
    }

    /* ---- 9 getpid, 18 getppid ---- */
    {
        long pid = kgetpid(), ppid = kgetppid();
        say("getpid", pid);
        say("getppid", ppid);
        say("getpid.gt0", pid > 0);
        say("getppid.gt0", ppid > 0);
    }

    /* ---- 2 open, 4 lseek, 0 read, 3 close ---- */
    {
        long fd = kopen("/README.md", 0);
        say("open.fd", fd);
        say("open.missing", kopen("/no-such-file", 0)); /* -2 ENOENT */
        say("close.badfd", kclose(123));                /* -9 EBADF */
        say("seek.set5", klseek(fd, 5, LNXRM_SEEK_SET));
        char b[16];
        long n = kread(fd, b, 15);
        b[n > 0 ? n : 0] = 0;
        say("read.after.seek", n);
        xputs("bytes=[");
        xputs(b);
        xputs("]\n");
        say("seek.end-4", klseek(fd, -4, LNXRM_SEEK_END));
        n = kread(fd, b, 15);
        b[n > 0 ? n : 0] = 0;
        say("read.at.end", n);
        xputs("bytes=[");
        xputs(b);
        xputs("]\n");
        say("seek.bad", klseek(fd, 0, 99));
        say("close", kclose(fd));
    }

    /* ---- 1 write: create, write, read back ---- */
    {
        kunlink("/st_w"); /* leftovers from an earlier run; errors ignored */
        long fd = kopen("/st_w", 0100 | 1 | 01000); /* O_CREAT(0100)|write-only(1)|O_TRUNC(01000) */
        say("write.open", fd);
        const char *msg = "systest-write";
        say("write.n", kwrite(fd, msg, 13));
        say("write.close", kclose(fd));

        fd = kopen("/st_w", 0);
        char b[32];
        long n = kread(fd, b, sizeof(b) - 1);
        b[n > 0 ? n : 0] = 0;
        say("write.readback", n);
        xputs("write.bytes=[");
        xputs(b);
        xputs("]\n");
        say("write.close2", kclose(fd));
        say("write.unlink", kunlink("/st_w")); /* 0 */
    }

    /* ---- 5 brk ---- */
    {
        long base = kbrk(0);
        say("brk.base", base);
        say("brk.grow", kbrk(base + 8192) == base + 8192);
        say("brk.below.base", kbrk(base - 4096)); /* -12 ENOMEM */
        say("brk.shrink", kbrk(base) == base);
    }

    /* ---- 7 dup2 ---- */
    {
        long fd = kopen("/README.md", 0);
        long d = kdup2(fd, 7);
        say("dup2.fd7", d);            /* 7 */
        say("dup2.self", kdup2(7, 7)); /* 7 (same fd, no-op) */
        say("dup2.bad", kdup2(-1, 8)); /* -9 EBADF */
        char b[5];
        long n = kread(7, b, 4);
        b[n > 0 ? n : 0] = 0;
        say("dup2.read", n);
        xputs("dup2.bytes=[");
        xputs(b);
        xputs("]\n");
        say("dup2.close7", kclose(7));
        say("dup2.closeold", kclose(fd));
    }

    /* ---- 6 getdent ---- */
    {
        long fd = kopen("/", 0);
        struct lnxrm_dirent e;
        long inos = 0, cnt = 0;
        while (kgetdent(fd, &e, sizeof(e)) > 0) {
            cnt++;
            if (e.d_ino) inos++;
        }
        kclose(fd);
        say("dirent.total", cnt);
        say("dirent.with_ino", inos);

        /* a plain file is not a directory -> EBADF (-9) */
        long f = kopen("/README.md", 0);
        say("getdent.nondir", kgetdent(f, &e, sizeof(e)));
        kclose(f);
    }

    /* ---- 8 nanosleep: full sleep returns 0 with rem == 0 ---- */
    {
        struct lnxrm_timespec rem = {123, 456};
        long r = ksleep_ms(50, &rem);
        say("sleep.full.ret", r);
        say("sleep.full.sec", (long)rem.tv_sec);
        say("sleep.full.nsec", (long)rem.tv_nsec);
    }

    /* ---- 14 kill: existence probe + T-031 checkpoints ---- */
    {
        say("kill.self.0", kkill(kgetpid(), 0));
        say("kill.9999.0", kkill(9999, 0));          /* -3 ESRCH */
        say("kill.badsig", kkill(kgetpid(), 99));    /* -22 EINVAL */
        /* T-031: pid 1 sits under kxld protection -- even root gets
         * EACCES (-13), for a real signal and for the probe alike. */
        say("kill.init.term", kkill(1, SIGTERM));    /* -13 */
        say("kill.init.0", kkill(1, 0));             /* -13 */
        /* T-031 control: the gate must still deliver to a same-uid
         * target.  Without this, "pid 1 refused" could equally mean
         * kill is simply broken -- "can kill" has to be proven too. */
        {
            long cp = kfork();
            if (cp == 0) {
                for (;;) ksleep_ms(1000, 0);
            }
            if (cp > 0) {
                int st = 0;
                say("kill.child.term", kkill(cp, SIGTERM));
                say("kill.child.wait", kwaitpid(cp, &st, 0));
                say("kill.child.status", st); /* -SIGTERM once delivered */
            }
        }
    }

    /* ---- 16 sigaction: catch a signal we send ourselves ---- */
    {
        struct lnxrm_sigaction act, old;
        long r;
        act.sa_handler = (void *)on_sigusr1;
        act.sa_mask = 0;
        act.sa_flags = 0;
        act.sa_reserved = 0;
        old.sa_handler = 0;
        old.sa_mask = 0;
        old.sa_flags = 0;
        old.sa_reserved = 0;

        r = ksigaction(SIGUSR1, &act, &old);
        say("sigaction.new", r);
        say("sigaction.old.dfl", old.sa_handler == SIG_DFL);

        r = ksigaction(SIGKILL, &act, 0);
        say("sigaction.SIGKILL", r); /* must be -22 EINVAL */

        got_sig = 0;
        got_no = 0;
        kkill(kgetpid(), SIGUSR1);
        say("handler.ran", got_sig);
        say("handler.sig", got_no);
    }

    /* ---- 17 sigprocmask: block, raise, unblock ---- */
    {
        uint64_t set = 1ULL << SIGUSR1;
        uint64_t oldm = 0xdead;
        long r = ksigprocmask(LNXRM_SIG_BLOCK, &set, &oldm);
        say("block.ret", r);
        say("block.old", (long)oldm);

        got_sig = 0;
        got_no = 0;
        kkill(kgetpid(), SIGUSR1);
        say("blocked.ran", got_sig); /* must be 0 */

        r = ksigprocmask(LNXRM_SIG_UNBLOCK, &set, 0);
        say("unblock.ret", r);
        say("unblocked.ran", got_sig); /* must be 1 */
        say("unblocked.sig", got_no);  /* must be 10 */
    }

    /* ---- 20 sigreturn outside a handler -> EINVAL ---- */
    say("sigreturn.nohandler", ksigreturn()); /* -22 EINVAL */

    /* ---- 8 nanosleep cut short by a signal -> EINTR + remainder ---- */
    {
        struct lnxrm_sigaction act;
        act.sa_handler = (void *)on_sigusr1;
        act.sa_mask = 0;
        act.sa_flags = 0;
        act.sa_reserved = 0;
        ksigaction(SIGUSR1, &act, 0);

        long pid = kfork();
        if (pid == 0) {
            ksleep_ms(100, 0);
            kkill(kgetppid(), SIGUSR1);
            kexit(0);
        }
        struct lnxrm_timespec rem = {0, 0};
        got_sig = 0;
        long r = ksleep_ms(3000, &rem);
        say("sleep.eintr.ret", r); /* must be -4 (LNXRM_EINTR) */
        say("sleep.eintr.sec", (long)rem.tv_sec);
        say("sleep.eintr.nsec", (long)rem.tv_nsec);
        say("sleep.eintr.handler", got_sig);
        int st = 0;
        kwaitpid(pid, &st, 0);
    }

    /* ---- 10 fork, 11 execve, 12 exit, 13 wait4 ---- */
    {
        long pid = kfork();
        if (pid == 0) {
            char *a[] = {"systest", "envchild", 0};
            char *e[] = {"FOO=bar", "PATH=/bin", 0};
            kexecve("/bin/systest", a, e);
            kexit(1); /* exec failed */
        }
        int st = 0;
        kwaitpid(pid, &st, 0);
        say("envchild.exit", st);
    }

    /* ---- 13 wait4 with WNOHANG (opts bit 0) ---- */
    {
        long pid = kfork();
        if (pid == 0) {
            ksleep_ms(300, 0);
            kexit(3);
        }
        int st = 0;
        long nh = kwaitpid(pid, &st, 1);
        say("wait.nohang", nh); /* 0 while the child still runs */
        int st2 = 0;
        long w = kwaitpid(pid, &st2, 0);
        say("wait.block", w);
        say("wait.code", st2); /* 3 */
    }

    /* ---- 19 ps fills the caller's buffer ---- */
    {
        struct lnxrm_ps_entry procs[16];
        long n = kps(procs, 16);
        say("ps.count", n);
        /* T-030: idle must report the kxld tier (2), init (pid 1) root
         * (1) with uid 0 -- ps carries kind + uid up from the kernel. */
        {
            long idle_kind = -1, init_kind = -1, init_uid = -1;
            for (long i = 0; i < n; i++) {
                if (procs[i].pid == 0) idle_kind = procs[i].kind;
                if (procs[i].pid == 1) {
                    init_kind = procs[i].kind;
                    init_uid = procs[i].uid;
                }
            }
            say("ps.kind.idle", idle_kind);
            say("ps.kind.init", init_kind);
            say("ps.uid.init", init_uid);
        }
        for (long i = 0; i < n && i < 4; i++) {
            xputs("ps ");
            xprinti(procs[i].pid);
            xputs(" ");
            xprinti(procs[i].ppid);
            xputs(" ");
            xputs(procs[i].name);
            xputs("\n");
        }
    }

    /* ---- 22 diskinfo: packed 28-byte records ---- */
    {
#pragma pack(push, 1)
        struct disk_ent {
            char name[16];
            uint32_t sector_size;
            uint64_t num_sectors;
        };
#pragma pack(pop)
        struct disk_ent devs[8];
        long n = kdiskinfo(devs, 8);
        say("diskinfo.count", n);
        for (long i = 0; i < n && i < 4; i++) {
            xputs("disk ");
            xputs(devs[i].name);
            xputs(" sectors=");
            xprinti((long)devs[i].num_sectors);
            xputs(" secsize=");
            xprinti((long)devs[i].sector_size);
            xputs("\n");
        }
    }

    /* ---- 22 mkdir, 30 rename, 28 unlink, 29 rmdir ---- */
    {
        kunlink("/st_d/x"); /* clear leftovers, errors ignored */
        kunlink("/st_f");
        kunlink("/st_g");
        krmdir("/st_d");

        say("mkdir", kmkdir("/st_d"));                    /* 0 */
        long fd = kopen("/st_d/x", 0100 | 1 | 01000);
        say("create.in.dir", fd);
        say("write.in.dir", kwrite(fd, "x", 1));
        say("close.in.dir", kclose(fd));
        say("rmdir.notempty", krmdir("/st_d"));           /* -39 ENOTEMPTY */
        say("unlink.isdir", kunlink("/st_d"));            /* -1 EFAIL */
        say("unlink.child", kunlink("/st_d/x"));          /* 0 */
        say("rmdir.empty", krmdir("/st_d"));              /* 0 */

        fd = kopen("/st_f", 0100 | 1 | 01000);
        say("create.st_f", fd);
        say("close.st_f", kclose(fd));
        say("rename", krename("/st_f", "/st_g"));         /* 0 */
        say("rename.missing", krename("/st_no", "/st_h"));/* -2 ENOENT */
        say("rmdir.onfile", krmdir("/st_g"));             /* -20 ENOTDIR */
        say("unlink.file", kunlink("/st_g"));             /* 0 */
        say("unlink.missing", kunlink("/st_g"));          /* -2 ENOENT */
    }

    /* ---- C46: an overlong path is refused, never truncated ----
     * abs_path() normalises a relative path into a 128-byte scratch
     * buffer.  Dropping the overflow instead of refusing it would make the
     * kernel look up a *different* file than the one named.  The relative
     * control below is the other half of the test: it proves a path that
     * does fit still reaches the filesystem, so "refuse everything"
     * cannot pass these assertions. */
    {
        char toolong[200];
        size_t i;
        for (i = 0; i + 1 < sizeof(toolong); i++) toolong[i] = 'a';
        toolong[sizeof(toolong) - 1] = 0;

        say("path.rel.mkdir", kmkdir("/st_rel"));                  /* 0 */
        long fd = kopen("st_rel/x", 0100 | 1 | 01000);             /* relative */
        say("path.rel.open", fd);                                  /* >= 0 */
        if (fd >= 0) say("path.rel.close", kclose(fd));            /* 0 */

        say("path.toolong.open", kopen(toolong, 0));               /* -36 */
        say("path.toolong.mkdir", kmkdir(toolong));                /* -36 */
        say("path.toolong.unlink", kunlink(toolong));              /* -36 */
        say("path.toolong.rename", krename("st_rel", toolong));    /* -36 */
        say("path.toolong.rename2", krename(toolong, "st_rel"));   /* -36 */

        say("path.rel.unlink", kunlink("st_rel/x"));               /* 0 */
        say("path.rel.rmdir", krmdir("st_rel"));                   /* 0 */
    }

    /* ---- T-033: one surface, one owner ----
     * CAP_FB answers "may this process draw at all"; it cannot answer
     * "which of two privileged processes is talking to the screen".
     * That is the owner gate, and it is checked here FIRST -- before
     * the primitives below touch anything -- because the whole point is
     * that the surface starts out free.
     *
     * The parent never draws in this block: ownership ends with the
     * holder's exit, so the holder has to be somebody who can leave,
     * while the parent has to outlive every step to report them.  Every
     * ordering comes from waitpid rather than from a sleep, so each
     * child's lines are on the console before its parent's waitpid can
     * return -- nothing here can be asserted out of order, on any
     * number of CPUs. */
    {
        int st = 0;
        long op = kfork();
        if (op == 0) {
            /* the holder: claim by drawing, then outlive a failed attempt */
            struct lnxrm_fb_rect mine = {0, 0, 8, 8, 0x002266cc};
            say("fb.owner.grab", kfb_fill(&mine)); /* 0: nobody held it */

            long dp = kfork();
            if (dp == 0) {
                struct lnxrm_fb_rect theirs = {0, 0, 8, 8, 0x00cc2222};
                /* root, so CAP_FB is not what turns this down: -13 here
                 * is the owner gate, and the kernel logs which pids */
                say("fb.owner.denied", kfb_fill(&theirs));
                kexit(0);
            }
            kwaitpid(dp, &st, 0); /* the refusal is on the console by now */

            /* the loser exiting released nothing: it was never the
             * holder, so the holder still draws on its own surface */
            say("fb.owner.held", kfb_fill(&mine)); /* 0 */
            kexit(0); /* ...and only now does it stop holding it */
        }
        kwaitpid(op, &st, 0); /* the holder is gone */

        long rp = kfork();
        if (rp == 0) {
            struct lnxrm_fb_rect r = {0, 0, 8, 8, 0x00cc2222};
            say("fb.owner.released", kfb_fill(&r)); /* 0: free for the next one */
            kexit(0);
        }
        kwaitpid(rp, &st, 0);
    }

    /* ---- 24-28 framebuffer: report the mode, then touch every primitive ----
     * The kernel clips every draw to the visible area, so an off-screen
     * rectangle is a no-op rather than a fault -- that is what
     * fb.clipped below proves.  When no framebuffer was set up (headless)
     * fb_info still succeeds with width == 0 and the draws do nothing. */
    {
        struct lnxrm_fb_info info = {0, 0, 0, 0};
        say("fb.info.ret", kfb_info(&info));
        say("fb.info.w", info.width);
        say("fb.info.h", info.height);
        say("fb.info.bpp", info.bpp);
        say("fb.info.pitch", info.pitch);
        say("fb.info.sane", info.width == 0 || (info.height > 0 && info.pitch >= info.width));

        struct lnxrm_fb_rect r = {0, 0, 64, 16, 0x002266cc}; /* RGB blue block */
        say("fb.fill", kfb_fill(&r));
        struct lnxrm_fb_rect off = {info.width + 16, info.height + 16, 64, 64, 0x00cc2222};
        say("fb.clipped", kfb_fill(&off)); /* outside the screen: 0, no write */

        struct lnxrm_fb_char ch = {8, 8, 'X', 0x00ffffff, 0x00000000}; /* white X */
        say("fb.char", kfb_char(&ch));
        struct lnxrm_fb_str str = {8, 32, 0x0033ff66, 0x00000000, "systest"};
        say("fb.puts", kfb_puts(&str));
        say("fb.clear", kfb_clear(0x00000000));
        say("fb.badptr", kfb_info((struct lnxrm_fb_info *)0x10)); /* -14 EFAULT */
    }

    /* ---- T-031 checkpoints: the capability gates, seen from below ----
     * Everything above ran as root, which proves nothing about a gate:
     * cred_capable() short-circuits on kind >= CRED_ROOT, so root holds
     * every capability by construction.  A forked child drops to the
     * plain-user tier with setuid(1000) -- one way, capabilities cleared
     * -- and then hits each checkpoint.  The parent stays root, watches
     * the drop land through ps, and re-runs the same calls as the
     * control: "child refused" only means something if root is allowed. */
    {
#pragma pack(push, 1)
        struct disk_ent { /* 28 bytes on the wire, no padding (ABI) */
            char name[16];
            uint32_t sector_size;
            uint64_t num_sectors;
        };
#pragma pack(pop)

        long cp = kfork();
        if (cp == 0) {
            say("setuid.drop", ksetuid(1000)); /* 0: root may name any uid */
            say("setuid.regain", ksetuid(0));   /* -13: the drop is one way */
            say("setuid.again", ksetuid(1000)); /* 0: already there */

            struct lnxrm_ps_entry procs[4];
            say("user.ps", kps(procs, 4)); /* -13 CAP_SYS_ADMIN */

            struct disk_ent devs[4];
            say("user.diskinfo", kdiskinfo(devs, 4)); /* -13 CAP_SYS_ADMIN */

            /* fb_info is deliberately the one that stays open: it reads
             * geometry and draws nothing, so there is nothing to gate. */
            struct lnxrm_fb_info info = {0, 0, 0, 0};
            say("user.fb.info", kfb_info(&info)); /* 0 */
            say("user.fb.clear", kfb_clear(0));   /* -13 CAP_FB */
            struct lnxrm_fb_rect r = {0, 0, 1, 1, 0};
            say("user.fb.fill", kfb_fill(&r)); /* -13 CAP_FB */
            struct lnxrm_fb_char ch = {8, 8, 'X', 0x00ffffff, 0};
            say("user.fb.char", kfb_char(&ch)); /* -13 CAP_FB */
            struct lnxrm_fb_str str = {8, 8, 0x00ffffff, 0, "x"};
            say("user.fb.puts", kfb_puts(&str)); /* -13 CAP_FB */

            /* the kill gate reads the tier as well: a different uid with
             * no CAP_KILL puts the root parent out of reach, while a
             * same-uid probe on itself still answers 0 */
            say("user.kill.parent", kkill(kgetppid(), SIGTERM)); /* -13 */
            say("user.kill.self", kkill(kgetpid(), 0));          /* 0 */
            /* and a broadcast is CAP_KILL on its own -- refused whole,
             * for delivery and for the sig==0 probe alike */
            say("user.kill.all", kkill(-1, SIGTERM)); /* -13 */
            say("user.kill.all.probe", kkill(-1, 0)); /* -13 */

            kexit(0);
        }

        if (cp > 0) {
            /* Two rules keep this block's lines from interleaving with
             * the child's on the shared console:
             *  1. capture the ps snapshot SILENTLY -- the child is the
             *     one printing right now;
             *  2. print nothing until waitpid returns, i.e. until the
             *     child has exited with every one of its lines written.
             * The snapshot must happen before the reap: after waitpid the
             * slot is recycled and no longer the child, so poll for the
             * drop instead of sleeping blind (a fixed delay would race
             * it on -smp 1).  A zombie keeps its cred, so finding the
             * drop is guaranteed whether the child is still running or
             * already exited. */
            long kind = -1, uid = -1;
            for (int i = 0; i < 100 && kind != LNXRM_CRED_USER; i++) {
                struct lnxrm_ps_entry snap[16];
                long n = kps(snap, 16);
                kind = -1;
                uid = -1;
                for (long j = 0; j < n; j++) {
                    if (snap[j].pid != cp) continue;
                    kind = snap[j].kind;
                    uid = snap[j].uid;
                }
                if (kind != LNXRM_CRED_USER) ksleep_ms(10, 0);
            }

            int st = 0;
            say("user.reap", kwaitpid(cp, &st, 0) > 0);
            say("user.exit", st);
            say("ps.kind.user", kind); /* 0 = LNXRM_CRED_USER */
            say("ps.uid.user", uid);   /* 1000 */

            /* control: the same calls still work as root */
            struct lnxrm_ps_entry rprocs[4];
            say("root.ps", kps(rprocs, 4) > 0);
            struct disk_ent rdevs[4];
            say("root.diskinfo", kdiskinfo(rdevs, 4) > 0);
            struct lnxrm_fb_info rinfo = {0, 0, 0, 0};
            say("root.fb.info", kfb_info(&rinfo));
            say("root.fb.clear", kfb_clear(0));
            /* CAP_KILL on the broadcast: only the sig==0 probe, so the
             * control cannot terminate the shell sitting above us */
            say("root.kill.all.probe", kkill(-1, 0));
        }
    }

    /* ---- numbers past LNXRM_SYSCALL_LAST and unknown ones -> -38 ---- */
    {
        static const long past_table[] = {32, 33, 34};
        for (int i = 0; i < (int)(sizeof(past_table) / sizeof(past_table[0])); i++) {
            xputs("past.");
            xprinti(past_table[i]);
            xputs("=");
            xprinti(syscall1(past_table[i], 0));
            xputs("\n");
        }
        say("unknown.99", syscall1(99, 0));
    }

    /* ---- environ is published by crt0 ---- */
    xputs("environ=");
    xphex((u64)environ);
    xputs("\n");

    /* ---- NX: brk pages are mapped non-executable ----
     * A `ret` sitting in a heap page must #PF (instruction fetch from an
     * XD page) instead of returning: if it were executable the child would
     * print nx.nofault and exit 9.  The page is written before the fork, so
     * the test also proves dup_user_aspace() carries PG_NX across. */
    {
        long base = kbrk(0);
        if (kbrk(base + 4096) == base + 4096) {
            volatile unsigned char *code = (volatile unsigned char *)base;
            code[0] = 0xc3; /* ret */
        }
        long pid = kfork();
        if (pid == 0) {
            void (*volatile jump)(void) = (void (*)(void))base;
            jump(); /* NX page -> #PF -> SIGSEGV */
            xputs("nx.nofault\n");
            kexit(9); /* not reached */
        }
        int st = 0;
        long w = kwaitpid(pid, &st, 0);
        say("nx.waitpid", w);
        say("nx.status", st); /* must be -11 */
        kbrk(base);           /* give the heap page back */
    }

    /* ---- CR0.WP / W^X: a read() into a read-only segment must be refused
     * copy_to_user() now checks the destination's R/W bit instead of
     * storing through it: under CR0.WP that store would #PF in ring 0, and
     * before WP it silently overwrote the process's own segments.  The
     * buffer is const -> .rodata -> mapped read-only by the ELF loader. */
    {
        static const volatile char rodst[8] = {'A', 'A', 'A', 'A', 'A', 'A', 'A', 0};
        long fd = kopen("/README.md", 0);
        say("read.ro.fd", fd);
        say("read.ro.ret", fd >= 0 ? kread(fd, (void *)rodst, 4) : -100);
        say("read.ro.untouched", rodst[0] == 'A' && rodst[3] == 'A');
        if (fd >= 0) kclose(fd);
    }

    /* ---- sigaction(SIGSEGV): the #PF path must deliver, not kill ---- */
    {
        long pid = kfork();
        if (pid == 0) {
            struct lnxrm_sigaction act;
            act.sa_handler = (void *)on_segv;
            act.sa_mask = 0;
            act.sa_flags = 0;
            act.sa_reserved = 0;
            ksigaction(SIGSEGV, &act, 0);

            volatile unsigned long bogus = 0x10;
            *(volatile char *)bogus = 1; /* #PF -> handler -> return -> #PF -> kill */
            xputs("segv.nofault\n");
            kexit(9); /* not reached */
        }
        int st = 0;
        long w = kwaitpid(pid, &st, 0);
        say("segv.waitpid", w);
        say("segv.status", st); /* must be -11 */
    }

    /* ---- T-004: brk to the point of failure, then give it all back ----
     * Growing the heap must stop at ENOMEM and unwind everything it had
     * already mapped (brk_cur never moved, so a later shrink would not
     * find those pages), and the kernel must still work afterwards.  The
     * frames come straight back: this block deliberately empties the
     * buddy, so anything left over would break the rest of the run. */
    {
        long base = kbrk(0);
        long cur = base;
        long step = 8L << 20;
        long rounds = 0, last = 0;

        for (;;) {
            last = kbrk(cur + step);
            if (last != cur + step) break; /* -12: out of frames */
            cur = last;
            if (++rounds > 4096) {         /* never spin, even if the box */
                last = -999;               /* somehow has terabytes        */
                break;
            }
        }
        say("brk.oom", last);                       /* -12 ENOMEM */
        say("brk.oom.rounds", rounds);              /* 8 MiB chunks handed out */
        say("brk.shrink", kbrk(base) == base);      /* all of them back */

        /* the machine still has to be able to run a process after that */
        long p = kfork();
        say("brk.after.fork", p);
        if (p == 0) kexit(0);
        if (p > 0) {
            int st = 0;
            kwaitpid(p, &st, 0);
            say("brk.after.status", st);
        }
    }

    /* ---- T-032: brk quota + per-task memory accounting ----
     * Four claims, one per acceptance item on the card:
     *   (a) ps reports exactly the pages brk maps -- 8 MiB in is 8192 KiB
     *       out, and the number returns to where it started on a shrink;
     *   (b) mem_peak remembers the high-water mark after the heap is back
     *       to size, which is how "who ballooned" is answered for a
     *       process that has since tidied up;
     *   (c) a plain user stops at the 64 MiB quota with ENOMEM -- a
     *       ceiling, not the buddy running dry: this box has 256 MiB and
     *       the same loop as root drains all of it (the brk.oom block
     *       above).  The parent's own account must not move;
     *   (d) the dead child's account dies with its address space, so the
     *       next fork gets a slot that starts at the parent's size rather
     *       than at the 64 MiB the previous occupant held.
     * Only root may call kps, so the measuring half runs as root and the
     * child reports what brk itself told it. */
    {
        long peak = -1, m0 = ps_mem_kib(kgetpid(), &peak, 0);
        long base = kbrk(0);

        say("mem.kib.base", m0); /* image + stack + restorer, nothing else */
        /* the T-004 block just drained the buddy and gave it all back: a
         * peak that tracked it must still be showing ~200 MiB */
        say("mem.peak.kib", peak);

        say("mem.grow8m", kbrk(base + (8L << 20)) == base + (8L << 20));
        long m1 = ps_mem_kib(kgetpid(), 0, 0);
        say("mem.delta.kib", m1 - m0); /* exactly 8192: 2048 pages */
        say("mem.shrink", kbrk(base) == base);
        long m2 = ps_mem_kib(kgetpid(), 0, 0);
        say("mem.back", m2 == m0); /* 1 */
        say("mem.peak.holds", peak >= m1 && peak > 4 * m0);

        long cp = kfork();
        if (cp == 0) {
            /* drop to the plain-user tier (one way, caps cleared), then
             * burn heap in 1 MiB steps until the quota refuses.  This is
             * the acceptance case: ENOMEM from a ceiling, still alive,
             * and nowhere near the ~200 MiB it would take to empty the
             * buddy the way root does above. */
            say("quota.drop", ksetuid(1000));
            long cur = kbrk(0), last = 0, rounds = 0;
            for (;;) {
                last = kbrk(cur + (1L << 20));
                if (last != cur + (1L << 20)) break;
                cur = last;
                if (++rounds > 4096) { last = -999; break; }
            }
            say("quota.oom", last);          /* -12 ENOMEM */
            say("quota.rounds", rounds);     /* 63: 1 MiB steps up to 64 MiB */
            say("quota.alive", kbrk(0) > 0); /* 1: kernel and process survive */
            /* a ceiling must still allow the heap to be given back */
            say("quota.shrink", kbrk(cur - 4096) == cur - 4096);
            kexit(0);
        }

        /* a zombie keeps its pages until it is reaped, so the child's
         * final size is readable once it stops running */
        long cm = -1, cpk = -1;
        for (int i = 0; cp > 0 && i < 500 && cm < 0; i++) {
            int st = -1;
            long m = ps_mem_kib(cp, &cpk, &st);
            if (st == LNXRM_TS_ZOMBIE && m >= 0) cm = m;
            else ksleep_ms(10, 0);
        }
        say("quota.mem.kib", cm); /* stopped just under 64 MiB */
        say("quota.mem.peak", cpk);
        say("mem.parent.unchanged", ps_mem_kib(kgetpid(), 0, 0) == m0);
        if (cp > 0) {
            int st = 0;
            say("mem.reap", kwaitpid(cp, &st, 0) > 0);
            say("mem.status", st); /* 0 */
        }

        /* (d) the account went with the address space: the recycled slot
         * reports the parent's size, not the 64 MiB the dead task held */
        long c2 = kfork();
        if (c2 == 0) kexit(0);
        long c2m = -1;
        for (int i = 0; c2 > 0 && i < 500 && c2m < 0; i++) {
            int st = -1;
            long m = ps_mem_kib(c2, 0, &st);
            if (st >= 0) c2m = m;
            else ksleep_ms(10, 0);
        }
        say("mem.slot.clean", c2m == m0); /* 1 */
        if (c2 > 0) {
            int st = 0;
            kwaitpid(c2, &st, 0);
        }
    }

    say("systest.done", 1);

    /* ---- true-colour card: one swatch per named FB_CLR_* colour plus an
     * RGB gradient.  Colours are 0x00RRGGBB (never palette indices), so a
     * screendump of this band must match the table byte for byte. ---- */
    {
        static const u32 sw[32] = {
            0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xc0c0c0,
            0x808080, 0x0000ff, 0x00ff00, 0x00ffff, 0xff0000, 0xff00ff, 0xff8040, 0xffffff,
            0x6699ff, 0x66ff66, 0x66ffff, 0xff6666, 0xff66ff, 0xffff00, 0xff8000, 0xffd700,
            0xff69b4, 0xfa8072, 0x87ceeb, 0xd2b48c, 0xfffff0, 0xdc143c, 0x228b22, 0x66ccff};
        struct lnxrm_fb_info info = {0, 0, 0, 0};
        u32 width = kfb_info(&info) == 0 && info.width ? info.width : 1280;
        for (int i = 0; i < 32; i++) {
            struct lnxrm_fb_rect s;
            s.x = (u32)i * (width / 32);
            s.y = 100;
            s.w = width / 32;
            s.h = 64;
            s.color = sw[i];
            kfb_fill(&s);
        }
        for (u32 i = 0; i < 64; i++) { /* blue -> red ramp, any RGB works */
            struct lnxrm_fb_rect s;
            s.x = i * (width / 64);
            s.y = 164;
            s.w = width / 64;
            s.h = 48;
            s.color = ((u32)(i * 255 / 63) << 16) | (255 - i * 255 / 63);
            kfb_fill(&s);
        }
    }
    return 0;
}
