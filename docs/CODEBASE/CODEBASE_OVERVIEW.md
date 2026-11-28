# lnxrm 代码库组织与子系统说明（1：总览）

> 配套文件：`CODEBASE_BOOT.md`（§2 启动）、`CODEBASE_MEMORY.md`（§3 内存）、`CODEBASE_PROC.md`（§4 进程·调度·信号）、`CODEBASE_SYSCALL.md`（§5 系统调用与 ABI）、`CODEBASE_FS.md`（§6 文件系统）、`CODEBASE_DRIVERS.md`（§7 驱动）、`CODEBASE_SMP.md`（§8 SMP 与 ACPI）、`CODEBASE_SELFTEST.md`（§9 自测）、`CODEBASE_INVARIANTS.md`（§10 关键不变式）。构建与运行见 `../BUILD/BUILD.md`，差距与计划见 `../GAP_ANALYSIS.md`，项目概览见 `../README.md`。本文为 §1 总览与容量常量。

x86-64 freestanding 单体内核：C 为主体，启动/中断/上下文切换用 NASM，产出 Linux boot protocol 2.08 的 `bzImage`，**ext2 系统盘挂根**（S2 起；FAT32 驱动仍在，只是不再当根）并跑自带 shell。**只有 C 与 ASM**，构建零警告。

## 1. 总览

| 产物 | 说明 |
|---|---|
| `build/bzImage` | `setup.bin` + `vmlinux.bin`，两个 Python 脚本回填 boot header |
| `build/vmlinux.elf/.bin` | 链接产物（`arch/kernel.ld`，单一 `.image` 段 VMA `0xffffffff80100000` / LMA `0x100000`）/ 裸 payload |
| `build/disk.img` | 64 MiB ext2 系统盘（S2 起：`mke2fs` + `debugfs` 造，装 `/README.md`、`/bin`、`/mod`；4 KiB 块 × 16384、16384 个 256 B inode、单块组、**无 journal**） |
| `build/user/<name>` | 16 个静态 ELF64 用户程序 |
| `build/mod/<name>.km` | ring3 驱动模块 ELF（**`.km` 由 `MOD_ELFS` 在链接期加上**，镜像 `/mod` 里同名），现为 `ps2-keyboard.km`、`ps2-mouse.km` 两个（2026-11-19 加 `ps2-` 前缀） |
| `build/lnxrm.iso` | GRUB 引导 ISO（仅 bootloader + 内核，无根文件系统） |
| `build/obj0/`、`build/obj1/` | 两种 `KTEST` 模式各自的对象与 `.d`（两模式并存，切换只重链 ~0.2 s、零重编；`trampoline.bin` 仍留 `build/` 根，符号名由路径导出） |

补丁脚本是构建链关键：`patch_setup_jmp.py` 回填 `setup.bin` 哨兵 `0xF00DFACE` 的真实跳转，`patch_bzimage.py` 填 boot header 并断言 `HdrS`/`0x55AA`/payload 对齐（详见 `../BUILD/BUILD.md`）。

