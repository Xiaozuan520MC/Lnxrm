# lnxrm 差距分析 · 评审记录 · 行动计划

> 评估对象：`lnxrm` 内核，10,151 行内核 + 761 行用户态（11,068 行源码，80 个源文件 + 2 个构建脚本）
> 评估方式：全量阅读源码 + 从零构建 + QEMU 实机启动验证
> 文档性质：工程差距分析，不是路线图许愿，也不是缺陷清单堆砌
> 本文三部分：**第一部分 差距分析**（66 条编号缺陷，§0–§11） · **第二部分 评审记录**（2026-10-01 实测与安全审查，§12–§18） ·
> **第三部分 行动计划**（T-000… 任务卡，按依赖排序，§19–§37）；章节号在全文件范围内唯一

---

## 第一部分 · 差距分析


### 0. 结论摘要

**lnxrm 是一个内核。** 不是"像内核"，是内核：ring-0 特权代码、直接管理硬件（PCI 枚举 / IDE PIO / AHCI / PS/2 / 16550 / VBE）、每进程独立地址空间、进程生命周期、SMP 抢占调度、系统调用边界、ELF 装载、持久化存储、POSIX 信号、init + 用户态 shell。10,151 行内核达到这个功能密度，在同类项目中处于很高的分位，而且**它真的能跑**——从 VBE 选型一路到 `[smp] 2 CPUs online` 再到用户态 `/bin/init` 输出，没有任何一步是纸面上的。

**但它不是"合格"的内核。** 三句话概括差距：

1. **它假设所有代码都可信，而缺的不是补丁，是整个安全模型。** 全树没有 `uid`/`gid`/`capability`/`RLIMIT` 的任何概念；`EFER.NXE`、`CR0.WP`、`CR4.WP`、`SMEP`、`SMAP` **一个都没开**。所有进程都是 root，所有内存页都可执行，只读页实际可写。没有"不可信代码"这个概念，意味着所有内存安全缺陷的杀伤半径都是整机。

2. **它没有验证能力，因此没有工程纪律。** 零测试、零 CI、零可观测性、零内部断言。唯一的验证回路是"启动，看串口日志"。这直接导致第三点。

3. **它存在系统性的契约漂移。** 意图写进注释和 ABI，实现漂移了，没有任何东西会发现——因为没有测试会失败。`fat32_journal` 承诺原子性但提供的是"多写一遍"；调度器注释声称 circular queue 而实现是 LIFO；`mm.h` 的槽号注释与实际相差 31 个槽。

**量化差距：综合成熟度 2.1 / 5.0，目标 3.9 / 5.0。** 详见第 8 节。

**如果只能做一件事**：给 `console_write` 加 `print_lock`（一行，解掉 SMP 下最显眼的竞争）。**如果只能做两件事**：给 FAT 簇链遍历加迭代上限（十几行，解掉"一个坏扇区挂死整机"）。**如果只能做一件战略性的事**：砍掉一半子系统，把剩下的做到能写测试、能被测。

---

### 1. 评估基准：什么叫"真正的内核"

"真正的内核"是个模糊词，不先立标准，"还差什么"就没有答案。本文档采用 5 级成熟度标尺：

| 级别 | 名称 | 判据 |
|---|---|---|
| **L0** | 玩具 | 能启动、能打印字符。仅此而已。 |
| **L1** | 作品 | 有进程、有系统调用、有内存管理、有存储。功能面 50%+。 |
| **L2** | 工程可用 | 边界清晰（权限、资源、地址空间隔离真实生效）；失败路径正确；有基本测试。 |
| **L3** | 稳健 | 并发正确性经审计；持久化有崩溃一致性；硬件错误可诊断；契约与实现一致。 |
| **L4** | 可投产 | 可测试、可观测、有 CI、有回归防护；经过长时间与真实负载验证。 |

**lnxrm 当前落在 L1（偏 L1 上沿），在并发正确性、安全模型、测试、可观测性四个维度上是 L0。**

需要说明的是：**L4 不是"更好的 hobby kernel"的目标。** 一个认真做完 L3 的单子系统内核，比一个 8 个子系统都 60% 的内核有价值得多。本文档后面的路线图会反复回到这一点。

---

### 2. 现状盘点：已经有什么

先说清楚基线，否则无法度量距离。以下都是**核实过的**（读代码 + 实机启动）：

#### 2.1 引导与启动 —— 强项

- **双启动路径**，且选择机制干净：
  - QEMU `-kernel`：`arch/setup.asm` 自己实现真模式 VBE 选型 + `int15/E820` 采集
  - GRUB `linux`：`arch/entry64.S:_start32` 接管，从 `boot_params.screen_info` 取 LFB
  - 物理地址 `0x6F00`（**DWORD，只写低 4 字节**）存 GRUB 的 `boot_params` 指针作哨兵，`main.c:57` 据此选数据源
- **VBE 选型是 4 遍级联**（1920×1080 → 1280×720 → 最大可用），每遍都要求 `4F02` 真的成功才信任该模式（`setup.asm:189-235`）
- **LMA/VMA 单一连续段**：`arch/kernel.ld` 把整个镜像压成一个 `.image` 段，VMA=`0xffffffff80100000` / LMA=`0x100000`，严格差值，于是 `objcopy -O binary` 一步出 bzImage。这个设计省掉了一整类链接脚本问题。

#### 2.2 地址空间模型 —— 全项目最好的设计

三槽 PML4（`kernel/mm/vmm.c:1-11`）：

| PML4 槽 | 用途 | 共享性 |
|---|---|---|
| 0 | 低内存恒等映射 | 全部 AS 共享，仅内核 |
| 255 | 每进程用户空间 `[0x7f8000000000, 0x7ffffffff000]` | 每进程独立 |
| 511 | 高半别名（内核镜像 / 堆 / 设备窗口 / LFB） | 全部 AS 共享 |

`vmm_new_user_aspace()` 只复制除 255 外的所有 PML4 条目，于是**内核映射对新进程零成本传播，无需 refcount**。`fork` 则深拷贝 255 槽。这个模型比"共享内核半 + 每进程下半"的经典方案简单得多，而且隔离边界落在 PML4 条目的 `PG_U` 上——是真实的（虽然只有 1 bit 深，见 4.4）。

#### 2.3 信号 —— 只剩投递，没有用户态处理器

32 个 POSIX 信号的**编号**保留，但用户态那一半整条链路已经不存在了：

- 只有 `sys_kill` 一个投递入口，`send_signal`（`kernel/signal.c:6-44`）置 `signal_pending` 位
- `do_signal_check`（`:50-100`）只在返回用户态前查这一位，执行**默认处置**：
  `SIGSTOP` → `TASK_STOPPED` + 通知父进程；`SIGCONT` → 恢复；`SIGKILL` → 立即终止；
  `SIGINT/SIGTERM/SIGQUIT/SIGILL/SIGABRT/SIGSEGV` → 终止进程组；**其余一律静默丢弃**
- ~~没有 `sigaction` / `sigprocmask` / `sigreturn`，用户态拿不到信号~~ **已补齐**：
  16/17/20 三个编号现在是真实现（`kernel/signal.c`），handler 帧由 `deliver_handler`
  在用户栈上就地构造（压入一段 9 字节的 `mov eax,20; int 0x80; ud2` 作为返回桩），
  `sigreturn` 把 `saved_tf` 写回当前帧。CS/SS 不经用户之手，整帧由内核复制，
  没有可伪造的恢复入口；handler 地址入库前经 `user_ptr_ok` 校验。

~~`kill` 的语义很窄：不能通知进程任何事~~ `kill(pid, 0)` 现在是真正的存在性探测
（不存在返回 `ESRCH`），不再是"返回 0 但什么都没发生"的假成功。

另一个单槽隐患保留了下来：`con_waiter` 是**单槽全局**（C5）。

#### 2.4 图形栈 —— 一个合格的字符控制台，不是图形栈

- LFB 以 `PG_PCD`（不可缓存）映射到专用高半窗口 `FB_VMA = PD_HI[104]`，8 槽 = 16 MiB（`include/framebuffer.h`）
- `fb_pack` 按 `r_pos/r_size/...` 位域打包 8-bit 通道，8/15/16/24/32 bpp 都渲染同一套 `0x00RRGGBB` 颜色
- 内置 CP437 8×16 点阵字体；ANSI 转义序列双后端（VGA 文本 + 帧缓冲）同时解释
- 没有双缓冲、没有直线/圆一类的图元；**有 5 个图形系统调用**：编号 23–27 的
  `fb_info`/`fb_clear`/`fb_fill`/`fb_char`/`fb_puts` + `kfb_*` 包装，
  4 个内核原语（`fb_clear`/`fb_fill_rect`/`fb_draw_char`/`fb_puts`）全部裁剪到可见区域，
  但 `fb_box_rgb`/`fb_line_rgb`/`fb_circle_rgb`/`fb_scroll` 这类高级图元仍然没有
- **绘图入口两道门，顺序是硬的**（T-031 + T-033，2026-10-05 齐活）：先
  `cap_gate(CAP_FB, ...)`（**够不够格**——普通进程止步于此），再
  `fb_owner_gate()`（**够格的里哪一个在屏上**——第一个成功的绘图调用占住
  `fb_owner`，后来者回 `-13`，持有者退出即释放）。`fb_info` 两道门都没有：
  只读几何，且不学尺寸就没资格选画哪儿。原语层**一道门都不查**——控制台和
  用户态共用那 4 个函数，一个能被用户进程拒绝的控制台就是一个能被用户进程弄哑的控制台

**定位**：702 行、31 个已定义系统调用中的 5 个与图形相关，
消费方是控制台（`fbc_*`）和用户态直接绘图。**C3（归属强制）已于 2026-10-05
完成（T-033）**——单 pid 归属 + CAS 占用 + 拒绝不改归属 + 释放只在 `sys_exit`；
C2/C6（双缓冲本身、后缓冲跨核 cache 维护）**随后缓冲一起悬空**（双缓冲从不存在，
编号继续保留）——**有裁剪保护的绘图入口和一个单一 owner，但没有经过验证的
图形合成路径**。
VGA 文本后端仍作为 `fb_init` 失败时的回退保留。

#### 2.5 多语言工程

C 主体 / C++ 做 SMP+AHCI / Rust 做 16550 驱动、`Spinlock<T>`、`KBox<T>`、trampoline 共享数据（`kernel/rust/*.rs`）。Rust 侧以 `staticlib` 形式被链接脚本整体拉入，`panic_handler` 手写 `format_into` 后停机。构建零警告。

#### 2.6 实测启动日志

```
[fb] mapped at 0xffffffff8d000000 (phys=0xfd000000), 1920x1080 32 bpp, pitch=7680
[fat32] mounted hda: 64 MiB, 512 B/sector x 1/cluster, root=2
[vfs] FAT32 mounted at / (whole disk)
[task] spawned pid=1 (/bin/init) entry=7f8000400640
[smp] 2 CPUs online
[lapic] timer vec=32 hz=100 period=623880
```

#### 2.7 代码注释的质量

注释普遍解释**为什么**，并经常引用促使该修改的历史 bug。这是工程成熟度的标志，例如 `kernel/task.c:166-173`：

> `/* ELF load failed: roll back to the caller's original address space. Previously this sys_exit(-8)'d after already switching CR3, leaving current->pml4 pointing at a destroyed (or never assigned) pml4 and leaking new_pml4. POSIX says exec failure returns to the caller. */`

**这是真正做过调试、真正修过 bug 的人留下的痕迹**，不是从模板抄来的。问题不在注释质量，在于**注释和代码会漂移，且没有机制阻止**（第 7 节）。

---

### 3. 缺口分类学

后面四节按这个分类展开。把它们分开很重要，因为**成本和优先级差异极大**：

| 类别 | 含义 | 修复成本 | 举例 |
|---|---|---|---|
| **A. 缺失的机制** | 整块子系统不存在，需要新建 | 高（数百到数千行） | 特权模型、`mmap`、测试框架 |
| **B. 缺失的工程纪律** | 机制存在但无验证、无可观测、无断言 | 中（数百行，但需要持续投入） | 测试、可观测性、内部断言、CI |
| **C. 存在但不可靠** | 机制存在，实现有正确性缺陷 | 低到中（单点修复） | FAT 环检测、`console_write` 绕锁 |
| **D. 契约漂移** | 文档/ABI 承诺与实现不符 | 极低（但影响信任） | `fat32_journal` 头注释、`circular` 队列注释、槽号注释 |

**关键洞察：A 类和 C 类是"能不能用"的问题，D 类是"能不能维护"的问题，B 类是"能不能演进"的问题。** B 类缺失是 A/C/D 三类问题的**共同根因**——因为没有测试会失败，所以缺陷不会暴露、文档不会被迫更新、修改不会被发现是否破坏了什么。这是本文档最重要的一个论点。

---

### 4. A 类：缺失的机制

#### 4.1 特权模型 —— 最本质的缺口

**核实结论**：全树 grep `uid` / `gid` / `euid` / `capability` / `privilege` / `RLIMIT` / `setuid` / `credential` —— **零命中**。

具体后果（现在就成立，不是"以后可以加"）：

| 现状 | 位置 | 后果 |
|---|---|---|
| ~~`sys_kill(-1, sig)` 无权限检查~~ **已修复（2026-10-03）** | `kernel/syscall.c` `sys_kill` | 广播现在要 `CAP_KILL`（`sig == 0` 探针一样）；单播要同 uid 或 `CAP_KILL`，pid 1 / kxld 一律 `EACCES` |
| ~~`sys_ps` / `SYS_diskinfo` / `sys_fdisk` 全开放~~ **已修复（2026-10-03）** | `syscall.c` 分发表 | `sys_ps` / `SYS_diskinfo` 要 `CAP_SYS_ADMIN`；`sys_fdisk` 已随 `usr/fdisk.c` 删除，其功能就是 `SYS_diskinfo`，一并被挡 |
| ~~无 `RLIMIT_AS` / 内存 cgroup / CPU 时间限制~~ **内存一半已修（2026-10-05，T-032）** | `include/sys/cred.h` `cred_mem_quota()` | 普通用户的整个用户地址空间限 64 MiB（root/kxld 无上限），超限 `sys_brk` 回 `ENOMEM`；**CPU 时间限制与 cgroup 仍无** |
| `NR_FDS 16` 是编译期常量 | `include/types.h:59` | 不是限制，是常量 |
| 用户栈/用户堆完全可写可执行 | 见 4.2 | 无沙箱 |

**这一条是 disqualifier。** 内核里所有进程都是 root，因此不存在"不可信代码"这个概念。所有 C 类缺陷（内存破坏、越界写、未初始化数据）的杀伤半径都从"一个进程"升级为"整机"。

**要建的东西**：
- `struct cred { uid_t uid, gid_t gid; u64 caps; }`，挂进 `struct task`，`fork` 继承、`execve` 按 ELF `e_uid`/`e_gid` 设置
- 至少两类能力：`CAP_KILL`、`CAP_SYS_ADMIN`（diskinfo/fdisk/brk 扩张）
- 首个进程（`kernel_spawn("/bin/init")`）为 uid 0；用户程序默认为 uid 1000
- `sys_kill` 校验：非 `CAP_KILL` 只能发给自己或同 uid 进程
- `sys_fdisk` / `SYS_diskinfo` / `sys_ps` 校验 `CAP_SYS_ADMIN`

成本估计：**150–300 行 + 现有调用点加检查**。收益：把"一个用户程序弄死内核"降级为"一个用户程序弄死自己"。

> **✅ 2026-10-03 已落地主体（T-030 + T-031，分三天）**
>
> - `struct cred { kind, uid, caps }` 挂进 `struct task`，`fork` 继承，`kernel_spawn` 为 uid 0；`cred_capable()` 让 root/kxld 天生持有全部能力。
> - 三类能力齐了：`CAP_KILL`、`CAP_SYS_ADMIN`（ps/diskinfo）、`CAP_FB`（4 个绘图调用，`fb_info` 只读开放）。
> - **降权路径改了**：上表写的"`execve` 按 ELF `e_uid`/`e_gid`"做不到——ELF64 的 `Elf64_Ehdr` 里没有这两个字段。改为新增 `SYS_setuid`（编号 31，追加不重命名），`uid != 0` 单向降到 `CRED_USER` 并清空 caps，用户态再喊 `setuid(0)` 得 `EACCES`。**"用户程序默认 uid 1000"因此仍是"root 可以降下去"，不是"天生就是 1000"**——够测试用，但还不是完整的登录模型。
> - 检查点：`sys_kill`（跨 uid + 广播）、`sys_ps`、`SYS_diskinfo`、`fb_clear/fill/char/puts`。
> - 每处拒绝打回执 `[cap] denied <call>: pid .. lacks CAP_*`。
> - **仍缺**：文件写操作的"所有者"判断（无 inode 属主元数据）、CPU 时间限制
>   （内存限额已由 T-032 补上，2026-10-05）、`fb_*` 的 owner 归属
>   （~~T-033~~ **已由 T-033 补上，2026-10-05**：能力门之后再一道归属门，
>   回执形如 `[fb] denied fb_fill: pid 13 holds the surface, pid 14 draws`；
>   `sys_exit` 释放，原语层不查）。
> - 测试：`ktest/t_cred.c`（7 例，开机即断言放行/拒绝两半）、`usr/systest.c` 降权子进程段、`scripts/smoke_test.py` 新增 24 条 tag（135/135 通过）。

#### 4.2 CPU 安全特性 —— 三个都没开

逐条核对了 `arch/entry64.S` 与 `arch/trampoline.S` 中**每一处** CR0/CR4/EFER 写入：

| 特性 | 位 | 现状 | 证据 |
|---|---|---|---|
| **`EFER.NXE`** | EFER bit 11 = `0x800` | **从未置位** | `entry64.S:122` 只有 `or eax, 0x100`(LME)；全树搜不到 `0x800` |
| **`CR0.WP`** | bit 16 = `0x10000` | **从未置位** | `entry64.S:128` 只有 `or eax, 0x80000000`(PG) |
| **`CR4.WP`** | bit 16 = `0x10000` | **从未置位** | `entry64.S:70` 只有 `or eax, 0x20`(PAE) |
| **`SMEP`** | CR4 bit 20 | **从未置位** | `entry64.S:261` 只有 `or rax, 0x20`；`smp.cpp:206` 只有 `OSFXSR\|OSXMMEXCPT` |
| **`SMAP`** | CR4 bit 21 | **从未置位** | 同上 |

**后果逐条**：

1. **NXE 未开 ⇒ 内核从不设置任何 PTE 的 NX 位**。用户栈、用户堆、整个用户地址空间**全部可执行**。往用户 buffer 里写一段 shellcode 直接运行。
2. **`CR0.WP` + `CR4.WP` 未开 ⇒ `vmm_map_user(..., writable=false)` 提供零保护**。`kernel/elf.c:54-55` 按 `PF_W` 传进去的只读 text 段，ring-3 照样能写。`vmm_map_user` 的 `writable` 参数目前是装饰性的。
3. **SMEP/SMAP 未开 ⇒ ring-3 可以执行 ring-0 文本页、可以访问数据页**。任何用户态内存破坏原语都直接升级为内核代码执行。

**成本极低、收益极高**：`entry64.S` 的 `.activate` 段加 `or rax, 0x800`（NXE）、`or rax, 0x10000`（CR4.WP），`cr0` 那段加 `or eax, 0x10000`。加上 `smp.cpp` 的 `ap_main` 同样设置。**加起来不到 15 行汇编**，就建立起"任意用户代码只能执行用户代码、只能写自己的可写页"这个最基本的边界。这是本文档里投入产出比最高的一项。

**还需要但没提的：KPTI。** 三槽设计意味着**每个进程的 PML4 里都有一份完整的内核映射**（槽 0 和 511 被复制进去）。这是经典的"共享内核地址空间"设计，速度好但意味着 Meltdown 类攻击面存在。对这个体量的内核不是必需项，但如果要往 L3 走，终点是把内核页表从用户 PML4 里摘掉（在 syscall 入口/返回时换 CR3）。这一点应该明确写进设计文档，而不是默认不提。

#### 4.3 内存管理策略 —— 用户态有了天花板，内核态仍是"死"

**无回收、无 swap、无 OOM killer**；**按进程记账已有（T-032，2026-10-05）**。

- ~~`kmalloc` 失败直接 `panic`（`kernel/mm/kheap.c:54,70`）~~ **已修（T-004，2026-10-03）**：
  返回 `NULL`，19 处调用点逐个核对；FS 层那些 `if (!kmalloc(...)) return LNXRM_ENOMEM`
  因此**不再是死代码**
- ~~`sys_brk` 无上限（`syscall.c:19` 只检查 `< USER_STACK_TOP - 1MB`）~~
  **已修（T-032，2026-10-05）**：增长前查 `cred_mem_quota()`，
  **普通用户 64 MiB（整个用户地址空间）、root/kxld 无上限**，超限回 `ENOMEM`
- ~~内存耗尽是**无归因**的全局致命事件，回答不了"谁吃掉的 RAM"~~
  **已修（T-032）**：`struct task` 的 `mem_pages`/`mem_peak` + `ps` 的 `MEM`/`PEAK`
  两列；唯一真相是**页表行走** `vmm_count_user_pages()`，不是各处手工累加

**仍然成立的**：

- 内核侧 `kmalloc` **没有配额**：`kheap` 是固定的 16 MiB 窗口，buddy 见底时
  `kmalloc` 返回 `NULL` —— 靠调用点自己检查，**没有受害者选择，也没有归因**
  （谁申请的只有调用栈知道）
- **没有 OOM killer**：`pmm` 耗尽时内核分配失败、用户分配被配额挡住，但已驻留的
  进程不会被杀，谁也不让位
- 没有 swap / page reclaim，`mem_peak` 只是观测值，不触发任何动作
- 没有 CPU 时间限制、没有内存 cgroup（`RLIMIT_AS` 的语义由 `cred_mem_quota()` 承担）

**要建的东西**（剩余）：
- 至少一个 OOM 策略：超限时杀掉 `mem_pages` 最大的进程（承认是启发式），或
  让内核侧分配也按进程记账
- 把 `mem_peak` 接进观测面（`/proc` 或扩展 `diskinfo`），目前只能 `ps`

成本：~~记账+配额 ~100 行~~（**已花，T-032**）；OOM 策略 ~150 行。

#### 4.4 地址空间能力 —— 最大的能力缺口

对着 31 个系统调用编号清单（`include/abi/lnxrm_syscalls.def`）看，缺的不只是功能，是**天花板**：

| 缺失 | 后果 |
|---|---|
| **`mmap` / `munmap` / `mprotect`** | **最大缺口**。没有共享库、没有晚绑定、没有 JIT、不能映射文件。`brk` 是唯一拿内存的方式 → 任何"想在上面构建的东西"都构建不了 |
| `stat` / `fstat` | `ls` 只有 dirent 一个元数据来源（`d_ino` 已填充）。无文件大小、无时间戳、无类型判定 |
| `gettimeofday` / `clock_gettime` | 用户程序**完全无法测量时间**，只能睡。`ls` 显示不了 mtime，`ps` 显示不了运行时长，没有任何东西能超时 |
| `pipe` / 共享内存 / unix socket | IPC 只有信号 + 文件。shell 无法做管道 |
| 用户栈自动增长 / guard page | 深递归直接 `#PF` 死掉。`sys_execve` 只映射 16 KiB（4 页，`task.c:141`） |
| COW（写时复制） | `fork` 永远深拷贝整个用户地址空间（`dup_user_aspace`），代价随地址空间线性增长 |
| demand paging | 无页错误处理路径（见 4.9） |

**T-032 之后的边界（2026-10-05）**：普通用户的**整个用户地址空间**限 64 MiB
（`cred_mem_quota()`；root/kxld 无上限），超限 `sys_brk` 回 `ENOMEM`。所以第一行的
处境变了个方向：以前是"没有 `mmap`，但 `brk` 可以无限涨"，现在两个口对普通用户都关着
——既换不了文件，也过不了 64 MiB。**匿名 `mmap` 的优先级因此上升**：它是唯一还能给
用户程序扩内存的路径，而且落地时**必须走 `cred_mem_quota()` 这同一道门**，否则配额就
从"地址空间上限"退化成"堆上限"，`vmm_count_user_pages` 也就漏算了 mmap 出来的部分。

**`mmap` 缺失的连锁后果**（这是文档里最容易被低估的一条）：用户态只能构建**静态、单一文件、ET_EXEC** 的程序（`Makefile:45-47` 的 `-static -no-pie -mcmodel=large` 就是这个约束的直接体现）。这意味着这个内核**只能运行它自己那一组 16 个程序**，上面跑不了任何第三方软件。这是能力天花板，不是 bug 列表能表达的。

**要建的东西**（按依赖顺序）：
1. `mremap` 语义的 `mmap(NULL, len, prot, flags, fd, off)` —— 匿名映射先行（供 `brk` 迁移、共享内存、栈增长共用一条路径）
2. `munmap` / `mprotect`（后者顺带给 4.2 的 `CR4.WP` 一个真实使用者）
3. 用户栈 guard page + `#PF` 处理里的栈增长
4. COW：把 `dup_user_aspace` 改为"读时复制"，只需在 `ensure_table` 里检测 `PG_W` 且已映射的槽
5. `gettimeofday` / `clock_gettime`（4.6 的 per-CPU 时钟源已就绪，只差 syscall 本身）

成本：**mmap + munmap + mprotect + 页错误分发 ~600–1000 行**。这是单项最大投入。

#### 4.5 身份与生命周期 —— 缺一整层机制

`struct task` 是静态 64 槽数组里的裸槽位（`sched.c:23`）。`find_task(pid)` 线性扫描。`parent` 是指向**会被复用的槽位**的裸指针，无世代计数。

**具体后果**：

- **`task_free_slot()` 把槽置 `T_UNUSED`，但从不清理其他任务指向它的 `parent` 指针**（`sched.c:340-356`）。父进程被 `waitpid` 回收后，兄弟任务的 `parent` 就指向一个可被 `task_alloc_slot` 重新分配的槽——于是它们"认"了一个刚出生的新进程当父亲
- **经典 PID 复用**：`find_task(pid)` 扫表返回的是当前占用该 pid 的任务，与当初拿到这个数字的进程可能毫无关系。`kill(42)` 可以命中完全不相干的进程
- `sys_exit` 的孤儿重挂载用 `find_task(1)`（`task.c:240`），pid 1 一旦先退出就返回 NULL，`parent` 直接变 NULL
- `find_task` / `task_iter`（`sched.c:358,386`）在 SMP 下**无锁线性扫 64 槽**，与并发 `task_alloc_slot` 竞争

**这不是"少了个校验"，是缺一整层机制。** Linux 专门用 refcount 的 `struct pid` + `pidfd` 解决这一类（`pidfd` 的存在理由就是 PID 复用）。

**要建的东西**：
- `struct pid { u32 nr; atomic_t refcnt; }`，任务持引用
- 或最低成本方案：`struct task` 加 `u32 generation`，`find_task` 比对 `(pid, generation)`，并让 `parent` 改为 pid 存储而非指针
- 回收时主动遍历清理指向自己的 `parent` 指针（64 槽，O(n) 可接受）
- `find_task` / `task_iter` 加 `task_table_lock`

成本：**pid 对象 ~200 行；世代号 + 回收清理 + 加锁 ~80 行**。后者能解决 80% 的实际问题。

#### 4.6 时间与时钟 —— 每 CPU 本地 tick + 实测标定（2026-10-01 修，遗留：时间系统调用）

> **2026-10-01 第二轮已修复主体**（见第二部分 §16）。现状：
>
> - **每个 CPU 有本地 LAPIC 周期 tick**：BSP 在 `smp_init()` 前调一次
>   `lapic_timer_calibrate()` 缓存周期（`s_lapic_period`），AP 在 `ap_main`
>   里用缓存武装自己的 timer（各自标定会竞争 PIT 锁存端口）。
> - **`jiffies` 仅 BSP 递增**（`pit_handler` 里 `if (this_cpu_data()->bsp)`），
>   否则全局时钟会变成 NCPU×HZ；其余 CPU 仍然跑 `sched_tick`/`sched_maybe_preempt`。
> - **`mdelay` 用实测 TSC 频率**（标定窗口顺带测出 `s_tsc_per_ms`），
>   删除了 `smp.cpp` 硬编码 ~2 GHz 的本地版本。
> - **PIT 模式 3 → 模式 2 修复**：原 `pit_init` 用 `0x36`（方波），QEMU 的
>   模式 3 每 `divisor/2` 就重装，标定窗口只有 5 ms → period 减半 →
>   **jiffies 实为 200 Hz**（所有 sleep 以 2 倍速跑，潜伏已久）。改 `0x34`
>   （速率发生器）后实测 ≈100 Hz。
>
> 仍缺：
>
> - 没有任何时间类系统调用（`gettimeofday`/`clock_gettime`，见 4.4 与
>   第三部分 T-098——依赖的时钟源现已就绪）
> - 无 invariant-TSC 检查、无 HPET 兜底；单次标定，无运行期重标定
> - 没有 monotonic / realtime 时钟分离

**要建的东西（剩余）**：
- `gettimeofday` / `clock_gettime`（TSC + `s_tsc_per_ms` 换算）
- CPUID leaf 0x15/0x16 校验标定结果；考虑 HPET 兜底
- monotonic clock 与 realtime clock 分离

成本：时间系统调用 ~100 行；标定校验 ~40 行。

#### 4.7 元数据与文件操作

- `stat` / `fstat` / `access`：需要把 `struct vnode` 的 `size`/`type` 暴露成稳定的用户 ABI 结构
- `chmod` / `chown`：FAT32 无权限位，实现成本低（仅存储）但意义有限
- `link` / `symlink`：FAT32 无硬链接
- 目录操作：目前无 `mkdir -p` 语义的原子性保证

#### 4.8 IPC

- `pipe` / `pipe2`：shell 管道的前提
- 共享内存：需要 `shm_open` 或基于 `mmap(MAP_SHARED, fd)`
- unix domain socket：最完整但成本最高，建议最后考虑

**注意**：4.4 的 `mmap` + 4.8 的 pipe 是有依赖关系的。**建议先做 `mmap`**——它同时解锁共享内存、栈增长、文件映射、`brk` 迁移。

#### 4.9 缺页处理路径

目前 `#PF`（`intno 14`）从用户态触发时，`isr_common` 直接 `sys_exit(-SIGSEGV)`（`kernel/isr.c:153-171`）。这意味着：

- 无 demand paging
- 无栈自动增长（4.4）
- 无 COW 缺页（4.4）
- 无 mmap 缺页处理（4.4）

**`mmap` 和页错误分发是一个整体**，必须一起做。`isr_common` 需要从"异常即杀进程"演进为"缺页 → 尝试修复 → 修不了才杀进程"，同时保留现在这个安全的默认行为。

#### 4.10 块设备抽象

`struct blkdev`（`include/sys/vfs.h:96-103`）是 4 个函数指针，**无队列、无合并、无锁定、无分区抽象、无取消**。每次访问都是同步不可中断的。

- **IDE 无条件赢过 AHCI**：`main.c:261-262` 先 `ide_init` 后 `ahci_init`，而 `vfs_try_mount_disk` 只看 `blk_first`（`vfs.c:606`）。AHCI 盘被注册、能被 `fdisk` 看到、**永远挂不上**
- `blk_register` 有 off-by-one：`blk_count >= 8` 没加进 `blk_list` 却仍设置 `blk_first`（`vfs.c:580-581`）
- 分区设备从不 `blk_register`，因此 `diskinfo`/`fdisk` 看不到分区
- 没有中断驱动的 I/O（见 6.3）

