# lnxrm 构建指南

> 面向：要在本地构建、运行、调试这个内核的人
> 代码库规模：16,029 行内核 + 1,768 行用户态（2026-10-06 实测，口径见 `CODEBASE.md` §35）
> 配套文档：[CODEBASE.md](CODEBASE.md)（代码导览 + 功能总览） · [GAP_ANALYSIS.md](GAP_ANALYSIS.md)（差距 · 评审 · 行动计划）

本文所有数据都是**在本机实测**得到的，不是估算。凡是给出命令的地方都验证过能跑通。

---

## 0. 概览

| 项 | 实测值 |
|---|---|
| 全量构建耗时（`make -B -j8` 强制重建） | **1.4–2.3 秒** |
| 构建可复现性 | 设 `SOURCE_DATE_EPOCH` 后**逐字节可复现**（3 次重建 SHA256 全同）；默认构建仅 1 字节的 uname 构建时间戳不同（见 §14.3） |
| 产物 `bzImage` | 162,200 字节（2026-10-06 C71/C72 修复重编；`--gc-sections` 前是 254,920） |
| 工具链 | gcc 15.2.0 / binutils 2.46 / nasm 3.01 / rustc 1.97.1 / python 3.14.4 |
| 模拟器 | QEMU 10.2.1 |
| 调试器 | gdb（带完整源码级符号） |
| 需要的额外工具 | `mtools`（`mcopy`/`mmd`）、`dosfstools`（`mkfs.vfat`） |

**并行构建安全**：`make -j8` 无竞态，连续 3 次从零构建产物完全一致。

---

## 1. 前置依赖

### 1.1 必须的编译器与工具

| 工具 | 实测版本 | 用途 | Debian/Ubuntu 包 |
|---|---|---|---|
| `gcc` | 15.2.0 | C 编译（内核 + 用户态） | `gcc` |
| `g++` | 15.2.0 | C++ 编译（`smp.cpp` `ahci.cpp`） | `g++` |
| `ld` | 2.46 | 内核链接（`-T arch/kernel.ld`） | `binutils` |
| `objcopy` | 2.46 | 3 处：ELF→bin、trampoline bin→obj、复制的 usr ELF | `binutils` |
| `nm` | 2.46 | `patch_setup_jmp.py` 查符号 | `binutils` |
| `nasm` | 3.01 | 4 个汇编源文件 | `nasm` |
| `rustc` | 1.97.1 | Rust 组件 → staticlib | `rustc` |
| `python3` | 3.14.4 | 2 个二进制补丁脚本 | `python3` |

### 1.2 必需的系统工具（构建磁盘镜像用）

| 工具 | 实测路径 | 用途 | 包 |
|---|---|---|---|
| `mkfs.vfat` | `/usr/sbin/mkfs.vfat` | 在 `disk.img` 上建 FAT32 | `dosfstools` |
| `mcopy` | `/usr/bin/mcopy` | 往镜像里拷文件 | `mtools` |
| `mmd` | `/usr/bin/mmd` | 往镜像里建目录 | `mtools` |
| `dd` | 系统自带 | 生成 64 MiB 空镜像 | `coreutils` |

### 1.3 运行与调试

| 工具 | 实测版本 | 用途 |
|---|---|---|
| `qemu-system-x86_64` | 10.2.1 | 运行内核 |
| `gdb` | 系统自带 | 源码级调试 |

### 1.4 一键安装（Debian/Ubuntu）

```sh
sudo apt install gcc g++ binutils nasm rustc python3 \
                 dosfstools mtools qemu-system-x86 gdb
```

### 1.5 交叉编译

`Makefile:1` 的 `CROSS` 变量**默认为空**，即用宿主工具链：

```make
CROSS :=
CC := $(CROSS)gcc
```

因为内核是 `-ffreestanding -nostdlib`，实际上只需要一个能生成 x86-64 目标码的编译器，不真的需要交叉工具链。要用交叉工具链：

```sh
make CROSS=x86_64-linux-gnu-
```

**但 rustc 不跟着 `CROSS` 走**（`Makefile:6` 是裸 `RUSTC := rustc`），所以真正的交叉编译需要额外处理。这一项目前未支持。

---

## 2. 快速开始

```sh
# 1. 构建内核（2 秒）
make

# 2. 构建用户态程序 + 磁盘镜像（首次必须，见 §5 的坑）
make usr build/disk.img

# 3. 运行
make run
```

**或者一步到位**（`run` 目标会自动带上 `usr` 和 `disk.img` 依赖）：

```sh
make run
```

**做可启动 ISO**（GRUB 引导，全部产物落在 `build/` 内，见 §8.2）：

```sh
make imageiso               # → build/lnxrm.iso（暂存树 build/iso/）
```

### ⚠️ 最大的坑：`make` 不生成磁盘镜像

默认目标 `all: $(BUILD)/bzImage`（`Makefile:68`）**只构建内核**。磁盘镜像是 `run` 目标的依赖（`Makefile:158`），不是 `all` 的依赖。

后果：`make clean && make` 之后直接 `qemu-system-x86_64 ... -drive file=build/disk.img` 会**立即失败**：

```
qemu-system-x86_64: -drive file=build/disk.img,format=raw,if=ide:
                    Could not open 'build/disk.img': No such file or directory
```

**记住：`make clean` 之后要么 `make run`，要么 `make usr build/disk.img`。**

---

## 3. 构建系统架构

### 3.1 四组编译标志

`Makefile:32-47` 定义了四套完全不同的标志，每一套都有明确的设计理由。

#### 内核 C（`KERNFLAGS`）

```make
BASEFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
             -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel \
             -O2 -g $(WARN) -Iinclude -Iinclude/cpp -I. -MMD -MP \
             -ffunction-sections -fdata-sections
KERNFLAGS := $(BASEFLAGS) -std=gnu11
```

| 标志 | 理由 |
|---|---|
| `-ffreestanding` | 不假设标准库存在（内核有自己的 `string.c`） |
| `-fno-stack-protector` | 栈保护需要 `__stack_chk_fail`，freestanding 下链接不上；也**消除一个真实的攻击面** |
| `-fno-pic -fno-pie` | 内核是**单一连续段**（VMA `0xffffffff80100000`），位置无关代码反而会生成 GOT 间接访问 |
| `-mno-red-zone` | **关键**：中断可以在函数序言期间打断，必须假设栈已被使用 |
| `-mno-mmx -mno-sse -mno-sse2` | 省掉 XMM 寄存器的保存/恢复；且进入内核时 XMM 状态不保证（所以 `cpu_init` 才要开 OSFXSR） |
| `-mcmodel=kernel` | 代码假设在 `0xffffffff8xxxxxxx` 高半，编译器可用短（32 位）偏移访问数据 |
| `-O2 -g` | 优化 + **完整调试信息**（gdb 能看到源码行，见 §11） |
| `-std=gnu11` | 允许 GNU 扩展（语句表达式等） |
| `-MMD -MP` | 生成头文件依赖，见 §12 |
| `-ffunction-sections -fdata-sections` | 每个函数/数据独占一个段，让链接器 `--gc-sections` 能按可达性裁剪（见 §4.4）。没有它，Rust staticlib 的整个 `core` 会拖进镜像，**约 70% 的 `.image` 是死代码**（2026-10-01 加） |

#### 内核 C++（`CXXFLAGS`）

```make
CXXFLAGS := $(BASEFLAGS) -std=c++20 -fno-exceptions -fno-rtti \
            -fno-threadsafe-statics -fno-use-cxa-atexit -nostdinc++
```

