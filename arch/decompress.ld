/* SPDX-License-Identifier: Apache-2.0
 *
 * Linker script for the gzip decompression stub.
 *
 * Linked at physical 0x100000.  Entered from arch/setup.asm after the
 * long-mode switch.  Contains both the stub entry (decompress_head.S)
 * and the inflate implementation (kernel/lib/inflate.c).  After
 * inflating the kernel back to 0x100000, control jumps there and
 * _start32/_start64/start_kernel() run exactly as in the uncompressed
 * build.
 *
 * bzImage payload layout (loaded at 0x100000 by setup.asm):
 *   [0x100000,             0x100000+decompress_size)   decompress stub
 *   [0x100000+decompress_size, ... )                   gzip-compressed kernel
 */

ENTRY(decompress_entry)

DECOMPRESS_BASE = 0x100000;

SECTIONS
{
    . = DECOMPRESS_BASE;

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    /* _decompress_end 必须对齐到 512 字节，与 decompress.bin 的
     * objcopy 填充一致，这样 stub 从 _decompress_end 读取的 gzip
     * 数据才与 bzImage 中紧随其后的 vmlinux.bin.gz 对齐。 */
    _decompress_end = ALIGN(512);
    decompress_size = _decompress_end - DECOMPRESS_BASE;

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
    }

    _inflate_end = .;
}
