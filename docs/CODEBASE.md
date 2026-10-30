# lnxrm 代码库组织与子系统说明

面向要改这套代码的开发者：讲代码怎么组织、各子系统怎么工作。构建与运行见 BUILD.md 的
《快速开始》与《构建系统架构》，差距与计划见 GAP_ANALYSIS.md，项目概览见 README.md。

## 1. 总览

lnxrm 是一个 x86-64 freestanding 单体内核：C 为主体，启动 / 中断 / 上下文切换用 NASM 汇编，
产出符合 Linux x86 boot protocol 2.08 的 `bzImage`，在 FAT32 系统盘上挂根并跑自带 shell。
**语言只有 C 与 ASM 两种**（Rust、C++ 组件均已移除），构建零警告。

### 1.1 产物

| 产物 | 说明 |
|---|---|
| `build/bzImage` | 可引导镜像 = `setup.bin` + `vmlinux.bin`，两个 Python 脚本回填 boot header |
| `build/vmlinux.elf` | 链接产物（带符号与调试信息），链接脚本 `arch/kernel.ld` |
| `build/vmlinux.bin` | 按 LMA 导出的裸 payload，落在物理 `0x100000` |
| `build/disk.img` | 64 MiB FAT32 系统盘：`mkfs.vfat` + mtools 装入 `README.md` 与 `bin/` |
| `build/user/<name>` | 17 个静态 ELF64 用户程序 |
| `build/lnxrm.iso` | GRUB 引导 ISO（`make imageiso`），只含 bootloader + 内核，不含根文件系统 |

工具链 `gcc / nasm / ld / objcopy / python3`。构建阶段与目标依赖见 BUILD.md 的
《内核构建阶段详解》《目标依赖图》。两个补丁脚本是构建链的关键：

- `scripts/patch_setup_jmp.py`：链接后用 `nm` 查 `_start64`，把 `setup.bin` 里的哨兵
  `0xF00DFACE` 回填成真实的 `jmp 0x18:imm32` 目标（幂等）。
- `scripts/patch_bzimage.py`：填 `setup_sects`(0x1F1) 与 `syssize`(0x1F4)，并断言
  `HdrS` 魔数、`0x55AA` boot flag、`payload_off` 为 512 倍数且 ≥ 2048。

### 1.2 目录树

```
Makefile               唯一构建入口：源文件自动发现 + 内核/汇编/用户态三套编译规则 +
                       链接、镜像、initramfs、运行、冒烟
arch/                 架构相关：汇编、CPU 状态、链接脚本
  setup.asm           实模式：E820 采集、VBE 四遍选型、临时页表、进长模式
  entry64.S           _start32(GRUB)/_start64、中断桩、swtch/glue_first/load_cr3
  trampoline.S        AP 极简入口（被拷到物理 0x8000）
  cpu.c               GDT/TSS/IDT、IRQ/IPI 表、PIT、LAPIC 定时器标定、cpu_protect_init
  kernel.ld           单一连续 .image 段（VMA 0xffffffff80100000 / LMA 0x100000）+ .bss + 启动栈
include/              全部头文件，本目录没有实现文件
  abi/                lnxrm_abi.h（跨态结构体/错误码/信号号）+ lnxrm_syscalls.def（编号 X-macro）
  sys/                sched cred vfs cpu apic smp acpi percpu spinlock protect ktest
  mm/                 mm.h（pmm/kheap/vmm 接口与常量）、pagefault.h
  其余                 types.h boot.h console.h framebuffer.h disk.h elf.h io.h pci.h
                      font8x16.h fat32_priv.h
kernel/               内核 C 实现
  main.c              bring-up 顺序（整个内核的入口与初始化编排）
  syscall.c           分发、能力门/归属门、brk/kill/uname/diskinfo/setuid
  task.c sched.c      进程生命周期 / 调度与自旋锁 / 信号 isr.c elf.c apic.c acpi.c
  signal.c            信号投递、handler 投递、sigreturn
  smp.c               AP 拉起与 ap_main
  percpu.c protect.c framebuffer.c print.c selftest.c
  mm/                 pmm.c(buddy) kheap.c(分箱堆) vmm.c(页表) pagefault.c(#PF 分发)
  fs/                 vfs.c blk_cache.c mbr.c initramfs.c
  fs/fat32/           fat32.c fat32_dir.c fat32_file.c fat32_meta.c fat32_journal.c
  drivers/            pci.c ide.c ahci.c uart.c ps2mouse.c
  lib/string.c        memcpy memset strcmp strcpy strlen
ktest/                启动自测：ktest.c 框架 + 22 个 t_*.c 用例文件
init/                 用户态运行库（crt0.S + ulib.c/h），链接进每个用户程序
user/                 16 个用户程序，一个 .c 一个二进制
scripts/              patch_setup_jmp.py patch_bzimage.py mkinitramfs.py smoke_test.py
```

`init/` 不是 `/init` 那个程序，是**每个**用户态程序都链接的运行库；`ktest/` 在仓库根，
不在 `kernel/` 下。

### 1.3 容量常量

| 常量 | 值 | 定义处 |
|---|---|---|
| `MAX_CPUS` | 8 | `include/types.h` |
| `NR_TASKS` | 64 | `include/types.h` |
| `NR_FDS` | 16 | `include/types.h` |
| `KSTACK_SIZE` | 32768 | `include/sys/sched.h` |
| `LNXRM_NR_SIGNALS` | 32 | `include/abi/lnxrm_abi.h` |
| `LNXRM_SYSCALL_LAST` | 38（编号 0–38 共 39 个） | `include/abi/lnxrm_abi.h` |
| `CACHE_ENTRIES` | 64 | `include/disk.h` |
| `MAX_MOUNTS` | 4 | `kernel/fs/vfs.c` |
| `HZ` | 100 | `include/sys/cpu.h` |
| `MAX_ORDER` / `MIN_ORDER_BITS` | 11 / 12（buddy 最大块 4 MiB） | `kernel/mm/pmm.c` |
| `PMM_WINDOW_TOP` | `0x80000000`（2 GiB 管理窗口） | `include/mm/mm.h` |
| `RESV_MAX` | 8 | `kernel/mm/pmm.c` |
| `KHEAP_NBINS` | 9（16…4096 字节分箱） | `include/mm/mm.h` |
| `CRED_USER_MEM_QUOTA_PAGES` | 64 MiB / 4 KiB | `include/sys/cred.h` |
| `AP_STARTUP_TIMEOUT_MS` | 100 | `include/sys/smp.h` |

## 2. 启动

### 2.1 两条路径与 `0x6F00` 哨兵

两条路径在 `kernel/main.c` 的 `start_kernel` 汇合，靠**物理 `0x6F00` 的一个 DWORD**区分：

- **QEMU `-kernel`（`make run`）**：`arch/setup.asm` 在实模式下关中断、采 E820（表写
  `0x7000`，条目数在 `0x6FFC`，上限 64 条）、四遍级联选 VBE 模式（1920×1080 → 1280×720
  → 8bpp 传统 LFB 模式 → `vbe_enum_best`，每遍都要 `4F02` 真的设成功），把
  `struct vbe_lfb_info` 写 `0x8C00`，向 `0x6F00` 写 **0**（"我不是 GRUB"哨兵），建
  `0x60000` 临时页表、开长模式，`jmp 0x18:哨兵`（链接后由 patch 脚本修正）。
- **GRUB `linux`（`make imageiso`）**：`arch/entry64.S` 的 `_start32` 把 ESI 里的
  `boot_params` 指针按 **DWORD** 写 `0x6F00`（非 0），自建临时 GDT/页表进长模式，
  跳进同一个 `_start64`。此路径下 `setup.asm` 不执行，VBE 模式由 GRUB 编程并填
  `boot_params.screen_info`；内核无 EFI handover stub，仅支持 legacy BIOS/CSM。

`start_kernel` 读哨兵决定数据源：0 → E820 在 `0x7000`、LFB 在 `0x8C00`、cmdline 在
`0x20000`；非 0 → 三者都取自 `boot_params`。**必须按 4 字节读**：两个写入者都只写低
4 字节，按 `u64` 读会把内存残留拼进指针。

### 2.2 `_start64` 与 master 页表

`_start64` 重建固定物理地址的 master 页表（`arch/entry64.S`），清 `.bss`，设启动栈后
`call start_kernel`：

| 表 | 物理地址 | 作用 |
|---|---|---|
| `PML4_M` | `0x50000` | 主 PML4：`[0]` 恒等、`[511]` 高半 |
| `PDPT_M` | `0x51000` | 恒等子树，`[0]`/`[1]` = 低 1 GiB / 1..2 GiB |
| `PD_M` / `PD_M2` | `0x52000` / `0x57000` | 恒等 2 MiB 大页（0..1 / 1..2 GiB） |
| `PDPT_HI` | `0x53000` | 高半子树，`[510]`/`[511]` = `PD_HI` / `PD_HI2` |
| `PD_HI` / `PD_HI2` | `0x54000` / `0x58000` | 高半别名（0..1 / 1..2 GiB） |
| `PT_VGA` | `0x55000` | `PD_HI[96]` → 文本窗口 `0xB8000` |
| `PT_DEV` | `0x56000` | `PD_HI[128]` → 设备 MMIO（PTE 由 C 侧填） |

恒等区与高半别名各铺 **2 GiB**，这就是 `PMM_WINDOW_TOP` 的上界来源。

### 2.3 `start_kernel` 顺序

顺序即契约，`kernel/main.c` 里的编排：

1. `console_init → gdt_init → cpu_set_gs_base(&cpu_table[0]) → idt_init`
   —— **必须是头四步**：IDT 就绪前任何异常都是 #PF → #DF → 三重故障，整机无痕复位。
2. `boot_collect_e820` → 打印横幅与 e820 表 → `boot_read_vbe` → `boot_read_cmdline`。
3. `ktest_run(KTEST_EARLY)`：只要控制台能用的自测。
4. `cpu_init_percpu(0,0)` → **`pmm_reserve` ×7**（见《内存》的保留表，必须在 `pmm_init` 之前）。
5. `pmm_init → vmm_init → kheap_init`。
6. `acpi_init`（RSDP/MADT，需 `vmm_map_kernel_page`，故在 `vmm_init` 之后；
   IOAPIC 定址要它，故在 `cpu_init` 之前）→ `cpu_init` → `pit_init(HZ)`。
7. `ktest_run(KTEST_MM)`：分配器与页表自测。
8. `fb_init` → `ps2_kbd_init` → `serial_irq_init` → `vfs_init` → `ide_init` → `ahci_init`
   → `vfs_try_mount_disk()`；根没就绪或缺 `/bin/init` 时回退 `vfs_mount_initramfs()`。
9. `do_global_ctors()`（当前无 C++ 全局构造，空转）→ `sched_init`。
10. `ktest_run(KTEST_LATE)` + `ktest_summary()`。
11. `kernel_spawn("/bin/init")`；失败则打印 `Stop!`、`fb_flip()` 后 hlt 死循环
    （此时还没有 tick，没人替它呈现）。