| 标志 | 理由 |
|---|---|
| `-fno-exceptions` | 没有 unwinder（没有 `.eh_frame`，`kernel.ld:48` 把它 discard 了），抛异常会跳到不存在的代码 |
| `-fno-rtti` | 去掉 vtable/typeinfo 生成 |
| `-fno-threadsafe-statics` | 静态局部变量的 guard 变量需要 `__cxa_guard_acquire` |
| `-fno-use-cxa-atexit` | 不生成 `__cxa_atexit`，所以 `main.c:21-24` **手动遍历 `__init_array`** 调全局构造函数 |
| `-nostdinc++` | 不引入 libstdc++ 头文件。代价：必须自己提供 `operator new`（见 `kernel/mm/SlabAllocator.cpp`） |

#### Rust（`RUSTFLAGS`）

```make
RUSTFLAGS := --crate-type staticlib --edition 2021 -C panic=abort -C opt-level=s \
             -C relocation-model=static -C code-model=kernel \
             -C target-feature=-sse -C debuginfo=0 \
             --target x86_64-unknown-none
```

| 标志 | 理由 |
|---|---|
| `--crate-type staticlib` | 产出 `liblnxrm.a`，被 `ld` 整体拉入 |
| `--target x86_64-unknown-none` | 裸机目标，**没有 `std`** |
| `-C panic=abort` | 不生成 unwinder；`kernel/rust/lib.rs:16-27` 自定义 `#[panic_handler]` |
| `-C relocation-model=static` | 禁用 PIE/PIC 重定位 |
| `-C code-model=kernel` | 同 `-mcmodel=kernel` |
| `-C target-feature=-sse` | 与 C 侧一致 |
| `-C debuginfo=0` | 减小编译时间和产物体积（Rust 侧调试信息价值低，C 侧才是主体） |

#### 用户态（`USRFLAGS`）

```make
USRFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-pie -O2 \
            -nostdlib -nostartfiles -static -no-pie -mcmodel=large $(WARN) \
            -mno-mmx -mno-sse -Iinit -Iinclude -idirafter include
```

**与内核的关键差异**：

| 差异 | 理由 |
|---|---|
| `-nostdlib -nostartfiles` | 不用任何 libc，也不用 crt1.o。入口是自己写的 `init/crt0.S` |
| `-static` | **必须是静态链接** —— 内核的 `elf_load` 不支持 `ET_DYN`/PIE，也不做动态链接 |
| `-mcmodel=large` | 用户程序链接在 `0x7f8000400000`（`USER_TEXT_VMA`），超出 `small`（±2 GiB）和 `kernel`（高半）的假设 |
| `-Wl,-Ttext=0x7f8000400000` | `Makefile:151`，把 `.text` 固定到用户地址空间 |
| `-Iinit` | 找到 `ulib.h` |
| `-idirafter include` | 让 `ulib.h` 里的 `#include <abi/lnxrm_abi.h>` 能找到（`abi/` 在 `include/` 下） |

### 3.2 目标依赖图

```
all (默认) ──► build/bzImage
                 ├─ build/setup.bin
                 │   ├─ arch/setup.asm
                 │   └─ build/vmlinux.elf        （为了 patch_setup_jmp.py 查符号）
                 └─ build/vmlinux.bin
                     └─ build/vmlinux.elf
                         ├─ build/**/*.o + build/**/*.opp
                         │   └─ kernel/**/*.c, kernel/**/*.cpp, arch/*.S
                         ├─ arch/kernel.ld
                         └─ build/liblnxrm.a
                             └─ kernel/rust/*.rs（整体编，单文件 crate）

run ──► usr + build/bzImage + build/disk.img
         │
         ├─ usr ──► build/usr/<每个 usr/*.c>
         │           └─ build/usro/<name>.o + ulib.o + crt0.o
         └─ build/disk.img
             ├─ build/README.md（一个 echo 生成的小文件）
             └─ build/usr/*（全部用户态 ELF）
```

### 3.3 源文件发现

`Makefile:51-57` 用 `$(shell find ...)` **自动发现源文件**：

```make
KSRC_C   := $(shell find kernel arch -name '*.c')
KSRC_CXX := $(shell find kernel arch -name '*.cpp')
KASM_ALL := $(shell find arch -name '*.S')
KASM     := $(filter-out arch/trampoline.S,$(KASM_ALL))   # ← trampoline 特殊处理
RUST_SRC := $(shell find kernel/rust -name '*.rs')
USR_ALL_C:= $(shell find usr -maxdepth 1 -name '*.c')
UIMG     := $(patsubst usr/%.c,%,$(USR_ALL_C))
```

**含义：新增内核 `.c` 文件不需要改 Makefile**，放进去 `make` 就会编。

新增用户态 `.c` 也自动生效 —— 每个 `usr/*.c` 都是一个独立程序。

⚠️ **注意 `arch/trampoline.S` 被显式排除**在 `KASM` 之外，因为它的目标文件名不是 `build/arch/trampoline.o` 而是 `build/trampoline.o`（为了 `objcopy` 处理），`Makefile:87` 单独加了这个目标。

### 3.4 KTEST 开关（自测套件的可选编译）

`Makefile:29-45`：**默认不编译** `ktest/` 套件，`make KTEST=1` 才把它编进内核。

```make
KTEST ?= 0
ifeq ($(KTEST),1)
KTEST_DIR  := ktest
KTEST_DEFS := -DCONFIG_KTEST=1
else
KTEST_DIR  :=
KTEST_DEFS :=
endif
```

| 用法 | 效果 |
|---|---|
| `make`（默认 `KTEST=0`） | `ktest/` 不进源列表；`kernel/main.c` 的 `ktest_run()` 调用点全部包在 `#ifdef CONFIG_KTEST` 里。镜像约小一半（87 KB vs 带套件 ≈179 KB），启动串口零 `[ktest]` 行 |
| `make KTEST=1` | `find kernel arch $(KTEST_DIR)`（`Makefile:61-62`）把 `ktest/*.c` 纳入源列表，`-DCONFIG_KTEST=1` 挂进 `BASEFLAGS`（`Makefile:45`，C/C++ 共用），启动时自动跑全部用例 |
| `make smoke` | **自动强制 `KTEST=1`**（`Makefile:191-192`：先把 `MAKEFLAGS` 里的 `KTEST=%` 滤掉再以 `KTEST=1` 调起脚本——smoke 校验的 `[ktest]` 标记只有这个模式才有）。`make KTEST=0 smoke` 同样生效，且不会把模式戳记带偏 |

⚠️ **模式戳记 `build/.ktest-mode-$(KTEST)`**（`Makefile:80-85`）：所有 `.o` 依赖它——翻转开关会触发戳记重建，强制**全量重编**，防止两种模式的对象文件混装（残留的 `CONFIG_KTEST` 宏会让符号对不上）。戳记规则必须排在 `all:` 之后，否则它会抢成 make 的默认目标。

**断言基础设施不随开关走**：计数器与 `selftest_case_failed` / `selftest_bug_seen` 在 `kernel/selftest.c`（始终编译），extern 声明在 `include/console.h`；`ktest/ktest.c` 只剩 harness 本体（`KTEST=1` 才编）——所以 `KTEST=0` 的镜像里这些符号仍然存在，链接不缺。

---

## 4. 内核构建阶段详解

### 4.1 单文件规则（`Makefile:100-108`）

```make
$(BUILD)/%.o: %.c | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  CC      %@\n"
	@$(CC) $(KERNFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/%.opp: %.cpp | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  CXX     %@\n"
	@$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<
```

- `%.o: %.c` 模式规则 + `@mkdir -p $(dir $@)` ⇒ **目录结构自动创建**，不需要手写 `build/kernel/fs/fat32/` 之类
- `@printf` 而不是 `echo` ⇒ 构建输出干净可读，且 `@` 抑制命令回显
- 目标后缀 `.opp` 用来区分 C++ 的 `.o`（否则模式规则会混淆）

