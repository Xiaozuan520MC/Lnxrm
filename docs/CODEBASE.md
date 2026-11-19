# lnxrm 代码库组织与子系统说明

面向要改这套代码的开发者。构建与运行见 BUILD.md，差距与计划见 GAP_ANALYSIS.md，项目概览见 README.md。

x86-64 freestanding 单体内核：C 为主体，启动/中断/上下文切换用 NASM，产出 Linux boot protocol 2.08 的 `bzImage`，FAT32 系统盘挂根并跑自带 shell。**只有 C 与 ASM**，构建零警告。

## 1. 总览

| 产物 | 说明 |
|---|---|
| `build/bzImage` | `setup.bin` + `vmlinux.bin`，两个 Python 脚本回填 boot header |
| `build/vmlinux.elf/.bin` | 链接产物（`arch/kernel.ld`，单一 `.image` 段 VMA `0xffffffff80100000` / LMA `0x100000`）/ 裸 payload |
| `build/disk.img` | 64 MiB FAT32 系统盘（mkfs.vfat + mtools，装 `README.md`、`bin/` 与 `mod/`） |
| `build/user/<name>` | 16 个静态 ELF64 用户程序 |
| `build/mod/<name>.km` | ring3 驱动模块 ELF（**`.km` 由 `MOD_ELFS` 在链接期加上**，`::mod` 里同名），现为 `ps2-keyboard.km`、`ps2-mouse.km` 两个（2026-11-19 加 `ps2-` 前缀） |
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
  fs/fat32/            fat32.c fat32_dir.c fat32_file.c fat32_meta.c fat32_journal.c
  ipc/                 dipc.c(驱动 IPC 通道：名字表+共享帧环+等待链, 53/54/55)
  drivers/             pci.c ahci.c uart.c ps2-mouse.c(辅助口鼠标 + dipc 通道；重组/字段解码抽到 abi/ps2-mouse.h，**2026-11-19 由 `ps2mouse.c` 改名**) framebuffer.c input_event.c ps2-keyboard.c(扫描码解码单源 + dipc 通道创建)
ktest/                 启动自测：ktest.c 框架 + 32 个 t_*.c + include/（在仓库根）
userland/              Makefile + bin/ 16 个程序（lsh 解析 `|` 与 `<`/`>`、内建 cd/pwd；mv 兼做改名；cat 无参数读 stdin；ls 无参时 kgetcwd 取当前目录）+ mod/ ring3 驱动模块（S1b，现为 ps2-keyboard 与 ps2-mouse 两个 `.km`（2026-11-19 加 `ps2-` 前缀），与 bin/ 同配方、独立对象子目录、装进 ::mod）+ lib/(crt0.S + ulib.c)
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
| 22–23 | `mkdir/fb_info` | 建目录；帧缓冲几何（两道门都不过） | `fat32/fat32_meta.c`+`core/syscall.c` |
| 24–27 | `fb_clear/fill/char/puts` | 绘图，`CAP_FB`+归属门 | `drivers/framebuffer.c` |
| 28–31 | `unlink/rmdir/rename/setuid` | 删文件/删空目录/改名；`setuid` 单向降权 | `fat32/fat32_meta.c`+`core/protect.c` |
| 32–33 | `fb_flip/sync` | 离屏后缓冲推前台；脏扇区交设备 | `drivers/framebuffer.c`+`fs/vfs.c` |
| 34–36 | `kbd_get/inject/info` | 弹键/代打/快照；get·inject 带 `CAP_SYS_ADMIN`。**S1c 起 IRQ1 在通道有 opener 时把原始字节交给 `dipc` 通道 `"kbd"`（`dipc_push_if_listening`）、无 opener 本地解码，34/36 契约不变** | `irq/isr.c`+`core/syscall.c`+`drivers/ps2-keyboard.c` |
| 37–38 | `mouse_get/info` | 弹指针事件/aux 环快照；get `CAP_SYS_ADMIN`。**S1c 起 IRQ12 在通道有 opener 时把原始字节交给 `dipc` 通道 `"mouse"`、无 opener 本地 `ms_feed_byte`，37/38 契约不变** | `core/syscall.c`+`ps2-mouse.c` |
| 39–41 | `input_pop/info/wait` | 统一队列 24B 事件；pop·wait `CAP_SYS_ADMIN`，wait 阻塞+EINTR+ms 超时 | `drivers/input_event.c` |
| 42–44 | `mmap/munmap/mprotect` | mmap **六参** `addr,len,prot,flags,fd,offset`（4–6 参读 trap frame `rdx/rcx/r8`），addr=0、top-down、**只登记 VMA**，`fd<0` 匿名、shared 走共享页缓存；munmap 挖洞拆分 + 逐页 unmap，零相交 `EINVAL`；mprotect 任意范围，洞 `ENOMEM`、未对齐 `EINVAL`，**授予 W 不分配** | `core/syscall.c`+`mm/vma.c`+`mm/filepage.c` |
| 45–46 | `stat/gettimeofday` | 56 字节路径元数据（`st_dev` 槽从 1 起，`ino` 与 getdent 同号）；`tv` 微秒（CMOS 一次 + TSC 演进，单调）、`tz` 恒 0 | `core/syscall.c`+`fs/vfs.c`+`core/rtc.c` |
| 47 | `pipe` | 单向字节流：4096 B 环 + 等待队列（空环读者/满环写者 park，close 末端产 EOF/EPIPE）；`file->drv` 存 `struct pipe *`，两端各一个 `file_ops` | `fs/pipe.c`+`fs/vfs.c` |
| 48–50 | `reboot/chdir/getcwd` | 重启（`GATE_ADMIN`）；改 `task.cwd[128]`（≤127 字节，超 `ENAMETOOLONG`）即相对路径基准；把 cwd 拷回用户缓冲，装不下 `ERANGE` | `core/syscall.c`（48）+`fs/vfs.c`（49/50） |
| 51–52 | `shm_open/shm_unlink` | **命名共享内存对象**：名字表项持唯一一个 `struct file`，所有 opener 同指针 ⇒ 共享页缓存天然一帧、MM 层零改动；entry 引用只由 unlink 放，`close_node` 恒在其后无条件 `kfree`（内容跨 close 存活）；mode 固定 `O_RDWR`、无 gate、`<32` 字节名 / 1 GiB / 64 对象 | `fs/shm.c`（+`include/sys/shm.h`） |
| 53–55 | `dipc_open/dipc_recv/dipc_send` | **驱动 IPC 通道**（S1a；环结构/park 协议/生命周期见 §6）：名字表 + 唯一 `struct file`（同 shm 的「所有 opener 同指针 ⇒ 一帧」），**不进 shm 名字表**故 `shm_open` 摸不到原始字节流；recv 空则 park 到通道 `waiters`（前插 `task->wait_next`，与 pipe/input_event 同字段），**wake 恒锁外**、醒后 unlink + 恢复 `T_RUNNING`；send 不走环、拷进内核缓冲锁外调 create 时装的 sink。三行全 `GATE_ADMIN`；open flags 必 0、recv `NONBLOCK`→`EAGAIN`、`len∈[1,4096]`、send `len∈[1,1024]`、坏目的 `EFAULT` **不消耗数据**、满环丢字节计入 `dropped` | `ipc/dipc.c`（+`include/sys/dipc.h`） |

