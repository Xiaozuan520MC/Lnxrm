# lnxrm 代码库组织与子系统说明

面向要改这套代码的开发者：代码怎么组织、各子系统怎么工作。构建与运行见 BUILD.md，差距与计划见 GAP_ANALYSIS.md，项目概览见 README.md。

## 1. 总览

x86-64 freestanding 单体内核：C 为主体，启动 / 中断 / 上下文切换用 NASM，产出符合 Linux x86 boot protocol 2.08 的 `bzImage`，在 FAT32 系统盘上挂根并跑自带 shell。**语言只有 C 与 ASM**，构建零警告。

### 1.1 产物

| 产物 | 说明 |
|---|---|
| `build/bzImage` | 可引导镜像 = `setup.bin` + `vmlinux.bin`，两个 Python 脚本回填 boot header |
| `build/vmlinux.elf` / `.bin` | 链接产物（符号+调试信息，链接脚本 `arch/kernel.ld`）/ 按 LMA 导出的裸 payload（物理 `0x100000`） |
| `build/disk.img` | 64 MiB FAT32 系统盘（`mkfs.vfat` + mtools 装入 `README.md` 与 `bin/`） |
| `build/user/<name>` | 16 个静态 ELF64 用户程序 |
| `build/lnxrm.iso` | GRUB 引导 ISO，只含 bootloader + 内核，不含根文件系统 |

补丁脚本（构建链关键）：`scripts/patch_setup_jmp.py` 用 `nm` 查 `_start64` 把 `setup.bin` 的哨兵 `0xF00DFACE` 回填成真实跳转（幂等）；`scripts/patch_bzimage.py` 填 `setup_sects`(0x1F1)/`syssize`(0x1F4)，断言 `HdrS`、`0x55AA`、payload 对齐。

### 1.2 目录树

```
Makefile               构建入口：源文件自动发现 + 编译/链接/镜像/运行/冒烟；用户态委托 userland/Makefile
arch/                  架构：setup.asm(实模式 E820/VBE/长模式) entry64.S(_start32/64、中断桩、swtch)
                       trampoline.S(AP 入口,拷到 0x8000) cpu.c(GDT/TSS/IDT/PIT/LAPIC 标定)
                       kernel.ld(单一 .image 段 VMA 0xffffffff80100000 / LMA 0x100000)
include/               全部头文件（无实现）：abi/(lnxrm_abi.h + lnxrm_syscalls.def) sys/ mm/ 其余
kernel/
  main.c               start_kernel 编排
  core/                syscall.c(分发/门) print.c percpu.c smp.c protect.c selftest.c
  sched/               task.c(生命周期/fork/execve/exit/wait4) sched.c(RR/锁) elf.c signal.c
  irq/                 isr.c(分发/异常/IPI) apic.c acpi.c
  mm/                  pmm.c(kheap 之上: buddy, order-0 帧引用计数) kheap.c vmm.c vma.c(匿名 VMA 列表) pagefault.c(缺页分发: demand/stack/COW)
  fs/                  vfs.c blk_cache.c mbr.c initramfs.c
  fs/fat32/            fat32.c(挂载/链) fat32_dir.c fat32_file.c fat32_meta.c fat32_journal.c
  drivers/             pci.c ide.c ahci.c uart.c ps2mouse.c framebuffer.c input_event.c
  lib/string.c
ktest/                 启动自测：ktest.c 框架 + 27 个 t_*.c（在仓库根，不在 kernel/ 下）
  include/             ktest 私有头（-Iktest/include）
userland/              Makefile + bin/ 16 个程序 .c（自动发现）+ lib/(crt0.S + ulib.c 运行库)
scripts/               patch_setup_jmp.py patch_bzimage.py mkinitramfs.py smoke_test.py
```

`userland/lib/` 不是 `/init` 那个程序，是**每个**用户程序都链接的运行库。

### 1.3 容量常量

| 常量 | 值 | 定义处 |
|---|---|---|
| `MAX_CPUS` / `NR_TASKS` / `NR_FDS` | 8 / 64 / 16 | `include/types.h` |
| `KSTACK_SIZE` | 32768 | `include/sys/sched.h` |
| `LNXRM_NR_SIGNALS` | 32 | `include/abi/lnxrm_abi.h` |
| `LNXRM_SYSCALL_LAST` | 44（编号 0–44 共 45 个） | `include/abi/lnxrm_abi.h` |
| `CACHE_ENTRIES` | 64 | `include/disk.h` |
| `MAX_MOUNTS` | 4 | `kernel/fs/vfs.c` |
| `HZ` | 100 | `include/sys/cpu.h` |
| `MAX_ORDER` / `MIN_ORDER_BITS` | 11 / 12（buddy 最大块 4 MiB） | `kernel/mm/pmm.c` |
| `PMM_WINDOW_TOP` | `0x80000000`（2 GiB 管理窗口） | `include/mm/mm.h` |
| `RESV_MAX` / `KHEAP_NBINS` | 8 / 9（16…4096 字节分箱） | `kernel/mm/pmm.c` / `include/mm/mm.h` |
| `CRED_USER_MEM_QUOTA_PAGES` | 64 MiB / 4 KiB | `include/sys/cred.h` |
| `AP_STARTUP_TIMEOUT_MS` | 100 | `include/sys/smp.h` |

## 2. 启动

### 2.1 两条路径与 `0x6F00` 哨兵

两条路径在 `start_kernel` 汇合，靠**物理 `0x6F00` 的 DWORD** 区分：

- **QEMU `-kernel`**：`arch/setup.asm` 实模式采 E820（表 `0x7000`、条目数 `0x6FFC`、上限 64）、四遍级联选 VBE 模式（写 `0x8C00`）、向 `0x6F00` 写 **0**、建 `0x60000` 页表进长模式。
- **GRUB `linux`**：`_start32` 把 `boot_params` 指针按 DWORD 写 `0x6F00`（非 0），自建页表进长模式；VBE 由 GRUB 编程。无 EFI handover，仅 legacy BIOS/CSM。

哨兵 0 → E820/LFB/cmdline 取 `0x7000`/`0x8C00`/`0x20000`；非 0 → 全取 `boot_params`。**必须按 4 字节读**（两个写入者只写低 4 字节，按 `u64` 读会拼进内存残留）。

### 2.2 `_start64` 与 master 页表（固定物理地址）

| 表 | 物理地址 | 作用 |
|---|---|---|
| `PML4_M` | `0x50000` | `[0]` 恒等、`[511]` 高半 |
| `PDPT_M` | `0x51000` | 恒等子树，`[0]`/`[1]` = 低 1 / 1..2 GiB |
| `PD_M` / `PD_M2` | `0x52000` / `0x57000` | 恒等 2 MiB 大页 |
| `PDPT_HI` | `0x53000` | 高半子树，`[510]`/`[511]` = `PD_HI` / `PD_HI2` |
| `PD_HI` / `PD_HI2` | `0x54000` / `0x58000` | 高半别名（0..1 / 1..2 GiB） |
| `PT_VGA` / `PT_DEV` | `0x55000` / `0x56000` | `PD_HI[96]`→`0xB8000`；`PD_HI[128]`→MMIO |

恒等区与高半别名各铺 **2 GiB** —— `PMM_WINDOW_TOP` 的上界来源。

### 2.3 `start_kernel` 顺序（顺序即契约）

1. `console_init → gdt_init → cpu_set_gs_base(&cpu_table[0]) → idt_init` —— **必须头四步**：IDT 前任何异常 = #PF → #DF → 三重故障无痕复位。
2. `boot_collect_e820` → 横幅/e820 表 → `boot_read_vbe` → `boot_read_cmdline`。
3. `ktest_run(KTEST_EARLY)`。
4. `cpu_init_percpu(0,0)` → **`pmm_reserve` ×7**（必须在 `pmm_init` 前）。
5. `pmm_init → vmm_init → kheap_init`。
6. `acpi_init`（需 `vmm_map_kernel_page` 故在 vmm 之后；IOAPIC 定址故在 cpu_init 前）→ `cpu_init` → `pit_init(HZ)`。
7. `ktest_run(KTEST_MM)`。
8. `fb_init → ps2_kbd_init → serial_irq_init → vfs_init → ide_init → ahci_init → vfs_try_mount_disk()`；根没就绪或缺 `/bin/init` 回退 `vfs_mount_initramfs()`。
9. `do_global_ctors()`（空转）→ `sched_init`。
10. `ktest_run(KTEST_LATE)` + `ktest_summary()`。
11. `kernel_spawn("/bin/init")`；失败打 `Stop!` → `fb_flip()` → hlt 死循环（此时无 tick）。
12. `lapic_timer_calibrate`（BSP 一次，AP 用缓存值）→ `smp_init` → `lapic_timer_start(32, HZ)`。
13. `[mem] available memory` + `[boot] ready in N ms` → `sti` → `idle_loop()`。

### 2.4 三处必须手工同步的硬编码

`arch/kernel.ld`（单一 `.image` 段，VMA−LMA = `0xffffffff80000000`）/ `arch/entry64.S`（页表物理地址 `0x50000`…）/ `kernel/mm/vmm.c`（同一组常量写那些表）——三者只有注释互相提醒，改一处必改另两处。

## 3. 内存

流水线：**E820 → buddy 物理页 → 分离空闲链内核堆 → 页表**（`kernel/mm/`）。

### 3.1 `pmm.c`：buddy 物理页分配器

- 播种取**最大的一块** E820 可用区（1 MiB..4 GiB，钳到 `PMM_WINDOW_TOP`），整段在窗口外的跳过；`best_end < __bss_end + 18 MiB` 直接 `panic`。
- 窗口 `[base_addr, top_addr)`：`.bss` 末 + 2 MiB 边距 .. 堆区下方（堆由 `kheap_place` 先从顶部挖走 16 MiB）。
- **侵入式空闲链**（块头 8 字节写下一块指针）+ **逐 order 位图**（物理 `BITS_BASE = 0x10000`），`buddy_of(pa,o) = pa ^ size`；播种按最大对齐块下探、碰保留区逐块收缩。
- `pmm_reserve(lo,hi)` 只登记，`resv_hits()` 播种时生效；`pmm_inject_oom(n)` 让接下来 n 次分配确定性失败（自测用）。
- 对外：`pmm_init / pmm_reserve / pmm_alloc_order / pmm_free_order`、`pmm_alloc()/pmm_free()` 宏、`pmm_free_bytes()`。

### 3.2 PMM 保留表（`kernel/main.c`，`RESV_MAX 8`）

| # | 范围（物理） | 用途 | 条件 |
|---|---|---|---|
| 1 | `LFB .. +pitch*height` | MMIO，永不当帧 | 有帧缓冲 |
| 2 | `FB_PD_BASE..+fb_slots` | LFB 窗 `PD_HI[104…]`，别名已消失 | 有帧缓冲 |
| 3 | `[97,104)` | **双缓冲后缓冲整窗** | 有帧缓冲 |
| 4 | slot 96 / 128 / 256..271 / 108 | VGA 文本窗 / `DEV_VMA` / `KHEAP_VMA` / `ACPI_SCRATCH_VA` | 无条件 |

规则：**凡被固定窗口占用的高半别名槽，背后物理帧必须同时 `pmm_reserve`**（否则 `PHYS_TO_VIRT()` 落到窗口上）。有帧缓冲 7 笔、无则 4 笔。

### 3.3 `kheap.c`：内核堆

- **分离空闲链**，9 箱对应 16/32/…/4096 字节；`n+8 ≤ 4096` 走分箱（箱空向 buddy 要一页切块），更大直接 `pmm_alloc_order`，header 记 `0x80|order`。
- 每分配 8 字节头 `{bin_or_order, magic}`，`magic 0xECEC0001`，`kfree` 校验；大块头写在物理页首。
- **OOM 返回 `NULL`**，调用方必须检查（`kmalloc_or_panic()` 当前零调用者）。
- 堆区 16 MiB，映射在 `KHEAP_VMA = 0xffffffffa0000000`（`PD_HI[256..271]`）。

