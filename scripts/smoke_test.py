#!/usr/bin/env python3
"""lnxrm end-to-end smoke test: build -> artifacts -> QEMU boot -> shell.

Six phases, each one must pass:

    1. toolchain   required host tools are on PATH
    2. build       make all + disk image (skipped with --no-build)
    3. artifacts   bzImage boot header, vmlinux symbols, FAT32 image, user ELFs
    4. boot        headless QEMU, ordered markers on the serial console
    5. shell       drive /bin/sh over serial: ls, cat, systest
    6. forged FAT  boot a copy of the image whose /CYC chain is forged and
                   require the driver to report it and keep running

Usage:
    scripts/smoke_test.py [--no-build] [--clean] [--timeout SECS] [--jobs N]
                           [--qemu-args "-extra qemu flags"]

Exit status is 0 only when every check passed.  The full serial transcript
is kept in build/smoke/serial.log.
"""

import argparse
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
SMOKE_DIR = os.path.join(BUILD, "smoke")
SERIAL_LOG = os.path.join(SMOKE_DIR, "serial.log")

BZIMAGE = os.path.join(BUILD, "bzImage")
VMLINUX = os.path.join(BUILD, "vmlinux.elf")
DISK = os.path.join(BUILD, "disk.img")

USER_LINK_BASE = 0x7F8000400000
README_LINE = b"A tiny unix-like kernel in ASM + C + C++ + Rust!"

BUILD_TIMEOUT = 600

REQUIRED_TOOLS = [
    "make", "gcc", "g++", "ld", "objcopy", "nm", "nasm", "rustc",
    "python3", "dd", "mkfs.vfat", "mcopy", "mmd", "qemu-system-x86_64",
]
OPTIONAL_TOOLS = ["fsck.vfat", "mdir"]

BOOT_MARKERS = [
    ("kernel banner", br"lnxrm v[0-9][^\s]* -- x86-64"),
    ("e820 map", br"\[boot\] \d+ usable e820 entries"),
    ("early selftest", br"\[ktest\] stage early: [1-9]\d* cases, [1-9]\d* checks, 0 failed"),
    ("pmm", br"\[pmm\] frames [0-9a-f]+\.\.#[0-9a-f]+"),
    ("vmm", br"\[vmm\] CR3=0x[0-9a-f]+"),
    ("NXE enabled", br"\[vmm\] EFER\.NXE=1"),
    ("CR0.WP + SMEP + SMAP enabled", br"\[cpu\] CR0\.WP=1 CR4\.SMEP=1 CR4\.SMAP=1"),
    ("mm selftest", br"\[ktest\] stage mm: [1-9]\d* cases, [1-9]\d* checks, 0 failed"),
    # C53/C54: the capacity the driver publishes must be the image's real
    # size.  IDENTIFY words 60-61 used to be taken verbatim, so a device
    # reporting ATA's "use 48-bit" marker (FFFFFFFFh) registered
    # num_sectors = 0xFFFFFFFF and this line read "2097151 MB" on hardware.
    # 64 MiB == 131072 sectors, and 131072 / 2048 == 64 exactly.
    # BOOT_MARKERS are matched in log order against a cursor that only moves
    # forward, so this has to sit before "fat32 mounted": the mount attempt is
    # printed before the mount succeeds.
    ("disk capacity is the real 64 MiB image",
     br"\[vfs\] attempting disk mount on \w+ \(131072 sectors, 64 MiB\)"),
    ("fat32 mounted", br"\[vfs\] FAT32 mounted at /"),
    # T-005: the late self-test arms a failing block device; the FAT32 layer
    # must say so out loud rather than pass the garbage through.
    ("t-005 injected I/O error reported", br"\[fat32\] I/O error"),
    ("late selftest", br"\[ktest\] stage late: [1-9]\d* cases, [1-9]\d* checks, 0 failed"),
    ("selftest verdict", br"\[ktest\] selftest PASS \([1-9]\d* cases, [1-9]\d* checks\)"),
    # exactly one warning is expected: the deliberate one t_lib.c fires to
    # prove WARN() is counted but not fatal.  The C46 abs_path WARN used to
    # be the second one and is gone now that overlong paths are refused.
    ("selftest tally", br"\[selftest\] [1-9]\d*/[1-9]\d* pass \[1 warn\]"),
    ("pid 1 spawned", br"\[task\] spawned pid=1 \(/bin/init\)"),
    # the AP half: each AP must report the same CR0/CR4 as the BSP, or the
    # cpu_protect_init() call in ap_main silently rotted away.
    ("CR0.WP + SMEP + SMAP on APs", br"\[cpu\] AP\d+ CR0\.WP=1 CR4\.SMEP=1 CR4\.SMAP=1"),
    ("2 cpus online", br"\[smp\] 2 CPUs online"),
    ("boot ready", br"\[boot\] ready in \d+ ms"),
    ("init read /README.md", re.escape(README_LINE)),
    ("shell prompt", br"# "),
]