统一输入队列（39–41）把两生产者的已解码结果合成一条流：`KEY` 只出自 IRQ1 解码（`kbd_inject` 不进队列），`REL/BTN/WHEEL` 出自 aux 包尾；34–38 的环与门原样保留。**S1b 额外开的是一条并行的原始字节流**（`dipc_push("kbd")`），与这两套队列互不改道。

## 6. 文件系统

层次：`vfs.c`（vnode/file/fd 表/路径解析/挂载）→ `blk_cache.c`（64 项时间戳 LRU + `blk_io_*` **唯一驱动调用点**，C10 起还负责每设备请求队列）→ `mbr.c` → `fat32/`（挂载/目录/文件/元数据/redo journal）。`pipe.c`、`shm.c` 与 `ipc/dipc.c` 都不在层次里——前者没有 vnode、直接挂在 fd 表上，后两者连路径都不走（名字在它们自己的表里），见下。

- **VFS 对象**：`vnode` 每次 open 在栈上创建拷贝（无持久 inode，两 fd 的 `pos` 独立，`ino/dev/ro` 供 `stat`）；`file` 走 kmalloc + CAS 引用计数（不回绕、不二次 free），**`priv` 与 `drv` 分开**——`priv` 是 `struct vnode *`（`vfs_file_size`/`vn_size_now`/`sys_mmap` 都解引用它），`drv` 是无 vnode flavour 的私有对象（`pipe` 端点存 `struct pipe *`），两者不会同时非 NULL；`fs_ops` 12 钩子（`size` 供 `O_APPEND`、`close_node` 由 fs 决定释放方式）。
- **pipe（47，`fs/pipe.c`）**：`struct pipe` = 自旋锁 + 4096 B 环 + `rd/wr/used` + `nreaders/nwriters`（计**文件描述**，dup2/fork 共享一份、refcnt 归零才轮到 close 递减）+ `rwait/wwait`；两端各一个 `file_ops`（读端只有 `read`、写端只有 `write`，缺门回 `EBADF`）。等待队列前插单链复用 `task->wait_next`（与 input_event 的 evq 同字段，同一时刻只 park 一处）：push/unlink/detach 持 pipe 锁、**wake 必须锁外**（`sched_wake_task` 取 `runq_lock`，全仓不嵌套），每方向**唤醒全部**；park 协议同 `input_event_wait`（锁内入队 + `T_SLEEPING` + 出队 → `schedule()` → 锁内 unlink + 恢复 `T_RUNNING` → 重查条件）。写入醒读者、读腾空间醒写者、末端 close 悟醒对侧（EOF/EPIPE），**先 wake 再 `kfree`**；整笔写放得下才一次锁内落盘（`sys_write` 已钳 4096 = `PIPE_SZ`）。
- **shm（51/52，`fs/shm.c` + `include/sys/shm.h`）**：32 桶名字哈希（`shm_lock` irqsave），每个 `struct shm_ent` 持**唯一的一个 `struct file`**——`shm_open` 对已存在的名字只 `refcnt++` 交出同一指针。这一条让三件事自动成立、**MM 层零改动**：`mm/filepage.c` 以 file 指针作键故两个 opener 必然同一帧、`sys_mmap` 的 `!priv`/`V_REG` 门开、`vma_fork_shared` 复制的还是同一个 file。伪 vnode 合成 `V_REG` 走 **`priv` 不走 `drv`**（C42 与 pipe 的标记不冲突）：`fs_ops.size` 创建时页对齐**定死**、`read` 补零、`write` **返回 n 却不存字节**（让 `vma_writeback` 清 `PG_D` 而不是无限重试）；`file_ops` 只有 `.close = reg_close`，`read/write/getdent = NULL` ⇒ `sys_read/write` 回 `EBADF`（所有 opener 共享一个游标）。**生命周期两条线一扇门**：entry 的引用**只有 `shm_unlink` 放**，故 `close_node` 必然在 unlink 之后并无条件 `kfree(ent)`（仍 named 则先摘表 + 警告），副带**内容跨 close 存活**（POSIX）；两个顺序坑与 `kmalloc` 二次查表见 §10。名字去前导 `/`、拒空与内部 `/`、`<32` 字节（`ENAMETOOLONG`）；mode 恒 `O_RDWR`（`O_RDONLY/WRONLY/RDWR` 接受并忽略），只认 `O_CREAT|O_EXCL`、其余位 `EINVAL`；1 GiB（`ENOMEM`）、64 对象（`ENOSPC`）、**无 gate**。51/52 不进 systest（`SYSTEST_TAGS` 173 条、smoke 总数不变），只落 `t_shm.c` 三例（全不阻塞）。
- **dipc（53–55，`ipc/dipc.c` + `include/sys/dipc.h`）**：结构抄 shm（唯一 `struct file` ⇒ 共享帧缓存、mmap 门、`vma_fork_shared` 自动成立、**MM 层零改动**），但**连路径都不走、不注册进 `shm_open` 命名空间**——`shm_open("ps2")` 恒 `ENOENT`，原始字节流只可能从 `dipc_open` 拿。**环**住通道自己的 file page：页首 `struct dipc_hdr{u32 rd,wr}`（8 B）+ 4088 B payload，容量 4087、满 = `ring_next(wr)==rd`（payload 非 2 的幂，`ring_next` 分支绕回而非掩码）；**索引写在帧里**，ring3 mmap 出来即无 syscall 读余量，越界由 `ring_check` 归零（见 §10）。`copy_*_user` 不补缺页 ⇒ 直接在 `dipc_lock` 内从帧拷到用户缓冲、**无需 kstack bounce**；wrap 拆两段拷、`rd` 只在两段都成功后推进（**`EFAULT` 不消耗数据**）。**等待链**复用 `task->wait_next`（与 pipe/input_event 同字段），push **锁内摘整条链、锁外**逐个 wake（先取 `next`）；recv 的 park 协议照抄 `pipe.c:186-197`（醒后 unlink + **恢复 `T_RUNNING`** 再重查）。**生命周期两条线一扇门**照搬 shm（见 §10）；失败回滚走 `pmm_free → filepage_drop_file → kfree(priv) → kfree(f) → kfree(d)` 而非 `vfs_close_file`。反向 send **不走环**：拷进内核 1 KiB 缓冲后**锁外**调 create 时装的 `sink`——**S1c 起 `kbd`/`mouse` 的 sink 承载 ring3 解码结果的回传**（`kbd_sink`/`mouse_sink`，见第 6 节）。三行全 `GATE_ADMIN`（8→11）；**第 4 参走 `rdx`**（与 `h_mmap` 同款，分发层零布线）。`dipc_push(name,…)` **按名字查表**（IRQ 安全），环满丢字节计 `dropped`。53–55 刻意不进 systest（173 条、smoke 总数不变），只落 `t_dipc.c` 四例（**全不阻塞**，park 半段用假任务直接驱动）。
- **路由**：`fs_for_path` 按**最长前缀 + 组件边界**选唯一挂载项（`"/"` 不吞 `/alt`），`ops` 与 `priv` 成对取自它；`vfs_umount` 只动路由**不 free priv**；跨挂载 `rename` 回 `EXDEV`；挂载入口只有 `vfs_mount_fs`/`vfs_umount`（**非 syscall**）。
- **cwd（49/50）**：`struct task.cwd[128]` 是所有路径解析的基准——`abs_path()` 把相对路径拼在它之后（slot claim 置 `"/"`、`fork` 复制、`execve` 保留），故 open/stat/unlink/mkdir/… 无需逐个改动；`chdir` 解析存在性后整段替换，`getcwd` 只读拷回。`abs_path` **不折叠 `.`/`..`**（点段交给 `fat_resolve_path`，FAT 根无点记录故根上 `ls .` 仍 `ENOENT`，见 GAP 第 6 节）。
- **错误契约**：`blk_io_*` 驱动非 0 原样上抛 → `fat_*_sector` 只有 0 或 `LNXRM_EIO` → 目录扫描三态 `1/0/负` → 语义层负数一路上传不被改写（对外 `[fat32] I/O error` + `-5`）。
- **路径不变式（同一扇区只走一条路）**：数据/目录经块缓存，FAT 区 `m->fat_pages` 分帧直写/入日志，挂载期 `blk_io_*` 直读；FAT 表 4 KiB 分帧驻留，表项与扇区永不跨帧，访问只走 `fat_entry_get/put_raw/sector_ptr`（其余见 §10）。
- **块缓存**：64 项 × 512 B，键 `(dev,lba)`，时间戳 LRU；读失败作废 victim；**三道同步门**进 `blk_cache_sync_all()`——BSP 每秒 tick / 最后 fd close / `panic()` try 回写（外加 `SYS_sync(33)`），**写回失败保持脏**。**C7 起 `cache_lock` 只做元数据**：fill 先占槽、写回先拷任务自己的弹跳缓冲，victim/purge 跳 `busy` 槽，`blk_io_*` 全在锁外；tick 回写门改 trylock；写回失败进 `tried` 位图重试。
- **块请求队列（C10）**：`blk_io_read/write` 两段式——phase 1 持设备 `io_lock` 把请求（节点在调用者栈上）追入该设备 FIFO 后放锁；phase 2 重新持锁 `blk_drain_locked()` **排空整队按到达序执行**（含别的 CPU 中途入队的），排空到空即自身 `BLK_REQ_DONE`，读栈上 `status` 即返回值、无自旋。错误只记在该请求自己的 `status`；**驱动不再自带锁**（`ide_lock`/`ahci_lock` 已删），两盘经各自 `io_lock` 真并行，分区走「分区 → 父盘」嵌套序。合并/取消未做（调用者仍压着 `fat_fs_lock`，待 C7 余项）。
- **FAT32**：挂载校验 BPB（55AA、`"FAT32"`、簇因子 2 的幂、`bps==512`），九处拒绝统一 `fat_reject()`；目录可用槽 = 尾槽 0x00 与已删 0xE5，**0x05 是活记录**，槽不够先扩链 ⇒ 目录永不满而 ENOSPC；getdent cookie = `(簇<<32)|字节偏移` 物理恢复点；LFN 校验 checksum + 序号递减，坏即回落 8.3；簇链 `FAT_CHAIN_MAX_STEPS` 步数预算 + 单簇自环即断。
- **redo journal**：reserved 空闲扇区，扇区 8 superblock（magic `JRN1`，`head` 是相对偏移）；locked wrapper `begin → *_impl → commit`，**失败路径也 commit**；顺序 ①flush 数据 ②写记录 ③superblock 置 **COMMIT（唯一提交点）** ④apply ⑤flush ⑥CLEAN，**④ 失败必须保留 COMMIT**；mount 时 `state==COMMIT` 幂等 redo。
- **挂载顺序**：`vfs_try_mount_disk` 逐盘先整盘 FAT 再 MBR 分区（每分区先 `blk_register` 再试）→ 分流（有 `/bin/init` 挂 `/`，余挂 `/mnt`）。`blk_register` 满 8 槽**大声拒绝且表不变**。**没有第二条路**：initramfs（`initramfs.c`/`mkinitramfs.py`/`.initramfs` 段/`vfs_mount_initramfs()` 回退）整套删除——它曾让「根挂不上」悄悄降级成只读 cpio 根，掩盖真故障；现在挂不上就是挂不上。
- **`/mod`（S1b）**：`disk.img` 多一个 `::mod` 目录（`mmd` + `mcopy`，文件名与 `build/mod/` 里**完全一致**——`.km` 由 `MOD_ELFS` 在**链接期**加上，安装步骤只做 `basename` 搬运），装 ring3 驱动模块。`init` 在 README 打印之后、`fork` 终端之前 `kopen("/mod") → kgetdent` 扫一遍，校验 `.km` 三字符后缀（**大小写不敏感**：FAT 短名的大小写取决于写入方的 NT 位，而路径解析侧的 `fat_name_eq_short` 本就不分大小写，筛子敏感反而会拒掉 exec 打得开的文件）后 `kfork + kexecve`，**每个模块一个子进程**、init 本身继续当 reaper；**成功路径一行不打印**（2026-11-18：`init: module ...` 与统计行都拿掉了，驱动正常加载本就该是隐形的），**失败才出声**——`/mod` 打不开、`kgetdent` 出错、名字过长、`kfork`/`kexecve` 失败各一行，另外扫描后**一个模块都没起来**才打统计行 `init: N file(s) in /mod, 0 spawned -- no module was started`（健康 boot 静默），**pid 1 仍永不因此退出**。发现靠目录扫描而非链接期名单——加一个驱动 = 往镜像里放一个文件。

