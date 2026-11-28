# lnxrm 差距分析：已收口条目（原 GAP_ANALYSIS.md 第 6 节）

> 2026-11-24 从 `GAP_ANALYSIS.md` 拆出：那篇只留**仍然开放**的条目（§1–§5），已关闭的全部在本文。
> 编号 C1–C72 是稳定 ID（README 与代码注释有交叉引用），不重排；本文小节号沿用原文的 **§6**，散见各处的「第 6 节」即指这里。
> 各轮次的基线与新增摘要仍在 `GAP_ANALYSIS.md` 头部。

## 6. 已收口条目

一行一条，只记结论与关键坑；实现细节看对应 ktest 用例与 git log。

- **2026-10-25**：C16 写超卷拒 `ENOSPC`、C17 LFN 校验和/序号、C19/C20/C21 静默损坏三件（`O_TRUNC` 落盘、按槽写回、找不到 dirent 回滚 EIO）、C23 目录扩链、C27 三道同步门 + `SYS_sync`、C29 `bps==512`、C35 空写回 0、C38 BPB 校验、C40 close 两层释放、C41 访问位、C44 getdent 小缓冲 `EINVAL`。

- **2026-10-30 边界与校验**：C30 getdent 恢复点游标、C31 LFN 260 字符缓冲、C32 表项 >2^28 拒挂载、C33 `max_cluster` 上限、C34 resolve 全对象清零、C36 MBR/EBR 范围与链遍历；目录结束符回归规范（0x00 止看、0x05 活记录），单簇自环即断。

- **2026-11-04 挂载与设备**：C28 分区先注册入 `diskinfo`、C39 挂载 ops/priv 成对下传、C43 close CAS 计数不回绕、C47 最长前缀路由 + `vfs_umount`（不加 syscall，ABI 冻结）、C55 IDE 写后 `0xE7` FLUSH 查返回。

- **2026-11-05 中断 I/O 与容量**：C49 AHCI 中断驱动、C51 多盘挂载分流（`/bin/init` 挂 `/`、余挂 `/mnt`）、C53 IDE LBA48；#UD 根因修复（`isr_common` 只发布 ring-3 帧）；q35 幻影盘回收。

- **2026-11-06 双缓冲与 LFB 写合并**：呈现频率门只认首 tick、`present` 全程持 `console_out_lock`、`panic()` 改无锁 flip；每 CPU PAT/MTRR 写合并（固件 UC 覆盖则落 UC）。

- **2026-11-07 匿名 mmap / 缺页分发 / COW fork**：42/43 匿名映射 + VMA 登记 + 需求分页（`brk` 上限移到 `MMAP_LO`）、`page_fault_try_fix` 三段分发、COW 共享帧（先复制再换 PTE）。

- **2026-11-07 栈保护页与 mprotect**：64 KiB 守护区永不映射 + RSP 邻近检查；44 `mprotect` 作用任意范围、授予 W 永不分配（共享帧标 `PG_COW`）；#PF `err` 位逐位认；拷贝门先补页；修 `MMAP_LO` 1 TiB 错值。

- **2026-11-03**：FAT 表分帧驻留（挂载上限改由物理内存决定）；`sched_wake_task` 自死锁修复；`t_input` waitlist 补排空。

- **2026-11-08 大扫除 1、2 轮**：信号表压实成 1..13 无空洞；删 11 处零引用 include、6 处重复逻辑合并；`syscall_do` 表驱动化；`systest.c` main 拆分。

- **2026-11-09 大扫除第 3 轮**：`isr_common`/`ahciProbe`/`sys_execve`/`fat_mount`/`fat_scan_dir_at` 函数瘦身（输出逐字不变）；smoke `SYSTEST_TAGS` 按 `SYSCALL_LAST` 现算。

- **2026-11-09 文件映射**：42 扩成 Linux 六参（匿名 + `MAP_SHARED` 共享页缓存 + `MAP_PRIVATE` 自有帧）；校验全在选址之前；写回只在 munmap/exec/exit 三处，脏页由硬件 `PG_D` 认定。

- **2026-11-11 stat / gettimeofday**：45 `stat` 56 字节路径元数据（不发描述符，size 取实时值）；46 `gettimeofday` CMOS 采样一次 + TSC 演进，单调不回退。

- **2026-11-12 OOM Killer**：用户缺页取不到帧时杀持有最多用户页的进程，一次短缺只杀一个、闩由帧真出手开；能回答的失败照旧只回答。

- **2026-11-12 死代码删除**：`isr.c` 的 `irq==7||irq==15` 里 `intno==0xFF` 分支可证不可达并删除，判据不变。

- **2026-11-12 内核 W^X**：`kernel.ld` 段边界 + `vmm_kernel_wx()` 按段给镜像两个别名赋权（text R-X / `.rodata` R-- / data、bss RW-），镜像外低内存与共享槽保持 `P|W`（AP trampoline 可执行）；ktest 逐页钉点。

