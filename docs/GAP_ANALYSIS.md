# lnxrm 差距分析：现在缺什么、错什么、先修什么

> 对象：`lnxrm` —— x86-64 freestanding 单体内核（C + NASM），QEMU 为主目标。构建与运行见 BUILD.md，代码结构见 CODEBASE.md。
>
> 基线（2026-11-16）：系统调用 **53 个（0–52 连续无空洞）**，末五条 `reboot` 48、`chdir` 49、`getcwd` 50、`shm_open` 51、`shm_unlink` 52；ktest **158 cases / 4754 checks**（3 skipped、1 刻意告警）、`make smoke` **270/270**、三档 `make` 零警告——各项来历见第 6 节。
>
> 编号 C1–C72 是稳定 ID（README 与代码注释有交叉引用），不重排；主体只收**仍然开放**的条目，已关闭的在第 6 节。

---

## 1. 成熟度一览

评级：0 缺失 · 1 骨架 · 2 部分可用有明显缺陷 · 3 可用且语义正确 · 4 稳健 · 5 可投产。

| 维度 | 分 | 现状与主要缺口 |
|---|---|---|
| 引导与启动 | 4 | QEMU/GRUB 双路径、VBE 级联、E820；缺 GRUB 自动化（C68）、只用单个最大 E820 区（C69）、固定地址与 loader 指针未校验（C66/C67/C70） |
| 物理内存 | 3 | buddy、按进程记账、64 MiB 配额、OOM killer（2026-11-12 收口）；无回收、无 swap |
| 虚拟内存 | 3 | 三槽 PML4、用户页 NX、懒栈、栈保护页、`mmap` 三件套、COW fork、内核 W^X（2026-11-12 收口）；无 swap/reclaim |
| 进程与信号 | 3 | `fork/execve/wait4/brk/kill` + 信号三件套，父链接存 `(pid, 世代)`；无 CPU 时间限制 |
| 调度 | 3 | 每 CPU 队列、负载均衡、本地 tick；尾插 FIFO/RR（C62）、内核态不可抢占、无优先级 |
| SMP | 3 | MADT 枚举、xAPIC/x2APIC 双路、AP 时钟标定；全局锁 + 轮询 I/O 使延迟敏感场景不可用 |
| 系统调用 / ABI | 3 | 53 个全实现，指针一律 `copy_*_user` → `user_ptr_ok`（写侧先解 COW）；缺 `fstat`、单调钟 |
| 并发正确性 | 2 | 打印锁/任务表锁/fd 引用计数已就位；C4、C62、C10 半修，**C7 半修（2026-11-13）**已把缓存锁移出设备 I/O；余 `fat_fs_lock` 仍跨 I/O（见 C7） |
| 设备驱动 | 3 | PS/2 键鼠中断驱动、AHCI 中断 + DMA、IDE LBA48、多盘挂载分流（C49/C51/C53）；PCI 配置机制半修（C56） |
| 文件系统 | 3 | FAT32 + on-disk redo journal + 时间戳 LRU 块缓存 + 簇链步数预算；边界/挂载批次已收口（C28–C47，见第 6 节） |
| 图形 / 显示 | 3 | 帧缓冲控制台 + 双缓冲 `fb_flip` + `CAP_FB`/归属两道门；无图元（暂不作为缺口） |
| 用户态 | 2 | 16 个静态 ELF64 + 自带 crt0/迷你 libc，mmap 三件套就位；无动态链接器，跑不了第三方软件 |
| 安全模型 | 2 | `cred` + 3 能力位 + `setuid` 单向降权；NXE/WP/SMEP/SMAP + **W^X 已收口（2026-11-12）**；无 gid、RLIMIT、文件属主、KASLR、栈保护 |
| 测试 | 3 | ktest 158/4754、smoke 270/270 全绿（2026-11-16 S0 轮实测）、`--repeat 2` 连跑 487/487（2026-11-15，本轮未复跑）；无 CI、无宿主单测 |
| 可观测性 | 3 | 串口日志、分阶段启动、`ps` 内存列、块缓存统计；**loglevel + 崩溃环 + backtrace 已收口（2026-11-15）**；无 `/proc` |
| 可移植 / 启动安全 | 2 | 门控固定 QEMU pc + TCG + `-m 256`；无 KASLR/KPTI、入口无 `swapgs`、无 IST |

