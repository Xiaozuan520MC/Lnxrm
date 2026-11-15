# lnxrm 差距分析：现在缺什么、错什么、先修什么

> 对象：`lnxrm` —— x86-64 freestanding 单体内核（C + NASM），QEMU 为主目标。构建与运行见 BUILD.md，代码结构见 CODEBASE.md。
>
> 已核对基线：系统调用 **47 个（0–46 连续无空洞）**，图形 6（23–27 + `fb_flip` 32）、输入 5（34–38）+ 统一队列 3（39–41）、`mmap`/`munmap`/`mprotect` 42–44（42 为 Linux 六参：匿名 + `MAP_SHARED`/`MAP_PRIVATE` 文件映射）、`stat` 45、`gettimeofday` 46；
> ktest **148 cases / 4561 checks**（q35，2 skipped、1 刻意告警；pc 档 **4539 checks** 0 failed，`ahci` 无设备即 skip）；`make smoke` **270/270**（`--repeat 2` 连跑 **487/487**，2026-11-13 首验与 2026-11-14 C7 复验均两轮全绿，`--timeout 900`）；`make` 零警告。
>
> 编号 C1–C72 是稳定 ID（README 与代码注释有交叉引用），不重排；主体只收**仍然开放**的条目，已关闭的在第 6 节。

---

## 1. 成熟度一览

评级：0 缺失 · 1 骨架 · 2 部分可用有明显缺陷 · 3 可用且语义正确 · 4 稳健 · 5 可投产。

| 维度 | 分 | 现状与主要缺口 |
|---|---|---|
| 引导与启动 | 4 | QEMU/GRUB 双路径、VBE 级联、E820；缺 GRUB 自动化（C68）、只用单个最大 E820 区（C69）、固定地址与 loader 指针未校验（C66/C67/C70） |
| 物理内存 | 3 | buddy、按进程记账、64 MiB 配额；**OOM killer 已收口（2026-11-12）**；仍无回收、无 swap |
| 虚拟内存 | 3 | 三槽 PML4、用户页 NX、懒栈、栈保护页、`mmap`/`munmap`/`mprotect`（匿名 + 文件映射）、COW fork；**内核镜像 W^X 已收口（2026-11-12）**；无 swap/reclaim |
| 进程与信号 | 3 | `fork/execve/wait4/brk/kill` + 信号三件套，父链接存 `(pid, 世代)`；无 CPU 时间限制 |
| 调度 | 3 | 每 CPU 队列、负载均衡、本地 tick；队列为尾插 FIFO/RR（C62 收口 2026-11-13）、内核态不可抢占、无优先级 |
| SMP | 3 | MADT 枚举、xAPIC/x2APIC 双路、AP 时钟标定；全局锁 + 轮询 I/O 使延迟敏感场景不可用 |
| 系统调用 / ABI | 3 | 47 个全实现，指针一律 `copy_*_user` → `user_ptr_ok`（写侧先解 COW）；缺 `fstat`、单调钟、`pipe` |
| 并发正确性 | 2 | 打印锁/任务表锁/fd 引用计数已就位；`pick_next` 读已纳入锁（C4 收口）、块设备每设备锁 + FIFO 队列已半修（C10，余项合并/取消待 C7 余项）；缓存锁已移出设备 I/O、tick 回写门改 trylock、FS 锁改不接管中断状态（**C7 半修 2026-11-13**）；余 `fat_fs_lock` 仍跨 I/O |
| 设备驱动 | 3 | PS/2 键鼠中断驱动、AHCI 中断 + DMA、IDE LBA48、多盘挂载分流（C49/C51/C53）；PCI 配置机制半修（C56） |
| 文件系统 | 3 | FAT32 + on-disk redo journal + 时间戳 LRU 块缓存 + 簇链步数预算；边界/挂载批次已收口（C28–C47 族，见第 6 节） |
| 图形 / 显示 | 3 | 帧缓冲控制台 + 双缓冲 `fb_flip` + `CAP_FB`/归属两道门；无图元（暂不作为缺口） |
| 用户态 | 2 | 16 个静态 ELF64 + 自带 crt0/迷你 libc，mmap 三件套就位；无动态链接器，跑不了第三方软件 |
| 安全模型 | 2 | `cred` + 3 能力位 + `setuid` 单向降权；NXE/WP/SMEP/SMAP + **W^X 已收口（2026-11-12）**；无 gid、RLIMIT、文件属主、KASLR、栈保护 |
| 测试 | 3 | ktest 148/4561（故障注入、OOM 注入与四例、W^X 逐页钉点、mmap 十一例、块队列 FIFO、C7 锁外 I/O 钉点）、smoke 270/270 与 `--repeat 2` 连跑 487/487 全绿；无 CI、无宿主单测 |
| 可观测性 | 2 | 串口日志、分阶段启动、`ps` 内存列、块缓存统计；无 loglevel、崩溃环形缓冲、backtrace、`/proc` |
| 可移植 / 启动安全 | 2 | 门控固定 QEMU pc + TCG + `-m 256`；无 KASLR/KPTI、入口无 `swapgs`、无 IST |

