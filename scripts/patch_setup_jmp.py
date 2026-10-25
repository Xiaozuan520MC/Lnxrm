#!/usr/bin/env python3
"""Patch setup.bin's final long-mode jmp target to _start64's physical address.

Idempotent: if the instruction already targets _start64_phys, it's a no-op.

Usage: patch_setup_jmp.py <setup.bin> <vmlinux.elf>
"""
import struct
import subprocess
import sys

SENTINEL = 0xF00DFACE

def get_symbol(elf, name):
    out = subprocess.check_output(["nm", elf]).decode()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] == name:
            return int(parts[0], 16)
    raise KeyError("symbol %s not found in %s" % (name, elf))

def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: %s <setup.bin> <vmlinux.elf>\n" % sys.argv[0])
        return 1

    setup_bin = sys.argv[1]
    elf       = sys.argv[2]

    start32 = get_symbol(elf, "_start32")
    start64 = get_symbol(elf, "_start64")
    target  = 0x100000 + (start64 - start32)

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
        if imm == SENTINEL or imm == target:
            idx = i
            break

    if idx is None:
        raise SystemExit("could not find `jmp 0x18:{0x%08X,0x%08X}` in %s"
                         % (SENTINEL, target, setup_bin))

    imm = struct.unpack_from("<I", data, idx + 1)[0]
    if imm == target:
        '''print("  PATCH   jmp 0x18:0x%08X already correct (offset 0x%x)"
              % (target, idx))'''
        return 0

    struct.pack_into("<I", data, idx + 1, target)
    with open(setup_bin, "wb") as f:
        f.write(bytes(data))

    '''print("  PATCH   jmp 0x18:0x%08X -> 0x18:0x%08X (offset 0x%x)"
          % (SENTINEL, target, idx))'''
    return 0

if __name__ == "__main__":
    sys.exit(main())
