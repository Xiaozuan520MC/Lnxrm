# lnxrm 差距分析：现在缺什么、错什么、先修什么

> 对象：`lnxrm` —— x86-64 freestanding 单体内核（C + NASM），QEMU 为主目标。构建与运行见 BUILD.md，代码结构见 CODEBASE.md。
>
> 已核对基线：系统调用 **47 个（0–46 连续无空洞）**，图形 6（23–27 + `fb_flip` 32）、输入 5（34–38）+ 统一队列 3（39–41）、`mmap`/`munmap`/`mprotect` 42–44（42 为 Linux 六参：匿名 + `MAP_SHARED`/`MAP_PRIVATE` 文件映射）、`stat` 45、`gettimeofday` 46；
> ktest **146 cases / 4522 checks**（q35，2 skipped、1 刻意告警；pc 同判据 0 failed，`ahci` 无设备即 skip）；`make smoke` **270/270**；`make` 零警告。
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
| 调度 | 3 | 每 CPU 队列、负载均衡、本地 tick；队列为 push-front LIFO（C62）、内核态不可抢占、无优先级 |
| SMP | 3 | MADT 枚举、xAPIC/x2APIC 双路、AP 时钟标定；全局锁 + 轮询 I/O 使延迟敏感场景不可用 |
| 系统调用 / ABI | 3 | 47 个全实现，指针一律 `copy_*_user` → `user_ptr_ok`（写侧先解 COW）；缺 `fstat`、单调钟、`pipe` |
| 并发正确性 | 2 | 打印锁/任务表锁/fd 引用计数已就位；跨设备 I/O 两层 irqsave（C7/C10）、`pick_next` 无锁读（C4） |
| 设备驱动 | 3 | PS/2 键鼠中断驱动、AHCI 中断 + DMA、IDE LBA48、多盘挂载分流（C49/C51/C53）；PCI 配置机制半修（C56） |
| 文件系统 | 3 | FAT32 + on-disk redo journal + 时间戳 LRU 块缓存 + 簇链步数预算；边界/挂载批次已收口（C28–C47 族，见第 6 节） |
| 图形 / 显示 | 3 | 帧缓冲控制台 + 双缓冲 `fb_flip` + `CAP_FB`/归属两道门；无图元（暂不作为缺口） |
| 用户态 | 2 | 16 个静态 ELF64 + 自带 crt0/迷你 libc，mmap 三件套就位；无动态链接器，跑不了第三方软件 |
| 安全模型 | 2 | `cred` + 3 能力位 + `setuid` 单向降权；NXE/WP/SMEP/SMAP + **W^X 已收口（2026-11-12）**；无 gid、RLIMIT、文件属主、KASLR、栈保护 |
| 测试 | 3 | ktest 146/4522（故障注入、OOM 注入与四例、W^X 逐页钉点、mmap 十一例）、smoke 270/270；无 CI、无宿主单测 |
| 可观测性 | 2 | 串口日志、分阶段启动、`ps` 内存列、块缓存统计；无 loglevel、崩溃环形缓冲、backtrace、`/proc` |
| 可移植 / 启动安全 | 2 | 门控固定 QEMU pc + TCG + `-m 256`；无 KASLR/KPTI、入口无 `swapgs`、无 IST |

---

## 2. 开放缺陷