**要建的东西**：请求队列（多 slot）+ 简单合并 + `blk_first` 改为"选一个能挂载的"，或者干脆引入明确的设备选择策略。

成本：**~300 行**。

#### 4.11 日志与崩溃转储

当前可观测性 = 串口日志。缺：

- loglevel 过滤
- dmesg 式环形缓冲（内核崩溃后可回读）
- **完整 backtrace**（现在 `panic` 只打印 8 个栈字，`isr.c:181-184`）
- 全部 CPU 的寄存器 + CR3 转储
- `reboot` / `halt` 系统调用（现在 `panic` 后永久 `hlt`，QEMU 必须加 `-no-reboot`）
- panic 路径绕过 `print_lock`（`print.c:346-356` 调三次 `kprintf`，而 `kvprintf` 要拿锁——**从已持锁的上下文 panic 就是永久自旋而不是停下**）

成本：**~250 行**，且与 5.2 高度重叠。

---

### 5. B 类：缺失的工程纪律

#### 5.1 测试 —— 最大的单项缺口

**核实结论**：零测试文件、零 CI 配置、零断言框架。Makefile 只有 `all / usr / rust / clean / run`。

这一条的杀伤力远超其表面：**本文档列出的全部 C 类缺陷，都是"读"出来的，没有一个是"跑"出来的，因为没有任何一个测试会因为它们失败。** 改一行代码之后，作者也没有任何手段知道有没有弄坏别的东西。

内核这种硬件 × 并发 × 内存的交汇处，没有测试和可观测性，所有正确性都只能靠作者的记忆和运气维持。

**建议的分层测试架构**（成本递增、价值递增）：

| 层 | 手段 | 覆盖 | 成本 |
|---|---|---|---|
| **L-单元** | `assert()` + panic 打印，串口输出计数 | `bin_of`、`fb_pack`/`fb_rgb_px`、`ansi_feed` 状态机、`fat_short_to_name`、CRC/LBA 计算 | **~1 天** |
| **L-内核自测** | 启动时跑一组内核态用例，通过后打印 `[selftest] N/M pass` | PMM 分配/释放/合并一致性、buddy 合并不变量、kheap 分配释放配对、页表 walk/map/unmap 往返、copy_from/to_user 边界 | **~3–5 天** |
| **L-黑盒** | 用户态测试程序，从 shell 或 init 自动运行，退出码编码结果 | 文件系统 CRUD + 崩溃恢复、fork/exec/wait 语义、信号投递/屏蔽/嵌套、mprotect 拒绝写只读页 | **~1–2 周** |
| **L-属性测试** | 对页表映射、USERCOPY 边界、ELF 解析做随机化输入 + 不变量断言 | 页表 walk 不变量、"任意输入不 panic"、"任意输入不越界" | **~1 周** |
| **L-故障注入** | 分配器失败注入、块设备返回错误、丢中断 | OOM 路径全返回错误而非 panic、I/O 错误正确传播到用户态 | **~1 周** |
| **L-CI** | QEMU 无头 + 串口日志断言 + 退出码 | 每次改动验证 | **~2 天** |

**最小可行起点**：`L-单元` + `L-内核自测` + `L-CI` 三件套，大约 **一周**，就能让"改代码会不会弄坏东西"这个问题有答案。而这会连带解决 D 类（契约漂移）：一旦 `fat32_journal` 的原子性有测试、`console_write` 的锁有测试，注释里的谎就会立刻暴露。

**故障注入尤其值得优先做**，因为 C 类里最严重的一批缺陷（未检查 I/O 返回值、未初始化缓冲区、`kmalloc` panic）**全部是失败路径缺陷**——正常路径下永远看不到。

#### 5.2 可观测性与内部断言

- **debug 系统调用**：`ps_dump` 现在直接往内核控制台打（`syscall.c:143`），**不填用户缓冲区**。这是"没有可测试性"的直接原因——你无法从用户态断言任何内核状态。加一个 `SYS_debug_dump(struct u_debug_req*)`（查询内存用量、页表项、任务快照）能立刻解锁 L-黑盒测试
- **`/proc` 式命名空间**：至少 `/proc/meminfo`、`/proc/uptime`、`/proc/self/maps`。有了它，`ps` 就能变成填用户缓冲区的系统调用而不是内核打印
- **`BUG_ON` / `WARN_ON`**：页表 walk 返回 NULL、页表项 PG_P 置位但父表项缺失、`rmb` 不变量、分配释放不配对…… 这些地方现在静默通过或者 panic，应该分级
- **内核统计计数器**：分配失败次数、I/O 错误次数、页错误次数、调度次数。让 `free` 命令之外有东西可看

成本：**~300 行**，与 5.1 强耦合。

#### 5.3 编码规约

`.clang-format` 已存在（好事），但缺少：

- 改动前的静态检查（`clang-tidy` / `cppcheck`）
- 关键不变量写成断言而非注释
- 头文件 include 图的自动检查（`include/sys/percpu.h` ↔ `include/sys/sched.h` 互相 include 靠 `#ifdef` 打破，脆弱）
- ABI 结构体的静态断言与版本号（现在 `boot.h` 对 `struct vbe_lfb_info` 有 static_assert，是好起点）

---

### 6. C 类：存在但不可靠

以下缺陷**机制本身是对的**，实现有正确性问题。按严重度排列，全部核实过。

> **编号 C1–C72 是稳定 ID，不是序号。** 已随死代码一起消失的条目（C2/C6，以及
> 曾经的 C3）保留编号并标记为已删除，**不重排**——因为 C5/C13/C16/C28/C48/C57/C62/C65/C68/C69 等
> 编号被 `docs/README.md` 和本文第三部分交叉引用，重排会让所有
> 引用失效。当前有效条目 **66 条**（已修复或判定误报的条目保留编号，就地标注
> 状态——2026-10-01 第二轮处理了 C1/C8/C9/C63，见第二部分 §16）。C3 是唯一
> 一条**重新生效**的：它曾随后缓冲一起被判死并从计数里去掉，又因为 23–27 五个
> 绘图系统调用恢复而重新适用，**2026-10-05 以 T-033 完成**。同日 T-080 又修掉
> C50、C54 两条（C53 只修了容量字段，LBA48 命令仍缺，保持有效）
> ——所以是 **70 − C2/C6/C50/C54 = 66**。
>
> **2026-10-06 真机复位事故一轮**（详见 C71、C72 与 `CODEBASE.md` §22.6）：
> 新增 C71、C72 两条，**出生即已修复**，不计入有效条目；C56 部分修复（递归深度）
> 但后一半未做，仍有效；C57 的窗口口径从 4 GiB 改成 `PMM_WINDOW_TOP`（1 GiB）；
> C68 由"完全未经测试"改为"已手工实测，未纳入自动化"，仍有效。
> **有效条目数不变，仍是 66。**

#### 6.1 并发正确性 —— 最大的实现缺口

| # | 问题 | 位置 | 触发条件 |
|---|---|---|---|
| C1 | ~~用户态写终端绕过 `print_lock`~~（原描述：`console_write` 逐字节裸调 `console_putc`，无锁改写 `vrow/vcol`、`fbc_row/fbc_col/fbc_fg/fbc_bg`、两套 ANSI 解析器状态、`ring_head/ring_count`） | `kernel/fs/vfs.c` `console_write` vs `kernel/print.c` | **已修复（2026-10-01）**：`console_out_lock/unlock`（irqsave）导出，`kvprintf` 与 `console_write`（64 字节分块，限制关中断时长）共用（编号保留，不再重排） |
| C2 | ~~帧缓冲双缓冲全局无保护~~ | — | **已随双缓冲一起删除**（编号保留，不再重排） |
| C3 | ~~后缓冲归属只记录不强制~~ | `kernel/framebuffer.c` 末尾 `static u32 fb_owner` + `kernel/syscall.c` `fb_owner_gate()` | ~~已随后缓冲一起删除~~（编号保留，不再重排）。**重新生效并于 2026-10-05 完成（T-033）**：单 pid 归属，`fb_claim()` CAS 占用（拒绝不改归属），释放只在 `sys_exit`，原语层一道门都不查（控制台共用那 4 个函数）。后缓冲本身仍不存在（见 C2/C6） |
| C4 | `pick_next()` **不持 `runq_lock`** 就读共享可变的 `cpu->runq_head.rq_next`，而 `runqueue_add` 可在**远端** CPU 队列上写 | `sched.c:41-47` vs `:67-86` | 跨 CPU 入队时 |
| C5 | `con_waiter` 是**单槽全局** | `isr.c:96,112` | 第二个进程阻塞在 `console_read` 会覆盖第一个的等待者，后者永久卡死 |
| C6 | ~~后缓冲跨核无 cache 维护~~ | — | **已随后缓冲一起删除**（编号保留，不再重排） |
| C7 | 块缓存 `cache_lock` + `fat_fs_lock` **两层 irqsave 嵌套自旋锁，跨越设备 I/O** | `blk_cache.c:92,141` → `ahci.cpp:122` | 任何其他 CPU 碰缓存/文件系统就关中断空转 |
| C8 | ~~`sys_fork` fd 表竞争 → use-after-free~~（原描述：先 `memcpy` 父 fds，**再**加引用计数；父在另一 CPU 关 fd 则 `kfree`） | `task.c` `sys_fork` | **判定为误报（2026-10-01）**：进程单线程，`fork` 执行期间父没有第二条执行流能并发 `close`，fd 条目持引用计数（编号保留，不修） |
| C9 | ~~`find_task` / `task_iter` 无锁扫 64 槽~~ | `sched.c` | **已修复（2026-10-01）**：`find_task` 在 `task_table_lock`（irqsave）内扫描；附带 `task_alloc_slot` 清 `pid=0`、`send_signal` 拒绝 `T_UNUSED`。`task_iter` 有意保持无锁（裸指针语义 + 调用方无并发写者，见第二部分 §16.1） |
| C10 | 块设备全局锁 + 无 per-task 锁的 `blkdev` | `include/sys/vfs.h:96-103` | 任何并发磁盘访问 |

**C1 曾是最刺眼的**（默认 `make run` 就是 SMP 2，任何用户程序每次打印都在无锁改写两套 ANSI 解析器状态），**已于 2026-10-01 修复**——`console_write` 与 `kvprintf` 共用 `console_out_lock`。

#### 6.2 存储与文件系统

**会挂死整机的**（优先级最高）：

| # | 问题 | 位置 |
|---|---|---|
| C11 | **FAT 簇链无环检测**。`fat_next_cluster` 把任何 `< 0x0FFFFFF8` 的值当合法下一簇；所有遍历 `while (!eoc && c>=2)` 无迭代上限。一个自引用或保留值条目（如 `0x0FFFFFF7`）→ **持有 `fat_fs_lock` 且关中断**死循环 → 整机挂死。触发条件：**一个坏扇区** | `fat32.c:17-18`；`fat32_dir.c:47`；`fat32_file.c:95,150,156,180`；`fat32_meta.c:142,187,291,318,347` |
| C12 | ~~**用户态程序能让内核 panic**（`kmalloc` OOM → panic，FS 层 NULL 检查全是死代码，`brk` 无上限）~~ **已修复（2026-10-03）**：`kmalloc` 返回 NULL、19 处调用点全部核对（补 9 处），同链路的 `ensure_table`/`vmm_new_user_aspace`/`signal_map_restorer` panic 与 `sys_brk` 泄漏一并修掉（见第三部分 T-004）。`brk` 配额与记账**也已修复（2026-10-05，T-032）**：64 MiB/user + `ps` 内存列 | ~~`kheap.c:54,70`~~ `kheap.c`（`kmalloc_or_panic` 留给 bring-up） |

**静默数据损坏**：

| # | 问题 | 位置 |
|---|---|---|
| C13 | **`fat32_journal` 不是 journal**。存**后像**不是前像；`abort` 只清计数器，**无法回滚**；`commit` 只是把同样扇区**再写一遍**。无预留日志区、无落盘 superblock、无 replay/recovery。头注释"provides atomic operations"不成立 | `fat32_journal.c:1-2,51-64,67-94,97-102` |
| C14 | ~~journal 32 条上限溢出时 `add` 返回错误，而**全部 6 个调用点丢弃返回值**~~（原描述：1 MB 写要记 2048 扇区，2016 个静默丢弃；`begin` 返回值同样被 6 处忽略） | `fat32_journal.c`（**已删除**）；**已失效（2026-10-01 登记）**：journal 文件删除，写路径改走块缓存写回，编号保留 |
| C15 | ~~**未检查 `dev->read` 返回值 + 未初始化缓冲区 → 未初始化的内核栈被写进磁盘目录项**~~ **已修复（2026-10-05）**：全内核只剩 `blk_io_read()`/`blk_io_write()`（`blk_cache.c`）一处调用 `dev->read/write`，分区层（`mbr.c`）也改走同一口子；块层提供故障注入 `blk_inject_io(dev, lba, ops)` 让指定 LBA 的传输一律回 `LNXRM_EIO`（与真驱动拒绝同路径）。契约自下而上：扇区层只答 0/`LNXRM_EIO`，扫描层改三态（`fat_scan_dir`/`fat_resolve_path`/`fat_dir_iter`：1/0/负），VFS 不再把 `EIO` 折成 `ENOENT`/`EACCES`/「目录是空的」；`fat_write_impl` 三种结局（读失败不落盘 / 中途失败返回短计数 / 元数据失败 `EIO`）。缓存读失败作废 victim 槽（CODEBASE 9.3 一并修）。测试：`ktest/t_ioerr.c` 4 例 + smoke marker `t-005 injected I/O error reported`，`make smoke` **150/150**（详见第三部分 T-005） | ~~`fat32_meta.c:58,107,116,294`；`fat32_file.c:160,183`~~ `blk_cache.c:47,54,137,198`；`fat32_file.c:194,200`；`vfs.c:195,473` |
| C16 | **`alloc_chain` 簇数截断**：`u32 nclus = (need_bytes + cluster-1)/cluster`，`need_bytes` 是 u64（最大 4 GB，来自 `off + n`）→ 只分配少量簇 → 随后仍把 `filesize` 设成 `off+n`。文件**报称比实际数据大**，之后读该区间返回未初始化簇内容 | `fat32_file.c:88` |
| C17 | **栈内容泄漏成文件名**：`struct lfn_state lf;` 未初始化，`lfn_reset` 只清 3 字段，**253 字节栈垃圾留着**。LFN 序列号有空洞时 `memcpy` 会把上一个条目的残留拷过来，经 `d->name` → `getdent` **泄漏给用户态**。且从不校验 `raw[13]`（短名校验和）与序列连续性 | `fat32_dir.c:27-40,44,94` |
| C18 | **镜 FAT 从不更新**（只写 FAT #0），尽管 `fat32.c:165` 明确接受 `num_fats == 2`；FSINFO 也从不写 → 写完之后两个 FAT 不一致，其他实现会判定卷脏 | `fat32.c:28-29` |
| C19 | **`O_TRUNC` 只改内存**：只改 RAM 里的 `struct resolve`，磁盘目录项还是旧大小，簇链不释放。不带 `O_TRUNC` 重新打开还是老内容 | `vfs.c:176-178` → `fat32_file.c:122-125` |
| C20 | **写成功后找不到 dirent 则静默丢数据**：若父目录重扫找不到匹配的 8.3 别名（如文件被 unlink 而 fd 仍打开），落到 `done_write` 返回成功值，磁盘 `fstclus*`/`filesize` 仍指向旧链，写入的簇成孤儿 | `fat32_file.c:180-206` |
| C21 | **8.3 别名冲突写错文件**：`fat32_file.c:188` 纯按 11 字节别名匹配；`fat_scan_find_cb` 即使候选有 LFN 也回退到别名匹配 | `fat32_file.c:188`；`fat32_dir.c:113` |
| C22 | `create`/`mkdir` **从不写 LFN 记录** → `touch "很长的名字.txt"` 静默截断。非 ASCII 名按 `(u8)name[idx]` 每码元一字节 → UTF-8 被逐字节拆散 | `fat32_meta.c:64,394` |
| C23 | `parent_entry_add` **会覆盖 `ENT_END` 0x05 标记**，而 `dir_claim_run` 在第一个 0x00 处停 —— 两条路径对"哪些槽存在"判断不一致；且两者**都从不扩展目录链** | `fat32_meta.c:61` vs `:197-199` |

**缓存与一致性**：

| # | 问题 | 位置 |
|---|---|---|
| C24 | ~~**块缓存 LRU 是坏的**：`cache_find` 命中时设 `used=true` 且**永不清除**；`cache_find_victim` 前两遍找不到就走第三遍"清空所有标志"再返回 → **受害者永远是索引 0**。超过 64 扇区的工作集（一次 1 MB 写就是 2048 扇区）永久抖动同一槽位~~ **已修复（2026-10-05）**：改成时间戳 LRU —— `struct cache_entry.stamp` 记单调计数器 `lru_seq` 的快照，命中/填充/暂存都刷新，受害者 = 最小 stamp（先要空槽，拿空槽不丢任何东西）；三遍扫描整段删除，`used` 字段**整个移除**（只剩一份记账，没有第二份状态可与之矛盾）。配套新增 `blk_cache_stats()`（hits/misses/evictions/writebacks/dev_reads/dev_writes）、`blk_cache_lookup()`、`blk_cache_purge()`。测试 `ktest/t_cache.c` 5 例：200 扇区文件回写 137 次 ≤ 208、8 扇区热集被重写 6 轮并冲过 96 个冷扇区只回写 9 次 ≤ 12；把受害者换回旧算法这两例立刻红（17 次 > 12、被 touch 的扇区被赶走）。详见第三部分 T-007 | ~~`blk_cache.c:24-59`~~ `blk_cache.c:118-127`（受害者）、`disk.h:98-113`（`stamp`） |
| C25 | ~~读失败时受害条目已回写但 `valid` 仍为真、lba/dev 仍是旧的 → 返回错误后缓存里留着一份被半覆盖的脏数据~~ **已修复（2026-10-05，随 T-005 一并修掉）**：`blk_cache_read` 读失败即作废 victim 槽（`valid`/`dirty` 清零、`stamp` 归零）。丢一份缓存副本只多一次 I/O，留着它会把半写坏的扇区发给下一个读者 | ~~`blk_cache.c:116-120`~~ `blk_cache.c:201-213` |
| C26 | ~~**FAT 读路径绕过块缓存，写路径不绕过**。`fat_dir` / `dirent_locate` / `dir_claim_run` / `fat_set_entry` / `mbr_parse` 也都是直接打设备 → 读可能看到陈旧数据。靠 `fat32_journal_commit` 最后那次 flush 偶然掩盖，不是设计~~ **已修复（2026-10-05）**：数据/目录的读写全部走 `fat_read_sector`/`fat_write_sector` → `blk_cache_*`（读路径在 T-005 期间一并改完），直调 `dev->read/write` 全内核只剩 `blk_io_*` 一处。剩下的两条路各管一段互不重叠：数据区读写走缓存，FAT 区读 `m->fat`、写 `fat_set_entry` → `blk_io_write` 直写（分配先落盘，才能被指向它的目录项暂存；改走缓存会把顺序倒过来）。不变式「FAT 区永不进缓存」由 `test_fat_region_is_never_cached` 守住（2018 个 FAT 扇区逐个 `blk_cache_lookup()` 必须为 -1）。`fat32_journal` 已删除，「靠它那次 flush 掩盖」的前提不复存在。详见第三部分 T-008 | ~~`fat32_file.c:70` vs `:160,165,183,195`；`fat32_dir.c:48`；`fat32_meta.c:144,189`；`fat32.c:28`；`mbr.c:42`~~ `blk_cache.c:67,75`（唯一直调点）；`fat32.c:114,126`（FAT 直写）；`fat32_priv.h:80,91`（缓存读写）；`t_cache.c:315` |
| C27 | 无周期性回写、无退出/panic 时同步 → 最多 32 KB 脏扇区可能丢失 | `blk_cache.c`（全） |
| C28 | 分区 `blkdev` 从不 `blk_register`；`blk_cache_flush` 只对挂载中的那块整盘，从不对分区 flush（**T-081 后前半段的连带影响已解**：`vfs_try_mount_disk` 不再读 `blk_first`、改为遍历 `blk_list[]`，"谁先注册谁当根"这条隐性策略没了；分区仍不注册） | `vfs.c:631,639,658,664,721` |

**边界与算术**：

| # | 问题 | 位置 |
|---|---|---|
| C29 | **512 字节栈缓冲区却按 `bytes_per_sector` 索引**（`fat32_meta.c:59,106-117,431,449`；`fat32_file.c:57,155,184,295`）→ `bps > 512` 时全部溢出。目前只因 `fat32.c:163` 恰好要求 `bps == dev->sector_size` 且两驱动都硬编码 512 才没炸——**不变量守在了错误的层** | 同左 |
| C30 | `fat_dir_iter` **每次 getdent 都从簇 0 重扫整个目录**（cookie 还把索引和标志打包进一个 u32）→ 一次 `ls` 是 O(n²) 磁盘读 | `fat32_dir.c:188-198` |
| C31 | LFN 缓冲 256 字节 vs FAT 规范 260 字符上限 → 247+ 字符文件名被静默截断 | `fat32_dir.c:28` vs `fat32_meta.c:87` |
| C32 | `u32 fat_entries` 对超过 4 GiB 的 FAT 溢出 → 静默把 `max_cluster` 砍半 | `fat32.c:192` |
| C33 | `max_cluster = fat_entries - 1` 但所有消费者当**开区间**用 → 最后一个可用 FAT 条目不可达 | `fat32.c:193` |
| C34 | `strncpy(r->name, comp, 55)` **不写终止符**，而调用方的 `struct resolve` 未初始化 | `fat32_dir.c:144,156`；`fat32_file.c:8`；`fat32_meta.c:281,330` |
| C35 | 空文件上 `write(fd, buf, 0)` 返回 **EIO**（`need==0` → `nclus==0` → `alloc_chain` 返回 0 → 被当成磁盘满） | `fat32_file.c:139` |
| C36 | MBR 条目**不校验**是否落在设备范围内；扩展分区标记了但 EBR 链从不遍历 | `mbr.c:67-69,116` |
| C37 | `static char part_name_buf[16]` 被**所有分区共享** → 每个分区设备的名字都是最后生成的那个 | `mbr.c:107,114` |
| C38 | 无 `0x55AA` BPB 签名检查、无 `"FAT32   "` OEM 检查、无 `bpb[42]==0`；`sectors_per_cluster` 未验证是 2 的幂而 `fat_set_entry` 依赖它 | `fat32.c:143-170` |
| C39 | `fat_freespace` 是 O(FAT) 全扫且**无调用者**；所有 `_impl` 用全局 `fat_priv` 而 vnode 的 `mnt_data` 是硬编码哨兵 `(void*)1` → 整个 mount 间接层是装饰性的 | `fat32.c:47-54`；`fat32_file.c:15,28` |

**VFS 层**：

| # | 问题 | 位置 |
|---|---|---|
| C40 | **每次 `open` 普通文件泄漏一个 vnode**（`f->priv` 是 vnode 拷贝，而 `reg_fops.close = noop_close` 不释放）→ 约 48 字节/次永久泄漏 | `vfs.c:208-210,480` |
| C41 | **访问模式从不强制**：`reg_read`/`reg_write` 无条件调底层，从不看 `f->flags` → `O_RDONLY` 打开的文件**可写**；`O_APPEND`（`vfs.h:51`）完全未实现 | `vfs.c:407-421` |
| C42 | `vfs_file_size` 对字符设备把 NULL 的 `priv` 强转 `struct vnode *` 解引用 | `vfs.c:229-233` |
| C43 | `vfs_close_file` 无下溢保护，二次 close 双重 `kfree` | `vfs.c:238` |
| C44 | `sys_getdent` 截断到 1024 而 `dir_getdent` 要求 `len>=72`；更小的 `len` 让 `ls` 零返回死循环 | `vfs.c:394,423` |
| C45 | ~~`dir_getdent` 从不设 `d_ino`~~ **已修**：`dirent_out.ino` 由 FAT32 以 `(cluster<<32)\|offset` 提供，`/dev` 给 1，`dir_getdent` 逐条填充 | `vfs.c` `fat32_dir.c` |
| C46 | ~~`abs_path` 静默截断而非返回 `ENAMETOOLONG`~~ **已修复（2026-10-03）**：放不下的路径返回 `NULL`，6 个调用点（`open`/`mkdir`/`unlink`/`rmdir`/`rename` 两端）回 `LNXRM_ENAMETOOLONG = -36`（新增 errno）；截断会让内核去查一个调用方没命名的文件。`vfs.c` 的 WARN 一并删除（它存在的唯一理由就是"还没错误路径"），`t_fs.c` 改断言"超长 → NULL、29 字符边界 → 成功"，`t_vfs.c` 新增 `test_path_too_long`，`systest` 加 `path.toolong.*` 5 条 tag | `vfs.c:106` |
| C47 | 挂载表基本是装饰：`register_mount` 只用 `"/"` 调用过；`fs_for_path` 的"无前导斜杠"分支不可达 | `vfs.c:15,40-97,597,628` |

#### 6.3 设备驱动

| # | 问题 | 位置 |
|---|---|---|
| C48 | **AHCI 设备寄存器按可缓存映射**：`vmm_map_kernel_page(..., 0x03)`，`vmm.c:286` 拼 `PG_P\|PG_W\|flags`，无 `PG_PCD`。对 PCIe 设备的 CLB/FB/CI 写不能走 RFO/reorder。且 `P_CLB/P_FB/P_CI` 写后**无 `clflush`/`sfence`**——这是 AHCI 挂死的最常见原因 | `ahci.cpp:256`；`vmm.c:270-286` |
| C49 | **AHCI 完全无中断**：无 `P_IE`/`HBA_GHC` 使能、无 MSI/MSI-X、无 ISR、无 `HBA.IS` 排空。每个命令 `cli` 住自旋 `P_CI` 最多 10⁷ 次，**在两层嵌套 irqsave 锁里** | `ahci.cpp:122,273` |
| C50 | ~~**AHCI 完成判定只看 `IS_TFES`，从不看 `PxTFD.ERR`** → ATA 级失败被当成功 → 预填的 `0xCC` IDENTIFY 变成 `num_sectors = 0xCCCCCCCC`，还被 `blk_register` 当真设备~~ **已修复（2026-10-05，T-080）**：`issueCmd` 在 `P_CI` 清零后先查 `IS_TFES`，再查 `PxTFD.ERR`（bit 0）——完成不等于成功；容量另走 `ata_total_sectors()`，读不出就拒绝注册。修在**完成判定**这一层，正是 `0xCC` 泄漏进 `num_sectors` 的唯一入口 | `ahci.cpp:119-134,362-377` |
| C51 | AHCI BAR 类型未校验（假定 64-bit memory BAR，32-bit BAR 时 `0x28` 是下一个 BAR）；`abar==0` 不拒绝；只映射 4 KiB ABAR（实际 32 KiB–1 MiB）；FIS 的 LBA 48-63 位留零；`ahciProbe` 第一个盘就 `return 1` | `ahci.cpp:252-257,136-141,284,356` |
| C52 | AHCI "flush" 实为**发一条多余的 READ 命令**到 512 字节栈缓冲，忽略结果，且读的是 `lba` 而非 `lba+cnt-1`；从不发 SATA FLUSH CACHE | `ahci.cpp:235-236` |
| C53 | **IDE 无 LBA48**：只编程 28 位寄存器 → >128 GiB 磁盘读不到（**部分修复 2026-10-05，T-080**：① 容量不再只读 words 60-61，`ata_total_sectors()` 先取 48 位 words 100-103，`FFFFFFFFh` 哨兵不再被当成 2 TiB；② 读写扇区前置 `lba > 0x0FFFFFFF` 上界检查，超界返回 `LNXRM_EFAIL` 而不是把高 4 位丢掉静默读错扇区。**仍未做**：`READ/WRITE SECTORS EXT`（0x24/0x25）命令本身） | `ide.c:52-53,72-73,173` |
| C54 | ~~**IDE `ide_init` 无 DRQ/错误轮询**：`s == 0` 的"无设备"判定与命令竞争，无 400 ns 稳定期，256 字读取无状态检查 → 垃圾 `lba28` 被注册成 `num_sectors`~~ **已修复（2026-10-05，T-080）**：选盘后 4 次状态读当 400 ns 稳定期；无设备判据改成 `s == 0x00 \|\| s == 0xFF`（**空总线回 `FF`，不是 `00`**，且判定挪到发命令之前）；IDENTIFY 后走 `ide_wait_drq()` —— 它是全文件唯一判 `IDE_SR_ERR` 的地方，而 IDENTIFY 以前**一次都没调过它**，这就是 `0xFFFF` → 词 60–61 全 1 → `num_sectors = 0xFFFFFFFF` 的入口 | `ide.c:133-194` |
| C55 | `ide_wait_ready` **从不检查 `IDE_SR_ERR`** → 出错状态被报成超时；`ide_write_sector` 的"flush" 忽略返回值且不发 `0xE7` CACHE FLUSH | `ide.c:28-35,82` |
| C56 | ~~**PCI 桥递归无深度限制** → 桥成环则递归到爆栈~~ **部分修复（2026-10-06）**：`bus_seen[32]` 位图（`pci.c:37`）做 visited、每次 `pci_scan` 前清零（`:78`），`PROBE_MAX_DEPTH 16`（`:38`）在 `:68` 卡深度。**仍未做**：对所有总线都用 0xCF8/0xCFC，而它只对 bus 0 有架构保证 | `pci.c:37-38,66-68,78` |

#### 6.4 内存子系统

| # | 问题 | 位置 |
|---|---|---|
| C57 | `pmm` 位图固定放在物理 `0x10000`（`BITS_BASE`），只因为管理窗口被钳住才没和 0x100000 的内核镜像重叠。**这是个未被文档化的隐式依赖**（2026-10-06 起有了文档，但依赖本身还在：把 `PMM_WINDOW_TOP` 调回 4 GiB 就会撞上镜像） | `pmm.c:23,105,119-125`；`mm.h:20` |
| C71 | **PMM 管理窗口大于恒等映射范围 → 启动即三重故障**。恒等映射/高半区别名只铺了 1 GiB（`entry64.S` 只填 `PDPT_M[0]`、`PDPT_HI[510]`），窗口却钳在 4 GiB：`pmm_init` 播种到 `0x40000000` 按恒等地址解引用 → `#PF (CR2=0x40000000)`；而全树唯一 `lidt` 在 `arch/cpu.c:120`、由 `cpu_init()` 调用且**晚于** `pmm_init` → 无 IDT → `#DF` → **三重故障，无痕复位**（`panic()` 是 `cli;hlt`，只挂死不复位，所以真机上"闪几行就重启"必然是这条）。QEMU 全部测试 `-m 256`，永远碰不到 1 GiB 边界，故长期未暴露。**已修复（2026-10-06）**：`PMM_WINDOW_TOP = 0x40000000` 钳住窗口与别名循环、整段在窗口外的区域跳过；IDT/GDT 提前到 `start_kernel` 头四步；无可用 RAM 改 `panic` 而不是 `memset` 抹掉自己的页表。**代价**：>1 GiB 内存不参与分配。完整事故复盘见 `CODEBASE.md` §22.6 | `mm.h:20`；`pmm.c:105-106,122-124`；`vmm.c:237`；`main.c:221-224` |
| C58 | ~~`kmalloc` OOM → `panic`（见 C12）~~ **已修复（2026-10-03）**：返回 NULL，只有 `kmalloc_or_panic`（`operator new`）仍 panic | `kheap.c` |
| C59 | `vmm_map_kernel_page` 假定"调用者只碰新槽位"（注释自陈），但 DISPI 覆盖 pitch 后可能踩到已有映射的槽 → 把 2 MiB 数据页当页表写 PTE，破坏活内存 | `vmm.c:274-286`；`framebuffer.c:512` |
| C60 | ~~`sys_brk` 中途 `pmm_alloc` 失败时**已映射的页泄漏**~~ **已修复（2026-10-03）**：失败即回滚本次已映射的页再返回 ENOMEM（`brk.oom` 段覆盖） | `syscall.c` |
| C61 | `vmm_map_user` 不检查已有 PTE、`invlpg` 打在当前 AS 而非目标 AS（无害但无用） | `vmm.c:57-75` |
| C62 | 调度器**不是 round-robin，是 LIFO**：`runqueue_add` 往头部插（`sched.c:78-79`），`struct cpu_info`（`percpu.h:21`）只有 `runq_head`、**无尾指针**，而注释声称是 circular queue | `sched.c:3-5,78-79`；`percpu.h:21` |
| C63 | ~~`sched_maybe_preempt` 只在 LAPIC tick 和 IPI 路径被调；AP 无定时器~~ | `arch/cpu.c` `pit_handler`；**已修复（2026-10-01）**：AP 在 `ap_main` 武装自己的 LAPIC 周期 tick（共享 BSP 标定的 period），见 4.6 |
| C64 | `find_task(1)` 可能是 NULL（pid 1 已退出），`parent` 变 NULL（见 4.5） | `task.c:223` |