### 3.4 `vmm.c`：三槽地址空间与固定窗过滤

| PML4 槽 | 用途 | 共享性 |
|---|---|---|
| 0 | 低内存恒等映射（2 MiB 大页，0..2 GiB） | 全部 AS，仅内核 |
| 255 | 每进程用户空间 `[0x7f8000000000, 0x7ffffffff000]` | 每进程独立 |
| 511 | 高半别名（镜像/堆/VGA/设备/LFB/ACPI） | 全部 AS |

- `vmm_new_user_aspace()`：新 PML4 **复制除槽 255 外的所有条目** —— 内核映射对新进程零成本传播（无 refcount、无 TLB shootdown 计数）。没帧返回 0，不 panic。
- `vmm_init()` 别名环：`kernel_end..heap_end` 每个 2 MiB 槽映进 `PD_HI`/`PD_HI2`，**跳过** slot 96(VGA)、128(DEV)、108(ACPI 弹跳)、LFB 窗 `[104,104+fb_slots)`、LFB 物理范围、**双缓冲窗 `[97,104)`**；循环后整体覆盖 256..271 为堆帧；最后置 `EFER.NXE`。
- 关键函数：`vmm_map_user`（四级映射，`writable/user/exec` 三权限参数，失败 `ENOMEM`）、`vmm_unmap_user`（清 PTE 返回物理地址）、`vmm_count_user_pages`（**用户页数唯一真相**，记账与 `ps` 都读它）、`vmm_translate_in`、`vmm_map_kernel_page`、`vmm_switch_to`、`master_pml4_phys`。
- ⚠️ `vmm_destroy_user_aspace()` 释放所有仍 present 的数据页：想自己释放已映射帧，**必须先 `vmm_unmap_user` 再 `pmm_free`**，否则同一帧还两次、空闲链毒化。

### 3.5 COW fork、NX 与懒惰栈

- `dup_user_aspace` **共享帧**（2026-11-07 起不再逐页 `pmm_alloc + memcpy`）：可写页两边清 `PG_W` 置 `PG_COW`（PTE bit 9）并 `invlpg` 源侧，只读页原样共享；每帧 `pmm_ref` 加签，父子各记各的页数。写保护缺页经 `page_fault_try_fix` 的 COW fixer（`page_cow_break`：先复制、再换 PTE、最后放旧引用）取回私有可写页；`!W !COW` 的写保护是真 rodata，落 SIGSEGV。
- `vmm_map_user` 的 widen（已存在即并权限）**拒绝**给 `PG_COW` 页回写 `PG_W`，否则两个共享者会静默重新共享可写帧。
- `vmm_map_user` 的 `exec=false` 即打 `PG_NX`：栈/堆/数据默认不可执行；**唯一 `exec=true` 的用户页是信号 restorer**（`signal_map_restorer`，`USER_SIGRETURN_VA`）与 `PROT_EXEC` 的 mmap 段。
- 用户栈 exec 时只映 16 KiB；落在 `[USER_STACK_TOP - USTACK_MAX_GROW, ustack_lo)` 的 #PF **懒惰下扩**（下限 512 KiB），否则按栈溢出。
- **栈保护页**（2026-11-07）：`STACK_GUARD` 64 KiB 的 `[MMAP_TOP, USER_STACK_TOP - USTACK_MAX_GROW)` **永不映射**——mmap 窗在它下面收口（`MMAP_TOP` = 栈地板 − `STACK_GUARD`），`demand_page` 落在守护区一律拒绝，`mprotect` 对它回 `ENOMEM`（既无 VMA 也无页）、`munmap` 零相交回 `EINVAL`（`vma_erase` 的约定）。`STACK_GROW_WINDOW` 与它**静态断言相等**（`USER_STACK_TOP - MMAP_TOP == USTACK_MAX_GROW + STACK_GROW_WINDOW`）：栈最深时，凡被认定为栈访问的 fault 都落在守护区内、绝够不到 mmap VMA。`stack_grow` 另有 **RSP 邻近检查**：`f->ussp`（换权时硬件压的用户 RSP；`intr_frame` 没有 `rsp` 字段）在用户空间内时，`cr2 + STACK_GROW_WINDOW < rsp` 一律拒绝——x86-64 SysV 红色区 128 B 加序言一次减帧就是合法触达的全部，再往下是野指针，**取帧之前**先毙；ktest 合成帧 `ussp=0` 自动跳过。
- 用户态 #PF 走 `page_fault_try_fix()` 三段分发（2026-11-07 起）：**demand**（cr2 落在某 VMA 且 not-present → **先按 VMA prot 拒掉越权的写/取指**，再补零帧、按 prot 给 W/NX、过配额、已 present 则幂等拒绝）→ **stack_grow**（not-present 且非取指）→ **COW**（present **且带写位** + `PG_COW`）；三段各自声明所认的 `err` 位，全不认才转 SIGSEGV/panic。
- **`err` 位必须逐位认**：`cow_fault` 只认写位（bit1）——否则 NX 取指（`P|I = 0x11`）会被当成"COW 已破环"，什么都没改就回报已修复，`iretq` 重试同一指令再入同一 #PF，**无限故障循环**（NXE 已开，每个栈/堆/mmap 数据页都踩得到）；`stack_grow` 拒取指位（bit4），栈是 NX，长出来也白长。这与 Linux 把 `access_error()` 排在 `handle_mm_fault()` 之前是同一条规则：**先问权限，再动分页器**。

### 3.6 `brk`、匿名 `mmap` 与配额

- `brk` 合法区间 `[brk_base, MMAP_LO)`，先算术拒绝再取帧；`MMAP_LO` 是 mmap 窗下沿（2026-11-07 从 `USER_STACK_TOP - 1 MiB` 移来——旧上限落在 mmap 窗**内**，堆与 mmap 能争同一页面）。
- 匿名 `mmap`（42/43，`kernel/mm/vma.c` + `sys_mmap/sys_munmap`）：`addr` 必须为 0，top-down 扫 `[MMAP_LO, MMAP_TOP)` 空隙（`MMAP_TOP` = 栈增长地板 − `STACK_GUARD`），**只登记 VMA 不映射**；首触由 demand fixer 补帧（配额同 brk）。`munmap` 摘 VMA（中段挖洞拆分、零相交 `EINVAL`）→ 逐页 unmap → `vmm_reclaim_empty_tables` 回收空 PT/PD/PDPT。VMA 列表挂 `struct task.vmas`，fork 深拷（`vma_dup`）、execve 与 `task_free_slot` 释放。
- **`mprotect`（44，`sys_mprotect` + `vma_protect`）**：`[addr, ALIGN_UP(len))` 作用于**任意用户范围**（不只 mmap 段——翻 JIT、改 .text、封栈缓冲），未对齐/零长/回滚/出用户空间 `EINVAL`；范围内存在「既不在场又无 VMA」的页回 `ENOMEM`，**先查全再改动**（跨 VMA 按步长快走，落进 VMA 就跳到它的尾）。**先 `vma_protect` 原子改列表、再改 PTE**（后者只重写刚读过的叶，不可能失败）：`vma_protect` 先数节点（全覆盖 0、切头/切尾 1、中间打洞 2）→ 预分配池 → 才写 prot，`ENOMEM` = 列表零改动（与 `vma_erase` 的拆分同规）。授予 W **永不分配**：独占帧直接 `PG_W`，共享帧（`pmm_refcount > 1` 或已 `PG_COW`）只标 `PG_COW`、复制推迟到下一次写故障——与 Linux `can_change_private_pte_writable()` 同规则（只在 `PageAnonExclusive` 时才改可写）；**撤销写必须同时清 `PG_W | PG_COW`**，否则下一次 COW 缺页会把只读悄悄改回可写。NX 随 `PROT_EXEC` 翻转；`PG_W` 却 `refcount > 1` 的不可能态防御性降级为 COW。
- 判据是**页表行走** `vmm_count_user_pages()`，配额只来自 `cred_mem_quota()`（0 = 无上限，root/kxld）；`have + delta > quota` 回 `ENOMEM`；shrink 永不拒绝；部分失败回滚。
- 配额**只来自 `cred_mem_quota()`** —— `struct task` 没有配额字段，`setuid` 改 tier，两套记账必然打架。
- 生命周期：exec/spawn/fork 后按页表行走重算并重置峰值；brk/mmap-demand/munmap 后增减并抬峰值；槽位回收与下次 `task_alloc_slot` 归零（**复用不继承上一个占用者的读数**）；zombie reap 前仍占页，`ps` 显示其最终大小。
- **两个拷贝门会替拷贝补页**（2026-11-07）：`user_ptr_ok` / `user_ptr_writable` 跑在拷贝**之前**，遇到 not-present 页先调一次 `page_fault_try_fix`（`err` = 真实 not-present 码，写门带 bit1，所以 demand 会先问 VMA 要权限），补不上才拒绝——否则 `read()` 进一个没碰过的 mmap 缓冲回 `EFAULT`，匿名 mmap 就只有程序自己逐页戳过才可用（Linux 的 `copy_to_user` 是直接吃 #PF 补页）。该拒的照旧拒：内核地址、非规范地址、窗口里的洞、只读页底下的写，一条不变。

## 4. 进程、调度与信号

### 4.1 task 槽与复用

- `static struct task task_table[NR_TASKS]`（64 槽静态数组），槽 0 = BSP idle。
- 关键字段：`pid/generation/cpu_id/state/name/cred/tf/ctx/kstack/pml4/brk_base/brk_cur/ustack_lo/mem_pages/mem_peak/fds[16]/信号字段/parent_pid+parent_gen/rq_next+rq_cpu/con_next`。
- **父链接是 `(pid, generation)` 不是指针**：槽位复用后裸指针悬空，generation 让过期引用查不中而不是认错人。`find_task(pid)` / `find_task_gen(pid,gen)` 分工，`task_parent()` 失效返回 NULL。
- 状态机 `T_UNUSED/EMBRYO/RUNNABLE/RUNNING/SLEEPING/ZOMBIE/STOPPED`。
- 内核栈 32 KiB：顶部 `intr_frame`，其下 56 字节 `cpu_ctx`（`area[6] = glue_first`）。
- `task_alloc_slot/task_free_slot` 走 `task_table_lock`；`task_iter()` **有意无锁**，返回表内裸指针由调用方锁外使用。
- **无主僵尸由分配器兜底回收**：父链接 `(0,0)` 是"无人领养"标记（generation 从 1 起步，写它的只有 exit 时无活 init 可领养、清扫时无 init 存在）。三层语义：
  ① init 活着时是唯一收尸者，exit 重挂 `(1, init 世代)`，健康 init 面前不出现 `(0,0)`；
  ② 表未满时无主僵尸仍占槽（惰性回收），只在 `task_alloc_slot` 撞满表时逐个触发，fork 不会因此永久失败；`ps` 里这些孤儿 `ppid=0` 是"无领养者"不是数据损坏；
  ③ `(0,0)` 检测与 `T_EMBRYO` 占位在 `task_table_lock` 内，地址空间销毁/释放栈/`task_free_slot`（自带同一把锁）**全在放锁之后** —— 占位挡并发双回收、放锁避免自锁、`for(;;)` 重试兜住最后领取；占位勒死后崩溃则槽位泄漏，但那时已 panic 停机。

### 4.2 fork / execve / exit

