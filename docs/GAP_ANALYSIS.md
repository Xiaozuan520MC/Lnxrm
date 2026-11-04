# lnxrm 差距分析：现在缺什么、错什么、先修什么

> 对象：`lnxrm` —— x86-64 freestanding 单体内核（C + NASM），QEMU 为主目标。
> 读者：要决定下一步改什么的开发者。本文只回答三件事：**仍然开放的缺陷**、
> **与一个稳健内核之间的能力差距**、**先修什么**。
> 构建与运行见 BUILD.md；代码结构、文件清单与特性介绍见 CODEBASE.md。
>
> 已核对的基线：系统调用 41 个（0–40 连续无空洞），其中图形 6 个
> （`fb_info` / `fb_clear` / `fb_fill` / `fb_char` / `fb_puts` + `fb_flip`(32)），
> 输入 4 个（`kbd_get/inject/info` 34–36、`mouse_get/info` 37–38）加统一队列
> `input_pop/info` 39–40；ktest 120 cases / 3955 checks（2 skipped，1 条刻意告警）；
> `make smoke` 205/205（默认 `-smp 2`，`--smp 1` 同数）；`make` 零警告。
>
> 编号 C1–C72 是稳定 ID（README 与代码注释里有交叉引用），本文主体**只收仍然开放的条目**，
> 不重排编号；半修的条目在「后果」列注明现状；已关闭的条目一行一条记在第 6 节。

---

## 1. 成熟度一览

评级：0 缺失 · 1 骨架 · 2 部分可用有明显缺陷 · 3 可用且语义正确 · 4 稳健 · 5 可投产。

