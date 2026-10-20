# lnxrm 构建指南

lnxrm 是自制的 x86-64 freestanding 内核，语言只有 C 与汇编两种，构建链零警告。本文面向要构建、运行、调试这台机器的开发者，覆盖：依赖、make 目标、构建流程与产物、QEMU 运行与冒烟测试、磁盘镜像与 GRUB ISO、调试手段与排障。代码结构与功能清单见 CODEBASE.md。

## 1. 依赖与工具链

### 1.1 编译与运行所需工具

| 工具 | 用途 | Debian/Ubuntu 包 |
|---|---|---|
| `make` | 构建驱动 | `make` |
| `gcc` | C 编译（内核 + 用户态） | `gcc` |
| `ld` / `objcopy` / `nm` | 内核链接、ELF 与裸二进制互转、补丁脚本查符号 | `binutils` |
| `nasm` | 汇编：`arch/*.S`、`arch/setup.asm`、`usr/init/crt0.S` | `nasm` |
| `python3` | 补丁与打包脚本（`scripts/patch_*.py` 等） | `python3` |
| `mkfs.vfat` | 在磁盘镜像上建 FAT32 | `dosfstools` |
| `mcopy` / `mmd` | 往镜像拷文件、建目录 | `mtools` |
| `qemu-system-x86_64` | 运行内核 | `qemu-system-x86` |
| `gdb` | 源码级调试 | `gdb` |

```sh
sudo apt install make gcc binutils nasm python3 \
                 dosfstools mtools qemu-system-x86 gdb
```

可选工具：

| 工具 | 用途 | 说明 |
|---|---|---|
| `grub-mkrescue` + `xorriso` | `make imageiso` 出可启动 ISO | 装 `grub-pc-bin` + `xorriso`（Fedora：`grub2-pc`/`grub2-tools` + `xorriso` + `mtools`） |
| `fsck.vfat` / `mdir` | 检查、列出磁盘镜像内容 | smoke 会用；缺了对应检查记为 SKIP |
| `g++` | 不参与构建 | 但 smoke 第一阶段的工具链检查仍要求它在 PATH（装上即可） |

### 1.2 版本要求

Makefile 不钉版本。已验证可用的组合：gcc 15 / binutils 2.46 / nasm 3.01 / python 3.14 / QEMU 10.x。旧版工具链未验证，遇到奇怪的链接或汇编错误先怀疑版本差异。

`CROSS` 变量默认为空（用宿主工具链）。内核是 `-ffreestanding -nostdlib`，只需一个能生成 x86-64 目标码的编译器；要换交叉工具链：

```sh
make CROSS=x86_64-linux-gnu-
```

## 2. make 目标

| 目标 | 做什么 | 备注 |
|---|---|---|
| `all`（默认） | 构建 `build/bzImage` | 只有内核，不含用户态与磁盘镜像 |
| `usr` | 构建全部用户态 ELF 到 `build/usr/` | 每个 `usr/*.c` 一个程序，自动发现 |
| `run` | `usr` + `bzImage` + `disk.img`，然后启动 QEMU | 见第 4 节；唯一自动带上磁盘镜像的常用入口 |
| `smoke` | 端到端冒烟测试（六阶段） | 强制 `KTEST=1`，不依赖你当前的 KTEST 设置 |
| `imageiso` | 生成 `build/lnxrm.iso` | 依赖 `grub-mkrescue`；不重建 `disk.img` |
| `initramfs` | 生成 `build/initramfs.cpio` | 配合 `INITRAMFS=` 嵌入内核 |
| `clean` | 删除整个 `build/` | 之后需重新 `make usr build/disk.img` |
| `help` | 打印常用目标清单 | — |

可用变量：

