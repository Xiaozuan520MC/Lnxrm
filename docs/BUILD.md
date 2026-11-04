# lnxrm 构建指南

x86-64 freestanding 内核（C + NASM），构建链零警告。本文覆盖：依赖、make 目标、构建流程与产物、QEMU 运行与冒烟、磁盘镜像与 GRUB ISO、调试与排障。代码结构见 CODEBASE.md，缺陷与计划见 GAP_ANALYSIS.md。

## 1. 依赖

```sh
sudo apt install make gcc binutils nasm python3 \
                 dosfstools mtools qemu-system-x86 gdb
```

可选：`grub-pc-bin` + `xorriso`（`make imageiso`）、`fsck.vfat`/`mdir`（smoke 检查，缺则 SKIP）、`g++`（构建不用，但 smoke 工具链检查要求在 PATH）。

不钉版本；已验证 gcc 15 / binutils 2.46 / nasm 3.01 / python 3.14 / QEMU 10.x。`CROSS=x86_64-linux-gnu-` 可换交叉工具链。

## 2. make 目标与变量

| 目标 | 做什么 |
|---|---|
| `all`（默认） | 只建 `build/bzImage`，**不含**用户态与磁盘镜像 |
| `user` | 全部用户态 ELF → `build/user/`（`userland/bin/*.c` 自动发现） |
| `run` | `user` + `bzImage` + `disk.img` 后启动 QEMU（唯一自动挂盘的常用入口） |
| `smoke` | 六阶段冒烟，强制 `KTEST=1` |
| `imageiso` | `build/lnxrm.iso`（GRUB，不重建 `disk.img`） |
| `initramfs` | `build/initramfs.cpio`，配 `INITRAMFS=` 嵌入内核 |
| `clean` | 删整个 `build/` |

| 变量 | 默认 | 作用 |
|---|---|---|
| `KTEST=1` | `0` | 编入 `ktest/` 自测；翻转会触发全量重编，防两模式对象混装 |
| `DISK=0` | `1` | 不挂盘，快速看早期启动日志 |
| `MACHINE` / `KVM` | `q35` / 自动探测 | QEMU 机型；`/dev/kvm` 可用即加 `--enable-kvm` |
| `INITRAMFS` | 空 | cpio 路径，编进内核 |
| `SMOKE_FLAGS` | 空 | 原样传给 `scripts/smoke_test.py`，如 `--smp 1` |

## 3. 构建流程与产物

### 3.1 流程

```
kernel/**.c, arch/*.c ──cc──► build/**/*.o ─┐
arch/*.S ──nasm──► build/arch/*.o           │
trampoline.S ─nasm─► bin ─objcopy─► .o      ├─ ld -T arch/kernel.ld --gc-sections ─► vmlinux.elf
ktest/*.c（仅 KTEST=1）──cc──► .o           │      └─ objcopy -O binary ─► vmlinux.bin ─┐
setup.asm ─nasm─► 补零 2 KiB 整数倍 ─patch_setup_jmp.py─► setup.bin ────────────────┴─ cat ─► patch_bzimage.py ─► bzImage

userland/bin/*.c + crt0.S + ulib.c ─► build/user/<名>（静态 ELF）
build/user/* + README.md ──mkfs.vfat/mcopy──► build/disk.img
```

- 源文件用 `find`/`wildcard` 自动发现：新增内核 `.c` 或 `userland/bin/*.c` 不用改 Makefile；头依赖由 `-MMD -MP` 的 `.d` 跟踪。
- 关键标志：`-ffreestanding -fno-stack-protector -mno-red-zone -mno-sse* -mcmodel=kernel -O2 -g -ffunction-sections -fno-pic`；用户态 `-nostdlib -static -no-pie -mcmodel=large`，链接在 `0x7f8000400000`。
- `vmlinux.elf` 查符号：`grep <sym> build/vmlinux.map` 或 `nm build/vmlinux.elf`。
- `trampoline.S` 走两段式（裸二进制 → ELF 对象，符号改名防被通配进 `.image`）；`setup.bin` 依赖 `vmlinux.elf` 只为 `nm` 查 `_start32/64` 回填跳转；`patch_bzimage.py` 填 boot protocol 头并断言 `HdrS`/`0x55AA`/对齐。
- 可复现：设 `SOURCE_DATE_EPOCH` 后 `bzImage` SHA256 稳定。
- `make -j$(nproc)` 安全；改了 `_start32/64` 相对位置而启动跳错：`rm -f build/setup.bin build/bzImage && make`。

### 3.2 产物

| 文件 | 说明 |
|---|---|
| `build/bzImage` | 最终可引导镜像（大小随 KTEST 变） |
| `build/vmlinux.elf` / `.bin` / `.map` | 符号+调试信息 / 裸 payload / 链接映射 |
| `build/setup.bin` | 真模式 setup（已补零、回填跳转） |
| `build/disk.img` | 64 MiB FAT32 整盘（无分区表） |
| `build/user/<名>` | 静态用户态 ELF |
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
- `-m 256` 够用；要覆盖 2 GiB 管理窗口（`PMM_WINDOW_TOP`）的高地址路径需 `-m 2048`。
- 无头环境加 `-display none`；AHCI 测试改 `if=ahci,id=ahci0`（已知问题见 GAP_ANALYSIS）。

### 4.2 `make smoke`

