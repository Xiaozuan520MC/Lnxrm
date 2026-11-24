# lnxrm 代码库组织与子系统说明

面向要改这套代码的开发者。构建与运行见 BUILD.md，差距与计划见 GAP_ANALYSIS.md，项目概览见 README.md。本文为上篇（§1–§5）；**§6–§10 见 `CODEBASE_SUBSYSTEMS.md`**（2026-11-24 拆分）。

x86-64 freestanding 单体内核：C 为主体，启动/中断/上下文切换用 NASM，产出 Linux boot protocol 2.08 的 `bzImage`，**ext2 系统盘挂根**（S2 起；FAT32 驱动仍在，只是不再当根）并跑自带 shell。**只有 C 与 ASM**，构建零警告。

## 1. 总览

| 产物 | 说明 |
|---|---|
| `build/bzImage` | `setup.bin` + `vmlinux.bin`，两个 Python 脚本回填 boot header |
| `build/vmlinux.elf/.bin` | 链接产物（`arch/kernel.ld`，单一 `.image` 段 VMA `0xffffffff80100000` / LMA `0x100000`）/ 裸 payload |
| `build/disk.img` | 64 MiB ext2 系统盘（S2 起：`mke2fs` + `debugfs` 造，装 `/README.md`、`/bin`、`/mod`；4 KiB 块 × 16384、16384 个 256 B inode、单块组、**无 journal**） |
| `build/user/<name>` | 16 个静态 ELF64 用户程序 |
| `build/mod/<name>.km` | ring3 驱动模块 ELF（**`.km` 由 `MOD_ELFS` 在链接期加上**，镜像 `/mod` 里同名），现为 `ps2-keyboard.km`、`ps2-mouse.km` 两个（2026-11-19 加 `ps2-` 前缀） |
| `build/lnxrm.iso` | GRUB 引导 ISO（仅 bootloader + 内核，无根文件系统） |
| `build/obj0/`、`build/obj1/` | 两种 `KTEST` 模式各自的对象与 `.d`（两模式并存，切换只重链 ~0.2 s、零重编；`trampoline.bin` 仍留 `build/` 根，符号名由路径导出） |

补丁脚本是构建链关键：`patch_setup_jmp.py` 回填 `setup.bin` 哨兵 `0xF00DFACE` 的真实跳转，`patch_bzimage.py` 填 boot header 并断言 `HdrS`/`0x55AA`/payload 对齐（详见 BUILD.md）。

```
Makefile               构建入口（源文件自动发现 + 镜像/运行/冒烟）；用户态委托 userland/Makefile；工具链 clang → ld.lld → llvm-objcopy（binutils 留给 `nm` 与 smoke 检查）；对象按 KTEST 分目录 build/obj<N>/，模式标记 .mode-<N> 挂链接侧
arch/                  setup.asm(实模式) entry64.S(_start32/64、中断桩、swtch) trampoline.S(AP 入口,拷 0x8000)
                       cpu.c(GDT/TSS/IDT/PIT/LAPIC) kernel.ld(段边界符号 __text_*/__rodata_*/__data_* 供 W^X)
include/               全部头文件：abi/(lnxrm_abi.h + lnxrm_syscalls.def + keyboard.h + mouse.h) sys/ mm/ 其余
kernel/                main.c(start_kernel 编排)
  core/                syscall.c(分发/门) print.c klog.c(环+loglevel) backtrace.c percpu.c smp.c protect.c selftest.c rtc.c
  sched/               task.c(fork/execve/exit/wait4) sched.c(RR/锁) elf.c signal.c
  irq/                 isr.c(分发/异常/IPI) apic.c acpi.c
  mm/                  pmm.c(buddy+帧引用计数) kheap.c vmm.c(三槽地址空间+W^X) vma.c pagefault.c
                       filepage.c(文件映射共享页缓存) oom.c(OOM killer)
  fs/                  vfs.c pipe.c shm.c blk_cache.c mbr.c
  fs/ext2/             ext2.c(挂载校验/路径切分/目录记录/12 个 fs_ops 回调) block/block.c(块层：读写+bmap+组位图分配+截断) inode/inode.c(inode 表读写)；公共头 include/ext2.h（S2 起挂根）
  fs/fat32/            fat32.c fat32_dir.c fat32_file.c fat32_meta.c fat32_journal.c
  ipc/                 dipc.c(驱动 IPC 通道：名字表+共享帧环+等待链, 53/54/55)
  drivers/             pci.c ahci.c uart.c ps2-mouse.c(辅助口鼠标 + dipc 通道；重组/字段解码抽到 abi/ps2-mouse.h，**2026-11-19 由 `ps2mouse.c` 改名**) framebuffer.c input_event.c ps2-keyboard.c(扫描码解码单源 + dipc 通道创建)
ktest/                 启动自测：ktest.c 框架 + 33 个 t_*.c + include/（在仓库根）
userland/              Makefile + bin/ 16 个程序（lsh 解析 `|` 与 `<`/`>`、内建 cd/pwd；mv 兼做改名；cat 无参数读 stdin；ls 无参时 kgetcwd 取当前目录）+ mod/ ring3 驱动模块（S1b，现为 ps2-keyboard 与 ps2-mouse 两个 `.km`（2026-11-19 加 `ps2-` 前缀），与 bin/ 同配方、独立对象子目录、装进镜像 `/mod`）+ lib/(crt0.S + ulib.c)
scripts/               patch_setup_jmp.py patch_bzimage.py smoke_test.py
```