#### 6.5 引导与架构

| # | 问题 | 位置 |
|---|---|---|
| C65 | **`EFER.NXE` / `CR0.WP` / `CR4.WP` / `SMEP` / `SMAP` 全部未置位**（详见 4.2） | `entry64.S:70,122,128,261`；`trampoline.S:41-76`；`smp.cpp:199-206` |
| C66 | `boot_collect_e820` 把 `grub_params_pa` 强转后**无任何合理性校验**就解引用 | `main.c:60-67` |
| C67 | `boot_read_cmdline` 在 QEMU 无 `-append` 时读物理 `0x20000`，靠"那区域是零"侥幸 | `main.c:199-204` |
| C68 | GRUB `linux` 路径**没有自动化测试**（`make run` 与 `make smoke` 只走 QEMU 路径）。**半条（2026-10-06）**：已落地 **`make imageiso`**（`Makefile:174`，暂存树 `build/iso/`、产物 `build/lnxrm.iso`、`grub.cfg` 由 Makefile 生成），手工实测三条子路径全过——`gfxpayload=1280x720x32`（32 bpp / pitch 5120）、无 fb、以及错写法 `1280x720,32`（只给 24 bpp）分别 1546/1546、1530/1530、1546/1546 checks，无三重故障，且 `mode=0x0` 证明读的是 `boot_params.screen_info` 而非 `0x8C00`。**仍未做**：纳入 smoke。命令见 `BUILD.md` §8.2 | ~~`main.c:50-58`~~ → `main.c:57,97,199`；`Makefile:165-202` |
| C69 | `boot` 只用**单个最大** E820 区域，忽略其他 RAM 区域 → 多段/非连续内存布局下浪费巨大。**2026-10-06 起每个区域还要先按 1 GiB 窗口裁剪**（`hi = MIN(hi, PMM_WINDOW_TOP)`），裁完一个不剩就 panic | `pmm.c:95-109,122-124` |
| C70 | 固定物理地址 0x50000–0x57000（PML4/PDPT/PD/PT）**硬编码且从不校验是否与 E820 冲突** | `entry64.S:4-10` |
| C71 | 见 6.4（内存子系统） | — |
| C72 | **`0x6F00` 哨兵按 `u64` 读，两个写入者却只写 DWORD** → 高 4 字节是那块内存的残留（真机上是固件/前一个 OS 留下的垃圾），拼进指针就是野指针解引用。QEMU 下该处恰好为 0，所以从不暴露。**已修复（2026-10-06）**：三处读点全部改 `*(volatile u32 *)0x6F00`，并在注释里写明 "DWORD, see boot_collect_e820()" 防回退 | `main.c:57,97,199`；写入者 `setup.asm:556`、`entry64.S:52` |

---

### 7. D 类：契约漂移

这一类单列，因为它成本极低但**危害最大**：一个工程师如果信任这些注释和 ABI，会做出错误的修改。**注释说谎比没有注释更危险。**

| 承诺 | 位置 | 实际 |
|---|---|---|
| `fat32_journal` "provides atomic operations for FAT32 updates" | `fat32_journal.c:1-2` | 后像 + 无落盘 + 无法回滚的重复写回（见 C13） |
| `struct cpu_info` 注释 "circular run queue" | `sched.c:3-5` | LIFO（见 C62） |
| ✅ **已修复（2026-10-01）**：`DEV_VMA` 标为 "PD_HI[98]"、`FIXMAP_VA` 标为 "slot 97" | `types.h:37-38,41-42` | 实际是 **128** 和 **112**；`vmm.c` 跳过的 97 是死项。**处理**：`FIXMAP_VA` 整体删除、注释改为 128、`vmm.c` 不再跳 97/112 |
| ✅ **已修复（2026-10-01）**：`mm.h` 重复 "slot 98" | `include/mm/mm.h:45` | 同上，随 `FIXMAP_VA` 删除一并清理，注释改为 PD slot 128 |
| ✅ **已修复（2026-10-01）**：`mm.h` 注释 "remote PTE editing via the fixmap slot" | `include/mm/mm.h:42-43` | **全树零引用**——最后一个使用者（`signal.c` 的单页临时别名）已随信号 trampoline 一起删除。**处理**：注释与宏直接删除 |
| `smp.rs:23` 字段名 `ap_main_phys` | `kernel/rust/smp.rs:23` | 存的是**虚拟**地址（`smp.cpp:75-80` 有注释说明） |
| 6 项硬编码容量常量无动态分配 | `NR_TASKS 64`、`NR_FDS 16`、`CACHE_ENTRIES 64`、`MAX_MOUNTS 4`、`MAX_CPUS 8`、`blk_list[8]` | 编译期固定；`task_alloc_slot` 返回 NULL 尚可，但 `blk_register` 有 off-by-one（C28） |

**共同模式：意图写进注释，实现漂移了，然后没有任何东西会发现——因为没有测试会失败。** 所以 B 类（5.1）是 D 类的根因。**修完 D 类而不修 B 类，三个月后 D 类会重新长出来。**

---

### 8. 差距量化

0 = 缺失，1 = 骨架，2 = 部分可用有明显缺陷，3 = 可用且语义正确，4 = 稳健，5 = 可投产。

| 维度 | 现状 | 目标 | 主要差距 |
|---|---|---|---|
| 引导 / 启动 | **4** | 4 | GRUB 路径只有 2026-10-06 手工实测、未纳入 smoke（C68）；硬编码 phys addr（C70）；只用一个 E820 区（C69） |
| 物理内存管理 | **3** | 4 | 无回收、无 swap、无 OOM killer；**按进程记账已有**（`task.mem_pages`/`mem_peak`，T-032）；位图位置是隐式依赖（C57）；**管理窗口只有 1 GiB，>1 GiB 的内存不参与分配**（C71 修复的代价） |
| 虚拟内存 / 地址空间 | **3** | 4 | 三槽设计优雅；但无 `mmap`、无 WP 语义、隔离只有 1 bit 深、无 PTI |
| 进程管理 | **3** | 4 | fork/exec/wait/brk/信号齐全；但无 pid 对象（4.5）；**内存配额已有**（4.3，T-032），CPU 时间限制仍无 |
| 调度 | **3** | 4 | 实为 LIFO（C62）、无亲和性；**已有按空闲/队列长度的负载均衡与本地 `need_resched`**，每 CPU 本地 tick（4.6，2026-10-01 修） |
| SMP | **3** | 4 | 2 CPU 能起；BSP/AP 各有本地 LAPIC tick、共享标定、TSC 实测 `mdelay`（4.6，2026-10-01 修）；fd/块缓存并发保护仍缺（6.1） |
| 系统调用 / ABI | **3** | 4 | 31 个编号全部实现、用户指针校验良好；但缺 `mmap`/时间/`stat` |
| 信号 | **3** | 4 | 投递 + 默认处置 + 用户态 handler（`sigaction`/`sigprocmask`/`sigreturn` 已实现）；仍无可靠的 CS/SS 校验与嵌套限制 |
| **并发正确性** | **2** | 4 | C1（终端锁）、C9（任务表锁）**已修**；块缓存跨核 I/O（C7/C10）、多等待者（C5）仍无保护（6.1） |
| 设备驱动 | **2** | 4 | AHCI 三个致命问题（C48-50）、IDE 28 位、仲裁先到先得、无中断 I/O |
| 文件系统 | **2** | 4 | 能挂载能读写；但非崩溃安全、缓存坏、无环检测、多处泄漏（6.2） |
| 图形 / 显示 | **3** | 3 | 帧缓冲控制台 + 23–27 五个绘图系统调用（裁剪保护）；**绘图入口两道门**（`CAP_FB` 能力 + 单 pid 归属，T-033，2026-10-05）；VGA 文本回退正常 |
| 用户态 | **2** | 3 | 16 个程序（`init`、`sh` + 14 个命令）；但无动态链接可能（受 `mmap` 缺失制约） |
| **测试** | **0** | 3 | **完全缺失**。这是 B 类其余部分的根因 |
| **可观测性** | **0** | 3 | **完全缺失**。无 debug syscall、无 `/proc`、无 backtrace、无 loglevel |
| **安全模型** | **0** | 4 | **完全缺失**（2026-10-01 记）。此后特权（T-030/T-031 的 `CAP_*`）、NXE/WP/SMEP/SMAP、内存上限（T-032 的 64 MiB 配额）、**画屏归属**（T-033 的单 owner + 两道门，2026-10-05）陆续落地，**此格分数未随之重算** |
| 硬件支持广度 | **2** | 3 | 仅 x86-64、QEMU/PC 中心、IDE 28 位、TSC 单次标定无校验 |

**综合：39/85 ≈ 2.2**（现状，2026-10-01 第二轮后；此前 36/85）对 **67/85 ≈ 3.9**（L3 稳健级目标）。

**注意权重的分布**：综合分 2.1 掩盖了一个极不均衡的分布——有 4 个维度是 3–4 分（三槽地址空间、信号、启动、图形内核侧），有 4 个维度是 **0 分**（测试、可观测性、安全模型，以及受其制约的并发正确性 1 分）。**提升空间的 80% 集中在 5 个维度上，而不是均匀分布在 17 个维度上。**

---

### 9. 路线图

按"投入产出比 × 依赖顺序"排序。粗略人日估计，仅供参考。

#### 阶段 0：止血（~2 人日）—— 修会挂死整机的问题

| 项 | 内容 | 成本 |
|---|---|---|
| ~~C1~~ | ~~`console_write` 走 `print_lock`~~（**已做，2026-10-01**：`console_out_lock` 共用） | 1 行 |
| C11 | FAT 链遍历加迭代上限 + 环检测（`fat_next_cluster` 加 `>= max_cluster` 校验，遍历加计数器上限） | ~20 行 |
| ~~C12~~ | ~~`kmalloc` 失败返回 NULL（保留 `panic` 为可选项）并修正所有调用点~~（**已做，2026-10-03**：19 处调用点核对、补 9 处检查，同链路的 `ensure_table`/`vmm_new_user_aspace`/`signal_map_restorer` panic 与 `sys_brk` 泄漏一并修掉，见第三部分 T-004） | ~30 行 |
| ~~C14~~ | ~~检查 journal `add`/`begin` 返回值~~（**已失效**：journal 已删除） | — |
| ~~C71~~ | ~~PMM 管理窗口 > 恒等映射范围 → 启动即三重故障复位~~（**已做，2026-10-06**：窗口钳到 `PMM_WINDOW_TOP` 1 GiB + IDT/GDT 提前到 `start_kernel` 头四步 + 无可用 RAM 改 panic；真机"闪几行就重启"的根因，见 `CODEBASE.md` §22.6） | ~30 行 |
| ~~C72~~ | ~~`0x6F00` 哨兵按 `u64` 读但只写了 DWORD~~（**已做，2026-10-06**：三处读点改 `*(u32*)`；QEMU 下残留恰为 0，只有真机会踩） | 3 行 |
| ~~C24~~ | ~~`blk_cache` 的 `used` 改成访问时清零~~（**已做，2026-10-05**：没按这个便宜办法做——清零只换来时钟轮转，还是没有"最近"信息；改成时间戳 LRU 并删掉 `used`，见第三部分 T-007） | ~50 行（含计数与内省） |
| ~~C26~~ | ~~FAT 读路径改走块缓存~~（**已做，2026-10-05**：读路径已全走缓存；FAT 区保持「读内存 + 直写」，并用 `test_fat_region_is_never_cached` 守住不变式，见第三部分 T-008） | ~15 行 + 3 例测试 |
| B13 | `panic` 绕过 `print_lock`（`cli` + 裸写） | ~10 行 |

**验收**：写一个带自引用簇的 FAT 镜像，挂载后不挂死；双核下两个 shell 交替输出 1000 行不撕裂。

#### 阶段 1：安全边界（~3 人日）—— 最高投入产出比

| 项 | 内容 | 成本 |
|---|---|---|
| C65 | 置 `EFER.NXE` + `CR0.WP` + `CR4.WP` + `SMEP`，AP 同步设置 | **~15 行汇编** |
| — | 验证只读 text 段确实拒绝写入（此时 `vmm_map_user` 的 `writable` 才有意义） | 测试 |
| 4.1 | `struct cred` + 三类能力 + `kill`/`diskinfo`/`fdisk`/`ps` 检查点 | ~250 行 |
| ~~4.3~~ | ~~`brk` 配额 + 记账，超限返回 `ENOMEM`~~（**已做，2026-10-05**：64 MiB/user、root 无上限、`ps` 加 `MEM`/`PEAK` 列，见第三部分 T-032；CPU 时间限制仍未做） | ~100 行 |

**验收**：`sh` 里跑一个野程序 → 只杀野程序，内核不 panic；野程序改自己的只读 text 段 → `SIGSEGV`；`kill -1` 从普通用户被拒。

#### 阶段 2：可验证性（~8 人日）—— 决定后续所有工作的可行性

| 项 | 内容 | 成本 |
|---|---|---|
| 5.1 L-单元 | `assert()` 基础设施 + 首批用例 | ~1 天 |
| 5.1 L-自测 | 启动时跑内核态用例，打印 `[selftest] N/M pass`，失败即停 | ~4 天 |
| 5.1 L-CI | QEMU 无头 + 串口日志断言 + 退出码 | ~2 天 |
| 5.1 L-内存档位 | smoke 加 `-m 64/256/2048/4096` 矩阵跑（**C71 就是 `-m 256` 单一档位漏掉的**，2026-10-06 手工补测过一次，仍未自动化） | ~0.5 天 |
| 5.2 | `SYS_debug_dump` + `/proc/meminfo` `/proc/uptime` / `/proc/self/maps` | ~3 天 |
| 5.2 | `BUG_ON` / `WARN_ON` 分级断言（页表 walk、PMM 不变量、分配释放配对） | ~2 天 |

**验收**：`make ci` 在每次改动后自动验证；故意引入一个 bug 会被自测抓到。

**这一步必须紧跟阶段 0/1。** 没有它，后面每一次修改都是盲改。

#### 阶段 3：修契约漂移（~1 人日）

- ✅ 修正 `types.h` / `mm.h` 的槽号（98→128、97→112）——**已随 FIXMAP_VA 清理完成（2026-10-01）**
- ✅ 修正 `mm.h` 的 "remote PTE editing" 注释（`FIXMAP_VA` 已零引用）——**已选择直接删掉宏与 fixmap 概念（2026-10-01）**
- 修正 `sched.c` 的 "circular" 注释
- 修正 `fat32_journal` 的头注释，或直接重命名为 `fat32_writeback`
- **文档同步**：`docs/README.md`、`docs/CODEBASE.md`、`docs/GAP_ANALYSIS.md`、
  本文第三部分中任何提到已删代码的句子都要改（本轮就是这么做的）
- **建立规约**：任何写进注释和 ABI 的行为，必须有一个断言或测试守着（依赖阶段 2）

**验收**：`grep -rn "TODO\|FIXME\|not implemented" docs/ include/` 的每一项都有归属。

#### 阶段 4：身份与生命周期（~5 人日）

- 4.5 `struct pid` + 世代号，或至少 `generation` 字段 + 回收时清理 `parent` 指针
- ~~6.1 C9 `find_task` / `task_iter` 加 `task_table_lock`~~（`find_task` 已加锁，`task_iter` 有意保持无锁——2026-10-01）
- ~~6.1 C8 `sys_fork` fd 表~~（判定为误报：单线程进程模型下无并发 `close`——2026-10-01）
- 6.1 C5 `con_waiter` 改链表

**验收**：压力测试下并发 fork+close+kill 不产生悬垂指针（用 ASan 式故障注入验证）。

#### 阶段 5：文件系统硬化（~15 人日）

- C29 统一 `bytes_per_sector` 缓冲为 `kmalloc` 分配
- C16 修 `nclus` 截断（改 u64 + 边界检查）
- ~~C15 检查所有 `dev->read` 返回值~~（**已做，2026-10-05**：`blk_io_*` 成为唯一调用点 + 故障注入 `blk_inject_io()`，见第三部分 T-005）
- C18 镜 FAT + FSINFO 一致性
- C19/C20 真正的 `O_TRUNC` + 写失败回滚
- C30 `fat_dir_iter` 加游标（真正的 getdents 语义）
- C40 修 vnode 泄漏；C41 强制访问模式
- ~~C24/C25 重写块缓存（正确的 LRU + 读失败路径）~~（**已做，2026-10-05**：时间戳 LRU + 读失败作废 victim 槽，见第三部分 T-007；C26 同日一并修掉，见 T-008）
- **要么真正实现日志（落盘 + replay），要么删掉 `fat32_journal` 并诚实命名为写回缓冲**

**验收**：FAT32 官方 `fsck.vfat` 报告干净；`dd` + 随机断电模拟后能恢复或至少不返回错误数据。

#### 阶段 6：驱动（~10 人日）

- C48 AHCI `PG_PCD` + `clflush`/fence
- C49 AHCI 中断路径（`P_IE`、`HBA.IS` 排空、ISR）
- ~~C50 检查 `PxTFD.ERR`~~ **✅ 2026-10-05 T-080**
- C53 IDE LBA48 命令（**容量字段已修 + 超界即失败，`0x24`/`0x25` 命令未实现**）
- ~~C54 正确的 IDENTIFY 轮询~~ **✅ 2026-10-05 T-080**
- 4.10 块设备队列 + 明确的设备选择策略（修 C28 先到先得）—— **设备选择部分 2026-10-05 T-081 落地**，队列与分区 `blk_register` 仍缺
- ~~C56 PCI 桥递归深度限制~~（**半条，2026-10-06**：visited + 深度 16 已加；0xCF8/0xCFC 的 bus 0 限制仍在）

**或者**：如果决定只保留 IDE，把 AHCI 明确标为"实验性"并从默认启动路径移除。**这也是一个合格的选择——诚实的功能边界优于半成品。**

#### 阶段 7：能力扩展（~30 人日）

按依赖顺序：`mmap`/`munmap`/`mprotect` → 缺页分发 → 用户栈增长 → COW → `gettimeofday`（含 per-CPU 时钟 + TSC 标定）→ `stat` → `pipe` → `/proc` 完善。

**这一步是最大的投入，也是最能解锁生态的一步。** 在此之前，任何第三方软件都跑不了这个内核。

#### 阶段 8：收敛（~10 人日）

- 修 C62/C63：真正的轮转 + AP 时钟
- 4.8 剩余 IPC
- 4.11 日志与崩溃转储完整化
- ~~GRUB 路径实测（C68）~~ **✅ 2026-10-06 手工实测通过**（两条子路径 + framebuffer 分支）；**纳入 smoke 自动化仍未做**
- `docs/` 四份文档随代码同步（建立规约：改代码必须改文档）

---

### 10. 不建议做的事

明确列出**不应该做**的方向，避免路线图跑偏：

- ❌ **不要实现 ext2/ext4/NTFS。** FAT32 已经够这个内核用了。崩溃一致性（C13-23）比换一个文件系统重要得多。
- ❌ **不要追求 POSIX 完整兼容。** 当前 31 个已实现的系统调用已经能支撑一个可用的 shell。追 POSIX 会陷入无穷无尽的语义细节，而真正的问题是安全模型和测试。
- ❌ **不要再加新子系统。** 8 个 60% 的子系统不如 3 个 100% 的子系统。当前最大风险是**宽度**，不是深度。
- ❌ **不要在阶段 2（可验证性）之前做阶段 4-8。** 没有测试的并发修复和 MMU 改动会引入比现在更多的 bug。
- ❌ **不要为了"看起来像 Linux"而加 Linux 的机制。** KPTI、eBPF、cgroup、namespace 都不是这个体量的目标。**加机制前先问：它解决的是这个内核的实际问题吗？**

---

### 11. 一页总结

**已有的**：三槽地址空间模型（真正优雅的设计）、LMA/VMA 单一连续段（省掉一整类链接问题）、双启动路径 + 哨兵机制、帧缓冲控制台 + VGA 文本回退（591 行，层次清晰）、会引用历史 bug 的注释（真实调试的痕迹）。**10,151 行内核做到这个功能密度并真的能跑，是可以引以为傲的。**

**最缺的**：
1. **特权模型**（全树零命中）—— 这是"作品"和"内核"的分水岭
2. **CPU 安全特性**（NXE/WP/SMEP 全关）—— **15 行汇编**，投入产出比最高
3. **测试与可观测性**（全零）—— B 类其余部分和 D 类的共同根因
4. **并发正确性**（1/5）—— 终端、fd、块缓存、任务表都缺保护
5. **崩溃一致性**（journal 名不符实，FAT 单点可挂死整机）

**最重要的判断**：这个内核缺的不是设计品味，也不是补丁，而是**工程纪律**。前者它已经有了（三槽地址空间就是明证），后者它一样都没有。**从"能跑"到"合格"的距离，几乎全部由"能不能验证自己"决定。**

**如果只做一件事**：置 `EFER.NXE` + `CR0.WP` + `CR4.WP` + `SMEP`。十五行汇编，让这个内核第一次拥有一个真实的、硬件强制的用户/内核边界。

---

### 附录：核实方法与数据

**核实方式**
- 全量阅读 11,068 行源码（`wc -l` 统计 `.c/.h/.cpp/.hpp/.rs/.S/.asm/.ld/.def`，另有 156 行 Python 构建脚本）
- `make clean && make` 从零构建：**零警告通过**
- QEMU 实机启动验证（`qemu-system-x86_64 -m 256 -smp 2`，抓取串口日志）
- 对以下项做了针对性 grep 确认：CPU 控制寄存器写入点（`cr0`/`cr4`/`EFER`/`SMEP`/`SMAP`/`NXE`）、特权概念（`uid`/`capability`/`RLIMIT`）、测试设施（`tests`/CI/断言）、时间系统调用、可观测性设施（`loglevel`/`dmesg`/`backtrace`）、`mktime` 类调用

**关键实测数据**

| 项 | 值 | 来源 |
|---|---|---|
| 总行数 | 11,068（内核 10,151 + 用户态 761 + 脚本 156） | `wc -l` |
| 系统调用数 | 31 个编号（0–30，连续无空洞），全部实现 | `include/abi/lnxrm_syscalls.def` |
| 用户态二进制 | 17 | `ls build/usr/` |
| 测试 | **0** | 全树搜索 |
| CI 配置 | **0** | 全树搜索 |
| 特权概念 | **0** | `grep -iE 'uid\|gid\|capability\|RLIMIT\|privilege'` |
| 时间类系统调用 | **0** | `grep -E 'gettimeofday\|clock_gettime'` |
| 可观测性设施 | **0** | `grep -E 'loglevel\|dmesg\|backtrace'` |
| `mmap` / `munmap` / `mprotect` | **0** | 系统调用清单 |
| bzImage 大小 | 254,920 字节（启动消息扩展后；SMP 轮 253,448，第一轮 250,600） | `make` 输出 |
| 构建警告 | 0 | `make clean && make` |

**未在本轮核实的事项**（可能存在但未确认）
- GRUB `linux` 启动路径（需要实体机或 GRUB 环境）
- 512B 扇区以外的 `bytes_per_sector` 布局（需特制镜像）
- >2 TiB 设备的 LBA48 行为
- 多 CPU 满载下的调度质量（需要压力测试负载）
- 长时间运行的内存碎片行为

---

*文档版本 1.1 · 所有行号引用基于死代码删除后的代码状态 · 编号 C1–C70 是稳定 ID，失效条目留空不重排*

---

## 第二部分 · 评审记录

本部分是 2026-10-01 两轮实测工作的记录：第一轮是完整评审（这个内核算不算
"真内核"、距离一个好的内核还差多远、安全面上**做对了什么 / 不安全在哪**，
§12–§15）；第二轮是随后完成的 SMP/AP 完善，以及过程中挖出的一个**潜伏已久的
时钟根因**（§16）。

评审方法是实测与交叉核验，不是静态推断：当日完成了 `make -j4`
（`-Wall -Wextra` 零警告）、QEMU 双核启动、`systest` 全量系统调用测试，以及
"源码调用图 + 段字节扫描 + 反汇编 + `.o` 未定义符号"四路核验。所有行号与
数字都是当日实测值。

与本文另外两部分的关系：

* 第一部分（差距分析）—— 66 条缺陷的静态编目（A/B/C/D 四类）。本部分不重复
  它的编号内容，引用时标注如 ``C11``；已修复项在该表中标注状态，编号保留。
* 第三部分（行动计划，§19 起）—— 任务卡与执行顺序（T-000…）。本部分的建议映射到该处。
* 结论冲突时以本部分为准（更新，且带实测证据）；§17 列出已被实测证伪的
  旧文档条目。


### 12. 结论：它算一个真内核吗

**算。** 它不是"会打印字符串的引导程序"，也不是玩具级单任务内核——它有完整的
特权级边界、物理/虚拟内存两层管理、多核 SMP、抢占式调度、真实文件系统驱动和
一个可用的用户态。按第一部分 §1 的 L0–L3 分级，它稳定落在 **L1 上沿**：
边界清晰、机制基本完整，但安全模型与可验证性仍是 L0。

#### 12.1 实测证据（2026-10-01）

| 证据 | 结果 |
|---|---|
| `make -j4` | 零错误、零警告（`-Wall -Wextra`，C/C++/ASM/Rust 四语言） |
| 产物 | `bzImage` 252,616 字节、text 段 249,760 字节（2026-10-01 起点）→ 253,448（第二轮 SMP 完善）→ 254,920（§16.4 启动消息扩展）→ 158,968（`--gc-sections`）→ 160,024（`.image` 157,824，T-033）→ 160,856（`.image` 158,656）**，2026-10-05 T-080 重编** → 161,944（FAT32 大小写修复）→ **162,200（`.image` 160,000），2026-10-06 C71/C72 重编** |
| 启动 | QEMU `-m 256 -smp 2`：BSP + 1 个 AP 上线，串口 + framebuffer 双控制台，FAT32 挂载，pid 1 → shell |
| `systest` | 文件系统错误码路径、`fb_*` 图形调用、未知 syscall 返回 `ENOSYS(-38)`，**故意访问未映射页 → SIGSEGV → 进程被杀（返回 -11），shell 与内核存活** |
| 代码规模 | 内核（`kernel/ arch/ include/`，含 Rust/ASM 与 `ktest/` 自测）15,895 行；用户态 `init/`+`usr/` 1,768 行、16 个程序（2026-10-05 复测；T-080 后 +143）。**2026-10-06 再测：内核 16,029 行、合计 17,797 行**（+43 FAT32 修复、+91 真机三重故障修复，口径见 `CODEBASE.md` §35） |
| syscall | `lnxrm_syscalls.def` 32 项（0–31 连续无空洞，含 T-031 新增的 `setuid`），`syscall.c` 32 个 `case` 全部落地 |

"故意段错误被隔离"是关键证据：它证明 **ring-0/ring-3 边界不是纸面存在**——
用户程序崩溃不会带走内核。

#### 12.2 准确的能力清单

已实现（当日核验）：

* 启动：自实现 real-mode setup、VBE 分辨率级联选型、符合 Linux boot protocol 2.08
* 内存：buddy 物理页分配器、分离空闲链内核堆、三槽 PML4 用户地址空间
* 多核：INIT-SIPI-SIPI 拉起 AP，每 CPU 独立 GDT/IDT/TSS/运行队列
* 进程：`fork / execve / wait4 / exit / brk / kill / ps`，64 槽任务表
* 信号：`sigaction / sigprocmask / sigreturn`，**用户态信号处理器已实现并实测**
  （`systest` 捕获 SIGSEGV 成功）
* 文件：VFS + FAT32 + 块缓存，`open/read/write/lseek/close/getdent/dup2/mkdir/unlink/rmdir/rename`
* 图形：`fb_info/fb_clear/fb_fill/fb_char/fb_puts` 五个帧缓冲调用，ANSI 转义
* 用户态：自带 crt0 与迷你 libc，16 个静态 ELF64 程序

#### 12.3 诚实的边界：它不是什么

* 不是多用户系统：**没有 uid/gid/capability/RLIMIT 的任何概念**（全树 grep 零
  命中），所有进程都是 root（详见 §14.3-N1）。
* 不是通用 Linux 替代：31 个 syscall vs 常用程序期望的 300+；无
  `pipe/mmap/stat/chdir`。
* 不是可跑不可信代码的平台：无权限模型 + 无 CPU 安全特性（§14.3-N3）+ 一个坏
  磁盘可挂死整机（§14.3-N2）。
* 不是崩溃安全的存储：FAT32 无日志，写到一半断电即损坏。


### 13. 距离一个好的内核还差多远

"好"取第一部分 §1 的 **L2（工程可用）**定义：边界真实生效、失败路径
正确、有基本测试。lnxrm 到 L2 的距离可以分成三条战线：

```
可验证性（前置）───── 决定其他工作能否安全推进，约 1 周
功能闭环（P0）─────── 让系统"可用"，约 1 周
内存现代化（P1）───── 让系统"像现代内核"，约 2–4 周
```

#### 13.1 前置：可验证性（不修它，后面都不可靠）

全树零测试、零 CI，唯一验证回路是"启动看串口"。已知 66 条缺陷**全部是读出来的，
没有一条是跑出来的**。任何改动（尤其并发与 MMU）在没有门控的情况下都是盲飞。

→ 对应第三部分 Step 0（T-000 启动冒烟测试，0.5 天）与 Step 2
（内核自测框架）。这是唯一的硬前置。

#### 13.2 P0 · 功能闭环 —— "能用"的最小集合

现状实测：shell 只有 `>` 重定向，**没有管道**；`init` 只 `waitpid` 一次就进入
无限睡眠（`usr/init.c:47`），**孤儿进程无人回收**；没有时间与目录状态类 syscall。

| # | 缺口 | 现状证据 | 不做的后果 | 参考任务卡 |
|---|---|---|---|---|
| P0-1 | `pipe` + shell `\|` | def 表无 `pipe`；`usr/sh.c` 无 `\|` 处理 | 无法组合工具，shell 实用性锁死在玩具档 | 需新任务卡 |
| P0-2 | init 回收孤儿 | `usr/init.c:47` 只等 shell 一次；僵尸唯一释放点是 `sys_waitpid`（`kernel/task.c:310`）；`NR_TASKS = 64` | 孤儿僵尸**永久占槽**，63 个之后 `fork` 全系统失败——资源耗尽漏洞，不只是整洁问题 | 需新任务卡（小） |
| P0-3 | `gettimeofday` / `clock_gettime` | def 表无 | 耗时/日志/超时逻辑无时间源 | T-098（依赖的 per-CPU 时钟源已就绪，见 §16/T-097） |
| P0-4 | `stat` / `chdir` / `getcwd` / `umask` | def 表无；无进程工作目录概念 | 现实程序第一行就失败；相对路径全靠用户态硬拼 | 中等 |
| P0-5 | 失败路径回归测试 | 无测试 | P0-1/2 改动无法验证 | T-010…T-013 |