- **2026-11-13 PP0 批次**：C4 `pick_next` 队头读纳入 `runq_lock`；C62 运行队列改尾插 FIFO/RR（钉 LIFO 的用例同步改写）；C10 半修——`blkdev` 每设备 `io_lock` + FIFO 队列两段式入队/排空，删 `ide_lock`/`ahci_lock`；smoke 加 `--repeat N` 连跑门。

- **2026-11-13 PP1 C7 半修**：`cache_lock` 改纯元数据锁——fill 先占槽、写回先拷任务自己的弹跳缓冲，victim/purge 跳 `busy` 槽，`blk_io_*` 全移到锁外；tick 回写门改 `trylock`；`fat_fs_lock` 改 `spin_lock_irqon` 不接管中断状态。

- **2026-11-14 换链（LLVM 工具链）**：`ld`→`ld.lld`、`objcopy`→`llvm-objcopy`；删 `--no-warn-rwx-segments`；`kernel.ld` 把孤儿段 `.trampoline` 显式摆成输出段。

- **2026-11-14 构建提速（KTEST 对象分目录）**：`OBJDIR := $(BUILD)/obj$(KTEST)`，两模式并存，删旧 `.ktest-mode-<n>` stamp，切换 0.2 s。

- **2026-11-15 注释清理**：只动注释、代码零改动——删复述代码的废话、历史叙事与多处重复，保留结构横幅、行尾契约、测试 ID 与非显然的「为什么」。

- **2026-11-15 PP2 可观测性最小集**：新增 `klog.h`/`klog.c`/`backtrace.c`/`t_klog.c`。loglevel 四级、只过滤控制台、环恒录；崩溃环 96 槽 ×160 B，panic 路径锁一律 `spin_trylock`；backtrace 帧指针走链，`Makefile` 加 `-fno-omit-frame-pointer`。

- **2026-11-15 PP3 `pipe(47)`**：新增 `pipe.h`/`fs/pipe.c`/`t_pipe.c`；`LNXRM_SYSCALL_LAST` 46→47、`LNXRM_EPIPE=-32`（信号表无 SIGPIPE 号，只回 errno 不发信号）。

- **2026-11-15 PP4 shell `\|`/`<`/`>`**：只改用户态 `lsh.c`/`cat.c`，内核与 ABI 零改动。

- **2026-11-15 用户态收敛（`rn` 并入 `mv`、`sh` 改名 `lsh`）**：程序数 16→15，删 `rn.c`，`sh.c`→`lsh.c` 且 `init.c` 三处 `/bin/sh` 同步改，提示符 `root@lnxrm$` 不动。

- **2026-11-15 syscall 轮（`reboot`/`chdir`/`getcwd` + lsh `cd`/`pwd` + `ls` 无参修复）**：`LNXRM_SYSCALL_LAST` 47→50、`LNXRM_ERANGE=-34`，`reboot`(48) 挂 `GATE_ADMIN`，用户程序数 15→16；`ls` 无参改 `kgetcwd()` 回退 `"/"`。

- **2026-11-16 S0 `shm_open(51)`/`shm_unlink(52)` 共享内存对象**：新增 `include/sys/shm.h`、`kernel/fs/shm.c`、`ktest/t_shm.c`；`LNXRM_SYSCALL_LAST` 50→52。

- **2026-11-17 S1a `dipc_open(53)`/`dipc_recv(54)`/`dipc_send(55)` 驱动 IPC 通道**：新增 `include/sys/dipc.h`、`kernel/ipc/dipc.c`、`ktest/t_dipc.c`；`LNXRM_SYSCALL_LAST` 52→55。

- **2026-11-18 死代码删除：initramfs 整套 + `ide.c` + smoke 切 q35**：删 `kernel/fs/initramfs.c`、`scripts/mkinitramfs.py`、`vfs_mount_initramfs()` 回退及相关声明、`Makefile` 的 `initramfs` 目标族、`arch/kernel.ld` 的 `.initramfs` 段；删 `kernel/drivers/ide.c`，`SMOKE_QEMU_ARGS ?= -machine q35`，`main.c` 删 `ide_init()`。

- **2026-11-18 S1b 键盘通道 + ring3 驱动模块**：解码器单源——`include/abi/keyboard.h`，内核 `kernel/drivers/keyboard.c` 持自己的状态结构，导出 `kbd_process_sc()`/`kbd_decode_init()`。IRQ1 桩 ADDITIVE 不 REPLACE，`kbd_decode_init()` 排在 ktest 之后。模块 `userland/mod/keyboard.c` 不 mmap 环，成功路径不打印。`init.c` 新增 `spawn_modules()`，扫 `/mod`，`.km` 后缀大小写不敏感，一模块一子进程，pid 1 永不因此退出。

- **2026-11-18 S1b 第二半：鼠标通道**：`include/abi/mouse.h`，`lnxrm_mouse_decode()`，`ms_feed_byte` 逐字节等价，`t_mouse`/`t_input` 断言一字未改。IRQ12 桩镜像原始字节。模块 `userland/mod/mouse.c` 成功路径不打印。帧长自推：ring3 拿不到 `ms_dec.bytes`，按 3 字节读时 4 字节流在 flags 字节撞上滚轮字节，需两份证据才切 4 字节。

