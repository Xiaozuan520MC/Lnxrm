#pragma once

#include <stddef.h>
#include <stdint.h>

enum lnxrm_errno {
    LNXRM_EFAIL = -1,
    LNXRM_ENOENT = -2,
    LNXRM_ESRCH = -3,
    LNXRM_EINTR = -4,
    LNXRM_EIO = -5,
    LNXRM_ENOEXEC = -8,
    LNXRM_EBADF = -9,
    LNXRM_ECHILD = -10,
    LNXRM_EAGAIN = -11,
    LNXRM_ENOMEM = -12,
    LNXRM_EACCES = -13,
    LNXRM_EFAULT = -14,
    LNXRM_EEXIST = -17,
    LNXRM_EXDEV = -18,
    LNXRM_ENOTDIR = -20,
    LNXRM_EINVAL = -22,
    LNXRM_EMFILE = -24,
    LNXRM_ENOSPC = -28,
    LNXRM_ENAMETOOLONG = -36,
    LNXRM_ENOSYS = -38,
    LNXRM_ENOTEMPTY = -39
};

#if defined(__cplusplus)
#define LNXRM_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define LNXRM_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* Authoritative syscall numbering.  Entries are contiguous 0..LAST with no
 * holes; anything not named here falls into the dispatcher's `default` arm
 * (unknown syscall -> LNXRM_ENOSYS). */
enum lnxrm_syscall_number {
#define LNXRM_SYS(name, nr) SYS_##name = nr,
#include "lnxrm_syscalls.def"
#undef LNXRM_SYS
};

#define LNXRM_SYSCALL_FIRST 0
#define LNXRM_SYSCALL_LAST  31

/* Compile-time guard: every number in the .def must fit in [FIRST, LAST].
 * `1 / (cond)` is a constant expression, so a stale LNXRM_SYSCALL_LAST
 * (for example after appending a call without bumping the macro) fails
 * the build instead of silently accepting an out-of-range number. */
enum {
#define LNXRM_SYS(name, nr) LNXRM_NR_IN_RANGE_##name = 1 / ((nr) <= LNXRM_SYSCALL_LAST),
#include "lnxrm_syscalls.def"
#undef LNXRM_SYS
};

/* ---- signals (shared by the kernel's signal code and user handlers) ---- */
#define LNXRM_SIGHUP  1
#define LNXRM_SIGINT  2
#define LNXRM_SIGQUIT 3
#define LNXRM_SIGILL  4
#define LNXRM_SIGTRAP 5
#define LNXRM_SIGABRT 6
#define LNXRM_SIGBUS  7
#define LNXRM_SIGFPE  8
#define LNXRM_SIGKILL 9
#define LNXRM_SIGUSR1 10
#define LNXRM_SIGSEGV 11
#define LNXRM_SIGUSR2 12
#define LNXRM_SIGPIPE 13
#define LNXRM_SIGALRM 14
#define LNXRM_SIGTERM 15
#define LNXRM_SIGSTKFLT 16
#define LNXRM_SIGCHLD 17
#define LNXRM_SIGCONT 18
#define LNXRM_SIGSTOP 19
#define LNXRM_SIGTSTP 20
#define LNXRM_SIGTTIN 21
#define LNXRM_SIGTTOU 22
#define LNXRM_SIGURG  23
#define LNXRM_SIGXCPU 24
#define LNXRM_SIGXFSZ 25
#define LNXRM_SIGVTALRM 26
#define LNXRM_SIGPROF 27
#define LNXRM_SIGWINCH 28

#define LNXRM_NR_SIGNALS 32

/* Unwrapped names: identical to the kernel's, so both sides agree by
 * construction instead of by hand-maintained duplicates. */
#define SIGINT  LNXRM_SIGINT
#define SIGQUIT LNXRM_SIGQUIT
#define SIGILL  LNXRM_SIGILL
#define SIGABRT LNXRM_SIGABRT
#define SIGBUS  LNXRM_SIGBUS
#define SIGFPE  LNXRM_SIGFPE
#define SIGKILL LNXRM_SIGKILL
#define SIGUSR1 LNXRM_SIGUSR1
#define SIGSEGV LNXRM_SIGSEGV
#define SIGTERM LNXRM_SIGTERM
#define SIGCHLD LNXRM_SIGCHLD
#define SIGCONT LNXRM_SIGCONT
#define SIGSTOP LNXRM_SIGSTOP