# The block cache reports what it measured (T-007/T-008).  These are checked
# against the whole transcript instead of waited on in order: their relative
# order is the order of the `.ktest` section, which is the compiler's business
# (today t_cache.c's cases come out in reverse source order), while the wait
# list above is about "did the boot get this far".
LATE_REPORTS = [
    ("t-007 lru evicts the least recently used", br"\[blk_cache\] T-007 lru:"),
    ("t-007 200-sector file written back once each", br"\[blk_cache\] T-007 200-sector file:"),
    ("t-007 hot set written back once", br"\[blk_cache\] T-007 hot set:"),
    ("t-008 write then read is coherent", br"\[fat32\] T-008 coherent:"),
    ("t-008 FAT region stays out of the cache", br"\[fat32\] T-008 FAT region:"),
]

SYSTEST_TAGS = [
    ("uname.ret", br"uname\.ret=0"),
    ("unknown.99 = -ENOSYS", br"unknown\.99=-38"),
    ("past.32 = -ENOSYS", br"past\.32=[\s\S]{0,80}?-38"),
    ("past.33 = -ENOSYS", br"past\.33=[\s\S]{0,80}?-38"),
    ("open.fd >= 0", br"open\.fd=\d+"),
    ("read.after.seek = 15", br"read\.after\.seek=15"),
    ("read.at.end = tail only", br"read\.at\.end=[1-9]\d*"),
    ("ps.count > 0", br"ps\.count=[1-9]\d*"),
    # T-030: cred tiers visible through ps (idle=kxld=2, init=root=1)
    ("ps idle kind = kxld", br"ps\.kind\.idle=2"),
    ("ps init kind = root", br"ps\.kind\.init=1"),
    ("ps init uid = 0", br"ps\.uid\.init=0"),
    # T-031: kill gates -- same-uid probe passes, pid 1 is kxld-protected
    ("kill self probe = 0", br"kill\.self\.0=0"),
    ("kill init term refused", br"kill\.init\.term=-13"),
    ("kill init probe refused", br"kill\.init\.0=-13"),
    # T-031 control: the gate must let ordinary same-uid targets through,
    # or "pid 1 refused" just means kill never works at all.
    ("kill child term = 0", br"kill\.child\.term=0"),
    ("kill child reaped", br"kill\.child\.wait=[1-9]\d*"),
    ("kill child status = -SIGTERM", br"kill\.child\.status=-15"),
    # denial receipt: which layer said no (kxld rule vs uid/cap gate)
    ("kxld denial receipt", br"\[kxld\] denied kill: pid"),
    ("diskinfo.count > 0", br"diskinfo\.count=[1-9]\d*"),
    # T-031 (day 3): a uid-1000 child is refused the admin/framebuffer
    # gates it used to pass, root still is not, and ps sees the drop.
    ("setuid drop = 0", br"setuid\.drop=0"),
    ("setuid cannot climb back", br"setuid\.regain=-13"),
    ("setuid is idempotent", br"setuid\.again=0"),
    ("user ps refused", br"user\.ps=-13"),
    ("user diskinfo refused", br"user\.diskinfo=-13"),
    ("user fb_info still allowed", br"user\.fb\.info=0"),
    ("user fb_clear refused", br"user\.fb\.clear=-13"),
    ("user fb_fill refused", br"user\.fb\.fill=-13"),
    ("user fb_char refused", br"user\.fb\.char=-13"),
    ("user fb_puts refused", br"user\.fb\.puts=-13"),
    ("user kill root refused", br"user\.kill\.parent=-13"),
    ("user self probe allowed", br"user\.kill\.self=0"),
    ("user kill -1 refused", br"user\.kill\.all=-13"),
    ("user kill -1 probe refused", br"user\.kill\.all\.probe=-13"),
    ("root kill -1 probe allowed", br"root\.kill\.all\.probe=0"),
    ("user child reaped", br"user\.reap=1"),
    ("user child exit status", br"user\.exit=0"),
    ("ps sees the user tier", br"ps\.kind\.user=0"),
    ("ps sees uid 1000", br"ps\.uid\.user=1000"),
    ("root control: ps", br"root\.ps=1"),
    ("root control: diskinfo", br"root\.diskinfo=1"),
    ("root control: fb_info", br"root\.fb\.info=0"),
    ("root control: fb_clear", br"root\.fb\.clear=0"),
    # denial receipt: which capability said no (never a silent refusal)
    ("cap denial receipt", br"\[cap\] denied"),
    ("wait.code = 3", br"wait\.code=3"),
    ("segv.status = -SIGSEGV", br"segv\.status=-11"),
    ("nx.status = -SIGSEGV", br"nx\.status=-11"),
    ("read into .rodata = -EFAULT", br"read\.ro\.ret=-14"),
    # T-033: one surface, one owner.  The first drawer claims it, a second
    # privileged process is refused and takes nothing with it, and the
    # screen is free again once the holder has exited -- plus the receipt
    # that says which layer refused (never a silent "it just didn't draw")
    ("owner: first drawer claims it", br"fb\.owner\.grab=0"),
    ("owner: second drawer refused", br"fb\.owner\.denied=-13"),
    ("owner: the refusal took nothing", br"fb\.owner\.held=0"),
    ("owner: free once the holder left", br"fb\.owner\.released=0"),
    ("owner denial receipt names the holder", br"\[fb\] denied fb_fill:"),
    ("fb.info.sane = 1", br"fb\.info\.sane=1"),
    # T-004: brk runs the buddy dry, answers ENOMEM, gives every frame back
    # and the kernel is still running processes afterwards
    ("brk stops at ENOMEM", br"brk\.oom=-12"),
    ("brk really ran out", br"brk\.oom\.rounds=[1-9]\d*"),
    ("brk gave every frame back", br"brk\.shrink=1"),
    ("fork works after the OOM trip", br"brk\.after\.fork=[1-9]\d*"),
    ("reaped child after the OOM trip", br"brk\.after\.status=0"),
    # C46: an overlong path answers ENAMETOOLONG instead of being silently
    # truncated to a different path; the relative control proves the
    # normaliser still passes names that do fit
    ("relative path is normalised", br"path\.rel\.mkdir=0"),
    ("relative path reaches the fs", br"path\.rel\.open=\d+"),
    ("relative path cleaned up", br"path\.rel\.rmdir=0"),
    ("overlong open = -ENAMETOOLONG", br"path\.toolong\.open=-36"),
    ("overlong mkdir = -ENAMETOOLONG", br"path\.toolong\.mkdir=-36"),
    ("overlong unlink = -ENAMETOOLONG", br"path\.toolong\.unlink=-36"),
    ("overlong rename = -ENAMETOOLONG", br"path\.toolong\.rename=-36"),
    ("overlong rename (new) = -ENAMETOOLONG", br"path\.toolong\.rename2=-36"),
    # T-032: the account ps prints, the quota a plain user hits, and the
    # fact that neither is shared with anybody else
    ("ps reports image+stack, not nothing", br"mem\.kib\.base=[1-9]\d*"),
    ("peak remembers the T-004 drain", br"mem\.peak\.kib=[1-9]\d{5}"),
    ("8 MiB in = 8192 KiB of account", br"mem\.delta\.kib=8192"),
    ("shrink puts the account back", br"mem\.back=1"),
    ("peak outlives the shrink", br"mem\.peak\.holds=1"),
    ("user brk stops at ENOMEM", br"quota\.oom=-12"),
    ("user handed out 64 MiB", br"quota\.rounds=6\d"),
    ("user process survived", br"quota\.alive=1"),
    ("user may still shrink", br"quota\.shrink=1"),
    ("user stopped at the quota", br"quota\.mem\.kib=6[45]\d{3}"),
    ("user peak is the quota", br"quota\.mem\.peak=6[45]\d{3}"),
    ("parent's account untouched", br"mem\.parent\.unchanged=1"),
    ("child reaped cleanly", br"mem\.status=0"),
    ("recycled slot starts clean", br"mem\.slot\.clean=1"),
    ("systest.done = 1", br"systest\.done=1"),
]


