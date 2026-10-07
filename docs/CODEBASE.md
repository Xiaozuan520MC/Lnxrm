# lnxrm 代码库导览与功能总览

> 面向：想读懂 / 接手 / 扩展这个内核的人
> 代码库规模：16,029 行内核 + 1,768 行用户态（2026-10-06 实测；内核 = `kernel/` 12,190（含 `ktest/` 3,297 行自测）+ `arch/` 1,670 + `include/` 2,169。2026-10-05 口径为 15,895，差 +134 = FAT32 修复 +43 与真机三重故障修复 +91，见 §35）
> 本文两部分：**第一部分 代码导览**（每个文件是什么、怎么组织，§0–§19） · **第二部分 功能总览**（能干什么、实测出处，§20–§36）
> 配套文档：[BUILD.md](BUILD.md)（构建与调试） · [GAP_ANALYSIS.md](GAP_ANALYSIS.md)（差距 · 评审 · 计划）

---

## 第一部分 · 代码导览


### 0. 一句话概括

**lnxrm 是一个 x86-64 单体内核**：C 为主体，C++ 负责 SMP 与 AHCI，Rust 负责串口驱动与同步原语，启动/中断/上下文切换用 NASM 汇编。产出符合 Linux x86 boot protocol 2.08 的 `bzImage`，在 QEMU 下挂载 FAT32 并运行自带 shell。

本文按"**每个文件是什么、它依赖谁、谁依赖它**"组织，不是按重要性。

---

### 1. 项目统计

#### 1.1 代码量

| 语言 | 文件数 | 行数 | 占比 | 主要用途 |
|---|---|---|---|---|
| C | 62 | 13,236 | 74% | 内核主体 + `ktest/` 自测 + 用户态程序 |
| C 头文件 `.h` | 22 | 2,218 | 12% | 接口定义 |
| NASM `.S` | 3 | 650 | 4% | 入口、AP trampoline、`crt0` |
| NASM `.asm` | 1 | 572 | 3% | 实模式 setup + VBE 选型 |
| C++ | 3 | 679 | 4% | SMP 管理、AHCI 驱动、C++ 全局 `new/delete` |
| Rust | 3 | 296 | 2% | UART、AP trampoline 共享数据 |
| 链接脚本 `.ld` | 1 | 65 | — | 内核布局 |
| C++ 头文件 `.hpp` | 1 | 27 | — | `cpp/smp.hpp` |
| 系统调用定义 `.def` | 1 | 54 | — | 32 个系统调用编号（0–31 连续）+ 编号规则说明 |
| **代码合计** | **97** | **17,797** | **100%** | |
| Python 脚本 | 4 | 967 | | 构建补丁（2 个）+ 冒烟测试（`smoke_test.py` 762 行、`smoke_smp1.py` 49 行） |
| **总计** | **101** | **18,764** | | |

目录分布：`kernel/` 12,190 + `arch/` 1,670 + `include/` 2,169 = **16,029（内核）**，
`init/` 239 + `usr/` 1,529 = **1,768（用户态，16 个程序）**，`scripts/` 967。

#### 1.2 产出物

| 项 | 值 |
|---|---|
| `bzImage` | 162,200 字节（158 KB），2026-10-06 真机三重故障修复重编后（`--gc-sections` 使 254,920 → 158,968，T-033 → 160,024，T-080 → 160,856，FAT32 修复 → 161,944，C71/C72 → 162,200） |
| 内核 `.image` 段（代码+rodata+data） | 160,000 字节（LMA `0x100000`） |
| `.bss` 段 | 405,520 字节（≈396 KB，大头是 64×1,024 B 的 `task_table`） |
| 入口点 | `_start64` @ `0xffffffff8010010e` |
| 段属性 | `.image` 是 **RWE**（单一连续段的设计副作用，见 §3.3） |
| 系统调用 | 0–31 连续定义 32 个、全部有实现；32 及以上返回 ENOSYS（`include/abi/lnxrm_syscalls.def`）。其中 `ps`/`diskinfo`/`fb_*`/`kill` 带 T-031 权限检查点，4 个绘图调用再带 T-033 的画屏归属门 |
| 用户态程序 | 16（`init`、`sh` + 14 个命令） |
| 容量常量 | `MAX_CPUS 8` / `NR_TASKS 64` / `NR_FDS 16` / `NR_SIGNALS 32` / `CACHE_ENTRIES 64` / `MAX_MOUNTS 4` |

#### 1.3 构建产物链

```
usr/*.c  ──gcc(static)──►  build/usr/<name>        16 个静态 ELF64
                                        │
arch/*.S ──nasm──► *.o ─┐                  │
kernel/rust/lib.rs ──rustc──► liblnxrm.a ─┤
arch/setup.asm ──nasm──► setup.bin ─┐      ├──ld -T arch/kernel.ld──► vmlinux.elf
                          (pad 512) │      │                              │
kernel/*.c|cpp ──gcc/g++──► *.o ───┴──────┤                              ├─objcopy -O binary─► vmlinux.bin
                                     │      │                              │
                    patch_setup_jmp.py ◄──┘                              │  (按 LMA 导出)
                          │                                              │
                          └──► setup.bin (jmp 目标已修正)                │
                                        │                                │
                                        ▼                                ▼
                        cat setup.bin vmlinux.bin ──► bzImage
                                        │
                    patch_bzimage.py ───┘  填 0x1F1 setup_sects、0x1F4 syssize
```

**两个 Python 补丁脚本是这个构建链的精髓**，也是这个项目"从零实现"的体现：

- **`scripts/patch_setup_jmp.py`** —— NASM 汇编时还不知道 `_start64` 的最终地址，所以先写哨兵 `0xF00DFACE`，链接后再用 `nm` 查出 `_start64` 相对 `_start32` 的偏移，算出物理地址 `0x100000 + (start64 - start32)`，回填进 `setup.bin`。**幂等**：已经是正确值就 no-op。
- **`scripts/patch_bzimage.py`** —— 拼接后填充 Linux boot protocol 要求的 `setup_sects`（0x1F1）和 `syssize`（0x1F4）。带 3 个断言：头部魔数必须是 `HdrS`、boot flag 必须是 `0x55AA`、`payload_off` 必须是 512 的倍数且 ≥ 2048。

---

### 2. 目录地图

```
arch/          架构相关：启动、CPU 状态、上下文切换     1,670 行
include/       全部头文件（23 个）                      2,169 行
  abi/         内核↔用户态 ABI（结构体 + 系统调用编号）   278 行
  cpp/         C++ 包装类（smp）                          27 行
  sys/         核心子系统接口（sched/vfs/cred/ktest/...） 850 行
kernel/        内核主体                                 12,190 行
  mm/          物理页 / 内核堆 / 虚拟内存                822 行
  fs/          VFS + FAT32 + 块缓存 + MBR              2,797 行
    fat32/     FAT32 的 4 个模块                        1,586 行
  drivers/     PCI / IDE / AHCI                          673 行
  rust/        Rust 组件                                 296 行
  lib/         内存与字符串                              90 行
  ktest/       启动自测套件（17 个文件，T-007）         3,297 行
init/          用户态 C 运行时（crt0 + ulib）              239 行
usr/           16 个用户态程序                         1,529 行
scripts/       构建补丁 + 冒烟测试                        967 行
```

#### 各目录职责一句话

| 目录 | 职责 |
|---|---|
| `arch/` | 一切必须用汇编或直接操作 CPU 状态的东西：实模式 setup、64 位入口、中断桩、GDT/IDT/TSS、上下文切换、AP trampoline |
| `include/` | 全部接口。**没有实现文件**。ABI 定义集中在 `include/abi/` |
| `include/abi/` | 内核和用户态**共同**包含的头文件，`lnxrm_syscalls.def` 用 X-macro 同时生成 C 枚举 |
| `kernel/` | 内核 C/C++/Rust 实现 |
| `init/` | 链接进**每个**用户态程序的运行库（不是 `/init` 那个程序） |
| `usr/` | 用户态程序源码，一个 `.c` 一个二进制 |
| `scripts/` | 构建期对已生成二进制打补丁 |

---

### 3. 启动全链路

这是理解整个内核最值得先掌握的一条链。两条路径在 `main.c:56` 汇合。

#### 3.1 路径 A：QEMU `-kernel`（`make run` 走的这条）

```
① 真实模式 setup.asm
   arch/setup.asm
   ├─ 关中断，段寄存器清零
   ├─ int 15h AX=E820 ──► E820 表写到物理 0x7000，条目数在 0x6FFC（上限 64 条）
   ├─ VBE 选型（4 遍级联）：
   │   遍 A: 1920×1080，bpp 依次试 32→24→15
   │   遍 B: 1280×720，同上
   │   遍 C: 8bpp 传统 LFB 模式 0x101/0x103/0x105
   │   遍 D: vbe_enum_best() 取最大分辨率
   │   每遍都必须 4F02 真的 set 成功才信任
   │   筛选条件（vbe_probe）: ModeAttr 的 0x02/0x80/0x10 位、
   │                         320≤W≤4096、200≤H≤4096、PhysBasePtr≠0、
   │                         bpp∈{8,15,16,24,32}、MemoryModel∈{4,6}、
   │                         pitch*height ≤ 16 MiB
   ├─ 成功则把 struct vbe_lfb_info 写到物理 0x8C00（0x17 字节）
   ├─ 物理 0x6F00 写 0（DWORD，低 4 字节） ← 这是"我不是 GRUB"的哨兵
   ├─ 建临时页表（0x60000），恒等映射低 1 GiB + 高半 32 槽 + VGA 窗口
   ├─ 置 EFER.LME、CR4.PAE、CR0.PG
   └─ jmp 0x18:0xF00DFACE   ← 哨兵，链接后被 patch_setup_jmp.py 修正

② 保护模式入口
   arch/entry64.S  _start64  (BITS 64)
   ├─ 向 0xE9 debugcon 打 'K'（QEMU 专用，抓启动早期问题）
   ├─ 段寄存器全设 0x10
   ├─ 重建 master 页表（固定物理地址）：
   │   PML4_M  0x50000
   │   PDPT_M  0x51000   PML4[0]   → 恒等低内存
   │   PD_M    0x52000   PDPT[0]   → 恒等低 1 GiB（2 MiB 页）
   │   PDPT_HI 0x53000   PML4[511] → 高半
   │   PD_HI   0x54000   PDPT[510]→ 内核/堆/VGA/设备窗口
   │   PT_VGA  0x55000   PD_HI[96] → 0xB8000 文本窗口
   │   PT_DEV  0x56000   PD_HI[128]→ 设备 MMIO（C 侧填 PTE）
   ├─ 填 PD_HI：0..31 槽 = 内核镜像，96 = VGA，128 = 设备
   ├─ load_cr3(PML4_M)
   ├─ 清 .bss（__bss_start .. __bss_end）
   ├─ rsp = __boot_stack_top（.bss 末尾额外 0x4000 就是启动栈）
   └─ call start_kernel   （绝对地址，高半）

③ C 世界
   kernel/main.c  start_kernel()
   ├─ console_init() → gdt_init() → cpu_set_gs_base(&cpu_table[0]) → idt_init()
   │    ← 必须是头四步（main.c:214-224 注释）。IDT/内核 GDT 就绪之前任何异常
   │      都是 #PF → #DF → 三重故障，整机无痕复位——真机上表现为"闪几行就重启"
   ├─ boot_collect_e820()      读 *(u32*)0x6F00 决定数据源
   ├─ 横幅 + 逐条打印 e820 表 + 汇总（[mem]）、内核镜像范围（[boot]）
   ├─ boot_read_vbe()          再看 0x6F00：0 就读 0x8C00
   ├─ boot_read_cmdline()      读物理 0x20000（QEMU -append 放这里）
   ├─ ktest_run(KTEST_EARLY)
   ├─ cpu_init_percpu(0,0)     GS 基址 = &cpu_table[0]（已是 &cpu_table[0]，这里补全记账）
   ├─ pmm_reserve(LFB + FB_PD + slot96 VGA + slot128 DEV + slots256..271 KHEAP)
   │                              ← 必须在 pmm_init 之前（main.c:270-304）
   ├─ pmm_init() → vmm_init() → kheap_init()
   ├─ cpu_init()               TSS/APIC/IOAPIC/SSE（GDT/IDT 已提前建好）
   ├─ pit_init(HZ)             PIT 模式 2 编程（标定窗口用；0x36 模式 3 会跑 200 Hz）
   ├─ fb_init()                映射 LFB（PG_PCD）+ 控制台
   ├─ ps2_kbd_init()           IOAPIC IRQ1→向量 33
   ├─ serial_irq_init()        IOAPIC IRQ4→向量 36
   ├─ vfs_init() + vfs_try_mount_disk()
   │   └─ ide_init() → ahci_init() → 挂载 FAT32
   ├─ do_global_ctors()        遍历 __init_array（C++ 全局构造）
   ├─ sched_init()             task_table 清零 + BSP idle 任务
   ├─ kernel_spawn("/bin/init")  第一个用户进程（pid 1）
   ├─ lapic_timer_calibrate()  BSP 一次性标定 LAPIC period + TSC 速率（缓存供 AP 用）
   ├─ smp_init()               INIT-SIPI-SIPI 拉起 AP
   ├─ lapic_timer_start(32, HZ)  ← BSP 的周期时钟源（不是 PIT）；AP 在 ap_main 各自武装
   ├─ [mem] 可用内存 + [boot] ready in N ms   ← 启动汇总（仍在关中断下打印）
   ├─ sti
   └─ idle_loop()              永不返回
```

#### 3.2 路径 B：GRUB `linux`

```
GRUB
  └─ jmp 0x18:0x100000   （= _start32，链接在 _start64 之前）
     arch/entry64.S  _start32  (BITS 32)
     ├─ 把 ESI 里的 boot_params 指针存到物理 0x6F00（**DWORD**，只写低 4 字节）  ← 哨兵 = 非 0
     ├─ 装临时 GDT（gdt32，含 64 位代码段 0x18）
     ├─ CR4.PAE = 1
     ├─ 在 0x60000 建临时页表（同布局）
     ├─ CR3 = 0x60000, EFER.LME = 1, CR0.PG = 1
     └─ jmp 0x18:(_start64 - _start32 + 0x100000)
        └─ 汇入 _start64，与路径 A 相同
```

**关键点**：`0x6F00` 这一个 **4 字节（DWORD）**决定了后续所有数据来源。`main.c:57` 读它（三处读点 `:57`/`:97`/`:199` 全部是 `*(volatile u32*)`）：
- 为 0 → QEMU 路径，E820 在 `0x7000`、LFB 在 `0x8C00`、cmdline 在 `0x20000`
- 非 0 → GRUB 路径，E820 在 `boot_params.e820_map`、LFB 在 `boot_params.screen_info`、cmdline 在 `boot_params.hdr.cmd_line_ptr`（偏移 0x228）

⚠️ **必须按 DWORD 读**：两个写入者（`setup.asm:556`、`entry64.S:52`）都只写低 4 字节，
按 `u64` 读会让高 4 字节的内存残留（真机上是别的固件/OS 留下的垃圾）拼进指针 →
野指针解引用。2026-10-06 修（见 GAP_ANALYSIS C72）。

#### 3.3 三个文件协作定出内核布局

| 文件 | 职责 |
|---|---|
| `arch/kernel.ld` | 把所有东西压进**一个连续 `.image` 段**（text + rodata + init_array + data），`.bss` 紧随其后但 FileSiz=0。VMA=`0xffffffff80100000`，LMA=`0x100000`，严格差值 ⇒ `objcopy -O binary` 按 LMA 导出，payload 直接落在物理 `0x100000` |
| `arch/entry64.S` | 建 master 页表时**硬编码** `PML4_M=0x50000` … `PT_DEV=0x56000` |
| `kernel/mm/vmm.c` | 用**同样的常量**（`PD_HI 0x54000`、`PT_FIX 0x57000`）去写那些表 |

⚠️ 这三个地方必须手工保持同步，代码里只有注释提醒（`vmm.c:17-18` "keep in sync with entry64.S"）。

---

### 4. 内存管理（`kernel/mm/`，793 行）

三个文件，一条流水线：**buddy 页分配器 → 分离空闲链内核堆 → 页表管理**。

#### 4.1 `kernel/mm/pmm.c`（253 行）— 物理页 buddy 分配器

**职责**：把 E820 里的可用 RAM 变成 4 KiB 页的分配/释放接口。

**关键设计**：

| 设计 | 说明 | 位置 |
|---|---|---|
| 侵入式空闲链表 | 每个空闲块头 8 字节存下一指针，直接写在空闲内存里 | `:14-16` |
| **逐 order 位图** | 每个 order 一张位图记录哪些块空闲，**用来快速定位 buddy** | `:19-20` |
| `MAX_ORDER 11` | 最大 4 MiB 块 | `:10` |
| `buddy_of` = `pa ^ size` | 伙伴地址就是异或块大小 | `:77-81` |
| MMIO 保留区 | `pmm_reserve(lo,hi)` 把 LFB 等排除，播种时逐块收缩。**`RESV_MAX 8`**（2026-10-06 由 4 扩到 8：LFB、FB_PD、VGA、DEV、KHEAP 五类之后只剩 3 槽） | `:37-51` |
| 位图放在物理 `0x10000` | **隐式依赖**：只因 `pmm.c:105` 把管理窗口钳在 `PMM_WINDOW_TOP`（**1 GiB**，`include/mm/mm.h:20`）以下才不会撞上 0x100000 的镜像 | `:23,119-125` |
| **管理窗口 = 1 GiB** | `hi = MIN(hi, PMM_WINDOW_TOP)`（`:105`）+ `lo >= hi` 整段跳过（`:106`）。**这是硬上限，不是省事**：恒等映射/高半区别名在 `entry64.S` 里只有 1 GiB，超过就拿不到恒等地址（详见 §22.6） | `mm.h:20`、`pmm.c:105` |
| 无可用 RAM 时 panic | `best_end < kern_end + 18 MiB` → `panic(...)`（`:122-124`）。**2026-10-06 新增**：原来这条路径会去 `memset` 从 `kern_end` 起的"空闲"内存，把 `0x50000` 的页表本身抹掉 | `:122-124` |

**API**：`pmm_init` / `pmm_reserve` / `pmm_alloc_order` / `pmm_free_order`，以及 `pmm_alloc()` / `pmm_free(pa)` 宏；另有 `pmm_inject_oom(n)`——**ktest 专用故障注入**（T-004），让接下来 n 次分配像空 buddy 一样失败，好在内存充裕的机器上确定性地测 OOM 分支。

**只用一个 E820 区域**（`:95-109` 取最大的那个，且每个区域先按 1 GiB 窗口裁剪），
多段内存布局下会浪费；裁剪后若一个区域都不剩，`pmm_init` 直接 `panic` 而不是继续跑
（C69 + 2026-10-06 的守卫）。

#### 4.2 `kernel/mm/kheap.c`（141 行）— 内核堆

**职责**：`kmalloc` / `kfree`。

**设计**：**分离空闲链（segregated free list）**，`NBINS 9` 对应 16/32/64/…/4096 字节（`bin_of(n)` 从 bin 4 起算）。

```
n + 8 ≤ 4096  →  从 9 个 bin 之一取
                bin 空时从 buddy 拿 1 页，切成 PAGE_SIZE >> (b+4) 个块
n + 8 > 4096  →  直接 pmm_alloc_order(order)，header 记 0x80|order
```

**关键点**：
- 每个分配有 8 字节 `struct hdr { u32 bin_or_order; u32 magic; }`，`magic = 0xECEC0001`，`kfree` 校验
- 大块（>4096）的 header 写在**物理页首**，释放时 `VIRT_TO_PHYS` 转回
- **OOM 返回 `NULL`**（T-004，2026-10-03）：19 处调用点全部核对过，原先缺失的 7 处补了检查。唯一的例外是 `kmalloc_or_panic()`——给没有错误分支的 bring-up 路径用，当前唯一调用者是 C++ `operator new`。OOME 还会连带暴露 vmm/signal 的同款 panic，那些一并修在 T-004 里
- ⚠️ 堆区域本身是从内存顶部**预留**的 16 MiB（`pmm_init` 调 `kheap_place`），映射在 `KHEAP_VMA`（`PD_HI[256..271]`，2 MiB 页）

#### 4.3 `kernel/mm/vmm.c`（409 行）— 虚拟内存 / 页表

**全项目最重要的文件。** 职责：建 master 页表、管理每进程用户地址空间、地址翻译。

**三槽地址空间模型**（文件头注释就是最好的文档）：

| PML4 槽 | 用途 | 共享性 | 谁建立 |
|---|---|---|---|
| **0** | 低内存恒等映射（1 GiB，2 MiB 页） | 全部 AS 共享，仅内核 | `entry64.S:180` |
| **255** | 每进程用户空间 `[0x7f8000000000, 0x7ffffffff000]` | **每进程独立** | `vmm_map_user` |
| **511** | 高半别名（内核镜像/堆/VGA/设备/LFB） | 全部 AS 共享 | `entry64.S:181` |

⚠️ **槽 0 / 511 都只铺了 1 GiB**（`PDPT_M[0]`、`PDPT_HI[510]`），这就是
`PMM_WINDOW_TOP = 0x40000000` 的由来——**物理内存管理窗口不能超过恒等映射的范围**，
否则 `pmm_init` 播种时按恒等地址解引用就会 #PF，而在 IDT 建好之前 = 三重故障（见 §22.6）。

**核心机制**：
```c
u64 vmm_new_user_aspace(void) {
    u64 root = pmm_alloc();
    if (!root) return 0;               // ← 没帧就失败，不 panic（T-004）
    for (int i = 0; i < 512; i++)
        if (i != USER_SLOT)              // ← 除 255 外全部复制
            ptable_ptr(root)[i] = ptable_ptr(pml4_phys)[i];
    return root;
}
```
⇒ **内核映射对新进程零成本传播，不需要 refcount、不需要 TLB shootdown 计数**。这是这个设计最大的价值。

**关键函数**：

| 函数 | 职责 | 备注 |
|---|---|---|
| `vmm_init` | 填 `PD_HI`：把 buddy/堆范围内的每个 2 MiB 槽恒等映射，**上界 `PMM_WINDOW_TOP`**（`vmm.c:237`，注释在 `:234-236` 说明为何不是 3 GiB：超过 1 GiB 后 `(pa>>21) & 511` 会绕回槽 0） | 跳过 96/97/112/128 和 LFB 窗口 |
| `vmm_new_user_aspace` | 建新 PML4（复制除 255 外全部） | 没帧返回 0（`sys_fork` 走清理分支） |
| `vmm_destroy_user_aspace` | 递归释放 255 槽的 PDPT/PD/PT + 数据页 + PML4 本体 | 跳过槽 0 和 511 |
| `vmm_map_user` | 在 slot 255 里建四级映射 | 手工做 4 次 `ensure_table`；页表建不出来回 `LNXRM_ENOMEM`（T-004，`brk` 每涨 2 MiB 要一张新 PT） |
| `vmm_unmap_user` | 拆映射，返回被释放的物理地址 | 处理 2 MiB 大页情况。**只清 PTE，不回收空页表**（文件注释说"空中间表一并释放"是过期的），也不动数据页 —— 见下方 ⚠️ |
| `vmm_count_user_pages` | 数 255 槽里 present 的叶子页 | **T-032 记账的唯一真相**：exec/fork/spawn/brk 三处都按它刷新 `task.mem_pages`，`ps` 打印的就是这个数。只读行走，`root == 0` 或槽空返回 0 |
| `vmm_translate_in` | VA → PA | **纯四级 PTE 行走**。~~`ALIAS_BASE` 快捷路径~~ 已于 2026-10-06 删除：`ALIAS_BASE + 0x100000000UL` 在 u64 下溢出成 `0x80000000`，那条路一直是死代码；即便不溢出，高半窗口也不是 1:1 内存镜像（KHEAP/DEV/FB 都是另映射的），对它们算 `va - ALIAS_BASE` 本来就是错的 |
| `dup_user_aspace` | **深拷贝** 255 槽（`fork` 用） | 逐页 `pmm_alloc` + `memcpy` |
| `vmm_map_kernel_page` | 把一个 4 KiB 物理页映射进 `DEV_VMA`（PD slot 128） | 假定调用者只碰新槽位 |
| `kheap_place` | 从内存顶部向下预留堆区域 | **`pmm_init` 之前调用** |