`userland/lib/` 是**每个**用户程序链接的运行库，不是 `/init`。

### 容量常量

| 常量 | 值 | 定义处 | 常量 | 值 | 定义处 |
|---|---|---|---|---|---|
| `MAX_CPUS`/`NR_TASKS`/`NR_FDS` | 8/64/16 | `include/types.h` | `MAX_ORDER`/`MIN_ORDER_BITS` | 11/12（buddy 最大 4 MiB） | `pmm.c` |
| `KSTACK_SIZE` | 32768 | `sys/sched.h` | `PMM_WINDOW_TOP` | `0x80000000`（2 GiB） | `mm/mm.h` |
| `LNXRM_NR_SIGNALS` | 14（编号 1..13） | `lnxrm_abi.h` | `RESV_MAX`/`KHEAP_NBINS` | 8/9 | `pmm.c`/`mm.h` |
| `LNXRM_SYSCALL_LAST` | 55（共 56 个） | `lnxrm_abi.h` | `CRED_USER_MEM_QUOTA_PAGES` | 64 MiB/4 KiB | `sys/cred.h` |
| `LNXRM_SHM_MAX_SIZE`/`MAX_OBJECTS` | 1 GiB/64 | `sys/shm.h` | `LNXRM_SHM_NAME_MAX` | 32（含 NUL） | `sys/shm.h` |
| `LNXRM_DIPC_MAX`/`NAME_MAX` | 8/32（含 NUL） | `sys/dipc.h` | `LNXRM_DIPC_RING_SIZE`/`PAY_SIZE` | 4096/4088（−8 B 头） | `lnxrm_abi.h`/`sys/dipc.h` |
| `CACHE_ENTRIES`/`MAX_MOUNTS` | 64/4 | `disk.h`/`vfs.c` | `HZ`/`AP_STARTUP_TIMEOUT_MS` | 100/100 | `sys/cpu.h`/`sys/smp.h` |

## 2. 启动

**两条路径在 `start_kernel` 汇合，靠物理 `0x6F00` 的 DWORD 区分（必须按 4 字节读）**：QEMU `-kernel` 写 0（E820 表 `0x7000`、VBE 写 `0x8C00`）；GRUB `linux` 写 `boot_params` 指针（非 0）。无 EFI handover，仅 legacy BIOS/CSM。

| master 表 | 物理 | 作用 |
|---|---|---|
| `PML4_M` | `0x50000` | `[0]` 恒等、`[511]` 高半 |
| `PDPT_M`/`PD_M`/`PD_M2` | `0x51000`/`0x52000`/`0x57000` | 恒等 2 MiB 大页（**镜像 slot 被 `vmm.c` 拆 4 KiB 赋 W^X**） |
| `PDPT_HI`/`PD_HI`/`PD_HI2` | `0x53000`/`0x54000`/`0x58000` | 高半别名（`PD_HI` 镜像槽同样拆页赋权） |
| `PT_VGA`/`PT_DEV` | `0x55000`/`0x56000` | `PD_HI[96]`→`0xB8000`；`PD_HI[128]`→MMIO |

恒等区与高半别名各铺 2 GiB —— `PMM_WINDOW_TOP` 的上界来源。

### `start_kernel` 顺序（顺序即契约）