## 7. 驱动

- **串口/控制台**：16550 115200 8N1，COM1→IRQ4→向量 36；`console_putc` 扇出 fb→VGA→串口→4096 B 环形缓冲；`kvprintf` 持 `console_out_lock`(irqsave)；`panic()` 打印后 **`fb_flip_unlocked()` 再 `cli;hlt`**（双缓冲下无 tick 呈现，无锁版防自死锁 T-002）。**loglevel + 崩溃环 + backtrace（PP2）**：四级 `LOG_ERR/WARN/INFO/DEBUG`（低=严重），`klog_setlevel` 只过滤控制台、**环恒录**（`kprintf`≡INFO，cmdline `loglevel=<0..3>`）；`klog.c` 96 槽 ×160 B 记录环，panic 路径一律 `spin_trylock_irqsave`（拿不到就丢，绝不自旋到不了 `hlt`），`klog_dump` 走原始 `console_putc` 不回灌；`panic()` 顺序 = 消息(LOG_ERR) → 环 tail → `backtrace()`（帧指针走链，逐帧校验 canonical/对齐/已映射/ret∈text/rbp 递增，地址对 `nm`）→ flip → hlt。9 处诊断行降 DEBUG（清单见 `klog.c`），smoke 标记全留 INFO。
- **键盘（IRQ1，S1b 解码器单源）**：IOAPIC IRQ1→33，set 1 扫描码→ASCII 进 256 B `inbuf`，等待者 `con_next` 链表。**解码器单源**：`kbd_process_sc()` 在 `drivers/ps2-keyboard.c`，状态机与两张表住共享头 `include/abi/ps2-keyboard.h`（`static inline` + 函数内 block-scope 表，规避 `-Wunused-const-variable`），内核与 ring3 模块**各持自己的 `struct lnxrm_kbd_state`**（模块的 shift/caps 不能反手清掉内核注入器正持有的位）。IRQ1 桩 = `inb(KBD_DATA)` → `kbd_scans++` → **二选一（S1c）**：`dipc_push_if_listening("kbd",…)` 在通道有 opener（file refcnt>1）时把字节存进环并返回 true ⇒ **内核不解码**（ring3 是存活时的唯一解码者，解出的字符经 sink 回传）；无 opener（镜像里没 `.km`、模块退出、通道未建）返回 false 且**什么都不存** ⇒ `kbd_process_sc(sc)` 按 S1b 之前的方式本地解码——**没放 .km 照样能打字，模块死掉下一个字节就自动交还**，ADDITIVE 的保证原样保留。`sys_kbd_get 34`/`sys_kbd_inject 35` 契约原样（35 本就直推 ASCII、不经解码）。`kbd_decode_init()` = `dipc_create("kbd", kbd_sink)` + 一行 kprintf——**sink 收模块发回的 `[char, mods]` 对，落 `input_push` + `input_push_key`，与 `kbd_process_sc` 尾部逐位一致**，**刻意排在 ktest 之后**（`t_dipc` 断言第 9 个通道 `-ENOSPC`，预建会把 `live0+8` 顶出界，ktest 数字一位不动）。
- **鼠标（IRQ12，S1b 解码器单源；`ps2-mouse.c`，2026-11-19 由 `ps2mouse.c` 加前缀改名）**：i8042 aux→IRQ12→44，3/4 字节包（`0xF2` identify 探测滚轮，QEMU 的 200/200/80 与 200/100/80 都认），64 槽事件环，内核不带屏上指针。**重组与字段解码抽到共享头 `include/abi/ps2-mouse.h`**（`struct lnxrm_mouse_dec{pkt[4],n,bytes}` + `lnxrm_mouse_decode()`，`static inline`；`dy = -dy` 是这份解码最容易漂的一行）；`ps2-mouse.c` **只保留内核私有的部分**——`ms_lock`、计数器、帧长（原 `ms_bytes` 现为 `ms_dec.bytes`，**0xF2 探测谈下的那份是权威**）与两个消费队列。**`ms_feed_byte` 逐字节等价**（resync 计数、溢出置 0、`buttons = f & 7`、先解锁再 `ms_push` + `input_push_mouse`），故 `t_mouse`/`t_input` 断言一字未改。IRQ12 桩 = 判 AUX → `inb(MS_DATA)` → **二选一（S1c）**：`dipc_push_if_listening("mouse",…)` 有 opener ⇒ 字节进环、**内核不 `ms_feed_byte`**（ring3 跑同一个 `lnxrm_mouse_decode`，必须看见帧长、resync 与滚轮字节，解完整包经 sink 发回）；无 opener ⇒ 本地 `ms_feed_byte(b)`，与 S1b 之前逐位一致；`mouse_decode_init()` = `dipc_create("mouse", mouse_sink)` + 一行 kprintf——sink 收整 `struct lnxrm_mouse_packet`，走新抽的 `ms_feed_packet`（= `ms_push` + `input_push_mouse`，37/38/39–41 与计数器两侧不可分），与 `kbd_decode_init()` 并排**排在 ktest 之后**（同一 `LNXRM_DIPC_MAX=8` 容量阀）。**环在没有鼠标时照样建**：空环就是「没有指针」的诚实答案，明说的那句由 `mouse_info(38).inited` 负责。**统一队列**（`input_event.c`）：63 槽 24 B 事件，**叠加不改道**（kbd/mouse 两旧环保留）；`input_wait(41)` 空队睡 wait list、生产者解锁后唤醒；ktest 钩子 `input_inject()`。
- **ring3 驱动模块（S1b，现为两个）**：`userland/mod/ps2-keyboard.c` 与 `ps2-mouse.c`（2026-11-19 加 `ps2-` 前缀）都是**普通静态 ELF64**，只因住在 `/mod` 而非 `/bin` 才是模块——init 扫目录发现、`kfork+kexecve` 各拉起一个，内核不链接、不感知、不等待。keyboard：`dipc_open("kbd") → dipc_recv` 阻塞取字节、跑**同一个** `lnxrm_kbd_decode()`，解出的 `[char, mods]` 对经 `dipc_send(55)` 发回内核 sink；mouse：跑**同一个** `lnxrm_mouse_decode()`，整包同样经 `dipc_send` 发回。**成功路径一行不打印**（2026-11-18 删掉 banner 与 `ring live` 首报：`lsh` 已回显按键，模块再报一次每键两行，QEMU 的指针事件还会把行打进 shell 输出中间）；**唯一输出是拿不到通道**（`*.km: no dipc channel "…"`，即 `*_decode_init()` 没跑过）。**mouse 的帧长 ring3 拿不到**（`mouse_info(38)` 无此字段、`mouse_get(37)` 是 `GATE_ADMIN`）⇒ 模块**从字节流自己推**：按 3 字节读时，4 字节流正好在「本该是 flags 字节」的位置撞上滚轮字节，滚轮值 0..7 过不了 `0x08` 同步位而真 flags 永远带这位；正确的 3 字节流在包边界**一个证据都产不出来**，故要**两份证据**才切到 4 字节（一次损坏的 flags 不得把读者永久锁到错帧长——其引发的后续 resync 都在边界旗标被消费之后，不计入）。**两个模块都不 mmap 环**：`LNXRM_DIPC_HDR_SIZE` 带 spinlock 依赖只在 `sys/dipc.h`，ring3 拿不到；mmap 路径已由 `t_dipc` 覆盖。**S1c（2026-11-19）已把解码整体下沉**：IRQ 只在无 reader 时本地兜底，ring3 是存活时的唯一解码者，结果经通道 sink 回传内核输入队列——模块退出即交还（见第 6 节末条）。
- **AHCI**：pci_scan → 映射 ABAR → 复位 → 逐端口 → `blk_register`（多盘 `sd_names[8]`）；**中断驱动**（`GHC_IE`+`PxIM`+`ahciIsr`，轮询只作超时兜底）；完成判定**先查 `IS_TFES` 再查 `PxTFD.ERR`**；probe 单出口 `ahciPortRelease()` 兜全部失败路径。**无驱动全局锁**（C10：`ahci_lock` 已删，跨盘串行解开；`AhciPort`/`blkdev` 构造点 memset 清零——kmalloc 不保证零）。
- ⚠️ `make run` 默认 `MACHINE=q35`（AHCI 主路径），`-drive if=ide` 在 q35 上由 QEMU 挂到 ICH9 AHCI 的 `ide.0`。`smoke` 同样 q35：`SMOKE_QEMU_ARGS ?= -machine q35`，在 `SMOKE_FLAGS` **之前**传给 `--qemu-args`（argparse 取最后一个，用户 `--qemu-args` 仍可覆盖）+ TCG + `-m 256`。**`kernel/drivers/ide.c` 已删**（备份 `/tmp/opencode/s1_backup/ide.c.bak`）：机型注入之后再留一个端口级 PIO 驱动，只会让「同一张镜像在两台机器上走两套磁盘栈」掩盖真故障。**后果**：`MACHINE=pc`（i440fx）既无 AHCI 也无 IDE ⇒ **不再能挂根**，只适合跑不碰盘的启动自测（`t_ahci` 自动 skip）；要跑磁盘就用默认的 q35。
- **帧缓冲**：`FB_VMA 0xffffffff8d000000`（`PD_HI[104]`），双缓冲后缓冲窗 `[97,104)`（14 MiB，逐 4 KiB 帧拼虚拟连续）；**`pmm_reserve` 与别名过滤必须成对按整窗算**；**画的与看的不是同一块**——`fb_ptr` 指后缓冲，只有 `fb_flip` 拷脏区域。呈现：首 tick 前按写批次边界 present，tick 后 BSP 每 2 tick 节流、批次门永久关死；**逐行拷贝全程持 `console_out_lock`**（消撕裂），拷贝尾 `mfence`；绕过 `fb_store_px` 的写必须自己补标。LFB 写合并 `fb_memtype_program()` 每 CPU 一次（PAT WC 槽 + MTRR 采纳，固件 UC 覆盖则落 UC）。失败即退回单缓冲，无开关。
- **归属**：`fb_owner` 用 CAS 而非 check-then-set；释放只在 `sys_exit`。

