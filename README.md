# lnxrm 项目概览

> 自制 x86-64 混合内核，使用 C和ASM 两种语言打造

<img src="image/bitcat.svg" width="120" align="right" alt="bitcat 吉祥物" />

## 构建依赖

| 类别 | 依赖 |
|------|------|
| 工具链 | clang、nasm、lld、llvm、python3、qemu-system-x86_64 |
| 镜像 | grub-mkrescue + grub-pc-bin |
| 磁盘 | mkfs.vfat + mtools |

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