- `sys_fork`：槽 + 内核栈 + pid → 新 PML4 → `dup_user_aspace`（**COW 共享**，见 3.5）→ `vma_dup` 深拷 mmap 列表（失败与页表 dup 同款回滚）→ 克隆 fd 表（逐个 `refcnt++`）→ 复制 brk/`ustack_lo` → 伪造子进程 `intr_frame`（rax=0、开 IF）→ frame 下方构造 `cpu_ctx` 以 `glue_first` 为入口 → 入队。
- `sys_execve`：`copy_user_str` 取路径（128 字节上限，argv ≤16×64）→ 读整个文件 → 新 PML4 + 16 KiB 栈 → **先 `vmm_switch_to(new)` 再 `elf_load`**（装载器 `memcpy` 直写用户地址）→ 失败切回旧并回滚 → 成功**先**赋 `current->pml4` **再**销毁旧地址空间、`vma_free_all`（新镜像零 mmap；失败回滚路径在赋值之前返回，调用方列表原样保留）→ 关 `O_CLOEXEC`、清 `signal_pending` → 按 SysV ABI 摆 `argc/argv/envp/auxv`。
- `sys_exit`：关所有 fd → 撤控制台等待者 → **`fb_disown(current->pid)`（画屏归属唯一释放点，所有死法都经过）** → 孤儿重挂 pid 1（垂死 init 不领养自己、僵尸也不领养 → `(0,0)`）→ 给父 `SIGCHLD` → `T_ZOMBIE` → 出队 → 唤醒父 → `schedule()`。
- `kernel_spawn`：第一个用户进程，类 execve 但简化，结尾**切回 master PML4**（跑在内核上下文）。
- `elf.c` 只接受静态 `ET_EXEC`，`e_entry ∈ [USER_BASE, USER_MAX_VMA)`，返回 `brk_end`；不支持动态链接/共享库/需求分页。
- 用户内存访问：`user_ptr_ok`（逐页确认已映射）、`user_ptr_writable`（内核写用户页必须逐页确认可写，否则 CR0.WP 下 #PF 且无 fixup 表；**COW 页先在这一道门里被 `page_cow_break` 解析**——fork 后两边都只读，copy_to_user/信号帧 push/vfs 写共用这一个规则）、`copy_from/to_user`、`copy_user_str`。

### 4.3 调度（`kernel/sched/sched.c`）

- 每 CPU 一个运行队列，**push-front 单链表，实为 LIFO 不是环形**（无尾指针），只有 1 个时间量（`quantum = 6` tick = 60 ms），无优先级无公平性。
- 一把全局 `runq_lock`（总是 irqsave）串行化入队/出队/选队；idle **永不在队列上**，只作 `pick_next()` 返回 NULL 的兜底。
- `schedule()`：选 next → `runqueue_add_local()` 重排 current（**只回本核队列**：放远端会在切换完成前被抢走 → 双跑）→ `switch_to`。
- `runqueue_add(t)` 走 `preferred_cpu()`：自己空闲 → 空闲 AP（先于 BSP，保 BSP 处理设备中断）→ BSP → 最短队列。**负载均衡只在唤醒/新建时决定落位，无任务迁移**。
- `sched_tick()` 递减 quantum + 全局唤醒 sleepers；`sched_maybe_preempt` 在返回用户态前查 `need_resched`；远端唤醒发 `IPI_VECTOR_RESCHEDULE`。
- `switch_to(next)`：**只有 `prev->pml4 != next->pml4` 才切 CR3**（槽 0/511 共享），设 TSS.rsp0、更新 per-cpu `_current`、`swtch()`；idle 的 pml4 显式设 `master_pml4_phys()`（否则切到 idle 时 CR3=0 复位）。
- `swtch` 只存 7 个 u64（`r15 r14 r13 r12 rbp rbx rip`）；`cpu_ctx` 只有 `sp`，真寄存器区在任务内核栈上；`glue_first` 是新任务首调度入口，`swtch` 的 `ret` 落在 `intr_frame` 上，`POPALL; add rsp,16; iretq` 直接进用户态。
- `sched_wake_task(t)`（input_wait 用）：`T_SLEEPING → T_RUNNABLE + runqueue_add`，与 `send_signal` 同款**免锁**写法（2026-11-03 修：原实现先拿 `runq_lock` 再调 `runqueue_add`，后者内部再拿同一把锁 → 自旋死锁）；NULL/非睡眠直接 false。

### 4.4 信号（`kernel/sched/signal.c`）

- 32 槽；`SIGKILL 9`/`SIGSTOP 19` 不可屏蔽不可忽略，`SIGCONT 18` 额外唤醒 `T_STOPPED`；默认终止集只有 6 个（`INT TERM QUIT ILL ABRT SEGV`），其余默认忽略。
- per-task：`sig_handlers[32]`（`SIG_DFL`=0、`SIG_IGN`=1、否则 handler 地址）、`sig_masks[32]`、`sig_blocked`、`signal_pending`、`sig_in_handler`、`saved_tf`、`sig_saved_blocked`、`sig_fault_rip`。
- `send_signal`：入口守卫（范围 + `T_UNUSED` 直接返回，槽可能已被 waitpid 释放/复用）→ KILL/STOP 硬置位并唤醒 → 其余置位，目标在睡就转 RUNNABLE 入队。
- `do_signal_check`：三个调用点都在 `iretq` **之前**且带 ring-3 守卫（syscall 返回、IRQ 返回、异常路径）；一次只投一帧，handler 运行期间的第二信号保持 pending。
- `deliver_handler`：被打断的帧存 `saved_tf`，在用户栈伪造 `call` 帧（返回地址指 restorer 页，`rsp % 16 == 8`），改 `f->rip/rdi` 跳 handler；restorer 内容是 9 字节 `mov eax,20; int 0x80; ud2`，每个地址空间映一次，用户二进制不用自带。
- `sys_sigreturn` 仅 `sig_in_handler` 时有效（`saved_tf` 拷回 `tf`，其 `rax` 即返回值）；`sys_sigaction` 拒绝 KILL/STOP、handler 地址须 `user_ptr_ok`；`sys_sigprocmask`：BLOCK/UNBLOCK/SETMASK。

### 4.5 凭据与能力（`include/sys/cred.h`、`kernel/core/protect.c`）

| 层级 | 含义 |
|---|---|
| `CRED_USER = 0` | 普通进程，uid 1000 |
| `CRED_ROOT = 1` | uid 0，仍不能破坏 kxld 保护与其系统文件 |
| `CRED_KXLD = 2` | 最高层，任何人都进不去；kxld 不走 syscall 路径 |

- 能力位：`CAP_KILL (1<<0)`、`CAP_SYS_ADMIN (1<<1)`、`CAP_FB (1<<2)`；root/kxld 按构造拥有全部。
- `cred_setuid()` **单向降权**：root → 任意 uid 且 <0 时 tier 掉 `CRED_USER` 并清能力；普通用户改别的 uid 回 `EACCES`（不能爬升、不能横移）。
- `kxld_protect_kill()`：pid 1 任何人均不可杀，KXLD 任务不可杀。
- `protect.h` 的规则都是**纯函数**（不读 `current`），自测可用合成凭据在无用户进程时驱动。

## 5. 系统调用与 ABI

### 5.1 调用约定与分发

- 向量 `int 0x80`，编号 `rax`，参数 `rdi/rsi/r10`，返回 `rax`；IDT 128 号门 DPL=3。
- 入口桩 `isr128` 压出帧 → `call syscall_entry` → `syscall_do`（**分发表就是一个大 `switch`**）→ 写回 `rax` → 置 IF → 返回前 `do_signal_check()`。
- **只追加不重排**：唯一定义源 `include/abi/lnxrm_syscalls.def`（X-macro）——编号紧凑连续、新调用追加末尾并把 `LNXRM_SYSCALL_LAST` 加一、永不重排已有编号；`lnxrm_abi.h` 逐编号编译期校验（忘改宏直接编译失败）。
- 未知编号落 `default`，打 `[sys] unknown syscall …` 回 `-38`。

### 5.2 45 项总表（编号 0–44，连续无空洞）

| # | 名字 | 用途 | 实现文件 |
|---|---|---|---|
| 0 | `read` | 从 fd 读，含 `/dev/console` 阻塞读 | `kernel/fs/vfs.c` |
| 1 | `write` | 写 fd，控制台走共享打印锁 | `kernel/fs/vfs.c` |
| 2 | `open` | 打开/创建（`O_RDONLY/WRONLY/RDWR/CREAT/TRUNC/APPEND/CLOEXEC`）；访问位错配在读写端拒 | `kernel/fs/vfs.c` |
| 3 | `close` | 关闭 fd（最后一个 fd 关闭即同步脏扇区） | `kernel/fs/vfs.c` |
| 4 | `lseek` | `SEEK_SET/CUR/END` | `kernel/fs/vfs.c` |
| 5 | `brk` | 调整用户堆，带配额算术门 | `kernel/core/syscall.c` |
| 6 | `getdent` | 目录枚举（cookie 迭代） | `kernel/fs/vfs.c` |
| 7 | `dup2` | 复制 fd | `kernel/fs/vfs.c` |
| 8 | `nanosleep` | 睡眠；信号打断回 EINTR 并回填余量 | `kernel/sched/sched.c` |
| 9 | `getpid` | 进程号 | `kernel/core/syscall.c` |
| 10 | `fork` | 复制进程（地址空间 COW 共享 + VMA 列表深拷） | `kernel/sched/task.c` |
| 11 | `execve` | 装载静态 ELF64 | `kernel/sched/task.c` |
| 12 | `exit` | 退出（释放画屏归属） | `kernel/sched/task.c` |
| 13 | `wait4` | 等子进程，支持 WNOHANG | `kernel/sched/task.c` |
| 14 | `kill` | 发信号，kxld / 同 uid / `CAP_KILL` 三道闸 | `kernel/core/syscall.c` |
| 15 | `uname` | 系统标识 | `kernel/core/syscall.c` |
| 16 | `sigaction` | 装信号处理器 | `kernel/sched/signal.c` |
| 17 | `sigprocmask` | BLOCK / UNBLOCK / SETMASK | `kernel/sched/signal.c` |
| 18 | `getppid` | 父进程号（无父为 0） | `kernel/core/syscall.c` |
| 19 | `ps` | 内核填任务表，带 `CAP_SYS_ADMIN` | `kernel/sched/task.c` |
| 20 | `sigreturn` | 从 handler 返回，恢复被中断现场 | `kernel/sched/signal.c` |
| 21 | `diskinfo` | 枚举块设备，带 `CAP_SYS_ADMIN` | `kernel/core/syscall.c` |
| 22 | `mkdir` | 建目录 | `kernel/fs/fat32/fat32_meta.c` |
| 23 | `fb_info` | 读帧缓冲几何（**两道门都不过**） | `kernel/core/syscall.c` |
| 24–27 | `fb_clear/fill/char/puts` | 绘图，过 `CAP_FB` + 归属门 | `kernel/drivers/framebuffer.c` |
| 28 | `unlink` | 删文件 | `kernel/fs/fat32/fat32_meta.c` |
| 29 | `rmdir` | 删空目录 | `kernel/fs/fat32/fat32_meta.c` |
| 30 | `rename` | 改名 / 移动 | `kernel/fs/fat32/fat32_meta.c` |
| 31 | `setuid` | 单向降权 | `kernel/core/protect.c` |
| 32 | `fb_flip` | 把离屏后缓冲推上前台 | `kernel/drivers/framebuffer.c` |
| 33 | `sync` | 缓存里所有脏扇区交给各自设备 | `kernel/fs/vfs.c` |
| 34 | `kbd_get` | 非阻塞取一个键（0..255），空回 `EAGAIN`；`CAP_SYS_ADMIN` | `kernel/irq/isr.c` |
| 35 | `kbd_inject` | 代打键序列进输入环（唤醒 waiter），≤256 字节；`CAP_SYS_ADMIN` | `kernel/core/syscall.c` |
| 36 | `kbd_info` | 输入环计数快照（32 字节，两道门都不过） | `kernel/irq/isr.c` |
| 37 | `mouse_get` | 非阻塞弹一个指针事件（16 字节，空回 `EAGAIN`）；`CAP_SYS_ADMIN` | `kernel/core/syscall.c` |
| 38 | `mouse_info` | aux 环计数快照（32 字节，两道门都不过） | `kernel/drivers/ps2mouse.c` |
| 39 | `input_pop` | 统一输入队列出一个 24 字节事件（成功回 1，空回 `EAGAIN`）；`CAP_SYS_ADMIN` | `kernel/drivers/input_event.c` |
| 40 | `input_info` | 统一队列计数快照（32 字节，分类型和恒等 `pushed_total`） | `kernel/drivers/input_event.c` |
| 41 | `input_wait` | 阻塞弹出：空则睡 wait list，事件到达唤醒一个，信号打断 `EINTR`，ms 超时（0=非阻塞、-1=永等），成功 1 / 超时 0；`CAP_SYS_ADMIN` | `kernel/drivers/input_event.c` |
| 42 | `mmap` | 匿名映射：`addr` 必须 0、top-down 选址 `[MMAP_LO, MMAP_TOP)`、只登记 VMA（需求分页），`prot` 为 `LNXRM_PROT_*` | `kernel/core/syscall.c` + `kernel/mm/vma.c` |
| 43 | `munmap` | 摘除 `[addr, ALIGN_UP(len))`：VMA 挖洞拆分、逐页 unmap、空页表回收；零相交/未对齐 `EINVAL` | `kernel/core/syscall.c` + `kernel/mm/vma.c` |
| 44 | `mprotect` | 把 `[addr, ALIGN_UP(len))` 改成 `prot`，**任意用户范围**（不只 mmap 段）：在场页立即翻 W/NX，未触碰的改 VMA 交给需求分页；范围里有洞 `ENOMEM`、未对齐/零长/出用户空间 `EINVAL`；授予 W 不分配（共享帧标 `PG_COW` 推迟复制） | `kernel/core/syscall.c` + `kernel/mm/vma.c` |