---

## 2. 开放缺陷

| 编号 | 位置 | 后果 | 修法与量级 |
|---|---|---|---|
| C7 | `blk_cache.c` + `FAT_ENTER()`（**半修**） | 缓存层已收口（2026-11-13）：`cache_lock` 只做元数据（槽位预留 + 弹跳写回），`blk_io_*` 全在锁外；tick 回写门改 trylock，中断不再自旋等锁；`fat_fs_lock` 改不接管中断状态（取用点本就 IF=0，契约改动）。**余项**：`fat_fs_lock` 仍跨整个 FS 操作含 I/O（拆它要给每个读-改-写序列加重校验/重试）；tick 门胜出时回写 I/O 仍在 IRQ 里做（`jiffies` 传输段冻结，C27 既有） | FS 层按「纯读放锁 / 变更持锁」拆两套入口，约 3–5 天 |
| C10 | `include/sys/vfs.h` `blkdev`（**半修**） | 每设备 `io_lock` + FIFO 请求队列已落地、驱动全局锁已删（2026-11-13）；合并与取消仍缺——调用者全程压着 `fat_fs_lock`（C7 半修未拆它），待 C7 余项 | 合并/取消（依赖 C7 余项），约 200 行 |
| C56 | `pci.c`（**半修**） | visited + 深度限制已加；`0xCF8/0xCFC` 仍对所有总线同拼 type-1（仅 bus 0 有保证），头注释与实现矛盾 | 按总线选机制 + 改注释，半天 |
| C57 | `mm.h` `PMM_WINDOW_TOP`、`pmm.c` | 位图固定从物理 0x10000 排布，窗口再扩会撞 0x50000 页表——只写在注释里的隐式依赖 | 位图随窗口核算 + 断言，半天 |
| C61 | `vmm.c:57-84`（**半修**） | 只放宽不覆盖已收口；`invlpg` 打在当前 AS 而非目标 root，目标不是当前 AS 时刷新无效 | 目标为当前 AS 才 invlpg，几行 |
| C66 | `main.c:61-90` | `boot_params` 指针只判非零不校验范围（注释却称"已校验"） | 落在已知 RAM 内才用 + 改注释，约 20 行 |
| C67 | `main.c:208-216` | 无 loader 参数时读固定物理 `CMDLINE_QEMU_PA`（0x20000）当命令行 | 只在 loader 声明时读 + 长度上限，约 20 行 |
| C68 | `Makefile`、`smoke_test.py` | smoke 固定 pc + TCG + `-m 256`：ISO、q35+KVM（`make run` 默认）、内存档位、双盘都不在门控里 | smoke 矩阵化，约 1 天 |
| C69 | `pmm.c:96-112` | 只用单个最大 E820 区并钳到 2 GiB，其余 RAM 完全不参与分配 | 合并全部区域 + 分段别名，约 2 天 |
| C70 | `entry64.S:4-7` | 固定物理 0x50000–0x57000 的页表从不与 E820 校验冲突 | 启动早期校验并拒绝，约 20 行 |

---

## 3. 能力差距

