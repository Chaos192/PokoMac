# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
# Copyright (c) 2026 Chaos192
"""appletOE / appletAE of runtime/src/services/am.cpp and every interface they hand out: the spec's test vectors (coord/cleanroom/specs/
am.cpp.md section 10) in order, then state sequences (messages and focus, the notification flags, storages pushed and popped, library
applets of every kind with their storages, launch parameters from a launch user channel file), then argument and buffer variants of every
command and an id sweep per interface. Ids: Switchbrew "Applet Manager services", "Software Keyboard", "Internet Browser".

Nothing here waits for a person: the keyboard and controller prompts stay off (nothing in the replay turns them on), no web view host is
registered. Nothing ends the process: ExecuteProgram (IApplicationFunctions 120) and the relaunch (10 / 12) of this application end it,
so the main script only sends 10 / 12 for another application (refused, AppletLaunchFailed) and never fuzzes or sweeps 10, 12, 120.
Variant "handoff" sends them with [am] execute_program_file under a regular file (the hand-off cannot be written: the spec's 5.3 step 4,
the call answers 0 and the program goes on). Variant "handoff_write" sends them with the replay's exit hook set (`api am_exit_hook`: the
exit becomes a line and the call returns) and a writable hand-off, shows both files after each call (`api file`), then loads the
written channel as the next program's launch parameters.

[applet] swkbd_text is set at the very end only: once set, a key stays set for the process (config_has), and the keyboards' other answer
paths (the initial text, "Player") must be seen first. Host-side controls (amctl::set_docked / set_focus / applet_open) are asked for with
`api am_docked <0|1>`, `api am_focus <0|1>`, `api am_applet <0|1>`, `api am_state`; a replay without them answers "unknown" (both builds
alike). The script is written as bytes (UTF-8, surrogateescape): some config values carry bytes that are not UTF-8 on purpose."""
import struct
from pathlib import Path

from synth import LOCAL_UID

OE, AE = "appletOE", "appletAE"
PROXY = "IApplicationProxy"
CSG = "ICommonStateGetter"
SELF = "ISelfController"
WIN = "IWindowController"
AUDIO = "IAudioController"
DISP = "IDisplayController"
WIND = "IProcessWindingController"
CREATOR = "ILibraryAppletCreator"
APPLET = "ILibraryAppletAccessor"
FUNCS = "IApplicationFunctions"
DEBUG = "IDebugFunctions"
STORAGE = "IStorage"
ACCESS = "IStorageAccessor"
LOCK = "ILockAccessor"
SCALE = "IGraphicsScalingParametersFunctions"
PERF = "IPerformanceFunctions"

NO_SID = 999999          # an object no reply returned: the request carries a null object
LOCAL = struct.pack("<QQ", *LOCAL_UID)
# reads of a storage whose size is not known here: the head, the UTF-16 text area, the tails of the inline keyboard's replies
PEEK = ((0, 8), (0, 0x3F8), (0x3F4, 0x10), (0x7DC, 0x10), (0, 0x7E0))
WEB_OUT = ((0, 0x1010), (0, 0x2000))
SMALL = tuple((0, k) for k in (1, 2, 3, 4, 5, 16))

CSG_CMDS = [0, 1, 2, 5, 6, 8, 9, 10, 11, 12, 13, 30, 31, 32, 50, 51, 52, 53, 54, 55, 59, 60, 61, 62, 63, 66, 67, 68, 130, 300, 900, 1000,
            1001, 1002]
SELF_CMDS = [0, 1, 2, 3, 4, 9, 10, 11, 12, 13, 14, 16, 40, 41, 44, 62, 63, 68, 69, 90, 91, 100, 110, 120, 2000, 2100, 2200, 2300, 2400,
             2401, 2402]
FUNCS_CMDS = [1, 20, 21, 22, 23, 25, 26, 27, 28, 32, 33, 34, 35, 37, 40, 50, 60, 65, 66, 67, 68, 90, 100, 101, 110, 121, 122, 123, 124,
              130, 140, 141, 150, 151, 160, 170, 210, 320, 321]      # 10, 12, 120 end the process: never here
APPLET_CMDS = [0, 1, 10, 20, 25, 30, 50, 60, 90, 100, 101, 102, 103, 104, 105, 106, 110, 120, 150]


def u32(*v):
    return struct.pack(f"<{len(v)}I", *v)


def i64(v):
    return struct.pack("<q", v)


def u16text(t):
    return t.encode("utf-16-le") if isinstance(t, str) else t


def raw_text(b):
    """Bytes for a config value in the script (written back as these bytes)."""
    return b.decode("utf-8", "surrogateescape")


class Am:
    """One application proxy (appletOE 0 unless told) and the interfaces a game takes from it."""

    def __init__(self, s, svc=OE, cmd=0):
        self.s = s
        sm = s.open(svc)
        self.proxy = s.q(sm, svc, cmd, raw=struct.pack("<Q", 0), pid=True, hc=("p",), nobj=1)[0]
        self.csg = self.get(0)
        self.self_ = self.get(1)
        self.cr = self.get(11)
        self.af = self.get(20)

    def get(self, cmd):
        return self.s.q(self.proxy, PROXY, cmd, nobj=1)[0]

    def storage(self, data, size=None):
        s = self.s
        n = len(data) if size is None else size
        st = s.q(self.cr, CREATOR, 10, raw=i64(n), nobj=1)[0]
        if data:
            acc = s.q(st, STORAGE, 0, nobj=1)[0]
            s.q(acc, ACCESS, 10, raw=i64(0), bufs=(("a", len(data), data),))
        return st

    def applet(self, aid, mode=0, ins=(), start=True, create=0):
        s = self.s
        raw = u32(aid, mode) + (struct.pack("<Q", 0x1234) if create == 3 else b"")
        app = s.q(self.cr, CREATOR, create, raw=raw, nobj=1)[0]
        for d in ins:
            s.q(app, APPLET, 100, os_=(self.storage(d),))
        if start:
            s.q(app, APPLET, 10)
        return app

    def push(self, app, data, cmd=103):
        self.s.q(app, APPLET, cmd, os_=(self.storage(data),))

    def take(self, owner, iface, cmd, n, raw=b""):
        """A storage popped from `owner`: its size, then its first n bytes, or each (offset, size) of a tuple n (a read past the
        storage's end fails, so a storage of unknown size is read in several pieces)."""
        s = self.s
        st = s.q(owner, iface, cmd, raw=raw, nobj=1)[0]
        acc = s.q(st, STORAGE, 0, nobj=1)[0]
        s.q(acc, ACCESS, 0)
        for off, size in (n if isinstance(n, tuple) else ((0, n),) if n else ()):
            s.q(acc, ACCESS, 11, raw=i64(off), bufs=(("b", size, None),))
        return st

    def out(self, app, n):
        return self.take(app, APPLET, 101, n)

    def reply(self, app, n):
        return self.take(app, APPLET, 104, n)

    def replies(self, app, sizes):
        for n in sizes:
            self.reply(app, n)
        self.s.q(app, APPLET, 104)   # nothing more

    def drain(self, n=4):
        for _ in range(n):
            self.s.q(self.csg, CSG, 1)

    def focus(self):
        self.s.q(self.csg, CSG, 9)

    def events(self, app):
        for cmd in (0, 105, 106):
            self.s.q(app, APPLET, cmd)


def api(s, what):
    s.lines.append("api " + what)


# ---- argument builders -------------------------------------------------------------------------------------------------------------
def common(lib=0x80000):
    return u32(1, 0x20, lib) + bytes(0x14)


def kb_config(size=0x3D4, utf8=0, check=0, max_len=0, min_len=0, password=0, init_off=0, init_len=0, ok="", header="", sub="",
              guide=""):
    c = bytearray(size)
    packed = size == 0x530
    for off, text, units in ((0x04, ok, 9), (0x24, header, 65), (0xA6, sub, 129), (0x1A8, guide, 257)):
        b = u16text(text)[:units * 2]
        c[off:off + len(b)] = b
    struct.pack_into("<III", c, 0x3AC, max_len & 0xFFFFFFFF, min_len & 0xFFFFFFFF, password)
    if packed:
        c[0x3AA] = utf8
        struct.pack_into("<ii", c, 0x3BC, init_off, init_len)
        c[0x48D] = check
    else:
        c[0x3BD] = utf8
        struct.pack_into("<ii", c, 0x3C0, init_off, init_len)
        c[0x3D0] = check
    return bytes(c)


