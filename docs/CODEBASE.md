# lnxrm 代码库组织与子系统说明

面向要改这套代码的开发者。构建与运行见 BUILD.md，差距与计划见 GAP_ANALYSIS.md，项目概览见 README.md。

x86-64 freestanding 单体内核：C 为主体，启动/中断/上下文切换用 NASM，产出 Linux boot protocol 2.08 的 `bzImage`，FAT32 系统盘挂根并跑自带 shell。**只有 C 与 ASM**，构建零警告。

## 1. 总览

| 产物 | 说明 |
|---|---|
| `build/bzImage` | `setup.bin` + `vmlinux.bin`，两个 Python 脚本回填 boot header |
| `build/vmlinux.elf/.bin` | 链接产物（`arch/kernel.ld`，单一 `.image` 段 VMA `0xffffffff80100000` / LMA `0x100000`）/ 裸 payload |
| `build/disk.img` | 64 MiB FAT32 系统盘（mkfs.vfat + mtools，装 `README.md` 与 `bin/`） |
| `build/user/<name>` | 16 个静态 ELF64 用户程序 |
| `build/lnxrm.iso` | GRUB 引导 ISO（仅 bootloader + 内核，无根文件系统） |

补丁脚本是构建链关键：`scripts/patch_setup_jmp.py` 用 `nm` 查 `_start64` 把 `setup.bin` 哨兵 `0xF00DFACE` 回填真实跳转；`scripts/patch_bzimage.py` 填 `setup_sects`/`syssize` 并断言 `HdrS`、`0x55AA`、payload 对齐。

```
Makefile               构建入口（源文件自动发现 + 镜像/运行/冒烟）；用户态委托 userland/Makefile
arch/                  setup.asm(实模式) entry64.S(_start32/64、中断桩、swtch) trampoline.S(AP 入口,拷 0x8000)
                       cpu.c(GDT/TSS/IDT/PIT/LAPIC) kernel.ld(段边界符号 __text_*/__rodata_*/__data_* 供 W^X)
include/               全部头文件：abi/(lnxrm_abi.h + lnxrm_syscalls.def) sys/ mm/ 其余
kernel/                main.c(start_kernel 编排)
  core/                syscall.c(分发/门) print.c percpu.c smp.c protect.c selftest.c rtc.c(CMOS+TSC 演进)
  sched/               task.c(fork/execve/exit/wait4) sched.c(RR/锁) elf.c signal.c
  irq/                 isr.c(分发/异常/IPI) apic.c acpi.c
  mm/                  pmm.c(buddy+帧引用计数) kheap.c vmm.c(三槽地址空间+W^X) vma.c pagefault.c
                       filepage.c(文件映射共享页缓存) oom.c(OOM killer)
  fs/                  vfs.c blk_cache.c mbr.c initramfs.c
  fs/fat32/            fat32.c fat32_dir.c fat32_file.c fat32_meta.c fat32_journal.c
  drivers/             pci.c ide.c ahci.c uart.c ps2mouse.c framebuffer.c input_event.c
ktest/                 启动自测：ktest.c 框架 + 28 个 t_*.c + include/（在仓库根）
userland/              Makefile + bin/ 16 个程序 + lib/(crt0.S + ulib.c 运行库)
scripts/               patch_setup_jmp.py patch_bzimage.py mkinitramfs.py smoke_test.py
```

`userland/lib/` 是**每个**用户程序链接的运行库，不是 `/init`。

### 容量常量