⚠️ 两个已知薄弱点（详见 GAP_ANALYSIS C59）：`vmm_map_kernel_page` 不拆已有 2 MiB 大页；`invlpg` 打在当前 AS 而非目标 AS。

⚠️ **`vmm_destroy_user_aspace()` 会 `pmm_free` 所有仍 present 的数据页**
（`ktest/t_mm.c` 为这条付过一次学费）：调用方若想自己释放某个已映射的帧，
必须**先 `vmm_unmap_user` 把 PTE 清掉、再 `pmm_free`**，否则同一个帧会被还给 buddy
两次 —— 空闲链表被毒化之后，下一次 `pmm_alloc()` 就是 `#GP pmm_alloc+0x4b`。
`test_vmm_count_user_pages` 的收尾注释守着这条。

#### 4.4 `kernel/mm/SlabAllocator.cpp`（19 行）

**职责**：提供全局 `operator new` / `operator delete`。

**为什么需要**：`-nostdinc++` 的 freestanding C++ 构建里没有 libc++，内核里每一个 `new` 都需要自己提供。这里把它们路由到 `kmalloc_or_panic`/`kfree`——`new` 没有异常可抛、契约又要求非 NULL，所以它是全内核唯一保留 OOM panic 的分配入口（T-004）。

**这是 19 行但不可删的文件** —— `ahci.cpp` 和 `smp.cpp` 里的 C++ 代码依赖它。

---

### 5. 进程与调度（`kernel/task.c` + `kernel/sched.c` + `kernel/signal.c`，1,427 行）

#### 5.1 三个文件的分工

| 文件 | 职责 |
|---|---|
| `task.c` | pid 分配、fork/execve/exit/waitpid、用户内存读写helpers、**第一个进程的诞生** |
| `sched.c` | 任务表、每 CPU 运行队列、抢占调度、idle、上下文切换驱动 |
| `signal.c` | 信号投递、默认处置（终止 / stop / continue） |

#### 5.2 `kernel/sched.c`（516 行）— 调度器

**队列拓扑**（文件头注释）：

```
cpu_info.runq_head  ← 哨兵，push-front 链表
     ↓
每 CPU 一个队列，MAX_CPUS=8
一把全局 runq_lock（总是 irqsave）串行化所有入队/出队/选择
idle 任务永不在任何队列上，只在 pick_next() 返回 NULL 时作为兜底
```

**任务表**：`static struct task task_table[NR_TASKS]`（64 槽静态数组，**零动态分配**）。
- 槽 0 = BSP idle
- `task_alloc_slot` / `task_free_slot` 用 `task_table_lock` 保护；alloc 时清 `pid=0`，
  两端都把 `mem_pages`/`mem_peak` 归零（T-032）——**槽位复用不继承上一个占用者的读数**
- `find_task(&pid)` 在 `task_table_lock`（irqsave）**锁内**扫描（2026-10-01 修，C9）
- `task_iter(&i)` 是线性扫描，**有意保持无锁**——返回表内裸指针，调用方
  （`sys_waitpid`/`ps_dump`/`sys_kill`）在锁外使用；单线程进程模型下没有并发写者
  （论证见 `GAP_ANALYSIS.md` §16.1）

**调度参数**：`HZ 100`（`include/sys/cpu.h`），时间量 6 tick = 60 ms。

**关键函数**：

| 函数 | 职责 |
|---|---|
| `sched_init` | 清表 + 建 BSP idle（`pid=0`），设 GS 基址 |
| `sched_create_idle(cpu_id)` | 给 AP 建 idle（`ap0`/`ap1`/…） |
| `schedule()` | 核心：选 next → 重排 current → `switch_to`。current 用 **`runqueue_add_local()`**（只回本核队列——放到远端队列会被对方在 `swtch` 完成前抢走，双跑竞态） |
| `runqueue_add(t)` | 策略入队：选 `preferred_cpu`，本地目标则顺手置本核 `need_resched`（睡眠/新建等**不在任何核上执行**的任务走这里） |
| `runqueue_add_local(t)` | 本核队列入队（`schedule()` 专用） |
| `sched_tick()` | 时间量递减 + **全局唤醒** sleepers（每 CPU 的 tick 都会跑） |
| `sched_maybe_preempt(f)` | 在返回用户态前检查 `need_resched` |
| `switch_to(next)` | 切 CR3（若需要）、设 TSS.rsp0、更新 per-cpu `_current`、`swtch()` |
| `idle_loop()` | `for(;;) { schedule(); sti; hlt; }` |
| `preferred_cpu()` | 选目标 CPU：自己空闲 → 空闲 AP（先于 BSP，保 BSP 处理设备中断）→ BSP → `runq_len` 最短的队列 |
| `runq_len(cpu)` | 目标队列长度（`preferred_cpu` 的最短队列依据） |

**调度策略真相**：
- ⚠️ **实为 LIFO**（`runqueue_add` 往头部插，`struct cpu_info` 无尾指针），注释说的 "circular" 不成立
- 只有 1 个时间量（6 tick），无优先级、无公平性保证、无亲和性
- **有负载均衡**（2026-10-01）：新任务按"空闲优先、最短队列兜底"落位；**无任务迁移**——已入队的任务不会被挪走

**`switch_to` 的 SMP 正确性关键**（`:300`）：
```c
if (prev != next && prev && prev->pml4 != next->pml4) vmm_switch_to(next->pml4);
```
只有 pml4 不同才切 CR3 —— 因为槽 0/511 是共享的，大部分切换不需要动 CR3。**并且 idle 任务的 pml4 被显式设为 `master_pml4_phys()`**（`sched.c:139,184`），否则第一次切到 idle 会因为 CR3=0 而三重故障。

#### 5.3 `kernel/task.c`（608 行）— 进程管理

**pid 分配**：`next_pid()` 用 `pid_lock` + `__sync_fetch_and_add` 语义，pid 1 保留给 init。

**用户内存访问helpers**（安全边界的关键）：

| 函数 | 语义 |
|---|---|
| `user_ptr_ok(p,n)` | `vmm_is_user_range` + **逐页 `vmm_translate_in` 确认已映射** |
| `copy_from_user` | 先 `user_ptr_ok` 再 memcpy |
| `copy_to_user` | 同上 |
| `copy_user_str(dst,src,max)` | **逐字节**校验并复制（防跨未映射页） |

**`sys_fork`**（`:43-96`）：
1. 分配槽 + 内核栈（32 KiB）+ pid
2. `vmm_new_user_aspace()` 建新 PML4
3. `dup_user_aspace()` 深拷贝用户地址空间
4. 克隆 fd 表（`memcpy` + 逐个 refcnt++）
5. 复制 brk 边界
6. **伪造子进程的 `intr_frame`**（复制父的，rax=0，开 IF）
7. 在 frame 下方 56 字节构造 `cpu_ctx`，`area[6] = glue_first` 作为"入口地址"
8. 入队

**`sys_execve`**（`:98-214`）：
1. `copy_user_str` 取路径（128 字节上限），逐个取 argv（最多 16 个 × 64 字节）
2. `vfs_open_file` + 读整个文件到 `kmalloc` 的缓冲
3. `vmm_new_user_aspace` + 映射 16 KiB 用户栈（4 页）
4. **`vmm_switch_to(new_pml4)` 之后才 `elf_load`**（因为 `elf_load` 用 `memcpy((void*)p_vaddr,...)` 往用户地址写）
5. 失败则**切回旧 PML4 并回滚**（`:171-173`，注释引用了历史 bug）
6. 成功则先赋 `current->pml4 = new_pml4` **再**销毁旧的（顺序反了会出 stale CR3）
7. 关闭 `O_CLOEXEC` fd，清空 `signal_pending`
8. 在用户栈上按 SysV ABI 摆 `argc/argv/envp/auxv`

**`sys_exit`**（`:325-365`）：关所有 fd → `console_waiter_disarm` → **`fb_disown(current->pid)`**（T-033：画屏归属的**唯一**释放点，所有死法都经过这里）→ 孤儿重挂载到 `find_task(1)` → 给父发 `SIGCHLD` → `T_ZOMBIE` → 从队列摘除 → 唤醒睡眠的父 → `schedule()`。

**内存记账的生命周期（T-032）** —— 卡片上写的是"exit 时释放全部记账"，落地时
按**地址空间**的真实生命周期改了语义：

| 时机 | 动作 |
|---|---|
| `sys_execve` 装好新地址空间后 | `mem_pages = vmm_count_user_pages(pml4)`，`mem_peak = mem_pages`（换了镜像，峰值重置） |
| `kernel_spawn`（首个进程） | 同上 |
| `sys_fork` 深拷贝子地址空间后 | 子进程 `mem_pages = vmm_count_user_pages(ch->pml4)`、`mem_peak = mem_pages` —— **父子各记各的**，父的读数不因子进程撞配额而动 |
| `sys_brk` 成功后 | `mem_pages = have ± delta`，并按需抬 `mem_peak`（`kernel/syscall.c`） |
| `waitpid` 真正回收槽位时（`task_free_slot`） | 归零 |
| 下一次 `task_alloc_slot` | 再复位一次（不继承上一个占用者） |

**为什么不是 `exit`**：`vmm_destroy_user_aspace` 在**reap** 才跑（`exit` 只是把任务
变成 zombie），zombie 在被收走之前仍然占着页。所以 `ps` 会显示 zombie 的**最终**
大小 —— `usr/systest.c` 的 T-032 段正是靠轮询 `LNXRM_TS_ZOMBIE` 来读这个终值。

**`kernel_spawn`**（`:309-415`）—— 第一个用户进程：
和 `execve` 类似但更简化，且结尾 **`vmm_switch_to(master_pml4_phys())` 切回内核 PML4**（`:399`）—— 因为它是在内核上下文跑的，不能留在用户 AS 里。

#### 5.4 `kernel/signal.c`（263 行）— 信号

**三件事：默认处置、用户态 handler 投递、`sigreturn` 恢复。** 这条链路在
ABI 里是完整的：`sigaction(16)` / `sigprocmask(17)` / `sigreturn(20)`，
`systest` 实测 handler 捕获 SIGSEGV 成功（`GAP_ANALYSIS.md` §12.1）。per-task 状态
在 `struct task`：`sig_handlers[32]`（NULL=默认、`(void*)1`=忽略、其余=用户
handler 地址）、`sig_masks[32]`（每个信号的 `sa_mask`）、`sig_blocked`、
`signal_pending`、`sig_in_handler`、`saved_tf`。

**`send_signal(t, sig)`**：
- 入口守卫：`sig` 范围校验 + **`t->state == T_UNUSED` 直接返回**（查表解锁后
  槽位可能已被 `waitpid` 释放/复用，2026-10-01 加，C9 相关）
- `SIGKILL` / `SIGSTOP` 硬置位并唤醒 `T_SLEEPING`（不可屏蔽）
- `SIGCONT` 额外唤醒 `T_STOPPED`（STOPPED 不在任何运行队列上，通用唤醒路径抓不到）
- 其余信号置位；目标在睡就置 `T_RUNNABLE` 入队

**`do_signal_check(t)`** —— 三个调用点都在 `iretq` 之前且有 ring-3 守卫
（`f->cs & 3`）：`syscall.c` 返回路径、`isr.c` IRQ 返回路径、`isr.c` 异常路径
（后者供 SIGSEGV 这类故障信号重定向）：
```
1. pid == 0（idle）或 pending == 0 → 直接返回
2. SIGSTOP → 清位，state = T_STOPPED，schedule() 原地停住
3. SIGCONT → 清位，若 T_STOPPED 则放回运行队列
4. 遍历 pending 中未 blocked 的信号：
   ├─ SIG_IGN / 默认忽略        → 清位丢弃
   ├─ 用户 handler              → deliver_handler()（一次只投一帧，
   │                              handler 运行期间的第二信号保持 pending）
   └─ 默认终止（INT/TERM/QUIT/ILL/ABRT/SEGV/KILL）→ sys_exit(-sig)
```

**`deliver_handler`**：把被打断的帧存进 `saved_tf`，向用户栈写入 9 字节
trampoline 机器码（`mov eax,20; int 0x80; ud2`，`SYS_sigreturn=20`），
伪造一个 `call` 帧（返回地址指向 trampoline、`rsp%16==8` 符合 SysV ABI），
然后改 `f->rip/rdi` 跳进 handler。**用户二进制不需要自带 restorer**；代价是
依赖用户页无 NX（栈可执行，见 GAP 4.2）。栈放不下帧时按栈溢出同款
`sys_exit(-SIGSEGV)` 收场。

**`sys_sigaction`**：KILL/STOP 不可捕获/忽略；handler 地址必须是哨兵值
或 `user_ptr_ok` 校验过的用户页（否则下次信号 `iretq` 进虚空）；`sa_mask`
屏蔽掉 KILL/STOP 位后存 per-signal 槽。
**`sys_sigprocmask`**：`BLOCK/UNBLOCK/SETMASK`，KILL/STOP 恒可投递。
**`sys_sigreturn`**：仅 `sig_in_handler` 时有效；把 `saved_tf` 拷回 `tf`
（其 `rax` 作为 syscall 返回值，因为 `syscall_entry` 随后会覆写 `f->rax`）、
恢复 `sig_blocked`。**已知缺口**：恢复帧不做 CS/SS 再校验、嵌套限制只靠
`sig_in_handler` 单标志（见 GAP 信号行）。

---

### 6. 系统调用与 ABI

#### 6.1 `include/abi/lnxrm_syscalls.def` — 编号表

X-macro 清单，`lnxrm_abi.h` 包含它来生成 `SYS_*` 枚举：
```
 0 read       1 write       2 open        3 close      4 lseek       5 brk
 6 getdent    7 dup2        8 nanosleep   9 getpid    10 fork       11 execve
12 exit      13 wait4      14 kill       15 uname     16 sigaction  17 sigprocmask
18 getppid   19 ps         20 sigreturn  21 diskinfo 22 mkdir
23 fb_info   24 fb_clear   25 fb_fill    26 fb_char  27 fb_puts   28 unlink
29 rmdir     30 rename    31 setuid
```

**32 个编号 0–31 连续无空洞，全部有实现。**

编号做过一次**紧凑重排**：原先表里给退役调用留了 3 个洞（21 `getcpu` /
31 `getkey` / 33 `move`），现在整张表被压成 0..30，那些洞不存在了，
`diskinfo` 拿到 21、`rename` 拿到 30。此后**只追加不重排**：`setuid` 于 2026-10-03
追加为 31（T-031 的降权路径）。凡是 `> LNXRM_SYSCALL_LAST` 的编号
（32 及以上）落进 `syscall_do` 的 `default` 分支：打印一行
`[sys] unknown syscall` 并返回 `-38`。

23–27 的 `fb_info`/`fb_clear`/`fb_fill`/`fb_char`/`fb_puts` 曾经停用过一轮，现已恢复；
随紧凑重排它们从 24–28 平移到 23–27，`SYS_*` 与 `kfb_*` 仍然一一对应。

`LNXRM_SYSCALL_LAST 31` 仍是硬编码，但 `lnxrm_abi.h` 用一个
`1 / (nr <= LAST)` 的常量枚举给每个编号做编译期校验：往 `.def` 里加一个
超过 31 的编号却忘了改宏，**编译直接失败**，而不是悄悄放过。

#### 6.2 `include/abi/lnxrm_abi.h` — 共享 ABI 头

**内核和用户态共同包含**，是所有跨态结构体的唯一权威：
- `enum lnxrm_errno` —— 21 个 `LNXRM_E*` 错误码（含 `LNXRM_EINTR` / `LNXRM_EAGAIN` /
  `LNXRM_ENAMETOOLONG`：路径放不进 `abs_path` 的暂存缓冲时由 VFS 回给调用方，见 C46）
- `enum lnxrm_syscall_number` —— 由 `.def` 生成的 32 个 `SYS_*` 编号，外加
  `LNXRM_SYSCALL_FIRST 0` / `LNXRM_SYSCALL_LAST 31` 和逐编号的编译期范围校验
- **信号号**：`LNXRM_SIG*` 编号表 + 代码实际用到的无前缀别名（`SIGINT`、`SIGTERM`…）、
  `LNXRM_NR_SIGNALS 32`、`SIG_DFL`/`SIG_IGN`、`LNXRM_SIG_BLOCK/UNBLOCK/SETMASK`
  —— `include/sys/sched.h` 不再自己维护一份，两边靠同一个头对齐（sched.h 里留了
  `SIGKILL==9 && SIGSTOP==19 && SIGCONT==18` 的 `_Static_assert` 兜底）
- `struct lnxrm_sigaction`（24 B）— `sigaction` 的进出参数
- `struct lnxrm_timespec`（16 B）— `nanosleep` 的剩余时间输出
- `struct lnxrm_utsname`（320 B）— `uname`
- `struct lnxrm_ps_entry`（**48 B**：T-030 加的 `kind`/`uid`，T-032 加的
  `mem_kib`/`peak_kib`）+ `enum lnxrm_task_state` — `ps`。**结构体一变，
  用户态所有引用它的程序都必须重编**（`ps.c`、`systest.c`），否则按旧步长读
  会拿到错位的字段
- `struct lnxrm_dirent`（72 B，含真正填充的 `d_ino`）+ 3 个 `_Static_assert`
- `LNXRM_SEEK_SET/CUR/END`
- `LNXRM_STATIC_ASSERT` 宏（C/C++ 双分支）

#### 6.3 `kernel/syscall.c`（448 行）— 分发

**调用约定**：向量 `int 0x80`，`rax`=编号，`rdi`/`rsi`/`r10`=参数，返回值在 `rax`。

**入口 stub**（`arch/entry64.S:415-424`）：
```asm
isr128:
    push QWORD 0          ; 合成 err 槽
    push QWORD 128        ; 向量号
    PUSHALL               ; 15 个通用寄存器
    mov rdi, rsp          ; intr_frame* 传给 C
    call syscall_entry
    POPALL
    add rsp, 16
    iretq
```
IDT 里 128 号门的 DPL=3（`arch/cpu.c:113`），所以 ring-3 可以直接调用。

**`syscall_do`（`:82-186`）就是整张表** —— 一个大 `switch`，注释说得很清楚：
> "Read the syscall arguments out of the trap frame and run fn. This is the whole dispatch table: register ABI plumbing lives in syscall_entry(), everything else is a plain C call."

`syscall_entry`（`:188-199`）：存 `current->tf = f` → 调分发 → 写回 `rax` → 置 IF →
**返回用户态前处理待决信号**（`do_signal_check`，此时返回值已在 `rax` 里）。信号既可能
stop/continue/kill 进程，也可能**改写这个帧**把 `rip` 指向用户装的 handler（见 §5.4）。

switch 与 `.def` 一一对应：32 个编号（0–31）各有真实实现，`default` 收下
≥32 的未知编号，打印 `[sys] unknown syscall` 后返回 `LNXRM_ENOSYS`。

**权限检查点（T-031，2026-10-03）**：分发表里 `ps` / `diskinfo` 走
`cap_gate(CAP_SYS_ADMIN, ...)`，`fb_clear/fill/char/puts` 走 `cap_gate(CAP_FB, ...)`
（`fb_info` 只读、不设门），`kill` 在 `kill_allowed()` 里过 kxld 规则 + 同 uid /
`CAP_KILL`、`pid == -1` 广播另要 `CAP_KILL`；拒绝一律回执一行
`[cap] denied <call>: pid .. lacks CAP_*`。

**归属门（T-033，2026-10-05）——同一批调用的第二道门**。`cap_gate` 只回答
"够不够格画"，回答不了"够格的里哪一个正在屏上"。所以 4 个绘图 case 在能力门
**之后**各多一行 `fb_owner_gate(what)`（定义紧跟 `cap_gate`）：

```c
static int fb_owner_gate(const char *what)
{
    u32 me = current->pid;
    if (fb_claim(me)) return 0;                    /* 屏空闲，或者我已经持有 */
    kprintf("[fb] denied %s: pid %u holds the surface, pid %u draws\n",
            what, fb_owner_get(), me);
    return LNXRM_EACCES;                           /* -13 */
}
```

- **顺序是硬的**：先能力后归属。倒过来普通用户会听到"你不是 owner"，
  而它连碰都不该碰——`user.fb.clear=-13` 必须还是"你没有 `CAP_FB`"这句完整的话；
  归属与能力都回 `-13`，**由两行不同回执区分**（不为此新增 `EBUSY`/`EPERM`）
- `sys_fb_info` **两道门都不过**：只读几何，且不学尺寸就没资格选画哪儿
- `fb_owner` 这个状态放在 `framebuffer.c` 而不是 `task.c`：*"这块屏归谁"是驱动的
  事实，不是任务的属性*；释放只在 `sys_exit` 一处（§11.2）
- ABI 表**没有**为此加系统调用：编号 0–31 连续封闭，`systest.c` 的
  `past_table[] = {32,33,34}` 断言 32+ 返回 `-38`，归属只在既有入口后多一道门

**`sys_brk` 的配额门（T-032，2026-10-05）**：不是 `cap_gate`，而是算术 ——
```c
u64 have  = vmm_count_user_pages(current->pml4);   /* 页表行走，不看 mem_pages */
u64 quota = cred_mem_quota(&current->cred);        /* 0 = 无上限（root/kxld） */
if (quota && have + (tgt - cur) / PAGE_SIZE > quota) return LNXRM_ENOMEM;
```
- 检查在**取帧之前**；**shrink 永不拒绝**（`tgt <= cur` 走另一支）
- 配额覆盖**整个用户地址空间**（image+栈+restorer+堆），与 `RLIMIT_AS` 同语义
- 配额唯一来源是 `include/sys/cred.h` 的 `cred_mem_quota()`：user / `NULL` cred
  返回 `CRED_USER_MEM_QUOTA_PAGES`（64 MiB，**fail-closed**），`kind >= CRED_ROOT`
  返回 0 = 无上限。**没有 per-task 配额字段** —— `SYS_setuid` 会改 tier，两套记账
  必然打架
- 成功路径顺手把 `mem_pages` 刷成 `have ± delta`、抬 `mem_peak`；T-004 的局部
  回卷（失败即归还本次已映射的页）原样保留

**辅助模式**：两个小 helper 把"取用户路径"这件事集中化
- `path_syscall(upath, fn)` — 单路径（mkdir/unlink/rmdir）
- `path2_syscall(u1, u2, fn)` — 双路径（rename/move）

⚠️ 所有走字符串的调用都经 `copy_user_str` 校验；走指针的由调用方（`vfs.c` 的
`copy_from_user`/`copy_to_user`、`task.c` 的 `UCP`）自己校验。**没有统一的
前置检查**——加新调用时要自己记得验，漏了就是内核任意读写。

---

### 7. 中断与 SMP

#### 7.1 `arch/entry64.S`（527 行）— 汇编核心

一个文件承担四件事：

| 段 | 行 | 内容 |
|---|---|---|
| `_start32` | 45–137 | GRUB 的 32 位入口（见 §3.2） |
| `_start64` | 160–289 | 64 位入口，建 master 页表、清 bss、进 C |
| 中断桩 | 363–470 | `ISR_NOERR` / `ISR_ERR` 宏 + `isr0..isr47` + `isr128` + `isr240/241/242` |
| 上下文切换 | 477–508 | `swtch` / `glue_first` / `load_cr3` |

**中断桩的两个宏**：
```asm
%macro ISR_NOERR 1        ; CPU 不压错误码
    push QWORD 0           ; 合成 err 槽
    push QWORD %1          ; 向量号
%macro ISR_ERR 1          ; CPU 已压错误码
    push QWORD %1          ; 只压向量号
```

**`struct intr_frame` 布局**（`include/sys/sched.h:68-76`，**地址递增顺序**）：
```
r15 r14 r13 r12 r11 r10 r9 r8 rbp rdi rsi rdx rcx rax rbx
intno err rip cs rflags ussp usss
```
`rbx` 在 `rax` 之后是刻意的 —— 它是 SysV callee-saved，C 处理器必须能保住它。

