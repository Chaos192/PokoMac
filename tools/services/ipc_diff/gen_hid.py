# SPDX-License-Identifier: GPL-3.0-or-later
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
# Copyright (c) 2026 Chaos192
"""The hid service of runtime/src/services/hid.cpp: nn::hid::IHidServer (every command of its own table, the spec's vectors in order),
nn::hid::IAppletResource (the shared memory), nn::hid::IActiveVibrationDeviceList, and its C++ entry points through the replay's hid
api lines (runtime/tests/ipc_replay/replay_main.cpp): one sample at a time with the 5 ms sampler's background sampling off, a scripted
input source (pads, styles, colours, six-axis, sticks past the threshold, touch and gestures, keyboard, mouse), the offline web page's
input hold, the controller applet's bench, the console mode change, the supported styles and npad ids, vibration values and the
forwarding to a sink, and the Joy-Con 2 mouse (the Nmouse and Jcms blocks in transfer memory, the play layer's feed, mode, request and
pointer space). Then fuzz per command and an id sweep per interface.

Determinism: every `reset` turns background sampling on again, so each is followed by `api hid_bg 0` before an applet resource is
made (CreateAppletResource starts the thread); touches never move between two samples (a Pan's velocity divides by the time between
samples); the Joy-Con 2 mouse feed is sampled right after it is given (it counts as live for 250 ms), and `api sleep_ms 300` ages it."""
import struct

HID = "nn::hid::IHidServer"
RES = "nn::hid::IAppletResource"
VIB = "nn::hid::IActiveVibrationDeviceList"

# hid.cpp's own commands (spec section 3.1); 0 and 203 return objects
OWN = ([0, 1, 11, 21, 31, 32, 40, 51, 55, 56, 58, 59] + list(range(60, 84)) + [91]
       + [100, 101, 102, 103, 104, 106, 107, 108, 109] + list(range(120, 135))
       + list(range(200, 215)) + list(range(300, 311)) + [400, 1000, 1001, 1002, 1003, 1004]
       + [3110, 3111, 3112, 3113, 3130, 3131, 3132, 3133, 3135, 3136, 3137, 3138, 3139, 3140, 3141, 3142, 3143, 3150])
# the ones whose answer only says "accepted" (fuzzed less)
IGNORED = [32, 40, 51, 55, 56, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 83, 125, 126,
           127, 130, 134, 207, 208, 209, 210, 212, 213] + list(range(300, 311)) + [1002, 1003, 1004, 3113, 3142, 3143, 3150]
# extended by ext_hid.cpp by name: hid.cpp must not register them (their answers come from there)
EXT = [26, 41, 84, 85, 87, 88, 89, 92, 135, 136, 401, 402, 403, 404, 405, 406, 407, 500, 501, 522, 528, 529, 3000, 3006, 3007, 3015]
UNKNOWN = [2, 3, 4, 5, 10, 12, 20, 22, 30, 33, 42, 50, 52, 57, 86, 90, 93, 105, 110, 119, 137, 199, 215, 299, 311, 399, 408, 499, 530,
           999, 1005, 2000, 2999, 3016, 3100, 3109, 3114, 3120, 3129, 3134, 3144, 3149, 3151, 4000, 0xFFFF, 0x7FFFFFFF]

STYLE = {"none": 0, "full": 1, "handheld": 2, "dual": 3, "left": 4, "right": 5}   # nxrt::hid::NpadStyle


def u32(*w):
    return b"".join(struct.pack("<I", x & 0xFFFFFFFF) for x in w)


def u64(*w):
    return b"".join(struct.pack("<Q", x & 0xFFFFFFFFFFFFFFFF) for x in w)


def f32(*v):
    return b"".join(struct.pack("<f", x) for x in v)


def handle(dev, npad, pos=0):
    return (dev & 0xFF) | ((npad & 0xFF) << 8) | ((pos & 0xFF) << 16)


class Hid:
    """The script with the hid api lines."""

    def __init__(self, s):
        self.s = s

    def api(self, what):
        self.s.lines.append("api " + what)

    def reset(self, keep=False):
        self.s.lines.append("reset keep" if keep else "reset")
        self.api("hid_bg 0")

    def sample(self, n=1):
        self.api("hid_sample" if n == 1 else f"hid_sample {n}")

    def pad(self, slot, connected=1, style="full", buttons=0, lx=0, ly=0, rx=0, ry=0):
        self.api(f"hid_pad {slot} {connected} {STYLE[style]} {buttons:x} {lx} {ly} {rx} {ry}")

    def q(self, sid, cmd, raw=b"", bufs=(), pid=False, hc=(), nobj=0, iface=HID):
        return self.s.q(sid, iface, cmd, raw=raw, bufs=bufs, pid=pid, hc=hc, nobj=nobj)


