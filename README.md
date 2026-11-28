<p align="center">
  <img src="image/bitcat.svg" width="160" alt="bitcat 吉祥物" style="border-radius: 50%; background-color: white; padding: 15px; box-shadow: 0 4px 15px rgba(0,0,0,0.3);" />
</p>

[![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)](#) [![Language](https://img.shields.io/badge/language-C%20%7C%20Assembly-blue.svg)](#) [![Arch](https://img.shields.io/badge/arch-x86__64-orange.svg)](#) [![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

# lnxrm 项目概览 
> 自制 x86-64 混合内核，使用 C和ASM 两种语言打造

## 构建依赖

| 类别 | 依赖 |
|------|------|
| `工具链` | clang、nasm、lld、llvm、python3、qemu-system-x86_64 |
| `镜像` | grub-mkrescue + grub-pc-bin |
| `磁盘` | e2fsprogs（mke2fs / debugfs） |

## 产物

| 文件 | 说明 |
|------|------|
| `build/bzImage` | 内核镜像（setup + 解压 stub + gzip 内核） |
| `build/disk.img` | ext2 系统盘 |
| `build/lnxrm.iso` | GRUB ISO |

## 文档

| 文件 | 说明 |
|------|------|
| `docs/BUILD.md` | 构建、运行、调试、排障 |
| `docs/CODEBASE.md` | 代码结构与各子系统契约 |
| `docs/GAP_ANALYSIS.md` | 当前开放缺陷、能力差距、修复优先级 |

## License

本项目采用 **Apache 2.0** 协议开源。
详情请查阅根目录下的 [LICENSE](LICENSE) 文件。