| 主题 | 缺口 | 影响 | 量级 |
|---|---|---|---|
| IPC | `pipe` 与 shell 管道 | 工具无法组合 | 1–2 天 |
| IPC | shm 便利层（底座 `mmap(MAP_SHARED, fd)` 已就位） | 交换仍得自建文件、自己映射 | 0.5 天 |
| IPC | unix domain socket | 服务/客户端模型前提 | 约 1 周（最后做） |
| IPC | 线程 / `clone` | 进程模型是单线程假设 | 约 2 周，需重审并发结论 |
| IPC | `alarm` / 定时信号 | 无超时与定时唤醒，只有 `nanosleep` | 0.5 天 |
| 内存 | 动态链接器与 `.so` 装载 | 文件映射已就位但 loader 不用它，跑不了第三方程序 | 1–2 周 |
| 内存 | swap/reclaim、`kmalloc`/`brk` 失败归因 | OOM killer 只补了缺页这半边：杀掉的帧要等 exit + reap 才回池 | 1 周起 / 归因约 200 行 |
| 权限 | gid 与文件属主 | 任何进程可 `write`/`unlink` 任意文件 | 约 200 行 |
| 权限 | CPU 时间限制、RLIMIT | 可无限占 CPU；`NR_FDS 16` 等编译期常量 | 80 / 100 行 |
| 权限 | 登录会话、文件系统沙箱 | 无"天生的普通用户"，冒烟程序都以 root 跑 | 约 1 周 / mmap 就位后再评估 |
| 加固 | KASLR、栈 canary | 链接地址固定、gadget 定位零成本；靠 `-fno-stack-protector` 纪律 | 100–200 / 50 行 |
| 加固 | `swapgs`+TLS、IST、KPTI | 无 TLS、无故障栈（引导栈 16 KiB 无 guard）、用户页表带完整内核映射 | 30 / 80 行 / 先量代价 |
| CI | CI 配置、smoke 矩阵、宿主单测 | 门控只在本地手动跑一轮（C68）；连跑抖动门 `--repeat` 已加（2026-11-13），缺 CI 接入 | 各 1–2 天 |
| 抢占 | syscall 全程关中断、内核态不可抢占 | 慢盘 I/O 期间本核无时钟、无抢占（C7 后仅剩本核：别核的时钟不再被慢盘拖住）；长临界区拖住整核 | 1 天+审计 / 1–2 周 |
| 可观测 | loglevel、崩溃环形缓冲、backtrace、`/proc`、debug syscall、`reboot`、统计计数器 | panic 后串口无从回读、黑盒测试受限 | 合计约 700 行 |
| syscall | `fstat`（描述符侧元数据）、`clock_gettime`（单调钟）、`chdir`/`getcwd`/`umask`、`fcntl`/`poll`/`select`（多路等待）、`chmod`/`link` | 路径侧 `stat`(45) 与 `gettimeofday`(46) 已收口，以上仍缺 | 各半天–3 天 |
| syscall | 固定容量 `NR_TASKS 64`/`NR_FDS 16`/`MAX_MOUNTS 4`/缓存 64 槽 | 用满即失败且诊断有限 | 约 1 天 |
| 存储 | 块请求合并/取消（队列已就位，见 C10 半修）、用户可见 umount | 同 LBA 的连续请求仍各发一趟；内核侧挂载 API 齐但无 syscall | 见 C10 / 约 1 天 |

---

## 4. 安全定性

| 场景 | 结论 | 依据（简） |
|---|---|---|
| 跑自己写的 `userland/` 程序 | 安全 | `sigreturn` 恢复内核保存的帧；ELF 七项校验；指针一律 `copy_*_user`；表外编号 `ENOSYS`；用户态异常只杀进程；`kprintf` 无 `%n` |
| 跑不完全信任的程序 | 不安全 | 权限止于 uid + 3 能力位；全程关中断 + 一把全局 FS 锁；已 W^X 但无栈保护、无 KASLR |
| 挂载可信磁盘 | 基本安全 | redo journal + mount replay + 故障注入；三道同步门（C27）；静默损坏三件与 vnode 泄漏已修（C19/C20/C21/C40）；写路径 `0xE7` FLUSH 检查（C55） |
| 挂载不可信/可能损坏的磁盘 | 不安全 | 簇链步数预算 + 单簇自环即断、BPB 校验、目录游标/LFN/容量/MBR 边界均收口；但有界长走持 irqsave 锁、无 checksum、目录内容逐扇区信任读回 |
| 硬件缓解 | 已开但不完整 | NXE/WP/SMEP/SMAP 置位有 ktest 钉点，**内核 W^X 已收口（2026-11-12）**；缺 KASLR、KPTI、canary、IST |
| 横向定位 | 与 `xv6` 同档 | 同为简单权限模型、同无 KASLR；`sigreturn`、ELF 解析、`copy_user` 更严谨 |

---

## 5. 优先级清单

一行一项，按依赖顺序；括号引用第 2、3 节。