class Report:
    def __init__(self):
        self.checks = []
        self.skipped = []

    def add(self, name, ok, detail=""):
        self.checks.append((name, bool(ok), detail))
        mark = "PASS" if ok else "FAIL"
        suffix = ("  <- " + detail) if detail and not ok else ""
        print("  %-4s  %s%s" % (mark, name, suffix), flush=True)
        return bool(ok)

    def skip(self, name, why):
        self.skipped.append((name, why))
        print("  SKIP  %s  (%s)" % (name, why), flush=True)

    @property
    def failed(self):
        return [c for c in self.checks if not c[1]]


class Phase:
    """Prints a header and remembers how many checks were already failing,
    so a phase can report its own verdict with .ok."""

    def __init__(self, report, title):
        self.report = report
        self.title = title
        self.baseline = 0

    def __enter__(self):
        print("\n== %s ==" % self.title, flush=True)
        self.baseline = len(self.report.failed)
        return self

    def __exit__(self, *exc):
        return False

    @property
    def ok(self):
        return len(self.report.failed) == self.baseline


def sh(cmd, timeout=600):
    """Run a command, return (rc, combined output as str)."""
    try:
        p = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
    except FileNotFoundError as e:
        return 127, str(e)
    except subprocess.TimeoutExpired:
        return 124, "timed out after %ss: %s" % (timeout, " ".join(cmd))
    return p.returncode, p.stdout.decode("utf-8", "replace")


