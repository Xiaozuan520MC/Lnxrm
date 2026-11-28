# lnxrm 代码库组织与子系统说明（5：系统调用与 ABI）

> 配套文件：`CODEBASE_OVERVIEW.md`（§1 总览）、`CODEBASE_BOOT.md`（§2 启动）、`CODEBASE_MEMORY.md`（§3 内存）、`CODEBASE_PROC.md`（§4 进程·调度·信号）、`CODEBASE_FS.md`（§6 文件系统）、`CODEBASE_DRIVERS.md`（§7 驱动）、`CODEBASE_SMP.md`（§8 SMP 与 ACPI）、`CODEBASE_SELFTEST.md`（§9 自测）、`CODEBASE_INVARIANTS.md`（§10 关键不变式）。构建与运行见 `../BUILD/BUILD.md`，差距与计划见 `../GAP_ANALYSIS.md`，项目概览见 `../README.md`。本文为 §5 系统调用与 ABI。

- **两道门**（S3.2 起）：`syscall`（`EFER.SCE` → `LSTAR` = `entry_SYSCALL_64`，**用户态实际在用的那道**）与兼容门 `int 0x80`（IDT 128，DPL=3，`isr128` 原样保留）；两道门压出**同一份 `intr_frame`**，所以下面的分发层不知道自己从哪道门进来，**只有最末端的出口指令不同**（S3.3 起：`syscall` 走 `sysretq`、`int 0x80` 仍走 `iretq`）。编号 `rax`，参数 `rdi/rsi/rdx/r10/r8/r9`（Linux 六寄存器约定），返回 `rax`；`syscall_do` 查**一张 `sysents[]` 表** `{入口, 门, 门名}`——门与入口同一行，未知编号回 `-38` 且**先不问门**。
- **只追加不重排**：唯一定义源 `include/abi/lnxrm_syscalls.def`（X-macro），`lnxrm_abi.h` 逐编号编译期校验，`LNXRM_SYSCALL_LAST` 必须随末尾加一。
- 门：`GATE_ADMIN` 11 个 / `GATE_FB` 5 个，拒绝回 `EACCES=-13` 并打日志；**先能力门、后 `fb_owner_gate` 归属门**（顺序是硬的），`fb_info` 两道门都不过。走字符串的经 `copy_user_str`，走指针的调用方自校验——**没有统一前置检查**。
- **swapgs 与 TLS（S3.4 起）**：**入口** `entry_SYSCALL_64` 与每个 ISR 桩（`ISR_NOERR`/`ISR_ERR`、`isr128`、`isr240-242`）进栈即 `SWAPGS_IN`（由帧里的 `cs & 3` 判：来自 ring 3 才换，来自 ring 0 不换），出口在 `POPALL` 之后、`iretq`/`sysretq` 之前再 `SWAPGS_OUT` 还回 ring-3 的 GS——没有这个配对，中断/系统调用在 ring-3 到达时 `this_cpu_data()`（`rdmsr(IA32_GS_BASE)`）就会读成用户地址。**TLS 段选 FS**：x86-64 System V 的 `__thread` 降到 `%fs:`，`arch_prctl(56)` 的 `ARCH_SET_FS/ARCH_GET_FS` 直接写/读 `IA32_FS_BASE`；`ARCH_SET_GS/ARCH_GET_GS` 写/读 `IA32_KERNEL_GS_BASE`（swapgs 的影子段，不是 `IA32_GS_BASE`），两段都被每任务私有字段 `task.user_fs`/`task.user_gs` 缓存，`switch_to()` 进栈前存旧/装新。四种操作各自地址门限到 `USER_MAX_VMA`（拒内核/非规范），未知码回 `EINVAL`。`fork` 把两段一起拷给子、`execve` 两段都清 0（新镜像不该继承旧镜像的线程指针），`glue_first` 首次进 ring 3 时把该任务的 GS 安上。
- ABI 结构体（`lnxrm_abi.h`）：23 个 errno、信号号、`lnxrm_timespec` 16B / `utsname` 320B / `ps_entry` 56B（`name[24]`，2026-11-18 由 48B/16B 扩：`/mod/keyboard.km` 是 16 字符，16 字节的 `name` 连 NUL 都放不下，`ps` 里就成了 `/mod/keyboard.k`；2026-11-19 实测 `ps` 完整显示两个 `.km` 全名；同日改名加 `ps2-` 前缀后 `/mod/ps2-keyboard.km` 20 字符仍在 24 内）/ `dirent` 72B / `fb_info` 16B，`_Static_assert` 锁布局；**ABI 一改用户态必须全量重编**。