def open_hid(h, resource=True):
    """A hid session, and (resource) an applet resource whose shared memory handle makes the replay track the block."""
    sid = h.s.open("hid")
    r = None
    if resource:
        r = h.q(sid, 0, raw=u64(0), pid=True, nobj=1)[0]
        h.q(r, 0, iface=RES)
    return sid, r


# ---- the spec's vectors (section 10), in order ------------------------------------------------------------------------------------------
def vectors(h):
    s = h.s
    h.reset()
    h.api("am_docked 1")
    sid, _ = open_hid(h, resource=False)
    h.api("hid_sets")
    h.q(sid, 101, pid=True)
    h.q(sid, 100, raw=u32(3) + u64(0), pid=True)
    h.q(sid, 101, pid=True)
    for ids in ([0, 0x20], [0, 1, 0x20, 0x10, 0x99], None, [0, 1, 2, 3, 4, 5, 6, 7, 0x10, 0x20, 0, 0]):
        bufs = (("a", 4 * len(ids), u32(*ids)),) if ids is not None else ()
        h.q(sid, 102, raw=u64(0), bufs=bufs, pid=True)
        h.api("hid_sets")
    h.q(sid, 106, raw=u32(0) + u64(0, 0), pid=True)
    h.q(sid, 106, raw=u32(0x20) + u64(0, 0), pid=True)
    h.q(sid, 106, raw=u32(8) + u64(0, 0), pid=True)
    for i in (0, 3, 7, 0x20, 0x10, 8):
        h.q(sid, 108, raw=u32(i))
    h.q(sid, 121, raw=u64(0), pid=True)
    h.q(sid, 120, raw=u64(0, 1), pid=True)
    h.q(sid, 121, raw=u64(0), pid=True)
    h.q(sid, 129, raw=u64(0))
    h.q(sid, 128, raw=u64(0, 3))
    h.q(sid, 129, raw=u64(0))
    h.q(sid, 1001, raw=u64(0))
    h.q(sid, 1000, raw=u64(2, 0))
    h.q(sid, 1001, raw=u64(0))
    h.q(sid, 68, raw=u32(3) + u32(0) + u64(0))
    h.q(sid, 69, raw=u32(1, 3) + u64(0))
    h.q(sid, 68, raw=u32(3) + u32(0) + u64(0))
    h.q(sid, 205)
    h.q(sid, 204, raw=b"\x00")
    h.q(sid, 205)
    h.q(sid, 131, raw=u32(0, 0) + u64(0))
    h.q(sid, 132, raw=b"\x01")
    h.q(sid, 131, raw=u32(0, 0) + u64(0))
    h.q(sid, 133, raw=u32(0, 0) + u64(0, 0))
    h.api("hid_shm")
    for c in (400, 83, 1003, 55, 59, 71, 74, 208, 77, 80, 309, 40, 58, 62, 65):
        h.q(sid, c, raw=u64(0))
    h.q(sid, 82, raw=u32(handle(3, 0)) + u32(0) + u64(0))
    h.q(sid, 82, raw=u32(handle(3, 9)))
    for hd in (handle(3, 0), handle(3, 0, 1), handle(3, 0, 2), handle(8, 1), handle(9, 0), handle(3, 9), handle(5, 0x20)):
        h.q(sid, 200, raw=u32(hd))
    h.api("hid_vib_sink 1")
    h.q(sid, 201, raw=u32(handle(3, 0, 1)) + f32(0.5, 160.0, 0.25, 320.0) + u32(0) + u64(0))
    h.q(sid, 202, raw=u32(handle(3, 0, 1)) + u32(0) + u64(0))
    h.q(sid, 202, raw=u32(handle(3, 0)) + u32(0) + u64(0))
    for hd in (handle(5, 0x20), handle(3, 0x10), handle(3, 0, 2), handle(3, 9)):
        h.q(sid, 201, raw=u32(hd) + f32(0.75, 150.0, 0.5, 300.0) + u32(0) + u64(0))
        h.q(sid, 202, raw=u32(hd) + u32(0) + u64(0))
    h.q(sid, 206, raw=u64(0), bufs=(("a", 12, u32(handle(3, 0), handle(3, 0, 1), handle(3, 1))),
                                    ("a", 32, f32(0.1, 160.0, 0.2, 320.0, 0.3, 161.0, 0.4, 321.0))))
    h.q(sid, 211, raw=u32(handle(3, 0)) + u32(0) + u64(0))
    h.q(sid, 103, raw=u64(0))
    h.sample()
    h.q(sid, 211, raw=u32(handle(3, 0)) + u32(0) + u64(0))
    h.q(sid, 211, raw=u32(handle(3, 1)) + u32(0) + u64(0))
    h.q(sid, 213, raw=bytes(36))
    s.sweep(sid, HID, (4, 5, 105, 3114, 3134))
    h.q(sid, 3133, raw=u64(0))
    h.q(sid, 3132, raw=u32(0x500, 0x2D0) + u64(0))
    h.q(sid, 3133, raw=u64(0))
    h.q(sid, 3132, raw=u32(0, 0xFFFFFFFF) + u64(0))
    h.q(sid, 3133, raw=u64(0))
    r = h.q(sid, 0, raw=u64(0), pid=True, nobj=1)[0]
    h.q(r, 0, iface=RES)
    h.q(r, 0, iface=RES)
    v = h.q(sid, 203, nobj=1)[0]
    h.q(v, 0, iface=VIB)
    h.api("hid_live")


