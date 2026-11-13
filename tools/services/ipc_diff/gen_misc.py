# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
# Copyright (c) 2026 Chaos192
"""The small services of runtime/src/services/misc.cpp: csrng, mm:u, hwopus (managers, the silent decoder, multistream checks), aoc:u
(with two add-ons), caps:u, mii:u / mii:e, irs, nfc:user, nfc:mf:u, nfp:user, olsc:u, ectx:aw, pcm, banana, htc, htc:tenv, htcs, stpl:u.
Every listed command with argument and buffer variants plus an id sweep per interface. Random answers (csrng, BuildRandom's id) vary
run to run and are masked by ipc_diff.py."""
import struct
from pathlib import Path

LOCAL = struct.pack("<QQ", 1, 0x6E78727475736572)


def objects(s, sid, iface, cmd, raw=b"", n=1, bufs=()):
    return s.q(sid, iface, cmd, raw=raw, bufs=bufs, nobj=n)


def every(s, sid, iface, ids, variants, sweep=()):
    for cmd in ids:
        s.fuzz(sid, iface, cmd, n=variants)
    s.sweep(sid, iface, list(sweep))


def generate(s, variants):
    aux = Path(s.aux_dir)
    for i in (1, 2):
        (aux / "aoc" / f"{0x1000 + i:016X}" / "romfs").mkdir(parents=True, exist_ok=True)
    s.config("fs", "aoc_dir", str(aux / "aoc"))
    s.reset()
    # csrng (random bytes: masked)
    c = s.open("csrng")
    for size in (0, 16, 0x100):
        s.q(c, "csrng", 0, bufs=((("b", size, None),) if size else ()))
    # mm:u
    m = s.open("mm:u")
    for cmd, raw in ((4, struct.pack("<III", 5, 0, 0)), (4, b""), (6, struct.pack("<III", 1, 10000000, 0xFFFFFFFF)), (7, struct.pack("<I", 1)),
                     (0, struct.pack("<III", 3, 0, 0)), (4, struct.pack("<I", 3)), (3, struct.pack("<I", 3)), (2, struct.pack("<II", 3, 10)),
                     (3, struct.pack("<I", 3)), (7, struct.pack("<I", 3)), (5, struct.pack("<I", 1)), (7, struct.pack("<I", 1)),
                     (1, struct.pack("<I", 3)), (3, struct.pack("<I", 3))):
        s.q(m, "mmnv::IRequest", cmd, raw=raw)
    for _ in range(18):
        s.q(m, "mmnv::IRequest", 4, raw=struct.pack("<I", 7))
    s.q(m, "mmnv::IRequest", 7, raw=struct.pack("<I", 17))
    every(s, m, "mmnv::IRequest", range(8), variants // 4, range(0, 12))
    s.reset()
    m = s.open("mm:u")
    s.q(m, "mmnv::IRequest", 4, raw=struct.pack("<I", 1))
    # hwopus
    h = s.open("hwopus")
    for cmd in (1, 3, 5, 7, 8, 9):
        s.q(h, "IHardwareOpusDecoderManager", cmd)
    pkt = bytes.fromhex("0000000300000000f8fffe")
    for rate, ch in ((44100, 2), (48000, 3), (0, 0), (22050, 1), (48000, 9), (96000, 2)):
        for cmd in (0, 4):
            d = objects(s, h, "IHardwareOpusDecoderManager", cmd, raw=struct.pack("<III", rate, ch, 0x10000) + bytes(8))[0]
            for dc in range(10):
                for bv in ((("a", len(pkt), pkt), ("b", 0x1000, None)), (("a", len(pkt), pkt), ("b", 0x100, None)), (("a", len(pkt), pkt),),
                           (("a", 9, bytes.fromhex("0000ffff00000000f8")), ("b", 0x100, None)), (("x", 4, b"\0\0\0\0"),),
                           (("a", 12, bytes.fromhex("00000004000000000b0182")), ("b", 0x2000, None))):
                    s.q(d, "IHardwareOpusDecoder", dc, bufs=bv)
            s.sweep(d, "IHardwareOpusDecoder", range(10, 14))
    for cmd, need, at in ((2, 0x110, 0x10), (6, 0x118, 0x18)):
        for size, rate, ch, streams, coupled, mapping in (
                (need - 1, 48000, 2, 1, 1, [0, 1]), (need, 44100, 2, 1, 1, [0, 1]), (need, 48000, 0, 1, 1, [0, 1]), (need, 48000, 256, 1, 1, [0, 1]),
                (need, 48000, 4, 1, 2, [0, 1, 2, 3]), (need, 48000, 2, 1, 1, [0, 2]), (need, 48000, 2, 1, 1, [0, 1]), (need, 48000, 2, 0, 0, [0, 1]),
                (need, 16000, 6, 4, 2, [0, 4, 1, 2, 3, 5]), (need, 48000, 2, 1, 1, [255, 1]), (need + 0x20, 24000, 1, 1, 0, [0])):
            body = struct.pack("<IIII", rate, ch, streams, coupled) + bytes(at - 0x10) + bytes(mapping) + b"\x00" * 256
            objects(s, h, "IHardwareOpusDecoderManager", cmd, bufs=(("x", size, body[:size]),))
    every(s, h, "IHardwareOpusDecoderManager", [1, 3, 5, 7, 8, 9], variants // 4, range(10, 16))
    # aoc:u
    a = s.open("aoc:u")
    for cmd, raw, bv in ((0, struct.pack("<Q", 0), ()), (0, struct.pack("<Q", 0x1000), ()), (1, struct.pack("<QII", 0, 0, 8), (("b", 32, None),)),
                         (1, struct.pack("<QII", 5, 0, 8), (("b", 32, None),)), (3, struct.pack("<II", 0, 1), (("b", 32, None),)),
                         (3, struct.pack("<II", 5, 8), (("b", 32, None),)), (3, struct.pack("<II", 0, 8), ()), (3, struct.pack("<II", 1, 8), (("b", 32, None),)),
                         (3, struct.pack("<II", 0, 8), (("b", 6, None),)), (2, b"", ()), (4, struct.pack("<Q", 0x0100000000010040), ()), (5, b"", ()),
                         (9, b"", ()), (13, b"", ())):
        s.q(a, "IAddOnContentManager", cmd, raw=raw, bufs=bv)
    for cmd in (6, 7, 8, 10, 11, 12, 50, 200):
        s.q(a, "IAddOnContentManager", cmd)
    for cmd in (100, 101):
        p = objects(s, a, "IAddOnContentManager", cmd)[0]
        for pc in (0, 1, 2, 3, 4, 5):
            s.q(p, "IPurchaseEventManager", pc)
    cs = objects(s, a, "IAddOnContentManager", 110)[0]
    s.sweep(cs, "IContentsServiceManager", range(0, 3))
    every(s, a, "IAddOnContentManager", [0, 1, 2, 3, 4, 5, 9, 13], variants // 2, list(range(14, 20)) + [51, 102, 111, 201])
    # caps:u
    cp = s.open("caps:u")
    for cmd in (32, 102, 103, 104, 105, 110, 120, 130, 140, 141, 142, 143, 144):
        s.q(cp, "IAlbumApplicationService", cmd, raw=bytes(16), bufs=(("b", 0x400, None),))
    s.sweep(cp, "IAlbumApplicationService", [0, 1, 101, 106, 145, 150])
    # mii
    for name in ("mii:u", "mii:e"):
        st = s.open(name)
        db = objects(s, st, "IStaticService", 0, raw=struct.pack("<I", 0))[0]
        for flag in (0, 1, 2, 3):
            s.q(db, "IDatabaseService", 2, raw=struct.pack("<I", flag))
            for cmd, sizes in ((3, (6 * 0x5C, 0x200, 0x5C, 0x5B, 0)), (4, (6 * 0x58, 0x100, 0x58, 0x57, 0x1000))):
                for size in sizes:
                    s.q(db, "IDatabaseService", cmd, raw=struct.pack("<I", flag), bufs=((("b", size, None),) if size else ()))
        s.q(db, "IDatabaseService", 2)
        for k in range(8):
            s.q(db, "IDatabaseService", 7, raw=struct.pack("<I", k))
        for g in (0, 1, 2):
            s.q(db, "IDatabaseService", 6, raw=struct.pack("<III", 0, g, 0))
        for n in (0x5C, 0x58, 0x10, 0x48, 0x44, 4):
            body = bytes((i * 7 + 3) & 0xFF for i in range(n))
            s.q(db, "IDatabaseService", 5, raw=body)
            s.q(db, "IDatabaseService", 10, raw=body)
        for cmd in list(range(0, 2)) + [8, 9] + list(range(11, 27)):
            s.q(db, "IDatabaseService", cmd, bufs=(("b", 0x100, None),))
        every(s, db, "IDatabaseService", [0, 1, 2, 3, 4, 7, 8, 9, 20], variants // 4, range(27, 32))
    # irs
    ir = s.open("irs")
    for cmd in (302, 303, 304, 304, 305, 306, 307, 308, 309, 310, 311, 312, 313, 314, 315, 316, 317, 318, 319):
        s.q(ir, "IIrSensorServer", cmd, raw=struct.pack("<I", 0x20))
    s.sweep(ir, "IIrSensorServer", [0, 300, 301, 320, 500])
    # nfc / nfc:mf / nfp
    nm = s.open("nfc:user")
    u1 = objects(s, nm, "nn::nfc::detail::IUserManager", 0)[0]
    u2 = objects(s, nm, "nn::nfc::detail::IUserManager", 0)[0]
    for cmd in (402, 400, 402, 2, 1, 402, 3, 403, 0, 2, 401, 402):
        s.q(u1, "nn::nfc::detail::IUser", cmd)
    s.q(u2, "nn::nfc::detail::IUser", 402)
    for cmd in (404, 405, 406, 407, 408, 409, 410, 411, 412, 1000, 1001, 1300, 1301, 1302):
        s.q(u1, "nn::nfc::detail::IUser", cmd, bufs=(("b", 0x20, None),))
    s.sweep(u1, "nn::nfc::detail::IUser", [4, 5, 399, 413, 999, 1002, 1303])
    mf = s.open("nfc:mf:u")
    mu = objects(s, mf, "nn::nfc::mifare::detail::IUserManager", 0)[0]
    s.sweep(mu, "nn::nfc::mifare::detail::IUser", range(0, 16))
    pm = s.open("nfp:user")
    p1 = objects(s, pm, "nn::nfp::detail::IUserManager", 0)[0]
    for cmd in (19, 0, 19, 1, 19, 300, 19):
        s.q(p1, "nn::nfp::detail::IUser", cmd, raw=struct.pack("<IQQ", 1, 0, 0), pid=True, bufs=(("x", 0x20, bytes(0x20)),))
    for cmd in list(range(2, 26)) + [301, 302, 303]:
        s.q(p1, "nn::nfp::detail::IUser", cmd, bufs=(("c", 0x20, None),))
    s.sweep(p1, "nn::nfp::detail::IUser", [26, 27, 299, 304, 1000])
    # olsc:u
    for _ in range(2):
        o = s.open("olsc:u")
        for cmd, raw in ((13, struct.pack("<QQ", 1, 0)), (14, struct.pack("<QQQ", 1, 1, 0)), (0, b""), (13, struct.pack("<QQ", 0, 0)),
                         (13, struct.pack("<QQ", 1, 0)), (14, struct.pack("<QQQ", 1, 1, 0)), (13, struct.pack("<QQ", 1, 0)),
                         (14, struct.pack("<QQQ", 0, 1, 0)), (13, struct.pack("<QQ", 1, 0)), (14, struct.pack("<QQQ", 1, 0, 0)), (13, LOCAL)):
            s.q(o, "olsc:u", cmd, raw=raw, pid=(cmd == 0))
        s.sweep(o, "olsc:u", [1, 2, 12, 15, 100])
    # ectx:aw, pcm, banana, TMA
    e = s.open("ectx:aw")
    r = objects(s, e, "nn::err::context::IWriterForApplication", 0)[0]
    s.q(r, "nn::err::context::IContextRegistrar", 0, raw=struct.pack("<I", 0x1234), bufs=(("a", 0x10, bytes(16)),))
    s.sweep(r, "nn::err::context::IContextRegistrar", range(1, 4))
    for name, ids in (("pcm", range(0, 4)), ("banana", range(0, 4)), ("htc", range(0, 32)), ("htc:tenv", range(0, 3)),
                      ("htcs", list(range(0, 15)) + [100, 101, 130, 131, 132])):
        t = s.open(name)
        s.sweep(t, name, ids)
    # stpl:u
    sp = s.open("stpl:u")
    us = objects(s, sp, "stpl:u", 0)[0]
    for raw in (struct.pack("<BBHI", 0, 0, 0, 1), struct.pack("<BBHI", 0, 0, 0, 3), struct.pack("<BBHI", 1, 0, 0, 1), struct.pack("<BBHI", 1, 0, 0, 0),
                struct.pack("<BBHI", 0, 0, 0, 7), struct.pack("<BBHI", 2, 0, 0, 0), b""):
        s.q(us, "nn::streamplay::detail::IUserService", 1, raw=raw)
    for cmd in (9, 502, 503, 8, 504, 7, 300, 300, 3, 5, 25, 27, 505, 506, 507, 509, 510, 4, 500, 501):
        s.q(us, "nn::streamplay::detail::IUserService", cmd, raw=bytes(16))
    s.q(us, "nn::streamplay::detail::IUserService", 2, raw=struct.pack("<BBHI", 1, 2, 0, 1), pid=True)
    s.q(us, "nn::streamplay::detail::IUserService", 502)
    s.q(us, "nn::streamplay::detail::IUserService", 503)
    s.lines.append("reset keep")
    s.q(us, "nn::streamplay::detail::IUserService", 502)
    s.q(us, "nn::streamplay::detail::IUserService", 503)
    every(s, us, "nn::streamplay::detail::IUserService", [1, 8, 9, 502, 503, 504], variants // 4, [0, 6, 10, 26, 28, 301, 508, 511])
