# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
"""audout:u / audin:u and their IAudioOut / IAudioIn streams: the managers (lists, opens with names, rates, channels), every stream command,
the buffer protocol (append, contains, count, released tags, the 64-buffer limit, flush, stop and start), volumes, descriptors that cannot
be read. Release timing is real time: the replay sends requests back to back, so a started stream releases nothing between two of them
except what Stop / Flush release; fields that still vary are masked by ipc_diff.py. Ids: Switchbrew "Audio services"."""
import struct

SAMPLES = b"KKKKKKKK"   # the replay's sample area (nxrt_ipc_replay)
OUT = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13]
IN = [0, 1, 2, 3, 4, 5, 6, 8, 9, 11, 14]


def desc(addr=SAMPLES, capacity=0x780, size=0x780, offset=0):
    return struct.pack("<Q", 0) + (addr if isinstance(addr, bytes) else struct.pack("<Q", addr)) + struct.pack("<QQQ", capacity, size, offset)


def open_req(rate, ch, name=None, out_size=0x100):
    raw = struct.pack("<IHHQ", rate, ch, 0, 0)
    bufs = []
    if name is not None:
        n = name.encode() + b"\0"
        bufs.append(("a", 0x100, n.ljust(0x100, b"\0")))
    if out_size:
        bufs.append(("b", out_size, None))
    return raw, tuple(bufs)