---

## 2. 开放缺陷

| 编号 | 位置 | 后果 | 修法与量级 |
|---|---|---|---|
| C7 | `blk_cache.c` + `FAT_ENTER()`（**半修**） | 缓存层已收口（2026-11-13）：`cache_lock` 只做元数据（槽位预留 + 弹跳写回），`blk_io_*` 全在锁外；tick 回写门改 trylock；`fat_fs_lock` 改不接管中断状态。**余项**：`fat_fs_lock` 仍跨整个 FS 操作含 I/O（拆它要给每个读-改-写序列加重校验/重试）；tick 门胜出时回写 I/O 仍在 IRQ 里做（`jiffies` 传输段冻结，C27 既有） | FS 层按「纯读放锁 / 变更持锁」拆两套入口，约 3–5 天 |
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
| 可观测 | `/proc`、debug syscall、统计计数器 | panic 后已有崩溃环形缓冲 + backtrace 兜底（2026-11-15 收口），但运行期黑盒测试仍只能读串口 | 合计约 400 行 |
| syscall | `fstat`（描述符侧元数据）、`clock_gettime`（单调钟）、`umask`、`fcntl`/`poll`/`select`（多路等待）、`chmod`/`link` | 路径侧 `stat`(45)、`gettimeofday`(46)、`pipe`(47) 与 `reboot`(48)、`chdir`(49)、`getcwd`(50)、`shm_open`(51)/`shm_unlink`(52) 已收口，以上仍缺 | 各半天–3 天 |
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
3. 可观测性最小集：**已收口（2026-11-15）**——loglevel + 崩溃环形缓冲 + backtrace 见第 6 节；余项 debug syscall、统计计数器（第 3 节「可观测」）。
4. 内核自身加固：canary、`swapgs`/IST、KASLR（第 3 节「加固」）；内核 W^X 已收口（2026-11-12）。
5. 文件权限与限额：inode 属主 + 写检查点 + CPU 时间限制（第 3 节「权限」）。
6. 驱动收口：PCI 配置机制与注释（C56）；AHCI 中断/多盘、IDE LBA48 已收口（C49/C51/C53）。
7. 能力面：**`pipe` 与 shell 管道均已收口（2026-11-15，见第 6 节）**，下一项动态链接器与共享库（「内存」，文件映射已是前置）；IPC 余项（unix socket、线程、`alarm`）见第 3 节。
8. 暂不做：新子系统、更换文件系统、POSIX 全覆盖、网络栈——1–7 未落地前不加宽度。

---

## 6. 已收口条目

一行一条，只记结论与关键坑；实现细节看对应 ktest 用例与 git log。

