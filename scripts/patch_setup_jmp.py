#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Patch setup.bin's final long-mode jmp target.

With S3.5 (gzip decompression), setup.asm no longer jumps directly to
_start64.  Instead it jumps to 0x100000, where the decompression stub
(decompress_head.S, linked at 0x100000) lives.  The stub inflates the
gzip-compressed kernel back to 0x100000 and then jumps to _start64's
physical entry (kernel_entry_phys), so start_kernel() runs exactly as
before.

Usage: patch_setup_jmp.py <setup.bin>

Idempotent: if the instruction already targets 0x100000, it's a no-op.
"""
import struct
import sys

SENTINEL = 0xF00DFACE
TARGET = 0x100000


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: %s <setup.bin>\n" % sys.argv[0])
        return 1

    setup_bin = sys.argv[1]

    with open(setup_bin, "rb") as f:
        data = bytearray(f.read())

    # Find the `jmp dword 0x18:imm32` instruction (EA imm32 1800).
    idx = None
    for i in range(len(data) - 7):
        if data[i] != 0xEA:
            continue
        sel = struct.unpack_from("<H", data, i + 5)[0]
        if sel != 0x0018:
            continue
        imm = struct.unpack_from("<I", data, i + 1)[0]
        if imm == SENTINEL or imm == TARGET:
            idx = i
            break

    if idx is None:
        raise SystemExit("could not find `jmp 0x18:{0x%08X}` in %s"
                         % (SENTINEL, setup_bin))

    imm = struct.unpack_from("<I", data, idx + 1)[0]
    if imm == TARGET:
        return 0

    struct.pack_into("<I", data, idx + 1, TARGET)
    with open(setup_bin, "wb") as f:
        f.write(bytes(data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