### 4.2 汇编规则（`Makefile:95-98`）

```make
$(BUILD)/arch/%.o: arch/%.S | $(BUILD)
	@$(NASM) -f elf64 -F dwarf -g -o $@ $<
```

`-F dwarf -g` 让汇编也有调试信息，gdb 里能看汇编源码。

### 4.3 trampoline 的两段式处理（`Makefile:83-93`）

这是构建链里最特别的一段。`arch/trampoline.S` 必须在运行时被 `memcpy` 到物理 `0x8000`（AP trampoline 地址），但它同时要参与链接。

**解法：先编成裸二进制，再 objcpy 成 ELF 对象。**

```make
$(BUILD)/trampoline.bin: arch/trampoline.S
	@$(NASM) -f bin -o $@ $<              # ① 编成裸二进制

$(BUILD)/trampoline.o: $(BUILD)/trampoline.bin
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.trampoline $< $@ \
	  --redefine-sym _binary_build_trampoline_bin_start=trampoline_start \
	  --redefine-sym _binary_build_trampoline_bin_end=trampoline_end \
	  --redefine-sym _binary_build_trampoline_bin_size=trampoline_size
	                                        # ② 包成带 .trampoline 段的 ELF 对象
```

`objcopy -I binary` 会为输入文件生成三个符号：`_binary_<路径>_start/_end/_size`。`--redefine-sym` 把它们改名成 `trampoline_start` / `trampoline_end` / `trampoline_size`，这样 `kernel/smp.cpp:15-16` 就能用：

```cpp
extern u8 trampoline_start[];
extern u8 trampoline_end[];
```

`--rename-section .data=.trampoline` 把裸二进制的段名改成 `.trampoline`，**让 `kernel.ld` 的 `*(.text .text.*)` 之类的通配不影响它**（否则会被塞进 `.image`）。

### 4.4 链接（`Makefile:110-114`）

```make
$(BUILD)/vmlinux.elf: $(KOBJ) $(ARCHDIR)/kernel.ld $(BUILD)/liblnxrm.a
	@$(LD) -T $(ARCHDIR)/kernel.ld --gc-sections -o $@ $(KOBJ) \
	      $(BUILD)/liblnxrm.a -Map $(BUILD)/vmlinux.map \
	      --no-warn-rwx-segments
```

- `-T arch/kernel.ld` 用自定义链接脚本
- `--gc-sections` 配合编译期的 `-ffunction-sections/-fdata-sections`，把没有可达引用的段直接扔掉。**实测把 `.image` 从 253,472 缩到 ~77,000 字节（省 ~177 KB，约 70%）** —— 大头是 Rust staticlib 拖进来的 `core::text`（compiler_builtins、unicode 表、浮点格式化），这台内核一行都调不到。注意 `arch/kernel.ld` 必须 `KEEP(*entry64.o(.text.prologue))`：`scripts/patch_setup_jmp.py` 要用 `nm` 查 `_start32`/`_start64` 的符号值来回填 setup 扇区里的远跳转，段被 GC 掉脚本就失败
- `-Map build/vmlinux.map` 生成链接映射。**排查符号地址、段布局很有用**：`grep <symbol> build/vmlinux.map`
- `--no-warn-rwx-segments` 抑制警告，因为单一连续段必然是 **RWE**（见 §9.1）

**注意 Rust staticlib 放在最后** —— 这样 C 符号可以覆盖 Rust 的，反之不行。

### 4.5 导出裸二进制（`Makefile:116-118`）

```make
$(BUILD)/vmlinux.bin: $(BUILD)/vmlinux.elf
	@$(OBJCOPY) -O binary $< $@
```

**关键：`objcopy -O binary` 按 LMA（加载地址）导出，不是 VMA。**

因为 `kernel.ld` 里 `.image : AT(KERNEL_LMA)` 把 LMA 设为 `0x100000`，导出的裸二进制**从物理地址 `0x100000` 开始**，可以直接和 `setup.bin` 拼接。

### 4.6 setup.bin（`Makefile:120-124`）

```make
$(BUILD)/setup.bin: $(ARCHDIR)/setup.asm $(BUILD)/vmlinux.elf | $(BUILD)
	@$(NASM) -f bin -o $@ $<
	@python3 -c "d=open('$@','rb').read();pad=(2048-len(d)%2048)%2048;open('$@','wb').write(d+b'\x00'*pad)"
	@$(PYTHON3) scripts/patch_setup_jmp.py $@ $(BUILD)/vmlinux.elf
```

**三步**：
1. NASM 编真模式 setup
2. **补零到 2048 字节的整数倍**（Linux boot protocol 要求 setup 至少 4 个 512 字节扇区；padding 让 payload 从 4 KiB 边界开始，避免某些加载器算错偏移）
3. **`patch_setup_jmp.py` 回填跳转目标**（见 §6.1）

⚠️ 第 3 步依赖 `vmlinux.elf`，所以这个目标依赖 `vmlinux.elf`——但**只为了查符号**。这形成了一条 `setup.asm → vmlinux.elf` 的依赖边。

### 4.7 拼接与打补丁（`Makefile:126-131`）

```make
$(BUILD)/bzImage: $(BUILD)/setup.bin $(BUILD)/vmlinux.bin
	@cat $(BUILD)/setup.bin $(BUILD)/vmlinux.bin > $@
	@$(PYTHON3) scripts/patch_bzimage.py $@ $$(( $$(stat -c%s $(BUILD)/setup.bin) ))
	@printf "  SIZE    %s bytes\n" $$(stat -c%s $@)
```

`cat` 拼接后调 `patch_bzimage.py` 填 boot protocol 头。**实测输出**：

```
  BZIMAGE build/bzImage
  PATCH   build/bzImage
          setup_sects = 4 (header byte 0x03)
          syssize     = 16283 paragraphs (260520 payload bytes)
  SIZE    262568 bytes
```

---

## 5. 用户态与磁盘镜像

### 5.1 用户态程序（`Makefile:133-151`）

```make
$(USR_OBJDIR)/crt0.o: init/crt0.S
	@$(NASM) -f elf64 -o $@ $<

$(USR_OBJDIR)/ulib.o: init/ulib.c
	@$(CC) $(USRFLAGS) -MMD -MP -c -o $@ $<

$(USER_ELFS): $(BUILD)/usr/%: $(USR_OBJDIR)/%.o $(USR_OBJDIR)/ulib.o $(USR_OBJDIR)/crt0.o
	@$(CC) $(USRFLAGS) -Wl,-Ttext=$(USER_LINK_BASE) -o $@ $^
```

**每个用户程序都链接三个东西**：
```
xxx.o   ← 自己的代码
ulib.o  ← 迷你 libc（init/ulib.c）
crt0.o  ← 入口（init/crt0.S）
```

用 `$(CC)`（gcc）而不是 `ld` 驱动链接，是为了自动带上 crtbegin/crtend 之类的包装 —— 虽然 `-nostartfiles` 已经禁掉了它们，`gcc` 仍负责传递正确的 `-L`/`-dynamic-linker`（本项目不需要后者）。

`USER_LINK_BASE = 0x7f8000400000`（`Makefile:49`），对应 `USER_TEXT_VMA`（`include/types.h:51`）。

### 5.2 磁盘镜像（`Makefile:204-212`）

```make
$(BUILD)/disk.img: $(BUILD)/README.md $(USER_ELFS)
	@dd if=/dev/zero of=$@ bs=1M count=64 2>/dev/null
	@mkfs.vfat -F 32 $@ 2>/dev/null
	@mcopy -i $@ $(BUILD)/README.md ::README.md
	@mmd -i $@ ::bin
	@for f in $(USER_ELFS); do \
	  mcopy -i $@ $$f ::bin/$$(basename $$f); \
	done
```