| 常量 | 值 | 定义处 | 常量 | 值 | 定义处 |
|---|---|---|---|---|---|
| `MAX_CPUS`/`NR_TASKS`/`NR_FDS` | 8/64/16 | `include/types.h` | `MAX_ORDER`/`MIN_ORDER_BITS` | 11/12（buddy 最大 4 MiB） | `pmm.c` |
| `KSTACK_SIZE` | 32768 | `sys/sched.h` | `PMM_WINDOW_TOP` | `0x80000000`（2 GiB） | `mm/mm.h` |
| `LNXRM_NR_SIGNALS` | 14（编号 1..13） | `lnxrm_abi.h` | `RESV_MAX`/`KHEAP_NBINS` | 8/9 | `pmm.c`/`mm.h` |
| `LNXRM_SYSCALL_LAST` | 46（共 47 个） | `lnxrm_abi.h` | `CRED_USER_MEM_QUOTA_PAGES` | 64 MiB/4 KiB | `sys/cred.h` |
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
8. `fb_init → ps2_kbd_init → serial_irq_init → vfs_init → ide_init → ahci_init → vfs_try_mount_disk()`；根没就绪回退 `vfs_mount_initramfs()`。
9. `do_global_ctors()` → `sched_init`。
10. `ktest_run(KTEST_LATE)` + `ktest_summary()`。
11. `kernel_spawn("/bin/init")`；失败打 `Stop!` → `fb_flip()` → hlt 死循环。
12. `lapic_timer_calibrate`（BSP 一次）→ `smp_init` → `lapic_timer_start(32, HZ)`。
13. `[boot] ready in N ms` → `sti` → `idle_loop()`。

**手工同步的硬编码**：`arch/kernel.ld` / `arch/entry64.S`（页表物理地址）/ `kernel/mm/vmm.c`（同一组常量）三处只有注释互相提醒，改一处必改另两处；**第四条线**：`vmm.c` 还读 `kernel.ld` 的 `__text_end`/`__rodata_end`/`__bss_end` 决定 W^X 策略，`t_mm.c` 的 `test_vmm_kernel_wx` 钉住落点。

## 3. 内存

流水线：**E820 → buddy 物理页 → 分离空闲链内核堆 → 页表**（`kernel/mm/`）。

- **buddy（`pmm.c`）**：播种取最大 E820 可用区（钳到 `PMM_WINDOW_TOP`）；侵入式空闲链 + 逐 order 位图（物理 `BITS_BASE=0x10000`，`buddy_of = pa ^ size`）；`pmm_reserve` 只登记、播种时生效；每次真交出帧都调 `oom_frame_granted()`（OOM 闩）。
- **PMM 保留表（`main.c`，`RESV_MAX 8`，有帧缓冲 7 笔/无则 4 笔）**：LFB 物理、双缓冲窗 `[97,104)`、VGA/DEV/KHEAP/ACPI 固定窗——**凡固定窗占的别名槽，背后帧必须同时 reserve**。
- **内核堆（`kheap.c`）**：分离空闲链 9 箱（16…4096 字节），每分配 8 字节头校验 magic；**OOM 返回 `NULL` 必须检查**；16 MiB 映射在 `KHEAP_VMA 0xffffffffa0000000`。
- **三槽地址空间（`vmm.c`）**：PML4 槽 0 恒等（全 AS）、槽 255 用户空间 `[0x7f8000000000, 0x7ffffffff000]`（每进程独立）、槽 511 高半别名（全 AS）。`vmm_new_user_aspace` 复制除 255 外所有条目 ⇒ 内核映射零成本传播；`vmm_init` 别名环**跳过** VGA/DEV/ACPI/LFB/双缓冲固定窗。
- **内核 W^X（`vmm_kernel_wx()`，`vmm_init` 末步，2026-11-12）**：`PD_M`+`PD_HI` 覆盖 `[__kernel_start, ALIGN_UP(__bss_end))` 的 2 MiB 槽拆 4 KiB 按段赋权——text `PG_P`(R-X)、`.rodata` `PG_P|PG_NX`(R--)、其余（data/ktest/initramfs/trampoline/bss）`PG_P|PG_W|PG_NX`(RW-)；**两个别名一起改**，镜像外（1 MiB 以下低内存、共享槽 buddy 帧）保持 `P|W`——**AP trampoline 在 `0x8000` 从恒等映射执行，带 NX 会打死 SMP**；逐页 `invlpg`，日志一行 `[vmm] W^X: … both aliases`。
- **COW fork**：`dup_user_aspace` 共享帧，可写页清 `PG_W` 置 `PG_COW`（bit 9）；`page_cow_break` **先复制 → 再换 PTE → 最后放旧引用**；`user_ptr_writable` 是唯一 COW 解析门；widen 拒绝给 `PG_COW` 页回写 `PG_W`。用户页默认 `PG_NX`，**唯一 `exec=true` 是信号 restorer 与 `PROT_EXEC` mmap**。
- **#PF 三段分发 `page_fault_try_fix()`**：demand（先按 VMA prot 拒越权，再补帧）→ stack_grow（拒取指位）→ COW（只认写位）。**`err` 位必须逐位认**——否则 NX 取指被当成"COW 已破环" → 无限故障循环。
- **栈**：exec 只映 16 KiB，懒惰下扩（下限 512 KiB）；**64 KiB 守护区 `[MMAP_TOP, USER_STACK_TOP-USTACK_MAX_GROW)` 永不映射**，`STACK_GROW_WINDOW == STACK_GUARD` 静态断言；`stack_grow` 有 RSP 邻近检查（`cr2 + window < rsp` 拒）。
- **mmap/munmap/mprotect/brk**：六参 `mmap`(42) top-down 选址 `[MMAP_LO, MMAP_TOP)`、**只登记 VMA 靠需求分页补帧**；`brk` 上限 = `MMAP_LO`；配额唯一来源 `cred_mem_quota()`，页数真相 `vmm_count_user_pages()`。**文件映射**：`MAP_SHARED` 同一 `(file,offset)` 全系统一个帧（`filepage.c` 64 桶哈希 + 锁外读盘），`MAP_PRIVATE` 自有帧；写回只在 `sys_munmap`/`exec_commit`/`sys_exit` 三处，脏页由硬件 `PG_D` 认定。`mprotect`(44) 作用任意用户范围，**授予 W 永不分配**（共享帧标 COW），撤销 W 连 `PG_COW` 一起清。两个拷贝门遇 not-present 先补页再判。
- **OOM killer（`oom.c`，2026-11-12）**：触发面**只有用户缺页取不到帧**（`pagefault.c` 四个取帧失败点 + 空 buddy 的共享文件页）；**能回答的失败照旧只回答**（`brk` 回 `ENOMEM`、`kmalloc` 回 `NULL`）；受害者 = `mem_pages` 最大者（纯函数，排除 pid≤1/kxld/embryo/zombie/已带 SIGKILL）；**一次短缺只杀一个**，闩由 `oom_frame_granted()` 开；杀掉要等 exit+reap 才回池。
- ⚠️ `vmm_destroy_user_aspace()` 释放所有 present 数据页：**必须先 `vmm_unmap_user` 再 `pmm_free`**，否则还两次、空闲链毒化。