# ---- the shared memory: npad entries, styles, rings ------------------------------------------------------------------------------------
def npads(h):
    s = h.s
    # after 103 and one sample, docked, no source; then undocked (the mode change samples once)
    h.reset()
    h.api("am_docked 1")
    sid, r = open_hid(h)
    h.q(sid, 103, raw=u64(0))
    h.sample()
    h.api("hid_shm")
    for i in (0, 1, 7, 0x20, 0x10):
        h.q(sid, 106, raw=u32(i) + u64(0, 0), pid=True)
    h.api("am_docked 0")
    h.sample()
    h.api("hid_read")
    h.api("am_docked 1")
    h.sample()
    h.api("hid_mode_changed")
    h.reset(keep=True)                     # new style events: signal them one by one
    for i in (0, 1, 7, 0x20, 0x10):
        h.q(sid, 106, raw=u32(i) + u64(0, 0), pid=True)
    for i in (0, 3, 8, 9, 10, 0xFFFFFFFF):
        h.api(f"hid_signal {i}")
    # without 103: no npad entry written
    h.reset()
    sid, r = open_hid(h)
    h.sample(2)
    h.q(sid, 104, raw=u64(0))
    h.sample()
    # 100 with Handheld only, docked: entry 0 not on
    h.reset()
    sid, r = open_hid(h)
    h.q(sid, 100, raw=u32(2) + u64(0), pid=True)
    h.q(sid, 103, raw=u64(0))
    h.sample()
    h.api("am_docked 0")
    h.sample()
    h.api("am_docked 1")
    # a scripted source: styles, colours, sticks, six-axis, supported styles and ids
    h.reset()
    sid, r = open_hid(h)
    h.api("hid_src manual")
    h.api("hid_read")
    h.q(sid, 109, raw=u32(5) + u32(0) + u64(0))
    h.sample()
    h.pad(0)
    h.pad(1, style="dual")
    h.q(sid, 102, raw=u64(0), bufs=(("a", 12, u32(0, 1, 0x20)),), pid=True)
    for i in range(10):
        h.q(sid, 106, raw=u32(i if i < 8 else (0x20 if i == 8 else 0x10)) + u64(0, 0), pid=True)
    h.sample()
    h.api("hid_read")
    for lx, ly, rx, ry in ((20000, -20000, 0, 0), (16383, 0, 0, 0), (16384, 0, 0, 0), (-16383, -16383, 16383, 16383),
                           (-16384, 16384, -16384, 16384), (0, 0, 16384, -16384), (32767, -32767, -32768, 32767)):
        h.pad(0, buttons=0x1, lx=lx, ly=ly, rx=rx, ry=ry)
        h.sample()
        h.api("hid_read")
    h.pad(0, buttons=0x50000)   # stick bits the source set itself stay set
    h.sample()
    h.api("hid_color 0 ff102030 ff405060")
    h.sample()
    for style in ("left", "right", "dual", "handheld", "none", "full"):
        h.pad(0, style=style, buttons=0x3)
        h.sample()
        h.q(sid, 211, raw=u32(handle(3, 0)) + u32(0) + u64(0))
    h.api("hid_six 0 0 0.125 -0.5 -0.875 1.5 -2.25 3 0.25 0.5 0.75")
    h.sample()
    h.api("hid_six 1 0 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1 0 0 0 0 1 0 -1 0")
    h.sample()
    h.q(sid, 82, raw=u32(handle(3, 0)) + u32(0) + u64(0))
    h.q(sid, 82, raw=u32(handle(5, 1)) + u32(0) + u64(0))
    h.q(sid, 82, raw=u32(handle(3, 0x20)) + u32(0) + u64(0))
    h.q(sid, 82, raw=u32(handle(3, 0x10)) + u32(0) + u64(0))
    # handheld (slot 9) and other (slot 8) entries
    h.pad(9, style="handheld", buttons=0x10)
    h.pad(8, style="full", buttons=0x20)
    h.sample()
    h.q(sid, 102, raw=u64(0), bufs=(("a", 16, u32(0, 1, 0x20, 0x10)),), pid=True)
    h.sample()
    h.api("am_docked 0")
    h.sample()
    h.api("am_docked 1")
    for st in (0x1F, 0x8, 0x10, 0x4, 0x3, 0, 0xFFFFFFFF, 0x1F):
        h.q(sid, 100, raw=u32(st) + u64(0), pid=True)
        h.sample()
    h.q(sid, 102, raw=u64(0), bufs=(("a", 4, u32(1)),), pid=True)   # No1 no longer supported: entry 0 disconnects
    h.sample()
    h.q(sid, 102, raw=u64(0), bufs=(("x", 8, u32(0, 1)),), pid=True)
    h.sample()
    for slot in range(2, 8):
        h.pad(slot, style=("full", "dual", "left", "right", "handheld", "full")[slot - 2], buttons=1 << slot, lx=slot * 1000)
    h.q(sid, 102, raw=u64(0), bufs=(("a", 40, u32(0, 1, 2, 3, 4, 5, 6, 7, 0x20, 0x10)),), pid=True)
    h.sample()
    h.pad(3, connected=0)
    h.sample()
    # joy assignment writes, kept until the next style write
    for c, i in ((122, 0), (123, 1), (124, 2), (133, 3), (122, 0x20), (123, 0x10), (122, 8), (124, 0xFFFFFFFF), (133, 9)):
        h.q(sid, c, raw=u32(i) + u32(0) + u64(0, 0))
        h.api("hid_shm")
    h.sample()
    h.pad(0, style="left")
    h.sample()
    # the rings wrap: 17 entries, count kept at 16
    h.sample(20)
    h.q(sid, 104, raw=u64(0))
    h.sample(2)
    h.q(sid, 103, raw=u64(0))
    h.sample()
    # IsVibrationDeviceMounted of every entry after the samples
    for i in (0, 1, 2, 3, 7, 0x20, 0x10, 8):
        h.q(sid, 211, raw=u32(handle(3, i)) + u32(0) + u64(0))
    h.api("hid_src none")