- **2026-11-18 修：`ps` 看不到 `/mod` 的驱动进程（`.km` 后缀丢失）**：`Makefile` 进 `disk.img` 前置；筛子改大小写不敏感；后缀挪到链期 `MOD_ELFS`，安装 `basename` 原样搬运（再补一次就是 `*.km.km`）。

- **2026-11-18 第二次修：`ps` 的 `NAME` 截断 + 启动期刷屏**：`TASK_NAME_LEN` 与 `ps_entry.name` 扩到 24；删成功路径打印，保留失败路径；唯一保留 `A tiny unix-like kernel in ASM + C!` boot 段第 20 条 marker（smoke 红线不可改）。

- **2026-11-19 S1c 解码器下沉到 ring3（键鼠）**：IRQ1/IRQ12 改为二选一——`dipc_push_if_listening()`，通道有 opener 则字节进环、内核不解码；无 opener 本地兜底。回传走 `dipc_create(name, sink)`。判据零改动。

- **2026-11-19 改名：键鼠文件全线加 `ps2-` 前缀**：模块 + 内核驱动 + 共享头，`Makefile` 自动发现，零规则改动。

- **2026-11-22 S2 ext2 换根（完整读/写文件系统）**：`disk.img` 由 FAT32 换成 ext2；`find_volume()` ext2 先于 FAT；新驱动 `kernel/fs/ext2/`（`ext2.c`/`block.c`/`inode.c`/`ext2.h`）；块层直走 `blk_io_*`（不经过 `blk_cache_*`）；`e2fsck -n` 是硬约束。FAT32 不拆，ktest 在 `kfatseed` 种子卷上跑。

- **2026-11-23 phase 5 间歇挂死收敛**：ext2 块层从 `blk_cache_*` 剥离为 `blk_io_*` 直写，超级块读取同步改直读，`ext2_finish()` 去掉 `blk_cache_flush`。

- **2026-11-23 O_TRUNC 不走 `ext2_finish()` 的真因**：`ext2_write()` 的 `n==0`（`O_TRUNC`）分支绕过了 `ext2_finish()`；修法：`O_TRUNC` 分支也走 `ext2_finish()`（与 `create`/`mkdir`/`unlink`/`rmdir`/`rename` 同一扇门）。

- **2026-11-24 phase 4 等待超时真因**：`ext2_read/write_data_block` 按 512 B 逐扇区调 `blk_io_*`，flush 放大；修法：整块单请求，两个函数各发一次 `blk_io_*(lba, sectors_per_block, ...)`。

- **2026-11-25 S3 动态链接与共享库**：`ld.so` + `libulib.so`，15 个 execve 程序全动态，init 保持静态；`sys_mmap` 加 `MAP_FIXED`，`elf.c` 加 `elf_interp_path` 等，`sys_execve` 双 `elf_load` + auxv 5 对。

- **2026-11-26 S3.1 ET_DYN（PIE）+ `AT_BASE`**：全部用户态映像改为 `-pie` 链接的 ET_DYN，init 保持静态 ET_EXEC；`elf.c` 新增 `elf_load_dyn()`/`elf_place()`，`exec_build_stack` 改签名，`AT_BASE = ibias`。

- **2026-11-27 S3.2 系统调用入口切 `syscall`**：用户态不再发 `int 0x80`，改走 `EFER.SCE → LSTAR → entry_SYSCALL_64`，参数约定切到 Linux 六寄存器 `rdi rsi rdx r10 r8 r9`；`int 0x80` 双门保留。

- **2026-11-28 S3.3 系统调用出口切 `sysretq`**：GDT Option A（Linux 同形），`STAR[63:48]=0x1B`，CS `0x1B→0x2B`，`tss_load(0x30)`；出口守卫只查 `f->rip` 规范性；`isr128` 出口保持 `iretq`。

- **2026-11-28 S3.4 `swapgs` 与 TLS（fs segment + `arch_prctl`，深度浅）**：`SWAPGS_IN/OUT` 宏（`cs & 3` 判，帧偏移 144）；`entry_SYSCALL_64` 无条件 `swapgs` + 两出口 `cli; swapgs`；`glue_first` 单次无条件 `swapgs`；`switch_to()` 进栈前存旧/装新 `IA32_KERNEL_GS_BASE` + `IA32_FS_BASE`；`cpu_set_gs_base()` 合成写 `IA32_GS_BASE`=per-CPU 块 + `IA32_KERNEL_GS_BASE=0`；`arch_prctl(56)`：SET_FS 写 `IA32_FS_BASE`，SET_GS 写 `IA32_KERNEL_GS_BASE`（绝不碰 `IA32_GS_BASE`），四操作门限 `USER_MAX_VMA`，未知码 `EINVAL`；`fork` 继承两段，`execve` 两段清 0。