1. 锁与抢占：C7 半修已落地（缓存层锁外 I/O + tick 门 trylock + FS 锁不接管中断，2026-11-13）→ C7 余项（`fat_fs_lock` 拆出 I/O）→ C10 余项（合并/取消，依赖 C7 余项）→ 内核态抢占与 syscall 关中断（第 3 节「抢占」），每步以 smoke 兜底。
2. 验证面：smoke 矩阵化（C68）→ CI（第 3 节「CI」）；连跑抖动门 `--repeat N` 已就位（OBS-1 收口 2026-11-13）。
3. 可观测性最小集：loglevel + 崩溃环形缓冲 + backtrace（第 3 节「可观测」）——此后并发改动才可诊断。
4. 内核自身加固：canary、`swapgs`/IST、KASLR（第 3 节「加固」）；内核 W^X 已收口（2026-11-12）。
5. 文件权限与限额：inode 属主 + 写检查点 + CPU 时间限制（第 3 节「权限」）。
6. 驱动收口：PCI 配置机制与注释（C56）；AHCI 中断/多盘、IDE LBA48 已收口（C49/C51/C53）。
7. 能力面：`pipe`（第 3 节「IPC」）→ 动态链接器与共享库（「内存」，文件映射已是前置）。
8. 暂不做：新子系统、更换文件系统、POSIX 全覆盖、网络栈——1–7 未落地前不加宽度。

---

## 6. 已收口条目

一行一条，只记结论；实现细节看对应 ktest 用例与 git log。