**设计要点：用宿主机的 mtools 建 FAT32 镜像，而不是让内核挂载自己生成的镜像。**

这避免了鸡生蛋问题（内核要先能挂 FAT32 才能跑，但镜像需要有人来造）。`mkfs.vfat` 生成的镜像格式保证标准，**可以直接用 `fsck.vfat` 校验，也可以挂到宿主机上查看**。

⚠️ 几个注意点：
- `dd` 用 `2>/dev/null` 抑制进度输出（`64M` 会刷屏）
- `mkfs.vfat` 的 stderr 也被抑制，因为会问一堆确认
- 镜像是 `build/disk.img`，属于构建产物（`make clean` 会删除）
- `build/README.md` 由 `Makefile:214-215` 的一个 `echo` 生成（`|| order` 保证每次都重建，因为它是 phony-ish 依赖）

**验证镜像内容**：
```sh
mdir -i build/disk.img ::bin        # 列出 /bin
mdir -i build/disk.img ::/          # 列出根目录
fsck.vfat -n build/disk.img         # 只检查不修复
```

---

## 6. 两个补丁脚本详解

这是"从零实现 Linux boot protocol"的核心，也是这个项目最有意思的构建部分。

### 6.1 `scripts/patch_setup_jmp.py`（69 行）

**问题**：NASM 汇编 `setup.asm` 时，内核还没链接，`_start64` 的最终物理地址**未知**。

**解法**：先写哨兵值，链接后再回填。

```
① NASM 编 setup.asm，末尾跳转写成 jmp 0x18:0xF00DFACE
② 内核链接完成，_start64 地址确定
③ 脚本用 nm 从 vmlinux.elf 查符号：
       start32 = nm(_start32)     # 链接期偏移
       start64 = nm(_start64)
       target  = 0x100000 + (start64 - start32)
   因为整个 payload 是逐字节复制到物理 0x100000 的，
   链接期的相对偏移 = 运行期的相对偏移。
④ 在 setup.bin 里搜索 jmp 0x18:imm32 指令并回填 imm32
```

**怎么找到那条指令**（`:57-68`）：
```python
if data[i] != 0xEA:            # 0xEA = far jmp 的 opcode
    continue
sel = struct.unpack_from("<H", data, i + 5)[0]
if sel != 0x0018:              # 选择子必须是 0x18（64 位代码段）
    continue
```

**幂等**：`:70-74` 检查 `imm == target` 就直接返回。所以重复跑构建不会出问题 —— 这很重要，因为 `setup.bin` 可能已经是修好的（增量构建时）。

⚠️ **这个脚本依赖 `nm` 在 `PATH` 里**，且 `vmlinux.elf` 必须存在。

### 6.2 `scripts/patch_bzimage.py`（87 行）

填 Linux x86 boot protocol（`Documentation/arch/x86/boot.rst`）要求的两个字段：

| 偏移 | 大小 | 字段 | 含义 |
|---|---|---|---|
| `0x1F1` | 1 字节 | `setup_sects` | setup 段大小（512 字节扇区数）**减一**。0 特殊表示 4 扇区 |
| `0x1F4` | 4 字节 | `syssize` | 受保护模式 payload 大小，单位 **16 字节段** |

**三个输入校验（`:60-72`）**：
```python
assert data[0x202:0x206] == b"HdrS"        # 头部魔数
assert data[0x1FE:0x200] == b"\x55\xaa"     # boot flag
assert payload_off % 512 == 0               # payload 偏移 512 对齐
assert payload_off >= 4 * 512               # 至少 4 扇区（2 KiB）
```

这些断言保证任何加载器（SeaBIOS / GRUB / syslinux）算出的 payload 偏移都和我们一致。

**输出示例**（实测）：
```
  PATCH   build/bzImage
          setup_sects = 4 (header byte 0x03)
          syssize     = 16283 paragraphs (260520 payload bytes)
```

---

## 7. 链接脚本 `arch/kernel.ld`（52 行）

**全部内容就是三件事**：

```
1. 单一连续 .image 段
   . = KERNEL_VMA (0xffffffff80100000)
   .image : AT(KERNEL_LMA = 0x100000) {
       __kernel_start = .;
       */entry64.o(.text.prologue)   ← 入口代码必须在最前
       *(.text .text.*)
       *(.rodata .rodata.*)
       __init_array_start = .;  KEEP(*(.init_array*))  ...  __init_array_end = .;
       *(.data .data.*)
       . = ALIGN(16);
   }

2. .bss 段（FileSiz=0，只占内存）
   .bss : AT(LOADADDR(.image) + SIZEOF(.image)) {
       __bss_start = .;
       *(COMMON)  *(.bss .bss.*)
       . = ALIGN(16);
       . += 0x4000;              ← 启动栈！16 KiB
       __boot_stack_top = .;
       __bss_end = .;
   }

3. 丢弃段
   /DISCARD/ : { *(.eh_frame*)  *(.note*)  *(.comment) }
```

### 7.1 为什么 `.image` 必须是单一连续段

文件头注释说得很清楚：

> "One contiguous PROGBITS image so VMA/LMA stay perfectly in step: every byte satisfies VMA = LMA + (KERNEL_VMA - KERNEL_LMA). `objcopy -O binary` dumps by LMA => payload starts at physical 0x100000."

`objcopy -O binary` 输出的是**段的内容**，不填充空洞。如果 text 在一个段、data 在另一个段且物理上不连续，二进制会有大段空洞，拼接后无法启动。单一连续段彻底避免这个问题。

**代价**：
- 该段是 **RWE**（可读可写可执行）—— 没有 W^X 分离
- `objdump -h build/vmlinux.elf` 会看到 4 个 program header（`.image` `.bss` `.got.plt` `.trampoline`）

### 7.2 关键符号

| 符号 | 被谁用 | 用途 |
|---|---|---|
| `__kernel_start` / `__kernel_end` | `kernel/main.c:35-37`（`kern_text_ptr`）| 判断地址是否在内核文本区 |
| `__bss_start` / `__bss_end` | `entry64.S:269-274` | 启动时清零 |
| `__boot_stack_top` | `entry64.S:279` | 启动栈顶 |
| `__init_array_start` / `__init_array_end` | `main.c:18-24` | 手动调 C++ 全局构造函数 |
| `LOADADDR(.image)` / `SIZEOF(.image)` | `patch_setup_jmp.py`（经 `nm`） | 算 payload 起始 |

### 7.3 查段布局

```sh
readelf -lW build/vmlinux.elf       # program header（实测输出见 §8.1）
readelf -SW build/vmlinux.elf       # section 列表
objdump -h build/vmlinux.elf        # 更详细
grep ' start_kernel' build/vmlinux.map   # 查某符号的地址和所属段
```

---

## 8. 运行

### 8.1 `make run` 的 QEMU 命令

```make
run: usr $(BUILD)/bzImage $(BUILD)/disk.img
	@qemu-system-x86_64 -m 256 -smp 2 -kernel $(BUILD)/bzImage \
	  -drive file=$(BUILD)/disk.img,format=raw,if=ide,index=0,media=disk \
	  -serial stdio -no-reboot -d int -D build/qemu.log
```

| 参数 | 作用 | 注意 |
|---|---|---|
| `-m 256` | 256 MiB 内存 | 内核把管理窗口钳在 **`PMM_WINDOW_TOP` = 1 GiB** 以下（`include/mm/mm.h:20`）。要复现真机类问题得用 `-m 2048`（见 §8.3） |
| `-smp 2` | **两个 vCPU** | 触发 SMP 路径。⚠️ 会暴露并发缺陷（比如 C1） |
| `-kernel build/bzImage` | 用 QEMU 的 `linuxboot` 加载器 | **走 setup.asm 路径**（0x6F00 哨兵 = 0） |
| `-drive if=ide` | IDE 磁盘 | ⚠️ **AHCI 路径在 `make run` 下完全没被测过** |
| `-serial stdio` | 串口接到终端 | ⚠️ **和 gdb 抢终端**，调试时要改（见 §11） |
| `-no-reboot` | 三重故障等异常时退出而不是重启 | 避免无限重启 |
| `-d int -D build/qemu.log` | QEMU 中断跟踪日志 | **实测 25,420 行**，排查中断路由问题很有用 |