# --------------------------------------------------------------------------
# phase 1: toolchain
# --------------------------------------------------------------------------
def phase_toolchain(report):
    with Phase(report, "1/6 toolchain") as ph:
        missing = [t for t in REQUIRED_TOOLS if shutil.which(t) is None]
        report.add("required tools on PATH (%d)" % len(REQUIRED_TOOLS),
                   not missing, "missing: " + ", ".join(missing))
        for t in OPTIONAL_TOOLS:
            if shutil.which(t):
                report.add("optional tool %s" % t, True)
            else:
                report.skip("optional tool %s" % t, "not installed")
        return ph.ok


# --------------------------------------------------------------------------
# phase 2: build
# --------------------------------------------------------------------------
def phase_build(report, args):
    with Phase(report, "2/6 build") as ph:
        if args.clean:
            rc, out = sh(["make", "clean"])
            report.add("make clean", rc == 0, out.strip()[-400:])
            if rc != 0:
                return ph.ok

        if args.no_build:
            report.skip("build", "--no-build")
            return ph.ok

        jobs = args.jobs or min(8, (os.cpu_count() or 1) * 2)
        cmd = ["make", "-j%d" % jobs, "all", "build/disk.img"]
        t0 = time.monotonic()
        rc, out = sh(cmd, timeout=BUILD_TIMEOUT)
        dt = time.monotonic() - t0
        if not report.add("%s (%.1fs)" % (" ".join(cmd), dt), rc == 0,
                          tail(out, 40)):
            sys.stdout.write(out)
        return ph.ok


def tail(text, n):
    lines = text.strip().splitlines()
    return "\n".join(lines[-n:])[-600:]


def dump_tail(guest, n=20):
    lines = [l for l in bytes(guest.buf).decode("utf-8", "replace").splitlines()
             if l.strip()]
    if not lines:
        print("  ---- no serial output at all ----", flush=True)
        return
    print("  ---- last serial output ----", flush=True)
    for line in lines[-n:]:
        print("  | " + line, flush=True)
    print("  -----------------------------", flush=True)