**`swtch` 保存的 `struct cpu_ctx` 只有 7 个 u64**：
```
r15 r14 r13 r12 rbp rbx rip
```
"rip" 实际是 `ret` 用的返回地址。**`cpu_ctx` 只有一个 `sp` 字段**（`sched.h:80-82`），真正的寄存器区在任务内核栈上（`kernel_spawn`/`fork` 里的 `area[6] = glue_first`）。

**`glue_first`**：新任务第一次被调度的入口 —— `swtch` 的 `ret` 落在 `intr_frame` 上，于是 `POPALL; add rsp,16; iretq` 直接进用户态。

**`idt_init` 的坑**（`arch/cpu.c:112`）：向量 49–255 全指向 `isr19` 作 catch-all，所以 IPI 必须显式覆盖（`:115-117`）。注释记了这个历史 bug：
> "IPI vectors 0xF0..0xF2 — must push the real vector number, not 19. (idt_init used to alias 49..255 onto isr19, so a reschedule IPI was delivered as #XM and panicked the idle CPU.)"

#### 7.2 `arch/cpu.c`（342 行）— CPU 状态

| 职责 | 关键点 |
|---|---|
| **GDT + TSS** | `cpu_gdt_tab[MAX_CPUS]`，**每 CPU 一套**。注释解释了为什么：共享 TSS 会让 AP 的 ring0→ring3 返回落到 BSP 的栈上（`:26-28`） |
| 选择子 | 0x08 KCODE / 0x10 KDATA / 0x18 UCODE(DPL=3) / 0x20 UDATA(DPL=3) / 0x28 TSS |
| `tss_set_rsp0` | 按 `this_cpu_data()->id` 索引，切换任务时更新 |
| **IDT** | 256 项。0–47 → `isr_stub_table[v]`；48(=0x30)→ 同表但 DPL=3；49–255 → `isr19` catch-all；128 → `isr128` DPL=3；0xF0–0xF2 → 真实 IPI stub |
| IRQ 表 | `handlers[16]`，`irq_install` / `irq_get_handler` |
| IPI 表 | `ipi_handlers[3]`，`ipi_handler_install` / `ipi_dispatch` |
| **PIT** | `pit_init` 编程为**模式 2**（`0x34`，速率发生器）+ 注册 IRQ0。**PIT 不驱动时钟**（PIT→IOAPIC IRQ0 在此平台不置 LAPIC IRR），只给标定提供时间窗。⚠️ 曾用模式 3（`0x36`），QEMU 下窗口只有半周期 → jiffies 跑 200 Hz（2026-10-01 修复，`GAP_ANALYSIS.md` §16.2） |
| **LAPIC 定时器标定** | `lapic_timer_calibrate()`（BSP，`smp_init()` 前调一次）：把 CCR 设为 0xFFFFFFFF，对齐到 PIT 重装边沿，测窗口内 CCR 差值当 `s_lapic_period`，顺带用 `rdtsc` 测 `s_tsc_per_ms`；`lapic_timer_start` 对任意 CPU 用缓存 period 武装周期 tick（非 BSP 打印带 ` (AP)` 后缀） |
| **tick 与 jiffies** | `pit_handler`（向量 32）：**仅 BSP 递增 `jiffies`**，每个 CPU 都跑 `sched_tick`/`sched_maybe_preempt`；`mdelay(ms)` 用 `s_tsc_per_ms` 实测换算（原 2 GHz 硬编码已删） |
| **MSR/TSC** | `rdmsr` / `wrmsr` / `rdtsc` |
| `cpu_init` | GDT → **重设 GS 基址**（因为 `gdt_reload` 的 `mov gs,ax` 在 QEMU TCG 下会清掉隐藏基址，`:231-233`）→ IDT → APIC → IOAPIC → 清 EM/TS，置 MP/NE/OSFXSR/OSXMMEXCPT |

⚠️ **NXE / CR0.WP / CR4.WP / SMEP / SMAP 全部未置位**（见 GAP_ANALYSIS 4.2）。

#### 7.3 `kernel/isr.c`（210 行）— 中断分发

**只有一条输入路径**：

| 队列 | 大小 | 消费者 | 特点 |
|---|---|---|---|
| `inbuf` | 256 字节 | `console_read`（shell 键盘输入） | ASCII 转换后 |
| `con_waiter` | **单槽** | `console_read` | ⚠️ 第二个等待者会覆盖第一个（GAP_ANALYSIS C5） |

原始键事件队列 `keyq` / `key_waiter` 与 `SYS_getkey` 一起删除了：键盘事件现在
**只有** `inbuf` 这一条路，没有 ASCII 映射的键（方向键、F 键）直接被丢弃。

**键盘**（`kbd_process_sc`，`:40`）：scancode set 1 → ASCII，两张 `kmap[128]`/`kmap_shift[128]` 表，Shift/CapsLock/E0 前缀处理，然后 `console_push()` 进 `inbuf`。

**`isr_common`（`:138-210`）的三段分流**：
```
向量 0xF0..0xF2  → ipi_dispatch() + apic_eoi() + 必要时 sched_maybe_preempt
向量 < 32        → 异常：打印 + ring-3 则 sys_exit(-sig) / ring-0 则 panic
向量 >= 32       → IRQ：先 apic_eoi()（必须在 handler 前，见 :193-197 注释）
                   再 irq_get_handler(irq)(f)
最后             → 返回用户态前 do_signal_check(current)
```

**EOI 顺序的注释值得读**（`:193-197`）：
> "EOI BEFORE the handler: handlers such as the PIT tick may call schedule() and switch away; a delayed EOI would leave the LAPIC ISR bit set and stall this vector until the interrupted task resumes (losing ticks / hanging idle's hlt)."

#### 7.4 `kernel/apic.c`（139 行）— LAPIC + IOAPIC

两者都 MMIO 映射到 `DEV_VMA` 窗口的两个相邻 4 KiB 页：
```
LAPIC  DEV_VMA + 0x0000   （物理地址从 MSR 0x1B 读）
IOAPIC DEV_VMA + 0x1000   （物理固定 0xFEC00000）
```
都带 `PG_PCD`（不可缓存）—— **比 AHCI 做对了**。

| API | 用途 |
|---|---|
| `apic_init` / `apic_enable` | 从 MSR 定位、映射、屏蔽 8259（`outb(0x21/0xA1, 0xFF)`）、使能、屏蔽除 LINT0/1 外的所有 LVT |
| `apic_send_ipi(dst, vec, delivery, level)` | 通用 IPI，等 send queue 空 |
| `apic_send_init_ipi` | INIT，level assert |
| `apic_send_sipi` | SIPI，vector 0x08 → 物理 `0x8000`（trampoline 起始） |
| `apic_eoi` | 写 EOI |
| `ioapic_set_irq/mask_irq/unmask_irq` | 重定向表项操作 |

**没有中断使能**（`P_IE`/`HBA_GHC`）：当前只用于 INIT-SIPI-SIPI 和 IPI，不接收设备中断。

#### 7.5 `kernel/smp.cpp`（241 行，C++）— AP 拉起

**流程**（`Manager::init`）：
```
1. copy_trampoline()
     ├─ memcpy(物理 0x8000, trampoline_start, len)     // arch/trampoline.S
     ├─ trampoline_clear()                               // Rust 侧清共享数据
     └─ setup_ap_gdt()  在物理 0x8800 建 4 段 GDT（供 trampoline 过渡）
2. 注册 IPI handler：0xF0 reschedule（设 need_resched）、0xF2 stop（cli;hlt）
3. 循环 try_id = 1 .. MAX_CPUS-1，连续失败 2 次就停
   ap_startup(apic_id, cpu_id, pml4):
     ├─ trampoline_setup(pml4, ap_stacks[cpu_id] 顶, cpu_id, gdt_packed,
     │                  ap_main 的【虚拟地址】)
     │   ← 注释强调：传 VA 而非 PA，因为 ap_main 的代码用高半 RIP 相对寻址访问全局
     ├─ apic_send_init_ipi + mdelay(10)
     ├─ apic_send_sipi(0x08) + mdelay(2)   ×最多 3 次
     └─ 轮询 trampoline_is_ready()，AP_STARTUP_TIMEOUT_MS 超时
```

**`ap_main(ap_id)`（见 `smp.cpp`）的顺序很讲究**：
```
1. c->started = false              ← 必须在建 idle 之前，preferred_cpu 会检查
2. gdt_init_cpu(ap_id)             ← 装本 CPU 的 GDT
3. cpu_set_gs_base(&cpu_table[ap_id])
                                    ← 必须在 GDT 之后！注释解释：QEMU TCG 下
                                      gdt_reload 的 mov gs,ax 会清掉隐藏基址
4. idt_init()                      ← 本 CPU 独立 IDT
5. 修 CR0（清 EM/TS，置 MP/NE）、CR4（置 OSFXSR/OSXMMEXCPT）
6. 建 runq_head 环                 （原 spin_init(task_lock) 已随死字段删除）
7. apic_enable() + LAPIC_TPR=0
8. lapic_timer_start(32, HZ)       ← AP 本地周期 tick：用 BSP 在 main.c 缓存的
                                     period，不碰 PIT；jiffies 仍归 BSP（cpu.c）
9. 屏蔽 8259（outb 0xFF）
10. sched_create_idle() → c->idle = idle; c->_current = idle
11. c->started = true
12. trampoline_signal_ready()      ← BSP 的 ap_startup 轮询的就是这个
13. for(;;) { schedule(); sti; hlt; }
```

⚠️ `mdelay` 已移至 `arch/cpu.c`（`smp.cpp` 原先硬编码 ~2 GHz TSC 假设的本地
版本已删除），用标定得到的 `s_tsc_per_ms` 换算（`GAP_ANALYSIS.md` §16.1）。

#### 7.6 `arch/trampoline.S`（105 行）

AP 侧的极简入口。用 **VGA 段寄存器技巧**在 16 位实模式下拿到运行地址：

```asm
    mov ax, 0x1000          ; 数据段描述符：base=0, limit=0xFFFF, DPL=0
    mov ds, ax
    mov es, ax
    mov [ds:0x10], eax      ; ds:0x10 → 线性 0x10 ← eax(物理地址)
    mov [ds:0x12], ax       ; ds:0x12 → 线性 0x00 ← 0（高 16 位）
    lgdt [ds:0x0E]
```

共享数据在物理 `0x9000`，字段偏移 `0x00/0x08/0x10/0x18/0x20/0x28`（与 `kernel/rust/smp.rs:4-10` 的 `TrampolineData` 精确对应）。

---

### 8. 同步原语

#### 8.1 `include/sys/spinlock.h` + `kernel/spinlock.c`（30 + 21 行）

```c
typedef struct { volatile u32 locked; u32 pad; } spinlock_t;
#define SPINLOCK_INIT {0, 0}
```

**两个函数**：`spin_lock_irqsave` / `spin_unlock_irqrestore`（关中断版）。裸的 `spin_lock` / `spin_unlock` / `spin_init` 在树内**零调用**，2026-10-01 随死代码清理一并删除（初始化走 `SPINLOCK_INIT`）；另有零引用的 `pmm_total_bytes` 同日删除。

实现用 GCC 内建：`__sync_lock_test_and_set` + `__sync_synchronize()` 全屏障。

⚠️ 历史版本的头文件注释要求"持锁期间必须关中断或用 irqsave 变体"，且实际调用点曾混用裸 `spin_lock`（中断上下文里会死锁）。裸版本删除后全部加锁点都是 irqsave，混用问题从类型上消除了。

**内核里一共 6 把锁**：

| 锁 | 位置 | 保护 | 备注 |
|---|---|---|---|
| `runq_lock` | `sched.c:26` | 所有 per-CPU 运行队列 | 总是 irqsave |
| `task_table_lock` | `sched.c:28` | 任务槽分配状态 | |
| `pmm_lock` | `pmm.c:28` | buddy 空闲链 + 位图 | |
| `kheap_lock` | `kheap.c:18` | 内核堆 bin | |
| `cache_lock` | `blk_cache.c:8` | 块缓存 | ⚠️ **跨越设备 I/O** |
| `fat_fs_lock` | `fat32_priv.h:20` | 全部 FAT32 操作 | ⚠️ 与 `cache_lock` 嵌套 |
| `pid_lock` | `task.c:20` | pid 计数器 | |
| `console_out_lock`（内部 `print_lock`） | `print.c` | `kvprintf` 与 `console_write` 的控制台输出 | irqsave；2026-10-01 起两者共用（C1 已修） |

#### 8.2 `kernel/spinlock.c` 之外的一个隐式锁

`fat_fs_lock` 的模式值得注意：**一个全局锁串行化整个文件系统**，所有 FAT32 操作都 `irqsave` 它。简单正确但完全无并发度 —— 这是双核下文件系统性能的根本瓶颈。

---

### 9. 存储与文件系统（`kernel/fs/`，2,768 行）

> 本节数字与描述已按 **T-007 / T-008（2026-10-05，块缓存 LRU 与读写路径）** 之后的代码同步
> （更早一轮是 T-005 的块设备 I/O 返回值）；重点是 9.1 末尾的**错误契约**与**路径不变式**，
> 以及 9.3 的 `blk_io_*` 单一咽喉、时间戳 LRU 与计数器。

#### 9.1 层次结构

```
kernel/fs/
├── vfs.c            720 行   VFS 抽象层：vnode / file / fs_ops / fd 表 / 路径解析 / 挂载
├── blk_cache.c      321 行   64 项时间戳 LRU 扇区缓存 + blk_io_*（唯一驱动调用点）+ 故障注入 + 计数/内省
├── mbr.c            155 行   MBR 分区表解析
└── fat32/
    ├── fat32.c      323 行   BPB 解析、挂载、FAT 链读写、空闲簇查找、限流告警
    ├── fat32_dir.c  258 行   目录扫描、LFN（长文件名）重建、readdir 游标
    ├── fat32_file.c 260 行   文件读写、簇链分配
    └── fat32_meta.c 745 行   目录项增删改、mkdir/rmdir/unlink/rename  ← 最大
```

#### 9.1.1 错误契约（T-005 之后，自下而上，每层只翻译、不吞）

| 层 | 位置 | 回答什么 |
|---|---|---|
| 驱动口 | `blk_cache.c:47,54` `blk_io_read`/`blk_io_write`；`:137,198` 缓存入口 | 驱动的非 0 原样上抛；注入的故障回 `LNXRM_EIO`。**全内核只有这里调用 `dev->read`/`dev->write`**（`mbr.c` 的分区层也改走它） |
| 扇区 | `fat32_priv.h:81,84` `fat_read_sector`/`fat_write_sector` | **只有 0 或 `LNXRM_EIO`**，不透传驱动私有码 |
| 扫描 | `fat_scan_dir` / `fat_resolve_path` / `fat_dir_iter` | 三态 `1 / 0 / 负`：「读不到」与「没找到」「扫到末尾」不再共用一个值 |
| 语义 | `fat_*_impl`、`fat_set_entry` | 负数一路向上传，直到 VFS |
| VFS | `vfs.c:195-207`（open/create）、`:473-491`（readdir） | `EIO` 不再被改写成 `ENOENT`/`EACCES`/`0`（"目录是空的"） |

出错时的对外表现：`[fat32] I/O error <op> LBA <n>`（`fat_warn_io`，限流 16 条，
`fat32.c:55`），然后操作返回 `LNXRM_EIO = -5`。

**路径不变式（T-008，2026-10-05）**：错误契约管"失败怎么上报"，这一条管"同一个扇区
不能被两条路各写各的"——只要每类 LBA 只走一条路，就不可能读到另一条路留下的陈旧副本：

| 区域 | 读 | 写 |
|---|---|---|
| 数据/目录（≥ `data_start_lba`） | `fat_read_sector`/`fat_read_cluster` → `blk_cache_read` | `fat_write_sector` → `blk_cache_write`（写回） |
| FAT 区 `[reserved, reserved + num_fats*fatsz)` | `m->fat`（挂载时整表读进内存，`fat32.c:310`） | `fat_set_entry` → `blk_io_write` **直写**（`fat32.c:114,126`） |
| 挂载期（BPB `fat32.c:247`、FAT 表 `:310`、MBR `mbr.c:42`） | `blk_io_*` 直读 | — |

FAT 区直写是**刻意的**：分配要先落盘，才能被指向它的目录项暂存；若改走缓存，两者都成了
暂存态，而 `blk_cache_flush` 按槽号回写，就会出现"目录项先于 FAT 落盘"的窗口。代价是必须
保证 **FAT 区永远不进缓存**（否则那份没人刷新的缓存副本就是 C26 本身）——
`ktest/t_cache.c:315` 的 `test_fat_region_is_never_cached` 逐个扇区查那 2018 个 FAT
LBA 都不在缓存里，谁把一次 FAT 扇区读改成走缓存，这条立刻红。

#### 9.2 `include/sys/vfs.h`（144 行）— VFS 接口

**对象**：

| 对象 | 内容 | 生命周期 |
|---|---|---|
| `struct vnode` | `{type, size, fs_data, ops, mnt_data}` | **每次 `open` 在栈上创建并拷贝**（不是持久 inode） |
| `struct file` | `{ops, pos, priv, is_dir, flags, refcnt}` | `kmalloc` 分配，refcount 管理 |
| `struct dir_iter` | `{node, ops, cookie, mnt_data, err}` | `V_DIR` 的 `file->priv`；`err` 记住这次扫描已因 I/O 错误终止（T-005），下次 `getdent` 继续报错而不是"到头了" |
| `struct fs_ops` | 10 个函数指针的 vtable | 每文件系统一份 |
| `struct file_ops` | 4 个函数指针（read/write/getdent/close） | VFS 层的薄封装。**没有 `lseek`** |
| `ata_total_sectors()`（`vfs.h:121`） | 从 IDENTIFY 数据块算容量（512 B 扇区数） | **T-080**：`static inline`，`ide.c` 与 `ahci.cpp` 共用。words 100-103 非 0 即权威；words 60-61 == `FFFFFFFFh`（ATA 的"改用 48 位"哨兵，也是 ≥2 TiB 时该 32 位字段的饱和值）→ 返回 **0**，调用方据此**拒绝 `blk_register`**。`num_sectors` 从此不可能等于 `0xFFFFFFFF` |

`file->priv` 按类型解释：`V_REG` → vnode 的堆拷贝；`V_DIR` → `struct dir_iter`；`V_CHR` → NULL。

**fd 表**：每任务 `struct file *fds[16]`（`include/sys/sched.h:90`）。

**路径解析**（`vfs_lookup`，`vfs.c:175-211`）：`fs_for_path` 扫挂载表（线性匹配前缀）→ `ops->lookup` → 失败则试 `dev_lookup` → 再失败按 `O_CREAT` 试 `ops->create` → 再按 `O_TRUNC` 试 `ops->write`。
三处失败各有各的出口（T-005）：lookup 的 `EIO` 直接上抛（不再折成 `ENOENT`）、
create 的 `EIO` 上抛（其余仍是 `EACCES`）、`O_TRUNC` 的截断没做成返回 `EIO` 而不是成功。

**readdir**：`dir_getdent`（`vfs.c:467`）循环调 `fs_ops->getdent`，按 `1=有条目 /
0=到头 / 负=出错` 三态处理；出错时先记进 `dir_iter.err`，本次若还没攒到条目就直接
返回错误码。

**设备节点**：`dev_ops` 只有一个 `.getdent`，只认识 `/dev` 和 `/dev/console`。

#### 9.3 `kernel/fs/blk_cache.c`（321 行）

64 项 × 512 字节 = 32 KB，`(dev, lba)` 为键，**全局一块**所有块设备共享。

| 函数 | 职责 |
|---|---|
| `blk_io_read` / `blk_io_write`（`:67,75`） | **全内核唯一**调用 `dev->read`/`dev->write` 的地方：先查故障注入，再计一次数（`:71,79`），再问驱动 |
| `blk_inject_io` / `blk_inject_io_off`（`:46,53`） | 武装一个 `(dev, lba, 读/写)`，覆盖它的传输一律回 `LNXRM_EIO`。**只给自测用**，生产代码从不武装 |
| `io_fault_hit`（`:59`） | 判断一次 `[lba, lba+count)` 是否覆盖了武装的那个 LBA |
| `blk_cache_init`（`:83`） | 清表 + `lru_seq` 归零 |
| `cache_find`（`:99`） | 线性找，命中即把 `stamp` 刷成当前 `lru_seq`（= 变成最近使用） |
| `cache_find_victim`（`:118`） | **先要空槽**（拿空槽不丢任何东西），否则取 `stamp` 最小的常驻项 = 最近最少使用 |
| `cache_note_eviction`（`:132`） | 记一次淘汰（拿走一个常驻槽就计数，无论后面填充成不成功） |
| `cache_writeback`（`:138`） | 脏项回写，记 `writebacks` |
| `blk_cache_read` / `blk_cache_write`（`:163,227`） | **入口处也检查注入**（`:170,234`）：否则"扇区正好在缓存里"会把故障挡掉，测不到文件系统。未命中选 victim、回写、填充，并记 hits/misses |
| `blk_cache_flush`（`:272`） | 显式刷全部脏项（`cache_flush_locked` 持锁） |
| `blk_cache_lookup`（`:281`） | "这个扇区在不在缓存里"，返回槽号或 -1。**故意不碰 `stamp`**——问一句不能改变下一次淘汰谁 |
| `blk_cache_purge`（`:298`） | 写回 + 丢掉该设备的全部条目（`dev=NULL` 表示全表），给测试一个已知起点；写回失败时**保留脏槽** |
| `blk_cache_stats_get` / `_reset`（`:26,31`） | 计数器快照/清零 |

**时间戳 LRU（T-007）**：`struct cache_entry` 上只有一个 `u64 stamp`，取自文件顶部的单调
`lru_seq`（`:17`）；命中（`:103`）、填充（`:219`）、暂存（`:260`）都 `++lru_seq`。旧实现用
的是一个 `used` 位：命中置真、没人按条目清，于是 64 个槽全部"used"之后只能"全体清零再取 0"，
与谁刚被碰过毫无关系（C24）。**`used` 字段已整个删除**——两套记账互相打架正是那个 bug 的
根因，现在只剩 stamp 一份。

**计数器**（`struct blk_cache_stats`，`disk.h:131`）：`hits/misses/evictions/writebacks/
dev_reads/dev_writes`。递增走 `__sync_fetch_and_add`（`:23`）而不是锁：`blk_io_*` 既在锁外
被调（挂载、FAT 直写），也会在 `cache_writeback()` **持有 `cache_lock` 时**被调用。
`dev_reads/dev_writes` 只在真正交给驱动前递增——注入出来的失败不算一次传输。

**读失败的处理**（`:201-212`，T-005 一并修掉）：驱动可能已经把扇区写了一半才拒绝，
此时 victim 槽还挂着**上一个 LBA**（`valid` 只在成功后才置真）。旧代码失败即返回、
槽不动，下一次命中就把半覆盖的扇区当成那个 LBA 的好数据。现在失败就作废该槽
（`valid/dirty = false`、`stamp = 0`）——代价是多一次 I/O，不是一次文件系统损坏。

⚠️ 仍存在的问题：**没有周期性回写，也没有退出/panic 时的同步**（GAP C27）——脏扇区只在被
淘汰或 `blk_cache_flush` 时落盘，而 `blk_cache_flush` 目前只有挂载路径调用（`vfs.c:665,674,
705,714`）。`blk_list_all`（给 `SYS_diskinfo` 用）在 `vfs.c:639`，不在本文件。

**守着它的测试**：`ktest/t_cache.c`（342 行，5 例，suite `blkcache`，`KTEST_LATE`）。

#### 9.4 `kernel/fs/fat32/`（1,586 行，4 个模块）

**`fat32_priv.h`（247 行）** —— 私有数据结构总汇 + **I/O 契约**（T-005）：
```c
struct fat_mount {
    struct blkdev *dev;
    u8  *fat;              /* 整个 FAT 表的连续 kmalloc 副本 */
    u64  fat_bytes;
    u32  bytes_per_sector, sectors_per_cluster, num_fats;
    u32  reserved_sectors, root_cluster, max_cluster;
    u64  data_start_lba;
};
extern spinlock_t fat_fs_lock;    /* 全局锁，所有 FAT32 操作都拿 */

/* 扇区 I/O：只回答 0 或 LNXRM_EIO，且每个结果都必须被检查 */
static inline int fat_read_sector (struct fat_mount*, u64 lba, void*);
static inline int fat_write_sector(struct fat_mount*, u64 lba, const void*);
static inline int fat_read_cluster(struct fat_mount*, u64 clba, void*);
```

