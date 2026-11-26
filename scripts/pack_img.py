#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pack build/disk.img: a 64 MiB ext2 root image populated from build artifacts.

Replaces the inline shell recipe in the root Makefile.  It does three things:

  1. Create a 64 MiB blank file and mke2fs it as ext2 (4 KiB blocks, no
     journal, 16384 inodes of 256 B, one group) -- the geometry the ext2
     driver and the smoke test are written against.
  2. Populate it with debugfs: /README.md, /bin/<programs>, /lib/<ld.so +
     libulib.so>, /mod/<*.km>.
  3. Verify every expected path resolves to an inode (debugfs exits 0 even
     when a `write` silently fails, so the exit status alone is not enough).

Usage:
    python3 scripts/pack_img.py <build_dir> <readme> <output.img> \
        --user-elfs <file>... --libs <file>... --mods <file>...

The build_dir is only used to locate mke2fs/debugfs by name (they must be on
PATH) and to write the throwaway debugfs command script into.
"""

import os
import re
import sys
import subprocess


def die(msg, code=1):
    sys.stderr.write("pack_img.py: error: %s\n" % msg)
    sys.exit(code)


def run(cmd, **kw):
    """Run a command, printing its combined output on failure."""
    proc = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        die("command failed: %s" % " ".join(cmd), proc.returncode)
    return proc


def main(argv):
    if len(argv) < 5:
        die("usage: pack_img.py <build_dir> <readme> <output.img> "
            "--user-elfs <file>... --libs <file>... --mods <file>...")

    build_dir, readme, output = argv[1], argv[2], argv[3]

    # Parse the three optional --* lists from the remaining arguments.
    user_elfs, libs, mods = [], [], []
    cur = None
    for tok in argv[4:]:
        if tok == "--user-elfs":
            cur = user_elfs
        elif tok == "--libs":
            cur = libs
        elif tok == "--mods":
            cur = mods
        else:
            if cur is None:
                die("unexpected argument: %s" % tok)
            cur.append(tok)

    if not os.path.isfile(readme):
        die("readme not found: %s" % readme)
    for f in user_elfs + libs + mods:
        if not os.path.isfile(f):
            die("source file not found: %s" % f)

    os.makedirs(build_dir, exist_ok=True)

    # ---- 1. blank 64 MiB image ---------------------------------------
    with open(output, "wb") as f:
        f.truncate(64 * 1024 * 1024)

    # ---- 2. mke2fs: plain ext2, geometry pinned ----------------------
    run([
        "mke2fs", "-q", "-t", "ext2", "-F",
        "-b", "4096", "-I", "256", "-N", "16384",
        "-O", "^has_journal", output,
    ])

    # ---- 3. debugfs command script -----------------------------------
    cmds = []
    cmds.append("write %s /README.md" % readme)
    cmds.append("mkdir /bin")
    for f in user_elfs:
        cmds.append("write %s /bin/%s" % (f, os.path.basename(f)))
    cmds.append("mkdir /lib")
    for f in libs:
        cmds.append("write %s /lib/%s" % (f, os.path.basename(f)))
    cmds.append("mkdir /mod")
    for f in mods:
        cmds.append("write %s /mod/%s" % (f, os.path.basename(f)))

    cmds_path = os.path.join(build_dir, "disk.cmds")
    with open(cmds_path, "w") as f:
        f.write("\n".join(cmds) + "\n")

    run(["debugfs", "-w", "-f", cmds_path, output])

    # ---- 4. verify every expected path has an inode ------------------
    expected = ["/README.md", "/bin", "/lib", "/mod"]
    expected += ["/bin/%s" % os.path.basename(f) for f in user_elfs]
    expected += ["/lib/%s" % os.path.basename(f) for f in libs]
    expected += ["/mod/%s" % os.path.basename(f) for f in mods]

    missing = []
    for p in expected:
        out = subprocess.run(
            ["debugfs", "-R", "stat %s" % p, output],
            capture_output=True, text=True,
        ).stdout
        if not re.search(r"^Inode:", out, re.M):
            missing.append(p)
    if missing:
        die("disk.img is missing:%s (see %s/disk.log)"
            % (" " + " ".join(missing), build_dir))

    # ---- 5. /README.md must be >= 20 bytes, mode has 0400 and 0222 ---
    out = subprocess.run(
        ["debugfs", "-R", "stat /README.md", output],
        capture_output=True, text=True,
    ).stdout
    size = None
    mode = None
    for line in out.splitlines():
        if size is None:
            m = re.search(r"Size:\s*(\d+)", line)
            if m:
                size = int(m.group(1))
        if mode is None:
            m = re.search(r"Mode:\s*(\w+)", line)
            if m:
                mode = m.group(1)
        if size is not None and mode is not None:
            break
    if size is None or size < 20:
        die("/README.md on %s is too small (size=%s), want >= 20 bytes"
            % (output, size))
    # debugfs prints Mode as e.g. "0400|0222" or a single octal; parse both.
    mode_bits = 0
    if mode:
        for part in re.findall(r"\d+", mode):
            mode_bits |= int(part, 8)
    if (mode_bits & 0o400) == 0 or (mode_bits & 0o222) == 0:
        die("/README.md on %s has unusable mode %s, want 0400|0222"
            % (output, mode))

    print("  DISK    %s" % output)


if __name__ == "__main__":
    main(sys.argv)
