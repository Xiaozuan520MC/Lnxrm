# lnxrm

自制 x86-64 单体内核：裸机到带帧缓冲控制台、FAT32 文件系统、多核与 33 个系统调用的
Unix 风格小系统。C/ASM 两语言，无第三方库，全程零警告构建。

## 构建与运行

依赖：gcc、nasm、binutils、python3、qemu-system-x86_64；
ISO 额外要 grub-mkrescue；镜像要 dosfstools（`mkfs.vfat`）与 mtools。

| 命令 | 做什么 |
|---|---|
| `make` | 构建 `build/bzImage`（`KTEST=1 make` 为带自测的变体） |
| `make run` | 构建用户态与磁盘镜像，QEMU 启动到 shell |
| `make imageiso` | 产出 GRUB ISO `build/lnxrm.iso`（真机/BIOS 走这条） |
| `make clean` | 清理 `build/` |

QEMU 参数、GRUB gfxpayload 规则与排障见 `docs/BUILD.md`。

## 产物

| 文件 | 说明 |
|---|---|
| `build/bzImage` | 内核镜像 |
| `build/disk.img` | FAT32 磁盘 /bin/ |
| `build/lnxrm.iso` | GRUB ISO |
| `build/smoke/serial.log` | 冒烟运行的完整串口输出 |

## 文档

| 文件 | 内容 |
|---|---|
| `docs/BUILD.md` | 构建、运行、调试、排障 |
| `docs/CODEBASE.md` | 代码结构与各子系统契约 |
| `docs/GAP_ANALYSIS.md` | 当前开放缺陷、能力差距、修复优先级 |

