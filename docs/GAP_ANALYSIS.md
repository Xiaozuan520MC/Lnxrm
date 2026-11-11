# lnxrm 差距分析：现在缺什么、错什么、先修什么

> 对象：`lnxrm` —— x86-64 freestanding 单体内核（C + NASM），QEMU 为主目标。
> 读者：要决定下一步改什么的开发者。本文只回答三件事：**仍然开放的缺陷**、
> **与一个稳健内核之间的能力差距**、**先修什么**。
> 构建与运行见 BUILD.md；代码结构、文件清单与特性介绍见 CODEBASE.md。
>
> 已核对的基线：系统调用 47 个（0–46 连续无空洞），其中图形 6 个
> （`fb_info` / `fb_clear` / `fb_fill` / `fb_char` / `fb_puts` + `fb_flip`(32)），
> 输入 5 个（`kbd_get/inject/info` 34–36、`mouse_get/info` 37–38）加统一队列
> `input_pop/info` 39–40、阻塞 `input_wait` 41、`mmap`/`munmap` 42–43
> （42 已扩成 Linux 六参 `addr,len,prot,flags,fd,offset`：匿名 + 文件映射
> `MAP_SHARED`/`MAP_PRIVATE`）与 `mprotect` 44，元数据与时间 2 个
> （`stat` 45、`gettimeofday` 46）；
> ktest 141 cases / 4460 checks（q35，2 skipped；pc 同判据 0 failed，`ahci` 无设备即 skip；1 条刻意告警）；
> `make smoke` 270/270（默认 `-smp 2`，`--smp 1` 同数）；`make` 零警告。
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
| 虚拟内存 | 3 | 三槽 PML4（内核映射对新进程零成本传播）、用户页 NX、懒栈增长（`page_fault_try_fix` → `stack_grow`）、**栈保护页**（`[MMAP_TOP, 栈地板)` 永不映射 + RSP 邻近检查）、`mmap`/`munmap`/`mprotect`（VMA 登记 + 需求分页 + 运行期翻权限，**匿名与文件映射两支**：`MAP_SHARED` 走共享页缓存 `mm/filepage.c`、写回由硬件 `PG_D` 定）、COW `fork`（order-0 帧引用计数）；无 swap/reclaim，内核映射不带 NX（无 W^X） |
| 进程与信号 | 3 | `fork/execve/wait4/brk/kill`、`sigaction/sigprocmask/sigreturn`、`parent` 存 (pid, 世代) 并经 `task_parent()` 解析；无 CPU 时间限制 |
| 调度 | 3 | 每 CPU 运行队列、按空闲/队列长度的负载均衡、每 CPU 本地 LAPIC tick；队列实为 push-front LIFO（C62）、内核态不可抢占、无优先级/nice |
| SMP | 3 | ACPI MADT 枚举 + xAPIC/x2APIC 双路访问、AP 本地时钟与共享标定；全局锁 + 轮询 I/O 使延迟敏感场景不可用 |
| 系统调用 / ABI | 3 | 47 个编号（0..46）全部实现，用户指针一律 `copy_*_user` → `user_ptr_ok`（范围 + 逐页 present，写侧 `user_ptr_writable` 先解 COW）；`stat`(45)/`gettimeofday`(46) 已补，仍缺 `fstat`、单调钟、`pipe` |
| 并发正确性 | 2 | 终端打印锁、任务表锁、fd 引用计数、console 多等待者链表已就位；仍有跨设备 I/O 的两层 irqsave（C7/C10）、`pick_next` 无锁读（C4） |
| 设备驱动 | 3 | PS/2 键盘与鼠标中断驱动（IRQ1→键盘环、IRQ12→aux 事件环，串口 RX 中断已接）、AHCI 走 DMA + `clflush`/`mfence` + UC 映射、完成判定补查 `PxTFD.ERR`；**AHCI 中断驱动**（`GHC_IE` + PxIM + `ahciIsr`，命令经完成中断回执，`issueCmd` 保留超时轮询兜底）、**IDE LBA48**（`0x24/0x34` EXT，`>0xFFFFFFFFFFFF` 拒）、**多盘注册与挂载分流**（有 `/bin/init` 的盘挂 `/`、没有的挂 `/mnt`）均收口（C49/C51/C53）；剩 PCI 配置机制按总线选择（C56） |
| 文件系统 | 3 | FAT32 + on-disk redo journal（mount 时 replay，`ktest/t_journal.c`）+ 时间戳 LRU 块缓存 + 簇链步数预算；边界批次已收口（目录游标 C30、LFN 260 字符 C31、FAT 容量拒绝 C32、末条目可达 C33、短名终止符 C34、MBR 范围与 EBR 链 C36），目录结束符同时改回规范字义（0x00 止看，0x05 是别名转义/活记录）；vnode 泄漏（C40）已修；挂载层收口（C28/C39/C43/C47：分区入 `diskinfo`、ops/priv 成对下传、最长前缀多挂载与 `umount`、close 引用计数不回绕） |
| 图形 / 显示 | 3 | 帧缓冲控制台 + 5 个绘图调用 + `fb_flip`（默认双缓冲），`CAP_FB` 能力门 + 单 pid 归属门两道检查；无直线/圆等图元（暂不作为缺口） |
| 用户态 | 2 | 16 个静态 ELF64 程序 + 自带 crt0 与迷你 libc；匿名 `mmap`、`mprotect` 与**文件映射**（`kmmap_file`，`MAP_SHARED`/`MAP_PRIVATE`）已就位，但仍无动态链接器（缺解释器与 `.so` 装载），跑不了第三方软件 |
| 安全模型 | 2 | `struct cred{kind,uid,caps}` + `CAP_KILL`/`CAP_SYS_ADMIN`/`CAP_FB` + `setuid` 单向降权；`EFER.NXE`/`CR0.WP`/`CR4.WP`/`SMEP`/`SMAP` 均置位并有 ktest 钉点；无 gid、无 RLIMIT、无文件属主、无登录/沙箱、无 KASLR、无栈保护 |
| 测试 | 3 | ktest 141/4460（含属性测试、随机流暴力用例、`blk_inject_io` 故障注入、OOM 注入、统一输入队列五例、AHCI 中断自测与多盘挂载分流、mmap/COW/`mprotect`/栈守护**十一例**、stat/时钟三例）、`make smoke` 270/270（默认 `-smp 2`，`-smp 1` 用 `SMOKE_FLAGS="--smp 1"` 手跑另一套标记）；无 CI、无宿主侧单测与 sanitizer |
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

### 2.2 设备驱动

| 编号 | 位置 | 后果 | 修法与量级 |
|---|---|---|---|
| C56 | `kernel/drivers/pci.c`（文件头注释、`:37-38`） | **半修**：visited 位图 + `PROBE_MAX_DEPTH 16` 已加；`0xCF8/0xCFC` 对所有总线拼同一 type-1 地址，而该机制只对 bus 0 有架构保证；文件头注释仍写"既无 visited 也无深度限制"，与紧邻的实现矛盾 | 按总线选择配置机制 + 改注释，半天 |

### 2.3 内存

| 编号 | 位置 | 后果 | 修法与量级 |
|---|---|---|---|
| C57 | `include/mm/mm.h` `PMM_WINDOW_TOP`；`kernel/mm/pmm.c:24,139-146` | 位图从固定物理 0x10000 顺序排布：管理窗口一旦再扩大，位图会撞上 `arch/entry64.S` 固定在 0x50000 的页表；这是个只写在注释里的隐式依赖 | 位图范围随窗口一起核算或搬移 + 断言，半天 |
| C61 | `kernel/mm/vmm.c:57-84` | **半修**：已检查既有 PTE 并只做放宽（不再覆盖），原问题基本关闭；`invlpg` 仍打在当前 AS 而非目标 root，目标不是当前 AS 时刷新无效 | 目标为当前 AS 才 invlpg，或把刷新交给调用方，几行 |