**`fat32.c`（323 行）** — 挂载与 FAT 链
- `fat_mount_impl`：读扇区 0 → 校验 BPB（`bytes_per_sector == dev->sector_size`、spc/fatsz/reserved 非零、`num_fats ∈ {1,2}`、`root_cluster >= 2`）→ 算 `data_start_lba` / `max_cluster` → **把整个 FAT `kmalloc` 到内存**
- `fat_next_cluster(m, c)` → `m->fat[c*4] & 0x0FFFFFFF`
- `fat_set_entry(m, c, v)`（`:114`，返回 int）→ 改内存副本 + `blk_io_write` 立即写回一个扇区（**绕过块缓存**，故不受缓存写回影响；写失败回 `LNXRM_EIO`——静默丢一次 FAT 更新 = 下次挂载后两个文件拥有同一个簇）
- `fat_find_free_cluster` → 从簇 2 起线性扫
- `fat_warn_io(m, op, lba)`（`:55`）：限流 16 条的 `[fat32] I/O error <op> LBA <n>`，
  与 `fat_warn_entry`/`fat_warn_steps` 共用 `fat_warns` 计数器

**`fat32_dir.c`（258 行）** — 目录扫描
- `fat_scan_dir(m, dirclus, cb, ctx)`（`:41`）：沿簇链读，逐 32 字节步进目录项。
  **三态返回**：`1` = 回调拦下了（匹配成功）、`0` = 扫到目录末尾、负数 = 某个扇区读不进来
- `fat_resolve_path(m, path, r)`（`:139`）：同样三态，`1` 解析成功 / `0` 某个分量不存在 /
  负数 = 目录读不出来（**调用方必须报 `EIO`，不能报 `ENOENT`**）
- `fat_dir_iter(it, d)`（`:219`）：readdir 走一步，`1` 填好 `d` / `0` 没有更多 / 负数 = 出错。
  旧版本用同一个负值表示"到头了"和"读失败"，`ls` 因此能把死盘显示成空目录
- `lfn_state`（`char longname[256]`）+ `lfn_reset` —— **LFN 长文件名重建**
- `fat_short_to_name(out, raw)`：8.3 → 显示名（小写，第 8 字节非空格则插 `.`）
- `fat_name_eq_short`：大小写不敏感的 8.3 比较

**`fat32_file.c`（260 行）** — 文件读写
- `fat_read_impl`：重建头簇 → 钳制 n → 经 `fat_read_cluster` 走**块缓存**读，读不到回 `EIO`
- `fat_write_impl`（`:147`）：钳制 1 MB → `alloc_chain` 扩链 → 循环"读扇区→memcpy→
  `fat_write_sector`（=`blk_cache_write`，写回缓存）" → **重扫整个父目录链**
  更新 `filesize`/`fstclus*`。三种结局（T-005）：
  | 情况 | 结果 |
  |---|---|
  | 读旧扇区失败 | `EIO`，**一个字节不落盘、元数据不动**（`:215`）——C15 的原始现场 |
  | 中途失败 | 按 `done` 发布 `filesize`，返回**短计数**，不谎报整段写完 |
  | 目录项写不进去 | `EIO`（`:246`） |
- `alloc_chain(m, first, need_bytes, rc)`：数现有链长，按需补链；`fat_set_entry`
  失败会把 `*rc` 置成 `LNXRM_EIO`（否则"扩链失败"会被当成"磁盘满了"）
- 仍登记：`!buf && !n` 的 truncate 分支只改内存里的 `filesize`，不写回目录项（C19 的一半）

**`fat32_meta.c`（745 行）** — 目录项操作（最大的 FAT32 文件）
- `parent_entry_add` / `dirent_write` / `dirent_free`：创建/改写/删除目录项
  ——**全部返回 int**（T-005）：扫父目录读不到 → `EIO`，写不进去 → `EIO`，
  旧代码在这两处都回 0（"什么都没写"被当成"成功"）
- `fat_mkdir_impl` / `fat_rmdir_impl` / `fat_unlink_impl` / `fat_rename_impl` / `fat_move_impl`
- `dirent_locate` / `dir_claim_run` / `dir_contains` / `dir_dotdot` / `dir_set_dotdot`：三态
- `fat_lfn_count` / `lfn_fill`：**只有 rename 会写 LFN 记录**，create/mkdir 不会
- `name_to_short`：长名 → 8.3 别名

**写路径的持久化**（原 `fat32_journal.c` 已删除）：数据与目录的写入经
`fat_write_sector` → `blk_cache_write` 进入 64 项**写回块缓存**，由
`blk_cache_flush` 落盘。读路径（`fat_read_sector`/`fat_read_cluster`）刻意走同一缓存——绕过
它会让刚创建、还在缓存里的目录项对 `ls`/路径解析不可见（`fat32_priv.h`
注释记录了这个历史 bug：mkdir/touch 曾"成功"却永不出现）。
**FAT 区是另一条路**：读 `m->fat`（挂载时整表读进内存，`fat32.c:310`）、写
`fat_set_entry` → `blk_io_write` 直写（`:114,126`，FAT 项要立刻生效）；挂载时的 BPB/FAT 表
读取也直读。三类 LBA 各走各的、互不重叠——这就是 9.1.1 的**路径不变式**（T-008）。

**守着这套契约的测试**：`ktest/t_ioerr.c`（234 行，4 例，suite `blkio`，
`KTEST_LATE`）用 `blk_inject_io()` 造一个"特定 LBA 必死"的磁盘，断言
①写路径回 `EIO` 且数据/目录扇区快照逐字节不变 ②`create` 两支失败 ③
lookup/unlink/rmdir/readdir 在 **impl 层与 VFS 层**都是 `EIO`（不是 `ENOENT`、
不是"目录是空的"）④注入设施自身的行为（含缓存命中、相邻 LBA、解除武装）。
`scripts/smoke_test.py` 另有 marker `t-005 injected I/O error reported` 盯着
串口必须真的打出 `[fat32] I/O error`。

`ktest/t_cache.c`（342 行，5 例，suite `blkcache`，`KTEST_LATE`）守另一半：
淘汰顺序必须是 LRU、热集不被冷流冲掉、200 扇区文件不会被写回两次、写后读回（含 1000 次
往返与 purge 之后）字节不变、FAT 区 2018 个扇区一个都不许进缓存。`LATE_REPORTS`
（`smoke_test.py:97`）在整段日志里搜这 5 条报告行，而不是把它们塞进 `BOOT_MARKERS`
顺序等——`.ktest` 段内的先后是编译器的事（本机实测 `t_cache.c` 的 5 例**倒序**打印）。

#### 9.5 `kernel/fs/mbr.c`（155 行）

- `mbr_parse(dev, mbr)`：读扇区 0（`mbr.c:42`，经 `blk_io_read`），验 `0xAA55`，扫 4 个 `struct mbr_entry`（偏移 446）
- `mbr_get_partition(dev, idx, out_dev)`：`kmalloc` 一个 `struct part_dev {parent, start_lba, size_sectors}`，填 `blkdev` 的 `read`/`write` 为 `part_read`/`part_write`——两者都是**转发**到 `blk_io_read`/`blk_io_write`（`mbr.c:18,25`），所以分区设备与整盘设备走同一套检查与同一处注入
- ⚠️ 不校验分区是否落在设备范围内；扩展分区标记了但 EBR 链从不遍历
- ⚠️ `static char part_name_buf[16]` 被所有分区共享
- ⚠️ **标准 `make run` 流程下运行时不可达**：`vfs_try_mount_disk` 抽出的 `vfs_try_mount_one()` 第 1 步"整盘 FAT32"（`vfs.c:668`）对 `build/disk.img`（`mkfs.vfat` 直接格式化、MBR 分区表全 0）总是成功并直接返回，`mbr_parse`（`vfs.c:680`，第 2 步 fallback）永远走不到。代码有调用点、并非死代码，但**只有换带分区表的磁盘才会执行**——标准流程测不到它。（T-081 起这个函数**按 `blk_list[]` 依次试每块盘**，不再是"只挂 `blk_first`"，所以同一台机器上第一块盘挂不上时会多打一行 `attempting disk mount` 再试第二块。）

#### 9.6 `include/disk.h`（157 行）

```c
/* struct blkdev 实际定义在 include/sys/vfs.h:132（disk.h 一并 include 它）；容量取值
   ata_total_sectors() 在同文件 :121，T-080 起由 ide.c 与 ahci.cpp 共用 */
struct blkdev {
    const char *name;  u32 sector_size;  u64 num_sectors;
    int (*read) (struct blkdev*, u64 lba, u32 count, void *buf);
    int (*write)(struct blkdev*, u64 lba, u32 count, const void *buf);
    void *drv;
};
/* T-007：只有一个 stamp 记最近最少使用——旧的 used 位已删 */
struct cache_entry {
    struct blkdev *dev; u64 lba; u8 data[512];
    bool valid, dirty;
    u64 stamp;          /* 命中/填充/暂存都 ++lru_seq，最小者为受害者 */
};

/* T-005：所有传输都走这里，而不是 blkdev->read/write */
#define BLK_IO_READ  0x1
#define BLK_IO_WRITE 0x2
int blk_io_read (struct blkdev *dev, u64 lba, u32 count, void *buf);
int blk_io_write(struct blkdev *dev, u64 lba, u32 count, const void *buf);
/* 仅自测：武装一个 (dev, lba)，覆盖它的传输一律回 LNXRM_EIO */
void blk_inject_io(struct blkdev *dev, u64 lba, int ops);
void blk_inject_io_off(void);

/* T-007：缓存的可观测面——验收要的"统计"与"在不在缓存里" */
struct blk_cache_stats { u64 hits, misses, evictions, writebacks,
                          dev_reads, dev_writes; };
void blk_cache_stats_get(struct blk_cache_stats *out);
void blk_cache_stats_reset(void);
int  blk_cache_lookup(struct blkdev *dev, u64 lba);   /* 槽号或 -1，不动 stamp */
int  blk_cache_purge (struct blkdev *dev);            /* 写回+丢弃（dev=NULL 全表） */
```

⚠️ `blkdev` **无队列、无合并、无锁、无分区抽象** —— 每次访问同步不可中断。

---

### 10. 设备驱动（`kernel/drivers/`，602 行）

#### 10.1 `kernel/drivers/pci.c`（81 行）

- `pci_read(dev,off)` / `pci_write(dev,off,val)`：传统 0xCF8/0xCFC 窗口
- `pci_scan()` → `probe_bus(0)` → 对每个设备调 `probe_fn`
- 支持多功能设备（头位 0x800）
- ⚠️ 桥递归**无深度限制**；对所有总线都用 0xCF8/0xCFC（只对 bus 0 有架构保证）

#### 10.2 `kernel/drivers/ide.c`（194 行）

硬编码主通道 0x1F0、主设备 0、PIO 模式、一次一扇区。

| 函数 | 职责 |
|---|---|
| `ide_wait_ready` / `ide_wait_drq` | 轮询状态端口。⚠️ `ide_wait_ready` 不判 `IDE_SR_ERR`（**T-080 刻意不加**：它在发命令**之前**也调用，而 ATA 靠下一条命令清 ERR，加了会把残留 ERR 变成永久死等）；`ide_wait_drq` **判 ERR**，是全文件唯一的 ERR 判定点 |
| `ide_read_sector` / `ide_write_sector` | 先做 `lba > 0x0FFFFFFF` 上界检查（**T-080**：超界回 `EFAIL`，不再丢掉高 4 位静默读错扇区），再编程 SECCOUNT/LBAx/DRIVE，发 `0x20`/`0x30`，等 DRQ，mov 256 个 u16 |
| `ide_lock` | `irqsave` 锁，包住整个多扇区循环和所有轮询 |
| `ide_init`（`:133`） | 选盘 → 4 次状态读当 400 ns → `s == 0x00 \|\| s == 0xFF` 判无设备（**空总线回 `FF`，不是 `00`**）→ 等 DRDY → 发 `0xEC` → **`ide_wait_drq()`（等 DRQ 且判 ERR）** → 读 256 字 → `ata_total_sectors()` → 容量为 0 **不注册** → 打印型号 → `blk_register` |

⚠️ **无 LBA48 命令**（仍只编程 28 位寄存器；但容量取值已改走 `ata_total_sectors()`，words 100-103 会读、哨兵不再被当 2 TiB，C53 保持部分开放）；写后 "flush" 忽略返回值且不发 `0xE7` CACHE FLUSH（C55）。`ide_init` 的 DRQ/错误轮询问题（C54）已于 T-080 修复。

#### 10.3 `kernel/drivers/ahci.cpp`（398 行，C++）

**这是最复杂的驱动，也是问题最集中的文件。**

| 部分 | 内容 |
|---|---|
| `struct CmdHdr` (32 字节) | 命令表头 |
| `struct CmdTbl` (128 字节) | 命令表 + 柔性数组 `prdt[]` |
| `pci_enable_bm` / `pci_bar5` | 使能 bus master，取 ABAR（⚠️ 不校验 BAR 类型） |
| `ahciProbe` | 映射 ABAR → `ahci_hba_reset` → 逐端口 `ahciPortInit` → IDENTIFY → `blk_register` |
| `ahciPortInit` | 建命令表头/命令表/FIS 接收区，填 `P_CLB`/`P_FB` |
| `buildH2dFis` | 构造 64 字节 H2D FIS（type 0x27） |
| `ahciBlkRead` / `ahciBlkWrite` | 填命令表 → `*P_CI = slot` → **轮询 `P_CI` bit 0 清零** → 先查 `IS_TFES`、再查 `PxTFD.ERR`（**T-080 补的后半段**：完成 ≠ 成功） |
| `ahci_lock` | 全局锁（跨所有 AHCI 设备） |

**PRD（物理区域描述符）**：单条目，`flags = (bytes-1) | PRDT_IOC`。缓冲区必须是物理连续的 512 字节 —— 靠 `kmalloc` 的 1 KiB bin 块不跨 4 KiB 页这个巧合成立。

**三个致命问题**（详见 GAP_ANALYSIS C48–C52）：
1. 设备寄存器按**可缓存**映射（`vmm_map_kernel_page(..., 0x03)`，无 `PG_PCD`），doorbell 写后无 `clflush`/`sfence`
2. **完全无中断**：`cli` 住自旋 `P_CI` 最多 10⁷ 次，且在两层嵌套 irqsave 锁里
3. ~~完成判定只看 `IS_TFES`，**从不看 `PxTFD.ERR`** ⇒ ATA 失败被当成功 ⇒ `num_sectors = 0xCCCCCCCC` 被注册成真设备~~ **✅ C50 已修（T-080）**：`issueCmd` 在 `P_CI` 清零后先查 `IS_TFES`、再查 `PxTFD.ERR`（bit 0）；容量另走 `ata_total_sectors()`，读不出就拒绝 `blk_register`

⚠️ **标准 `make run` 流程下不可达**：`ahci_init`（`main.c:280`）虽然无条件执行 `pci_scan`，但 `make run` 的 QEMU 是 `-drive ...,if=ide`——**机器上没有 SATA 控制器**，`ahciProbe` 的 class 过滤（`class_ != 1` / subclass != 6）对每个设备都提前返回，后面的映射、端口初始化、IDENTIFY、读写路径（本节其余全部内容）**零执行**。全文件只跑过"扫描并放弃"的前两行。连接进 bzImage 但从未经真实验证（详见 §15.1）。

---

### 11. 图形与控制台（`kernel/framebuffer.c` + `kernel/print.c` + `kernel/ansi.c`，1,106 行）

#### 11.1 `include/framebuffer.h`（114 行）

```c
#define FB_MAX_DIM  8192
#define FB_VMA      0xffffffff8d000000UL   /* PD_HI[104], 4 KiB 页 */
#define FB_PD_BASE  104
#define FB_PD_SLOTS 8                        /* 16 MiB 窗口 */
#define FB_MAX_BYTES (FB_PD_SLOTS * 0x200000UL)
#define FB_CELL_W 8
#define FB_CELL_H 16
#define FB_CLR_BLACK .. FB_CLR_AZURE         /* 32 个 0x00RRGGBB 命名色 */
```
导出五组，按"谁能调"排：
**尺寸/状态**（`fb_width/height/pitch/bpp/pxsz`、`fb_active`）、**控制台**
（`fb_init` / `fb_console_init` / `fb_console_putc` / `fb_console_get`）、**4 个绘图
原语**（`fb_clear` / `fb_fill_rect` / `fb_draw_char` / `fb_puts`，见 §11.2 的 ⚠️）、
**ktest 专用**（`fb_layout_get/set` / `fb_pack` / `fb_rgb_px` / `fb_read_px` /
`fb_write_px` —— 自测要直接读写像素），
以及 T-033 的**三个归属操作**（`fb_owner_get` / `fb_claim` / `fb_disown`）。
**没有双缓冲 API** —— `fb_buffer_set` 曾经存在，已连同实现一起删除；归属不恢复它：
那三个函数管的是"谁可以调那 4 个原语"，不是"画什么"。

#### 11.2 `kernel/framebuffer.c`（702 行）— 图形核心

**分层（自底向上）**：

| 层 | 函数 | 做什么 |
|---|---|---|
| **原始像素** | `fb_store`（`:551`） | 按 `fb_pxsz`（1/2/3/4）把打包好的像素写进 LFB |
| **颜色** | `fb_pack`（`:154`）/ `fb_cube_level` / `fb_rgb_px` | `0x00RRGGBB` 按 `r_pos/r_size/...` 打包；8bpp 索引模式就近量化到 6×6×6 立方 |
| **DAC** | `fb_setup_palette`（`:193`） | 仅 `fb_pxsz==1`：编程 VGA DAC 的 16 VGA 色 + 6×6×6 色立方 + 24 级灰阶 |
| **字体** | `font8x16[96][16]`（`:33`） | CP437 点阵，ASCII `0x20..0x7E` |
| **内部绘制** | `fb_put_pixel`（`static`）/ `fb_clear` / `fb_fill_rect` / `fb_draw_char` / `fb_puts` | 导出给 23–27 五个图形系统调用 + 控制台，全部带边界裁剪 |
| **控制台** | `fbc_*`（`:298-441`）cursor / fg / bg / ANSI / scroll | 帧缓冲上的 ANSI 终端 |
| **归属**（不是绘制层） | `fb_owner_get` / `fb_claim` / `fb_disown`（`:681-701`） | T-033：一块屏一个持有者。**只记状态、不参与绘制**——见下 |

⚠️ **矩形/字符级绘图原语仍然导出**（`fb_clear` / `fb_fill_rect` / `fb_draw_char` /
`fb_puts`），由 23–27 的 5 个图形系统调用调用（§11.5）；`fb_put_pixel` 保持 `static`，
只服务这四个原语。直线/圆/`fb_box_rgb` 一类的高级图元仍然没有。滚动由 `fbc_scroll`
（`:372`，逐行 `memcpy` + `fb_fill_rect` 清最后一行）在控制台内部完成。

**归属（T-033，2026-10-05）——状态记在驱动里，强制做在系统调用里**。
`static u32 fb_owner`（`:681`，0 = 空闲）+ 三个操作：

- `fb_claim(pid)` —— 用 `__atomic_compare_exchange_n`（SEQ_CST）**而不是** check-then-set：
  两个 CPU 能同时看到空闲屏，普通写法会两边都回"可以"、只留下最后落笔的那个。
  CAS 最多两趟：要么赢，要么看到屏幕已被别人换走（回 `false`）
- `fb_disown(pid)` —— 同样 CAS，**只清自己持有的**，所以输家退出清不掉赢家的屏；
  拒绝也不改归属（否则被挡下的那个反而占住屏，真正画的人下一次被拒）
- `fb_owner_get()` —— 给拒绝回执和自测读

**上面表里那 4 个原语一道归属门都不查。** 控制台与用户态共用它们，
一个能被用户进程拒绝的控制台就是一个能被用户进程弄哑的控制台。强制全在
`kernel/syscall.c` 的 `fb_owner_gate()`（§6.3，能力门之后），释放全在
`kernel/task.c` 的 `sys_exit`——所有死法（`exit` 系统调用、SIGKILL、默认处置的
信号、无人处理的 #PF/#GP）都落进它，pid 由只增的 `next_pid()` 发、永不复用，
一个释放点覆盖全部，`task_free_slot` 里故意**不**放第二份。

**像素格式抽象仍然保留**：`fb_pack` 按 `r_pos/r_size/g_pos/g_size/b_pos/b_size`
把 8-bit 通道打包到运行模式，所以 8/15/16/24/32 bpp 都渲染同一套 `0x00RRGGBB` 颜色。
`fb_init`（`:466`）对引导加载器留空的通道布局做了 `15/16/>=24 bpp` 的兜底。

**`fb_init` 与内存分配的耦合**（这个设计很巧但脆弱）：
`fb_init` 用 4 KiB 页把 LFB 映射进 `PD_HI[104..111]`，这些槽**失去恒等别名**。
所以 `start_kernel` 必须在 `pmm_init` **之前**调 `pmm_reserve` 把实际用到的槽数
对应的物理页从 buddy 里挖掉（`main.c:223-234`）——那些页既不能被分配，也没有
别名了。（曾经还要为后缓冲预留两倍槽数，后缓冲已删除。）

#### 11.3 `kernel/print.c`（373 行）— 串口 + VGA 文本双后端

```
console_putc(c)
   ├─ fbc_putc(c)          ← 帧缓冲控制台（先喂，ANSI 序列才能被识别）
   ├─ vga_putc(c)          ← VGA 文本 80×25（硬件光标）
   ├─ lnxrm_uart_putc(c)   ← Rust 16550
   └─ ring_putc(c)         ← 4096 字节环形缓冲（fb_console_init 用它回放）
```

`kvprintf` 持 `console_out_lock`（内部叫 `print_lock`，`spin_lock_irqsave`）。
支持 `%s %d %u %x %X %p %%`（`%X`/`%04X` 是 2026-10-01 补的——MBR 错误路径
原来输出全是字面量）。

**环形缓冲回放很巧妙**：`fb_console_init`（`framebuffer.c`）在切换到帧缓冲
之后回放 `ring_buf` 里已滚出 VGA 的内容，所以串口上看到的历史消息也能出现在
帧缓冲控制台上，且不会画两次。

`console_out_lock/unlock` 导出给 `console_write`（`vfs.c`）共用——**C1 已修复
（2026-10-01）**：用户态 `write(1,…)` 与 `kvprintf` 串行化两套 ANSI 解析器状态
与 `ring_head`；`console_write` 64 字节分块拿锁，限制关中断时长。

**VGA 文本后端仍然完整保留**：`fb_init` 因模式数据非法而提前返回时，`fbc_*`
全程不激活，输出落在 `vga_putc` 上 —— 这就是"帧缓冲回退到 VGA 文本"的路径。

#### 11.4 `kernel/ansi.c`（31 行）— ANSI 解析器

极简三状态机：
```c
struct ansi_seq { int phase; int len; char buf[8]; };
// phase 0: 普通文本
// phase 1: 刚收到 ESC
// phase 2: 收到 ESC[，正在收集参数
```
`ansi_feed(s, c)` 返回 `ANSI_TEXT` / `ANSI_CONSUMED` / `ANSI_DONE`。

**支持的序列**（被 `framebuffer.c:307-371` 和 `print.c:86-160` 各自解释一遍）：
- `ESC[2J` 清屏
- `ESC[nH` / `ESC[r;cH` 光标定位（1-based，钳制）
- SGR `ESC[...m`：`0` 重置、`30-37` 前景、`40-47` 背景、`1` 粗体

⚠️ `ESC[3J`（`usr/clear.c` 想用的）**是空操作** —— 没有 scrollback。

#### 11.5 用户态图形

**5 个图形编号 + `kfb_*` 包装**：`lnxrm_syscalls.def` 的 23–27 与
`init/ulib.h` 的 `kfb_info`/`kfb_clear`/`kfb_fill`/`kfb_char`/`kfb_puts` 一一对应，
参数结构体 `lnxrm_fb_info`（16 B）/`lnxrm_fb_rect`（20 B）/`lnxrm_fb_char`（20 B）/
`lnxrm_fb_str`（24 B）带 `_Static_assert` 锁布局。