def work_buffer(text, off=0x10, size=0x100):
    w = bytearray(size)
    b = u16text(text)
    w[off:off + len(b)] = b[:max(0, size - off)]
    return bytes(w)


def calc(flags, size=0x4E8, text=None, utf8=None, cursor=None, amax=None, amin=None, ok=None, total=None):
    """An inline keyboard Calc request (the storage: u32 10, then the argument)."""
    old = size == 0x4A0
    st = bytearray(4 + size if total is None else total)
    room = len(st)

    def put(off, b):
        if off < room:
            st[off:off + len(b)] = b[:room - off]

    put(0, u32(10))
    put(8, struct.pack("<H", size & 0xFFFF))
    put(12, struct.pack("<Q", flags))
    ap, cur, tx, u8 = (0x24, 0x20, 0x6C, 0x460) if old else (0x1C, 0x90, 0x94, 0x488)
    if text is not None:
        put(tx, u16text(text))
    if utf8 is not None:
        put(u8, bytes([utf8]))
    if cursor is not None:
        put(cur, struct.pack("<i", cursor))
    if ok is not None:
        put(ap + 4, u16text(ok)[:18])
    if amax is not None:
        put(ap + 0x20, u32(amax & 0xFFFFFFFF))
    if amin is not None:
        put(ap + 0x24, u32(amin & 0xFFFFFFFF))
    return bytes(st)


def tlv_arg(entries, size=0x400, shim=0, count=None):
    """A web argument; cut to `size` bytes (an entry may run past the end on purpose)."""
    a = bytearray(max(size, 8 + sum(8 + len(d) for _, d in entries)))
    off = 8
    for t, data in entries:
        struct.pack_into("<HH", a, off, t, len(data) & 0xFFFF)
        a[off + 8:off + 8 + len(data)] = data
        off += 8 + len(data)
    struct.pack_into("<HHI", a, 0, len(entries) if count is None else count, 0, shim)
    return bytes(a[:size])


def session_arg(boot_mode=1, session=1, extra=()):
    return tlv_arg([(0x41, u32(boot_mode)), (0x42, bytes([session]))] + list(extra), size=0x100)


def message(kind, data=b"", size=None):
    return u32(kind, len(data) if size is None else size) + bytes(8) + data


APPEAR = message(0x100)


def ctrl_arg(lo, hi, single=0, n=8):
    return (struct.pack("<bb", lo, hi) + bytes(3) + bytes([single]) + bytes(2))[:n]