图形 6 个（23–27 + 32）。统一输入队列（39/40/41）把两个生产者的**已解码结果**合成一条流：`KEY` 只出自 IRQ1 扫描码解码（`kbd_inject(35)` 是合成终端输入，**不进队列**），`REL/BTN/WHEEL` 出自 aux 包尾——34–38 的环、语义、门原样保留（叠加，不改道）。

### 5.3 两道门与其它检查点

- `cap_gate(cap, name, what)`：`ps/diskinfo/kbd_get/kbd_inject/mouse_get/input_pop/input_wait` 过 `CAP_SYS_ADMIN`，`fb_*` 绘制过 `CAP_FB`；拒绝回 `EACCES = -13` 并打 `[cap] denied …`。
- `fb_owner_gate(what)`：紧接能力门**之后**，5 个绘制/发布 case 各一道归属门，拒绝同样 `-13`、打 `[fb] denied … pid A holds the surface`。
- **顺序是硬的：先能力、后归属**（倒过来普通用户会听到"你不是 owner"，而它连碰都不该碰）；`fb_info` 两道门都不过。
- `kill`：`kxld_protect_kill()` → 同 uid 或 `CAP_KILL` → `pid == -1` 广播另要 `CAP_KILL`。
- 归属状态 `fb_owner` 存在 `framebuffer.c`（"屏归谁"是驱动的事实），释放只在 `sys_exit` 一处。
- 走字符串的调用经 `copy_user_str`；走指针的由调用方自己校验——**没有统一前置检查**，加新调用必须自己记得验。

### 5.4 共享 ABI（`include/abi/lnxrm_abi.h`）

内核与用户态共同包含，所有跨态结构体的唯一权威：

| 项 | 规格 |
|---|---|
| `enum lnxrm_errno` | 21 个 `LNXRM_E*`（`EINTR -4`、`EAGAIN -11`、`EACCES -13`、`ENOSYS -38`…） |
| `enum lnxrm_syscall_number` | 由 `.def` 生成的 `SYS_*` + `LNXRM_SYSCALL_FIRST/LAST` + 逐编号校验 |
| 信号号 / `lnxrm_sigaction` | `LNXRM_SIG*`、`SIG_DFL/IGN`、BLOCK/UNBLOCK/SETMASK / 24 字节 |
| `lnxrm_timespec / utsname / ps_entry / dirent` | 16 / 320 / 48（含 `kind/uid/mem_kib/peak_kib`）/ 72 字节 |
| `lnxrm_fb_info / fb_rect / fb_char / fb_str` | 16 / 20 / 20 / 24 字节，`_Static_assert` 锁布局 |

⚠️ **ABI 结构体一改，引用它的用户态程序必须全量重编**（否则按旧步长读错位字段）。用户态入口是 `ulib.h` 的 `int 0x80` 内联包装（第三参数手动搬 `r10`，clobber 必含 `r10`），45 个全覆盖；`crt0.S` 从栈取 argc/argv 调 `main`。

## 6. 文件系统

### 6.1 层次

```
kernel/fs/
  vfs.c           VFS：vnode/file/fs_ops/fd 表/路径解析/挂载/设备节点/read·write·close·dup2
  blk_cache.c     64 项时间戳 LRU 扇区缓存 + blk_io_*（唯一驱动调用点）+ 故障注入 + 统计
  mbr.c           MBR 分区表解析（整盘 FAT 挂不上时的第二步 fallback）
  initramfs.c     newc cpio 只读根（`make INITRAMFS=` 链进镜像），无盘时充当 /
  fat32/          fat32.c 挂载与 FAT 链 · fat32_dir.c 目录扫描与 LFN
                  fat32_file.c 文件读写 · fat32_meta.c 目录项增删改 · fat32_journal.c redo 日志
```

### 6.2 VFS 对象

| 对象 | 内容 | 生命周期 |
|---|---|---|
| `struct vnode` | `{type, size, fs_data, ops, mnt_data}` | **每次 open 在栈上创建并拷贝**，非持久 inode——两个 fd 的 `pos` 各自独立，无 link count |
| `struct file` | `{ops, pos, priv, is_dir, flags, refcnt}` | `kmalloc`，引用计数：`vfs_close_file` 用 CAS 循环只减调用者真有的那份，refcnt ≤ 0 的多余 close 打日志并忽略（**计数永不回绕、对象不二次 free**） |
| `struct dir_iter` | `{node, ops, cookie, mnt_data, err}` | `V_DIR` 的 `file->priv`；`err` 记住扫描已因 I/O 错误终止，下次 `getdent` 继续报错而不是"到头了" |
| `struct fs_ops` | 12 钩子：lookup/getdent/read/write/**size/close_node**/create/mkdir/unlink/rmdir/rename/freespace | `size` 给 `O_APPEND` 取**实时**大小；`close_node` 由 fs 决定节点释放方式（FAT `kfree` resolve，initramfs 空操作） |
| `struct file_ops` | read/write/getdent/close（**没有 lseek**） | 普通文件 close 即 `reg_close`：先 `close_node` 归还 fs 节点、再释放 vnode 拷贝（refcount 门控恰跑一次） |

- fd 表每任务 `NR_FDS 16`；挂载表 `MAX_MOUNTS 4`。
- `vfs_lookup`：`fs_for_path()` 按**最长前缀 + 组件边界**选出唯一挂载项（`"/"` 不再吞掉 `/alt`），成功/创建/截断三条出口都把 `vn->mnt_data` 置成**该条挂载的 priv**（`dir_iter.mnt_data` 同源），失败再试 `dev_lookup`（虚拟节点 `mnt_data = NULL`）；`O_CREAT` 试 `create` → `O_TRUNC` 试截断；三处失败各有出口。`abs_path()` 超长直接 `ENAMETOOLONG`，不写半个路径。
- 挂载入口只有 `vfs_mount_fs(ops, priv, prefix)`（前缀必须 `/` 开头、同前缀不重复）与 `vfs_umount(prefix)`（sync → 去激活 → 重算 `disk_ready`，**不 free priv**：挂载数据与其他挂载共享）；两者都**不是 syscall**。`vfs_free_space(path)` 是 `fs_ops.freespace` 的唯一调用者。跨挂载点 `rename` 回 `EXDEV`。
- 块注册表 `blk_list[BLK_MAX=8]`：`blk_register()` 回 0 / `EFAIL`，满槽打 `[blk] … NOT registered` 并**保持表不变**；分区在尝试挂载前就入表，所有权归注册表、永不被挂载失败回收。
- 设备节点只有 `/dev`、`/dev/console`；console 读阻塞等待输入，**信号 EINTR 优先于睡眠**，arm waiter 后二次复查竞态，等待者是 `con_next` 链表（多读者互不覆盖）。
- 访问语义：`reg_read/reg_write` 按 fd 访问位放行（`O_RDONLY` 拒写、`O_WRONLY` 拒读，皆 `EBADF`）；`O_APPEND` 每次写落 fs **实时** size；`getdent` 缓冲 < 单条 dirent（72 字节含 0）即 `EINVAL`（小缓冲不回 0 让调用方忙等）。

### 6.3 错误契约（自下而上，每层只翻译、不吞）

| 层 | 回答什么 |
|---|---|
| `blk_io_read/write` | 驱动的非 0 原样上抛；注入故障回 `EIO`。**全内核只有这里调 `dev->read/write`** |
| `fat_read/write_sector` | **只有 0 或 `LNXRM_EIO`**，不透传驱动私有码 |
| `fat_scan_dir` / `fat_resolve_path` / `fat_dir_iter` | 三态 `1 / 0 / 负`：「读不到」与「没找到」「扫到末尾」不共用一个值 |
| 语义层 `fat_*_impl` / VFS | 负数一路向上传，`EIO` 不被改写成 `ENOENT`/`EACCES`/`0` |

对外表现：限流 16 条的 `[fat32] I/O error <op> LBA <n>`，操作回 `LNXRM_EIO = -5`。

### 6.4 路径不变式（同一扇区只走一条路）

| 区域 | 读 | 写 |
|---|---|---|
| 数据/目录（≥ `data_start_lba`） | `fat_read_sector/cluster` → 块缓存 | `fat_write_sector` → 块缓存（写回） |
| FAT 区 `[reserved, reserved + num_fats*fatsz)` | `m->fat_pages`（分帧驻留内存） | `fat_set_entry` 直写 / 入日志事务 |
| 挂载期（BPB、FAT 表、MBR） | `blk_io_*` 直读 | — |

**FAT 区永远不进块缓存**：FAT 更新必须先落盘才能被指向它的目录项暂存；改走缓存两者都成暂存态，按槽回写的 `blk_cache_flush` 就会写出"目录项先于 FAT"的窗口。`ktest/t_cache.c` 逐扇区断言 FAT 区一个都不在缓存里。

**FAT 表分帧驻留**：`m->fat` 不是一块连续缓冲，而是 `fat_npages` 个 4 KiB 帧（`m->fat_pages[]`，每帧 `pmm_alloc()` + `PHYS_TO_VIRT`）。原因：FAT 大小正比分区（16 GiB 分区 = 8 MiB 表），`kmalloc` 受 buddy 最大块 4 MiB 约束，整表 kmalloc 在真实大卷上必失败。分帧后表多大只受物理内存限制（硬上限仍是挂载层的 2^28 入口 = 1 GiB 表）。表项 4 B 与扇区 512/1024/2048/4096 B 都整除 4096，**表项与扇区永不跨帧**：`帧 = off / 4096`、`帧内 = off & 4095`。访问收敛到三个 helper（`fat_entry_get` / `fat_entry_put_raw` / `fat_sector_ptr`），驱动与 ktest 只走它们；挂载用 128 KiB（256 扇区 = AHCI 单次上限）bounce 分批散射读，命令数与整表单读一致（逐帧读在真机上每 4 KiB 一个非 NCQ 往返）。