**缺失 syscall 全表**（当日逐条 grep 核验，31 个已有编号之外的高价值项）：

```
pipe  dup  fcntl  stat  fstat  lstat  chdir  getcwd  umask
chmod  link  truncate  time  gettimeofday  clock_gettime
alarm  pause  select  poll  mmap  munmap  mprotect
```

已有且容易被旧文档误报为"缺失"的：`lseek(4)`、`uname(15)`、`dup2(7)`、
`getdent(6)`、`fb_*(23–27)` —— 见 §17。

**P0 合计量级：约 1 周**（pipe + init 回收 1–2 天；时间调用半天；`stat/chdir`
约一天）。

#### 13.3 P1 · 内存管理现代化

| # | 缺口 | 为什么现在不行 |
|---|---|---|
| P1-1 | 写时复制 fork | `sys_fork` 深拷贝页表，shell 每条命令都付全量地址空间拷贝成本 |
| P1-2 | 需求分页 | `elf_load` 加载即全部分配（`kernel/elf.c`），大二进制启动慢且浪费物理页 |
| P1-3 | `mmap/munmap` | 无堆外映射能力；也是 P1-1/P1-2 的自然载体——三者必须一起设计（`isr_common` 要从"异常即杀进程"演进为"缺页→尝试修复→修不了才杀"） |
| P1-4 | OOM 策略 | **按进程记账 + 用户态配额已有（T-032，2026-10-05）**；但内核侧 `kmalloc` 失败仍是"分配失败就死"，无换出、无受害者选择（第一部分 §4.3） |

→ 第一部分 §4.3/§4.4；第三部分 Step 4（T-032 brk 配额，
**2026-10-05 已完成**，剩下的是 OOM 策略）
是入口。**量级：2–4 周**，且必须在 §13.1 测试就位后进行。

#### 13.4 P2 · 系统性能力（按需，不建议现在做）

| 项 | 现状 |
|---|---|
| 定时信号 | `alarm`/`setitimer` 缺失；tick 由 BSP 的 LAPIC 定时器驱动 `jiffies`（PIT 只做标定窗口，见 §16.2） |
| 网络栈 | 完全没有（无 socket 类 syscall、无协议栈） |
| 调度策略 | 纯 RR + 每 CPU 运行队列，无优先级、无 nice、无实时类 |
| 内核态抢占 | `sched_maybe_preempt` 只在 `(cs&3)==3` 或 idle 时调度（`kernel/sched.c:188-196`）——**内核态任务不可抢占**，长临界区拖住整核 |
| 共享内存/线程 | 无 `clone`、无 shm，进程模型是单线程假设 |
| 特权模型 | 见 §14.3-N1，它同时是安全问题与功能问题 |

#### 13.5 到 L2 的关键判断

lnxrm 到"工程可用"**不缺新机制，缺的是三样旧东西**：

1. **测试**（0 部分）—— 决定其他一切能否推进
2. **权限模型**（0 行代码）—— 决定能否跑任何不完全信任的程序
3. **失败路径**（散布 66 条缺陷中）—— 决定它会不会在你没犯错时自己坏

功能上（P0）距离"日常可用"只剩约一周工作量；工程上（L2）距离主要是测试与
权限两块，各是天/周量级而非月量级。**主要矛盾不是功能。**


### 14. 安全审查

#### 14.1 方法

四路交叉核验，取交集为准：`.o` 未定义符号差集、反汇编操作数扫描、段字节地址
扫描、源码调用图。对用户可控输入的每条路径（syscall 指针、ELF 镜像、磁盘镜像、
信号帧）单独走查"输入从哪来 → 谁校验 → 溢出/越界能到哪"。

#### 14.2 做对的（正面清单）

本节是本次评审的新增内容——`GAP_ANALYSIS.md` 以记录缺口为主，而 lnxrm 在
**经典提权攻击面**上的实现比多数 hobby 内核认真：

| # | 攻击面 | 实现 | 位置 |
|---|---|---|---|
| G1 | **`sigreturn` 伪造特权级**（从用户栈恢复帧 → 自造 `CS` 回 ring-0，这类漏洞在真实内核里出过多次） | 恢复的是内核保存的 `t->saved_tf`，**不读用户栈**；且要求 `sig_in_handler` 为真 | `kernel/signal.c` `sys_sigreturn()` |
| G2 | **恶意 ELF 镜像** | 七项校验：magic/`ELFCLASS64`、仅 `ET_EXEC`、ph 表整体在镜像内（含 `e_phnum*sizeof` 乘法溢出）、`p_offset+p_filesz ≤ imgsize`、`p_filesz ≤ p_memsz`、`p_vaddr` 落用户区且 `vaddr+memsz` 不回绕、页清零后再拷贝 | `kernel/elf.c` `elf_load()` |
| G3 | **syscall 用户指针** | 分发层传裸指针，责任落在各 `sys_*`；实测 `read/write/getdent/uname/sigaction/sigprocmask/ps/diskinfo/fb_*` 全部经 `copy_from/to_user` → `user_ptr_ok`（用户地址范围检查 **+ 逐页 present 检查**，防"校验时已映射、使用时被换出"类竞态） | `kernel/task.c` `user_ptr_ok()`、`kernel/fs/vfs.c:359-395`、`kernel/syscall.c` |
| G4 | **`execve` 二维指针**（argv/envp 是指针数组，每个元素再指向字符串） | 数组元素逐个 `copy_from_user`，每串走逐字节验证的 `copy_user_str`，硬上限 16×64 字节，路径 128 字节 | `kernel/task.c` `sys_execve()` |
| G5 | **栈上缓冲** | 最大缓冲 4KB（`sys_read/write` 的 `kbuf`），内核栈 `KSTACK_SIZE = 32768`——4/32 有余量 | `include/sys/sched.h:97`、`kernel/fs/vfs.c:366,377` |
| G6 | **格式化串** | `kprintf` 格式串全部是字面量（grep 无一例变量格式串），仅支持 `d u x p s c %`——**没有 `%n` 写原语** | `kernel/print.c:284-312` |
| G7 | **用户异常不击穿内核** | 按 `cs&3` 判别来源，用户态异常只杀进程；`systest` 故意 `#PF` 实测：进程 -11 退出，shell 存活 | `kernel/isr.c` |
| G8 | **未知 syscall** | 表外编号回 `ENOSYS` 并打日志，不崩 | `kernel/syscall.c` `default:` |
| G9 | **畸形 FAT BPB 挂载** | 校验 `bytes_per_sector`/`num_fats`/`root_cluster` 上下界，`totsec > dev->num_sectors` 时裁剪，FAT 表容量与 `max_cluster` 对齐（防簇号 OOB） | `kernel/fs/fat32/fat32.c` `fat_mount()` |
| G10 | **读写偏移** | `off ≥ size` 截零、`off+n > size` 钳制、单次 1MB 上限、栈缓冲替代共享静态缓冲（注释记录了修过的 SMP 并发缺陷） | `kernel/fs/fat32/fat32_file.c` `fat_read_impl` / `fat_write_impl` |

**G1/G2/G3 是这个规模的内核最常被打穿的三处，这里都做对了。**

#### 14.3 不安全的（负面清单）

按危险度排序。已在 `GAP_ANALYSIS.md` 编目的标注其编号，避免重复统计。

**N1 · 没有特权模型**（第一部分 A1 · 第三部分 T-030/031）

全树 `grep -iE '\buid\b|\bgid\b|capability|RLIMIT|privilege'` **零命中**。后果是
具体的：

* 任何进程可 `kill(1, 9)` 杀掉 init；
* 任何进程可 `kill(-1, sig)` **广播杀死全系统**（`sys_kill` 对 `pid == -1`
  无任何权限检查，`kernel/syscall.c`）；
* 任何进程可写任意文件、`diskinfo` 枚举磁盘、`fb_*` 抢占屏幕。

安全边界是"内核 vs 用户"，但**用户态内部人人平权**。这是设计（与 xv6 同期
形态），但它意味着：现在系统里每一个用户程序 bug 都等价于 root 拿全机。

> **状态更新（2026-10-03，T-030/T-031 分三天落地）**：上面三个具体后果已处理——
> `kill(1, 9)` 被 kxld 规则挡下（回执 `[kxld] denied kill`）、`kill(-1, sig)` 广播
> 现在要 `CAP_KILL`、`diskinfo`/`ps` 要 `CAP_SYS_ADMIN`、`fb_*` 四个绘图调用要
> `CAP_FB`；`struct cred`/`setuid` 让"普通用户"第一次真实存在（新增 `SYS_setuid`
> 编号 31，单向降权）。
>
> **状态更新（2026-10-05）**：内存限额已有（T-032 的 64 MiB/user 配额 + `ps` 的
> `MEM`/`PEAK` 列）；**"抢屏幕"那一半也处理了（T-033）**——`fb_*` 不再是先到先得：
> 第一个成功的绘图调用占住 `fb_owner`，后来者回 `-13`（回执
> `[fb] denied fb_fill: pid 13 holds the surface, pid 14 draws`），持有者退出即释放，
> 拒绝不改归属。**仍未处理**：`write` 到任意文件与终端仍无属主检查（缺 inode
> 属主元数据）、CPU 时间限额与 cgroup。本条从"没有特权模型"降级为
> **"特权模型不完整"**，编号保留。

**N2 · FAT 簇链无环检测 → 内核死循环挂整机**（第一部分 C11 ·
第三部分 T-003）

13 处跟随簇链的循环，其中 8 处**无任何步数上限**（当日逐处核验）：

```
kernel/fs/fat32/fat32_dir.c:47    目录簇扫描（每次路径解析都走）
kernel/fs/fat32/fat32_file.c:95   alloc_chain 扫描既有链
kernel/fs/fat32/fat32_file.c:172  链尾定位
kernel/fs/fat32/fat32_meta.c:53   父目录项插入
kernel/fs/fat32/fat32_meta.c:135  目录项查找
kernel/fs/fat32/fat32_meta.c:284  目录项查找
kernel/fs/fat32/fat32_meta.c:310  rmdir 释放整链
kernel/fs/fat32/fat32_meta.c:337  unlink 释放整链
```

（另 5 处由有限字节预算/计数器兜底：`fat32_file.c:61,66,144,150`、
`fat32_meta.c:180`。行号相比第一部分 旧记录整体左移约 7 行。）

`fat_next_cluster` 只挡**非法簇号**（`clus >= max_cluster` → 返回 EOC），不挡
**合法自环**（如 5→5、5→6→5）。攻击面是外部输入：一张构造好的磁盘镜像，或
运行中坏一个扇区。

放大机制让"死循环"变成"整机挂死"：

1. 路径跑在内核态，而 `sched_maybe_preempt` 只对 ring-3 与 idle 调度
   （`kernel/sched.c:188-196`）→ **该核永远不切换**；
2. FAT 操作经 `FAT_ENTER()` = `spin_lock_irqsave(&fat_fs_lock, …)`
   （`include/fat32_priv.h:24`）→ **持全局锁且关中断**，本核定时器也停了；
3. 其他核来碰文件系统 → 在同一把锁上**关中断空转**（即第一部分 C7）。

结论：**一个坏扇区 = 整机永久挂死**。这是当前唯一能从外部输入直接打挂内核的
路径，也是投入产出比最高的修复（加步数上限约十几行）。

**N3 · CPU 安全特性一个都没开**（第一部分 A2 · 第三部分 T-020…T-023）

当日逐位核验了全部控制寄存器写入点：

| 位 | 作用 | 实测 |
|---|---|---|
| `CR0.WP` (bit 16) | 内核不得写只读页 | **未设**——`setup.asm:475,552`、`entry64.S:126`、`trampoline.S:41,74` 的 `or` 里只有 PE/PG |
| `CR4.SMEP` (bit 20) | 禁 ring-0 执行用户页 | **未设**——所有 CR4 写入只有 PAE (1<<5) 与 PGE (1<<7) |
| `CR4.SMAP` (bit 21) | 禁 ring-0 访问用户数据页 | **未设** |
| `EFER.NXE` (bit 11) | 允许页表 NX 位 | **未设**——只置了 LME (bit 8)；全树 grep `NXE\|_NX\|0x8000000000000000` **零命中**，`vmm_map_user` 写入 `pa \| PG_P \| PG_W? \| PG_U`，从不设 NX |

合起来：**所有内存页可执行、内核可写只读页、ring-3 数据页对 ring-0 无隔离**。
一条用户态 shellcode 可以直接跑在 ring-0 物理内存上——前提是它先进 ring-0，
而 G1/G2/G3 恰好把最常见的进入路径堵住了。这就是"缺缓解不等于立刻被拿"的
准确说法：**边界靠软件校验撑着，硬件缓解一层都没有。**

**N4 · 无 KASLR / 无 KPTI**（第一部分 A2 附带）

内核链接在固定地址 `0xffffffff80100000`（`arch/kernel.ld`），三槽 PML4 设计
使每个用户页表都携带完整内核映射——经典"共享内核地址空间"，速度快但存在
Meltdown 类攻击面。对本体量非必需，但往 L3 走必须摘掉用户态里的内核映射。

**N5 · 栈保护关闭**

`Makefile` 明确带两处 `-fno-stack-protector`。内核栈上目前没有危险的可变长
写入（最大 4KB 定长缓冲，见 G5），但这是一个"靠纪律而非机制"的保证。

**N6 · 内核态不可抢占 + 全局锁跨越设备 I/O**

见 N2 放大机制与第一部分 C7：任何在 `fat_fs_lock` 内变慢或变死的
路径（坏扇区、坏镜像）都会以"关中断自旋"的形式放大到全机。这不是理论问题，
是坏一条 IDE 命令就会触发的路径。

**N7 · 资源耗尽无防线**

* 任务表 64 槽，僵尸唯一释放点是 `sys_waitpid`，而 init 只等一次（§13.2 P0-2）
  → 孤儿积累即 `fork` 全系统失败；
* 无 `RLIMIT`/记账：内存靠 `brk` 无限涨（`brk` 配额 = 第一部分 T-032，未做）；
  `kmalloc` 失败路径**已改为返回 NULL**（第三部分 T-004，2026-10-03——
  但"brk 一直涨到把机器耗干"仍只是**不崩**，没有配额与记账）；
* 无 fd 上限策略以外的防线（`NR_FDS` 固定），`ulimit` 概念不存在。

#### 14.4 定性结论：对谁安全

| 场景 | 结论 |
|---|---|
| 跑自己写的 `usr/` 程序 | **安全**。边界实现（G1–G10）认真程度超过多数 hobby 内核 |
| 跑不完全信任的用户程序 | **不安全**。无权限模型（N1）：一个 bug = root；无硬件缓解（N3） |
| 挂载可信磁盘 | **基本安全**，失败路径有 66 条已知缺陷 |
| 挂载不可信/可能损坏的磁盘 | **不安全**。N2 可整机挂死，且无日志会二次损坏数据 |
| 横向定位 | 与 `xv6` 同档（同无权限、同无 KASLR），但 `sigreturn`/ELF/`copy_user` 三处比部分教程实现更严谨 |

#### 14.5 修复的投入产出比排序

1. **簇链步数上限**（N2，≈十几行）—— 堵掉唯一外部输入可触发的整机挂死
2. **init 孤儿回收**（P0-2，≈10 行）—— 堵掉任务表耗尽
3. **CR0.WP + EFER.NXE + 用户页 NX**（N3 的前三项，≈15 行汇编 + 页表一个位）
   —— 硬件缓解里最便宜的三件，第三部分 T-020/021/022 已有卡片
4. **`kill` 权限最小化**（N1 的止血版：禁杀 init、禁 `kill(-1)`，≈20 行）
   —— 完整 `struct cred` 见 T-030/031
5. **SMEP/SMAP**（N3 余项）—— 必须在 3 之后（先 NX 后 SMEP 顺序有意义）


### 15. 本轮死代码清理记录（已完成，2026-10-01）

审计方法：`.o` 未定义符号差集 + 反汇编操作数扫描 + 段字节地址扫描 + 源码调用图，
四路取交集；字节与反汇编双盲区交集为最终判定。已知假阳性（保留不删）：
`gdt32`/`gdt32_desc`（32 位启动路径链接期相对引用）、`gdt_reload.reload`、
`_GLOBAL_OFFSET_TABLE_`。

| 类别 | 删除内容 | 结果 |
|---|---|---|
| 汇编死函数 | `arch/entry64.S` 的 `tf_exit`、`read_cr3`、`invlpg`（保留 `load_cr3`） | 文件 508 行 |
| 死函数 | `kernel/sched.c` `task_state_name` 及其头文件声明、`extern void tf_exit` | — |
| 死字段 | `struct task *next`（`include/sys/sched.h`） | — |
| Makefile | `FORCE`/`.PHONY: FORCE`、不存在的 `USR_LIB_C := usr/ulib.c`、简化 `UIMG`、`.PHONY` 补 `rust` | — |
| 死宏 50 个 | `framebuffer.h` 16 个 `FB_CLR_*`、`elf.h` `PF_X/PF_R`、`types.h` 4 个、`apic.h` 12 个、`vfs.h` `O_RDWR/O_APPEND`、`mm.h` `PG_PWT`、`fat32_priv.h` 3 个、`boot.h` 4 个、`smp.h` 3 个、`isr.c`/`ide.c`/`vmm.c` 各 1 个 | 见 `CODEBASE.md` 文件表 |
| 死别名 | `lnxrm_abi.h` 中 15 个未使用的裸 `SIG*` 别名 + `LNXRM_SA_RESTART`；**`LNXRM_SIG*` 数值定义全部保留**（信号编号是 ABI，永不重排） | `sa_flags` 注释改为 reserved |
| Rust 死代码 | `kernel/rust/kalloc.rs`、`sync.rs`、`lib.rs` 的 `rust_hello`、`main.c` 的调用与 extern | `rust/` 仅余 `lib.rs` `smp.rs` `uart.rs`（后两者被 C 调用，是活代码） |
| 死类 | `SlabAllocator.cpp` 重写为仅保留 `operator new`（内核从不 `delete`） | 17 行 |
| 文档同步 | `CODEBASE.md` 9 处、`BUILD.md` `USR_LIB_C` 描述 | — |

验证：`make -j4` 零警告；`bzImage` 253,288 → **252,616**（-672 字节），text
250,432 → **249,760**；双核启动 + `systest` 通过；删除符号全树源码/文档残留扫描
为零。


### 16. SMP/AP 完善记录（第二轮，已完成，2026-10-01）

第一轮评审之后同日完成。目标是把第一部分 §4.6 的时间时钟缺口、6.1 的
C1/C9、以及第三部分的 T-001/T-052/T-097 一并落地；过程中逐层排查出
一个潜伏已久的时钟根因（§16.2）。

#### 16.1 实现清单

| 项 | 做法 | 对应缺口 |
|---|---|---|
| AP 本地时钟 | `lapic_timer_calibrate()` 在 `smp_init()` 前由 BSP 一次性标定 LAPIC 周期并缓存（`s_lapic_period`）；AP 在 `ap_main` 里用缓存武装自己的周期 tick（让每个 AP 各自标定会去竞争 PIT 锁存端口）；**`jiffies` 仅 BSP 递增**（否则全局时钟变 NCPU×HZ） | 4.6、C63、T-097 |
| TSC 标定 | 标定窗口顺带测出 `s_tsc_per_ms`，`arch/cpu.c` 导出 `mdelay()`；删除 `smp.cpp` 里硬编码 ~2 GHz 的本地版本 | 4.6、T-097 |
| 负载均衡 | `preferred_cpu`：自己空闲 → 空闲 AP（先于 BSP，保 BSP 留着处理设备中断）→ 最短队列（新增 `runq_len`）；本地入队立即置 `need_resched`。**`schedule()` 重排出去的 current 改走 `runqueue_add_local()`**——正在执行的任务落到远端队列，对方可能在本核 `swtch` 完成前就把它切走（双跑竞态）；只有睡眠/新建这类"不在任何核上执行"的任务走策略入队 | 6.1（C4 相邻） |
| 终端打印锁 | `console_out_lock/unlock`（irqsave）从 `print.c` 导出；`kvprintf` 与 `console_write`（64 字节分块，限制关中断时长）共用同一把锁 | **C1 已修复** |
| 任务表 | `find_task` 在 `task_table_lock` 内扫描；`task_alloc_slot` 清 `pid=0`（防复用槽匹配旧 pid）；`send_signal` 拒绝 `T_UNUSED`（覆盖查表解锁后的窗口）。`task_iter` **有意保持无锁**——它返回表内裸指针，而调用方全是单线程进程模型下的无并发写者路径，改回调式遍历收益低于风险 | **C9 已修复**（附带两处加固） |
| 死字段 | `cpu_info.{kstack_top,idle_ctx,idle_stack}` 删除；顺带发现 `task_lock` 只初始化从不加锁，一并删除 | T-040 |
| `%X` 格式化 | `vsnprintf` 原先只认 `d,u,x,p,s,c,%`，`%X`/`%04X` 全是字面量（MBR 错误路径实测暴露）；`putnum` 加 upper 参数支持 | 实测发现 |

**C8（fork fd 表竞争）判定为误报，不修**：进程是单线程的——`sys_fork`
执行期间父进程没有第二条执行流可以并发 `close`（fork 的整个临界区里父都在
内核里跑自己的代码），fd 条目本身也持引用计数。编号保留。

#### 16.2 根因：QEMU PIT 模式 3 让 jiffies 跑在 200 Hz

修完"AP 无时钟"后实测 **jiffies 恰好 200 Hz（2×HZ）**。逐层排除（关 LAPIC
归零 → gdb 读 LAPIC MMIO 不可信 → 串口墙钟打点 → 直接读 PIT 计数率），
锁定链路：

1. `pit_init` 用命令字 `0x36`——**模式 3（方波）**，装 divisor 11931；
2. QEMU 的 PIT 模式 3 每 `divisor/2` 个时钟就重装一次 → LAPIC 标定窗口
   实测只有 **5 ms 而不是 10 ms**（PIT 计数率直测 ≈2.53M/s ≈ 2×1.193182M，
   方波双倍递减是模式 3 的定义行为）；
3. `lapic_timer_calibrate` 测得的 period 只有应有值的一半 → 每秒中断 200 次。

修复：`pit_init` 改**模式 2**（`0x36` → `0x34`，速率发生器数满才重装）。
标定窗口恢复 10 ms，`period` 从 ≈312k 回到 ≈624k，串口墙钟实测
**200 jiffies / 1.99 s ≈ 100.6 Hz**。这是内核里潜伏已久的隐性 bug——
所有 `nanosleep` 一直在以 2 倍速跑（睡 1 秒实际只过 0.5 秒），`systest`
的时间断言宽松，测不出来。

#### 16.3 验证（全部当日实测）

| 检查 | 结果 |
|---|---|
| `make -j4` | `-Wall -Wextra` 零警告 |
| 产物 | `bzImage` 253,448 字节（第一轮清理后 252,616 → +832，标定与均衡代码；§16.4 启动消息扩展后 254,920） |
| `-smp 2` 启动 | `[smp] 2 CPUs online`；BSP/AP 两行 `[lapic] ... period=623880` **完全一致**（共享标定）；AP 行带 ` (AP)` 后缀 |
| 负载均衡 | `ps` 显示 `/bin/ps` 跑在 **CPU 1** |
| `-smp 1` 启动 | 1 CPU，`period=624399` 同量级，正常运行 |
| `systest` | `systest.done=1`，**0 失败**（双核、单核各一轮），含 sleep/signal/wait/segv 全项 |
| tick 速率 | 串口行打墙钟时间戳：200 jiffies 间隔 ≈1.99 s → ≈100 Hz ✓ |

已知观察（非回归）：一次双核启动首现 `[mbr] invalid signature`（IDE 冷读，
`disk.img` offset 510 处 `55 aa` 校验通过），重跑即过。

#### 16.4 启动消息扩展与启动耗时（同日第三步）

按需求「启动消息更详细」，新增 10 类启动输出：e820 逐条内存表 + 可用/预留
汇总、内核镜像物理范围、`[vmm]`/`[kheap]`/`[sched]`/`[kbd]`/`[serial]`
就绪行、CPUID 型号行、启动结束的可用内存与耗时。过程中用 TSC 打点定位到
一个真问题：**`smp_init()` 固定烧 2.06 s** —— 扫描不存在的 APIC ID
（`-smp 2` 下 try_id=2、3 各一次）时每个探测都烧满
`AP_STARTUP_TIMEOUT_MS 1000`。超时降到 100 ms（QEMU 实测 AP ~20 ms 就绪）：

| 检查 | 结果 |
|---|---|
| `[boot] ready in` | **407 ms**（改前 2211 ms；`-smp 1` 396 ms） |
| `[mem]` 行 | e820 逐条表 + `usable 255.4 MiB, reserved 0.4 MiB`；启动结束 `available memory 229.7 MiB` |
| `systest` | 双核 `systest.done=1`，0 失败（改后重跑） |
| 产物 | `bzImage` 254,920 字节（253,448 → +1,472） |


### 17. 已被实测证伪的旧文档条目

评审中发现以下旧文档陈述与当日实测冲突。**前三条（``README.md``）本轮已
直接改正**；后三条仅登记，未逐条改写（``CODEBASE.md``/``BUILD.md``/
``GAP_ANALYSIS.md`` 中另有约 30 处行号漂移与已删文件引用，本轮未处理）：

| 位置 | 旧陈述 | 实测事实 | 状态 |
|---|---|---|---|
| `docs/README.md:7` | "约 10,100 行内核 + 760 行用户态" | 10,460 行 + 1,101 行（第一轮评审时，`find \| xargs cat \| wc -l`）；SMP + 启动消息改造后 10,592 + 1,333 | 已改 |
| `docs/README.md` 实现范围 | "未实现：… `lseek`、`uname`、图形绘制系统调用" | 三者均已实现：`lseek(4)`、`uname(15)`、`fb_*(23–27)`，31/31 `case` 齐全，`systest` 实测通过。与同一文件开头"31 个全部已实现"自相矛盾 | 已改 |
| `docs/README.md` 实现范围 | "32 个 POSIX 信号的默认处置…**无用户态信号处理器**" | `sigaction(16)`/`sigreturn(20)` 已实现，`kernel/signal.c` 有 `sig_handlers[]` 分发；`systest` 实测 handler 捕获 SIGSEGV 成功 | 已改 |
| `docs/GAP_ANALYSIS.md` 第一部分 | "当前 23 个已实现的系统调用" | 32 个全部实现 | 仅登记 |
| 第三部分 T-006 | "检查 `fat32_journal` 的返回值" | `kernel/fs/fat32/fat32_journal.c` 已于会话前删除，该任务失效 | 仅登记 |
| `docs/CODEBASE.md` 等 | 文件表含 `usr/cpu.c`、`usr/fdisk.c`、引用 `fat32_journal.c` | 三者均已删除；`usr/` 实为 16 个程序（含 `systest.c`） | 仅登记 |

**2026-10-03 追加**（随 T-030/T-031 落地顺手改掉的，同样是"改文档而不是删编号"）：

| 位置 | 旧陈述 | 实测事实 | 状态 |
|---|---|---|---|
| `docs/CODEBASE.md` / `docs/README.md` / `docs/GAP_ANALYSIS.md` | "31 个编号 0–30"、`LNXRM_SYSCALL_LAST 30`、"≥31 返回 ENOSYS" | `setuid` 追加为 31 → **32 个 0–31**、LAST 31、≥32 ENOSYS（追加不重排） | 已改 |
| `docs/CODEBASE.md` §6.2 | `struct lnxrm_ps_entry`（32 B） | **40 B**——T-030 加了 `kind`/`uid` 两个 `int32`，`_Static_assert` 守着 | 已改 |
| `docs/CODEBASE.md` §12.1 | "ulib.h 26 个包装覆盖 31 个（`kill`/`getcpu`/`getkey`/`move` 直发编号）" | **32 个包装覆盖 32 个，全覆盖**；`getcpu`/`getkey`/`move` 早已随重排删除 | 已改 |
| `docs/CODEBASE.md` §6.1 | "从现在起 `.def` 里的编号**不再改动**" | 表述过强：规则是**只追加不重命名**，setuid 就是追加的 | 已改 |

**2026-10-05 追加**（T-032/T-033 收尾时逐个复核的数字；方法仍是"改文档而不是删编号"）：

| 位置 | 旧陈述 | 实测事实 | 状态 |
|---|---|---|---|
| `docs/CODEBASE.md` §18 全文件清单 | 80 个行数字段里 **26 处过期**（`syscall.c 381`、`task.c 511`、`framebuffer.c 609`、`systest.c 811`…） | 逐个 `wc -l` 复核重写，复核脚本报 **stale=0 / missing=0**；§11 的 `:NNN` 行号引用同步修正（`fb_store :529→:551`、`fb_init :444→:466`） | 已改 |
| `docs/CODEBASE.md` §12.1/§13 规模 | 10,592 内核 + 1,333 用户态（2026-10-01 口径，**早于 `ktest/` 出现**） | **15,752 + 1,768**（kernel 11,983 + arch 1,670 + include 2,099；init 239 + usr 1,529），语言表 97 文件 / **17,520 行** —— T-080 后已再改为 15,895/17,663，见下一张表 | 已改（`README.md:7`、本文 §12.1 同步） |
| `docs/GAP_ANALYSIS.md` C 类计数 | "当前有效条目 67 条"（= 70 − C2/C3/C6） | C3 重新生效并于 T-033 完成、T-080 再修 C50/C54 → **66 条**（= 70 − C2/C6/C50/C54） | 已改（README 与本文三部分 11 处同步） |
| `docs/BUILD.md` §0/§14.4 | `bzImage` 254,920、`vmlinux.bin` 252,872 | **160,024 / 157,976**（`--gc-sections` + T-033 重编后；T-080 后再改为 160,856 / 158,808，见下表） | 已改 |
| 本文 §12.1 | "`lnxrm_syscalls.def` 31 项（0–30）、`syscall.c` 31 个 `case`" | **32 项（0–31）、32 个 `case`**（T-031 的 `setuid`） | 已改 |
| `docs/CODEBASE.md` §18 `ktest/` | **表里根本没有这 17 个文件**（3,249 行自测全缺席） | 补一行说明指路（17 个文件、3,249 行，T-080 后 **3,290 行**、`.ktest` 段三阶段） | 已改 |
| `docs/GAP_ANALYSIS.md` C14/C45/C63 三行 | 3 列表里多出**第 4 格**（C14/C63 的状态注记），或单元格里有字面量 `\|`（C45 的 `(cluster<<32)\|offset`） | 多余的格**渲染时会被整格丢掉**：并回第 3 格、`\|` 转义；全部 6 份文档表格列数复核 **0 问题** | 已改 |


**2026-10-05 追加（T-080 磁盘容量与 IDENTIFY 验证后逐个复核的数字；方法仍是"改文档而不是删编号"）**：