### 2.4 引导与架构

| 编号 | 位置 | 后果 | 修法与量级 |
|---|---|---|---|
| C66 | `kernel/main.c:61-90` | `boot_params` 指针只判非零、`e820_entries` 只截断到 64，指针本身不校验范围；`:96` 的注释却称该指针"已校验" | 指针落在已知 RAM 内才使用 + 改注释，约 20 行 |
| C67 | `kernel/main.c:208-216` | 没有 loader 参数时读固定物理 `CMDLINE_QEMU_PA`（0x20000）当命令行，依赖"那块内存恰好是 0" | 只在 loader 声明时读 + 长度上限，约 20 行 |
| C68 | `Makefile` `imageiso`；`scripts/smoke_test.py` | GRUB/ISO 路径只手工跑过，未进 `make smoke`；smoke 固定 `-m 256` + pc + TCG，内存档位、q35+KVM（`make run` 的默认配置）、IDE+AHCI 双盘都不在门控里 | smoke 矩阵化参数，约 1 天 |
| C69 | `kernel/mm/pmm.c:96-112` | 只用单个最大 E820 区，且每区钳到 `PMM_WINDOW_TOP`（2 GiB）：其余 RAM 完全不参与分配 | 合并全部区域 + 分段别名，约 2 天 |
| C70 | `arch/entry64.S:4-7` | 固定物理 0x50000–0x57000 的页表从不与 E820 校验冲突 | 启动早期校验并拒绝冲突布局，约 20 行 |

### 2.5 未定位的观察项

| 编号 | 位置 | 后果 | 修法与量级 |
|---|---|---|---|
| OBS-1 | `ktest/t_cache.c` `test_write_readback_is_coherent`；2026-10-21 一次连跑 | 历史出现过一次失败（伴随 FAT/dir `I/O error`），重跑通过。I/O 类根因已定位并修掉：IDE 轮询预算（旧 10 万次 port read）短于 QEMU 在 vCPU 线程同步落宿主盘时的 BSY 保持，正常命令被判 EIO（改 `IDE_POLL_ITERS`，旧预算 A/B 必现）；修后连跑 16/17 全绿，余 1 次（3 用例 5 断言，伴 fs 用例整段 skip）未复现 | 连跑 N 次的抖动门 + 失败时保存串口现场，约 1 天 |

---

## 3. 能力差距

按主题分组。每项格式：**缺什么 → 影响谁 → 量级估计**。

### 3.1 进程间通信（IPC）

| 缺口 | 影响 | 量级 |
|---|---|---|
| `pipe` 与 shell 管道 | shell 只能单命令，工具无法组合 | 约 1–2 天（内核 pipe + `userland/sh.c` 的管道解析） |
| 共享内存（`shmget`/`shmat` 那一层） | 底座 `mmap(MAP_SHARED, fd)` 已就位（2026-11-09），但没有 shm 的便利约定：要交换仍得自己造文件、自己映射、自己删 | 约 0.5 天（`/dev/shm` 或 `/tmp` 约定文件 + 包装） |
| unix domain socket | 最完整的 IPC，服务/客户端模型的前提 | 建议最后做，约 1 周 |
| 线程 / `clone` | 进程模型是单线程假设，多处并发结论以此为前提 | 引入需重审并发结论，约 2 周 |
| `alarm` / 定时信号 | 无法做超时与定时唤醒，只有 `nanosleep` | 约 0.5 天 |

### 3.2 内存映射与 mmap

匿名 `mmap`/`munmap`、缺页分发器三段化、COW `fork`、`mprotect`（44）与栈保护页已于 2026-11-07 收口；**文件映射 `mmap`（42 扩成 Linux 六参 `addr,len,prot,flags,fd,offset`，`MAP_SHARED` 共享页缓存 + 三处写回）已于 2026-11-09 收口**（见第 6 节）。此处只留仍然开放的。

| 缺口 | 影响 | 量级 |
|---|---|---|
| 动态链接器与共享库装载 | 文件映射已就位，但 `execve` 仍是一次性把 ELF 段读进全新帧（不经过 mmap），用户态也没有 `ld.so`：跑不了依赖 `.so` 的第三方程序，"可执行文件映射"也还没被 loader 用上 | 在文件映射之上约 1–2 周（`PT_INTERP`、解释器、`.so` 装载与重定位） |
| OOM 策略与回收 | 无 OOM killer、无 swap/reclaim：内核侧 `kmalloc` 失败没有受害者选择也没有归因（mmap 需求分页已过 `cred_mem_quota()`，但超限只拒该次 fault） | 约 150 行（启发式杀最大进程，或内核分配按进程记账） |

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
| 轮询块 I/O | AHCI 已中断驱动（C49 收口），但 IDE 仍轮询 + 全局锁（C7/C10），磁盘延迟直接进关键路径 | 见 2.1 |

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
| `fstat` | `stat`(45) 已收口（2026-11-11）：路径元数据一次到位；但仍**无描述符侧**的元数据——`ls`/`cat` 拿到 fd 后问不到同源的 ino/size，也没有时间戳（FAT 目录项里有，未翻译） | 沿 `vfs_attr` 加 `fstat(fd)`，约半天 |
| `clock_gettime` | `gettimeofday`(46) 已收口（2026-11-11）：RTC 采样一次 + TSC 演进，UTC、微秒精度；但仍无单调钟（`CLOCK_MONOTONIC`）与 `nanosleep` 的绝对定时变体 | 约 1 天 |
| `chdir` / `getcwd` / `umask` | 没有进程工作目录，相对路径靠用户态硬拼 | 约 1 天 |
| `fcntl` / `poll` / `select` | 无法做非阻塞**多路**等待，`console_read` 是单读者阻塞；非阻塞输入只补了单键探针（`kbd_get` 34）、指针事件探针（`mouse_get` 37）与统一队列单事件探针（`input_pop` 39，成功回 1、空回 `EAGAIN`） | 约 2–3 天 |
| `chmod` / `link` / `symlink` | FAT32 无权限位与硬链接，软链可做 | 按需，各半天 |
| 固定容量：`NR_TASKS 64`、`NR_FDS 16`、`MAX_MOUNTS 4`、缓存 64 槽 | 编译期常量，用满即失败且诊断有限 | 动态扩容或可配置，约 1 天 |

### 3.9 存储与驱动能力

| 缺口 | 影响 | 量级 |
|---|---|---|
| 块请求队列 / 合并（C10） | 串行、不可取消（AHCI 已中断驱动、IDE 仍轮询 + 全局锁） | 见 2.1 |
| 用户可见的挂载 / 卸载 | 内核侧已有 `vfs_mount_fs`/`vfs_umount`/`vfs_free_space` 并有 ktest 钉点，但用户态没有 umount、也读不到挂载表（ABI 冻结，本批未加 syscall） | 若要暴露成 syscall，约 1 天 |

---

## 4. 安全定性