## 8. SMP 与 ACPI

- **ACPI**：RSDP→RSDT/XSDT→MADT，产 `acpi_collect_ap_ids()`；**表读取必须经 `ACPI_SCRATCH_VA` 弹跳窗**（`PD_HI[108]`，真机表常在恒等窗外）；`acpi_init` 在 `vmm_init` 后、`cpu_init` 前。
- **AP 拉起**（`smp.c`+`trampoline.S`）：trampoline 拷 `0x8000`、共享块 `0x9000`（**`ap_main` 传虚拟地址**）→ INIT+SIPI×3 → 轮询 ready 100 ms 超时。`ap_main` 顺序硬：`apic_enable` 最先（x2APIC 访 MMIO 会 #GP）→ GDT → GS → IDT → `cpu_protect_init` → idle → **`trampoline_signal_ready()` 必须先于一切控制台输出**（慢控制台吃光预算会被恢复 INIT 复位）→ LAPIC timer → online。
- **APIC**：xAPIC(MMIO)/x2APIC(MSR) 双路，每核各定型；只用于 INIT/SIPI/IPI（`0xF0` reschedule、`0xF2` stop）。
- **时钟与锁**：`HZ 100`，PIT 只供 LAPIC 标定；BSP LAPIC tick → `jiffies`/`sched_tick`/`fb_tick_flush`。锁表：`runq_lock`（全队列）、`task_table_lock`/`pid_lock`、`pmm_lock`/`kheap_lock`、`cache_lock`（**只做元数据、不跨 I/O**，C7，tick 门 trylock）、`blkdev->io_lock`（**每设备一把**，C10，持有期间排空该设备 FIFO、分区嵌套「分区 → 父盘」）、`fat_fs_lock`（全局 FS 锁，与 `cache_lock` 嵌套；**不接管中断状态**，C7——取用点本就 IF=0，仍跨 I/O）、`print_lock`（含 present 拷贝）、`fb_box_lock`（脏箱快照）。`fat_fs_lock` 是双核 FS 性能瓶颈。
- **中断**：统一 `intr_frame`；`isr_common` 三段分流——IPI→`apic_eoi`、`<32` 异常（ring-3 转信号，ring-0 panic）、`≥32` **先 `apic_eoi` 再调 handler**（handler 可能切走）；IDT 49–255 全指 catch-all（IPI 必须显式覆盖）；#PF 用 `rflags.AC` 区分 WP 与 SMAP。向量：32 timer、33 键盘、36 COM1。保护位 `EFER.NXE`/`CR0.WP`/`CR4.SMEP`/`CR4.SMAP` 由 BSP/AP 共同走 `cpu_protect_init()`，**内核镜像 W^X** 由 `vmm_kernel_wx()` 保证。