| 位置 | 旧陈述 | 实测事实 | 状态 |
|---|---|---|---|
| `docs/CODEBASE.md` §12.1/§13 规模 | 15,752 内核（kernel 11,983 + arch 1,670 + include 2,099）、97 文件 / **17,520** 行、`scripts/` 952 | T-080 改了 6 个文件、净 **+143 行** → **15,895 内核**（kernel 12,102 + arch 1,670 + include 2,123）、**17,663**、`scripts/` **967**、总计 **18,630**；文件数仍 97/101 | 已改（`README.md:7`、本文 §12.1、`CODEBASE` 各处同步） |
| `docs/CODEBASE.md` §18 逐文件行数 | `ide.c 163`（§10.2 写 164）、`ahci.cpp 378`（§10.3 写 365）、`fs/vfs.c 720`、`sys/vfs.h 120`、`t_vfs.c 138`、`smoke_test.py 747`、`ktest/` 3,249、"上表 32 个文件 = 8,696" | 逐个 `wc -l` → **194 / 398 / 735 / 144 / 179 / 762 / 3,290 / 8,774**（`ide.c`、`ahci.cpp` 在 §10.2/§10.3 与 §18 原本就自相矛盾，一并拉齐） | 已改 |
| `docs/BUILD.md` §0/§14.4、`CODEBASE` §1.2 | `bzImage` 160,024、`vmlinux.bin` 157,976、`.image` 157,824 | **160,856 / 158,808 / 158,656**（`.bss` 仍 405,520） | 已改 |
| `docs/BUILD.md` 样例日志 | `[vfs] attempting disk mount on hda (131072 sectors, 64 MB)` | **64 MiB** —— `num_sectors / 2048` 是 MiB，标签写成 MB 正是这次要修的单位错误本身 | 已改 |
| `docs/GAP_ANALYSIS.md` C50/C53/C54 | 三条**有效**缺陷 | C50、C54 已修（T-080），C53 只修了容量字段（LBA48 命令仍缺，保持有效）→ C 类 **68 → 66** | 已改（11 处同步） |
| `docs/GAP_ANALYSIS.md` C48/C49/C51/C52/C55 的 `:NNN` | `ahci.cpp:256/122,273/252-257,…/235-236`、`ide.c:28-35,82` | T-080 在这两个文件里增删行，行号整体漂移（`vmm_map_kernel_page` 256 → 266、flush 235-236 → 245-246、`ide_wait_ready` 28-35 → 27-44、`GHC` 写 273 → 283） | **仅登记**，未逐条改写（沿用本节既定口径） |

**2026-10-06 追加**（真机三重故障事故修复后逐条复核；方法仍是"改文档而不是删编号"）：

| 位置 | 旧陈述 | 实测事实 | 状态 |
|---|---|---|---|
| `docs/CODEBASE.md` §3.1 ③、§3.2 | `boot_collect_e820()` 读 `*(u64*)0x6F00`、"`0x6F00` 这一个 8 字节"、`main.c:56` | 两个写入者只写 DWORD → 高 4 字节是残留；三处读点已改 `*(u32*)`，读点在 `main.c:57/97/199` | 已改 |
| `docs/CODEBASE.md` §3.1 ③ bring-up 次序 | `boot_collect_e820 → console_init → …`，`cpu_init()` 建 GDT/IDT 排在 `pmm_init` 之后 | **console/gdt/gs/idt 现在是头四步**（`main.c:221-224`），`pmm_reserve` 从 2 处变 5 处；§21.3 的行号清单整段重写 | 已改 |
| `docs/CODEBASE.md` §4.1、§4.3、`docs/BUILD.md` §8.1 | "管理窗口钳在 4 GiB 以下（`pmm.c:93`）" | 实为 **`PMM_WINDOW_TOP` = 1 GiB**（`mm.h:20`、`pmm.c:105`）；4 GiB 口径会让复现者误以为大内存没问题 | 已改 |
| `docs/CODEBASE.md` §4.3 | `vmm_translate_in` "识别 `ALIAS_BASE` 快捷路径" | 该路径 `ALIAS_BASE + 0x100000000UL` 溢出成 `0x80000000`，**一直是死代码**；2026-10-06 删除，改纯 PTE 行走 | 已改 |
| `docs/BUILD.md` §8.2、`docs/CODEBASE.md` §21.1 | `grub-mkrescore -o build/core.img boot/grub/grub.cfg`（命令名错、`-o core.img` 不是 ISO、还得 `-bios` 才起得来）；"⚠️ GRUB 路径未经实测"；`gfxpayload=1280x720,32` | §8.2 重写为 **`make imageiso`**（`Makefile:174`）+ 可直接跑的 QEMU 命令 + 排错段；§21.1 同步。**并实测出新事实**：`gfxpayload` 的逗号写法是 GRUB 后备模式列表语法，只能拿到 **24 bpp**，必须写 `1280x720x32` 才是 32 bpp（pitch 5120） | 已改 |
| `docs/GAP_ANALYSIS.md` C56/C57/C68/C69 | 递归无深度限制、4 GiB 窗口、GRUB 完全未测、`pmm.c:86-100` | C56 半修、C57 口径改 1 GiB、C68 改"手工实测未自动化"、C69 行号改 `95-109` 并补 1 GiB 裁剪 | 已改（新增 C71/C72，有效条目数仍 66） |
| 四份文档的行数与产物大小 | CODEBASE §1.1/§1.2/§2/§9.4/§14/§18/§33.4/§35、BUILD §0 与 §14.4、`README.md:7` 记内核 **15,895** / 合计 **17,663** / `bzImage` **160,856** | 逐个 `wc -l`、`stat` 重算 → **16,029 / 17,797 / 162,200**（+43 FAT32 修复、+91 本次修复）；顺带修掉 `fat32_priv.h` 213→247、`fat32_dir.c` 251→258、`fat32_meta.c` 738→745 这类上一轮遗留漂移 | 已改（口径说明留在 `CODEBASE.md` §35） |

### 18. 如何复核本文件的结论

```sh
make -j4                              # 零警告构建
ls -la build/bzImage                  # 160024 字节（--gc-sections 之后；旧值 254920/158968 已过期）

# 1.3/1.2 · syscall 全表与实现数
grep -c '^LNXRM_SYS' include/abi/lnxrm_syscalls.def     # 32
grep -c 'case SYS_'    kernel/syscall.c                 # 32

# N1 · 特权模型（2026-10-03 起不再是"零命中"，编号保留）
grep -rn 'CAP_' include/sys/cred.h                      # CAP_KILL / CAP_SYS_ADMIN / CAP_FB
grep -c 'cap_gate' kernel/syscall.c                     # 8 行 = 1 定义 + 6 调用（ps、diskinfo、fb×4）+ 1 注释（fb_owner_gate 头提它）
grep -rniE '\bRLIMIT\b' kernel/ include/                # 仍为空：机制不叫这个名字（T-032 用 cred_mem_quota()），CPU 限额仍未做

# T-004 · OOM 是错误返回，不是 panic（19 处调用点逐个核对过）
grep -rn 'kmalloc(\|cluster_buf_alloc(' kernel/ arch/ --include=*.c --include=*.cpp \
  | grep -vcE 'ktest|kheap.c|SlabAllocator'             # 19：每处后面都有 NULL 检查
grep -c 'panic("kmalloc' kernel/mm/kheap.c              # 1：只在 kmalloc_or_panic 里
grep -rn 'panic("vmm: out of frames"\|panic("vmm: no frame for aspace"\|panic("signal:' \
  kernel/                                                # 空：同链路三处 panic 已删
grep -c 'pmm_inject_oom' ktest/t_oom.c            # 9：4 例的注入点（+文件头注释）
grep -c 'brk\.oom' usr/systest.c                         # 2：tag 与循环到 ENOMEM 的那段
grep -c 'brk\.shrink\|brk\.after' usr/systest.c          # 4：归还 + 复活自检

# T-005 · 块设备 I/O 返回值（C15：未初始化栈被写进磁盘目录项，2026-10-05）
grep -rn 'dev->read\|dev->write' kernel/ | grep -v '//'       # 6 行：2 处分区表赋值 + 4 行都在 blk_io_* 里
grep -rn 'fat_read_sector(\|fat_write_sector(\|fat_read_cluster(' kernel/fs \
  | grep -vcE 'if \(|= '                                      # 0：20 处调用全在条件/赋值里，没有裸调用
grep -c 'blk_io_read\|blk_io_write' kernel/fs/mbr.c           # 3：part_read / part_write / mbr_parse
grep -c 'fat_warn_io(m' kernel/fs/fat32/*.c                   # 1+1+5+14 = 21 处告警点（限流 16 条）
grep -c 'blk_inject_io' ktest/t_ioerr.c                # 13：4 例里的武装/解除
grep -c '^KTEST' ktest/t_ioerr.c                       # 4：suite blkio（KTEST_LATE）
grep -cF 't-005 injected I/O error reported' scripts/smoke_test.py   # 1：串口 marker
grep -c 'it->err' kernel/fs/vfs.c                             # 2：readdir 记住"这次扫描已经死了"
grep -c 'LNXRM_EIO' kernel/fs/vfs.c                           # 4：lookup/create×2/O_TRUNC 的出口
grep -c 'LNXRM_EIO = ' include/abi/lnxrm_abi.h                # 1：-5（EIO 从此有独立编号语义）
# 运行时：make smoke 应为 176/176（T-080 后），且串口里能看到注入打出来的错误
grep -c '\[fat32\] I/O error' build/smoke/serial.log           # 9：4 例注入期间的真实 I/O 失败
                                                                #    （T-005 当时记的是 10：t_cache 的临时文件
                                                                #     改变了根目录扫描的长度，4 例断言本身全过）
grep '\[ktest\] selftest PASS' build/smoke/serial.log          # (72 cases, 1546 checks) [2 skipped]
                                                               #    （T-005 记的是 68/1508：+t_cred 2 例 +t_mm 1 例；
                                                               #      T-032 +t_mm 配额 1 例 → 70/1530；T-033 +t_fb 归属 1 例 → 71/1542）

# T-007 · 块缓存 LRU（C24：受害者永远是同一个槽，2026-10-05）
grep -c '\.used' kernel/fs/blk_cache.c include/disk.h           # 0：used 字段整个删了，只剩 stamp 一份记账
grep -c 'stamp' kernel/fs/blk_cache.c                           # 11：字段 + 命中/填充/暂存三处 ++lru_seq + 归零
grep -n 'cache_find_victim' kernel/fs/blk_cache.c               # 3：1 定义 + 1 调用 + 1 注释；体内没有"清空标志重来"
grep -c 'bump(&stats' kernel/fs/blk_cache.c                     # 8：hits×2 misses×2 evictions writebacks dev_r dev_w
grep -c 'blk_cache_stats_get\|blk_cache_stats_reset\|blk_cache_lookup\|blk_cache_purge' include/disk.h   # 4：四个声明
grep -c '^KTEST' ktest/t_cache.c                         # 5：suite blkcache（KTEST_LATE）
grep -c 'blk_cache_lookup\|blk_cache_purge\|blk_cache_stats' ktest/t_cache.c   # 23：每个数都要能被看见
grep -cF 'LATE_REPORTS' scripts/smoke_test.py                   # 2：定义 + 使用（整段日志搜，不进 BOOT_MARKERS）
grep -cF 'T-007 lru' scripts/smoke_test.py                      # 1：串口必须打出这行报告

# T-008 · 读写路径（C26：每类 LBA 只有一条路，2026-10-05）
grep -rn 'dev->read\|dev->write' kernel/ | grep -v '//'         # 仍 6 行：2 处 mbr 赋值 + 4 行都在 blk_io_* 里
grep -c 'blk_io_write(m->dev' kernel/fs/fat32/fat32.c            # 1：fat_set_entry 的 FAT 直写（刻意保留，见 C26）
grep -c 'fat_read_sector(\|fat_write_sector(' include/fat32_priv.h  # 3：2 个声明 + fat_read_cluster 里 1 次调用
grep -c 'test_fat_region_is_never_cached' ktest/t_cache.c    # 2：定义 + 注册（FAT 区 2018 扇区必须都不在缓存）
# 运行时
grep -c 'T-007\|T-008' build/smoke/serial.log                   # 5：5 条报告行，一条都不能少
grep '\[selftest\]' build/smoke/serial.log | tail -1           # 1542/1542 pass [1 warn]

# T-032 · brk 配额与内存记账（C12 后半段："谁占的 RAM"，2026-10-05）
grep -c 'cred_mem_quota' include/sys/cred.h                    # 1：配额的唯一来源（tier 派生，无 per-task 字段）
grep -c 'cred_mem_quota' kernel/syscall.c                      # 1：唯一调用点，且在取帧之前
grep -c 'CRED_USER_MEM_QUOTA_PAGES' include/sys/cred.h         # 2：常量定义 + cred_mem_quota 里用（64 MiB）
grep -c 'vmm_count_user_pages' include/mm/mm.h                 # 1：声明（记账的唯一真相 = 页表行走）
grep -c 'vmm_count_user_pages' kernel/mm/vmm.c                 # 1：实现，只读不改
grep -c 'vmm_count_user_pages' kernel/syscall.c                # 1：brk 成功后刷新
grep -c 'vmm_count_user_pages' kernel/task.c                   # 3：exec / fork / spawn 各刷新一次
grep -c 'mem_pages' include/sys/sched.h                        # 2：字段 + 注释（mem_peak 紧随其后）
grep -c 'mem_kib' include/abi/lnxrm_abi.h                      # 2：mem_kib + peak_kib（结构体 40 → 48 B）
grep -c 'mem_kib' usr/ps.c                                     # 1：MEM/PEAK 两列的打印
grep -c 'ps_mem_kib' usr/systest.c                             # 7：root 父进程取读数的 helper + 各断言
grep -c '^KTEST' ktest/t_mm.c                           # 8：suite mm 里新增 test_vmm_count_user_pages
grep -c '^KTEST' ktest/t_cred.c                         # 8：suite cred 里新增 test_mem_quota_follows_the_tier
grep -cF 'T-032:' scripts/smoke_test.py                        # 1：14 条 tag 的注释头（行 179-194；T-033 的 5 条插在它前面，整体下移 7 行）
grep -cF 'user brk stops at ENOMEM' scripts/smoke_test.py      # 1：quota.oom=-12 那条
# 运行时
make smoke                                                      # PASS 174/174 checks（连跑两次；smp1 下 81 tags）
grep 'stage mm' build/smoke/serial.log                          # 15 cases, 429 checks, 0 failed（T-032 前是 14/415）
grep '\[ktest\] selftest PASS' build/smoke/serial.log           # (71 cases, 1542 checks) [2 skipped]
grep 'quota\.mem\.kib\|quota\.rounds\|mem\.delta' build/smoke/serial.log   # 64548 / 63 / 8192
grep -E 'WARN\(' build/smoke/serial.log | grep -v 't_oom'       # 空：新增代码没有引入 WARN()（仍是 [1 warn]）

# T-033 · 画屏归属（C3 重新生效："后缓冲归属只记录不强制" → 强制，2026-10-05）
grep -c 'fb_owner_gate' kernel/syscall.c                    # 5：1 定义 + 4 个绘图 case 各一行，全部排在 cap_gate 之后
grep -c 'cap_gate' kernel/syscall.c                         # 仍 8 行：能力门没有被归属门顶掉
grep -n 'static u32 fb_owner' kernel/framebuffer.c          # 1：状态在驱动侧（"这块屏归谁"不是"这任务有什么"）
grep -c 'fb_claim' kernel/framebuffer.c                     # 2：CAS 实现 + 注释（两趟，不是 check-then-set）
grep -c 'fb_disown' kernel/framebuffer.c                    # 2：CAS 条件清 + 注释（只清自己持有的）
grep -c 'fb_disown' kernel/task.c                           # 1：sys_exit 里唯一那处释放，task_free_slot 故意不放第二份
grep -c 'LNXRM_EACCES' kernel/syscall.c                     # 5：既有 3 处 + cap_gate 1 + fb_owner_gate 1（不新增 EBUSY/EPERM）
grep -c 'LNXRM_SYS' include/abi/lnxrm_syscalls.def          # 仍 32：没有为归属加系统调用
grep -c 'past_table' usr/systest.c                          # 仍 4：{32,33,34} 返回 -38 的断言原样，ABI 表照旧封闭
grep -c '^KTEST' ktest/t_fb.c                        # 6：新增 test_fb_owner_is_one_pid_at_a_time（12 条断言）
grep -c 'fb\.owner' usr/systest.c                           # 4：grab / denied / held / released 四个 tag
grep -cF 'fb\.owner' scripts/smoke_test.py                  # 4：与之对应的 4 条判据（+1 条回执 = 169 → 174 的 +5）
grep -cF '\[fb\] denied fb_fill:' scripts/smoke_test.py     # 1：回执必须点名持有者与闯入者，不能只是"它没画出来"
# 运行时
make smoke                                                   # 169 → 174/174（连跑两次）；smp1 76 → 81 tags
grep 'stage late' build/smoke/serial.log                     # 32 cases, 767 checks → 33 cases, 779 checks, 0 failed
grep '\[ktest\] selftest PASS' build/smoke/serial.log        # (71 cases, 1542 checks) [2 skipped]
grep -c '\[fb\] denied' build/smoke/serial.log               # 1：回执真的打出来了
grep 'fb\.owner\|\[fb\] denied' build/smoke/serial.log       # grab=0 / 回执 / denied=-13 / held=0 / released=0
grep -c 'WARN(' build/smoke/serial.log                       # 0：没新增 WARN()（[1 warn] 仍是原来的那条）

# C46 · 超长路径被拒绝，不再被静默截断（2026-10-03）
grep -c 'LNXRM_ENAMETOOLONG' kernel/fs/vfs.c            # 7：6 个调用点 + 1 处注释
grep -c 'LNXRM_ENAMETOOLONG' include/abi/lnxrm_abi.h    # 1：新增的 -36
grep -c 'WARN(' kernel/fs/vfs.c                         # 0：WARN 已随错误路径一起删除
grep -c 'K_EXPECT(r == NULL)' ktest/t_fs.c       # 3：超长 / 29+1 边界 / bufsz<2
grep -c 'path\.toolong' usr/systest.c                   # 5：open/mkdir/unlink/rename×2
grep -cF 'path\.toolong' scripts/smoke_test.py          # 5：与之对应的 5 条 tag
grep -nF 'pass \[1 warn\]' scripts/smoke_test.py        # 期望值 2 warn -> 1 warn

# N3 · CPU 安全特性（每处写入只 OR 了 PE/PG/PAE/PGE，无 WP/SMEP/SMAP）
grep -rn -A1 'mov  *\(eax\|rax\), *cr[04]' arch/*.asm arch/*.S
grep -rniE 'NXE|_NX|0x8000000000000000' kernel/ arch/ include/            # 空
grep -c 'fno-stack-protector' Makefile                   # 2 处

# N2 · 无步数上限的簇链循环
grep -rn 'cluster_is_eoc' kernel/fs/fat32/*.c            # 13 处，对照 §14.3 名单

# N7 · 僵尸唯一释放点
grep -rn 'state == T_ZOMBIE' kernel/                     # 仅 sys_waitpid 的判定与释放

# P0 · 缺失 syscall
grep -E 'pipe|stat|chdir|getcwd|umask|alarm|mmap|gettimeofday' include/abi/lnxrm_syscalls.def  # 空

# §16 第二轮 · 时钟与并发（对照 §16.1/§16.2）
grep -n '0x34' arch/cpu.c                                # PIT 模式 2（0x36 是模式 3，会跑 200 Hz）
grep -n 'this_cpu_data()->bsp' arch/cpu.c                # jiffies 仅 BSP 递增
grep -n 'lapic_timer_calibrate' kernel/main.c            # BSP 标定一次（smp_init 之前）
grep -n 'lapic_timer_start' kernel/main.c kernel/smp.cpp # BSP 与 AP 各自武装周期 tick
grep -n 'runqueue_add_local' kernel/sched.c              # schedule() 本地重排（防双跑）
grep -n 'console_out_lock' kernel/print.c kernel/fs/vfs.c     # C1：kvprintf/console_write 同锁
```

运行时验证（对照 §16.3）：启动日志里两行 `[lapic]` 的 `period` 应完全一致且
≈624k；tick 速率 = 串口行墙钟间隔 × HZ ÷ 行间隔（200 jiffies ≈ 2.0 s）：

启动与运行时验证见 ``BUILD.md`` §8/§9；`systest` 的喂入方式：

```sh
(sleep 4; echo "systest"; sleep 14) | timeout 30 qemu-system-x86_64 \
    -m 256 -smp 2 -kernel build/bzImage \
    -drive file=build/disk.img,format=raw,if=ide,index=0,media=disk \
    -serial stdio -display none -no-reboot
```

```sh
# T-080/T-081 · 磁盘容量与 IDENTIFY 验证、挂载设备选择（C50/C53 部分/C54/C28 部分，2026-10-05）

# 静态：容量字段的收口——`num_sectors` 不可能再是 0xFFFFFFFF
grep -c 'ata_total_sectors' include/sys/vfs.h kernel/drivers/ide.c kernel/drivers/ahci.cpp   # 1 定义 + 1 + 2 = 4
grep -c '0xFFFFFFFFu' include/sys/vfs.h                    # 1：哨兵返回 0 的那一行（0 代替 2 TiB）
grep -c 'lba > 0x0FFFFFFFUL' kernel/drivers/ide.c           # 2：读、写各一处，超界即 EFAIL（不静默回绕）
grep -c 'ide_wait_drq()' kernel/drivers/ide.c              # 4：3 个调用点（读/写/IDENTIFY）+ 1 处注释
                                                          #     IDENTIFY 那处是新增的——C54 的入口
grep -c 'outb(IDE_CMD, IDE_CMD_IDENTIFY)' kernel/drivers/ide.c  # 1：命令仍要发（改写时漏过一次，被 smoke 抓住）
grep -c 'P_TFD\|PxTFD' kernel/drivers/ahci.cpp             # 6：waitTfd + 完成判定新加的 ERR 位 + 注释
grep -c 'MB)' kernel/fs/vfs.c                              # 0：单位标签只剩 MiB
grep -c 'blk_first' kernel/fs/vfs.c                        # 4：声明 + blk_register 的赋值 + 2 行注释；挂载路径不再读它
grep -c '^KTEST' ktest/t_vfs.c                      # 5：新增 test_ata_capacity（4 行断言）
grep -cF 'disk capacity is the real 64 MiB image' scripts/smoke_test.py   # 1：正向判据
grep -cF 'no all-ones disk capacity' scripts/smoke_test.py                # 1：反向判据

# 运行时
make smoke                                                 # PASS 176/176 checks（174 + 2 条，连跑两次）
python3 scripts/smoke_smp1.py                              # tags: 81 checked, 0 failed（systest 没动，tag 数不变）
grep 'stage early\|stage mm\|stage late' build/smoke/serial.log
    # 23/334、15/429 未变（测试加在 suite vfs / LATE）
    # late 33 cases/779 checks → 34 cases/783 checks
grep '\[ktest\] selftest PASS' build/smoke/serial.log      # (72 cases, 1546 checks) [2 skipped]
    # 链条：T-005 68/1508 → T-032 70/1530 → T-033 71/1542 → T-080 72/1546
grep '\[selftest\]' build/smoke/serial.log                 # 1546/1546 pass [1 warn]：warn 计数仍是 1
grep -c 'WARN(' build/smoke/serial.log                     # 0：没新增 WARN()
grep 'attempting disk mount' build/smoke/serial.log        # hda (131072 sectors, 64 MiB)
    # 64 MiB 不是 64 MB，也不是 2097151 —— 两个新判据分别从正反两面钉这一行
grep -c '4294967295' build/smoke/serial.log                # 0：全 1 容量不再出现

# 验收"无设备时正确报告'无设备'而非超时"的唯一测法：不给 -drive
qemu-system-x86_64 -m 256 -smp 2 -cpu qemu64,+smep,+smap \
    -display none -serial stdio -no-reboot -kernel build/bzImage
#   [ide] no device on primary channel (status=00)
#   [vfs] no disk found
# 空总线回 00（老判据 `s == 0` 恰好能过）；真机上回 FF —— 两个值现在都拦，
# 且判定挪到发 IDENTIFY 之前，不再与命令竞争。

# 未达成的验收（诚实登记）
#   T-080：`>128 GiB 镜像读写正确` —— LBA48 命令（0x24/0x25）未实现，C53 保持部分开放
#   T-081：`同时挂 IDE 和 AHCI 盘` —— QEMU 只给 -drive if=ide，AHCI class 过滤全失败，
#          双盘同时在场的路径一次都没跑过；这是策略落地后最该补的那条测试
```



---

## 第三部分 · 行动计划

> 本文第三部分只回答"明天开始具体做什么"：差距见第一部分，实测与安全审查见第二部分
> 文档性质：可执行计划。每项都有触发原因、改动位置、验收标准、成本估计、依赖

---

### 19. 这份文档是什么

第一部分回答"离真正的内核还差什么"。本部分回答另一个问题：**明天开始，具体做什么。**

因此本部分的组织原则是：

- 每项都是**可独立验证**的改动，有明确的"做完"定义
- 顺序不是按重要性排，是按**依赖关系**排——有些最重要的改动必须等别的东西做完才能安全落地（见第 20 节）
- 明确写出**不该做什么**（第 32 节冻结清单）
- 给出**时间盒方案**（第 35 节）：只有 1 天怎么办，有 3 个月怎么办

**一句话版本**：先用半天建立最小验证，再用两天修掉会挂死整机的问题，然后用一周建立真正的自测框架，然后花一天点亮真正的硬件安全边界——之后所有的改进才有意义。

---

### 20. 排序原则：为什么是这个顺序

有四条原则，它们决定了下面的排序：

#### 原则 1：最便宜的验证必须最先做

当前**唯一的验证手段是"启动，看串口日志"**。这个循环太慢也太脆，所以每个改动都只能靠人眼判断。

但**启动冒烟测试只需要半天**，而且它立刻给后面每一个改动提供了回归门。这是整个计划里投入产出比最高的一项，所以放在 Step 0。

#### 原则 2：止血优先于建设

有些缺陷会**挂死整机或让内核永久 panic**，且触发条件极低（一个坏扇区、一个失控的 `while` 循环）。这些必须在投入任何建设之前修掉。

#### 原则 3：安全特性不能盲开

这是一个**非显然但关键的顺序约束**：

打开 `CR0.WP` / `CR4.WP` 会立刻改变行为——内核对只读页的写会开始 `#PF`。如果代码里存在任何"内核往只读用户页写"的潜在 bug（现在不会 fault，所以没人发现），打开 WP 之后会立刻变成页错误风暴。

**而现在没有测试，你无法判断这是新引入的 bug 还是本来就有的 bug。**

所以顺序必须是：**先有自测框架（Step 2），再开安全特性（Step 3）**。反过来做是在赌。

#### 原则 4：修注释必须晚于建立测试

差距分析列了 8 条"注释/ABI 说谎"。 tempting 的做法是立刻全部改对。

但**改完还会重新长出来**——因为没有任何机制阻止它。所以顺序是：先建立测试（Step 2），让"文档承诺的行为"变成"有断言守着的断言"，再改注释（Step 5）。这样改完才不会反弹。

例外：那些**会误导下一步改动**的注释（比如 "circular run queue" 实际是 LIFO）可以随手改，因为它们就在你马上要动的代码旁边。

---

### 21. 依赖图

```
Step 0  启动冒烟测试（0.5 天）
   │   ← 门控以下所有步骤
   ▼
Step 1  止血：挂死/失控缺陷（2 天）
   │
   ▼
Step 2  内核自测框架（1 周）
   │   ← 必须先于 Step 3
   ▼
Step 3  硬安全边界 NXE/WP/SMEP（1 天）★★ 最高性价比
   │
   ▼
Step 4  特权模型 + 资源限制（2 周）
   │
   ├──► Step 5  契约对齐（1 天）
   ├──► Step 6  身份与生命周期（1 周）
   │      │
   │      ▼
   └──► Step 7  文件系统硬化（2 周）
          │
          ├──► Step 8  驱动（1.5 周）
          │
          ▼
Step 9  地址空间能力 mmap/COW/缺页（4 周）
```

**关键路径**：`0 → 1 → 2 → 3 → 4 → 7 → 9`。Step 5/6/8 可与 4/7 并行。

---

### 22. Step 0 · 建立最小验证（0.5 天）★

**目标**：给出一个能在 CI 里跑的"内核还活着吗"判定。这是后面每一步的门控。

#### T-000 · 启动冒烟测试

- **触发**：全树零测试（差距分析 B 类）
- **成本**：半天（含调试）

**做法**：写一个 `scripts/smoke.sh`，QEMU 无头启动，串口重定向到文件，grep 关键标记。

**已实测可用的配方**（在 `-smp 2` 下验证通过）：

```sh
#!/bin/sh
# scripts/smoke.sh — 内核启动冒烟测试
set -u
LOG=$(mktemp)
MARK1='smp] 2 CPUs online'
MARK2='vfs] disk mounted at /'
MARK3='A tiny unix-like kernel'      # 用户态跑起来了（端到端）

timeout 20 qemu-system-x86_64 -m 256 -smp 2 -display none -no-reboot \
  -kernel build/bzImage \
  -drive file=build/disk.img,format=raw,if=ide,index=0,media=disk \
  -serial "file:$LOG" >/dev/null 2>&1

FAIL=0
for m in "$MARK1" "$MARK2" "$MARK3"; do
  grep -qF "$m" "$LOG" && echo "PASS: $m" || { echo "FAIL: $m"; FAIL=1; }
done
grep -qiE 'exception|panic|reboot|triple' "$LOG" \
  && { echo "FAIL: 日志中出现异常/panic"; FAIL=1; }
[ "$FAIL" = 0 ] && echo "SMOKE OK" || { echo "SMOKE FAILED"; exit 1; }
rm -f "$LOG"
```

三个标记的选择理由：
- `smp] 2 CPUs online` —— 覆盖 AP 启动、trampoline、per-CPU 数据、APIC
- `vfs] disk mounted at /` —— 覆盖 VFS + IDE + FAT32 + 块缓存
- `A tiny unix-like kernel` —— **最有价值的一个**，它证明用户态真的跑了，也就是 VBE→PMM→VMM→KHEAP→APIC→IDT→VFS→FAT32→ELF→调度→SMP→用户态**整条链是通的**

**注意**：QEMU 会超时退出（exit 124），这是预期的——内核目前没有退出路径。这是 T-040 要修的。

**Makefile 集成**：

```make
.PHONY: smoke ci
smoke: all usr
	@sh scripts/smoke.sh

ci: smoke
	@echo "CI: OK"
```

**验收标准**：
- [ ] `make smoke` 在当前代码上返回 0
- [ ] 故意在 `main.c` 里插一行 `panic("test")`，`make smoke` 必须返回非 0
- [ ] 在 QEMU 参数里去掉 `-smp 2`（单核）也必须 PASS（验证测试不依赖 SMP）

---

### 23. Step 1 · 止血（2 天）

**目标**：修掉会导致**整机挂死**或**内核永久 panic**的缺陷。这一步全是小改动，全部由 Step 0 门控。

#### T-001 · `console_write` 走 `print_lock` 【最高优先级】

> **✅ 已完成（2026-10-01 第二轮，方案 B）**：`print.c` 导出
> `console_out_lock/unlock`（`spin_lock_irqsave`）；`kvprintf` 与
> `console_write`（`vfs.c`，64 字节分块调用以限制关中断时长）共用同一把锁。
> 验证：`-smp 2` 启动 + `ps`/`systest` 全过（第二部分 §16.3）。
> 未做：并发 write 压测程序（依赖 Step 0 的测试框架）。

- **触发**：C1 —— SMP 下用户态写终端无锁改写两套 ANSI 解析器状态
- **现状风险**：`make run` 默认 `-smp 2`，任何用户程序每次 `write(1,…)` 都在无锁改写 `vrow/vcol`、`fbc_row/fbc_col/fbc_fg/fbc_bg`、`ring_head/ring_count`
- **位置**：`kernel/fs/vfs.c:540-545` vs `kernel/print.c:329-336`
- **成本**：**1 行 + 1 处取舍**（见下）