1. `console_init → gdt_init → cpu_set_gs_base → idt_init` —— **必须头四步**：IDT 前任何异常 = #PF → #DF → 三重故障无痕复位。
2. `boot_collect_e820` → 横幅 → `boot_read_vbe` → `boot_read_cmdline`。
3. `ktest_run(KTEST_EARLY)`。
4. `cpu_init_percpu(0,0)` → **`pmm_reserve` ×7**（必须在 `pmm_init` 前）。
5. `pmm_init → vmm_init → kheap_init`。
6. `acpi_init` → `cpu_init` → `pit_init(HZ)`。
7. `ktest_run(KTEST_MM)`。
8. `fb_init → ps2_kbd_init → serial_irq_init → vfs_init → ahci_init → vfs_try_mount_disk()`；**根没就绪就是没就绪**（initramfs 回退已删，见第 6 节）。
9. `do_global_ctors()` → `sched_init`。
10. `ktest_run(KTEST_LATE)` + `ktest_summary()`。
11. `kernel_spawn("/bin/init")`；失败打 `Stop!` → `fb_flip()` → hlt 死循环。
12. `lapic_timer_calibrate`（BSP 一次）→ `smp_init` → `lapic_timer_start(32, HZ)`。
13. `[boot] ready in N ms` → `sti` → `idle_loop()`。

**手工同步的硬编码**：`arch/kernel.ld` / `arch/entry64.S`（页表物理地址）/ `kernel/mm/vmm.c`（同一组常量）三处只有注释互相提醒，改一处必改另两处；**第四条线**：`vmm.c` 还读 `kernel.ld` 的 `__text_end`/`__rodata_end`/`__bss_end` 决定 W^X 策略，`t_mm.c` 的 `test_vmm_kernel_wx` 钉住落点。

## 3. 内存

流水线：**E820 → buddy 物理页 → 分离空闲链内核堆 → 页表**（`kernel/mm/`）。

- **buddy（`pmm.c`）**：播种取最大 E820 可用区（钳到 `PMM_WINDOW_TOP`）；侵入式空闲链 + 逐 order 位图（物理 `BITS_BASE=0x10000`，`buddy_of = pa ^ size`）；`pmm_reserve` 只登记、播种时生效；每次真交出帧都调 `oom_frame_granted()`。
- **PMM 保留表（`main.c`，`RESV_MAX 8`，有帧缓冲 7 笔/无则 4 笔）**：LFB 物理、双缓冲窗 `[97,104)`、VGA/DEV/KHEAP/ACPI 固定窗——**凡固定窗占的别名槽，背后帧必须同时 reserve**。
- **内核堆（`kheap.c`）**：分离空闲链 9 箱（16…4096 字节），每分配 8 字节头校验 magic；**OOM 返回 `NULL` 必须检查**；16 MiB 映射在 `KHEAP_VMA 0xffffffffa0000000`。
- **三槽地址空间（`vmm.c`）**：PML4 槽 0 恒等（全 AS）、槽 255 用户空间 `[0x7f8000000000, 0x7ffffffff000]`（每进程独立）、槽 511 高半别名（全 AS）。`vmm_new_user_aspace` 复制除 255 外所有条目 ⇒ 内核映射零成本传播；`vmm_init` 别名环**跳过** VGA/DEV/ACPI/LFB/双缓冲固定窗。
- **内核 W^X（`vmm_kernel_wx()`，`vmm_init` 末步）**：`PD_M`+`PD_HI` 覆盖 `[__kernel_start, ALIGN_UP(__bss_end))` 的 2 MiB 槽拆 4 KiB 按段赋权——text `PG_P`(R-X)、`.rodata` `PG_P|PG_NX`(R--)、其余（data/ktest/trampoline/bss）`PG_P|PG_W|PG_NX`(RW-)；**两个别名一起改**，镜像外低内存与共享槽 buddy 帧保持 `P|W`——**AP trampoline 在 `0x8000` 从恒等映射执行，带 NX 会打死 SMP**；逐页 `invlpg`。
- **COW fork**：`dup_user_aspace` 共享帧，可写页清 `PG_W` 置 `PG_COW`（bit 9）；`page_cow_break` **先复制 → 再换 PTE → 最后放旧引用**；`user_ptr_writable` 是唯一 COW 解析门。用户页默认 `PG_NX`，**唯一 `exec=true` 是信号 restorer 与 `PROT_EXEC` mmap**。
- **#PF 三段分发 `page_fault_try_fix()`**：demand（先按 VMA prot 拒越权，再补帧）→ stack_grow（拒取指位）→ COW（只认写位）。**`err` 位必须逐位认**——否则 NX 取指被当成"COW 已破环" → 无限故障循环。
- **栈**：exec 只映 16 KiB，懒惰下扩（下限 512 KiB）；**64 KiB 守护区 `[MMAP_TOP, USER_STACK_TOP-USTACK_MAX_GROW)` 永不映射**，`STACK_GROW_WINDOW == STACK_GUARD` 静态断言；`stack_grow` 有 RSP 邻近检查（`cr2 + window < rsp` 拒）。
- **mmap/munmap/mprotect/brk**：六参 `mmap`(42) top-down 选址 `[MMAP_LO, MMAP_TOP)`、**只登记 VMA 靠需求分页补帧**；`brk` 上限 = `MMAP_LO`；配额 `cred_mem_quota()`、页数真相 `vmm_count_user_pages()`。**文件映射**：`MAP_SHARED` 同一 `(file,offset)` 全系统一个帧（`filepage.c` 64 桶哈希 + 锁外读盘），`MAP_PRIVATE` 自有帧；写回只在 `sys_munmap`/`exec_commit`/`sys_exit` 三处，脏页由硬件 `PG_D` 认定。`mprotect`(44) 作用任意用户范围，**授予 W 永不分配**（共享帧标 COW），撤销 W 连 `PG_COW` 一起清；拷贝门遇 not-present 先补页再判。
- **OOM killer（`oom.c`）**：触发面**只有用户缺页取不到帧**（`pagefault.c` 四个取帧失败点 + 空 buddy 的共享文件页）；**能回答的失败照旧只回答**（`brk` 回 `ENOMEM`、`kmalloc` 回 `NULL`）；受害者 = `mem_pages` 最大者（排除 pid≤1/kxld/embryo/zombie/已带 SIGKILL）；**一次短缺只杀一个**，闩由 `oom_frame_granted()` 开；杀掉要等 exit+reap 才回池。
- ⚠️ `vmm_destroy_user_aspace()` 释放所有 present 数据页：**必须先 `vmm_unmap_user` 再 `pmm_free`**，否则还两次、空闲链毒化。