## 9. 自测

**ktest（`KTEST=1`；`make smoke` 强制，直跑 `smoke_test.py` 会绕过）**：框架 + 32 个 `t_*.c`，`KTEST(suite, stage, fn)` 注册，按 `EARLY/MM/LATE` 三阶段跑。当前判据（**2026-11-19 `make smoke` 实测，q35 + TCG**；上一基线 2026-11-17 pc 档 162/4951、3 skipped）：

| 阶段 | cases | checks | failed |
|---|---|---|---|
| early | 25 | 426 | 0 |
| mm | 18 | 462 | 0 |
| late | 119 | 4086 | 0 |
| **合计** | **162** | **4974** | **0**（2 skipped、1 刻意 warn） |

相对 158/4754 的累计 **+220 checks**，两笔大账对得上：early 399→426 的 **+27** = S1a 的 `t_abi` `.def` 逐项循环 **+6**（3 个编号 × 每项 2 条）+ q35 下 E820 `map_len` 7→9 的 **+21**（`test_boot_e820_map` 逐条 3 检查 ×2 条目 + 两两重叠对数 3×2+(36−21)=21，逐项对上）；late 3893→4086 的 **+193** = `dipc` 四例 **+191**（S1a）+ `t_ahci` 由 skip 转实测 **+1** + 运行期条件波动 **+1**；mm 档 462 未动。skip **3→2**（`t_ahci` 不再 skip，剩 `fb` 两条条件 skip）；三轮（主跑 + 两轮 forged）late 有 **±1** 的运行期条件检查波动。