# ---- touch, gestures, mouse, keyboard -----------------------------------------------------------------------------------------------------
def devices(h):
    s = h.s
    h.reset()
    sid, r = open_hid(h)
    for c in (1, 11, 21, 31):
        h.q(sid, c, raw=u64(0), pid=True)
    h.api("hid_src manual")
    h.pad(0)
    h.sample()
    # a tap, a double tap (taps well within 0.35 s of each other), a held finger (no Press: less than 0.5 s), a second finger (Cancel)
    for touches in (["0:100:200:15:15:0"], [], [], [], ["0:100:200:15:15:0"], [], [], [], ["3:640:360:20:30:-90"], ["3:640:360:20:30:-90"],
                    ["3:640:360:20:30:-90", "4:700:400:10:10:45"], ["3:640:360:20:30:-90"], [], [], [],
                    ["0:1:2:3:4:5", "1:11:12:13:14:15", "2:21:22:23:24:25", "3:31:32:33:34:35", "4:41:42:43:44:45"], [], [], []):
        h.api(f"hid_touch {len(touches)} " + " ".join(touches))
        h.sample()
    h.api("hid_touch 17 " + " ".join(f"{k}:{k * 10}:{k * 20}:1:1:{k}" for k in range(16)))   # count capped at 16
    h.sample()
    h.api("hid_touch 0")
    h.sample(3)
    # ActivateGesture: a new id empties the ring's count and pushes the last state once
    for gid in (0, 0, 1, -1, 1, 7):
        h.q(sid, 91, raw=struct.pack("<i", gid) + u32(0) + u64(0), pid=True)
    h.sample()
    # keyboard and mouse
    h.api("hid_kbd 1 102 10 0 0 200000000000000")
    h.sample()
    h.api("hid_mouse 1 640 360 5 -5 1 -1 1")
    h.sample()
    h.api("hid_kbd 0 102 10 0 0 0")
    h.api("hid_mouse 0 1 2 3 4 5 6 1f")
    h.sample()
    h.api("hid_read")
    # the offline web page holds the input: the page gets the buttons and the first deflected left stick
    h.pad(1, buttons=0x2, lx=20000, ly=-5000)
    h.pad(0, buttons=0x1, rx=9000)
    h.api("hid_touch 1 0:320:240:15:15:0")
    h.api("hid_kbd 1 2 10 0 0 0")
    h.api("hid_mouse 1 10 20 7 8 1 2 1")
    h.api("hid_read")
    h.api("web_hold 1")
    h.api("hid_read")
    h.api("hid_raw")
    h.sample()
    h.q(sid, 82, raw=u32(handle(3, 0)) + u32(0) + u64(0))
    h.api("web_hold 0")
    h.api("hid_read")
    h.sample()
    # without a source the page's hold is not consulted
    h.api("hid_src none")
    h.api("web_hold 1")
    h.api("hid_read")
    h.api("web_hold 0")