| 变量 | 默认 | 作用 |
|---|---|---|
| `KTEST=1` | `0` | 把 `ktest/` 自测套件编进内核（`-DCONFIG_KTEST`）。翻转开关会触发全量重编，防止两种模式的对象混装 |
| `DISK=0` | `1` | `make DISK=0 run` 不挂磁盘镜像，适合快速看早期启动日志 |
| `MACHINE` | `q35` | QEMU 机型 |
| `KVM` | 自动探测 | `/dev/kvm` 可读写时自动加 `--enable-kvm` |
| `GRUB_TIMEOUT` | `0` | ISO 里 GRUB 的倒计时秒数 |
| `INITRAMFS` | 空 | 设为 cpio 路径（如 `INITRAMFS=build/initramfs.cpio`）时把 initramfs 编进内核 |
| `SMOKE_FLAGS` | 空 | 原样传给 `scripts/smoke_test.py`，如 `SMOKE_FLAGS="--smp 1"` |

## 3. 构建流程与产物

### 3.1 结构与源文件发现

根 `Makefile` 负责全局配置、对象装配、链接/镜像/运行/冒烟，并 `include` `arch/Makefile`、`kernel/Makefile`、`usr/Makefile` 三个片段（共享同一变量空间，不能单独 make）。三个片段各自用 `$(shell find ...)` 自动发现本目录源文件，所以：

- 新增内核 `.c`（`kernel/` 任意子目录）或 `arch/*.c`：放进文件即可，不用改 Makefile；
- 新增 `usr/*.c`：自动成为新用户程序并自动进磁盘镜像；
- 头文件放 `include/`（`-Iinclude` 已在标志里），`-MMD -MP` 的 `.d` 文件自动跟踪头依赖。

`arch/trampoline.S` 被显式排除在普通汇编规则外，单独走两段式处理（见 3.3）。

### 3.2 编译标志要点

内核侧 `KERNFLAGS = $(BASEFLAGS) -std=gnu11`，用户态侧 `USRFLAGS`，关键差异：

| 标志 | 理由 |
|---|---|
| `-ffreestanding -fno-stack-protector` | 无标准库、无 `__stack_chk_fail`，也消除一个攻击面 |
| `-mno-red-zone` | 中断可能在函数序言期间打断，栈必须可用 |
| `-mno-mmx -mno-sse -mno-sse2` | 免去 XMM 保存/恢复；进入内核时 XMM 状态不保证 |
| `-mcmodel=kernel` | 内核在高半地址，编译器可用短偏移访问数据 |
| `-O2 -g` | gdb 有完整源码级信息 |
| `-ffunction-sections -fdata-sections` | 配合链接器 `--gc-sections` 按可达性裁剪死代码 |
| `-fno-pic -fno-pie` | 内核是单一连续段，PIC 反而引入 GOT 间接访问 |
| 用户态 `-nostdlib -nostartfiles -static -no-pie -mcmodel=large` | 入口是自写的 `crt0.S`；内核 `elf_load` 不支持 PIE/动态链接；用户程序链接在 `0x7f8000400000`（`-Wl,-Ttext=`） |

### 3.3 阶段与中间物

```
kernel/**.c, arch/*.c  ──cc──► build/**/*.o ─┐
arch/*.S              ──nasm─► build/arch/*.o ─┤
arch/trampoline.S ─nasm─► trampoline.bin ─objcopy─► trampoline.o ─┤
ktest/*.c（仅 KTEST=1） ──cc──► build/ktest/*.o ─┤
                                              └─ld -T arch/kernel.ld --gc-sections─► build/vmlinux.elf
                                  vmlinux.elf ─objcopy -O binary─► build/vmlinux.bin ─┐
arch/setup.asm ─nasm─► 补零到 2 KiB 整数倍 ─patch_setup_jmp.py─► build/setup.bin ────┴─cat─► patch_bzimage.py ─► build/bzImage

usr/*.c + init/crt0.S + init/ulib.c ──cc──► build/usr/<程序名>（静态 ELF）
build/usr/* + build/README.md ──dd/mkfs.vfat/mcopy──► build/disk.img
```

要点：