- **2026-10-25**：C16 写超卷先拒 `ENOSPC` 零分配；C17 LFN 清零 + 校验和/序号校验；C19/C20/C21 静默数据损坏三件（`O_TRUNC` 落盘、写回按槽定位、找不到 dirent 即 EIO 回滚）；C23 目录槽不足先扩链；C27 脏页三道同步门 + `SYS_sync(33)`；C29 `bps==512` 挂载层锚定；C35 空写回 0；C38 BPB 三项校验；C40 close 释放两层分配；C41 访问位生效；C44 getdent 小缓冲即 `EINVAL`。
- **2026-10-30 边界与校验**：C30 getdent 物理恢复点游标；C31 LFN 260 字符缓冲；C32 表项 >2^28 拒挂载；C33 `max_cluster` 独占上限；C34 resolve 全对象清零；C36 MBR/EBR 范围校验与链遍历；目录结束符回归规范（0x00 止看、0x05 活记录），单簇自环即断。
- **2026-11-04 挂载与设备**：C28 分区先注册入 `diskinfo`；C39 挂载 ops/priv 成对下传；C43 close CAS 引用计数不回绕；C47 最长前缀挂载路由 + `vfs_umount`（不加 syscall，ABI 冻结）；C55 IDE 写后 `0xE7` FLUSH 并检查返回。
- **2026-11-05 中断 I/O 与容量**：C49 AHCI 中断驱动；C51 多盘挂载分流（有 `/bin/init` 挂 `/`、余挂 `/mnt`）；C53 IDE LBA48；#UD 根因修复（`isr_common` 只发布 ring-3 帧）；q35 幻影盘回收，boot 33.8 s → 8.5 s。
- **2026-11-06 双缓冲与 LFB 写合并**：呈现频率门只认首 tick、present 全程持 `console_out_lock` 消撕裂、`panic()` 改无锁 flip；每 CPU PAT/MTRR 写合并（固件 UC 覆盖则如实落 UC）。
- **2026-11-07 匿名 mmap / 缺页分发 / COW fork**：42/43 匿名映射 + VMA 登记 + 需求分页（`brk` 上限移到 `MMAP_LO`）；`page_fault_try_fix` 三段分发；COW 共享帧（order-0 引用计数，先复制再换 PTE）。
- **2026-11-07 栈保护页与 mprotect**：64 KiB 守护区永不映射 + RSP 邻近检查；44 `mprotect` 作用任意用户范围、授予 W 永不分配（共享帧标 `PG_COW`）；#PF `err` 位逐位认；拷贝门先补页；修 `MMAP_LO` 1 TiB 错值。
- **2026-11-03**：FAT 表分帧驻留（挂载上限改由物理内存决定）；`sched_wake_task` 自死锁修复；`t_input` waitlist 补排空。
- **2026-11-08 大扫除 1、2 轮**：信号表压实成 1..13 无空洞（`LNXRM_NR_SIGNALS 14`，smoke 断言改 `abi_define()` 现读）；删 11 处零引用 include；6 处重复逻辑合并；`syscall_do` 表驱动化（门与入口同一行、未知编号先不问门）；`systest.c` 942 行 main 拆 35 函数。
- **2026-11-09 大扫除第 3 轮**：`isr_common` 264→32 行、`ahciProbe` 334→48、`sys_execve` 187→49、`fat_mount` 196→48、`fat_scan_dir_at` 137→54（输出逐字不变）；删零引用 `S_IFREG`；smoke 判据 `SYSTEST_TAGS` 120 条、`past.*` 改按 `SYSCALL_LAST` 现算。
- **2026-11-09 文件映射**：42 扩成 Linux 六参（匿名 + `MAP_SHARED` 共享页缓存 + `MAP_PRIVATE` 自有帧）；校验全在选址之前；写回只在 munmap/exec/exit 三处，脏页由硬件 `PG_D` 认定。
- **2026-11-11 stat / gettimeofday**：45 `stat` 56 字节路径元数据（不发描述符，size 取实时值）；46 `gettimeofday` CMOS 采样一次 + TSC 演进，单调不回退。
- **2026-11-12 OOM Killer**：用户缺页取不到帧时杀持有最多用户页的进程（`kernel/mm/oom.c`），一次短缺只杀一个、闩由帧真出手开；能回答的失败照旧只回答。
- **2026-11-12 死代码删除**：`isr.c` 的 `irq==7||irq==15` 里 `intno==0xFF` 分支可证不可达并删除，判据不变。
- **2026-11-12 内核 W^X**：`arch/kernel.ld` 段边界 + `vmm.c` `vmm_kernel_wx()` 把镜像两个别名按段赋权（text R-X / `.rodata` R-- / data、bss RW-），镜像外低内存与共享槽保持 `P|W`（AP trampoline 可执行）；ktest 逐页钉点。
- **2026-11-13 PP0 批次**：C4 `pick_next` 的队头读纳入 `runq_lock`；C62 运行队列改尾插 FIFO/RR（钉 LIFO 的用例同步改写为 `test_runqueue_is_fifo_round_robin`）；C10 半修——`blkdev` 每设备 `io_lock` + FIFO 请求队列两段式入队/排空，删 `ide_lock`/`ahci_lock`，新增每设备锁与 FIFO 判据；OBS-1 smoke 加 `--repeat N` 连跑门（失败轮串口留档 `serial-fail-<i>.log`），`--repeat 2` 首验 **487/487 两轮全绿**。
- **2026-11-13 PP1 C7 半修**：`cache_lock` 改纯元数据锁——fill 先占槽（`valid=0`+`busy=1`，驱动直写 `data[]`）、写回先拷贝到任务自己的弹跳缓冲（`dirty=0`+`busy=1`，读者仍可命中），victim 与 purge 一律跳 `busy` 槽，`blk_io_*` 全部移到锁外（写回失败进 `tried` 位图防线性搜索回头）；tick 回写门改 **trylock**（中断里不再自旋等锁，同 CPU 持锁撞 tick 门的自死锁隐患一并消除）；`fat_fs_lock` 改 `spin_lock_irqon` **不接管中断状态**（取用点本就 IF=0——中断门进 syscall、ktest 在最终 `sti` 之前，故为契约改动而非行为变化，为第 3 节「抢占」的 syscall 开中断预留）；新增 `test_cache_lock_is_released_across_device_io`（IO probe 逐点断言锁外），**148 cases / 4561 checks**。余项：`fat_fs_lock` 仍跨 I/O，C10 合并/取消继续被挡。
- **2026-11-14 换链（LLVM 工具链）**：`Makefile` `LD` `ld`→`ld.lld`、`OBJCOPY` `objcopy`→`llvm-objcopy`，`userland/Makefile` `USERLDFLAGS` 加 `-fuse-ld=lld`（clang 驱动默认仍调 `/usr/bin/ld`）；删 `--no-warn-rwx-segments`（GNU ld 2.39+ 专属，lld 不认、也本就没有 RWX 段警告）。`arch/kernel.ld` 把孤儿段 `.trampoline` **显式摆成输出段**（VMA = `.image` 末尾、LMA 紧随其后、`.bss` 的 `AT` 随之后移）——lld 对「PROGBITS 与 NOBITS 负载地址重叠」比 GNU 严格，原脚本会以 `section .trampoline load address range overlaps with .bss` 拒链；对齐填充用 `ALIGN` 会多 3 字节，已去掉以求与基线逐位一致。**产物对齐基线**：KTEST=0 **117405** 字节、KTEST=1 **331037** 字节，`.image`/`.trampoline`/`.bss` 段布局与 trampoline 字节与 GNU ld 逐位相同，`nm` 全表仅 `__kernel_end_lma`（全仓零引用）与 17 个 `.rodata` 局部符号位移；三档零警告，ktest q35 **148 cases / 4561 checks / 0 failed**、C7 钉点在位，同负载启动 `ready` **243.6 s（lld）vs 295.3 s（GNU）** 无回退。注意：payload 字节与 `strings` 集合**不再与 GNU 逐字一致**——lld 对 `SHF_MERGE|SHF_STRINGS` 段做尾串合并并裁掉 `--gc-sections` 后无人引用的死串（如无调用者的 `kmalloc_or_panic` 的 panic 格式串），GNU ld 留着；判定口径是符号表 + 运行时，不是字节。
- **2026-11-14 构建提速（KTEST 对象分目录）**：`Makefile` 引入 `OBJDIR := $(BUILD)/obj$(KTEST)`，对象与 `.d` 全部按模式分目录（`build/obj0` 42 个 / `build/obj1` 72 个），两模式并存，删掉旧的 `build/.ktest-mode-<n>` stamp——它靠「翻转即作废全部对象」防两模式混装，代价是 `make`↔`make smoke` 每次互切都全量重编 70 个 TU（无 `-j` 实测 **8.89 s + 5.42 s = 14.4 s** 白编，源码零改动、刷 70 行 `CC`）。改后**切换 0.19–0.22 s、`CC`=0**（只剩一次重链 + `patch_setup_jmp.py`/`patch_bzimage.py`），产物随模式正确翻转 117405 ↔ 331037。两个坑（详见 BUILD.md）：① 模式标记 `build/.mode-<n>` 必须挂 `vmlinux.elf` 而非对象——翻转后每个 `.o` 都比 elf 旧，不挂链上 `make` 会把上一模式的 `bzImage` 当最新发出；挂回对象则退回全量重编。② 该规则必须排在 `all:` 之后——它是 Makefile 里第一个不以 `.` 开头的目标，写前面会夺走默认目标，`make` 从此只检查标记、再也不建 `bzImage`。产物一律仍在 `build/` 根（`smoke_test.py` 硬编码 `bzImage`/`vmlinux.elf`/`setup.bin`/`disk.img`/`user/`，不碰对象布局）。**验证止于编译层**（本轮按约定不跑测试）：KTEST=0 冷构建 40 CC 零警告、`nm` 756 符号与基线逐字一致；KTEST=1 首建 70 CC 零警告、`nm` 1239 符号与基线逐字一致；`.image`/`.trampoline`/`.bss` 段布局正常；`make user` 零警告；smoke 硬编码产物齐全（`disk.img` 64 MiB 已建）。ktest/smoke 回归留待下轮补跑。
- **2026-11-15 注释清理**：只动注释、代码零改动——删掉复述代码的废话、历史叙事（`used to`/`the old`/旧实现对照）、超长文件头与同一规则的多处重复（留 `/* see mm.h */` 一处权威），保留结构横幅、行尾契约、测试 ID 与非显然的「为什么」（硬件故障诊断记录）。15 个文件（`filepage.h`/`vma.h`/`vfs.h`/`fat32.h`/`mm.h`/`pagefault.c`/`oom.c`/`ahci.c`/`framebuffer.c`/`t_cache.c`/`ktest.h`/`t_prop.c`/`blk_cache.c`/`task.c`/`vfs.c`）注释 2796 → 2533（-263），全树 8335 → 8213、占非空行 26.9% → 26.6%。注释对编译耗时零影响（ahci/vfs/sched 各 5 轮完整 `-c` 差 +0.00 s）。**验证止于编译层**（按约定不跑测试）：三档零警告（`-Wall -Wextra -Wno-unused-parameter -Os`），KTEST=0 117405 / `nm` 756、KTEST=1 331037 / `nm` 1239 与基线一致，`make user` 零警告；`git diff` 逐块确认每文件只有注释/空行变化（`K_ASSERT`/`ASSERT` 烤入 `__FILE__`/`__LINE__`，故产物 md5 会随注释行号变化，判据用尺寸 + `nm` 计数而非逐字节）。