## 4. 进程、调度与信号

- **task 槽**：64 槽静态数组，槽 0 = BSP idle；**父链接是 `(pid, generation)` 不是指针**（槽位复用不认错人）；状态机 `UNUSED/EMBRYO/RUNNABLE/RUNNING/SLEEPING/ZOMBIE/STOPPED`；内核栈 32 KiB（顶 `intr_frame`，下 56 字节 `cpu_ctx`）；父链接 `(0,0)` = 无主僵尸标记，惰性回收在 `task_alloc_slot` 撞满表时触发。
- **fork**：COW 共享 + `vma_dup` 深拷 + fd 表 `refcnt++` + 伪造子帧（rax=0）。**execve**：先 `vmm_switch_to(new)` 再 `elf_load`，成功先赋 `current->pml4` 再销毁旧地址空间；只接受静态 `ET_EXEC`。**exit**：关 fd → **`fb_disown`（画屏归属唯一释放点）** → 孤儿挂 pid 1 → `SIGCHLD` → ZOMBIE。
- **调度**：每 CPU push-front **LIFO 队列**（非环形）、`quantum=6` tick=60 ms、无优先级；全局 `runq_lock`（irqsave）；**重排 current 必须走 `runqueue_add_local()`**；`switch_to` 只有 `pml4` 不同才切 CR3，idle 显式设 `master_pml4_phys()`；`swtch` 只存 7 个 u64。
- **信号**：14 槽编号 1..13，`SIGKILL 7`/`SIGSTOP 13` 硬置位且 KILL 醒 SLEEPING+STOPPED，默认终止集 6 个；`do_signal_check` 只在 `iretq` 前带 ring-3 守卫、一次一帧；restorer 页 9 字节 `mov eax,20; int 0x80; ud2`。
- **凭据**：`CRED_USER/ROOT/KXLD` 三级 + `CAP_KILL/SYS_ADMIN/FB`；`setuid` **单向降权**；`kxld_protect_kill()`：pid 1 与 KXLD 不可杀；规则都是纯函数。

## 5. 系统调用与 ABI