- **链接**：`ld -T arch/kernel.ld --gc-sections`，`-Map build/vmlinux.map`。查符号地址用 `grep <symbol> build/vmlinux.map`。链接脚本把内核做成单一连续 `.image` 段（VMA `0xffffffff80100000`，LMA `0x100000`），`objcopy -O binary` 按 LMA 导出，所以 `vmlinux.bin` 可以直接与 `setup.bin` 拼接；`.bss` 单独一段（FileSiz=0），内含 16 KiB 启动栈。
- **trampoline 两段式**：先 `nasm -f bin` 编成裸二进制（运行时要被拷到物理 `0x8000`），再 `objcopy -I binary` 包成 ELF 对象，符号改名成 `trampoline_start/end/size`、段改名 `.trampoline`（避免被链接脚本通配进 `.image`）。
- **setup.bin 三步**：nasm 编真模式 setup → 补零到 2048 的整数倍（payload 从 4 KiB 边界开始）→ `patch_setup_jmp.py` 用 `nm` 查 `_start32`/`_start64` 回填 `jmp 0x18:imm32` 的跳转目标（脚本幂等；依赖 `nm` 在 PATH）。`setup.bin` 依赖 `vmlinux.elf` 只为查符号。
- **bzImage**：`cat setup.bin vmlinux.bin` 后由 `patch_bzimage.py` 填 boot protocol 头（`setup_sects`、`syssize`、`init_size`），并断言 `HdrS` 魔数、`0x55AA` boot flag、payload 偏移 512/2 KiB 对齐。

### 3.4 产物清单

| 文件 | 说明 |
|---|---|
| `build/bzImage` | 最终可引导镜像（大小随 KTEST 等开关变化） |
| `build/vmlinux.elf` | 带符号与调试信息，gdb 用 |
| `build/vmlinux.bin` | 裸 payload（按 LMA 导出） |
| `build/vmlinux.map` | 链接映射，排符号地址用 |
| `build/setup.bin` | 真模式 setup（补零、已回填跳转） |
| `build/disk.img` | 64 MiB FAT32 整盘镜像（无分区表） |
| `build/usr/<程序名>` | 静态用户态 ELF |
| `build/**/*.d` | 头文件依赖（`-MMD -MP` 生成） |
| `build/qemu.log` | `make run` 产生的 QEMU 中断跟踪 |
| `build/lnxrm.iso` | `make imageiso` 产物（GRUB 引导） |

`build/` 不入库。首次 clone 与 `make clean` 之后必须重跑构建。默认 `make` **不生成** `disk.img`（它是 `run` 的依赖），直接拿 QEMU 手动挂 `build/disk.img` 会报 `Could not open`——先 `make usr build/disk.img` 或直接 `make run`。

### 3.5 增量构建与可复现性

- 依赖跟踪：每个 `.o` 有同名 `.d`，头文件改动只触发受影响文件重编；`touch include/types.h` 会引发大面积重编是正常现象。
- 并行安全：`make -j$(nproc)` 无竞态；目录创建靠 `| $(BUILD)` 依赖 + `mkdir -p` 双保险。
- 改了 `_start32`/`_start64` 相对位置但启动跳错地址：可能是符号地址变了而 `setup.bin` 未重 patch，`rm -f build/setup.bin build/bzImage && make`。
- 可复现构建：`uname.version` 里嵌了 `__DATE__`/`__TIME__` 构建时间戳；设 `SOURCE_DATE_EPOCH` 后 gcc 钉死这两个宏，连续重建 `bzImage` 的 SHA256 完全一致。

## 4. 运行

### 4.1 `make run` 的 QEMU 命令

```sh
qemu-system-x86_64 [--enable-kvm] -machine q35 -m 256 -smp 2 \
  -cpu qemu64,+smep,+smap -kernel build/bzImage \
  -drive file=build/disk.img,format=raw,if=ide,index=0,media=disk \
  -serial stdio -no-reboot -d int -D build/qemu.log
```

