# lnxrm 差距分析：现在缺什么、错什么、先修什么

> 对象：`lnxrm` —— x86-64 freestanding 单体内核（C + NASM），QEMU 为主目标。构建与运行见 BUILD.md，代码结构见 CODEBASE.md。
>
> 基线（**2026-11-19 `make smoke` 实测**，S1b + 两轮修复后）：系统调用 **56 个（0–55 连续无空洞）**，末八条 `reboot` 48、`chdir` 49、`getcwd` 50、`shm_open` 51、`shm_unlink` 52、`dipc_open` 53、`dipc_recv` 54、`dipc_send` 55；ktest **162 cases / 4974 checks**（2 skipped、1 刻意告警）、`make smoke` **270/270**、三档 `make` 零警告——各项来历见第 6 节（2026-11-17 S1a 轮为 162/4951、3 skipped，差账见第 6 节末）。
>
> 2026-11-18/19 新增（**2026-11-19 实跑全绿**）：S1b 键盘**与鼠标**通道 + ring3 `.km` 模块、initramfs 整套与 `ide.c` 删除、smoke 切 q35——`make smoke` **270/270**、ktest **162/4974**（`kbd_decode_init`/`mouse_decode_init` 都排在 ktest 之后，故 case 数一位不变；+23 的账与 `t_ahci` 转实测见第 6 节末），见第 6 节 2026-11-18 各条。**S1c 解码器下沉**同日落地（第 6 节末条；**smoke 复跑 2026-11-19 实测 270/270**、ktest **162/4974** 一位不变）。
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
| 系统调用 / ABI | 3 | 56 个全实现，指针一律 `copy_*_user` → `user_ptr_ok`（写侧先解 COW）；缺 `fstat`、单调钟 |
| 并发正确性 | 2 | 打印锁/任务表锁/fd 引用计数已就位；C4、C62、C10 半修，**C7 半修（2026-11-13）**已把缓存锁移出设备 I/O；余 `fat_fs_lock` 仍跨 I/O（见 C7） |
| 设备驱动 | 3 | PS/2 键鼠中断驱动、AHCI 中断 + DMA、多盘挂载分流（C49/C51）；**`ide.c` 已删、smoke 切 q35（2026-11-18）**，全仓只剩 AHCI 一条块设备栈；PCI 配置机制半修（C56） |
| 文件系统 | 3 | FAT32 + on-disk redo journal + 时间戳 LRU 块缓存 + 簇链步数预算；边界/挂载批次已收口（C28–C47，见第 6 节） |
| 图形 / 显示 | 3 | 帧缓冲控制台 + 双缓冲 `fb_flip` + `CAP_FB`/归属两道门；无图元（暂不作为缺口） |
| 用户态 | 2 | 16 个静态 ELF64 + 自带 crt0/迷你 libc，mmap 三件套就位；**`/mod` ring3 驱动模块已就位（S1b，init 扫目录拉起）**；无动态链接器，跑不了第三方软件 |
| 安全模型 | 2 | `cred` + 3 能力位 + `setuid` 单向降权；NXE/WP/SMEP/SMAP + **W^X 已收口（2026-11-12）**；无 gid、RLIMIT、文件属主、KASLR、栈保护 |
| 测试 | 3 | ktest 162/**4974**、smoke **270/270** 全绿（**2026-11-19 S1b 轮实测**；2026-11-17 S1a 轮 162/4951）、`--repeat 2` 连跑 487/487（2026-11-15，本轮未复跑）；无 CI、无宿主单测 |
| 可观测性 | 3 | 串口日志、分阶段启动、`ps` 内存列、块缓存统计；**loglevel + 崩溃环 + backtrace 已收口（2026-11-15）**；无 `/proc` |
| 可移植 / 启动安全 | 2 | 门控已切 q35 + TCG + `-m 256`（`SMOKE_QEMU_ARGS`，2026-11-18）；无 KASLR/KPTI、入口无 `swapgs`、无 IST |

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
| C68 | `Makefile`、`smoke_test.py` | smoke 只跑 q35 + TCG + `-m 256`（2026-11-18 起机型经 `SMOKE_QEMU_ARGS` 注入，不再落 QEMU 默认 i440fx）：ISO、q35+KVM（`make run` 默认）、内存档位、双盘都不在门控里 | smoke 矩阵化，约 1 天 |
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
| syscall | `fstat`（描述符侧元数据）、`clock_gettime`（单调钟）、`umask`、`fcntl`/`poll`/`select`（多路等待）、`chmod`/`link` | 路径侧 `stat`(45)、`gettimeofday`(46)、`pipe`(47) 与 `reboot`(48)、`chdir`(49)、`getcwd`(50)、`shm_open`(51)/`shm_unlink`(52)、`dipc_open`(53)/`dipc_recv`(54)/`dipc_send`(55) 已收口，以上仍缺 | 各半天–3 天 |
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
6. 驱动收口：PCI 配置机制与注释（C56）；AHCI 中断/多盘已收口（C49/C51），**IDE LBA48 随 `ide.c` 删除一并作废**（C53 不再有对应代码，2026-11-18）。
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

- **2026-11-16 S0 `shm_open(51)`/`shm_unlink(52)` 共享内存对象**：新增 `include/sys/shm.h`、`kernel/fs/shm.c`、`ktest/t_shm.c`（833 行），`LNXRM_SYSCALL_LAST 50→52`，`O_CREAT 0100`/`O_EXCL 0200` 与 `sys/vfs.h` 同名常量靠文件内 `_Static_assert` 钉死。**设计一句话**（细节见 CODEBASE §6）：名字表项永久持**唯一一个 `struct file`**，所有 opener 同指针 ⇒ 共享页缓存天然一帧、mmap 门开、`vma_fork_shared` 同 file，**MM 层零改动**；生命周期两条线一扇门——entry 引用**只由 `shm_unlink` 放** ⇒ `close_node` 恒在 unlink 之后并无条件 `kfree(ent)`，副带**内容跨 close 存活**（POSIX）。**三个坑**：① `shm_unlink` 必须「锁内摘表 → **解锁** → 才 `vfs_close_file`」（锁内触发 `close_node` 自死锁）；② `kmalloc` 放锁外、拿回**二次查表**（输者直接 `kfree`，不走 `vfs_close_file`）；③ `file_ops` 只 `.close`、`read/write/getdent=NULL` ⇒ `EBADF`，而 `fs_ops.write` 要**返回 n** 让 `vma_writeback` 清 `PG_D`。mode 恒 `O_RDWR`、名字 `<32` 字节、1 GiB（`ENOMEM`）/64 对象（`ENOSPC`）、无 gate；**刻意不进 systest**（173 条、smoke 总数不变），只落 `t_shm` 三例（全不阻塞）。**验证（2026-11-16 实跑全绿）**：三档零警告，KTEST=0 125821/nm 806、KTEST=1 360253/nm 1331，16 ELF、`/bin/ls` md5 `5e84dec2e1058e04a3887675fdcc0ef3`，ktest 155→**158 cases**、4638→**4754 checks**、0 failed（3 skipped、1 warn），smoke **270/270**；**+116 拆账** = shm 三例 +106（late 3787→3893）+ `t_abi` `.def` 循环 +4 + 上轮 syscall 欠账 +6，late ±1 为运行期波动。
- **2026-11-17 S1a `dipc_open(53)`/`dipc_recv(54)`/`dipc_send(55)` 驱动 IPC 通道**：新增 `include/sys/dipc.h`、`kernel/ipc/dipc.c`、`ktest/t_dipc.c`（约 1280 行），`LNXRM_SYSCALL_LAST 52→55`，`LNXRM_DIPC_NONBLOCK 0x01`/`LNXRM_DIPC_RING_SIZE 4096` 落 `lnxrm_abi.h`（`dipc.c` 静态断言 `RING_SIZE == PAGE_SIZE`）。**设计**（细节见 CODEBASE §6）：结构抄 shm 但**不进 `shm_open` 命名空间** ⇒ `shm_open("ps2")` 恒 `ENOENT`；环 = 页首 `dipc_hdr{rd,wr}` + 4088 B payload（容量 4087），**索引写在帧里** ⇒ ring3 mmap 无 syscall 读余量、`ring_check` 越界归零；锁内帧拷用户**无需 kstack bounce**，wrap 两段拷、`rd` 只在两段成功后推进（**`EFAULT` 不消耗数据**）。**park 协议照抄 `pipe.c:186-197`**，push **锁内摘链、锁外** wake（先取 `next`）；生命周期两条线一扇门照搬 shm（destroy 只摘表 + 解锁后放引用、**不返回 `-EBUSY`**，park 中的 recv 靠 fd pin 住；`close_node` 恰好一次 `pmm_free(ring_pa)` + `kfree(d)`）。三行全 `GATE_ADMIN`（8→11）、**第 4 参走 `rdx`**、`dipc_push` **按名字查表**、send 不走环（1 KiB 缓冲锁外调 sink）。**刻意不进 systest**（173 条不变），只落 `t_dipc` 四例（**全不阻塞**，park 半段用假任务驱动）。**验证（2026-11-17 实跑全绿）**：ktest 158→**162 cases**、4754→**4951 checks**、0 failed，smoke **270/270**（`past.56/57/58` 按 `SYSCALL_LAST=55` 自动前移，判据零改动）；**+197 拆账** = `t_abi` `.def` 循环 +6（3 编号 × 每项 2 条）+ dipc 四例 +191，mm 未动。**首轮揪出一个测试自身的错**（内核无恙）：「同一帧」断言按新环写死 `rd=0/wr=2`、payload 从 0 起算，实测 `rd=2/wr=4`、字节在 `pay[rd]`——改成「读回 hdr 等于内核最后写入值 + `pay[rd]` 处 memcmp」后全绿。`nm` 数字并入本节 11-18 那条。
- **2026-11-18 死代码删除：initramfs 整套 + `ide.c` + smoke 切 q35**：删 `kernel/fs/initramfs.c`、`scripts/mkinitramfs.py`、`vfs_mount_initramfs()` 回退及 `vfs.h`/`main.c` 声明块、`Makefile` 的 `initramfs` 目标族与 help 行、`arch/kernel.ld` 的 `.initramfs` 段（备份 `/tmp/opencode/s1_backup/`）。**动机**：回退根把「根挂不上」悄悄降级成只读 cpio 根、掩盖真故障，而它从未在门控里被跑到。**`ide.c` 同批删**：`Makefile` 新增 `SMOKE_QEMU_ARGS ?= -machine q35`（在 `SMOKE_FLAGS` **之前**传 `--qemu-args`，用户自带 `--qemu-args` 仍可覆盖），smoke 从 i440fx + PIIX3 IDE 改为 q35 + ICH9 AHCI，`main.c` 删 `ide_init()`。**代价（见 CODEBASE §7）**：`MACHINE=pc` 既无 AHCI 也无 IDE ⇒ **不再能挂根**，只跑不碰盘自测（`t_ahci` skip）；旧的「IDE `0xE7` FLUSH 瞬时 → `blkcache` panic」偶发随之消失；`vfs_root_is_fat()` 保留（仍是分流判据）。**验证（独立 `BUILD=/tmp/opencode/kbuild`）**：两档全量零 warning/error，`bzImage` 125757 / 376701 B、`nm` 797 / 1352（S0 基线 125821·806、360253·1331，差额 = +S1a `dipc.c`+`t_dipc.c` −本批两文件）。**2026-11-19 实跑**：`make smoke` **270/270**，`t_ahci` 确已转实测通过（`ahci/test_a_command_is_retired_by_its_interrupt` PASS，`[ahci] first command completed by interrupt`）。

> ⚠️ 顺带发现一个**与本批无关的既有**限制：`Makefile` 的 `--redefine-sym _binary_build_trampoline_bin_*` 写死了默认路径，而 `llvm-objcopy -I binary` 的符号名由**输入路径**导出——所以 `make BUILD=<别处>` 会在链接期报 `undefined symbol: trampoline_start`。默认 `BUILD=build` 不受影响；要不要把该规则改成按路径现算，留待后续（已记入 BUILD.md 排障表）。
- **2026-11-18 S1b 键盘通道 + ring3 驱动模块**：**解码器单源**——共享头 `include/abi/keyboard.h` 装 `LNXRM_KBD_CHANNEL "kbd"`、扫描码状态机与两张表（表在 `static inline` 函数内的 block-scope `static`，规避 `-Wunused-const-variable`）；内核 `kernel/drivers/keyboard.c` 持自己的 `struct lnxrm_kbd_state`，导出 `kbd_process_sc()`/`kbd_decode_init()`。**IRQ1 桩 ADDITIVE 不 REPLACE**：原解码一字未改（`sys_kbd_get 34`/`sys_kbd_inject 35` 契约原样）**加** `dipc_push(..., &sc, 1)`；push 对未建名返回 0 ⇒ `kbd_decode_init()` 之前的字节安全丢弃，ring3 挂了也不影响真键盘。**`kbd_decode_init()`（只 `dipc_create` + 一行 kprintf）刻意排在 ktest 之后**：`t_dipc.c:247` 断言第九个通道 `-ENOSPC`，预建会让 `live0+8` 越界——**`t_dipc.c` 一行未改、ktest 数字不动**。**模块** `userland/mod/keyboard.c`（普通静态 ELF64，`open → 循环 recv → lnxrm_kbd_decode`）**不 mmap 环**（`LNXRM_DIPC_HDR_SIZE` 带 spinlock 依赖只在 `sys/dipc.h`，mmap 路径已由 `t_dipc` 覆盖），**成功路径打印同日全删**（见末条）。**发现机制**：`init.c` 新增 `spawn_modules()`，README 打印后、fork 终端前扫 `/mod`，`.km` 后缀**大小写不敏感**，一模块一子进程、失败出声、**pid 1 永不因此退出**（最终形态见 CODEBASE §6）。**构建**：两份 Makefile 加 `MOD_*` 变量、`mod:` 目标、`usero/mod/%.o` 独立对象子目录、链接规则，`disk.img` 依赖加 `$(MOD_ELFS)` + `mmd ::mod` + `mcopy`。**验证**：独立 `BUILD` 零警告，16 个 `/bin` + `mod/keyboard`（**7640 B**（2026-11-19 实测，删打印前 8536 B；**S1c 之前的数**），ELF64 `ET_EXEC`、entry `0x7f8000400000` 与 smoke 判据一致）；**2026-11-19 实跑**：`make smoke` **270/270**，`[kbd] raw scancodes available on dipc "kbd"` 在 `spawned pid=1` 之前、**不进判据**，ktest case 数 162 不变（+23 checks 的账见末条）。鼠标半边见下条。
- **2026-11-18 S1b 第二半：鼠标通道（`ps2mouse.c` 文件名不变）**：共享头 `include/abi/mouse.h` 装 `LNXRM_MOUSE_CHANNEL "mouse"`、flags 位、`struct lnxrm_mouse_dec{pkt[4],n,bytes}`、`lnxrm_mouse_decode()`（`static inline`，`1` 整包 / `0` 还差字节 / `-1` 同步位丢弃）。**文件名刻意不改**：只把重组 + 字段解码换成共享头那份，`ms_bytes`（0xF2 探测谈下的帧长）变 `ms_dec.bytes`，**内核这份是权威**；**`ms_feed_byte` 逐字节等价**（resync 计数、`dy = -dy`、`buttons = f & 7`、先解锁再 `ms_push` + `input_push_mouse`）⇒ `t_mouse`/`t_input` 断言一字未改（顺带删的 `ms_wheel_decode()` 与新增的 `n >= 4` 分支只影响内联位置与不可达路径）。IRQ12 桩镜像**线上原始字节**（不是重组包——ring3 要跑同一解码器就必须看见帧长、resync、滚轮字节）；ADDITIVE：先消费完再 `dipc_push`，`mouse_decode_init()` 之前 push 回 0；与 `kbd_decode_init()` 并排**排在 ktest 之后**（同 `LNXRM_DIPC_MAX=8` 阀），**没有鼠标也照样建通道**（明说「有没有指针」的是 `mouse_info(38).inited`）。**模块** `userland/mod/mouse.c`（8200 B）成功路径同样一行不打印（见末条）。**唯一新逻辑是帧长自推**——ring3 拿不到 `ms_dec.bytes`（`mouse_info` 无此字段、`mouse_get 37` 是 `GATE_ADMIN`）：按 3 字节读时，4 字节流在「本该是 flags 字节」的位置撞上滚轮字节（0..7 过不了 `0x08` 同步位，真 flags 永远带这位），而正确的 3 字节流在包边界**零证据**，故要**两份证据**才切 4 字节（一次损坏的 flags 不得永久锁错帧长——其引发的 resync 都在边界旗标被消费之后，不计入）。**三档零警告（同树复测）**：`KTEST=0` **125789/nm 796**（2026-11-19 实测，与 11-18 同——ABI 扩 24 不动内核镜像；**S1c 之前的数**）、`KTEST=1` **376733/nm 1351**（2026-11-19 实测，与 11-18 同；nm 少 1 = 删掉的 `ms_wheel_decode`；**S1c 之前的数**），`mod/keyboard` **7640 B** + `mod/mouse` **7880 B**（2026-11-19 实测，删打印前 8536/8200；**S1c 之前的数**，S1c 后见末条）。**2026-11-19 实跑**：`make smoke` **270/270**——`[mouse] raw bytes available on dipc "mouse"` 在 `spawned pid=1` 之前、**不进判据**，`init: module`/`attached`/`ring live` **一行未出现**（静默生效），ktest case 数 162 不变。
- **2026-11-18 修：`ps` 看不到 `/mod` 的驱动进程（`.km` 后缀丢失）**。现象：`ps` 表头与行都正常、**唯独没有两个模块**，串口也没有模块行——**镜像、串口、`ps` 三处各自看起来都正常**。根因在构建层：`mcopy` 把 `build/mod/keyboard` 原样拷成 `::mod/keyboard`（后缀原指望安装步骤补，而它没补），`spawn_modules` 的筛子全部 `continue`；且 `disk.img` 没把 `Makefile` 当前置，改了规则也不重建镜像（另一半成因）。**修法**：① `Makefile` 进 `disk.img` 前置；② 每条失败路径出声（统计行随后改为仅 `0 spawned` 时才打，见末条）；③ 筛子改**大小写不敏感**（FAT 短名大小写由写入方 NT 位决定、解析侧 `fat_name_eq_short` 本就不分）。**随后按用户要求把后缀挪到链接期**：`MOD_ELFS` `addsuffix .km` + 静态模式规则改 `$(BUILD)/mod/%.km:`（两处 Makefile），安装回到 `basename` **原样搬运**——**再补一次就是 `keyboard.km.km`**。**验证（独立 `BUILD`）**：重建镜像零警告，`mdir ::mod` 两个 `km`，按 `fat_short_to_name` 解目录项复现判定 → 均 `d_type=8 → SPAWN`；**实跑确认**（用户 2026-11-18 boot）：`ps` 出现 pid 2/3 两个模块。
- **2026-11-18 第二次修：`ps` 的 `NAME` 截断 + 启动期刷屏**。**① 根因是两层 16**：`TASK_NAME_LEN 16`（`sys/sched.h`）与 `lnxrm_ps_entry.name[16]`——`/mod/keyboard.km` 正好 16 字符、加 NUL 要 17，`exec` 存 `t->name` 时先截、`sys_ps` 出口再截。**修**：两处一并扩到 **24**（`ps_entry` 48B → **56B**，`_Static_assert` 同步），下游 `strncpy(..., sizeof(e.name)-1)` 与 `ps.c`/`systest.c` 缓冲全用 `sizeof` 跟随。**判据核查**：无任何 ktest/smoke 断言 48 或 `sizeof(struct task)`（`t_syscall.c:77` 用 `2 * sizeof(...)` 分配），`SYSTEST_TAGS` 的 ps 判据全是数值（`ps.count`/`ps.kind.*`/`ps.uid.*`/`root.ps`/`user.ps`）、无一条匹配 `NAME` 列 ⇒ 不碰判据，但**用户态必须 `make clean` 全量重编**。**② 删打印只删成功路径**：`init: module ...`、统计行、两模块 `attached`/`ring live` 全删（连带只为打印而生的计数器防 `-Wunused-variable`；mouse 的 `at_boundary`/`evidence` 是帧长自推的**功能**代码，保留）；**失败路径一条没删**——`no /mod`、`listing failed`、名字过长、`fork/exec failed`、`no dipc channel`，以及**「扫到却一个都没起来」才打**的 `init: N file(s) in /mod, 0 spawned -- no module was started`（上一条 bug 的读数器；健康 boot 静默）。**唯一保留**：`A tiny unix-like kernel in ASM + C!`——boot 段第 20 条 marker（`scripts/smoke_test.py:118`，红线不可改），删了 `make smoke` 必红。**验证（独立 `BUILD`）**：userland+mod+disk **0 issues**、内核两档编译 **0 warnings**（链接仍只止于 `trampoline` 的 `BUILD=` 既有限制，见 BUILD 排障表）。**2026-11-19 实跑**：`make smoke` **270/270**，启动期静默生效——serial.log 里 `init: module`、统计行、`attached`、`ring live` **一行未出现**，`[boot] ready` 之后只剩 README 行（init 打印 + shell `cat` 各一次，均在判据内）与提示符；ktest **162 cases / 4974 checks / 0 failed / 2 skipped / 1 warn**（early 426、mm 462、late 4086）。**+23 差账**（对 2026-11-17 的 4951）：early **+21** = q35 下 E820 `map_len` 7→9（`test_boot_e820_map` 逐条 3 检查 ×2 + 两两重叠对数 3×2+(36−21)=21，逐项对上）；late **+2** = `t_ahci` 由 skip 转实测（`K_EXPECT(r > 0)` +1）+ 运行期条件波动（+1）；skip **3→2**（剩 `fb` 两条条件 skip），mm 462 未动、1 warn 照旧。**ps 截断实测消失**（用户 2026-11-19 boot）：`NAME` 列完整显示 `/mod/keyboard.km` 与 `/mod/mouse.km`（pid 2/3，不再是 `/mod/keyboard.k`）。
- **2026-11-19 S1c 解码器下沉到 ring3（键鼠）**：IRQ1/IRQ12 从「内核解码 + 镜像」改为**二选一**——新增 `dipc_push_if_listening(name,…)`（与 `dipc_push` 共用 `dipc_push_common`，**`dipc_push` 语义一字未改**，`t_dipc` 四例不动）：通道有 opener（file refcnt > 1）⇒ 字节进环、返回 true，**内核不解码**；无 opener（镜像里没 `.km`、模块退出、通道未建）⇒ 返回 false 且**什么都不存**，内核按 S1b 之前的方式本地解码 ⇒ **没放 .km 键鼠照常**，模块死掉下一个字节就自动交还。回传走 S1a 预留的反向门：`kbd_decode_init`/`mouse_decode_init` 改为 `dipc_create(name, sink)`——`kbd_sink` 收 `[char, mods]` 对（与 `kbd_process_sc` 尾部逐位一致：先 `input_push` 进 inbuf 再 `input_push_key`，mods 在解码**后**读），`mouse_sink` 收整 `struct lnxrm_mouse_packet`（走新抽的 `ms_feed_packet` = `ms_push` + `input_push_mouse`，37/38/39–41 与计数器两侧不可分）；sink 在 `dipc_lock` **之外**被调（`sys_dipc_send` 原注释承诺的锁序），模块 `kdipc_send` 失败即退出、fd 一关内核接管。**判据零改动**：34/35/37/38/39–41 契约原样（35 本就直推 ASCII、不经解码）、ktest 不碰通道名且仍先于通道创建（`-ENOSPC` 第九槽不变）、`SYSTEST_TAGS` 173 与 `smoke_test.py` 未动。**验证（独立树全量三档零警告）**：KTEST=0 **125757/nm 800**（S1c 前主树 125789/796）、KTEST=1 **376701/nm 1356**（前 376733/1351）、`mod/ps2-keyboard` **8312 B** + `mod/ps2-mouse` **8072 B**（前 7640/7880，两树一致；模块当日见末条改名）。**2026-11-19 主树 `make smoke -j8` 实测（S1c 后复跑）**：**270/270**、ktest **162 cases/4974 checks [2 skipped]**、selftest **4974/4974 [1 warn]**——判据一字未改全绿，串口 `build/smoke/serial.log`；bzImage **376733**/nm **1356**，nm 与独立树同为 1356 即证同码，32 B 镜像差 = 内核嵌入的构建路径长度（主树 `/…/easycaikuai/Lnxrm` vs 独立树 `/tmp/…`，`strings` 可见 3 处）。**真实按键实测（2026-11-19 `make run`）**：QEMU 窗口打字回显正常——smoke 全程无真实 PS/2 按键（shell 由串口驱动），IRQ1→环→`keyboard.km` 解码→sink→inbuf→lsh 回显这条 **ring3 接管链由此人工验证工作**，非 .km 场景由「无 opener 本地兜底」保证（设计上模块退出同理自动交还）；主树 KTEST=0 尺寸未跑（独立树 125757，嵌路径差异同理）。
- **2026-11-19 改名：键鼠文件全线加 `ps2-` 前缀（模块 + 内核驱动 + 共享头）**：`userland/mod/keyboard.c`→`ps2-keyboard.c`、`mouse.c`→`ps2-mouse.c`，产物与镜像随之 `ps2-keyboard.km`（**8312 B**）/`ps2-mouse.km`（**8072 B**）——字节数与改名前逐字节同大（文件名只进失败打印那一行串，段对齐吸收了差额）。**构建与判据零改动**：两份 Makefile 的模块清单都是 `wildcard`/`find` 自动发现（改名零改动），`smoke_test.py` 的程序名从 `userland/bin/*.c` 现场发现且 artifacts 阶段不查 `::mod`（红线未动）。`ps2-keyboard` 基名 12 字符超 8.3 ⇒ 镜像走 **LFN 长名**（`mdir` 显示短别名 `PS2-KE~1.KM`、长名在其后），内核 `getdent`/路径解析只在 LFN 校验和对上时回全名（ktest `test_lfn_full_run_survives` 覆盖）；`ps` 的 `name[24]` 装 `/mod/ps2-keyboard.km`（20 字符 + NUL 21）仍富余，`sched.h` 注释已记新值。共享头/内核注释 8 处 `userland/mod/*.c` 路径与 `init.c`/`Makefile` 举例同步。**独立树 KTEST=1 + `disk.img` 全量零警告**：bzImage **376701**/nm **1356**（与 S1c 同——注释不进二进制）、新名 `.km` 装镜像确认；**2026-11-19 smoke 实测 270/270**（bzImage 376733 与 S1c 后主树同——改名不进二进制；ktest/`SYSTEST_TAGS` 一字未动，判据体系全 PASS）；**`ps` 实测完整显示 `/mod/ps2-keyboard.km` 与 `/mod/ps2-mouse.km`**（pid 2/3，init 拉起——超 8.3 长名走 LFN 的通路实证）。 内核侧同日：`kernel/drivers/keyboard.c`→`ps2-keyboard.c`、`ps2mouse.c`→`ps2-mouse.c`（S1b「文件名刻意不改」就此翻篇，第 142 条历史原文保留），`include/abi/keyboard.h`→`ps2-keyboard.h`、`abi/mouse.h`→`ps2-mouse.h`——Makefile 内核源也是 `find kernel -name '*.c'` 自动发现，零规则改动；`#include` 与注释路径 10 文件同步（isr.c 那行 `LNXRM_KBD_CHANNEL` 注释顺手从 S1b 的 mirror 措辞改成 S1c 的门）。