def stream(s, sid, iface, out, variants):
    app = 3 if out else 3
    rel = 5
    cnt = 9 if out else 11
    flush = 11 if out else 14
    contains = 6
    s.q(sid, iface, 0)
    if out:
        s.q(sid, iface, 13)
        s.q(sid, iface, 10)
        for v in (0.5, -1.0, 2.0, 0.0, 1.0):
            s.q(sid, iface, 12, raw=struct.pack("<f", v))
            s.q(sid, iface, 13)
        s.q(sid, iface, 12, raw=b"")
        s.q(sid, iface, 13)
    s.q(sid, iface, 4)
    s.q(sid, iface, 4)
    # stopped: queue kept by Stop, Flush answers 0
    for tag in (1, 2, 3):
        s.q(sid, iface, app, raw=struct.pack("<Q", tag), bufs=(("a", 0x28, desc()),))
    s.q(sid, iface, cnt)
    s.q(sid, iface, flush)
    s.q(sid, iface, 2)
    s.q(sid, iface, cnt)
    for tag in (2, 9):
        s.q(sid, iface, contains, raw=struct.pack("<Q", tag))
    # descriptors: none, empty, short, out of range, unmapped samples, offset past capacity
    for bv in ((), (("a", 0, None),), (("a", 0x10, desc()[:0x10]),), (("x", 0x28, desc()),),
               (("a", 0x28, desc(capacity=0x100, size=0x80, offset=0xC0)),), (("a", 0x28, desc(addr=0x10)),),
               (("a", 0x28, desc(addr=0)),), (("a", 0x28, desc(size=0)),), (("a", 0x28, desc(size=7, capacity=7)),),
               (("a", 0x40, desc() + bytes(0x18)),)):
        s.q(sid, iface, app, raw=struct.pack("<Q", 100), bufs=bv)
    s.q(sid, iface, cnt)
    # start, stop at once: everything released; GetReleased with many buffer shapes
    s.q(sid, iface, 1)
    s.q(sid, iface, 1)
    s.q(sid, iface, 0)
    s.q(sid, iface, 2)
    s.q(sid, iface, 0)
    s.q(sid, iface, cnt)
    for tag in (1, 2, 100):
        s.q(sid, iface, contains, raw=struct.pack("<Q", tag))
    for bv in ((("b", 0x0C, None),), (), (("b", 0x20, None),), (("c", 0x8, None),), (("b", 0x400, None),), (("b", 0x7, None),)):
        s.q(sid, iface, rel, bufs=bv)
        s.q(sid, iface, rel + (3 if out else 4), bufs=bv)
    # the 64-buffer limit
    for tag in range(200, 266):
        s.q(sid, iface, app, raw=struct.pack("<Q", tag), bufs=(("a", 0x28, desc()),))
    s.q(sid, iface, cnt)
    s.q(sid, iface, flush)
    s.q(sid, iface, 2)
    s.q(sid, iface, cnt)
    for _ in range(3):
        s.q(sid, iface, rel, bufs=(("b", 0x200, None),))
    s.q(sid, iface, app, raw=struct.pack("<Q", 300), bufs=(("a", 0x28, desc()),))
    # started: flush releases, played count does not move for flushed buffers
    s.q(sid, iface, 1)
    s.q(sid, iface, flush)
    s.q(sid, iface, cnt)
    s.q(sid, iface, rel, bufs=(("b", 0x200, None),))
    if out:
        s.q(sid, iface, 10)
    s.q(sid, iface, 2)
    s.fuzz(sid, iface, app, n=variants // 2, sizes=(0, 8, 16))
    s.q(sid, iface, 2)
    for cmd in (OUT if out else IN):
        if cmd in (1,):
            continue
        s.fuzz(sid, iface, cmd, n=variants // 4, sizes=(0, 4, 8))
    s.q(sid, iface, 2)
    s.sweep(sid, iface, list(range(0, 20)) + [100])
    s.q(sid, iface, 2)


def generate(s, variants):
    for name, out in (("audout:u", True), ("audin:u", False)):
        s.reset()
        sid = s.open(name)
        dev = "DeviceOut" if out else "BuiltInHeadset"
        for cmd in ([0, 2] if out else [0, 2, 4]):
            for bv in ((("b", 0x100, None),), (), (("b", 0x40, None),), (("b", 0x200, None),), (("c", 0x100, None),), (("b", 1, None),)):
                s.q(sid, name, cmd, bufs=bv)
        iface = "IAudioOut" if out else "IAudioIn"
        for cmd in (1, 3):
            for rate in (48000, 0, 44100, 200000, 192000, 7999, 8000, 32000, 0xFFFFFFFF):
                for ch in (2, 0, 1, 4, 6, 3, 8):
                    raw, bufs = open_req(rate, ch)
                    obj = s.q(sid, name, cmd, raw=raw, bufs=bufs, pid=True, nobj=1)[0]
                    s.lines.append(f"X {obj}")
            for nm in ("DeviceOut", "BuiltInHeadset", "Speaker", "", "deviceout"):
                raw, bufs = open_req(48000, 2, nm)
                obj = s.q(sid, name, cmd, raw=raw, bufs=bufs, nobj=1)[0]
                s.lines.append(f"X {obj}")
            raw, _ = open_req(48000, 2)
            obj = s.q(sid, name, cmd, raw=raw[:4], nobj=1)[0]
            s.lines.append(f"X {obj}")
        for rate, ch in ((48000, 2), (32000, 2), (48000, 6)) if out else ((48000, 2), (16000, 1)):
            raw, bufs = open_req(rate, ch, dev)
            obj = s.q(sid, name, 1, raw=raw, bufs=bufs, pid=True, nobj=1)[0]
            stream(s, obj, iface, out, variants)
            s.lines.append(f"X {obj}")
        s.fuzz(sid, name, 1, n=variants // 2, sizes=(0, 4, 8, 16), nobj=1)
        s.sweep(sid, name, list(range(0, 12)) + [100])
    # a reset with a started stream holding buffers: the object stays usable
    s.reset()
    sid = s.open("audout:u")
    raw, bufs = open_req(48000, 2, "DeviceOut")
    obj = s.q(sid, "audout:u", 1, raw=raw, bufs=bufs, nobj=1)[0]
    s.q(obj, "IAudioOut", 1)
    for tag in (7, 8):
        s.q(obj, "IAudioOut", 3, raw=struct.pack("<Q", tag), bufs=(("a", 0x28, desc()),))
    s.lines.append("reset keep")
    s.q(obj, "IAudioOut", 0)
    s.q(obj, "IAudioOut", 5, bufs=(("b", 0x20, None),))
    s.q(obj, "IAudioOut", 1)
    s.q(obj, "IAudioOut", 2)