# ---- the controller applet's bench and the supported ids ---------------------------------------------------------------------------------
def bench(h):
    h.reset()
    sid, r = open_hid(h)
    h.q(sid, 103, raw=u64(0))
    h.api("hid_src manual")
    h.pad(0)
    h.pad(1, buttons=0x4)
    h.pad(2, buttons=0x8)
    h.api("hid_bench_close 1")
    h.api("hid_read")
    h.api("hid_raw")
    h.sample()
    h.q(sid, 102, raw=u64(0), bufs=(("a", 12, u32(0, 1, 0x20)),), pid=True)   # the mask changed: the bench is cleared
    h.api("hid_bench")
    h.api("hid_bench_close 1")
    h.q(sid, 102, raw=u64(0), bufs=(("a", 12, u32(0, 1, 0x20)),), pid=True)   # the same mask: kept
    h.api("hid_bench")
    h.api("hid_bench_open 4")
    h.api("hid_bench_close 2")
    h.pad(2, connected=0)
    h.sample()
    h.api("hid_bench")
    h.api("hid_bench_close 1")
    h.api("hid_sets")
    h.q(sid, 102, raw=u64(0), pid=True)
    h.api("hid_bench")
    h.api("hid_sets")
    h.api("hid_src none")


# ---- vibration -----------------------------------------------------------------------------------------------------------------------
def vibration(h):
    h.reset()
    sid, r = open_hid(h)
    h.api("hid_vib_sink 1")
    for hd in (handle(3, 0), handle(3, 0, 1), handle(4, 0x20), handle(4, 0x20, 1), handle(5, 7, 1), handle(6, 0x10), handle(7, 1, 3),
               handle(8, 2), handle(32, 3), handle(0, 0x11), 0xFFFFFFFF):
        h.q(sid, 201, raw=u32(hd) + f32(0.25, 100.0, 0.75, 200.0) + u32(0) + u64(0))
        h.q(sid, 202, raw=u32(hd) + u32(0) + u64(0))
        h.q(sid, 200, raw=u32(hd))
    for cmd in (206, 214):
        for nh, nv in ((0, 0), (1, 1), (2, 1), (1, 2), (16, 16), (17, 17), (20, 3), (3, 20)):
            hs = [handle(3, i % 8, i % 3) for i in range(nh)]
            vs = [x for i in range(nv) for x in (0.01 * i, 100.0 + i, 0.02 * i, 200.0 + i)]
            bufs = (("a", 4 * nh, u32(*hs)), ("a", 16 * nv, f32(*vs)))
            h.q(sid, cmd, raw=u64(0) + struct.pack("<H", 2), bufs=bufs)
        h.q(sid, cmd, raw=u64(0), bufs=(("x", 8, u32(handle(3, 0), handle(3, 0, 1))), ("x", 32, f32(1, 2, 3, 4, 5, 6, 7, 8))))
        h.q(sid, cmd, raw=u64(0), bufs=(("a", 6, u32(handle(3, 1)) + b"\x00\x00"), ("a", 20, f32(0.5, 1.5, 2.5, 3.5, 4.5))))
        for hd in (handle(3, 0), handle(3, 0, 1), handle(3, 1, 2), handle(3, 2)):
            h.q(sid, 202, raw=u32(hd) + u32(0) + u64(0))
    h.q(sid, 204, raw=b"\x00")
    h.q(sid, 201, raw=u32(handle(3, 0)) + f32(1.0, 1.0, 1.0, 1.0) + u32(0) + u64(0))   # not gated by PermitVibration
    h.q(sid, 204, raw=b"\x07")
    h.q(sid, 205)
    h.api("hid_vib_sink 0")
    h.q(sid, 201, raw=u32(handle(3, 0)) + f32(0.0, 160.0, 0.0, 320.0) + u32(0) + u64(0))
    h.q(sid, 202, raw=u32(handle(3, 0)) + u32(0) + u64(0))
    h.api("hid_vib_sink 1")


