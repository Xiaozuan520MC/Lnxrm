#!/usr/bin/env python3
"""Pack a directory tree into a newc cpio archive -- the format the kernel's
initramfs driver (kernel/fs/initramfs.c) parses and serves as "/".

    usage: mkinitramfs.py <out.cpio> <srcdir>

Every file and directory under <srcdir> is recorded with a path relative to
<srcdir>, so a staging tree of

    build/ifsrc/bin/init        ->  bin/init
    build/ifsrc/README.md       ->  README.md

boots as /bin/init and /README.md.  Nothing outside the tree is read and the
output is byte-for-byte reproducible: mtimes are forced to 0, ownership to
root, and entries are emitted in sorted order.  A reproducible archive keeps
"the kernel changed" and "the initramfs changed" separable when diffing two
builds, which is the whole point of this being a build step and not a blob.

Format (Documentation/filesystems/ramfs-rootfs-initramfs.rst, "Initramfs
wire format"): each record is a 110-byte ASCII header -- '070701' followed by
thirteen fields of eight lower-case hex digits -- then the NUL-terminated
name, then the file body.  Header+name and body are each padded to a 4-byte
boundary *measured from the start of the archive*, not from the record, so
the padding is computed from the running offset and never from a field size.
The archive ends with a zero-length record named 'TRAILER!!!'.
"""

import os
import stat
import sys

MAGIC = b"070701"  # 'newc': 32-bit fields, no checksum ('070702' would be CRC)
HDR = 110  #6 bytes of magic + 13 fields of 8 digits


def header(ino, mode, uid, gid, nlink, mtime, filesize, namesize):
    """One newc record header.  Field order is fixed by the format; the
    unused ones (dev/rdev/check) are zero, which is what every writer does
    for an initramfs -- it has no device nodes and no checksum to verify."""
    fields = (ino, mode, uid, gid, nlink, mtime, filesize,
              0, 0, 0, 0, namesize, 0)
    return MAGIC + b"".join(b"%08x" % (v & 0xFFFFFFFF) for v in fields)


def main():
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__ + "\n")
        return 1

    out_path, src = sys.argv[1], sys.argv[2]
    if not os.path.isdir(src):
        sys.stderr.write("mkinitramfs: %s is not a directory\n" % src)
        return 1

    # Collect relative paths first so the archive order does not depend on
    # readdir() (which is not sorted on every filesystem).
    rels = []
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames.sort()
        filenames.sort()
        for d in dirnames:
            rels.append(os.path.relpath(os.path.join(dirpath, d), src))
        for f in filenames:
            rels.append(os.path.relpath(os.path.join(dirpath, f), src))
    rels.sort()

    buf = bytearray()
    nfile = ndir = nbytes = 0
    ino = 0

    def add(name, mode, data):
        nonlocal ino, nfile, ndir, nbytes
        ino += 1
        raw = name.encode("utf-8") + b"\0"
        # nlink: 2 for a directory ('.' and its parent), 1 for a file.  The
        # kernel ignores it; keeping it honest costs nothing.
        nlink = 2 if stat.S_ISDIR(mode) else 1
        buf.extend(header(ino, mode, 0, 0, nlink, 0, len(data), len(raw)))
        buf.extend(raw)
        while len(buf) % 4:
            buf.append(0)  # padding is measured from the archive start
        buf.extend(data)
        while len(buf) % 4:
            buf.append(0)
        if stat.S_ISDIR(mode):
            ndir += 1
        else:
            nfile += 1
            nbytes += len(data)

    for rel in rels:
        full = os.path.join(src, rel)
        st = os.lstat(full)
        name = rel.replace(os.sep, "/")
        if name.startswith(".."):
            sys.stderr.write("mkinitramfs: %s escapes the tree\n" % name)
            return 1
        if stat.S_ISDIR(st.st_mode):
            add(name, stat.S_IFDIR | (st.st_mode & 0o777), b"")
        elif stat.S_ISREG(st.st_mode):
            with open(full, "rb") as fh:
                add(name, stat.S_IFREG | (st.st_mode & 0o777), fh.read())
        else:
            # A symlink or device node has nothing to point at in a kernel
            # that has neither, and silently dropping it would look like the
            # file simply vanished.  Say so.
            sys.stderr.write("mkinitramfs: skipping %s (not a regular file)\n" % name)

    add("TRAILER!!!", stat.S_IFREG | 0o644, b"")

    with open(out_path, "wb") as fh:
        fh.write(bytes(buf))

    print("  CPIO    %s (%d files, %d dirs, %d bytes of content, %d bytes total)"
          % (out_path, nfile, ndir, nbytes, len(buf)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
