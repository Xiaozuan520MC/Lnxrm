# lnxrm 代码库组织与子系统说明（3：内存）

> 配套文件：`CODEBASE_OVERVIEW.md`（§1 总览）、`CODEBASE_BOOT.md`（§2 启动）、`CODEBASE_PROC.md`（§4 进程·调度·信号）、`CODEBASE_SYSCALL.md`（§5 系统调用与 ABI）、`CODEBASE_FS.md`（§6 文件系统）、`CODEBASE_DRIVERS.md`（§7 驱动）、`CODEBASE_SMP.md`（§8 SMP 与 ACPI）、`CODEBASE_SELFTEST.md`（§9 自测）、`CODEBASE_INVARIANTS.md`（§10 关键不变式）。构建与运行见 `../BUILD/BUILD.md`，差距与计划见 `../GAP_ANALYSIS.md`，项目概览见 `../README.md`。本文为 §3 内存。

流水线：**E820 → buddy 物理页 → 分离空闲链内核堆 → 页表**（`kernel/mm/`）。

- **buddy（`pmm.c`）**：播种取最大 E820 可用区（钳到 `PMM_WINDOW_TOP`）；侵入式空闲链 + 逐 order 位图（物理 `BITS_BASE=0x10000`，`buddy_of = pa ^ size`）；`pmm_reserve` 只登记、播种时生效；每次真交出帧都调 `oom_frame_granted()`。
- **PMM 保留表（`main.c`，`RESV_MAX 8`，有帧缓冲 7 笔/无则 4 笔）**：LFB 物理、双缓冲窗 `[97,104)`、VGA/DEV/KHEAP/ACPI 固定窗——**凡固定窗占的别名槽，背后帧必须同时 reserve**。
- **内核堆（`kheap.c`）**：分离空闲链 9 箱（16…4096 字节），每分配 8 字节头校验 magic；**OOM 返回 `NULL` 必须检查**；16 MiB 映射在 `KHEAP_VMA 0xffffffffa0000000`。
- **三槽地址空间（`vmm.c`）**：PML4 槽 0 恒等（全 AS）、槽 255 用户空间 `[0x7f8000000000, 0x7ffffffff000]`（每进程独立）、槽 511 高半别名（全 AS）。`vmm_new_user_aspace` 复制除 255 外所有条目 ⇒ 内核映射零成本传播；`vmm_init` 别名环**跳过** VGA/DEV/ACPI/LFB/双缓冲固定窗。
- **内核 W^X（`vmm_kernel_wx()`，`vmm_init` 末步）**：`PD_M`+`PD_HI` 覆盖 `[__kernel_start, ALIGN_UP(__bss_end))` 的 2 MiB 槽拆 4 KiB 按段赋权——text `PG_P`(R-X)、`.rodata` `PG_P|PG_NX`(R--)、其余（data/ktest/trampoline/bss）`PG_P|PG_W|PG_NX`(RW-)；**两个别名一起改**，镜像外低内存与共享槽 buddy 帧保持 `P|W`——**AP trampoline 在 `0x8000` 从恒等映射执行，带 NX 会打死 SMP**；逐页 `invlpg`。
- **COW fork**：`dup_user_aspace` 共享帧，可写页清 `PG_W` 置 `PG_COW`（bit 9）；`page_cow_break` **先复制 → 再换 PTE → 最后放旧引用**；`user_ptr_writable` 是唯一 COW 解析门。用户页默认 `PG_NX`，**唯一 `exec=true` 是信号 restorer 与 `PROT_EXEC` mmap**。
- **#PF 三段分发 `page_fault_try_fix()`**：demand（先按 VMA prot 拒越权，再补帧）→ stack_grow（拒取指位）→ COW（只认写位）。**`err` 位必须逐位认**——否则 NX 取指被当成"COW 已破环" → 无限故障循环。
- **栈**：exec 只映 16 KiB，懒惰下扩（下限 512 KiB）；**64 KiB 守护区 `[MMAP_TOP, USER_STACK_TOP-USTACK_MAX_GROW)` 永不映射**，`STACK_GROW_WINDOW == STACK_GUARD` 静态断言；`stack_grow` 有 RSP 邻近检查（`cr2 + window < rsp` 拒）。
- **mmap/munmap/mprotect/brk**：六参 `mmap`(42) top-down 选址 `[MMAP_LO, MMAP_TOP)`、**只登记 VMA 靠需求分页补帧**；`brk` 上限 = `MMAP_LO`；配额 `cred_mem_quota()`、页数真相 `vmm_count_user_pages()`。**文件映射**：`MAP_SHARED` 同一 `(file,offset)` 全系统一个帧（`filepage.c` 64 桶哈希 + 锁外读盘），`MAP_PRIVATE` 自有帧；写回只在 `sys_munmap`/`exec_commit`/`sys_exit` 三处，脏页由硬件 `PG_D` 认定。`mprotect`(44) 作用任意用户范围，**授予 W 永不分配**（共享帧标 COW），撤销 W 连 `PG_COW` 一起清；拷贝门遇 not-present 先补页再判。
- **OOM killer（`oom.c`）**：触发面**只有用户缺页取不到帧**（`pagefault.c` 四个取帧失败点 + 空 buddy 的共享文件页）；**能回答的失败照旧只回答**（`brk` 回 `ENOMEM`、`kmalloc` 回 `NULL`）；受害者 = `mem_pages` 最大者（排除 pid≤1/kxld/embryo/zombie/已带 SIGKILL）；**一次短缺只杀一个**，闩由 `oom_frame_granted()` 开；杀掉要等 exit+reap 才回池。
- ⚠️ `vmm_destroy_user_aspace()` 释放所有 present 数据页：**必须先 `vmm_unmap_user` 再 `pmm_free`**，否则还两次、空闲链毒化。