- 向量 `int 0x80`，编号 `rax`，参数 `rdi/rsi/r10`，返回 `rax`；`syscall_do` 查**一张 `sysents[]` 表** `{入口, 门, 门名}`——门与入口同一行，未知编号回 `-38` 且**先不问门**。
- **只追加不重排**：唯一定义源 `include/abi/lnxrm_syscalls.def`（X-macro），`lnxrm_abi.h` 逐编号编译期校验，`LNXRM_SYSCALL_LAST` 必须随末尾加一。
- 门：`GATE_ADMIN` 7 个 / `GATE_FB` 5 个，拒绝回 `EACCES=-13` 并打日志；**先能力门、后 `fb_owner_gate` 归属门**（顺序是硬的），`fb_info` 两道门都不过。走字符串的经 `copy_user_str`，走指针的调用方自校验——**没有统一前置检查**。
- ABI 结构体（`lnxrm_abi.h`）：21 个 errno、信号号、`lnxrm_timespec` 16B / `utsname` 320B / `ps_entry` 48B / `dirent` 72B / `fb_info` 16B，`_Static_assert` 锁布局；**ABI 一改用户态必须全量重编**。

### 47 项总表（编号 0–46，连续无空洞）

| # | 名字 | 要点 | 实现 |
|---|---|---|---|
| 0–4 | `read/write/open/close/lseek` | fd 读写；`open` 七标志；close 即同步脏扇区 | `fs/vfs.c` |
| 5 | `brk` | 用户堆 + 配额算术门 | `core/syscall.c` |
| 6–7 | `getdent/dup2` | 目录 cookie 迭代；复制 fd | `fs/vfs.c` |
| 8 | `nanosleep` | 信号打断回 EINTR + 余量 | `sched/sched.c` |
| 9 | `getpid` | 进程号 | `core/syscall.c` |
| 10–13 | `fork/execve/exit/wait4` | COW fork / 静态 ELF / 释放画屏 / WNOHANG | `sched/task.c` |
| 14 | `kill` | kxld / 同 uid / `CAP_KILL` 三道闸 | `core/syscall.c` |
| 15 | `uname` | 系统标识 | `core/syscall.c` |
| 16–17 | `sigaction/sigprocmask` | 处理器 / BLOCK·UNBLOCK·SETMASK | `sched/signal.c` |
| 18 | `getppid` | 无父为 0 | `core/syscall.c` |
| 19 | `ps` | 内核填任务表，`CAP_SYS_ADMIN` | `sched/task.c` |
| 20 | `sigreturn` | 恢复内核保存的帧 | `sched/signal.c` |
| 21 | `diskinfo` | 枚举块设备，`CAP_SYS_ADMIN` | `core/syscall.c` |
| 22 | `mkdir` | 建目录 | `fat32/fat32_meta.c` |
| 23 | `fb_info` | 帧缓冲几何（两道门都不过） | `core/syscall.c` |
| 24–27 | `fb_clear/fill/char/puts` | 绘图，`CAP_FB`+归属门 | `drivers/framebuffer.c` |
| 28–30 | `unlink/rmdir/rename` | 删文件/删空目录/改名 | `fat32/fat32_meta.c` |
| 31 | `setuid` | 单向降权 | `core/protect.c` |
| 32 | `fb_flip` | 离屏后缓冲推前台 | `drivers/framebuffer.c` |
| 33 | `sync` | 脏扇区交设备 | `fs/vfs.c` |
| 34–36 | `kbd_get/inject/info` | 弹键/代打/快照；get·inject 带 `CAP_SYS_ADMIN` | `irq/isr.c`+`core/syscall.c` |
| 37–38 | `mouse_get/info` | 弹指针事件/aux 环快照 | `core/syscall.c`+`ps2mouse.c` |
| 39–41 | `input_pop/info/wait` | 统一队列 24B 事件；pop·wait `CAP_SYS_ADMIN`，wait 阻塞+EINTR+ms 超时 | `drivers/input_event.c` |
| 42 | `mmap` | **六参** `addr,len,prot,flags,fd,offset`（4–6 参读 trap frame `rdx/rcx/r8`）：addr=0、top-down、只登记 VMA；`fd<0` 匿名，shared 走共享页缓存 | `core/syscall.c`+`mm/vma.c`+`mm/filepage.c` |
| 43 | `munmap` | VMA 挖洞拆分 + 逐页 unmap + 空页表回收；零相交 `EINVAL` | `core/syscall.c`+`mm/vma.c` |
| 44 | `mprotect` | 任意用户范围改 prot；洞 `ENOMEM`、未对齐 `EINVAL`；授予 W 不分配 | `core/syscall.c`+`mm/vma.c` |
| 45 | `stat` | 56 字节路径元数据，不发描述符；`st_dev` 挂载槽从 1 起，`ino` 与 getdent 同号 | `core/syscall.c`+`fs/vfs.c` |
| 46 | `gettimeofday` | `tv` 微秒（CMOS 采样一次 + TSC 演进，单调）、`tz` 恒写 0 | `core/syscall.c`+`core/rtc.c` |

