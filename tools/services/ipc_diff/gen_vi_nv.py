# SPDX-License-Identifier: GPL-3.0-or-later
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
# Copyright (c) 2026 Chaos192
"""The display and GPU driver services of runtime/src/services/vi_nv.cpp: vi:u / vi:s / vi:m, their display services, the layer slots,
the binder's IGraphicBufferProducer codes (empty and SDK interface tokens, short parcels and replies, every binder id), the frame
history (api vi_present / vi_history), and nvdrv's device files (Open, every handled ioctl of each device and unhandled ones, the three
ioctl forms, size and direction checks, QueryEvent, Close, GetStatus) shared by nvdrv / nvdrv:a / nvdrv:s / nvdrv:t. The vsync event's
state and the history's times vary run to run and are masked by ipc_diff.py."""
import struct

DISP = "nn::visrv::sf::IApplicationDisplayService"
SYS = "nn::visrv::sf::ISystemDisplayService"
MGR = "nn::visrv::sf::IManagerDisplayService"
BINDER = "nns::hosbinder::IHOSBinderDriver"
NV = "nns::nvdrv::INvDrvServices"
SDK_NAME = "android.gui.IGraphicBufferProducer"


def name(s, n=0x40):
    b = s.encode()[:n]
    return b + bytes(n - len(b))


def token(kind):
    if kind == "empty":
        return struct.pack("<II", 0, 0) + bytes(4)
    if kind == "sdk":
        u = (SDK_NAME + "\0").encode("utf-16-le")
        u += bytes((-len(u)) % 4)
        return struct.pack("<II", 0x100, len(SDK_NAME)) + u
    if kind == "long":                                  # length >= 0x100: nothing skipped
        return struct.pack("<II", 0, 0xFFFFFFFF)
    return b""


def parcel(words, tok="empty", data_off=0x10, pad=0, hdr_off=None):
    """hdr_off: the header names this data offset while the data stays at data_off (an offset past the buffer)."""
    data = token(tok) + b"".join(struct.pack("<I", w & 0xFFFFFFFF) for w in words)
    off = data_off if hdr_off is None else hdr_off
    hdr = struct.pack("<IIII", len(data), off, 0, (off + len(data)) & 0xFFFFFFFF)
    return hdr + bytes(max(0, data_off - 0x10)) + data + bytes(pad)


def transact(s, b, binder, code, words=(), tok="empty", reply=0x100, cmd=0, **kw):
    p = parcel(words, tok, **kw)
    bufs = [("a", len(p), p)]
    if reply:
        bufs.append(("b", reply, None))
    s.q(b, BINDER, cmd, raw=struct.pack("<iII", binder, code, 0), bufs=tuple(bufs))