# ---- section 10 of the spec, in order ----------------------------------------------------------------------------------------------
def vectors(s):
    s.comment("10: ICommonStateGetter")
    s.reset()
    a = Am(s)
    s.q(a.csg, CSG, 0)                     # the message event (signalled: the start's FocusStateChanged)
    a.drain(2)                             # 0f000000, then 0x680 and the event cleared
    s.q(a.csg, CSG, 130)
    a.drain(2)                             # 5f000000
    for c in (5, 6, 9, 60, 2, 8, 62, 300):
        s.q(a.csg, CSG, c)
    s.config("am", "operation_mode", "handheld")
    s.reset()
    a = Am(s)
    for c in (5, 6, 60):
        s.q(a.csg, CSG, c)
    s.config("am", "operation_mode", "docked")
    s.reset()
    a = Am(s)
    a.drain(2)
    a.focus()
    api(s, "am_applet 1")
    a.focus()
    a.drain(2)
    api(s, "am_applet 0")
    a.focus()
    a.drain(2)
    for v in ("2560x1440", "1280X720", "1280x720 ", "0x720", "70000x10", " +1280x +720", "1x1", "65535x65535", "65536x1", "-1x5",
              "12x", "x12", "1280x720x", "4294967297x5", "1280 x720", "+0x5", "640x480junk", "800x600"):
        s.config("am", "resolution", v)
        s.q(a.csg, CSG, 60)
    s.config("am", "resolution", "")
    s.q(a.csg, CSG, 60)
    s.q(a.csg, CSG, 51, raw=b"\x01")
    s.q(a.csg, CSG, 50)
    s.q(a.csg, CSG, 54)
    s.q(a.csg, CSG, 50)
    for c in (0, 13, 61, 63):
        s.q(a.csg, CSG, c)

    s.comment("10: ISelfController")
    s.reset()
    a = Am(s)
    for c in (40, 44, 40):
        s.q(a.self_, SELF, c)
    s.q(a.self_, SELF, 62, raw=bytes.fromhex("78563412"))
    s.q(a.self_, SELF, 63)
    s.q(a.self_, SELF, 68, raw=b"\x01")
    s.q(a.self_, SELF, 69)
    s.q(a.self_, SELF, 90)
    a.drain(2)
    s.q(a.self_, SELF, 13, raw=bytes.fromhex("000001"))
    api(s, "am_applet 1")
    a.focus()
    api(s, "am_applet 0")
    a.focus()
    a.drain(2)
    s.q(a.self_, SELF, 13, raw=bytes.fromhex("010001"))
    s.q(a.self_, SELF, 11, raw=b"\x01")
    api(s, "am_docked 0")
    a.drain(3)
    s.q(a.self_, SELF, 12, raw=b"\x01")
    api(s, "am_docked 0")
    a.drain(3)
    api(s, "am_docked 0")
    a.drain(3)
    api(s, "am_state")
    api(s, "am_docked 1")
    a.drain(3)
    s.q(a.self_, SELF, 2000)
    for c in (2100, 2200, 2300, 2400, 2401, 2402):
        s.q(a.self_, SELF, c, raw=b"\x01\x02\x03\x04")

    s.comment("10: IWindowController, IAudioController")
    win = a.get(2)
    s.q(win, WIN, 1)
    aud = a.get(3)
    s.q(aud, AUDIO, 1)
    s.q(aud, AUDIO, 0, raw=bytes.fromhex("0000003f0000c03f"))
    s.q(aud, AUDIO, 1)
    s.q(aud, AUDIO, 2)
    s.q(aud, AUDIO, 0, raw=bytes.fromhex("000080bf0000003f"))
    s.q(aud, AUDIO, 1)
    s.q(aud, AUDIO, 2)

    s.comment("10: ILockAccessor")
    lk = s.q(a.csg, CSG, 30, nobj=1)[0]
    s.q(lk, LOCK, 1, raw=b"\x01")
    s.q(lk, LOCK, 1, raw=b"\x00")
    s.q(lk, LOCK, 4)
    s.q(lk, LOCK, 2)
    s.q(lk, LOCK, 4)

    s.comment("10: storages")
    st = s.q(a.cr, CREATOR, 10, raw=i64(16), nobj=1)[0]
    acc = s.q(st, STORAGE, 0, nobj=1)[0]
    s.q(acc, ACCESS, 0)
    s.q(a.cr, CREATOR, 10, raw=bytes.fromhex("0100001000000000"))
    s.q(a.cr, CREATOR, 10, raw=bytes.fromhex("ffffffffffffffff"))
    s.q(a.cr, CREATOR, 10, raw=bytes.fromhex("0000001000000000"))   # 256 MiB: dropped at once (no sid)
    data = bytes(range(0x40, 0x50))
    s.q(acc, ACCESS, 10, raw=i64(0), bufs=(("a", 16, data),))
    s.q(acc, ACCESS, 10, raw=i64(8), bufs=(("a", 16, data),))
    s.q(acc, ACCESS, 10, raw=i64(-1), bufs=(("a", 1, b"\x01"),))
    s.q(acc, ACCESS, 10, raw=i64(16))
    s.q(acc, ACCESS, 11, raw=i64(17))
    s.q(acc, ACCESS, 11, raw=i64(0), bufs=(("b", 16, None),))
    app = s.q(a.cr, CREATOR, 0, raw=u32(0x15, 0), nobj=1)[0]
    s.q(app, APPLET, 100)

    s.comment("10: IApplicationFunctions")
    s.reset()
    a = Am(s)
    s.q(a.af, FUNCS, 1, raw=u32(1))
    a.take(a.af, FUNCS, 1, 0x88, raw=u32(2))
    s.q(a.af, FUNCS, 1, raw=u32(2))
    s.q(a.af, FUNCS, 1, raw=u32(3))
    s.q(a.af, FUNCS, 122, os_=(a.storage(bytes.fromhex("aabbcc")),))
    s.q(a.af, FUNCS, 122, os_=(a.storage(bytes.fromhex("ddee")),))
    a.take(a.af, FUNCS, 1, 2, raw=u32(1))
    a.take(a.af, FUNCS, 1, 3, raw=u32(1))
    s.q(a.af, FUNCS, 1, raw=u32(1))
    s.q(a.af, FUNCS, 122)
    s.q(a.af, FUNCS, 122, os_=(a.csg,))
    s.q(a.af, FUNCS, 21)
    for langs in ("0x1", "AmericanEnglish", "French,German", "0x8000000", "0x20000", ""):
        s.config("title", "supported_languages", langs)
        s.q(a.af, FUNCS, 21)
    s.q(a.af, FUNCS, 23)
    for dv in ("1234567890abcdefXYZ", "123456789012345", "1.0.0", ""):
        s.config("title", "display_version", dv)
        s.q(a.af, FUNCS, 23)
    for c in (40, 50, 34, 65, 110, 320, 321, 28, 35):
        s.q(a.af, FUNCS, c)
    s.q(a.af, FUNCS, 26, raw=b"\x01" + bytes(7) + LOCAL)
    s.q(a.af, FUNCS, 25, raw=b"\x01" + bytes(7) + LOCAL + i64(0x100000) + i64(0x80000))
    s.q(a.af, FUNCS, 26, raw=b"\x01" + bytes(7) + LOCAL)
    s.q(a.af, FUNCS, 26, raw=b"\x02" + bytes(7) + LOCAL)
    s.q(a.af, FUNCS, 27, raw=bytes.fromhex("0300") + bytes(6) + i64(0x40000) + i64(0x20000))
    s.q(a.af, FUNCS, 123)
    s.config("am", "previous_program_index", "3")
    s.q(a.af, FUNCS, 123)
    s.config("am", "previous_program_index", "0x1FFFFFFFF")
    s.q(a.af, FUNCS, 123)
    s.config("am", "previous_program_index", "-1")
    s.q(a.af, FUNCS, 123)
    for c in (141, 151, 37, 130, 140, 150, 160, 210):
        s.q(a.af, FUNCS, c)
    # 120 and 12(0) end the process (variant "handoff"); another application is refused and nothing ends
    s.config("title", "program_id", "0x0100ABCD00002002")
    s.config("title", "application_id", "")
    s.q(a.af, FUNCS, 12, raw=struct.pack("<Q", 0x0100FFFF00010000))
    s.q(a.af, FUNCS, 10, raw=struct.pack("<Q", 0x0100FFFF00010000), os_=(a.storage(b"fr-FR\0"),))
    s.q(a.af, FUNCS, 12, raw=struct.pack("<Q", 0xFFFFFFFFFFFFF000))
    s.config("title", "program_id", "0")
    s.config("title", "application_id", "0")

    s.comment("10: library applets")
    s.reset()
    a = Am(s)
    app = a.applet(0x01020011, 1, start=False)
    s.q(app, APPLET, 120)
    s.q(a.applet(0x14, start=False), APPLET, 30)
    s.q(a.applet(0x10, start=False), APPLET, 30)
    app = a.applet(0x15)
    s.q(app, APPLET, 1)
    s.q(app, APPLET, 101)
    s.q(app, APPLET, 30)
    for mode in (0, 1, 2, 10):
        app = a.applet(0x10, ins=(common(), u32(mode) + bytes(0x9C)))
        a.out(app, 0x18)
        s.q(app, APPLET, 30)
    app = a.applet(0x11, ins=(common(),))
    a.out(app, 0x7D8)
    app = a.applet(0x11, ins=(common(), kb_config(utf8=1)))
    a.out(app, 0x7D8)
    app = a.applet(0x11, ins=(common(), kb_config(check=1)))
    s.q(app, APPLET, 101)
    a.reply(app, 0x7DC)
    a.push(app, u32(0) + bytes(0x7D4))
    a.out(app, 0x7D8)
    s.q(app, APPLET, 1)
    app = a.applet(0x11, ins=(common(), kb_config(check=1)))
    a.reply(app, 0x7DC)
    a.push(app, u32(1) + bytes(0x7D4))
    a.out(app, 0x7D8)
    s.q(app, APPLET, 1)
    app = a.applet(0x0E, ins=(common(), bytes(0x20)))
    s.q(app, APPLET, 1)
    s.q(app, APPLET, 30)
    s.q(app, APPLET, 101)
    for arg in (ctrl_arg(1, 4), ctrl_arg(2, 4), ctrl_arg(1, 4, 1), ctrl_arg(0, 4)):
        app = a.applet(0x0C, ins=(bytes(0x20), arg))
        a.out(app, 12)
    for kind in (0x17, 0x18, 0x19):
        app = a.applet(kind, ins=(bytes(0x20),))
        a.out(app, 0x1010)
    for view in ("false", "true"):
        s.config("web", "view", view)
        a.drain(4)
        app = a.applet(0x17, ins=(bytes(0x20), session_arg(1, 1)))
        a.focus()
        a.reply(app, 0x11)
        s.q(app, APPLET, 101)
        a.push(app, APPEAR)
        if view == "true":
            a.reply(app, 0x20)
        a.out(app, 0x1010)
        a.focus()
        a.drain(4)
    s.config("web", "view", "false")
    # the inline keyboard
    app = a.applet(0x11, 1, ins=(bytes(0x20), bytes(8)))
    s.q(app, APPLET, 1)
    a.push(app, calc(0x1))
    a.replies(app, (8, 9))
    a.push(app, calc(0x440))
    a.replies(app, (8, 8))
    a.push(app, calc(0xC, text="Link"))
    a.replies(app, (8, 8, 0x404, 0x3F8, 8, 8))
    a.push(app, u32(13) + b"\x01")
    a.push(app, calc(0xC, text="Link"))
    a.replies(app, (8, 8, 0x405, 0x3F8, 8, 8))
    a.push(app, u32(4))
    a.replies(app, (8,))
    s.q(app, APPLET, 1)
    app = a.applet(0x11, 1, ins=(bytes(0x20),))
    a.push(app, calc(0x1))
    a.replies(app, (8, 9))
    a.push(app, calc(0x2C, text="Link", utf8=1))
    a.replies(app, (8, 8, 0x7EC, 0x7E0, 8, 8))
    app = a.applet(0x11, 1, ins=(bytes(0x20),))
    a.push(app, calc(0x4, text="Link"))
    s.q(app, APPLET, 104)


