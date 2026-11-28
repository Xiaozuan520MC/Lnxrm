# lnxrm 构建指南

x86-64 freestanding 内核（C + NASM），构建链零警告。本文覆盖：依赖、make 目标、构建流程与产物、QEMU 运行与冒烟、磁盘镜像与 GRUB ISO。**调试与排障见 `ERROR.md`。**代码结构见 `../CODEBASE.md`（§1–§10 分布于 `CODEBASE/CODEBASE_*.md`），缺陷与计划见 `../GAP_ANALYSIS.md`（已收口条目在 `../GAP_ANALYSIS_DONE.md`）。

## 1. 依赖

```sh
sudo apt install make clang lld llvm binutils nasm python3 \
                 e2fsprogs qemu-system-x86 gdb
```

可选：`grub-pc-bin` + `xorriso`（`make imageiso`）、`e2fsck`（smoke 检查，缺则 SKIP）、`g++`（构建不用，但 smoke 工具链检查要求在 PATH）。

`binutils` 仍要装：编译/链接/取 payload 已经全走 LLVM（`clang` → `ld.lld` → `llvm-objcopy`），但 `scripts/patch_setup_jmp.py` 用 `nm` 查 `_start32/64`，且 smoke 的工具链检查把 `ld`/`objcopy` 列为必需，缺则整个 smoke 起不来。

**S2 起根文件系统是 ext2**，造镜像/列目录/查错分别是 `mke2fs`、`debugfs`、`e2fsck`（同属 `e2fsprogs`）；`dosfstools`/`mtools` 只有在你还要手工造 FAT 卷时才需要，构建与 smoke 已不再用它们。

不钉版本；已验证 clang 21 / lld 21 / LLVM objcopy 21 / binutils 2.46 / nasm 3.01 / python 3.14 / QEMU 10.x。`CROSS` 前缀同时作用于 `CC`/`LD`/`OBJCOPY`，如 `CROSS=x86_64-linux-gnu-` 可换整套交叉工具链——注意此时 `LD` 解析成 `<prefix>ld.lld`、`OBJCOPY` 成 `<prefix>llvm-objcopy`，要求同前缀的 LLVM 链接器与 objcopy 在 PATH。

## 2. make 目标与变量

| 目标 | 做什么 |
|---|---|
| `all`（默认） | 只建 `build/bzImage`，**不含**用户态与磁盘镜像 |
| `user` | 全部用户态 ELF → `build/user/`（`userland/bin/*.c` 自动发现） |
| `mod` | 全部 ring3 驱动模块 → `build/mod/<名>.km`（`userland/mod/*.c` 自动发现，S1b；`disk.img` 会自动触发） |
| `run` | `user` + `mod` + `bzImage` + `disk.img` 后启动 QEMU（唯一自动挂盘的常用入口） |
| `smoke` | 六阶段冒烟，强制 `KTEST=1` |
| `imageiso` | `build/lnxrm.iso`（GRUB，不重建 `disk.img`） |
| `clean` | 删整个 `build/` |

| 变量 | 默认 | 作用 |
|---|---|---|
| `KTEST=1` | `0` | 编入 `ktest/` 自测；对象按模式分目录（`build/obj0` / `build/obj1`），两模式并存，切换只重链约 0.2 s、零重编 |
| `DISK=0` | `1` | 不挂盘，快速看早期启动日志 |
| `MACHINE` / `KVM` | `q35` / 自动探测 | QEMU 机型；`/dev/kvm` 可用即加 `--enable-kvm`。**`MACHINE=pc`（i440fx）已无磁盘驱动**（`ide.c` 删除）⇒ 挂不了根，只用于不碰盘的自测 |
| `CMDLINE` | 空 | `make run` 传给 QEMU `-append` 的内核命令行（内核解析 `loglevel=<0..3>`、`ktest=off\|continue`）；**空则不加 `-append`**，与不带它的旧行为逐位一致 |
| `SMOKE_FLAGS` | 空 | 原样传给 `scripts/smoke_test.py`，如 `--smp 1` |
| `SMOKE_QEMU_ARGS` | `-machine q35` | 在 `SMOKE_FLAGS` **之前**传给 `--qemu-args`；`smoke_test.py` 的 argparse 取**最后一个**，故你自己给 `--qemu-args` 仍可覆盖。2026-11-18 起 smoke 由此走 q35（ICH9 AHCI），不再落 QEMU 默认 i440fx + PIIX3 IDE |