| 维度 | 分 | 现状与主要缺口 |
|---|---|---|
| 引导与启动 | 4 | QEMU `-kernel` 与 GRUB `linux` 双路径、VBE 级联选型、E820 采集；缺 GRUB 路径自动化（C68）、只用单个最大 E820 区（C69）、固定物理地址与 loader 指针未校验（C66/C67/C70） |
| 物理内存 | 3 | buddy 分配器、按进程记账（`ps` 的 MEM/PEAK）、普通用户 64 MiB 配额（`cred_mem_quota`）、管理窗口 `PMM_WINDOW_TOP` = 2 GiB；无回收、无 swap、无 OOM killer |
| 虚拟内存 | 3 | 三槽 PML4（内核映射对新进程零成本传播）、用户页 NX、懒栈增长（`page_fault_try_fix` → `stack_grow`）；无 `mmap`/`mprotect`/COW/需求分页，内核映射不带 NX（无 W^X） |
| 进程与信号 | 3 | `fork/execve/wait4/brk/kill`、`sigaction/sigprocmask/sigreturn`、`parent` 存 (pid, 世代) 并经 `task_parent()` 解析；无 CPU 时间限制 |
| 调度 | 3 | 每 CPU 运行队列、按空闲/队列长度的负载均衡、每 CPU 本地 LAPIC tick；队列实为 push-front LIFO（C62）、内核态不可抢占、无优先级/nice |
| SMP | 3 | ACPI MADT 枚举 + xAPIC/x2APIC 双路访问、AP 本地时钟与共享标定；全局锁 + 轮询 I/O 使延迟敏感场景不可用 |
| 系统调用 / ABI | 3 | 41 个编号（0..40）全部实现，用户指针一律 `copy_*_user` → `user_ptr_ok`（范围 + 逐页 present）；缺 `mmap`/时间类/`stat`/`pipe` |
| 并发正确性 | 2 | 终端打印锁、任务表锁、fd 引用计数、console 多等待者链表已就位；仍有跨设备 I/O 的两层 irqsave（C7/C10）、`pick_next` 无锁读（C4） |
| 设备驱动 | 2 | PS/2 键盘与鼠标中断驱动（IRQ1→键盘环、IRQ12→aux 事件环，串口 RX 中断已接）、AHCI 走 DMA + `clflush`/`mfence` + UC 映射、完成判定补查 `PxTFD.ERR`；AHCI/IDE 纯轮询无中断（C49）、多盘只注册第一块（C51），IDE 无 LBA48 命令（C53） |
| 文件系统 | 3 | FAT32 + on-disk redo journal（mount 时 replay，`ktest/t_journal.c`）+ 时间戳 LRU 块缓存 + 簇链步数预算；边界批次已收口（目录游标 C30、LFN 260 字符 C31、FAT 容量拒绝 C32、末条目可达 C33、短名终止符 C34、MBR 范围与 EBR 链 C36），目录结束符同时改回规范字义（0x00 止看，0x05 是别名转义/活记录）；vnode 泄漏（C40）已修；剩 mount 数据下传（C39）与分区注册（C28） |
| 图形 / 显示 | 3 | 帧缓冲控制台 + 5 个绘图调用 + `fb_flip`（默认双缓冲），`CAP_FB` 能力门 + 单 pid 归属门两道检查；无直线/圆等图元（暂不作为缺口） |
| 用户态 | 2 | 16 个静态 ELF64 程序 + 自带 crt0 与迷你 libc；受 `mmap` 缺失制约无法动态链接，跑不了第三方软件 |
| 安全模型 | 2 | `struct cred{kind,uid,caps}` + `CAP_KILL`/`CAP_SYS_ADMIN`/`CAP_FB` + `setuid` 单向降权；`EFER.NXE`/`CR0.WP`/`CR4.WP`/`SMEP`/`SMAP` 均置位并有 ktest 钉点；无 gid、无 RLIMIT、无文件属主、无登录/沙箱、无 KASLR、无栈保护 |
| 测试 | 3 | ktest 120/3955（含属性测试、随机流暴力用例、`blk_inject_io` 故障注入、OOM 注入、统一输入队列五例）、`make smoke` 205/205（默认 `-smp 2`，`-smp 1` 用 `SMOKE_FLAGS="--smp 1"` 手跑另一套标记）；无 CI、无宿主侧单测与 sanitizer |
| 可观测性 | 2 | 串口日志、启动分阶段消息、`ps` 内存列（填用户缓冲）、块缓存统计；无 loglevel、无崩溃环形缓冲、无 backtrace/寄存器转储、无 `/proc`，`panic` 最终 `hlt` |
| 可移植 / 启动安全 | 2 | 门控参数固定为 QEMU pc + TCG + `-m 256`；q35+KVM（`make run` 默认）、GRUB ISO、内存档位靠手动跑；无 KASLR/KPTI、入口无 `swapgs`、无 IST |

---

## 2. 开放缺陷

列含义：**位置** = 文件与函数；**后果** = 现在会发生什么；**修法与量级** = 思路与预计投入。
"半修"表示主体已落地、余项仍开放，同样收在这里。

### 2.1 并发与调度

| 编号 | 位置 | 后果 | 修法与量级 |
|---|---|---|---|
| C4 | `kernel/sched/sched.c` `pick_next()` | 不持 `runq_lock` 读 `cpu->runq_head.rq_next`，而跨核入队/出队会写同一条链，可能读到半更新状态 | 把读纳入 `runq_lock`（临界区只有几次指针写），约 10 行，配双核压测 |
| C7 | `kernel/fs/blk_cache.c`（`cache_lock`）+ `include/fat32.h` `FAT_ENTER()` | 缓存锁与 `fat_fs_lock` 两层 irqsave 嵌套且跨越设备 I/O：任一 CPU 碰缓存或文件系统就关中断自旋，慢盘把等待放大到全机 | 元数据与 I/O 分离（取条目 → 解锁 → I/O → 重新加锁回写），约 2 天 |
| C10 | `include/sys/vfs.h` `struct blkdev`；`ide.c` / `ahci.c` 设备锁 | 块设备只有全局锁，无请求队列、无合并、无取消：并发磁盘访问完全串行且不可中断 | 多 slot 请求队列 + 每设备锁，约 300 行 |
| C62 | `kernel/sched/sched.c` `runqueue_add`（插队首）、`include/sys/percpu.h` | 队列是 push-front（注释已如实描述为 LIFO），持续争用下短任务可能被长队列饿死；无亲和性 API | 队尾指针 + 尾插 + 公平性用例，约 20 行 |

