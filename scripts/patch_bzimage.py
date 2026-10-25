#!/usr/bin/env python3
"""Patch the bzImage setup header after concatenation.

Linux x86 boot protocol (Documentation/arch/x86/boot.rst) fields we fill:

  offset 0x1F1 (byte) : setup_sects
        Size of the setup code in 512-byte sectors, **minus one**.
        A value of 0 is special and means 4 sectors (2 KiB).

  offset 0x1F4 (dword): syssize
        Size of the protected-mode payload in 16-byte paragraphs.

The payload must start at a 512-byte boundary.  We require at least 4
setup sectors (2 KiB) so that any loader (SeaBIOS, GRUB, syslinux, ...)
computes the same payload offset we do.

Usage:
    patch_bzimage.py <bzImage> <payload_off>

    <payload_off> is the byte offset in <bzImage> where the payload
    begins.  It must equal the padded size of setup.bin and be a
    multiple of 512, >= 2048.
"""

import struct
import sys


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: %s <bzImage> <payload_off>\n" % sys.argv[0])
        return 1

    path = sys.argv[1]
    payload_off = int(sys.argv[2], 0)

    with open(path, "rb") as f:
        data = bytearray(f.read())

    # Sanity: the setup header magic must be present at 0x202.
    assert data[0x202:0x206] == b"HdrS", "bad header magic at 0x202"

    # The boot flag must be 0xAA55 at 0x1FE.
    assert data[0x1FE:0x200] == b"\x55\xaa", "bad boot flag at 0x1FE"

    total = len(data)

    # ---- validate payload_off -------------------------------------------
    assert payload_off % 512 == 0, \
        "payload_off (0x%x) must be a multiple of 512" % payload_off
    assert payload_off >= 4 * 512, \
        "setup must be at least 4 sectors (2 KiB), got %d bytes" % payload_off
    assert payload_off < total, \
        "payload_off (0x%x) must be less than file size (0x%x)" % (payload_off, total)

    # ---- setup_sects: sectors - 1 ---------------------------------------
    setup_sects = payload_off // 512          # actual setup sectors (>= 4)
    data[0x1F1] = (setup_sects - 1) & 0xFF

    # ---- syssize: payload size in 16-byte paragraphs --------------------
    payload = total - payload_off
    assert payload > 0, "empty payload"
    syssize = (payload + 15) // 16
    struct.pack_into("<I", data, 0x1F4, syssize)

    # ---- code32_start (0x214): 32-bit protected-mode entry --------------
    # We leave this alone; setup.asm already sets it to 0x100000.
    # Uncomment and set if you move the payload:
    # struct.pack_into("<I", data, 0x214, 0x100000)

    # ---- init_size (0x260): memory the loader must keep for the kernel ---
    # setup.asm hardcodes 4 MiB, which is enough for the kernel alone and too
    # small once `make INITRAMFS=` has linked a cpio archive into the image:
    # the payload is uncompressed, so the memory the image occupies is the
    # payload, and a loader that trusts this field (GRUB does -- it sizes the
    # window it reserves and may load the kernel into from it) would cut the
    # image off mid-way.  Take the larger of setup.asm's floor and the
    # payload rounded up to 2 MiB, plus 2 MiB of headroom above that for the
    # .bss, the boot stack and whatever the kernel allocates before the
    # buddy is up.
    need = max(0x400000, ((payload + 0x1FFFFF) & ~0x1FFFFF) + 0x200000)
    struct.pack_into("<I", data, 0x260, need)

    with open(path, "wb") as f:
        f.write(bytes(data))

    '''print("  PATCH   %s" % path)
    print("          setup_sects = %d (header byte 0x%02x)"
          % (setup_sects, setup_sects - 1))
    print("          syssize     = %d paragraphs (%d payload bytes)"
          % (syssize, payload))
    print("          init_size   = %d bytes (0x%x)" % (need, need))'''
    return 0


if __name__ == "__main__":
    sys.exit(main())