统一输入队列（39–41）把两生产者的已解码结果合成一条流：`KEY` 只出自 IRQ1 解码（`kbd_inject` 不进队列），`REL/BTN/WHEEL` 出自 aux 包尾；34–38 的环与门原样保留。

## 6. 文件系统

层次：`vfs.c`（vnode/file/fd 表/路径解析/挂载）→ `blk_cache.c`（64 项时间戳 LRU + `blk_io_*` **唯一驱动调用点**）→ `mbr.c`/`initramfs.c` → `fat32/`（挂载/目录/文件/元数据/redo journal）。

- **VFS 对象**：`vnode` 每次 open 在栈上创建拷贝（无持久 inode，两 fd 的 `pos` 独立，`ino/dev/ro` 供 `stat`）；`file` 走 kmalloc + CAS 引用计数（不回绕、不二次 free）；`fs_ops` 12 钩子（`size` 供 `O_APPEND` 实时大小、`close_node` 由 fs 决定释放方式）。
- **路由**：`fs_for_path` 按**最长前缀 + 组件边界**选唯一挂载项（`"/"` 不吞 `/alt`），`ops` 与 `priv` 成对取自它；`vfs_umount` 只动路由**不 free priv**；跨挂载 `rename` 回 `EXDEV`；挂载入口只有 `vfs_mount_fs`/`vfs_umount`（**非 syscall**）。
- **错误契约**：`blk_io_*` 驱动非 0 原样上抛 → `fat_*_sector` 只有 0 或 `LNXRM_EIO` → 目录扫描三态 `1/0/负` → 语义层负数一路上传不被改写。对外 `[fat32] I/O error` + `-5`。
- **路径不变式（同一扇区只走一条路）**：数据/目录经块缓存，FAT 区 `m->fat_pages` 分帧直写/入日志，挂载期 `blk_io_*` 直读。**FAT 区永不进块缓存**（否则"目录项先于 FAT"的窗口）；FAT 表 4 KiB 分帧驻留，表项与扇区永不跨帧，访问只走 `fat_entry_get/put_raw/sector_ptr`。
- **块缓存**：64 项 × 512 B，键 `(dev,lba)`，时间戳 LRU；读失败作废 victim；**三道同步门**进 `blk_cache_sync_all()`——BSP 每秒 tick / 最后 fd close / `panic()` try 回写（外加 `SYS_sync(33)`），**写回失败保持脏**。
- **FAT32**：挂载校验 BPB（55AA、`"FAT32"`、簇因子 2 的幂、`bps==512`）、九处拒绝统一 `fat_reject()`；目录可用槽 = 尾槽 0x00 与已删 0xE5，**0x05 是活记录**；槽不够先扩链 ⇒ 目录永不满而 ENOSPC；getdent cookie = `(簇<<32)|字节偏移` 物理恢复点；LFN 校验 checksum + 序号递减，坏即回落 8.3；簇链 `FAT_CHAIN_MAX_STEPS` 步数预算 + 单簇自环即断。
- **redo journal**：reserved 空闲扇区，扇区 8 superblock（magic `JRN1`，`head` 是相对偏移）；locked wrapper `begin → *_impl → commit`，**失败路径也 commit**；顺序 ①flush 数据 ②写记录 ③superblock 置 **COMMIT（唯一提交点）** ④apply ⑤flush ⑥CLEAN，**④ 失败必须保留 COMMIT**；mount 时 `state==COMMIT` 幂等 redo。
- **挂载顺序**：`vfs_try_mount_disk` 逐盘先整盘 FAT 再 MBR 分区（每分区先 `blk_register` 再试）→ 分流（有 `/bin/init` 挂 `/`，余挂 `/mnt`）→ 都不成回 `vfs_mount_initramfs()`（newc cpio 只读根）。`blk_register` 满 8 槽**大声拒绝且表不变**，注册表拥有分区对象。