# --------------------------------------------------------------------------
# phase 3: static artifact checks
# --------------------------------------------------------------------------
def read_elf(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != 2:
        raise ValueError("not a 64-bit ELF")
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    e_entry = struct.unpack_from("<Q", data, 24)[0]
    e_phoff, e_phentsize, e_phnum = struct.unpack_from("<QHH", data, 32)
    interp = False
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = struct.unpack_from("<I", data, off)[0]
        interp = interp or p_type == 3  # PT_INTERP
    return {"type": e_type, "machine": e_machine, "entry": e_entry,
            "interp": interp}


def phase_artifacts(report):
    with Phase(report, "3/6 artifacts") as ph:
        try:
            data = open(BZIMAGE, "rb").read()
        except OSError as e:
            report.add("read build/bzImage", False, str(e))
            return False
        report.add("read build/bzImage (%d bytes)" % len(data), len(data) > 0)

        report.add("boot flag 0x55aa at 0x1fe",
                   data[0x1FE:0x200] == b"\x55\xaa")
        report.add("HdrS magic at 0x202", data[0x202:0x206] == b"HdrS")

        # setup_sects is stored as sectors-minus-one; 0 is the protocol's
        # shorthand for 4 sectors (see scripts/patch_bzimage.py).
        sects_field = data[0x1F1]
        setup_sects = 4 if sects_field == 0 else sects_field + 1
        syssize = struct.unpack_from("<I", data, 0x1F4)[0]
        payload_off = setup_sects * 512
        report.add("setup_sects >= 4 (header 0x%02x)" % sects_field,
                   setup_sects >= 4, "got %d" % setup_sects)
        report.add("payload offset 2 KiB aligned", payload_off % 2048 == 0,
                   "0x%x" % payload_off)
        report.add("syssize matches payload size",
                   payload_off < len(data) and
                   syssize * 16 + 15 >= len(data) - payload_off >= syssize * 16 - 15,
                   "syssize=%d payload=%d" % (syssize, len(data) - payload_off))
        setup_bin = os.path.join(BUILD, "setup.bin")
        report.add("setup.bin size == payload offset",
                   os.path.exists(setup_bin) and
                   os.path.getsize(setup_bin) == payload_off,
                   "setup.bin=%s offset=%d"
                   % (os.path.getsize(setup_bin) if os.path.exists(setup_bin)
                      else "missing", payload_off))

        rc, out = sh(["nm", VMLINUX])
        for sym in ("_start32", "_start64", "start_kernel", "panic"):
            report.add("vmlinux symbol %s" % sym,
                       rc == 0 and re.search(r"\b%s\b" % sym, out) is not None)

        try:
            elf = read_elf(VMLINUX)
            report.add("vmlinux entry in kernel text",
                       0xFFFFFFFF80100000 <= elf["entry"] < 0xFFFFFFFF81000000,
                       "entry=0x%x" % elf["entry"])
        except (OSError, ValueError, struct.error) as e:
            report.add("parse build/vmlinux.elf", False, str(e))

        # --- disk image ---------------------------------------------------
        if not os.path.exists(DISK):
            report.add("build/disk.img exists", False,
                       "run 'make build/disk.img' (make all does not build it)")
        else:
            size = os.path.getsize(DISK)
            report.add("build/disk.img is 64 MiB", size == 64 * 1024 * 1024,
                       "%d bytes" % size)
            if shutil.which("fsck.vfat"):
                rc, out = sh(["fsck.vfat", "-n", DISK], timeout=120)
                report.add("fsck.vfat -n", rc == 0, tail(out, 5))
            else:
                report.skip("fsck.vfat -n", "fsck.vfat not installed")
            if shutil.which("mdir"):
                rc, out = sh(["mdir", "-i", DISK, "::/bin"])
                report.add("FAT32 /bin listed", rc == 0, tail(out, 5))
                for prog in sorted(p[:-2] for p in os.listdir(os.path.join(ROOT, "usr"))
                                   if p.endswith(".c")):
                    report.add("/bin/%s on image" % prog,
                               rc == 0 and re.search(r"\b%s\b" % re.escape(prog), out),
                               "not in mdir ::/bin")
                rc, out = sh(["mdir", "-i", DISK, "::"])
                report.add("/README.md on image", rc == 0 and "README" in out)

        # --- user space ELFs ----------------------------------------------
        usrdir = os.path.join(ROOT, "usr")
        sources = sorted(f for f in os.listdir(usrdir) if f.endswith(".c"))
        for src in sources:
            name = src[:-2]
            path = os.path.join(BUILD, "usr", name)
            if not os.path.exists(path):
                report.add("user ELF %s" % name, False, path + " missing")
                continue
            try:
                e = read_elf(path)
            except (OSError, ValueError, struct.error) as exc:
                report.add("user ELF %s" % name, False, str(exc))
                continue
            good = (e["type"] == 2 and e["machine"] == 0x3E and not e["interp"]
                    and USER_LINK_BASE <= e["entry"] < USER_LINK_BASE + 0x10000000)
            report.add("user ELF %s (ET_EXEC, static, entry 0x%x)"
                       % (name, e["entry"]), good)
        report.add("usr/*.c covered (%d programs)" % len(sources), bool(sources))
        return ph.ok


# --------------------------------------------------------------------------
# phase 4/5: boot the guest and drive the shell
# --------------------------------------------------------------------------
class Guest:
    """QEMU with -serial stdio: stdout is the guest console, stdin its input."""

    def __init__(self, args, timeout, disk=DISK):
        self.timeout = timeout
        cmd = ["qemu-system-x86_64",
               "-m", "256", "-smp", "2",
               # the stock qemu64 model has neither SMEP (CR4 bit 20) nor
               # SMAP (bit 21) -- both would be reserved bits => #GP; the
               # kernel CPUID-gates on them, so offer both here or the
               # mitigations never actually turn on.
               "-cpu", "qemu64,+smep,+smap",
               "-display", "none", "-no-reboot",
               "-kernel", BZIMAGE,
               "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % disk,
               "-serial", "stdio"] + shlex.split(args.qemu_args)
        self.cmd = cmd
        self.proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, bufsize=0)
        self.buf = bytearray()
        self.cursor = 0
        self.eof = False
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()

    def _pump(self):
        fd = self.proc.stdout.fileno()
        while True:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            self.buf.extend(chunk)
        self.eof = True

    def send(self, line):
        """Write one command line, char by char: the 16550 FIFO is only 16 deep."""
        payload = line.encode() + b"\n"
        for b in payload:
            try:
                self.proc.stdin.write(bytes([b]))
                self.proc.stdin.flush()
            except OSError:
                return False
            time.sleep(0.003)
        time.sleep(0.05)
        return True

    def wait(self, pattern, timeout=None):
        """Find pattern in output newer than self.cursor; advance past it."""
        rx = re.compile(pattern.encode() if isinstance(pattern, str) else pattern)
        deadline = time.monotonic() + (timeout or self.timeout)
        while True:
            m = rx.search(bytes(self.buf), self.cursor)
            if m:
                self.cursor = m.end()
                return m
            if self.proc.poll() is not None:
                return None
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.02)

    def region(self, start):
        return bytes(self.buf[start:])

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        self.thread.join(timeout=5)