颜色参数一律是 **32 位真彩色 `0x00RRGGBB`**（见 `FB_CLR_*` 的 32 个命名色），
矩形/字符坐标是**像素坐标**，内核侧四个原语全部裁剪到可见区域——
传屏幕外的矩形是零写入而不是越界。`fb_puts` 的字符串经 `copy_user_str`
（256 字节上限）取出。用户态另有 `write(1, ...)` 和 `ESC[2J` 这条纯文本路径。

**两道门（T-031 能力 + T-033 归属）**：这 4 个绘图调用先 `cap_gate(CAP_FB, ...)`
再 `fb_owner_gate()`，第一个成功的调用占住屏，后来者回 `-13`；持有者退出即释放
（回执 `[fb] denied fb_fill: pid 13 holds the surface, pid 14 draws`）。
`kfb_info`（27 号 `fb_info`）**两道门都没有**——只读几何。
测试见 `ktest/t_fb.c`（`test_fb_owner_is_one_pid_at_a_time`，12 条断言）与
`usr/systest.c` 的 T-033 段（三层 `kfork`：抢屏 → 撞墙 → 释放，全部靠 `kwaitpid`
排序，不用 sleep）。

---

### 12. 用户态

#### 12.1 `init/`（239 行）— 链接进每个程序的运行库

⚠️ **注意**：`init/` 不是 `/init` 那个程序，是所有用户态程序都链接的运行库。

| 文件 | 行 | 职责 |
|---|---|---|
| `init/crt0.S` | 29 | `_start`：从 `[rsp]` 取 argc、`[rsp+8]` 取 argv → `call main` → 用返回值作 `SYS_exit` |
| `init/ulib.h` | 130 | 32 个内联包装，32 个已实现系统调用**全覆盖**（只有 `systest` 用 `syscall1` 直发不存在的编号去测 ENOSYS）+ 迷你 libc 原型 |
| `init/ulib.c` | 80 | 迷你 libc 实现：`xstrlen` `xputs` `xprintf` `xprinti` `xphex` |

**系统调用内联汇编**（`ulib.h:20-30`）：
```c
static inline long sys_call3(long n, long a, long b, long c)
{
    long r;
    register long r10 __asm__("r10");
    (void)r10;                                    /* 仅为把 r10 约束住 */
    __asm__ volatile("mov %4, %%r10\n\tint $0x80"
                     : "=a"(r) : "a"(n), "D"(a), "S"(b), "r"((long)c)
                     : "rcx", "r11", "r10", "memory");
    return r;
}
```
第三个参数要手动搬进 `r10`（内核约定 `rax`/`rdi`/`rsi`/`r10`），且 clobber 列表必须含 `r10`。

没有 `xstrcpy` / `xmemcpy` / `kgetkey` ——它们对应的内核侧不存在。
`kfb_*` 5 个图形包装见 §11.5。输出走 `xprintf`（不是 `printf`）。

#### 12.2 `usr/`（16 个程序，1,481 行）

| 程序 | 行 | 职责 |
|---|---|---|
| `systest.c` | 859 | **全量系统调用测试**：错误码路径、信号/等待、`fb_*`、故意段错误隔离，外加 T-031 的降权子进程段（`setuid(1000)` 后逐个撞权限检查点）与 C46 的超长路径段（相对路径正常通过、`path.toolong.*` 5 条回 `-36`），**T-032 的记账/配额段**（父进程量 `mem.*`、子进程撞墙报 `quota.*`，全部在 `systest.done=1` 之前）与 **T-033 的归属段**（三层 `kfork`：首绘抢屏 `fb.owner.grab=0` → 第二个绘图被拒 `fb.owner.denied=-13` → 持有者退出 `fb.owner.released=0`，输家的 `held=0` 证明它没抢到），输出 `systest.done=1` |
| `sh.c` | 111 | 交互式 shell：读命令 → 解析 → 内建或 `fork+execve` |
| `rm.c` | 91 | 删除文件；`-p <dir>` **递归**删目录 |
| `mv.c` | 90 | 移动/改名：路径拼接在用户态完成，内核只见 `SYS_rename` |
| `ps.c` | 90 | 内核填 `lnxrm_ps_entry[64]` 缓冲，用户态格式化成 PID/PPID/CPU/STATE/KIND/UID/**MEM/PEAK** 表（后两列是 T-032 的内存记账，单位 KiB） |
| `init.c` | 52 | **pid 1**。打开 `/dev/console`、打印 `/README.md`，然后 `fork + execve("/bin/sh")` 循环 |
| `kill.c` | 45 | 发信号 |
| `ls.c` | 29 | 列目录（`SYS_getdent`） |
| `cat.c` | 30 | 读文件到 stdout |
| `rn.c` | 29 | 重命名 |
| `echo.c` | 12 | 回显参数 |
| `hello.c` | 25 | 测试程序 |
| `mkdir.c` | 20 | 创建目录 |
| `touch.c` | 19 | 创建空文件 |
| `clear.c` | 17 | 发 `ESC[2J` |
| `help.c` | 10 | 列出可用命令 |

（已删除：`cpu.c`、`fdisk.c`。）**链接方式**（`Makefile`）：`-static -no-pie
-mcmodel=large -Wl,-Ttext=0x7f8000400000`。**必须是 ET_EXEC 静态单文件**
（内核的 `elf_load` 不支持 PIE/ET_DYN，见 §18 的 `elf.c`）。

#### 12.3 `include/cpp/`（27 行）— C++ 包装类

| 文件 | 内容 |
|---|---|
| `smp.hpp`（27） | `smp::Manager` 类声明（实现在 `smp.cpp`） |

（`File.hpp` 和 `Process.hpp` 是从未被使用的死包装，已删除。）

---

### 13. Rust 组件（`kernel/rust/`，296 行）

一个 staticlib，由链接脚本的 `*(.text .text.*)` 通配整体拉入。

| 文件 | 行 | 职责 | C 接口 |
|---|---|---|---|
| `lib.rs` | 57 | crate 根、`#[panic_handler]`、自写 `format_into` | 无 |
| `uart.rs` | 106 | **16550 UART 驱动**（完全由 Rust 拥有）：115200 8N1 + FIFO、LSR.THRE/DR 轮询、`\n`→`\r\n` | `lnxrm_uart_init/putc/trygetc/irq_enable` |
| `smp.rs` | 133 | `TrampolineData`（`#[repr(C)]`，物理 `0x9000`）+ 四个访问器，`read_volatile` + `fence(SeqCst)` | `trampoline_setup/is_ready/signal_ready/clear` |

**`#[panic_handler]`**（`lib.rs:16-27`）：打印 `[rust PANIC] file:line` 然后停机。`format_into`（`:30-57`）是手写的最小格式化器（因为 `core::fmt` 的完整实现需要 `compiler_builtins` 支持）。

**`smp.rs` 的 `TrampolineData` 布局必须与 `arch/trampoline.S:4-10` 精确对应**：
```
0x00  pml4
0x08  stack_top
0x10  ap_id
0x18  gdt_packed
0x20  ap_main（虚拟地址！）
0x28  （ready 标志）
+2 个保留 u64
```

---

### 14. 头文件地图（`include/`，2,169 行，23 个文件）

| 文件 | 行 | 内容 | 关键定义 |
|---|---|---|---|
| `types.h` | 67 | **基础类型 + 地址空间常量** | `u8..u64` `ALIGN_UP/DOWN` `PHYS_TO_VIRT` `VIRT_TO_PHYS` `VGA_VMA` `DEV_VMA` `KHEAP_VMA` `USER_BASE` `USER_STACK_TOP` `PAGE_SIZE` `NR_*` `CONFIG_SMP 1` |
| `boot.h` | 107 | 引导期数据结构 | `struct e820_entry` `struct boot_params` `struct vbe_lfb_info` `struct screen_lfb` `struct fb_info` `struct boot_info` + **4 个 `_Static_assert` 校验偏移** |
| `mm/mm.h` | 102 | 内存子系统接口 + 页表标志 | `PG_P/W/U/PWT/PCD/PS` `PTE_PA_MASK` **`PMM_WINDOW_TOP`**（2026-10-06，物理内存窗口上界）`pmm_*`（`pmm_free_bytes`、`pmm_inject_oom` 故障注入；`pmm_total_bytes` 零引用已删）`kheap_*`（含 `kmalloc_or_panic`）`vmm_*`（含 **`vmm_count_user_pages`**，T-032 记账的唯一真相） |
| `sys/sched.h` | 167 | **任务与调度的核心头** | `struct intr_frame` `struct cpu_ctx` `enum task_state` `struct task`（含信号字段与 T-032 的 **`mem_pages`/`mem_peak`**）信号号/常量 `current` 宏 `runqueue_add_local` |
| `sys/cred.h` | 72 | **特权档与能力**（T-030/T-031/T-032） | `enum cred_kind` `struct cred` `CAP_KILL/SYS_ADMIN/FB` `cred_capable()` `cred_setuid()` **`cred_mem_quota()`** + `CRED_USER_MEM_QUOTA_PAGES`（64 MiB，T-032 配额的唯一来源） |
| `sys/vfs.h` | 144 | VFS 接口 | `struct vnode` `struct fs_ops` `struct file_ops` `struct file` `struct dir_iter`（含 `err`：readdir 已死则继续报错）`struct blkdev` **`ata_total_sectors()`**（T-080：IDENTIFY 容量取值）错误码 |
| `sys/cpu.h` | 127 | CPU 状态接口 + GDT/TSS | `HZ 100` 选择子常量 `struct gdtr` `gdt_init_cpu` `idt_init` `lapic_timer_start` `lapic_timer_calibrate` `mdelay` `rdmsr`/`wrmsr`/`rdtsc` `tsc_per_ms` |
| `sys/apic.h` | 62 | LAPIC/IOAPIC 寄存器与接口 | `LAPIC_*` 寄存器偏移 `IOAPIC_*` `apic_send_*` `ioapic_*` |
| `sys/smp.h` | 71 | SMP 接口 | `smp_init` `ap_main` `ipi_handler_install` `trampoline_start/end` |
| `sys/percpu.h` | 42 | per-CPU 数据（GS 相对） | `struct cpu_info` `CPU_DATA_SIZE 4096` `this_cpu_data()` `this_cpu` 宏 `cpu_table[]` |
| `sys/spinlock.h` | 30 | 自旋锁 | `spinlock_t` `SPINLOCK_INIT` 2 个函数（irqsave 对；裸版本零引用已删） |
| `fat32_priv.h` | 247 | FAT32 私有结构 | `struct fat_mount` `fat_fs_lock` `fat_read_sector`/`fat_write_sector`（走块缓存，**只答 0 或 `LNXRM_EIO`**）`fat_scan_dir`/`fat_resolve_path`/`fat_dir_iter` 三态契约 |
| `disk.h` | 157 | 块设备 + 缓存 | `struct cache_entry`（`valid`/`dirty`/`stamp`，**`used` 已删**）`struct mbr_entry` `CACHE_ENTRIES 64` `blk_io_read`/`blk_io_write`（唯一驱动调用点）`blk_inject_io` 故障注入 `blk_cache_stats_*` `blk_cache_lookup` `blk_cache_purge`（T-007/T-008） |
| `elf.h` | 43 | ELF64 结构 | `Elf64_Ehdr` `Elf64_Phdr` `PT_LOAD` `PF_W` |
| `framebuffer.h` | 114 | 图形接口 | `FB_VMA` `FB_PD_*` `FB_CELL_*` `fb_init` `fb_clear` `fb_fill_rect` `fb_draw_char` `fb_puts` `fb_console_*` `fb_owner_get` `fb_claim` `fb_disown`（T-033 归属）`fb_layout_*` `fb_pack` `fb_read_px`/`fb_write_px`（ktest） |
| `console.h` | 111 | 控制台 + 打印 | `kprintf` `panic` `console_putc` `console_out_lock/unlock`（`kvprintf` 与 `console_write` 共用） |
| `io.h` | 40 | 端口 I/O | `inb` `outb` `inw` `outw` `inl` `outl` |
| `pci.h` | 26 | PCI 配置空间 | `pci_scan` `pci_read` `pci_write` |
| `abi/lnxrm_abi.h` | 224 | **内核↔用户态共享 ABI** | `LNXRM_SYS_*` 错误码 `LNXRM_SYS_*` 编号 `lnxrm_fb_*` `lnxrm_dirent` `lnxrm_sigaction` **`lnxrm_ps_entry`（48 B，含 `mem_kib`/`peak_kib`）** 信号号 |
| `abi/lnxrm_syscalls.def` | 54 | 系统调用编号（X-macro） | 32 个编号 0–31 连续，全部实现 |
| `cpp/smp.hpp` | 27 | C++ 包装 | `smp::Manager` |

✅ **槽号注释已修正（2026-10-01）**：`FIXMAP_VA` 整个宏（连同 `mm.h` 里的重复定义、fixmap 概念、`vmm.c` 对 slot 97/112 的跳过）已删除；`DEV_VMA` 注释由错的 "PD_HI[98]" 改为实际的 **PD_HI[128]**，地址空间列表补上了 framebuffer 窗口 [104,112)。

---

### 15. 构建系统

#### 15.1 `Makefile`（162 行）

**四个工具链变量**：`gcc`（C）、`g++`（C++）、`nasm`（汇编）、`rustc`（Rust）、`ld`（链接）、`objcopy`、`python3`。

**四组编译标志**：

| 组 | 标志 | 理由 |
|---|---|---|
| `KERNFLAGS` | `-ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -O2 -std=gnu11` + `-ffunction-sections -fdata-sections` | 无 libc、无栈保护、非 PIC（`mcmodel=kernel` 让代码假设在 `0xffffffff8xxxxxxx`）、**禁 SSE**（省下保存/恢复）；两个 `-sections` 让每个符号独占段，配合链接 `--gc-sections` 裁掉不可达代码（见 §15.1 关键规则） |
| `CXXFLAGS` | 同上 + `-std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -nostdinc++` | 无异常无 RTTI，**`__init_array` 手动调**（`main.c:21-24`） |
| `RUSTFLAGS` | `--crate-type staticlib -C panic=abort -C relocation-model=static -C code-model=kernel -C target-feature=-sse --target x86_64-unknown-none` | `no_std` 等价（`-C panic=abort` + 自定义 panic_handler） |
| `USRFLAGS` | `-ffreestanding -nostdlib -nostartfiles -static -no-pie -mcmodel=large -Iinit -Iinclude` | 用户态同样无 libc |

**关键规则**：
```make
$(BUILD)/vmlinux.elf: $(KOBJ) arch/kernel.ld $(BUILD)/liblnxrm.a
	$(LD) -T arch/kernel.ld --gc-sections -o $@ $(KOBJ) \
	      $(BUILD)/liblnxrm.a -Map build/vmlinux.map \
	      --no-warn-rwx-segments

$(BUILD)/vmlinux.bin: build/vmlinux.elf
	$(OBJCOPY) -O binary $< $@          # 按 LMA 导出 → 落在物理 0x100000

$(BUILD)/trampoline.o: arch/trampoline.S
	$(NASM) -f bin -o build/trampoline.bin $<   # 先编成裸二进制
	$(OBJCOPY) -I binary -O elf64-x86-64 ... --rename-section .data=.trampoline
		--redefine-sym _binary_build_trampoline_bin_start=trampoline_start ...
```

**`--gc-sections` + `-ffunction-sections`/`-fdata-sections`（2026-10-01 加）**：Rust staticlib 的 `core`（编译器内建、unicode 表、浮点格式化）原先整个被拖进 `.image`，**约 155 KB（占镜像 70%）是永远不会执行的代码**。编译期按符号分段 + 链接期垃圾回收后，`.image` 从 253,472 缩到 ~77,000 字节。`.text.prologue` 必须 `KEEP`（`patch_setup_jmp.py` 要按 `_start32`/`_start64` 符号算跳转，见 §15.2），`entry64.S` 里的绝对地址引用（`isr_stub_table`、trampoline 符号）会自然锚定住对应段。

**trampoline 的两段式处理很有意思**：AP trampoline 要被 `memcpy` 到物理 `0x8000`，所以先编成 `bin`，再 objcpy 成 ELF 对象（带 `.trampoline` 段），才能参与链接。

**`build/disk.img` 目标**（`:204-212`）：`dd` 64 MiB → `mkfs.vfat -F 32` → `mcopy` 拷 `README.md` → `mmd ::bin` → 循环 `mcopy` 拷所有用户态 ELF 到 `::bin/`。**用宿主机的 mtools 构建 FAT32 镜像** —— 聪明，避免了"内核挂载自己生成的镜像"的鸡生蛋问题。

**目标**：`all`（bzImage）/ `usr` / `rust` / `clean` / `run`（QEMU `-m 256 -smp 2`）/ `smoke`（跑 `scripts/smoke_test.py`）/ `imageiso`（`build/lnxrm.iso`，见 §33.1）。

⚠️ `run` 的 QEMU 参数是 `-drive ...,if=ide`（走 IDE 驱动），机器上**没有 SATA 控制器** —— 所以 `ahci.cpp` 虽然链接进镜像、`ahci_init()` 也每轮启动都执行 `pci_scan`，但 class 过滤对所有设备都失败，**AHCI 的实际读写路径在 `make run` 下零执行、完全没被测过**（详见 §10.3）。同理 `mbr.c` 是 `vfs_try_mount_disk` 的第 2 步 fallback，整盘 FAT 挂载总能先成功，标准镜像下也走不到（§9.5）。

#### 15.2 `scripts/patch_setup_jmp.py`（69 行）

从 `nm` 查 `_start32` 和 `_start64` 的偏移，算出 `_start64` 的物理地址 `0x100000 + (start64 - start32)`，然后在 `setup.bin` 里**找 `jmp 0x18:imm32` 指令（opcode `0xEA`，选择子 `0x0018`）**并回填。哨兵 `0xF00DFACE`。**幂等**。

#### 15.3 `scripts/patch_bzimage.py`（87 行）

填 Linux boot protocol 头：
- `0x1F1`（字节）`setup_sects` = 实际扇区数 **减一**（0 特殊表示 4 扇区）
- `0x1F4`（dword）`syssize` = payload 字节数 / 16（16 字节段）

带 3 个 assert：`HdrS` 魔数 @0x202、boot flag `0x55AA` @0x1FE、`payload_off` 是 512 倍数且 ≥ 2048。

---

### 16. 关键数据结构速查

#### 16.1 `struct task`（`include/sys/sched.h`）

```c
struct task {
    u32 pid;                        /* 0 = idle */
    u32 cpu_id;                     /* 运行它的 CPU，-1 = 无 */
    enum task_state state;          /* UNUSED/EMBRYO/RUNNABLE/RUNNING/SLEEPING/ZOMBIE/STOPPED */
    char name[16];
    struct cred cred;               /* T-030：特权档 + uid（fork 复制） */
    struct intr_frame *tf;          /* 指进内核栈 */
    struct cpu_ctx ctx;             /* 只有 sp */
    char *kstack, *kstack_top;      /* kmalloc 的 32 KiB */
    u64 pml4;                       /* CR3 物理地址 */
    void *brk_base, *brk_cur;
    u64 mem_pages;                  /* ← T-032：用户页数，页表行走刷新 */
    u64 mem_peak;                   /* ← T-032：峰值，ps 的 PEAK 列 */
    struct file *fds[16];
    int exit_code; u64 sleep_until; int quantum;
    struct task *parent;            /* 裸指针（⚠️ 槽位会复用） */
    struct task *rq_next;           /* 运行队列链接 */
    int rq_cpu;                     /* 队列所属 CPU，-1 = 不在队列 */
    /* 信号（sigaction/sigprocmask/sigreturn 链路的状态） */
    u64 signal_pending;             /* 待决位掩码 */
    u64 sig_blocked;                /* sigprocmask 的屏蔽集 */
    u64 sig_saved_blocked;          /* sigreturn 要恢复的屏蔽集 */
    void *sig_handlers[32];         /* NULL=默认 (void*)1=忽略 其余=用户 handler */
    u64  sig_masks[32];             /* 每信号的 sa_mask */
    bool sig_in_handler;            /* 用户 handler 帧存活中（限制嵌套） */
    struct intr_frame saved_tf;     /* 被打断的帧，sigreturn 还原 */
    u64 sig_fault_rip;              /* 已给过故障 handler 的 RIP（防重复） */
};
```
**1,024 字节 × 64 槽 = 64 KB**（`.bss` 里的 `task_table`；`sizeof` 实测
2026-10-05）。**本节早先记的 992 B 已过期**——T-030 加 `struct cred`、
T-032 加 `mem_pages`/`mem_peak` 各推高一次，`task_table` 的起止地址也随之挪动
（`.bss` 里它紧挨着 `cpu_table`，两者之间只有 736 B 的间隙）。
（历史：旧版背着 `signal_mask` + `struct sigaction sa[32]` 共 776 字节/槽，
删掉后随 `sigaction` 实现又以现在的紧凑形式加回。）

⚠️ **`sizeof(struct task)` 不是它自己的事**：`struct cpu_info` 里嵌着一个
**按值**的 `runq_head`，所以 `cpu_info` 的 `rq_next` 偏移（现为 `0x138`）跟着
`task` 走。任何仍按旧头文件编译的目标文件都会在**错误的偏移**上读写运行队列 ——
`pick_next()` 从一个错位字段里取到的"指针"就是非规范地址，下一次 `schedule()`
就是 `#GP`。改 `struct task` 必须**全量重编**（`make clean`），不能只看时间戳。

#### 16.2 `struct cpu_info`（`include/sys/percpu.h`）

```c
struct cpu_info {
    u32 id, apic_id;                /* 逻辑 CPU 号 / APIC ID */
    bool started, bsp;              /* bsp: jiffies 归它递增 */
    struct task *_current;          /* ← current 宏的来源 */
    struct task *idle;              /* 永不在运行队列上 */
    struct task runq_head;          /* 队列哨兵（runq_lock 保护，见 sched.c） */
    volatile bool need_resched;     /* 本核重调度标志 */
};
```
`CPU_DATA_SIZE = 4096`（只是**数组基址的对齐**，不是元素步长），经 GS 基址访问。
`current` 展开为 `get_current()` → `this_cpu_data()->_current`。
（`kstack_top`/`idle_ctx`/`idle_stack`/`task_lock` 已随死代码与 2026-10-01
清理删除——idle 上下文在 idle 任务里，`task_lock` 从未被真正加锁。）

**实测布局（2026-10-05）**：`sizeof = 1,064 B`，`_current` @ `0x10`、`idle` @ `0x18`、
`runq_head` @ `0x20`（其内部 `rq_next` 因此在 `0x138`）。`cpu_table[8]` 是
`aligned(4096)` 的**数组**，`&cpu_table[i]` 按 1,064 递增——头文件里那句
"Size must be a power of 2" 只是个愿望，实际既不是 1024 也不是 1064 的幂。
`_current`/`idle` 的偏移不受 `struct task` 长度影响，但 **`runq_head` 内部的
偏移完全受影响**（见 §16.1 末尾的 ⚠️）。

#### 16.3 `struct vnode` / `struct file`（`include/sys/vfs.h:12-74`）

```c
struct vnode { u16 type; u64 size; void *fs_data; struct fs_ops *ops; void *mnt_data; };
struct file  { struct file_ops *ops; u64 pos; void *priv; bool is_dir; int flags; int refcnt; };
enum { V_REG, V_DIR, V_CHR };
```

⚠️ `vnode` **不是持久 inode** —— 每次 `open` 在栈上创建并 `memcpy` 到堆。所以两个 fd 各自独立的 `pos` 和 `de.filesize`，无 link count，无共享。

#### 16.4 `struct blkdev`（`include/disk.h`）

```c
struct blkdev {
    const char *name; u64 num_sectors; u32 sector_size;
    long (*read)(struct blkdev*, u64 lba, u32 cnt, void *buf);
    long (*write)(struct blkdev*, u64 lba, u32 cnt, const void *buf);
    void *priv;
};
```

---

### 17. 建议阅读顺序