### 2.2 存储与文件系统

| 编号 | 位置 | 后果 | 修法与量级 |
|---|---|---|---|
| C28 | `kernel/fs/vfs.c:660`；`kernel/fs/mbr.c` | 分区设备从不 `blk_register`，`diskinfo` 看不到分区；`blk_count` 满 8 之后新设备进不了 `blk_list` 却仍会顶掉 `blk_first` | 分区注册 + 满槽一致处理，约 1 天 |
| C39 | `kernel/fs/fat32/fat32_file.c:15,32` | vnode 的 `mnt_data` 仍是 `(void*)1` 哨兵，`fat_freespace` 没有调用者：mount 间接层是装饰，多挂载没有基础 | 挂载数据真正下传（与 C47 一起做），约 2 天 |

### 2.3 VFS 与文件语义

| 编号 | 位置 | 后果 | 修法与量级 |
|---|---|---|---|
| C43 | `kernel/fs/vfs.c:293-298` | `vfs_close_file` 对 refcnt 没有下溢保护，完全依赖 `sys_close` 先清 fd 槽这条调用序 | 饱和判断 + 断言，几行 |
| C47 | `kernel/fs/vfs.c:19,45-98,704` | 挂载表只注册过一个 `"/"`，`fs_for_path` 的"无前导斜杠"分支不可达：多挂载与卸载路径从未运行 | 第二挂载点 + `umount` + 用例，约 1 天 |

### 2.4 设备驱动

| 编号 | 位置 | 后果 | 修法与量级 |
|---|---|---|---|
| C49 | `kernel/drivers/ahci.c`（`GHC_ARE`，无 `GHC_IE` / MSI / ISR） | AHCI 纯轮询：每条命令在 irqsave 锁里自旋等完成，多核下与 C7 相互放大 | 中断路径（`P_IE`、`HBA.IS` 排空、ISR）约 1 周；或把 AHCI 标为实验性、默认只走 IDE，约 1 小时 |
| C51 | `kernel/drivers/ahci.c:694-716,959` | **半修**：ABAR 已映射 8 KiB、容量读不出即拒绝注册；仍未校验 BAR 类型（假定 64-bit memory BAR）、不拒 `abar == 0`，且第一块盘注册后 `return 1` —— 多盘只认一块、盘名硬编码 `sda` | BAR/`abar` 校验 + 多盘注册，约 1 天 |
| C53 | `kernel/drivers/ide.c` | **半修**：容量按 48 位 IDENTIFY 读、LBA28 超界回 `EFAIL`；`READ/WRITE SECTORS EXT`(0x24/0x25) 未实现，>128 GiB 盘仍读不到 | 补 EXT 命令路径，约 1 天 |
| C55 | `kernel/drivers/ide.c:54-61,126-127` | 写路径的"flush"只等 DRDY：不发 `0xE7` CACHE FLUSH 且忽略返回值，断电时盘内写缓存可能丢失；`ide_wait_ready` 仍不判 `IDE_SR_ERR`（有意保留：发命令前判错会把上一条的 ERR 变成死等，错误已由命令后的 `ide_wait_drq` 兜住） | 写后发 `0xE7` 并检查返回，约 30 行 |
| C56 | `kernel/drivers/pci.c`（文件头注释、`:37-38`） | **半修**：visited 位图 + `PROBE_MAX_DEPTH 16` 已加；`0xCF8/0xCFC` 对所有总线拼同一 type-1 地址，而该机制只对 bus 0 有架构保证；文件头注释仍写"既无 visited 也无深度限制"，与紧邻的实现矛盾 | 按总线选择配置机制 + 改注释，半天 |

### 2.5 内存