12. `lapic_timer_calibrate`（BSP 标定一次，AP 用缓存值）→ `smp_init` →
    `lapic_timer_start(32, HZ)`。
13. 打印 `[mem] available memory` 与 `[boot] ready in N ms` → `sti` → `idle_loop()`。

### 2.4 三处必须手工同步的硬编码

| 位置 | 内容 |
|---|---|
| `arch/kernel.ld` | 压成单一 `.image` 段；VMA 与 LMA 差 `0xffffffff80000000` |
| `arch/entry64.S` | master 页表的物理地址（`0x50000`…`0x56000`） |
| `kernel/mm/vmm.c` | 用同一组常量（`PD_HI`/`PD_HI2`/`PT_FIX`）去写那些表 |

三者只有注释互相提醒，改一处必须另改两处。

## 3. 内存

流水线：**E820 → buddy 物理页 → 分离空闲链内核堆 → 页表**（`kernel/mm/` 四个文件）。

### 3.1 `pmm.c`：buddy 物理页分配器

- 播种：取**最大的一块** E820 可用区域（下界 1 MiB、上界 4 GiB、再钳到
  `PMM_WINDOW_TOP` = 2 GiB），整段落在窗口外的区域直接跳过。
  `best_end < __bss_end + 18 MiB` 直接 `panic`（防止在"没有内存"的前提下
  `memset` 抹掉自己的页表）。
- 窗口 `[base_addr, top_addr)`：`base_addr` = `.bss` 末尾 + 2 MiB 安全边距，
  `top_addr` = 堆区下方（堆在播种前由 `kheap_place` 从顶部挖走 16 MiB）。
- **侵入式空闲链**（空闲块头 8 字节写下一块指针）+ **逐 order 位图**（位图放在物理
  `BITS_BASE = 0x10000` 的安全区），`buddy_of(pa, o) = pa ^ size`。
- 播种时按最大对齐块下探，碰到保留区就**逐块收缩**到不触碰任何保留范围。
- `pmm_reserve(lo, hi)` 只是登记，`resv_hits()` 在播种时生效。
- `pmm_inject_oom(n)`：让接下来 n 次分配像空 buddy 一样失败，自测用它在内存充裕的
  机器上确定性地驱动 OOM 分支。
- 对外：`pmm_init / pmm_reserve / pmm_alloc_order / pmm_free_order`，`pmm_alloc()/pmm_free()`
  宏，`pmm_free_bytes()`。

### 3.2 PMM 保留表（`kernel/main.c`，`RESV_MAX 8`）

| # | 范围（物理） | 槽位 / 用途 | 条件 |
|---|---|---|---|
| 1 | LFB `phys .. phys+pitch*height` | MMIO，永不当帧 | 有帧缓冲 |
| 2 | `FB_PD_BASE..+fb_slots`（2 MiB 槽） | LFB 窗口 `PD_HI[104…]`，这些槽的别名已消失 | 有帧缓冲 |
| 3 | `[97,104)` = `FB_DB_PD_BASE..+7` | **双缓冲后缓冲整窗** | 有帧缓冲 |
| 4 | `96<<21 .. 97<<21` | slot 96 = VGA 文本窗 | 无条件 |
| 5 | `128<<21 .. 129<<21` | slot 128 = `DEV_VMA` | 无条件 |
| 6 | `256<<21 .. 272<<21` | slot 256..271 = `KHEAP_VMA` | 无条件 |
| 7 | `108<<21 .. 109<<21` | slot 108 = `ACPI_SCRATCH_VA` 弹跳页 | 无条件 |

有帧缓冲时 7 笔、只剩 1 槽；无帧缓冲时 4 笔。规则：**凡是被固定窗口占用的高半别名槽，
背后的物理帧必须同时 `pmm_reserve`**（否则 `PHYS_TO_VIRT()` 会落到窗口上）。

### 3.3 `kheap.c`：内核堆

- **分离空闲链**，`NBINS 9` 对应 16/32/…/4096 字节；`n+8 ≤ 4096` 走分箱，箱空时向
  buddy 要一页切块；`n+8 > 4096` 直接 `pmm_alloc_order`，header 记 `0x80|order`。
- 每个分配 8 字节头 `struct hdr { u32 bin_or_order; u32 magic; }`，`magic 0xECEC0001`，
  `kfree` 校验；大块头写在物理页首，释放时 `VIRT_TO_PHYS` 转回。
- **OOM 返回 `NULL`**，调用方必须检查；`kmalloc_or_panic()` 是给无法返回错误的
  bring-up 路径留的 panic 入口，当前零调用者。
- 堆区 16 MiB，`pmm_init` 里 `kheap_place` 从内存顶部向下预留，映射在
  `KHEAP_VMA = 0xffffffffa0000000`（`PD_HI[256..271]`，2 MiB 页）。

### 3.4 `vmm.c`：三槽地址空间与固定窗过滤

| PML4 槽 | 用途 | 共享性 |
|---|---|---|
| 0 | 低内存恒等映射（2 MiB 大页，0..2 GiB） | 全部 AS 共享，仅内核 |
| 255 | 每进程用户空间 `[0x7f8000000000, 0x7ffffffff000]` | 每进程独立 |
| 511 | 高半别名（镜像 / 堆 / VGA / 设备 / LFB / ACPI） | 全部 AS 共享 |

- `vmm_new_user_aspace()`：分配新 PML4，**复制除槽 255 外的所有条目**——内核映射对
  新进程零成本传播，不需要 refcount、不需要 TLB shootdown 计数。没帧返回 0，不 panic。
- `vmm_init()` 的别名环：把 `kernel_end .. heap_end` 内每个 2 MiB 槽映射进 `PD_HI`
  （低 1 GiB）或 `PD_HI2`（1..2 GiB），**跳过**：slot 96（VGA）、128（DEV）、
  108（ACPI 弹跳页）、LFB 窗口 `[104, 104+fb_slots)`、LFB 物理范围、
  **双缓冲窗 `[97, 97+FB_DB_SLOTS)` = `[97,104)`**；循环之后再整体覆盖 256..271 为堆帧。
  最后置 `EFER.NXE`。
- 其余关键函数：`vmm_map_user`（四级映射，`writable/user/exec` 三个权限参数，页表建不出
  来回 `LNXRM_ENOMEM`）、`vmm_unmap_user`（只清 PTE 并返回被释放的物理地址）、
  `vmm_count_user_pages`（**用户页数的唯一真相**，记账与 `ps` 都读它）、
  `vmm_translate_in`（纯四级行走）、`vmm_map_kernel_page`（4 KiB 页映进 `DEV_VMA`）、
  `vmm_switch_to`、`master_pml4_phys`。
- ⚠️ `vmm_destroy_user_aspace()` 会释放所有仍 present 的数据页：想自己释放某个已映射帧，
  **必须先 `vmm_unmap_user` 清掉 PTE 再 `pmm_free`**，否则同一帧被还两次、空闲链被毒化。

### 3.5 fork 深拷贝、NX 与懒惰栈

- `dup_user_aspace(src, dst)` 逐页 `pmm_alloc + memcpy` **深拷贝**槽 255，父子各记各的页数。
- `vmm_map_user(pml4, va, pa, writable, user, exec)`：`exec=false` 即打 `PG_NX`——
  栈、堆、数据段默认不可执行，只有代码段可执行；信号 restorer 页是唯一按
  `exec=true` 映射的用户页（`signal_map_restorer`，落在 `USER_SIGRETURN_VA`）。
- 用户栈在 exec/spawn 时只映 16 KiB（`USER_STACK_TOP - 0x4000` 起），未映射页的 #PF 若落在
  `[USER_STACK_TOP - USTACK_MAX_GROW, ustack_lo)` 内则**懒惰下扩**（`mm/pagefault.c`，
  下限 512 KiB），否则按栈溢出处理；`kernel/isr.c` 的用户态 #PF 先试
  `page_fault_try_fix()` 再转信号。

### 3.6 `brk` 与配额

- `sys_brk` 合法区间 `[brk_base, USER_STACK_TOP - 1 MiB)`，先算术拒绝再取帧。
- 判据是**页表行走**：`have = vmm_count_user_pages(current->pml4)`，配额
  `quota = cred_mem_quota(&current->cred)`（`0` = 无上限，root/kxld），
  `have + 增量 > quota` 直接回 `LNXRM_ENOMEM`；shrink 永不拒绝；部分失败回滚本次映射。
- 配额**只来自 `cred_mem_quota()`**，`struct task` 里没有 per-task 配额字段——
  `setuid` 改 tier，两套记账必然打架。
- 记账生命周期：exec/spawn/fork 深拷贝后按 `vmm_count_user_pages` 重算并重置峰值；
  `brk` 成功后 `mem_pages = have ± delta` 并抬 `mem_peak`；槽位回收（`waitpid` reap）与
  下次 `task_alloc_slot` 都归零——**槽位复用不继承上一个占用者的读数**。zombie 在被
  reap 前仍占页，所以 `ps` 显示的是它的最终大小。

## 4. 进程、调度与信号

### 4.1 task 槽与复用

- `static struct task task_table[NR_TASKS]`（64 槽静态数组，零动态分配），槽 0 = BSP idle。
- `struct task` 关键字段：`pid` / `generation` / `cpu_id` / `state` / `name` /
  `cred` / `tf` / `ctx` / `kstack` / `pml4` / `brk_base` / `brk_cur` / `ustack_lo` /
  `mem_pages` / `mem_peak` / `fds[16]` / 信号字段 / `parent_pid`+`parent_gen` /
  `rq_next`+`rq_cpu` / `con_next`。
- **父链接是 `(pid, generation)` 而不是裸指针**：槽位一复用裸指针就悬空，generation 让
  过期引用查不中而不是认错人。`find_task(pid)` 与 `find_task_gen(pid, gen)` 分工，
  `task_parent()` 解析父链接，失效返回 NULL。
- 状态机 `T_UNUSED / T_EMBRYO / T_RUNNABLE / T_RUNNING / T_SLEEPING / T_ZOMBIE / T_STOPPED`。
- 内核栈 32 KiB：顶部是 `intr_frame`，其下 56 字节是 `cpu_ctx`（`area[6] = glue_first`）。
- `task_alloc_slot / task_free_slot` 走 `task_table_lock`；`task_iter()` 是**有意无锁**的
  线性扫描，返回表内裸指针，由调用方在锁外使用。
