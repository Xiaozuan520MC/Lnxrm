# lnxrm 差距分析：现在缺什么、错什么、先修什么

> 对象：`lnxrm` —— x86-64 freestanding 单体内核（C + NASM），QEMU 为主目标。构建与运行见 BUILD.md，代码结构见 CODEBASE.md（§6–§10 在 CODEBASE_SUBSYSTEMS.md）。
>
> 基线（**2026-11-19 `make smoke` 实测**，S1b + 两轮修复后）：系统调用 **56 个（0–55 连续无空洞）**，末八条 `reboot` 48、`chdir` 49、`getcwd` 50、`shm_open` 51、`shm_unlink` 52、`dipc_open` 53、`dipc_recv` 54、`dipc_send` 55；ktest **162 cases / 4974 checks**（2 skipped、1 刻意告警）、`make smoke` **270/270**、三档 `make` 零警告——各项来历见 GAP_ANALYSIS_DONE.md（2026-11-17 S1a 轮为 162/4951、3 skipped，差账见 GAP_ANALYSIS_DONE.md末）。
>
> 2026-11-18/19 新增（**2026-11-19 实跑全绿**）：S1b 键盘**与鼠标**通道 + ring3 `.km` 模块、initramfs 整套与 `ide.c` 删除、smoke 切 q35——`make smoke` **270/270**、ktest **162/4974**（`kbd_decode_init`/`mouse_decode_init` 都排在 ktest 之后，故 case 数一位不变；+23 的账与 `t_ahci` 转实测见 GAP_ANALYSIS_DONE.md末），见 GAP_ANALYSIS_DONE.md 2026-11-18 各条。**S1c 解码器下沉**同日落地（GAP_ANALYSIS_DONE.md 末条；**smoke 复跑 2026-11-19 实测 270/270**、ktest **162/4974** 一位不变）。
>
> **S2 ext2 换根（2026-11-22 交付，2026-11-24 收口：smoke 270/270、ktest 167/5290 [2 skipped] 实测全绿）**：`disk.img` 换 ext2 并接管 `/`，新驱动 `kernel/fs/ext2/`（block/inode 子目录 + `include/ext2.h`）、ktest 新增 `t_ext2` 五例、smoke 判据换 e2fsprogs + forged ext2（**总数仍 270**，构成见 GAP_ANALYSIS_DONE.md末条）。三档零警告：KTEST=0 **146333/nm 846**、KTEST=1 **422013/nm 1425**、`mod` 8312/8072 B；phase 1+3 对独立树实跑 51/51。首跑（同日）late 档 4 case/22 check 红——1 个驱动真 bug（`ext2_rename` 截断返回值当错误码）、1 处测试期望错（EOF 钳位）、2 处 `t_ioerr` 环境锚点，四项已修、修后独立树三档复测零警告（详见 GAP_ANALYSIS_DONE.md末条）。**ktest 预期 167 cases / 5290 checks [2 skipped]**（首跑实测 5281 + 写例补跑 13 − lookup VFS 半段门住 4；+5 case / +317 checks 来自 `t_ext2`）——**2026-11-24 `make smoke -j8` 实测精确命中 167 cases / 5290 checks [2 skipped]**。
>
> 编号 C1–C72 是稳定 ID（README 与代码注释有交叉引用），不重排；主体只收**仍然开放**的条目，已关闭的全部在 `GAP_ANALYSIS_DONE.md`（原第 6 节）。

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
| 并发正确性 | 2 | 打印锁/任务表锁/fd 引用计数已就位；C4、C62、C10 半修，**C7 半修（2026-11-13）**已把缓存锁移出设备 I/O；余 `fat_fs_lock`/`ext2_fs_lock` 仍跨 I/O（见 C7） |
| 设备驱动 | 3 | PS/2 键鼠中断驱动、AHCI 中断 + DMA、多盘挂载分流（C49/C51）；**`ide.c` 已删、smoke 切 q35（2026-11-18）**，全仓只剩 AHCI 一条块设备栈；PCI 配置机制半修（C56） |
| 文件系统 | 3 | **ext2 挂根（S2，完整读/写 + 目录/改名，`e2fsck -n` 为判据）** + FAT32（on-disk redo journal）+ 时间戳 LRU 块缓存 + 簇链步数预算；FAT 边界/挂载批次已收口（C28–C47，见 GAP_ANALYSIS_DONE.md）；ext2 无 journal（靠 `ext2_finish` 失败路径也 flush + 每轮 e2fsck 兜底）、不落 uid/gid |
| 图形 / 显示 | 3 | 帧缓冲控制台 + 双缓冲 `fb_flip` + `CAP_FB`/归属两道门；无图元（暂不作为缺口） |
| 用户态 | 2 | 16 个静态 ELF64 + 自带 crt0/迷你 libc，mmap 三件套就位；**`/mod` ring3 驱动模块已就位（S1b，init 扫目录拉起）**；无动态链接器，跑不了第三方软件 |
| 安全模型 | 2 | `cred` + 3 能力位 + `setuid` 单向降权；NXE/WP/SMEP/SMAP + **W^X 已收口（2026-11-12）**；无 gid、RLIMIT、文件属主、KASLR、栈保护 |
| 测试 | 3 | ktest **167/5290 [2 skipped]**（S2 预期：+`t_ext2` 五例 + `t_ioerr` 环境适配；2026-11-24 `make smoke` 实测精确命中，上一版 2026-11-19 为 162/4974）、smoke **270/270**（S2 换根后构成变、总数不变；最近实测 **2026-11-24**）、`--repeat 2` 连跑 487/487（2026-11-15，本轮未复跑）；无 CI、无宿主单测 |
| 可观测性 | 3 | 串口日志、分阶段启动、`ps` 内存列、块缓存统计；**loglevel + 崩溃环 + backtrace 已收口（2026-11-15）**；无 `/proc` |
| 可移植 / 启动安全 | 2 | 门控已切 q35 + TCG + `-m 256`（`SMOKE_QEMU_ARGS`，2026-11-18）；无 KASLR/KPTI、入口无 `swapgs`、无 IST |