| 参数 | 含义 |
|---|---|
| `-machine q35` | Q35 机型（可用 `MACHINE=` 换） |
| `-m 256` | 256 MiB 内存。内核物理内存管理窗口上界是 2 GiB（`PMM_WINDOW_TOP`），要覆盖高地址路径需 `-m 2048` 以上 |
| `-smp 2` | 两个 vCPU，触发 SMP 路径 |
| `-cpu qemu64,+smep,+smap` | 基础 qemu64 没有 SMEP/SMAP，不显式打开则内核的 CPUID 门控直接跳过这两项防护 |
| `-kernel build/bzImage` | QEMU 的 linuxboot 加载器，走 `setup.asm` 路径 |
| `-drive ...,if=ide` | IDE 磁盘（内核只测过 IDE 路径） |
| `-serial stdio` | 串口接到终端——所有 `kprintf` 输出在这里 |
| `-no-reboot` | 三重故障等严重异常时停住而不是重启 |
| `-d int -D build/qemu.log` | 每次中断的 CPU 状态快照写进 `build/qemu.log`，排查中断路由/三重故障用 |

无头环境（SSH/CI）加 `-display none`；换 AHCI 测试时把 `-drive` 改为 `if=ahci,id=ahci0`（AHCI 路径另有已知问题，见 GAP_ANALYSIS.md）。

### 4.2 `make smoke`

冒烟测试 = 构建 + 静态产物检查 + 无头启动 + 驱动 shell + 伪造损坏盘，共六阶段，全部通过才退出码 0：

| 阶段 | 做什么 | 判据类别 |
|---|---|---|
| 1 toolchain | 检查必需工具在 PATH | 缺工具即 FAIL；`fsck.vfat`/`mdir` 缺失记 SKIP |
| 2 build | `make -j all build/disk.img`（`--clean` 先删 `build/`） | make 退出码 |
| 3 artifacts | 静态检查 `bzImage` 头（boot flag、`HdrS`、`setup_sects`、`syssize` 与 `setup.bin` 大小互证） | `vmlinux.elf` 关键符号与入口地址、`disk.img` 大小与 `fsck.vfat`、`/bin` 内容、每个用户 ELF 是 ET_EXEC/静态/入口在用户基址 |
| 4 boot | 无头 QEMU 启动，按顺序等串口标记 | e820 → pmm/vmm → CR0.WP+SMEP+SMAP → 磁盘容量 64 MiB → FAT32 日志与挂载 → `[ktest]` 各阶段 0 failed → `selftest PASS` → pid 1 → AP 标记 → `[boot] ready` → shell 提示符；另有反向检查：无 `[PANIC]`、无 FAIL、selftest 计数自洽、无全 1 磁盘容量 |
| 5 shell | 通过串口驱动 `ls`/`cat`/`kill`，跑 `systest` | 逐条匹配 `systest` 的 `tag=value` 输出直到 `systest.done=1` |
| 6 forged FAT | 造镜像副本、伪造 `/CYC` 的 FAT 链（自环、越界项），再启动并读它 | 驱动必须报出损坏且 shell 仍返回，不许跟着坏链走死 |

用法与产物：

```sh
make smoke                          # 当前全绿（179/179 checks）
make smoke SMOKE_FLAGS="--smp 1"    # 单 CPU 跑；必须用这个开关
make smoke SMOKE_FLAGS="--no-build" # 复用 build/ 里现成产物
```

- 强制 `KTEST=1`：smoke 校验的 `[ktest]` 标记只有该模式才有，`make KTEST=0 smoke` 不会把它带偏。
- 单/双 CPU 各有独立标记集（双 CPU 要求 AP 打印 CR0/CR4、内核报 2 CPUs online；单 CPU 则反过来禁止出现 AP 行），所以**单 CPU 必须用 `--smp 1` 而不是 `--qemu-args "-smp 1"`**。
- 完整串口记录在 `build/smoke/serial.log`（`--smp 1` 时是 `serial-smp1.log`，伪造盘的两次在 `serial-forged-*.log`），可直接核对内核自测行，如 `[ktest] selftest PASS (87 cases, 1651 checks)`。
- 阶段 6 用的是副本 `build/smoke/forged.img`，不碰 `build/disk.img`。