- **无主僵尸由分配器兜底回收（P0-2）**。父链接 `(0,0)` 是内核专属的"无人领养"标记
  （generation 从 1 起步，带活父的僵尸永不落上它；写它的只有 exit 时无活 init 可领养、
  `task_free_slot` 清扫时无 init 存在），三层语义必须一起理解：

  ① **init 活着时是唯一收尸者**——exit 重挂写 `(1, init 当前世代)`，`wait4(-1)` 按
  incarnation 匹配必然查得到；健康 init 面前不会出现 `(0,0)` 孤儿。
  ② **表未满时无主僵尸仍占槽位**——这是惰性回收的代价：回收只在 `task_alloc_slot`
  撞满表时逐个触发（回收一具、本调用即成），没有主动扫描。只要系统最终产生 fork
  压力就会清理，fork 永远不会因此永久失败；init 死后的系统也不挂死，`ps` 里这些
  孤儿的父 pid 就是 0（`ppid` 字段即 `parent_pid`，读到 0 = 无领养者，不是数据损坏）。
  ③ **锁窗口是两阶段**：`(0,0)` 检测与 `T_EMBRYO` 占位在 `task_table_lock` 内完成，
  地址空间销毁、释放内核栈、`task_free_slot`（自带同一把锁的外部函数）**全在放锁
  之后**——占位挡住并发双回收，放锁避免自锁，`for(;;)` 重试兜住最后的领取。
  占位勒死（T_EMBRYO）若在回收动作中段崩溃，槽位随之泄漏——但那时已 panic 停机，
  内核把"抢救到一半 died"的生命线交给 panic 本身兜底，不再设计恢复路径。

### 4.2 fork / execve / exit

- `sys_fork`：分配槽 + 内核栈 + pid → 新 PML4 → `dup_user_aspace` 深拷贝 → 克隆 fd 表
  （逐个 `refcnt++`）→ 复制 brk 与 `ustack_lo` → 伪造子进程 `intr_frame`（rax=0、开 IF）→
  在 frame 下方构造 `cpu_ctx` 并以 `glue_first` 为入口 → 入队。
- `sys_execve`：`copy_user_str` 取路径（128 字节上限，argv 最多 16×64 字节）→ 读整个文件
  → 新 PML4 + 映射 16 KiB 栈 → **先 `vmm_switch_to(new_pml4)` 再 `elf_load`**（装载器用
  `memcpy` 直写用户地址）→ 失败切回旧 PML4 并回滚 → 成功则**先**赋 `current->pml4`
  **再**销毁旧地址空间 → 关 `O_CLOEXEC` fd、清 `signal_pending` → 按 SysV ABI 摆
  `argc/argv/envp/auxv`。
- `sys_exit`：关所有 fd → 撤控制台等待者 → **`fb_disown(current->pid)`（画屏归属的唯一
  释放点，所有死法都经过这里）** → 孤儿重挂到 pid 1（垂死的 init 不能领养自己，僵尸也
  不领养：两者落 `(0,0)` 标记，由 `task_alloc_slot` 满表时回收，见 §4.1）→ 给父发
  `SIGCHLD` → `T_ZOMBIE` → 出队 → 唤醒睡眠的父 → `schedule()`。
- `kernel_spawn`：第一个用户进程，类似 execve 但简化，结尾**切回 master PML4**
  （它跑在内核上下文里）。
- `kernel/elf.c`：只接受静态 `ET_EXEC`；`e_entry` 必须落在 `[USER_BASE, USER_MAX_VMA)`；
  返回 `brk_end`。不支持动态链接 / 共享库 / 需求分页。
- 用户内存访问 helpers：`user_ptr_ok`（逐页确认已映射）、`user_ptr_writable`
  （内核要写用户页时必须逐页确认可写，否则 CR0.WP 下 #PF 且无 fixup 表）、
  `copy_from_user` / `copy_to_user` / `copy_user_str`（逐字节校验）。

### 4.3 调度（`kernel/sched.c`）

- 每 CPU 一个运行队列（`cpu_info.runq_head` 哨兵），**push-front 单链表**：
  **实为 LIFO，不是环形**（结构体没有尾指针），只有 1 个时间量（`quantum = 6` tick = 60 ms），
  无优先级、无公平性保证。
- 一把全局 `runq_lock`（总是 irqsave）串行化入队 / 出队 / 选队；idle 任务**永不在队列上**，
  只作 `pick_next()` 返回 NULL 时的兜底。
- `schedule()`：选 next → 用 `runqueue_add_local()` 重排 current（**只回本核队列**：
  放到远端队列会被对方在切换完成前抢走，造成双跑）→ `switch_to`。
- `runqueue_add(t)` 走 `preferred_cpu()`：自己空闲 → 空闲 AP（先于 BSP，保 BSP 处理设备
  中断）→ BSP → 最短队列。这是**负载均衡**，只在唤醒 / 新建时决定落位，**无任务迁移**。
- `sched_tick()`：递减 quantum + 全局唤醒 sleepers；`sched_maybe_preempt(f)` 在返回
  用户态前查 `need_resched`。唤醒远端核时发 `IPI_VECTOR_RESCHEDULE`。
- `switch_to(next)`：**只有 `prev->pml4 != next->pml4` 才切 CR3**（槽 0/511 共享，
  多数切换不用动 CR3），设 TSS.rsp0、更新 per-cpu `_current`、`swtch()`。
  idle 任务的 pml4 被显式设为 `master_pml4_phys()`，否则第一次切到 idle 会因 CR3=0 复位。
- 上下文切换：`swtch` 只保存 7 个 u64（`r15 r14 r13 r12 rbp rbx rip`）；`cpu_ctx` 只有一个
  `sp` 字段，真正的寄存器区在任务内核栈上；`glue_first` 是新任务第一次被调度的入口，
  `swtch` 的 `ret` 落在 `intr_frame` 上，`POPALL; add rsp,16; iretq` 直接进用户态。

### 4.4 信号（`kernel/signal.c`）

- 32 个信号槽；`SIGKILL 9` / `SIGSTOP 19` 不可屏蔽、不可忽略，`SIGCONT 18` 额外唤醒
  `T_STOPPED`。默认终止集只有 6 个：`SIGINT SIGTERM SIGQUIT SIGILL SIGABRT SIGSEGV`，
  其余默认忽略。
- per-task 状态：`sig_handlers[32]`（`SIG_DFL`=0、`SIG_IGN`=`(void*)1`、其余为 handler 地址）、
  `sig_masks[32]`、`sig_blocked`、`signal_pending`、`sig_in_handler`、`saved_tf`、
  `sig_saved_blocked`、`sig_fault_rip`。
- `send_signal(t, sig)`：入口守卫（范围校验 + `T_UNUSED` 直接返回，槽位可能已被
  waitpid 释放 / 复用）→ KILL/STOP 硬置位并唤醒 → 其余置位，目标在睡就转 `T_RUNNABLE` 入队。
- `do_signal_check(t)`：三个调用点都在 `iretq` **之前**且带 ring-3 守卫（syscall 返回路径、
  IRQ 返回路径、异常路径）。一次只投一帧；handler 运行期间的第二信号保持 pending。
- `deliver_handler`：把被打断的帧存进 `saved_tf`，在用户栈上伪造 `call` 帧（返回地址指向
  `USER_SIGRETURN_VA` 的 restorer 页，`rsp % 16 == 8`），改 `f->rip/rdi` 跳进 handler。
  restorer 页内容是 9 字节 `mov eax,20; int 0x80; ud2`，由 `signal_map_restorer()` 每个
  地址空间映射一次（**唯一 exec=true 的用户页**），用户二进制不需要自带 restorer。
- `sys_sigreturn`：仅 `sig_in_handler` 时有效，把 `saved_tf` 拷回 `tf`（其 `rax` 作为
  系统调用返回值）、恢复 `sig_blocked`。
- `sys_sigaction`：KILL/STOP 不可捕获 / 忽略；handler 地址必须是哨兵或
  `user_ptr_ok` 校验过的用户页。`sys_sigprocmask`：BLOCK/UNBLOCK/SETMASK。

### 4.5 凭据与能力（`include/sys/cred.h`、`kernel/protect.c`）

| 层级 | 含义 |
|---|---|
| `CRED_USER = 0` | 普通进程，uid 1000 |
| `CRED_ROOT = 1` | 管理员，uid 0，仍不能破坏 kxld 保护与其系统文件 |
| `CRED_KXLD = 2` | 最高层，任何人都进不去；kxld 任务不走 syscall 路径 |

- 能力位三个：`CAP_KILL (1<<0)`、`CAP_SYS_ADMIN (1<<1)`、`CAP_FB (1<<2)`；
  root 与 kxld 按构造拥有全部能力。
- `cred_setuid()`：**单向降权**——root → 任意 uid 且低于 0 时 tier 掉到 `CRED_USER`
  并清空能力；普通用户改别的 uid 回 `LNXRM_EACCES`（不能爬升、不能横移）。
- `kxld_protect_kill()`：pid 1（init）任何人均不可杀，KXLD 任务不可杀。
- `protect.h` 里的规则都是**纯函数**（不读 `current`），自测可以用合成凭据在还没有任何
  用户进程时驱动这些门。

## 5. 系统调用与 ABI

### 5.1 调用约定与分发

- 向量 `int 0x80`，编号在 `rax`，参数在 `rdi / rsi / r10`，返回值在 `rax`；
  IDT 128 号门 DPL=3（`arch/cpu.c`），ring-3 可直接调用。
- 入口桩 `isr128`（`arch/entry64.S`）压出合成 err 槽 + 向量号 + PUSHALL，
  `mov rdi, rsp` 后 `call syscall_entry`，回来 POPALL + `iretq`。
- `syscall_entry`：存 `current->tf` → `syscall_do`（**整张分发表就是一个大 `switch`**）→
  写回 `rax` → 置 IF → 返回用户态前 `do_signal_check()`（信号可能 stop/kill 进程，
  也可能改写这个帧把 `rip` 指向 handler）。
- **只追加不重排**：编号唯一定义源是 `include/abi/lnxrm_syscalls.def`（X-macro），
  文件头写明三条规则——编号紧凑连续、新调用追加到末尾并把 `LNXRM_SYSCALL_LAST`
  加一、**永不重排已有编号**（用户态二进制终身按同一槽调用）。`lnxrm_abi.h` 用
  `1 / (nr <= LNXRM_SYSCALL_LAST)` 做逐编号编译期校验：加了超界编号却忘了改宏，编译直接失败。
- 未知编号（≥33）落进 `default` 分支，打印一行 `[sys] unknown syscall …` 后返回 `-38`。

### 5.2 33 项总表（编号 0–32，连续无空洞）