# ---- state sequences ---------------------------------------------------------------------------------------------------------------
def messages_and_focus(s):
    s.comment("messages: the event, the queue, the notification flags, host focus and applets")
    s.reset()
    a = Am(s)
    s.q(a.csg, CSG, 0)
    for _ in range(3):
        s.q(a.csg, CSG, 130)
    a.drain(1)
    s.q(a.csg, CSG, 0)
    a.drain(5)
    s.q(a.csg, CSG, 130)
    a.drain(2)
    for flags in ((1, 0), (0, 1), (1, 1), (0, 0), (2, 0xFF)):
        s.q(a.self_, SELF, 11, raw=bytes([flags[0]]))
        s.q(a.self_, SELF, 12, raw=bytes([flags[1]]))
        api(s, "am_docked 0")
        api(s, "am_docked 1")
        a.drain(5)
    s.q(a.self_, SELF, 11)
    s.q(a.self_, SELF, 12, raw=bytes(8))
    for mode in (b"", b"\x01", b"\x00\x01\x01", b"\x02\x03\x04", b"\x01\x00\x00\x00"):
        s.q(a.self_, SELF, 13, raw=mode)
        api(s, "am_focus 0")
        a.focus()
        api(s, "am_applet 1")
        api(s, "am_focus 1")
        a.focus()
        api(s, "am_applet 0")
        api(s, "am_applet 0")
        a.focus()
        api(s, "am_state")
        a.drain(4)
    # applets take the focus while open; the inline keyboard and a hidden web session do not
    s.q(a.self_, SELF, 13, raw=b"\x01")
    app1 = a.applet(0x11, 1, ins=(bytes(0x20),))
    a.focus()
    app2 = a.applet(0x17, ins=(bytes(0x20), session_arg(1, 1)))
    a.focus()
    app3 = a.applet(0x17, ins=(bytes(0x20), session_arg(0, 1)))
    a.focus()
    app4 = a.applet(0x17, ins=(bytes(0x20), session_arg(1, 1)))
    api(s, "am_applet 1")
    a.focus()
    a.push(app3, APPEAR)
    a.focus()
    api(s, "am_applet 0")
    a.focus()
    s.q(app2, APPLET, 25)
    s.q(app4, APPLET, 20)
    s.q(app1, APPLET, 25)
    a.focus()
    a.drain(8)
    # an applet open (not ended), then a reset: the new state's count
    app5 = a.applet(0x17, ins=(bytes(0x20), session_arg(0, 1)))
    a.focus()
    s.lines.append("reset keep")
    a.focus()
    s.q(app5, APPLET, 20)
    a.focus()
    a.drain(4)
    s.q(a.csg, CSG, 13)
    s.q(a.csg, CSG, 10)
    s.q(a.csg, CSG, 13)
    s.q(a.self_, SELF, 9)
    s.q(a.self_, SELF, 91)
    for c in (53, 50, 1000, 1002, 1001, 1002, 54, 50):
        s.q(a.csg, CSG, c)
    s.q(a.self_, SELF, 0)
    s.q(a.self_, SELF, 40)
    s.q(a.self_, SELF, 44)
    s.q(a.self_, SELF, 62, raw=b"\xff\xff\xff\xff")
    s.q(a.self_, SELF, 63)
    s.q(a.self_, SELF, 68, raw=b"\x02")
    s.q(a.self_, SELF, 69)
    s.q(a.self_, SELF, 68)
    s.q(a.self_, SELF, 69)
    # every opener, and a second proxy sharing the state
    s.q(a.proxy, PROXY, 40, nobj=1)
    for c in (0, 1, 2, 3, 4, 10, 11, 20, 60, 1000):
        s.q(a.proxy, PROXY, c, nobj=1)
    b = Am(s, OE, 10)
    s.q(b.self_, SELF, 40)
    s.q(b.csg, CSG, 130)
    a.drain(2)
    c = Am(s, AE, 350)
    s.q(c.csg, CSG, 9)
    s.q(c.csg, CSG, 5)
    sc = c.get(60)
    s.q(sc, SCALE, 10)
    s.q(sc, SCALE, 1050, raw=b"\x01")
    s.q(sc, SCALE, 10)
    for lock_cmd in (30, 31, 32):
        lk = s.q(c.csg, CSG, lock_cmd, raw=u32(7), nobj=1)[0]
        for cmd, raw in ((4, b""), (3, b""), (1, b"\x00"), (1, b"\x02"), (2, b""), (2, b""), (1, b""), (4, b""), (3, b"")):
            s.q(lk, LOCK, cmd, raw=raw)


def storages(s, aux):
    s.comment("storages: shared bytes, copies at push, ranges, memory storages")
    s.reset()
    a = Am(s)
    st = a.storage(b"", size=0x40)
    acc1 = s.q(st, STORAGE, 0, nobj=1)[0]
    acc2 = s.q(st, STORAGE, 0, nobj=1)[0]
    s.q(acc1, ACCESS, 10, raw=i64(0x10), bufs=(("a", 8, b"ABCDEFGH"),))
    s.q(acc2, ACCESS, 11, raw=i64(0), bufs=(("b", 0x40, None),))
    for off, kind, n in ((0x38, "a", 8), (0x39, "a", 8), (0x40, "a", 0), (0x41, "x", 0), (0, "x", 0x40), (0, "x", 0x41), (0x20, "a", 0x20),
                         (-0x8000000000000000, "a", 1), (0x7FFFFFFFFFFFFFFF, "a", 1), (0x3F, "x", 1)):
        bufs = ((kind, n, bytes(range(n % 256)) if n else b""),) if n else ()
        s.q(acc1, ACCESS, 10, raw=i64(off), bufs=bufs)
    for off, kind, n in ((0x38, "b", 8), (0x39, "b", 8), (0x40, "b", 0), (0x40, "c", 1), (0, "c", 0x40), (0, "b", 0x41), (-1, "b", 0),
                         (0x10, "b", 0x10)):
        bufs = ((kind, n, None),) if n else ()
        s.q(acc2, ACCESS, 11, raw=i64(off), bufs=bufs)
    s.q(acc1, ACCESS, 10, raw=b"\x01", bufs=(("a", 4, b"wxyz"),))   # a short request: offset 0
    s.q(acc1, ACCESS, 11, bufs=(("b", 4, None),))
    s.q(acc1, ACCESS, 11, raw=i64(0), bufs=(("b", 4, None), ("c", 8, None)))
    s.q(acc1, ACCESS, 11, raw=i64(0), bufs=(("c", 8, None),))
    s.q(acc1, ACCESS, 10, raw=i64(0), bufs=(("x", 4, b"XXXX"), ("a", 2, b"AA")))
    s.q(acc2, ACCESS, 11, raw=i64(0), bufs=(("b", 8, None),))
    # copies: a pushed (unpopped) storage changed afterwards pops with its old bytes
    s.q(a.af, FUNCS, 121)
    s.q(a.af, FUNCS, 122, os_=(st,))
    s.q(acc1, ACCESS, 10, raw=i64(0), bufs=(("a", 4, b"NEW!"),))
    a.take(a.af, FUNCS, 1, 0x40, raw=u32(1))
    # a popped storage is the stored bytes: two accessors of it see each other's writes
    s.q(a.af, FUNCS, 122, os_=(a.storage(b"popped"),))
    p = s.q(a.af, FUNCS, 1, raw=u32(1), nobj=1)[0]
    pa = s.q(p, STORAGE, 0, nobj=1)[0]
    pb = s.q(p, STORAGE, 0, nobj=1)[0]
    s.q(pa, ACCESS, 10, raw=i64(1), bufs=(("a", 2, b"OO"),))
    s.q(pb, ACCESS, 11, raw=i64(0), bufs=(("b", 6, None),))
    s.q(a.af, FUNCS, 122, os_=(p,))
    s.q(a.af, FUNCS, 122, os_=(pa,))      # an accessor is not a storage
    s.q(a.af, FUNCS, 122, os_=(NO_SID,))  # a null object: nothing, success
    s.q(a.af, FUNCS, 121)
    s.q(a.af, FUNCS, 1, raw=u32(1))
    for n in (0, 1, 0x8, 0x1000):
        s.q(a.af, FUNCS, 122, os_=(a.storage(bytes((i * 5) & 0xFF for i in range(n)), size=n),))
    for n in (0x1000, 8, 1, 0):
        a.take(a.af, FUNCS, 1, n, raw=u32(1))
    s.q(a.af, FUNCS, 1, raw=u32(1))
    s.q(a.af, FUNCS, 1, raw=u32(2))
    s.q(a.af, FUNCS, 1)
    s.q(a.af, FUNCS, 1, raw=u32(0xFFFFFFFF))
    # memory storages (transfer memory handles are zero memory in the replay)
    for cmd in (11, 12):
        for size, hc in ((0x20, ("t20",)), (0x20, ("t10",)), (0x10, ("t20",)), (0, ("t20",)), (0x20, ("e",)), (0x20, ()), (-1, ("t20",)),
                         (0x10000001, ("t20",)), (0x30, ("p",))):
            raw = (b"\x01" + bytes(7) + i64(size)) if cmd == 11 else i64(size)
            got = s.q(a.cr, CREATOR, cmd, raw=raw, hc=hc, nobj=1)
            if size == 0x20 and hc == ("t20",):
                ma = s.q(got[0], STORAGE, 0, nobj=1)[0]
                s.q(ma, ACCESS, 0)
                s.q(ma, ACCESS, 11, raw=i64(0), bufs=(("b", 0x20, None),))
        s.q(a.cr, CREATOR, cmd, raw=b"")
    # IStorage 1 (OpenTransferStorage) is not answered
    s.q(st, STORAGE, 1)
    # in-data of applets: non-storage objects, null, none
    app = a.applet(0x15, start=False)
    for os_ in ((), (NO_SID,), (a.csg,), (acc1,), (st,)):
        for cmd in (100, 102, 103):
            s.q(app, APPLET, cmd, os_=os_)
    s.q(app, APPLET, 101)
    s.q(app, APPLET, 104)