| 场景 | 结论 | 依据 |
|---|---|---|
| 跑自己写的 `userland/` 程序 | 安全 | `sigreturn` 恢复内核保存的帧（不读用户栈，CS/SS 不经用户之手）；ELF 装载七项校验；syscall 指针一律 `copy_*_user` → `user_ptr_ok`（范围 + 逐页 present）；表外编号回 `ENOSYS`；用户态异常只杀进程；`kprintf` 格式串全是字面量且无 `%n` |
| 跑不完全信任的用户程序 | 不安全 | 权限止于 `uid` + 3 个能力位（无 gid / 文件属主 / CPU 限额 / 沙箱）；内核全程关中断加一把全局 FS 锁，一个 bug 可拖死整机；内核镜像可写可执行、无栈保护、无 KASLR |
| 挂载可信磁盘 | 基本安全 | on-disk redo journal + mount replay，故障注入覆盖提交点前后两个窗口，块缓存时间戳 LRU，簇链有步数预算；静默损坏三件已修（C19/C20/C21：`O_TRUNC` 落盘、写回按槽位定位、找不到 dirent 即 EIO 回滚）；vnode 泄漏（C40）已修；脏页三道同步门已就位（C27：BSP tick 每秒 / 普通文件 close / panic try 回写，外加 `SYS_sync(33)`；崩溃窗口从 32 KiB 收窄到「上次同步以来写过扇区」）；路由与挂载本身也收口了（C39/C47：ops/priv 成对下传、最长前缀匹配、第二挂载点与 `umount` 有 ktest 钉点；C28：分区与满槽处理一致，`diskinfo` 看得到每个分区）；C55 让 IDE 写路径也以 `0xE7` CACHE FLUSH 收尾并检查返回，落盘承诺不再只是轮询 DRDY |
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
6. 驱动收口：PCI 配置机制按总线选择与注释（C56）；AHCI 中断/多盘、IDE LBA48 已收口（C49/C51/C53）。
7. 能力面：`pipe`（3.1）→ 动态链接器与共享库（3.2，文件映射已是它的前置）。匿名映射、COW、需求分页与 `mprotect`/栈保护页已收口（2026-11-07），**文件映射已收口（2026-11-09）**，**`stat`/`gettimeofday` 已收口（2026-11-11）**。
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

### 2026-11-04 挂载与设备批次

- **C28** 分区在尝试挂载前就 `blk_register` 进 `diskinfo`（扩展分区除外）；`blk_register` 改回 0/`EFAIL`，满 8 槽大声拒绝并保持表不变；`blk_first` 删除，挂载改「第一个能挂上的设备」；注册表所有权归 `blk_list`，失败挂载不再留下悬空指针。
- **C39** 挂载数据真正下传：`fs_for_path` 返回整条挂载项（ops+priv 成对），`vnode.mnt_data` / `dir_iter.mnt_data` 一律是解析时那条挂载；FAT32 各 `_impl` 改用形参 `mnt`，`(void*)1` 哨兵删除；`fs_ops.freespace` 有了调用者 `vfs_free_space(path)`。
- **C43** `vfs_close_file` 改 CAS 循环：refcnt ≤ 0 时打 `[vfs] close: refcnt=…` 并忽略，绝不二次 free、计数不回绕。
- **C47** 挂载表按**最长前缀 + 组件边界**匹配（`"/"` 不再吞掉一切，`/alt` 可达）；新增 `vfs_mount_fs`/`vfs_umount`（sync → 去激活 → 重算 `disk_ready`，共享挂载数据不 free），`vfs_mount_initramfs` 复用同一条卸载路径；跨挂载点 `rename` 回 `EXDEV`。不加 syscall，ABI 冻结。
- **C55** IDE 写路径收尾改发 `0xE7` CACHE FLUSH 并用 `ide_wait_flush` 检查 BSY/ERR/DRDY，失败打 `[ide] FLUSH CACHE (E7) failed` 回 `EFAIL`（与 AHCI 行为一致）；`ide_wait_ready` 不判 ERR 的既有决定保持。
- ktest 基线升到 **125 cases / 4033 checks**：新增 `ktest/t_blk.c`（分区注册 + 满槽拒绝）与 `t_vfs.c` 三例（close 饱和、`mnt_data` 下传、第二挂载点各自路由）。

### 2026-11-05 中断 I/O 与容量批次

- **C49** AHCI 中断驱动：`GHC_IE` + 端口 `PxIM` arm + `ahciIsr()` 排空 HBA/端口 IS 并回执；`issueCmd` 在写 `P_CI` **之前**开中断、完成走中断、轮询只作超时兜底（超时 dump TFD/IS/SERR）；`ahciRouteIrq()` 把 INTA 经 IOAPIC 接到 vector 32+line 并 unmask；多盘名 `sd_names[8]`。自测 `ahci_irq_selftest()` + `ktest/t_ahci.c`（无 AHCI 设备即 skip）。
- **C51** 多盘挂载分流：有 `/bin/init` 的卷挂 `/`，其余盘挂 `/mnt`（`home_volume()`/`find_volume()`，`refresh_root()` 重算根）；`register_mount()` 复用槽位、`vfs_try_mount_one()` 先查 `mounts_active()` 防重复泄漏；分区在尝试挂载前 `blk_register` 入表。
- **C53** IDE LBA48：`0x24/0x34` EXT 命令，`ide_issue(lba,n,cmd)` 按**最后一个扇区**选 LBA28/LBA48，`last > 0xFFFFFFFFFFFF` 拒绝，首次走 EXT 打 `[ide] LBA48:`。
- **`#UD` 根因（C49 引爆的既有缺陷）**：`isr_common` 曾把**任何**中断帧发布为 `current->tf`；嵌套在 ring-0（syscall 内）到来的 IRQ 会把它改指向用完即弃的帧，`sys_execve` 随后把 `{rip, ussp}` 写进死帧，`iretq` 用**旧 rip + 新地址空间** → 用户 `#UD`（rip 甚至不是指令边界）。修法：只发布 ring-3 帧（`(f->cs & 3) == 3`），`sys_fork` 的帧拷贝同源受益；q35 连跑 12/12 无复现（修前 5/1、1/1、1/2）。
- q35 幻影 port 2 区分"无响应"与"被拒绝"并回收 CLB/FIS/CT（`ahciPortReclaim`），boot 就绪 33.8 s → 8.5 s。
- ktest 基线升到 **127 cases / 4054 checks**：新增 `ktest/t_ahci.c`（中断路由 + 首命令中断完成）与 `t_vfs.c` 多盘分流用例。

### 2026-11-06 双缓冲呈现与 LFB 写合并批次

- **P0-A 频率门**：`fb_present_wanted()` 从 `jiffies == fb_last_present` 改 `jiffies == 0`——旧条件在同 tick 内 present 落地后重开写批次门，快速滚屏时控制台每个 64 B 分块都做一次全屏 present（每次拷贝都是整帧预算），输出一帧一爬；现在首 tick 后批次门**永久关死**，呈现只由 tick 节流。
- **P0-C 撕裂**：present 的逐行拷贝从"锁外"改为**全程持 `console_out_lock`**（拆出 `fb_present_held()`；`fb_flip`/`fb_tick_flush` 走 lock→held→unlock，批次路径调用方已持锁直调 held）；4 个绘图 syscall（clear/fill/char/puts）**整个原语持锁**、unlock 兼批次尾；`panic()` 改 `fb_flip_unlocked()`（T-002：调用方可能已持锁，再拿即关中断自旋死锁）；拷贝尾补 `mfence`（WC 合并缓冲排空，否则 flip 返回即读会读到旧帧）。staging 快照方案放弃：1080p back+staging = 15.8 MiB > 14 MiB 后缓冲窗，真机会被逼回单缓冲。
- **P0-B PAT/MTRR**：新增 `fb_memtype_program()`（每 CPU 一次：BSP 在 `fb_init`、AP 在 `ap_main` 顶）——① `IA32_PAT` 无 WC 槽时补 `PAT[7]=WC`（全仓无人选它）；② BSP 存 DEFTYPE+variable range 快照，AP 关 range→清零→回放→DEFTYPE 收尾（MTRR per core，AP 复位态与 BSP 不保证一致，不采纳即 BSP=UC/AP=WC 混写同一 PTE；fixed 只达 1 MiB 下不镜像）；③ 「MTRR 开、无 range 覆盖孔径、默认 UC/WP」时给孔径**新增** pow2 WC range（只用空槽、不碰固件已有范围、块恒在 BAR 内，撞范围/无空槽/对不齐即放弃并点名）；固件 UC **覆盖**孔径（QEMU：`mtrr[0]=0xc0000000/1 GiB/UC` 盖住 `0xfd000000`）时架构性胜过 PAT WC，如实打点名判定行 `MTRR[i] UC @base -> UC`，不承诺 WC。`vcnt > 16` 时整体不改写（无法忠实镜像），全 CPU 统一停在固件类型。
- 验证：q35 boot 0 异常、`ready in ~8.1 s`、`[smp] AP1 online` 正常、判定行为点名形式；KTEST=0/1 与 userland 零警告；ktest **127 cases / 4054 checks，0 failed**（q35 全量 239 s）。真机 `make smoke` 与撕裂/滚屏延迟实测由用户执行。