手动判据（不跑 smoke 时）：串口出现 `[boot] ready in N ms` 与 `[ktest] selftest PASS`，且无 `[PANIC]`、无三重故障。

## 5. 磁盘镜像与 GRUB ISO

### 5.1 `disk.img`

配方（Makefile 内）：`dd` 造 64 MiB 空镜像 → `mkfs.vfat -F 32 -R 1024` → 拷入 `README.md`、建 `::bin`、逐个 `mcopy` 用户 ELF。用宿主机 mtools 造镜像，避免"内核要先能挂 FAT32 才能跑、但镜像要有人造"的鸡生蛋问题；造出的镜像是标准 FAT32，可直接 `fsck.vfat`/`mdir` 检查，也可挂到宿主机查看。

`-R 1024` 保留 1024 个扇区：内核把崩溃一致性日志放在保留区 8..reserved-1，mkfs 默认的 32 扇区只够十几条记录，不够 unlink/write 事务用。

镜像是整盘 FAT32（无 MBR 分区）。内核挂载时先试整盘 FAT32，失败才走 MBR 分区。

```sh
mdir -i build/disk.img ::/          # 列根目录
mdir -i build/disk.img ::bin        # 列 /bin
fsck.vfat -n build/disk.img         # 只检查不修复
```

### 5.2 GRUB ISO（`make imageiso`）

`make imageiso` 一条命令出 `build/lnxrm.iso`：暂存树 `build/iso/`（`boot/bzImage` + `boot/grub/grub.cfg`），全部中间文件在 `build/` 内，`make clean` 一并清掉。`grub.cfg` 由 Makefile 生成，`make clean && make imageiso` 也能一次跑通；缺 `grub-mkrescue` 会打印安装提示而不是留半成品（`.DELETE_ON_ERROR`）。

生成的 `grub.cfg`：

```
serial --unit=0 --speed=115200
terminal_output serial console
set timeout=0
set default=0
insmod all_video
set gfxmode=1920x1080x32,1280x720x32,auto
set gfxpayload=1920x1080x32,1280x720x32,keep
menuentry "lnxrm" {
  linux /boot/bzImage console=ttyS0
  boot
}
```

规则与坑：

- **`gfxpayload` 必须写 `x32` 形式**（如 `1280x720x32`），**不能写 `1280x720,32`**：逗号是 GRUB 的后备模式列表语法，`1280x720,32` 会被解析成"先试 1280x720，再试 32"，内核实际拿到 **24 bpp**；只有 `x32` 才让 GRUB 真的编程 32 bpp 模式。列表末位用 `keep` 表示沿用 GRUB 已设的模式。
- 不设 `gfxpayload` 时 GRUB 留下文本模式，内核报 `[fb] no linear framebuffer (loader left text mode)`，回退 VGA 文本路径继续启动，自测照常通过，只是帧缓冲用例跳过。
- `terminal_output serial console` 必须有：不设的话 GRUB 输出全在 VGA 上，`-display none` 时什么都看不到。
- **ISO 里没有根文件系统**，`disk.img` 仍是独立 IDE 盘，启动必须两个都挂；`imageiso` 不重建磁盘镜像。

```sh
# 从光盘启动（-boot d 不加可能先从硬盘起）
qemu-system-x86_64 -machine q35 -m 2048 -smp 2 -cpu qemu64,+smep,+smap \
  -display none -boot d -cdrom build/lnxrm.iso \
  -drive file=build/disk.img,format=raw,if=ide,index=0,media=disk \
  -serial stdio -no-reboot
```