六阶段全过才退出码 0：

| 阶段 | 做什么 |
|---|---|
| 1 toolchain | 必需工具在 PATH（缺 `fsck.vfat`/`mdir` 记 SKIP） |
| 2 build | `make -j all build/disk.img`（`--clean` 先删 `build/`） |
| 3 artifacts | bzImage 头静态检查、`vmlinux.elf` 符号与入口、`disk.img` 与 `fsck.vfat`、`/bin` 内容、用户 ELF 形态 |
| 4 boot | 无头启动按序等串口标记：e820 → 内存 → CR0/CR4 → FAT32 挂载 → `[ktest]` 各阶段 0 failed → `selftest PASS` → pid 1 → AP → `[boot] ready` → shell；反向查无 `[PANIC]`/FAIL |
| 5 shell | 串口驱动 `ls`/`cat`/`kill` + `systest`，匹配 `tag=value` 直到 `systest.done=1` |
| 6 forged FAT | 副本镜像伪造某文件的 FAT 链，必须报 `chain loop`/`corrupt FAT entry` 且 shell 仍返回 |

```sh
make smoke                          # 205/205 checks
make smoke SMOKE_FLAGS="--smp 1"    # 单 CPU 必须用这个，不用 --qemu-args
make smoke SMOKE_FLAGS="--no-build" # 复用现有 build/
```

- 单/双 CPU 标记集不同（双 CPU 要求 AP 行、2 CPUs online；单 CPU 反之），所以单 CPU 必须走 `--smp 1` 换标记集。
- 日志：`build/smoke/serial.log`（`--smp 1` → `serial-smp1.log`，伪造盘 → `serial-forged-*.log`）；阶段 6 用副本 `forged.img`，不碰 `disk.img`。
- 手动判据：`[boot] ready in N ms` + `[ktest] selftest PASS`，无 `[PANIC]`、无三重故障。

## 5. 磁盘镜像与 GRUB ISO

### 5.1 `disk.img`

配方（Makefile 内）：`dd` 64 MiB → `mkfs.vfat -F 32 -R 1024` → 拷 `README.md`、建 `::bin`、逐个 `mcopy` 用户 ELF。宿主机 mtools 造镜像，绕开"内核要能挂 FAT32 才能跑、镜像要有人造"的鸡生蛋问题。

`-R 1024` 保留 1024 扇区：内核把崩溃一致性日志放在保留区 `8..reserved-1`，mkfs 默认 32 扇区只够十几条记录。镜像是整盘 FAT32（无 MBR），内核先试整盘、失败才走分区。

```sh
mdir -i build/disk.img ::bin        # 列 /bin
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
| 光盘启动停在 `Stop!`，`cannot open /bin/init` | `disk.img` 损坏/过期（`imageiso` 不重建） | `rm build/disk.img && make build/disk.img`，`fsck.vfat -n` 复核 |
| `fsck.vfat` 报 `FATs differ` | `mcopy` 写一半被打断（多 QEMU 抢写） | 删掉重建；`make` 只看时间戳，别用它判断镜像健康 |
| `patch_setup_jmp.py` 找不到 `jmp 0x18:0xF00DFACE` | 哨兵跳转被改，或缺 `_start32/64` | `grep -n F00DFACE arch/setup.asm`；`nm build/vmlinux.elf \| grep _start` |
| `patch_bzimage.py` 断言失败 | `HdrS`/`0x55AA` 被破坏或 setup 补零逻辑被改 | `xxd -s 0x1f0 -l 32 build/bzImage` |
| `undefined reference to '__stack_chk_fail'` | 某文件没继承 `-fno-stack-protector` | 检查是否走 `KERNFLAGS`/`USERFLAGS` |
| 用户态 `undefined reference to '_start'` | 链接没带 `crt0.o` | 走根 Makefile 的 `$(USER_ELFS)` 规则 |
| 改 `_start32/64` 后跳错地址 | 符号变了但 `setup.bin` 没重 patch | `rm -f build/setup.bin build/bzImage && make` |
| 串口停在 `M` 无输出 | `console_init`/bootinfo 问题 | gdb 断 `start_kernel` 单步 |
| `[vfs] no disk found` | IDE 探测失败或镜像不是合法整盘 FAT32 | `rm build/disk.img` 重建 |
| GRUB 下 24 bpp 而非 32 | `gfxpayload` 写成逗号形式 | 写 `x32` 形式，见 5.2 |
| 无 framebuffer | GRUB 没设 `gfxpayload` | 内核回退 VGA；要 32 bpp 补 5.2 配置 |
| `imageiso` 报 `grub-mkrescue not found` | 缺工具 | 装 `grub-pc-bin` + `xorriso` |
| smoke 阶段 4 卡在 AP 标记 | 用 `--qemu-args "-smp 1"` 关了 CPU 但标记仍是双 CPU 集 | 用 `SMOKE_FLAGS="--smp 1"` 换标记集 |
| smoke 阶段 1 报缺 `g++` | 工具链检查仍列了它 | 装 `g++`（不影响构建） |
| gdb 与串口抢终端 | 都用 stdio | 调试时 `-serial null` |
| 真机闪一下就重启 | 三重故障（`panic` 只挂死不复位） | `-m 2048` 复现（`-m 256` 走不到窗口上界），配 `-no-reboot -d int` 按 6 节定位 |