### 2026-11-07 匿名 mmap / 缺页分发 / COW fork 批次

- **匿名 `mmap`/`munmap`（42/43，ABI 只有 3 参）**：`sys_mmap(addr, len, prot)` 要求 `addr == 0`（内核选址，hint 不假装采纳）、top-down 扫 `[MMAP_LO, MMAP_TOP)` 空隙；窗宽 64 GiB、`MMAP_TOP` = 栈增长地板（`USER_STACK_TOP-0x80000`），`sys_brk` 上限从 `USER_STACK_TOP-1MiB` 移到 `MMAP_LO`——旧上限落在 mmap 窗**内**，堆与 mmap 可以争同一页面。**只登记不映射**：首触经缺页分发器的 demand fixer 补零帧、按 VMA prot 给 W/NX、先过 `cred_mem_quota()`（配额从"堆上限"升级为"一切映射上限"）；`sys_munmap` 从 VMA 列表摘除（中段挖洞自动拆分、部分相交按 POSIX 静默截断、零相交 `EINVAL`）→ 逐页 unmap → `vmm_reclaim_empty_tables` 回收空 PT/PD/PDPT（否则每个新触碰的 2 MiB 区永久搁浅 2 个表帧，buddy 差额恰好 2 页）。VMA 列表 `kernel/mm/vma.c` 挂 `struct task.vmas`（fork 深拷、execve 与 `task_free_slot` 释放，alloc 槽位清 NULL 防继承）。
- **缺页分发器三段化**：`page_fault_try_fix` 从单 `stack_grow` 变 demand（VMA 内 not-present，含"已 present 不再分配"幂等防御）→ stack_grow → COW（保护错 + `PG_COW`）；**真 rodata 写保护（!W !COW）仍落 SIGSEGV**（若只查"非 COW 即放行"会把 fault 当已修复，用户态死循环）；already-W 的竞争故障 claim 并重写 PTE flush TLB。
- **COW `fork`**：`dup_user_aspace` 不再逐页 `pmm_alloc+memcpy`——**共享帧**：可写页两边清 `PG_W` 置 `PG_COW`（PTE bit 9，软件可用位）并 `invlpg` 源侧，只读页原样共享（写保持非法）。order-0 帧引用计数 `pmm.c frame_ref[]`（2 GiB 窗 1 MiB .bss；alloc=1、free CAS 递减、归零才回 buddy、`pmm_ref` 供 fork 加签；**ref==0 释放照旧放行**，播种与遗留路径不会被计数卡死；order>0 不经过计数）。写保护缺页 → `page_cow_break`：**先复制、再换 PTE、最后放旧引用**——本方引用在复制完成前钉住旧帧，计数归零必然发生在所有复制者之后，双核同帧竞争读不到已回收帧；`user_ptr_writable` 成为唯一 COW 解析门（`copy_to_user`/信号帧 push/vfs 写一处生效），`vmm_map_user` 的 widen 分支拒绝给 `PG_COW` 页回写 `PG_W`（否则两个共享者会静默重新共享可写）。
- ktest 基线升到 **132 cases / 4163 checks**（该批次当时；同日栈保护页/mprotect 批次后 137/4308，2026-11-09 文件映射批次后 138/4360，2026-11-11 stat/gettimeofday 批次后 141/4460，见文首基线与第 6 节末两条）：新增 `ktest/t_mmap.c` 五例——syscall 分发与负例（hint/未对齐/零长/窗外）、demand（配额拒绝、幂等、权限来源、buddy 精确复原）、COW fork（dup 后 `refcount==2` → fault break → payload 跟帧走 → rodata 放行 SIGSEGV → 全 user leaf 快照恢复）、pmm refcount 语义、VMA 列表单元（排序/拆洞/深拷）。
- 验证：KTEST=0/1 与 userland 零警告；q35 ktest 全量 0 failed、KTEST=0 `ready in ~8.3 s` 到 `root@lnxrm$` 0 异常；16 bins。

### 2026-11-07 栈保护页与 mprotect 批次

- **栈保护页**：`STACK_GUARD` 64 KiB 的 `[MMAP_TOP, 栈地板)` **永不映射**——`MMAP_TOP` 从栈地板下移一个守护区，mmap 窗在它下面收口；`demand_page` 落在守护区一律拒，`mprotect` 对它回 `ENOMEM`（既无 VMA 也无页）、`munmap` 零相交回 `EINVAL`。`STACK_GROW_WINDOW` 与 `STACK_GUARD` **静态断言相等**（`USER_STACK_TOP - MMAP_TOP == USTACK_MAX_GROW + STACK_GROW_WINDOW`）：栈最深时，凡被认定为栈访问的 fault 都落在守护区内、绝够不到 mmap VMA。`stack_grow` 新增 **RSP 邻近检查**（`f->ussp` 是换权时硬件压的用户 RSP，`intr_frame` 没有 `rsp` 字段）：`cr2 + 64 KiB < rsp` 一律拒绝——红色区 128 B 加序言一次减帧就是合法触达的全部，**取帧之前**先毙；ktest 合成帧 `ussp=0` 自动跳过。
- **`mprotect`（44，`sys_mprotect` + `vma_protect`）**：作用于**任意用户范围**（不只 mmap 段——翻 JIT、改 .text、封栈缓冲）：在场页立即翻 W/NX，未触碰的改 VMA 交给需求分页；范围里有洞 `ENOMEM`（**先查全再改动**）、未对齐/零长/回滚/出用户空间 `EINVAL`。**先 `vma_protect` 原子改列表、再改 PTE**（后者只重写刚读过的叶，不可能失败）：`vma_protect` 先数节点（全覆盖 0、切头/切尾 1、中间打洞 2）→ 预分配池 → 才写 prot，`ENOMEM` = 列表零改动（与 `vma_erase` 的拆分同规）。授予 W **永不分配**：独占帧直接 `PG_W`，共享帧（`refcount>1` 或已 `PG_COW`）只标 `PG_COW`、复制推迟到写故障——与 Linux `can_change_private_pte_writable()` 同规则（只在 `PageAnonExclusive` 时才改可写）；**撤销写同时清 `PG_W|PG_COW`**，否则下一次 COW 缺页会把只读悄悄改回可写。
- **窗口常量修正（新用例撞出的前批遗留缺陷）**：`MMAP_LO` 的 `0x10000000000` 是 **1 TiB** 不是 64 GiB，窗口底掉到 `USER_BASE` 之下 8 TiB——mmap top-down 只在窗顶放置所以从未暴露，`mprotect(MMAP_LO,…)` 一测就 `EINVAL` 而非 `ENOMEM`。改回 `0x1000000000`，补 `MMAP_LO >= USER_BASE`、`MMAP_TOP <= 栈地板` 两条静态断言。
- **#PF 的 `err` 位逐位认**（对照 Linux：`access_error()` 排在 `handle_mm_fault()` **之前**）：`cow_fault` 只认写位——否则 NX 取指（`P|I = 0x11`）会被当成"COW 已破环"，什么都没改就回报已修复，`iretq` 重试同一指令再入同一 #PF → **无限故障循环**（NXE 已开，每个栈/堆/mmap 数据页都踩得到，野跳转即挂死）；`stack_grow` 拒取指位（栈是 NX，长出来也白长）；`demand_page` 先按 VMA prot 拒掉越权的写/取指，**再**取帧。
- **拷贝门先补页**：`user_ptr_ok`/`user_ptr_writable` 跑在拷贝之前，遇 not-present 页先走一次 `page_fault_try_fix` 再判——否则 `read()` 进没碰过的 mmap 缓冲回 `EFAULT`，匿名 mmap 只有程序自己逐页戳过才可用（Linux 的 `copy_to_user` 是直接吃 #PF 补页）。该拒的照旧拒：内核地址、非规范地址、窗口里的洞、只读页底下的写，一条不变。
- ktest 基线升到 **137 cases / 4308 checks**：`ktest/t_mmap.c` 从五例扩到**十例**，新增 `test_mprotect_syscall`（负例 EINVAL/ENOMEM、撤销→PTE 与 VMA 跟随、写保护 fault 拒绝、授予 RWX、中段拆三段、demand 按新 prot 补页、buddy/list/账目精确复原）、`test_mprotect_sharing`（私有往返零复制、伪造第二持有者、授予只标 COW 不复制、撤销清 COW 后写 fault 必须拒、再授予才复制且 payload 跟帧走）、`test_stack_guard`（守护区不可映射/不可 mprotect/fault 不可修复、懒增长照常、**RSP 远高于 cr2 的 fault 被拒且不取帧**、尾部 unmap 归还借来的栈页）、`test_fault_kinds`（NX 取指不得当 COW、写进只读 VMA 取帧前就拒）、`test_copy_gates_fault_in`（uname/open 两道门替拷贝补页、只读 VMA 下的写照旧 `EFAULT` 且仍懒）。
- 验证：KTEST=0/1 与 userland 零警告；q35 ktest 全量 **137 cases / 4308 checks / 0 failed**（`rc=0`）；KTEST=0 `ready in 8309 ms` 到 `root@lnxrm$` 0 异常；16 bins。真机 `make smoke` 由用户执行。