| 编号 | 位置 | 后果 | 修法与量级 |
|---|---|---|---|
| C57 | `include/mm/mm.h` `PMM_WINDOW_TOP`；`kernel/mm/pmm.c:24,139-146` | 位图从固定物理 0x10000 顺序排布：管理窗口一旦再扩大，位图会撞上 `arch/entry64.S` 固定在 0x50000 的页表；这是个只写在注释里的隐式依赖 | 位图范围随窗口一起核算或搬移 + 断言，半天 |
| C61 | `kernel/mm/vmm.c:57-84` | **半修**：已检查既有 PTE 并只做放宽（不再覆盖），原问题基本关闭；`invlpg` 仍打在当前 AS 而非目标 root，目标不是当前 AS 时刷新无效 | 目标为当前 AS 才 invlpg，或把刷新交给调用方，几行 |

### 2.6 引导与架构

| 编号 | 位置 | 后果 | 修法与量级 |
|---|---|---|---|
| C66 | `kernel/main.c:61-90` | `boot_params` 指针只判非零、`e820_entries` 只截断到 64，指针本身不校验范围；`:96` 的注释却称该指针"已校验" | 指针落在已知 RAM 内才使用 + 改注释，约 20 行 |
| C67 | `kernel/main.c:208-216` | 没有 loader 参数时读固定物理 `CMDLINE_QEMU_PA`（0x20000）当命令行，依赖"那块内存恰好是 0" | 只在 loader 声明时读 + 长度上限，约 20 行 |
| C68 | `Makefile` `imageiso`；`scripts/smoke_test.py` | GRUB/ISO 路径只手工跑过，未进 `make smoke`；smoke 固定 `-m 256` + pc + TCG，内存档位、q35+KVM（`make run` 的默认配置）、IDE+AHCI 双盘都不在门控里 | smoke 矩阵化参数，约 1 天 |
| C69 | `kernel/mm/pmm.c:96-112` | 只用单个最大 E820 区，且每区钳到 `PMM_WINDOW_TOP`（2 GiB）：其余 RAM 完全不参与分配 | 合并全部区域 + 分段别名，约 2 天 |
| C70 | `arch/entry64.S:4-7` | 固定物理 0x50000–0x57000 的页表从不与 E820 校验冲突 | 启动早期校验并拒绝冲突布局，约 20 行 |

### 2.7 未定位的观察项

| 编号 | 位置 | 后果 | 修法与量级 |
|---|---|---|---|
| OBS-1 | `ktest/t_cache.c` `test_write_readback_is_coherent`；`userland/sh`；2026-10-21 一次连跑 | 历史各出现一次失败（伴随 FAT/dir `I/O error`；用户态一次性 `#UD`），重跑通过。其中 I/O 类的常见根因已定位并修掉：IDE 轮询预算（旧 10 万次 port read）短于 QEMU 在 vCPU 线程同步落宿主盘时的 BSY 保持，正常命令被判 EIO（改 `IDE_POLL_ITERS`，旧预算 A/B 必现）；修后连跑 16/17 全绿，余 1 次（3 用例 5 断言，伴 fs 用例整段 skip）未复现；`#UD` 仍未复现 | 连跑 N 次的抖动门 + 失败时保存串口现场，约 1 天 |

---

## 3. 能力差距

按主题分组。每项格式：**缺什么 → 影响谁 → 量级估计**。

### 3.1 进程间通信（IPC）

| 缺口 | 影响 | 量级 |
|---|---|---|
| `pipe` 与 shell 管道 | shell 只能单命令，工具无法组合 | 约 1–2 天（内核 pipe + `userland/sh.c` 的管道解析） |
| 共享内存（`mmap(MAP_SHARED, fd)` 或 shm） | 高吞吐进程间交换只能靠文件读写 | 依赖 mmap，约 2 天 |
| unix domain socket | 最完整的 IPC，服务/客户端模型的前提 | 建议最后做，约 1 周 |
| 线程 / `clone` | 进程模型是单线程假设，多处并发结论以此为前提 | 引入需重审并发结论，约 2 周 |
| `alarm` / 定时信号 | 无法做超时与定时唤醒，只有 `nanosleep` | 约 0.5 天 |

### 3.2 内存映射与 mmap