def keyboards(s):
    s.comment("the software keyboard: configs, layouts, initial text, text check rounds")
    s.reset()
    a = Am(s)
    work = work_buffer("\x01Ini\ttial\r\nText\x7fé", 0x10)
    cases = [
        (),
        (common(), b"\x00" * 0x3D3),
        (common(), kb_config(size=0x3D4)),
        (common(), kb_config(size=0x530, utf8=1)),
        (common(), kb_config(size=0x530, utf8=0, check=0, init_off=0x10, init_len=20), work),
        (common(), kb_config(size=0x400, utf8=1, init_off=0x10, init_len=6), work),
        (common(), kb_config(utf8=0, init_off=0x10, init_len=100), work),
        (common(), kb_config(utf8=1, init_off=0x10, init_len=3), work),
        (common(), kb_config(utf8=0, init_off=-2, init_len=3), work),
        (common(), kb_config(utf8=0, init_off=0x10, init_len=0), work),
        (common(), kb_config(utf8=0, init_off=0xF8, init_len=100), work),
        (common(), kb_config(utf8=0, init_off=0x1000, init_len=1), work),
        (common(), kb_config(utf8=0, init_off=0x10, init_len=5)),
        (common(), kb_config(max_len=3, init_off=0x10, init_len=20), work),
        (common(), kb_config(max_len=501, init_off=0x10, init_len=20), work),
        (common(), kb_config(max_len=0xFFFFFFFF, min_len=7, password=1, ok="OK!", header="Name", sub="sub", guide="guide")),
        (common(), kb_config(size=0x530, max_len=2, utf8=1, init_off=0x10, init_len=20), work),
        (common(), kb_config(size=0x52F, utf8=1)),
        (common(), kb_config(size=0x531, utf8=1)),
    ]
    for ins in cases:
        app = a.applet(0x11, ins=ins)
        s.q(app, APPLET, 1)
        a.out(app, 0x7D8)
        s.q(app, APPLET, 101)
        s.q(app, APPLET, 104)
    # Switch 2 ids: the kind is the low 16 bits
    for aid in (0x01020011, 0x00010011, 0xFFFF0011):
        app = a.applet(aid, ins=(common(), kb_config(size=0x530, utf8=1)))
        s.q(app, APPLET, 120)
        a.out(app, 0x7D8)
    # the text check: answers of each kind and size, before and after Start, in both encodings
    for cfg in (kb_config(check=1), kb_config(size=0x530, check=1, utf8=1), kb_config(size=0x530, check=1, utf8=0, init_off=0x10,
                                                                                      init_len=4)):
        for answer in (u32(0) + bytes(0x7D4), u32(2) + bytes(0x7D4), u32(1) + u16text("Too short") + bytes(0x7C2),
                       u32(3) + b"msg" + bytes(0x7D1), b"\x00\x00", b"", u32(0), u32(5) + bytes(0x900)):
            app = a.applet(0x11, ins=(common(), cfg, work), start=False)
            a.events(app)
            a.push(app, answer)          # before Start: waits
            s.q(app, APPLET, 10)
            a.events(app)
            s.q(app, APPLET, 101)
            a.reply(app, 0x7DC)
            a.push(app, answer)
            a.events(app)
            a.out(app, 0x7D8)
            s.q(app, APPLET, 1)
            a.push(app, answer)          # after the end: kept, nothing
            s.q(app, APPLET, 101)
    app = a.applet(0x11, ins=(common(), kb_config(check=1)))
    s.q(app, APPLET, 20)
    s.q(app, APPLET, 1)
    s.q(app, APPLET, 101)
    a.push(app, u32(0))
    s.q(app, APPLET, 101)
    app = a.applet(0x11, ins=(common(), kb_config(check=1)))
    s.q(app, APPLET, 25)
    s.q(app, APPLET, 25)
    s.q(app, APPLET, 1)
    s.q(app, APPLET, 10)
    s.q(app, APPLET, 101)


def inline_keyboard(s):
    s.comment("the inline keyboard: states, Calc flags, layouts, replies, V2, UTF-8, requests")
    s.reset()
    a = Am(s)
    app = a.applet(0x11, 1, ins=(common(),), start=False)
    a.events(app)
    a.push(app, calc(0x1))            # before Start: waits for the first push after it
    a.push(app, u32(12))
    s.q(app, APPLET, 104)
    s.q(app, APPLET, 10)
    s.q(app, APPLET, 10)
    s.q(app, APPLET, 1)
    a.push(app, u32(6))
    a.events(app)
    a.replies(app, (8, 9, 8))
    for req in (u32(7), u32(11), u32(12), u32(14) + b"\x01", u32(99), b"\x0d", b"", u32(13), u32(13) + b"\x00\x01"):
        a.push(app, req)
    a.replies(app, (8,))
    # flags in every state
    for flags in (0x40, 0x400, 0x440, 0x1, 0x80, 0x10, 0x20, 0x8, 0x2, 0x100, 0xFFFFFFFFFFFFFFFF & ~0x4):
        a.push(app, calc(flags, text="Abc", cursor=2, utf8=1))
        s.q(app, APPLET, 104)
        s.q(app, APPLET, 104)
        s.q(app, APPLET, 104)
    a.push(app, calc(0x20, utf8=0))
    for text, amax, amin, ok, old in (("Link", 0, 0, "Go", False), ("Zelda Princess", 5, 2, "OK", False), ("", 3, 9, None, False),
                                      ("Ganon", 600, 0, None, True), ("Hyérule\U0001F600", 1, 0, "ok", True),
                                      ("\ud800x\udc00y", 0, 0, None, False), ("a\x05b\nc\td", 500, 0, None, False)):
        size = 0x4A0 if old else 0x4E8
        t = text.encode("utf-16-le", "surrogatepass")
        a.push(app, calc(0xC | 0x10, size=size, text=t, cursor=1, amax=amax, amin=amin, ok=ok))
        for _ in range(6):
            a.reply(app, PEEK)
        s.q(app, APPLET, 104)
    # appear without new text: the current one; without any: Player
    a.push(app, calc(0x4))
    for _ in range(6):
        a.reply(app, PEEK)
    app2 = a.applet(0x11, 1, ins=(common(),))
    a.push(app2, calc(0x5))
    for _ in range(8):
        a.reply(app2, PEEK)
    s.q(app2, APPLET, 104)
    # V2 and UTF-8 together, the old layout's UTF-8 byte
    a.push(app2, u32(13) + b"\x01")
    a.push(app2, calc(0x2C, size=0x4A0, text="Old", utf8=1))
    for n in (8, 8, 0x7ED, 0x7E0, 8, 8):
        a.reply(app2, n)
    a.push(app2, calc(0x2C, size=0x4A0, text="Old", utf8=0))
    for n in (8, 8, 0x405, 0x3F8, 8, 8):
        a.reply(app2, n)
    # short and odd Calc storages
    for st in (calc(0x4, total=12), calc(0x4, total=16), calc(0x4, total=20), calc(0xC, total=0x96, text="Q"),
               calc(0xC, size=0x4A0, total=0x70, text="Q"), calc(0xC, size=0x4A1, text="New"), calc(0xC, size=0, text="New", total=0x4EC),
               calc(0x4, size=0xFFFF, total=0x4EC), calc(0x5, size=0x4A0, total=0x45, amax=3), b"\x0a\x00\x00"):
        a.push(app2, st)
        for _ in range(6):
            a.reply(app2, PEEK)
        s.q(app2, APPLET, 104)
    # several requests queued at once, Finalize in the middle: the rest stays queued
    app3 = a.applet(0x11, 1, ins=(common(),), start=False)
    for req in (calc(0x1), u32(12), u32(4), u32(12), calc(0x1)):
        a.push(app3, req)
    s.q(app3, APPLET, 10)
    a.push(app3, u32(12))
    a.events(app3)
    a.replies(app3, (8, 9, 8, 8))
    s.q(app3, APPLET, 1)
    a.push(app3, u32(12))
    s.q(app3, APPLET, 104)
    s.q(app3, APPLET, 30)
    # Terminate / RequestExit of an inline keyboard, then pushes
    for end in (25, 20):
        app4 = a.applet(0x11, 2, ins=(common(),))
        a.push(app4, calc(0x1))
        s.q(app4, APPLET, end)
        s.q(app4, APPLET, 1)
        a.push(app4, calc(0xC, text="late"))
        for _ in range(4):
            s.q(app4, APPLET, 104)
    app5 = a.applet(0x11, 1, create=3)
    s.q(app5, APPLET, 120)
    a.push(app5, calc(0x1))
    a.replies(app5, (8, 9))