**用别的驱动跑（测 AHCI）**：
```sh
qemu-system-x86_64 -m 256 -smp 2 -display none -kernel build/bzImage \
  -drive file=build/disk.img,format=raw,if=ahci,id=ahci0 \
  -serial stdio -no-reboot
```
⚠️ 但 AHCI 有已知问题（见 `GAP_ANALYSIS.md` C48–C52），可能出现挂死。

**无头运行**（SSH/CI 环境）：
```sh
qemu-system-x86_64 -m 256 -smp 2 -display none -kernel build/bzImage \
  -drive file=build/disk.img,format=raw,if=ide \
  -serial stdio -no-reboot
```

### 8.2 GRUB 启动路径（`make imageiso`）

代码支持 GRUB `linux` 命令，但 `make run` 不走这条。**`make imageiso` 一条命令出 ISO**
（2026-10-06 已实测通过，原 `GAP_ANALYSIS.md` C68「完全未经测试」）：

```sh
make imageiso            # → build/lnxrm.iso（13,342,720 B）
```

* 暂存树 `build/iso/`（`boot/bzImage` + `boot/grub/grub.cfg`），产物 `build/lnxrm.iso`
  —— **所有中间文件都在 `build/` 里**，源码树一个字节都不写，`make clean` 一并清掉。
* `grub.cfg` 是 **Makefile 生成的**（`Makefile:176`），所以 `make clean && make imageiso`
  也能一次跑通，不依赖仓库里有没有模板文件。
* 依赖 `grub-mkrescue`（`grub-pc-bin` / `grub2-pc`）+ `xorriso`；缺了会打印安装提示而不是
  留半成品——目标上有 `.DELETE_ON_ERROR`。
* **ISO 里没有根文件系统**，`disk.img` 仍是独立的 IDE 盘，启动必须两个都挂。

```sh
# 从光盘启动（-boot d 不加的话可能先从硬盘起）
qemu-system-x86_64 -m 2048 -smp 2 -cpu qemu64,+smep,+smap -display none \
  -boot d -cdrom build/lnxrm.iso \
  -drive file=build/disk.img,format=raw,if=ide,index=0,media=disk \
  -serial stdio -no-reboot
```

**生成的 `grub.cfg`**（`Makefile:176-190`）：

```
serial --unit=0 --speed=115200
terminal_output serial        # ← 不设这行，GRUB 输出全在 VGA 上，-display none 什么都看不到
set timeout=0
set default=0
insmod all_video
set gfxmode=1280x720x32       # 分辨率 1280x720 + 32 bpp
set gfxpayload=1280x720x32    # ← 给内核用的模式，必须是这个值
menuentry "lnxrm" {
  linux /boot/bzImage console=ttyS0
  boot
}
```

⚠️ **`gfxpayload` 必须写 `1280x720x32`，不能写 `1280x720,32`**。逗号形式是 GRUB 的
「后备模式列表」语法，`1280x720,32` 会被解析成「先试 1280x720，再试 32」，实测内核拿到
的是 **24 bpp**；只有 `x32` 才让 GRUB 真的编程 32 bpp 模式。两种都实测过（下表）。

**判断走了哪条路径**：串口日志第一行的行为不同，但更直接的判据是 `main.c:57` 读
`0x6F00`（**DWORD**）—— GRUB 路径会从 `boot_params.screen_info` 取 LFB 而不是 `0x8C00`。

**实测结果（2026-10-06，`-m 2048`）**：

| grub.cfg | 关键输出 | 自测 |
|---|---|---|
| 不设 `gfxpayload` | `[boot] cmdline='BOOT_IMAGE=/boot/bzImage console=ttyS0'`、`[fb] no linear framebuffer (loader left text mode)` | 1530/1530（4 个 fb 用例 skip） |
| `set gfxpayload=1280x720,32`（**错写法**） | `[fb] lfb 1280x720 **24 bpp** phys=0xfd000000 pitch=3840 **mode=0x0**` | **1546/1546** |
| `set gfxpayload=1280x720x32`（**`make imageiso` 默认**） | `[fb] lfb 1280x720 **32 bpp** phys=0xfd000000 pitch=5120 **mode=0x0**`、`[task] spawned pid=1 (/bin/init)`、`[boot] ready in 1255 ms` | **1546/1546** |

三种都 `[boot] ready`、无三重故障、无 PANIC。**`mode=0x0` 与 QEMU 路径的 `0x18f`
不同**，正是「确实读了 `boot_params.screen_info` 而不是 `0x8C00`」的证据。

**如果光盘启动停在 `Stop!`**：看日志里有没有
`[init] cannot open /bin/init`——那是 `disk.img` 的问题，不是 ISO 的问题（`imageiso`
不重建磁盘镜像）。两条自查：

```sh
mdir -i build/disk.img ::bin    # 报 "Fat problem while decoding" = 镜像是半成品
fsck.vfat -n build/disk.img     # "FATs differ" = 两个 FAT 副本不一致
# 修法：rm build/disk.img && make build/disk.img
```

`make` 只看时间戳，会把损坏的镜像判成"已是最新"，所以**别用 `make build/disk.img`
判断镜像健康**。成因通常是 `mcopy` 循环写到一半被打断（`Makefile` 头部
`.DELETE_ON_ERROR` 那段注释警告的就是这个），比如并行跑多个 QEMU 与重建镜像抢写锁。
`make smoke` 的 forged-FAT 阶段用的是**副本** `build/smoke/forged.img`，不会动
`build/disk.img`；实测连跑 `make smoke`（176/176，含会写盘的 `systest`）之后
`fsck.vfat -n` 仍干净，内核不写坏镜像。

### 8.3 内存档位矩阵（真机类 bug 必跑）

```sh
for m in 64 256 2048 4096; do
  timeout 60 qemu-system-x86_64 -m $m -smp 2 -cpu qemu64,+smep,+smap \
    -kernel build/bzImage -drive file=build/disk.img,format=raw,if=ide,index=0,media=disk \
    -serial stdio -display none -no-reboot
done
```

**为什么必须跑**：`make smoke` 固定 `-m 256`，而物理内存管理窗口的上界是
1 GiB —— 只有 `-m 2048` 以上才会走到 `0x40000000`，而那正是 2026-10-06
真机三重故障的爆点（`GAP_ANALYSIS.md` C71、`CODEBASE.md` §22.6）。
`-m 256` 下这个 bug **一动不动地存在，却永远不会复现**。

判据：串口里出现 `[ktest] selftest PASS` 与 `[boot] ready in`
（`[ktest]` 行来自 `KTEST=1` 构建——跑本流程前先 `make KTEST=1 -j4`，
见 §3.4；`make smoke` 自动强制该模式），且 `grep -ci "triple fault"` 为 0、
`grep -ci PANIC` 为 0。

---

## 9. 调试

四种手段，从轻到重。

### 9.1 串口日志（最常用）

内核所有 `kprintf` 走 Rust 的 UART 驱动 → `-serial` 指定的目标。

```sh
# 终端直接看
make run

# 存文件
qemu-system-x86_64 ... -serial file:/tmp/ser.log

# null（只要 gdb 时）
qemu-system-x86_64 ... -serial null
```

### 9.2 debugcon 早期启动标记（最快的分诊手段）

`arch/entry64.S` 在关键阶段向端口 `0xE9`（QEMU 的 debug console）打单字符标记：