37 个 suite：`abi ahci blk blkcache blkio boot console cpu cred dipc draw elf fat32 fatfile fb input jrn kbd kheap klog ktest lib lock mbr mmap mouse oom pipe pmm prop shm stat syscall task usercopy vfs vmm`（含 late 的 `klog` 3 例、`pipe` 4 例、`shm` 3 例、`dipc` 4 例；q35 档最近一次为 **2026-11-19 的 162/4974**）。重点钉点：OOM killer 四例、**W^X 逐段逐页**、`t_mmap` 十一例、`t_stat` 三例、**块队列 FIFO/每设备锁**、调度 FIFO/RR（旧用例原钉 LIFO，已随 C62 改写）、**C7 锁外 I/O**、`t_klog` 三例、`t_pipe`/`t_shm`/`t_dipc` **全不阻塞**（LAPIC timer 未起，park 即挂 boot；shm 另钉「两次 open 同 `struct file` 同帧、unlink 只走名字、内容跨 close 存活」，dipc 另钉「三行门 -13、`MAP_SHARED` 读得到环字节、`EFAULT` 不消耗数据、满环计 `dropped`、假任务被 push 唤醒」）、AHCI 中断自测（无设备即 skip）。`MACHINE=pc` 同判据 0 failed——**这一条随 `ide.c` 删除失效**：i440fx 无 AHCI，`t_ahci` 转 skip 且根挂不上，见第 7 节。