### 6.5 块缓存（`kernel/fs/blk_cache.c`）

- 64 项 × 512 B = 32 KB，键 `(dev, lba)`，全局一块，所有块设备共享。
- **时间戳 LRU**：条目一个 `u64 stamp` 取自 `lru_seq`；命中/填充/暂存都 `++lru_seq`，受害者 = 空槽优先否则 stamp 最小。
- `blk_io_read/write` 是唯一咽喉：先查故障注入、再计数、再问驱动。`blk_inject_io(dev,lba,ops)` 只给自测，**缓存入口也有注入点**（否则"扇区正好在缓存里"会把故障挡掉）。
- 读失败时**作废 victim 槽**（驱动可能写了一半才拒绝，留着下次命中就是把半覆盖扇区当好数据）。
- 统计 `hits/misses/evictions/writebacks/dev_reads/dev_writes`（原子递增）；`blk_cache_lookup` 返回槽号且**故意不动 stamp**；`blk_cache_purge` 写回+丢弃（`dev=NULL` 全表），写回失败保留脏槽。
- **持久化同步**：三道门都进 `blk_cache_sync_all()`——扫 64 槽把脏扇区交设备，**写回失败保持脏**（是唯一拷贝）、最后错误上抛。① BSP tick 每秒 `blk_cache_tick`（只在 `current != NULL` 且非 panic 时出力，保证 ktest 计数确定性）；② 普通文件最后一个 fd `close` 即同步（目录/字符设备无数据不触发）；③ `panic()` 走 `blk_cache_sync_all_try()`（`spin_trylock`，panic 持有的锁**绝不等待**）；④ `SYS_sync(33)` 同步门（userland `ksync()`）。sync 只把当前脏扇区**提前**送设备，不会重排 journal 的「先刷日志再盖提交戳」（两侧都串行于 `cache_lock`/`ide_lock`）。

### 6.6 FAT32 与 on-disk redo journal

**挂载**（`fat_mount_impl`）：读扇区 0 → 校验 BPB（55AA @[510]、`"FAT32"` @[82]、簇因子 2 的幂；`bytes_per_sector == 512` —— 全部扇区缓冲与块缓存按 512 搬移，≠512 直接拒；spc/fatsz/reserved 非零、`num_fats ∈ {1,2}`、`root_cluster >= 2`）→ 算 `data_start_lba`/`max_cluster` → **FAT 分帧读进内存**（表项数 > 2^28 直接 `reject`；`max_cluster` 是独占上限，最后 FAT 格可达）→ `fat_journal_mount` 回放。

**目录槽模型**：可用槽 = 从未用过的尾槽（0x00，终止符本身）与已删除记录（0xE5）；其余首字节都是活记录——**包括 0x05**（规范里 8.3 别名对 0xE5 的转义，而 LFN 第五片首字节恰是序号 5）。写端两处认领（`parent_entry_add()` / `dir_claim_run()`）共享同一判定；槽不够先 `dir_chain_extend()` 扩链（分配簇→尾链→整簇清零，失败自回滚）再落位——**目录永不再因满而 ENOSPC**；目录链最后一个槽从不可认领（终止符必须活过每次落位）。**扫描端止于 0x00**，尾槽之后的残留字节浮不出伪记录；坏簇/自环被步数预算或自环即断拦下。

**目录游标**：getdent 的 cookie 是 `(簇<<32)|字节偏移` 的物理恢复点（`DIRIT_START=0`、`DIRIT_END=(u64)-1`），引擎按扇区步进（32 字节记录不跨 512 扇区）、整簇级跳过零设备读——一条 readdir 的扇区查找计数 ≈ 链内扇区数 + 条目数，不再每次从首簇重扫 O(n²)；坏/越界 cookie 一律回"已结束"不编造条目。

**LFN 读验证**：长名 run 由带 0x40 位、序号最高的记录开启，之后各记录须同别名 checksum 且序号严格递减；缺失/乱序/异 checksum 即弃用整个 run 回落 8.3。拼接缓冲 260 字符 + 终止符，`lfn_reset()` 整段清零。

**写路径**：元数据（目录项/FAT/FSInfo）走 redo 日志事务，文件数据字节走写回缓存。写需求先按 `off + n` 做 u64 回卷感知合并，`alloc_chain` 以 `fat_chain_cap_bytes()`（`MIN(FAT_CHAIN_MAX_STEPS, data_clusters) × cluster_size`）为上界：越过本卷最大可表达链的写**先拒 `ENOSPC`、零分配**。

**journal 契约**（`fat32_journal.c`）：

- 落在 mkfs 的 reserved 空闲扇区 `[8, reserved_sectors)`（Makefile 给 `mkfs.vfat -R 1024`）。扇区 8 是 superblock：magic `JRN1`/version/state/nrec/head/CRC32；每条记录 2 扇区（头 + 512 字节后像）。`head` 是**相对 `j_start` 的偏移**不是绝对 LBA——写入端与 replay 端必须一致。
- 每个变更 op 一个事务，locked wrapper：`fat_journal_begin → *_impl → fat_journal_commit`，**失败路径也 commit**（impl 可能已分配簇或改了半截，留在 RAM 而磁盘看不见正是交叉链接的来源）。
- commit 顺序就是协议：① flush 文件数据（ordered，数据字节不入日志）② 写记录 ③ superblock 置 **COMMIT——唯一提交点** ④ `jrn_write_home` 逐条 apply ⑤ flush ⑥ 置 CLEAN。**④ 失败必须保留 COMMIT**（清掉等于把"可恢复的半途"变成永久丢失）。
- 事务期间后像只在 RAM（按 lba 去重），读己经写由 `fat_read_sector → fat_journal_read` 提供；缓冲满时**中途 checkpoint 再续**不静默丢弃。
- `fat_journal_mount` 在加载 FAT 之后跑：superblock 过期/损坏则重建，`state == COMMIT` 则幂等 redo 全部记录（重放的 FAT 扇区同步回 `m->fat_pages`），完成后置 CLEAN。失败语义：只可能泄漏簇、不交叉链接。

**簇链安全**：`FAT_CHAIN_MAX_STEPS` 限制步数，`fat_chain_limit = MIN(上限, 数据簇数)`；非法首簇直接拒绝并告警；坏 FAT 项只报告不跟随；**单簇自环**在 `fat_chain_advance` 当即断链 + `chain loop` 告警（数据读取会合法"回放同一簇"直到字节量耗尽、永远触不到步数预算，靠这条第一时间暴露）。

### 6.7 挂载顺序与回退

1. `vfs_try_mount_disk()` 按 `blk_list[]` 快照逐盘（遍历中分区会入表，快照避免把分区当盘再解析一遍）：先"整盘 FAT32"，失败再试"MBR 分区表"（`mbr.c` 把分区包成 `part_dev`，读写转发 `blk_io_*`；**每个非扩展分区先 `blk_register` 再试挂载**，盘名在对象里、每个分区各自一份）。
2. **挂载点分流（C51）**：能挂上的卷先问 `volume_has_init()`——有 `/bin/init` 的占 `/`，其余挂 `/mnt`（`home_volume()` 定"家"、`find_volume()` 按路径选卷，`refresh_root()` 在根变化后重算）；`main.c` 按三分支打印提示（根在盘 / 根在 initramfs / 数据盘在 `/mnt`）。
3. `mbr.c` 解析期掐死坏输入：签名 55AA 必须；每表项验 `lba + count <= 盘容量`，越界（含 count==0）报 `[mbr] … outside the disk` 后丢弃；EBR 链真正遍历（表 0 是相对 `ebr+rel` 的逻辑分区、表 1 是相对前移的链接，上限 32 段，`MBR_MAX_PARTITIONS` = 4 主 + 32 逻辑）。
4. 根没就绪或缺 `/bin/init` → `vfs_mount_initramfs()` 用 newc cpio 当根，它复用 `umount_prefix("/")` 先把旧根去激活（与 `vfs_umount` 同一条路径）；没有 `INITRAMFS=` 的构建带空段，行为不变。

## 7. 驱动

### 7.1 串口与控制台

- `uart.c`：16550，115200 8N1 + FIFO；COM1 → IRQ4 → 向量 36。
- `console_putc` 扇出四路：帧缓冲控制台 → VGA 文本 80×25（硬件光标）→ 串口 → 4096 字节环形缓冲（`fb_console_init` 用它回放已滚出的历史）。
- `kvprintf` 持 `console_out_lock`（irqsave），与 `write(1,…)` 共用（64 字节分块拿锁，限制关中断时长）；格式 `%s %d %u %x %X %p %c %%` + 宽度/`0` 填充/`l/ll/z`。
- `panic()` 打 `[PANIC]` 后 **`fb_flip_unlocked()` 一次再 `cli; hlt`**（双缓冲下文本只在后缓冲，panic 后再没有 tick 去呈现；必须无锁版——panic 可能来自控制台路径内部，调用方已持 `console_out_lock`，再拿一把就是关中断自旋到死，T-002）。
- ANSI 三状态机 `ansi_feed`（`ESC[2J` 清屏、`ESC[nH` 定位、`ESC[...m` SGR），VGA 与帧缓冲两后端各自消费；`ESC[3J` 空操作。

### 7.2 键盘、鼠标与统一输入队列

**键盘**：IOAPIC IRQ1 → 向量 33。`kbd_process_sc` 用 set 1 → ASCII（`kmap[128]`/`kmap_shift[128]`，Shift/CapsLock/E0 前缀，无映射的键丢弃）。输入只有一条路：ASCII 进 256 字节 `inbuf`（容量 255，满即丢弃 `dropped++`），`console_read` 消费，等待者 `con_next` 链表。

三支调用：`kbd_get(34)` 非阻塞弹一个键、空回 `EAGAIN`（按无符号返回 0..255，0xFF 不与空环哨兵混淆）；`kbd_inject(35)` 喂真实 `input_push()`（waiter 照常唤醒，返回实际接受数，满环丢键如实计数）；`kbd_info(36)` 给 `{pending, capacity, pushed_total, dropped, ps2_scans}`（`ps2_scans` 是 IRQ1 自启动以来见过的全部 scancode 数，键盘"活着"的判据）。get/inject 门 `CAP_SYS_ADMIN`（要和 shell 抢共享输入流），info 只读全放行。语义与计数器在 `kernel/irq/isr.c`，门与用户副本在 `kernel/core/syscall.c`。

**鼠标**（`ps2mouse.c`）：同颗 i8042 的 aux 口走 IRQ12 → 向量 44（无鼠标回 `[mouse] no aux device` 不挂死——所有回复都带预算轮询）。`ps2_mouse_init()` 排在键盘初始化**之前**（探测全程无已注册中断，配置字节与 aux 回包读不到 race）：0xA8 使能 aux → 配置字节 → 0xFF 复位 → 0xF6 默认 → **滚轮探测**（`200/200/10`、`200/200/80`、`200/100/80` 各跟 0xF2，回 id 0x03/0x04 即 4 字节 wheel）→ 0xF4 流式 → 最后挂 IRQ12。3 字节包 `{LB,MB,RB,1,Xs,Ys,Xov,Yov}, dx, dy`：9 位符号在 flags，溢出包强制 delta=0，flags 缺 `0x08` 一致位即失步丢弃记 `resyncs`；PS/2 线上 y 正=上，解码出口取负统一成屏幕向；4 字节包第 4 字节是 4 位符号扩展滚动 delta（-8..7）。IRQ12 handler 按状态寄存器 AUX 位过滤，单中断至多倒 32 字节。

鼠标整面 = 64 槽事件环（容量 63，满丢弃计 `dropped`，事件多带 `wheel`）+ `ms_stats` 计数器；语义对 kbd 环逐项镜像。**内核不带屏上指针**。`mouse_get(37)` 门在指针校验**之前**（`EFAULT` 对坏指针在空环时也先答，refuse-then-pop-then-copy）；`mouse_info(38)` 只读全放行。ktest 驱动合成包的入口是 `ms_feed_byte`。