| 缺口 | 影响 | 量级 |
|---|---|---|
| `mmap` / `munmap` / `mprotect`（匿名先行） | 最大单点缺口：无共享库、无晚绑定、无 JIT、不能映射文件；`brk` 是唯一扩内存途径且受 64 MiB/用户配额限制 | 约 600–1000 行（3–4 周）；落地必须复用 `cred_mem_quota()`，否则配额从"地址空间上限"退化成"堆上限" |
| 页错误分发只有一个 fixer | `page_fault_try_fix` 目前只调 `stack_grow`：无需求分页、无 COW 缺页、无文件映射缺页 | 分发框架已就位，补 fixer 约 1 周 |
| COW `fork` | `dup_user_aspace` 永远深拷贝整个用户地址空间，成本随地址空间线性增长 | 约 150 行 + 专项用例（依赖 `mprotect`） |
| 栈 guard page | 已有懒增长（`ustack_lo`）但无保护页：越过下界的越界写不被拦截 | 约 0.5 天 |
| OOM 策略与回收 | 无 OOM killer、无 swap/reclaim：内核侧 `kmalloc` 失败没有受害者选择也没有归因 | 约 150 行（启发式杀最大进程，或内核分配按进程记账） |

### 3.3 权限与隔离

| 缺口 | 影响 | 量级 |
|---|---|---|
| `gid` 与文件属主（inode 元数据） | 任何进程都能 `write`/`mkdir`/`unlink` 任意文件：文件级权限不存在 | dirent/vnode 加属主字段 + 写操作检查点，约 200 行 |
| CPU 时间限制 | 普通进程可无限占 CPU；内存侧只有 64 MiB/用户配额 | tick 累计 + 超限终止，约 80 行 |
| RLIMIT 语义（fd 数、栈、core） | `NR_FDS 16` 是编译期常量，`ulimit` 概念不存在 | 约 100 行 |
| 登录 / 会话模型 | `setuid` 只能由 root 单向往下：没有"天生的普通用户"，冒烟里的程序仍以 root 跑 | 身份来源 + 会话概念，约 1 周 |
| 文件系统级沙箱 | 无 chroot/namespace：进程能触达整个根文件系统 | 建议在权限与 mmap 就位后再评估 |

### 3.4 启动安全与内核加固

| 缺口 | 影响 | 量级 |
|---|---|---|
| KASLR | 内核固定链接在 `0xffffffff80100000`，地址完全可预测，gadget 定位零成本 | 加载偏移 + 全局指针修正，约 100–200 行 |
| 内核镜像 W^X | 高半区与恒等映射用 2 MiB 页 `PG_P \| PG_W`，从不带 `PG_NX`：内核 text 可写可执行 | 分段加 NX，约 10 行加逐段核对 |
| 栈保护 | Makefile 两处 `-fno-stack-protector`，安全性靠"当前没有危险的变长写"这条纪律 | canary 放取 + 入口校验，约 50 行 |
| `swapgs` 与 GS 契约 | 入口/返回从不 `swapgs`：今天安全是因为用户态没有任何途径改 GS base（无 `arch_prctl`，WRMSR 是特权指令），但用户态也因此没有 TLS；加 TLS 必须同时引入 | 入口与返回各 2 行 + 契约检查，约 30 行 |
| IST / 双故障栈 | 无独立故障栈：引导栈 16 KiB 无 guard，栈被打穿后下一个异常落在被毁的 IDT 上 | TSS IST 表 + 双故障/NMI 栈，约 80 行 |
| KPTI | 三槽设计让每个用户页表都带完整内核映射（Meltdown 类攻击面） | 按需：syscall 进出换 CR3，先量代价再决定 |

### 3.5 CI 与自动化