| 标记 | 位置 | 含义 |
|---|---|---|
| `K` | `entry64.S:162` | 进入 `_start64` |
| `b` | `:201` | 段寄存器设完 |
| `c` | `:231` | 主页表建完 |
| `d` | `:257` | 进入 `.activate` |
| `f` | `:262` | `CR4.PAE` 设完 |
| `e` | `:267` | `load_cr3` 完成 |
| `M` | `:277` | bss 已清，即将 `call start_kernel` |

**必须显式开 debugcon**（实测：不加 `-debugcon` 什么都看不到）：
```sh
qemu-system-x86_64 ... -debugcon stdio
```

**实测输出**：
```
1 23V4567KbcdfeM
```

`1 23V4567` 是 SeaBIOS 固件输出的，**`KbcdfeM` 是 lnxrm 自己的**。

**分诊表**：
| 看到什么 | 诊断 |
|---|---|
| `1 23V4567` 然后没了 | 内核镜像没被加载 —— 检查 `setup_sects`/`syssize` 是否被正确 patch |
| 到 `K` 停 | 卡在 `_start64` 开头 —— 段寄存器？ |
| 到 `c` 停 | 主页表有问题 —— `pmm_init` 之前不能用 C，检查 `PML4_M` 物理地址 |
| 到 `e` 停 | `load_cr3` 之后、清 bss 时崩 —— `.bss` 的 LMA 算错 |
| 全部 `KbcdfeM` 都有 | C 世界进不去 —— 看串口日志 |

这个通道的价值在于：**串口日志要 `console_init()` 之后才有，而这个通道从第一条指令就有。**

### 9.3 GDB（源码级）

**构建已经有完整调试信息**（`-O2 -g` + nasm `-F dwarf -g`）。

```sh
# 终端 1：启动 QEMU 并冻结
qemu-system-x86_64 -m 256 -smp 2 -display none -no-reboot \
  -kernel build/bzImage \
  -drive file=build/disk.img,format=raw,if=ide,index=0,media=disk \
  -serial null -S -s
#   -S  = 启动时冻结 CPU
#   -s  = 在 TCP 1234 开 GDB stub

# 终端 2：连上去
gdb build/vmlinux.elf
```

**实测验证的 GDB 流程**：
```
$ gdb -q build/vmlinux.elf
(gdb) target remote :1234
0x000000000000fff0 in ?? ()          ← -S 冻结在复位向量
(gdb) break start_kernel
Breakpoint 1 at 0xffffffff80101640: file kernel/main.c, line 207.
(gdb) continue
Thread 1 hit Breakpoint 1, start_kernel () at kernel/main.c:207
207  {
(gdb) info registers rip rsp cr3
rip            0xffffffff80101640  0xffffffff80101640 <start_kernel>
rsp            0xffffffff801b1018  0xffffffff801b1018
cr3            0x50000             [ PDBR=80 PCID=0 ]
(gdb) backtrace
#0  start_kernel () at kernel/main.c:207
#1  0x00000000001002d9 in ?? ()       ← entry64.S 的 call 指令
```

**注意 `-serial null`**：默认的 `-serial stdio` 和 gdb 抢终端，两者都跑不好。

**常用断点**：

| 断点 | 用途 |
|---|---|
| `start_kernel` | 进 C 世界的第一行 |
| `panic` | 任何 panic 之前 |
| `schedule` | 调度路径 |
| `syscall_entry` | 所有系统调用 |
| `isr_common` | 所有中断/异常 |
| `fat_mount_impl` | 挂载阶段 |

**常用命令**：
```gdb
x/8i $rip          # 反汇编当前（能同时看到 C 和 asm 的机器码）
info symbol $rip   # 当前地址属于哪个符号
p *current        # 打印当前任务（需要能看到 GS 里的 per-cpu 指针）
p/x this_cpu_data()->_current
info threads       # 另一个 CPU 的上下文
```

**查段/符号**：
```gdb
info symbol 0xffffffff80101640     # → start_kernel
x/s 0xffffffff80140720             # → bootinfo 的内容
maintenance info sections .image
```

⚠️ **GDB 断点在 SMP 下有坑**：断点会命中所有 CPU 的执行流，单任务场景下正常，调试 AP 时可能需要一个专门的 gdb 命令文件把 AP 的 `rip` 拉出来看。

### 9.4 QEMU 中断跟踪

```sh
qemu-system-x86_64 ... -d int -D /tmp/qemu-int.log
```

**实测产出 25,420 行**，格式是每次中断的 CPU 状态快照：
```
EIP=000ebe6f EFL=00000046 [---Z-P-] CPL=0 II=0 A20=1 SMM=0 HLT=0
ES =0010 00000000 ffffffff 00cf9300 DPL=0 DS   [-WA]
CS =0008 00000000 ffffffff 00cf9b00 DPL=0 CS32 [-RA]
```

**用途**：
- 排查中断向量路由（哪个 IRQ 映射到哪个向量）
- 找三重故障（会看到连续大量 `#PF` / `#DF`）
- 对照 `arch/setup.asm` 的 VBE 选型和 `main.c` 的 bootinfo 采集
- **抓"启动闪一下就重启"**：真机上这类现象必然是三重故障（`panic()` 是 `cli;hlt`
  只挂死不复位，所以能重启的不是 panic）。配 `-no-reboot` 让 QEMU 停在当场，
  日志尾部形如 `CR2=0000000040000000` 的 `#PF` 紧跟一条 `#DF`，再
  `addr2line -e build/vmlinux <EIP>` 就是根因。**2026-10-06 那次就是这么定位到
  `pmm.c:222` 的**（详见 `GAP_ANALYSIS.md` C71）。注意：IDT 建好之前的三重故障
  在 `-d int` 里同样有记录，但串口上一行都不会打——别因为"没日志"以为没跑到那。

```sh
# 找出所有不同的 EIP，看哪些代码在反复执行
grep '^EIP=' /tmp/qemu-int.log | sort | uniq -c | sort -rn | head -20
```

### 9.5 QEMU monitor（交互式）

```sh
qemu-system-x86_64 ... -monitor stdio
```

有用的命令：
```
info registers      # 所有寄存器
info cpus           # 每个 vCPU 的状态（看 AP 有没有在跑）
x/16i $eip          # 反汇编
xp /16gx 0x50000    # 读物理地址（页表）
info mtree          # 内存树（看 E820）
quit
```

⚠️ `-monitor stdio` 和 `-serial stdio` 冲突，要用 `-serial null` 错开。

---

## 10. 增量构建与依赖跟踪

### 10.1 依赖机制

`Makefile:218`：
```make
-include $(KOBJ_C:.o=.d) $(KOBJ_CXX:.opp=.d) $(USR_ALL_C:usr/%.c=$(USR_OBJDIR)/%.d)
```

配合 `-MMD -MP`（`Makefile:34`），gcc 为每个 `.o` 生成同名的 `.d` 文件。

**实测产物**：
```
build/arch/cpu.d
build/kernel/ansi.d
build/kernel/apic.d
build/kernel/elf.d
...
```

`-MMD` 只依赖头文件（不含 `.c` 自身），`-MP` 为每个头文件生成 phony 目标，这样删掉头文件不会导致 make 报错。

**实测验证**：
```sh
$ make
make: 对“all”无需做任何事。          ← 无改动，零输出

$ touch include/types.h
$ make | wc -l
40                                       ← 40 行输出 = 重编了多个文件
```

依赖跟踪有效：`types.h` 被几乎所有文件包含，所以大面积重编。

### 10.2 `FORCE` 目标

`Makefile:59-60`：
```make
FORCE:
.PHONY: FORCE
```