## 3. 构建流程与产物

### 3.1 流程

```
kernel/**.c, arch/*.c ──cc──► build/obj<N>/**/*.o ─┐
arch/*.S ──nasm──► build/obj<N>/arch/*.o    │
trampoline.S ─nasm─► bin ─llvm-objcopy─► .o  ├─ ld.lld -T arch/kernel.ld --gc-sections ─► vmlinux.elf
ktest/*.c（仅 KTEST=1）──cc──► .o           │      └─ llvm-objcopy -O binary ─► vmlinux.bin ─┐
setup.asm ─nasm─► 补零 2 KiB 整数倍 ─patch_setup_jmp.py─► setup.bin ────────────────┴─ cat ─► patch_bzimage.py ─► bzImage

userland/bin/*.c + crt0.S + ulib.c ─► build/user/<名>（init 静态 ELF 链在 `0x7f8000400000`；其余程序 `-pie` 链的 ET_DYN，link-at-0，基址由内核 `elf_place()` 选并在 `AT_BASE`/`AT_ENTRY` 里公布）
userland/mod/*.c + crt0.S + ulib.c ─► build/mod/<名>.km（同上 ET_DYN，装进 `/mod` 由 `init` 扫目录拉起；`.km` 是链接目标名的一部分）
build/user/* + build/mod/* + README.md ──mke2fs + debugfs──► build/disk.img（/README.md、/bin、/mod，/lib 下装 `ld.so` + `libulib.so`）
```