```c
// 现状
long console_write(struct file *f, const void *buf, size_t n)
{
    const char *p = buf;
    for (size_t i = 0; i < n; i++) console_putc(p[i]);
    return n;
}
```

问题在于 `console_putc` 只有经 `kvprintf` 才会持锁（`print.c:329`）。两种修法：

- **方案 A（推荐）**：在 `console_write` 里 `spin_lock(&print_lock)`。问题是 `print_lock` 是 `print.c` 的 `static`，不可见 → 需要导出或加访问器。
- **方案 B（更干净）**：把 `console_putc` 拆成 `console_putc_unlocked`（内部用），`kvprintf` 和 `console_write` 各自持锁调用。

**验收标准**：
- [ ] `make smoke`（`-smp 2`）通过
- [ ] 新增一个用户程序，两个实例并发各 `write` 1000 行，串口输出无字符丢失/错序/光标错位
- [ ] 连续运行 20 次 `-smp 2` 启动无异常

#### T-002 · `panic` 绕过 `print_lock`

- **触发**：`print.c:346-356` 调三次 `kprintf`，而 `kvprintf` 要拿 `print_lock` —— **从已持锁的上下文 panic 就是永久自旋而不是停下**
- **成本**：~10 行
- **做法**：`panic` 内部 `cli` 后走一个不持锁的裸写路径（或直接用 `lnxrm_uart_putc`）
- **验收**：从 `console_putc` 内部触发 `panic`，系统停机而不是死锁

#### T-003 · FAT 簇链加迭代上限与环检测 【最高数据风险】

- **触发**：C11 —— **一个坏扇区就能让整机永久挂死**
- **现状风险**：`fat_next_cluster`（`fat32.c:17-18`）把任何 `< 0x0FFFFFF8` 的值当合法下一簇；所有遍历 `while (!eoc && c >= 2)` 无迭代上限。命中自引用或保留值（如 `0x0FFFFFF7`）→ 持有 `fat_fs_lock` **且关中断**死循环
- **位置**：`kernel/fs/fat32/fat32.c:17-18` + 8 处遍历点：`fat32_dir.c:47`、`fat32_file.c:95,150,156,180`、`fat32_meta.c:142,187,291,318,347`
- **成本**：~20 行，半天

**做法（两处都要）**：

1. `fat_next_cluster` 增加边界校验：返回值必须 `< max_cluster` 且 `>= 2`
2. 每个遍历加计数器上限（例如 `max_cluster + 2` 次），超限返回失败而不是继续

**验收标准**：
- [ ] 构造一个自引用簇的 FAT32 镜像（用 `mtools` 手工改 FAT 项），挂载后**不挂死**，返回 `EIO`
- [ ] 构造一个 `0x0FFFFFF7` 保留值簇，同样不挂死
- [ ] `make smoke` 通过
- [ ] 正常镜像 `fsck.vfat` 干净

#### T-004 · `kmalloc` 失败返回 NULL 而不是 panic

> **✅ 已完成（2026-10-03）**：`kmalloc` 两处 OOM 改成返回 `NULL`，新增
> `kmalloc_or_panic()` 给**没有错误分支**的 bring-up 路径用（当前唯一调用者
> 是 C++ `operator new`——freestanding 没有异常可抛，`new` 的契约就是非 NULL）。
> 逐个核对了**全部 19 处调用点**，补上原先缺失的 **9 处**检查：
> `ahci.cpp:293-295`（3 处；AHCI 是可选设备 → 分配失败就跳过该 port 并打日志，
> 不值得为它停机）、`fat32_file.c:16,26`、`fat32.c:231`（挂载）、
> `fat32_dir.c:43`、`vfs.c:225,238`。
>
> 文档预言的"会暴露一批被 panic 掩盖的 NULL 解引用"应验了；**更大的收获是同
> 一条链路上另外三处 panic——不修它们，本任务的验收判据（brk 循环到失败、内核
> 存活）根本过不了**，因为 `brk` 每涨 2 MiB 就要一张新页表：
>
> | 位置 | 原行为 | 现在 |
> |---|---|---|
> | `vmm.c` `ensure_table()` | `panic("vmm: out of frames")` | 返回 0，`vmm_map_user` 回 `LNXRM_ENOMEM` |
> | `vmm.c` `vmm_new_user_aspace()` | `panic("vmm: no frame for aspace")` | 返回 0；`sys_fork` 走清理分支（顺带修掉 `ch->pml4 == 0` 时 `dup_user_aspace` 去写物理 0 的问题） |
> | `signal.c` `signal_map_restorer()` | 两处 `panic` | 返回 `LNXRM_ENOMEM`，exec/spawn 两条路径传播 |
> | `syscall.c` `sys_brk()` | 中途失败时**已映射的页泄漏**（`brk_cur` 没动，之后的 shrink 根本找不到它们） | 失败即回滚本次已映射的页再返回 ENOMEM——第一部分 C60 一并修掉 |
> | `elf.c` / `task.c` 的栈与段映射 | 忽略 `vmm_map_user` 返回值 | 失败即释放刚分配的帧并解卷 |
>
> **测试**（改动必须有测试守着）：
> - `ktest/t_oom.c`（4 例，`KTEST_MM`）——新增 `pmm_inject_oom(n)`
>   把下一次分配变得**像空 buddy 一样失败**，从而在内存充裕的机器上确定性地
>   走到 OOM 分支：`kmalloc` 的 buddy 分支 / 补页分支 / 超大请求，以及 vmm 的
>   aspace root 与页表两支。不注入就得真把机器跑干。
> - `usr/systest.c` 新增 `brk.oom` 段：真实地把 buddy 掏空（8 MiB 一步直到
>   ENOMEM），断言**全部归还**后再 fork+wait 一次证明内核还活着。
> - `scripts/smoke_test.py` 新增 5 条 tag 守着上面这段。
> - 结果：`make smoke` **141/141 PASS**（`-smp 1` 下 `smoke_smp1.py` 同步生效）。
>
> **有意保留的 panic**：`vmm_map_kernel_page`（MMIO，只在启动/驱动路径可达）
> 与 `operator new`——都够不到用户态，且没有有意义的错误分支。
> **未做**：块设备 I/O 返回值（T-005）是另一条独立的链，本次没动。
> `brk` 配额与内存记账曾是 T-032，**已于 2026-10-05 完成**（见 T-032 卡片）。

- **触发**：C12 —— 用户态程序能让内核永久 panic
- **现状风险**：`kheap.c:54,70` panic ⇒ FS 层 7 处 `if (!kmalloc(...)) return ENOMEM` 全是死代码（`vfs.c:187,201,210,617`；`fat32.c:197`；`fat32_meta.c:97,182`；`mbr.c:97`）。配合无上限的 `brk`，一个 `while(1) brk(1<<30);` 就能停机
- **成本**：~30 行，半天
- **做法**：`kmalloc` 返回 NULL；保留一个 `kmalloc_or_panic` 给"内核启动早期不能失败"的路径用。逐个确认所有调用点真的检查返回值
- **注意**：这会暴露出一批**之前被 panic 掩盖的 NULL 解引用**，所以 T-004 必须和 T-005 一起做
- **验收标准**（2026-10-03 全部通过）：
  - [x] 一个用户程序循环 `brk` 到失败，返回 `ENOMEM`，**内核存活**
        （`brk.oom=-12`、`brk.oom.rounds=[1-9]\d*`、`brk.shrink=1`、
        `brk.after.fork=[1-9]\d*`、`brk.after.status=0`）
  - [x] `grep -n "kmalloc(" kernel/ | grep -v "if (!"` 逐条确认有 NULL 检查
        （19 处调用点全部核对，9 处原本缺失、已补；见上表与上方完成记录）
  - [x] `make smoke` 通过（**141/141**）

#### T-005 · 检查所有块设备 I/O 的返回值

> **✅ 已完成（2026-10-05）**：卡上写的两处现场只是这条链的末端。真正的修法是让
> "块 I/O 一定有人看结果"变成**结构上绕不过去**的事实：全内核现在只剩**一处**直接
> 调用 `dev->read`/`dev->write`——`blk_io_read()`/`blk_io_write()`
> （`kernel/fs/blk_cache.c:47,54`），分区设备、缓存 miss、原始读写全都必须穿过它。
>
> **自下而上的错误契约**（每层只做翻译，不做吞并）：
>
> | 层 | 位置 | 契约 |
> |---|---|---|
> | 驱动口 | `blk_cache.c:47,54`（`blk_io_*`）、`:137,198`（`blk_cache_read/write` 入口） | 驱动的非 0 原样上抛；**缓存读失败顺带作废 victim 槽**（`:171-183`，一并修掉 CODEBASE 9.3「读失败后 `valid` 仍为真」：半写坏的扇区会被缓存成"好数据"） |
> | 分区 | `kernel/fs/mbr.c:18,25,42`（`part_read`/`part_write`/`mbr_parse`） | 全改走 `blk_io_*`，分区设备继承同一套检查与注入 |
> | 扇区 | `include/fat32_priv.h:81,84` | `fat_read_sector`/`fat_write_sector` **只回答 0 或 `LNXRM_EIO`**，不透传驱动私有码（`fat_read_cluster` 同理，`:91-99`） |
> | 扫描 | `fat_scan_dir`（`fat32_dir.c:41`）、`fat_resolve_path`（`:139`）、`fat_dir_iter`（`:219`） | 三态 `1 / 0 / 负`：「读不到」不再和「没找到」「扫到末尾」共用同一个返回值 |
> | 语义 | `fat_lookup_impl`/`fat_read_impl`/`fat_write_impl`/`create`/`mkdir`/`rmdir`/`unlink`/`rename` | 负数一路上传；`fat_set_entry`（`fat32.c:114`）也由 void 改为返回状态，FAT 项写失败不再是"分配成功" |
> | VFS | `kernel/fs/vfs.c:195-207`（open/create）、`:473-491`（readdir） | `EIO` 不再被改写成 `ENOENT`/`EACCES`；`dir_getdent` 不再把死盘折成 `0`（= 目录是空的），`struct dir_iter.err` 会记住这次扫描已经死了 |
>
> **写路径的三种结局**（`fat_write_impl`，`fat32_file.c:147`）：
>
> | 情况 | 旧行为 | 现在 |
> |---|---|---|
> | 读旧扇区失败（**C15 原始场景**，`:194`） | 把栈上垃圾 merge 进去照写 | 立刻 `EIO`，**一个字节都不落盘、元数据不动**（`:215`） |
> | 中途失败 | 假装写完了 | 按 `done` 发布 `filesize` 并返回**短计数**（`:219`），不谎报整段写完 |
> | 目录项写不进去（`:246`） | 静默成功 | `EIO` |
>
> **告警**：`fat_warn_io()`（`fat32.c:55`）限流 16 条，打印
> `[fat32] I/O error <op> LBA <n>` ——被吞掉的返回值唯一还能留下的证据。
> **没有新增 `WARN()`**，smoke 的 `[1 warn]` 判据保持不变。
>
> **故障注入**（验收判据要求"让 `dev->read` 对特定 LBA 返回错误"）：
> `blk_inject_io(dev, lba, BLK_IO_READ|BLK_IO_WRITE)` / `blk_inject_io_off()`
> （`include/disk.h:81-91`，实现 `blk_cache.c:26-45`）——武装一个 `(dev, lba)`，
> 覆盖它的每一次传输都回答 `LNXRM_EIO`，走的是**与真驱动拒绝完全相同的代码路径**，
> 且一个字节都不碰磁盘。测试专属，生产代码从不武装。`blk_cache_read` 在查表**之前**
> 就检查注入，否则"扇区正好在缓存里"会把故障挡掉，测不到 FS 层。
>
> **测试**（改动必须有测试守着）：
> - `ktest/t_ioerr.c`（4 例，suite `blkio`，`KTEST_LATE`）：
>   ① 注入设施自检——原始路径、缓存路径、相邻 LBA、解除武装各自的表现；
>   ② `fat_write_impl` 读到不可读扇区 → `EIO`，数据扇区与目录扇区**逐字节不变**，
>      文件读回仍是原来的内容；
>   ③ `create` 两支失败（目录读不到 / 写不进去）→ 都是 `EIO`，目录扇区不变，
>      事后 `resolve` 两个名字都不在；
>   ④ `lookup`/`unlink`/`rmdir`/`readdir` 的 impl 层 **与** VFS 层（`vfs_open_file`
>      → `EIO` 而非 `ENOENT`，`getdent` → `EIO` 而非 `0`，且第二次仍是 `EIO`）
>      都是 `EIO`。每例都用**绕过缓存的直读**做前后扇区快照比对。
> - `scripts/smoke_test.py` BOOT_MARKERS 新增一条
>   `("t-005 injected I/O error reported", br"\[fat32\] I/O error")`，
>   夹在 `fat32 mounted` 与 `late selftest` 之间：注入必须真的把错误喊出来。
> - 结果：`make smoke` **150/150 PASS**（基线 149/149 + 新 marker）；
>   `[ktest] selftest PASS (63 cases, 1046 checks) [2 skipped]`（基线 59/1003，0 failed）；
>   `[selftest] 1046/1046 pass [1 warn]`；`scripts/smoke_smp1.py` 62 tags / 0 failed。
>
> **顺手修掉的两处同类问题**（不在卡上，但违反同一条"不得伪装"的要求）：
> `vfs_lookup` 把 lookup 的 `EIO` 折成 `ENOENT`、create 的 `EIO` 折成 `EACCES`；
> `dir_getdent` 把 readdir 的 `EIO` 折成 `0`（= 目录到头了）。
>
> **仍登记、本卡不修**：truncate（`fat_write_impl` 的 `!buf && !n` 分支）只改内存里的
> `filesize`、不写回目录项；`alloc_chain` 中途失败可能泄漏它已经链接上的簇；
> `vfs_lookup` 把 create 的 `ENOSPC` 等错误折成 `EACCES`（`vfs.c` 注释自己写着
> "EACCES-ish"）。

- **触发**：C15 —— 未初始化内核栈被写进磁盘目录项
- **现状风险**：`fat32_meta.c:58,107,116,294`、`fat32_file.c:160,183` 调用 `fat_read_sector`/`dev->read` 后**忽略返回值**，随后 `memset`+写入+落盘。读失败就把栈垃圾当目录数据写进磁盘（行号为修复前的坐标）
- **成本**：~40 行，1 天（实际：块层 + 6 个 FS 文件 + VFS + 4 例测试）
- **做法**：每处检查返回值；`fat32_file.c:160` 特别要注意——它是"读旧扇区 → memcpy 新数据 → 写回"，读失败时新数据被合并到垃圾上
- **验收**（2026-10-05 全部通过）：
  - [x] 故障注入（让 `dev->read` 对特定 LBA 返回错误）后，写操作返回 `EIO` 而非写入垃圾
        （`t_ioerr.c` 的 `test_write_refuses_an_unreadable_sector`：注入 `BLK_IO_READ|BLK_IO_WRITE`
        后 `fat_write_impl` 回 `-5`，数据扇区与目录扇区**快照逐字节相同**，
        随后文件读回仍是原内容；`test_create_reports_the_failure` 断言目录扇区同样不变）
  - [x] 正常路径 `make smoke` 通过（**150/150**，含新增的
        `t-005 injected I/O error reported` marker；`[ktest]` 63 cases / 1046 checks / 0 failed）

#### T-006 · 检查 `fat32_journal` 的返回值

> **❌ 任务已失效（2026-10-01 登记）**：`kernel/fs/fat32/fat32_journal.c`
> 已删除，写路径改走块缓存写回（`fat_write_sector` → `blk_cache_write`），
> 本卡描述的 32 条上限与 `begin`/`add` 不复存在。C14 一并作废。

- **触发**：C14 —— 32 条上限静默溢出
- **现状风险**：`add` 溢出返回错误，但**全部 6 个调用点丢弃返回值**（`fat32_file.c:135,164,194`；`fat32_meta.c:54,74,109,118,314,338,456,485,593`）。一次 1 MB 写要记 2048 个扇区，2016 个静默丢弃。`begin` 返回值同样被忽略，嵌套失败会去操作**外层**事务并提前清 `active`，把外层账目搞乱
- **成本**：~20 行，半天
- **验收**：
- [ ] 写一个超过 32 扇区的文件，明确返回错误而不是静默丢数据
- [ ] 嵌套事务场景（如 `fat32_meta.c` 的 `unlink`）不破坏外层事务

#### T-007 · 修块缓存的 LRU

> **✅ 已完成（2026-10-05）**：卡上"受害者永远是索引 0"其实低估了病情——第三遍
> "清空所有 `used`"之后确实返回 0，但下一次 miss 走第二遍拿到的是 1、2、3……
> 真正的病是**这套记账从来没记过"最近"**：命中只把 `used` 置真、没有任何地方按条目清它，
> 于是 64 个槽全部 `used` 之后，挑选只能"全体清零再取 0"，与谁刚被碰过毫无关系。
>
> **修法**（`kernel/fs/blk_cache.c` + `include/disk.h`）：
>
> | 改动 | 位置 | 说明 |
> |---|---|---|
> | `struct cache_entry.stamp` | `disk.h:98-113` | 单调计数器 `lru_seq`（`blk_cache.c:17`）的快照：命中 `:103`、填充 `:219`、暂存 `:260` 都 `++lru_seq` |
> | 受害者 = 最小 stamp | `blk_cache.c:118-127` | 先要空槽（拿空槽不丢任何东西），否则在全部常驻项里取 stamp 最小的那个；旧的三遍扫描整段删除 |
> | **删掉 `used` 字段** | `disk.h` 结构体、`blk_cache.c` 全文 | 两套记账互相打架正是 C24 的根因；现在只剩 stamp 一份，没有"第二份状态"能与之矛盾 |
> | `struct blk_cache_stats` + `blk_cache_stats_get/reset` | `disk.h:131-143`、`blk_cache.c:18-33` | `hits/misses/evictions/writebacks/dev_reads/dev_writes` —— 验收要的"统计"就是它 |
> | `blk_cache_lookup(dev, lba)` | `disk.h:147`、`blk_cache.c:281` | "这个扇区在不在缓存里"，**故意不碰 stamp**：问一句不能改变下一次淘汰谁 |
> | `blk_cache_purge(dev)` | `disk.h:153`、`blk_cache.c:298` | 写回 + 全部丢弃，给测试一个已知起点；写回失败时保留脏槽（宁可留着也不丢唯一一份数据） |
>
> 计数走 `__sync_fetch_and_add` 而不是锁（`blk_cache.c:23`）：`blk_io_*` 既在锁外被调
> （挂载、FAT 直写），也会在 `cache_writeback()` **持有 `cache_lock` 的同时**被调用，
> 在这里拿锁就是自锁。`dev_reads/dev_writes` 只在真正交给驱动之前递增（`:71,79`）——
> 注入出来的失败不算一次传输。
>
> **测试**：`ktest/t_cache.c`（5 例，suite `blkcache`，`KTEST_LATE`）
>
> - `test_lru_victim_is_the_least_recently_used`（`:100`）——`purge` 后灌满 64 槽，
>   回头碰 S0，再读第 65 个扇区：受害者必须是**最老的 S1**，不是刚碰过的 S0，
>   新来者必须落进 S1 空出来的槽。断言 `dev_reads=65 / hits=1 / misses=65 / evictions=1`
>   和 `lookup(S0)=0、lookup(S1)=-1、lookup(S63)=63、lookup(S64)=1`。
> - `test_a_200_sector_file_costs_one_writeback_each`（`:142`）——**验收要的"跨 200 扇区的
>   文件"**：512 B × 200 逐扇区写，writebacks **137 ≤ 208**（= 200 数据扇区 + 目录项余量），
>   即**没有任何扇区被写回两次**；随后每 37 扇区采样读回，purge **前后各比一次**。
> - `test_hot_sectors_survive_a_cold_stream`（`:203`）——8 个自建文件的扇区被重写 6 轮，
>   期间冲过 96 个只读冷扇区：writebacks **9 ≤ 12**、`dev_reads=96`（冷扇区各读一次）。
>   只有"淘汰脏槽"才花一次设备写，所以这个界等价于"热集从未被赶走"。
>
> **旧策略下这两个测试确实会红**（真做过：把 `cache_find_victim` 换回旧的三遍扫描重新编译
> 再跑）——`test_lru_victim_...` 三个断言失败（被 touch 的 S0 被赶走、S64 抢了槽 0）、
> `test_hot_sectors_...` writebacks **17 > 12**，`[ktest] selftest FAIL`、smoke 4 项 FAIL；
> 换回来又全绿。测试不是"必然通过"的摆设。
>
> **结果**：`make smoke` **155/155 PASS**；`[ktest] selftest PASS (68 cases, 1508 checks)
> [2 skipped]`（基线 63/1046，0 failed）；`[selftest] 1508/1508 pass [1 warn]`；
> `scripts/smoke_smp1.py` 62 tags / 0 failed。
> smoke 侧新增的 5 条判据放在 `LATE_REPORTS`（`smoke_test.py:97`，在整段日志里搜）而不是
> `BOOT_MARKERS` 里顺序等：`.ktest` 段内的先后是编译器的事（本机实测 `t_cache.c` 的 5 例
> **倒序**打印），拿它当 boot 时序会假红。

- **触发**：C24 —— 受害者永远是索引 0
- **现状风险**：`cache_find` 命中时设 `used=true` 且**永不清除**；`cache_find_victim` 前两遍找不到就走第三遍"清空所有标志"再返回 → 永远返回索引 0。超过 64 扇区的工作集（一次 1 MB 写就是 2048 扇区）**永久抖动同一槽位**（行号为修复前的坐标）
- **位置**：`kernel/fs/blk_cache.c:24-59`
- **成本**：~5 行（改成"命中时清 `used`"）或 ~30 行（改成带时间戳的真正 LRU）
- **验收**（2026-10-05 全部通过）：
  - [x] 写一个跨 200 个扇区的文件，用 `blk_list_all`/统计确认 `write` 系统调用次数显著下降
        （卡上写 `blk_list_all` 是因为它当时是唯一的统计口；现在有真的计数器
        `blk_cache_stats()`：200 扇区文件 **137 次回写 ≤ 208**；8 扇区热集被重写 6 轮、
        期间冲过 96 个冷扇区，只回写 **9 次 ≤ 12**。把受害者换回旧算法同一组断言变红 → 17 次）
  - [x] 旧的三遍扫描删除，`used` 字段一并移除（只剩 `stamp` 一份记账）

#### T-008 · FAT 读路径改走块缓存

> **✅ 已完成（2026-10-05）**：卡上的现场**已经过时**——`fat32_file.c:70` 早就不走
> `m->dev->read`（读扇区一律 `fat_read_sector`/`fat_read_cluster` → `blk_cache_read`），
> `fat32_journal` 也已删除，所以"靠 `fat32_journal_commit` 最后那次 flush 偶然掩盖"
> 不再成立。真正要做的不是再改一遍调用点，而是把**每个 LBA 类只有一条路径**这条
> 不变式写下来、并让测试抓得到它的破坏。
>
> **现在的三条路径**（全内核直调 `dev->read/write` 的只剩 `blk_cache.c:67,75` 两处）：
>
> | 区域 | 读 | 写 | 为什么这样 |
> |---|---|---|---|
> | 数据/目录（≥ `data_start_lba`） | `fat_read_sector`/`fat_read_cluster` → `blk_cache_read`（`fat32_priv.h:80,91`） | `fat_write_sector` → `blk_cache_write`（写回缓存） | 读写同一条路，读到的必是本层最后一次写的字节 |
> | FAT 区 `[reserved, reserved + num_fats*fatsz)` | `m->fat`（挂载时 `fat32.c:310` 整表读进内存） | `fat_set_entry` → `blk_io_write` **直写**（`fat32.c:114,126`） | 分配必须先落盘，才能被指向它的目录项暂存 |
> | 挂载期（BPB `:247`、FAT 表 `:310`、MBR `mbr.c:42`） | `blk_io_*` 直读 | — | 发生在任何缓存流量之前 |
>
> **卡上的"注意"我们有意不照做**：`fat_set_entry` **没有**改走缓存，理由是**顺序**——
> 直写让"簇已分配"先于"目录项指向它"存在；改成走缓存后两者都成了暂存态，而
> `blk_cache_flush` 按槽号顺序回写，就可能出现**目录项先于 FAT 落盘**的窗口
> （崩溃后指向一个仍标着空闲的簇）。而"不一致"真正的风险——缓存里躺着一份没人刷新的
> FAT 副本——由不变式加测试封死：**只要 FAT 区永远不进缓存，直写就不可能绕过缓存里的
> 一份陈旧副本**。
>
> **测试**（同 `ktest/t_cache.c`）：
> - `test_write_readback_is_coherent`（`:269`）——**验收要的"写后立即读回、重复 1000 次"**：
>   每轮图案都不同（头两字节就是轮数），1000 轮逐轮比对；然后 `blk_cache_purge()`
>   写回并丢弃全部条目，**再读一次字节必须仍然相同**（这一次只能来自设备）。
> - `test_a_200_sector_file_...`（`:183`）：同一个文件 purge **前后**各采样读回一次。
> - `test_fat_region_is_never_cached`（`:315`）：FAT 区 **2018 个扇区**逐个
>   `blk_cache_lookup()` 必须都是 `-1`。谁把一次 FAT 扇区读改成走缓存，这条立刻变红。
>
> **结果**：`make smoke` **155/155 PASS**；`[ktest] selftest PASS (68 cases, 1508 checks)
> [2 skipped]`；`[selftest] 1508/1508 pass [1 warn]`；`scripts/smoke_smp1.py` 62 tags / 0 failed。

- **触发**：C26 —— 读绕过缓存、写不绕过 ⇒ 读可能看到陈旧数据
- **现状风险**：`fat32_file.c:70` 用 `m->dev->read`，而 `:160,165,183,195` 用缓存；`fat_dir`/`dirent_locate`/`dir_claim_run`/`fat_set_entry`/`mbr_parse` 也都是直接打设备。现在靠 `fat32_journal_commit` 最后那次 flush 偶然掩盖，不是设计（行号为修复前的坐标，journal 已删除）
- **成本**：~20 行，半天
- **注意**：改完必须确认 `fat_set_entry`（`fat32.c:28-29`）的 FAT 扇区写也统一走缓存，否则会不一致
- **验收**（2026-10-05 全部通过）：
  - [x] `fsck.vfat` 干净 —— smoke 第 6 阶段 `fsck.vfat -n`（`smoke_test.py:364-366`）4 项 PASS
  - [x] 写后立即读回内容一致（重复 1000 次）—— `test_write_readback_is_coherent`：
        1000 轮逐轮比对 + purge 后从设备读回仍一致
  - [x] 上面的"注意"按**保持直写 + 守住不变式**落地（理由见上方完成记录），
        `test_fat_region_is_never_cached` 是那条不变式的守卫

#### Step 1 完成定义

- [ ] T-001 ~ T-008 全部完成
- [ ] `make smoke` 在 `-smp 1` 和 `-smp 2` 下都通过
- [ ] 故障注入测试存在：能验证 I/O 错误、OOM、坏簇三条路径
- [ ] 每个修复是一个可独立撤销的改动单元，说明里引用缺陷编号（C1/C11/C12/...）

---

### 24. Step 2 · 内核自测框架（1 周）★

**目标**：把"启动冒烟"升级成"能断言内核内部状态"。**这一步决定后面所有工作是否可能。**

**为什么不能跳过**：差距分析里 C 类的 66 条缺陷**全部是"读"出来的，没有一条是"跑"出来的**。没有测试，接下来每一个并发修复和 MMU 改动都是盲改。

#### T-010 · 断言基础设施

- **成本**：半天
- **做法**：
  - `include/console.h` 加 `ASSERT(cond, fmt, ...)` / `WARN(cond, fmt, ...)` / `BUG_ON(cond)`，失败时打印文件/行号/表达式
  - 一个全局 `selftest_pass` / `selftest_fail` 计数器
  - 失败计数 > 0 时在启动日志末尾打印 `[selftest] FAILED` 并停机（这样冒烟测试的 grep 就能抓到）
- **验收**：`ASSERT(0, "x")` 能打印文件行号并让 `make smoke` 失败

#### T-011 · L-单元测试（内核态纯函数）

- **成本**：1–2 天
- **目标**：覆盖不需要硬件的纯逻辑
- **首批用例**（按投入产出排序）：

| 被测对象 | 位置 | 断言要点 |
|---|---|---|
| `bin_of` | `kheap.c:29-34` | 边界：1, 8, 16, 17, 4096, 4097 |
| `fb_pack` / `fb_rgb_px` | `framebuffer.c:157-196` | 真彩色往返：对若干 `0x00RRGGBB`，`fb_rgb_px` 在每种位域布局 × 每种 bpp 下都还原同一颜色（8bpp 量化到 6×6×6 立方） |
| `fbc_esc_process` | `framebuffer.c:295-346` | 越界行/列被钳制、`ESC[2J` 复位光标、未终止序列不越界 |
| `ansi_feed` 状态机 | `ansi.c` | 全部支持的序列 + 截断 + 未终止序列 |
| `fat_short_to_name` | `fat32_dir.c:5-14` | 8.3 → 显示名的边界（11 字符、含 `.`、全空格） |
| `abs_path` | `vfs.c:106` | 相对/绝对/超长（**✅ 2026-10-03 已覆盖**：`t_fs.c` 的 `test_abs_path` 钉住 29 字符边界与超长拒绝，`t_vfs.c` 的 `test_path_too_long` 钉住调用点回 `ENAMETOOLONG`；C46 已修，见第一部分 C46）。**另注**：同处提到的"截断到 1024 而 `dir_getdent` 要求 `len>=72`"是 **C44**（`sys_getdent`），不是 C46，仍未修 |
| `fat_name_eq_short` | `fat32_dir.c:16-23` | 大小写不敏感 |
| buddy 不变量 | `pmm.c` | 分配/释放配对后 `free_pg` 回到原值 |
| 页表 walk 往返 | `vmm.c` | map → translate → unmap → translate == 0 |

- **验收**：
  - [x] 全部用例通过，`[selftest] N/N pass`
  - [x] 故意把 `fb_pack` 的一个位移改错，测试必须失败（**验证测试真的有断言能力**）

#### T-012 · L-内核自测（带状态的不变量）

- **成本**：2–3 天
- **做法**：把 T-011 的框架扩展到能在启动时跑的一组用例，检查跨模块不变量
- **首批用例**：

| 不变量 | 检查方法 |
|---|---|
| PMM 分配/释放配对 | 分配 N 帧再全部释放，`free_pg` 必须回到初始值；buddy 合并不变量 |
| kheap 分配/释放配对 | 混合大小分配后全部释放，`heap_used` 回到 0 |
| 页表 map/unmap 往返 | 建 aspace → map 一片 → translate 全部命中 → unmap → translate 全部为 0 |
| USERCOPY 边界 | 对越界地址、未映射页、跨页区间、整数溢出区间调用 `copy_from_user`/`copy_to_user`，必须全部失败且不 panic |
| `user_ptr_ok` 单调性 | 未映射页的任意子区间必须全部失败 |
| ELF 解析健壮性 | 构造畸形 ELF（`e_phoff` 越界、`p_filesz > p_memsz`、`e_entry` 越界、`e_type` 错误）必须全部拒绝且不 panic |
| 帧缓冲控制台裁剪 | 行尾/列尾写入不越界；`fbc_scroll` 后首行恰是原第二行 |
| 绘制裁剪 | `fb_fill_rect` / `fb_draw_char` 传屏幕外坐标时零写入（边界像素读回不变） |