def phase_boot_and_shell(report, args):
    if not (os.path.exists(BZIMAGE) and os.path.exists(DISK)):
        with Phase(report, "4/6 boot"):
            report.add("bootable artifacts present", False,
                       "build/bzImage or build/disk.img is missing")
        report.skip("shell steps", "nothing to boot")
        return False

    os.makedirs(SMOKE_DIR, exist_ok=True)
    try:
        guest = Guest(args, args.timeout)
    except OSError as e:
        with Phase(report, "4/6 boot"):
            report.add("start qemu-system-x86_64", False, str(e))
        return False

    try:
        booted = True
        with Phase(report, "4/6 boot") as boot_phase:
            for name, pat in BOOT_MARKERS:
                m = guest.wait(pat, args.timeout)
                if not report.add("serial: %s" % name, m is not None):
                    booted = False
                    dump_tail(guest)
                    break

            whole = bytes(guest.buf)
            for name, pat in LATE_REPORTS:
                report.add("serial: %s" % name,
                           re.search(pat, whole) is not None,
                           "line never printed")
            report.add("no [PANIC] in console", b"[PANIC]" not in whole)
            report.add("no failed self-test assertion",
                       b"[ktest] FAIL" not in whole
                       and b"[selftest] FAIL" not in whole
                       and b"[selftest] FAILED" not in whole)
            tally = re.search(br"\[selftest\] (\d+)/(\d+) pass", whole)
            report.add("selftest tally counts every check",
                       tally is not None and tally.group(1) == tally.group(2))
            report.add("no 'command not found' so far",
                       b"command not found" not in whole)
            # C53/C54: an all-ones capacity means IDENTIFY was read without
            # validating it, or the ATA "use 48-bit" marker was taken as a
            # size.  Either way every bounds check downstream is lying.
            report.add("no all-ones disk capacity",
                       b"4294967295 sectors" not in whole)

        with Phase(report, "5/6 shell") as shell_phase:
            if not booted:
                report.skip("shell steps", "guest did not boot")
            else:
                run_shell_steps(report, guest, args)
        return boot_phase.ok and shell_phase.ok
    finally:
        with open(SERIAL_LOG, "wb") as f:
            f.write(bytes(guest.buf))
        guest.stop()
        print("\n  serial transcript: %s (%d bytes)"
              % (os.path.relpath(SERIAL_LOG, ROOT), os.path.getsize(SERIAL_LOG)),
              flush=True)