- 源文件用 `find`/`wildcard` 自动发现：新增内核 `.c`、`userland/bin/*.c` 或 `userland/mod/*.c` 不用改 Makefile；头依赖由 `-MMD -MP` 的 `.d` 跟踪。
- 对象按 `KTEST` 分目录（`build/obj0` / `build/obj1`），两种模式并存，`make` ↔ `make KTEST=1` 切换只重链一次（0.2 s、零 `CC`），不再全量重编 70 个 TU——旧的 `build/.ktest-mode-<n>` stamp 因此删除。**模式标记 `build/.mode-<n>` 必须挂在 `vmlinux.elf` 上、不能挂在对象上**：翻转后每个 `.o` 都比 elf 旧，标记不挂链上，`make` 就会认定上一模式的 `bzImage` 仍最新，把 `KTEST=1` 的内核当 `KTEST=0` 发给你；挂回对象上则又变回全量重编。**该规则还必须排在 `all:` 之后**——它是 Makefile 里第一个不以 `.` 开头的目标，写在 `all:` 前面会夺走默认目标，`make` 从此只检查这个标记、再也不建 `bzImage`。产物一律仍在 `build/` 根（`smoke_test.py` 硬编码 `bzImage`/`vmlinux.elf`/`setup.bin`/`disk.img`/`user/`），`make clean` 照旧整个删。
- 关键标志：`-ffreestanding -fno-stack-protector -fno-omit-frame-pointer -mno-red-zone -mno-sse* -mcmodel=kernel -Os -g -ffunction-sections -fno-pic`（优化档是 **`-Os`** 而非 -O2；`-fno-omit-frame-pointer` 是 PP2 backtrace 的前提——`-Os` 下 clang 默认丢 rbp，帧指针走链会当场断掉）；用户态编译 `-ffreestanding -fno-pic -mcmodel=large`（静态 init）、`-ffreestanding -fPIC ... -Ilib -I../include`（动态程序与 ld.so，`-fno-pie` 改为 `-pie`，不再烙 `--image-base`）；链接 `-nostdlib -nostartfiles -static`（静态 init，`USERLDFLAGS`，链接专用开关单独放，混进编译命令会被 clang 报 `-Wunused-command-line-argument`）、`-nostdlib -nostartfiles -fuse-ld=lld -pie -Wl,--hash-style=sysv -Wl,--dynamic-linker=/lib/ld.so -L$(LIBDIR) -lulib`（动态程序），ld.so 自身用 `--dynamic-linker=`（空，避免给解释器本身挂 PT_INTERP）。链接在 `0x7f8000400000`（静态 init）；动态程序 link-at-0。
- `vmlinux.elf` 查符号：`grep <sym> build/vmlinux.map` 或 `nm build/vmlinux.elf`。
- `trampoline.S` 走两段式（裸二进制 → ELF 对象，符号改名防被通配进 `.image`）；它在 `arch/kernel.ld` 里**显式摆成 `.trampoline` 段**（VMA = `.image` 末尾、LMA 紧随其后），不能留作孤儿段——GNU ld 与 ld.lld 对孤儿段的摆放不同，lld 会因 `.trampoline` 压在 `.bss` 负载地址上直接报 `load address range overlaps with .bss` 而拒链。`setup.bin` 依赖 `vmlinux.elf` 只为 `nm` 查 `_start32/64` 回填跳转；`patch_bzimage.py` 填 boot protocol 头并断言 `HdrS`/`0x55AA`/对齐。
- 链接器与对象工具是 LLVM 一套（2026-11-14 换链：`ld` → `ld.lld`、`objcopy` → `llvm-objcopy`）。几个坑：lld 不认 GNU 的 `--no-warn-rwx-segments`（它本身也没有 RWX 段警告，直接删）；link map 要写成 `-Map <file>`（空格形式），`-Map=<file>` 不产文件。
- 换链后 `nm`/`strings` 逐字节比对 GNU 产物会看到差异，属预期而非回归：lld 对 `.rodata.str1.1` 这类 `SHF_MERGE|SHF_STRINGS` 段会做尾串合并并**裁掉无人引用的死串**（如无调用者的 `kmalloc_or_panic` 的 panic 格式串），GNU ld 留着；段内布局随之位移，`.text` 里 `mov rdi, imm32` 的 rodata 地址、以及 data 段的对齐填充（GNU `nop` / lld `int3`）都会不同。判定口径是**符号表 + 运行时**，不是 payload 字节。
- 可复现：设 `SOURCE_DATE_EPOCH` 后 `bzImage` SHA256 稳定。
- `make -j$(nproc)` 安全；改了 `_start32/64` 相对位置而启动跳错：`rm -f build/setup.bin build/bzImage && make`。

### 3.2 产物

| 文件 | 说明 |
|---|---|
| `build/bzImage` | 最终可引导镜像（大小随 KTEST 变） |
| `build/vmlinux.elf` / `.bin` / `.map` | 符号+调试信息 / 裸 payload / 链接映射 |
| `build/setup.bin` | 真模式 setup（已补零、回填跳转） |
| `build/disk.img` | 64 MiB ext2 整盘（无分区表；含 `/README.md`、`/bin`、`/mod`，S2 起） |
| `build/user/<名>` | 静态用户态 ELF |
| `build/mod/<名>.km` | ring3 驱动模块 ELF（**`.km` 是链接出来的名字**，`MOD_ELFS` 直接 `addsuffix`；装进 `/mod` 时名字原样搬，由 `init` 扫目录拉起） |
| `build/qemu.log` / `build/lnxrm.iso` | `make run` 的中断跟踪 / GRUB ISO |

`build/` 不入库。**默认 `make` 不建 `disk.img`**：直接手动挂会报 `Could not open`，先 `make user build/disk.img` 或用 `make run`。

## 4. 运行

### 4.1 `make run`

```sh
qemu-system-x86_64 [--enable-kvm] -machine q35 -m 256 -smp 2 \
  -cpu qemu64,+smep,+smap -kernel build/bzImage \
  -drive file=build/disk.img,format=raw,if=ide,index=0,media=disk \
  -serial stdio -no-reboot -d int -D build/qemu.log
```