| # | 名字 | 用途 | 实现文件 |
|---|---|---|---|
| 0 | `read` | 从 fd 读，含 `/dev/console` 阻塞读 | `kernel/fs/vfs.c` |
| 1 | `write` | 写 fd，控制台走共享打印锁 | `kernel/fs/vfs.c` |
| 2 | `open` | 打开 / 创建（`O_RDONLY/O_WRONLY/O_RDWR/O_CREAT/O_TRUNC/O_APPEND/O_CLOEXEC`）；访问位错配在读写端拒绝（C41） | `kernel/fs/vfs.c` |
| 3 | `close` | 关闭 fd | `kernel/fs/vfs.c` |
| 4 | `lseek` | `SEEK_SET/CUR/END` | `kernel/fs/vfs.c` |
| 5 | `brk` | 调整用户堆，带配额算术门 | `kernel/syscall.c` |
| 6 | `getdent` | 目录枚举（cookie 迭代） | `kernel/fs/vfs.c` |
| 7 | `dup2` | 复制 fd | `kernel/fs/vfs.c` |
| 8 | `nanosleep` | 睡眠；被信号打断回 EINTR 并回填余量 | `kernel/sched.c` |
| 9 | `getpid` | 进程号 | `kernel/syscall.c` |
| 10 | `fork` | 复制进程（深拷贝地址空间） | `kernel/task.c` |
| 11 | `execve` | 装载静态 ELF64 | `kernel/task.c` |
| 12 | `exit` | 退出（释放画屏归属） | `kernel/task.c` |
| 13 | `wait4` | 等子进程，支持 WNOHANG | `kernel/task.c` |
| 14 | `kill` | 发信号，kxld / 同 uid / `CAP_KILL` 三道闸 | `kernel/syscall.c` |
| 15 | `uname` | 系统标识 | `kernel/syscall.c` |
| 16 | `sigaction` | 装信号处理器 | `kernel/signal.c` |
| 17 | `sigprocmask` | BLOCK / UNBLOCK / SETMASK | `kernel/signal.c` |
| 18 | `getppid` | 父进程号（父链接的 pid 半边，无父为 0） | `kernel/syscall.c` |
| 19 | `ps` | 内核填任务表，带 `CAP_SYS_ADMIN` | `kernel/task.c` |
| 20 | `sigreturn` | 从 handler 返回，恢复被中断现场 | `kernel/signal.c` |
| 21 | `diskinfo` | 枚举块设备，带 `CAP_SYS_ADMIN` | `kernel/syscall.c` |
| 22 | `mkdir` | 建目录 | `kernel/fs/fat32/fat32_meta.c` |
| 23 | `fb_info` | 读帧缓冲几何（**两道门都不过**） | `kernel/syscall.c` |
| 24 | `fb_clear` | 清屏 | `kernel/framebuffer.c` |
| 25 | `fb_fill` | 矩形填充 | `kernel/framebuffer.c` |
| 26 | `fb_char` | 画字符 | `kernel/framebuffer.c` |
| 27 | `fb_puts` | 画字符串 | `kernel/framebuffer.c` |
| 28 | `unlink` | 删文件 | `kernel/fs/fat32/fat32_meta.c` |
| 29 | `rmdir` | 删空目录 | `kernel/fs/fat32/fat32_meta.c` |
| 30 | `rename` | 改名 / 移动 | `kernel/fs/fat32/fat32_meta.c` |
| 31 | `setuid` | 单向降权 | `kernel/protect.c` |
| 32 | `fb_flip` | 把离屏后缓冲推上前台 | `kernel/framebuffer.c` |
| 33 | `sync` | 把缓存里所有脏扇区交给各自设备（C27） | `kernel/fs/vfs.c` |
| 34 | `kbd_get` | 非阻塞取一个键（0..255），空则 `EAGAIN`；`CAP_SYS_ADMIN` | `kernel/isr.c` |
| 35 | `kbd_inject` | 代打键序列进输入环（唤醒 waiter），≤256 字节；`CAP_SYS_ADMIN` | `kernel/syscall.c` |
| 36 | `kbd_info` | 键盘/输入环计数快照（32 字节 `lnxrm_kbd_info`，两道门都不过） | `kernel/isr.c` |
| 37 | `mouse_get` | 非阻塞弹一个 PS/2 指针事件（16 字节 `lnxrm_mouse_state`：`dx/dy/buttons/wheel`，空则 `EAGAIN`）；`CAP_SYS_ADMIN` | `kernel/syscall.c` |
| 38 | `mouse_info` | aux 事件环计数快照（32 字节 `lnxrm_mouse_info`，两道门都不过） | `kernel/drivers/ps2mouse.c` |

图形相关共 6 个：`fb_info`/`fb_clear`/`fb_fill`/`fb_char`/`fb_puts`（23–27）与 `fb_flip`(32)。
指针输入共 2 个消费口（`mouse_get`）+ 1 个只读口（`mouse_info`），结构同 kbd 三件套。

### 5.3 两道门与其它检查点

- `cap_gate(cap, capname, what)`（`kernel/syscall.c`）：`ps`/`diskinfo`/`kbd_get`/`kbd_inject`/
  `mouse_get` 过 `CAP_SYS_ADMIN`（前四个动的是共享资源），`fb_clear/fill/char/puts/flip`
  过 `CAP_FB`；拒绝统一回 `LNXRM_EACCES = -13` 并打一行
  `[cap] denied <call>: pid … lacks CAP_*`。
- `fb_owner_gate(what)`：紧接在能力门**之后**，5 个绘制 / 发布 case 各多一道归属门——
  `fb_claim(pid)` 抢不到就回 `-13` 并打 `[fb] denied <what>: pid A holds the surface, pid B draws`。
- **顺序是硬的：先能力、后归属**。两道门都回 `-13`，靠两行不同的回执区分，不为此新增
  错误码；倒过来普通用户会听到"你不是 owner"，而它连碰都不该碰。
- `fb_info` 两道门都不过：只读几何，且不学尺寸就没资格选画哪儿。
- `kill`：`kxld_protect_kill()` → 同 uid 或 `CAP_KILL` → `pid == -1` 广播另要 `CAP_KILL`。
- 归属状态 `fb_owner` 存在 `framebuffer.c` 而不是 `task.c`（"屏归谁"是驱动的事实）；
  释放只在 `sys_exit` 一处（见《进程》一节）。
- 所有走字符串的调用经 `copy_user_str` 校验；走指针的由调用方自己校验——
  **没有统一的前置检查**，加新调用必须自己记得验。

### 5.4 共享 ABI（`include/abi/lnxrm_abi.h`）

内核与用户态共同包含，是所有跨态结构体的唯一权威：

| 项 | 规格 |
|---|---|
| `enum lnxrm_errno` | 21 个 `LNXRM_E*`，含 `EINTR -4`、`EAGAIN -11`、`EACCES -13`、`ENAMETOOLONG -36`、`ENOSYS -38` |
| `enum lnxrm_syscall_number` | 由 `.def` 生成的 33 个 `SYS_*` + `LNXRM_SYSCALL_FIRST/LAST` + 逐编号编译期校验 |
| 信号号 | `LNXRM_SIG*` 编号、无前缀别名、`LNXRM_NR_SIGNALS 32`、`SIG_DFL/SIG_IGN`、`LNXRM_SIG_BLOCK/UNBLOCK/SETMASK` |
| `struct lnxrm_sigaction` | 24 字节 |
| `struct lnxrm_timespec` | 16 字节（`nanosleep` 剩余时间） |
| `struct lnxrm_utsname` | 320 字节 |
| `struct lnxrm_ps_entry` | 48 字节（含 `kind`/`uid`/`mem_kib`/`peak_kib`） |
| `struct lnxrm_dirent` | 72 字节，含填充的 `d_ino` |
| `lnxrm_fb_info / fb_rect / fb_char / fb_str` | 16 / 20 / 20 / 24 字节，带 `_Static_assert` 锁布局 |

⚠️ **ABI 结构体一改，引用它的用户态程序必须全量重编**，否则按旧步长读会拿到错位字段。

用户态入口：`int 0x80` 的内联汇编包装在 `init/ulib.h`（第三个参数手动搬进 `r10`，
clobber 列表必须含 `r10`），33 个调用全覆盖；`init/crt0.S` 从栈取 argc/argv 调 `main`。

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
| `struct vnode` | `{type, size, fs_data, ops, mnt_data}` | **每次 open 在栈上创建并拷贝**，不是持久 inode——两个 fd 的 `pos` 与状态各自独立，无 link count |
| `struct file` | `{ops, pos, priv, is_dir, flags, refcnt}` | `kmalloc` 分配，引用计数 |
| `struct dir_iter` | `{node, ops, cookie, mnt_data, err}` | `V_DIR` 的 `file->priv`；`err` 记住这次扫描已因 I/O 错误终止，下次 `getdent` 继续报错而不是"到头了" |
| `struct fs_ops` | 12 个钩子：lookup/getdent/read/write/**size/close_node**/create/mkdir/unlink/rmdir/rename/freespace | 每文件系统一份；`size` 给 O_APPEND 取**实时**大小（FAT 重读槽位 dirent，initramfs 返回静态 size）；`close_node` 由 fs 决定节点释放方式（FAT `kfree` resolve，initramfs 是挂载数组的一片，空操作） |
| `struct file_ops` | 4 个：read/write/getdent/close（**没有 lseek**） | VFS 层薄封装；普通文件 close 即 `reg_close`：先 `close_node` 归还 fs 节点、再释放 vnode 拷贝（C40，refcount 门控恰跑一次） |

- fd 表：每任务 `struct file *fds[NR_FDS]`；挂载表 `MAX_MOUNTS 4`。
- 路径解析 `vfs_lookup`：扫挂载表匹配前缀 → `ops->lookup` → 失败试 `dev_lookup` →
  按 `O_CREAT` 试 `create` → 按 `O_TRUNC` 试截断；三处失败各有各的出口（见下节）。
  `abs_path()` 超长直接 `LNXRM_ENAMETOOLONG`，不写半个路径。
- 设备节点只有 `/dev` 与 `/dev/console`；`/dev/console` 的读是阻塞等待输入，
  **信号 EINTR 优先于睡眠**，arm waiter 后二次复查竞态窗口，等待者是链表
  （`con_next`），多个读者互不覆盖。
- 文件访问语义（C41/C44，2026-10-25）：`reg_read/reg_write` 按 fd 访问位放行
  （`O_RDONLY` 拒写、`O_WRONLY` 拒读，皆 `EBADF`）；`O_APPEND` 每次写都落到 fs
  实时 size（`fs_ops.size`），打开时刻的快照不做数；`getdent` 缓冲小于单条
  dirent（72 字节，含 0）即 `EINVAL`——小缓冲不再回 0 让调用方忙等。

### 6.3 错误契约（自下而上，每层只翻译、不吞）

| 层 | 回答什么 |
|---|---|
| `blk_io_read` / `blk_io_write` | 驱动的非 0 原样上抛；注入的故障回 `LNXRM_EIO`。**全内核只有这里调 `dev->read/write`** |
| `fat_read_sector` / `fat_write_sector` | **只有 0 或 `LNXRM_EIO`**，不透传驱动私有码 |
| `fat_scan_dir` / `fat_resolve_path` / `fat_dir_iter` | 三态 `1 / 0 / 负`：「读不到」与「没找到」「扫到末尾」不再共用一个值 |
| 语义层 `fat_*_impl` | 负数一路向上传 |
| VFS | `EIO` 不再被改写成 `ENOENT`/`EACCES`/`0` |

出错时的对外表现：限流 16 条的 `[fat32] I/O error <op> LBA <n>`，操作回 `LNXRM_EIO = -5`。

### 6.4 路径不变式（同一扇区只走一条路）

| 区域 | 读 | 写 |
|---|---|---|
| 数据 / 目录（≥ `data_start_lba`） | `fat_read_sector`/`fat_read_cluster` → 块缓存 | `fat_write_sector` → 块缓存（写回） |
| FAT 区 `[reserved, reserved + num_fats*fatsz)` | `m->fat`（挂载时整表读进内存） | `fat_set_entry` 直写 / 入日志事务 |
| 挂载期（BPB、FAT 表、MBR） | `blk_io_*` 直读 | — |