def run_shell_steps(report, guest, args):
    def step(cmd, expect, label, timeout=None):
        start = guest.cursor
        if not guest.send(cmd):
            report.add("shell: send '%s'" % cmd, False, "stdin closed")
            return None
        m = guest.wait(expect, timeout or args.timeout)
        report.add("shell: %s" % label, m is not None,
                   "no %r within %ss" % (expect, timeout or args.timeout))
        return guest.region(start)

    # FAT32 hands back 8.3 names, so the root file shows up as readme.md
    step("ls", br"(?i)readme\.md", "ls shows /README.md")
    step("ls /bin", br"(?i)systest", "ls /bin shows /bin/systest")
    step("cat /README.md", re.escape(README_LINE), "cat /README.md")
    # T-031 interactive face: a refused kill must SAY so, and an absent
    # pid must say that too (the old build printed nothing on success
    # *or* refusal -- impossible to tell apart).
    step("kill 1", br"kill: permission denied", "shell kill 1 refused")
    step("kill 9999", br"kill: no such process", "shell kill 9999 = ESRCH")

    if guest.send("systest"):
        region_start = guest.cursor
        m = guest.wait(br"systest\.done=1", args.timeout)
        report.add("shell: systest completed", m is not None,
                   "systest.done=1 missing")
        region = guest.region(region_start)
        for label, pat in SYSTEST_TAGS:
            report.add("systest %s" % label,
                       re.search(pat, region) is not None, "pattern not found")
        report.add("systest ran clean", b"command not found" not in region and
                   b"[PANIC]" not in region)
    else:
        report.add("shell: send 'systest'", False, "stdin closed")

    tail_bytes = bytes(guest.buf)[-4096:]
    report.add("console still alive at shutdown", b"[PANIC]" not in tail_bytes)


# --------------------------------------------------------------------------
# phase 6: a forged cluster chain must be reported, never followed
# --------------------------------------------------------------------------
def fat_layout(path):
    """(FAT byte offset, data sector, sectors/cluster, root cluster)."""
    with open(path, "rb") as f:
        b = f.read(512)
    reserved = struct.unpack_from("<H", b, 14)[0]
    fatsz = struct.unpack_from("<I", b, 36)[0] or struct.unpack_from("<H", b, 22)[0]
    return (reserved * 512, reserved + b[16] * fatsz, b[13],
            struct.unpack_from("<I", b, 44)[0])