| 缺口 | 影响 | 量级 |
|---|---|---|
| CI 配置 | 全树无 `.github/` 等，门控只在本地手动 `make smoke` | 一条 QEMU 无头 workflow，约 1 天 |
| smoke 矩阵 | 参数固定（pc + TCG + `-m 256` + 三档 CPU 数）：q35+KVM、内存档位、GRUB ISO、双盘都不覆盖（C68） | 矩阵化参数，约 1 天 |
| 宿主侧单测与 sanitizer | 只有内核态 ktest；纯函数（`fb_pack`、FAT 解析、LBA 计算）无法在宿主上带 ASan/UBSan 跑 | 抽纯函数 + 构建开关，约 2 天 |
| 抖动与压力门 | 门控跑一轮，并发/时序类缺陷可能漏过（OBS-1） | 连跑 N 次 + fork/IO 压测，约 1 天 |

### 3.6 抢占与延迟敏感

| 缺口 | 影响 | 量级 |
|---|---|---|
| 系统调用全程关中断 | `int 0x80` 与异常走中断门（`arch/cpu.c:104` 的 `0x8E`/`0xEE`）→ 整个 syscall 期间 IF=0：一次慢盘 I/O 期间本核无时钟、无抢占 | 改陷阱门或逐路径开中断，约 1 天加逐调用点审计 |
| 全局锁跨 I/O（C7、C10） | 任一核持锁期间其他核关中断自旋 | 见 2.1 |
| 内核态不可抢占 | `sched_maybe_preempt`（`kernel/sched/sched.c:297-309`）只对 ring-3 与 idle 生效：内核态长临界区拖住整核 | 抢占点 + 重入审计，约 1–2 周 |
| LIFO 队列与无优先级（C62） | 无真正 RR、无 nice/实时类、无亲和性接口 | 见 2.1 |
| 轮询块 I/O（C49） | AHCI/IDE 都是轮询 + 全局锁，磁盘延迟直接进关键路径 | 见 2.4 |

### 3.7 可观测性

| 缺口 | 影响 | 量级 |
|---|---|---|
| loglevel 过滤 | 无法降噪，关键行被淹没 | 约 50 行 |
| 崩溃环形缓冲 | panic 与复位后串口内容无从回读（`panic` 最终 `hlt`） | 约 100 行 |
| backtrace 与寄存器转储 | 异常只打印帧的少数字段，`panic` 没有调用栈 | 栈帧指针约定，约 150 行 |
| `/proc` 式查询 | 无 `/proc/meminfo`、`/proc/self/maps`，内核状态只能靠 `ps` 与日志 | 约 200 行 |
| debug 系统调用 | 用户态无法断言内核状态，黑盒测试受限 | 约 100 行 |
| `reboot` / `halt` 系统调用 | `panic` 后永久停机，QEMU 必须加 `-no-reboot` | 约 30 行 |
| 统计计数器 | 块缓存已有 hits/misses/evictions，分配失败、I/O 错误、页错误、调度次数无处可看 | 约 80 行 |

### 3.8 其他 syscall 覆盖与资源限制

| 缺口 | 影响 | 量级 |
|---|---|---|
| `stat` / `fstat` | `ls` 只有 dirent 一个元数据来源：无大小、时间戳、类型判定 | 把 `vnode` 字段落成 ABI 结构，约 2 天 |
| `gettimeofday` / `clock_gettime` | 用户程序完全无法测量时间（只能睡）；per-CPU 时钟源与 TSC 标定已就绪 | 约 100 行 |
| `chdir` / `getcwd` / `umask` | 没有进程工作目录，相对路径靠用户态硬拼 | 约 1 天 |
| `fcntl` / `poll` / `select` | 无法做非阻塞**多路**等待，`console_read` 是单读者阻塞；非阻塞输入只补了单键探针（`kbd_get` 34）、指针事件探针（`mouse_get` 37）与统一队列单事件探针（`input_pop` 39，成功回 1、空回 `EAGAIN`） | 约 2–3 天 |
| `chmod` / `link` / `symlink` | FAT32 无权限位与硬链接，软链可做 | 按需，各半天 |
| 固定容量：`NR_TASKS 64`、`NR_FDS 16`、`MAX_MOUNTS 4`、缓存 64 槽 | 编译期常量，用满即失败且诊断有限 | 动态扩容或可配置，约 1 天 |

