# lnxrm 构建指南

x86-64 freestanding 内核（C + NASM），构建链零警告。本文覆盖：依赖、make 目标、构建流程与产物、QEMU 运行与冒烟、磁盘镜像与 GRUB ISO、调试与排障。代码结构见 CODEBASE.md，缺陷与计划见 GAP_ANALYSIS.md。

## 1. 依赖

```sh
sudo apt install make clang lld llvm binutils nasm python3 \
                 dosfstools mtools qemu-system-x86 gdb
```

可选：`grub-pc-bin` + `xorriso`（`make imageiso`）、`fsck.vfat`/`mdir`（smoke 检查，缺则 SKIP）、`g++`（构建不用，但 smoke 工具链检查要求在 PATH）。

`binutils` 仍要装：编译/链接/取 payload 已经全走 LLVM（`clang` → `ld.lld` → `llvm-objcopy`），但 `scripts/patch_setup_jmp.py` 用 `nm` 查 `_start32/64`，且 smoke 的工具链检查把 `ld`/`objcopy` 列为必需，缺则整个 smoke 起不来。

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

userland/bin/*.c + crt0.S + ulib.c ─► build/user/<名>（静态 ELF）
userland/mod/*.c + crt0.S + ulib.c ─► build/mod/<名>.km（同配方，独立对象子目录；`.km` 是链接目标名的一部分）
build/user/* + build/mod/* + README.md ──mkfs.vfat/mcopy──► build/disk.img（::bin 与 ::mod/<名>.km，名字原样搬）
```

- 源文件用 `find`/`wildcard` 自动发现：新增内核 `.c`、`userland/bin/*.c` 或 `userland/mod/*.c` 不用改 Makefile；头依赖由 `-MMD -MP` 的 `.d` 跟踪。
- 对象按 `KTEST` 分目录（`build/obj0` / `build/obj1`），两种模式并存，`make` ↔ `make KTEST=1` 切换只重链一次（0.2 s、零 `CC`），不再全量重编 70 个 TU——旧的 `build/.ktest-mode-<n>` stamp 因此删除。**模式标记 `build/.mode-<n>` 必须挂在 `vmlinux.elf` 上、不能挂在对象上**：翻转后每个 `.o` 都比 elf 旧，标记不挂链上，`make` 就会认定上一模式的 `bzImage` 仍最新，把 `KTEST=1` 的内核当 `KTEST=0` 发给你；挂回对象上则又变回全量重编。**该规则还必须排在 `all:` 之后**——它是 Makefile 里第一个不以 `.` 开头的目标，写在 `all:` 前面会夺走默认目标，`make` 从此只检查这个标记、再也不建 `bzImage`。产物一律仍在 `build/` 根（`smoke_test.py` 硬编码 `bzImage`/`vmlinux.elf`/`setup.bin`/`disk.img`/`user/`），`make clean` 照旧整个删。
- 关键标志：`-ffreestanding -fno-stack-protector -fno-omit-frame-pointer -mno-red-zone -mno-sse* -mcmodel=kernel -Os -g -ffunction-sections -fno-pic`（优化档是 **`-Os`** 而非 -O2；`-fno-omit-frame-pointer` 是 PP2 backtrace 的前提——`-Os` 下 clang 默认丢 rbp，帧指针走链会当场断掉）；用户态编译 `-ffreestanding -fno-pic -mcmodel=large`，链接 `-nostdlib -nostartfiles -static`（链接专用开关单独放 `USERLDFLAGS`，混进编译命令会被 clang 报 `-Wunused-command-line-argument`），链接在 `0x7f8000400000`。
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
| `build/disk.img` | 64 MiB FAT32 整盘（无分区表；含 `::README.md`、`::bin`、`::mod`） |
| `build/user/<名>` | 静态用户态 ELF |
| `build/mod/<名>.km` | ring3 驱动模块 ELF（**`.km` 是链接出来的名字**，`MOD_ELFS` 直接 `addsuffix`；装进 `::mod` 时名字原样搬，由 `init` 扫目录拉起） |
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
| 1 toolchain | 必需工具在 PATH（缺 `fsck.vfat`/`mdir` 记 SKIP） |
| 2 build | `make -j all build/disk.img`（`--clean` 先删 `build/`） |
| 3 artifacts | bzImage 头静态检查、`vmlinux.elf` 符号与入口、`disk.img` 与 `fsck.vfat`、`/bin` 内容（`::mod` 只要求存在，**模块名从不进判据**）、用户 ELF 形态 |
| 4 boot | 无头启动按序等串口标记：e820 → 内存 → CR0/CR4 → FAT32 挂载 → `[ktest]` 各阶段 0 failed → `selftest PASS` → pid 1 → AP → `[boot] ready` → shell；反向查无 `[PANIC]`/FAIL |
| 5 shell | 串口驱动 `ls`/`cat`/`kill` + `systest`，匹配 `tag=value` 直到 `systest.done=1` |
| 6 forged FAT | 副本镜像伪造某文件的 FAT 链，必须报 `chain loop`/`corrupt FAT entry` 且 shell 仍返回 |

```sh
make smoke                          # 270/270 checks
make smoke SMOKE_FLAGS="--smp 1"    # 单 CPU 必须用这个，不用 --qemu-args
make smoke SMOKE_FLAGS="--no-build" # 复用现有 build/
make smoke SMOKE_FLAGS="--repeat 2" # 连跑抖动门（OBS-1）：boot+shell 与伪造盘
                                    # 各跑 2 轮，失败轮串口留档 serial-fail-<i>.log
make smoke SMOKE_FLAGS="--timeout 900"  # 主机高负载时加宽每次等待（默认 420 s）：
                                    # TCG 下 enospc 填 62 MiB 实测可达 504 s，判据不变
make smoke SMOKE_QEMU_ARGS="-machine q35 -smp 1"   # 换机型（默认已是 -machine q35）
```

- 机型：`SMOKE_QEMU_ARGS`（默认 `-machine q35`）在 `SMOKE_FLAGS` **之前**注入 `--qemu-args`；argparse 取最后一个，所以自带 `--qemu-args` 的 `SMOKE_FLAGS` 优先。**别用 `SMOKE_FLAGS="--qemu-args '-machine pc'"`**——i440fx 既无 AHCI 也无 IDE 驱动（`ide.c` 已删），根挂不上。

- 单/双 CPU 标记集不同（双 CPU 要求 AP 行、2 CPUs online；单 CPU 反之），所以单 CPU 必须走 `--smp 1` 换标记集。
- 日志：`build/smoke/serial.log`（`--smp 1` → `serial-smp1.log`，伪造盘 → `serial-forged-*.log`，`--repeat` 失败轮 → `serial-fail-<i>.log`）；阶段 6 用副本 `forged.img`，不碰 `disk.img`。
- 手动判据：`[boot] ready in N ms` + `[ktest] selftest PASS (162 cases, 4974 checks) [2 skipped]`（2026-11-19 基线；2026-11-17 S1a 为 162/4951、3 skipped），无 `[PANIC]`、无三重故障。

## 5. 磁盘镜像与 GRUB ISO

### 5.1 `disk.img`

配方（Makefile 内）：`dd` 64 MiB → `mkfs.vfat -F 32 -R 1024` → 拷 `README.md`、建 `::bin` 与 `::mod`、逐个 `mcopy` 用户 ELF 与模块。宿主机 mtools 造镜像，绕开"内核要能挂 FAT32 才能跑、镜像要有人造"的鸡生蛋问题。**`::mod` 里的文件名与 `build/mod/` 里的一模一样**——`.km` 在链接期就由 `MOD_ELFS` 加好，安装只是 `basename` 搬运（再补一次会得到 `ps2-keyboard.km.km`），之后由 `init` 启动时扫目录拉起（见 CODEBASE §6）；`disk.img` 把 `Makefile` 列为前置，改这条规则会自动重建镜像。

`-R 1024` 保留 1024 扇区：内核把崩溃一致性日志放在保留区 `8..reserved-1`，mkfs 默认 32 扇区只够十几条记录。镜像是整盘 FAT32（无 MBR），内核先试整盘、失败才走分区。

```sh
mdir -i build/disk.img ::bin        # 列 /bin
mdir -i build/disk.img ::mod        # 列 /mod（ring3 驱动模块）
fsck.vfat -n build/disk.img         # 只检查不修复
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

## 6. 调试

- **串口**（最常用）：所有 `kprintf` 走这里，`-serial stdio` 直看 / `file:` 存档 / `null` 让给 gdb。
- **debugcon 早期标记**（最快的分诊）：`arch/entry64.S` 向端口 `0xE9` 打单字符，`-debugcon stdio` 才可见——`K`(进 `_start64`) `b`(段寄存器) `c`(主页表) `d`(.activate) `f`(CR4.PAE) `e`(load_cr3) `M`(bss 清完、进 C)。`KbcdfeM` 全有 = 已到 C 世界门口；串口要 `console_init()` 之后才有。
- **GDB**：构建带 `-O2 -g` / nasm `-F dwarf -g`。
  ```sh
  qemu-system-x86_64 ... -serial null -S -s   # 冻结 + TCP 1234 stub
  gdb build/vmlinux.elf
  (gdb) target remote :1234
  (gdb) break start_kernel
  ```
  常用断点：`start_kernel`、`panic`、`schedule`、`syscall_entry`、`isr_common`；`info symbol $rip`、`p/x this_cpu_data()->_current`。SMP 下断点命中所有 CPU。
- **`-d int` 中断跟踪**（`make run` 默认写 `build/qemu.log`）：找三重故障（大量 `#PF` 紧跟 `#DF`）、抓"闪一下就重启"（能复位的不是 `panic()`，它 `cli;hlt`），尾部 EIP 用 `addr2line -e build/vmlinux.elf <EIP>` 定位；`grep '^EIP=' build/qemu.log | sort | uniq -c | sort -rn | head` 看热点。IDT 之前的三重故障也有记录，但串口无输出——别以为"没日志 = 没跑到"。
- **monitor**：`-monitor stdio`（与 `-serial stdio` 冲突，用 `-serial null` 错开）。`info registers`、`info cpus`、`x/16i $eip`、`xp /16gx 0x50000`、`screendump`。

## 7. 排障

| 症状 | 原因 | 处置 |
|---|---|---|
| `Could not open 'build/disk.img'` | 默认 `make` 只建内核 | `make user build/disk.img` 或 `make run` |
| `ps` 里没有 `/mod/*.km`、串口也没有模块行 | **`::mod` 里的文件名不以 `.km` 结尾**（`MOD_ELFS` 的输出名或 `mcopy` 的目标名被改错）：init 的筛子只认 `.km`，两个模块全被静默 `continue`——镜像、串口、`ps` 三处**各自看起来都正常**（2026-11-18 实测的成因：后缀原先指望安装步骤补，而它没补）。**成功路径现在一行不打印**，所以「有没有起来」只能看 `ps` | `mdir -i build/disk.img ::mod` 看后缀；`rm build/disk.img && make build/disk.img`（`disk.img` 现在把 `Makefile` 当依赖，改这两条规则会自动重建）；起不来时串口会打 `init: N file(s) in /mod, 0 spawned -- no module was started` |
| 链接报 `undefined symbol: trampoline_start` | **`BUILD=` 被覆盖到别处**：`Makefile` 的 `--redefine-sym _binary_build_trampoline_bin_*` 写死了默认路径，而 `llvm-objcopy -I binary` 的符号名由输入路径导出 | 用默认 `BUILD=build`（唯一受支持的用法）；或手工按实际路径重新 `--redefine-sym` 生成 `$(OBJDIR)/trampoline.o` |
| 光盘启动停在 `Stop!`，`cannot open /bin/init` | `disk.img` 损坏/过期（`imageiso` 不重建） | `rm build/disk.img && make build/disk.img`，`fsck.vfat -n` 复核 |
| `fsck.vfat` 报 `FATs differ` | `mcopy` 写一半被打断（多 QEMU 抢写） | 删掉重建；`make` 只看时间戳，别用它判断镜像健康 |
| `patch_setup_jmp.py` 找不到 `jmp 0x18:0xF00DFACE` | 哨兵跳转被改，或缺 `_start32/64` | `grep -n F00DFACE arch/setup.asm`；`nm build/vmlinux.elf \| grep _start` |
| `patch_bzimage.py` 断言失败 | `HdrS`/`0x55AA` 被破坏或 setup 补零逻辑被改 | `xxd -s 0x1f0 -l 32 build/bzImage` |
| `undefined reference to '__stack_chk_fail'` | 某文件没继承 `-fno-stack-protector` | 检查是否走 `KERNFLAGS`/`USERFLAGS` |
| 用户态 `undefined reference to '_start'` | 链接没带 `crt0.o` | 走根 Makefile 的 `$(USER_ELFS)` 规则 |
| 改 `_start32/64` 后跳错地址 | 符号变了但 `setup.bin` 没重 patch | `rm -f build/setup.bin build/bzImage && make` |
| 串口停在 `M` 无输出 | `console_init`/bootinfo 问题 | gdb 断 `start_kernel` 单步 |
| `[vfs] no disk found` | AHCI 没探到盘，或镜像不是合法整盘 FAT32（**不是 IDE 的事**：`ide.c` 已删，只剩 AHCI 一条栈） | `rm build/disk.img` 重建；确认没用 `MACHINE=pc`/`-machine pc`（i440fx 无 AHCI） |
| GRUB 下 24 bpp 而非 32 | `gfxpayload` 写成逗号形式 | 写 `x32` 形式，见 5.2 |
| 无 framebuffer | GRUB 没设 `gfxpayload` | 内核回退 VGA；要 32 bpp 补 5.2 配置 |
| `imageiso` 报 `grub-mkrescue not found` | 缺工具 | 装 `grub-pc-bin` + `xorriso` |
| smoke 阶段 4 卡在 AP 标记 | 用 `--qemu-args "-smp 1"` 关了 CPU 但标记仍是双 CPU 集 | 用 `SMOKE_FLAGS="--smp 1"` 换标记集 |
| smoke 在 enospc 填盘段撞等待超时（阶段 4 等 `late selftest` 超时、阶段 6 forged 无 prompt），串口停在 `vfs/test_enospc_on_full_volume` | `test_enospc_on_full_volume` 静默填满 62 MiB——实测主机 load≈6.7 时 TCG 需 **504 s**，超过默认 420 s 门（forged 是整个 boot 一次预算，余量最薄）；上一轮若被杀在中途留下 `ENOSPC.T` 残留（`mdir -i build/disk.img ::` 可见），O_TRUNC 重填翻倍更必超 | 主机降载后重跑，或 `SMOKE_FLAGS="--timeout 900"`（判据不变、只放宽等待）；有残留先 `rm build/disk.img && make build/disk.img`（`fsck.vfat -n` 应显示 `374/128046 clusters`） |
| smoke 阶段 1 报缺 `g++` | 工具链检查仍列了它 | 装 `g++`（不影响构建） |
| gdb 与串口抢终端 | 都用 stdio | 调试时 `-serial null` |
| 真机闪一下就重启 | 三重故障（`panic` 只挂死不复位） | `-m 2048` 复现（`-m 256` 走不到窗口上界），配 `-no-reboot -d int` 按 6 节定位 |
