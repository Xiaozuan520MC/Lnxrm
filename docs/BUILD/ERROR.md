# lnxrm 构建指南（附录：调试与排障）

本文是 `BUILD.md` 的第 6–7 节拆出：调试手段（串口、debugcon、GDB、`-d int`、monitor）与常见故障排障表。第 1–5 节（依赖、make 目标、构建流程与产物、QEMU 运行、磁盘镜像与 GRUB ISO）见 `BUILD.md`。代码结构见 `../CODEBASE.md`（§1–§10 分布于 `CODEBASE/CODEBASE_*.md`），缺陷与计划见 `../GAP_ANALYSIS.md`（已收口条目在 `../GAP_ANALYSIS_DONE.md`）。

---

## 6. 调试

- **串口**（最常用）：所有 `kprintf` 走这里，`-serial stdio` 直看 / `file:` 存档 / `null` 让给 gdb。
- **debugcon 早期标记**（最快的分诊）：`arch/entry64.S` 向端口 `0xE9` 打单字符，`-debugcon stdio` 才可见——`K`(进 `_start64`) `b`(段寄存器) `c`(主页表) `d`(.activate) `f`(CR4.PAE) `e`(load_cr3) `M`(bss 清完、进 C)。`KbcdfeM` 全有 = 已到 C 世界门口；串口要 `console_init()` 之后才有。
- **解压 stub 串口标记**：`arch/decompress_head.S` 在 COM1 直接打 `D`(进 stub) `1`(inflate blob 搬到 `0x200000`) `2`(gzip 搬到 `0x300000`) `I`(即将跳 `inflate_main`)，`-serial stdio` 即可见。`D12I` 之后无输出 = 解压失败或入口跳转失败，按第 7 节对应行排查。
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
| `ps` 里没有 `/mod/*.km`、串口也没有模块行 | **`/mod` 里的文件名不以 `.km` 结尾**（`MOD_ELFS` 的输出名或 debugfs 脚本的目标名被改错）：init 的筛子只认 `.km`，两个模块全被静默 `continue`——镜像、串口、`ps` 三处**各自看起来都正常**（2026-11-18 实测的成因：后缀原先指望安装步骤补，而它没补）。**成功路径现在一行不打印**，所以「有没有起来」只能看 `ps` | `debugfs -R "ls /mod" build/disk.img` 看后缀；`rm build/disk.img && make build/disk.img`（`disk.img` 现在把 `Makefile` 当依赖，改这两条规则会自动重建）；起不来时串口会打 `init: N file(s) in /mod, 0 spawned -- no module was started` |
| 链接报 `undefined symbol: trampoline_start` | **`BUILD=` 被覆盖到别处**：`Makefile` 的 `--redefine-sym _binary_build_trampoline_bin_*` 写死了默认路径，而 `llvm-objcopy -I binary` 的符号名由输入路径导出 | 用默认 `BUILD=build`（唯一受支持的用法）；或手工按实际路径重新 `--redefine-sym` 生成 `$(OBJDIR)/trampoline.o` |
| 光盘启动停在 `Stop!`，`cannot open /bin/init` | `disk.img` 损坏/过期（`imageiso` 不重建） | `rm build/disk.img && make build/disk.img`，`e2fsck -n` 复核 |
| `e2fsck -n` 报错（计数对不上、目录记录越界等） | 上一轮 QEMU 被杀在写盘中途——**QEMU 直接写 `build/disk.img`，没有快照**；或驱动留了不一致的元数据 | `rm build/disk.img && make build/disk.img` 重建；若是稳定复现，那是驱动的 bug 不是镜像的 |
| `patch_setup_jmp.py` 找不到 `jmp 0x18:0xF00DFACE` | `setup.asm` 里的哨兵被改或删了（脚本已不查内核符号） | `grep -n F00DFACE arch/setup.asm` |
| `patch_bzimage.py` 断言失败 | `HdrS`/`0x55AA` 被破坏或 setup 补零逻辑被改 | `xxd -s 0x1f0 -l 32 build/bzImage` |
| `undefined reference to '__stack_chk_fail'` | 某文件没继承 `-fno-stack-protector` | 检查是否走 `KERNFLAGS`/`USERFLAGS` |
| 用户态 `undefined reference to '_start'` | 链接没带 `crt0.o` | 走根 Makefile 的 `$(USER_ELFS)` 规则 |
| 改 `_start32/64` 后跳错地址 | `kernel_entry.S` 没重生成（`_start64` 物理入口烘焙在解压 stub 里） | `rm -f build/kernel_entry.S build/decompress.elf build/bzImage && make`；正常会随 `vmlinux.elf` 依赖自动重算 |
| 串口停在 `D12I` | `inflate()` 返回负数，或 `_start64` 入口地址不对 | 看是否有错误码；核对 `cat build/compressed_size.S build/kernel_entry.S`；也可在宿主机直接编译 `kernel/lib/inflate.c` 单测解压结果 |
| 串口停在 `M` 无输出 | `console_init`/bootinfo 问题 | gdb 断 `start_kernel` 单步 |
| `[vfs] no disk found` | AHCI 没探到盘，或镜像既不是合法整盘 ext2、也不是可挂的 FAT32/分区（**不是 IDE 的事**：`ide.c` 已删，只剩 AHCI 一条栈） | `rm build/disk.img` 重建；确认没用 `MACHINE=pc`/`-machine pc`（i440fx 无 AHCI） |
| GRUB 下 24 bpp 而非 32 | `gfxpayload` 写成逗号形式 | 写 `x32` 形式，见 5.2 |
| 无 framebuffer | GRUB 没设 `gfxpayload` | 内核回退 VGA；要 32 bpp 补 5.2 配置 |
| `imageiso` 报 `grub-mkrescue not found` | 缺工具 | 装 `grub-pc-bin` + `xorriso` |
| smoke 阶段 4 卡在 AP 标记 | 用 `--qemu-args "-smp 1"` 关了 CPU 但标记仍是双 CPU 集 | 用 `SMOKE_FLAGS="--smp 1"` 换标记集 |
| smoke 在 enospc 填盘段撞等待超时（阶段 4 等 `late selftest` 超时、阶段 6 forged 无 prompt），串口停在 `[t-enspc] wrote=3819 hit=1 bad=0` 之后 | `test_enospc_on_full_volume` 静默填满整个根卷（S2 起 ext2，15295 块）再 unlink 释放；**2026-11-24 实证真因是 ext2 块层按 512 B 逐扇区发 `blk_io_*`，而 `ahciBlkWrite` 每次写请求收尾发一次 `0xE7` FLUSH**——两个循环合计约 50 万次 flush 直接烧满 420 s（被杀在 free 的 64% 处，镜像留下孤儿 `ENOSPC.T` + 位图半释放），已改整块单请求（账见 `kernel/fs/ext2/block/block.c` 的 `ext2_read_data_block` 注释；2026-11-24 修后实测 **270/270**）；主机高负载时 TCG 仍可能慢（FAT32 时代 62 MiB 实测 load≈6.7 需 504 s） | 有残留先 `rm build/disk.img && make build/disk.img`（被杀轮的镜像是脏的，`e2fsck -n` 必红、不重建必挂），`e2fsck -n` 应报「没有问题」；仍慢则主机降载或 `SMOKE_FLAGS="--timeout 900"`（判据不变、只放宽等待） |
| smoke 复跑挂死改点（停在 `[ahci] port 2: device present`、QEMU 被 SIGTERM、`serial.log` 只有 82 行无 mount 标记） | `serial.log` 与 `serial-fail-1.log` 内容不同：前者是本轮的、`serial-fail-1.log` 是上一轮旧记录 | 看 `serial.log`（本轮）；若抓现场，用 `-monitor tcp:127.0.0.1:4444,server,nowait` 起同一 QEMU，停住后 `info cpus` / `info registers` 抓 RIP（QEMU monitor 的 `x/16i $rip` 在此版本不认 `$rip`，改用 `x/16i <RIP 数值>`） |
| smoke 阶段 1 报缺 `g++` | 工具链检查仍列了它 | 装 `g++`（不影响构建） |
| gdb 与串口抢终端 | 都用 stdio | 调试时 `-serial null` |
| 真机闪一下就重启 | 三重故障（`panic` 只挂死不复位） | `-m 2048` 复现（`-m 256` 走不到窗口上界），配 `-no-reboot -d int` 按 6 节定位 |