### 2026-11-03 批次

- **FAT 表分帧驻留**：每帧 4 KiB `pmm_alloc` + 128 KiB bounce 散射读；挂载上限从「kmalloc 4 MiB 连续块」变为「全部空闲帧」，硬上限仍是 2^28 入口（1 GiB 表）。分配器回基线（`MAX_ORDER 11`、kmalloc 4 MiB）。
- **`sched_wake_task` 自死锁**：原实现持 `runq_lock` 再调 `runqueue_add`（内部再拿同一把锁）→ 对齐 `send_signal` 的免锁写法。
- **ktest `t_input` waitlist**：用例开头补 `evq_drain()`（t_mouse 生产者路径把共享队列灌满）。

### 2026-11-08 大扫除第 1、2 轮（死代码 / 重复逻辑 / 史山）

- **信号表压实（编号重排，用户拍板「删掉并重排成无空洞」）**：删 15 个全仓零引用的 `LNXRM_SIG*`（HUP/TRAP/USR2/PIPE/ALRM/STKFLT/TSTP/TTIN/TTOU/URG/XCPU/XFSZ/VTALRM/PROF/WINCH），幸存 13 个按 Linux 顺序压成 **1..13 连续无空洞**，`LNXRM_NR_SIGNALS 32 → 14`——从此内核接受的每个号都有名字，14..31 那段「被接受但没人拼得出来」的匿名区间消失；`sig_handlers[]`/`sig_masks[]` 随之缩到 14 槽（所有下标路径都过 `sig < 1 || sig >= _NSIG`，缩数组无越界）。三处硬编码同步：`sched.h` 静态断言（`KILL 7/STOP 13/CONT 12`）、`kill.c` 提示与范围改为从 `LNXRM_NR_SIGNALS` 派生、`CODEBASE`。**顺带挖出两处会被重排打破的隐性硬编码**：smoke 的 `kill.child.status=-15`、`segv/nx.status=-11`（被信号杀的子进程退出码 = `-sig`）——改为 `abi_signal()` 现读 `lnxrm_abi.h`，以后压实该表不必再有人记得去改测试。
- **P0 `systest.c` 速查表**止于 31、写死 `LNXRM_SYSCALL_LAST (31)`（实际 44）→ 补 32–44 行、计数 32→45、`LAST`→44。
- **删 11 处零使用 `#include`**：`sched.h`/`percpu.h` 的 spinlock、`percpu.c`/`print.c` 的 `mm/mm.h`、`vfs.c` 的 `io.h`、`t_cpu/t_draw/t_oom/t_task` 的 `console.h`、`kblk.c` 的 `sys/ktest.h`、`ktest/include/kblk.h` 的 `abi/lnxrm_abi.h`。连锁修一处：`isr.c` 原本靠 `sched.h` 顺带拿到 spinlock，删掉后编译暴露——补它自己的 `#include <sys/spinlock.h>`（真正的消费者自己 include）。
- **6 处重复逻辑合并**：`user_stack_map()`（execve/fork 两处「16 KiB 栈 + restorer」映射，各自的 unwind 留在调用点）；`fat_free_chain()`（rmdir/unlink/截断三处释簇，`rollback_growth` **有意不并**——它忽略错误继续走，语义不同）；`evq_take()`（pop / wait 快路径 / wait 锁内重查三处出队，wait「锁持到挂上 waiter 才放」的语义原样保留）；`ata_model_string()`（ahci/ide 的 IDENT 27–46 解码，与 `ata_total_sectors` 并排）；`cache_reserve_victim()`（读/写 miss 各一处选牺牲者+回写，失败时已自行解锁）；`ide_blk_xfer()`（读写共用主体，`ide_read/write_sectors` 签名未动）。
- **`syscall_do` 表驱动化**：183 行 / 45 case 的 `switch` → `sysents[45]`（一行一个 `{入口, 门, 门名}`）+ 15 行分发器。**门与入口同一行**（7 个 `GATE_ADMIN`、5 个 `GATE_FB`），新增受限调用不可能只加调用忘加门；参数拆包集中在 `h_*` 包装；`exit` 不再靠「掉进 `wait4`」的 fallthrough；未知编号（超表界或 `fn==NULL`）**先不问门**直接 `ENOSYS`（没实现的调用不该先问它从没资格拿的能力）。脚本核对表与 `lnxrm_syscalls.def`：45 条无缺行无多余行、`h_*` 全有定义、门集合与改前逐条相同。
- **`systest.c` 的 942 行 `main` 拆分**：35 个 `static void t_*(void)`（段注释原地变成函数头上的文档）+ 47 行 `main`。纯代码搬移：564 条可执行语句与拆前**逐行等价**（内容+顺序脚本比对），35 段注释原序保留，`systest.done=1` 位置不变，109 条 `SYSTEST_TAGS` 不涉及。
- 一个环境敏感点（未改）：本批 ktest 首跑时 `t_input` 两例断言「aux 环为空」却读到 46 个事件（`ms_pending()==0x2e`）——全仓唯一能往该环放字节的是 IRQ12 的 `mouse_irq_handler`，即 QEMU 窗口里真有鼠标移动；本批改动不触及 PS/2 路径，复跑即 0 failed。若反复出现，应给这两个断言加「入环先排空」的容忍。
- 验证：KTEST=0/1 与 userland 零警告；ktest **137 cases / 4308 checks / 0 failed**（early 25/404、mm 17/437、late 95/3467、2 skipped、1 刻意 warn）；16 bins；`make run` `ready in 8234 ms` → `root@lnxrm$` 0 异常。真机 `make smoke` 由用户执行。