## 4. 进程、调度与信号

- **task 槽**：64 槽静态数组，槽 0 = BSP idle；**父链接是 `(pid, generation)` 不是指针**（槽位复用不认错人），`(0,0)` = 无主僵尸标记，惰性回收在 `task_alloc_slot` 撞满表时触发；状态机 `UNUSED/EMBRYO/RUNNABLE/RUNNING/SLEEPING/ZOMBIE/STOPPED`；内核栈 32 KiB（顶 `intr_frame`，下 56 字节 `cpu_ctx`）。
- **fork**：COW 共享 + `vma_dup` 深拷 + fd 表 `refcnt++` + 伪造子帧（rax=0）。**execve**：先 `vmm_switch_to(new)` 再 `elf_load`，成功先赋 `current->pml4` 再销毁旧地址空间；只接受静态 `ET_EXEC`。**exit**：关 fd → **`fb_disown`（画屏归属唯一释放点）** → 孤儿挂 pid 1 → `SIGCHLD` → ZOMBIE。
- **调度**：每 CPU **尾插 FIFO 队列**（`enqueue_on` 追尾、`pick_next` 取头，持续争用下即真 RR，C62；非环形，哨兵闭链）、`quantum=6` tick=60 ms、无优先级；全局 `runq_lock`（irqsave，`pick_next` 读队头也在锁内——C4）；**重排 current 必须走 `runqueue_add_local()`**；`switch_to` 只有 `pml4` 不同才切 CR3，idle 显式设 `master_pml4_phys()`；`swtch` 只存 7 个 u64。
- **信号**：14 槽编号 1..13，`SIGKILL 7`/`SIGSTOP 13` 硬置位且 KILL 醒 SLEEPING+STOPPED，默认终止集 6 个；`do_signal_check` 只在 `iretq` 前带 ring-3 守卫、一次一帧；restorer 页 9 字节 `mov eax,20; int 0x80; ud2`。
- **凭据**：`CRED_USER/ROOT/KXLD` 三级 + `CAP_KILL/SYS_ADMIN/FB`；`setuid` **单向降权**；`kxld_protect_kill()`：pid 1 与 KXLD 不可杀；规则都是纯函数。

## 5. 系统调用与 ABI