| 编号 | 位置 | 后果 | 修法与量级 |
|---|---|---|---|
| C4 | `sched.c` `pick_next()` | 不持 `runq_lock` 读队列链，跨核入队/出队时可能读到半更新状态 | 读纳入锁内，约 10 行 + 双核压测 |
| C7 | `blk_cache.c` + `FAT_ENTER()` | 缓存锁与 FS 锁两层 irqsave 跨设备 I/O，慢盘把等待放大到全机 | 元数据与 I/O 分离，约 2 天 |
| C10 | `include/sys/vfs.h` `blkdev` | 块设备只有全局锁：无队列、无合并、无取消，并发访问完全串行 | 多 slot 请求队列，约 300 行 |
| C62 | `sched.c` `runqueue_add` | push-front LIFO，持续争用下短任务可能被饿死；无亲和性 API | 队尾指针 + 公平性用例，约 20 行 |
| C56 | `pci.c`（**半修**） | visited + 深度限制已加；`0xCF8/0xCFC` 仍对所有总线同拼 type-1（仅 bus 0 有保证），头注释与实现矛盾 | 按总线选机制 + 改注释，半天 |
| C57 | `mm.h` `PMM_WINDOW_TOP`、`pmm.c` | 位图固定从物理 0x10000 排布，窗口再扩会撞 0x50000 页表——只写在注释里的隐式依赖 | 位图随窗口核算 + 断言，半天 |
| C61 | `vmm.c:57-84`（**半修**） | 只放宽不覆盖已收口；`invlpg` 打在当前 AS 而非目标 root，目标不是当前 AS 时刷新无效 | 目标为当前 AS 才 invlpg，几行 |
| C66 | `main.c:61-90` | `boot_params` 指针只判非零不校验范围（注释却称"已校验"） | 落在已知 RAM 内才用 + 改注释，约 20 行 |
| C67 | `main.c:208-216` | 无 loader 参数时读固定物理 `CMDLINE_QEMU_PA`（0x20000）当命令行 | 只在 loader 声明时读 + 长度上限，约 20 行 |
| C68 | `Makefile`、`smoke_test.py` | smoke 固定 pc + TCG + `-m 256`：ISO、q35+KVM（`make run` 默认）、内存档位、双盘都不在门控里 | smoke 矩阵化，约 1 天 |
| C69 | `pmm.c:96-112` | 只用单个最大 E820 区并钳到 2 GiB，其余 RAM 完全不参与分配 | 合并全部区域 + 分段别名，约 2 天 |
| C70 | `entry64.S:4-7` | 固定物理 0x50000–0x57000 的页表从不与 E820 校验冲突 | 启动早期校验并拒绝，约 20 行 |
| OBS-1 | `t_cache.c` 写后读一致用例 | 设备层时序偶发：2026-11-12 smoke 一次 `FLUSH CACHE (E7)` 设备 ERR → ktest 失败无 shell（判据 269 = 270 − 2 + 1），同次主启动同用例通过、紧接复跑 270/270 全绿——非内核回归 | 连跑 N 次抖动门 + 失败保存串口现场，约 1 天 |

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
| CI | CI 配置、smoke 矩阵、宿主单测、抖动门 | 门控只在本地手动跑一轮（C68、OBS-1） | 各 1–2 天 |
| 抢占 | syscall 全程关中断、内核态不可抢占 | 慢盘 I/O 期间本核无时钟、无抢占；长临界区拖住整核 | 1 天+审计 / 1–2 周 |
| 可观测 | loglevel、崩溃环形缓冲、backtrace、`/proc`、debug syscall、`reboot`、统计计数器 | panic 后串口无从回读、黑盒测试受限 | 合计约 700 行 |
| syscall | `fstat`（描述符侧元数据）、`clock_gettime`（单调钟）、`chdir`/`getcwd`/`umask`、`fcntl`/`poll`/`select`（多路等待）、`chmod`/`link` | 路径侧 `stat`(45) 与 `gettimeofday`(46) 已收口，以上仍缺 | 各半天–3 天 |
| syscall | 固定容量 `NR_TASKS 64`/`NR_FDS 16`/`MAX_MOUNTS 4`/缓存 64 槽 | 用满即失败且诊断有限 | 约 1 天 |
| 存储 | 块请求队列/合并、用户可见 umount | 见 C10；内核侧挂载 API 齐但无 syscall | 见 C10 / 约 1 天 |

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

1. 锁与抢占：C7（锁外做 I/O）→ C10（请求队列）→ C62/C4 → 内核态抢占与 syscall 关中断（第 3 节「抢占」），每步以 smoke 兜底。
2. 验证面：smoke 矩阵化（C68）+ 连跑抖动门（OBS-1）→ CI（第 3 节「CI」）。
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