# ---- the Joy-Con 2 mouse ------------------------------------------------------------------------------------------------------------------
def nmouse_raw(w=1920, h=1080, size=0xC000, kind=0):
    return struct.pack("<BxxxiiI", kind, w, h, 1) + u64(0, size)


def mouse(h):
    h.reset()
    sid, r = open_hid(h)
    h.api("jc2_mode auto")
    h.api("jc2_request 0")
    h.api("jc2_space 0 0")
    h.api("jc2_state")
    h.q(sid, 3139, raw=nmouse_raw(), hc=("tc000",), pid=True)
    h.api("jc2_state")
    h.api("tmem")
    h.q(sid, 3138, raw=u32(6) + u32(0) + u64(0))
    h.q(sid, 3136, raw=u64(0) + bytes([1, 2, 4, 3, 0, 5, 0, 0]))
    h.q(sid, 3137, raw=u64(0) + bytes([1, 2, 4, 5, 3, 0, 0, 0]))
    for jh in (0x100, 0x101, 0x0, 0x1):
        h.q(sid, 3112, raw=u32(jh) + u32(0) + u64(0))
    h.q(sid, 3113, raw=u32(0x100) + u32(0) + u64(0))
    h.q(sid, 3140, raw=struct.pack("<ii", 100, 200) + u64(0, 0x4E4D0001))
    h.q(sid, 3142, raw=f32(0.0, 0.0, 1.0, 1.0) + u64(0))
    h.q(sid, 3143, raw=u64(0, 5))
    h.q(sid, 3150, raw=u32(1) + u32(0) + u64(0))
    h.sample()                                                    # no feed: the last pointer (0, 0)
    h.api("tmem")
    h.api("jc2_request 1")
    h.api("jc2_state")
    h.api("jc2_feed 0.5 0.25 1 0 1.0 1 0")
    h.api("jc2_state")
    h.sample()
    h.api("tmem")
    h.api("jc2_space 1280 720")
    h.api("jc2_feed 0.5 0.25 3 0.5 -1.25 1 0")
    h.api("jc2_feed 0.75 1.5 2 0.25 0.5 1 0")                     # the wheel adds up; v past 1 is clamped
    h.sample()
    h.api("tmem")
    for w, hh in ((20000, 10), (0, 720), (16384, 16384), (1, 1), (-5, 5)):
        h.api(f"jc2_space {w} {hh}")
        h.api("jc2_feed 1.0 1.0 0 0 0 1 0")
        h.sample()
        h.api("tmem")
    h.api("jc2_space 0 0")
    h.q(sid, 3132, raw=u32(1280, 720) + u64(0))
    h.api("jc2_feed -0.5 0.999 4 0 0 1 0")
    h.sample()
    h.api("tmem")
    h.api("jc2_feed 0.3 0.3 1 2 2 1 1")                           # captured by the camera: not active
    h.api("jc2_state")
    h.sample()
    h.api("tmem")
    h.api("jc2_feed 0.3 0.3 1 2 2 0 0")                           # the window lost its focus
    h.api("jc2_state")
    h.sample()
    h.api("jc2_feed 0.6 0.6 1 0 0 1 0")
    h.sample()
    h.api("sleep_ms 300")                                         # the feed went stale
    h.api("jc2_state")
    h.sample()
    h.api("tmem")
    for on, hd in ((1, 0x4E4D0002), (0, 0x4E4D0002), (0, 0x4E4D0001), (1, 0x12345678), (0, 0)):
        h.q(sid, 3141, raw=bytes([on]) + bytes(7) + u64(0, hd))
        h.api("tmem")
    for on in (1, 0, 0x80):
        h.q(sid, 3135, raw=bytes([on]) + bytes(7) + u64(0))
        h.api("tmem")
    for mode in ("on", "off", "auto"):
        h.api(f"jc2_mode {mode}")
        h.api("jc2_state")
    h.api("jc2_request 0")
    h.api("jc2_state")
    h.q(sid, 3133, raw=u64(0))
    # FinalizeNmouse, then the forms and the refusals
    h.q(sid, 3131, raw=u64(0))
    h.api("jc2_state")
    h.sample()
    h.api("tmem")
    h.q(sid, 3139, raw=nmouse_raw(size=0x8000), hc=("t8000",), pid=True)
    h.api("jc2_state")
    h.q(sid, 3139, raw=nmouse_raw(), pid=True)
    h.api("jc2_state")
    h.q(sid, 3139, raw=nmouse_raw(size=0x8E6F), hc=("tc000",), pid=True)
    h.q(sid, 3139, raw=nmouse_raw(size=0x8E70), hc=("t8e70",), pid=True)
    h.api("jc2_state")
    h.api("tmem")
    h.q(sid, 3131, raw=u64(0))
    h.q(sid, 3130, raw=nmouse_raw(2560, 1440, kind=2), hc=("tc000",), pid=True)
    h.q(sid, 3133, raw=u64(0))
    h.api("jc2_feed 0.5 0.5 0 0 0 1 0")
    h.sample()
    h.api("tmem")
    h.api("jc2_feed 0.5 0.5 0 0 0 0 0")                           # not focused: never live again, whatever the time
    h.q(sid, 3139, raw=nmouse_raw(0, -1), hc=("tc000",), pid=True)   # no range given: 1920 x 1080
    h.q(sid, 3133, raw=u64(0))
    h.api("jc2_mode off")                                         # listed stays: the zeroed block is still sampled
    h.q(sid, 3139, raw=nmouse_raw(), hc=("tc000",), pid=True)
    h.api("jc2_state")
    h.sample()
    h.api("tmem")
    h.q(sid, 3131, raw=u64(0))
    h.q(sid, 3139, raw=nmouse_raw(), hc=("tc000",), pid=True)     # off and not listed: nothing listed, nothing sampled
    h.api("jc2_state")
    h.sample()
    h.api("tmem")
    h.api("jc2_mode auto")
    # Jcms: player 1's two rings advance with empty states (no Joy-Con 2 over Bluetooth here)
    h.q(sid, 3110, raw=u64(0, 0x4000), hc=("t4000",), pid=True)
    h.api("tmem")
    h.sample()
    h.api("tmem")
    h.sample(18)
    h.api("tmem")
    h.q(sid, 3111, raw=u64(0))
    h.sample()
    h.api("tmem")
    h.q(sid, 3110, raw=u64(0, 0x2C7F), hc=("t4000",), pid=True)
    h.q(sid, 3110, raw=u64(0, 0x4000), hc=("t2000",), pid=True)
    h.q(sid, 3110, raw=u64(0, 0x2C80), hc=("t2c80",), pid=True)
    h.q(sid, 3110, raw=u64(0, 0x4000), pid=True)
    h.sample()
    h.api("tmem")
    # a reset forgets both blocks and the listing
    h.q(sid, 3139, raw=nmouse_raw(), hc=("tc000",), pid=True)
    h.q(sid, 3110, raw=u64(0, 0x4000), hc=("t4000",), pid=True)
    h.reset(keep=True)
    h.api("jc2_state")
    h.sample()
    h.api("tmem")