/* sa_handler values other than a real code address. */
#define LNXRM_SIG_DFL ((void *)0)
#define LNXRM_SIG_IGN ((void *)1)
#define SIG_DFL LNXRM_SIG_DFL
#define SIG_IGN LNXRM_SIG_IGN

/* sigprocmask `how` */
#define LNXRM_SIG_BLOCK   0
#define LNXRM_SIG_UNBLOCK 1
#define LNXRM_SIG_SETMASK 2

struct lnxrm_sigaction {
    void *sa_handler;  /* LNXRM_SIG_DFL / LNXRM_SIG_IGN / handler address */
    uint64_t sa_mask;  /* signals blocked while the handler runs */
    int32_t sa_flags;  /* reserved, must be 0 */
    int32_t sa_reserved;
};

LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_sigaction) == 24, "sigaction ABI");

/* lseek */
#define LNXRM_SEEK_SET 0
#define LNXRM_SEEK_CUR 1
#define LNXRM_SEEK_END 2

/* nanosleep's optional remainder output */
struct lnxrm_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_timespec) == 16, "timespec ABI");

/* uname */
struct lnxrm_utsname {
    char sysname[64];
    char nodename[64];
    char release[64];
    char version[64];
    char machine[64];
};

LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_utsname) == 320, "utsname ABI");

/* framebuffer syscalls (24-28).  Every colour field (color/fg/bg) is a
 * true-colour 0x00RRGGBB value -- 24-bit RGB carried in 32 bits, never a
 * palette index. */
struct lnxrm_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
};

struct lnxrm_fb_rect {
    uint32_t x, y, w, h, color;
};

struct lnxrm_fb_char {
    uint32_t x, y, ch, fg, bg;
};

struct lnxrm_fb_str {
    uint32_t x, y, fg, bg;
    const char *str;
};

LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_fb_info) == 16, "fb_info ABI");
LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_fb_rect) == 20, "fb_rect ABI");
LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_fb_char) == 20, "fb_char ABI");
LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_fb_str) == 24, "fb_str ABI");

/* ps: one fixed-size record per task, written into the caller's buffer. */
enum lnxrm_task_state {
    LNXRM_TS_UNUSED = 0,
    LNXRM_TS_EMBRYO,
    LNXRM_TS_RUNNABLE,
    LNXRM_TS_RUNNING,
    LNXRM_TS_SLEEPING,
    LNXRM_TS_ZOMBIE,
    LNXRM_TS_STOPPED
};

/* Task privilege tier (T-030), ordered by power: user < root < kxld.
 * The kernel's enum cred_kind aliases these values one for one. */
enum lnxrm_cred_kind {
    LNXRM_CRED_USER = 0,
    LNXRM_CRED_ROOT = 1,
    LNXRM_CRED_KXLD = 2
};

struct lnxrm_ps_entry {
    int32_t pid;
    int32_t ppid;
    int32_t cpu;   /* -1 when the task is not running anywhere */
    int32_t state; /* enum lnxrm_task_state */
    char name[16];
    int32_t kind; /* enum lnxrm_cred_kind (T-030) */
    int32_t uid;
    /* T-032: what the task's user space costs right now, and the largest
     * it has ever been -- both in KiB of 4 KiB pages the kernel mapped
     * for that task.  `mem_kib` answers "who owns the RAM"; the pair
     * answers "who ballooned and shrank again".  0/0 means the task owns
     * no user pages (idle, or a slot waiting to be reused). */
    int32_t mem_kib;
    int32_t peak_kib;
};

LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_ps_entry) == 48, "ps ABI");

struct lnxrm_dirent {
    uint64_t d_ino; /* filesystem object id; 0 only if the fs has none */
    uint8_t d_type;
    char d_name[56];
};

LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_dirent) == 72, "dirent ABI");
LNXRM_STATIC_ASSERT(offsetof(struct lnxrm_dirent, d_type) == 8, "dirent ABI");
LNXRM_STATIC_ASSERT(offsetof(struct lnxrm_dirent, d_name) == 9, "dirent ABI");