- **验收**：
  - [x] `[selftest] N/N pass` 出现在串口日志中（可被 `smoke.sh` 抓取）
  - [x] 任意一个不变量被人为破坏时，测试失败
  - [x] 自测本身不 panic（20 次连续启动，`[selftest]` 全部 1072/1072、零 panic）

#### T-013 · L-属性测试（随机化输入）

- **成本**：1 天
- **理由**：差距分析里大量缺陷是**边界/截断/溢出类**（C16 `nclus` 截断、C30 溢出、C31 缓冲、C32 `fat_entries` 溢出、C33 区间语义错误）。这些靠手写用例很难穷尽
- **做法**：对页表映射、`USERCOPY`、路径长度、ELF 字段、FAT 目录项做伪随机输入，断言两条不变式：
  1. "任意输入不 panic"
  2. "任意输入不越界"（用 canary 页夹住被测缓冲区，事后扫描）
- **验收**：随机种子固定（可复现），跑 10000 次无 panic、无 canary 破坏

#### Step 2 完成定义

- [x] `make smoke` 会检查 `[selftest] N/N pass` 且 N == 总数
- [x] 故意注入 3 个不同类型的 bug（位运算错误、边界 off-by-one、NULL 未检查），测试**全部能抓到**
- [ ] 属性测试跑 10000 次无 panic
- [x] 自测覆盖至少 9 个模块（表 T-011/T-012 中的全部对象）

**注入证据**（每条都让 `make smoke` 打红，注入已还原、最终 97/97、自测 1072/1072）：

| 注入类型 | 改动 | 抓到它的东西 |
|---|---|---|
| 位运算错误 | `fb_pack` 红通道 `<< (fb_r_pos + 1)` | `fb.test_fb_pack_layouts` 按布局报 `got/want` |
| 边界 off-by-one | `kfree` 的 `heap_used -= 1UL << (bin + 5)` | `kheap.test_kheap_used_pairing` → mm 阶段标记 + `[selftest] FAILED` + panic |
| NULL 未检查 | 去掉 `vfs_file_size` 对 `priv` 的保护（C42） | `vfs.test_char_device_size` 报 `got=0xf000ff53f000e2c3` |
| 断言本身 | `ASSERT(0, "x")` | `[selftest] FAIL kernel/main.c:302: 0: x` + `[selftest] FAILED` |

---

### 25. Step 3 · 硬安全边界（1 天）★★

**目标**：让内核第一次拥有一个**硬件强制**的用户/内核边界。

**这是整个计划里投入产出比最高的一项。** 差距分析核实过：`EFER.NXE`、`CR0.WP`、`CR4.WP`、`SMEP`、`SMAP` **一个都没开**。后果是：所有用户页可执行、只读段实际可写、ring-3 可执行 ring-0 文本。

**必须在 Step 2 之后**（见第 20 节原则 3：打开 WP 会让潜在的只读页写入 bug 立刻变成页错误风暴，没有测试就分不清是新引入还是原有的）。

#### T-020 · 给用户页加 NX 位

- **成本**：半天
- **位置**：`include/mm/mm.h:35-40`（缺 `PG_NX`）、`kernel/mm/vmm.c:57-75`（`vmm_map_user`）
- **关键点**：**光打开 NXE 没有任何用。** 必须先在 PTE 里设 NX 位
  - `include/mm/mm.h` 加 `#define PG_NX 0x800`
  - `vmm_map_user` 的 `pt[i1] = pa | PG_P | (writable?PG_W:0) | uf;` 加上 `| PG_NX`
  - `dup_user_aspace`（`vmm.c:312`）用 `npa | (spt[i1] & 0xFFF)` 复制标志，**NX 位会被自动带上** ✓
- **验收**：
  - [ ] 自测里加一条：往用户栈写入一段 `ret` 指令后跳过去，必须 `#PF` → `SIGSEGV`
  - [ ] `readelf -l build/usr/sh` 的 text 段（只读）实测拒绝写入

#### T-021 · 打开 `EFER.NXE`

- **成本**：10 分钟
- **位置**：`arch/entry64.S:119-124`（QEMU 路径）、`arch/trampoline.S:68-71`（GRUB 路径）
- **做法**：`or eax, 0x100` 改为 `or eax, 0x100 | 0x800`
- **注意**：`ap_main`（`smp.cpp:198-206`）继承的是 BSP 的 EFER，**但 trampoline 走的 GRUB 路径要单独设**——两处都要改，否则 GRUB 启动时 NXE 未开
- **验收**：
  - [ ] `/proc/cpuinfo` 风格的启动打印里显示 NX 使能状态（顺带解决"怎么确认它真的开了"）
  - [ ] T-020 的 NX 测试通过
  - [ ] `make smoke` 通过（内核自身代码不受影响——内核从不设 NX）

#### T-022 · 打开 `CR0.WP` + `CR4.WP`

- **成本**：10 分钟
- **位置**：`arch/entry64.S:128-129`（CR0）、`:70-71` 和 `:259-261`（CR4）、`arch/trampoline.S:63-76`、`kernel/smp.cpp:198-206`
- **做法**：
  - CR0：`or eax, 0x80000000` → `or eax, 0x80000000 | 0x10000`
  - CR4：`or eax/rax, 0x20` → 加 `| 0x10000`
- **这一步的风险最高**（原则 3），所以：
  - **先跑一遍 Step 2 的全部自测**，记录基线
  - 打开后**再跑一遍**，任何失败都是被 WP 暴露出来的既有 bug，**必须逐个查清而不是简单关掉 WP**
- **我预判会被暴露的地方**（值得提前排查）：
  - `elf.c:60` `memcpy((void*)ph[i].p_vaddr, ...)` —— 目标段若为只读 text 会 fault（正确行为，但 ELF 加载器不该写只读页，需要检查 `p_offset`/`p_vaddr` 的页归属）
  - `kernel_spawn`（`task.c:382`）`strcpy` 到用户栈 —— 栈是 `writable=true` ✓
  - `dup_user_aspace`（`vmm.c:312`）经 `PHYS_TO_VIRT` 写新页 —— 那是内核侧，WP 不影响（WP 只影响 ring-3 写和 ring-0 写只读页；`PHYS_TO_VIRT` 的高半别名是 `PG_W` ✓）
  - `framebuffer.c:512` 用 4 KiB 页映射 LFB —— `vmm_map_kernel_page` 带了 `PG_W` ✓
- **验收**：
  - [ ] 自测全部通过
  - [ ] 用户程序尝试写自己的只读 text 段 → `SIGSEGV`
  - [ ] `make smoke` 通过

#### T-023 · 打开 `SMEP`

- **成本**：10 分钟
- **位置**：`kernel/smp.cpp:206`（`or rax, (1<<9)|(1<<10)` → 加 `(1<<20)`）、`arch/entry64.S:259-261`（`or rax, 0x20` → 加 `0x100000`）
- **现状（比原计划简单）**：信号用户态处理器整条链路（handler / trampoline / `sigreturn`）
  已经不存在，`do_signal_check` 只做 stop/continue/kill，**永远不会把 `tf->rip` 改成
  用户地址**。所以 SMEP 没有"内核误跳用户页"的风险面，只需要验证它不误伤正常路径。
  - `do_signal_check` 的两个调用点（`isr.c:209`、`syscall.c:198`）都有
    `if (current && (f->cs & 3) == 3)` 守卫
- **验收**：
  - [ ] `SIGKILL` / `SIGSTOP` / `SIGCONT` 投递测试通过（T-024）
  - [ ] 自测全部通过

#### T-024 · 补安全边界的测试

- **成本**：1 天（与 T-020~023 并行做）
- **新增测试用例**（这批测试是**长期资产**，因为它们守着安全边界）：

| 测试 | 断言 |
|---|---|
| `test_nx_stack` | 用户栈上的代码不可执行 |
| `test_nx_heap` | `brk` 出来的内存不可执行 |
| `test_ro_text` | 只读 text 段拒绝写入 |
| `test_smep` | ring-3 无法通过内核文本地址执行（构造一个指向 `__kernel_start` 的函数指针并调用） |
| `test_signal_delivery` | `SIGSTOP` 停住进程、`SIGCONT` 恢复、`SIGKILL` 终止；handler 路径不存在时 `kill` 不改变 `tf->rip` |
| `test_userptr_boundary` | `user_ptr_ok` 在所有边界（`USER_BASE`/`USER_MAX_VMA` 上下各 1 页、跨页、整数溢出）都正确 |

#### Step 3 完成定义

- [ ] NXE / CR0.WP / CR4.WP / SMEP 全部开启，且启动日志打印确认
- [ ] T-024 的 6 个测试全部存在且通过
- [ ] 自测无回归
- [ ] `make smoke` 在 `-smp 1` 和 `-smp 2` 下都通过
- [ ] **这个内核第一次拥有硬件强制的用户/内核边界**

---

### 26. Step 4 · 特权模型与资源限制（2 周）

**目标**：从"所有进程都是 root"变成"有权限边界"。

**依赖**：Step 3（否则权限模型没有意义——没有内存安全边界时，权限检查可被绕过）

#### T-030 · `struct cred` 与能力位

- **成本**：1 天
- **做法**：
  - `struct task` 加 `struct cred { u32 uid, gid; u64 caps; } cred;`
  - 三类能力：`CAP_KILL`、`CAP_SYS_ADMIN`（diskinfo/fdisk/ps/brs 扩张）、`CAP_FB`（帧缓冲）
  - `fork` 继承、`execve` 按 ELF `e_uid`/`e_gid` 设置（需要 `elf.c` 读 `e_uid`/`e_gid`，目前只读 `e_type`）
  - `kernel_spawn("/bin/init")` 为 uid 0 + 全能力
  - 用户程序默认 uid 1000 + 无能力
- **验收**：`ps` 输出能看出 uid 差异
- **✅ 已完成（2026-10-03 收口）**：`kind`/`uid`/`caps` 三件套 + `cred_capable()`
  （root/kxld 天生全能力）+ `kernel_spawn` root + `fork` 继承都已落地，
  `ps` 能打印 `kind`/`uid`（`ps.kind.idle=2`、`ps.uid.user=1000` 都有断言）。
  **唯一改动的子项**：`execve` 按 ELF `e_uid`/`e_gid` 这条**做不到**——ELF64 的
  `Elf64_Ehdr` 里没有 `e_uid`/`e_gid` 字段（那是文档的想当然），改成由 T-031 的
  `SYS_setuid` 提供"往下降"的路径，见下。

#### T-031 · 给敏感系统调用加检查点

> **✅ 已完成（2026-10-03，分三天落地，下面的清单逐条打勾）**
>
> | 落点 | 门 | 备注 |
> |---|---|---|
> | `sys_kill` 单播 | kxld 规则（pid 1 / kxld 不可动，root 也不行）→ 同 uid 或 `CAP_KILL` | 第 1–2 天；`kernel/protect.c` + `syscall.c:kill_allowed` |
> | `sys_kill(-1, ...)` | `CAP_KILL`（广播本身就是特权；`sig == 0` 探针同样要） | 第 3 天补上，原先 `pid == -1` 直接 `return 0` |
> | `sys_ps` / `SYS_diskinfo` | `CAP_SYS_ADMIN` | 第 3 天 |
> | `fb_clear` / `fb_fill` / `fb_char` / `fb_puts` | `CAP_FB` | 第 3 天；`fb_info` 只读几何信息，**有意不挡** |
> | `sys_fdisk` | —— | 这棵树里已没有 fdisk（`usr/fdisk.c` 删除、`.def` 无编号），它当年的全部功能就是列块设备 = `SYS_diskinfo`，挡住那个即覆盖 |
> | 文件写操作（`mkdir`/`unlink`/`rmdir`/`rename`） | 未做 | 需要"文件所有者"这个 inode 属主概念，A1 的第四条，留给有属主元数据时 |
>
> **降权机制**：没有普通用户进程，"普通用户被拒"就无法端到端证明——原计划的
> `execve` 按 ELF `e_uid` 也走不通（ELF64 头里根本没有这个字段）。改为新增
> **`SYS_setuid`（编号 31，`.def` 追加、`LNXRM_SYSCALL_LAST` 31，编号规则允许）**：
> root 可以命名任意 uid，但命名非 0 uid 会连 `kind` 一起降到 `CRED_USER` 并清空
> caps，**单向**（用户态再喊 `setuid(0)` 得 `EACCES`）。`systest` fork 出的子进程
> 降权后逐个撞检查点，父进程保持 root 作对照。
>
> **回执**：每个拒绝都打一行 `[cap] denied <call>: pid .. lacks CAP_*`，与
> `[kxld] denied kill` 同风格——否则"拒绝"和"功能坏了"在串口上无法区分。
>
> **测试**：`ktest/t_cred.c`（7 个用例，开机即断言"放行"与"拒绝"两半，
> 不依赖用户态存在）+ `usr/systest.c` 的降权子进程段 + `scripts/smoke_test.py`
> 新增 24 条 tag + `scripts/smoke_smp1.py`（`-smp 1` 用，见下）。

- **成本**：1 天
- **位置**：`kernel/syscall.c:37-56`（kill）、`:143-146`（ps）、`:153-155`（diskinfo）、`sys_mkdir`/`sys_unlink`/`sys_rmdir`/`sys_rename`/`sys_move`（`fdisk` 会调用）
- **做法**：
  - `sys_kill`：`pid == -1` 或跨 uid 需要 `CAP_KILL`
  - `sys_ps` / `SYS_diskinfo` / `sys_fdisk`：需要 `CAP_SYS_ADMIN`
  - 文件写操作：需要文件所有者或 `CAP_SYS_ADMIN`
- **验收**（2026-10-03 全部通过，每条都有 tag 守着）：
  - [x] 普通用户 `kill -1` 被拒（`user.kill.all=-13` / `user.kill.all.probe=-13`）
  - [x] 普通用户 `kill` 别人被拒（`user.kill.parent=-13`，同 uid 探针 `user.kill.self=0` 仍通）
  - [x] 普通用户 `ps` / `diskinfo` 被拒（`user.ps=-13` / `user.diskinfo=-13`）
  - [x] 普通用户 `fdisk` 被拒（并入 `user.diskinfo=-13`，见上表）
  - [x] 普通用户画屏被拒、读几何仍可（`user.fb.clear/fill/char/puts=-13`、`user.fb.info=0`）
  - [x] root 一切照旧（`root.ps=1` / `root.diskinfo=1` / `root.fb.*=0` / `root.kill.all.probe=0`）
  - [x] `init` 作为 pid 1 仍能管理子进程（`kill.child.*`，第 1–2 天）
  - [x] `ps` 能看出降权后的 tier（`ps.kind.user=0` / `ps.uid.user=1000`）
  - [x] `make smoke` **135/135**；`-smp 1` 下 49/49 tag（`scripts/smoke_smp1.py`，
        `smoke_test.py` 的 BOOT_MARKERS 是 `-smp 2` 专属的 AP 检查，见该脚本注释）

#### T-032 · `brk` 配额与内存记账

> **✅ 已完成（2026-10-05）**。卡上四条做法照做了三条，第四条
> （"`exit` 时释放全部记账"）**按真实生命周期改了语义**：地址空间是
> `waitpid` 收尸时才 `vmm_destroy_user_aspace`（`task.c` 的 reap 路径），
> zombie 在被收走之前仍然占着页。所以记账跟着**地址空间**走而不是跟着
> `exit` 走 —— `task_free_slot` 归零、`task_alloc_slot` 复位（不继承上一个
> 占用者的读数）。`ps` 因此会显示 zombie 的**最终**大小，测试正是靠这个读数。
>
> **三个决策点**（每条都是"两份记账必然分叉"的反面）：
>
> | 决策 | 取值 | 理由 |
> |---|---|---|
> | 记账的真相 | **页表行走** `vmm_count_user_pages(root)`（`vmm.c` 末尾） | 不在各映射点手工累加。exec / fork / spawn / brk 三处一律按表刷新，谁漏写一处就不会变成"ps 说谎"（C24 的教训：访问时清零只换来时钟轮转，没有"最近"信息） |
> | 配额放哪 | `cred.h` 的 `static inline cred_mem_quota(c)` 一处 | 不给 `task` 加 per-task 配额字段：`SYS_setuid` 会改 tier，两套记账会打架。`kind >= CRED_ROOT`（root / kxld）返回 `0` = 无上限，user 返回 `CRED_USER_MEM_QUOTA_PAGES`（64 MiB），`cred == NULL` 也返回 64 MiB（**fail-closed**） |
> | 配额覆盖谁 | **整个用户地址空间**（image + 栈 + restorer + 堆） | 与 `RLIMIT_AS` 同语义。只管堆的话，64 MiB 的堆叠在任意大的 image 上照样能把机器吃干 |
>
> `sys_brk` 在**取帧之前**拒绝（`have + (tgt-cur)/PAGE_SIZE > quota →
> LNXRM_ENOMEM`），**shrink 永不拒绝**，`mem_peak` 只在成功路径抬升。
> T-004 的局部回卷逻辑（失败即归还本次已映射的页）原样保留，配额检查只是
> 在它之前多了一道更便宜的门。
>
> **ABI 扩展走 `ps` 而不是 `SYS_diskinfo`**：`struct lnxrm_ps_entry` 加
> `int32_t mem_kib` / `int32_t peak_kib`（40 → 48 B，`_Static_assert` 同步），
> `usr/ps.c` 因此多出 `MEM` / `PEAK` 两列。`sys_ps` 要 `CAP_SYS_ADMIN`，
> 所以**读数一律由 root 父进程取**；普通子进程只回报 `kbrk` 自己告诉它的值。
>
> **测试**：
> - `ktest/t_mm.c` `test_vmm_count_user_pages` —— 0/1/2 页、重映射仍 2、
>   unmap 回 1、前后 `pmm_free_bytes()` 相等。
>   ⚠️ **`vmm_destroy_user_aspace()` 会释放仍映射的帧**，所以必须先 unmap 再
>   `pmm_free`，否则就是对 buddy 的双倍释放 —— 这条真把内核打出过
>   `#GP pmm_alloc+0x4b`（buddy 空闲链表被毒化），是断言 + 这行注释一起守着的。
> - `ktest/t_cred.c` `test_mem_quota_follows_the_tier` —— 三档 tier 的
>   配额、`cred == NULL` fail-closed、`cred_setuid` 之后配额跟着 tier 变。
> - `usr/systest.c` T-032 段（**`systest.done` 之前**）：父进程量自己
>   （`mem.kib.base` / `mem.delta.kib=8192` / `mem.back` / `mem.peak.holds`），
>   子进程 `setuid(1000)` 后 1 MiB 步进撞墙（`quota.oom=-12`、
>   `quota.rounds=63`、`quota.alive=1`、`quota.shrink=1`），父进程轮询 zombie
>   读它的终值（`quota.mem.kib=64548`、`quota.mem.peak=64552`），再验
>   `mem.parent.unchanged` 与收尸后的 `mem.slot.clean=1`。
>   **标签前缀刻意分成 `mem.*`（父）与 `quota.*`（子）**：叫 `user.brk.oom`
>   之类会匹配既有判据 `brk\.oom=-12`，把回归掩成"通过"。
>
> **结果**：`make smoke` **169/169 PASS**（连跑两次）；`scripts/smoke_smp1.py`
> 76 tags / 0 failed；`[ktest] stage mm: 15 cases, 429 checks, 0 failed`；
> `[ktest] selftest PASS (70 cases, 1530 checks) [2 skipped]`；
> `[selftest] 1530/1530 pass [1 warn]`（warn 仍是启动那 1 条，未新增 `WARN()`）；
> smoke 第 6 阶段 `fsck.vfat -n` 4 项 PASS。

- **触发**（T-032 之前的状态，下述两半都已修复）：C12 后半段 —— `brk` 既无上限也不计数，内存耗尽是无归因的全局致命事件
- **成本**：1 天
- **依赖**：T-004（`kmalloc` 返回 NULL）必须先完成 —— **✅ 2026-10-03 已满足**
- **做法**（2026-10-05 落地）：
  - `struct task` 加 `u64 mem_pages`、`u64 mem_peak`（紧邻 `brk_cur`）
  - `sys_brk`（`kernel/syscall.c`）增长前查 `cred_mem_quota()`，超限返回 `ENOMEM`
  - 默认配额 64 MiB（`CRED_USER_MEM_QUOTA_PAGES`），root / kxld 无限制
  - 记账随**地址空间**释放（`task_free_slot` 归零 + `task_alloc_slot` 复位），
    而非字面的 `exit` —— 见上方完成记录的说明
- **验收**（2026-10-05 全部通过）：
  - [x] 一个用户程序循环 `brk` 到失败，返回 `ENOMEM`，内核存活 ——
        `quota.oom=-12` + `quota.alive=1`（子进程自己还活着），
        `quota.shrink=1` 证明是天花板不是把 buddy 掏空（这台机 256 MiB，
        root 侧 `brk.oom` 段要 200 MiB 才会真见底）
  - [x] 能回答"谁占的 RAM" —— `ps` 加 `MEM`/`PEAK` 两列（48 B 的 `lnxrm_ps_entry`），
        `mem.kib.base` / `mem.delta.kib=8192` / `quota.mem.kib=64548`
  - [x] fork 出的子进程内存独立记账，超额时子进程 `ENOMEM` 而父进程不受影响 ——
        `mem.parent.unchanged=1`；且收尸后 `mem.slot.clean=1` 证明记账随地址空间走

#### T-033 · 帧缓冲归属强制（重新生效：23–27 已恢复）

> **✅ 已完成（2026-10-05）**。卡上给了两条路——owner 归属强制，或干脆上双缓冲——
> 选了第一条：卡片标题就是"归属强制"，而双缓冲要凭空造出 3.5 MB 的第二份缓冲、
> 一个 `fb_buffer_set` 编号和"谁的缓冲此刻在屏上"的仲裁，成本是它的三倍，
> 解决的却是同一个问题——**两个进程互相覆盖对方的绘图**。
>
> **没有新增任何编号**：`systest` 的 `past.32/33/34 = -38` 和 `t_abi.c` 的
> 连续性断言都守着 0–31 这张表，归属只需要在已有的 4 个绘图入口后面多一道门。
>
> **三层，各管一件事**：
>
> | 层 | 位置 | 管什么 |
> |---|---|---|
> | 能力（T-031） | `cap_gate(CAP_FB, ...)` | 这个进程**够不够格**画屏 —— 普通用户止步于此（`user.fb.*=-13`） |
> | 归属（本条） | `fb_owner_gate(...)` → `fb_claim()` | 够格的里面，**哪一个**正在屏上 —— 第一个成功的绘图调用占住，后来者回 `-13` |
> | 原语 | `fb_clear`/`fb_fill_rect`/`fb_draw_char`/`fb_puts` | **谁都不查**。控制台和用户态共用这四个，一个能被用户进程拒绝的控制台就是一个能被用户进程弄哑的控制台 |
>
> 两道门的**顺序是硬的**：先能力后归属。反过来普通用户会收到"你不是 owner"，
> 而它连碰都不该碰——`user.fb.clear=-13` 必须还是那句完整的话。`sys_fb_info`
> 两道门都没有：它只读几何，而且一个学不会屏幕尺寸的进程根本没资格选画哪儿。
>
> **状态记在驱动里，不记在调度器里**（`static u32 fb_owner`，`framebuffer.c` 末尾）：
> 它是"这块屏归谁"，不是"这个任务有什么"。对外只有三个函数——`fb_claim` /
> `fb_disown` / `fb_owner_get`。
>
> **释放只有一处——`sys_exit`**：所有死法（`exit` 系统调用、`SIGKILL`、默认终止的
> 信号、无人处理的 #PF/#GP）最后都落在这里，而 pid 由只增不减的 `next_pid()` 发，
> 永不复用，所以一个释放点覆盖全部。`task_free_slot` 里**故意不放第二份**——
> 今天所有 free 路径要么是从未跑过的胚胎槽，要么是已经走过 `sys_exit` 的 zombie，
> 那一行永远不会执行，留着就是又一条要靠注释解释的死代码。
>
> **拒绝不改变归属**：否则被挡下的那个反而成了 owner，真正画屏的进程下一次调用
> 会被拒绝。`fb_claim` 用 CAS 而不是 check-then-set，也是因为两个 CPU 能同时看到
> 空闲屏——普通写法会两边都发"可以"，只存下最后落笔的那个。
>
> **测试**：
> - `ktest/t_fb.c` `test_fb_owner_is_one_pid_at_a_time`（suite `fb`）：
>   占住 → 自己再占仍成 → 别人被拒**且什么都没拿走** → 非持有者 `fb_disown`
>   不清屏（这正是输家退出时会发出的那个调用）→ 持有者离开后下一个能占。
>   首尾两条断言是承重的：开头断言"还没人画过"——否则留下的 owner 会让
>   `systest` 第一个 `fb.fill` 莫名其妙变红，失败现场离病因十万八千里；
>   结尾断言"测试把屏还成了原样"。
> - `usr/systest.c` T-033 段（**插在四个绘图原语之前**，屏还是空的）：
>   父进程全程不画——归属随持有者退出而消失，所以持有者必须是那个**能退出**的
>   人，父进程得活到最后才能报数。三层进程用 `kfork` 嵌套，**排序全靠
>   `kwaitpid`**（子进程的行在父进程 waitpid 返回前必然已在控制台上），不靠
>   sleep，任何 CPU 数下都不会读错序：`fb.owner.grab=0`（占）→
>   `fb.owner.denied=-13`（同为 root 的兄弟被挡）→ `fb.owner.held=0`
>   （兄弟退出**没有**释放它的屏）→ `fb.owner.released=0`（持有者退出后下一个能画）。
>
> **结果**：`make smoke` **174/174 PASS**（连跑两次，169 + 5 条 T-033）；
> `scripts/smoke_smp1.py` **81 tags / 0 failed**（76 + 5）；
> `[ktest] stage late: 33 cases, 779 checks, 0 failed`（+1 case、+12 checks）；
> `[ktest] selftest PASS (71 cases, 1542 checks) [2 skipped]`；
> `[selftest] 1542/1542 pass [1 warn]`（warn 仍是启动那 1 条，未新增 `WARN()`）；
> 串口回执一行 `[fb] denied fb_fill: pid 13 holds the surface, pid 14 draws`。
> **既有判据一条没动**：`fb.fill=0` / `fb.clipped=0` / `fb.clear=0` /
> `user.fb.clear=-13` / `root.fb.clear=0` 全是原值。
>
> **没做的（仍开着）**：双缓冲（`fb_buffer_set`）与图形合成路径——C2/C6 编号继续保留。

- **状态**：~~**重新需要。**~~ 已完成（见上方记录）。
- **编号保留**（C3 / T-033 是稳定 ID，不重排）。
- **✅ 2026-10-03 完成一半（`CAP_FB` 已恢复）**：`fb_clear/fill/char/puts` 现在要
  `CAP_FB`，普通进程根本画不了屏（`user.fb.*=-13` 有断言），`fb_info` 保持只读开放。
- **✅ 2026-10-05 完成另一半（owner 归属强制）**：~~拿到 `CAP_FB` 的两个 root 进程
  之间依旧没有归属，互相覆盖照旧——owner 归属 / 双缓冲未做。~~ 归属已做强制，
  双缓冲按上方"没做的"一节有意不做。

- **验收**（2026-10-05 全部通过）：
  - [x] 两个都有 `CAP_FB` 的进程不能互相覆盖绘图 —— 同为 root 的兄弟画屏
        回 `-13`（`fb.owner.denied=-13`），串口回执指名道姓：`[fb] denied fb_fill:
        pid 13 holds the surface, pid 14 draws`
  - [x] 归属不泄漏 —— 持有者退出后屏幕重新可用（`fb.owner.released=0`）；
        输家退出不碰它（`fb.owner.held=0`）；首占成功 `fb.owner.grab=0`
  - [x] 拒绝不改归属，且普通用户仍先撞能力门 —— ktest 断言"被拒后 owner 不变"，
        `user.fb.clear=-13` 仍由 `cap_gate` 出（能力门在前，顺序是硬的）
  - [x] 不新增系统调用编号、不动 ABI 结构体 —— `past.32/33/34` 仍是 `-38`，
        `lnxrm_ps_entry` / `lnxrm_fb_info` 尺寸未变
  - [x] 控制台不受影响 —— 原语层不查归属，`fb.info.sane=1` 与自测滚动/裁剪照旧

#### Step 4 完成定义

- [x] 普通用户无法 `kill` 别人 / `kill -1` / `fdisk` / `diskinfo` / `ps`（T-031，2026-10-03）
- [ ] 普通用户无法弄死内核（内存、写终端）—— **内存一半已由 T-032 配额解决
  （2026-10-05，用户态 `brk` 撞 64 MiB 即 `ENOMEM`，`mem.parent.unchanged` 守着
  别人不受牵连）**；写终端仍开着：`CAP_FB` 挡住了画屏，但 stdout 是 `write` 到
  `/dev/console`，**没有单独的门**
- [x] 存在内存记账，能归因（T-032，2026-10-05：`struct task` 的
  `mem_pages`/`mem_peak`，`ps` 的 `MEM`/`PEAK` 两列，页表行走为唯一真相）
- [x] `init`/root 功能不受影响（`make smoke` 135/135）
- [x] 全部有测试守着（就已完成的项而言：`t_cred.c` + `systest` 降权段 + smoke 24 条 tag）

---

### 27. Step 5 · 契约对齐（1 天）

**目标**：消除"注释/ABI 说谎"，并**建立机制防止它重新长出来**。

**为什么放在 Step 2 之后**：先有测试，才能把"文档承诺"变成"有断言守着的断言"。否则改完还会反弹。

#### T-040 · 修 8 条契约漂移

- **成本**：1 天
- **做法**（逐条，代码在第一部分 §7 有完整表）：

| 项 | 动作 | 成本 |
|---|---|---|
| `fat32_journal` 头注释声称原子性 | 改注释为"写回缓冲，非日志"；或重命名为 `fat32_writeback.c`（**推荐后者，去掉误导**） | 20 行 |
| `sched.c` 注释说 circular queue | 改为"push-front 链表（实为 LIFO）"，或 Step 8 之后改成真循环队列 | 3 行 |
| `types.h:42,43` 槽号 98/97 | **✅ 已完成（2026-10-01）**：改为 128，`FIXMAP_VA` 整体删除 | — |
| `mm.h:45` 重复"slot 98" | **✅ 已完成（2026-10-01）**：随 `FIXMAP_VA` 删除一并清理，注释改为 PD slot 128 | — |
| `mm.h:42-43` "fixmap used to touch other ASes" | **✅ 已完成（2026-10-01）**：选了「直接删掉 `FIXMAP_VA`」，连同 `vmm.c` 对 slot 97/112 的死跳过 | — |
| `smp.rs:23` 字段名 `ap_main_phys` | 改名 `ap_main_va` | 2 行 |
| `struct task.next` 是死字段 | 删除，或加注释说明是为将来准备 | 3 行 |
| `cpu_info.{kstack_top,idle_ctx,idle_stack}` 死字段 | **✅ 已删除（2026-10-01）**，顺带发现 `task_lock` 只初始化从不加锁，一并删除 | 10 行 |

- **验收**：`grep -rniE "circular|atomic|remote PTE" include/ kernel/ docs/ usr/` 的每一处命中都指向真实行为

#### T-041 · 建立"契约必须有测试"的过程规约

- **成本**：半天
- **做法**（写进 `docs/` 或 README）：
  - 任何写进注释或 ABI 的行为，必须有一个断言或测试守着
  - 任何"已定义但未实现"的 flag/结构体字段，必须在 ABI 头里显式标注 `/* reserved, unimplemented */`
  - 修改 ABI 结构的任何字段，必须同步改 `docs/` 里的 ABI 描述