- **2026-10-25**：C16 写超卷拒 `ENOSPC`、C17 LFN 校验和/序号、C19/C20/C21 静默损坏三件（`O_TRUNC` 落盘、按槽写回、找不到 dirent 回滚 EIO）、C23 目录扩链、C27 三道同步门 + `SYS_sync`、C29 `bps==512`、C35 空写回 0、C38 BPB 校验、C40 close 两层释放、C41 访问位、C44 getdent 小缓冲 `EINVAL`。
- **2026-10-30 边界与校验**：C30 getdent 恢复点游标、C31 LFN 260 字符缓冲、C32 表项 >2^28 拒挂载、C33 `max_cluster` 上限、C34 resolve 全对象清零、C36 MBR/EBR 范围与链遍历；目录结束符回归规范（0x00 止看、0x05 活记录），单簇自环即断。
- **2026-11-04 挂载与设备**：C28 分区先注册入 `diskinfo`、C39 挂载 ops/priv 成对下传、C43 close CAS 计数不回绕、C47 最长前缀路由 + `vfs_umount`（不加 syscall，ABI 冻结）、C55 IDE 写后 `0xE7` FLUSH 查返回。
- **2026-11-05 中断 I/O 与容量**：C49 AHCI 中断驱动、C51 多盘挂载分流（`/bin/init` 挂 `/`、余挂 `/mnt`）、C53 IDE LBA48；#UD 根因修复（`isr_common` 只发布 ring-3 帧）；q35 幻影盘回收，boot 33.8 s → 8.5 s。
- **2026-11-06 双缓冲与 LFB 写合并**：呈现频率门只认首 tick、present 全程持 `console_out_lock`、`panic()` 改无锁 flip；每 CPU PAT/MTRR 写合并（固件 UC 覆盖则落 UC）。
- **2026-11-07 匿名 mmap / 缺页分发 / COW fork**：42/43 匿名映射 + VMA 登记 + 需求分页（`brk` 上限移到 `MMAP_LO`）、`page_fault_try_fix` 三段分发、COW 共享帧（先复制再换 PTE）。
- **2026-11-07 栈保护页与 mprotect**：64 KiB 守护区永不映射 + RSP 邻近检查；44 `mprotect` 作用任意范围、授予 W 永不分配（共享帧标 `PG_COW`）；#PF `err` 位逐位认；拷贝门先补页；修 `MMAP_LO` 1 TiB 错值。
- **2026-11-03**：FAT 表分帧驻留（挂载上限改由物理内存决定）；`sched_wake_task` 自死锁修复；`t_input` waitlist 补排空。
- **2026-11-08 大扫除 1、2 轮**：信号表压实成 1..13 无空洞（smoke 断言改 `abi_define()` 现读）；删 11 处零引用 include、6 处重复逻辑合并；`syscall_do` 表驱动化；`systest.c` 942 行 main 拆 35 函数。
- **2026-11-09 大扫除第 3 轮**：`isr_common` 264→32、`ahciProbe` 334→48、`sys_execve` 187→49、`fat_mount` 196→48、`fat_scan_dir_at` 137→54（输出逐字不变）；smoke `SYSTEST_TAGS` 120 条、`past.*` 按 `SYSCALL_LAST` 现算。
- **2026-11-09 文件映射**：42 扩成 Linux 六参（匿名 + `MAP_SHARED` 共享页缓存 + `MAP_PRIVATE` 自有帧）；校验全在选址之前；写回只在 munmap/exec/exit 三处，脏页由硬件 `PG_D` 认定。
- **2026-11-11 stat / gettimeofday**：45 `stat` 56 字节路径元数据（不发描述符，size 取实时值）；46 `gettimeofday` CMOS 采样一次 + TSC 演进，单调不回退。
- **2026-11-12 OOM Killer**：用户缺页取不到帧时杀持有最多用户页的进程，一次短缺只杀一个、闩由帧真出手开；能回答的失败照旧只回答。
- **2026-11-12 死代码删除**：`isr.c` 的 `irq==7||irq==15` 里 `intno==0xFF` 分支可证不可达并删除，判据不变。
- **2026-11-12 内核 W^X**：`kernel.ld` 段边界 + `vmm_kernel_wx()` 按段给镜像两个别名赋权（text R-X / `.rodata` R-- / data、bss RW-），镜像外低内存与共享槽保持 `P|W`（AP trampoline 可执行）；ktest 逐页钉点。
- **2026-11-13 PP0 批次**：C4 `pick_next` 队头读纳入 `runq_lock`；C62 运行队列改尾插 FIFO/RR（钉 LIFO 的用例同步改写）；C10 半修——`blkdev` 每设备 `io_lock` + FIFO 队列两段式入队/排空，删 `ide_lock`/`ahci_lock`；smoke 加 `--repeat N` 连跑门，`--repeat 2` 首验 **487/487**。
- **2026-11-13 PP1 C7 半修**：`cache_lock` 改纯元数据锁——fill 先占槽、写回先拷任务自己的弹跳缓冲，victim/purge 跳 `busy` 槽，`blk_io_*` 全移到锁外；tick 回写门改 **trylock**（消除同 CPU 持锁撞 tick 门的自死锁）；`fat_fs_lock` 改 `spin_lock_irqon` **不接管中断状态**（取用点本就 IF=0，为 syscall 开中断预留）；新增锁外 I/O 钉点，**148 cases / 4561 checks**。余项：`fat_fs_lock` 仍跨 I/O，C10 合并/取消被挡。
- **2026-11-14 换链（LLVM 工具链）**：`ld`→`ld.lld`、`objcopy`→`llvm-objcopy`；删 `--no-warn-rwx-segments`（GNU ld 专属）。`kernel.ld` 把孤儿段 `.trampoline` **显式摆成输出段**——lld 对「PROGBITS 与 NOBITS 负载重叠」比 GNU 严格，原脚本会拒链；**产物与 GNU 逐位对齐**（117405/331037、段布局与 trampoline 字节相同），但 payload 字节与 `strings` 集合**不再逐字一致**（lld 做尾串合并并裁死串），判定口径是符号表 + 运行时；三档零警告、ktest 148/4561、`ready` 243.6 s（lld）vs 295.3 s（GNU）。
- **2026-11-14 构建提速（KTEST 对象分目录）**：`OBJDIR := $(BUILD)/obj$(KTEST)`，两模式并存，删旧 `.ktest-mode-<n>` stamp（代价是每次切换全量重编 70 个 TU = 14.4 s 白编）→ **切换 0.2 s、`CC`=0**。**两个坑（详见 BUILD.md）**：模式标记必须挂 `vmlinux.elf` 而非对象（挂错会把上一模式的 `bzImage` 当最新发出）、规则必须排在 `all:` 之后（否则夺走默认目标）。**验证止于编译层**，ktest/smoke 随 PP3 补跑全绿。
- **2026-11-15 注释清理**：只动注释、代码零改动——删复述代码的废话、历史叙事与多处重复（留 `/* see mm.h */` 一处权威），保留结构横幅、行尾契约、测试 ID 与非显然的「为什么」。15 个文件 2796 → 2533，全树 8335 → 8213（占非空行 26.6%）。因 `K_ASSERT` 烤入 `__FILE__`/`__LINE__`，判据用尺寸 + `nm` 而非 md5。
- **2026-11-15 PP2 可观测性最小集**：新增 `klog.h`/`klog.c`/`backtrace.c`/`t_klog.c`（377 行）。**loglevel** 四级、只过滤控制台、**环恒录**，cmdline `loglevel=<0..3>`；**崩溃环** 96 槽 ×160 B，panic 路径锁一律 `spin_trylock`（拿不到就丢，绝不自旋到不了 `hlt`），dump 走原始 `console_putc` 不回灌；**backtrace** 帧指针走链，故 `Makefile` 加 `-fno-omit-frame-pointer`（`-Os` 默认丢 rbp）。9 处诊断行降 DEBUG（不在 smoke 标记内，判据零改动）；ktest 加 `klog` 三例；Makefile 加 `CMDLINE ?=`。**验证止于编译层**：三档零警告，ktest 148→151，三例随 PP3 补跑全绿。
- **2026-11-15 PP3 `pipe(47)`**：新增 `pipe.h`/`fs/pipe.c`/`t_pipe.c`（约 700 行）；`LNXRM_SYSCALL_LAST 46→47`、`LNXRM_EPIPE=-32`（信号表无 SIGPIPE 号，只回 errno 不发信号）。结构与 park 协议见 CODEBASE §6。**坑**：`struct file` 的 `priv` 仍 NULL、私有对象走新字段 `drv`（C42「无 vnode → size 0」与 close 跳过回写的既有标记不能动）；等待队列复用 `task->wait_next`（同一时刻只 park 一处），push/unlink 持锁、**wake 必须锁外**，每方向**唤醒全部**；park 后必须**恢复 `T_RUNNING`** 再重查条件；**先 wake 再 `kfree`**。**ktest 四例**全不阻塞（LAPIC timer 未起，park 即挂 boot）。**验证**：三档零警告，KTEST=0 125693/nm 783、KTEST=1 351837/nm 1297，ktest 151→**155/4638**，`make smoke` **270/270**。
- **2026-11-15 PP4 shell `|`/`<`/`>`**：只改用户态 `lsh.c` 111→245 行、`cat.c` 30→35 行，内核与 ABI 零改动。**三个硬顺序**：① **全部 fork 完才 wait**（否则第一段写满 4096 B 环就 park 在还不存在的读者上）；② 子进程 dup2 后**关掉全部 pipe 端**（含刚 dup 的两个，否则 EOF 被本段自己挡住）；③ 重定向**最后**做（POSIX：重定向属于命令）。另：`|`/`<`/`>` 只认单字符 token，`a|b`、`>>` 仍是普通词，语法错整行不执行；`cat` 无操作数改读 fd 0 才能当管道对端。**验证**：三档零警告（内核零改动）、`make smoke` **270/270**、端到端脚本 20/20（每条都等到 prompt 回来——prompt 不回 = EOF 没来 = 挂死）。
- **2026-11-15 用户态收敛（`rn` 并入 `mv`、`sh` 改名 `lsh`）**：只改 `userland/bin/`，程序数 **16 → 15**。`rn` 的功能本就是 `mv` 的第二支（非目录目标即改名），删 `rn.c`；`sh.c`→`lsh.c` 且 `init.c` 三处 `/bin/sh` 同步改（路径写错就没有终端），提示符 `root@lnxrm$ ` **不动**（smoke 认它）。两份 Makefile 由 `wildcard` 现场发现，零改动；过期产物手删；**smoke 判据零改动**（程序名从不进判据，`smoke_test.py` 头注释里的 `/bin/sh` 是红线不可改的过时描述）。**验证**：三档零警告（内核零改动）、15 个 ELF 零警告；端到端核对交用户执行。
- **2026-11-15 syscall 轮（`reboot`/`chdir`/`getcwd` + lsh `cd`/`pwd` + `ls` 无参修复）**：`LNXRM_SYSCALL_LAST 47→50`、`LNXRM_ERANGE=-34`，`reboot`(48) 挂 `GATE_ADMIN`（sysents 内 7→8），用户程序数 15 → 16。**cwd 设计**见 CODEBASE §6——关键在于 `abs_path()` 只做拼接，**既有路径 syscall 全部自动获得正确基准、解析逻辑零改动**。**用户态**：`kreboot/kchdir/kgetcwd`、新程序 `reboot.c`、`lsh` 内建 `cd`/`pwd`（提示符不动）、`help` 补三项；systest 头注释改「51 个编号」，**48/49/50 刻意不自测**（`reboot` 重启机器、`chdir` 污染 shell 判据），照旧只测 `LAST+1..+3`。**`ls` bug**：无参 `ls` 恒列 `/`（写死 `"/"`），改 `kgetcwd()` + 回退 `"/"`，只改 `ls.c` 约 6 行——**拒绝**「在 `fat_resolve_path` 折叠点段」的内核方案；**用户实测通过**。**残留**：FAT 根无点记录，根上 `ls .`、`cat ./README.md` 仍 `ENOENT`（`t_prop.c` 钉死「不得改写路径」，`abs_path` 不折叠点段）。**验证（止于编译层）**：三档零警告，KTEST=0 125693/nm 789、KTEST=1 351837/nm 1302；16 ELF，镜像内 `/bin/ls` md5 与 `build/user/ls` 一致。**完整 smoke 回归交用户执行**。
- **2026-11-16 S0 `shm_open(51)`/`shm_unlink(52)` 共享内存对象**：新增 `include/sys/shm.h`、`kernel/fs/shm.c`、`ktest/t_shm.c`（合计 833 行），`LNXRM_SYSCALL_LAST 50→52`，`LNXRM_O_CREAT 0100`/`LNXRM_O_EXCL 0200` 与 `sys/vfs.h` 的 `O_CREAT`/`O_EXCL` 由文件内 `_Static_assert` 钉死（两处各维护一份，靠断言不靠默契）。**设计一句话**：名字表的每个表项自己永久持**唯一的一个 `struct file`**，所有 opener 拿到的都是同一指针——于是 `mm/filepage.c` 的共享页缓存按 file 指针作键**天然只有一帧**、`sys_mmap` 的 `!priv`/`V_REG` 门自动开、`vma_fork_shared` 复制的也是同一个 file，**MM 层零改动**（`vma.c`/`filepage.c`/`pagefault.c` 一行未动），伪 vnode 是 `V_REG` + `priv` 而非 `drv`（C42 与 pipe 的标记不冲突）。**生命周期两条线、一扇门**：entry 的那份引用**只有 `shm_unlink` 放**，故 `close_node` 必然发生在 unlink 之后，里面无条件 `kfree(ent)`（仍 named 则先摘表 + `kprintf` 警告）；链条是 `vfs_close_file` CAS → `reg_close` → `close_node` **恰好一次**（为此 `reg_close` 从 `vfs.c` 去 `static` 并在 `vfs.h` 导出），副带收益是**内容跨 close 存活**（POSIX 语义，会合两端不必重叠 open 窗口）。**三个坑**：① `shm_unlink` 必须「锁内摘表置 `linked=false` → **解锁** → 才 `vfs_close_file`」，锁内触发 `close_node` 会自死锁；② `kmalloc` 必须放锁外、拿到后**二次查表**（与 `pipe_new` 同形，输的那支直接 `kfree` 包装、不走 `vfs_close_file`，否则会 close 一个 `shm_live` 没数过的 entry）；③ fd 的 `file_ops` 只给 `.close = reg_close`、`read/write/getdent = NULL` → `sys_read`/`sys_write` 回 `EBADF`（所有 opener 共享一个游标、写也没有落点），fs_ops 的 `write` 却要**返回 n**，好让 `vma_writeback` 清 `PG_D` 计数而不是无限重试。mode 固定 `O_RDWR` 共享（`O_RDONLY/WRONLY/RDWR` 接受并忽略、其余位 `EINVAL`），名字去前导 `/`、拒空与内部 `/`、`<32` 字节（`ENAMETOOLONG`），容量 1 GiB（`ENOMEM`）/ 64 对象（`ENOSPC`），无 gate。**51/52 刻意不进 systest**（不改 173 条 `SYSTEST_TAGS`、smoke 总数不变），只落 ktest 三例（全不阻塞）。**验证（2026-11-16 `make smoke` 实跑，全绿）**：三档零警告，KTEST=0 125821/nm 806、KTEST=1 360253/nm 1331，16 ELF、`/bin/ls` md5 仍 `5e84dec2e1058e04a3887675fdcc0ef3`；ktest 155→**158 cases**、4638→**4754 checks**、0 failed（3 skipped、1 warn），`make smoke` **270/270**（`past.53/54/55` 按 `SYSCALL_LAST` 自动前移到 53–55，判据零改动）。**checks 的 +116 拆开看**：shm 三例 **+106**（late 3787→3893，逐条静态计数命中）、S0 的 51/52 让 `t_abi` 的 `.def` 逐项循环 **+4**、而 early 389→399 里另有 **+6** 是上一轮 syscall（48/49/50）就该记却只做了编译层验证没回写的账。三轮（主跑 + 两轮 forged）late 在 3893/3894 之间有 **±1** 的运行期条件检查波动，非 shm 所致。