目前**没有规则使用它**（预留）。通常的用途是强制某些目标每次重建，比如：
```make
$(BUILD)/setup.bin: FORCE      # 每次重编 setup
```
因为 `setup.bin` 的真正输入是 `setup.asm` + `vmlinux.elf` 里的符号，而符号变化不触发依赖 —— **`FORCE` 就是为这种情况准备的**（见 §13.3 的常见问题）。

### 10.3 并行构建

**实测安全**：`make -j8` 全量重建 1.4–2.3 秒，产物与串行构建一致
（唯一的字节级变量是 §14.3 的 uname 构建时间戳，与并行无关；设
`SOURCE_DATE_EPOCH` 后可做逐字节比对）。

```sh
make -j$(nproc)          # 推荐
make -j8 all usr         # 同时构建内核和用户态
```

**为什么安全**：所有目标都有正确的依赖声明，且 `$(BUILD)` 目录用 `| $(BUILD)` 序贯依赖 + `@mkdir -p $(dir $@)` 双重保险。

---

## 11. 清理与重建

```make
clean:
	@rm -rf $(BUILD)
	@rm -f usr/*.o usr/*.d
	@echo Done!
```

```sh
make clean            # 删 build/ 全部 + usr/ 的临时文件（当前没有）
make                  # 重build 内核（2 秒）
make usr build/disk.img   # 重建用户态 + 磁盘
```

⚠️ **`make clean` 会删掉 `disk.img`**，所以之后必须 `make run` 或 `make usr build/disk.img`（见 §2 的坑）。

**只清部分**：
```sh
rm -f build/disk.img                    # 只重造磁盘
rm -rf build/kernel                     # 只重编内核 C 文件
rm -f build/vmlinux.elf build/vmlinux.bin build/bzImage   # 强制重链接+重拼接
```

⚠️ **`build/` 不在版本控制里**（它是产物目录）。首次 clone 后必须 `make` 一次。

---

## 12. 常见构建问题

### 12.1 QEMU 立即失败：`Could not open 'build/disk.img'`

**原因**：`make` 不生成磁盘镜像（§2）。`make clean` 后或首次 clone 后必然遇到。

**解决**：
```sh
make usr build/disk.img
# 或直接 make run
```

### 12.2 `patch_setup_jmp.py: could not find jmp 0x18:0xF00DFACE`

**原因**：脚本在 `setup.bin` 里找不到预期的跳转指令。可能是：
- `setup.asm` 被改过，跳转目标不再是 `0xF00DFACE`
- `setup.bin` 已经是修好的（但脚本是幂等的，所以更可能是前者）
- `vmlinux.elf` 里找不到 `_start64` 符号

**排查**：
```sh
grep -n 'F00DFACE\|0x18' arch/setup.asm | tail -5     # 确认哨兵还在
nm build/vmlinux.elf | grep -E '_start(32|64)'        # 确认符号存在
```

### 12.3 `patch_bzimage.py: AssertionError`

三个可能：

| 断言 | 原因 |
|---|---|
| `bad header magic at 0x202` | `setup.asm` 的 `HdrS` 魔数被破坏了 |
| `bad boot flag at 0x1FE` | `0x55AA` 被破坏 |
| `payload_off must be a multiple of 512` | `setup.bin` 的补零逻辑（`Makefile:123`）被改了 |
| `setup must be at least 4 sectors` | `setup.asm` 缩小到 2 KiB 以下 |

**排查**：`xxd -s 0x1f0 -l 32 build/bzImage` 看头部区域。

### 12.4 Rust 编译失败

```sh
rustup update                    # 确认 rustc 版本
rustc --version
```

⚠️ `Makefile:6` 是裸 `RUSTC := rustc`，不受 `CROSS` 影响。如果 PATH 里的 `rustc` 是别的 toolchain 的 wrapper，可能出问题。用 `make RUSTC=/path/to/rustc`。

**已知限制**：`--target x86_64-unknown-none` 需要对应 target 的 `core` 库。如果报找不到 `core`，说明 `rust-std` 没装：
```sh
rustup target add x86_64-unknown-none
```

### 12.5 改动符号后 `setup.bin` 没重新 patch

**症状**：改了 `entry64.S` 里 `_start32`/`_start64` 的相对位置，但启动时跳到错误的地址。

**原因**：`setup.bin` 依赖 `vmlinux.elf`，理论上会重建。但如果只是 `vmlinux.elf` 的**符号地址**变了而文件 mtime 没变，make 不会重跑脚本。

**解决**：
```sh
rm -f build/setup.bin build/bzImage && make
# 或者用预留的 FORCE 目标（§10.2）
```

### 12.6 链接错误：`undefined reference to '__stack_chk_fail'`

**原因**：某处没加 `-fno-stack-protector`。

**解决**：检查该文件的编译规则。`BASEFLAGS`（`Makefile:32`）里已经有了，所以通常是新加的构建规则忘了继承。

### 12.7 链接错误：`undefined reference to '_start'`

**原因**：用户态链接缺 `crt0.o`。`Makefile:133` 的规则已经包含它。

**解决**：确认目标走的是 `$(USER_ELFS)` 规则而不是手写的链接命令。

### 12.8 C++ 链接错误：`undefined reference to 'operator new'`

**原因**：`kernel/mm/SlabAllocator.cpp` 没被编进来。

**排查**：
```sh
ls build/kernel/mm/SlabAllocator.opp
grep SlabAllocator build/vmlinux.map
```
**解决**：确认文件在 `kernel/` 下（`find kernel arch -name '*.cpp'` 才会找到它）。

### 12.9 运行时：`[vfs] no disk found`

**可能原因**：
1. 磁盘镜像不存在（但那样 QEMU 会先报错）
2. IDE 探测失败 —— 检查 `[ide]` 开头的日志
3. `mkfs.vfat` 失败 —— `Makefile:207` 把 stderr 抑制了，手动验证：
   ```sh
   rm -f build/disk.img && make build/disk.img
   # 如果 mkfs.vfat 报错，去掉 2>/dev/null 看真实错误
   ```
4. 分区表问题 —— `disk.img` 是整盘 FAT32（无 MBR 分区）。`vfs_try_mount_disk` 先试整盘 FAT32，失败才走 MBR（`vfs.c:606-646`）

### 12.10 运行时：启动卡在 `KbcdfeM` 之后（串口无输出）

`M` 之后是 `call start_kernel` → `boot_collect_e820` → `console_init`。

**排查**：
```sh
# 用 gdb 断在 start_kernel，看能不能进
(gdb) b start_kernel
(gdb) c
# 进了之后单步，看 console_init 前后
(gdb) b console_init
(gdb) b kvprintf
```

常见原因：`console_init` 依赖的环形缓冲/串口初始化有问题，或者 `bootinfo` 采集读到垃圾。

---

## 13. 扩展指南

### 13.1 加一个用户态程序

**只需建一个 `.c` 文件**，Makefile 会自动发现（`USR_ALL_C` 用 `find`）：

```sh
cat > usr/myprog.c <<'EOF'
#include "ulib.h"
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    xputs("hello from myprog\n");
    return 0;
}
EOF
make usr          # build/usr/myprog 会自动出现
make run
```

**约束**（因为 `elf_load` 的限制）：
- 必须**静态链接**（Makefile 已处理）
- 必须是 **ET_EXEC**（`-no-pie`，Makefile 已处理）
- 用户地址空间是 `0x7f8000000000`–`0x7ffffffff000`，链接基址 `0x7f8000400000`
- 只能用 `ulib.h` 里的东西，**没有 libc**
- 用户栈只有 16 KiB（4 页，`task.c:150`），深递归会 `#PF`

⚠️ **然后要在 `usr/init.c` 或 `usr/sh.c` 里能调用它** —— sh 是 `fork + execve` 任意 `/bin/*`，所以自动可用。