- **验收**：这条规约存在于文档里，且 T-040 的每一项都有对应测试（若某项确实无测试，说明为什么可以没有）

#### Step 5 完成定义

- [ ] 第一部分 §7 的 8 条全部处理
- [ ] `docs/` 里有一条明确的"契约必须有测试"规约
- [ ] 至少 3 条原本无测试的行为现在有测试（从 T-040 里挑：`fat_short_to_name`、`sched.c` 的队列语义、`types.h` 槽号断言）

---

### 28. Step 6 · 身份与生命周期（1 周）

**目标**：消除 PID 复用和悬垂 `parent` 指针。

**触发**：差距分析 4.5 节。`task_free_slot` 把槽置 `T_UNUSED` 但**从不清理指向它的 `parent` 指针**；`kill(42)` 可能命中完全不相干的进程。

#### T-050 · `struct pid` 或世代号（二选一）

- **成本**：3 天（世代号方案）/ 5 天（pid 对象方案）
- **推荐**：先做**世代号**（成本低，解决 80% 问题），把 `struct pid` 列为将来项
  - `struct task` 加 `u32 generation`
  - `find_task` 比对 `(pid, generation)`
  - `parent` 改为存 pid + generation 而非裸指针
- **注意**：`parent` 改成 pid+generation 后，`ps_dump`（`task.c:417`）和 `sys_waitpid`（`task.c:242-255`）都要改
- **验收**：
  - [ ] 大量 fork/exit 后，进程树的 parent 关系始终正确
  - [ ] PID 复用不会导致误杀（构造测试：fork 到耗尽槽位，确认 `kill` 不会命中错误进程）

#### T-051 · 回收时清理 `parent` 指针

- **成本**：1 天
- **位置**：`kernel/sched.c:340-356`（`task_free_slot`）
- **做法**：回收时遍历 64 槽，把指向自己的 `parent` 改掉（重挂载到 `find_task(1)` 或置 NULL）
- **验收**：回收后再无悬垂 `parent`（用 T-050 的压力测试验证）

#### T-052 · `task_table` 访问加锁

> **✅ 已完成（2026-10-01 第二轮）**：`find_task` 改在 `task_table_lock`
> （irqsave）内扫描；附带 `task_alloc_slot` 清 `pid=0`、`send_signal` 拒绝
> `T_UNUSED`。**`task_iter` 有意保持无锁**——它返回表内裸指针，调用方
> （`sys_waitpid`/`ps_dump`/`sys_kill`）全是单线程进程模型下的路径，
> 改成回调式遍历收益低于风险（论证见第二部分 §16.1）。

- **触发**：C9 —— SMP 下 `find_task`/`task_iter` 无锁线性扫 64 槽
- **位置**：`kernel/sched.c:358,386`
- **成本**：1 天
- **做法**：给 `find_task`/`task_iter` 加 `task_table_lock`（`sched.c:28` 已存在但只用于 alloc/free）
- **注意**：`task_iter` 返回的是**表内裸指针**，调用方（`sys_waitpid`/`ps_dump`/`sys_kill`）在锁外用它——需要改成"回调式遍历"（在锁内执行回调）或者用世代号保证条目在锁外仍有效
- **验收**：并发 fork/exit/kill 压测 10000 次，无异常

#### T-053 · `sys_fork` 的 fd 表竞争

> **❌ 已取消（2026-10-01 判定为误报）**：进程是单线程的——`sys_fork`
> 执行期间父进程没有第二条执行流能并发 `close`，fd 条目也持引用计数。
> C8 保留编号、不修（第一部分 §6.1、第二部分 §16.1）。

- **触发**：C8 —— 先 `memcpy` 父 fds，再加引用计数
- **位置**：`kernel/task.c:70-72`
- **成本**：1 天
- **做法**：给每个 task 加 `spinlock_t fd_lock`；`fork` 在锁内完成"复制+加计数"；`sys_close` 也在锁内
- **验收**：并发 fork+close 压测，用故障注入验证无 use-after-free

#### T-054 · `con_waiter` 改链表

- **触发**：C5 —— 单槽全局，第二个进程阻塞会覆盖第一个的等待者
- **位置**：`kernel/isr.c:96,112`
- **成本**：1 天
- **做法**：改成"所有等待者"链表，或用唤醒全部（`console_push` 唤醒所有 SLEEPING 的等待者）
- **验收**：两个进程同时阻塞在 `console_read`，按键后**都**被唤醒
- **注**：原来的 `key_waiter` / `SYS_getkey` 已删除，现在只剩 `console_read` 这一条阻塞路径

#### Step 6 完成定义

- [ ] PID 复用不会导致误杀
- [ ] 进程树 parent 关系在压力测试下始终正确
- [ ] `task_table` 所有访问有锁
- [ ] `fork`/`close` 并发无 use-after-free
- [ ] 多等待者阻塞场景正确

---

### 29. Step 7 · 文件系统硬化（2 周）

**目标**：从"能读写"到"崩溃安全 + 无泄漏 + 边界正确"。

**依赖**：Step 1（T-003/005/006/007/008）、Step 2（测试框架）、Step 6（失败路径需要正确的错误传播）

#### 按优先级排序的任务

| ID | 任务 | 触发 | 成本 | 验收 |
|---|---|---|---|---|
| T-060 | 统一 `bytes_per_sector` 缓冲为 `kmalloc` | C29：512 字节栈缓冲按 `bps` 索引，`bps>512` 全溢出 | 1 天 | 构造 4 KB 扇区镜像不溢出 |
| T-061 | 修 `alloc_chain` 的 `nclus` 截断 | C16：`u32 nclus` 截断 u64 → 文件报称比数据大 | 半天 | 连续写跨过 4 GB 边界，文件大小与实际数据一致 |
| T-062 | 修 vnode 泄漏 | C40：每次 `open` 泄漏 ~48 字节 | 2 小时 | 开 10000 次文件，内存用量不增长 |
| T-063 | 强制访问模式 | C41：`O_RDONLY` 文件可写；`O_APPEND` 未实现 | 1 天 | `O_RDONLY` 写入返回 `EBADF`；`O_APPEND` 正确追加 |
| T-064 | 真正的 `O_TRUNC` | C19：只改内存，磁盘目录项还是旧大小 | 1 天 | `O_TRUNC` 后重新打开大小为 0 |
| T-065 | 写失败回滚 | C20：找不到 dirent 时静默返回成功 | 1 天 | 写期间 unlink 该文件，写返回错误而非丢数据 |
| T-066 | 镜 FAT + FSINFO | C18：只写 FAT #0，卷会被其他实现判脏 | 1 天 | 写入后 `fsck.vfat` 干净 |
| T-067 | `fat_dir_iter` 加游标 | C30：每次 getdent 全目录重扫，`ls` 是 O(n²) 磁盘读 | 1 天 | 1000 文件的目录 `ls` 明显变快 |
| T-068 | 修块缓存读失败路径 | C25：读失败后条目仍 `valid`，留下半覆盖脏数据 | 半天 | 故障注入后缓存不返回脏数据 |
| T-069 | 统一磁盘 `bytes_per_sector` 不变量 | C29 的根因：守在错误的层 | 半天 | 校验在驱动层而非 FAT 层 |
| T-070 | MBR 范围校验 + 扩展分区 | C36/C37：MBR 条目不校验、静态名缓冲共享 | 1 天 | 畸形 MBR 不越界；分区名正确 |
| T-071 | PCI 桥递归深度限制 | C56：桥成环会爆栈 | 1 小时 | 构造桥环不爆栈 |
| T-072 | **`fat32_journal` 二选一** | C13：journal 名不符实 | 1–3 天 | **见下** |

#### T-072 的决策点

`fat32_journal` 现在承诺原子性但提供的是"改后像 + 无落盘 + 无法回滚的重复写回"。两条路：

- **路线 A（推荐，3 天）**：**删掉它**，重命名为 `fat32_writeback.c`，诚实描述为"去重回写缓冲"。FAT32 的实际崩溃一致性靠 Step 1 的 T-005/006 + T-066（镜 FAT）保证。诚实的功能边界优于假装的原子性。
- **路线 B（3–5 天）**：**真正实现日志** —— 预留日志区、落盘前像、写日志 superblock、mount 时 replay。工程量大很多，但这才让"journal"这个名字成立。

**建议先走 A**（把名字和注释改对），把 B 作为独立项目评估。理由：崩溃一致性对当前使用场景（单机玩具系统）价值有限，而路线 A 顺带消除了一个误导源。

#### Step 7 完成定义

- [ ] 写入后 `fsck.vfat` 干净
- [ ] 随机断电模拟（QEMU `-device` 或直接 kill QEMU）后，重启要么恢复要么不返回错误数据
- [ ] 长时间循环（开 10000 文件 / 写 100 MB / 建删 10000 文件）无泄漏
- [ ] 畸形磁盘镜像（坏簇、坏 MBR、4 KB 扇区）不 panic 不挂死不越界
- [ ] 目录操作是 O(n) 而非 O(n²)

---

### 30. Step 8 · 驱动（1.5 周）

**目标**：让 AHCI 真正可用，或者诚实标注为实验性。

#### 决策点：AHCI 修还是删？

差距分析核实了三个**致命**问题：

- C48：设备寄存器按**可缓存**映射（无 `PG_PCD`），且 doorbell 写后无 `clflush`/`sfence` —— 这是 AHCI 挂死的最常见原因
- C49：**完全无中断**，`cli` 住自旋 10⁷ 次，**在两层嵌套 irqsave 锁里**
- C50：完成判定只看 `IS_TFES`，**从不看 `PxTFD.ERR`** ⇒ ATA 级失败被当成功 ⇒ `num_sectors = 0xCCCCCCCC` 被注册成真设备

**两条路，都算合格**：

- **路线 A（推荐，3 天）**：修 C48（`PG_PCD` + fence）、C50（检查 ERR）、C51（BAR 校验、4 KiB→完整 ABAR、LBA48），并**在代码里明确标注"轮询模式，AP 并发下未验证"**
- **路线 B（1 周）**：修 A + 补 C49（`P_IE`、`HBA.IS` 排空、ISR、IOAPIC 路由）
- **路线 C（1 小时）**：把 AHCI 标为实验性，从默认启动路径移除，只保留 IDE（IDEA 全功能只是 28 位 LBA）

**建议先走 C 或 A。** AHCI 在 QEMU 上能工作不代表它正确，而当前它有 3 个会导致静默数据损坏的问题。**在修好之前，IDE 反而是更安全的选择**（虽然只有 28 位 LBA，但错误会被正确报告）。

#### T-080 · IDE LBA48 + 正确 IDENTIFY 轮询

- **触发**：C53/C54/C55
- **成本**：1 天
- **做法**：
  - 读 IDENTIFY words 100-103 得 LBA48
  - `ide_init` 加 400 ns 稳定期 + DRQ 轮询 + 状态检查
  - `ide_wait_ready` 检查 `IDE_SR_ERR`
  - 写后发 `0xE7` CACHE FLUSH
- **验收**：>128 GiB 镜像读写正确；无设备时正确报告"无设备"而非超时

> **✅ 部分完成（2026-10-05，T-080）**。卡上四条做法做掉两条，顺带修掉 C50 和一个单位标签：
>
> | 卡上的做法 | 状态 | 落点 |
> |---|---|---|
> | 读 IDENTIFY words 100-103 得 LBA48 | ⚠️ **只有容量**做了 | `include/sys/vfs.h` 新增 `ata_total_sectors()`，IDE 与 AHCI 共用：words 100-103 非 0 即权威值，`FFFFFFFFh` 哨兵返回 0，调用方**拒绝注册**而不是发布一个编造的容量。命令本身仍 28 位 |
> | `ide_init` 加 400 ns 稳定期 + DRQ 轮询 + 状态检查 | ✅ 全做了 | `ide.c:133-194` |
> | `ide_wait_ready` 检查 `IDE_SR_ERR` | ⏸ **故意没做** | 它在**发命令之前**也调用，而 ATA 靠下一条命令清 ERR——加了会把上一条命令的 ERR 变成永久死等。出错已由命令后的 `ide_wait_drq()` 兜住，那里判 ERR |
> | 写后发 `0xE7` CACHE FLUSH | ❌ 没做 | C55，与本卡要解决的问题无关 |
>
> **真机上 `2097151 MB` 是怎么来的**：`num_sectors == 0xFFFFFFFF`（4,294,967,295 扇区 = 2 TiB − 512 B）
> 被 `vfs.c` 的 `/ 2048` 整数截断成 2097151。两条来源都堵上了：
>
> - **AHCI 侧**：回退条件写成 `if (!lba28)`，而 ATA 规定"改用 words 100-103"的哨兵是 `FFFFFFFFh`（或盘 ≥2 TiB 该字段饱和）——方向反了，唯一会出事的值恰好永远不会回退。
> - **IDE 侧**：IDENTIFY 发完不查 DRQ/ERR，空数据口 `inw` 回 `0xFFFF` → 词 60–61 全 1（C54）。
>
> **同批顺带（不新增编号）**：
>
> - **C50**：AHCI `issueCmd` 在 `P_CI` 清零后补查 `PxTFD.ERR`——完成不等于成功，`0xCC` 预填的 IDENTIFY 从此进不了 `num_sectors`。
> - **`MB` → `MiB`**（`vfs.c:660`）：`num_sectors / 2048` 是 MiB，标签却写 `MB`。
> - **LBA28 超界即失败**（`ide.c:52,72`）：`lba > 0x0FFFFFFF` 直接回 `EFAIL`，不再把高 4 位丢掉静默读别的扇区。
>
> **验收**：
>
> - `无设备时正确报告"无设备"而非超时` → **达成**，实测（QEMU 不挂盘）：`[ide] no device on primary channel (status=00)`，随后正常走到 `[vfs] no disk found`。
> - `>128 GiB 镜像读写正确` → **未达成**，LBA48 命令未实现（C53 保持部分开放）。
>
> **实测**：`make smoke` **176/176**（174 + 2 条新判据：正向 `disk capacity is the real 64 MiB image`
> 钉住 `[vfs] attempting disk mount on hda (131072 sectors, 64 MiB)`，反向 `no all-ones disk capacity`
> 钉住 `4294967295 sectors` 不许出现）；`smoke_smp1` 81 tags 0 failed；`stage late` **34 cases / 783 checks**
> （`t_vfs.c` 新增 `test_ata_capacity` 4 行断言）；`selftest PASS (72 cases, 1546 checks)`；
> `[selftest] 1546/1546 pass [1 warn]`；`WARN(` = 0。

#### T-081 · 块设备抽象与设备选择策略

- **触发**：C28 / 4.10 —— IDE 无条件赢过 AHCI，AHCI 盘永远挂不上
- **成本**：2 天
- **做法**：
  - `blk_register` 的 off-by-one（`vfs.c:557`）
  - 引入明确的设备选择策略（"第一个能成功挂载 FAT32 的设备"，而不是 `blk_first`）
  - 分区设备 `blk_register`（现在 `diskinfo`/`fdisk` 看不到分区）
- **验收**：同时挂 IDE 和 AHCI 盘时，行为符合明确文档的策略；`fdisk` 能看到分区

> **✅ 部分完成（2026-10-05，T-081）**：三条做法做掉中间那条，而它正是核心。
>
> - **设备选择策略** ✅ —— `vfs_try_mount_disk()` 拆出 `vfs_try_mount_one(bd)`，然后
>   `for (i < blk_count) if (try(blk_list[i]) == 0) return 0`。以前是
>   `if (!blk_first) return EFAIL` + 只挂 `blk_first`，而 `blk_first` 由谁先 `blk_register`
>   决定、`main.c` 里 `ide_init()` 恒早于 `ahci_init()` —— **真机上一个 speculative 的 IDE 探测
>   找到的幻影盘，能把真正能挂上的那块锁在门外**。现在"第一个能成功挂载的设备"才是策略，
>   每块盘各打一行 `[vfs] attempting disk mount on ...`，回退到第二块盘在日志里看得见。
>
>   顺带一条测试判据：smoke 的 `disk capacity is the real 64 MiB image` 就咬在这一行上。
>
> - **`blk_register` 的 off-by-one** ⏸ —— `vfs.c:631` 现在是
>   `if (blk_count < 8) blk_list[blk_count++] = b;`，8 槽边界本身没有可复现的越界。
>   真正剩下的是**不一致**：`blk_count` 满 8 之后，新设备进不了 `blk_list`（遍历看不见它），
>   却仍然会顶掉 `blk_first`（`if (!blk_first) blk_first = b`）。归到"分区设备 `blk_register`"一起改。
>
> - **分区设备 `blk_register`** ❌ 没做（C28 主体）：`mbr_get_partition()` 造出来的 `blkdev`
>   仍不注册，`diskinfo`/`fdisk` 看不到分区。
>
> **验收**：
>
> - `同时挂 IDE 和 AHCI 盘时行为符合文档的策略` → **未达成**。QEMU 的 `make run`/`make smoke`
>   只给 `-drive ...,if=ide`，AHCI 的 class 过滤对所有设备失败（CODEBASE §15.1）——
>   **双盘同时在场的路径一次都没跑过**，这是策略落地后最该补的那条测试。
> - `fdisk 能看到分区` → **未达成**。

#### T-082 · 锁粒度（消除嵌套 irqsave 跨 I/O）

- **触发**：C7 —— 块缓存锁 + FAT 锁两层嵌套，跨越设备 I/O 与无界忙等
- **成本**：2 天
- **做法**：`blk_cache` 锁只保护元数据，**在锁外做设备 I/O**（用 refcount 或"取出条目→解锁→I/O→重新加锁写回"模式）
- **验收**：一个 CPU 在做慢速 I/O 时，其他 CPU 的 `kprintf` 仍然响应（这个测试需要一个可控的慢速设备模拟）

#### Step 8 完成定义

- [ ] AHCI 要么修好并有测试，要么明确标注实验性并从默认路径移除（**C50 已修**，2026-10-05 T-080：完成判定补 `PxTFD.ERR`；C48/C49/C51/C52 未动）
- [ ] IDE 支持 LBA48，错误正确报告（**错误报告已做**：IDENTIFY 走 DRQ/ERR、LBA28 超界回 `EFAIL`；**LBA48 命令未做**，见 C53）
- [x] 设备选择策略明确且有文档 —— **2026-10-05 T-081**：`vfs_try_mount_disk()` 遍历 `blk_list[]`，取"第一个能成功挂载的设备"，不再听 `blk_first`（= 谁先 `blk_register` 谁赢，而 `ide_init()` 恒早于 `ahci_init()`）。策略写进 `vfs.c` 注释与第一部分 C28 行。**仍缺**：IDE 与 AHCI 同时在场的双盘验收测试
- [ ] 慢速 I/O 不阻塞其他 CPU 的内核操作

---

### 31. Step 9 · 地址空间能力（4 周）

**目标**：解锁"能在上面构建东西"的能力。

**这是最大的投入，也是最能解锁生态的一步。** 在此之前，这个内核只能运行它自己那 16 个程序。

**依赖**：Step 2（测试）、Step 3（WP 有了 `mprotect` 的使用者）、Step 7（错误路径正确）

#### 任务顺序（严格按依赖）

| ID | 任务 | 成本 | 依赖说明 |
|---|---|---|---|
| T-090 | **补页错误分发** | 3 天 | 必须先有。`isr_common` 现在对 `#PF` 直接杀进程（`isr.c:153-171`），要演进为"缺页 → 尝试修复 → 修不了才杀" |
| T-091 | 匿名 `mmap` | 4 天 | 依赖 T-090 |
| T-092 | `munmap` | 2 天 | |
| T-093 | `mprotect` | 2 天 | 让 T-022 的 WP 有真实使用者；COW 依赖它 |
| T-094 | 用户栈自动增长 + guard page | 2 天 | 依赖 T-090/T-091 |
| T-095 | COW（`fork` 改写时复制） | 4 天 | 依赖 T-093。`dup_user_aspace`（`vmm.c:274-321`）改为"读时复制" |
| T-096 | 文件映射 `mmap(fd)` | 3 天 | 依赖 T-091 + 块设备就绪 |
| T-097 | per-CPU 时钟 + TSC 标定 | 2 天 | **✅ 已完成（2026-10-01）**：`lapic_timer_calibrate` 共享标定 + AP 本地 tick + BSP-only `jiffies` + 实测 `mdelay`；顺带修出 PIT 模式 3 → 模式 2（jiffies 曾跑 200 Hz，见第二部分 §16.2） |
| T-098 | `gettimeofday` / `clock_gettime` | 2 天 | 依赖 T-097（✅ 已就绪，随时可做） |
| T-099 | `stat` / `fstat` | 2 天 | 差距分析 4.7：`ls` 目前只能显示名字 |

#### 关键验收标准

- [ ] 一个递归到 64 KiB 的用户程序不崩溃（栈自动增长）
- [ ] `fork` 一个占用 100 MB 的进程，耗时与 1 MB 的进程**相近**（COW 生效）
- [ ] `mprotect` 保护一页为只读后，写入触发 `SIGSEGV`
- [ ] 能构建并运行一个 **动态链接** 的用户程序（`ld-linux` + `.so`）—— **这是整个 Step 9 的终极验收**
- [ ] `ls -l` 能显示文件大小和时间戳
- [ ] 用户程序能测量时间（`clock_gettime`）

#### T-099 之后的可选延伸

- `pipe` / `pipe2`（shell 管道的前提）
- 共享内存（基于 `mmap(MAP_SHARED, fd)`）
- unix domain socket
- `/proc` 完善（`/proc/self/maps` 在 COW 之后才有意义）

---

### 32. 冻结清单：这些阶段里不要动

明确列出**不该做**的事，避免范围蔓延：

#### ❌ 不要在 Step 0–4 期间碰

| 对象 | 原因 |
|---|---|
| `arch/setup.asm` 的 VBE 选型 | 工作正常，改动风险高收益低 |
| 三槽地址空间模型 | 核心设计，除非发现真 bug 否则不要重构 |
| 信号投递的默认处置 | 已精简到 100 行且行为窄（只 stop/continue/kill），加用户态处理器前不要动 |
| 任何新子系统 | 最大风险是宽度不是深度。8 个 60% 不如 3 个 100% |
| IDE/AHCI 之外的新驱动 | USB、声卡、网卡…… 全部延后 |
| 帧缓冲控制台的图形能力（新增直线/圆/图元类绘图系统调用） | 23–27 的矩形/字符/字符串绘图已够用且有回归覆盖，先让它有非控制台的用户再说 |
| `usr/` 下的新命令 | 先修内核 |

#### ❌ 永远不要做

- 不要实现 ext2/ext4/NTFS（FAT32 够用；崩溃一致性更重要）
- 不要追求 POSIX 完整兼容（会陷入无穷语义细节，而真正问题是安全模型和测试）
- 不要为了"看起来像 Linux"加 KPTI / eBPF / cgroup / namespace（**加机制前先问：它解决的是这个内核的实际问题吗？**）
- 不要在 Step 2 之前做 Step 4–9（没有测试的并发修复和 MMU 改动会引入比现在更多的 bug）

#### ⚠️ 需要特别小心的重构

- `fat32_*` 里的全局 `fat_priv` 和 `mnt_data = (void*)1` 哨兵：改成真正的 mount 间接层是正确方向，但**必须等 Step 7 完成**且有测试守着，否则会引入难查的回归
- `struct vnode` 改成真正的 inode（加 link count、加设备指针、加锁）：这是 C40/C41/C42 的根本解法，但改动面大，**建议在 Step 7 之后单独进行**

---

### 33. 落地策略：改动单元与门控

当前内核**是能跑的**。这个状态很宝贵，必须保护。

#### 原则：一个阶段一批可独立回退的改动

不要一次性大改。每个 Step 拆成若干**互相独立、可以单独撤销的改动单元**，一个单元只做一件事：

| 单元类型 | 例子 |
|---|---|
| 一个缺陷修复 | `fat32: 给簇链遍历加迭代上限和环检测` |
| 一个测试 | `selftest: 加 fb_pack/fb_rgb_px 往返测试` |
| 一个能力位 | `x86: 置 EFER.NXE` |
| 一个文档修正 | `docs: 修正 DEV_VMA/FIXMAP_VA 槽号` |

**每个单元都要能单独撤销而不留残留。** 如果一个改动无法独立撤销（比如同时改了调用方和被调方），说明它切得太大，应该再拆。

#### 改动说明格式

每个改动单元配一段说明，讲清"为什么"和"怎么验证的"：

```
fat32: 给簇链遍历加迭代上限和环检测

问题：fat_next_cluster 接受任何 < 0x0FFFFFF8 的值，且所有链遍历无上限。
      一个自引用或保留值 FAT 条目会在持有 fat_fs_lock 且关中断的情况下
      永久自旋。

改动：fat_next_cluster 增加边界校验；8 处链遍历各加迭代计数上限。

验证：构造自引用簇镜像，现在返回 EIO 而非挂死。回归测试 T-003。
```

这个格式的价值在于**"问题"段落是未来读者唯一能依赖的信息** —— 代码会被改，注释会漂移，这段说明会留下来。

#### 门控

```
任何改动进入主线前必须：
  [ ] make smoke 通过（-smp 1）
  [ ] make smoke 通过（-smp 2）
  [ ] [selftest] N/N pass
  [ ] 改动了 include/abi/ 的，ABI 文档同步更新
  [ ] 改了注释承诺的行为，有对应测试
```

涉及并发或内存的改动，额外要求 `-smp 4` 也过。

#### 回退

- **主线永远保持可启动**——这条比任何功能都重要
- 某个 Step 引入无法定位的回归时，**逐个撤销改动单元定位**，不要在已污染的主线上做考古
- 撤销的顺序应该与施加的顺序相反（先撤销后加的）

---

### 34. 风险登记

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| 打开 `CR0.WP`/`CR4.WP` 暴露大量既有 bug | **高** | 页错误风暴，难定位 | 严格按 Step 2→3 顺序；先记录自测基线；准备逐个撤销定位 |
| 打开 `SMEP` 后异常退出 | 中 | 用户态意外被杀 | T-023 已预判并设计 T-024 测试；已核实 `do_signal_check` 不改 `tf->cs` 且两个调用点（`isr.c:209`/`syscall.c:198`）都有 ring-3 守卫 |
| COW 改 `dup_user_aspace` 引入难查的写时复制 bug | 中 | 内存损坏 | 依赖 T-093 `mprotect` 已验证；必须有 COW 专项测试（写只读共享页 → 私有副本） |
| 自测框架本身 panic | 中 | 自测不可用 | T-012 验收里明确要求"自测本身不 panic，反复跑 20 次" |
| 范围蔓延（想顺手修别的） | **高** | 计划失效 | 冻结清单（第 32 节）；每个改动单元只做一件事 |
| T-072 选了路线 B（真日志）导致工期失控 | 中 | Step 7 延期 1 周+ | 明确推荐路线 A（删掉改名），B 单独评估 |
| 加了测试但没人在意 / 跳过门控 | 中 | 一切白做 | `make ci` 作为唯一入口，禁止绕过 |

#### 三个"停止思考"的检查点

如果你在以下时刻犹豫，说明范围该收紧了：

- **Step 1 结束**：如果 2 天没修完 8 个缺陷，说明估时太乐观，砍到 T-001/T-003/T-004 三个
- **Step 2 结束**：如果自测框架写了超过两周，砍掉 T-013 属性测试，只保留 T-010~T-012
- **任何时候**：如果开始想做 Step 9 的事，先确认 Step 2 的测试真的在 CI 里跑着

---

### 35. 时间盒方案

#### 只有 1 天

```
T-000 启动冒烟测试（半天）
T-003 FAT 链环检测（半天）    ← 一个坏扇区挂死整机，风险最高
T-001 console_write 加锁（✅ 2026-10-01 已完成，可跳过）
```
产出：一个能跑的 CI 门控 + 两个最致命的单点缺陷修复。**这一天就把"改代码会不会弄坏东西"变成有答案的问题。**

#### 只有 1 周

```
Step 0（0.5 天）+ Step 1（2 天）+ Step 2 的 T-010/T-011（3 天）
```
产出：CI 门控 + 止血 + 断言框架 + 9 个模块的单元测试。

#### 只有 1 个月

```
Step 0 + Step 1 + Step 2 + Step 3 + Step 5 的 T-040
```
产出：**一个拥有硬件强制安全边界、且这个边界有测试守着的内核。** 这是从"能跑"到"合格"的分水岭。

#### 有 3 个月

```
Step 0 → Step 8（跳过 Step 9）
```
产出：安全边界 + 特权模型 + 契约对齐 + 身份生命周期 + 文件系统硬化 + 驱动诚实化。**综合成熟度从 2.1 → 3.4 左右。**

#### 有 6 个月以上

```
全部 Step 0 → Step 9
```
产出：综合 3.9（L3 稳健级）。加上 `mmap` 后，用户态可以构建动态链接程序——**这个内核才第一次能运行不是自己写的软件。**

---

### 36. 度量：怎么知道计划在起作用

如果没有度量，计划会慢慢腐烂。以下指标应该每月看一次：

| 指标 | 起点 | 目标 | 说明 |
|---|---|---|---|
| **自测用例数** | 0 | ≥ 40 | `grep -c '^static void test_' kernel/` |
| **自测通过率** | — | 100% | `[selftest] N/N pass` |
| **CI 门控的覆盖率** | 0% | 100% | 走完整门控清单的改动单元 / 全部改动单元 |
| **属性测试迭代数** | 0 | ≥ 10000 | 无 panic、无 canary 破坏 |
| **"注释说谎"条目** | 14 | 0 | 第一部分 §7 表格 |
| **"一个坏输入挂死/越界"缺陷** | 见 GAP_ANALYSIS | 0 | 每修一个划掉一个 |
| **提权路径** | ≥ 3 | 0 | 无 NX、无 WP、无 SMEP、无特权模型 |
| **零测试的子系统** | 8/8 | 0/8 | |
| **多 CPU 下的竞争缺陷** | 10（C1–C10） | 0 | |
| **未初始化内存流入磁盘** | 6（C15 等） | 0 | |

**最重要的单一指标**：

> **"缺陷被测试发现" vs "缺陷被人读出来"的比例。**

起点是 0:1（所有 66 条都是读出来的）。目标至少是 1:1——**当一半以上的缺陷是测试抓到的，这个内核就进入了可维护状态。** 在此之前，无论修多少条，都是在不可验证的地基上盖楼。

---

### 37. Day 1：立即可做

如果今天只有时间做一件事，做这个：

#### 任务：加上启动冒烟测试

**为什么是它**：半天的成本，让后面所有工作从盲改变成有门控。而且它是唯一一个"做它不会引入任何回归"的任务（纯新增，不改内核逻辑）。

**具体步骤**：

1. 创建 `scripts/smoke.sh`（配方见第 22 节，已实测可用）
2. 在 `Makefile` 加 `smoke` 和 `ci` 目标
3. 验证：
   ```sh
   make smoke          # 应当返回 SMOKE OK
   ```
4. **验证这个测试真的有效**（这一步不能省）：
   ```sh
   # 临时在 kernel/main.c 的 start_kernel 末尾插 panic("test")
   make smoke          # 应当返回 SMOKE FAILED
   # 撤销
   ```
5. 验证不依赖 SMP：
   ```sh
   # 把 -smp 2 改成 -smp 1
   make smoke          # 应当仍然 PASS
   ```

**产出**：一个能回答"内核还活着吗"的自动化判定。

**这就是全部。** 明天可以开始 T-001（**注**：T-001 已于 2026-10-01 完成，
下一个未做的止血项是 T-002/T-003）。

---

*文档版本 1.1 · 缺陷编号（C1–C70，稳定 ID 不重排）引用自本文第一部分 §6 · 行号基于死代码删除后*
