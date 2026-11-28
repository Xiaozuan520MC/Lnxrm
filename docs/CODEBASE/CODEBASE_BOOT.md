# lnxrm 代码库组织与子系统说明（2：启动）

> 配套文件：`CODEBASE_OVERVIEW.md`（§1 总览）、`CODEBASE_MEMORY.md`（§3 内存）、`CODEBASE_PROC.md`（§4 进程·调度·信号）、`CODEBASE_SYSCALL.md`（§5 系统调用与 ABI）、`CODEBASE_FS.md`（§6 文件系统）、`CODEBASE_DRIVERS.md`（§7 驱动）、`CODEBASE_SMP.md`（§8 SMP 与 ACPI）、`CODEBASE_SELFTEST.md`（§9 自测）、`CODEBASE_INVARIANTS.md`（§10 关键不变式）。构建与运行见 `../BUILD/BUILD.md`，差距与计划见 `../GAP_ANALYSIS.md`，项目概览见 `../README.md`。本文为 §2 启动。

**两条路径在 `start_kernel` 汇合，靠物理 `0x6F00` 的 DWORD 区分（必须按 4 字节读）**：QEMU `-kernel` 写 0（E820 表 `0x7000`、VBE 写 `0x8C00`）；GRUB `linux` 写 `boot_params` 指针（非 0）。无 EFI handover，仅 legacy BIOS/CSM。

### 压缩启动链（S3.5）

`build/bzImage` 的 payload 加载到 `0x100000` 后是两段：解压 stub（`decompress.bin`，512 B 对齐）和紧随其后的 gzip 内核（`vmlinux.bin.gz`）。`arch/setup.asm` 切到长模式后跳到 `0x100000` 的 `decompress_entry`：

1. `rsp = 0x400000 - 8`：C 入口要求 `rsp % 16 == 8`（`call` 已压返回地址的约定）。
2. 把 incbin 在 stub 里的解压器搬到 `0x200000`，把 gzip 数据搬到 `0x300000`；输出回 `0x100000`（当前内核约 145 KiB，不会覆盖这两块）。
3. 按 SysV 四参数跳 `inflate_main`：`rdi = 0x300000`（压缩数据）、`rsi = 0x100000`（输出）、`rdx = compressed_size`、`rcx = kernel_entry_phys`（`_start64` 物理地址，当前 `0x10010e`；`0x100000` 是 32 位 `_start32`，长模式下不能直接跳）。
4. `inflate()` 校验 gzip 头、解 stored/fixed/dynamic 三种块，末尾核对 CRC32/ISIZE；成功后 `jmp rcx` 进 `_start64`，之后与未压缩启动完全相同。

串口标记（`-serial stdio`）：`D` 进 stub、`1` inflate blob 搬完、`2` gzip 搬完、`I` 即将跳 `inflate_main`。

`.bss` 不在 `inflate.bin` 里也不保证为零：`inflate()` 每次调用都重建 CRC32/fixed Huffman 表并把 `window_pos` 归零。`compressed_size`、`kernel_entry_phys`、`inflate_main` 由 Makefile 构建期生成 `.S` 并 `nasm`（见 `../BUILD/BUILD.md`）。

| master 表 | 物理 | 作用 |
|---|---|---|
| `PML4_M` | `0x50000` | `[0]` 恒等、`[511]` 高半 |
| `PDPT_M`/`PD_M`/`PD_M2` | `0x51000`/`0x52000`/`0x57000` | 恒等 2 MiB 大页（**镜像 slot 被 `vmm.c` 拆 4 KiB 赋 W^X**） |
| `PDPT_HI`/`PD_HI`/`PD_HI2` | `0x53000`/`0x54000`/`0x58000` | 高半别名（`PD_HI` 镜像槽同样拆页赋权） |
| `PT_VGA`/`PT_DEV` | `0x55000`/`0x56000` | `PD_HI[96]`→`0xB8000`；`PD_HI[128]`→MMIO |

恒等区与高半别名各铺 2 GiB —— `PMM_WINDOW_TOP` 的上界来源。

### `start_kernel` 顺序（顺序即契约）

1. `console_init → gdt_init → cpu_set_gs_base → idt_init` —— **必须头四步**：IDT 前任何异常 = #PF → #DF → 三重故障无痕复位。
2. `boot_collect_e820` → 横幅 → `boot_read_vbe` → `boot_read_cmdline`。
3. `ktest_run(KTEST_EARLY)`。
4. `cpu_init_percpu(0,0)` → **`pmm_reserve` ×7**（必须在 `pmm_init` 前）。
5. `pmm_init → vmm_init → kheap_init`。
6. `acpi_init` → `cpu_init`（其收尾调 `cpu_protect_init()`：`CR0.WP`/`SMEP`/`SMAP` + **SYSCALL 门四 MSR**——`EFER.SCE`、`STAR`（`[47:32]=KCODE 0x08` SYSCALL 用、`[63:48]=0x1B` SYSRET 用户 CS/SS 基数，S3.3 起）、`LSTAR`、`SYSCALL_MASK`，AP 侧走同一函数）→ `pit_init(HZ)`。**S3.4 起**三段基 MSR 另立：`cpu_set_gs_base()`（`main.c` BSP、`gdt_init_cpu`/`sched.c` `switch_to`）写 `IA32_GS_BASE`=本 CPU per-CPU 块并把 `IA32_KERNEL_GS_BASE` 归 0（即「ring-3 无 GS」），`switch_to()` 给每条任务保存/恢复 `IA32_KERNEL_GS_BASE`（该任务的 ring-3 GS）与 `IA32_FS_BASE`（该任务的 FS 线程指针，System V TLS 段），故调度边界两段都跟着走；`arch_prctl(56)` 写两者由 `h_arch_prctl` 完成（SET_FS 写 `IA32_FS_BASE` 直下，SET_GS 写 `IA32_KERNEL_GS_BASE`**绝不**碰 `IA32_GS_BASE`——前者是正在执行的 per-CPU 块），GET_* 读回。两段合起来给 ring-3 的 `%fs`/`%gs` 一个可争用、可隔离、每任务私有的居所。
7. `ktest_run(KTEST_MM)`。
8. `fb_init → ps2_kbd_init → serial_irq_init → vfs_init → ahci_init → vfs_try_mount_disk()`；**根没就绪就是没就绪**（initramfs 回退已删，见第 6 节）。
9. `do_global_ctors()` → `sched_init`。
10. `ktest_run(KTEST_LATE)` + `ktest_summary()`。
11. `kernel_spawn("/bin/init")`；失败打 `Stop!` → `fb_flip()` → hlt 死循环。
12. `lapic_timer_calibrate`（BSP 一次）→ `smp_init` → `lapic_timer_start(32, HZ)`。
13. `[boot] ready in N ms` → `sti` → `idle_loop()`。

**手工同步的硬编码**：`arch/kernel.ld` / `arch/entry64.S`（页表物理地址）/ `kernel/mm/vmm.c`（同一组常量）三处只有注释互相提醒，改一处必改另两处；**第四条线**：`vmm.c` 还读 `kernel.ld` 的 `__text_end`/`__rodata_end`/`__bss_end` 决定 W^X 策略，`t_mm.c` 的 `test_vmm_kernel_wx` 钉住落点。