### 58 项总表（编号 0–57，连续无空洞）

| # | 名字 | 要点 | 实现 |
|---|---|---|---|
| 0–4 | `read/write/open/close/lseek` | fd 读写；`open` 七标志；close 即同步脏扇区 | `fs/vfs.c` |
| 5 | `brk` | 用户堆 + 配额算术门 | `core/syscall.c` |
| 6–7 | `getdent/dup2` | 目录 cookie 迭代；复制 fd | `fs/vfs.c` |
| 8–9 | `nanosleep/getpid` | 信号打断回 EINTR + 余量；进程号 | `sched/sched.c`+`core/syscall.c` |
| 10–13 | `fork/execve/exit/wait4` | COW fork / 静态 ELF / 释放画屏 / WNOHANG | `sched/task.c` |
| 14–15 | `kill/uname` | kxld / 同 uid / `CAP_KILL` 三道闸；系统标识 | `core/syscall.c` |
| 16–17 | `sigaction/sigprocmask` | 处理器 / BLOCK·UNBLOCK·SETMASK | `sched/signal.c` |
| 18–21 | `getppid/ps/sigreturn/diskinfo` | 无父为 0；内核填任务表（`CAP_SYS_ADMIN`）；恢复内核保存的帧；枚举块设备（`CAP_SYS_ADMIN`） | `core/syscall.c`+`sched/task.c`+`sched/signal.c` |
| 22–23 | `mkdir/fb_info` | 建目录；帧缓冲几何（两道门都不过） | `fs/vfs.c`→根 fs（`ext2/ext2.c`，FAT 卷则 `fat32/fat32_meta.c`）+`core/syscall.c` |
| 24–27 | `fb_clear/fill/char/puts` | 绘图，`CAP_FB`+归属门 | `drivers/framebuffer.c` |
| 28–31 | `unlink/rmdir/rename/setuid` | 删文件/删空目录/改名；`setuid` 单向降权 | `fs/vfs.c`→根 fs（`ext2/ext2.c`，FAT 卷则 `fat32/fat32_meta.c`）+`core/protect.c` |
| 32–33 | `fb_flip/sync` | 离屏后缓冲推前台；脏扇区交设备 | `drivers/framebuffer.c`+`fs/vfs.c` |
| 34–36 | `kbd_get/inject/info` | 弹键/代打/快照；get·inject 带 `CAP_SYS_ADMIN`。**S1c 起 IRQ1 在通道有 opener 时把原始字节交给 `dipc` 通道 `"kbd"`（`dipc_push_if_listening`）、无 opener 本地解码，34/36 契约不变** | `irq/isr.c`+`core/syscall.c`+`drivers/ps2-keyboard.c` |
| 37–38 | `mouse_get/info` | 弹指针事件/aux 环快照；get `CAP_SYS_ADMIN`。**S1c 起 IRQ12 在通道有 opener 时把原始字节交给 `dipc` 通道 `"mouse"`、无 opener 本地 `ms_feed_byte`，37/38 契约不变** | `core/syscall.c`+`ps2-mouse.c` |
| 39–41 | `input_pop/info/wait` | 统一队列 24B 事件；pop·wait `CAP_SYS_ADMIN`，wait 阻塞+EINTR+ms 超时 | `drivers/input_event.c` |
| 42–44 | `mmap/munmap/mprotect` | mmap **六参** `addr,len,prot,flags,fd,offset`（4–6 参读 trap frame `r10/r8/r9`，S3.2 起），addr=0、top-down、**只登记 VMA**，`fd<0` 匿名、shared 走共享页缓存；munmap 挖洞拆分 + 逐页 unmap，零相交 `EINVAL`；mprotect 任意范围，洞 `ENOMEM`、未对齐 `EINVAL`，**授予 W 不分配** | `core/syscall.c`+`mm/vma.c`+`mm/filepage.c` |
| 45–46 | `stat/gettimeofday` | 56 字节路径元数据（`st_dev` 槽从 1 起，`ino` 与 getdent 同号）；`tv` 微秒（CMOS 一次 + TSC 演进，单调）、`tz` 恒 0 | `core/syscall.c`+`fs/vfs.c`+`core/rtc.c` |
| 47 | `pipe` | 单向字节流：4096 B 环 + 等待队列（空环读者/满环写者 park，close 末端产 EOF/EPIPE）；`file->drv` 存 `struct pipe *`，两端各一个 `file_ops` | `fs/pipe.c`+`fs/vfs.c` |
| 48–50 | `reboot/chdir/getcwd` | 重启（`GATE_ADMIN`）；改 `task.cwd[128]`（≤127 字节，超 `ENAMETOOLONG`）即相对路径基准；把 cwd 拷回用户缓冲，装不下 `ERANGE` | `core/syscall.c`（48）+`fs/vfs.c`（49/50） |
| 51–52 | `shm_open/shm_unlink` | **命名共享内存对象**：名字表项持唯一一个 `struct file`，所有 opener 同指针 ⇒ 共享页缓存天然一帧、MM 层零改动；entry 引用只由 unlink 放，`close_node` 恒在其后无条件 `kfree`（内容跨 close 存活）；mode 固定 `O_RDWR`、无 gate、`<32` 字节名 / 1 GiB / 64 对象 | `fs/shm.c`（+`include/sys/shm.h`） |
| 53–55 | `dipc_open/dipc_recv/dipc_send` | **驱动 IPC 通道**（S1a；环结构/park 协议/生命周期见 `CODEBASE_FS.md` §6）：名字表 + 唯一 `struct file`（同 shm 的「所有 opener 同指针 ⇒ 一帧」），**不进 shm 名字表**故 `shm_open` 摸不到原始字节流；recv 空则 park 到通道 `waiters`（前插 `task->wait_next`，与 pipe/input_event 同字段），**wake 恒锁外**、醒后 unlink + 恢复 `T_RUNNING`；send 不走环、拷进内核缓冲锁外调 create 时装的 sink。三行全 `GATE_ADMIN`；open flags 必 0、recv `NONBLOCK`→`EAGAIN`、`len∈[1,4096]`、send `len∈[1,1024]`、坏目的 `EFAULT` **不消耗数据**、满环丢字节计入 `dropped` | `ipc/dipc.c`（+`include/sys/dipc.h`） |
| 57 | `shutdown` | **停机 / 断电 / 重启**（S4.0）：`shutdown(cmd)`，cmd 取 `LNXRM_RB_HALT`（0，`cli;hlt` 永停）、`LNXRM_RB_REBOOT`（1，8042/0xCF9 软复位）、`LNXRM_RB_POWER_OFF`（2，向 FADT 披露的 ACPI PM1a_CNT 端口写 SLP_TYP=5\|SLP_EN，QEMU 会优雅退出）；`sys_sync()` 先落盘，成功路径不返回。`GATE_ADMIN`；未知命令 `EINVAL`。`/bin/reboot`、`/bin/poweroff`、`/bin/halt` 三个用户命令分别对应三种命令。 | `core/syscall.c`+`irq/acpi.c`（PM1a_CNT 端口） |

统一输入队列（39–41）把两生产者的已解码结果合成一条流：`KEY` 只出自 IRQ1 解码（`kbd_inject` 不进队列），`REL/BTN/WHEEL` 出自 aux 包尾；34–38 的环与门原样保留。**S1b 额外开的是一条并行的原始字节流**（`dipc_push("kbd")`），与这两套队列互不改道。