### 2026-11-09 大扫除第 3 轮（史山重写 / 零引用收尾）

- **`isr_common` 264 行 / 嵌套 6 → `isr_common` 32 行 / 嵌套 3**（行数按签名到收尾 `}`，嵌套按花括号深度，下同）：拆成 `handle_ipi` / `cr2_read` / `struct pf_walk`+`pf_walk`+`pf_slot_again`+`pf_leaf_alive`+`pf_walk_print` / `pf_error_print` / `pf_tables_dump` / `exception_report` / `fault_to_signal` / `kernel_context_dump` / `irq_path`。**逐条比对 `kprintf` 格式串序列，运行期输出逐字不变**；唯一删除是 `irq==7||irq==15` 分支里 `if (f->intno==0xFF) return;`（`irq==7→intno 39`、`irq==15→intno 47`，永不等 0xFF，**可证不可达**）。新增 `#include <mm/mm.h>`（原为函数内 `extern vmm_translate_in`）。
- **`ahciProbe` 334 行 / 嵌套 4 / 9 条清理梯子 → 48 行 / 嵌套 2**：引入 `struct ahci_port_res`（`px/p/clb/fis/ct/port/ident/bd/started/keep`）+ 单出口 `ahciPortRelease()`——原先 9 处「port skipped」各自重复 `dmaFree()/kfree()/ahciPortReclaim` 的子集，每加一条退出路径就多一次漏掉刚分配缓冲的机会；拆出 `ahciMapBar`/`ahciHbaReset`/`ahciWaitLink`/`ahciPortAlloc`/`ahciPortProgram`/`ahciPortIdentify`/`ahciPortPublish`/`ahciPortBringup`。`dmaFree(NULL)`/`kfree(NULL)` 本就 NULL-safe，已核实。
- **`sys_execve` 187 行 → 49 行**：`struct exec_args`（16×64 argv/envp + path[128] + argc/envc）+ `exec_copy_args`/`exec_read_image`/`exec_commit`/`exec_build_stack` 四段，`sys_execve` 只剩编排。原第一大半是「free img / close file / destroy new_pml4」的资源阶梯，每个阶段各自记着已经拿了哪几样；现在清理规则只剩调用点的「free img、destroy new_pml4」。POSIX 语义（失败返回、不退出）保留在注释与控制流里。
- **`fat_mount` 196 行 / 嵌套 4 → 48 行 / 嵌套 1**：9 处一模一样的 `kfree(m); return NULL;` 收成 `fat_reject()`（内部 `fat_pages_free(m); kfree(m);`，`fat_pages_free` 对 NULL 安全，页数组未分配时也无害）；拆出 `fat_bpb_ok`/`fat_layout`（带 `u32 *totsec_out` 回传**卷** totsec 给挂载横幅——原打印的就是 BPB 的 totsec，不是设备容量，已逐字对齐）/`fat_fat_alloc`/`fat_fat_read`。每处拒绝的打印留在出问题的那条检查旁边。
- **`fat_scan_dir_at` 137 行 / 嵌套 7 → 54 行 / 嵌套 3**：`struct dir_scan`（buf + LFN 状态 + 回调 + 每扇区目录项数）+ `lfn_record()`（LFN 状态机，标记记录开新 run、非标记记录校验和/递减序号双验）+ `fat_entry_name()`（长名可用才给长名，否则 8.3 别名，C17）+ `dir_scan_cluster()`。枚举 `DIR_SCAN_MORE=0 / DIR_SCAN_STOP=1 / DIR_SCAN_END=2`——**`DIR_SCAN_END` 必须让外层 `break` 且把 `rc` 归 0**（对应原 `goto done`，否则会继续扫 0x00 终止符之后的后续簇，把已终止目录的尾巴当目录项送进回调）。
- **删 `S_IFREG`**（`kernel/fs/initramfs.c:41`）：全仓唯一零引用的 `S_IFREG`；`S_IFMT`/`S_IFDIR` 仍在用，未动。
- **`systest.c` 补 4 个零调用包装**：新建 `t_mmap_surface()`，插在 `t_input()` 之后、`say("systest.done",1)` 之前，覆盖 42 `kmmap` / 43 `kmprotect` / 44 `kmunmap` / 32 `kfb_flip`（这 4 个此前全仓只有定义没有调用）。实测输出：`mmap.base=140737483554816`（0x7fffffb6c000，页对齐）、`mmap.read=170`、`mmap.prot=0`、`mmap.reread=170`、`mmap.zerolen=-22`、`mmap.unaligned=-22`、`mmap.unmap=0`、`mmap.hole=-12`、`mmap.tail=0`、`fb.flip=0`。**munmap 之后不再碰该地址**。
- **smoke `past.*` 判据修成动态**（既有缺陷，不修则判据永远匹配不到）：`abi_signal()` 改名 **`abi_define()`**（现读 `lnxrm_abi.h` 的 `LNXRM_*`），新增 `SYSCALL_LAST = abi_define("SYSCALL_LAST")`；`past.42/past.43` 写死的 42/43 → 从 `SYSCALL_LAST+1..+3` 现算（现为 45/46/47），与 systest 实打的编号一致。
- **smoke 两个自伤**：① 新判据误写成 `str` 正则（region 是 `bytes`）→ `re.search` 中途 `TypeError`；全改 `br""`，并在列表后加启动期断言 `_str_tags` → `SystemExit` 防复发。② `SYSTEST_TAGS` **109 → 120 条**（past +1、新增 10 条 mmap/fb.flip 判据）。
- 验证：KTEST=0/1 与 userland 零警告；ktest **137 cases / 4308 checks / 0 failed**（early 25/404、mm 17/437、late 95/3467、2 skipped、1 刻意 warn），无 PANIC/BUG/Oops/`[exception]`；16 bins；`make run` `ready in 8210 ms` → `root@lnxrm$` 0 异常；`make smoke` **217/217 PASS** 两次——isr 重写后单独一次（990 s）与本轮全部落码后的终跑（784 s，`timeout 1500`，rc=0）；serial 里 3 次用户 #PF 报告（共 45 行 `[exception]` 输出：2 次 `err=7` 写保护、1 次 `err=15` 取指）正走重构后的 `pf_*`/`exception_report` 路径，输出格式与重构前一致。已知环境观察（非回归）：ktest 日志里 `port 2: device present (SSTS=00000113)` + `FLUSH CACHE (E7) failed` + `device does not respond, port skipped` 是 QEMU q35 幻影盘的既有行为（`ahciPortReclaim` 注释已记）；smoke 的 serial.log 不含任何 `[ahci]` 行（smoke 走 IDE），无法对照。

### 2026-11-09 文件映射（file-backed mmap）批次