def other_applets(s):
    s.comment("player select, error, controller, cancelled and unknown kinds")
    s.reset()
    a = Am(s)
    for ui in (None, b"", b"\x01\x00", u32(0), u32(1), u32(3), u32(4), u32(5), u32(11), u32(12), u32(0x80000000) + bytes(0x9C)):
        ins = (common(),) if ui is None else (common(), ui)
        app = a.applet(0x10, ins=ins)
        a.out(app, 0x18)
        s.q(app, APPLET, 1)
    for arg in (b"", bytes(0x10), b"\x00" + bytes(0x1F), b"\x04" + bytes(7) + u32(0x2A202) + bytes(0x10),
                b"\x02" + bytes(7) + u32(2800) + b"en-US\0\0\0" + b"Dialog text" + bytes(0x800) + b"Full screen" + bytes(0x800),
                b"\x02" + bytes(7) + u32(5), b"\x01" + bytes(0x100), b"\x09"):
        app = a.applet(0x0E, ins=(common(), arg) if arg else (common(),))
        s.q(app, APPLET, 1)
        s.q(app, APPLET, 101)
        s.q(app, APPLET, 30)
    for arg in (None, b"\x02", ctrl_arg(1, 4, n=2), ctrl_arg(3, 4, 1, n=5), ctrl_arg(3, 4, 1, n=6), ctrl_arg(1, 1), ctrl_arg(-1, -1),
                ctrl_arg(0, 0), ctrl_arg(4, 2), ctrl_arg(1, 8, 2), ctrl_arg(127, -128), ctrl_arg(2, 2, 1)):
        app = a.applet(0x0C, ins=(bytes(0x20),) if arg is None else (bytes(0x20), arg))
        a.events(app)
        a.out(app, 12)
        s.q(app, APPLET, 10)       # a second Start: hid's calls again, no answer
        s.q(app, APPLET, 101)
    a.drain(6)
    for kind in (0x14, 0x1A, 0x0D, 0x0F, 0x24, 0x15, 0x01, 0x0A, 0x0B, 0x12, 0x16, 0x23, 0x25, 0xFFFF):
        for aid in (kind, 0x01000000 | kind, 0x01030000 | kind):
            app = a.applet(aid, mode=aid & 3)
            a.events(app)
            s.q(app, APPLET, 1)
            s.q(app, APPLET, 30)
            s.q(app, APPLET, 120)
            s.q(app, APPLET, 101)
    a.drain(4)
    # the accessor's other commands, before and after Start
    app = a.applet(0x15, start=False)
    for c in (1, 30, 20, 1, 25, 10, 1, 50, 60, 90, 110, 150, 120, 101, 104):
        s.q(app, APPLET, c, raw=b"\x01")
    app = a.applet(0x10, start=False)
    s.q(app, APPLET, 25)
    s.q(app, APPLET, 10)
    s.q(app, APPLET, 101)
    # CreateLibraryAppletEx and short creation requests
    for raw in (b"", u32(0x10), u32(0x10, 1), u32(0x10, 1) + bytes(8)):
        for cmd in (0, 3):
            s.q(a.cr, CREATOR, cmd, raw=raw, nobj=1)
    a.drain(4)


def web(s):
    s.comment("the web family: stand-ins, sessions, return values with and without [web] view")
    s.reset()
    a = Am(s)
    doc = tlv_arg([(0x6, b"menu.htdocs/index.html\0"), (0x3, b"http://localhost/cb\0"), (0x1C, u32(10)), (0x1D, b"\x03")])
    online = tlv_arg([(0x1, b"https://www.example.com/news?id=4\0"), (0x3, b"https://www.example.com/done\0")], size=0x2000, shim=5)
    nintendo = tlv_arg([(0x1, b"https://ctest.cdn.nintendo.net/\0")], size=0x2000, shim=5)
    plain = tlv_arg([(0x1, b"ftp://files\0")], size=0x2000, shim=5)
    cut_walk = tlv_arg([(0x41, u32(1)), (0x42, b"\x01")], size=0x16)                      # the second entry runs past the end
    count_short = tlv_arg([(0x41, u32(1)), (0x42, b"\x01")], size=0x100, count=1)          # 0x42 never reached
    big_values = tlv_arg([(0x41, u32(0x101)), (0x42, b"\x00\x01")], size=0x100)
    zero_size = tlv_arg([(0x41, b""), (0x42, b"\x05")], size=0x100)
    for view in ("false", "true"):
        s.config("web", "view", view)
        for online_pages in ("false", "true"):
            s.config("web", "online_pages", online_pages)
            for kind, arg in ((0x13, None), (0x13, online), (0x13, nintendo), (0x13, plain), (0x13, b"\x01"), (0x17, doc), (0x17, b""),
                              (0x18, online), (0x19, None), (0x17, cut_walk), (0x17, count_short)):
                for lib in (0, 0x30000, 0x80000):
                    ins = (common(lib),) if arg is None else (common(lib), arg)
                    app = a.applet(kind, ins=ins)
                    s.q(app, APPLET, 1)
                    a.out(app, WEB_OUT)
                    s.q(app, APPLET, 104)
            app = a.applet(0x13, ins=(u32(1, 0x20), online))   # short common arguments
            a.out(app, WEB_OUT)
            app = a.applet(0x13)                               # no common arguments at all
            a.out(app, WEB_OUT)
        # sessions: hidden or not, Appear, content messages, other kinds, RequestExit / Terminate
        for arg in (session_arg(1, 1), session_arg(0, 1), session_arg(2, 1), session_arg(1, 0), big_values, zero_size,
                    session_arg(1, 1, extra=[(0x6, b"nowhere.html\0")])):
            for finish in ("appear", "exit", "terminate"):
                a.drain(3)
                app = a.applet(0x17, ins=(common(0x80000), arg))
                a.events(app)
                a.focus()
                s.q(app, APPLET, 104)
                for m in (message(0, b"abc", 3), message(0, b"hi!\0", 4), message(0, b"x", 9), message(0, b"", 0), message(0x1000),
                          message(0x1001), message(7, b"zz"), b"\x00" * 0xF, message(0, b"long" * 8, 0x20)):
                    a.push(app, m)
                    s.q(app, APPLET, 104)
                    s.q(app, APPLET, 104)
                s.q(app, APPLET, 101)
                if finish == "appear":
                    a.push(app, APPEAR + b"tail")
                    s.q(app, APPLET, 104)
                elif finish == "exit":
                    s.q(app, APPLET, 20)
                else:
                    s.q(app, APPLET, 25)
                a.events(app)
                a.out(app, WEB_OUT)
                s.q(app, APPLET, 101)
                a.focus()
                a.push(app, APPEAR)
                s.q(app, APPLET, 104)
                s.q(app, APPLET, 20)
                s.q(app, APPLET, 25)
                s.q(app, APPLET, 1)
                a.drain(3)
        # pushes before Start; a session's Start twice
        app = a.applet(0x17, ins=(common(), session_arg(1, 1)), start=False)
        a.push(app, APPEAR)
        s.q(app, APPLET, 10)
        s.q(app, APPLET, 10)
        s.q(app, APPLET, 104)
        s.q(app, APPLET, 104)
        a.push(app, message(0, b"go\0", 3))
        s.q(app, APPLET, 104)
        a.push(app, APPEAR)
        a.out(app, WEB_OUT)
    s.config("web", "view", "false")
    s.config("web", "online_pages", "false")


def launch_channel(s, aux):
    s.comment("launch parameters from [am] launch_user_channel (read at start-up and at each reset)")
    files = {
        "three.bin": u32(3) + struct.pack("<Q", 3) + b"abc" + struct.pack("<Q", 0) + struct.pack("<Q", 5) + b"hello",
        "big.bin": u32(3) + struct.pack("<Q", 2) + b"ok" + struct.pack("<Q", (64 << 20) + 1) + b"x" * 16,
        "max.bin": u32(2) + struct.pack("<Q", 1) + b"m" + struct.pack("<Q", 64 << 20) + b"y" * 8,
        "short.bin": u32(3) + struct.pack("<Q", 4) + b"good" + struct.pack("<Q", 100) + b"0123456789",
        "count.bin": u32(0xFFFFFFFF) + struct.pack("<Q", 1) + b"z",
        "tiny.bin": b"\x01\x00",
        "zero.bin": u32(0) + b"junk",
        "halfsize.bin": u32(2) + struct.pack("<Q", 1) + b"q" + b"\x01\x00\x00",
    }
    for name, data in files.items():
        (aux / name).write_bytes(data)
    for name in list(files) + ["missing.bin"]:
        s.config("am", "launch_user_channel", str(aux / name))
        s.reset()
        a = Am(s)
        for _ in range(4):
            a.take(a.af, FUNCS, 1, SMALL, raw=u32(1))
        s.q(a.af, FUNCS, 1, raw=u32(1))
    # the channel comes back at a reset; ClearUserChannel and Unpop on top of it
    s.config("am", "launch_user_channel", str(aux / "three.bin"))
    s.reset()
    a = Am(s)
    s.q(a.af, FUNCS, 122, os_=(a.storage(b"mine"),))
    a.take(a.af, FUNCS, 1, 4, raw=u32(1))
    s.q(a.af, FUNCS, 121)
    s.q(a.af, FUNCS, 1, raw=u32(1))
    s.lines.append("reset keep")
    a.take(a.af, FUNCS, 1, 5, raw=u32(1))
    s.config("am", "launch_user_channel", "")
    s.reset()
    a = Am(s)
    s.q(a.af, FUNCS, 1, raw=u32(1))
    s.q(a.af, FUNCS, 1, raw=u32(2))
    s.lines.append("reset keep")
    a.take(a.af, FUNCS, 1, 0x88, raw=u32(2))