---

## 2. 开放缺陷

| 编号 | 位置 | 后果 | 修法与量级 |
|---|---|---|---|
| C7 | `blk_cache.c` + `FAT_ENTER()`/`EXT2_ENTER()`（**半修**） | 缓存层已收口（2026-11-13）：`cache_lock` 只做元数据（槽位预留 + 弹跳写回），`blk_io_*` 全在锁外；tick 回写门改 trylock；`fat_fs_lock` 改不接管中断状态。**余项**：`fat_fs_lock` 与 S2 新增的 `ext2_fs_lock`（挂根，实际热点）都仍跨整个 FS 操作含 I/O（拆它要给每个读-改-写序列加重校验/重试）；tick 门胜出时回写 I/O 仍在 IRQ 里做（`jiffies` 传输段冻结，C27 既有） | FS 层按「纯读放锁 / 变更持锁」拆两套入口，约 3–5 天 |
| C10 | `include/sys/vfs.h` `blkdev`（**半修**） | 每设备 `io_lock` + FIFO 请求队列已落地、驱动全局锁已删（2026-11-13）；合并与取消仍缺——调用者全程压着 FS 锁（`fat_fs_lock`/`ext2_fs_lock`，C7 半修未拆它），待 C7 余项 | 合并/取消（依赖 C7 余项），约 200 行 |
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
| 挂载可信磁盘 | 基本安全 | ext2 侧：无 journal，靠 `ext2_finish()` 失败路径也 flush + **`e2fsck -n` 每轮兜底**（S2 硬判据）；FAT 侧：redo journal + mount replay + 故障注入；三道同步门（C27）；静默损坏三件与 vnode 泄漏已修（C19/C20/C21/C40）；写路径 `0xE7` FLUSH 检查（C55） |
| 挂载不可信/可能损坏的磁盘 | 不安全 | FAT：簇链步数预算 + 单簇自环即断、BPB 校验、目录游标/LFN/容量/MBR 边界均收口；ext2（S2）：非法 `rec_len` 每处扫描出声即停、越界块拒读拒写、组描述符越界拒挂载；但两者都**无 checksum**、有界长走持 irqsave/irqon 锁、目录内容逐块信任读回 |
| 硬件缓解 | 已开但不完整 | NXE/WP/SMEP/SMAP 置位有 ktest 钉点，**内核 W^X 已收口（2026-11-12）**；缺 KASLR、KPTI、canary、IST |
| 横向定位 | 与 `xv6` 同档 | 同为简单权限模型、同无 KASLR；`sigreturn`、ELF 解析、`copy_user` 更严谨 |

---

## 5. 优先级清单

一行一项，按依赖顺序；括号引用第 2、3 节。

1. 锁与抢占：C7 半修已落地（缓存层锁外 I/O + tick 门 trylock + FS 锁不接管中断，2026-11-13）→ C7 余项（FS 锁 `fat_fs_lock`/`ext2_fs_lock` 拆出 I/O）→ C10 余项（合并/取消，依赖 C7 余项）→ 内核态抢占与 syscall 关中断（第 3 节「抢占」），每步以 smoke 兜底。
2. 验证面：smoke 矩阵化（C68）→ CI（第 3 节「CI」）；连跑抖动门 `--repeat N` 已就位（OBS-1 收口 2026-11-13）。
3. 可观测性最小集：**已收口（2026-11-15）**——loglevel + 崩溃环形缓冲 + backtrace 见 GAP_ANALYSIS_DONE.md；余项 debug syscall、统计计数器（第 3 节「可观测」）。
4. 内核自身加固：canary、`swapgs`/IST、KASLR（第 3 节「加固」）；内核 W^X 已收口（2026-11-12）。
5. 文件权限与限额：inode 属主 + 写检查点 + CPU 时间限制（第 3 节「权限」）。
6. 驱动收口：PCI 配置机制与注释（C56）；AHCI 中断/多盘已收口（C49/C51），**IDE LBA48 随 `ide.c` 删除一并作废**（C53 不再有对应代码，2026-11-18）。
7. 能力面：**`pipe` 与 shell 管道均已收口（2026-11-15，见 GAP_ANALYSIS_DONE.md）**，下一项动态链接器与共享库（「内存」，文件映射已是前置）；IPC 余项（unix socket、线程、`alarm`）见第 3 节。
8. 暂不做：新子系统、POSIX 全覆盖、网络栈——1–7 未落地前不加宽度。（「更换文件系统」一项已由 **S2 ext2 换根**完成，见 GAP_ANALYSIS_DONE.md末条。）

---

已收口（已关闭）的条目全部在 **`GAP_ANALYSIS_DONE.md`**（2026-11-24 从原第 6 节拆出，小节号沿用 §6）。