**统一输入事件队列**（`input_event.c` + `include/sys/input.h`）：把两个生产者的已解码结果汇成一条 63 槽流（24 字节 `lnxrm_input_event{seq,type,a,b,flags,resv}`，`type = KEY/REL/BTN/WHEEL`，`flags` 只在 KEY 上带 `INPUT_MOD_SHIFT/CAPS`）：

- **叠加不改道**：console 环与 msring 原样保留，`kbd_process_sc` 尾部追加 `input_push_key()`、`ms_feed_byte` 包尾追加 `input_push_mouse()`，两旧环与新队列互不知晓各自计数，34–38 的门与语义全部不动。
- KEY 只出自 IRQ1 扫描码解码（E0 扩展键与 break 不产事件）；`kbd_inject(35)` **不进**统一队列；鼠标一包最多三事件固定次序 BTN→REL→WHEEL（BTN 仅掩码变化、REL 仅 dx‖dy、WHEEL 仅非 0，全零无变化的包静默）。
- 满即丢新如实计数；seq 队内单调（每收受 +1），`pushed_total` 恒等于分类型计数和。
- `input_pop(39)`：门 `CAP_SYS_ADMIN` 在指针校验之前（空队时 `EFAULT` 也先答），成功回 **1**（read 风格），空回 `EAGAIN`；`input_info(40)` 只读全放行（`capacity/pending/pushed_total/dropped/keys/rel/btn/wheel`）。
- `input_wait(41)`：evq_lock（irqsave）保护的 wait list（穿过 `task->wait_next`），空队时消费者 `T_SLEEPING` + `sleep_until`（ms 截止或 `U64_MAX` 永等）停机；生产者 evq_put 成功后 evict 一个 waiter、**解锁后** `sched_wake_task`（避免 evq_lock 嵌套 runq_lock），事件留队里供被唤醒者回环 pop；信号打断回 `EINTR`，ms==0 等价非阻塞 pop、ms==-1 永等；门先于 `EFAULT`。
- ktest 钩子：`input_inject()`（绕过所有 producer 策略 verbatim 收受，seq/resv 仍由队列做主）与 `kbd_inject_scancode()`（喂进**真解码路径**但刻意不碰 `kbd_scans`——那是"硬件搬过的字节"计数器，注入不是硬件）。

### 7.3 IDE 与 AHCI

| 驱动 | 要点 |
|---|---|
| `ide.c` | 硬编码主通道 `0x1F0`、主设备、PIO、单命令 ≤256 扇区。选盘 → 4 次状态读当 400 ns → `s==0x00\|\|0xFF` 判无设备 → 等 DRDY → `0xEC` → `ide_wait_drq()`（**ERR 判定点**）→ IDENTIFY → `ata_total_sectors()` 取容量，0 则不注册。**LBA48**：`ide_issue(lba,n,cmd)` 按最后一个扇区选 `0x20/0x30`（LBA28）或 `0x24/0x34`（EXT），`last > 0xFFFFFFFFFFFF` 直接拒，首次走 EXT 打 `[ide] LBA48:`；**写完发 `0xE7` CACHE FLUSH 并由 `ide_wait_flush()` 判 BSY/ERR/DRDY**，失败打 `[ide] FLUSH CACHE (E7) failed` 回 `EFAIL`（与 AHCI 一致；`ide_wait_ready` 仍不判 ERR，是既有决定：发命令前判错会把上一条的 ERR 变成死等）。`ide_lock` 包住整个循环。轮询预算两档：探测 `IDE_PROBE_ITERS`、读写 `IDE_POLL_ITERS`（QEMU 在 vCPU 线程里同步落宿主文件，宿主回写压力下 BSY 可持数十毫秒，旧 10 万次预算会把正常命令饿成 EIO） |
| `ahci.c` | `ahci_init → pci_scan → ahciProbe`（class 过滤）→ 映射 ABAR → HBA 复位 → `ahciProbePort` → `blk_register`（多盘：`sd_names[8]` 按注册序命名）。**中断驱动**：`GHC_IE` 使能 + 每端口 `PxIM` arm INTA → `ahciRouteIrq()` 把端口中断经 IOAPIC 接到 vector 32+line 并 unmask；`issueCmd` 下发顺序 = 清 `PxIS` → **开中断** → 写 `P_CI` → 完成经 `ahciIsr()`（排空 IS、`irq_done` 回执）→ 轮询 `P_CI` 只作超时兜底（超时 dump TFD/IS/SERR 后判失败）；探测期与超时路径用 `ahciPortReclaim()` 回收 CLB/FIS/CT，停不下来则保留内存。下发 `buildH2dFis`：**先查 `IS_TFES` 再查 `PxTFD.ERR`**（完成 ≠ 成功），超时用 TSC 折算。单 PRD，缓冲必须物理连续的 512 字节。自测 `ahci_irq_selftest()`（无 AHCI 设备回 -1 即 ktest skip） |
| `pci.c` | 传统 `0xCF8/0xCFC` 配置窗口，`pci_scan` 递归（visited + 深度上限），支持多功能设备 |

⚠️ **`make run` 默认 `MACHINE=q35`，AHCI 是主路径**（`sda`）；`MACHINE=pc` 是 IDE/hda，无 SATA 控制器，`ahciProbe` 的 class 过滤对所有设备提前返回，AHCI 路径零执行、`ktest/t_ahci.c` 自动 skip；`mbr.c` 是第二步 fallback，整盘 FAT 总能先成功。`smoke` 固定 pc + TCG + `-m 256`。

容量统一走 `ata_total_sectors()`（`include/sys/vfs.h` 的 static inline）：IDENTIFY words 100-103 非 0 即权威；words 60-61 为 `FFFFFFFFh` 哨兵返回 0，调用方据此**拒绝 `blk_register`**。

### 7.4 帧缓冲：几何、8bpp、控制台

| 常量 | 值 |
|---|---|
| `FB_MAX_DIM` | 8192 |
| `FB_VMA` | `0xffffffff8d000000`（`PD_HI[104]`，4 KiB 页） |
| `FB_PD_BASE` / `FB_PD_SLOTS` | 104 / 8（16 MiB 窗，实际只有 `[104,108)` 空闲） |
| `FB_DB_PD_BASE` / `FB_DB_SLOTS` | 97 / 7（后缓冲窗 `[97,104)` = 14 MiB） |
| `FB_DB_VMA` | `0xffffffff8c200000`（`PD_HI[97]`） |
| `FB_CELL_W` / `FB_CELL_H` | 8 / 16 |

分层（自底向上）：

| 层 | 函数 | 做什么 |
|---|---|---|
| 原始像素 | `fb_store/fb_store_px/fb_put_pixel` | 按 `fb_pxsz`（1/2/3/4）写 `fb_ptr`，只裁剪不标记（标记归调用者） |
| 颜色 | `fb_pack` / `fb_rgb_px` | `0x00RRGGBB` 打包；**8bpp 就近量化到 6×6×6 立方** |
| DAC | `fb_setup_palette` | 仅 8bpp：16 VGA 色 + 色立方 + 24 级灰阶 |
| 字体 | `font8x16[96][16]` | CP437 点阵，ASCII `0x20..0x7E` |
| 绘图原语 | `fb_clear/fill_rect/draw_char/puts` | **全部裁剪到可见区域**（屏外矩形零写入），写完即 `fb_mark_dirty(x,y,w,h)` |
| 控制台 | `fbc_*` / `fb_console_putc` / `fbc_scroll` | ANSI 终端，滚动 = 逐行 `memcpy` + 清末行 |
| 归属 | `fb_owner_get / fb_claim / fb_disown` | 只记状态，不参与绘制 |

`fb_init`：映射 LFB（缓存型交给 `fb_memtype_program()` 定：本 CPU 补 PAT 的 WC 槽 + MTRR 快照/采纳/孔径 WC range，固件 UC 覆盖则如实落 UC 并打点名判定行，见 7.5）、按 DISPI 回报的 pitch 定几何、对引导加载器留空的通道布局做 15/16/≥24 bpp 兜底、建后缓冲、`fb_console_init` 回放环形缓冲；VBE 模式非法则提前返回，输出落 VGA 文本后端。

### 7.5 双缓冲契约（默认开启）

**画的与看的不是同一块内存**：`fb_ptr`（所有绘制的落笔处）被 `fb_back_init()` 改指离屏后缓冲，可见的 LFB 是 `fb_front`；只有 `fb_flip()` 把**脏区域**逐行前拷。

- **后缓冲在独立 PD 窗 `[97,104)`**（14 MiB）：不能塞进 LFB 窗（`[104,108)` 才空，slot 108 归 ACPI），也不能整块分配（buddy 最大 4 MiB，1080p32 一幅 7.9 MiB），所以逐个 `pmm_alloc()` 4 KiB 帧再 `vmm_map_kernel_page()` 拼成**虚拟连续**的面。
- **`pmm_reserve` 与别名过滤必须成对**：`main.c` 在 `pmm_init` 前按**整窗** reserve `[97,104)`，`vmm.c` 别名环把这 7 槽整体滤掉；两边都按整窗算（窗口实际用多少由 DISPI 的 pitch 决定，而那在两处代码之后才知道），条件一致 `bootinfo.fb.ok && phys_addr`（无帧缓冲时这 14 MiB 仍在 buddy 里）。
- **内存类型 WB**（与帧自带线性别名同型）：慢速 aperture 从"每像素一次 UC 写"变成"每次呈现一次逐行宽拷贝"。
- **LFB 侧写合并（2026-11-06）**：`fb_memtype_program()` 每 CPU 跑一次（BSP 在 `fb_init`、AP 在 `ap_main` 顶、`idt_init` 之前）——① `IA32_PAT` 无 WC 槽时补 `PAT[7]`（全仓无人选它：早期页表 `0x83`、APIC 用 `[2]`、ABAR/UC 兜底用 `[3]`）；② MTRR 快照与采纳：BSP（首个调用者）存 DEFTYPE+全部 variable range，AP 先关 variable range、清零自身复位态再回放、DEFTYPE 收尾（MTRR per core，AP 复位态与 BSP 不保证一致——不采纳就是 BSP=UC/AP=WC 混写同一 PTE；fixed MTRR 只达 1 MiB 以下，不镜像）；③ 固件「MTRR 开、**无 range 覆盖孔径**、默认 UC/WP」时给孔径**新增**一条 pow2 WC range（只用空槽、不碰固件已有范围；VBE 给的是 BAR 基址，BAR 恒 pow2 对齐且不小于孔径 → 块不出 BAR；撞已有 range/无空槽/对不齐即放弃并点名打日志）。固件 UC **覆盖**孔径时架构性胜过 PAT 的 WC（SDM 有效类型表），如实打 `[fb] LFB cache: IA32_PAT[i]=WC MTRR[j] UC @base -> UC` 行、不承诺 WC（QEMU 即此情形：`mtrr[0]=0xc0000000/1 GiB/UC` 盖住 `0xfd000000`）。present 拷贝尾补 `mfence`：WC 合并缓冲不排空，flip 返回后立刻读 LFB 会读到旧帧。
- **呈现只拷脏区域，且整段拷贝持 `console_out_lock`**：写入后把包围盒并入脏箱（裁剪到可见区）。present 分两段加锁：`fb_box_lock`（irqsave）内**快照并清空**脏箱，随后**逐行拷贝全程在 `console_out_lock` 下**（`fb_present_held()`）——所有后缓冲写者（控制台批次、4 个绘图 syscall）都为整批持有同一把锁，另一 CPU 的滚动/光标重绘插不进拷贝中缝（旧"拷贝在锁外"留下的正是快速滚屏时两帧叠一屏的撕裂窗）。三条结构性规则：**先写后标**（present 最多漏一帧，不会消费"还没发生"的写）、**box 锁内快照+清空**（标记要么进本次快照、要么留给下一次——标记本身也要过 `console_out_lock`，所以只能落在拷贝前或后）、**拷贝尾 `mfence`**（WC 排空）。逐行拷贝走 `fb_copy_fwd`（对齐前奏 + u64 宽循环 + 字节收尾——宽路径收尾必须先把字节指针跟到宽循环之后，否则每行末尾 `n % 8` 字节即脏箱右缘漏拷，2026-10-21 修复，`test_fb_dirty_brute` 钉住）。`fbc_scroll` 的裸 `fb_copy_fwd` 是唯一绕过 `fb_store_px` 的写，自己补标；ktest 专用 `fb_poke_back_px()` 不标记。
- **谁来 present**：控制台每字符只标记其字格；**定时器启动后**由 BSP tick 节流（`FB_PRESENT_TICKS = 2`，≤50 次/秒，总在 tick 处理器里），**写批次门 `fb_present_wanted()` 此后永久关死**——它只认 `jiffies == 0`（首 tick 前）；旧条件 `jiffies == fb_last_present` 会在同 tick 内 present 落地后把门重开，快速滚屏时控制台每个 64 B 分块都全屏 present 一次（每次拷贝都是整帧预算），输出一帧一爬（P0-A）；**第一次 tick 之前**（含 ktest 与 `kernel_spawn`）没有时钟，由控制台在**写批次边界** present——`kvprintf` 与 `console_write` 都在 `console_out_lock` 下出字，`console_out_unlock` 调 `fb_console_flush()`，**一条消息一帧**（按字符数分帧的旧方案在 ~3 KB 的整个启动日志上只出 3 帧，画面看着就是冻住的）；4 个绘图 syscall（23–26 clear/fill/char/puts）**整个原语持 `console_out_lock`**，unlock 兼作批次尾（tick 后不再 present，只管互斥）；`FB_PRESENT_CHARS = 1024` 只剩无锁输出的兜底，单批永远到不了（kvprintf 缓冲 1024 B、console_write 64 B 分块）；`panic()` 手动 `fb_flip_unlocked()`（无锁，见上）、`Stop!` 路径手动 `fb_flip()`（之后再无 tick）。三个 present 入口里批次路径（`fb_console_flush`/`fb_present_due`，调用方已持锁）直调 `_held`，`fb_flip`/`fb_tick_flush` 自己 lock→held→unlock；BSP 不可能在 tick 里自死锁（持锁者 IRQ 关）。
- **失败即退回单缓冲**：模式超 14 MiB 窗或 RAM 不足时 `fb_back` 保持 `NULL`，`fb_ptr` 留在 LFB，flip 全是空操作，其余路径一行不改。没有开关，没有可选缓冲 API。
- 归属用 `__atomic_compare_exchange_n`（SEQ_CST）而非 check-then-set（两 CPU 可能同时看到空闲屏）；`fb_disown` 只清自己持有的，拒绝也不改归属。