- `-cpu` 必须显式 `+smep,+smap`：基础 qemu64 没有，内核 CPUID 门控会直接跳过这两项。
- 要带内核命令行（如 `CMDLINE='loglevel=3'`）就用 `make run CMDLINE='…'`——它以 QEMU `-append` 传入；不设 `CMDLINE` 时不加该参数，启动输出与旧版一致。
- `-m 256` 够用；要覆盖 2 GiB 管理窗口（`PMM_WINDOW_TOP`）的高地址路径需 `-m 2048`。
- 无头环境加 `-display none`；AHCI 测试改 `if=ahci,id=ahci0`（已知问题见 GAP_ANALYSIS）。

### 4.2 `make smoke`

六阶段全过才退出码 0：

| 阶段 | 做什么 |
|---|---|
| 1 toolchain | 必需工具在 PATH（`mke2fs`/`debugfs` 必需，缺 `e2fsck` 记 SKIP） |
| 2 build | `make -j all build/disk.img`（`--clean` 先删 `build/`） |
| 3 artifacts | bzImage 头静态检查、`vmlinux.elf` 符号与入口、`disk.img` 与 `e2fsck -n`、`debugfs` 列 `/bin` 与逐个 `stat`（`/mod` 只要求存在，**模块名从不进判据**）、用户 ELF 形态 |
| 4 boot | 无头启动按序等串口标记：e820 → 内存 → CR0/CR4 → **ext2 挂根** → **ktest 的 FAT 种子卷挂载** → `[ktest]` 各阶段 0 failed → `selftest PASS` → pid 1 → AP → `[boot] ready` → shell；反向查无 `[PANIC]`/FAIL |
| 5 shell | 串口驱动 `ls`/`cat`/`kill` + `systest`，匹配 `tag=value` 直到 `systest.done=1` |
| 6 forged ext2 | 副本镜像伪造 `/CYC` 的目录记录（`rec_len=4`）或把文件数据块指到卷外，必须报 `rec_len … lookup stopped`/`beyond the volume … read refused` 且 shell 仍返回 |

```sh
make smoke                          # 270/270 checks（S2 起判据构成变了、总数不变：
                                    # 有序 marker 21→22 条 +1，可选工具 2→1 个 -1）
make smoke SMOKE_FLAGS="--smp 1"    # 单 CPU 必须用这个，不用 --qemu-args
make smoke SMOKE_FLAGS="--no-build" # 复用现有 build/
make smoke SMOKE_FLAGS="--repeat 2" # 连跑抖动门（OBS-1）：boot+shell 与伪造盘
                                    # 各跑 2 轮，失败轮串口留档 serial-fail-<i>.log
make smoke SMOKE_FLAGS="--timeout 900"  # 主机高负载时加宽每次等待（默认 420 s）：
                                    # TCG 下 enospc 要填满整个根卷，实测曾达 504 s，
                                    # 判据不变
make smoke SMOKE_QEMU_ARGS="-machine q35 -smp 1"   # 换机型（默认已是 -machine q35）
```

- 机型：`SMOKE_QEMU_ARGS`（默认 `-machine q35`）在 `SMOKE_FLAGS` **之前**注入 `--qemu-args`；argparse 取最后一个，所以自带 `--qemu-args` 的 `SMOKE_FLAGS` 优先。**别用 `SMOKE_FLAGS="--qemu-args '-machine pc'"`**——i440fx 既无 AHCI 也无 IDE 驱动（`ide.c` 已删），根挂不上。

