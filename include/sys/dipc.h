/* dipc_open(53) / dipc_recv(54) / dipc_send(55): driver inter-process
 * communication channels (S1a).
 *
 * WHAT A CHANNEL IS
 *   A name in the kernel's OWN table (never a path) bound to ONE struct
 *   file, plus a byte ring that lives in a shared frame.  Every dipc_open
 *   of that name hands out a reference to the same file, exactly as
 *   shm_open(51) does -- which is the whole trick: mm/filepage.c keys its
 *   shared-frame cache on the struct file POINTER, so two openers hash to
 *   one entry and both mappings land on ONE physical frame.  mm/ is not
 *   touched by this file at all.
 *
 *   The file is deliberately NOT registered in shm's name table: a channel
 *   is a kernel object, not a user-named segment, and leaving it out of
 *   shm_open's namespace means no process can shm_open() its way to the
 *   raw byte stream behind the driver's back.  The descriptor still looks
 *   ordinary to every other VFS door (a synthesised V_REG vnode behind
 *   file->priv, never `drv`), so sys_mmap's "!priv / type != V_REG" gate
 *   opens for it and the demand pager works untouched.
 *
 * WHO OWNS THE BYTES
 *   The ring is one order-0 frame reached through filepage_get(f, 0): the
 *   first caller allocates and seeds it, later callers bump the refcount,
 *   and there is NO eviction (mm/filepage.h), so the kernel's own ring
 *   pointer stays valid for the channel's whole life.  The channel holds
 *   its OWN reference on that frame -- dropped by dipc_close_node, which
 *   runs behind vfs_close_file's refcount gate -- and that is what keeps
 *   the bytes alive after every name, fd and VMA has gone.
 *
 * THE FRAME'S LAYOUT (one page, shared between ring 0 and ring 3)
 *
 *      0                 8                                    4096
 *      +-----------------+------------------------------------+
 *      | struct dipc_hdr | payload: LNXRM_DIPC_PAY_SIZE bytes |
 *      |   rd, wr        | the byte ring itself               |
 *      +-----------------+------------------------------------+
 *
 *   The indices live IN the frame, not in struct dipc, so a consumer that
 *   maps the page can read how much is waiting without a syscall -- that is
 *   the point of putting the ring in shared memory at all.  They are
 *   written by the kernel only; a ring3 writer that scribbles on them gets
 *   ring_check()'s reset rather than an out-of-frame index.
 *
 * FLOW CONTROL
 *   One producer (the IRQ side) and one consumer (the ring3 driver) push
 *   and pop a single byte stream; full is `ring_next(wr) == rd`, capacity
 *   LNXRM_DIPC_CAPACITY.  Pushing into a full ring drops the byte and
 *   counts it in `dropped`: an IRQ handler cannot block, and losing a
 *   keystroke under flood beats stalling interrupt delivery.
 *
 * THE DOORBELL (this half is what makes it an IPC)
 *   A consumer finding the ring empty parks on `waiters`, a push-front
 *   singly linked list threaded through task->wait_next -- the same field
 *   pipe's rwait/wwait and input_event's evq list use, which is safe
 *   because a task parks in exactly ONE place at a time.  The list is
 *   guarded by dipc_lock; every wake happens OUTSIDE that lock, the rule
 *   irq/isr.c and drivers/input_event.c already follow.
 *
 * THE REVERSE DIRECTION
 *   dipc_send does not use the ring at all: it copies the caller's bytes
 *   into a kernel buffer and calls the channel's sink (installed at create
 *   time, never changed).  A decoded keystroke or mouse packet is under 32
 *   bytes and travels once per event, so a second ring and its pump would
 *   cost more than the copy it saves.  S1c (2026-11-19) is exactly this
 *   door: the "kbd" and "mouse" channels install sinks so the ring3
 *   module's decoded result can travel back into the kernel's input
 *   queues, while the ring carries raw bytes the other way.
 *
 * CAPABILITY.  All three calls sit behind GATE_ADMIN in the dispatch
 *   table: enumerating or speaking to a driver channel is administrative,
 *   exactly like the raw key bytes and the event queue.  A root process
 *   (init, and anything it forks) passes; a plain user answers -EACCES.
 */
#pragma once
#include <types.h>
#include <sys/spinlock.h>

