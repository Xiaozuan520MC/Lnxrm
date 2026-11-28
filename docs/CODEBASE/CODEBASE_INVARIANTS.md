# lnxrm 代码库组织与子系统说明（10：关键不变式）

> 配套文件：`CODEBASE_OVERVIEW.md`（§1 总览）、`CODEBASE_BOOT.md`（§2 启动）、`CODEBASE_MEMORY.md`（§3 内存）、`CODEBASE_PROC.md`（§4 进程·调度·信号）、`CODEBASE_SYSCALL.md`（§5 系统调用与 ABI）、`CODEBASE_FS.md`（§6 文件系统）、`CODEBASE_DRIVERS.md`（§7 驱动）、`CODEBASE_SMP.md`（§8 SMP 与 ACPI）、`CODEBASE_SELFTEST.md`（§9 自测）。构建与运行见 `../BUILD/BUILD.md`，差距与计划见 `../GAP_ANALYSIS.md`，项目概览见 `../README.md`。本文为 §10 关键不变式。

一条一行，改代码时逐条对：

- `.def` 是 syscall 编号**唯一**定义源：只追加不重排；未知编号回 `-38`；ABI 结构体一改用户态全量重编。
- syscall 参数是 **Linux 六寄存器 `rdi rsi rdx r10 r8 r9`**（S3.2）：`syscall_entry` 把第 3 参从 `rdx` 交给 `syscall_do`，`h_mmap`/`h_dipc_recv`/`h_dipc_send` 按这套读帧，`ulib.h` 的四个 stub、`crt0.S` 与 signal restorer 的字节码按这套发——**改约定这五处必须同改**；`entry64.S` 里 `struct cpu_info` 的两个 `%define` 偏移另有 `percpu.c` 的 `_Static_assert` 钉着。
- 图形门**先 `cap_gate` 后 `fb_owner_gate`**，两种拒绝都 `-13`；画屏归属释放**只在 `sys_exit` 一处**。
- `main.c` 的 `pmm_reserve` 与 `vmm.c` 别名过滤**必须成对**，双缓冲窗 `[97,104)` 两边按整窗算（`RESV_MAX 8` 已用 7）；物理窗口 ≤ `PMM_WINDOW_TOP`(2 GiB)，越界播种 = #PF、IDT 前 = 三重故障无痕复位。
- `console_init → gdt_init → gs → idt_init` 必须是头四步；`0x6F00` 哨兵必须按 **DWORD** 读。
- `kernel.ld` / `entry64.S` / `vmm.c` 三处页表与段布局硬编码手工同步；`vmm.c` 跟随 `kernel.ld` 段边界赋 W^X。
- 新地址空间只复制**除槽 255 外**的条目，槽 0/511 永远共享；只有 `pml4` 不同才切 CR3，idle 必须 `master_pml4_phys()`。
- 释放已映射用户帧必须**先 `vmm_unmap_user` 再 `pmm_free`**；COW 帧 `pmm_free` 只减 `frame_ref`、归零才回 buddy。
- `PG_COW` 只与清 `PG_W` **成对**出现；`!W !COW` 写保护必落 SIGSEGV；widen 不给 `PG_COW` 页回写 `PG_W`，**撤写必须连 `PG_COW` 一起清**；COW 破环恒为**先复制 → 再换 PTE → 最后放旧帧**，`user_ptr_writable` 是唯一门。
- mmap 窗 `[MMAP_LO, MMAP_TOP)`、`brk` 上限 = `MMAP_LO`、**守护区永不映射**；`STACK_GROW_WINDOW == STACK_GUARD` 静态断言：堆/mmap/守护/栈两两不相交。
- `PG_W ⇒ pmm_refcount == 1`（授予写只给独占帧，共享帧标 COW 推迟复制）。
- #PF 的 `err` 位**逐位认**：`cow_fault` 只认写位、`stack_grow` 拒取指位；demand **先按 VMA prot 拒、再取帧**；拷贝门遇 not-present **先补页再判**，别把「lazy」当「unmapped」。
- buddy 最大块 4 MiB；`kmalloc` OOM 回 `NULL` 必须检查；配额唯一来源 `cred_mem_quota()`，页数真相 `vmm_count_user_pages()`。
- **OOM killer 只在用户缺页取不到帧时触发**；能回答的失败永远只回答；一次短缺只杀一个，闩由帧真出手开。
- 改 `struct task` 必须**全量重编**（`cpu_info` 内嵌按值 `runq_head`）。
- 运行队列是**尾插 FIFO/RR**（`enqueue_on` 追尾、`pick_next` 取头且读在 `runq_lock` 内，C4/C62）；重排 current 走 `runqueue_add_local()`；`sched_wake_task` 不得持 `runq_lock` 调 `runqueue_add`。
- 信号只在 `iretq` 前带 ring-3 守卫处投递，一次一帧；**`current->tf` 只发布 ring-3 帧**；restorer 页是唯一 `exec=true` 的用户页。
- `blk_io_read/write` 是全内核唯一调 `dev->read/write` 的地方（ktest-only 的 AHCI 中断自测除外）；驱动函数只在 `blk_drain_locked()` 持**该设备** `io_lock` 时执行，嵌套恒为「分区 → 父盘」；**FAT 区永不进块缓存**，一个扇区只走一条读写路，FAT 表只走三个 helper；**ext2 相反——每个字节都必须进块缓存**（挂载期也不许直读），写回缓存是 flush 前唯一的真相。
- 路径路由只认**最长前缀 + 组件边界**，`ops`/`priv` 成对取自它；`vfs_close_file` 只减真有的引用；`vfs_umount` 不 free priv；`blk_register` 满槽大声拒绝且表不变。
- shm 是两条线一扇门：表项那份 file 引用**只由 `shm_unlink` 放**，故 `close_node` 恒在 unlink **之后**触发并无条件 `kfree(ent)`；`shm_unlink` 必须「锁内摘表 → **解锁** → `vfs_close_file`」，`kmalloc` 必须在锁外并**二次查表**（输者直接 `kfree`，不走 `vfs_close_file`）。
- dipc 同一扇门的第二条：`dipc_destroy` **只摘表 + 解锁后放 entry 引用**（绝不 `-EBUSY`，park 中的 recv 靠自己的 fd pin 住），`dipc_close_node` 恰好一次里先 `pmm_free(ring_pa)` 再 `kfree(d)`（cache 的引用仍在，帧死于第二次释放）；**wake 恒在 `dipc_lock` 之外**且先取 `next` 再唤醒；`ring_take` 的 `rd` 只在两段拷贝**都成功**后推进（`EFAULT` 不消耗数据）；索引在帧内、越界由 `ring_check` 归零。
- AHCI 的写都以 `0xE7` CACHE FLUSH 收尾——`ahciBlkWrite` 每次 `blk_io_write` 请求发一次；失败打印 `[ahci] FLUSH CACHE (E7) failed …` 但**不改写写的结果**（盘已收下扇区，flush 失败不把一次完成的写变成失败；`ahciFlushCache` 注释写明了这个取舍，port 探针 1/3 也拿 0xE7 验非数据路径）。`ide.c` 已删，全仓只剩这一处块设备写。
- 解码器单源：`include/abi/ps2-keyboard.h`（扫描码→ASCII 的**唯一**表定义）与 `include/abi/ps2-mouse.h`（aux 包重组/符号扩展的**唯一**定义），内核 `drivers/ps2-keyboard.c`/`drivers/ps2-mouse.c` 与 ring3 `userland/mod/ps2-keyboard.c`/`userland/mod/ps2-mouse.c` 各持**自己的**状态结构；IRQ1/IRQ12 **先解码再镜像**，`dipc_push` 失败（通道未建/环满）绝不影响解码路径。帧长以**内核 `ms_dec.bytes` 为权威**，ring3 的推断只影响它自己。
- `kbd_decode_init()`/`mouse_decode_init()` 必须排在 ktest 之后：`t_dipc` 断言第 9 个通道 `-ENOSPC`，预建 `"kbd"`/`"mouse"` 会把 `live0+8` 顶出界。
- `init` 是 `/mod` 的**唯一**发现者与 reaper：扫目录 + `.km` 后缀（大小写不敏感）+ 一个模块一个子进程，**成功路径静默、每条失败路径各打印一行**（「扫到文件却一个都没起来」才打统计行），任何失败只跳过，**pid 1 不因此退出**。
- `/mod` 里的文件名必须带 `.km`（**后缀由 `MOD_ELFS` 在链期加**，安装只做 `basename` 原样搬运，再补一次就是 `ps2-keyboard.km.km`）：不带后缀时 init 一个都不起，而**镜像、串口、`ps` 三处各自看起来都正常**——这就是「`ps` 看不到驱动进程」的成因（2026-11-18 实测）。
- **ext2 是 `e2fsck -n` 的**：位图位与描述符/超级块三处空闲计数必须同一次改，目录 `rec_len` 链必须恒落块尾（照抄 Linux `ext2/dir.c` 的切分/合并），每个变更回调必须以 `ext2_finish()` 收尾（同步空闲计数，**失败路径也走**；块层直写，没有待 flush 的缓存尾巴）；Makefile 造完镜像即查、smoke 每轮 carry-over 再查，**不过就是驱动的 bug 不是镜像的**。
- journal `commit` 第 ④ 步失败必须**保留 COMMIT**；失败路径也必须 commit。
- EOI 必须在 handler **之前**写；IPI 向量必须显式盖过 catch-all。
- AP 的 `ready` 必须先于一切控制台输出；trampoline 传的 `ap_main` 是**虚拟地址**。
- 自旋锁默认 irqsave，唯一例外 `spin_lock_irqon` 只给 `fat_fs_lock` 与 `ext2_fs_lock`（C7 起的同一理由：**这两把锁永不进中断路径**，不接管中断状态也安全——中断只会打断它自己的行，不会来抢 FS 锁）；FS 锁内嵌 `cache_lock`，顺序不得颠倒；`cache_lock` **不跨 `blk_io_*`**（`busy` 槽不可被 victim/purge 挑中）。
- 双缓冲呈现只拷脏区域、**先写后标**，逐行拷贝全程在 `console_out_lock` 内、拷贝尾 `mfence`；绕过 `fb_store_px` 的写必须自己补标；首 tick 前只在写批次边界 present。