**smoke（`make smoke`，六阶段）**：`toolchain → build → artifacts → boot（21 条有序 marker）→ shell（串口驱动 /bin/lsh + systest 173 条 `SYSTEST_TAGS`）→ forged FAT`。ABI 常量从 `lnxrm_abi.h` **现读**（`abi_define()`，`past.*` 按 `SYSCALL_LAST` 现算），判据一律 `br""`、负面判据禁 `WARN(`/`[PANIC]`；**程序名从不进判据**——按 `userland/bin/*.c` 现场发现、shell 只认 `root@lnxrm$ ` 提示符。当前 **270/270**（**2026-11-19 实测**，S1b + 两轮修复后；2026-11-17 S1a 轮亦 270/270；**S1c 与当日两轮改名后各复跑一次均 270/270**、ktest 162/4974 [2 skipped] 一位不变），transcript 在 `build/smoke/serial.log`；高负载下 TCG boot 可超 420 s 等待门，`--timeout 900` **只放宽等待、判据不变**；`--repeat N` 连跑门**`--repeat 2` = 487/487**（2026-11-15，S0/S1a 轮未复跑）。**S1b 起机型 q35**（`SMOKE_QEMU_ARGS`）。**2026-11-18 起模块的用户态串口行全部从成功路径拿掉**（`init: module ...`、统计行、`attached ...`、`ring live ...`）：正常 boot 在 `[boot] ready` 之后只剩 `A tiny unix-like kernel in ASM + C!`（**boot 段第 20 条 marker `init read /README.md`，判据、红线不可改，必须留**）与 shell 提示符——总数仍 270；内核侧 `[kbd] raw scancodes...`/`[mouse] raw bytes...` 仍在（通道已创建的唯一可见证据，在 ktest 段之后、`spawned pid=1` 之前）；失败行（`init: ... 0 spawned`、`no dipc channel`）只在出问题时出现且**都不进判据**。`t_ahci` 由 skip 转**实测**（`ahci_irq_selftest()` 必须 `>0`，TCG 下不稳会挂）——**2026-11-19 实测通过**（`[ahci] first command completed by interrupt`）。

**systest（`userland/systest.c`）**：按 syscall 编号分节的全量自检（38 个 `t_*` 函数 + 54 行 main），末尾 `systest.done=1`，由 smoke 的 shell 阶段驱动。

## 10. 关键不变式

一条一行，改代码时逐条对：

