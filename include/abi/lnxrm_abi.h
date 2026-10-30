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
#define LNXRM_SYSCALL_LAST  40

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

/* framebuffer syscalls: fb_info 23 and the drawing calls 24-27, plus
 * fb_flip 32 (double buffering: publish the back surface).  Every colour
 * field (color/fg/bg) is a true-colour 0x00RRGGBB value -- 24-bit RGB
 * carried in 32 bits, never a palette index. */
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

/* kbd_info (36): the keyboard/input-stream counters a keyboard probe
 * wants.  capacity is the ring's usable size (255: full at in_w+1==in_r),
 * pending is what a kbd_get would answer, pushed/dropped are the ring's
 * acceptance bookkeeping, and scans counts every scancode the PS/2
 * handler has seen, make or break -- proof the IRQ1 line is alive. */
struct lnxrm_kbd_info {
    uint32_t pending;       /* keys buffered right now */
    uint32_t capacity;      /* ring capacity in keys */
    uint32_t pushed_total;  /* keys ever accepted into the ring */
    uint32_t dropped;       /* keys lost to a full ring */
    uint32_t ps2_scans;     /* raw scancodes seen on IRQ1 (in+out) */
    uint32_t reserved[3];   /* room to grow without breaking the ABI */
};

LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_kbd_info) == 32, "kbd_info ABI");

/* mouse_get (37): one decoded PS/2 packet.  Deltas are screen-space
 * (positive x = right, positive y = down): the stream's 9-bit signed
 * values, with y negated at the exit -- PS/2 wires positive y as *up*
 * (overflow packets push 0, never garbage).  Buttons are bit0 left,
 * bit1 middle, bit2 right.  Wheel is the IntelliMouse 4-byte mode's
 * scroll delta (-8..7, positive = scroll down), 0 in 3-byte mode. */
struct lnxrm_mouse_state {
    int32_t dx;
    int32_t dy;
    uint32_t buttons;
    int32_t wheel;
};

LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_mouse_state) == 16, "mouse ABI");

/* mouse_info (38): the aux-port counters a pointer probe wants.  Mirrors
 * lnxrm_kbd_info: pending is what a mouse_get would answer, capacity the
 * ring's usable size, and the rest is monotonic bookkeeping -- `inited`
 * says ps2_mouse_init() saw a real reset/ack probe (headless QEMU and
 * mouse-less machines answer 0). */
struct lnxrm_mouse_info {
    uint32_t pending;        /* events buffered right now */
    uint32_t capacity;       /* ring capacity in events */
    uint32_t packets_total;  /* events ever accepted into the ring */
    uint32_t moved_total;    /* events with a nonzero delta */
    uint32_t resyncs;        /* packets lost to a corrupt stream */
    uint32_t dropped;        /* events lost to a full ring */
    uint32_t inited;         /* aux reset probe succeeded */
    uint32_t reserved[1];
};

LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_mouse_info) == 32, "mouse_info ABI");

/* ---- unified input event queue (39-40) ----
 * One stream a compositor can read instead of per-device calls: the
 * keyboard decode path (real IRQ1 scancodes only -- kbd_inject 35 types
 * on the console the old way and deliberately never enters this queue),
 * the relative mouse deltas, button transitions and the wheel.  24 bytes,
 * no pointers, copied whole across every syscall.
 *
 * Per field:
 *  - seq: the queue's event counter, first event 1, strictly increasing
 *    for every accepted event (a REL+BTN batch hands out consecutive
 *    numbers in emission order).
 *  - type: INPUT_EV_*, never 0 -- a zeroed struct always means "no
 *    event", which a caller that memsets its receiver can trust.
 *  - a/b: the payload (KEY: the decoded character; REL: screen-space
 *    dx,dy; BTN: the button mask after the change; WHEEL: signed scroll).
 *  - flags: INPUT_MOD_* for KEY -- why the map produced this character.
 *    The character is already transformed; these are the recipe, for
 *    programs that act on the transformation itself (a text editor's
 *    cursor word-motions, a shell's history).  0 for the other types.
 *  - resv: zero forever, room to grow without breaking the ABI. */
enum input_event_type {
    INPUT_EV_NONE  = 0, /* zeroed struct: no event */
    INPUT_EV_KEY   = 1, /* a is the decoded character */
    INPUT_EV_REL   = 2, /* a,b: dx,dy, screen-space (x right, y down) */
    INPUT_EV_BTN   = 3, /* a: the button mask, bit0 left bit1 middle
                         * bit2 right -- the PS/2 flags byte's own f&7 */
    INPUT_EV_WHEEL = 4, /* a: signed scroll delta, positive = down */
};

#define INPUT_MOD_SHIFT 0x1u /* shift was held when the key decoded */
#define INPUT_MOD_CAPS  0x2u /* capslock was on -- why the letter is upper */
#define INPUT_MOD_MASK  0x3u /* everything a producer may ever set */

struct lnxrm_input_event {
    uint32_t seq;
    uint32_t type;
    int32_t a;
    int32_t b;
    uint32_t flags;
    uint32_t resv;
};

LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_input_event) == 24, "input event ABI");

/* input_info (40): the unified queue's counters, the same shape and the
 * same honesty as kbd_info (36) and mouse_info (38): capacity/pending
 * describe the 63 slots, pushed_total/dropped the acceptance bookkeeping,
 * and keys/rel/btn/wheel count every accepted event per type -- so their
 * sum is exactly pushed_total. */
struct lnxrm_input_info {
    uint32_t capacity;
    uint32_t pending;
    uint32_t pushed_total;
    uint32_t dropped;
    uint32_t keys;
    uint32_t rel;
    uint32_t btn;
    uint32_t wheel;
};

LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_input_info) == 32, "input_info ABI");

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
