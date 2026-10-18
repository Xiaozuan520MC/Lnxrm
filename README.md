lnxrm
=====

lnxrm官网: https://develop2026.dpdns.org

lnxrm 是一个 x86-64 单体内核，用 ASM + C 从零写成。
它管理硬件、内存、进程、文件系统，并向用户态提供 32 个系统调用编号（0–31 连续无空洞），全部已实现。

约 20,260 行内核代码（kernel/+arch/+include/，含 4,744 行内核自测 ktest/，85 个用例）
+ 1,812 行用户态程序（16 个）。产出符合 Linux x86 boot protocol 2.08 的 bzImage，
在 QEMU 下挂载 FAT32 并运行自带 shell。

快速开始
--------

    make                      # 构建内核（约 2 秒）
    make usr build/disk.img   # 构建用户态程序和 FAT32 磁盘镜像
    make run                  # 在 QEMU 中启动
    make imageiso             # 生成 GRUB 引导的可启动 ISO → build/lnxrm.iso

注意 make 不生成磁盘镜像；make clean 之后需要重新执行上面第二行。
ISO 也不含根文件系统，光盘启动仍要另挂 build/disk.img（见 docs/BUILD.md §8.2）。

文档（3 篇）
-------------

* docs/BUILD.md          构建指南：工具链、构建链、运行方式、四种调试手段
* docs/CODEBASE.md       代码导览 + 功能总览
    * 第一部分 代码导览 —— 每个文件是什么、如何组织、建议阅读顺序
    * 第二部分 功能总览 —— 能力清单，逐项给出代码/实测出处
* docs/GAP_ANALYSIS.md   差距 · 评审 · 行动计划
    * 第一部分 差距分析 —— 距离"真正的内核"还差什么（72 条编号缺陷，C1–C72）
    * 第二部分 评审记录 —— 实测结论、与好内核的差距、安全审查正/负面清单
    * 第三部分 行动计划 —— 55 个任务卡（T-000–T-099），按依赖关系排序

你是谁？
========

* 想读懂代码       → CODEBASE.md 第一部分
* 想知道能干什么   → CODEBASE.md 第二部分
* 想构建和调试     → BUILD.md
* 想参与开发       → GAP_ANALYSIS.md 第三部分（行动计划）
* 评估健壮性或安全 → GAP_ANALYSIS.md 第一部分（72 条缺陷编目，C1–C72）与第二部分（评审与安全审查）

给特定读者的说明
================

想读懂这个内核
--------------

代码库的组织方式是非显然的，建议按这个顺序：

1. include/abi/lnxrm_syscalls.def —— 32 个编号（0–31，连续无空洞），这是内核对外的全部接口
2. include/sys/sched.h —— struct task 和 struct intr_frame，最核心的头文件
3. kernel/main.c —— 整个 bring-up 顺序，一目了然
4. kernel/mm/vmm.c 文件头 —— 三槽 PML4 地址空间模型的说明

docs/CODEBASE.md 第一部分第 17 节给出了完整的七遍阅读顺序，以及建议一开始
不要读的文件（fat32_meta.c、framebuffer.c、setup.asm）。

想构建和运行
------------

需要 gcc、binutils、nasm、python3，以及 mtools 和 dosfstools
（后两个用来在宿主机构造 FAT32 镜像）。

安装与工具链版本见 docs/BUILD.md 第 1 节。构建并行安全；设
SOURCE_DATE_EPOCH 后可逐字节复现（唯一的默认变量是 uname
里嵌的构建时间戳，见 docs/BUILD.md §14.3）。

调试有四种手段，其中最有用的是 debugcon —— arch/entry64.S 在启动的
七个阶段向端口 0xE9 打单字符标记，串口日志要等 console_init() 之后
才有，而这条通道从第一条指令就可用。分诊表见 docs/BUILD.md 第 9.2 节。

GDB 断在 start_kernel 的实测输出见 docs/BUILD.md 第 9.3 节：
源码级符号完整，可直接 break start_kernel。

想参与开发
----------

验证基建已经就位，不再是"全树零测试"：
* 内核自测 —— ktest/ 85 个用例（4,741 行），启动时跑，`[ktest]` 打点，失败即停
* 端到端冒烟 —— `make smoke`（scripts/smoke_test.py，179/179），QEMU 无头 + 串口断言，
  含 `-m`/`-smp` 矩阵、故障注入、forged-FAT、fsck.vfat 固定检查

路线图（GAP_ANALYSIS 第三部分）的 Step 0–3 已完成：冒烟门控、止血、自测框架、
SMEP/SMAP/NX 硬边界。当前关键路径在 **Step 5 契约对齐 → Step 7 文件系统硬化**，
之后是 Step 6 身份生命周期与 Step 9 地址空间（mmap/COW）。

仍然成立的纪律：**不要在没有测试覆盖的情况下改动并发路径或 MMU** ——
这类缺陷无法定位。新增行为必须带一个断言或 smoke tag 守着。

明确不做（GAP_ANALYSIS §10）：ext2/4、POSIX 完整兼容、新子系统，
以及 KPTI/eBPF/cgroup/namespace —— 加机制前先问它解决的是这个内核的实际问题吗。