走 GRUB 的判据：串口日志出现 `BOOT_IMAGE=/boot/bzImage`，且帧缓冲来自 `boot_params.screen_info`（`mode` 字段与 `-kernel` 路径不同）。

## 6. 调试

### 6.1 串口日志（最常用）

所有 `kprintf` 走串口驱动，输出去 `-serial` 指定的目标：`-serial stdio` 终端直看、`-serial file:路径` 存文件、`-serial null` 只要 gdb 时。smoke 的完整记录见 4.2。

### 6.2 debugcon 早期标记（最快的分诊手段）

`arch/entry64.S` 在关键阶段向端口 `0xE9` 打单字符标记：

| 标记 | 位置 |
|---|---|
| `K` | 进入 `_start64` |
| `b` | 段寄存器设完 |
| `c` | 主页表建完 |
| `d` | 进入 `.activate` |
| `f` | `CR4.PAE` 设完 |
| `e` | `load_cr3` 完成 |
| `M` | bss 已清，即将 `call start_kernel` |

必须显式开 `-debugcon stdio`（不加看不到）。串口日志要 `console_init()` 之后才有，而这条通道从第一条指令就有——`KbcdfeM` 全有说明已经进到 C 世界的门口。

### 6.3 GDB（源码级）

构建自带完整调试信息（gcc `-O2 -g`、nasm `-F dwarf -g`）。

```sh
# 终端 1：冻结启动并开 GDB stub（-serial null 避免与 gdb 抢终端）
qemu-system-x86_64 -machine q35 -m 256 -smp 2 -display none -no-reboot \
  -kernel build/bzImage \
  -drive file=build/disk.img,format=raw,if=ide,index=0,media=disk \
  -serial null -S -s          # -S 启动即冻结；-s 在 TCP 1234 开 stub

# 终端 2
gdb build/vmlinux.elf
(gdb) target remote :1234
(gdb) break start_kernel
(gdb) continue
```

常用断点：`start_kernel`、`panic`、`schedule`、`syscall_entry`、`isr_common`、`fat_mount_impl`。常用命令：`x/8i $rip`、`info symbol $rip`、`info threads`（另一个 CPU 的上下文）、`p/x this_cpu_data()->_current`。注意 SMP 下断点会命中所有 CPU 的执行流，调试 AP 需要额外手段拉出它的 `rip`。

### 6.4 QEMU 中断跟踪（`build/qemu.log`）

`make run` 默认带 `-d int -D build/qemu.log`，记录每次中断的 CPU 状态快照（EIP/EFL/段寄存器）。用途：

- 排查中断向量路由；找三重故障（连续大量 `#PF` 紧跟 `#DF`）。
- 抓"启动闪一下就重启"：能复位的必然不是 `panic()`（它只 `cli;hlt` 挂死），配 `-no-reboot` 让 QEMU 停在当场，日志尾部的出错 EIP 用 `addr2line -e build/vmlinux.elf <EIP>` 定位根因。
- `grep '^EIP=' build/qemu.log | sort | uniq -c | sort -rn | head -20` 看哪些地址在反复执行。

IDT 建好之前的三重故障同样有 `-d int` 记录，但串口一行都不会打——别因为"没日志"以为没跑到那。

### 6.5 QEMU monitor（交互式/截图）

```sh
qemu-system-x86_64 ... -monitor stdio   # 与 -serial stdio 冲突，用 -serial null 错开
```

有用命令：`info registers`、`info cpus`、`x/16i $eip`、`xp /16gx 0x50000`（读物理地址看页表）、`info mtree`、`screendump /tmp/screen.ppm`（抓当前屏幕，PPM 可用常见看图工具打开）、`quit`。

## 7. 排障