## 7. 驱动

- **串口/控制台**：16550 115200 8N1，COM1→IRQ4→向量 36；`console_putc` 扇出 fb→VGA→串口→4096 B 环形缓冲；`kvprintf` 持 `console_out_lock`(irqsave)；`panic()` 打印后 **`fb_flip_unlocked()` 再 `cli;hlt`**（双缓冲下再无 tick 呈现；无锁版防自死锁 T-002）。
- **键盘**：IOAPIC IRQ1→33，set 1 扫描码→ASCII 进 256 B `inbuf`，等待者 `con_next` 链表。**鼠标**：i8042 aux→IRQ12→44，3/4 字节包（含滚轮探测），64 槽事件环；内核不带屏上指针。**统一队列**（`input_event.c`）：63 槽 24 B 事件，**叠加不改道**（两旧环保留）；`input_wait(41)` 空队睡 wait list、生产者解锁后唤醒；ktest 钩子 `input_inject()`。
- **IDE**：PIO、主通道 `0x1F0`、单命令 ≤256 扇区；LBA28/LBA48 按最后扇区选（`>0xFFFFFFFFFFFF` 拒）；**写完发 `0xE7` CACHE FLUSH 并查返回**；轮询预算两档（`IDE_PROBE_ITERS`/`IDE_POLL_ITERS`，旧 10 万次预算会把 QEMU 正常命令饿成 EIO）。
- **AHCI**：pci_scan → 映射 ABAR → 复位 → 逐端口 → `blk_register`（多盘 `sd_names[8]`）；**中断驱动**（`GHC_IE`+`PxIM`+`ahciIsr`，轮询只作超时兜底）；完成判定**先查 `IS_TFES` 再查 `PxTFD.ERR`**；probe 单出口 `ahciPortRelease()` 兜全部失败路径。
- ⚠️ `make run` 默认 `MACHINE=q35`（AHCI 主路径）；`MACHINE=pc` 是 IDE，`t_ahci.c` 自动 skip；`smoke` 固定 pc + TCG + `-m 256`。
- **帧缓冲**：`FB_VMA 0xffffffff8d000000`（`PD_HI[104]`），双缓冲后缓冲窗 `[97,104)`（14 MiB，逐 4 KiB 帧拼虚拟连续）；**`pmm_reserve` 与别名过滤必须成对按整窗算**；**画的与看的不是同一块**——`fb_ptr` 指后缓冲，只有 `fb_flip` 拷脏区域。呈现：首 tick 前由写批次边界 present（一条消息一帧），tick 后 BSP 每 2 tick 节流、批次门永久关死；**逐行拷贝全程持 `console_out_lock`**（消撕裂），拷贝尾 `mfence`；绕过 `fb_store_px` 的写必须自己补标。LFB 写合并 `fb_memtype_program()` 每 CPU 一次（PAT WC 槽 + MTRR 快照/采纳，固件 UC 覆盖则如实落 UC）。失败即退回单缓冲，无开关。
- **归属**：`fb_owner` 用 CAS 而非 check-then-set；释放只在 `sys_exit`。

## 8. SMP 与 ACPI

