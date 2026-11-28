/* SPDX-License-Identifier: Apache-2.0
 *
 * Linker script for the inflate implementation (kernel/lib/inflate.c).
 * Linked at physical 0x200000 so the decompression stub can copy this
 * blob here and run it from a location that the in-place decompression
 * (output -> 0x100000) never clobbers.
 */

ENTRY(inflate_main)

INFLATE_BASE = 0x200000;

SECTIONS
{
    . = INFLATE_BASE;

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
    }

    _inflate_end = .;
}