- `.def` 是 syscall 编号**唯一**定义源：只追加不重排；未知编号回 `-38`；ABI 结构体一改用户态全量重编。
- 图形门**先 `cap_gate` 后 `fb_owner_gate`**，两种拒绝都 `-13`；画屏归属释放**只在 `sys_exit` 一处**。
- `main.c` 的 `pmm_reserve` 与 `vmm.c` 别名过滤**必须成对**，双缓冲窗 `[97,104)` 两边按整窗算（`RESV_MAX 8` 已用 7）；物理窗口 ≤ `PMM_WINDOW_TOP`(2 GiB)，越界播种 = #PF、IDT 前 = 三重故障无痕复位。
- `console_init → gdt_init → gs → idt_init` 必须是头四步；`0x6F00` 哨兵必须按 **DWORD** 读。
- `kernel.ld` / `entry64.S` / `vmm.c` 三处页表与段布局硬编码手工同步；`vmm.c` 跟随 `kernel.ld` 段边界赋 W^X。
- 新地址空间只复制**除槽 255 外**的条目，槽 0/511 永远共享；只有 `pml4` 不同才切 CR3，idle 必须 `master_pml4_phys()`。
- 释放已映射用户帧必须**先 `vmm_unmap_user` 再 `pmm_free`**；COW 帧 `pmm_free` 只减 `frame_ref`、归零才回 buddy。
- `PG_COW` 只与清 `PG_W` **成对**出现；`!W !COW` 写保护必落 SIGSEGV；widen 不给 `PG_COW` 页回写 `PG_W`，**撤写必须连 `PG_COW` 一起清**；COW 破环恒为**先复制 → 再换 PTE → 最后放旧帧**，`user_ptr_writable` 是唯一门。
- mmap 窗 `[MMAP_LO, MMAP_TOP)`、`brk` 上限 = `MMAP_LO`、**守护区永不映射**；`STACK_GROW_WINDOW == STACK_GUARD` 静态断言：堆/mmap/守护/栈两两不相交。
- `PG_W ⇒ pmm_refcount == 1`（授予写只给独占帧，共享帧标 COW 推迟复制）。
- #PF 的 `err` 位**逐位认**：`cow_fault` 只认写位、`stack_grow` 拒取指位；demand **先按 VMA prot 拒、再取帧**；拷贝门遇 not-present **先补页再判**，别把「lazy」当「unmapped」。
- buddy 最大块 4 MiB；`kmalloc` OOM 回 `NULL` 必须检查；配额唯一来源 `cred_mem_quota()`，页数真相 `vmm_count_user_pages()`。
- **OOM killer 只在用户缺页取不到帧时触发**；能回答的失败永远只回答；一次短缺只杀一个，闩由帧真出手开。
- 改 `struct task` 必须**全量重编**（`cpu_info` 内嵌按值 `runq_head`）。
- 运行队列是**尾插 FIFO/RR**（`enqueue_on` 追尾、`pick_next` 取头且读在 `runq_lock` 内，C4/C62）；重排 current 走 `runqueue_add_local()`；`sched_wake_task` 不得持 `runq_lock` 调 `runqueue_add`。
- 信号只在 `iretq` 前带 ring-3 守卫处投递，一次一帧；**`current->tf` 只发布 ring-3 帧**；restorer 页是唯一 `exec=true` 的用户页。
- `blk_io_read/write` 是全内核唯一调 `dev->read/write` 的地方（ktest-only 的 AHCI 中断自测除外）；驱动函数只在 `blk_drain_locked()` 持**该设备** `io_lock` 时执行，嵌套恒为「分区 → 父盘」；**FAT 区永不进块缓存**，一个扇区只走一条读写路，FAT 表只走三个 helper。
- 路径路由只认**最长前缀 + 组件边界**，`ops`/`priv` 成对取自它；`vfs_close_file` 只减真有的引用；`vfs_umount` 不 free priv；`blk_register` 满槽大声拒绝且表不变。
- shm 是两条线一扇门：表项那份 file 引用**只由 `shm_unlink` 放**，故 `close_node` 恒在 unlink **之后**触发并无条件 `kfree(ent)`；`shm_unlink` 必须「锁内摘表 → **解锁** → `vfs_close_file`」，`kmalloc` 必须在锁外并**二次查表**（输者直接 `kfree`，不走 `vfs_close_file`）。
- dipc 同一扇门的第二条：`dipc_destroy` **只摘表 + 解锁后放 entry 引用**（绝不 `-EBUSY`，park 中的 recv 靠自己的 fd pin 住），`dipc_close_node` 恰好一次里先 `pmm_free(ring_pa)` 再 `kfree(d)`（cache 的引用仍在，帧死于第二次释放）；**wake 恒在 `dipc_lock` 之外**且先取 `next` 再唤醒；`ring_take` 的 `rd` 只在两段拷贝**都成功**后推进（`EFAULT` 不消耗数据）；索引在帧内、越界由 `ring_check` 归零。
- AHCI 的写都以 `0xE7` CACHE FLUSH 收尾并**检查返回值**（`ide.c` 已删，全仓只剩这一处块设备写）。
- 解码器单源：`include/abi/ps2-keyboard.h`（扫描码→ASCII 的**唯一**表定义）与 `include/abi/ps2-mouse.h`（aux 包重组/符号扩展的**唯一**定义），内核 `drivers/ps2-keyboard.c`/`drivers/ps2-mouse.c` 与 ring3 `userland/mod/ps2-keyboard.c`/`userland/mod/ps2-mouse.c` 各持**自己的**状态结构；IRQ1/IRQ12 **先解码再镜像**，`dipc_push` 失败（通道未建/环满）绝不影响解码路径。帧长以**内核 `ms_dec.bytes` 为权威**，ring3 的推断只影响它自己。
- `kbd_decode_init()`/`mouse_decode_init()` 必须排在 ktest 之后：`t_dipc` 断言第 9 个通道 `-ENOSPC`，预建 `"kbd"`/`"mouse"` 会把 `live0+8` 顶出界。
- `init` 是 `/mod` 的**唯一**发现者与 reaper：扫目录 + `.km` 后缀（大小写不敏感）+ 一个模块一个子进程，**成功路径静默、每条失败路径各打印一行**（「扫到文件却一个都没起来」才打统计行），任何失败只跳过，**pid 1 不因此退出**。
- `::mod` 里的文件名必须带 `.km`（**后缀由 `MOD_ELFS` 在链接期加**，安装只做 `basename` 原样搬运，再补一次就是 `ps2-keyboard.km.km`）：不带后缀时 init 一个都不起，而**镜像、串口、`ps` 三处各自看起来都正常**——这就是「`ps` 看不到驱动进程」的成因（2026-11-18 实测）。
- journal `commit` 第 ④ 步失败必须**保留 COMMIT**；失败路径也必须 commit。
- EOI 必须在 handler **之前**写；IPI 向量必须显式盖过 catch-all。
- AP 的 `ready` 必须先于一切控制台输出；trampoline 传的 `ap_main` 是**虚拟地址**。
- 自旋锁默认 irqsave，唯一例外 `spin_lock_irqon` 只给 `fat_fs_lock`（C7：永不进中断路径的锁才可不接管中断状态）；`fat_fs_lock` 内嵌 `cache_lock`，顺序不得颠倒；`cache_lock` **不跨 `blk_io_*`**（`busy` 槽不可被 victim/purge 挑中）。
- 双缓冲呈现只拷脏区域、**先写后标**，逐行拷贝全程在 `console_out_lock` 内、拷贝尾 `mfence`；绕过 `fb_store_px` 的写必须自己补标；首 tick 前只在写批次边界 present。