# ---- resets, objects, the sampler's state -------------------------------------------------------------------------------------------------
def lifetimes(h):
    s = h.s
    h.reset()
    sid, r = open_hid(h)
    h.q(sid, 103, raw=u64(0))
    h.q(sid, 100, raw=u32(0x3) + u64(0), pid=True)
    h.q(sid, 102, raw=u64(0), bufs=(("a", 4, u32(0)),), pid=True)
    h.q(sid, 120, raw=u64(0, 1), pid=True)
    h.q(sid, 106, raw=u32(0) + u64(0, 0), pid=True)
    h.sample(3)
    h.api("hid_live")
    h.reset(keep=True)                     # the same block re-initialised in place; new style events; the thread stopped
    h.api("hid_live")
    h.api("hid_sets")
    for c in (101, 121, 129, 1001, 68, 131, 205, 3133):
        h.q(sid, c, raw=u64(0), pid=True)
    h.q(r, 0, iface=RES)
    h.q(sid, 106, raw=u32(0) + u64(0, 0), pid=True)
    h.q(sid, 103, raw=u64(0))
    h.sample()
    h.s.lines.append("reset keep")         # background sampling back on, but no thread runs until the next applet resource
    h.api("hid_live")
    h.api("hid_bg 0")
    h.api("hid_live")
    s.lines.append(f"X {r}")
    h.api("hid_live")
    r2 = h.q(sid, 0, raw=u64(0), pid=True, nobj=1)[0]
    h.api("hid_live")
    h.q(r2, 0, iface=RES)
    s.sweep(r2, RES, (1, 2, 100))
    v = h.q(sid, 203, nobj=1)[0]
    s.sweep(v, VIB, (1, 2, 100))
    h.reset()
    h.api("hid_shm")
    h.sample()
    h.api("hid_shm")