def save_queries(s, aux):
    s.comment("save sizes: ExtendSaveData, GetSaveDataSize, EnsureSaveData, CreateCacheStorage, the NACP maxima")
    s.reset()
    a = Am(s)
    other = struct.pack("<QQ", 5, 6)
    for uid in (LOCAL, other, bytes(16)):
        s.q(a.af, FUNCS, 20, raw=uid)
        s.q(a.af, FUNCS, 26, raw=b"\x01" + bytes(7) + uid)
    for t in (0, 2, 3, 0xFF):
        s.q(a.af, FUNCS, 25, raw=bytes([t]) + bytes(7) + LOCAL + i64(0x5000) + i64(0x6000))
        s.q(a.af, FUNCS, 26, raw=bytes([t]) + bytes(7) + LOCAL)
    s.q(a.af, FUNCS, 26, raw=b"\x01" + bytes(7) + other)
    s.q(a.af, FUNCS, 25, raw=b"\x01" + bytes(7) + other + i64(-5) + i64(0x7FFFFFFFFFFFFFFF))
    s.q(a.af, FUNCS, 26, raw=b"\x01" + bytes(7) + LOCAL)
    s.q(a.af, FUNCS, 25, raw=b"\x01")
    s.q(a.af, FUNCS, 26, raw=b"\x01")
    s.reset()
    a = Am(s)
    s.q(a.af, FUNCS, 26, raw=b"\x01" + bytes(7) + LOCAL)
    for idx, size, journal in ((3, 0x40000, 0x20000), (3, 0x40000, 0x20000), (0, 0, 0), (0xFFFF, 0x1000, 0x1000), (1, -1, -1)):
        s.q(a.af, FUNCS, 27, raw=struct.pack("<H", idx) + bytes(6) + i64(size) + i64(journal))
    s.q(a.af, FUNCS, 27, raw=b"\x03")
    nacp = bytearray(0x4000)
    struct.pack_into("<qqqq", nacp, 0x3148, 0x100000, 0, -5, 0x2000)
    (aux / "control.nacp").write_bytes(bytes(nacp))
    (aux / "short.nacp").write_bytes(bytes(nacp[:0x314C]))
    (aux / "mid.nacp").write_bytes(bytes(nacp[:0x3154]))
    (aux / "dev.nacp").write_bytes(bytes(nacp[:0x3164]))
    for f in ("control.nacp", "short.nacp", "mid.nacp", "dev.nacp", "missing.nacp"):
        s.config("fs", "nacp", str(aux / f))
        s.q(a.af, FUNCS, 28)
        s.q(a.af, FUNCS, 35)
    s.config("fs", "nacp", "")
    s.q(a.af, FUNCS, 28)


