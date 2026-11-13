# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
"""acc:u0 / acc:u1 and the objects they hand out (IProfile, IManagerForApplication, IAsyncContext): every command with argument and buffer
variants, the users' list writes, the profile picture, the network service account id, the id token and the licence context.
Ids: Switchbrew "Account services". The picture is chosen once per process, so each picture case is its own script (variant=...)."""
import struct
from pathlib import Path

from synth import LOCAL_UID

LOCAL = struct.pack("<QQ", *LOCAL_UID)
OTHER = struct.pack("<QQ", 5, 6)
NULL = bytes(16)
COMMON = [0, 1, 2, 3, 4, 5, 50, 51, 52, 60, 110, 111]
U0_ONLY = [100, 101, 130, 131, 140, 141, 150, 160]
MGR = [0, 1, 2, 3, 4, 130, 136, 150, 160, 170]
CTX = [0, 1, 2, 3, 100]


def list_bufs():
    return [(), (("c", 0xA0, None),), (("b", 0x18, None),), (("b", 0x08, None),), (("b", 0x100, None),), (("c", 0x10, None),),
            (("b", 0, None),), (("b", 0x200, None), ("c", 0x40, None))]


def objects(s, sid, iface, variants):
    """Profile, manager and context objects of one service session, each probed fully."""
    for uid in (NULL, OTHER, LOCAL, LOCAL[:8], LOCAL + b"\x01"):
        s.q(sid, iface, 5, raw=uid, nobj=1)
    prof = s.q(sid, iface, 5, raw=LOCAL, nobj=1)[0]
    for cmd in (0, 1, 10, 11):
        for bv in [(), (("c", 0x80, None),), (("c", 0x40, None),), (("b", 0x20000, None),), (("b", 0x40, None),), (("b", 0x2000, None),)]:
            s.q(prof, "IProfile", cmd, bufs=bv)
    s.fuzz(prof, "IProfile", 0, n=variants // 4)
    s.fuzz(prof, "IProfile", 11, n=variants // 4)
    s.sweep(prof, "IProfile", range(0, 20))
    if iface != "acc:u0":
        return
    for cmd in (101, 130):
        for uid in (NULL, OTHER, LOCAL):
            s.q(sid, iface, cmd, raw=uid, nobj=1)
    mgr = s.q(sid, iface, 101, raw=LOCAL, nobj=1)[0]
    mgr2 = s.q(sid, iface, 130, raw=LOCAL, nobj=1)[0]
    for m in (mgr, mgr2):
        for cmd in MGR:
            for bv in [(), (("b", 0xC00, None),), (("b", 0x40, None),), (("b", 0x3, None),), (("b", 0x1000, None),), (("c", 0x100, None),)]:
                s.q(m, "IManagerForApplication", cmd, bufs=bv)
        s.sweep(m, "IManagerForApplication", list(range(0, 12)) + [100, 131, 135, 137, 140, 151, 161, 171, 200])
    ctx = s.q(mgr, "IManagerForApplication", 2, nobj=1)[0]
    lic = s.q(mgr, "IManagerForApplication", 170, nobj=1)[0]
    for c in (ctx, lic):
        for cmd in CTX + [4, 99, 101]:
            s.q(c, "IAsyncContext", cmd)
        s.q(c, "IAsyncContext", 0)
    return mgr


def tokens(s, mgr, aux):
    tf = aux / "acc_token.txt"
    tf.write_bytes(b"tok\r\nsecond")
    longtf = aux / "acc_token_long.txt"
    longtf.write_bytes(b"L" * 0xD00 + b"\n")
    cases = [("net", "account_token", "  abc\t"), ("net", "account_token", "abc"), ("net", "account_token", "X" * 0xD00),
             ("net", "account_token", ""), ("net", "account_token_file", str(tf)), ("net", "account_token_file", str(longtf)),
             ("net", "account_token_file", str(aux / "missing.txt")), ("net", "account_token_file", "")]
    for sec, key, val in cases:
        s.config(sec, key, val)
        for bv in [(), (("b", 0xC00, None),), (("b", 0x3, None),), (("b", 0x2, None),), (("b", 0x40, None),), (("b", 0x1000, None),)]:
            s.q(mgr, "IManagerForApplication", 4, bufs=bv)
            s.q(mgr, "IManagerForApplication", 3, bufs=bv)
        for redirect in ("", "example.com=127.0.0.1:9"):
            s.config("net", "redirect", redirect)
            lic = s.q(mgr, "IManagerForApplication", 170, nobj=1)[0]
            for cmd in (2, 3, 100, 0):
                s.q(lic, "IAsyncContext", cmd)
    s.config("net", "redirect", "")
    s.config("net", "account_token", "")
    s.config("net", "account_token_file", "")


def account_ids(s):
    for val in ("", " 00000000DEADBEEF ", "0000000000000000", "DEADBEEF", "xyz", "00000000deadbeef", "FFFFFFFFFFFFFFFF", "\t0123456789abcdef"):
        s.reset()
        s.config("account", "network_account_id", val)
        acc = s.open("acc:u0")
        mgr = s.q(acc, "acc:u0", 101, raw=LOCAL, nobj=1)[0]
        s.q(mgr, "IManagerForApplication", 1)
        s.config("account", "network_account_id", "1111111111111111")   # cached: no change until reset
        s.q(mgr, "IManagerForApplication", 1)
    s.config("account", "network_account_id", "")
    # another local user: the manager's uid goes into the hash
    s.config("account", "uid", "00000000000000026e78727475736572")
    s.reset()
    acc = s.open("acc:u0")
    s.q(acc, "acc:u0", 4)
    s.q(acc, "acc:u0", 3, bufs=(("c", 0xA0, None),))
    mgr = s.q(acc, "acc:u0", 101, raw=struct.pack("<QQ", 2, LOCAL_UID[1]), nobj=1)[0]
    s.q(mgr, "IManagerForApplication", 1)
    s.config("account", "uid", "")
    s.reset()


def generate(s, variants, variant="main"):
    aux = Path(s.aux_dir)
    aux.mkdir(parents=True, exist_ok=True)
    if variant != "main":
        # the picture: [account] avatar before the first IProfile 10 / 11 of the process
        jpg = aux / "acc_avatar.jpg"
        jpg.write_bytes(b"\xff\xd8\xff\xe0" + bytes(range(256)) * 9 + b"\xff\xd9")
        bad = aux / "acc_avatar.png"
        bad.write_bytes(b"\x89PNG\r\n\x1a\n" + bytes(300))
        big = aux / "acc_avatar_big.jpg"
        big.write_bytes(b"\xff\xd8" + bytes(1 << 20) + b"\xff\xd9")
        tiny = aux / "acc_avatar_tiny.jpg"
        tiny.write_bytes(b"\xff\xd8\xff\xd9")
        path = {"avatar_jpg": jpg, "avatar_bad": bad, "avatar_big": big, "avatar_tiny": tiny, "avatar_missing": aux / "none.jpg"}[variant]
        s.config("account", "avatar", str(path))
        s.reset()
        acc = s.open("acc:u0")
        prof = s.q(acc, "acc:u0", 5, raw=LOCAL, nobj=1)[0]
        for bv in [(), (("b", 0x200000, None),), (("b", 0x40, None),), (("b", 0x2000, None),)]:
            s.q(prof, "IProfile", 10)
            s.q(prof, "IProfile", 11, bufs=bv)
        return
    for name in ("acc:u0", "acc:u1"):
        s.reset()
        sid = s.open(name)
        s.comment(name + ": users")
        for cmd in COMMON + U0_ONLY:
            for raw in (b"", LOCAL, NULL, OTHER, LOCAL[:8], b"\x01", b"\x00"):
                for bv in list_bufs()[:3]:
                    s.q(sid, name, cmd, raw=raw, bufs=bv, nobj=1 if cmd in (5, 101, 130) else 0)
        for cmd in (2, 3, 60, 131, 140, 141):
            for bv in list_bufs():
                s.q(sid, name, cmd, bufs=bv)
        for raw in (LOCAL, NULL, OTHER):
            for bv in [(), (("a", 0x24000, None),), (("a", 0x1000, None),), (("x", 0x24000, None),), (("a", 0x24001, None),)]:
                s.q(sid, name, 110, raw=raw, bufs=bv)
            s.q(sid, name, 111, raw=raw)
        for cmd in COMMON + U0_ONLY:
            s.fuzz(sid, name, cmd, n=variants, nobj=1 if cmd in (5, 101, 130) else 0)
        s.sweep(sid, name, list(range(0, 8)) + list(range(48, 64)) + list(range(96, 170)) + [200, 9999])
        mgr = objects(s, sid, name, variants)
        if mgr:
            tokens(s, mgr, aux)
    # the open list before any reset of this script's process is covered by the first reset above; config changes of the user
    s.reset()
    s.config("account", "nickname", "A nickname that is longer than thirty-two bytes")
    acc = s.open("acc:u0")
    prof = s.q(acc, "acc:u0", 5, raw=LOCAL, nobj=1)[0]
    s.q(prof, "IProfile", 1)
    s.config("account", "nickname", "x" * 32)
    s.q(prof, "IProfile", 1)
    s.config("account", "nickname", "")
    s.q(prof, "IProfile", 1)
    account_ids(s)