| 症状 | 原因 | 处置 |
|---|---|---|
| QEMU 报 `Could not open 'build/disk.img'` | 默认 `make` 只构建内核，磁盘镜像是 `run` 的依赖 | `make usr build/disk.img` 或直接 `make run` |
| 光盘启动停在 `Stop!`，日志有 `[init] cannot open /bin/init` | `disk.img` 损坏或过期（`imageiso` 不重建它） | `rm build/disk.img && make build/disk.img`，用 `fsck.vfat -n`/`mdir` 复核 |
| `fsck.vfat` 报 `FATs differ`，或 `mdir` 报 Fat problem | `mcopy` 循环写到一半被打断（如多个 QEMU 与重建镜像抢写） | 删掉重建（同上）。`make` 只看时间戳，会把损坏镜像判成"已是最新"，别用它判断镜像健康 |
| `patch_setup_jmp.py` 报找不到 `jmp 0x18:0xF00DFACE` | `setup.asm` 的哨兵跳转被改，或 `vmlinux.elf` 缺 `_start32`/`_start64` | `grep -n 'F00DFACE' arch/setup.asm`；`nm build/vmlinux.elf \| grep -E '_start(32\|64)'` |
| `patch_bzimage.py` 断言失败 | `HdrS` 魔数或 `0x55AA` 被破坏、setup 补零逻辑被改、setup 缩到 2 KiB 以下 | `xxd -s 0x1f0 -l 32 build/bzImage` 查头部区域 |
| 链接报 `undefined reference to '__stack_chk_fail'` | 某处没继承带 `-fno-stack-protector` 的标志集 | 检查该文件的编译规则是否用 `KERNFLAGS`/`USRFLAGS` |
| 用户态链接报 `undefined reference to '_start'` | 链接没带 `crt0.o` | 确认走 `usr/Makefile` 的 `$(USER_ELFS)` 规则而非手写链接命令 |
| 改了 `_start32`/`_start64` 后启动跳错地址 | 符号地址变了但 `setup.bin` 没重 patch | `rm -f build/setup.bin build/bzImage && make` |
| 串口停在 `M` 之后无输出 | `console_init`/串口初始化问题，或 bootinfo 采集读到垃圾 | gdb 断 `start_kernel`，单步看 `console_init`/`kvprintf` 前后 |
| 内核日志 `[vfs] no disk found` | IDE 探测失败（看 `[ide]` 日志），或镜像不是合法整盘 FAT32 | `rm build/disk.img` 重建；怀疑 `mkfs.vfat` 静默失败时去掉 Makefile 里的 stderr 抑制重跑 |
| GRUB 下拿到 24 bpp 而不是 32 | `gfxpayload` 写成 `1280x720,32`（逗号形式是后备列表语法） | 写成 `1920x1080x32,1280x720x32,keep` 这种 `x32` 形式，见 5.2 |
| 无 framebuffer，日志报 `no linear framebuffer` | GRUB 没设 `gfxpayload`，留在文本模式 | 内核会回退 VGA 文本模式继续跑；要 32 bpp 就补上 5.2 的配置 |
| `make imageiso` 报 `grub-mkrescue not found` | 缺 GRUB 救援盘工具 | 装 `grub-pc-bin` + `xorriso`（Fedora：`grub2-pc`/`grub2-tools` + `xorriso` + `mtools`） |
| `make smoke` 阶段 4 卡在 AP 相关标记 | 用 `--qemu-args "-smp 1"` 关了第二个 CPU，但标记仍是双 CPU 集 | 用 `make smoke SMOKE_FLAGS="--smp 1"`，脚本会换标记集并另存日志 |
| `make smoke` 第一阶段报缺 `g++` | 构建已不用 C++，但 smoke 的工具链检查仍列了它 | 装 `g++`（不影响构建本身） |
| gdb 与串口互相干扰 | `-serial stdio` 和 gdb/monitor 抢终端 | 调试时用 `-serial null` |
| 真机启动闪一下就重启 | 三重故障（`panic` 只挂死不复位，能复位的另有其因） | 用 `-m 2048` 以上复现（`-m 256` 走不到 2 GiB 窗口上界，高地址 bug 永远不暴露），配 `-no-reboot` + `-d int` 按 6.4 定位 |