如果你是第一次读这个代码库：

#### 第一遍：搞清"它能做什么"（约 1 小时）

1. `docs/README.md` —— 功能列表
2. `make run` 看一次实际输出
3. `include/abi/lnxrm_syscalls.def` —— 32 个编号（0–31，连续无空洞），**这是内核对外的全部接口**
4. `include/sys/sched.h` —— `struct task` / `struct intr_frame` / `current` 宏，**这是最核心的头文件**

#### 第二遍：搞清启动（2 小时）

5. `kernel/main.c` —— **整个 bring-up 顺序一目了然**，先读这个
6. `arch/kernel.ld` —— 52 行，理解 VMA/LMA
7. `arch/entry64.S` 的 `_start64`（`:161-289`）—— 页表怎么建的
8. `kernel/mm/vmm.c` 文件头注释（`:1-11`）—— **三槽模型的说明写得比任何文档都好**

#### 第三遍：搞清内存（2 小时）

9. `kernel/mm/pmm.c` → `kernel/mm/kheap.c` → `kernel/mm/vmm.c`
10. 重点：`vmm_new_user_aspace` 的复制循环、`dup_user_aspace` 的深拷贝

#### 第四遍：搞清进程与调度（3 小时）

11. `kernel/sched.c` 文件头注释 → `schedule()` → `switch_to()`
12. `arch/entry64.S` 的 `swtch` / `glue_first`（`:475-511`）
13. `kernel/task.c` 的 `sys_fork` → `sys_execve` → `kernel_spawn`
14. `kernel/signal.c` 的 `send_signal` + `do_signal_check`

#### 第五遍：搞清中断与 SMP（2 小时）

15. `arch/entry64.S` 的 `PUSHALL`/`POPALL` 宏和 `isr_stub_table`
16. `arch/cpu.c` 的 `idt_init` + `gdt_fill_one`
17. `kernel/isr.c` 的 `isr_common`
18. `kernel/smp.cpp` 的 `ap_main`（顺序很讲究，逐行都有注释解释为什么）

#### 第六遍：搞清存储（3 小时）

19. `include/sys/vfs.h` 的对象与 `dir_iter.err`
20. `kernel/fs/vfs.c` 的 `vfs_lookup`（`:175-211`）
21. `kernel/fs/fat32/fat32.c` 的 `fat_mount_impl`（挂载 + 整个 FAT 进内存）
22. `kernel/fs/fat32/fat32_file.c` 的读写两个实现

#### 第七遍：按需

23. 图形：`kernel/framebuffer.c` 的分层表（§11.2）
24. 硬件：`kernel/drivers/ahci.cpp`
25. 构建：`Makefile` + 两个 patch 脚本

#### 不要一开始就读

- `kernel/fs/fat32/fat32_meta.c`（745 行，最绕）
- `kernel/framebuffer.c`（702 行，但层次清晰可以跳读）
- `arch/setup.asm`（572 行真模式汇编，除非你要改 VBE 选型）

---

### 18. 全文件清单

#### `arch/`（1,670 行，5 个文件）

| 文件 | 行 | 职责 |
|---|---|---|
| `setup.asm` | 572 | 实模式 setup：E820 采集、VBE 4 遍级联选型、建临时页表、进长模式 |
| `entry64.S` | 513 | `_start32`（GRUB 入口）、`_start64`、中断桩宏与表、`swtch`/`glue_first`/`load_cr3` |
| `trampoline.S` | 108 | AP 侧极简入口，VGA 段技巧取运行地址，操作物理 `0x9000` 共享数据 |
| `cpu.c` | 412 | GDT/TSS（每 CPU 一套）、IDT、IRQ/IPI 表、PIT 编程（模式 2）、LAPIC 定时器标定 + BSP-only `jiffies` + `mdelay`/`tsc_per_ms`、CPUID 型号打印、MSR/TSC |
| `kernel.ld` | 65 | 单一连续 `.image` 段（VMA `0xffffffff80100000` / LMA `0x100000`）+ `.bss` + 启动栈 |

#### `kernel/`（12,190 行，50 个文件）

| 文件 | 行 | 职责 |
|---|---|---|
| `syscall.c` | 448 | 32 槽分发表（全部有实现），`int 0x80`，rax/rdi/rsi/r10；`ps`/`diskinfo`/`fb_*` 在此过 `cap_gate` |
| `fs/vfs.c` | 735 | VFS：vnode/file/fs_ops、fd 表、路径解析、挂载（`vfs_try_mount_one` 逐盘尝试，T-081）、设备节点、read/write/close/dup2、`console_write`（共享打印锁） |
| `framebuffer.c` | 702 | 图形核心：像素格式抽象、CP437 字体、ANSI 控制台、4 个裁剪绘图原语（无双缓冲） |
| `fs/fat32/fat32_meta.c` | 745 | FAT32 目录项增删改 + mkdir/rmdir/unlink/rename/move（错误码一路上传） |
| `print.c` | 373 | 串口 + VGA 文本双后端 + 环形缓冲回放 + `kvprintf` + `console_out_lock` |
| `task.c` | 619 | pid、fork/execve/exit/waitpid、USERCOPY helpers、`kernel_spawn` |
| `sched.c` | 516 | 任务表、per-CPU 运行队列、负载均衡、抢占调度、idle、`switch_to`、`nanosleep` |
| `mm/vmm.c` | 409 | 三槽地址空间、页表 map/unmap/walk、`fork` 深拷贝、MMIO 映射 |
| `smp.cpp` | 262 | AP 拉起（INIT-SIPI-SIPI）、trampoline 部署、`ap_main`（AP 武装本地 tick） |
| `isr.c` | 267 | 中断分发、异常报告、键盘（scancode→ASCII）、1 个输入队列 |
| `signal.c` | 292 | 信号投递、默认处置、`sigaction`/`sigprocmask`/`sigreturn` 链路 |
| `fs/fat32/fat32.c` | 323 | BPB 解析、挂载、FAT 链读写、空闲簇查找、限流 I/O 告警 |
| `mm/pmm.c` | 253 | buddy 物理页分配器 + 逐 order 位图 + MMIO 保留 + 可用量统计接口（`pmm_total_bytes` 零引用已删） |
| `fs/fat32/fat32_dir.c` | 258 | 目录扫描、LFN 长文件名重建、路径解析（三态返回） |
| `fs/fat32/fat32_file.c` | 260 | 文件读写、簇链分配（读写失败回 EIO / 短计数） |
| `fs/blk_cache.c` | 321 | 64 项时间戳 LRU 扇区缓存（`used` 已删）+ `blk_io_*`（唯一驱动调用点）+ 故障注入 + `blk_cache_stats/lookup/purge` |
| `drivers/ide.c` | 194 | IDE PIO 驱动（28 位 LBA 命令 + 48 位容量取值，T-080） |
| `fs/mbr.c` | 155 | MBR 分区表解析（**标准镜像下运行时不可达**：整盘 FAT 挂载先成功，见 §9.5） |
| `apic.c` | 139 | LAPIC + IOAPIC 驱动、IPI 发送 |
| `mm/kheap.c` | 141 | 内核堆（分离空闲链，>4 KiB 走 buddy） |
| `drivers/ahci.cpp` | 398 | AHCI 驱动（H2D FIS、命令表、PRD、DMA；完成判定含 `PxTFD.ERR`，T-080） |
| `fs/fat32/fat32_journal.c` | — | **已删除**：写路径改走块缓存写回（见 §9.4） |
| `elf.c` | 100 | 静态 ELF64 装载器（ET_EXEC only） |
| `drivers/pci.c` | 81 | PCI 配置空间枚举（2026-10-06 起带桥递归 visited + 深度上限） |
| `ansi.c` | 31 | ANSI 转义序列三状态机 |
| `percpu.c` | 39 | per-CPU 数据初始化、GS 基址读写 |
| `lib/string.c` | 90 | `memcpy` `memset` `strcmp` `strcpy` `strlen` |
| `spinlock.c` | 21 | 自旋锁 2 个函数（irqsave 对；裸 `spin_lock/spin_unlock/spin_init` 零引用已删） |
| `mm/SlabAllocator.cpp` | 19 | 全局 `operator new` → kmalloc（内核从不 `delete`，故无 delete 重载） |
| `main.c` | 368 | **bring-up 顺序**（console/GDT/IDT 在最前，见 §22.6）、E820 表/汇总打印、内核镜像范围、VBE/cmdline 采集、`lapic_timer_calibrate`、`kernel_spawn("/bin/init")`、启动汇总（可用内存 + ready 耗时） |
| `rust/uart.rs` | 106 | 16550 UART 驱动（Rust） |
| `rust/lib.rs` | 57 | crate 根、`#[panic_handler]` |
| `rust/smp.rs` | 133 | `TrampolineData` + 四个访问器 |

**上表 32 个文件 = 8,855 行**；另有 `kernel/protect.c`（38 行）与
`ktest/`（17 个文件、3,297 行）—— 32 + 1 + 17 = **50 个文件、12,190 行**，
与上面的小标题一致（`grep` 实测，逐行 `wc -l` 对齐）。
`ktest/` 是启动自测套件，用 `KTEST(suite, stage, fn)` 注册进 `.ktest` 段，
分 early/mm/late 三阶段在 `start_kernel` 里跑，各章"守着它的测试"指的就是它们；
`scripts/smoke_test.py`（762 行）再把用户态那半接上。

#### `include/`（2,169 行，23 个文件）

`sys/sched.h` 167 · `sys/cpu.h` 127 · `sys/vfs.h` 144 · `sys/ktest.h` 99 ·
`sys/cred.h` 72 · `mm/mm.h` 102 · `fat32_priv.h` 247 · `abi/lnxrm_abi.h` 224 ·
`boot.h` 107 · `console.h` 111 · `framebuffer.h` 114 · `sys/smp.h` 71 ·
`sys/apic.h` 62 · `types.h` 67 · `io.h` 40 · `elf.h` 43 · `sys/percpu.h` 42 ·
`abi/lnxrm_syscalls.def` 54 · `disk.h` 157 · `sys/spinlock.h` 30 · `sys/protect.h` 36 ·
`pci.h` 26 · `cpp/smp.hpp` 27

#### `init/`（239 行）· `usr/`（1,529 行，16 个程序）· `scripts/`（967 行，4 个脚本）

见 §12 和 §15。

---

### 19. 读代码时容易踩的坑

这些不是 bug，是**设计上容易误解的地方**，我在读代码时花过时间：

| 坑 | 说明 |
|---|---|
| **三处硬编码必须手工同步** | `entry64.S:4-10` 的页表物理地址、`vmm.c:17-18` 的 `PD_HI`/`PT_FIX`、`types.h:37-42` 的 VMA 槽号。注释只在两处提醒 |
| **`FIXMAP_VA` 已整体删除** | 曾经头文件声称"用于远程编辑其他 AS 的 PTE"但全树零引用（最后的使用者 `signal.c` 临时别名已随信号 trampoline 删除）。2026-10-01 连同 `mm.h` 重复定义、fixmap 概念、`vmm.c` 对 slot 97/112 的跳过一起删掉 |
| **`vmm_map_user` 的 `writable` 无效** | 因为 `CR0.WP`/`CR4.WP` 未置位，只读页实际可写（见 GAP_ANALYSIS 4.2） |
| **运行队列不是环形** | 注释说 circular，实际 push-front 无尾指针 ⇒ LIFO；`schedule()` 重排 current 走 `runqueue_add_local()` 防跨核双跑（2026-10-01） |
| **写路径是块缓存写回，不是 journal** | 原 `fat32_journal` 已删除；写经 `blk_cache_write`，无日志、崩溃不一致 |
| **`vnode` 不是 inode** | 每次 open 一份拷贝，无 link count，两 fd 状态独立 |
| **`console_write` 与 `kvprintf` 共用一把锁** | `console_out_lock`（irqsave，64 字节分块）——2026-10-01 起不再裸奔（C1 已修） |
| **`PIT` 不驱动时钟** | BSP 与每个 AP 各有 LAPIC 周期 tick（period 来自 BSP 的 `lapic_timer_calibrate`），PIT 只提供标定窗口（必须模式 2，模式 3 会跑 200 Hz）；`jiffies` 只在 BSP 上递增 |
| **`kstack` 里藏了两样东西** | 顶部是 `intr_frame`，其下 56 字节是 `cpu_ctx`（`area[6] = glue_first`） |
| **`current` 是宏不是变量** | `CONFIG_SMP 1` → `#define current (get_current())` → `this_cpu_data()->_current` |
| **`/bin/desktop` 从不存在过** | 曾经的根 README 和 `help` 都宣传过它，两处都已改正；`usr/desktop.c` 从来没写过 |
| **AHCI 在 `make run` 下零执行** | QEMU 参数是 `if=ide`，机器上没有 SATA 控制器 → `ahciProbe` 对所有设备 class 不匹配提前返回，读写路径从未运行过（§10.3） |
| **`boot` 目录是 0x100000 起** | LMA 与 VMA 差 `0xffffffff80000000`；`objcopy -O binary` 按 LMA 导出 |

---

*文档版本 1.1 · 行数统计基于 `wc -l`（含注释与空行）· 已删代码只在「已删除」处留痕*

---

## 第二部分 · 功能总览

> 面向：想知道"这个内核到底能干什么"的人
> 文档性质：能力清单，逐项列出**已实现的功能**并给出代码/实测出处
> 实测日期：2026-10-06（`make smoke` PASS 176/176，`make run` 起到 `# ` 提示符）
> 承接本文第一部分（代码导览）；构建与调试见 [BUILD.md](BUILD.md)，
> 差距 · 评审 · 行动计划见 [GAP_ANALYSIS.md](GAP_ANALYSIS.md)

本文所有数字都标注了出处：`文件:行` 表示源码位置，"实测"表示 2026-10-06 当日运行结果。
**没有出处的数字不会写进本文**；口径差异集中列在 §35。

---

### 20. 一分钟概览

| 维度 | 现状 | 出处 |
|---|---|---|
| 架构 | x86-64 单体内核，ring 0/ring 3，SMP 最多 8 CPU | `include/sys/apic.h:33` `MAX_CPUS 8` |
| 语言 | ASM + C + C++ + Rust，**97 个源文件 / 17,706 行** | 实测（口径见 §35） |
| 启动 | Linux x86 boot protocol **2.08** 的 bzImage；QEMU `-kernel` 或 GRUB 双路径 | `arch/setup.asm:25` |
| 系统调用 | **32 个（编号 0–31 连续无空洞），全部已实现** | `include/abi/lnxrm_syscalls.def:23-54` |
| 内存 | buddy 物理页 + 分箱内核堆 + 三槽 PML4 虚拟内存；NX/WP/SMEP/SMAP 全开 | `kernel/mm/pmm.c:10`、`kernel/mm/vmm.c` 文件头 |
| 进程 | 64 槽任务表，fork/execve/exit/wait4，ELF64 静态加载 | `include/types.h:60`、`kernel/elf.c` |
| 调度 | 每 CPU 环形 runq + 全局锁，轮转抢占，100 Hz tick | `kernel/sched.c`、`include/sys/cpu.h:123` |
| 文件系统 | VFS + FAT32（8.3 与 LFN，**大小写按磁盘原样**）+ 块缓存 + MBR | `include/sys/vfs.h:29`、`include/fat32_priv.h:117` |
| 设备驱动 | PCI 枚举、IDE PIO、AHCI DMA、PS/2 键盘、COM1 串口、VBE 帧缓冲 | `kernel/drivers/`、`kernel/apic.c` |
| 凭据 | 三级（user/root/kxld）+ 3 个能力位 + 64 MiB 内存配额 | `include/sys/cred.h:19-21,33-35,66` |
| 信号 | 32 个信号槽（26 个已命名），sigaction/sigprocmask/sigreturn | `include/abi/lnxrm_abi.h:88`、`kernel/signal.c` |
| 用户态 | 16 个程序 + shell，1,768 行 | 实测（见 §31） |
| 自测 | 启动自测 **72 cases / 1546 checks**；端到端冒烟 **176/176** | 实测（见 §32） |

---

### 21. 引导与启动

#### 21.1 双启动路径

* **QEMU `-kernel`**：`make run` 直接加载 `build/bzImage`（`Makefile:158-161`）。
* **GRUB `linux`**：**`make imageiso` 生成 `build/lnxrm.iso`**（`Makefile:174`，暂存树
  `build/iso/`，`grub.cfg` 由 Makefile 生成），里面是 `linux /boot/bzImage`、`timeout=0`、
  **`gfxpayload=1280x720x32`**。GRUB 路径下 `setup.asm` 不执行，所以必须由 GRUB 自己编程
  VBE 模式并填 `boot_params.screen_info.lfb_*`。
  判据是 `arch/entry64.S:_start32` 把 `boot_params` 指针存进物理 `0x6F00`（DWORD），
  `kernel/main.c` 读到非 0 就走这条路径取 E820 / LFB / cmdline。
  内核没有 EFI handover stub（`handover_offset=0`），只支持 legacy BIOS/CSM。
  ⚠️ **`gfxpayload` 必须写 `x32` 而不是 `,32`**：逗号形式是 GRUB 的「后备模式列表」
  语法，`1280x720,32` 会被读成「1280x720，然后 32」，实测拿到的是 **24 bpp**
  （pitch 3840）；`1280x720x32` 才真的编程 32 bpp 模式（pitch 5120）。这条写在
  `Makefile:168-173` 的注释里。
* ✅ **已实测（2026-10-06，原 C68「完全未经测试」解除）**，三种配置都过：
  * **`make imageiso` + `gfxpayload=1280x720x32`**（默认配置）
    → `[fb] lfb 1280x720 **32 bpp** phys=0xfd000000 pitch=5120 mode=0x0`、
    selftest **1546/1546**、`[task] spawned pid=1 (/bin/init)`、shell 打出 README 并给
    `# ` 提示符、`[boot] ready in 1255 ms`、0 三重故障、0 PANIC，启动后
    `fsck.vfat -n` 仍干净（`-m 2048 -boot d`）
  * 无 framebuffer（GRUB 留文本模式）→ `[fb] no linear framebuffer`，selftest **1530/1530**
    （4 个 fb 用例 skip），`[boot] ready in 1612 ms`
  * `gfxpayload=1280x720,32`（**错写法**，留档）→ `[fb] lfb 1280x720 24 bpp
    phys=0xfd000000 pitch=3840 mode=0x0`，selftest **1546/1546**，`ready in 2483 ms`
    —— `mode=0x0` 与 QEMU 路径的 `0x18f` 不同，正是「确实读的是
    `boot_params.screen_info` 而不是 `0x8C00`」的证据
  * 三条都无三重故障、无 PANIC。命令见 `docs/BUILD.md` §8.2

  ⚠️ **ISO 不含根文件系统**：`imageiso` 只打包 bootloader + 内核，`disk.img` 仍是
  独立的 IDE 盘，所以 `-cdrom` 之外还必须 `-drive file=build/disk.img,...`，否则
  `[vfs] no disk found` → `[init] cannot open /bin/init` → `Stop!`。同理，`disk.img`
  自身损坏（半成品 `mcopy`）也是这个表现：`mdir -i build/disk.img ::bin` 报
  `Fat problem while decoding` 时 `rm build/disk.img && make build/disk.img` 重建。

#### 21.2 引导协议与实模式阶段

* 协议版本 **2.08**（`arch/setup.asm:25` `version: dw 0x0208`），头部按
  `Documentation/arch/x86/boot.rst` 的偏移摆放（`arch/setup.asm:2,12`）。
* 实模式收集 **E820 内存图**，`start_kernel` 逐条打印 `[mem] e820 ...` 并汇总
  usable/reserved（`kernel/main.c:205` 起，`[boot] N usable e820 entries`）。
* **VBE 模式级联**（`arch/setup.asm:147-221`）四趟：
  1. pass 0：首选分辨率 1280x720 → 1920x1080，深度 32 bpp → 24 bpp → 任意真彩；
  2. pass 1：按 VRAM 自适应挑最大的真彩模式；
  3. pass A：经典 VESA 8 bpp LFB 模式 0x101 / 0x103 / 0x105；
  4. pass B：第一个任意深度的 BIOS 模式。
  候选模式必须通过 **4F01 校验 + 4F02 真实设模式**，并满足 VRAM 容量与内核映射的
  16 MiB LFB 窗口（`arch/setup.asm:155-161` 注释）。
* 实测（2026-10-06）：`[fb] lfb 1280x720 32 bpp phys=0xfd000000 pitch=5120 mode=0x18f`。
* **debugcon 0xE9 分段标记**：`arch/entry64.S` 7 处、`arch/setup.asm` 3 处
  （`grep -c "out 0xE9"`），QEMU 下可用来定位死在哪一段。

#### 21.3 内核 bring-up 顺序

`kernel/main.c:210 start_kernel()` 的实际次序（行号即调用点）：

```
console_init → gdt_init → cpu_set_gs_base(&cpu_table[0]) → idt_init   main.c:221-224
  ← 头四步。IDT 之前任何异常 = 三重故障复位（见 §22.6）
  → boot_collect_e820        main.c:226
  → boot_read_vbe / boot_read_cmdline   main.c:252-253
  → ktest_run(KTEST_EARLY)   main.c:265
  → cpu_init_percpu(0,0)     main.c:268
  → pmm_reserve ×5（LFB / FB_PD / slot96 / slot128 / slots256..271）  main.c:271-304
  → pmm_init / vmm_init / kheap_init / cpu_init / pit_init   main.c:306-310
  → ktest_run(KTEST_MM)      main.c:314
  → fb_init                  main.c:317
  → serial_irq_init / ps2_kbd_init      main.c:319-320
  → vfs_init                 main.c:322
  → ide_init / ahci_init     main.c:326-327
  → do_global_ctors（C++ 全局构造） main.c:333
  → sched_init               main.c:335
  → ktest_run(KTEST_LATE) + ktest_summary   main.c:339-340
  → kernel_spawn("/bin/init") main.c:342
  → lapic_timer_calibrate / smp_init / lapic_timer_start(32, HZ)   main.c:351-356
  → sti → idle_loop          main.c:366-367
```

实测启动日志（2026-10-06，`build/smoke/serial.log`）关键行：

```
[fb] lfb 1280x720 32 bpp phys=0xfd000000 pitch=5120 mode=0x18f
[kbd] PS/2 ready, IRQ1 -> vector 33
[serial] COM1 ready, IRQ4 -> vector 36
[blk] registered hda (131072 sectors, 64 MiB)
[vfs] FAT32 mounted at / (whole disk)
[smp] 2 CPUs online
[mem] available memory 227.7 MiB
[boot] ready in 1020 ms
```

#### 21.4 启动日志的可观测性

* 三阶段自测打印 `[ktest] stage {early,mm,late}: N cases, M checks, 0 failed`。
* 保护特性逐项回读：`[vmm] EFER.NXE=1`、`[cpu] CR0.WP=1 CR4.SMEP=1 CR4.SMAP=1`
  ——这些是 smoke 的有序 marker，不匹配就判失败（`scripts/smoke_test.py:52` BOOT_MARKERS，共 **20 条**）。
* 磁盘容量行必须是 `64 MiB` 而非 `2097151 MB`（T-080/T-081 修复，见 §28.4）。

---

### 22. 内存管理

#### 22.1 物理页分配器（buddy）

* `MAX_ORDER 11`、`MIN_ORDER_BITS 12`（即最小块 4 KiB，`kernel/mm/pmm.c:10-11`）。
* **侵入式空闲链**：每个空闲块头 8 字节放下一个指针；每阶另配位图，用来找 buddy 合并
  （`kernel/mm/pmm.c:1-5`）。
* 位图放在低地址安全区 `BITS_BASE 0x10000`（64 KiB，位于 trampoline/GDT/E820 之上、
  EBDA 之下，`kernel/mm/pmm.c:24-26`）。
* **保留区机制**：内核映像等 `reserved_lo/reserved_hi` 永不分配；LFB 等 MMIO 帧
  永不下发（`kernel/mm/pmm.c:29-30` 注释）。
* 对外接口：`pmm_init`、`pmm_free_bytes`、`pmm_init_reserve`、`pmm_reserve`、
  `pmm_release_mmio`、`pmm_free_pages_no_lock`（`include/mm/mm.h:65-73`）。