**FAT 区永远不进块缓存**：FAT 更新必须先落盘，才能被指向它的目录项暂存；若改走缓存，
两者都成暂存态，按槽号回写的 `blk_cache_flush` 就会写出"目录项先于 FAT 落盘"的窗口。
自测 `ktest/t_cache.c` 逐扇区断言 FAT 区一个都不在缓存里。

### 6.5 块缓存（`kernel/fs/blk_cache.c`）

- 64 项 × 512 字节 = 32 KB，键 `(dev, lba)`，全局一块，所有块设备共享。
- **时间戳 LRU**：条目上只有一个 `u64 stamp`，取自单调 `lru_seq`；命中 / 填充 / 暂存都
  `++lru_seq`，受害者 = 空槽优先，否则 `stamp` 最小的常驻项。
- `blk_io_read/write` 是唯一咽喉：先查故障注入、再计数、再问驱动。
  `blk_inject_io(dev, lba, ops)` 只给自测用，武装的 LBA 上一切传输回 `LNXRM_EIO`；
  注入点在**缓存入口也有**，否则"扇区正好在缓存里"会把故障挡掉。
- 读失败时**作废 victim 槽**（`valid/dirty = false`）：驱动可能已经写了一半才拒绝，
  留着它下一次命中就是把半覆盖扇区当好数据。
- 统计 `hits/misses/evictions/writebacks/dev_reads/dev_writes`（原子递增，因为既在锁外
  也在 `cache_writeback` 持锁时被调）；`blk_cache_lookup` 返回槽号且**故意不动 stamp**；
  `blk_cache_purge` 写回 + 丢弃（`dev=NULL` 全表），写回失败保留脏槽。
- **持久化同步（C27，2026-10-25）**：三道门都进 `blk_cache_sync_all()`——扫全部 64 槽
  把脏扇区交给各自设备，写回失败保持脏（是唯一拷贝）、最后错误上抛。①BSP tick 每秒
  `blk_cache_tick`：只在 `current != NULL` 且非 panic 时出力（boot 自测全程无任务、且
  LAPIC 计时器晚于 ktest 阶段才启动，ktest 的计数断言因此保持确定性）；②普通文件的
  最后一个 fd `close` 即同步（`sys_close`；目录与字符设备无数据不触发）；③`panic()`
  在 `fb_flip` 后走 `blk_cache_sync_all_try()`——配 `spin_trylock_irqsave`（sched.c），
  panic 持有的锁**绝不等待**，仍能到 `hlt`。④`SYS_sync(33)` 同步门，userland
  `ksync()` + systest `sync.ret=0`。写回语义与 journal 的先后约束不冲突：sync 只把
  当前缓存里的脏扇区**提前**送到设备，commit 的「先刷日志扇区再盖提交戳」次序不会被
  重排（两侧都串行于 `cache_lock`/`ide_lock`）。ktest `test_sync_hands_dirty_sectors_
  to_the_device` 用驱动层直读证明「sync 前设备上是旧字节，sync 后与暂存一致，二次
  sync 零设备写」。

### 6.6 FAT32 与 on-disk redo journal

**挂载**（`fat_mount_impl`）：读扇区 0 → 校验 BPB（签名 55AA @[510]、文件系统串
`"FAT32"` @[82]、`sectors_per_cluster` 为 2 的幂；`bytes_per_sector == CACHE_SECTOR_SIZE`
即 512 —— 全部扇区缓冲与块缓存都按 512 字节搬移，bps ≠ 512 的盘在挂载层直接拒绝
（C29/C38）；spc/fatsz/reserved 非零、`num_fats ∈ {1,2}`、`root_cluster >= 2`）→ 算
`data_start_lba` 与 `max_cluster` → **把整个 FAT `kmalloc` 到内存**。

**目录槽模型**（C23）：可用槽 = 从未用过的尾槽（0x00，终止符本身）与已删除记录
（0xE5）；其余首字节都是活记录——包括 0x05：规范的 8.3 别名里 0x05 是 0xE5 的转义，
而 LFN 第五片的首字节恰是序号 5。写端两处认领（create/mkdir 的 `parent_entry_add()`
取首个可用槽；rename 的 `dir_claim_run()` 取连续可用槽段）从此共享同一判定。槽不够时
先 `dir_chain_extend()` 扩展目录链（分配簇→尾部链接→整簇清零，失败即自回滚），再在新
簇落位——目录永不再因满而 `ENOSPC`（只有卷真满才回）。终止符必须活过每次落位：
目录链**最后一个槽**从不可认领，认领者宁可先扩链。**扫描端**（本批改回规范字义）：
止于 0x00——规范的目录结束符，不再跑满整条链；藏在尾槽之后的残留字节再也浮不出伪
记录。坏簇/自环照样被步数预算或自环即断拦下并告警。

**目录游标**（C30）：getdent 的 cookie 是 `(簇<<32)|字节偏移` 的物理恢复点
（`DIRIT_START=0`、`DIRIT_END=(u64)-1` 出在 `include/fat32_priv.h`），引擎按扇区
步进（32 字节记录不跨 512 扇区），整簇级跳过零设备读。一条 readdir 从头到尾的扇区
查找计数 ≈ 链内扇区数 + 条目数（实测 `dir 3 clusters, one walk = 39 lookups
(bound 46)`）——不再是每次 getdent 从首簇重扫的 O(n²)；坏/越界 cookie 一律回
"已结束"而不是编造条目。

**LFN 读验证**（C17/C31）：一个长名 run 由带 0x40 位、序号最高的记录开启，之后各记录必须
带同一别名 checksum 且序号严格递减；缺失/乱序/异 checksum 即弃用整个 run，回落 8.3
别名。拼接缓冲 260 字符 + 终止符（261 字节，C31），`lfn_reset()` 整段清零，残留在
缓冲里的栈垃圾进不了名字。

**写路径**：元数据（目录项 / FAT / FSInfo）走 redo 日志事务，文件数据字节走写回缓存。
写需求先按 `off + n` 做 u64 回卷感知合并（C16），`alloc_chain` 再以
`fat_chain_cap_bytes()`（`MIN(FAT_CHAIN_MAX_STEPS, data_clusters) × cluster_size`）为
上界：越过本卷最大可表达链的写**先拒 `ENOSPC`、零分配**——u32 截断回卷不会发生，
也不会把整个空闲池预订给一条写不到达的链。

`kernel/fs/fat32/fat32_journal.c` 的契约：

- 落在 mkfs 的 reserved 空闲扇区 `[8, reserved_sectors)`（Makefile 给 `mkfs.vfat` 加
  `-R 1024`）。扇区 8 是 superblock：magic `JRN1`、version、state、nrec、head、CRC32；
  之后每条记录占 2 扇区（记录头 + 512 字节后像）。`head` 是**相对 `j_start` 的偏移**，
  不是绝对 LBA——写入端与 replay 端必须一致。
- 每个变更 op 一个事务，由 `fat32.c` 的 locked wrapper 包住：
  `fat_journal_begin → *_impl → fat_journal_commit`，**失败路径也 commit**
  （impl 可能把簇分配了或改了半截，留在 RAM 而磁盘看不见正是交叉链接的来源）。
- commit 顺序就是协议：① flush 文件数据（ordered：数据字节不入日志）② 写记录
  ③ 写 superblock 置 **COMMIT —— 这是唯一提交点** ④ `jrn_write_home` 逐条 apply
  ⑤ flush ⑥ superblock 置 CLEAN。**④ 失败必须保留 COMMIT**：清掉它等于把
  "可恢复的半途"变成永久丢失。
- 事务期间后像只在 RAM（按 lba 去重覆盖），读己经写由 `fat_read_sector →
  fat_journal_read` 提供；缓冲满时**中途 checkpoint 再续**而不是静默丢弃。
- `fat_journal_mount` 在挂载加载 FAT 之后跑：superblock 过期 / 损坏则重建，
  state == COMMIT 则幂等 redo 全部记录（并把重放的 FAT 扇区同步回 `m->fat`），
  完成后置 CLEAN。失败语义：只可能泄漏簇、不交叉链接。

**簇链安全**：`FAT_CHAIN_MAX_STEPS` 限制步数，`fat_chain_limit = MIN(上限, 数据簇数)`；
非法首簇直接拒绝并告警；坏 FAT 项只报告不跟随，长环由步数预算拦下，
**单簇自环**（cell 指回自己的簇）在 `fat_chain_advance` 里当即断链 + `chain loop`
告警——数据读取会合法地"回放同一簇"直到字节量耗尽，永远触不到步数预算，靠这一检测
第一时间暴露。挂载前还有两项容量守门（C32/C33）：卷 FAT 表项数 > 2^28 直接
`[fat32] reject …`（设备恰发一个扇区），`max_cluster` 是独占上限（最后的 FAT 格可达）。

### 6.7 挂载顺序与回退

1. `vfs_try_mount_disk()` 按 `blk_list[]` 依次试每块盘：先"整盘 FAT32"，失败再试
   "MBR 分区表"（`mbr.c` 把分区包成 `struct part_dev`，读写转发 `blk_io_*`）。
2. `mbr.c`（C36）解析期就该把坏输入掐死：签名 55AA 必须；每个表项验
   `lba + count <= 盘容量`，越界（含 count==0）条目报 `[mbr] … outside the disk`
   后丢弃而不是往下传；EBR 链真正遍历——表 0 是相对 `ebr+rel` 的逻辑分区，
   表 1 是相对前移的下一段链接，链上限 32 段，`include/disk.h` 的
   `MBR_MAX_PARTITIONS` = 4 主 + 32 逻辑。
3. 根没就绪或缺 `/bin/init` → `vfs_mount_initramfs()` 用镜像里的 newc cpio 当根；
   没有 `INITRAMFS=` 的构建带空 `.initramfs` 段，行为不变。

## 7. 驱动

### 7.1 串口与控制台

- `kernel/drivers/uart.c`：16550，115200 8N1 + FIFO，`lnxrm_uart_init/putc/trygetc/irq_enable`；
  COM1 → IRQ4 → 向量 36（`[serial] COM1 ready`）。
- `kernel/print.c` 的 `console_putc` 扇出四路：帧缓冲控制台 → VGA 文本 80×25（硬件光标）
  → 串口 → 4096 字节环形缓冲（`fb_console_init` 用它回放已滚出 VGA 的历史，串口上的
  老消息也能出现在帧缓冲控制台上且不画两次）。
- `kvprintf` 持 `console_out_lock`（irqsave），与用户态 `write(1,…)` 共用这把锁
  （64 字节分块拿锁，限制关中断时长）。支持 `%s %d %u %x %X %p %c %%`、宽度、`0` 填充、
  `l/ll/z` 长度修饰。
- `panic()` 打印 `[PANIC] …` 后 **`fb_flip()` 一次再 `cli; hlt`**——双缓冲下文本只在
  后缓冲里，而 panic 之后再也不会有 tick 去呈现它。