def vi_part(s, variants):
    s.reset()
    roots = {}
    for svc, iface, cmds in (("vi:u", "nn::visrv::sf::IApplicationRootService", (0, 1)), ("vi:s", "nn::visrv::sf::ISystemRootService", (1, 3)),
                             ("vi:m", "nn::visrv::sf::IManagerRootService", (2, 3))):
        r = s.open(svc)
        roots[svc] = (r, iface)
        for c in cmds:
            s.q(r, iface, c, raw=struct.pack("<I", 0), nobj=1)
        s.sweep(r, iface, (4, 5, 100, 101, 102, 103))
    r, iface = roots["vi:u"]
    d = s.q(r, iface, 0, raw=struct.pack("<I", 0), nobj=1)[0]
    b = s.q(d, DISP, 100, nobj=1)[0]
    sysd = s.q(d, DISP, 101, nobj=1)[0]
    mgr = s.q(d, DISP, 102, nobj=1)[0]
    b2 = s.q(d, DISP, 103, nobj=1)[0]
    # displays
    for bv in ((("b", 0x60, None),), (("b", 0x20, None),), (("b", 0x100, None),), (), (("c", 0x60, None),)):
        s.q(d, DISP, 1000, bufs=bv)
    for raw in (name("Default"), name("External"), name("Edid"), name("Internal"), name("Null"), name("default"), name("Nope"), b"",
                name("Default\0junk"), b"A" * 0x40, b"Null", b"Default", name("Internal") + b"\xff" * 8, name("Null", 8)):
        s.q(d, DISP, 1010, raw=raw)
    for c in (1011, 1020, 1101, 1102):
        s.q(d, DISP, c, raw=struct.pack("<Q", 0))
    # layers: 8 slots, the shared id counter
    for i, bv in enumerate(((("b", 0x100, None),), (("b", 0x3C, None),), (("b", 0x20, None),), (), (("c", 0x100, None),),
                            (("b", 0x100, None),), (("b", 0x100, None),), (("b", 0x100, None),), (("b", 0x100, None),), (("b", 0x100, None),))):
        s.q(d, DISP, 2030, raw=struct.pack("<IIQ", i, 0, 0), bufs=bv)
    s.q(mgr, MGR, 2010, raw=bytes(16))
    s.q(d, DISP, 2020, raw=name("Default") + struct.pack("<QQ", 1, 0), bufs=(("b", 0x100, None),))   # all slots used
    for lid in (0x10003, 0x10005, 0x99999):
        s.q(d, DISP, 2021, raw=struct.pack("<Q", lid))
    for raw in (name("Default") + struct.pack("<QQ", 1, 0), name("Default") + struct.pack("<QQ", 1, 0), name("Nope") + struct.pack("<QQ", 2, 0),
                name("Null") + struct.pack("<QQ", 2, 7), name("Default") + struct.pack("<QQ", 3, 0), name("Default")):
        s.q(d, DISP, 2020, raw=raw, bufs=(("b", 0x100, None),), pid=True)
    s.q(d, DISP, 2031, raw=struct.pack("<Q", 1))
    s.q(d, DISP, 2030, raw=bytes(16), bufs=(("b", 0x100, None),))
    for m in (0, 1, 2, 3, 4, 5, 6, 0xFFFFFFFF):
        s.q(d, DISP, 2102, raw=struct.pack("<I", m))
    s.q(d, DISP, 2102)
    s.q(d, DISP, 2101, raw=struct.pack("<IQ", 2, 0x10000))
    for w, h in ((1280, 720), (1920, 1080), (0, 0), (1, 1), (-1, -1), (-1, 1), (-0x10000, 4), (0x7FFFFFFFFFFFFFFF, 3), (1 << 40, 1 << 30),
                 (0x20000, -1), (3, 0x2AAA)):
        s.q(d, DISP, 2460, raw=struct.pack("<qq", w, h))
    s.q(d, DISP, 2460)
    for c in (5202, 5203, 5202):
        s.q(d, DISP, c, raw=struct.pack("<Q", 0))
    s.q(d, DISP, 5202, raw=struct.pack("<Q", 7))
    s.api("vi_vsync")
    for c in (1200, 1202, 1203, 3200):
        s.q(sysd, SYS, c)
    for c in (2010, 2300, 2302, 2303, 2300):
        s.q(mgr, MGR, c, raw=bytes(16))
    s.sweep(d, DISP, (0, 1, 1001, 1012, 1103, 2022, 2032, 2103, 2450, 5204, 7000))
    s.sweep(sysd, SYS, (1201, 2204, 2205, 2207, 2312, 3201, 8250))
    s.sweep(mgr, MGR, (1102, 2011, 2012, 2301, 6000, 6002))
    # the binder (slots 1..8 are open, slot 1 holds layer 1, slot 2 was reopened)
    transact(s, b, 1, 11)                               # Disconnect before Connect
    transact(s, b, 1, 10, (0, 2, 0))
    transact(s, b, 1, 10, (0, 2, 0), tok="sdk")
    transact(s, b, 1, 11)
    transact(s, b, 1, 11, tok="sdk")
    transact(s, b, 1, 3, (1, 0x500, 0x2D0, 1, 0xB00))
    transact(s, b, 1, 1, (0,))
    transact(s, b, 1, 7, (0,) * 20)
    for n in (3, 0, -1, 1, 64, 0x7FFFFFFF):
        transact(s, b, 1, 2, (n,))
        transact(s, b, 1, 9, (12,))
    for what in list(range(16)) + [0xFFFFFFFF, 0x100]:
        transact(s, b, 2, 9, (what,))
        transact(s, b, 2, 9, (what,), tok="sdk")
    for code in range(0, 40):
        transact(s, b, 3, code, (1, 2, 3, 4))
    s.api("vi_frames")
    s.api("vi_history 4")
    for want in (0, -1, 1, 8, 9):
        transact(s, b, 1, 17, (want,))
    for _ in range(3):
        s.api("vi_present")
    s.api("vi_frames")
    for want in (1, 2, 3, 8, 100, 0):
        transact(s, b, 1, 17, (want,))
        transact(s, b, 1, 17, (want,), tok="sdk", reply=0x200)
    for _ in range(9):
        s.api("vi_present")
    for m in (0, -1, 1, 3, 8, 9, 12):
        s.api(f"vi_history {m}")
    transact(s, b, 1, 17, (8,), reply=0x200)
    transact(s, b, 1, 17, (8,), reply=0x40)
    # parcel shapes
    transact(s, b, 1, 9, (0,), tok="long")
    transact(s, b, 1, 9, (1,), data_off=0x20)
    transact(s, b, 1, 9, (1,), hdr_off=0x10000)
    transact(s, b, 1, 9, (1,), hdr_off=0x1C)
    transact(s, b, 1, 9, (1,), hdr_off=0xFFFFFFF0)
    transact(s, b, 1, 10, (0, 2, 0), hdr_off=0xFFFFC, pad=0x10000)
    transact(s, b, 1, 2, (), tok=None)
    transact(s, b, 1, 9, (2,), pad=0x20000)
    s.q(b, BINDER, 0, raw=struct.pack("<iII", 1, 10, 0), bufs=(("a", 15, bytes(15)), ("b", 0x100, None)))
    s.q(b, BINDER, 0, raw=struct.pack("<iII", 1, 10, 0), bufs=(("a", 16, parcel(())[:16]), ("b", 0x100, None)))
    s.q(b, BINDER, 0, raw=struct.pack("<iII", 1, 10, 0), bufs=(("b", 0x100, None),))
    s.q(b, BINDER, 0, raw=struct.pack("<iII", 1, 10, 0), bufs=(("x", 0x40, parcel((0, 2, 0))), ("c", 0x100, None)))
    transact(s, b, 1, 10, (0, 2, 0), reply=8)
    transact(s, b, 1, 10, (0, 2, 0), reply=0)
    transact(s, b, 1, 3, (), reply=0x18)
    for binder in (0, 9, -1, 0x7FFFFFFF, 2, 4, 8):
        transact(s, b, binder, 10, (0, 2, 0))
        transact(s, b2, binder, 11, cmd=3)
        s.q(b, BINDER, 2, raw=struct.pack("<iI", binder, 0xF))
    s.q(b, BINDER, 1, raw=struct.pack("<iii", 1, 1, 0))
    s.q(b, BINDER, 2, raw=struct.pack("<iI", 1, 0xF))
    s.q(b, BINDER, 2, raw=struct.pack("<iI", 1, 0xF))
    s.q(d, DISP, 2021, raw=struct.pack("<Q", 0x10004))   # slot of layer 0x10004 closes: its binder answers not found
    transact(s, b, 6, 10, (0, 2, 0))
    s.q(b, BINDER, 2, raw=struct.pack("<iI", 6, 0xF))
    s.sweep(b, BINDER, (4, 5, 6, 100))
    # fuzz every command
    for c in (100, 101, 102, 103, 1000, 1010, 1011, 1020, 1101, 1102, 2020, 2021, 2030, 2031, 2101, 2102, 2460, 5202, 5203):
        s.fuzz(d, DISP, c, n=variants, nobj=1 if c in (100, 101, 102, 103) else 0)
    for c in (1200, 1202, 1203, 3200):
        s.fuzz(sysd, SYS, c, n=variants // 2)
    for c in (2010, 2300, 2302, 2303):
        s.fuzz(mgr, MGR, c, n=variants // 2)
    for c in (0, 1, 2, 3):
        s.fuzz(b, BINDER, c, n=variants)
    # handheld: every display size follows the operation mode
    s.config("am", "operation_mode", "handheld")
    s.reset()
    r = s.open("vi:m")
    d = s.q(r, "nn::visrv::sf::IManagerRootService", 2, raw=struct.pack("<I", 0), nobj=1)[0]
    b = s.q(d, DISP, 100, nobj=1)[0]
    sysd = s.q(d, DISP, 101, nobj=1)[0]
    s.q(d, DISP, 1102)
    s.q(d, DISP, 1000, bufs=(("b", 0x60, None),))
    s.q(sysd, SYS, 1203)
    s.q(sysd, SYS, 3200)
    s.q(d, DISP, 2030, raw=bytes(16), bufs=(("b", 0x100, None),))
    transact(s, b, 1, 10, (0, 2, 0))
    for what in (0, 1, 6, 7):
        transact(s, b, 1, 9, (what,))
    s.config("am", "operation_mode", "docked")
    s.reset()


# ---- nvdrv ------------------------------------------------------------------------------------------------------------------------------
def word(nr, typ, size, cin, cout):
    return (nr & 0xFF) | ((typ & 0xFF) << 8) | ((size & 0x3FFF) << 16) | (0x40000000 if cin else 0) | (0x80000000 if cout else 0)


class Nv:
    def __init__(self, s, sid):
        self.s, self.sid = s, sid

    def open(self, path, nul=True, size=None):
        p = path.encode() + (b"\0" if nul else b"")
        n = size if size is not None else len(p)
        self.s.q(self.sid, NV, 0, bufs=(("a", n, p),))

    def ioctl(self, fd, cmd, data=b"", out=None, form=1, inline=0, in_size=None):
        size = (cmd >> 16) & 0x3FFF
        bufs = []
        n_in = in_size if in_size is not None else (size if cmd & 0x40000000 else 0)
        if n_in or cmd & 0x40000000:
            d = (data + bytes(n_in))[:n_in]
            bufs.append(("a", n_in, d))
        if form == 2:
            bufs.append(("a", 0x10, bytes(range(16))))
        n_out = out if out is not None else (size if cmd & 0x80000000 else 0)
        if n_out or cmd & 0x80000000:
            bufs.append(("b", n_out, None))
        if form == 3:
            if not (n_out or cmd & 0x80000000):
                bufs.append(("b", 0, None))
            bufs.append(("b", inline, None))
        self.s.q(self.sid, NV, {1: 1, 2: 11, 3: 12}[form], raw=struct.pack("<iI", fd, cmd & 0xFFFFFFFF), bufs=tuple(bufs))


def u32s(*w):
    return b"".join(struct.pack("<I", x & 0xFFFFFFFF) for x in w)


# The handled (type, nr) pairs of each device with their real command words
NVMAP = [0xC0080101, 0xC0080103, 0xC0200104, 0xC0180105, 0xC00C0109, 0xC008010E]
CTRL = [0xC0080014, 0x40040015, 0xC00C0016, 0xC0100019, 0xC008001A, 0xC183001B, 0xC004001C, 0xC010001D, 0xC010001E, 0xC004001F,
        0xC0040020, 0xC0080021]
CTRL_GPU = [0x80044701, 0x80284702, 0x402C4703, 0xC0B04705, 0xC0184706, 0xC010471C]
AS_GPU = [0x40044101, 0xC0184102, 0xC0104103, 0xC0084105, 0xC0284106, 0xC0404108, 0x40284109]
CHANNEL = [0x40044801, 0xC0184808, 0xC018481B, 0xC0104809, 0x80804816, 0xC020481A, 0x40084714]


def nv_part(s, variants):
    s.reset()
    sid = s.open("nvdrv")
    nv = Nv(s, sid)
    # before Initialize
    nv.open("/dev/nvmap")
    s.q(sid, NV, 6)
    nv.ioctl(1, 0xC0080101, u32s(0x1000))
    s.q(sid, NV, 2, raw=struct.pack("<i", 1))
    s.q(sid, NV, 4, raw=struct.pack("<iI", 1, 1))
    s.q(sid, NV, 3, raw=struct.pack("<I", 0x300000), hc=("p", "t1000"))
    s.q(sid, NV, 3)
    s.q(sid, NV, 6)
    for path in ("/dev/nvmap", "/dev/nvhost-ctrl", "/dev/nvhost-ctrl-gpu", "/dev/nvhost-as-gpu", "/dev/nvhost-gpu", "/dev/nvhost-nvdec",
                 "/dev/nvhost-nvjpg", "/dev/nvhost-vic", "/dev/nvdisp-ctrl", "/dev/nvsched-ctrl-gpu", "/dev/nvhost-dbg-gpu",
                 "/dev/nvhost-prof-gpu", "/dev/nope", "/dev/nvhost-msenc", "/dev/nvsched-ctrl", "/dev/nvdisp-disp0", "", "/dev/NVMAP",
                 "/dev/nvmap/"):
        nv.open(path)
    nv.open("/dev/nvmapXX", size=10)                    # cut to the buffer: /dev/nvmap
    nv.open("/dev/nvhost-gpu", nul=False)
    s.q(sid, NV, 0)
    # fds: 1 nvmap, 2 ctrl, 3 ctrl-gpu, 4 as-gpu, 5 gpu, 6 nvdec, 7 nvjpg, 8 vic, 9 disp-ctrl, 10 sched, 11 nvmap(cut), 12 gpu
    # nvmap
    nv.ioctl(1, 0xC0080101, u32s(0x1234, 0))
    nv.ioctl(1, 0xC0080101, u32s(0, 0))
    nv.ioctl(1, 0xC0080101, u32s(0xFFFFFFFF, 0))
    nv.ioctl(1, 0xC0080101, u32s(0x1000, 0x77))
    nv.ioctl(1, 0xC0080101, u32s(0x123456, 0))
    for p in range(0, 9):
        nv.ioctl(1, 0xC00C0109, u32s(1, p, 0x55))
    nv.ioctl(1, 0xC00C0109, u32s(77, 1, 0))
    for align in (0x20000, 0x3000, 0, 1, 0x800, 0x1000, 0x80000000, 0xFFFFFFFF):
        nv.ioctl(1, 0xC0200104, u32s(1, 2, 3, align, 0x0507, 0, 0x123000, 0))
    nv.ioctl(1, 0xC0200104, u32s(4, 5, 6, 0x10000, 0xFE, 0, 0x777000, 0))
    nv.ioctl(1, 0xC0200104, u32s(9, 0, 0, 0x1000))
    for p in (1, 2, 4, 5, 6):
        nv.ioctl(1, 0xC00C0109, u32s(1, p, 0))
        nv.ioctl(1, 0xC00C0109, u32s(4, p, 0))
    nv.ioctl(1, 0xC008010E, u32s(0, 1))
    nv.ioctl(1, 0xC008010E, u32s(0, 50))
    nv.ioctl(1, 0xC0080103, u32s(1, 0))
    nv.ioctl(1, 0xC0080103, u32s(50, 0))
    for _ in range(4):
        nv.ioctl(1, 0xC0180105, u32s(1, 0, 0, 0, 0, 0))
    nv.ioctl(1, 0xC0180105, u32s(2, 9, 9, 9, 9, 9))
    for nr in range(0, 0x18):
        nv.ioctl(1, word(nr, 0x01, 0x20, True, True), u32s(3, 1, 2, 3, 4, 5, 6, 7))
    nv.ioctl(1, 0xC0080101, u32s(0x1000), in_size=4)   # input smaller than the size: InvalidSize
    nv.ioctl(1, 0xC0080101, u32s(0x1000), out=4)
    nv.ioctl(1, 0xC0080101, u32s(0x1000), in_size=0x40, out=0x40)
    nv.ioctl(1, word(0x01, 0x01, 4, True, True), u32s(0x1000))       # handle does not fit the block
    nv.ioctl(1, word(0x01, 0x01, 0, False, False))
    nv.ioctl(1, word(0x01, 0x01, 8, True, False), u32s(0x1000))
    nv.ioctl(1, word(0x01, 0x01, 8, False, True), u32s(0x1000))
    nv.ioctl(1, word(0x09, 0x01, 0xA, True, True), u32s(3, 1, 0))
    nv.ioctl(1, 0xC0080014, u32s(1, 0))                 # a ctrl ioctl on nvmap
    # ctrl
    for sp in (0, 191, 192, 0xFFFFFFFF):
        for c in CTRL:
            nv.ioctl(2, c, u32s(sp, 9, 8, 7, 6, 5))
    for nr in range(0x10, 0x26):
        nv.ioctl(2, word(nr, 0x00, 0x18, True, True), u32s(5, 1, 2, 3, 4, 5))
    nv.ioctl(2, 0xC0080014, u32s(1), form=2)
    nv.ioctl(2, 0xC0080014, u32s(1), form=3, inline=8)
    # ctrl-gpu
    for c in CTRL_GPU:
        nv.ioctl(3, c, u32s(1, 2, 3, 4, 5, 6))
        nv.ioctl(3, c, u32s(1, 2, 3, 4, 5, 6), form=3, inline=0xA0)
        nv.ioctl(3, c, u32s(0, 2, 3, 4, 5, 6), form=3, inline=4)
    nv.ioctl(3, 0xC0B04705, u32s(0xB0, 0, 0x12345678, 0x9ABC), form=3, inline=0x100)
    nv.ioctl(3, 0xC0B04705, u32s(0xB0), form=3, inline=0x9F)
    nv.ioctl(3, word(0x05, 0x47, 0x40, True, True), u32s(0x40), form=3, inline=0xA0)
    nv.ioctl(3, word(0x05, 0x47, 0x08, True, True), u32s(0x40))
    nv.ioctl(3, word(0x05, 0x47, 0x04, True, True), u32s(0x40))
    nv.ioctl(3, 0xC0184706, u32s(1, 0, 0, 0, 0, 0), form=3, inline=4)
    nv.ioctl(3, 0xC0184706, u32s(1, 0, 0, 0, 0, 0), form=3, inline=3)
    nv.ioctl(3, 0xC0184706, u32s(1, 0, 0, 0, 0, 0), form=2)
    nv.ioctl(3, word(0x06, 0x47, 0x10, True, True), u32s(1), form=3, inline=4)
    for nr in range(0, 0x20):
        nv.ioctl(3, word(nr, 0x47, 0x28, True, True), u32s(*range(1, 11)))
    nv.ioctl(3, 0x80084712, out=8)
    nv.ioctl(3, 0x40084714, u32s(1, 2))
    # as-gpu
    nv.ioctl(4, 0x40284109, u32s(0, 4, 0x10000, 0, 0, 0, 0, 0, 0, 0))
    nv.ioctl(4, 0x40284109, u32s(0, 4, 0, 0))
    nv.ioctl(4, 0x40044101, u32s(5))
    for pages, page, flags, off in ((0x10, 0x10000, 0, 0), (0x10, 0x10000, 0, 0), (0x10, 0x10000, 1, 0x400000000), (1, 0x1000, 0, 0),
                                    (0x20, 0x1000, 0, 0x10000), (2, 0x1000, 0, 0x3000), (1, 0x10000, 0, 0x30000), (0, 0x1000, 0, 0),
                                    (1, 0, 0, 0), (1, 0x1000, 1, 0x200000000), (1, 0x1000, 1, 0x200000000), (1, 0x1000, 1, 0x200000800),
                                    (1, 0x1000, 1, 0x3FFFFF000), (2, 0x1000, 1, 0x3FFFFF000), (1, 0x1000, 1, 0xFFFFF000),
                                    (1, 0x10000, 1, 0x1FFFFF0000), (1, 0x10000, 1, 0x2000000000), (0xFFFFFFFF, 0x10000, 0, 0),
                                    (0x1C0000, 0x10000, 0, 0), (0x8000, 0x20000, 0, 0), (3, 0x1000, 0, 0x5000), (1, 0x1000, 0, 0x100000000)):
        nv.ioctl(4, 0xC0184102, u32s(pages, page, flags, 0) + struct.pack("<Q", off))
    for h, flags, kind, page, boff, msize, off in ((1, 0, 0, 0, 0, 0, 0), (1, 0, 0, 0, 0, 0, 0), (99, 0, 0, 0, 0, 0, 0),
                                                   (4, 0, 0, 0, 0, 0, 0), (4, 0, 0x7, 0x10000, 0, 0x20000, 0), (3, 0, 0, 0x1000, 0, 0x3000, 0),
                                                   (3, 1, 0, 0x1000, 0, 0x1000, 0x300000000), (3, 1, 0, 0x1000, 0, 0x1000, 0x300000000),
                                                   (3, 0x101, 0, 0, 0x2000, 0x1000, 0x300001000), (3, 1, 0, 0x10000, 0, 0x10000, 0x400010000),
                                                   (3, 1, 0, 0x1000, 0, 0x1000, 0x50), (3, 0, 0, 0x3000, 0, 0x1000, 0),
                                                   (3, 0, 0, 0x1000, 0, 0x400000000, 0)):
        nv.ioctl(4, 0xC0284106, u32s(flags, kind, h, page) + struct.pack("<QQQ", boff, msize, off))
    for bsize in (0x30, 0x18, 0, 0x2F, 0x1000, 0xFFFFFFFF):
        nv.ioctl(4, 0xC0404108, struct.pack("<QII", 0x1234, bsize, 0x55))
        nv.ioctl(4, 0xC0404108, struct.pack("<QII", 0x1234, bsize, 0x55), form=3, inline=0x40)
    for off in (0x400000000, 0x400000000, 0x100000000, 0x300000000, 0x12345):
        nv.ioctl(4, 0xC0104103, struct.pack("<QII", off, 1, 0x1000))
    for off in (0x400100000, 0x400100000, 0x77):
        nv.ioctl(4, 0xC0084105, struct.pack("<Q", off))
    nv.ioctl(4, 0xC0184102, u32s(0x10, 0x10000, 0, 0) + struct.pack("<Q", 0))
    nv.ioctl(4, 0xC0284106, u32s(0, 0, 1, 0) + struct.pack("<QQQ", 0, 0, 0))
    for nr in range(0, 0x18):
        nv.ioctl(4, word(nr, 0x41, 0x28, True, True), u32s(1, 0x1000, 0, 0, 0, 0, 0, 0, 0, 0))
    # channels
    for fd in (5, 12, 5):
        nv.ioctl(fd, 0xC020481A, u32s(0x800, 0, 0, 0xAA, 0xBB, 0, 0, 0))
    nv.ioctl(5, 0xC0104809, u32s(0xB197, 0, 0, 0))
    nv.ioctl(5, 0xC0104809, u32s(0xB197, 0, 0, 0), in_size=0x40)
    nv.ioctl(5, 0x80804816, out=0x80)
    nv.ioctl(5, 0xC0804816, bytes(range(0x80)))
    nv.ioctl(5, 0xC0184808, u32s(0x1000, 0, 1, 0, 7, 9))
    nv.ioctl(5, 0xC018481B, u32s(0x1000, 0, 1, 0, 7, 9), form=2)
    nv.ioctl(5, 0x40084714, u32s(0xDEAD, 0xBEEF))
    nv.ioctl(5, 0x80084715, out=8)
    nv.ioctl(5, 0x40044801, u32s(1))
    for nr in range(0, 0x20):
        nv.ioctl(5, word(nr, 0x48, 0x20, True, True), u32s(*range(1, 9)))
    for typ in (0x00, 0x01, 0x41, 0x47):
        nv.ioctl(5, word(0x14, typ, 0x10, True, True), u32s(1, 2, 3, 4))
    # other devices: nothing handled
    for fd in (6, 7, 8, 9, 10):
        for c in (0xC0080101, 0x80084712, 0xC0B04705, 0xC020481A, 0x00000001):
            nv.ioctl(fd, c, u32s(1, 2, 3, 4) * 0x10)
    # bad fds and forms
    for fd in (0, 99, -1, -5, 0x7FFFFFFF):
        nv.ioctl(fd, 0xC0080101, u32s(0x1000))
    nv.ioctl(1, 0xC0080101, u32s(0x1000), form=2)
    nv.ioctl(1, 0xC0080101, u32s(0x1000), form=3, inline=8)
    # QueryEvent
    for fd, ids in ((2, (0x10000005, 0x40, 0x1000FFFF, 5, 63, 64, 0x1000003F, 0x10000040, 0x2000003F, 0xFFFFFF05, 0x10000005)),
                    (3, (0, 1, 2, 3, 2)), (5, (0, 1, 2, 3, 4, 3)), (1, (0, 1)), (4, (1,)), (6, (1,)), (12, (1, 2)), (99, (1,)), (-1, (1,))):
        for i in ids:
            s.q(sid, NV, 4, raw=struct.pack("<iI", fd, i))
    # the other names share the state
    for other in ("nvdrv:a", "nvdrv:s", "nvdrv:t"):
        o = s.open(other)
        s.q(o, NV, 6)
        s.q(o, NV, 0, bufs=(("a", 11, b"/dev/nvmap\0"),))
        s.q(o, NV, 1, raw=struct.pack("<iI", 1, 0xC00C0109), bufs=(("a", 12, u32s(2, 1, 0)), ("b", 12, None)))
    for c in (7, 8, 9, 13, 5, 10, 14, 15):
        s.q(sid, NV, c, raw=struct.pack("<Q", 0x55), pid=True)
    s.q(sid, NV, 6)
    # Close
    for fd in (1, 1, -1, 99, 12, 4):
        s.q(sid, NV, 2, raw=struct.pack("<i", fd))
    nv.ioctl(4, 0xC0184102, u32s(1, 0x1000, 0, 0) + struct.pack("<Q", 0))
    nv.ioctl(11, 0xC00C0109, u32s(3, 1, 0))         # objects outlive the fd that made them
    s.q(sid, NV, 4, raw=struct.pack("<iI", 12, 1))
    nv.open("/dev/nvhost-as-gpu")
    nv.ioctl(13, 0xC0184102, u32s(0x10, 0x10000, 0, 0) + struct.pack("<Q", 0))
    # fuzz every command
    for c in (0, 1, 2, 3, 4, 6, 7, 8, 9, 11, 12, 13):
        s.fuzz(sid, NV, c, n=variants)
    # reset: not initialised again, fds from 1
    s.reset()
    sid = s.open("nvdrv:s")
    nv = Nv(s, sid)
    nv.open("/dev/nvmap")
    s.q(sid, NV, 3)
    nv.open("/dev/nvmap")
    nv.ioctl(1, 0xC0080101, u32s(0x1000))
    nv.open("/dev/nvhost-gpu")
    nv.ioctl(2, 0xC020481A, u32s(0x800))
    s.reset()


def generate(s, variants, variant=None):
    s.api = lambda what: s.lines.append("api " + what)
    if variant in (None, "vi"):
        vi_part(s, variants)
    if variant in (None, "nv"):
        nv_part(s, variants)