# ---- every command with variants, then unknown ids ---------------------------------------------------------------------------------
def fuzz_all(s, variants):
    s.comment("argument and buffer variants of every command")
    n = max(4, variants // 4)
    s.reset()
    a = Am(s)
    for svc, cmds in ((OE, (0, 10)), (AE, (350,))):
        sm = s.open(svc)
        for cmd in cmds:
            s.fuzz(sm, svc, cmd, n=n)
    for cmd in (0, 1, 2, 3, 4, 10, 11, 20, 40, 60, 1000):
        s.fuzz(a.proxy, PROXY, cmd, n=max(2, variants // 8))
    for cmd in CSG_CMDS:
        s.fuzz(a.csg, CSG, cmd, n=n)
    for cmd in SELF_CMDS:
        s.fuzz(a.self_, SELF, cmd, n=n)
    win = a.get(2)
    for cmd in (1, 10, 11, 12):
        s.fuzz(win, WIN, cmd, n=n)
    aud = a.get(3)
    for cmd in (0, 1, 2, 3, 4):
        s.fuzz(aud, AUDIO, cmd, n=n, sizes=(0, 4, 8, 12, 16))
        s.q(aud, AUDIO, 1)
        s.q(aud, AUDIO, 2)
    for raw in (struct.pack("<ff", float("nan"), float("inf")), struct.pack("<ff", -0.0, 1e-45), struct.pack("<f", 0.25)):
        s.q(aud, AUDIO, 0, raw=raw)
        s.q(aud, AUDIO, 1)
        s.q(aud, AUDIO, 2)
        s.q(aud, AUDIO, 3, raw=raw)
        s.q(aud, AUDIO, 1)
    lk = s.q(a.csg, CSG, 30, nobj=1)[0]
    for cmd in (1, 2, 3, 4):
        s.fuzz(lk, LOCK, cmd, n=n)
    sc = a.get(60)
    for cmd in (10, 1050):
        s.fuzz(sc, SCALE, cmd, n=n)
    for cmd in (0, 3, 10, 11, 12):
        s.fuzz(a.cr, CREATOR, cmd, n=variants)
    for size in (0, 0x100):
        st = a.storage(b"", size=size)
        acc = s.q(st, STORAGE, 0, nobj=1)[0]
        s.fuzz(st, STORAGE, 0, n=max(2, variants // 8))
        for cmd in (0, 10, 11):
            s.fuzz(acc, ACCESS, cmd, n=variants)
        s.q(acc, ACCESS, 11, raw=i64(0), bufs=(("b", size, None),) if size else ())
    for kind, mode in ((0x15, 0), (0x11, 1), (0x11, 0), (0x17, 0), (0x0C, 0), (0x10, 0)):
        app = a.applet(kind, mode, start=False)
        for cmd in APPLET_CMDS:
            s.fuzz(app, APPLET, cmd, n=max(4, variants // 8))
    for cmd in FUNCS_CMDS:
        s.fuzz(a.af, FUNCS, cmd, n=max(4, variants // 8) if cmd in (20, 27) else n)
    perf = a.get(40)
    for cmd in (0, 1):
        s.fuzz(perf, PERF, cmd, n=max(2, variants // 8))


def fuzz_storages(s, variants):
    s.comment("random in-data and interactive storages for each applet kind")
    r = s.rng
    s.reset()
    a = Am(s)
    rnd = lambda k: bytes(r.randrange(256) for _ in range(k))
    for kind, mode in ((0x11, 0), (0x11, 1), (0x0C, 0), (0x10, 0), (0x0E, 0), (0x13, 0), (0x17, 0), (0x18, 0)):
        for i in range(max(2, variants // 8)):
            size = r.choice((0, 1, 2, 4, 6, 8, 0x10, 0x20, 0x3D4, 0x530, 0x100))
            arg = rnd(size)
            if kind == 0x17 and i % 2:
                arg = session_arg(r.choice((0, 1, 2)), r.choice((0, 1, 7)))
            ins = (common(r.choice((0, 0x30000, 0x80000))), arg) + ((rnd(0x40),) if kind == 0x11 else ())
            app = a.applet(kind, mode, ins=ins)
            for _ in range(3):
                n = r.choice((0, 2, 4, 8, 0x10, 0x14, 0x20, 0x4A4, 0x4EC, 0x7D8))
                body = rnd(n)
                if kind == 0x11 and mode and n >= 4 and r.randrange(2):
                    body = u32(r.choice((4, 10, 12, 13, 14, 6))) + body[4:]
                a.push(app, body)
                s.q(app, APPLET, 104)
                s.q(app, APPLET, 104)
            s.q(app, APPLET, 1)
            s.q(app, APPLET, 101)


def sweeps(s):
    s.comment("unknown ids of each interface (and some the extensions answer)")
    s.reset()
    a = Am(s)
    s.sweep(s.open(OE), OE, [1, 2, 3, 11, 100, 350, 1000])
    s.sweep(s.open(AE), AE, [0, 10, 100, 200, 201, 300, 351, 400])
    s.sweep(a.proxy, PROXY, [5, 12, 21, 30, 41, 50, 61, 100, 999, 1001])
    s.sweep(a.csg, CSG, [3, 7, 14, 15, 16, 21, 33, 56, 58, 65, 69, 91, 101, 120, 131, 301, 502, 700, 1003, 1006, 2000, 3000])
    s.sweep(a.self_, SELF, [5, 6, 7, 8, 15, 19, 21, 22, 42, 43, 47, 52, 67, 99, 121, 230, 2001, 2101, 2403])
    s.sweep(a.get(2), WIN, [0, 2, 3, 13, 20, 22])
    s.sweep(a.get(3), AUDIO, [5, 6, 10])
    s.sweep(a.get(4), DISP, [0, 7, 10, 29])
    s.sweep(a.get(10), WIND, [0, 1, 11, 22, 24])
    s.sweep(a.cr, CREATOR, [1, 2, 4, 13, 20])
    app = a.applet(0x15, start=False)
    s.sweep(app, APPLET, [2, 11, 21, 26, 31, 40, 107, 111, 121, 151, 160, 170, 200])
    # (11 .. 19 are launch commands of other kinds: not sent, though neither file answers them)
    s.sweep(a.af, FUNCS, [2, 24, 29, 36, 38, 41, 51, 70, 71, 102, 111, 125, 142, 152, 180, 190, 220, 322, 330, 1000, 2001])
    s.sweep(a.get(1000), DEBUG, [0, 1, 10, 20])
    st = a.storage(b"abcd")
    s.sweep(st, STORAGE, [1, 2])
    s.sweep(s.q(st, STORAGE, 0, nobj=1)[0], ACCESS, [1, 12])
    s.sweep(s.q(a.csg, CSG, 31, nobj=1)[0], LOCK, [0, 5])
    s.sweep(a.get(60), SCALE, [0, 11, 1049, 1051])
    s.sweep(a.get(40), PERF, [0, 1, 10])


# ---- [applet] swkbd_text (sticky: last) --------------------------------------------------------------------------------------------
def with_swkbd_text(s):
    s.comment("[applet] swkbd_text set: the keyboards answer with it (once set, it stays set)")
    s.reset()
    a = Am(s)
    work = work_buffer("Initial", 0x10)
    values = ["", "Alexander", "Ale", "A" * 600, " spaced ", raw_text(b"\xc3\xa9t\xc3\xa9"), raw_text(b"\xe9"), raw_text(b"\xf0\x80A"),
              raw_text(b"\xf0A"), raw_text(b"\xed\xa0\x80z"), raw_text(b"\xf4\x90\x80\x80"), raw_text(b"\xf0\x9f\x98\x80!"),
              raw_text(b"\xc0\x80x"), raw_text(b"\xff\x80\xbf"), raw_text(b"\xe2\x82"), raw_text(b"\xe2\x82\xac"),
              raw_text(b"\xf8\x88\x80\x80\x80"), raw_text(b"a\x01b\tc")]
    for v in values:
        s.config("applet", "swkbd_text", v)
        for cfg in (None, kb_config(max_len=3), kb_config(utf8=1), kb_config(size=0x530, utf8=1, max_len=4, init_off=0x10, init_len=7),
                    kb_config(check=1)):
            ins = (common(),) if cfg is None else (common(), cfg, work)
            app = a.applet(0x11, ins=ins)
            a.out(app, 0x7D8)
            if cfg is not None and cfg[0x3D0]:
                a.reply(app, 0x7DC)
                a.push(app, u32(1) + bytes(0x7D4))   # refused, prompt off: cancel
                a.out(app, 0x7D8)
        app = a.applet(0x11, 1, ins=(common(),))
        a.push(app, calc(0x1))
        a.push(app, calc(0x2C, text="Link", utf8=1, amax=4))
        a.push(app, calc(0x2C, text="Link", utf8=0, amax=0))
        for _ in range(14):
            a.reply(app, PEEK)
        s.q(app, APPLET, 104)


def handoff(s, aux, writable=False):
    """Not writable: the hand-off cannot be written (the call answers and the program goes on). Writable (variant "handoff_write", needs
    the replay's `api am_exit_hook` / `api file`): the exit becomes an "H exit" line, and both hand-off files are shown after each call;
    then the written channel is loaded as the next program's launch parameters."""
    if writable:
        s.comment("ExecuteProgram and the relaunch, hand-off written, exit hook set: each call's files and exit")
        ini = aux / "out" / "execute_program.ini"
        ini.parent.mkdir(parents=True, exist_ok=True)
        api(s, "am_exit_hook")
    else:
        s.comment("ExecuteProgram and the relaunch with a hand-off that cannot be written: the call answers and the program goes on")
        blocker = aux / "blocker"
        blocker.write_bytes(b"a regular file: nothing can be written below it")
        ini = blocker / "execute_program.ini"
    chan = str(ini) + ".user_channel.bin"

    def files(rm=False):
        if writable:
            for f in (str(ini), chan):
                api(s, ("file_rm " if rm else "file ") + f)

    files(rm=True)
    s.config("am", "execute_program_file", str(ini))
    s.config("title", "program_id", "0x0100ABCD00002002")
    s.config("title", "application_id", "")
    s.config("am", "program_index", "3")
    s.reset()
    a = Am(s)
    s.q(a.af, FUNCS, 122, os_=(a.storage(b"en-US\0\0\0" + bytes(range(8))),))
    for kind, value in ((0, 2), (0, 0x0100ABCD00002005), (1, 7), (2, 7), (3, 0x1FF), (0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF)):
        s.q(a.af, FUNCS, 120, raw=u32(kind, 0) + struct.pack("<Q", value))
        files()
    s.q(a.af, FUNCS, 120)
    files()
    a.take(a.af, FUNCS, 1, 16, raw=u32(1))      # the channel is not cleared by ExecuteProgram
    for aid in (0, 0x0100ABCD00002000, 0x0100ABCD00002FFF, 0x0100ABCD00002002, 0x0100FFFF00010000):
        files(rm=True)
        s.q(a.af, FUNCS, 12, raw=struct.pack("<Q", aid))
        files()
        s.q(a.af, FUNCS, 10, raw=struct.pack("<Q", aid), os_=(a.storage(b"fr-FR\0"),))
        files()
        s.q(a.af, FUNCS, 10, raw=struct.pack("<Q", aid), os_=(a.csg,))
        files()
        s.q(a.af, FUNCS, 10, raw=struct.pack("<Q", aid))
        files()
    s.q(a.af, FUNCS, 12)
    files()
    if writable:
        s.comment("a channel of several storages (one empty, one large), handed off, then loaded as the next program's")
        for data in (b"", bytes(range(256)) * 9, b"\x01"):
            s.q(a.af, FUNCS, 122, os_=(a.storage(data),))
        s.q(a.af, FUNCS, 120, raw=u32(0, 0) + struct.pack("<Q", 1))
        files()
        s.config("am", "launch_user_channel", chan)
        s.config("am", "previous_program_index", "3")
        s.reset()
        a = Am(s)
        s.q(a.af, FUNCS, 123)
        for _ in range(6):
            s.q(a.af, FUNCS, 1, raw=u32(1))
        s.config("am", "launch_user_channel", "")
        s.config("am", "previous_program_index", "-1")
        s.reset()
        a = Am(s)
    for cmd in (10, 12, 120):
        s.fuzz(a.af, FUNCS, cmd, n=10)
        files()
    s.config("title", "program_id", "0")
    s.config("title", "application_id", "0")
    s.config("am", "program_index", "0")


def generate(s, variants, variant=None):
    aux = Path(s.aux_dir)
    aux.mkdir(parents=True, exist_ok=True)

    def write_bytes(path):   # config values may hold bytes that are not UTF-8
        Path(path).write_bytes(("\n".join(s.lines) + "\n").encode("utf-8", "surrogateescape"))

    s.write = write_bytes
    if variant in ("handoff", "handoff_write"):
        handoff(s, aux, writable=variant == "handoff_write")
        return
    vectors(s)
    messages_and_focus(s)
    storages(s, aux)
    keyboards(s)
    inline_keyboard(s)
    other_applets(s)
    web(s)
    launch_channel(s, aux)
    save_queries(s, aux)
    fuzz_all(s, variants)
    fuzz_storages(s, variants)
    sweeps(s)
    with_swkbd_text(s)