### 7.6 用户态图形

`ulib.h` 的 `kfb_info/clear/fill/char/puts/flip` 与 23–27、32 一一对应，参数结构体 `_Static_assert` 锁布局。23–27 落笔进后缓冲，`kfb_flip()` 一次推上前台（不调也行，tick 每 20 ms 兜底）。颜色一律 `0x00RRGGBB`（16 个 `FB_CLR_*`），坐标是像素坐标。

## 8. SMP 与 ACPI

### 8.1 ACPI（`kernel/irq/acpi.c`）

- RSDP（1 MiB 以下，恒等映射内直读）→ RSDT/XSDT → MADT；产出 `acpi_collect_ap_ids()`（启用的 local APIC/x2APIC 名单，含 BSP，上限 `MAX_CPUS`）与 `acpi_ioapic_base()`（缺省 `0xFEC00000`）。找不到 MADT 不致命——`smp_init` 回退顺序探测。
- **表读取必须经 `ACPI_SCRATCH_VA` 弹跳窗**（`0xffffffff8e000000`，`PD_HI[108]`）：真机固件常把 ACPI 表放恒等窗口外，直读就是野访问。`acpi_init()` 在 `vmm_init()` 之后、`cpu_init()` 之前。

### 8.2 AP 拉起（`smp.c` + `trampoline.S`）

```
smp_init():
 1. copy_trampoline(): memcpy 到物理 0x8000、清共享块、0x8800 建过渡 GDT
 2. 注册 IPI handler：0xF0 reschedule（置 need_resched）、0xF2 stop（cli;hlt）
 3. 枚举 AP：MADT 名单非空按名单，否则 try_id = 1..MAX_CPUS-1 顺序探测
 4. ap_startup(apic_id, cpu_id, pml4)（持 smp_lock）:
      trampoline_setup(pml4, 栈顶, cpu_id, gdt, ap_main 的【虚拟地址】)  ← 传 VA 不传 PA
      INIT + mdelay(10) → SIPI(0x08) + mdelay(2) × 最多 3 次
      轮询 trampoline_is_ready()，100 ms 超时；失败补一发 INIT 复位 + trampoline_clear()
```

共享块 `struct trampoline_data`（`include/sys/smp.h` ↔ `trampoline.S` 手工偏移，物理 `0x9000`）：`0x00 page_dir`、`0x08 stack_top`、`0x10 ready`、`0x18 gdt_packed`、`0x20 ap_main 虚拟地址`、`0x28 其余字段`。

`ap_main(ap_id)` 顺序是硬约束：① `cpu_table[ap_id]` 单点初始化 `started = false`（BSP 不预填）② `apic_enable()` 必须最先（x2APIC 下访问 MMIO 会 #GP）③ `gdt_init_cpu` → `cpu_set_gs_base`（GDT 之后才能设 GS）④ `idt_init()` → 修 CR0/CR4 → `cpu_protect_init()` ⑤ 建运行队列环、TPR=0、屏蔽 8259 ⑥ `sched_create_idle()` → `started = true` → **`trampoline_signal_ready()`（ready 必须先于一切控制台输出**：慢控制台会吃光 BSP 的 100 ms 预算，BSP 判失败后发恢复 INIT 会把正持 `print_lock` 的 AP 复位）⑦ 打印回执 → `lapic_timer_start(32, HZ)`（用 BSP 缓存 period，不碰 PIT；`jiffies` 只归 BSP）→ `online` → `for(;;){schedule(); sti; hlt;}`。

### 8.3 LAPIC / IOAPIC（`kernel/irq/apic.c`）

- **xAPIC（MMIO）与 x2APIC（MSR）双路**，每核 `apic_enable()` 各自定型（`s_x2apic` 从不跨核继承）。xAPIC 下映射进 `DEV_VMA` 两个相邻 4 KiB 页（`PG_PCD`）；x2APIC 走 MSR `0x800 + (reg>>4)`，ICR 一次 64 位写。只懂 MMIO 的驱动在 x2APIC 机器上会把 INIT/SIPI 写进黑洞，AP 永远起不来。
- API：`apic_init / apic_enable / apic_get_id / apic_send_ipi / apic_send_init_ipi / apic_send_sipi`（vector 0x08 → 物理 `0x8000`）`/ apic_eoi / ioapic_set_irq / mask_irq / unmask_irq`。
- 当前只用于 INIT-SIPI-SIPI 与 IPI，不接收设备中断。

### 8.4 时钟与锁

- `HZ 100`；**PIT 不驱动时钟**（模式 2 只给 LAPIC 标定提供时间窗）。BSP 的 LAPIC 定时器 → 向量 32 → `pit_handler`：BSP 递增 `jiffies`，每 CPU 跑 `sched_tick` / `sched_maybe_preempt` / `fb_tick_flush()`。
- `lapic_timer_calibrate()` 在 `smp_init` 前只跑一次，period 与 `tsc_per_ms` 供所有 AP 复用；`mdelay()` 用 `tsc_per_ms` 换算。
- 锁（`spinlock_t`，只有 `spin_lock_irqsave`/`spin_unlock_irqrestore` 一对，初始化 `SPINLOCK_INIT`）：

| 锁 | 位置 | 保护 | 备注 |
|---|---|---|---|
| `runq_lock` | `sched/sched.c` | 所有 per-CPU 运行队列 | 总是 irqsave |
| `task_table_lock` / `pid_lock` | `sched/sched.c` / `task.c` | 任务槽分配状态 / pid 计数 | |
| `pmm_lock` / `kheap_lock` | `pmm.c` / `kheap.c` | buddy 空闲链+位图 / 堆分箱 | |
| `cache_lock` | `blk_cache.c` | 块缓存 | **跨越设备 I/O** |
| `fat_fs_lock` | `include/fat32.h` | 全部 FAT32 操作 | 与 `cache_lock` 嵌套 |
| `print_lock` | `print.c` | `kvprintf`、`console_write`，**并覆盖 present 的整段逐行拷贝** | 内部名 `console_out_lock`；绘图 syscall 23–26 也整原语持它 |
| `fb_box_lock` | `framebuffer.c` | 双缓冲脏箱（present 锁内快照+清空） | 拷贝移交 `console_out_lock` 持有 |

`fat_fs_lock` 是全局锁串行化整个文件系统：简单正确但零并发度，是双核下 FS 性能瓶颈。

### 8.5 中断与异常

- `entry64.S` 的 `ISR_NOERR`/`ISR_ERR` 压出统一 `struct intr_frame`（`r15…rax rbx` 然后 `intno err rip cs rflags ussp usss`；`rbx` 刻意排 `rax` 之后——SysV callee-saved）。
- `isr_common` 三段分流：`0xF0..0xF2` → `ipi_dispatch` → `apic_eoi`；`< 32` → 异常（ring-3 转信号，ring-0 panic）；`≥ 32` → **先 `apic_eoi` 再调 handler**（handler 可能 `schedule()` 切走，延迟 EOI 会卡住该向量）；返回用户态前 `do_signal_check()`。
- `idt_init`：0–47 → `isr_stub_table`；128 → `isr128`（DPL=3）；**49–255 全指向 catch-all**，IPI 向量必须显式覆盖否则重调度 IPI 被当 #XM。
- #PF 错误码逐位解码，用 `rflags.AC` 区分 WP 违例与 SMAP 违例（错误码相同）；ring-0 入口无条件清 `AC`。
- 向量：32 = LAPIC timer，33 = 键盘，36 = COM1，`0xF0` reschedule、`0xF2` stop，spurious IRQ7/15 有专门分支。
- 保护位：`EFER.NXE`（`vmm_init` 兜底）、`CR0.WP`、`CR4.SMEP`、`CR4.SMAP` 由 BSP `cpu_init` 与 AP `ap_main` 共同走 `cpu_protect_init()`。

## 9. 自测

### 9.1 内核自测（`ktest/`）

框架与用例分离：`ktest/ktest.c` + **27 个 `t_*.c`**；`KTEST(suite, stage, fn)` 注册进 `.ktest` 段，按 `EARLY/MM/LATE` 三阶段在 `start_kernel` 里跑。编译开关 `KTEST=1`；`make smoke` 强制 `KTEST=1`，**直跑 `python3 scripts/smoke_test.py` 会绕过它**，把内核建成无自测版本。

当前判据（2026-11-07 q35 boot 日志，加用例先改这里）：

| 阶段 | cases | checks | failed |
|---|---|---|---|
| early | 25 | 404 | 0 |
| mm | 17 | 437 | 0 |
| late | 95 | 3467 | 0 |
| 合计 | 137 | 4308 | 0（`[2 skipped]`、`[1 warn]`） |

汇总行 `[ktest] selftest PASS (137 cases, 4308 checks) [2 skipped]` 与 `[selftest] 4308/4308 pass [1 warn]`；那个 warn 是用例自己打的**故意告警**（`WARN` 只计数永不判负，`ASSERT` 失败计失败，`BUG_ON` 任何模式都停机）。`MACHINE=pc` 下同判据 0 failed（`ahci` 整例 skip + early 少 21 checks，均为机器差异，未在此钉具体数字）。