- ANSI 三状态机 `ansi_feed` 也在 `print.c`（`ESC[2J` 清屏、`ESC[nH` 定位、
  `ESC[...m` SGR 0/1/30-37/40-47），VGA 与帧缓冲两个后端各自消费；`ESC[3J` 是空操作。

### 7.2 键盘与鼠标

- PS/2 控制器初始化，IOAPIC IRQ1 → 向量 33（`[kbd] PS/2 ready`）。
- `kbd_process_sc`：scancode set 1 → ASCII，两张 `kmap[128]`/`kmap_shift[128]` 表，
  Shift / CapsLock / E0 前缀；没有 ASCII 映射的键直接丢弃。
- 输入只有一条路：ASCII 进 256 字节 `inbuf`（容量 255，满即丢弃并 `dropped++`），
  `console_read` 消费；等待者是 `con_next` 链表（`console_waiter_arm/disarm`），
  多个读者不互相覆盖。
- **输入/输出成对的三支调用（2026-10-25）**：`kbd_get(34)` 非阻塞弹一个键，空回
  `EAGAIN`（`input_pop` 按无符号返回 0..255，0xFF 不再和空环哨兵 `-1` 混淆）；
  `kbd_inject(35)` 把用户态字节喂给真实 `input_push()`（waiter 照常唤醒，返回实际
  接受数，满环丢键如实计数）；`kbd_info(36)` 给 `lnxrm_kbd_info{pending, capacity,
  pushed_total, dropped, ps2_scans}`——`ps2_scans` 是 IRQ1 自启动以来见过的全部
  scancode 数（make+break），键盘"活着"的判据。get/inject 都要和 shell 抢共享输入流，
  门同 `ps`（`CAP_SYS_ADMIN`），info 只读全放行。取走即消费的语义归 `kernel/isr.c`
  （计数器/辅助函数也都在这里），门与用户副本在 `kernel/syscall.c`。
  ktest 侧 `ktest/t_kbd.c` 用 `syscall_probe` 走真分发面：门后指针校验、FIFO 次序、
  255 容量 + 丢键记账 + 计数器单调、`0xFF` 字节身份（sys 层 -13 由 systest 降权
  子进程补证）。
- `kernel/drivers/ps2mouse.c`（2026-10-26）：同一颗 i8042 的 aux 口走 IRQ12 →
  向量 44（`[mouse] PS/2 aux ready`，无鼠标的机器回 `[mouse] no aux device` 而不是
  挂死——所有回复都带预算轮询）。`ps2_mouse_init()` 排在 `ps2_kbd_init()` **之前**：
  探测全程没有任何已注册中断，配置命令字节（走键盘通道）与 aux 回包都读不到 race；
  探测序列 `0xA8（使能 aux 口）→ 配置字节 bit1 置 1 / bit5 清 0 → 0xFF 复位（期望
  0xFA/0xAA/0x00）→ 0xF6 默认值 → **滚轮探测**（rate 三元组 `200/200/10`、`200/200/80`、
  `200/100/80` 各跟一次 `0xF2`：回 id `0x03`/`0x04` 即 4 字节 wheel 模式，都无应答保持
  3 字节；仍在无中断窗口内，回复全带预算）→ 0xF4 流式上报 → 最后才挂 IRQ12 向量 44`。
  3 字节标准包 `{LB,MB,RB,1,Xs,Ys,Xov,Yov}, dx, dy`：9 位符号在 flags 里，溢出包强制
  delta=0（不给用户垃圾值），flags 缺 `0x08` 一致位即失步丢弃并计 `resyncs`；PS/2 线上
  y 正=上，解码出口取负统一成屏幕向（dy 正=下，wheel 正=下滚）；4 字节包
  第 4 字节是 4 位符号扩展滚动 delta（-8..7），QEMU 探测器认 `200/200/80` 与
  `200/100/80` 两对三元组。IRQ12 handler
  按状态寄存器的 AUX 位过滤（键盘字节留给 IRQ1 handler），单中断内至多倒 32 字节。
- 鼠标整面 = 64 槽事件环（容量 63，满即丢弃并 `dropped`，事件多带 `wheel`，
  3 字节模式恒 0）+ `ms_stats` 计数器 `{pending, capacity, packets_total, moved_total,
  resyncs, dropped, inited}`；语义对 kbd 环逐项镜像，取走即消费。**内核不带屏上
  指针**——屏上有没有光标、怎么画，由用户态读事件流自行决定。`mouse_get(37)` 门（`CAP_SYS_ADMIN`）在指针校验
  之前——`EFAULT` 对 NULL/坏指针**在空环时也先答**（refuse-then-pop-then-copy）；
  `mouse_info(38)` 只读全放行。内核侧由 `kernel/drivers/ps2mouse.c` 提供
  `mouse_pop/ms_feed_byte`，`ms_feed_byte` 就是 ktest 驱动合成包、锁住确定性的入口。
- `ktest/t_mouse.c`（LATE 两例，USER_BASE+0xE000 独立 VA 页）：`syscall_probe` 走真
  分发面——空环 `EAGAIN`、空环也先答 `EFAULT` 的坏指针、跨页未映射、9 位符号
  解包并统一成屏幕向（PS/2 线上 y 正=上，出口取负：dy 正=下，dx 正=右，wheel 正=下滚）、
  双溢出包 delta=0、失步字节记账、63 容量 + 第 64 个事件如实丢弃 +
  FIFO 取回空；两种 framing 都被显式驱动（3 字节段 + 4 字节滚轮段含第 4 字节符号
  扩展、溢出、sanity 位不漏进 `buttons`），收尾 `ms_set_packet_bytes()`
  恢复实时模式。

### 7.3 IDE 与 AHCI

| 驱动 | 要点 |
|---|---|
| `drivers/ide.c` | 硬编码主通道 `0x1F0`、主设备、PIO、单命令至多 256 扇区（`IDE_MAX_CHUNK`）。`ide_init` 选盘 → 4 次状态读当 400 ns → `s == 0x00 \|\| s == 0xFF` 判无设备 → 等 DRDY → 发 `0xEC` → `ide_wait_drq()`（**全文件唯一的 ERR 判定点**）→ 读 IDENTIFY → `ata_total_sectors()` 取容量，容量为 0 不注册。读写先做 LBA 上界检查（28 位命令），`ide_lock` 包住整个循环。轮询预算分两档：探测 `IDE_PROBE_ITERS`（无盘口几秒内放弃）、读写 `IDE_POLL_ITERS`（QEMU 在 vCPU 线程里同步落宿主文件，宿主回写压力下 BSY 可持数十毫秒，旧 10 万次预算会把正常命令饿成 EIO） |
| `drivers/ahci.c` | `ahci_init → pci_scan → ahciProbe`（class 过滤）→ 映射 ABAR → HBA 复位 → `ahciProbePort`（命令表 / FIS 区 / IDENTIFY）→ `blk_register`。命令下发 `buildH2dFis` + `issueCmd`：轮询 `P_CI` 清零后**先查 `IS_TFES` 再查 `PxTFD.ERR`**（完成 ≠ 成功），超时用 TSC 折算。单 PRD，缓冲必须物理连续的 512 字节 |
| `drivers/pci.c` | 传统 `0xCF8/0xCFC` 配置窗口，`pci_scan` 递归（带 visited 与深度上限），支持多功能设备 |

⚠️ **`make run` 的 QEMU 是 `-drive …,if=ide`，机器上没有 SATA 控制器**：`ahciProbe`
的 class 过滤对所有设备提前返回，AHCI 的映射 / 端口初始化 / IDENTIFY / 读写路径在
标准流程下零执行。同理 `mbr.c` 是挂载的第二步 fallback，整盘 FAT 总能先成功，
标准镜像下也走不到。

容量取值统一走 `ata_total_sectors()`（`include/sys/vfs.h` 的 `static inline`，IDE 与 AHCI
共用）：IDENTIFY words 100-103 非 0 即权威；words 60-61 为 `FFFFFFFFh` 哨兵时返回 0，
调用方据此**拒绝 `blk_register`**。

### 7.4 帧缓冲：几何、8bpp、控制台

`include/framebuffer.h` 的常量：

| 常量 | 值 |
|---|---|
| `FB_MAX_DIM` | 8192 |
| `FB_VMA` | `0xffffffff8d000000`（`PD_HI[104]`，4 KiB 页） |
| `FB_PD_BASE` / `FB_PD_SLOTS` | 104 / 8（16 MiB 窗口，实际只有 `[104,108)` 空闲） |
| `FB_DB_PD_BASE` / `FB_DB_SLOTS` | 97 / 7（后缓冲窗 `[97,104)` = 14 MiB） |
| `FB_DB_VMA` | `0xffffffff8c200000`（`PD_HI[97]`） |
| `FB_CELL_W` / `FB_CELL_H` | 8 / 16（点阵字体单元） |

分层（自底向上）：

| 层 | 函数 | 做什么 |
|---|---|---|
| 原始像素 | `fb_store/fb_store_px/fb_put_pixel` | 按 `fb_pxsz`（1/2/3/4）写 `fb_ptr`，只裁剪不标记（标记归调用者）；`fb_put_pixel` 是 `static` |
| 颜色 | `fb_pack` / `fb_rgb_px` | `0x00RRGGBB` 按通道位置打包；**8bpp 索引模式就近量化到 6×6×6 立方** |
| DAC | `fb_setup_palette` | 仅 8bpp：编程 VGA DAC 的 16 VGA 色 + 色立方 + 24 级灰阶 |
| 字体 | `font8x16[96][16]` | CP437 点阵，ASCII `0x20..0x7E` |
| 绘图原语 | `fb_clear / fb_fill_rect / fb_draw_char / fb_puts` | **全部裁剪到可见区域**，传屏幕外的矩形是零写入；写完即标记所写区域 `fb_mark_dirty(x,y,w,h)` |
| 控制台 | `fbc_*` / `fb_console_putc` / `fbc_scroll` | 帧缓冲上的 ANSI 终端，滚动 = 逐行 `memcpy` + 清最后一行 |
| 归属 | `fb_owner_get / fb_claim / fb_disown` | 只记状态，不参与绘制 |

`fb_init`：映射 LFB（`PG_PCD`，PAT 无 WC 槽时落 UC）、按 DISPI 回报的 pitch 定几何、
对引导加载器留空的通道布局做 15/16/≥24 bpp 兜底、建后缓冲、`fb_console_init` 回放
环形缓冲。VBE 模式数据非法则提前返回，`fbc_*` 不激活，输出落 VGA 文本后端。

### 7.5 双缓冲契约（默认开启）

**画的与看的不是同一块内存。** `fb_ptr`（所有绘制原语的落笔处）被 `fb_back_init()`
改指到离屏后缓冲，可见的 LFB 是 `fb_front`；只有 `fb_flip()` 会把**脏区域**逐行前拷。

