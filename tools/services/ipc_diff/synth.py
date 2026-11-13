#!/usr/bin/env python3
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic request scripts for ipc_diff.py (the replay format of runtime/tests/ipc_replay/replay_main.cpp).

A Script opens services through sm: and calls their commands; `fuzz` sends one command many deterministic variants (argument sizes and
patterns, buffers of each kind and size, the pid flag). Per-service generators (gen_<service>.py next to this file) describe the
interfaces from the service's spec and call these helpers:

  python -I synth.py <service> <out.txt> [--seed N] [--variants N] [--variant NAME]

Files a script needs (fonts, pictures, tokens) are written next to it, in <out>.aux/, and named by absolute path in its config lines.
"""
import importlib
import random
import struct
import sys
from pathlib import Path

LOCAL_UID = (0x0000000000000001, 0x6E78727475736572)   # services::TitleProfile defaults (user_uid_hi / lo)


def sm_name(s):
    b = s.encode()[:8]
    return b + b"\0" * (8 - len(b))


class Script:
    def __init__(self, seed=1, aux_dir="."):
        self.aux_dir = aux_dir
        self.lines = []
        self.seq = 0
        self.next_sid = 2      # sid 1 = sm:
        self.rng = random.Random(seed)

    def comment(self, text):
        self.lines.append("# " + text)

    def reset(self):
        self.lines.append("reset")

    def config(self, section, key, value):
        self.lines.append(f"config {section} {key} {value}")

    def q(self, sid, iface, cmd, raw=b"", bufs=(), pid=False, hc=(), hm=(), os_=(), nobj=0, mtype=4):
        """One request. bufs: (kind, size, bytes or None) with kind a/x/w (input) or b/c (output). Returns the sids its objects get."""
        self.seq += 1
        parts = ["Q", str(self.seq), "0", str(sid), iface, str(cmd), str(mtype), ("51" if pid else "0"), "in=" + (raw.hex() or "-")]
        counts = {}
        for kind, size, data in bufs:
            n = counts.get(kind, 0)
            counts[kind] = n + 1
            if kind in "axw":
                data = data if data is not None else b""
                parts.append(f"{kind}{n}={size:x}:{data[:size].hex() or '-'}")
            else:
                parts.append(f"{kind}{n}={size:x}")
        if hc:
            parts.append("hc=" + ",".join(hc))
        if hm:
            parts.append("hm=" + ",".join(hm))
        if os_:
            parts.append("os=" + ",".join(str(x) for x in os_))
        sids = []
        if nobj:
            sids = list(range(self.next_sid, self.next_sid + nobj))
            self.next_sid += nobj
            parts.append("as=" + ",".join(str(x) for x in sids))
        self.lines.append(" ".join(parts))
        return sids

    def open(self, name):
        """sm:GetServiceHandle(name): the sid of the session (the service object)."""
        self.q(1, "sm:", 0, raw=struct.pack("<Q", 0), pid=True)   # RegisterClient
        return self.q(1, "sm:", 1, raw=sm_name(name), nobj=1)[0]

    # ---- variants -------------------------------------------------------------------------------------------------------------
    def raw_variants(self, sizes=(0, 4, 8, 16, 24, 32, 64)):
        r = self.rng
        out = []
        for n in sizes:
            out.append(bytes(n))
            out.append(bytes(r.randrange(256) for _ in range(n)))
            out.append(b"\xff" * n)
            words = b"".join(struct.pack("<I", r.choice((0, 1, 2, 3, 4, 5, 6, 7, 8, 15, 16, 17, 31, 32, 64, 255, 0x100, 0xFFFFFFFF)))
                             for _ in range((n + 3) // 4))[:n]
            out.append(words)
            if n >= 16:
                out.append((struct.pack("<QQ", *LOCAL_UID) + bytes(n))[:n])
        return out

    def buf_variants(self):
        r = self.rng
        sizes = (0, 1, 4, 8, 0x10, 0x18, 0x20, 0x40, 0x80, 0x100, 0x400, 0x1000, 0x8000)
        pick = lambda: r.choice(sizes)
        rnd = lambda n: bytes(r.randrange(256) for _ in range(n))
        v = [()]
        for kind in "bcax":
            for n in (pick(), pick(), 0x1000):
                v.append(((kind, n, rnd(n) if kind in "ax" else None),))
        v.append((("a", 0x100, rnd(0x100)), ("b", 0x1000, None), ("c", 0x1000, None)))
        v.append((("b", 0x10, None), ("b", 0x10, None), ("b", 0x10, None)))
        v.append((("c", 0x40, None), ("c", 0x40, None), ("c", 0x40, None)))
        v.append((("b", 0x200, None), ("b", 0x200, None), ("b", 0x200, None)))
        v.append((("x", 0x20, rnd(0x20)), ("c", 0x400, None)))
        v.append((("a", 0x8, rnd(8)), ("a", 0x40, rnd(0x40)), ("b", 0x80, None)))
        return v

    def fuzz(self, sid, iface, cmd, n=40, sizes=(0, 4, 8, 16, 24, 32, 64), bufs=None, pid_too=True, nobj=0):
        """n variants of one command: every argument pattern, then random pairings with the buffer variants and the pid flag."""
        raws = self.raw_variants(sizes)
        bvs = bufs if bufs is not None else self.buf_variants()
        got = []
        for i in range(n):
            raw = raws[i % len(raws)]
            bv = bvs[(i * 7 + self.rng.randrange(len(bvs))) % len(bvs)]
            pid = pid_too and (i % 3 == 1)
            got += self.q(sid, iface, cmd, raw=raw, bufs=bv, pid=pid, nobj=nobj)
        return got

    def sweep(self, sid, iface, ids):
        """Each id once with an ordinary request (8 zero bytes, a B and a C buffer): what an unknown command answers."""
        for cmd in ids:
            self.q(sid, iface, cmd, raw=bytes(8), bufs=(("b", 0x100, None), ("c", 0x100, None)))

    def write(self, path):
        Path(path).write_text("\n".join(self.lines) + "\n")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    seed, variants, variant = 1, 40, None
    args = sys.argv[3:]
    for i, a in enumerate(args):
        if a == "--seed":
            seed = int(args[i + 1])
        if a == "--variants":
            variants = int(args[i + 1])
        if a == "--variant":
            variant = args[i + 1]
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    mod = importlib.import_module("gen_" + sys.argv[1])
    out = Path(sys.argv[2]).resolve()
    s = Script(seed, str(out.parent / (out.name + ".aux")))
    if variant:
        mod.generate(s, variants, variant)
    else:
        mod.generate(s, variants)
    s.write(out)
    print(f"{s.seq} requests -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