- **ACPI**：RSDP→RSDT/XSDT→MADT，产 `acpi_collect_ap_ids()`；**表读取必须经 `ACPI_SCRATCH_VA` 弹跳窗**（`PD_HI[108]`，真机表常在恒等窗外）；`acpi_init` 在 `vmm_init` 后、`cpu_init` 前。
- **AP 拉起**（`smp.c`+`trampoline.S`）：trampoline 拷 `0x8000`、共享块 `0x9000`（**`ap_main` 传虚拟地址**）→ INIT+SIPI×3 → 轮询 ready 100 ms 超时。`ap_main` 顺序硬：`apic_enable` 最先（x2APIC 访 MMIO 会 #GP）→ GDT → GS → IDT → `cpu_protect_init` → idle → **`trampoline_signal_ready()` 必须先于一切控制台输出**（慢控制台吃光预算会被恢复 INIT 复位）→ LAPIC timer → online。
- **APIC**：xAPIC(MMIO)/x2APIC(MSR) 双路，每核各定型；只用于 INIT/SIPI/IPI（`0xF0` reschedule、`0xF2` stop）。
- **时钟与锁**：`HZ 100`，PIT 只供 LAPIC 标定；BSP LAPIC tick → `jiffies`/`sched_tick`/`fb_tick_flush`。锁表：`runq_lock`（全队列）、`task_table_lock`/`pid_lock`、`pmm_lock`/`kheap_lock`、`cache_lock`（**跨设备 I/O**）、`fat_fs_lock`（全局 FS 锁，与 cache_lock 嵌套）、`print_lock`（含 present 拷贝）、`fb_box_lock`（脏箱快照）。`fat_fs_lock` 是双核 FS 性能瓶颈。
- **中断**：统一 `intr_frame`；`isr_common` 三段分流——IPI→`apic_eoi`、`<32` 异常（ring-3 转信号，ring-0 panic）、`≥32` **先 `apic_eoi` 再调 handler**（handler 可能切走）；IDT 49–255 全指 catch-all（IPI 必须显式覆盖）；#PF 用 `rflags.AC` 区分 WP 与 SMAP。向量：32 timer、33 键盘、36 COM1。保护位 `EFER.NXE`/`CR0.WP`/`CR4.SMEP`/`CR4.SMAP` 由 BSP/AP 共同走 `cpu_protect_init()`，**内核镜像 W^X** 由 `vmm_kernel_wx()` 保证。

## 9. 自测

**ktest（`KTEST=1`；`make smoke` 强制，直跑 `smoke_test.py` 会绕过）**：框架 + 28 个 `t_*.c`，`KTEST(suite, stage, fn)` 注册，按 `EARLY/MM/LATE` 三阶段跑。当前判据（2026-11-12 q35）：

| 阶段 | cases | checks | failed |
|---|---|---|---|
| early | 25 | 408 | 0 |
| mm | 18 | 462 | 0 |
| late | 103 | 3652 | 0 |
| **合计** | **146** | **4522** | **0**（2 skipped、1 刻意 warn） |

33 个 suite：`abi ahci blk blkcache blkio boot console cpu cred draw elf fat32 fatfile fb input jrn kbd kheap ktest lib lock mbr mmap mouse oom pmm prop stat syscall task usercopy vfs vmm`——含 OOM killer 四例、**W^X 逐段逐页钉点**（`test_vmm_kernel_wx`）、`t_mmap.c` 十一例、`t_stat.c` 三例、AHCI 中断自测（无设备即 skip）。`MACHINE=pc` 同判据 0 failed。

**smoke（`make smoke`，六阶段）**：`toolchain → build → artifacts → boot（21 条有序 marker）→ shell（串口驱动 /bin/sh + systest 173 条 `SYSTEST_TAGS`）→ forged FAT`。信号与 ABI 常量从 `lnxrm_abi.h` **现读**（`abi_define()`，`past.*` 按 `SYSCALL_LAST` 现算），判据一律 `br""`；负面判据禁 `WARN(`/`[PANIC]`。当前 **270/270**，transcript 在 `build/smoke/serial.log`。

**systest（`userland/systest.c`）**：按 syscall 编号分节的全量自检（38 个 `t_*` 函数 + 54 行 main），末尾 `systest.done=1`，由 smoke 的 shell 阶段驱动。

## 10. 关键不变式

一条一行，改代码时逐条对：