- **后缓冲在单独的 PD 窗口 `[97,104)`**（`FB_DB_VMA`，14 MiB）。不能塞进 LFB 窗口
  （`[104,108)` 才空闲，slot 108 归 ACPI 弹跳页）；也不能整块分配（buddy 最大连续块
  4 MiB，1080p32 一幅 7.9 MiB），所以逐个 `pmm_alloc()` 4 KiB 帧再
  `vmm_map_kernel_page()` 拼成**虚拟连续**的面。
- **`pmm_reserve` 与别名过滤必须成对**：`kernel/main.c` 在 `pmm_init` 之前按**整窗**
  reserve `[97,104)` 的物理帧，`kernel/mm/vmm.c` 的别名环把这 7 个槽整体滤掉。
  两边都按整窗而不是按模式大小算，因为窗口实际用多少由 `fb_init` 里 DISPI 回报的
  pitch 决定，而那在两处代码之后才知道。两侧条件一致：`bootinfo.fb.ok && phys_addr`
  （无帧缓冲时这 14 MiB 仍在 buddy 里）。
- **内存类型用 WB**（与帧自带的线性别名同型）：慢速 aperture 从"每像素一次 UC 写"
  变成"每次呈现一次逐行宽拷贝（只拷脏区域）"。
- **呈现只拷脏区域**：每次写入后把所写包围盒并入脏箱（`fb_mark_dirty(x,y,w,h)`，
  裁剪到可见区，完全在屏外的写不标记）；`fb_flip` 在 `fb_box_lock`（irqsave）内
  **快照并清空**脏箱，逐行拷贝在锁外进行。两条结构性规则防止标记被 present 吞掉：
  **先写后标**（present 最多漏一帧，不会消费"还没发生"的写）与**锁内快照+清空**
  （标记要么完整进入本次快照，要么落在清空之后、留给下一次 present）。无待呈现内容时
  `fb_flip` 是空操作。逐行拷贝走 `fb_copy_fwd`（对齐前奏 + u64 宽循环 + 字节收尾）：
  宽路径的收尾必须先把字节指针跟到宽循环之后再用，否则每行末尾 `n % 8` 字节——
  也就是脏箱的右缘——会漏拷而留下旧像素（2026-10-21 修复，`test_fb_dirty_brute`
  以随机坐标钉住这个形状）。`fbc_scroll` 的裸 `fb_copy_fwd` 是唯一绕过 `fb_store_px` 的写，
  自己补标；ktest 专用 `fb_poke_back_px()` 不标记——"未标记不上屏"只有经由它才可观测。
- **谁来 present（两条时钟）**：控制台每个字符只标记其字格区域；
  **定时器启动后**由 BSP tick 节流（`FB_PRESENT_TICKS = 2`，HZ=100 即 ≤ 50 次/秒，
  且总在 tick 处理器里，不会出现在打印它的那个 ISR 里）；**第一次 tick 之前**
  （`fb_init` 到 `lapic_timer_start` 之间，含 ktest 与 `kernel_spawn`）没有时钟，
  由控制台按 `FB_PRESENT_CHARS = 1024` 字符自己 present。`panic()` 与 `kernel_spawn`
  失败的 Stop! 路径额外手动 `fb_flip()`——它们之后再也不会有 tick。
- **失败即退回单缓冲**：模式超出 14 MiB 窗口或 RAM 不足时 `fb_back` 保持 `NULL`，
  `fb_ptr` 留在 LFB，`fb_flip` / `fb_present_due` 全是空操作，其余路径一行不改
  （它们本来就只经由 `fb_ptr`）。没有开关，也没有可选缓冲 API。
- 归属操作：`fb_claim(pid)` 与 `fb_disown(pid)` 都用 `__atomic_compare_exchange_n`
  （SEQ_CST）而不是 check-then-set——两个 CPU 可能同时看到空闲屏。`fb_disown` 只清
  自己持有的；拒绝也不改归属（否则被挡下的那个反而占住屏）。

### 7.6 用户态图形

`init/ulib.h` 的 `kfb_info/kfb_clear/kfb_fill/kfb_char/kfb_puts/kfb_flip` 与 23–27、32
一一对应，参数结构体带 `_Static_assert` 锁布局。用法：23–27 落笔进离屏后缓冲，
`kfb_flip()` 一次把脏区域推上前台；不调 flip 也行，tick 每 20 ms 兜底 present。
颜色一律 32 位 `0x00RRGGBB`（16 个 `FB_CLR_*` 命名色），坐标是像素坐标。

## 8. SMP 与 ACPI

### 8.1 ACPI（`kernel/acpi.c`）

- RSDP 在传统 BIOS 区（1 MiB 以下，恒等映射内直读）→ RSDT/XSDT → MADT。
- 产出两样东西：`acpi_collect_ap_ids()`（启用的 local APIC/x2APIC 名单，含 BSP，去重，
  上限 `MAX_CPUS`）与 `acpi_ioapic_base()`（第一个 IOAPIC 的 MMIO 地址，缺省
  `0xFEC00000`）。找不到 MADT 不是致命错误——`smp_init` 回退顺序探测。
- **表读取必须经 `ACPI_SCRATCH_VA` 弹跳窗**（`0xffffffff8e000000`，`PD_HI[108]`）：
  真机固件常把 ACPI 表放在恒等窗口外，直读就是野访问。`acpi_init()` 在 `vmm_init()`
  之后、`cpu_init()` 之前跑（前者提供 `vmm_map_kernel_page`，后者要 IOAPIC 地址）。

### 8.2 AP 拉起（`kernel/smp.c` + `arch/trampoline.S`）

```
smp_init():
 1. copy_trampoline(): memcpy 到物理 0x8000、清共享数据块、在 0x8800 建过渡 GDT
 2. 注册 IPI handler：0xF0 reschedule（置 need_resched）、0xF2 stop（cli;hlt）
 3. 枚举 AP：MADT 名单非空则按名单逐个启动；否则 try_id = 1..MAX_CPUS-1 顺序探测
 4. ap_startup(apic_id, cpu_id, pml4)（全程持 smp_lock）:
      trampoline_setup(pml4, 栈顶, cpu_id, gdt, ap_main 的【虚拟地址】)
        ← 传 VA 不传 PA：ap_main 的代码用高半 RIP 相对寻址访问全局
      INIT + mdelay(10) → SIPI(0x08) + mdelay(2) × 最多 3 次
      轮询 trampoline_is_ready()，AP_STARTUP_TIMEOUT_MS(100ms) 超时
      失败路径：再复查一次 ready；仍失败则补一发 INIT 复位 + trampoline_clear()
```

共享块契约（`include/sys/smp.h` 的 `struct trampoline_data` ↔ `arch/trampoline.S`
的手工偏移，物理 `0x9000`）：

| 偏移 | 字段 |
|---|---|
| `0x00` | `page_dir`（CR3） |
| `0x08` | `stack_top` |
| `0x10` | `ready` |
| `0x18` | `gdt_packed` |
| `0x20` | `ap_main` 的虚拟地址 |
| `0x28` | 其余字段与保留位 |

`ap_main(ap_id)` 的顺序是硬约束，逐项都有注释解释原因：

1. `cpu_table[ap_id]` 的**单点初始化**（BSP 不预填）`started = false`。
2. `apic_enable()` —— 必须最先，定型本核的 LAPIC 访问模式（x2APIC 下访问 MMIO 会 #GP）。
3. `gdt_init_cpu(ap_id)` → `cpu_set_gs_base(&cpu_table[ap_id])` —— GDT 之后才能设 GS
   （QEMU TCG 下 `gdt_reload` 的 `mov gs,ax` 会清掉隐藏基址）。
4. `idt_init()`（本 CPU 独立 IDT）→ 修 CR0/CR4 → `cpu_protect_init()`（CR0.WP + CPUID
   门控的 SMEP/SMAP）。
5. 建运行队列环、TPR=0、屏蔽 8259。
6. `sched_create_idle()` → `c->started = true` → **`trampoline_signal_ready()`**
   —— **ready 信号先于一切控制台输出**：慢控制台（如 KVM 下 UC 映射的滚屏）会吃光
   BSP 的 100 ms 预算，BSP 判定失败后发恢复 INIT 会把正持 `print_lock` 的 AP 复位。
7. 打印回执 → `lapic_timer_start(32, HZ)`（用 BSP 缓存的 period，不碰 PIT；
   `jiffies` 仍只归 BSP 递增）→ 打印 `online` → `for(;;){schedule(); sti; hlt;}`。

### 8.3 LAPIC / IOAPIC（`kernel/apic.c`）

- **xAPIC（MMIO）与 x2APIC（MSR）双路**，由每核 `apic_enable()` 各自定型
  （`s_x2apic` 从不跨核继承）。xAPIC 下 LAPIC/IOAPIC 映射进 `DEV_VMA` 的两个相邻
  4 KiB 页，都带 `PG_PCD`；x2APIC 下寄存器走 MSR `0x800 + (reg >> 4)`，
  ICR 是一次 64 位写。只懂 MMIO 的驱动在 x2APIC 机器上会把 INIT/SIPI 写进黑洞，
  AP 永远起不来——这正是 AP 枚举改走 MADT 并按模式分路的原因。
- API：`apic_init`（定型 → 仅 xAPIC 映射 MMIO → 屏蔽 8259、TPR=0、全 LVT 掩蔽）、
  `apic_enable`、`apic_get_id`、`apic_send_ipi`、`apic_send_init_ipi`、
  `apic_send_sipi`（vector 0x08 → 物理 `0x8000`）、`apic_eoi`、
  `ioapic_set_irq / mask_irq / unmask_irq`。
- 当前只用于 INIT-SIPI-SIPI 与 IPI，不接收设备中断。

### 8.4 时钟与锁

- `HZ 100`；**PIT 不驱动时钟**（模式 2 编程，只给 LAPIC 标定提供时间窗）。
  BSP 的 LAPIC 定时器 → 向量 32 → `pit_handler`：BSP 递增 `jiffies`，每个 CPU 都跑
  `sched_tick` / `sched_maybe_preempt`，并调 `fb_tick_flush()`（双缓冲的呈现时钟）。
- `lapic_timer_calibrate()` 在 `smp_init` 之前只跑一次，测出 period 与 `tsc_per_ms`
  供所有 AP 复用；`mdelay()` 用 `tsc_per_ms` 换算。
- 内核里的锁（`spinlock_t`，实现只有 `spin_lock_irqsave` / `spin_unlock_irqrestore`
  一对，初始化走 `SPINLOCK_INIT`）：

| 锁 | 位置 | 保护 | 备注 |
|---|---|---|---|
| `runq_lock` | `sched.c` | 所有 per-CPU 运行队列 | 总是 irqsave |
| `task_table_lock` | `sched.c` | 任务槽分配状态 | |
| `pid_lock` | `task.c` | pid 计数器 | |
| `pmm_lock` | `pmm.c` | buddy 空闲链 + 位图 | |
| `kheap_lock` | `kheap.c` | 内核堆分箱 | |
| `cache_lock` | `blk_cache.c` | 块缓存 | **跨越设备 I/O** |
| `fat_fs_lock` | `include/fat32_priv.h` | 全部 FAT32 操作 | 与 `cache_lock` 嵌套 |
| `print_lock` | `print.c` | `kvprintf` 与 `console_write` | 内部名 `console_out_lock` |
| `fb_box_lock` | `framebuffer.c` | 双缓冲脏箱与 `fb_dirty`（present 只在锁内快照+清空） | 拷贝在锁外 |