# ---- fuzz and sweeps -------------------------------------------------------------------------------------------------------------------
def fuzz(h, variants):
    s = h.s
    h.reset()
    sid, r = open_hid(h)
    h.api("hid_src manual")
    h.pad(0, buttons=0x5, lx=100)
    h.pad(1, style="dual")
    h.api("hid_vib_sink 1")
    h.api("jc2_feed 0.5 0.5 1 1 1 0 0")    # not focused: the Joy-Con 2 mouse is never active here, whatever the time
    for cmd in OWN:
        if cmd in (3110, 3130, 3139):
            continue
        n = variants // 4 if cmd in IGNORED else variants
        s.fuzz(sid, HID, cmd, n=max(n, 4), nobj=1 if cmd in (0, 203) else 0)
        if cmd in (91, 100, 102, 103, 104, 122, 123, 124, 133):
            h.sample()
    # the transfer memory commands with lent memory of several sizes
    for cmd, sizes in ((3110, (0, 0x1000, 0x2C80, 0x4000)), (3139, (0, 0x8E70, 0xC000)), (3130, (0x8E70, 0x10000))):
        for i, size in enumerate(sizes):
            for raw in (u64(0, size), nmouse_raw(size=size), nmouse_raw(-1, 0x7FFFFFFF, size), bytes(8), b"\xff" * 32):
                h.q(sid, cmd, raw=raw, hc=(f"t{size:x}",), pid=bool(i & 1))
        s.fuzz(sid, HID, cmd, n=max(variants // 4, 4))
        h.sample()
        h.api("tmem")
    h.q(sid, 3139, raw=nmouse_raw(), hc=("tc000",), pid=True)
    for c in (3135, 3141):
        s.fuzz(sid, HID, c, n=max(variants // 4, 4))
        h.api("tmem")
    h.q(sid, 3141, raw=b"\x00" * 8 + u64(0, 0x4E4D0002))
    h.api("tmem")
    h.q(sid, 3131, raw=u64(0))
    h.q(sid, 3111, raw=u64(0))
    s.fuzz(r, RES, 0, n=max(variants // 2, 4))
    v = h.q(sid, 203, nobj=1)[0]
    s.fuzz(v, VIB, 0, n=max(variants // 4, 4))
    h.sample()
    h.api("hid_sets")
    h.api("hid_read")
    s.sweep(sid, HID, EXT)
    s.sweep(sid, HID, UNKNOWN)
    s.sweep(r, RES, (1, 2, 3, 0xFFFF))
    s.sweep(v, VIB, (1, 2, 3, 0xFFFF))
    h.api("hid_src none")
    h.reset()


def generate(s, variants, variant=None):
    h = Hid(s)
    s.config("input", "jc2_mouse", "auto")    # before anything reads it (the mode is read once per process)
    h.api("hid_bg 0")
    h.api("hid_vib_sink 1")
    parts = (("vectors", vectors), ("npads", npads), ("devices", devices), ("bench", bench), ("vibration", vibration),
             ("mouse", mouse), ("lifetimes", lifetimes))
    for name, fn in parts:
        if variant in (None, name):
            s.comment(name)
            fn(h)
    if variant in (None, "fuzz"):
        s.comment("fuzz")
        fuzz(h, variants)
    h.api("hid_vib_sink 0")