### 3.9 存储与驱动能力

| 缺口 | 影响 | 量级 |
|---|---|---|
| 分区设备可见性（C28） | `diskinfo` 与挂载看不到分区 | 见 2.2 |
| 块请求队列 / 合并 / 中断 I/O（C10、C49） | 串行、轮询、不可取消 | 见 2.1 与 2.4 |
| IDE LBA48 命令（C53） | >128 GiB 盘读不到 | 见 2.4 |
| AHCI 定位决策 | 完成判定、缓存一致性、FLUSH CACHE 已到位，余下 C49/C51 | 二选一：补中断与多盘（约 1 周），或标实验性、默认只走 IDE（约 1 小时） |
| `umount` 与显式同步 | 卸载只能靠 journal 提交点，没有用户可见的落盘边界 | 约 1 天 |

---

## 4. 安全定性

| 场景 | 结论 | 依据 |
|---|---|---|
| 跑自己写的 `userland/` 程序 | 安全 | `sigreturn` 恢复内核保存的帧（不读用户栈，CS/SS 不经用户之手）；ELF 装载七项校验；syscall 指针一律 `copy_*_user` → `user_ptr_ok`（范围 + 逐页 present）；表外编号回 `ENOSYS`；用户态异常只杀进程；`kprintf` 格式串全是字面量且无 `%n` |
| 跑不完全信任的用户程序 | 不安全 | 权限止于 `uid` + 3 个能力位（无 gid / 文件属主 / CPU 限额 / 沙箱）；内核全程关中断加一把全局 FS 锁，一个 bug 可拖死整机；内核镜像可写可执行、无栈保护、无 KASLR |
| 挂载可信磁盘 | 基本安全 | on-disk redo journal + mount replay，故障注入覆盖提交点前后两个窗口，块缓存时间戳 LRU，簇链有步数预算；静默损坏三件已修（C19/C20/C21：`O_TRUNC` 落盘、写回按槽位定位、找不到 dirent 即 EIO 回滚）；vnode 泄漏（C40）已修；脏页三道同步门已就位（C27：BSP tick 每秒 / 普通文件 close / panic try 回写，外加 `SYS_sync(33)`；崩溃窗口从 32 KiB 收窄到「上次同步以来写过扇区」） |
| 挂载不可信或可能损坏的磁盘 | 不安全 | 簇链步数预算（`FAT_CHAIN_MAX_STEPS`）+ 单簇自环即断（本批新增）拦住坏链挂钩；BPB 三项校验（55AA / `"FAT32"` / 簇因子 2 的幂）、目录游标（C30）、LFN 长度（C31）、FAT 容量（C32/C33）、短名终止符（C34）、MBR 范围与 EBR 链（C36）、目录结束符回归规范字义（0x00 止看，尾槽后的残留字节浮不出伪记录）均已收口；仍不安全是因为有界长走持 irqsave 锁、无 checksum、目录条目内容仍逐扇区信任读回 |
| 硬件缓解 | 已开但不完整 | `EFER.NXE`、`CR0.WP`、`CR4.WP`、`SMEP`、`SMAP` 全部置位，BSP 与 AP 都走 `cpu_protect_init()`，启动日志打印 `EFER.NXE` 并有 ktest 钉点；缺 KASLR、KPTI、canary、内核 W^X、IST |
| 横向定位 | 与 `xv6` 同档 | 同为简单权限模型、同无 KASLR；`sigreturn`、ELF 解析、`copy_user` 三处比常见教学实现更严谨 |

---

## 5. 优先级清单

一行一项，按依赖顺序排列；括号内引用第 2、3 节的编号。