- **ABI：`mmap`（42）从 3 参扩成 Linux 六参 `mmap(addr,len,prot,flags,fd,offset)`**，编号不动。`int 0x80` 仍是 `rdi/rsi/r10` 三参，第 4–6 参直接读 trap frame 的 `rdx/rcx/r8`（`PUSHALL` 本来就把它们推进来了，`h_mmap` 直取，未加任何寄存器管线）；ktest 侧新增 `syscall_probe6()`，而**三参 `syscall_probe()` 转发成 `probe6(nr,a1,a2,a3,0,(u64)-1,0)`**——"无 flags、无文件、无 offset" 正是匿名映射，既有 137 例一条未改。`addr` 仍必须为 0；`flags==0` 归一成 `MAP_PRIVATE`（老调用方行为不变）；`fd<0` 即匿名。用户态 `sys_call6()` 用 `register long asm("r10"/"r8")` 钉寄存器（`rdx`/`rcx` 走 `"d"`/`"c"` 约束），已用 `objdump` 核过生成汇编：`eax=0x2a`、`rdi=0`、`rsi=len`、`r10=prot`、`rdx=flags`、`rcx=fd`、`r8=off`。
- **校验全在取地址之前**（拒绝即零副作用）：`flags` 出 `MAP_SHARED|MAP_PRIVATE` 或两者同给 `EINVAL`；`offset` 未对齐/`offset+len` 回绕 `EINVAL`；匿名带 `offset` `EINVAL`；`fd` 越界或空 `EBADF`；非常规文件（目录/设备）`EINVAL`；**任何映射都要读文件** → `O_WRONLY` `EBADF`；`MAP_SHARED+PROT_WRITE` 必须 `O_RDWR`，否则 `EACCES`（写回要用的就是这个描述符）。
- **`MAP_SHARED` 走共享页缓存 `kernel/mm/filepage.c`**（新建 `include/mm/filepage.h`）：同一 `(file, offset)` 全系统**一个帧**，`fp_lock` + 64 桶哈希；**锁外读盘**——先查表、未命中则解锁 `pmm_alloc`+`vfs_pread` 后再抢锁二次查表（输掉竞态就丢弃自己刚填的帧），绝不在持锁时做 I/O 或取第二把锁。帧的所有权就是 order-0 引用计数：缓存持一引用，`filepage_get()` 另给调用方一个（交给它要装的 PTE），unmap 路径本来就会 `pmm_free`，因此缓存不必知道有几个地址空间指着这一页。**`MAP_PRIVATE` 刻意不进缓存**：私有映射的字节归进程所有，共用帧会让一个进程的写对另一个进程和文件都可见。
- **生命周期：VMA 持 file 引用 ⇒ 映射在，文件不会关 ⇒ 缓存不会在还有 PTE 指着时被丢**。`vma_insert` 接管调用方那一引用，split/fork 各自新取一个（**每节点恰一个**），erase/free 还回去；`vfs_close_file()` 在**最后一次引用**处挂 `filepage_drop_file(f)`，一并清掉该文件的所有缓存帧。
- **写回只在三处**（覆盖映射消失的每一条路）：`sys_munmap`（**`vma_erase` 之前**，若随后 `EINVAL` 则一帧未动、若 `ENOMEM` 则只是提前写回，方向不会错）、`exec_commit`（**第一行**，显式传 `old_pml4`——那时 CR3 已指向新地址空间，而旧根与旧 VMA 列表都还没被销毁）、`sys_exit`（`fb_disown` 之后、关 fd 之前——此后帧与打开的文件都还在）。**脏页由硬件 `PG_D` 认定**（`include/mm/mm.h` 补 `PG_A`/`PG_D`）：没写过的页零 I/O；**写回量 clamp 到 `vfs_file_size_now()`**——`vfs_file_size()` 是 open 时的快照，本进程刚建的文件在它眼里仍是 0，会把每一页都当"越过文件末尾"丢掉；写回成功才清 `PG_D`，失败留给下次重试。`vfs_pwrite(!buf)` 直接拒，防 `fat` 把 `NULL+0` 当 `O_TRUNC` 清空文件。
- **fork 不再把共享页变 COW**：`vma_fork_shared(t, ppml4, cpml4)` 在 `dup_user_aspace()` + `vma_dup()` 之后跑，把 `MAP_SHARED` 两侧 PTE 恢复 `PG_W`、清 `PG_COW`，**且不再 `pmm_ref`**（dup 已给子进程加过同一个帧的引用）。`sys_mprotect` 同理 consult VMA：共享页**直接给 `PG_W` 并清 `PG_COW`**——`pmm_refcount()>1` 对共享页是常态，按匿名规则标 COW 只会让本进程写一份谁也看不见的私有副本。
- **缺页分发器的 `demand_page()` 一支吃两支**（VMA 而非 fault 决定）：`MAP_SHARED` → `filepage_get()` 要那一帧；`MAP_PRIVATE` → 自取帧 + `vfs_pread` 填充（**短读正常**，帧先清零、尾部就是文件 EOF 外的零；负值才当作不可修复，取帧前就还回去）。配额照旧走 `cred_mem_quota()`：映射 100 MiB 文件不读就不花帧，也绕不过 64 MiB 上限。
- **`VMA` 扩三个字段**：`flags`（`LNXRM_MAP_*`）、`file_off`（`start` 在文件里的字节偏移，split 时随新 `start` 一起挪）、`file`；`vma_insert` 变 7 参。`vma_writeback(t,pml4,lo,hi)` 与 `vma_fork_shared(t,ppml4,cpml4)` 都显式收 `pml4`——两处调用时它都不是当前 CR3。
- **VFS 补页级 I/O**：`vfs_pread`/`vfs_pwrite` 走 vnode 节点操作、**绕开 `f->pos`**（需求分页与写回都不能动进程自己 `lseek` 出来的游标，也不能用"读改写游标"——两个调用方会抢一个位置），并各自检查描述符访问模式（`O_WRONLY` 照旧读不了页、`O_RDONLY` 照旧写不回）。
- **ktest 基线 137/4308 → 138 cases / 4360 checks / 0 failed**（late 95/3467 → 96/3519）：`ktest/t_mmap.c` 新增 `test_mmap_file`（52 checks）——拒绝零副作用（两种 flags 同给、未知位、未对齐 offset、`fd=99`、匿名带 offset、三参老形状）、私有映射读到文件字节且**不进缓存**、共享映射**两段区间同一帧**且缓存只增 1、共享页当场可写且**永不含 `PG_COW`**、`mprotect` 撤权/授予权都不改 COW、写回后 `PG_D` 清、文件内容变、**第二次写回 0 页**、`munmap`/`close`/`unlink` 后缓存与 buddy 与账目**逐字回到基线**。路径必须放用户页：`h_open` 走 `copy_user_str()`，字面量在 `.rodata` 会被拒（首跑就栽在这），改为 `syscall_probe6(SYS_mmap,…)` 出来的匿名页里放 `/FMAP.T`。
- **`systest` 新增 `t_mmap_file()`（ring 3 才能做的那半）+ smoke 判据 `SYSTEST_TAGS` 120 → 143**：私有读到文件/写不回、共享写在 `munmap` 落盘、**写回后文件仍 8 字节**（clamp 不是撑成一页）、四条拒绝（`-9`/`-22`/`-22`/`-13`），以及**真实 `fork`**：子进程写继承来的 `MAP_SHARED` 映射，父进程 `waitpid` 后必须读到同一个帧里的 `'C'`——这正是 `vma_fork_shared()` 要保的东西，漏掉它父进程只会读到 `'P'`(80)。用例自建自删 `/MF.T`，卷面不留下痕迹。
- **已知限制（如实记下，不粉饰）**：① 写回只在 `munmap`/`exec`/`exit` 三处发生，**没有 `msync`**——映射活着期间脏页只在帧里，`sync` 也不会替它落盘；② **共享页缓存与 `write()` 两条路互不知晓**：映射期间另一路 `write()` 改同一文件，映射看到的是旧字节，而映射侧的脏页写回还会盖掉 `write()` 的结果（Linux 靠同一套页缓存天然免疫；这里要修得跨所有地址空间 unmap 该文件范围的 PTE，代价不小）。约定：**别在有人 `MAP_SHARED` 映射的同时 `write()` 同一个文件**。详见 `include/mm/filepage.h` 头注释。
- **不做**：动态链接器与 `.so` 装载（已从"缺文件映射"改为"缺解释器"，见 3.2）、swap/OOM 回收、内核映射 NX/W^X（第 2 节既有开放项）。
- 验证：KTEST=0/1 与 userland 零警告；ktest **138 / 4360 / 0 failed**（early 25/404、mm 17/437、late 96/3519、2 skipped、1 刻意 warn），无 PANIC/BUG/Oops；16 bins；`make run` 正常到 `root@lnxrm$`；`make smoke` **240/240 PASS**。

