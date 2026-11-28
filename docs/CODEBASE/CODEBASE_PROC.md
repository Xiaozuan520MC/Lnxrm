# lnxrm 代码库组织与子系统说明（4：进程、调度与信号）

> 配套文件：`CODEBASE_OVERVIEW.md`（§1 总览）、`CODEBASE_BOOT.md`（§2 启动）、`CODEBASE_MEMORY.md`（§3 内存）、`CODEBASE_SYSCALL.md`（§5 系统调用与 ABI）、`CODEBASE_FS.md`（§6 文件系统）、`CODEBASE_DRIVERS.md`（§7 驱动）、`CODEBASE_SMP.md`（§8 SMP 与 ACPI）、`CODEBASE_SELFTEST.md`（§9 自测）、`CODEBASE_INVARIANTS.md`（§10 关键不变式）。构建与运行见 `../BUILD/BUILD.md`，差距与计划见 `../GAP_ANALYSIS.md`，项目概览见 `../README.md`。本文为 §4 进程、调度与信号。

- **task 槽**：64 槽静态数组，槽 0 = BSP idle；**父链接是 `(pid, generation)` 不是指针**（槽位复用不认错人），`(0,0)` = 无主僵尸标记，惰性回收在 `task_alloc_slot` 撞满表时触发；状态机 `UNUSED/EMBRYO/RUNNABLE/RUNNING/SLEEPING/ZOMBIE/STOPPED`；内核栈 32 KiB（顶 `intr_frame`，下 56 字节 `cpu_ctx`）。
- **fork**：COW 共享 + `vma_dup` 深拷 + fd 表 `refcnt++` + 伪造子帧（rax=0）。**execve**：读镜像 → `elf_interp_path`（PT_INTERP）→ `elf_place`（program 先、interp 后，ET_EXEC 用 bias=0、ET_DYN 用 bump 分配的 bias）→ overlap 证明 → 双 `elf_load*`（ET_EXEC 映射为链地址，ET_DYN 映射为 bias + p_vaddr）→ `exec_commit`（先赋 `current->pml4` 再销毁旧地址空间）；**接受 `ET_EXEC`（init 静态）与 `ET_DYN`（其余 15 个程序 + 2 个 `.km`）**，失败回滚，调用方地址空间不动。**exit**：关 fd → **`fb_disown`（画屏归属唯一释放点）** → 孤儿挂 pid 1 → `SIGCHLD` → ZOMBIE。
- **调度**：每 CPU **尾插 FIFO 队列**（`enqueue_on` 追尾、`pick_next` 取头，持续争用下即真 RR，C62；非环形，哨兵闭链）、`quantum=6` tick=60 ms、无优先级；全局 `runq_lock`（irqsave，`pick_next` 读队头也在锁内——C4）；**重排 current 必须走 `runqueue_add_local()`**；`switch_to` 只有 `pml4` 不同才切 CR3，idle 显式设 `master_pml4_phys()`；`swtch` 只存 7 个 u64。
- **信号**：14 槽编号 1..13，`SIGKILL 7`/`SIGSTOP 13` 硬置位且 KILL 醒 SLEEPING+STOPPED，默认终止集 6 个；`do_signal_check` 只在 `iretq` 前带 ring-3 守卫、一次一帧；restorer 页 9 字节 `mov eax,20; syscall; ud2`（S3.2 起，字节数与旧的 `int 0x80` 形式相同）。
- **凭据**：`CRED_USER/ROOT/KXLD` 三级 + `CAP_KILL/SYS_ADMIN/FB`；`setuid` **单向降权**；`kxld_protect_kill()`：pid 1 与 KXLD 不可杀；规则都是纯函数。
