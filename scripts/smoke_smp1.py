#!/usr/bin/env python3
"""Drive systest on a -smp 1 guest and check every SYSTEST tag.

scripts/smoke_test.py's BOOT_MARKERS are -smp 2 specific (they require AP
markers and "2 cpus online"), so `--qemu-args "-smp 1"` can never pass the
boot phase.  This is the -smp 1 half of the Step 3 acceptance instead: boot,
run systest, assert the same tag set the smoke test uses.
"""
import re
import sys

sys.path.insert(0, "scripts")
import smoke_test as st  # noqa: E402


class Args:
    pass


args = Args()
args.timeout = 60.0
args.qemu_args = "-smp 1"

guest = st.Guest(args, 60.0)
rc = 1
try:
    if guest.wait(br"# ", 60) is None:
        print("FAIL: no shell prompt on -smp 1")
        st.dump_tail(guest, 30)
    else:
        guest.send("systest")
        done = guest.wait(br"systest\.done=1", 60) is not None
        print("systest.done=1: %s" % ("PASS" if done else "FAIL"))
        region = bytes(guest.buf)
        failed = [lbl for lbl, pat in st.SYSTEST_TAGS
                  if re.search(pat, region) is None]
        print("tags: %d checked, %d failed" % (len(st.SYSTEST_TAGS), len(failed)))
        for lbl in failed:
            print("  FAIL %s" % lbl)
        print("[PANIC] present: %s" % (b"[PANIC]" in region))
        print("[ktest] FAIL present: %s" % (b"[ktest] FAIL" in region))
        rc = 0 if (done and not failed and b"[PANIC]" not in region) else 1
finally:
    with open("build/smoke/serial-smp1.log", "wb") as f:
        f.write(bytes(guest.buf))
    guest.stop()
    print("serial: build/smoke/serial-smp1.log (%d bytes)"
          % len(open("build/smoke/serial-smp1.log", "rb").read()))
sys.exit(rc)