* 实测启动后余量：`[mem] available memory 227.7 MiB`（`-m 256` 的 256 MiB 扣掉保留区）。

#### 22.2 内核堆（kmalloc）

* **9 个分箱**（`KHEAP_NBINS 9`，`include/mm/mm.h:25`），`kheap_bin_of()` 把
  `size` 映射到箱；**≥ 4096 字节直接走 buddy**，≤ 8192 词的请求在分箱里处理
  （`include/mm/mm.h:29,70`）。
* 块头 8 字节，含 bin/order 与魔数 `HEAP_MAGIC 0xECEC0001`，可检测越界写
  （`include/mm/mm.h:60`）。
* **分配失败返回 NULL 而不是 panic**（T-004，`include/mm/mm.h:67` 注释）；
  内核里唯一的 panic 路径是 C++ 的 `operator new` → `kmalloc_or_panic`
  （`kernel/mm/SlabAllocator.cpp` 文件头，只在 AHCI 探测这类 bring-up 路径上）。

#### 22.3 虚拟内存（三槽 PML4）

`kernel/mm/vmm.c` 文件头给出的地址空间模型：

| 槽 | 内容 | 说明 |
|---|---|---|
| slot 0 | 低内存恒等映射 | 所有 AS 共享，仅内核可见 |
| slot 255 | 进程用户空间 `[0x7f8000000000, 0x7fffffffffff]` | 每进程独立 |
| slot 511 | 物理内存高半区别名 | 内核映像、堆、设备窗口、VGA，全 AS 共享 |

* `vmm_new_user_aspace()` 复制除 slot 255 外的所有 PML4 条目，内核映射自动传播到所有任务。
* 关键常量：`ALIAS_BASE 0xffffffff80000000`、`USER_SLOT 255`、`PD_HI 0x54000`、
  `pml4_phys 0x50000`（`kernel/mm/vmm.c:21-26`）。
* 用户侧边界：`USER_BASE 0x7f8000000000`、`USER_STACK_TOP 0x7fffffbff000`、
  `USER_MAX_VMA 0x7ffffffff000`、信号 restorer 页 `USER_SIGRETURN_VA = USER_MAX_VMA - PAGE_SIZE`
  （`include/types.h:48-58`）。
* 设备窗口：`DEV_VMA 0xffffffff90000000`、`KHEAP_VMA 0xffffffffa0000000`
  （`include/types.h:40-41`）；MMIO 用 `vmm_devmap_mmio_4k` 单页映射（`include/mm/mm.h:84-86`）。

#### 22.4 保护特性（全部开启并有自测）

| 特性 | 状态 | 验证 |
|---|---|---|
| NX（`EFER.NXE`） | 开 | smoke marker `[vmm] EFER.NXE=1` |
| `CR0.WP` | 开 | smoke marker + `test_cr0_wp_on`（`ktest/t_cpu.c`） |
| SMEP | 开 | smoke marker + `test_cr4_smep_on`（t_cpu.c:97 无该 CPU 特性则 skip） |
| SMAP | 开 | smoke marker + `test_cr4_smap_on`（t_cpu.c:113） |
| 用户指针拷贝 | `copy_from_user`/`copy_to_user`/`copy_user_str` 检查目标 R/W 位 | `kernel/syscall.c` |

#### 22.5 用户堆 brk 与配额

* `sys_brk(0)` 返回当前 brk；合法区间 `[brk_base, USER_STACK_TOP - 1 MiB)`
  （`kernel/syscall.c:14-18`）。
* **T-032：先拒绝再取帧**。`vmm_count_user_pages()` 数出进程当前占用（与 `ps` 同一条
  walk，避免两个数字对不上），超 `CRED_USER_MEM_QUOTA_PAGES = 64 MiB / 4096`
  （`include/sys/cred.h:66`）直接返回 `LNXRM_ENOMEM`，一个帧都不拿。
* 部分增长失败会回滚（`kernel/syscall.c:40-52` 注释"rollback the partial growth"）。

#### 22.6 启动早期三重故障（2026-10-06 修复）

**现象**：真机上串口闪过几行就整机重启，看不到 `Stop!`。

**为什么看不到 panic**：`panic()` = `cli; hlt; jmp .`（`kernel/print.c:371`），只挂死不复位。
所以「重启」只可能是**三重故障**（CPU 连续两次异常且第二次没有 IDT 可用 → 复位）。

**根因链**（QEMU `-m 2048` 下复现，`addr2line` 定位到 `pmm.c:222`——**修复前行号**，
现播种裁剪在 `pmm.c:105`）：

```
pmm_init 播种走到 pa = 0x40000000
  → 恒等映射/高半区别名只有 1 GiB（entry64.S 只填 PDPT_M[0]、PDPT_HI[510]）
  → 按恒等地址解引用 → #PF (CR2=0x40000000)
  → 此刻还没有 IDT（全树唯一 lidt 在 arch/cpu.c:120，由 main.c 的 cpu_init() 调用，
    而 pmm_init 早于 cpu_init）→ #DF
  → 三重故障 → 无痕复位
```

QEMU 的既有测试全是 `-m 256`，管理窗口上限 4 GiB 但实际 RAM 只有 256 MiB，
永远碰不到 1 GiB 边界，所以一直没暴露。

**修复**（三处，缺一不可）：

| 改动 | 位置 |
|---|---|
| 管理窗口钳到 `PMM_WINDOW_TOP = 0x40000000`（= 恒等映射范围），整段落在窗口外的区域直接跳过 | `include/mm/mm.h:20`、`pmm.c:105-106`、`vmm.c:237` |
| **IDT/GDT 提前到 `start_kernel` 头四步**，此后任何早启动异常都有处理器而不是复位 | `main.c:214-224` |
| 无可用 RAM 时 `panic` 而不是去 `memset` 抹掉自己的页表 | `pmm.c:122-124` |

附带修的两处真机专属问题（QEMU 下测不出来）：`0x6F00` 哨兵按 `u64` 读但两个写入者
只写 DWORD（见 §3.2）；两个高半区窗口（slot 128 / 256..271）占用的帧没被
`pmm_reserve`（`main.c:293-304`）。

**代价**：内存 > 1 GiB 的部分不再被管理。`-m 2048` 下 `[mem] available memory 967.7 MiB`。
要吃满内存必须重构别名基址——`PHYS_TO_VIRT(p) = p + 0xffffffff80000000`
（`include/types.h:43`）在 **2 GiB** 处就回绕进低规范半区，窗口若越过 1 GiB
先撞的是这个（`include/mm/mm.h:10-19` 注释）；`vmm_init` 那边还有第二个独立上限：
超过 1 GiB 后 `(pa >> 21) & 511` 会绕回槽 0，把内核自己的别名指到别人的物理内存
（`kernel/mm/vmm.c:234-236` 注释）。

**验证**（2026-10-06）：`-m 64/256/2048/4096` 全部 `selftest PASS` + `[boot] ready`、
0 次三重故障；`make smoke` 176/176；GRUB ISO 路径见 §21.1。

---

### 23. 进程、调度与 SMP

#### 23.1 任务与生命周期

* 任务表 **64 槽**（`NR_TASKS 64`，`include/types.h:60`），每任务 **16 个 fd**
  （`NR_FDS 16`，`include/types.h:61`）、**32 KiB 内核栈**（`KSTACK_SIZE 32768`，
  `include/sys/sched.h:111`）。
* 状态机：`T_UNUSED / T_EMBRYO / T_RUNNABLE / T_RUNNING / T_SLEEPING / T_ZOMBIE / T_STOPPED`
  （`include/sys/sched.h:44-51`）。
* `struct task` 字段：`pid`、`cpu_id`、`state`、`name[16]`、`cred`（fork 时复制）、
  `tf`（指向内核栈上的 `intr_frame`）、`ctx`（swtch 的保存栈指针）、`kstack/kstack_top`、
  `pml4`（CR3）、`brk_base/brk_cur`、内存用量统计、`fds[16]`、信号相关字段
  （`include/sys/sched.h:56-81`）。
* 系统调用侧提供 `fork / execve / exit / wait4 / kill / getpid / getppid / ps`
  （`kernel/task.c` 文件头："pid allocation, fork/execve/exit/waitpid, the user
  memory access helpers and /bin/init spawning"）。
* `wait4` 支持 **WNOHANG（opts bit 0）**（`usr/systest.c:318` 注释）。

#### 23.2 ELF 加载

* **只接受静态 `ET_EXEC`** 的 ELF64：魔数、`e_type != 2` 直接拒绝并打印
  （`kernel/elf.c:1-18`）。
* `e_entry` 必须落在 `[USER_BASE, USER_MAX_VMA)`，否则拒绝——因为返回后立刻
  ring 3 跳转（`kernel/elf.c:22-26` 注释，自测 `ktest/t_elf.c` 用野 entry 验证拒绝）。
* 装载结束返回 `brk_end` 供 `brk` 使用。
* **不支持**：动态链接、共享库、需求分页（见 §34）。

#### 23.3 调度器

* **每 CPU 环形 runq**，加一把全局 `runq_lock`（irqsave）；**idle 不入队**
  （`kernel/sched.c` 文件头注释）。
* 轮转抢占，`sched_tick()`（`kernel/sched.c:269`）由每 CPU 的 tick 调用
  （`arch/cpu.c:166`）。
* **跨 CPU 负载均衡放在唤醒路径上**：唤醒远端 idle 时发
  `IPI_VECTOR_RESCHEDULE`（`kernel/sched.c:125-127`），注释明确写了"同 CPU 唤醒不发 IPI、
  便宜（无 IPI）；跨 CPU 均衡在 wakeup 时发生"（`kernel/sched.c:133`）。
* 上下文切换用 `cpu_ctx { u64 sp; }` 保存内核栈指针（`include/sys/sched.h:38-41`）。

#### 23.4 时钟

* tick 频率 **HZ = 100**（`include/sys/cpu.h:123`），全局 `volatile u64 jiffies`（`include/sys/cpu.h:122`）。
* **BSP 只标定一次 LAPIC period**，AP 用缓存值武装，避免多 CPU 抢 PIT 锁存端口
  （`kernel/main.c:318-321` 注释）。
* BSP LAPIC timer → **vector 32** → `pit_handler`；注释说明 PIT/IOAPIC IRQ0 在该平台
  不置 LAPIC IRR（`kernel/main.c:323-325`）。

#### 23.5 SMP

* 最多 **8 CPU**（`MAX_CPUS 8`，`include/sys/apic.h:33`、`include/sys/percpu.h:11`）。
* LAPIC/IOAPIC 均为 MMIO，映射进 `DEV_VMA` 窗口（`kernel/apic.c:1-10`）。
* IPI 向量 **0xF0–0xF2**：`IPI_VECTOR_RESCHEDULE 0xF0`、`IPI_VECTOR_STOP 0xF2`
  （`include/sys/smp.h:6-8`）；处理函数按向量注册（`include/sys/cpu.h:108-111`）。
* 每 CPU 独立 GDT/IDT/TSS（`gdt_init_cpu`，`include/sys/cpu.h:17`）。
* AP 启动走 `arch/trampoline.S`，`kernel/smp.cpp` 负责 INIT-SIPI 与 bring-up。
* 实测：`[smp] BSP online, APIC ID=0` / `AP1 online (APIC ID=1)` / `2 CPUs online`。
* 自旋锁：`spinl` + irqsave 变体，自测 `test_spinlock_irqsave`（`ktest/t_cpu.c`）。

---

### 24. 中断、异常与保护

#### 24.1 异常处理与诊断

* 异常名表 `exc_names[32]`（`kernel/isr.c:129`）；异常统一打印：

  ```
  [exception] <name> (<n>) err=<hex> rip=<hex> rsp=<hex> cr2=<hex>
  ```
  （`kernel/isr.c:159-160`）
* **#PF 错误码逐位解码**（`kernel/isr.c:162-`）：protection/not-present、write/read、
  user/supervisor、reserved-bit、instr-fetch；并利用 `f->rflags`（硬件压栈的 pre-clac 值）
  的 AC 位区分 **WP 违例与 SMAP 违例**——两者错误码完全相同，注释解释了判别逻辑
  （`kernel/isr.c:166-172`）。
* 用户态故障转成信号（SIGSEGV 等）而不是直接 panic；`SIGFPE/SIGBUS` 由 CPU 异常路径
  直接走到退出，因此不在默认终止表里（`kernel/signal.c:14-16` 注释）。

#### 24.2 panic

* `panic()` 打印 `\033[1;31m[PANIC]` + 消息 + 复位色，然后 `cli; hlt; jmp .`
  （`kernel/print.c:363-375`）。
* 断言三件套 `ASSERT / WARN / BUG_ON`（`include/console.h:24-38`）：
  `ASSERT` 失败计入自测失败，`WARN` 只计数永不判负，`BUG_ON` 表示状态不可信，
  在 `ktest=continue` 下也停机。**当前全局 `[1 warn]` 是刻意保留的**
  （`ktest/t_lib.c:250` 的 deliberate warning）。

#### 24.3 中断向量分配

| 向量 | 用途 | 出处 |
|---|---|---|
| 0–31 | CPU 异常 | `kernel/isr.c:158` |
| 32 | LAPIC timer → `pit_handler` | `kernel/main.c:324` |
| 33 | PS/2 键盘（IOAPIC IRQ1） | `kernel/main.c:169` |
| 36 | COM1 串口（IRQ4） | 实测 `[serial] COM1 ready, IRQ4 -> vector 36` |
| 0xF0–0xF2 | IPI（reschedule/stop） | `include/sys/smp.h:6-8` |

* spurious IRQ（IRQ7/IRQ15）有专门分支，避免卡死向量（`kernel/isr.c:251-256` 注释）。

---

### 25. 系统调用与 ABI

#### 25.1 调用约定

* 入口 `int 0x80`，**编号在 `rax`，参数在 `rdi rsi r10`**（`kernel/syscall.c:1`）。
* 分发表是一个大 `switch (nr)`（`kernel/syscall.c:299`），`default:` 返回
  `LNXRM_ENOSYS = -38`（`kernel/syscall.c:418`）。
* 编号定义在 X-macro 文件 `include/abi/lnxrm_syscalls.def`，**连续 0–31、无空洞**
  （文件头注释 18-21 行明确规定"只追加、不重排"规则）；自测
  `test_syscall_table_contiguous` 验证连续性（`ktest/t_abi.c`）。

#### 25.2 全部 32 个系统调用

| # | 名字 | 用途 | 实现位置 |
|---|---|---|---|
| 0 | `read` | 从 fd 读（含 `/dev/console` 阻塞读） | `kernel/fs/vfs.c:549` |
| 1 | `write` | 写 fd | `kernel/fs/vfs.c` |
| 2 | `open` | 打开/创建，flags 见 §25.3 | `kernel/fs/vfs.c:302` |
| 3 | `close` | 关闭 | `kernel/syscall.c:312` |
| 4 | `lseek` | SEEK_SET/CUR/END | `include/abi/lnxrm_abi.h:127-129` |
| 5 | `brk` | 调整用户堆，带 64 MiB 配额 | `kernel/syscall.c:14` |
| 6 | `getdent` | 目录枚举（cookie 迭代） | `include/sys/vfs.h:35` |
| 7 | `dup2` | 复制 fd | `kernel/syscall.c:324` |
| 8 | `nanosleep` | 睡眠；被信号打断返回 EINTR 并回填余量 | `kernel/syscall.c:327` |
| 9 | `getpid` | 进程号 | `kernel/syscall.c:330` |
| 10 | `fork` | 复制进程（cred 随行复制） | `kernel/task.c` |
| 11 | `execve` | 加载静态 ELF64 | `kernel/elf.c`、`kernel/task.c` |
| 12 | `exit` | 退出 | `kernel/task.c` |
| 13 | `wait4` | 等子进程，支持 WNOHANG | `kernel/task.c` |
| 14 | `kill` | 发信号，带 kxld/uid/能力三道闸 | `kernel/syscall.c:84-107` |
| 15 | `uname` | `lnxrm / v0.08-TEST / x86-64` | `kernel/syscall.c:203` |
| 16 | `sigaction` | 装信号处理器 | `kernel/signal.c` |
| 17 | `sigprocmask` | BLOCK/UNBLOCK/SETMASK | `include/abi/lnxrm_abi.h:113-115` |
| 18 | `getppid` | 父进程号 | — |
| 19 | `ps` | 内核填表（含内存列，见 §26.3） | `kernel/task.c:589` |
| 20 | `sigreturn` | 从处理器返回，恢复被中断现场 | `kernel/signal.c:96` |
| 21 | `diskinfo` | 28 字节打包记录（16B 名 + u32 扇区大小 + u64 扇区数） | `usr/systest.c:366-371` |
| 22 | `mkdir` | 建目录 | `kernel/fs/fat32/fat32_meta.c` |
| 23 | `fb_info` | 读帧缓冲几何（**不需 CAP_FB**） | `kernel/syscall.c:385` |
| 24 | `fb_clear` | 清屏，需 CAP_FB + 单一持有者 | `kernel/syscall.c:386` |
| 25 | `fb_fill` | 矩形填充 | `kernel/syscall.c:391` |
| 26 | `fb_char` | 画字符 | `kernel/syscall.c:396` |
| 27 | `fb_puts` | 画字符串 | `include/abi/lnxrm_syscalls.def:50` |
| 28 | `unlink` | 删文件 | `include/fat32_priv.h:244` |
| 29 | `rmdir` | 删空目录 | `include/fat32_priv.h:243` |
| 30 | `rename` | 改名/移动 | `include/fat32_priv.h:245` |
| 31 | `setuid` | 单向降权（配合能力闸测试） | `kernel/syscall.c:191` |

#### 25.3 open 标志与错误码

* 标志：`O_RDONLY 0`、`O_WRONLY 1`、`O_CREAT 0100`、`O_TRUNC 01000`、
  `O_CLOEXEC 010000`（`include/sys/vfs.h:49-53`）。
  `O_CLOEXEC` 在 `execve` 时被关闭（`kernel/task.c:250-252`）。
* **21 个错误码**（`include/abi/lnxrm_abi.h:7-27`）：
  `EFAIL -1, ENOENT -2, ESRCH -3, EINTR -4, EIO -5, ENOEXEC -8, EBADF -9,
  ECHILD -10, EAGAIN -11, ENOMEM -12, EACCES -13, EFAULT -14, EEXIST -17,
  EXDEV -18, ENOTDIR -20, EINVAL -22, EMFILE -24, ENOSPC -28,
  ENAMETOOLONG -36, ENOSYS -38, ENOTEMPTY -39`。

#### 25.4 ABI 结构体（带静态断言）

* `struct lnxrm_ps_entry` **48 字节**（`LNXRM_STATIC_ASSERT`，`include/abi/lnxrm_abi.h:214`）
* `struct lnxrm_utsname` **320 字节**（`lnxrm_abi.h:148`）
* `struct intr_frame` 布局由 `test_intr_frame_layout` 验证（`ktest/t_abi.c`）
* 帧缓冲 ABI 颜色统一为 **真彩 0x00RRGGBB**，永不使用调色板索引（`lnxrm_abi.h:151-153` 注释）。

---

### 26. 凭据、能力与资源配额

#### 26.1 三级凭据（T-030）

`include/sys/cred.h:3-16` 的注释定义：

| 层级 | 含义 |
|---|---|
| `CRED_USER = 0` | 普通进程，无自身特权 |
| `CRED_ROOT = 1` | 管理员：能做用户能做的一切，**仍不能破坏 kxld 状态与其系统文件** |
| `CRED_KXLD = 2` | 最高层：保护内核与 kxld 系统文件，**任何人都进不去**；kxld 任务不走 syscall 路径（`syscall_entry` 有 BUG_ON） |

* 顺序按权力排：`USER < ROOT < KXLD`，所以 `kind >= CRED_ROOT` 就是"至少 root"。
* uid：`CRED_UID_PRIV 0`（特权）、`CRED_UID_USER 1000`（普通）。
* 实测：`ps.kind.idle=2`、`ps.kind.init=1`、`ps.uid.init=0`（smoke SYSTEST_TAGS）。

#### 26.2 能力位（T-031，3 个）

| 能力 | 位 | 作用 |
|---|---|---|
| `CAP_KILL` | `1<<0` | 给自己 uid 之外的任务发信号（`cred.h:33`） |
| `CAP_SYS_ADMIN` | `1<<1` | `ps`/`diskinfo` 枚举任务与磁盘（`cred.h:34`） |
| `CAP_FB` | `1<<2` | 在帧缓冲上绘制（`cred.h:35`） |

* root 与 kxld **按构造拥有全部能力**（`cred.h:31` 注释）。
* 三道闸实测拒绝消息：`[kxld] denied kill`、`[kill] denied`、`[cap] denied kill(-1)`、
  `[cap] denied <what>`、`[fb] denied <what>`、`[cap] denied setuid`
  （`kernel/syscall.c:84,90,107,155,181,195`）。

#### 26.3 不可杀保护与配额

* `kxld_protect_kill()`：**pid 1（init）任何人均不可杀**——它是所有用户进程的祖先、
  孤儿回收者，杀了会让整个用户态悬空；**KXLD 任务不可杀**（`kernel/protect.c` 文件头与实现）。
* 内存配额：`CRED_USER_MEM_QUOTA_PAGES = (64 MiB) / PAGE_SIZE`（`cred.h:66`），
  由 `cred_mem_quota()` 按 tier 推导，setuid 与配额不可能给出两个不同答案（C24，
  `include/sys/sched.h:76-77` 注释）。
* 帧缓冲**单一持有者**（T-033）：`fb_claim(pid)` 用 CAS 抢占，`fb_disown(pid)` 只有
  持有者能清（`kernel/framebuffer.c:670-701`）；拒绝**不会**顺手把表面判给被拒者
  （`kernel/syscall.c:175-177` 注释）。实测：`[fb] denied fb_fill: pid 13 holds the surface, pid 14 draws`。

---

### 27. 信号

* 信号槽 **32 个**（`LNXRM_NR_SIGNALS 32`，`include/abi/lnxrm_abi.h:88`），已命名 **26 个**
  （`grep -c "^#define LNXRM_SIG" lnxrm_abi.h` = 26）：
  `HUP1 INT2 QUIT3 ILL4 TRAP5 ABRT6 BUS7 FPE8 KILL9 SEGV11 PIPE13 ALRM14 TERM15
  STKFLT16 CHLD17 CONT18 STOP19 TSTP20 TTIN21 TTOU22 URG23 XCPU24 XFSZ25
  VTALRM26 PROF27 WINCH28`（编号 10、12、29–31 保留未命名）。
* **处理器三态**：`SIG_DFL = (void*)0`、`SIG_IGN = (void*)1`、其余为处理器地址
  （`lnxrm_abi.h:107-110`、`kernel/signal.c:5-6`）。
* **默认终止集**只有 6 个：`SIGINT SIGTERM SIGQUIT SIGILL SIGABRT SIGSEGV`
  （`kernel/signal.c:20-21`），其余默认忽略——与该内核的历史行为一致。
* **SIGKILL / SIGSTOP 不可忽略、不可屏蔽**（`kernel/signal.c:26,30,46-48`）。
* 用户态返回用一个 9 字节 restorer：`mov eax,20; int 0x80; ud2`
  （`sigreturn_code[]`，`kernel/signal.c:96`），由 `signal_map_restorer()` 放进
  **独立一页**（`kernel/signal.c:103`），落在 `USER_SIGRETURN_VA`。
* 阻塞的信号保留 pending 位，解阻塞时投递；处理器运行期间再次到来的信号保持 pending
  直到 sigreturn（`kernel/signal.c:199,208` 注释）。
* `nanosleep` 被信号打断 → 返回 `EINTR` 并回填余量（`usr/systest.c:278` 注释）。

---

### 28. 文件系统

#### 28.1 VFS 层

* `struct fs_ops` **10 个钩子**（`include/sys/vfs.h:29-40`）：
  `lookup / getdent / read / write / create / mkdir / unlink / rmdir / rename / freespace`。
* `struct file_ops` **4 个**：`read / write / getdent / close`（`vfs.h:42-46`）。
* 路径解析 `abs_path()`：超长路径直接 `LNXRM_ENAMETOOLONG`，**不写半个路径**
  （`kernel/fs/vfs.c:108` 注释）。
