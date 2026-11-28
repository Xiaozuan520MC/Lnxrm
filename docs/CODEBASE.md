# lnxrm 代码库组织与子系统说明（导航入口）

本文按子系统拆分为若干分类文件，每个分类一个文件：

| 分类 | 文件 | 内容 |
|---|---|---|
| §1 总览 | `CODEBASE/CODEBASE_OVERVIEW.md` | 产物、目录树、容量常量 |
| §2 启动 | `CODEBASE/CODEBASE_BOOT.md` | 压缩启动链（S3.5）、`start_kernel` 顺序、页表、GS/MSR 初始化 |
| §3 内存 | `CODEBASE/CODEBASE_MEMORY.md` | buddy、kheap、三槽地址空间、W^X、COW、#PF、mmap、OOM |
| §4 进程·调度·信号 | `CODEBASE/CODEBASE_PROC.md` | task 槽、fork/execve/exit、调度、信号、凭据 |
| §5 系统调用与 ABI | `CODEBASE/CODEBASE_SYSCALL.md` | 两道门、sysents 表、swapgs/TLS、arch_prctl、56 项总表 |
| §6 文件系统 | `CODEBASE/CODEBASE_FS.md` | VFS、pipe/shm/dipc、ext2、FAT32、journal、/mod |
| §7 驱动 | `CODEBASE/CODEBASE_DRIVERS.md` | 串口/控制台、键盘、鼠标、ring3 模块、AHCI、帧缓冲 |
| §8 SMP 与 ACPI | `CODEBASE/CODEBASE_SMP.md` | ACPI、AP 拉起、APIC、锁表、中断、系统调用入口/出口 |
| §9 自测 | `CODEBASE/CODEBASE_SELFTEST.md` | ktest、smoke、systest |
| §10 关键不变式 | `CODEBASE/CODEBASE_INVARIANTS.md` | 改代码时逐条对的不变式 |

构建与运行见 `BUILD.md`，差距与计划见 `GAP_ANALYSIS.md`（已收口条目在 `GAP_ANALYSIS_DONE.md`），项目概览见 `README.md`。