- `.def` 是 syscall 编号**唯一**定义源：只追加不重排；未知编号一律回 `-38`；ABI 结构体一改用户态全量重编。
- 图形门顺序**先 `cap_gate` 后 `fb_owner_gate`**，两种拒绝都 `-13`；画屏归属释放**只在 `sys_exit` 一处**。
- `main.c` 的 `pmm_reserve` 与 `vmm.c` 的别名过滤**必须成对**，双缓冲窗 `[97,104)` 两边都按整窗算；`RESV_MAX 8` 已用 7。
- 物理窗口 ≤ `PMM_WINDOW_TOP`(2 GiB) = 恒等/高半别名范围；越界播种 = #PF，IDT 前 = 三重故障无痕复位。
- `console_init → gdt_init → gs → idt_init` 必须是头四步；`0x6F00` 哨兵必须按 **DWORD** 读。
- `kernel.ld` / `entry64.S` / `vmm.c` 三处页表与段布局硬编码手工同步；`vmm.c` 跟随 `kernel.ld` 段边界赋 W^X。
- 新地址空间只复制**除槽 255 外**的条目；槽 0/511 永远共享；只有 `pml4` 不同才切 CR3，idle 必须 `master_pml4_phys()`。
- 释放已映射用户帧必须**先 `vmm_unmap_user` 再 `pmm_free`**；COW 帧 `pmm_free` 只减 `frame_ref`、归零才回 buddy。
- `PG_COW` 只与清 `PG_W` **成对**出现；`!W !COW` 写保护必落 SIGSEGV；widen 永不给 `PG_COW` 页回写 `PG_W`；**撤写必须连 `PG_COW` 一起清**。
- COW 破环恒为**先复制 → 再换 PTE → 最后放旧帧**；`user_ptr_writable` 是 COW 解析的唯一门。
- mmap 窗 `[MMAP_LO, MMAP_TOP)`，`MMAP_TOP` = 栈地板 − `STACK_GUARD`，`brk` 上限 = `MMAP_LO`；**守护区永不映射**，`STACK_GROW_WINDOW == STACK_GUARD` 静态断言：堆/mmap/守护/栈两两不相交。
- `PG_W ⇒ pmm_refcount == 1`（授予写只给独占帧，共享帧标 COW 推迟复制）。
- #PF 的 `err` 位**逐位认**：`cow_fault` 只认写位、`stack_grow` 拒取指位——否则无限故障循环；demand **先按 VMA prot 拒、再取帧**（先问权限再动分页器）。
- 拷贝门遇 not-present **先补页再判**，该拒的照旧拒；别把「lazy」当「unmapped」。
- buddy 最大块 4 MiB；`kmalloc` OOM 回 `NULL` 必须检查；配额唯一来源 `cred_mem_quota()`，页数真相 `vmm_count_user_pages()`。
- **OOM killer 只在用户缺页取不到帧时触发**；能回答的失败永远只回答（`brk` 的 ENOMEM、`kmalloc` 的 NULL 一条不许杀）；一次短缺只杀一个，闩由帧真出手开。
- 改 `struct task` 必须**全量重编**（`cpu_info` 内嵌按值 `runq_head`）。
- 运行队列是 push-front **LIFO**；重排 current 走 `runqueue_add_local()`；`sched_wake_task` 不得持 `runq_lock` 调 `runqueue_add`（自死锁）。
- 信号只在 `iretq` 前带 ring-3 守卫处投递，一次一帧；**`current->tf` 只发布 ring-3 帧**（ring-0 帧用完即弃，发布出去 execve 写死帧 → 用户 `#UD`）。
- restorer 页是唯一 `exec=true` 的用户页。
- `blk_io_read/write` 是全内核唯一调 `dev->read/write` 的地方；**FAT 区永不进块缓存**；一个扇区只走一条读写路；FAT 表访问只走三个 helper。
- 路径路由只认**最长前缀 + 组件边界**，`ops`/`priv` 成对取自它；`vfs_close_file` 只减真有的引用；`vfs_umount` 不 free priv；`blk_register` 满槽大声拒绝且表不变。
- IDE 与 AHCI 的写都以 `0xE7` CACHE FLUSH 收尾并**检查返回值**。
- journal `commit` 第 ④ 步失败必须**保留 COMMIT**；失败路径也必须 commit。
- EOI 必须在 handler **之前**写；IPI 向量必须显式盖过 catch-all。
- AP 的 `ready` 必须先于一切控制台输出；trampoline 传的 `ap_main` 是**虚拟地址**。
- 自旋锁只用 irqsave 变体；`fat_fs_lock` 内嵌 `cache_lock`，加锁顺序不得颠倒。
- 双缓冲呈现只拷脏区域、**先写后标**，逐行拷贝全程在 `console_out_lock` 内、拷贝尾 `mfence`；绕过 `fb_store_px` 的写必须自己补标；首 tick 前只在写批次边界 present。