`fat_fs_lock` 是一个全局锁串行化整个文件系统：简单正确但零并发度，是双核下文件系统
性能的瓶颈。

### 8.5 中断与异常

- `arch/entry64.S` 的 `ISR_NOERR` / `ISR_ERR` 两个宏压出统一的 `struct intr_frame`
  （地址递增顺序：`r15…rax rbx`，然后 `intno err rip cs rflags ussp usss`；
  `rbx` 刻意排在 `rax` 之后——它是 SysV callee-saved）。
- `kernel/isr.c` 的 `isr_common` 三段分流：`0xF0..0xF2` → `ipi_dispatch` → `apic_eoi`；
  `< 32` → 异常（ring-3 转信号，ring-0 panic）；`≥ 32` → **先 `apic_eoi` 再调 handler**
  （handler 可能 `schedule()` 切走，延迟 EOI 会把这个向量卡住直到它被切回来）；
  最后返回用户态前 `do_signal_check()`。
- `idt_init`：0–47 → `isr_stub_table`；128 → `isr128`（DPL=3）；**49–255 全指向
  catch-all**，所以 IPI 向量必须显式覆盖，否则重调度 IPI 会被当成 #XM。
- #PF 错误码逐位解码，并用 `rflags.AC` 区分 WP 违例与 SMAP 违例（两者错误码相同）；
  ring-0 入口无条件清 `AC`，防止带着 STAC 状态回到内核。
- 向量分配：32 = LAPIC timer，33 = PS/2 键盘，36 = COM1，`0xF0` reschedule、
  `0xF2` stop，spurious IRQ7/IRQ15 有专门分支。
- 保护位：`EFER.NXE`（`vmm_init` 兜底置位）、`CR0.WP`、`CR4.SMEP`、`CR4.SMAP`
  由 BSP 的 `cpu_init` 与 AP 的 `ap_main` 共同走 `cpu_protect_init()` 置位。

## 9. 自测

### 9.1 内核自测（`ktest/`）

框架与用例分离：`ktest/ktest.c` + **22 个 `t_*.c`**；用 `KTEST(suite, stage, fn)`
注册进 `.ktest` 段，按 `KTEST_EARLY / KTEST_MM / KTEST_LATE` 三阶段在 `start_kernel`
里跑（位置见《启动》的 bring-up 顺序）。编译开关 `KTEST=1`；`make smoke` 会强制
`KTEST=1`，**直跑 `python3 scripts/smoke_test.py` 会绕过它**，把内核建成无自测版本。

当前判据（`build/smoke/serial.log`）：

| 阶段 | cases | checks | failed |
|---|---|---|---|
| early | 25 | 371 | 0 |
| mm | 17 | 437 | 0 |
| late | 73 | 2226 | 0 |
| 合计 | 115 | 3034 | 0（`[2 skipped]`、`[1 warn]`） |

汇总行 `[ktest] selftest PASS (115 cases, 3034 checks) [2 skipped]` 与
`[selftest] 3034/3034 pass [1 warn]`——两者以实机 boot 日志为准，此表是
2026-10-30（fatfile/mbr 边界批次之后）的快照，后续加用例先改这里；那个 warn 是
用例自己打的**故意告警**，`WARN` 只计数永不判负，`ASSERT` 失败计失败，
`BUG_ON` 在任何模式下都停机。

套件按主题分组（27 个 suite）：`abi boot blkcache blkio console cpu cred draw elf
fatfile fb jrn kbd kheap ktest lib lock mbr mouse oom pmm prop syscall task usercopy vfs
vmm`，覆盖 ABI 布局与
编号连续性、E820/命令行、锁与 TSC、CR0/CR4 保护位、凭据与能力、绘制与帧缓冲、
ELF 拒绝野入口、分箱堆与 OOM 注入、块缓存 LRU、I/O 错误注入、journal 先写日志后落盘、
系统调用分发的指针门（坏指针回 `EFAULT`、位置/目录 cookie 不被坏指针消耗）、
信号与任务、VFS/FAT32。

### 9.2 端到端冒烟（`scripts/smoke_test.py`，`make smoke`）

六个阶段，全过才退出 0：`toolchain`（工具在 PATH）→ `build`（`make all` + 磁盘镜像）→
`artifacts`（bzImage 头、vmlinux 符号、FAT32 镜像、用户 ELF）→ `boot`（无头 QEMU，
**21 条有序 marker**）→ `shell`（串口驱动 `/bin/sh`：ls、cat、systest）→
`forged FAT`（用被篡改簇链的镜像启动，要求驱动**报告**且不跟随）。

- 有序 marker 按游标顺序匹配，不匹配就判失败；`--smp N` 只替换其中两条 SMP 相关
  marker，另加一条**负向断言**（`-smp 1` 时日志里不得出现 AP marker）。
- 用户态判据 `SYSTEST_TAGS` **106 条**正则（`uname.ret=0`、`unknown.99=-38`、
  `ps.kind.idle=2`、`kill.init.term=-13`、`kbd.roundtrip=1`、
  `mouse.get.empty=-11` 之类），外加 `systest.done=1`。键盘（`kbd0` 注入→读回）与
  鼠标（空环只读探针）的自检都在 systest 进程内完成，不依赖额外 /bin 工具。
- LATE 阶段另搜 6 条报告行（块缓存 LRU、写后读一致、FAT 区不缓存、journal 先写日志 +
  重放），因为 `.ktest` 段内的先后顺序是编译器的事。
- 负面判据：日志不得出现 `WARN(`、`2097151`、`4294967295`、`[PANIC]`。
- 当前 `make smoke` **203/203**；transcript 写 `build/smoke/serial.log`
  （非默认 smp 档位写 `serial-smpN.log`，互不覆盖）。
- 日志里出现的 `[fat32] I/O error` 是错误注入用例**故意**打的，不是回归。

### 9.3 用户态自检（`user/systest.c`）

按系统调用编号分节的全量自检：错误码路径、信号与等待（含 WNOHANG）、`fb_*` 图形、
故意段错误隔离、降权子进程段（`setuid(1000)` 后逐个撞权限检查点）、超长路径段、
内存记账与配额段、画屏归属段（三层 fork：抢屏 → 撞墙 → 持有者退出），键盘段
（`kbd_info` 空环快照 + `kbd_get` 的 `EAGAIN` + 注入 `kbd0` 的注入/读回/清空 + 降权
拒绝回执），鼠标段（`mouse_info` 空环快照 + 只读 `EAGAIN` 探针 + 降权拒绝回执，
真实指针在环里时打 `mouse.skip` 不去碰事件），末尾打 `systest.done=1`。由 smoke 的
`shell` 阶段在串口上驱动。

## 10. 关键不变式

一条一行，改代码时逐条对：

- `include/abi/lnxrm_syscalls.def` 是编号**唯一**定义源：只追加、不重排；
  `LNXRM_SYSCALL_LAST` 必须随末尾编号加一（编译期校验会拦住忘改的）；
  未知编号一律回 `-38`。
- 图形调用的门顺序是**先 `cap_gate` 后 `fb_owner_gate`**；`fb_info` 两道门都不过；
  两种拒绝都回 `-13`，只靠回执区分。
- 画屏归属的释放**只在 `sys_exit` 一处**（所有死法都经过它），`task_free_slot` 不放第二份。
- `main.c` 的 `pmm_reserve` 与 `vmm.c` 的别名过滤**必须成对**，尤其双缓冲窗 `[97,104)`
  两边都按**整窗**算，不按模式大小算。
- `RESV_MAX 8` 已用 7（有帧缓冲时）：新增固定窗口必须同时加 reserve 与别名过滤。
- 物理管理窗口 ≤ `PMM_WINDOW_TOP`（2 GiB）= 恒等映射 / 高半别名的范围；
  越界播种 = #PF，而 IDT 建好之前 = 三重故障无痕复位。
- `console_init → gdt_init → gs → idt_init` 必须是 `start_kernel` 的头四步。
- `0x6F00` 哨兵必须按 **DWORD** 读（两个写入者都只写低 4 字节）。
- `arch/kernel.ld` / `arch/entry64.S` / `kernel/mm/vmm.c` 三处页表与段布局硬编码手工同步。
- 新地址空间只复制**除槽 255 外**的 PML4 条目；槽 0/511 永远共享。
- 释放已映射的用户帧必须**先 `vmm_unmap_user` 再 `pmm_free`**，否则同一帧还两次。
- buddy 最大块 4 MiB（`MAX_ORDER 11`）；`kmalloc` 超 4096 字节走 buddy，
  OOM 返回 `NULL`，调用方必须检查。
- 内存配额唯一来源是 `cred_mem_quota()`，`struct task` 不存配额字段；
  页数真相是 `vmm_count_user_pages()` 的页表行走，记账与 `ps` 都用它。
- 改 `struct task` 必须**全量重编**（`struct cpu_info` 内嵌按值的 `runq_head`，
  其内部偏移跟着变，按旧偏移读写运行队列就是 #GP）。
- 运行队列是 push-front **LIFO**，不是环形；`schedule()` 重排 current 必须走
  `runqueue_add_local()`（回远端队列会被对方抢走，双跑）。
- 只有 `pml4` 不同才切 CR3；idle 任务的 pml4 必须是 `master_pml4_phys()`。
- 信号只在 `iretq` 之前、带 ring-3 守卫的地方投递；一次只投一帧。
- restorer 页是唯一 `exec=true` 的用户页（其余用户页 NX），每个地址空间映射一次。
- `blk_io_read/write` 是全内核唯一调用 `dev->read/write` 的地方（含分区层）。
- FAT 区**永不进块缓存**；元数据走 redo 日志，文件数据字节走写回缓存（ordered）。
- journal `commit` 第 ④ 步失败必须**保留 COMMIT 状态**；失败路径也必须 commit。
- 一个扇区只走一条读写路（数据/目录经缓存、FAT 区直写、挂载期直读），三者不重叠。
- EOI 必须在 handler **之前**写（handler 可能切走）；IPI 向量必须显式盖过 catch-all。
- AP 的 `ready` 信号必须先于一切控制台输出；`cpu_table[ap_id]` 只在 `ap_main` 初始化。
- trampoline 传给共享块的 `ap_main` 是**虚拟地址**。
- ABI 结构体（`lnxrm_ps_entry` 等）一改，引用它的用户态程序必须全量重编。
- 自旋锁只用 irqsave 变体；`fat_fs_lock` 内嵌 `cache_lock`，加锁顺序不得颠倒。
- 双缓冲呈现只拷脏区域：**先写后标**；`fb_flip` 在 `fb_box_lock` 内快照+清空脏箱，
  拷贝在锁外；绕过 `fb_store_px` 的写（如 `fbc_scroll`）必须自己补标。