- 向量 `int 0x80`，编号 `rax`，参数 `rdi/rsi/r10`，返回 `rax`；`syscall_do` 查**一张 `sysents[]` 表** `{入口, 门, 门名}`——门与入口同一行，未知编号回 `-38` 且**先不问门**。
- **只追加不重排**：唯一定义源 `include/abi/lnxrm_syscalls.def`（X-macro），`lnxrm_abi.h` 逐编号编译期校验，`LNXRM_SYSCALL_LAST` 必须随末尾加一。
- 门：`GATE_ADMIN` 11 个 / `GATE_FB` 5 个，拒绝回 `EACCES=-13` 并打日志；**先能力门、后 `fb_owner_gate` 归属门**（顺序是硬的），`fb_info` 两道门都不过。走字符串的经 `copy_user_str`，走指针的调用方自校验——**没有统一前置检查**。
- ABI 结构体（`lnxrm_abi.h`）：23 个 errno、信号号、`lnxrm_timespec` 16B / `utsname` 320B / `ps_entry` 56B（`name[24]`，2026-11-18 由 48B/16B 扩：`/mod/keyboard.km` 是 16 字符，16 字节的 `name` 连 NUL 都放不下，`ps` 里就成了 `/mod/keyboard.k`；2026-11-19 实测 `ps` 完整显示两个 `.km` 全名；同日改名加 `ps2-` 前缀后 `/mod/ps2-keyboard.km` 20 字符仍在 24 内）/ `dirent` 72B / `fb_info` 16B，`_Static_assert` 锁布局；**ABI 一改用户态必须全量重编**。

### 56 项总表（编号 0–55，连续无空洞）

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
| 42–44 | `mmap/munmap/mprotect` | mmap **六参** `addr,len,prot,flags,fd,offset`（4–6 参读 trap frame `rdx/rcx/r8`），addr=0、top-down、**只登记 VMA**，`fd<0` 匿名、shared 走共享页缓存；munmap 挖洞拆分 + 逐页 unmap，零相交 `EINVAL`；mprotect 任意范围，洞 `ENOMEM`、未对齐 `EINVAL`，**授予 W 不分配** | `core/syscall.c`+`mm/vma.c`+`mm/filepage.c` |
| 45–46 | `stat/gettimeofday` | 56 字节路径元数据（`st_dev` 槽从 1 起，`ino` 与 getdent 同号）；`tv` 微秒（CMOS 一次 + TSC 演进，单调）、`tz` 恒 0 | `core/syscall.c`+`fs/vfs.c`+`core/rtc.c` |
| 47 | `pipe` | 单向字节流：4096 B 环 + 等待队列（空环读者/满环写者 park，close 末端产 EOF/EPIPE）；`file->drv` 存 `struct pipe *`，两端各一个 `file_ops` | `fs/pipe.c`+`fs/vfs.c` |
| 48–50 | `reboot/chdir/getcwd` | 重启（`GATE_ADMIN`）；改 `task.cwd[128]`（≤127 字节，超 `ENAMETOOLONG`）即相对路径基准；把 cwd 拷回用户缓冲，装不下 `ERANGE` | `core/syscall.c`（48）+`fs/vfs.c`（49/50） |
| 51–52 | `shm_open/shm_unlink` | **命名共享内存对象**：名字表项持唯一一个 `struct file`，所有 opener 同指针 ⇒ 共享页缓存天然一帧、MM 层零改动；entry 引用只由 unlink 放，`close_node` 恒在其后无条件 `kfree`（内容跨 close 存活）；mode 固定 `O_RDWR`、无 gate、`<32` 字节名 / 1 GiB / 64 对象 | `fs/shm.c`（+`include/sys/shm.h`） |
| 53–55 | `dipc_open/dipc_recv/dipc_send` | **驱动 IPC 通道**（S1a；环结构/park 协议/生命周期见 `CODEBASE_SUBSYSTEMS.md` §6）：名字表 + 唯一 `struct file`（同 shm 的「所有 opener 同指针 ⇒ 一帧」），**不进 shm 名字表**故 `shm_open` 摸不到原始字节流；recv 空则 park 到通道 `waiters`（前插 `task->wait_next`，与 pipe/input_event 同字段），**wake 恒锁外**、醒后 unlink + 恢复 `T_RUNNING`；send 不走环、拷进内核缓冲锁外调 create 时装的 sink。三行全 `GATE_ADMIN`；open flags 必 0、recv `NONBLOCK`→`EAGAIN`、`len∈[1,4096]`、send `len∈[1,1024]`、坏目的 `EFAULT` **不消耗数据**、满环丢字节计入 `dropped` | `ipc/dipc.c`（+`include/sys/dipc.h`） |

统一输入队列（39–41）把两生产者的已解码结果合成一条流：`KEY` 只出自 IRQ1 解码（`kbd_inject` 不进队列），`REL/BTN/WHEEL` 出自 aux 包尾；34–38 的环与门原样保留。**S1b 额外开的是一条并行的原始字节流**（`dipc_push("kbd")`），与这两套队列互不改道。

---

§6–§10（文件系统 / 驱动 / SMP 与 ACPI / 自测 / 关键不变式）拆在 **`CODEBASE_SUBSYSTEMS.md`**（2026-11-24 拆分，小节号沿用原编号）。
