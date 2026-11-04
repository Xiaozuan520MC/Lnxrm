# lnxrm

自制 x86-64 宏内核，使用 C/ASM 两语言构建

## 构建

**依赖**：gcc、nasm、binutils、python3、qemu-system-x86_64；

ISO 额外要 grub-mkrescue + grub-pc-bin；镜像要 dosfstools（mkfs.vfat）与 mtools。

## 产物

| 文件 | 说明 |
|------|------|
| `build/bzImage` | 内核镜像 |
| `build/disk.img` | FAT32 磁盘 |
| `build/lnxrm.iso` | GRUB ISO |

## 文档

| 文件 | 说明 |
|------|------|
| `docs/BUILD.md` | 构建、运行、调试、排障 |
| `docs/CODEBASE.md` | 代码结构与各子系统契约 |
| `docs/GAP_ANALYSIS.md` | 当前开放缺陷、能力差距、修复优先级 |