32 个 suite：`abi ahci blk blkcache blkio boot console cpu cred draw elf fat32 fatfile fb input jrn kbd kheap ktest lib lock mbr mmap mouse oom pmm prop syscall task usercopy vfs vmm`，覆盖 ABI 布局与编号连续性、E820/命令行、锁与 TSC、CR0/CR4 保护位、凭据与能力、绘制与帧缓冲、ELF 拒绝野入口、分箱堆与 OOM 注入、块缓存 LRU、I/O 错误注入、journal 先写日志后落盘、系统调用指针门、信号与任务、VFS/FAT32（含多盘挂载分流）、块注册表（分区与满槽）、AHCI 中断路由与首命令中断完成（无 AHCI 设备即 skip）、键盘/鼠标/统一输入队列、mmap 分发与需求分页/COW fork/`mprotect` 授受与共享帧、栈守护与 RSP 邻近、#PF 错误码逐位判别、拷贝门补页（`t_mmap.c` 十例）。

### 9.2 端到端冒烟（`scripts/smoke_test.py`，`make smoke`）

六阶段全过才退出 0：`toolchain` → `build` → `artifacts` → `boot`（无头 QEMU，**21 条有序 marker**）→ `shell`（串口驱动 `/bin/sh`：ls、cat、systest）→ `forged FAT`（篡改簇链的镜像，要求驱动**报告**且不跟随）。

- 有序 marker 按游标顺序匹配；`--smp N` 只替换两条 SMP 相关 marker，另加一条负向断言（`-smp 1` 时不得出现 AP marker）。
- 用户态判据 `SYSTEST_TAGS` **106 条**正则（`uname.ret=0`、`unknown.99=-38`、`kill.init.term=-13`、`input.pop.empty=-11`…）外加 `systest.done=1`；键盘/鼠标自检在 systest 进程内完成。
- LATE 阶段另搜 6 条报告行（块缓存 LRU、写后读一致、FAT 区不缓存、journal 先写日志+重放），因为 `.ktest` 段内先后是编译器的事。
- 负面判据：日志不得出现 `WARN(`、`2097151`、`4294967295`、`[PANIC]`。
- 当前 `make smoke` **205/205**；transcript 写 `build/smoke/serial.log`（非默认 smp 档写 `serial-smpN.log`）。日志里的 `[fat32] I/O error` 是错误注入用例**故意**打的，不是回归。

### 9.3 用户态自检（`userland/systest.c`）

按系统调用编号分节的全量自检：错误码路径、信号与等待（含 WNOHANG）、`fb_*` 图形、故意段错误隔离、降权子进程段（`setuid(1000)` 后逐个撞权限检查点）、超长路径段、内存记账与配额段、画屏归属段（三层 fork：抢屏 → 撞墙 → 持有者退出）、键盘段（空环快照 + `EAGAIN` + 注入 `kbd0` 读回 + 降权拒绝）、鼠标段（空环快照 + 只读探针 + 降权拒绝，真实指针在环里时打 `mouse.skip`）、统一队列段（`capacity=63`/`sane`/`sum` 恒等 + 空队 `EAGAIN` + 降权拒绝），末尾打 `systest.done=1`。由 smoke 的 `shell` 阶段在串口上驱动。

## 10. 关键不变式

一条一行，改代码时逐条对：

- `include/abi/lnxrm_syscalls.def` 是编号**唯一**定义源：只追加、不重排；`LNXRM_SYSCALL_LAST` 必须随末尾编号加一（编译期校验会拦住忘改的）；未知编号一律回 `-38`。
- 图形调用的门顺序是**先 `cap_gate` 后 `fb_owner_gate`**；`fb_info` 两道门都不过；两种拒绝都回 `-13`，只靠回执区分。
- 画屏归属的释放**只在 `sys_exit` 一处**（所有死法都经过它），`task_free_slot` 不放第二份。
- `main.c` 的 `pmm_reserve` 与 `vmm.c` 的别名过滤**必须成对**，尤其双缓冲窗 `[97,104)` 两边都按**整窗**算，不按模式大小算。
- `RESV_MAX 8` 已用 7（有帧缓冲时）：新增固定窗口必须同时加 reserve 与别名过滤。
- 物理管理窗口 ≤ `PMM_WINDOW_TOP`（2 GiB）= 恒等映射 / 高半别名的范围；越界播种 = #PF，而 IDT 建好之前 = 三重故障无痕复位。
- `console_init → gdt_init → gs → idt_init` 必须是 `start_kernel` 的头四步。
- `0x6F00` 哨兵必须按 **DWORD** 读（两个写入者都只写低 4 字节）。
- `arch/kernel.ld` / `arch/entry64.S` / `kernel/mm/vmm.c` 三处页表与段布局硬编码手工同步。
- 新地址空间只复制**除槽 255 外**的 PML4 条目；槽 0/511 永远共享。
- 释放已映射的用户帧必须**先 `vmm_unmap_user` 再 `pmm_free`**，否则同一帧还两次；COW 共享帧同理——`pmm_free` 只减 `frame_ref`，归零才回 buddy（order-0 计数，`ref==0` 照旧放行）。
- `PG_COW` 只与 `PG_W` 清除**成对**出现（`dup_user_aspace` 与 `page_cow_break` 是唯二写点）；`!W !COW` 的写保护是真 rodata 必须落 SIGSEGV，`!W + COW` 才可破；`vmm_map_user` 的 widen 永不给 `PG_COW` 页回写 `PG_W`。
- COW 破环顺序恒为**先复制 → 再换 PTE → 最后 `pmm_free` 旧帧**：本方引用在复制完成前钉住旧帧内容，计数归零必然晚于所有复制者。
- mmap 窗恒为 `[MMAP_LO, MMAP_TOP)`，`MMAP_TOP` = 栈增长地板 − `STACK_GUARD`，`brk` 上限 = `MMAP_LO`；**守护区 `[MMAP_TOP, 栈地板)` 永不映射**（无 VMA、`demand_page` 拒、`mprotect` 对它回 `ENOMEM`、`munmap` 零相交回 `EINVAL`），`STACK_GROW_WINDOW == STACK_GUARD` 由静态断言钉死：堆、mmap、守护、栈四段两两不相交。
- `PG_W ⇒ pmm_refcount == 1`：授予写只给独占帧，共享者一律标 `PG_COW` 推迟到写故障复制；**撤销写必须连 `PG_COW` 一起清**，留着它会让下一次 COW 缺页把只读悄悄改回可写。
- 用户 #PF 的 `err` 位**逐位认**：`cow_fault` 只认写位（bit1）、`stack_grow` 拒取指位（bit4）——否则 NX 取指会被当成"COW 已破环"，回报已修复却不改任何东西，`iretq` 重试同一指令 → 无限故障循环；`demand_page` 先按 VMA prot 拒掉越权的写/取指，**再**取帧（先问权限，再动分页器）。
- 拷贝门遇到 not-present 页**先走缺页修复再判**（`user_page_in`），修不好才拒——内核地址、非规范地址、窗口里的洞、只读页底下的写仍然一律拒；别把「lazy」当成「unmapped」，也别把「unmapped」当成「lazy」。
- `user_ptr_writable` 是 COW 解析的**唯一门**（内核写用户内存前先 `page_cow_break`）；调用点不得各自绕过它去直写用户页。
- buddy 最大块 4 MiB（`MAX_ORDER 11`）；`kmalloc` 超 4096 字节走 buddy，OOM 返回 `NULL`，调用方必须检查。
- 内存配额唯一来源是 `cred_mem_quota()`，`struct task` 不存配额字段；页数真相是 `vmm_count_user_pages()` 的页表行走，记账与 `ps` 都用它。
- 改 `struct task` 必须**全量重编**（`struct cpu_info` 内嵌按值的 `runq_head`，其内部偏移跟着变，按旧偏移读写运行队列就是 #GP）。
- 运行队列是 push-front **LIFO**，不是环形；`schedule()` 重排 current 必须走 `runqueue_add_local()`（回远端队列会被对方抢走，双跑）。
- `sched_wake_task` 不得持 `runq_lock` 调 `runqueue_add`（后者内部再拿同一把锁 → 自旋死锁）；双重唤醒由 `rq_cpu != -1` 复查兜住。
- 只有 `pml4` 不同才切 CR3；idle 任务的 pml4 必须是 `master_pml4_phys()`。
- 信号只在 `iretq` 之前、带 ring-3 守卫的地方投递；一次只投一帧。
- `current->tf` 只发布 **ring-3 帧**（`isr_common` 里 `(f->cs & 3) == 3` 才写）：它是该任务 `iretq` 回用户态要用的那帧，`sys_execve` 改它的 `rip/ussp`、`sys_fork` 拷贝它、signal 改投它；ring-0 中断帧用完即弃，发布出去会让 execve 写死帧 → 旧 rip + 新地址空间 → 用户 `#UD`。
- restorer 页是唯一 `exec=true` 的用户页（其余用户页 NX），每个地址空间映一次。
- `blk_io_read/write` 是全内核唯一调用 `dev->read/write` 的地方（含分区层）。
- 路径路由只认**最长前缀 + 组件边界**的那条挂载项，`ops` 与 `priv` 必须成对取自它；`vnode.mnt_data` / `dir_iter.mnt_data` 是解析时的那条挂载，绝不是占位符或"按 ops 反查的那一个"。
- `vfs_close_file` 只减调用者真有的那份引用，refcnt ≤ 0 的多余 close 记日志后忽略（不二次 free、计数不回绕）；`vfs_umount` 只动路由、**不 free priv**（挂载数据共享）。
- `blk_register` 满 8 槽必须**大声拒绝且表不变**；注册表拥有分区对象，挂载失败不回收它（`diskinfo` 正在枚举这些指针）。
- IDE 与 AHCI 的写都以 `0xE7` CACHE FLUSH 收尾并**检查返回值**：`write()` 回 0 才是可承诺的落盘。
- FAT 区**永不进块缓存**；元数据走 redo 日志，文件数据字节走写回缓存（ordered）。
- journal `commit` 第 ④ 步失败必须**保留 COMMIT 状态**；失败路径也必须 commit。
- 一个扇区只走一条读写路（数据/目录经缓存、FAT 区直写、挂载期直读），三者不重叠。
- FAT 表访问只走 `fat_entry_get` / `fat_entry_put_raw` / `fat_sector_ptr` 三个 helper（分帧驻留，表项与扇区永不跨帧）。
- EOI 必须在 handler **之前**写（handler 可能切走）；IPI 向量必须显式盖过 catch-all。
- AP 的 `ready` 信号必须先于一切控制台输出；`cpu_table[ap_id]` 只在 `ap_main` 初始化。
- trampoline 传给共享块的 `ap_main` 是**虚拟地址**。
- ABI 结构体（`lnxrm_ps_entry` 等）一改，引用它的用户态程序必须全量重编。
- 自旋锁只用 irqsave 变体；`fat_fs_lock` 内嵌 `cache_lock`，加锁顺序不得颠倒。
- 双缓冲呈现只拷脏区域：**先写后标**；`fb_flip` 在 `fb_box_lock` 内快照+清空脏箱，**逐行拷贝全程在 `console_out_lock` 内**（绘图 syscall 与控制台批次整批持同一把锁）、拷贝尾 `mfence`；绕过 `fb_store_px` 的写（如 `fbc_scroll`）必须自己补标。
- 首 tick 前的控制台呈现只认**写批次边界**：`console_out_unlock` → `fb_console_flush()`，一条消息一帧；`FB_PRESENT_CHARS` 只是无锁输出的兜底，锁内单批永远触发不到。