```
Makefile               构建入口（源文件自动发现 + 镜像/运行/冒烟）；用户态委托 userland/Makefile；工具链 clang → ld.lld → llvm-objcopy（binutils 留给 `nm` 与 smoke 检查）；对象按 KTEST 分目录 build/obj<N>/，模式标记 .mode-<N> 挂链接侧
arch/                  setup.asm(实模式) entry64.S(_start32/64、中断桩、swtch) trampoline.S(AP 入口,拷 0x8000)
                        cpu.c(GDT/TSS/IDT/PIT/LAPIC) kernel.ld(段边界符号 __text_*/__rodata_*/__data_* 供 W^X)
include/               全部头文件：abi/(lnxrm_abi.h + lnxrm_syscalls.def + keyboard.h + mouse.h) sys/ mm/ 其余
kernel/                main.c(start_kernel 编排)
  core/                syscall.c(分发/门) print.c klog.c(环+loglevel) backtrace.c percpu.c smp.c protect.c selftest.c rtc.c
  sched/               task.c(fork/execve/exit/wait4) sched.c(RR/锁) elf.c signal.c
  irq/                 isr.c(分发/异常/IPI) apic.c acpi.c
  mm/                  pmm.c(buddy+帧引用计数) kheap.c vmm.c(三槽地址空间+W^X) vma.c pagefault.c
                        filepage.c(文件映射共享页缓存) oom.c(OOM killer)
  fs/                  vfs.c pipe.c shm.c blk_cache.c mbr.c
  fs/ext2/             ext2.c(挂载校验/路径切分/目录记录/12 个 fs_ops 回调) block/block.c(块层：读写+bmap+组位图分配+截断) inode/inode.c(inode 表读写)；公共头 include/ext2.h（S2 起挂根）
  fs/fat32/            fat32.c fat32_dir.c fat32_file.c fat32_meta.c fat32_journal.c
  ipc/                 dipc.c(驱动 IPC 通道：名字表+共享帧环+等待链, 53/54/55)
  drivers/             pci.c ahci.c uart.c ps2-mouse.c(辅助口鼠标 + dipc 通道；重组/字段解码抽到 abi/ps2-mouse.h，**2026-11-19 由 `ps2mouse.c` 改名**) framebuffer.c input_event.c ps2-keyboard.c(扫描码解码单源 + dipc 通道)
ktest/                 启动自测：ktest.c 框架 + 33 个 t_*.c + include/（在仓库根）
userland/              Makefile + bin/ 16 个程序（lsh 解析 `|` 与 `<`/`>`、内建 cd/pwd；mv 兼做改名；cat 无参数读 stdin；ls 无参时 kgetcwd 取当前目录）+ mod/ ring3 驱动模块（S1b，现为 ps2-keyboard 与 ps2-mouse 两个 `.km`（2026-11-19 加 `ps2-` 前缀），与 bin/ 同配方、独立对象子目录、装进镜像 `/mod`）+ lib/(crt0.S + ulib.c)
scripts/               patch_setup_jmp.py patch_bzimage.py smoke_test.py
```

`userland/lib/` 是**每个**用户程序链接的运行库，不是 `/init`。

### 容量常量

| 常量 | 值 | 定义处 | 常量 | 值 | 定义处 |
|---|---|---|---|---|---|
| `MAX_CPUS`/`NR_TASKS`/`NR_FDS` | 8/64/16 | `include/types.h` | `MAX_ORDER`/`MIN_ORDER_BITS` | 11/12（buddy 最大 4 MiB） | `pmm.c` |
| `KSTACK_SIZE` | 32768 | `sys/sched.h` | `PMM_WINDOW_TOP` | `0x80000000`（2 GiB） | `mm/mm.h` |
| `LNXRM_NR_SIGNALS` | 14（编号 1..13） | `lnxrm_abi.h` | `RESV_MAX`/`KHEAP_NBINS` | 8/9 | `pmm.c`/`mm.h` |
| `LNXRM_SYSCALL_LAST` | 57（共 58 个） | `lnxrm_abi.h` | `CRED_USER_MEM_QUOTA_PAGES` | 64 MiB/4 KiB | `sys/cred.h` |
| `LNXRM_SHM_MAX_SIZE`/`MAX_OBJECTS` | 1 GiB/64 | `sys/shm.h` | `LNXRM_SHM_NAME_MAX` | 32（含 NUL） | `sys/shm.h` |
| `LNXRM_DIPC_MAX`/`NAME_MAX` | 8/32（含 NUL） | `sys/dipc.h` | `LNXRM_DIPC_RING_SIZE`/`PAY_SIZE` | 4096/4088（−8 B 头） | `lnxrm_abi.h`/`sys/dipc.h` |
| `CACHE_ENTRIES`/`MAX_MOUNTS` | 64/4 | `disk.h`/`vfs.c` | `HZ`/`AP_STARTUP_TIMEOUT_MS` | 100/100 | `sys/cpu.h`/`sys/smp.h` |