#ifdef __cplusplus
extern "C" {
#endif

struct file;
struct task;

#define LNXRM_DIPC_NAME_MAX 32  /* canonical name bound: 31 bytes + NUL */
#define LNXRM_DIPC_HDR_SIZE 8   /* sizeof(struct dipc_hdr) */
#define LNXRM_DIPC_PAY_SIZE (LNXRM_DIPC_RING_SIZE - LNXRM_DIPC_HDR_SIZE)
#define LNXRM_DIPC_CAPACITY (LNXRM_DIPC_PAY_SIZE - 1) /* one slot says "full" */
#define LNXRM_DIPC_RECV_MAX 4096 /* one dipc_recv may move at most this */
#define LNXRM_DIPC_SEND_MAX 1024 /* ... and one dipc_send/sink call */
#define LNXRM_DIPC_MAX 8        /* channels the table will hold */

/* The frame's front: both indices are payload offsets in [0, PAY_SIZE).
 * Full is ring_next(wr) == rd, which is why the ring holds one byte less
 * than the payload does. */
struct dipc_hdr {
    u32 rd, wr;
};

/* One channel.  Allocated by dipc_create, freed exactly once by
 * dipc_close_node (behind vfs_close_file's refcount gate, the way
 * shm_ent is), and reached by dipc_find under dipc_lock. */
struct dipc {
    char name[LNXRM_DIPC_NAME_MAX];
    bool linked;                    /* still in the table? cleared before
                                     * the entry's reference is dropped */
    struct file *file;              /* THE file: every opener shares it */
    u8 *ring;                       /* PHYS_TO_VIRT(ring_pa): the frame */
    u64 ring_pa;                    /* frame the ring lives in */
    u64 dropped;                    /* bytes dropped into a full ring */
    long (*sink)(const u8 *buf, u32 len); /* reverse path, set at create */
    struct task *waiters;           /* push-front list via task->wait_next */
    struct dipc *next;
};

/* The table lock.  It guards the name table, every channel's ring indices
 * and every channel's wait list -- one lock, because taking two would mean
 * an ordering nobody has proven.  It is irqsave: dipc_push runs from IRQ1. */
extern spinlock_t dipc_lock;

/* ---- kernel-internal API ---- */

/* Bind `name` to a fresh channel whose sink is `sink` (NULL for a one-way
 * channel).  Answers 0, -EEXIST if the name is taken, -EINVAL for an empty
 * name or one containing '/', -ENAMETOOLONG past 31 bytes, -ENOSPC when
 * LNXRM_DIPC_MAX channels already exist. */
int dipc_create(const char *name, long (*sink)(const u8 *, u32));

/* Take `name` out of the table and drop the ENTRY's reference on its file.
 * Every descriptor that is still open keeps working -- the name is what
 * goes, not the channel -- and close_node frees the rest when the last one
 * closes.  Answers 0 or -ENOENT.  Never called with dipc_lock held. */
int dipc_destroy(const char *name);

/* The channel called `name`, or NULL.  THE CALLER MUST HOLD dipc_lock: the
 * pointer is valid only while it does, since destroy/close_node free the
 * entry the moment the lock lets them in. */
struct dipc *dipc_find(const char *name);

/* The producer's door (IRQ-safe, never parks): append up to `len` bytes to
 * `name`'s ring and wake every waiter outside the lock.  Answers how many
 * bytes were stored; a full ring stores none and counts the rest in
 * dropped.  Answers 0 when no channel of that name exists. */
u32 dipc_push(const char *name, const u8 *buf, u32 len);

/* The producer's door for a byte the RING3 reader owns (S1c): store it
 * only while some descriptor other than the entry's own holds the channel
 * open -- the file's refcnt is that one reference plus one per dipc_open
 * -- and answer whether the byte went to the ring.  False means "no
 * reader, or no channel" and stores nothing: the caller decodes in ring0
 * itself, which is what keeps a boot with no .km module on the image a
 * working boot.  The module's exit closes its descriptor and hands the
 * decode straight back.  IRQ-safe, never parks, same wake rule as
 * dipc_push. */
bool dipc_push_if_listening(const char *name, const u8 *buf, u32 len);

/* The channel behind `fd`, or NULL -- NULL covers "no such slot", "an empty
 * slot" and "a descriptor that is not a channel".  Takes no lock: the
 * caller's own fd holds a reference on the file, and close_node (the only
 * thing that frees the entry) cannot run until that reference is gone. */
struct dipc *dipc_from_fd(int fd);

/* ---- syscall bodies (kernel/core/syscall.c only copies the name) ---- */
long sys_dipc_open(const char *name, u64 flags);
long sys_dipc_recv(int fd, void *ubuf, u64 len, u64 flags);
long sys_dipc_send(int fd, const void *ubuf, u64 len, u64 flags);

#ifdef CONFIG_KTEST
/* Live channels, bytes waiting in `name`'s ring, and `name`'s dropped
 * count -- the three numbers the lifecycle and flood cases pin. */
u32 dipc_live_objects(void);
u32 dipc_pending(const char *name);
u64 dipc_dropped(const char *name);
#endif

#ifdef __cplusplus
}
#endif