### 13.2 加一个内核 C 文件

**只需放进 `kernel/` 任何子目录**，`find` 会自动发现：

```sh
cat > kernel/myutil.c <<'EOF'
#include <console.h>
void myutil_hello(void) { kprintf("myutil says hi\n"); }
EOF
```

在头文件里声明，然后就能用。目录会自动创建（`@mkdir -p $(dir $@)`）。

⚠️ 如果新文件用了新头文件，记得头文件也要在 `include/` 下（`-Iinclude`），且 `.d` 依赖跟踪会自动处理。

### 13.3 加一个系统调用（4 处改动）

这是最需要小心的一类扩展。四处**必须同步**：

**① `include/abi/lnxrm_syscalls.def`** —— 加一行（编号接在末尾）：
```make
LNXRM_SYS(myfunc, 34)
```
同时把 `include/abi/lnxrm_abi.h` 的 `LNXRM_SYSCALL_LAST` 提到同一个数字；
漏了这一步，那个 `1 / (nr <= LNXRM_SYSCALL_LAST)` 的常量枚举会让**编译失败**
（这正是它存在的目的）。2026-10-03 加 `setuid`(31) 就是照这条走的。

**② `kernel/syscall.c`** —— 在 `syscall_do`（`:82-186`）的 `switch` 里加分支：
```c
case SYS_myfunc:
    ret = sys_myfunc(f->rdi, f->rsi);
    break;
```

**③ `init/ulib.h`** —— 加内联包装：
```c
static inline long kmyfunc(long a, long b)
{ return sys_call3(SYS_myfunc, a, b, 0); }
```

**④ `include/abi/lnxrm_abi.h`** —— 如果有结构体参数，加进这个**内核和用户态共享**的头文件。

⚠️ **用户内存参数必须验证**。三种安全做法：
- `user_ptr_ok(p, n)` 后直接访问（`sys_waitpid` 写 `ustatus` 的做法，见 `task.c:253`）
- `copy_from_user(&local, usrc, sizeof(local))`
- `copy_user_str(buf, usrc, sizeof(buf))`（字符串）

**绝不要**直接把用户指针当内核指针用。

⚠️ 改完 `lnxrm_syscalls.def` 后**必须重编用户态**（`make usr`），因为 `SYS_*` 枚举是靠 X-macro 在 `lnxrm_abi.h` 里生成的。

### 13.4 加一个头文件

放 `include/` 下即可，`-Iinclude` 和 `-Iinclude/cpp` 已在 `BASEFLAGS` 里。

⚠️ **如果新头文件定义了新的地址空间常量**，要同步更新 `kernel/mm/vmm.c:17-18`（`PD_HI`/`PT_FIX`）—— 那两个常量和 `arch/entry64.S:4-10` 的页表物理地址**必须手工保持同步**，编译器不会检查。

### 13.5 改 VBE 分辨率

`arch/setup.asm` 的 4 遍级联（`VBE_TRY_MODE 1920,1080` / `1280,720` → 8bpp → 枚举最大）。

⚠️ 改完**必须重建 `setup.bin`**，但因为 `setup.asm` 是真实依赖，make 会自动处理（`Makefile:120` 有 `$(ARCHDIR)/setup.asm` 依赖）。

---

## 14. 附录：实测数据

### 14.1 段布局（`readelf -lW build/vmlinux.elf`）

```
Entry point 0xffffffff8010010e
There are 4 program headers

  Type           Offset   VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align
  LOAD           0x001000 0xffffffff80100000 0x0000000000100000 0x03db18 0x03db18 RWE 0x1000
  LOAD           0x001000 0xffffffff8013e000 0x000000000013d8a0 0x000000 0x063010 RW  0x1000
  LOAD           0x03eb18 0xffffffff8013db18 0x000000000013db18 0x0000b0 0x0000b0 RW  0x1000
  GNU_STACK      0x000000 0x0000000000000000 0x0000000000000000 0x000000 0x000000 RW  0x10

 Section to Segment mapping:
  00     .image .got
  01     .bss
  02     .got.plt .trampoline
  03
```

解读：
- 第 1 段 252,696 字节 = 代码 + rodata + data（`.image` 252,064 + `.got` 632），**RWE**（单一连续段的设计代价）
- 第 2 段 `.bss` 405,520 字节（≈396 KB），`FileSiz=0`（不占文件）
- 第 3 段 176 字节 = `.got.plt`（24）+ `.trampoline`（152，AP trampoline）
- **`.bss` 396 KB 主要来自**：`ap_stacks[8][32 KiB]`（256 KB，`smp.cpp`）、`task_table[64]`（62 KB，每槽 992 字节）、`blk_cache[64]`（33.5 KB）、`cpu_table[8]`（8 KB，已不含 idle 栈）、`ring_buf` 与 `idt` 各 4 KB

### 14.2 构建耗时

| 操作 | 耗时 |
|---|---|
| `make -B -j8`（强制全量重建，等价 `make clean && make`） | **1.4–2.3 秒**（热/冷） |
| `make`（无改动） | < 0.1 秒（零输出） |
| `touch include/types.h && make` | 约 1 秒（重编大部分文件） |
| `make usr`（16 个程序） | 约 1 秒 |

### 14.3 可复现性

唯一的字节级变量是 `uname.version` 里嵌的构建时间戳（`__DATE__` +
`__TIME__`，`kernel/syscall.c:77`）：不设环境变量时相邻两次重建恰好
相差 1 个字节（时间戳的秒位）。固定 `SOURCE_DATE_EPOCH` 后 gcc 会钉死
这两个宏，构建恢复逐字节可复现::

    export SOURCE_DATE_EPOCH=1767225600
    make -B -j8     # 连续 3 次，SHA256 完全相同

    run 1  d191bba5f58c707d032bd92c5643c896
    run 2  d191bba5f58c707d032bd92c5643c896
    run 3  d191bba5f58c707d032bd92c5643c896

其余构建链（`-frandom-seed`、链接顺序、`ar` 归档顺序）已验证稳定。
⚠️ 这个结论依赖于它们未被改动。如果将来引入 LTO、新的时间戳或并行
归并，可能失去这个性质。

### 14.4 产物清单

| 文件 | 大小 | 说明 |
|---|---|---|
| `build/bzImage` | 162,200 | 最终可引导镜像（2026-10-06，C71/C72 后） |
| `build/vmlinux.elf` | — | 带符号和调试信息，GDB 用 |
| `build/vmlinux.bin` | 160,152 | 纯 payload（按 LMA 导出） |
| `build/setup.bin` | 2,048 | 真模式 setup（补零后） |
| `build/liblnxrm.a` | — | Rust 组件 |
| `build/vmlinux.map` | — | 链接映射，排符号地址用 |
| `build/disk.img` | 67,108,864 | 64 MiB FAT32 镜像 |
| `build/usr/<16 个>` | — | 静态用户态 ELF |
| `build/**/*.d` | — | 头文件依赖 |
| `build/qemu.log` | ~25,420 行 | `make run` 产生的中断跟踪 |

### 14.5 工具链版本矩阵

本项目**没有固定工具链版本**（`Makefile` 不检查版本）。实测可用：

| 工具 | 版本 |
|---|---|
| gcc / g++ | 15.2.0 (Ubuntu 15.2.0-16ubuntu1) |
| GNU ld / objcopy / nm | 2.46 |
| NASM | 3.01 |
| rustc | 1.97.1 |
| Python | 3.14.4 |
| QEMU | 10.2.1 |
| 操作系统 | Ubuntu（`x86_64`） |

⚠️ **未在旧版本上验证**。如果换工具链遇到奇怪的链接/汇编错误，先怀疑版本差异。

---

*文档版本 1.0 · 所有数据为本机实测 · 行号引用基于本文档写作时的代码状态*