- 单/双 CPU 标记集不同（双 CPU 要求 AP 行、2 CPUs online；单 CPU 反之），所以单 CPU 必须走 `--smp 1` 换标记集。
- 日志：`build/smoke/serial.log`（`--smp 1` → `serial-smp1.log`，伪造盘 → `serial-forged-*.log`，`--repeat` 失败轮 → `serial-fail-<i>.log`）；阶段 6 用副本 `forged.img`，不碰 `disk.img`。
- 手动判据：`[boot] ready in N ms` + `[ktest] selftest PASS (167 cases, 5290 checks) [2 skipped]`——**S2 预期值已由第二轮实测确认**（`ktest/t_ext2.c` 新增 5 case / 317 check，`t_ioerr` 环境适配净 −2、运行期条件 ±1；2026-11-19 基线 162/4974 [2 skipped]、2026-11-17 S1a 为 162/4951、3 skipped），无 `[PANIC]`、无三重故障。

## 5. 磁盘镜像与 GRUB ISO

### 5.1 `disk.img`

配方（Makefile 内，S2 起）：`dd` 64 MiB → `mke2fs -q -t ext2 -F -b 4096 -I 256 -N 16384 -O ^has_journal` → 一段 `debugfs -w` 脚本写 `/README.md`、建 `/bin` 与 `/mod` 并逐个写入用户 ELF 与 `.km` 模块 → 逐路径 `debugfs -R "stat …"` 复核（`debugfs` **出错也返回 0**，所以看 `Inode:` 行而不是退出码），外加 `/README.md` 的 size ≥ 20 且 mode 同时带 `0400`/`0222` 两条断言。宿主机 `e2fsprogs` 造镜像，绕开"内核要能挂 ext2 才能跑、镜像要有人造"的鸡生蛋问题；`disk.img` 把 `Makefile` 列为前置，改这条规则会自动重建镜像。

几何被 flags 钉死，驱动与 smoke 都按它写：**4 KiB 块 × 16384 = 64 MiB**、**16384 个 256 B inode**（inode 表占 4 MiB）、单块组（32768 块/组 > 16384），incompat 特性只有 `FILETYPE`。**`^has_journal` 不是省空间，是硬约束**：`ext2_mount()` 拒绝带日志的 ext3/4 卷（不回放它从不运行的日志），留着 journal 根本挂不上。`debugfs` 还能写出 FAT32 表达不了的 mode 位——`README.md` 落成 `0664`，`/` 因此是可写卷，systest 的 `stat.readable` 也看得到它的 `0400`。

镜像里的名字**原样保留大小写与长名**（`ps2-keyboard.km` 12 字符、`README.md`），不再有 8.3 截断；`init` 启动时扫 `/mod` 拉起模块（见 `CODEBASE_FS.md` §6）。

```sh
debugfs -R "ls /bin" build/disk.img          # 列 /bin
debugfs -R "ls /mod" build/disk.img          # 列 /mod（ring3 驱动模块）
debugfs -R "stat /README.md" build/disk.img  # 单个 inode（首行 Inode: N）
e2fsck -n build/disk.img                     # 只检查不修复（smoke 阶段 3 同款）
```

### 5.2 GRUB ISO（`make imageiso`）

暂存树 `build/iso/`（`boot/bzImage` + 生成的 `grub.cfg`），缺 `grub-mkrescue` 打印安装提示不留半成品。**ISO 里没有根文件系统**，启动仍要另挂 `disk.img`（`imageiso` 不重建它）。

三条硬规则：

- **`gfxpayload` 必须写 `x32` 形式**（`1920x1080x32,1280x720x32,keep`）：逗号形式 `1280x720,32` 是 GRUB 后备模式列表语法，会被解析成"再试 32"而拿到 24 bpp。
- `terminal_output serial console` 必须有，否则 `-display none` 下什么都看不到。
- 不设 `gfxpayload` 会留在文本模式：内核报 `no linear framebuffer` 后回退 VGA 继续跑，自测照常通过，只是帧缓冲用例跳过。

```sh
qemu-system-x86_64 -machine q35 -m 2048 -smp 2 -cpu qemu64,+smep,+smap \
  -display none -boot d -cdrom build/lnxrm.iso \
  -drive file=build/disk.img,format=raw,if=ide,index=0,media=disk \
  -serial stdio -no-reboot
```

走 GRUB 的判据：串口有 `BOOT_IMAGE=/boot/bzImage`，帧缓冲来自 `boot_params.screen_info`。