def subdir_cluster(path, name):
    """First cluster of a subdirectory of the root, or None."""
    _, data, spc, root = fat_layout(path)
    with open(path, "rb") as f:
        f.seek((data + (root - 2) * spc) * 512)
        blob = f.read(spc * 512)
    for off in range(0, len(blob), 32):
        e = blob[off:off + 32]
        if e[0] == 0x00:
            break
        if (e[11] & 0x10) and bytes(e[:11]).strip() == name:
            return struct.unpack_from("<H", e, 26)[0] | \
                   (struct.unpack_from("<H", e, 20)[0] << 16)
    return None


def build_forged_image(value_for):
    """Copy the rootfs, add /CYC and forge its FAT entry.

    value_for(cluster) yields the 32-bit value written into FAT[cluster].
    Returns (image path, cluster) or (None, reason)."""
    img = os.path.join(SMOKE_DIR, "forged.img")
    shutil.copyfile(DISK, img)
    rc, out = sh(["mmd", "-i", img, "::CYC"])
    if rc != 0:
        return None, "mmd ::CYC failed: " + tail(out, 5)
    clus = subdir_cluster(img, b"CYC")
    if not clus:
        return None, "/CYC not visible after mmd"
    fat0 = fat_layout(img)[0]
    with open(img, "r+b") as f:
        f.seek(fat0 + clus * 4)
        f.write(struct.pack("<I", value_for(clus) & 0x0FFFFFFF))
    return img, clus


FORGED_CASES = [
    ("self-loop", lambda c: c, br"\[fat32\] chain walk hit its \d+ step budget"),
    ("out-of-volume entry", lambda c: 0x00FFFFF0, br"\[fat32\] corrupt FAT entry"),
]


def phase_forged_fat(report, args):
    with Phase(report, "6/6 forged FAT") as ph:
        if not (os.path.exists(BZIMAGE) and os.path.exists(DISK)):
            report.skip("forged FAT cases", "nothing to boot")
            return ph.ok

        for label, value_for, warn in FORGED_CASES:
            img, info = build_forged_image(value_for)
            if img is None:
                report.add("forged image (%s)" % label, False, info)
                continue
            guest = Guest(args, args.timeout, disk=img)
            try:
                if guest.wait(br"# ", args.timeout) is None:
                    report.add("forged FAT boot (%s)" % label, False,
                               "no shell prompt")
                    dump_tail(guest)
                    continue
                guest.send("cat /CYC/zzz")
                report.add("forged FAT reported (%s)" % label,
                           guest.wait(warn, args.timeout) is not None,
                           "driver stayed silent about the forged entry")
                report.add("shell returns (%s)" % label,
                           guest.wait(br"# ", args.timeout) is not None,
                           "guest hung walking the forged chain")
            finally:
                with open(os.path.join(
                        SMOKE_DIR, "serial-forged-%s.log" % label.split()[0]),
                        "wb") as f:
                    f.write(bytes(guest.buf))
                guest.stop()
        return ph.ok


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="lnxrm smoke test")
    ap.add_argument("--no-build", action="store_true",
                    help="reuse the artifacts already in build/")
    ap.add_argument("--clean", action="store_true",
                    help="make clean before building")
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="seconds allowed per wait (default 60)")
    ap.add_argument("--jobs", type=int, default=None,
                    help="make -jN (default: 2x cores)")
    ap.add_argument("--qemu-args", default="",
                    help="extra QEMU arguments as one quoted string")
    args = ap.parse_args()

    print("lnxrm smoke test  (root: %s)" % ROOT)
    report = Report()

    if not phase_toolchain(report):
        return finish(report)
    if not phase_build(report, args):
        return finish(report)
    if not phase_artifacts(report):
        return finish(report)
    if not phase_boot_and_shell(report, args):
        return finish(report)
    if not phase_forged_fat(report, args):
        return finish(report)
    return finish(report)


def finish(report):
    failed = report.failed
    total = len(report.checks)
    print("\n" + "=" * 60)
    if failed:
        print("FAIL  %d/%d checks failed" % (len(failed), total))
        for name, _, detail in failed:
            print("  - %s%s" % (name, ("  <- " + detail) if detail else ""))
    else:
        print("PASS  %d/%d checks" % (total, total))
    if report.skipped:
        print("skipped: " + ", ".join(n for n, _ in report.skipped))
    print("=" * 60)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