* **伪设备节点**：`dev_lookup()` 只认 `/dev`（目录）和 `/dev/console`（字符设备）
  （`kernel/fs/vfs.c:152-176`）；`/dev/console` 的读实现是 `console_read()`
  （`kernel/fs/vfs.c:549`）——阻塞等待输入、**信号 EINTR 优先于睡眠**、
  下一 tick 兜底唤醒、arm waiter 后二次复查竞态窗口（`kernel/fs/vfs.c:549-600` 注释）。
* **T-005 错误契约**：目录迭代器一旦因 I/O 错误死掉就保持死状态（`include/sys/vfs.h:55-64` 注释），
  错误**不会**被 cookie 掩盖。

#### 28.2 FAT32

* **4 个模块**（`kernel/fs/fat32/`，1,586 行）：
  `fat32.c`（挂载/链/空间）、`fat32_dir.c`（目录项与名字）、`fat32_file.c`（读写）、
  `fat32_meta.c`（create/mkdir/unlink/rmdir/rename）。
* 实现的文件操作（`include/fat32_priv.h:237-245`）：
  `fat_lookup_impl / fat_create_impl / fat_mkdir_impl / fat_rmdir_impl /
  fat_unlink_impl / fat_rename_impl / fat32_getdent_impl`。
* **名字与大小写（本次修复）**：
  * 8.3 短名大小写由 NT 位决定：`FAT_NTRES_LOWER_BASE 0x08`、`FAT_NTRES_LOWER_EXT 0x10`
    （`include/fat32_priv.h:117-118`）。
  * `fat_short_to_name(const u8 *sn, u8 ntres, char out[13])` 按位渲染，
    **位为 0 就按磁盘原样返回**（`fat32_dir.c:5,11-12`），对齐 Linux `fs/fat/dir.c`
    的 `fat_shortname2uni(..., de->lcase & CASE_LOWER_*)`。
  * `fat_short_ntres(name)`（`fat32_priv.h:132`）计算"能让别名原样渲染回 name"的位；
    创建/改名三处写入点都写它：`fat32_meta.c:78`（parent_entry_add）、
    `fat32_meta.c:620`（rename_in_place，注释"旧位描述旧名字"）、`fat32_meta.c:734`（新建）。
  * `fat_lfn_count()` 用 `strcmp`（区分大小写），混合大小写必须分配 LFN
    （`fat32_meta.c:462-467`：别名回渲染后 `strcmp` 相等才可只用 8.3）。
  * 目录列举把 `e->ntres` 传进去（`fat32_dir.c:107`）。
  * 实测：根目录 `README  MD `（ntres=0x10）→ `README.md`，`BIN        `（ntres=0x08）→ `bin`，
    且 `README.md bin/` 大小写正确出现在日志里。
* **簇链安全**：`FAT_CHAIN_MAX_STEPS 0x800010`（8,388,624，`fat32_priv.h:161`），
  `fat_chain_limit = MIN(上限, 数据簇数)`（`fat32.c:78-82`）；非法首簇直接拒绝并告警
  （`fat32.c:84-95`），链有环/越界只报告不跟随（smoke 第 6 阶段专门伪造验证）。
* **容量如实报告**（T-080/T-081）：`[blk] registered hda (131072 sectors, 64 MiB)`，
  smoke 判据要求 `131072 / 2048 == 64`，并钉死 `2097151`/`4294967295` 不得出现
  （`scripts/smoke_test.py:81-91` 注释）。

#### 28.3 挂载与分区

* 挂载顺序：**先整盘 FAT32，再 MBR 分区**（`kernel/fs/vfs.c:656-690` 注释与实现），
  日志依次是 `[vfs] trying FAT32 (whole disk)...` → 失败则 `trying MBR partition table...`。
* MBR 解析出的每个分区包成**虚拟分区设备**（`struct part_dev { parent, start_lba, size_sectors }`），
  读写越界返回 `LNXRM_EFAIL`，底层转发 `blk_io_read/write`（`kernel/fs/mbr.c:9-26`）。

#### 28.4 块缓存

* **LRU 扇区缓存，64 项**（`CACHE_ENTRIES 64`，`include/disk.h:96`）。
* **唯一咽喉 `blk_io_read`/`blk_io_write`**：所有设备流量（含缓存自身）都过这一对，
  便于校验传输，也便于自测**故障注入**（T-005，`kernel/fs/blk_cache.c:1-6` 注释）。
* 六个统计计数：`hits / misses / evictions / writebacks / dev_reads / dev_writes`
  （`include/disk.h:131-138`），配 `blk_cache_stats_get/reset` 与
  `blk_cache_lookup()`（返回槽位或 -1，让测试观察**哪个**扇区被逐出）。
* `blk_cache_purge_dev()`：写回并丢弃某设备全部条目，保证下次一定落到设备
  （`disk.h:149-151` 注释"Test/tooling only"）。
* **FAT 区域永不缓存**（自测 `test_fat_region_is_never_cached`，
  `ktest/t_cache.c`）。
* 自测用例 5 个（`t_cache.c`）：LRU 受害者选择、200 扇区文件每扇区一次写回、
  热扇区挺过冷流、写读回一致、FAT 区不缓存。

---

### 29. 设备驱动

| 驱动 | 能力 | 出处 |
|---|---|---|
| **PCI** | 配置空间枚举，port IO `0xCF8/0xCFC`，bus 0..255，`pci_read/pci_write` | `kernel/drivers/pci.c:1-20` |
| **IDE** | PIO，主通道 `0x1F0`；IDENTIFY 取容量，48 位标记正确处理 | `kernel/drivers/ide.c` |
| **AHCI** | C++ 实现；通用/端口寄存器、FIS、**单 PRDT DMA**、FBS；CLB/CT 设置与命令下发由自旋锁串行，防止两 CPU 交错 | `kernel/drivers/ahci.cpp:12-40,210-212` |
| **PS/2 键盘** | 控制器初始化 + IOAPIC IRQ1 → vector 33 | `kernel/main.c:154-169` |
| **串口 COM1** | IRQ4 → vector 36；还有轮询兜底 `lnxrm_uart_trygetc` | `kernel/fs/vfs.c:552`、`kernel/rust/uart.rs` |
| **LAPIC/IOAPIC** | MMIO 映射进 `DEV_VMA`，`apic_send_ipi/sipi`、`ioapic_set_irq` | `kernel/apic.c:1-10`、`include/sys/apic.h:43-53` |

* **Rust 组件 3 个文件 / 296 行**：`kernel/rust/lib.rs`、`uart.rs`（串口驱动）、
  `smp.rs`（SMP 原语）——实测行数见 §35。
* **C++ 组件 3 个文件 / 679 行**：`smp.cpp`、`ahci.cpp`、`SlabAllocator.cpp`
  （后者提供 freestanding `operator new`）。
* 控制台**双路输出**：`console_putc` 同时写 VGA 文本与 COM1（`include/console.h:11`）。
  `kprintf`/`console_write` 共用一把 irqsave 的 `print_lock`（`console.h:13-16`）。

---

### 30. 控制台、帧缓冲与 ANSI

#### 30.1 输出

* `kprintf` 支持格式符 **`d u x X p s c %`**，含宽度、`0` 填充；flags 与精度解析但忽略；
  支持 `l`/`ll`/`z` 长度修饰（`kernel/print.c:263-330`）。
* `panic` 用红色高亮（`kernel/print.c:365`，见 §24.2）。

#### 30.2 ANSI 转义

* **分词器共享**：`kernel/ansi.c`（31 行）的 `ansi_feed()` 只管把字节流切成
  `ESC [` … 字母 的序列，`ansi_color_index()` 把 30–37 映射到 0–7。
* **两个后端各自消费**：
  * VGA 文本：`ESC[2J` 清屏、`ESC[H` / `ESC[r;cH` 定位、`ESC[...m` SGR，
    序列结束后 `vga_update_cursor()`（`kernel/print.c:94-176`）。
  * 帧缓冲控制台：同样三个终结字母 `J`/`H`/`m`（`kernel/framebuffer.c:310,315,342`），
    `ESC[m` 等价 `ESC[0m` 重置（`framebuffer.c:343`）；跑转义时先藏软件光标
    （`framebuffer.c:388` 注释）。
* 用户态 `/bin/clear` 发 `\x1b[2J\x1b[H\x1b[3J`（`usr/clear.c:11`）。

#### 30.3 帧缓冲能力

* 8x16 点阵字体，内嵌标准 VGA **CP437 可打印 ASCII 0x20..0x7E**（96 个字形，
  `kernel/framebuffer.c:31-33`）。
* 16 个命名色 `FB_CLR_*`（`include/framebuffer.h:94-110`：BLACK/GREEN/NAVY/PURPLE/
  TEAL/LGREY/DGREY/RED/BROWN/WHITE/LBLUE/LGREEN/LCYAN/LRED/LMAGENTA/YELLOW）。
* 绘制原语：`fb_clear / fb_fill_rect / fb_draw_char / fb_puts`（`include/framebuffer.h:35-38`），
  **全部裁剪到可见区域**——用户传进来再坏的矩形最坏也是什么都不画
  （`kernel/syscall.c` fb 段注释）。
* 布局与颜色打包：`fb_layout_get/set`、`fb_pack(r,g,b)`、`fb_owner_get/claim/disown`
  （`framebuffer.h:59-76`）。
* 归属与门：`fb_info` 不设门（只读几何、不绘制），四个绘制调用要 `CAP_FB` **加** 单一持有者
  （`kernel/syscall.c:380-399` 注释）。

---

### 31. 用户态

#### 31.1 运行时

* `init/` **239 行**：`crt0.S` 29 行（入口、发布 `environ`，`init/crt0.S:9,19,29`）、
  `ulib.c` 80 行、`ulib.h` 130 行。
* `ulib.h` 提供 `syscall1/syscall2/sys_call3` 内联汇编包装（`int $0x80`，
  注意第 3 个参数走 `%r10`）与 `kopen` 的约定：**open 只有 (path, flags) 两参，
  因为这个内核没有权限位可传**（`init/ulib.h:39-41` 注释）。

#### 31.2 十六个程序

`usr/` **1,529 行 / 16 个程序**（实测）：

```
cat(30)  clear(17)  echo(12)  hello(25)  help(10)  init(52)
kill(45) ls(29)     mkdir(20) mv(90)     ps(90)    rm(91)
rn(29)   sh(111)    systest(859)         touch(19)
```
（括号内为 `wc -l` 实测行数）

* `help` 列出 13 条命令：`cat mkdir clear echo help kill ls mv ps rn sh touch rm`
  （`usr/help.c`）。
* `/bin/init` 是第一个用户进程：打开 `/dev/console` 并 `dup2` 成 0/1/2，读一遍
  `/README.md` 验证磁盘链路，然后**只 fork 一次** `/bin/sh`——终端被 kill 后不重启
  （否则终端杀不死），init 自己继续常驻做 pid 1 收养孤儿（`usr/init.c` 文件头与注释）。

#### 31.3 shell

`usr/sh.c`（111 行）的能力：

* `tokenize()` 按空格/Tab 切词，最多 `MAX_ARGS 16` 个（`usr/sh.c:8-23`）。
* **`>` 重定向**：扫描到独立的 `>` 就把文件名从参数里摘掉，随后用 `dup2` 落地
  （`usr/sh.c:60-77`）。
* 命令解析：拼 `/bin/` 前缀后 `kexecve`（`usr/sh.c:96-100`）。
* 行编辑：退格 `\b`/127 处理（`usr/sh.c:40-48`），逐字回显。
* **不支持**：管道、后台作业、历史、通配、变量展开（源码中无相应实现）。

---

### 32. 测试体系

#### 32.1 内核自测（ktest）

* 框架与用例分离：`ktest/ktest.c` + **17 个用例文件**（`grep -c` 实测）。
* **三阶段** `early / mm / late`（`stage_name[]`，`ktest/ktest.c:16`），
  各阶段行号见 §21.3。
* 实测（2026-10-06 `build/smoke/serial.log`）：

  | 阶段 | cases | checks | failed |
  |---|---|---|---|
  | early | 23 | 334 | 0 |
  | mm | 15 | 429 | 0 |
  | late | 34 | 783 | 0 |
  | **合计** | **72** | **1546** | **0**（`[2 skipped]`，`[1 warn]`） |

* 覆盖面举例：ABI 布局与调用表连续性、E820/命令行、锁与 TSC、CR0/CR4 保护位、
  凭据与能力、绘制与帧缓冲控制台、ELF 拒绝野入口、分箱堆与 OOM、
  块缓存 LRU 语义、I/O 错误注入、信号与任务、VFS/FAT32（含容量判据）。
* `selftest_note_fail` / `WARN` 的计数器与判决集中在 `ktest.c`，
  汇总行 `[selftest] 1546/1546 pass [1 warn]`。

#### 32.2 端到端冒烟（`scripts/smoke_test.py`）

**6 个阶段**（文件头 docstring 第 4-11 行）：

1. `toolchain` — 必需工具在 PATH（`phase_toolchain`，`smoke_test.py:267`）
2. `build` — `make all` + 磁盘镜像（`phase_build`，`:283`）
3. `artifacts` — bzImage 引导头、vmlinux 符号、FAT32 镜像、用户 ELF
   （`phase_artifacts`，`:343`）
4. `boot` — 无头 QEMU，**20 条有序 marker**（`BOOT_MARKERS`，`:52`）
5. `shell` — 通过串口驱动 `/bin/sh`：ls、cat、systest
6. `forged FAT` — 用被篡改簇链的镜像启动，要求驱动**报告**且不跟随
   （`phase_forged_fat`，`:680`）

* 实测：**PASS 176/176**（2026-10-06）。
* `SYSTEST_TAGS` **81 条**正则（`smoke_test.py:105`），如 `uname.ret=0`、
  `unknown.99=-38`、`ps.kind.idle=2`、`kill.init.term=-13`。
* 负面判据同时钉死：日志里不得出现 `WARN(`、`2097151`、`4294967295`（实测 grep 计数 0）。
* **内存档位矩阵（2026-10-06 手工补测，不在 smoke 里）**：`-m 64 / 256 / 2048 / 4096`
  各起一次，全部 `[ktest] selftest PASS` + `[boot] ready`、0 次三重故障。
  这条守着 §22.6 那个 bug——smoke 固定 `-m 256`，永远碰不到 1 GiB 窗口上界。
* 日志里 9 条 `[fat32] I/O error` 是 `t_ioerr.c` **故意注入**的（基线就是 9，
  `GAP_ANALYSIS.md:1276`），不是回归。

#### 32.3 单 CPU 冒烟（`scripts/smoke_smp1.py`）

* `BOOT_MARKERS` 是 `-smp 2` 专用（要求 AP marker 与 "2 cpus online"），所以
  `-smp 1` 单独一个脚本：启动 → 跑 `systest` → 断言**同一套 81 条标签**
  （文件头 docstring，49 行）。

#### 32.4 用户态自检（`usr/systest.c`）

* **859 行**，按系统调用编号分节（`/* ---- 15 uname ---- */`、`/* ---- 10 fork, 11 execve ... ---- */`），
  末尾 `say("systest.done", 1)`。
* 覆盖：uname、getpid/getppid、open/lseek/read/close、write、brk、dup2、getdent、
  nanosleep、kill、sigaction、sigprocmask、sigreturn、fork/execve/exit/wait4（含 WNOHANG）、
  ps、diskinfo、mkdir/rename/unlink/rmdir、权限闸、内存配额与回收、真彩色卡等。
* 通过条件是 81 条标签全部命中 + `systest.done=1` + 无 `[PANIC]` + 无 `[ktest] FAIL`。

---

### 33. 构建系统与产物

#### 33.1 Make 目标

| 目标 | 作用 | 出处 |
|---|---|---|
| `all` | 构建 `build/bzImage` | `Makefile:68` |
| `usr` | 用户态 ELF | `Makefile:71` |
| `rust` | `build/liblnxrm.a` | `Makefile:81` |
| `clean` | 删 `build/` 与 `usr/*.o,*.d` | `Makefile:153` |
| `run` | QEMU 启动 | `Makefile:158` |
| `smoke` | 跑 `scripts/smoke_test.py` | `Makefile:163` |
| `imageiso` | GRUB 引导的可启动 ISO → `build/lnxrm.iso`（暂存树在 `build/iso/`） | `Makefile:174` |

**注意**：`make` **不生成磁盘镜像**；`make clean` 后需重跑
`make usr build/disk.img`（`docs/README.md` 快速开始）。

**`imageiso` 不依赖 `disk.img`**——ISO 里只有 bootloader + 内核，根文件系统仍是
单独的 IDE 盘。所以光盘启动要带上 `-drive file=build/disk.img,...`（见
`docs/BUILD.md` §8.2）。`grub.cfg` 是**生成**的（`Makefile:176`），因此
`make clean && make imageiso` 也能一次跑通；所需的 `gfxpayload` 写法与实测
结果见 §21.1。

#### 33.2 QEMU 固定参数

```
qemu-system-x86_64 -m 256 -smp 2 -cpu qemu64,+smep,+smap \
  -kernel build/bzImage \
  -drive file=build/disk.img,format=raw,if=ide,index=0,media=disk \
  -serial stdio -no-reboot -d int -D build/qemu.log
```
（`Makefile:158-161`）

#### 33.3 磁盘镜像

* `dd` 64 MiB → `mkfs.vfat -F 32` → `mcopy README.md` → `mmd ::bin` → 逐个 `mcopy` 用户程序
  （`Makefile:204-212`）。
* 镜像里根目录有 `README.md` 与 `bin/`（实测日志 `README.md bin/`）。

#### 33.4 产物（2026-10-06 实测）

| 产物 | 大小 | 说明 |
|---|---|---|
| `build/bzImage` | **162,200 B** | 最终可引导镜像（C71/C72 修复重编后） |
| `build/vmlinux.bin` | **160,152 B** | 纯 payload（按 LMA 导出） |
| `build/vmlinux.elf` | 755,920 B | 带调试信息（`.image` 160,000、`.bss` 405,520） |
| `build/disk.img` | 67,108,864 B | 64 MiB FAT32 |
| `build/liblnxrm.a` | 10,499,526 B | Rust 静态库 |

---

### 34. 明确未实现的能力

以下功能**当前没有**（依据 `docs/README.md` "未实现" 段 + 代码核对）：

| 类别 | 未实现项 |
|---|---|
| 内存 | `mmap`、需求分页、写时复制（fork 是复制不是 COW）、共享内存；**> 1 GiB 的物理内存不参与分配**（管理窗口 `PMM_WINDOW_TOP` = 1 GiB，见 §22.6） |
| 进程/IPC | 管道、socket、线程/`clone`、`pthread`（`grep pipe\|socket\|mmap\|clone\|pthread` 在 ABI 与 syscall.c 中无命中） |
| 时间 | `gettimeofday`、`clock_gettime` 等时间查询（有 `nanosleep` 与 `jiffies`，但没有读时钟的系统调用） |
| 文件 | `stat`、`chdir`、权限位/文件属主、日志式文件系统 |
| 身份 | 用户账号/口令/登录、按文件的属主模型（三级凭据 + 能力位是有的，见 §26） |
| 链接 | 动态链接、共享库（用户态只能构建静态单文件程序） |
| 网络 | 完整网络栈 |
| 硬件 | USB、AHCI 之外的 NVMe/SCSI；**VMware 真机上无可驱动块设备**（见下） |
| 引导 | UEFI/EFI handover stub（`handover_offset=0`） |

**已知问题**（详见 [GAP_ANALYSIS.md](GAP_ANALYSIS.md)）：

* **VMware 磁盘**：`15ad:07e0` 控制器下 IDE IDENTIFY 全 0xFF → 伪容量 → `[vfs] no disk found`
  → `Stop!`。三层结论：容量撒谎（**已修**）、幻影盘（未修）、真机无可驱动块设备（真正卡点，
  候选 ATAPI 只读或 SCSI/LSI 最小读盘，未选定）。
* **偶发 `#GP (13)` panic**：`rip=ffffffff8011e3c7` 落在 `isr32` 末尾 `iretq`，
  `err=0x440` 表示 IRET 弹出的 CS 无效选择子——是被打断现场帧被写坏，
  不是 `iretq` 本身有 bug（分析已交付，改造方案未获批）。
* 差距与计划：GAP_ANALYSIS 第一部分 **66 条**有效编号缺陷、第三部分 **55 个**任务卡
  （数字取自 `docs/README.md:31,33`）。

---

### 35. 数字口径说明

本文的规模数字是 **2026-10-06 实测**（脚本：遍历源码扩展名
`.c .h .hpp .cpp .S .asm .rs .ld .def`，排除 `.git/ build/ __pycache__/`）：

| 项 | 实测（2026-10-06） | docs 既有记载（2026-10-05） | 差 |
|---|---|---|---|
| 源文件数 | **97** | 97 | 0 |
| 内核 `kernel/ arch/ include/` | **16,029**（12,190 + 1,670 + 2,169） | 15,895（12,102 + 1,670 + 2,123） | **+134** |
| 用户态 `init/ usr/` | **1,768**（239 + 1,529） | 1,768 | 0 |
| 合计 | **17,797** | 17,663 | **+134** |
| `bzImage` | **162,200 B** | 160,856 B | +1,344 |
| `vmlinux.bin` | **160,152 B** | 158,808 B | +1,344 |

* 逐语言（实测）：`.c` 62 文件 13,236 行、`.h` 22 文件 2,218、`.cpp` 3 文件 679、
  `.S` 3 文件 650、`.asm` 1 文件 572、`.rs` 3 文件 296、`.ld` 1 文件 65、
  `.def` 1 文件 54、`.hpp` 1 文件 27。
* 子系统（实测）：`kernel/mm` 822、`kernel/fs` 1,211、`kernel/fs/fat32` 1,586、
  `kernel/drivers` 673、`kernel/rust` 296、`kernel/lib` 90、`ktest` 3,297、
  `include/abi` 278、`include/sys` 850、`scripts` 967（含 `.py`）。
* **差 +134 行的来源**：**+43** 2026-10-06 的 FAT32 大小写修复
  （`include/fat32_priv.h`、`kernel/fs/fat32/fat32_dir.c`、`kernel/fs/fat32/fat32_meta.c`、
  `ktest/t_fs.c` 四个文件）；**+91** 同日的真机三重故障修复
  （`main.c` +30、`pmm.c` +20、`pci.c` +20、`vmm.c` +9、`mm.h` +12，
  见 §22.6 与 `GAP_ANALYSIS.md` C71/C72）。产物 +1,344 B = 前者重编 +1,088 B、
  后者 +256 B（`.image` 159,744 → 160,000，`.bss` 因按 4 KiB 取整仍是 405,520）。
* **本轮已对齐**：本文 `:4,27,28,31,36,38,40,47`、§2 目录地图、§9.4、§14、§18
  与 §33.4 的行数/产物大小已逐个按 `wc -l`、`stat` 重算（连带修掉 `fat32_priv.h`
  213→247、`fat32_dir.c` 251→258、`fat32_meta.c` 738→745 这类上一轮的遗留漂移）。
* **仍待同步**：`docs/README.md:7`、`docs/BUILD.md:17,1184,1186`、
  `GAP_ANALYSIS.md` §12.1 的规模数字仍是 2026-10-05 的旧数。
  口径统一需要单独一次同步（口径与行数统计方法的既往争议见
  `GAP_ANALYSIS.md` 评审部分）。

---

### 36. 一分钟自查表

按"想确认什么"查：

| 想确认 | 看这里 |
|---|---|
| 有哪些系统调用、编号多少 | §25.2 |
| 内存怎么分的、配额多少 | §22、§26.3 |
| 进程/调度/SMP 怎么工作 | §23 |
| 几级权限、谁能画屏 | §26 |
| 信号支持到什么程度 | §27 |
| 文件系统能干什么、大小写怎么处理 | §28 |
| 有哪些驱动 | §29 |
| 控制台支持哪些转义序列 | §30.2 |
| 有哪些用户程序、shell 能干什么 | §31 |
| 测试判据是什么、怎么跑 | §32 |
| 产物多大、怎么构建 | §33 |
| **没有**什么 | §34 |
| 数字口径 | §35 |