1. 锁与抢占：先 C7（锁外做 I/O），再 C10（请求队列），然后 C62/C4（RR 与队列读），最后 3.6 的内核态抢占与 syscall 关中断——每一步都要有 smoke 兜底。
2. 验证面：smoke 矩阵化（C68：ISO、内存档位、q35+KVM、双盘）+ 连跑抖动门（OBS-1），再上 CI（3.5）。
3. 可观测性最小集：loglevel + 崩溃环形缓冲 + backtrace（3.7）——此后并发改动才可诊断。
4. 内核自身加固：内核 W^X、canary、`swapgs`/IST、KASLR（3.4）——排在测试与可观测性之后。
5. 文件权限与限额：inode 属主 + 写检查点 + CPU 时间限制（3.3）。
6. 驱动收口：AHCI 二选一（补 C49/C51 或标实验性），IDE LBA48（C53）与写后 flush（C55），PCI 配置机制与注释（C56）。
7. 能力面：`mmap` 家族（3.2）→ COW 与需求分页 → `stat`/时间类 syscall（3.8）→ `pipe`（3.1）。
8. 暂不做：新子系统、更换文件系统、POSIX 全覆盖、网络栈——上面 1–7 未落地前不加宽度。


---

## 6. 已收口条目

本文已关闭的 C 编号，一行一条，只记结论；实现细节看对应 ktest 用例与 git log。

### 2026-10-25 批次

- **C16** 写需求超卷：`fat_chain_cap_bytes()` 作上界，越界先拒 `ENOSPC`、零分配。
- **C17** LFN 读路径整段清零 + 校验和 / 序号连续性校验。
- **C19/C20/C21** 静默数据损坏三件：`O_TRUNC` 落盘并释放簇链；写回按目录簇+槽位定位+时间戳校验；找不到 dirent 即 `EIO` 并回滚。
- **C23** 目录槽不足先扩链再落位，永不吃目录末槽（保住终止符）。
- **C27** 脏页三道同步门（每秒 tick / 最后 fd close / panic try 回写）+ `SYS_sync(33)`；崩溃窗口收窄到「上次同步以来写过的扇区」。
- **C29** `bps == 512` 锚定在挂载层（两条 512 字节栈缓冲的不变量）。
- **C35** `write(buf, 0)` 返回 0，空文件不再把"没有簇"误报成 `EIO`。
- **C38** BPB 三项校验：55AA / `"FAT32"` / 簇因子 2 的幂。
- **C40** close 释放两层分配：`fs_ops.close_node`（FAT kfree resolve）+ vnode 拷贝。
- **C41** 访问位生效（`O_RDONLY` 拒写、`O_WRONLY` 拒读 → `EBADF`），`O_APPEND` 落在实时 size（`fs_ops.size`）。
- **C44** getdent 缓冲装不下一条记录即 `EINVAL`，忙等源头消灭。

### 2026-10-30 边界与校验批次

- **C30** getdent 游标 = `(簇<<32)|字节偏移` 物理恢复点，引擎按扇区步进。
- **C31** LFN 拼接缓冲 260 字符 + 终止符。
- **C32** FAT 表项数用 u64，`>2^28` 挂载前拒绝并在日志点名几何。
- **C33** `max_cluster` 改独占上限，最末 FAT 格可达、链尾可读。
- **C34** `resolve` 全对象清零 + 名字逐字节拷贝带终止符。
- **C36** MBR/EBR 表项 `lba+count <= 盘容量` 才收录，EBR 链真正遍历（上限 32 段）。
- 目录结束符回归规范字义：**0x00 止看**、**0x05 是活记录**（8.3 别名对 0xE5 的转义）；`fat_chain_advance` 单簇自环即断 + `chain loop` 告警。

### 2026-11-03 批次

- **FAT 表分帧驻留**：每帧 4 KiB `pmm_alloc` + 128 KiB bounce 散射读；挂载上限从「kmalloc 4 MiB 连续块」变为「全部空闲帧」，硬上限仍是 2^28 入口（1 GiB 表）。分配器回基线（`MAX_ORDER 11`、kmalloc 4 MiB）。
- **`sched_wake_task` 自死锁**：原实现持 `runq_lock` 再调 `runqueue_add`（内部再拿同一把锁）→ 对齐 `send_signal` 的免锁写法。
- **ktest `t_input` waitlist**：用例开头补 `evq_drain()`（t_mouse 生产者路径把共享队列灌满）。