### 2026-11-11 stat / gettimeofday 批次

- **ABI：`stat`（45）与 `gettimeofday`（46）追加到表尾**，`LNXRM_SYSCALL_LAST` 44 → 46，编号规则不变（追加、永不改号）。`struct lnxrm_stat` **56 字节**（`dev/ino/mode/nlink/uid/gid/size/blksize/blocks`，Linux 字段顺序的子集），`st_mode` 用 Linux 值的 `LNXRM_S_IF*` + 九个权限位（`lnxrm_abi.h` 里 `LNXRM_S_*` 全套），配 `_Static_assert` 钉死大小；`struct lnxrm_timeval` 16 字节、`struct lnxrm_timezone` 8 字节，同样有断言。
- **`st_dev` 是挂载槽号、从 1 起**（`vfs.c` 两处 `m - mounts + 1`）：0 专留"这个节点背后没有卷"（合成的 `/dev` 节点、未挂载时的回落），所以 0 与槽 0 的 `/` 不撞号；两条路径 `st_dev` 相等即同卷。`st_ino` 三源同号：FAT32 用 `fat_ino(ent_clus, ent_off)`（`include/fat32.h` 的 static inline，**与 `getdent` 共用同一份**，所以 stat 和 readdir 对同一条记录给出同一个号），initramfs 用归档槽 `i+1`（0 留给"无记录"），`/dev/console` 用 `dev_getdent` 已经在发的 1。`st_nlink` 恒 1（两套 fs 都没有硬链接，报 0 等于告诉调用方文件已消失）；`st_uid/st_gid` 恒 0（无文件属主，见 3.3）；`st_blksize` 512（一个扇区）。
- **`sys_stat` 收内核路径**（`h_stat` 做 `copy_user_str`，与 `sys_unlink` 同风格），VFS 语义走新 **`vfs_stat_path()`**：`flags=0`——**既不 create 也不 truncate**，所以 `vfs_lookup` 的 `O_CREAT` 臂从这里不可达，问一个不存在的路径就是 `ENOENT`，不会顺手造出一个没人要的文件；节点用完交 `close_node` 回收（C40，`/dev` 节点没有 op 就跳过）；**size 取 `fs_ops.size` 实时值**（`vfs_file_size_now()` 同源），不是 lookup 时 `vn->size` 的快照——刚写完关掉的文件必须报出现在的长度，否则 `cp` 还没读一个字节就被骗了。`mode` 由 `vnode_type` **全覆盖**地翻成 `S_IF*`：`V_REG` 0644（只读属性则 0444）、`V_DIR` 0755/0555、`V_CHR` 0666；FAT32 只有 FILE 只读属性一个权限位（`ATTR_READONLY 0x01`），initramfs 归档天生只读。
- **时间源三层落在新建的 `kernel/core/rtc.c` + `include/sys/rtc.h`**：① `rtc_utc_now()` 读 CMOS 寄存器——UIP 轮询上限 100000 次（正常为 0 次，QEMU 的 UIP 常清；卡死则回 `ok=false` 而不是无限转），BCD/12 小时制/世纪字节全解码，字段出界判为读数无效；② `wall_clock()` **首次调用采样一次**，之后一律 `rdtsc()` 按 `tsc_per_ms()` 演进（`tsc_per_ms()==0` 即 LAPIC 未标定时退 `jiffies`），单次状态机 0/1/2/3 + CAS，另加 `floor_raise()` 单调下限——两个 CPU 同时第一次调用不会各自发布一个基准，任何一次答案都不会比上一次更早；③ `rtc_format()` 逆 civil 换算出 `YYYY-MM-DDTHH:MM:SSZ`。UTC、无 DST：`gettimeofday` 的 timezone 半边**恒写 0**（与 Linux 一致）。`sys_gettimeofday(tv,tz)` 两指针都 NULL 回 `EINVAL`，单 NULL 合法。
- **ktest 新 suite `stat` 三例（LATE）**：`test_stat_paths`（`vfs_stat_path` 直调：常规文件/挂载根/合成设备/ENOENT 不创建/**size 实时性**——写 10 字节关掉报 10、再 O_TRUNC 报 0、unlink 后回 ENOENT）、`test_stat_dispatch`（`syscall_probe` 走用户页：字符串门与拷贝门逐个撞——NULL 缓冲、NULL 路径、内核 `.rodata` 当路径、未映射页、内核栈地址，全是 `-14`；ENOENT 连问两次证明没创建；`LAST+1` 仍 `-38`）、`test_wall_clock`（换算钉点 `1970-01-01`=0、`2000-03-01`=11017、`1980-01-01`=**3652**，格式化三钉点，RTC 读数落在 2020–2099，gettimeofday 与原始读数差 <60 s，两次调用**不回退**，timezone 回 0，两 NULL `-22`）。VA 用 0xB000/0xD000（0xC000 是共享的 EFAULT 保留页）。
  - 过程中踩到两个值得记的坑：**ktest LATE 阶段 `jiffies` 恒 0、`tsc_per_ms()==0`**（LAPIC tick 要等 ktest 全跑完才起），所以"等一个 tick"是死等——两处改用计数 `pause` 忙等，断言只要求"不回退"，`usec` 可能为 0 属正常；以及**测缓冲区门之前必须先把可解析的路径放回用户页**，否则 `/NOSUCH.T` 的 `ENOENT` 会先于缓冲区门返回，什么都证明不了。
- **`systest` 新增 `t_stat_time()`（ring 3 才能证明的那半）+ smoke 判据 `SYSTEST_TAGS` 143 → 173（+30）**：stat 侧 21 条（类型/权限条件、`nlink=1`、`uid=0`、`ino≠0`、`blksize=512`、`blocks` 进位、`/` 是目录且 size 0、`/dev/console` 是字符设备且 `ino=1`、`ENOENT`/`EFAULT`×2、**问两次都不创建**、写 10 字节后 `size=10`、unlink 复原）+ 时间侧 9 条（`epoch=1[78]\d{8}` 落在 2026–2028 一带、2020–2100 区间、usec 合法、不回退、timezone 清零、仅 zone 合法、两 NULL `-22`）。判据报**条件**而非原始数值——`README.md` 是 0644 还是只读取决于盘上那一个属性位，而"它是普通文件""size 就是现在这个 size"在任何能挂载的文件系统上都成立；`time.epoch` 原样打印，供人眼与宿主日期对表。
- **不做**：`fstat`/`futimes`（描述符侧元数据，见 3.8）、`clock_gettime` 的单调钟与 `CLOCK_REALTIME` 之外的钟、时区/DST 数据库、写时钟的 setter（CMOS 是权威，本内核不改它）。
- 验证：KTEST=0/1 与 userland 零警告；ktest **141 / 4460 / 0 failed**（early 25/408、mm 17/437、late 99/3615、2 skipped、1 刻意 warn），日志含 `[rtc] now=2026-11-11T05:18:54Z`（与当天一致）；16 bins、镜像含 `stat.enoent`/`time.bothnull` 串；`make run` 正常到 `root@lnxrm$` 无 PANIC；`make smoke` **270/270 PASS**。
